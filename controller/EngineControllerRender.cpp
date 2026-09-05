#include "EngineController.hpp"
#include "RenderOutput.hpp"
#include "platform/PathUtils.hpp"

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
        std::string path = platform::pathToUtf8(
            platform::pathFromUtf8(dir) /
            (name + "." + std::string(extension)));
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
        {
            const engine::RealtimeEngine::RenderGate gate(m_engine);
            snapshot = captureRecoverySnapshot();
        }
        EngineController scratch;
        scratch.m_isRenderClone = true;
        scratch.m_renderingPass = true;
        const double rate = spec.sampleRate > 0.0 ? spec.sampleRate : m_sampleRate;
        if (!std::isfinite(rate) || rate < 1000 || rate > 768000)
            return audio::Result::fail(audio::EngineError::InvalidArgument, "invalid render sample rate");
        if (const auto ready = scratch.initialize(rate, m_bufferSize, false); !ready) return ready;
        scratch.m_pluginManager.copyCatalogFrom(m_pluginManager);
        scratch.m_project = std::move(snapshot.project);
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
            if (!preparing()) return;
            InsertSlot* live = scratch.liveInsertSlot(channelId, slot.id);
            if (!live) return;
            const auto restoreNode = [&](const std::shared_ptr<plugins::PluginNode>& node,
                                          const std::string& file,
                                          const std::vector<InsertParameter>& parameters) {
                if (!node || !node->instance()) return;
                if (const auto state = states.find(file); state != states.end()) {
                    if (!node->instance()->loadState(*state->second))
                        throw std::runtime_error("cannot restore render plugin state: " + slot.name);
                }
                if (auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(node->instance());
                    sampler && !sampler->samplePath().empty() && !sampler->rawSample())
                    throw std::runtime_error("cannot load render sampler source: " + sampler->samplePath());
                scratch.applyStoredParameters(*node, parameters);
                node->invalidatePrepare();
            };
            restoreNode(live->node, slot.stateFile, slot.parameters);
            restoreNode(live->rightNode, slot.rightStateFile, slot.rightParameters);
        };
        for (const TrackModel& track : scratch.m_project.tracks) {
            if (track.instrument.isLoaded()) restoreSlot(track.id, track.instrument);
            for (const auto& slot : track.inserts) restoreSlot(track.id, slot);
            for (const auto& slot : track.samplerFx.inserts) restoreSlot(track.id, slot);
            for (const auto& clip : track.clips)
                for (const auto& slot : clip.inserts) restoreSlot(track.id, slot);
        }
        for (const auto& slot : scratch.m_project.masterInserts)
            restoreSlot(kMasterChannelId, slot);
        if (cancelled) { out.cancelled = true; return audio::Result::ok(); }
        const auto result = scratch.renderProjectPass(spec, onProgress, out);
        if (cancelled) { out.cancelled = true; return audio::Result::ok(); }
        return result;
    } catch (const std::exception& error) {
        if (cancelled) { out.cancelled = true; return audio::Result::ok(); }
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    } catch (...) {
        return audio::Result::fail(audio::EngineError::Unknown, "render failed with an unexpected exception");
    }
}

audio::Result EngineController::renderProjectPass(
    const rendering::Spec& spec,
    const std::function<bool(const rendering::Progress&)>& onProgress,
    rendering::Report& out) {
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
    if (endSeconds <= startSeconds) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "the render range is empty");
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

    // ── Bypass ──
    auto bypassSlots = [](std::vector<InsertModel>& slots) {
        for (InsertModel& slot : slots) {
            slot.bypassed = true;
        }
    };
    if (spec.bypassChannelInserts) {
        for (TrackModel& track : m_project.tracks) {
            bypassSlots(track.inserts);
            bypassSlots(track.samplerFx.inserts);
            for (ClipModel& clip : track.clips) bypassSlots(clip.inserts);
        }
    }
    if (spec.bypassClipInserts && !spec.bypassChannelInserts) {
        for (TrackModel& track : m_project.tracks)
            for (ClipModel& clip : track.clips)
                bypassSlots(clip.inserts);
    }
    if (spec.bypassTrackInserts && !spec.bypassChannelInserts) {
        const std::unordered_set<std::string> sources(
            spec.sourceTrackIds.begin(), spec.sourceTrackIds.end());
        for (TrackModel& track : m_project.tracks) {
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
        }
    }
    if (spec.bypassSummingInserts && !spec.bypassChannelInserts) {
        for (TrackModel& track : m_project.tracks) {
            if (!track.summing && track.kind != TrackKind::Bus &&
                track.kind != TrackKind::Group &&
                track.kind != TrackKind::Folder) {
                continue;
            }
            bypassSlots(track.inserts);
        }
    }
    if (spec.bypassSends) {
        for (TrackModel& track : m_project.tracks) {
            for (SendModel& send : track.sends) {
                send.enabled = false;
            }
        }
    }
    if (spec.bypassMasterChain) {
        bypassSlots(m_project.masterInserts);
    }

    // Isolate the requested musical material without touching automation
    // clips. Pattern owners admit their linked child MIDI clips as one source.
    if (!spec.sourceClipIds.empty() || !spec.sourceTrackIds.empty()) {
        const std::unordered_set<std::string> clipIds(
            spec.sourceClipIds.begin(), spec.sourceClipIds.end());
        const std::unordered_set<std::string> trackIds(
            spec.sourceTrackIds.begin(), spec.sourceTrackIds.end());
        std::unordered_set<std::string> patternOwners;
        for (const TrackModel& track : m_project.tracks) {
            if (!trackIds.empty() && !trackIds.contains(track.id)) continue;
            for (const ClipModel& clip : track.clips)
                if (clip.kind == ClipKind::Pattern)
                    patternOwners.insert(clip.id);
        }
        patternOwners.insert(clipIds.begin(), clipIds.end());

        std::unordered_set<std::string> sourceChannels = trackIds;
        for (TrackModel& track : m_project.tracks) {
            bool channelHasSource = false;
            for (ClipModel& clip : track.clips) {
                if (clip.kind == ClipKind::Automation) continue;
                const bool allowed = !clipIds.empty()
                    ? (clipIds.contains(clip.id) ||
                       (!clip.patternClipId.empty() &&
                        patternOwners.contains(clip.patternClipId)))
                    : (trackIds.contains(track.id) ||
                       (!clip.patternClipId.empty() &&
                        patternOwners.contains(clip.patternClipId)));
                clip.muted = !allowed;
                channelHasSource |= allowed;
            }
            if (channelHasSource) sourceChannels.insert(track.id);
        }
        for (TrackModel& track : m_project.tracks) {
            track.soloed = sourceChannels.contains(track.id);
            if (track.soloed) track.muted = false;
        }
    }

    // ── Mute and solo ──
    if (spec.ignoreMuteSolo) {
        for (TrackModel& track : m_project.tracks) {
            if (!track.muted && !track.soloed) continue;
            track.muted = false;
            track.soloed = false;
        }
    }

    // ── Sample rate ──
    if (std::abs(targetRate - m_sampleRate) > 0.01) {
        applyRenderSampleRate(targetRate);
    }

    // ── Taps ──
    m_renderTapsPreFader = spec.stemsPreFader;
    m_renderTapsAtSource = spec.stemsAtSource;
    std::vector<std::string> stems;
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

    // One rebuild puts the taps, the bypasses, the mutes and the rate into the
    // graph together, rather than recompiling once per change.
    if (auto graphStatus = rebuildGraph(); !graphStatus) return graphStatus;
    syncAllTrackGains();
    flushDeferredClipSync();

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
    const auto latency = engine::SamplePos(m_engine.latencySamples());

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
        if (tags.software.empty()) tags.software = "VLT Studio Pro";
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
        engine::OfflineOptions{.sourcesEndSample = rangeEnd});

    if (!renderStatus || !ioStatus || cancelled) {
        discard();
        if (cancelled) {
            out.cancelled = true;
            return audio::Result::ok();
        }
        if (!ioStatus) return ioStatus;
        return audio::Result::fail(
            audio::EngineError::Unknown,
            std::string(engine::describe(renderStatus.error())));
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

void EngineController::applyRenderSampleRate(double rate) {
    // Clip audio is converted to the session rate once, when it is decoded, and
    // then cached by path. Changing the rate without dropping those caches would
    // render every clip at the wrong speed — the reason this is a helper and not
    // three lines at the call site.
    m_samples.clear();
    m_clipSampleCache.clear();
    m_sharedClipSampleCache.clear();
    m_sampleRate = rate;
    m_engine.prepare(m_sampleRate, m_bufferSize, 2);
    m_recorder->initialize(m_sampleRate, 2);
    updateTimelineDuration();
}

} // namespace daw
