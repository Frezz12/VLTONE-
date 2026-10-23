#include "AudioRuntime.hpp"

#include <algorithm>

namespace daw {
namespace {
// FX layers share audio, but MIDI leaves the container exactly once, unchanged.
class RackJunctionNode final : public engine::Node {
public:
    RackJunctionNode(std::string label, bool dryMidi = false) : label(std::move(label)), dryMidi(dryMidi) {}
    std::string_view name() const noexcept override { return label; }
    engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::Passthrough; }
    engine::OfflineNodePolicy offlineNodePolicy() const noexcept override { return engine::OfflineNodePolicy::Ordered; }
    bool canFuseTask() const noexcept override { return true; }
    bool supportsInPlace() const noexcept override { return true; }
    void process(const engine::ProcessContext& context) override {
        const auto audio = dryMidi && !context.inputs.empty() ? context.inputs.first(context.inputs.size() - 1) : context.inputs;
        engine::dsp::sumInto(context.output, audio);
        if (!context.midiOutput) return;
        const auto first = dryMidi && !context.midiInputs.empty() ? context.midiInputs.size() - 1 : 0;
        for (auto i = first; i < context.midiInputs.size(); ++i) if (const auto* input = context.midiInputs[i])
            for (const auto& event : input->events()) if (!context.midiOutput->push(event)) return;
    }
private:
    std::string label;
    bool dryMidi;
};
}

void AudioRuntime::clearGraphIds(std::span<InsertSlot> slots) {
    for (auto& slot : slots) {
        slot.nodeId = slot.rightNodeId = engine::kInvalidNode;
        slot.leftSelectorId = slot.rightSelectorId = engine::kInvalidNode;
    }
}

void AudioRuntime::clearGraphIds(ClipFxChannel& channel) {
    channel.playerId = channel.faderId = channel.meterId = engine::kInvalidNode;
    channel.insertIds.clear();
    clearGraphIds(channel.inserts);
}

engine::NodeId AudioRuntime::connectSlots(engine::AudioGraph& graph,
                                         std::span<InsertSlot> live,
                                         std::vector<engine::NodeId>& ids,
                                         engine::NodeId head, engine::NodeId analysisSource) {
    ids.clear();
    engine::NodeId previous = head;
    // Analysis nodes are terminal readers: the graph schedules every node,
    // protects the sink buffer from reuse, and derives PDC from the sink only.
    // Each analyzer hears the same completed signal, never another analyzer.
    for (const bool analysis : {false, true}) for (InsertSlot& slot : live) {
        if (slot.configuration.analysisOnly != analysis) continue;
        clearGraphIds(std::span(&slot, 1));
        if (!slot.node) continue;
        const auto input = analysis && analysisSource != engine::kInvalidNode ? analysisSource : previous;
        if (slot.channelMode == PluginChannelMode::DualMono && slot.rightNode &&
            slot.leftSelector && slot.rightSelector && slot.stereoMerge) {
            slot.leftSelectorId = graph.adoptNode(slot.leftSelector);
            slot.rightSelectorId = graph.adoptNode(slot.rightSelector);
            slot.nodeId = graph.adoptNode(slot.node);
            slot.rightNodeId = graph.adoptNode(slot.rightNode);
            const engine::NodeId mergeId = graph.adoptNode(slot.stereoMerge);
            if (!analysis) ids.push_back(slot.leftSelectorId);
            graph.connect(input, slot.leftSelectorId);
            graph.connect(input, slot.rightSelectorId);
            graph.connect(slot.leftSelectorId, slot.nodeId);
            graph.connect(slot.rightSelectorId, slot.rightNodeId);
            graph.connect(slot.nodeId, mergeId);
            graph.connect(slot.rightNodeId, mergeId);
            graph.connect(input, mergeId);
            if (!analysis) previous = mergeId;
            continue;
        }
        const engine::NodeId id = graph.adoptNode(slot.node);
        slot.nodeId = id;
        if (!analysis) ids.push_back(id);
        graph.connect(input, id);
        if (!analysis) previous = id;
    }
    return previous;
}

engine::NodeId AudioRuntime::connectMiniModules(
    engine::AudioGraph& graph, TrackChannel& channel,
    const std::vector<AudioGraphSpec::MiniModuleRoute>& routes, bool postFx, engine::NodeId head,
    engine::NodeId* first) {
    std::vector<engine::NodeId> ids;
    for (auto& slot : channel.miniModules) {
        const auto route = std::find_if(routes.begin(), routes.end(),
            [&](const auto& entry) { return entry.slotId == slot.slotId; });
        if (route == routes.end() || route->postFx != postFx) continue;
        head = connectSlots(graph, std::span(&slot, 1), ids, head);
        if (first && *first == engine::kInvalidNode && !ids.empty()) *first = ids.front();
    }
    return head;
}

void AudioRuntime::updateRackLayers(const std::string& channelId, const std::vector<RackGroupModel>& groups) {
    const auto found = channels.find(channelId); if (found == channels.end()) return;
    for (const auto& group : groups) {
        const bool solo = std::any_of(group.layers.begin(), group.layers.end(), [](const auto& layer) { return layer.solo; });
        for (const auto& layer : group.layers) if (const auto fader = found->second.layerFaders.find(layer.id); fader != found->second.layerFaders.end()) {
            fader->second->setGain(layer.gain);
            fader->second->setSilent(layer.muted || (solo && !layer.solo));
        }
    }
}
engine::NodeId AudioRuntime::connectRackSlots(engine::AudioGraph& graph, TrackChannel& channel,
    std::span<InsertSlot> slots, const std::vector<RackGroupModel>& groups, engine::NodeId head, engine::NodeId analysisSource) {
    if (std::none_of(groups.begin(), groups.end(), [](const auto& g) { return !g.layers.empty(); })) {
        channel.layerFaders.clear();
        return connectSlots(graph, slots, channel.ids.inserts, head, analysisSource);
    }
    clearGraphIds(slots);
    channel.ids.inserts.clear();
    // All incoming audio joins before the split, including routed tracks and bounce injection.
    const auto entry = graph.adoptNode(std::make_shared<RackJunctionNode>("Rack input"));
    graph.connect(head, entry); channel.ids.inserts.push_back(entry);
    std::unordered_set<std::string> retained;
    using Chain = std::vector<InsertSlot*>;
    std::function<engine::NodeId(const Chain&, const std::string&, engine::NodeId, int)> connect;
    connect = [&](const Chain& chain, const std::string& parent, engine::NodeId input, int depth) {
        auto tail = input;
        for (std::size_t i = 0; i < chain.size();) {
            const RackGroupModel* group = nullptr;
            if (depth < 9) for (const auto& g : groups)
                if (g.parentId == parent && !g.insertIds.empty() && g.insertIds.front() == chain[i]->slotId) { group = &g; break; }
            if (group) {
                Chain members;
                while (i < chain.size() && std::find(group->insertIds.begin(), group->insertIds.end(), chain[i]->slotId) != group->insertIds.end())
                    members.push_back(chain[i++]);
                if (group->layers.empty()) tail = connect(members, group->id, tail, depth + 1);
                else {
                    const auto sum = graph.adoptNode(std::make_shared<RackJunctionNode>(group->name + " sum", true));
                    const bool solo = std::any_of(group->layers.begin(), group->layers.end(), [](const auto& layer) { return layer.solo; });
                    for (const auto& layer : group->layers) {
                        Chain branch;
                        for (auto* slot : members)
                            if (std::find(layer.insertIds.begin(), layer.insertIds.end(), slot->slotId) != layer.insertIds.end()) branch.push_back(slot);
                        const auto end = connect(branch, group->id, tail, depth + 1);
                        auto& fader = channel.layerFaders[layer.id];
                        if (!fader) fader = std::make_shared<engine::GainNode>(layer.name + " level");
                        retained.insert(layer.id); fader->setGain(layer.gain); fader->setSilent(layer.muted || (solo && !layer.solo));
                        const auto level = graph.adoptNode(fader);
                        graph.connect(end, level); graph.connect(level, sum);
                    }
                    graph.connect(tail, sum, engine::InputRole::Sidechain);
                    tail = sum;
                }
            } else {
                auto* slot = chain[i++];
                if (slot->configuration.analysisOnly) continue;
                std::vector<engine::NodeId> ids;
                tail = connectSlots(graph, std::span(slot, 1), ids, tail);
                channel.ids.inserts.insert(channel.ids.inserts.end(), ids.begin(), ids.end());
            }
        }
        // Analyzers hear the finished branch; they never become a serial processor.
        for (auto* slot : chain) if (slot->configuration.analysisOnly && slot->nodeId == engine::kInvalidNode) {
            std::vector<engine::NodeId> ids;
            connectSlots(graph, std::span(slot, 1), ids, tail, depth == 0 ? analysisSource : engine::kInvalidNode);
        }
        return tail;
    };
    Chain chain; for (auto& slot : slots) chain.push_back(&slot);
    const auto tail = connect(chain, {}, entry, 0);
    std::erase_if(channel.layerFaders, [&](const auto& item) { return !retained.contains(item.first); });
    return tail;
}

void AudioRuntime::buildGraph(const AudioGraphSpec& spec) {
    // Reuse the processors and their DSP state. The published graph co-owns
    // the old topology until every render using it has completed.
    auto& graph = engine.graph();
    graph = engine::AudioGraph{};
    std::unordered_set<std::string> retained{AudioGraphSpec::masterChannelId};
    for (const auto& channel : spec.channels) retained.insert(channel.id);
    std::erase_if(channels, [&](const auto& entry) { return !retained.contains(entry.first); });

    if (!masterSum) masterSum = std::make_shared<engine::SumNode>("Master Sum");
    if (!masterFader) masterFader = std::make_shared<engine::GainNode>("Master");
    masterSumId = graph.adoptNode(masterSum);
    masterFaderId = graph.adoptNode(masterFader);
    graph.setSink(masterFaderId);
    masterFader->setGain(spec.masterVolume);
    masterFader->setPan(spec.masterPan);

    // The master's inserts sit between its sum and its fader. Giving the master
    // a real TrackChannel — rather than special-casing it — is what lets every
    // insert command below work on it with no second code path.
    TrackChannel& masterChannel = channels[std::string(AudioGraphSpec::masterChannelId)];
    masterChannel.fader = masterFader;
    masterChannel.ids = TrackNodes{};
    masterChannel.ids.clips = masterSumId;
    masterChannel.ids.sourceTap = masterSumId;
    masterChannel.ids.fader = masterFaderId;
    masterChannel.ids.meter = masterFaderId;
    const auto masterMiniEnd = connectMiniModules(graph, masterChannel, spec.masterMiniModules, false, masterSumId);
    auto masterChainEnd = connectRackSlots(graph, masterChannel, masterChannel.inserts, spec.masterRackGroups, masterMiniEnd, masterFaderId);
    masterChainEnd = connectMiniModules(graph, masterChannel, spec.masterMiniModules, true, masterChainEnd);
    masterChannel.ids.preFaderTap = masterChainEnd;
    graph.connect(masterChainEnd, masterFaderId);

    // The metronome is a permanent source summed into the master; it only makes
    // sound while the transport rolls and the click is enabled.
    if (!metronome) metronome = std::make_shared<engine::MetronomeNode>();
    metronome->setEnabled(spec.metronomeEnabled);
    // Left out of the graph entirely during a render rather than merely
    // disabled: `MetronomeNode` gates its click on `context.playing`, which an
    // offline pass asserts, and it renders its count-in ahead of that gate. Not
    // connecting it is the only way to be sure none of it reaches the file.
    if (!renderingPass) {
        const engine::NodeId metronomeId = graph.adoptNode(metronome);
        graph.connect(metronomeId, masterSumId);
    }

    // Auditioning a file from the browser, on the same terms: a permanent
    // source into the master, silent unless a preview was asked for. Adopting
    // the same object every rebuild is what lets a preview keep playing while
    // the user adds a track.
    if (!preview) preview = std::make_shared<engine::PreviewPlayerNode>();
    const engine::NodeId previewId = graph.adoptNode(preview);
    graph.connect(previewId, masterSumId);

    // Which channels are fed from elsewhere — the destination of a track's
    // output, or of a send. Known *before* the strips are built, because a
    // channel that receives audio needs a merge point ahead of its inserts and
    // one that does not should not pay for a node it never uses.
    std::unordered_set<std::string> receivers;
    for (const auto& track : spec.channels) {
        if (!track.outputBusId.empty()) receivers.insert(track.outputBusId);
        for (const SendModel& send : track.sends) {
            receivers.insert(send.destinationTrackId);
        }
        for (const auto& clip : track.clipFx) {
            if (clip.playbackInjection.stage !=
                PlaybackInjectionStage::TrackSource) {
                continue;
            }
            receivers.insert(clip.playbackInjection.anchorChannelId.empty()
                                 ? track.id
                                 : clip.playbackInjection.anchorChannelId);
        }
    }

    struct OwnedTrackNodes { const AudioGraphSpec::Channel* track; engine::NodeId first, end; };
    std::vector<OwnedTrackNodes> frozenRanges;
    // ── One channel strip per track ──
    for (const auto& track : spec.channels) {

        const auto firstOwned = engine::NodeId(graph.nodeCount());
        TrackChannel& channel = channels[track.id];
        if (!channel.clips) {
            channel.clips = std::make_shared<engine::ClipPlayerNode>(track.name + " Clips");
        }
        if (!channel.fader) {
            channel.fader = std::make_shared<engine::GainNode>(track.name + " Fader");
        }
        if (!channel.meter) {
            channel.meter = std::make_shared<engine::MeterNode>(track.name + " Meter");
        }

        channel.ids = TrackNodes{};
        channel.ids.clips = graph.adoptNode(channel.clips);
        channel.ids.fader = graph.adoptNode(channel.fader);
        channel.ids.meter = graph.adoptNode(channel.meter);
        graph.connect(channel.ids.fader, channel.ids.meter);
        channel.fader->setGain(track.volume);
        channel.fader->setPan(track.pan);
        channel.fader->setMono(track.mono);
        channel.fader->setSilent(track.silent);

        // A clip with private inserts leaves the shared track clip player,
        // runs through its own chain, post-FX level and meter, then rejoins the
        // track here.  This is the clip equivalent of Sampler FX: other clips,
        // monitored input and routed audio never touch the private plugins.
        std::unordered_set<std::string> wantedClipFx;
        for (const auto& clip : track.clipFx) wantedClipFx.insert(clip.id);
        std::erase_if(channel.clipFx, [&](const auto& entry) {
            return !wantedClipFx.contains(entry.first);
        });

        // Recording suppresses the previous take, including its private DSP.
        // Keep the instances (and open editors) alive so Stop can reuse their
        // state, but clear every topology id before omitting them. Sidechains
        // and bounce injection are connected later from these same ids.
        const bool capturing = track.capturing;
        for (auto& [clipId, clipFx] : channel.clipFx) clearGraphIds(clipFx);
        clearGraphIds(channel.instrument);
        clearGraphIds(channel.samplerInserts);

        engine::NodeId sourceHead = channel.ids.clips;
        if (!wantedClipFx.empty()) {
            if (!channel.clipFxSum) {
                channel.clipFxSum =
                    std::make_shared<engine::SumNode>(track.name + " Clip FX Sum");
            }
            engine::NodeId sumId = engine::kInvalidNode;
            if (!capturing) {
                sumId = graph.adoptNode(channel.clipFxSum);
                graph.connect(channel.ids.clips, sumId);
            }
            for (const auto& clip : track.clipFx) {
                ClipFxChannel& clipChannel = channel.clipFx[clip.id];
                if (!clipChannel.player) {
                    clipChannel.player = std::make_shared<engine::ClipPlayerNode>(
                        track.name + " / " + clip.name + " Player");
                }
                if (!clipChannel.fader) {
                    clipChannel.fader = std::make_shared<engine::GainNode>(
                        track.name + " / " + clip.name + " Level");
                }
                if (!clipChannel.meter) {
                    clipChannel.meter = std::make_shared<engine::MeterNode>(
                        track.name + " / " + clip.name + " Meter");
                }
                // Dormant instances remain available to their editors; only
                // the executable topology is absent during capture.
                if (capturing) continue;
                clipChannel.playerId = graph.adoptNode(clipChannel.player);
                clipChannel.faderId = graph.adoptNode(clipChannel.fader);
                clipChannel.meterId = graph.adoptNode(clipChannel.meter);
                const engine::NodeId chainEnd = connectSlots(
                    graph, clipChannel.inserts, clipChannel.insertIds,
                    clipChannel.playerId);
                clipChannel.fader->setGain(clip.gain);
                clipChannel.fader->setPan(clip.pan);
                graph.connect(chainEnd, clipChannel.faderId);
                graph.connect(clipChannel.faderId, clipChannel.meterId);
                if (!clip.playbackInjection.active())
                    graph.connect(clipChannel.meterId, sumId);
            }
            if (!capturing) sourceHead = sumId;
        } else {
            channel.clipFxSum.reset();
        }

        // Retain an existing input node so routing and monitor changes are
        // one atomic publication; never reconstruct clip/MIDI/automation data.
        if (track.input.present) {
            if (!channel.input) channel.input = std::make_shared<engine::InputNode>(
                track.name + " Input", engine.inputBus(), track.input.channel,
                track.input.channelCount);
            channel.input->setRouting(track.input.channel, track.input.channelCount,
                track.input.enabled, track.input.monitorMask);
            channel.inputChannel = track.input.channel;
            channel.inputChannelCount = track.input.channelCount;
            channel.ids.input = graph.adoptNode(channel.input);
        } else {
            channel.input.reset();
        }

        // The instrument, and the notes that drive it. Only tracks that carry
        // notes get either — an audio track has nothing to sound.
        engine::NodeId head = sourceHead;
        if (track.acceptsMidi) {
            if (!channel.midiClips) {
                channel.midiClips =
                    std::make_shared<engine::MidiClipPlayerNode>(track.name + " Notes");
            }
            channel.ids.midiClips = graph.adoptNode(channel.midiClips);

            if (!channel.instrument.empty() && channel.instrument.front().node) {
                channel.ids.instrument = graph.adoptNode(channel.instrument.front().node);
                channel.instrument.front().nodeId = channel.ids.instrument;
                // Notes in, audio out. The instrument also takes the clip
                // player's (silent) output so the graph has one head to hang
                // the rest of the chain off.
                graph.connect(channel.ids.midiClips, channel.ids.instrument);
                graph.connect(sourceHead, channel.ids.instrument);
                head = channel.ids.instrument;
            } else {
                channel.ids.instrument = engine::kInvalidNode;
            }
        } else {
            channel.midiClips.reset();
        }

        // The sampler owns a private post-instrument strip. It is deliberately
        // completed before the channel merge point: monitored input, track
        // routing and sends entering this channel must never pass through FX
        // that belong to one sample instrument.
        const bool samplerOwned = track.samplerOwned;
        if (samplerOwned) {
            if (!channel.samplerFader) {
                channel.samplerFader =
                    std::make_shared<engine::GainNode>(track.name + " Sampler Level");
            }
            if (!channel.samplerMeter) {
                channel.samplerMeter =
                    std::make_shared<engine::MeterNode>(track.name + " Sampler Meter");
            }
            channel.samplerFader->setGain(track.samplerVolume);
            channel.samplerFader->setPan(track.samplerPan);
            channel.ids.samplerFader = graph.adoptNode(channel.samplerFader);
            channel.ids.samplerMeter = graph.adoptNode(channel.samplerMeter);

            if (channel.ids.instrument != engine::kInvalidNode) {
                const engine::NodeId samplerChainEnd = connectSlots(
                    graph, channel.samplerInserts, channel.ids.samplerInserts,
                    channel.ids.instrument);
                graph.connect(samplerChainEnd, channel.ids.samplerFader);
                graph.connect(channel.ids.samplerFader, channel.ids.samplerMeter);
                head = channel.ids.samplerMeter;
            }
        } else {
            channel.samplerFader.reset();
            channel.samplerMeter.reset();
        }

        // A synth in the first Audio FX slot is a source. Complete it before
        // merging monitored/routed audio, then color its audio output.
        const bool firstFxInstrument = channel.ids.instrument == engine::kInvalidNode &&
            !channel.inserts.empty() && channel.inserts.front().node &&
            channel.inserts.front().node->instance() &&
            channel.inserts.front().node->instance()->descriptor().isInstrument;
        std::vector<engine::NodeId> firstFxIds;
        if (firstFxInstrument) {
            head = connectSlots(graph, std::span(channel.inserts).first(1), firstFxIds, head);
            if (channel.ids.midiClips != engine::kInvalidNode) {
                graph.connect(channel.ids.midiClips, firstFxIds.front());
                if (channel.inserts.front().rightSelectorId != engine::kInvalidNode)
                    graph.connect(channel.ids.midiClips, channel.inserts.front().rightSelectorId);
            }
        }

        // Anything routed into this track joins here, ahead of COLOR and FX.
        // Without it the arriving audio would have to be connected straight to
        // the fader — which is exactly how a bus used to end up passing signal
        // through while its plugins did nothing.
        if (receivers.contains(track.id) || channel.ids.input != engine::kInvalidNode) {
            if (!channel.sum) {
                channel.sum = std::make_shared<engine::SumNode>(track.name + " In");
            }
            channel.ids.sum = graph.adoptNode(channel.sum);
            graph.connect(head, channel.ids.sum);
            if (channel.ids.input != engine::kInvalidNode) graph.connect(channel.ids.input, channel.ids.sum);
            head = channel.ids.sum;
        } else {
            channel.sum.reset();
        }

        channel.ids.sourceTap = head;
        head = connectMiniModules(graph, channel, track.miniModules, false, head, &channel.ids.channelColor);
        engine::NodeId chainEnd;
        if (firstFxInstrument) {
            chainEnd = connectRackSlots(graph, channel, std::span(channel.inserts).subspan(1), track.rackGroups, head);
            channel.ids.inserts.insert(channel.ids.inserts.begin(), firstFxIds.begin(), firstFxIds.end());
        } else chainEnd = connectRackSlots(graph, channel, channel.inserts, track.rackGroups, head);

        // With no instrument, the notes still have to reach the first insert —
        // that is where a user who loaded a synth into slot 1 expects them.
        // Routing them through the audio clip player instead would drop them:
        // that node is a source and writes only silence to its MIDI output.
        if (channel.ids.midiClips != engine::kInvalidNode &&
            channel.ids.instrument == engine::kInvalidNode &&
            !firstFxInstrument &&
            !channel.ids.inserts.empty()) {
            graph.connect(channel.ids.midiClips, channel.ids.inserts.front());
            if (!channel.inserts.empty() &&
                channel.ids.inserts.front() == channel.inserts.front().leftSelectorId &&
                channel.inserts.front().channelMode ==
                    PluginChannelMode::DualMono &&
                channel.inserts.front().rightSelectorId !=
                    engine::kInvalidNode) {
                graph.connect(channel.ids.midiClips,
                              channel.inserts.front().rightSelectorId);
            }
        }

        engine::NodeId firstPostMini = engine::kInvalidNode;
        chainEnd = connectMiniModules(graph, channel, track.miniModules, true, chainEnd, &firstPostMini);
        if (channel.ids.channelColor == engine::kInvalidNode && channel.ids.inserts.empty())
            channel.ids.channelColor = firstPostMini;
        channel.ids.preFaderTap = chainEnd;
        graph.connect(chainEnd, channel.ids.fader);
        if (track.frozenAudio) frozenRanges.push_back(
            {&track, firstOwned, engine::NodeId(graph.nodeCount())});
    }
    for (const auto& owned : frozenRanges) {
        const auto& track = *owned.track;
        auto& channel = channels.at(track.id);
        if (!channel.frozenPlayer) channel.frozenPlayer =
            std::make_shared<engine::ClipPlayerNode>(track.name + " Frozen");
        auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>();
        engine::ClipPlacement clip; clip.audio = track.frozenAudio;
        clip.lengthSamples = track.frozenFrames;
        clip.edgeTransitionsRendered = true;
        clips->push_back(std::move(clip));
        channel.frozenPlayer->setClips(std::move(clips));
        const auto fader = channel.ids.fader, meter = channel.ids.meter;
        for (auto node = owned.first; node < owned.end; ++node)
            if (node != fader && node != meter) (void)graph.removeNode(node);
        clearGraphIds(channel.instrument);
        clearGraphIds(channel.miniModules);
        clearGraphIds(channel.samplerInserts);
        clearGraphIds(channel.inserts);
        for (auto& [clipId, clipFx] : channel.clipFx) clearGraphIds(clipFx);
        channel.ids = TrackNodes{};
        channel.ids.fader = fader; channel.ids.meter = meter;
        channel.ids.clips = graph.adoptNode(channel.frozenPlayer);
        channel.ids.preFaderTap = channel.ids.sourceTap = channel.ids.clips;
        graph.connect(channel.ids.clips, fader);
    }

    // ── Main outputs and sends, once every channel exists ──

    // Where audio arriving from elsewhere has to land. The merge point when the
    // channel has one; otherwise the head of its insert chain, so that even a
    // channel built before the receiver set was known still gets its plugins
    // fed. Never the fader: that is what skipped the inserts.
    auto channelEntry = [](const TrackChannel& channel) {
        if (channel.ids.sum != engine::kInvalidNode) return channel.ids.sum;
        if (channel.ids.channelColor != engine::kInvalidNode) return channel.ids.channelColor;
        return channel.ids.inserts.empty() ? channel.ids.fader
                                           : channel.ids.inserts.front();
    };

    // Bounce clips are ordinary timeline players with a semantic output jack.
    // Their private player keeps them out of the owning track's normal source
    // sum; connect them exactly once after the stage already baked into audio.
    for (const auto& ownerTrack : spec.channels) {
        const auto owner = channels.find(ownerTrack.id);
        if (owner == channels.end()) continue;
        for (const auto& clip : ownerTrack.clipFx) {
            if (!clip.playbackInjection.active()) continue;
            const auto player = owner->second.clipFx.find(clip.id);
            if (player == owner->second.clipFx.end() ||
                player->second.meterId == engine::kInvalidNode) {
                continue;
            }
            const std::string anchorId =
                clip.playbackInjection.anchorChannelId.empty()
                    ? ownerTrack.id
                    : clip.playbackInjection.anchorChannelId;
            const auto anchor = channels.find(anchorId);
            engine::NodeId destination = engine::kInvalidNode;
            switch (clip.playbackInjection.stage) {
                case PlaybackInjectionStage::TrackSource:
                    destination = anchor != channels.end()
                                      ? channelEntry(anchor->second)
                                      : channelEntry(owner->second);
                    break;
                case PlaybackInjectionStage::BeforeTrackFader:
                case PlaybackInjectionStage::BeforeFolderFader:
                    destination = anchor != channels.end()
                                      ? anchor->second.ids.fader
                                      : owner->second.ids.fader;
                    break;
                case PlaybackInjectionStage::BeforeMasterFx:
                    destination = masterSumId;
                    break;
                case PlaybackInjectionStage::BeforeMasterFader:
                    destination = masterFaderId;
                    break;
                case PlaybackInjectionStage::None:
                    break;
            }
            if (destination != engine::kInvalidNode)
                graph.connect(player->second.meterId, destination);
        }
    }

    for (const auto& track : spec.channels) {
        auto found = channels.find(track.id);
        if (found == channels.end()) continue;
        TrackChannel& channel = found->second;

        engine::NodeId destination = masterSumId;
        if (!track.outputBusId.empty()) {
            auto bus = channels.find(track.outputBusId);
            if (bus != channels.end()) destination = channelEntry(bus->second);
        }
        const auto destinationSpec = std::find_if(spec.channels.begin(), spec.channels.end(),
            [&](const auto& s) { return s.id == track.outputBusId; });
        if (!track.renderControlOnly || (destinationSpec != spec.channels.end() && destinationSpec->renderControlOnly))
            graph.connect(channel.ids.meter, destination);

        channel.sends.resize(track.sends.size());
        channel.sendIds.clear();
        channel.sendIds.reserve(track.sends.size());
        for (const auto& send : track.sends) channel.sendIds.push_back(send.id);
        for (std::size_t i = 0; i < track.sends.size(); ++i) {
            const SendModel& send = track.sends[i];
            auto bus = channels.find(send.destinationTrackId);
            if (bus == channels.end()) continue;

            if (!channel.sends[i]) {
                channel.sends[i] =
                    std::make_shared<engine::SendNode>(track.name + " Send");
            }
            channel.sends[i]->setLevel(send.level);
            channel.sends[i]->setEnabled(send.enabled);
            const engine::NodeId sendId = graph.adoptNode(channel.sends[i]);
            channel.ids.sends.push_back(sendId);

            // Pre-fader taps the end of the insert chain — before the fader,
            // after the plugins, which is what "pre-fader" means everywhere.
            // Post-fader taps after the meter, so a fader move is heard on the
            // send too.
            graph.connect(send.preFader ? channel.ids.preFaderTap
                                        : channel.ids.meter,
                          sendId);
            graph.connect(sendId, channelEntry(bus->second));
        }
    }

    // ── Stem taps ──
    //
    // Leaves, deliberately: hanging one off a node that already has consumers
    // adds an edge and nothing else, so the compiled order, the buffer
    // assignment and the latency of everything else are exactly what they would
    // have been without it. Splicing a node into the chain instead would move
    // the PDC and the stems would no longer belong to the mix they came with.
    for (auto& [channelId, tap] : renderTaps) {
        auto found = channels.find(channelId);
        if (found == channels.end() || !tap) continue;
        const TrackNodes& ids = found->second.ids;
        const engine::NodeId source =
            renderTapsAtSource ? ids.sourceTap
            : renderTapsPreFader ? ids.preFaderTap
                                   : ids.meter;
        if (source == engine::kInvalidNode) continue;
        graph.connect(source, graph.adoptNode(tap));
    }

    // Sidechains are wired after every strip and meter exists. They are typed
    // edges: PluginNode sends them to auxiliary bus 1 and never sums them into
    // the main/dry signal. The graph's ordinary PDC aligns them automatically.
    const auto connectSidechains = [&](TrackChannel& channel,
                                      const std::vector<AudioGraphSpec::SidechainRoute>& routes) {
        std::unordered_map<std::string, const std::vector<std::string>*> sources;
        for (const auto& route : routes) sources.emplace(route.slotId, &route.sourceChannelIds);
        const auto connect = [&](std::vector<InsertSlot>& slots) {
            for (const auto& slot : slots) {
                if (slot.nodeId == engine::kInvalidNode) continue;
                const auto route = sources.find(slot.slotId);
                if (route == sources.end()) continue;
                for (const auto& sourceId : *route->second) {
                    const auto source = channels.find(sourceId);
                    if (source == channels.end() || source->second.ids.meter == engine::kInvalidNode) continue;
                    graph.connect(source->second.ids.meter, slot.nodeId, engine::InputRole::Sidechain);
                    if (slot.rightNodeId != engine::kInvalidNode)
                        graph.connect(source->second.ids.meter, slot.rightNodeId, engine::InputRole::Sidechain);
                }
            }
        };
        connect(channel.instrument);
        connect(channel.samplerInserts);
        for (auto& [clipId, clipFx] : channel.clipFx) connect(clipFx.inserts);
        connect(channel.inserts);
    };
    connectSidechains(masterChannel, spec.masterSidechains);
    for (const auto& track : spec.channels) connectSidechains(channels.at(track.id), track.sidechains);

    connectModulation(masterChannel, AudioGraphSpec::masterChannelId, spec.masterModulation);
    for (const auto& track : spec.channels) connectModulation(channels.at(track.id), track.id, track.modulation);

    if (!spec.auditionCapture.empty()) {
        const auto channel = channels.find(spec.auditionCapture);
        if (channel != channels.end()) graph.setSink(channel->second.ids.meter);
    }
}

audio::Result AudioRuntime::commitGraph(bool reconfigurePlugins) {
    // Retain the exact editable topology that was published, including render
    // taps. Recovery replaces only its failed processor; replaying an older
    // document projection here would reset current fader/input/content edits.
    auto candidate = engine.graph();
    // Activation can replace a VST3 parameter table. Keep the renderer gated
    // until stable IDs have been resolved against that new table as well.
    std::unique_ptr<engine::RealtimeEngine::RenderGate> gate;
    if (reconfigurePlugins) gate = std::make_unique<engine::RealtimeEngine::RenderGate>(engine);
    const auto result = engine.commitGraph(reconfigurePlugins);
    if (result) {
        publishedGraph = std::move(candidate);
        hasPublishedGraph = true;
        refreshMasterSafetyMute();
    }
    if (result && reconfigurePlugins)
        for (auto& [id, channel] : channels) applyPluginAutomation(channel);
    return result ? audio::Result::ok() : audio::Result::fail(
        audio::EngineError::InvalidArgument, std::string(engine::describe(result.error())));
}

void AudioRuntime::suspendRecordingClipFx(std::span<const std::string> recordingTracks) {
    std::vector<std::shared_ptr<plugins::PluginNode>> dormant;
    for (const auto& trackId : recordingTracks) {
        const auto channel = channels.find(trackId);
        if (channel == channels.end()) continue;
        for (const auto& [clipId, clipFx] : channel->second.clipFx)
            for (const auto& slot : clipFx.inserts)
                for (const auto& node : {slot.node, slot.rightNode})
                    if (node && node->instance() && node->instance()->isActive())
                        dormant.push_back(node);
    }
    if (dormant.empty()) return;
    // Publication alone does not finish the preceding block. Wait for its
    // workers, then release the gate BEFORE any plugin lifecycle calls: the
    // published graph no longer references these processors. In particular,
    // VST2 clears its stream through mains transitions, not realtime reset().
    { const engine::RealtimeEngine::RenderGate gate(engine); }
    for (const auto& node : dormant) {
        node->suspend();
        node->instance()->deactivate();
        node->reset();
        node->invalidatePrepare();
    }
}

std::shared_ptr<const engine::CompiledGraph> AudioRuntime::routingGraph() const {
    return engine.sessionGraph();
}

const AudioRuntime::TrackNodes* AudioRuntime::trackNodes(const std::string& channelId) const {
    const auto found = channels.find(channelId);
    return found == channels.end() ? nullptr : &found->second.ids;
}

} // namespace daw
