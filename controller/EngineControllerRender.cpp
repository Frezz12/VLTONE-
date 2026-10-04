#include "EngineController.hpp"
#include "RenderOutput.hpp"
#include "platform/PathUtils.hpp"
#include "Internal/ChannelColorInstance.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace daw {

namespace {

/// Characters a file name cannot carry on one platform or another, plus the
/// ones that would make a name ambiguous in a shell. A track called "Kick / Snr"
/// has to become a file, and it must not become two directories.
std::string sanitizeFileName(std::string name) {
    for (char& c : name) {
        const bool illegal = c == '/' || c == '\\' || c == ':' || c == '*' ||
                             c == '?' || c == '"' || c == '<' || c == '>' ||
                             c == '|';
        if (illegal || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    // Trailing dots and spaces are legal to create on Windows and impossible to
    // delete afterwards.
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    return name.empty() ? std::string("untitled") : name;
}

/// A path nothing else in this render is already writing to. Two tracks are
/// allowed to share a name, and neither should quietly overwrite the other.
std::string uniquePath(const std::string& dir, const std::string& stem,
                       std::string_view extension,
                       std::unordered_set<std::string>& taken) {
    for (int attempt = 1;; ++attempt) {
        std::string name = attempt == 1
                               ? stem
                               : stem + " (" + std::to_string(attempt) + ")";
        const fs::path fileName = platform::pathFromUtf8(
            name + "." + std::string(extension));
        std::string path = platform::pathToUtf8(
            platform::pathFromUtf8(dir) / fileName);
        std::string key = path;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        std::error_code ec;
        if (!fs::exists(platform::pathFromUtf8(path), ec) && !ec &&
            taken.insert(key).second) return path;
        if (ec) throw fs::filesystem_error("cannot select export path", ec);
    }
}

float dbToLinear(double db) { return float(std::pow(10.0, db / 20.0)); }

} // namespace

// ── Offline render ─────────────────────────────────────────────────────────

audio::Result EngineController::renderProject(
    const rendering::Spec& spec,
    const std::function<bool(const rendering::Progress&)>& onProgress,
    rendering::Report& out) {
    out = {};
    const auto nonnegative = [](double value) { return std::isfinite(value) && value >= 0.0; };
    if (!nonnegative(spec.sampleRate) || !nonnegative(spec.preRollSeconds) ||
        !nonnegative(spec.tailSeconds) || !nonnegative(spec.tailMaxSeconds) ||
        !nonnegative(spec.tailHoldSeconds) || !std::isfinite(spec.tailSilenceDb) ||
        !std::isfinite(spec.customStartSeconds) || !std::isfinite(spec.customEndSeconds) ||
        spec.blockSize > 8192)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "invalid render settings");
    if (m_exportInProgress)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "a render is already in progress");
    struct BusyScope {
        bool& flag;
        explicit BusyScope(bool& value) : flag(value) { flag = true; }
        ~BusyScope() { flag = false; }
    } busy(m_exportInProgress);
    bool cancelled = false;
    auto lastProgress = std::chrono::steady_clock::time_point{};
    const auto preparing = [&] {
        if (cancelled) return false;
        const auto now = std::chrono::steady_clock::now();
        if (onProgress && now - lastProgress >= std::chrono::milliseconds(33)) {
            lastProgress = now;
            rendering::Progress progress;
            progress.stage = rendering::Progress::Stage::Preparing;
            cancelled = !onProgress(progress);
        }
        return !cancelled;
    };
    try {
        if (!preparing()) { out.cancelled = true; return audio::Result::ok(); }
        stop();
        // Pending clip edits are already in the document. Bake them in the
        // isolated controller, where progress/cancellation can safely run.

        // Capture on the plugin control thread. The offline controller owns
        // its document, nodes, plugin instances, scheduler and rate; only
        // immutable sample data is shared with the live session. UI timers and
        // queued collaboration updates can therefore run at progress points.
        recovery::RecoverySnapshot snapshot;
        std::unordered_map<std::string, std::vector<InsertParameter>>
            pendingParameterOverrides;
        {
            const engine::RealtimeEngine::RenderGate gate(m_engine);
            snapshot.project = m_project;
            snapshot.project.sampleRate = m_sampleRate;
            applyRenderSelection(spec, snapshot.project);

            // A plugin's native state is the authoritative description of its
            // sound. Some commercial VST3 controllers expose a stale or lossy
            // parameter mirror; replaying every reported value after loadState
            // can therefore undo a valid component state. Keep only host edits
            // that have not reached the live processor yet and replay those on
            // top of the state in the render clone.
            const auto captureNode = [&](plugins::PluginNode* node,
                                            const std::string& key,
                                            std::string& stateFile,
                                            std::vector<InsertParameter>& fallback) {
                if (!node || !node->instance())
                    throw std::runtime_error("cannot capture unavailable render plugin: " + key);
                auto* instance = node->instance();
                stateFile.clear();
                fallback.clear();
                // Export needs a fresh opaque snapshot. Recovery deliberately
                // retains old chunks after a failed save, which must never be
                // used as if they described the sound of this render.
                if (instance->supportsState()) {
                    recovery::RecoverySnapshot::PluginState state;
                    state.fileName = key;
                    if (!instance->saveState(state.bytes))
                        throw std::runtime_error("cannot capture render plugin state: " +
                                                 std::string(node->name()));
                    stateFile = key;
                    snapshot.pluginStates.push_back(std::move(state));
                } else {
                    for (const auto& parameter : instance->parameters()) {
                        const double value = instance->parameterValue(parameter.index);
                        if (!parameter.id.empty() && std::isfinite(value))
                            fallback.push_back({parameter.id, value});
                    }
                }
                const auto events = node->pendingParameterEvents();
                const auto descriptors = node->instance()->parameters();
                auto& values = pendingParameterOverrides[key];
                for (const auto& event : events) {
                    if (event.paramIndex >= descriptors.size() ||
                        !std::isfinite(event.value)) {
                        continue;
                    }
                    const std::string& id = descriptors[event.paramIndex].id;
                    if (id.empty()) continue;
                    const auto existing = std::find_if(
                        values.begin(), values.end(), [&](const auto& value) {
                            return value.id == id;
                        });
                    if (existing != values.end()) existing->value = event.value;
                    else values.push_back({id, event.value});
                }
                if (stateFile.empty()) {
                    for (const auto& pending : values) {
                        const auto found = std::find_if(fallback.begin(), fallback.end(),
                            [&](const auto& value) { return value.id == pending.id; });
                        if (found != fallback.end()) found->value = pending.value;
                        else fallback.push_back(pending);
                    }
                }
                if (values.empty()) pendingParameterOverrides.erase(key);
            };
            const auto collectSlot = [&](const std::string& channelId,
                                         InsertModel& slot) {
                if (!slot.isLoaded() || slot.bypassed || slot.mix == 0.f) return;
                // COLOR's complete configuration is inline: static controls
                // and the saved component seed. Its live parameter mirror may
                // contain the last automation value from a previous playback.
                if (slot.uid == plugins::channel_color::ChannelColorInstance::uid() || slot.uid == plugins::mini::kUid) {
                    slot.stateFile.clear(); return;
                }
                InsertSlot* live = liveInsertSlot(channelId, slot.id);
                if (!live || !live->node || !live->node->instance())
                    throw std::runtime_error("cannot capture unavailable render plugin: " + slot.name);
                captureNode(live->node.get(), slot.id,
                               slot.stateFile, slot.parameters);
                if (slot.channelMode == PluginChannelMode::DualMono)
                    captureNode(live->rightNode.get(),
                                   slot.id + "-right", slot.rightStateFile,
                                   slot.rightParameters);
            };
            for (auto& track : snapshot.project.tracks) {
                if (!carriesAudio(track) || track.freeze.active()) continue;
                if (!spec.independentTrackId.empty() && track.id != spec.independentTrackId) continue;
                if (trackAccepts(track.kind, ClipKind::Midi))
                    collectSlot(track.id, track.instrument);
                for(auto& module:track.miniModules) collectSlot(track.id,module);
                for (auto& slot : track.inserts)
                    collectSlot(track.id, slot);
                if (track.samplerFx.isOwnedBy(track.instrument))
                    for (auto& slot : track.samplerFx.inserts)
                        collectSlot(track.id, slot);
                for (auto& clip : track.clips) {
                    if (clip.kind != ClipKind::Audio) continue;
                    for (auto& slot : clip.inserts)
                        collectSlot(track.id, slot);
                }
            }
            if (spec.independentTrackId.empty())
                for(auto& module:snapshot.project.masterMiniModules) collectSlot(kMasterChannelId,module);
                for (auto& slot : snapshot.project.masterInserts)
                    collectSlot(kMasterChannelId, slot);
        }
        EngineController scratch;
        scratch.m_isRenderClone = true;
        scratch.m_renderingPass = true;
        const double rate = spec.sampleRate > 0.0 ? spec.sampleRate : m_sampleRate;
        if (!std::isfinite(rate) || rate < 1000 || rate > 768000)
            return audio::Result::fail(audio::EngineError::InvalidArgument, "invalid render sample rate");
        const bool eventSensitive = std::any_of(snapshot.project.tracks.begin(),
            snapshot.project.tracks.end(), [](const auto& track) {
                return std::any_of(track.clips.begin(), track.clips.end(), [](const auto& clip) {
                    return !clip.muted && (clip.kind == ClipKind::Midi ||
                        clip.kind == ClipKind::Pattern || clip.kind == ClipKind::Automation);
                });
            });
        const auto renderBlockSize = spec.blockSize ? spec.blockSize
            : eventSensitive ? m_bufferSize : std::max(m_bufferSize, 1024u);
        if (const auto ready = scratch.initialize(rate, renderBlockSize, false); !ready) return ready;
        scratch.m_pluginManager.copyCatalogFrom(m_pluginManager);
        scratch.m_project = std::move(snapshot.project);
        if (!spec.independentTrackId.empty()) {
            std::erase_if(scratch.m_project.tracks, [&](const TrackModel& track) {
                return track.id != spec.independentTrackId;
            });
            if (scratch.m_project.tracks.size() != 1)
                return audio::Result::fail(audio::EngineError::TrackNotFound, "freeze source no longer exists");
            auto& track = scratch.m_project.tracks.front();
            track.freeze = {}; track.parentId.clear(); track.outputBusId.clear(); track.sends.clear();
            // The pre-fader stem bypasses pan/fader processing, but native
            // processors must keep the source channel's original mono layout.
            track.volume = 1.f; track.pan = 0.f;
            track.muted = track.soloed = track.armed = track.monitor = false;
            std::erase_if(track.clips, [](const ClipModel& clip) { return clip.kind == ClipKind::Automation; });
            scratch.m_project.masterInserts.clear();
            scratch.m_project.masterMiniModules.clear();
            scratch.m_project.masterVolume = 1.f; scratch.m_project.masterPan = 0.f;
            scratch.m_project.invalidateTrackIndex();
        }
        scratch.m_sourceSamples = m_sourceSamples;
        if (std::abs(rate - m_sampleRate) <= 0.01) {
            scratch.m_samples = m_samples;
            scratch.m_clipSampleCache = m_clipSampleCache;
            scratch.m_sharedClipSampleCache = m_sharedClipSampleCache;
        }
        scratch.m_sampleLoadContinue = preparing;
        scratch.m_engine.transport().setTempo(scratch.m_project.tempo);
        scratch.m_engine.transport().setTimeSignature(scratch.m_project.timeSigNumerator,
                                                     scratch.m_project.timeSigDenominator);
        scratch.m_engine.transport().setLoopRange(scratch.toSamples(m_project.loopStartSeconds),
                                                  scratch.toSamples(m_project.loopEndSeconds));
        scratch.m_engine.transport().setLoopEnabled(m_project.loopEnabled);
        if (const auto built = scratch.rebuildGraph(); !built) return built;

        std::unordered_map<std::string, const std::vector<std::uint8_t>*> states;
        for (const auto& state : snapshot.pluginStates) states[state.fileName] = &state.bytes;
        const auto restoreSlot = [&](const std::string& channelId, const InsertModel& slot) {
            if (!preparing() || slot.bypassed || slot.mix == 0.f) return;
            InsertSlot* live = scratch.liveInsertSlot(channelId, slot.id);
            if (!live) return;
            const auto restoreNode = [&](const std::shared_ptr<plugins::PluginNode>& node,
                                          const std::string& file,
                                          const std::vector<InsertParameter>& parameters,
                                          const std::string& pendingKey) {
                if (!node || !node->instance()) return;
                bool restored = false;
                if (const auto state = states.find(file); state != states.end()) {
                    if (!node->instance()->loadState(*state->second))
                        throw std::runtime_error("cannot restore render plugin state: " + slot.name);
                    restored = true;
                }
                if (auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(node->instance());
                    sampler && !sampler->samplePath().empty() && !sampler->rawSample())
                    throw std::runtime_error("cannot load render sampler source: " + sampler->samplePath());
                if (auto* slicer=dynamic_cast<plugins::slicer::SlicerInstance*>(node->instance());
                    slicer && !slicer->samplePath().empty() && !slicer->rawSample())
                    throw std::runtime_error("cannot load render slicer source: "+slicer->samplePath());
                node->discardPendingEvents();
                const auto pending = pendingParameterOverrides.find(pendingKey);
                if (!restored) {
                    scratch.applyStoredParameters(*node, parameters);
                } else if (pending != pendingParameterOverrides.end()) {
                    scratch.applyStoredParameters(*node, pending->second);
                }
                node->invalidatePrepare();
            };
            restoreNode(live->node, slot.stateFile, slot.parameters, slot.id);
            restoreNode(live->rightNode, slot.rightStateFile,
                        slot.rightParameters, slot.id + "-right");
        };
        for (const TrackModel& track : scratch.m_project.tracks) {
            if (track.instrument.isLoaded()) restoreSlot(track.id, track.instrument);
            for(const auto& module:track.miniModules) restoreSlot(track.id,module);
            for (const auto& slot : track.inserts) restoreSlot(track.id, slot);
            for (const auto& slot : track.samplerFx.inserts) restoreSlot(track.id, slot);
            for (const auto& clip : track.clips)
                for (const auto& slot : clip.inserts) restoreSlot(track.id, slot);
        }
        for(const auto& module:scratch.m_project.masterMiniModules) restoreSlot(kMasterChannelId,module);
        for (const auto& slot : scratch.m_project.masterInserts)
            restoreSlot(kMasterChannelId, slot);
        if (cancelled) { out.cancelled = true; return audio::Result::ok(); }
        // Some processors discover their final latency/layout only after
        // processing audio or restored parameter events. Keep the same clone
        // so that discovery survives, but discard the entire partial pass and
        // recompute PDC, capture windows and stems before starting over.
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            if (!preparing()) { out.cancelled = true; return audio::Result::ok(); }
            bool restartRequired = false;
            const auto result = scratch.renderProjectPass(spec, onProgress, out, restartRequired);
            if (cancelled) { out.cancelled = true; return audio::Result::ok(); }
            if (!restartRequired) return result;
            if (attempt == 7)
                return audio::Result::fail(audio::EngineError::Unknown,
                    "audio processor keeps changing configuration after 8 export attempts: " +
                    scratch.m_engine.offlineError());
        }
        return audio::Result::fail(audio::EngineError::Unknown, "export preparation failed");
    } catch (const std::exception& error) {
        if (cancelled) { out.cancelled = true; return audio::Result::ok(); }
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    } catch (...) {
        return audio::Result::fail(audio::EngineError::Unknown, "render failed with an unexpected exception");
    }
}

void EngineController::applyRenderSelection(const rendering::Spec& spec, ProjectModel& project) {
    // ── Bypass ──
    auto bypassSlots = [](std::vector<InsertModel>& slots) {
        for (InsertModel& slot : slots) {
            slot.bypassed = true;
        }
    };
    if (spec.bypassChannelInserts) {
        for (TrackModel& track : project.tracks) {
            for(auto& module:track.miniModules) module.bypassed=true;
            bypassSlots(track.inserts);
            bypassSlots(track.samplerFx.inserts);
            for (ClipModel& clip : track.clips) bypassSlots(clip.inserts);
        }
    }
    if (spec.bypassClipInserts && !spec.bypassChannelInserts) {
        for (TrackModel& track : project.tracks)
            for (ClipModel& clip : track.clips)
                bypassSlots(clip.inserts);
    }
    if (spec.bypassTrackInserts && !spec.bypassChannelInserts) {
        const std::unordered_set<std::string> sources(
            spec.sourceTrackIds.begin(), spec.sourceTrackIds.end());
        for (TrackModel& track : project.tracks) {
            const bool ordinarySource =
                track.kind == TrackKind::Audio ||
                track.kind == TrackKind::Midi ||
                track.kind == TrackKind::Instrument ||
                track.kind == TrackKind::Pattern;
            if ((!sources.empty() && !sources.contains(track.id)) ||
                (sources.empty() && !ordinarySource)) {
                continue;
            }
            bypassSlots(track.inserts);
            bypassSlots(track.samplerFx.inserts);
            for(auto& module:track.miniModules) module.bypassed=true;
        }
    }
    if (spec.bypassSummingInserts && !spec.bypassChannelInserts) {
        for (TrackModel& track : project.tracks) {
            if (!track.summing && track.kind != TrackKind::Bus &&
                track.kind != TrackKind::Group &&
                track.kind != TrackKind::Folder) {
                continue;
            }
            bypassSlots(track.inserts);
            bypassSlots(track.miniModules);
        }
    }
    if (spec.bypassSends) {
        for (TrackModel& track : project.tracks) {
            for (SendModel& send : track.sends) {
                send.enabled = false;
            }
        }
    }
    if (spec.bypassMasterChain) {
        bypassSlots(project.masterInserts);
        bypassSlots(project.masterMiniModules);
    }

    // Isolate the requested musical material without touching automation
    // clips. Pattern owners admit their linked child MIDI clips as one source.
    if (!spec.sourceClipIds.empty() || !spec.sourceTrackIds.empty()) {
        const std::unordered_set<std::string> clipIds(
            spec.sourceClipIds.begin(), spec.sourceClipIds.end());
        const std::unordered_set<std::string> trackIds(
            spec.sourceTrackIds.begin(), spec.sourceTrackIds.end());
        std::unordered_set<std::string> selectedPatterns;
        for (const TrackModel& track : project.tracks) {
            if (!trackIds.empty() && !trackIds.contains(track.id)) continue;
            for (const ClipModel& clip : track.clips)
                if (clip.kind == ClipKind::Pattern &&
                    (clipIds.empty() || clipIds.contains(clip.id)))
                    selectedPatterns.insert(clip.id);
        }

        std::unordered_set<std::string> sourceChannels = trackIds;
        std::unordered_set<std::string> gateOwners;
        std::unordered_set<std::string> mutedOwners;
        for (TrackModel& track : project.tracks) {
            bool channelHasSource = false;
            for (ClipModel& clip : track.clips) {
                if (clip.kind == ClipKind::Automation) continue;
                if (clip.kind == ClipKind::Pattern && clip.muted)
                    mutedOwners.insert(clip.id);
                const bool allowed = !clipIds.empty()
                    ? (clipIds.contains(clip.id) ||
                       (!clip.patternClipId.empty() &&
                        selectedPatterns.contains(clip.patternClipId)))
                    : (trackIds.contains(track.id) ||
                       (!clip.patternClipId.empty() &&
                        selectedPatterns.contains(clip.patternClipId)));
                clip.muted = clip.muted || !allowed;
                channelHasSource |= allowed && !clip.muted;
                if (allowed && !clip.muted && !clip.patternClipId.empty())
                    gateOwners.insert(clip.patternClipId);
            }
            if (channelHasSource) sourceChannels.insert(track.id);
        }
        // A selected child needs its Pattern clip as a playback gate. Keeping
        // that gate open must not admit its unselected sibling sources.
        for (TrackModel& track : project.tracks) {
            for (ClipModel& clip : track.clips) {
                if (clip.kind != ClipKind::Pattern ||
                    !gateOwners.contains(clip.id)) continue;
                clip.muted = mutedOwners.contains(clip.id);
                sourceChannels.insert(track.id);
            }
        }
        for (TrackModel& track : project.tracks) {
            track.soloed = sourceChannels.contains(track.id);
            if (track.soloed) track.muted = false;
        }
    }

    // ── Mute and solo ──
    if (spec.ignoreMuteSolo) {
        for (TrackModel& track : project.tracks) {
            if (!track.muted && !track.soloed) continue;
            track.muted = false;
            track.soloed = false;
        }
    }

}

audio::Result EngineController::renderProjectPass(
    const rendering::Spec& spec,
    const std::function<bool(const rendering::Progress&)>& onProgress,
    rendering::Report& out, bool& restartRequired) {
    restartRequired = false;
    out = rendering::Report{};

    if (!spec.writeMixdown && spec.stemChannelIds.empty()) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "nothing selected to render");
    }
    if (spec.outputDir.empty()) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "no output folder");
    }
    std::error_code dirError;
    const fs::path outputDir = platform::pathFromUtf8(spec.outputDir);
    fs::create_directories(outputDir, dirError);
    if (!fs::is_directory(outputDir)) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot write to " + spec.outputDir);
    }

    // A render is the one place where "one tick late" is not good enough, and a
    // rolling transport would fight the render gate for the whole pass.
    stop();
    flushDeferredClipSync();
    flushSamplerPrecompute();
    updateTimelineDuration();

    // ── Range, in seconds, resolved before anything is reconfigured ──
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    switch (spec.range) {
        case rendering::Range::WholeProject:
            endSeconds = durationSeconds();
            break;
        case rendering::Range::CycleRegion:
            startSeconds = loopStartSeconds();
            endSeconds = loopEndSeconds();
            break;
        case rendering::Range::Custom:
            startSeconds = spec.customStartSeconds;
            endSeconds = spec.customEndSeconds;
            break;
    }
    startSeconds = std::max(0.0, startSeconds);
    // Reserve room for plugin latency and prevent llround/addition overflow.
    const double maxSeconds = double(std::numeric_limits<engine::SamplePos>::max() / 2) / m_sampleRate;
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds) ||
        !std::isfinite(endSeconds + spec.tailMaxSeconds) ||
        endSeconds + spec.tailMaxSeconds > maxSeconds ||
        spec.preRollSeconds > maxSeconds || spec.tailHoldSeconds > maxSeconds ||
        endSeconds <= startSeconds) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "the render range is empty or invalid");
    }

    const double targetRate =
        spec.sampleRate > 0.0 ? spec.sampleRate : m_sampleRate;
    const engine::ChannelCount fileChannels =
        spec.channels == rendering::Channels::Mono ? 1 : 2;
    if (!audio::platform::isWriteSpecSupported(spec.file, fileChannels,
                                               targetRate)) {
        return audio::Result::fail(
            audio::EngineError::UnsupportedFormat,
            "this build cannot write that format at " +
                std::to_string(int(targetRate)) + " Hz");
    }

    // This controller belongs exclusively to the offline job. Its temporary
    // bypass/solo/tap configuration is discarded with it, so restoration never
    // rebuilds or overwrites the live session on an error path.
    UndoStack::Suspend quiet(m_undo);
    m_renderingPass = true;

    // ── Sample rate ──
    if (std::abs(targetRate - m_sampleRate) > 0.01) {
        if (auto prepared = applyRenderSampleRate(targetRate); !prepared) return prepared;
    }

    // ── Taps ──
    m_renderTapsPreFader = spec.stemsPreFader;
    m_renderTapsAtSource = spec.stemsAtSource;
    std::vector<std::string> stems;
    m_renderTaps.clear();
    for (const std::string& channelId : spec.stemChannelIds) {
        if (!m_channels.contains(channelId)) continue;   // deleted since
        if (m_renderTaps.contains(channelId)) continue;  // named twice
        m_renderTaps[channelId] =
            std::make_shared<engine::TapNode>(channelId + " Tap");
        stems.push_back(channelId);
    }
    if (stems.size() > kMaxRenderStems) {
        return audio::Result::fail(
            audio::EngineError::InvalidArgument,
            "too many stems in one render (" + std::to_string(stems.size()) +
                "); the limit is " + std::to_string(kMaxRenderStems));
    }
    if (!spec.writeMixdown && stems.empty())
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "no requested stem channels are available");

    // One rebuild puts the taps, the bypasses, the mutes and the rate into the
    // graph together, rather than recompiling once per change.
    if (auto graphStatus = rebuildGraph(); !graphStatus) return graphStatus;
    syncAllTrackGains();
    flushDeferredClipSync();

    // Prepare in the final processing mode before reading any latency. The
    // isolated controller never opens a device; all later compiles stay offline.
    if (const auto ready = m_engine.prepare(m_sampleRate, m_bufferSize, 2, true); !ready)
        return audio::Result::fail(audio::EngineError::Unknown,
            m_engine.offlineError().empty() ? std::string(engine::describe(ready.error()))
                                           : m_engine.offlineError());
    const auto graph = m_engine.compiledGraph();
    const auto requireSlot = [&](const std::string& channelId, const InsertModel& model, bool frozen = false) {
        if(model.uid==plugins::mini::kUid && !spec.bypassTrackInserts && !spec.bypassChannelInserts && !spec.stemsAtSource &&
           !(channelId==kMasterChannelId && spec.bypassMasterChain)) {
            const auto error=model.miniModule?plugins::mini::validate(*model.miniModule,model.miniModuleMode):"Missing module definition";
            if(!error.empty()) throw std::runtime_error("Mini module unavailable: "+model.name+" — "+error);
        }
        if (frozen || !model.isLoaded() || model.bypassed || model.mix == 0.f) return;
        auto* slot = liveInsertSlot(channelId, model.id);
        if (!slot || !slot->node || !slot->node->isReady() ||
            (model.channelMode == PluginChannelMode::DualMono &&
             (!slot->rightNode || !slot->rightNode->isReady())))
            throw std::runtime_error("cannot prepare export plugin: " + model.name +
                                     " (channel " + channelId + ", slot " + model.id + ")");
    };
    for (const auto& track : m_project.tracks) {
        if (!carriesAudio(track)) continue;
        for(const auto& module:track.miniModules) requireSlot(track.id,module,track.freeze.active());
        if(track.freeze.active()) continue;
        if (trackAccepts(track.kind, ClipKind::Midi)) requireSlot(track.id, track.instrument);
        for (const auto& slot : track.inserts) requireSlot(track.id, slot);
        if (track.samplerFx.isOwnedBy(track.instrument))
            for (const auto& slot : track.samplerFx.inserts) requireSlot(track.id, slot);
        for (const auto& clip : track.clips)
            if (clip.kind == ClipKind::Audio)
                for (const auto& slot : clip.inserts) requireSlot(track.id, slot);
    }
    for (const auto& slot : m_project.masterInserts) requireSlot(kMasterChannelId, slot);
    for (const auto& slot : m_project.masterMiniModules) requireSlot(kMasterChannelId, slot);

    // ── Files ──
    const engine::SamplePos from = toSamples(startSeconds);
    const engine::SamplePos rangeEnd = toSamples(endSeconds);

    double tailSeconds = 0.0;
    if (spec.tail == rendering::Tail::Fixed) {
        tailSeconds = std::clamp(spec.tailSeconds, 0.0, spec.tailMaxSeconds);
    } else if (spec.tail == rendering::Tail::UntilSilence) {
        tailSeconds = std::max(0.0, spec.tailMaxSeconds);
    }
    // The graph delays its output by whatever its plugins report, so the first
    // `latency` samples out of a pass are its compensation delay lines still
    // emptying. Rendering that much further and dropping that many frames off
    // the front is what keeps the file aligned with the timeline; without it a
    // lookahead limiter on the master shifts the whole render late by its own
    // latency and truncates the end by the same amount.
    engine::FrameCount captureLatency = graph->totalLatency;
    std::unordered_map<engine::TapNode*, engine::FrameCount> tapLatencies;
    for (const auto& [channelId, tap] : m_renderTaps) {
        const auto entry = std::find_if(graph->nodes.begin(), graph->nodes.end(),
            [&](const auto& node) { return node.node == tap.get(); });
        if (entry == graph->nodes.end())
            throw std::runtime_error("cannot capture export channel: " + channelId);
        tapLatencies.emplace(tap.get(), entry->latency);
        captureLatency = std::max(captureLatency, entry->latency);
    }
    for (const auto& [tap, ownLatency] : tapLatencies)
        tap->setCaptureDelay(captureLatency - ownLatency);
    engine::EdgeDelay masterCaptureDelay;
    const auto extraMasterDelay = captureLatency - graph->totalLatency;
    if (extraMasterDelay) masterCaptureDelay.prepare(2, extraMasterDelay, m_bufferSize);
    const auto latency = engine::SamplePos(captureLatency);

    // Pre-roll runs the arrangement ahead of the range and throws that audio
    // away, so a range starting mid-project opens with the reverb that was
    // already ringing instead of from silence. It cannot reach before zero.
    const engine::SamplePos renderStart = std::max<engine::SamplePos>(
        0, from - toSamples(std::max(0.0, spec.preRollSeconds)));
    engine::SamplePos discardFrames = (from - renderStart) + latency;

    const engine::SamplePos renderEnd =
        rangeEnd + toSamples(tailSeconds) + latency;
    const std::uint64_t expectedFrames = std::uint64_t(std::max<engine::SamplePos>(
        0, renderEnd - renderStart - discardFrames));
    const std::string extension =
        std::string(audio::platform::extensionFor(spec.file.container));

    rendering::OutputTransaction files;
    struct Sink {
        audio::platform::AudioFileWriter writer;
        std::string path;
        engine::TapNode* tap = nullptr;  // null for the mixdown
    };
    std::vector<Sink> sinks;
    std::unordered_set<std::string> taken;
    const std::string base = sanitizeFileName(spec.baseName);

    audio::Result ioStatus = audio::Result::ok();
    auto openSink = [&](const std::string& stem, engine::TapNode* tap) {
        if (!ioStatus) return;
        Sink sink;
        sink.tap = tap;
        sink.path = uniquePath(spec.outputDir, stem, extension, taken);
        ioStatus = sink.writer.open(files.stage(sink.path), spec.file, m_sampleRate,
                                    fileChannels, expectedFrames);
        if (!ioStatus) return;

        audio::platform::FileTags tags = spec.tags;
        // Each file is titled for itself, so a folder of stems reads without
        // opening them, and each carries the timeline position of the range —
        // which is what lets a stem be dropped back where it belongs.
        if (tags.title.empty()) tags.title = stem;
        if (tags.software.empty()) tags.software = "VLTONE";
        tags.timeReferenceSamples =
            std::uint64_t(std::max<engine::SamplePos>(0, from));
        ioStatus = sink.writer.setTags(tags);
        if (ioStatus) sinks.push_back(std::move(sink));
    };

    if (spec.writeMixdown) openSink(base, nullptr);
    for (const std::string& channelId : stems) {
        const TrackModel* track = m_project.findTrack(channelId);
        const std::string label =
            track ? track->name
                  : (channelId == kMasterChannelId ? "Master" : channelId);
        openSink(base + " - " + sanitizeFileName(label),
                 m_renderTaps[channelId].get());
    }

    // Anything half-written is worse than nothing: it looks like a finished
    // render until it is played.
    auto discard = [&sinks] {
        for (Sink& sink : sinks) {
            (void)sink.writer.close();
        }
    };
    if (!ioStatus) {
        discard();
        return ioStatus;
    }

    // ── The pass ──
    const float silenceThreshold = dbToLinear(spec.tailSilenceDb);
    const engine::SamplePos holdSamples =
        toSamples(std::max(0.0, spec.tailHoldSeconds));
    engine::SamplePos quietFor = 0;
    engine::SamplePos position = renderStart;
    engine::SamplePos written = 0;
    bool cancelled = false;
    std::vector<float> monoScratch(m_bufferSize);
    std::vector<const float*> offsetChannels(2, nullptr);

    // `offset` is how much of the block belongs to the pre-roll or the latency
    // flush and must not reach the file. The graph still had to render it.
    auto writeTo = [&](Sink& sink, const float* const* source,
                       engine::ChannelCount sourceChannels,
                       engine::FrameCount offset, engine::FrameCount frames) {
        if (fileChannels == 1) {
            const float* left = source[0] + offset;
            const float* right =
                (sourceChannels > 1 ? source[1] : source[0]) + offset;
            for (engine::FrameCount frame = 0; frame < frames; ++frame) {
                monoScratch[frame] = 0.5f * (left[frame] + right[frame]);
            }
            const float* mono[1] = {monoScratch.data()};
            return sink.writer.write(mono, frames);
        }
        if (offset == 0) return sink.writer.write(source, frames);
        for (engine::ChannelCount channel = 0; channel < 2; ++channel) {
            offsetChannels[channel] =
                source[channel < sourceChannels ? channel : 0] + offset;
        }
        return sink.writer.write(offsetChannels.data(), frames);
    };

    auto lastProgress = std::chrono::steady_clock::time_point{};
    auto renderStatus = m_engine.renderOffline(
        renderStart, renderEnd, m_bufferSize,
        [&](const engine::AudioBlock& block, engine::FrameCount frames) {
            if (extraMasterDelay) masterCaptureDelay.process(block, block, frames);
            const float* master[2] = {block.data(0), block.data(1)};

            // The pre-roll and the latency flush leave the graph first. They
            // are skipped here rather than rendered in a separate pass: the
            // graph has to run through them for its state to be right when the
            // part that does reach the file begins.
            engine::FrameCount skip = 0;
            if (discardFrames > 0) {
                skip = engine::FrameCount(
                    std::min<engine::SamplePos>(discardFrames, frames));
                discardFrames -= skip;
            }
            const engine::FrameCount keep = frames - skip;

            for (Sink& sink : sinks) {
                if (keep == 0) break;
                if (!sink.tap) {
                    ioStatus = writeTo(sink, master, 2, skip, keep);
                } else if (sink.tap->capturedFrames() == frames) {
                    ioStatus = writeTo(sink, sink.tap->captured(),
                                       sink.tap->capturedChannels(), skip, keep);
                } else {
                    // The tap saw a different block than the sink did, which
                    // would silently desynchronise a stem from the mix.
                    ioStatus = audio::Result::fail(
                        audio::EngineError::Unknown,
                        "a stem tap fell out of step with the render");
                }
                if (!ioStatus) return false;
            }
            written += keep;

            position += frames;

            // The tail ends when the decay has stayed under the threshold long
            // enough — measured on the master, which is the sum of everything
            // still ringing.
            if (spec.tail == rendering::Tail::UntilSilence &&
                position - latency > rangeEnd) {
                float peak = 0.0f;
                for (engine::ChannelCount channel = 0; channel < 2; ++channel) {
                    for (engine::FrameCount frame = 0; frame < frames; ++frame) {
                        peak = std::max(peak, std::fabs(master[channel][frame]));
                    }
                }
                // Stems can remain audible while the master is muted or
                // cancels them. All capture points share this time origin.
                for (const auto& sink : sinks) {
                    if (!sink.tap) continue;
                    for (engine::ChannelCount channel = 0; channel < sink.tap->capturedChannels(); ++channel)
                        for (engine::FrameCount frame = 0; frame < frames; ++frame)
                            peak = std::max(peak, std::fabs(sink.tap->captured()[channel][frame]));
                }
                quietFor = peak < silenceThreshold ? quietFor + frames : 0;
                if (quietFor >= holdSamples) return false;
            }

            const auto now = std::chrono::steady_clock::now();
            if (onProgress && (now - lastProgress >= std::chrono::milliseconds(33) ||
                               position >= renderEnd)) {
                lastProgress = now;
                rendering::Progress progress;
                progress.stage = position <= from + latency
                    ? rendering::Progress::Stage::PreRoll : rendering::Progress::Stage::Rendering;
                progress.renderedSeconds = double(position - renderStart) / m_sampleRate;
                progress.totalSeconds = double(renderEnd - renderStart) / m_sampleRate;
                progress.fraction =
                    progress.totalSeconds > 0.0
                        ? std::clamp(progress.renderedSeconds /
                                         progress.totalSeconds, 0.0, 1.0)
                        : 0.0;
                if (!onProgress(progress)) {
                    cancelled = true;
                    return false;
                }
            }
            return true;
        },
        engine::OfflineOptions{.sourcesEndSample = rangeEnd, .pipeline = spec.pipeline,
                               .forcePipeline = spec.forcePipeline});
    out.usedPipeline = m_engine.lastOfflineUsedPipeline();

    if (!renderStatus || !ioStatus || cancelled) {
        discard();
        if (cancelled) {
            out.cancelled = true;
            return audio::Result::ok();
        }
        if (!ioStatus) return ioStatus;
        restartRequired = !renderStatus &&
            renderStatus.error() == engine::EngineError::RenderRestartRequired;
        return audio::Result::fail(
            audio::EngineError::Unknown,
            m_engine.offlineError().empty() ? std::string(engine::describe(renderStatus.error()))
                                           : m_engine.offlineError());
    }

    for (Sink& sink : sinks) {
        if (const audio::Result closed = sink.writer.close(); !closed) {
            discard();
            return closed;
        }
    }
    if (const auto committed = files.commit(); !committed) return committed;
    for (const Sink& sink : sinks) out.files.push_back(sink.path);
    out.renderedSeconds = double(written) / m_sampleRate;
    return audio::Result::ok();
}

audio::Result EngineController::applyRenderSampleRate(double rate) {
    const double previousPosition = positionSeconds();
    // Clip audio is converted to the session rate once, when it is decoded, and
    // then cached by path. Changing the rate without dropping those caches would
    // render every clip at the wrong speed — the reason this is a helper and not
    // three lines at the call site.
    m_samples.clear();
    m_clipSampleCache.clear();
    m_sharedClipSampleCache.clear();
    m_sampleRate = rate;
    if (auto prepared = m_engine.prepare(m_sampleRate, m_bufferSize, 2); !prepared)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
            std::string(engine::describe(prepared.error())));
    m_engine.transport().seek(toSamples(previousPosition));
    m_recorder->shutdown();
    m_recorder->initialize(m_sampleRate, 2);
    updateTimelineDuration();
    return audio::Result::ok();
}

} // namespace daw
