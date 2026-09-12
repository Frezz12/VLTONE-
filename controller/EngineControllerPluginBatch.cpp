#include "EngineController.hpp"
#include "plugins/PluginConvert.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace daw {
namespace {
audio::Result batchError(const std::string& message) {
    return audio::Result::fail(audio::EngineError::InvalidArgument, message);
}
}

audio::Result EngineController::validatePluginBatch(
    const std::vector<PluginBatchTarget>& targets, std::size_t addedSlots) const {
    if (targets.empty()) return batchError("Select tracks or audio clips.");
    const bool clips = !targets.front().clipId.empty();
    std::unordered_set<std::string> seen;
    for (const auto& target : targets) {
        if (clips != !target.clipId.empty())
            return batchError("Track inserts and Clip FX cannot be combined in one batch.");
        if (!seen.insert(target.trackId + "/" + target.clipId).second)
            return batchError("A target is selected more than once.");
        const auto* track = m_project.findTrack(target.trackId);
        if (!track || !carriesAudio(*track))
            return batchError("A selected track does not support inserts.");
        if (clips) {
            const auto* slots = clipFx(target.trackId, target.clipId);
            if (!slots) return batchError("Clip FX are available for audio clips only.");
            if (slots->size() + addedSlots > kSamplerFxSlots)
                return batchError("Not enough free Clip FX slots on every selected clip.");
        }
    }
    return audio::Result::ok();
}

audio::Result EngineController::capturePluginBatchChain(
    const PluginBatchTarget& target, const std::vector<std::string>& slotIds,
    ChannelSnapshot& chain) {
    chain = {};
    const engine::RealtimeEngine::RenderGate gate(m_engine);
    const auto* models = target.clipId.empty() ? channelInserts(target.trackId)
                                              : clipFx(target.trackId, target.clipId);
    if (!models) return batchError("The source is no longer available.");
    for (const auto& id : slotIds) {
        const auto model = std::find_if(models->begin(), models->end(),
            [&](const auto& slot) { return slot.id == id; });
        if (model == models->end()) return batchError("A draft effect is no longer available.");
        auto* live = liveInsertSlot(target.trackId, id);
        if (!live || !live->node || !live->node->instance())
            return batchError("A draft plugin is not loaded.");
        ChainSlotSnapshot slot;
        slot.model = *model;
        if (!live->node->instance()->saveState(slot.state))
            return batchError("Could not capture settings for " + model->name);
        if (model->channelMode == PluginChannelMode::DualMono &&
            (!live->rightNode || !live->rightNode->instance() ||
             !live->rightNode->instance()->saveState(slot.rightState)))
            return batchError("Could not capture the right-channel plugin settings.");
        // Audio Unit native controls may not notify the host. A stale mirror
        // must never overwrite the freshly captured vendor state on Apply.
        if (model->format == PluginFormat::AudioUnit) {
            const auto refresh = [](const auto& node, auto& parameters) {
                parameters.clear();
                const auto& infos = node->instance()->parameters();
                for (std::size_t i = 0; i < infos.size(); ++i) {
                    const double value = node->instance()->parameterValue(std::uint32_t(i));
                    if (std::isfinite(value)) parameters.push_back({infos[i].id, value});
                }
            };
            refresh(live->node, slot.model.parameters);
            if (model->channelMode == PluginChannelMode::DualMono) refresh(live->rightNode, slot.model.rightParameters);
        }
        chain.inserts.push_back(std::move(slot));
    }
    return audio::Result::ok();
}

audio::Result EngineController::appendPluginBatch(
    const std::vector<PluginBatchTarget>& targets, const ChannelSnapshot& chain,
    std::vector<std::vector<std::string>>& addedIds) {
    addedIds.clear();
    if (const auto valid = validatePluginBatch(targets, chain.inserts.size()); !valid) return valid;
    if (chain.inserts.empty()) return batchError("Add an effect first.");
    if (isRecording() || m_exportInProgress || m_pluginAuditionNode)
        return batchError("Stop recording, rendering or preview before applying plugins.");
    // Native state chunks have no shared mutation representation yet, as in
    // createTracks. Never turn configured vendor plugins into default copies.
    if (cloudProjectBound()) return audio::Result::fail(audio::EngineError::NotSupported,
        "Configured plugin batches are available in local projects.");
    for (const auto& slot : chain.inserts) {
        const auto descriptor = m_pluginManager.find(toHostFormat(slot.model.format), slot.model.uid);
        if (!descriptor || descriptor->isInstrument)
            return batchError("This effect is unavailable: " + slot.model.name);
    }
    struct TargetState {
        PluginBatchTarget target;
        std::vector<InsertModel> before, after;
        TrackFreezeState freeze;
    };
    const auto states = std::make_shared<std::vector<TargetState>>();
    const auto settings = std::make_shared<ChannelSnapshot>(chain);
    for (const auto& target : targets) {
        TargetState state;
        state.target = target;
        state.before = target.clipId.empty() ? *channelInserts(target.trackId)
                                             : *clipFx(target.trackId, target.clipId);
        state.after = state.before;
        state.freeze = m_project.findTrack(target.trackId)->freeze;
        for (const auto& source : chain.inserts) {
            auto model = source.model;
            model.id = newUuid();
            model.stateFile.clear(); model.rightStateFile.clear();
            model.stateAsset = {}; model.rightStateAsset = {};
            model.sidechainTrackId.clear();
            model.windowOpen = false;
            model.windowX = model.windowY = model.windowWidth = model.windowHeight = 0;
            state.after.push_back(std::move(model));
        }
        states->push_back(std::move(state));
    }
    const auto writeModels = [this, states](bool after) {
        for (const auto& state : *states) {
            if (state.target.clipId.empty() ? !channelInserts(state.target.trackId)
                : !clipFx(state.target.trackId, state.target.clipId)) return false;
        }
        for (const auto& state : *states) {
            auto* slots = state.target.clipId.empty() ? mutableChannelInserts(state.target.trackId)
                : mutableClipFxInserts(state.target.trackId, state.target.clipId);
            if (!slots) return false;
            *slots = after ? state.after : state.before;
            if (auto* track = m_project.findTrack(state.target.trackId))
                track->freeze = after ? TrackFreezeState{} : state.freeze;
        }
        return true;
    };
    const auto apply = [this, states, settings, writeModels](bool after) -> audio::Result {
        if (!writeModels(after)) return batchError("A selected target was removed.");
        const auto rollback = [&] { writeModels(!after); (void)rebuildGraph(); };
        try {
        // Newly constructed instances stay unpublished until every opaque
        // state and parameter mirror has been restored successfully.
        if (const auto built = rebuildGraph(false, false); !built) { rollback(); return built; }
        if (after) {
            for (const auto& state : *states) {
                for (std::size_t i = 0; i < settings->inserts.size(); ++i) {
                    const auto& model = state.after[state.before.size() + i];
                    const auto& saved = settings->inserts[i];
                    auto* live = liveInsertSlot(state.target.trackId, model.id);
                    const auto restore = [this](const auto& node, const auto& bytes, const auto& parameters) {
                        if (!node || !node->instance()) return false;
                        if (!bytes.empty() && !node->instance()->loadState(bytes)) return false;
                        node->discardPendingEvents();
                        applyStoredParameters(*node, parameters);
                        node->invalidatePrepare();
                        return true;
                    };
                    bool ok = live && restore(live->node, saved.state, model.parameters);
                    if (ok && model.channelMode == PluginChannelMode::DualMono)
                        ok = restore(live->rightNode, saved.rightState.empty() ? saved.state : saved.rightState,
                                     model.rightParameters.empty() ? model.parameters : model.rightParameters);
                    if (!ok) { rollback(); return batchError("Could not load plugin settings. No plugins were applied."); }
                }
            }
        }
        const auto committed = m_engine.commitGraph();
        if (!committed) { rollback(); return batchError(std::string(engine::describe(committed.error()))); }
        return audio::Result::ok();
        } catch (const std::exception& error) {
            rollback(); return batchError(std::string("Could not apply plugin settings: ") + error.what());
        } catch (...) {
            rollback(); return batchError("Could not apply plugin settings.");
        }
    };
    if (const auto result = apply(true); !result) return result;
    std::size_t bytes = 0;
    for (const auto& slot : chain.inserts) bytes += slot.state.size() + slot.rightState.size();
    m_undo.push("Apply Shared Plugins", [apply] { (void)apply(false); }, [apply] { (void)apply(true); }, bytes);
    for (const auto& state : *states) {
        auto& ids = addedIds.emplace_back();
        for (std::size_t i = state.before.size(); i < state.after.size(); ++i) ids.push_back(state.after[i].id);
    }
    return audio::Result::ok();
}

audio::Result EngineController::createPluginBatchDraft(
    const PluginBatchTarget& source, std::shared_ptr<EngineController>& out) {
    if (const auto valid = validatePluginBatch({source}); !valid) return valid;
    auto snapshot = captureRecoverySnapshot();
    auto draft = std::make_shared<EngineController>();
    if (const auto ready = draft->initialize(m_sampleRate, m_bufferSize, false); !ready) return ready;
    draft->m_pluginManager.copyCatalogFrom(m_pluginManager);
    draft->m_project = std::move(snapshot.project);
    draft->m_sourceSamples = m_sourceSamples;
    draft->m_samples = m_samples;
    draft->m_clipSampleCache = m_clipSampleCache;
    draft->m_sharedClipSampleCache = m_sharedClipSampleCache;
    draft->m_pluginAuditionCapture = source.trackId;
    for (auto& track : draft->m_project.tracks) {
        track.freeze = {};
        track.armed = track.monitor = track.monitorAuto = false;
        track.inputEnabled = false;
    }
    rendering::Spec selection;
    // A bus/folder/Pattern source includes its inputs. Existing sidechains
    // are dependencies too, even when their signal is not mixed into it.
    std::unordered_set<std::string> upstream{source.trackId};
    std::unordered_set<std::string> mutedClips;
    {
        bool changed;
        do {
            changed = false;
            for (const auto& track : draft->m_project.tracks) {
                const auto output = track.outputBusId.empty()
                    ? summingParent(draft->m_project, track.id) : track.outputBusId;
                const bool feeds = upstream.contains(output) || std::any_of(track.sends.begin(), track.sends.end(),
                    [&](const auto& send) { return send.enabled && upstream.contains(send.destinationTrackId); });
                if (feeds) changed |= upstream.insert(track.id).second;
                if (!upstream.contains(track.id)) continue;
                const auto sidechain = [&](const InsertModel& slot) {
                    if (!slot.bypassed && !slot.sidechainTrackId.empty())
                        changed |= upstream.insert(slot.sidechainTrackId).second;
                };
                sidechain(track.instrument);
                for (const auto& slot : track.inserts) sidechain(slot);
                for (const auto& slot : track.samplerFx.inserts) sidechain(slot);
                for (const auto& clip : track.clips) {
                    if (clip.muted) mutedClips.insert(clip.id);
                    if (track.id != source.trackId || source.clipId.empty() || clip.id == source.clipId)
                        for (const auto& slot : clip.inserts) sidechain(slot);
                }
            }
        } while (changed);
    }
    selection.sourceTrackIds.assign(upstream.begin(), upstream.end());
    if (!source.clipId.empty()) {
        selection.sourceClipIds = {source.clipId};
        for (const auto& track : draft->m_project.tracks)
            if (track.id != source.trackId && upstream.contains(track.id))
                for (const auto& clip : track.clips) selection.sourceClipIds.push_back(clip.id);
    }
    draft->applyRenderSelection(selection);
    for (auto& track : draft->m_project.tracks) {
        for (auto& clip : track.clips)
            if (clip.id != source.clipId && mutedClips.contains(clip.id)) clip.muted = true;
        if (track.soloed) continue;
        // Unrelated unavailable plugins must not prevent opening this source,
        // nor consume realtime CPU while its draft is playing.
        track.instrument = {};
        track.inserts.clear(); track.samplerFx = {};
        for (auto& clip : track.clips) clip.inserts.clear();
    }
    draft->m_project.masterInserts.clear();
    if (const auto built = draft->rebuildGraph(); !built) return built;
    std::unordered_map<std::string, const std::vector<std::uint8_t>*> states;
    for (const auto& state : snapshot.pluginStates) states[state.fileName] = &state.bytes;
    const auto restore = [&](const std::string& track, const InsertModel& model) {
        if (!model.isLoaded()) return true;
        auto* live = draft->liveInsertSlot(track, model.id);
        if (!live) return model.bypassed;
        const auto restoreNode = [&](const auto& node, const auto& file, const auto& parameters) {
            if (!node || !node->instance()) return model.bypassed;
            if (const auto bytes = states.find(file); bytes != states.end())
                if (!node->instance()->loadState(*bytes->second)) return false;
            node->discardPendingEvents();
            draft->applyStoredParameters(*node, parameters);
            node->invalidatePrepare();
            return true;
        };
        return restoreNode(live->node, model.stateFile, model.parameters) &&
            (model.channelMode != PluginChannelMode::DualMono ||
             restoreNode(live->rightNode, model.rightStateFile, model.rightParameters));
    };
    for (const auto& track : draft->m_project.tracks) {
        if (!restore(track.id, track.instrument)) return batchError("Could not prepare the source instrument.");
        for (const auto& slot : track.inserts)
            if (!restore(track.id, slot)) return batchError("Could not prepare source effects.");
        for (const auto& slot : track.samplerFx.inserts)
            if (!restore(track.id, slot)) return batchError("Could not prepare source effects.");
        for (const auto& clip : track.clips)
            for (const auto& slot : clip.inserts)
                if (!restore(track.id, slot)) return batchError("Could not prepare source Clip FX.");
    }
    if (const auto built = draft->rebuildGraph(); !built) return built;
    draft->m_engine.transport().setTempo(m_project.tempo);
    draft->m_engine.transport().setTimeSignature(m_project.timeSigNumerator, m_project.timeSigDenominator);
    double start = std::numeric_limits<double>::max(), end = 0.0;
    for (const auto& track : draft->m_project.tracks)
        for (const auto& clip : track.clips)
            if (!clip.muted && clip.kind != ClipKind::Automation) {
                start = std::min(start, clip.startSeconds);
                end = std::max(end, clip.startSeconds + draft->clipPlaybackDuration(clip));
            }
    if (!(end > start)) { start = 0; end = beatsToSeconds(16, m_project.tempo); }
    draft->setLoopRangeSeconds(start, end);
    draft->setLoopEnabled(true);
    draft->seekSeconds(start);
    out = std::move(draft);
    return audio::Result::ok();
}

class EngineController::PluginAuditionNode final : public engine::Node {
public:
    explicit PluginAuditionNode(std::shared_ptr<EngineController> owner) : m_owner(std::move(owner)) {}
    std::string_view name() const noexcept override { return "Plugin draft audition"; }
    bool isSource() const noexcept override { return true; }
    engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::None; }
    void process(const engine::ProcessContext& context) override {
        if (context.offline || context.sampleRate != m_owner->m_sampleRate ||
            context.frames > m_owner->m_bufferSize) {
            for (engine::ChannelCount ch = 0; ch < context.output.numChannels(); ++ch)
                engine::dsp::clear(context.output.channel(ch));
            return;
        }
        m_owner->m_engine.renderBlock(context.output, nullptr, 0, context.frames);
    }
private:
    std::shared_ptr<EngineController> m_owner;
};

audio::Result EngineController::startPluginAudition(std::shared_ptr<EngineController> draft) {
    if (!draft || draft.get() == this || draft->m_liveDeviceAllowed ||
        draft->m_sampleRate != m_sampleRate || draft->m_bufferSize != m_bufferSize ||
        isRecording() || m_exportInProgress || m_pluginAuditionNode)
        return batchError("Plugin preview is unavailable right now.");
    pause();
    stopPreview();
    const engine::RealtimeEngine::RenderGate gate(m_engine);
    m_pluginAuditionOwner = draft;
    draft->m_externalPreviewDriven = true;
    m_pluginAuditionNode = std::make_shared<PluginAuditionNode>(draft);
    const auto result = rebuildGraph();
    if (!result) { stopPluginAudition(); return result; }
    draft->play();
    return audio::Result::ok();
}

void EngineController::stopPluginAudition() {
    if (!m_pluginAuditionNode) return;
    const engine::RealtimeEngine::RenderGate gate(m_engine);
    m_pluginAuditionNode.reset();
    (void)rebuildGraph();
    m_pluginAuditionOwner->pause();
    m_pluginAuditionOwner->m_externalPreviewDriven = false;
    m_pluginAuditionOwner.reset();
}
} // namespace daw
