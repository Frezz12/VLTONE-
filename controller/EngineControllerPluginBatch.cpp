#include "EngineController.hpp"
#include "plugins/PluginConvert.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <filesystem>
#include <fstream>

namespace daw {
namespace {
audio::Result batchError(const std::string& message) {
    return audio::Result::fail(audio::EngineError::InvalidArgument, message);
}

bool capturedSide(const AudioPluginStateSnapshot& snapshot,
                  EngineController::ChainSlotSnapshot& slot, bool right = false) {
    auto& bytes = right ? slot.rightState : slot.state;
    auto& parameters = right ? slot.model.rightParameters : slot.model.parameters;
    const auto format = slot.model.format;
    if (!snapshot.exists || (snapshot.failed && !snapshot.isolated) ||
        (snapshot.supportsState && !snapshot.stateCaptured)) return false;
    bytes = snapshot.state;
    (right ? slot.rightSource : slot.source) = snapshot.sample;
    (right ? slot.rightSourcePath : slot.sourcePath) = snapshot.samplePath;
    // Native AU controls need not notify the parameter mirror in the document.
    if (snapshot.failed || format == PluginFormat::AudioUnit ||
        (!snapshot.supportsState && !snapshot.documentParametersAuthoritative))
        parameters = snapshot.parameters;
    appendMissingParameters(parameters, snapshot.parameters);
    overlayPendingParameters(parameters, snapshot.pending);
    return true;
}


}

bool EngineController::submitSharedPluginSnapshotBatch(std::shared_ptr<collab::BatchCommand> batch,
    const std::vector<ChainSlotSnapshot>& snapshots, std::string label) {
    if (!sharedEditingAllowed() || !m_sharedMutationSink || m_sharedMutationSink->commandSchemaVersion() < 6) return false;
    for (const auto& source : snapshots) {
        const auto descriptor = m_pluginManager.find(toHostFormat(source.model.format), source.model.uid);
        if (!descriptor || !sharedPluginAllowed(*descriptor) ||
            (source.model.format != PluginFormat::Internal && descriptor->version != source.model.pluginVersion)) return false;
        if ((!source.model.stateFile.empty() && source.state.empty() && source.model.stateAsset.empty()) ||
            (!source.model.rightStateFile.empty() && source.rightState.empty() && source.model.rightStateAsset.empty())) return false;
        if (source.state.size() > 64u * 1024u * 1024u || source.rightState.size() > 64u * 1024u * 1024u) return false;
    }
    std::vector<std::string> files;
    struct Binding { std::size_t source; bool right; };
    std::vector<Binding> bindings;
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto& source = snapshots[i];
        for (bool right : {false, true}) {
            if (right && source.model.channelMode != PluginChannelMode::DualMono) continue;
            const auto& bytes = right && !source.rightState.empty() ? source.rightState : source.state;
            if (bytes.empty()) continue;
            const auto path = std::filesystem::temp_directory_path() / ("vlt-batch-state-" + newUuid() + ".bin");
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); output.close();
            files.push_back(platform::pathToUtf8(path));
            if (!output) { for (const auto& file : files) { std::error_code ignored; std::filesystem::remove(platform::pathFromUtf8(file), ignored); } return false; }
            bindings.push_back({i, right});
        }
    }
    const auto sourceCount = snapshots.size();
    return submitSharedDerivedMutation(std::move(files), AssetKind::PluginState,
        [batch, bindings, sourceCount](const auto& assets) -> collab::CommandBody {
            std::size_t next = 0;
            for (auto& command : batch->commands) if (auto* add = std::get_if<collab::AddPluginInsert>(&command.body);
                add && add->location.chain != collab::PluginChain::ChannelColor) {
                // The fixed COLOR stage has complete inline settings. It is
                // not a copied FX slot and must not consume an FX state asset.
                if (!sourceCount) return std::make_shared<collab::BatchCommand>();
                const auto source = next++ % sourceCount;
                for (std::size_t i = 0; i < bindings.size(); ++i) if (bindings[i].source == source)
                    (bindings[i].right ? add->insert.rightStateAsset : add->insert.stateAsset) = assets[i];
            }
            return batch;
        }, projectRevision(), std::move(label));
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

audio::Result EngineController::captureInsertState(const std::string& channelId,
    const InsertModel& model, ChainSlotSnapshot& slot) {
    slot = {};
    slot.model = model;
    const auto id = channelId.empty() ? kMasterChannelId : channelId;
    std::vector<AudioPluginStateRequest> requests{{{id, model.id}}};
    if (model.channelMode == PluginChannelMode::DualMono) requests.push_back({{id, model.id, true}});
    for (auto& request : requests) request.purpose = AudioPluginSnapshotPurpose::RecoverFailed;
    const auto snapshots = m_runtime.pluginStateSnapshots(requests);
    const bool left = capturedSide(snapshots[0], slot);
    const bool right = model.channelMode != PluginChannelMode::DualMono ||
        capturedSide(snapshots[1], slot, true);
    return left && right ? audio::Result::ok()
                        : batchError("Could not capture settings for " + model.name);
}

audio::Result EngineController::captureInsertChain(const std::string& channelId,
    const std::vector<std::string>& slotIds, std::vector<ChainSlotSnapshot>& chain) {
    chain.clear();
    std::vector<ChainSlotSnapshot> captured;
    std::vector<AudioPluginStateRequest> requests;
    captured.reserve(slotIds.size());
    for (const auto& id : slotIds) {
        const auto* model = insertModel(channelId, id);
        if (!model) return batchError("A plugin slot is no longer available.");
        captured.push_back({});
        captured.back().model = *model;
        const auto channel = channelId.empty() ? kMasterChannelId : channelId;
        requests.push_back({{channel, id}});
        if (model->channelMode == PluginChannelMode::DualMono) requests.push_back({{channel, id, true}});
    }
    for (auto& request : requests) request.purpose = AudioPluginSnapshotPurpose::RecoverFailed;
    const auto snapshots = m_runtime.pluginStateSnapshots(requests);
    std::size_t next = 0;
    for (auto& slot : captured) {
        if (!capturedSide(snapshots[next++], slot) ||
            (slot.model.channelMode == PluginChannelMode::DualMono &&
             !capturedSide(snapshots[next++], slot, true)))
            return batchError("Could not capture settings for " + slot.model.name);
    }
    chain = std::move(captured);
    return audio::Result::ok();
}

audio::Result EngineController::capturePluginBatchChain(
    const PluginBatchTarget& target, const std::vector<std::string>& slotIds,
    ChannelSnapshot& chain) {
    chain = {};
    const auto* models = target.clipId.empty() ? channelInserts(target.trackId)
                                              : clipFx(target.trackId, target.clipId);
    if (!models) return batchError("The source is no longer available.");
    for (const auto& id : slotIds)
        if (std::none_of(models->begin(), models->end(), [&](const auto& slot) { return slot.id == id; }))
            return batchError("A draft effect is no longer available.");
    return captureInsertChain(target.trackId, slotIds, chain.inserts);
}

audio::Result EngineController::appendPluginBatch(
    const std::vector<PluginBatchTarget>& targets, const ChannelSnapshot& chain,
    std::vector<std::vector<std::string>>& addedIds) {
    addedIds.clear();
    if (const auto valid = validatePluginBatch(targets, chain.inserts.size()); !valid) return valid;
    if (chain.inserts.empty()) return batchError("Add an effect first.");
    if (isRecording() || m_exportInProgress || m_pluginAuditionOwner)
        return batchError("Stop recording, rendering or preview before applying plugins.");
    if (cloudProjectBound()) {
        if (!sharedEditingAllowed() || !m_sharedAssetMutationSink || m_sharedMutationSink->commandSchemaVersion() < 6)
            return batchError("Configured plugins require an editable connected session.");
        std::vector<InsertModel> models;
        std::vector<std::string> files;
        struct Binding { std::size_t model; bool right; };
        std::vector<Binding> bindings;
        for (const auto& source : chain.inserts) {
            const auto descriptor = m_pluginManager.find(toHostFormat(source.model.format), source.model.uid);
            if (!descriptor || descriptor->isInstrument || !sharedPluginAllowed(*descriptor) ||
                (source.model.format != PluginFormat::Internal && source.model.pluginVersion != descriptor->version))
                return batchError("This effect is not in the session's compatible catalog: " + source.model.name);
        }
        for (const auto& source : chain.inserts) {
            const auto descriptor = m_pluginManager.find(toHostFormat(source.model.format), source.model.uid);
            if (!descriptor || descriptor->isInstrument || !sharedPluginAllowed(*descriptor))
                return batchError("This effect is not in the session's compatible catalog: " + source.model.name);
            auto model = source.model;
            model.path.clear(); model.stateFile.clear(); model.rightStateFile.clear();
            model.windowOpen = false; model.windowX = model.windowY = model.windowWidth = model.windowHeight = 0;
            model.editorChannel = PluginEditorChannel::Left; model.sidechainTrackIds.clear();
            models.push_back(std::move(model));
            for (bool right : {false, true}) {
                if (right && source.model.channelMode != PluginChannelMode::DualMono) continue;
                const auto& bytes = right && !source.rightState.empty() ? source.rightState : source.state;
                if (bytes.empty()) continue;
                const auto path = std::filesystem::temp_directory_path() / ("vlt-batch-state-" + newUuid() + ".bin");
                std::ofstream output(path, std::ios::binary);
                if (bytes.size() <= 64u * 1024u * 1024u)
                    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                output.close();
                if (!output || bytes.size() > 64u * 1024u * 1024u) {
                    files.push_back(platform::pathToUtf8(path));
                    for (const auto& file : files) { std::error_code ignored; std::filesystem::remove(platform::pathFromUtf8(file), ignored); }
                    return batchError("Could not stage plugin batch state (maximum 64 MiB per channel).");
                }
                files.push_back(platform::pathToUtf8(path)); bindings.push_back({models.size() - 1, right});
            }
        }
        std::vector<std::vector<std::string>> ids;
        for (const auto& target : targets) { auto& row = ids.emplace_back(); for (const auto& model : models) row.push_back(newUuid()); }
        const bool queued = submitSharedDerivedMutation(std::move(files), AssetKind::PluginState,
            [targets, models = std::move(models), bindings, ids, this](const auto& assets) mutable -> collab::CommandBody {
                for (std::size_t i = 0; i < assets.size(); ++i) {
                    auto& model = models[bindings[i].model];
                    (bindings[i].right ? model.rightStateAsset : model.stateAsset) = assets[i];
                }
                auto batch = std::make_shared<collab::BatchCommand>();
                for (std::size_t t = 0; t < targets.size(); ++t) {
                    const auto& target = targets[t];
                    const auto* current = target.clipId.empty() ? channelInserts(target.trackId) : clipFx(target.trackId, target.clipId);
                    if (!current) return std::make_shared<collab::BatchCommand>();
                    std::string anchor = current->empty() ? std::string{} : current->back().id;
                    for (std::size_t i = 0; i < models.size(); ++i) {
                        auto model = models[i]; model.id = ids[t][i];
                        collab::ProjectCommand command;
                        command.body = collab::AddPluginInsert{{target.clipId.empty() ? collab::PluginChain::Track : collab::PluginChain::Clip,
                            target.trackId, target.clipId}, model, anchor};
                        anchor = model.id; batch->commands.push_back(std::move(command));
                    }
                }
                return batch;
            }, projectRevision(), "Apply Shared Plugins");
        if (!queued) return batchError("The project changed or plugin state upload could not start.");
        addedIds = std::move(ids); return audio::Result::ok();
    }
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
            model.sidechainTrackIds.clear();
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
        // Failed applySession retains the acknowledged graph and native owners.
        const auto rollback = [&] { writeModels(!after); };
        try {
        std::vector<AudioPluginStateEdit> restores;
        if (after) {
            for (const auto& state : *states) {
                for (std::size_t i = 0; i < settings->inserts.size(); ++i) {
                    auto saved = settings->inserts[i];
                    saved.model = state.after[state.before.size() + i];
                    appendInsertStateEdits(restores, state.target.trackId, saved);
                }
            }
        }
        const auto committed = rebuildGraph(false, AudioPluginLoadPolicy::Required, restores);
        if (!committed) { rollback(); return batchError(committed.message()); }
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
    auto draft = std::make_shared<EngineController>(SecondaryRuntime{}, *this);
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
                    if (!slot.bypassed)
                        for (const auto& source : slot.sidechainTrackIds)
                            changed |= upstream.insert(source).second;
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
    applyRenderSelection(selection, draft->m_project);
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
    std::unordered_map<std::string, const std::vector<std::uint8_t>*> states;
    for (const auto& state : snapshot.pluginStates) states[state.fileName] = &state.bytes;
    std::vector<AudioPluginStateEdit> restores;
    const auto restore = [&](const std::string& track, const InsertModel& model) {
        if (!model.isLoaded()) return true;
        const auto bytes = [&](const std::string& file) -> const std::vector<std::uint8_t>& {
            static const std::vector<std::uint8_t> empty;
            const auto found = states.find(file);
            return found == states.end() ? empty : *found->second;
        };
        ChainSlotSnapshot saved;
        saved.model = model; saved.state = bytes(model.stateFile); saved.rightState = bytes(model.rightStateFile);
        draft->appendInsertStateEdits(restores, track, saved);
        return true;
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
    auto session = draft->prepareAudioSession(AudioPluginLoadPolicy::Required);
    // The former draft allowed an unavailable bypassed slot, but required every
    // audible source processor. Preserve that policy independently per slot.
    for (auto& chain : session.pluginChains) for (auto& slot : chain.slots)
        if (slot.bypassed) slot.loadPolicy = AudioPluginLoadPolicy::PreserveUnavailable;
    if (const auto built = draft->publishAudioSession(std::move(session), false, restores); !built) return built;
    draft->m_runtime.transportCommand({.action = AudioTransportCommand::Action::Tempo, .value = m_project.tempo});
    draft->m_runtime.transportCommand({.action = AudioTransportCommand::Action::TimeSignature,
        .numerator = m_project.timeSigNumerator, .denominator = m_project.timeSigDenominator});
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

audio::Result EngineController::startPluginAudition(std::shared_ptr<EngineController> draft) {
    if (!draft || draft.get() == this || draft->m_liveDeviceAllowed ||
        draft->m_sampleRate != m_sampleRate || draft->m_bufferSize != m_bufferSize ||
        isRecording() || m_exportInProgress || m_pluginAuditionOwner)
        return batchError("Plugin preview is unavailable right now.");
    draft->stopPreview();
    draft->flushDeferredClipSync();
    draft->flushSamplerPrecompute();
    draft->applyTransportStartPolicy();
    const auto result = m_runtime.startAudition(draft->m_audioRuntime);
    if (!result) return result;
    m_pluginAuditionOwner = std::move(draft);
    m_pluginAuditionOwner->m_externalPreviewDriven = true;
    return audio::Result::ok();
}

void EngineController::stopPluginAudition() {
    if (!m_pluginAuditionOwner) return;
    m_runtime.stopAudition();
    m_pluginAuditionOwner->m_externalPreviewDriven = false;
    m_pluginAuditionOwner.reset();
}
} // namespace daw
