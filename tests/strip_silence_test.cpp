#include "EngineController.hpp"
#include "StripSilence.hpp"
#include "Core/AudioBuffer.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

using namespace daw;
namespace {
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("%s Strip Silence: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
bool near(double a, double b, double tolerance = .002) { return std::abs(a - b) <= tolerance; }
StripSilenceSettings exactSettings() {
    StripSilenceSettings s;
    s.thresholdDb = -20; s.hysteresisDb = 6;
    s.minimumSilenceMs = 50; s.minimumSoundMs = 10;
    s.preRollMs = s.postRollMs = s.gridBeats = 0;
    return s;
}
std::vector<std::vector<SilenceRegion>> analyze(const std::vector<EngineController::StripSilenceSource>& sources,
                                             StripSilenceSettings settings) {
    std::vector<std::vector<SilenceRegion>> result;
    for (const auto& source : sources) {
        const auto envelope = buildSilenceEnvelope(source.placements, source.durationSeconds, source.sampleRate);
        result.push_back(detectSilenceRegions(envelope, settings, source.original.startSeconds, source.tempo));
    }
    return result;
}
}

int main() {
    auto options = exactSettings();
    SilenceEnvelope envelope;
    envelope.durationSeconds = 1;
    envelope.peaks.resize(1000, 0);
    for (int i = 200; i < 400; ++i) envelope.peaks[i] = .5f;
    for (int i = 600; i < 800; ++i) envelope.peaks[i] = .5f;
    auto regions = detectSilenceRegions(envelope, options, 0, 120);
    check(regions.size() == 2 && near(regions[0].begin, .2) && near(regions[1].end, .8),
          "detects head, tail and internal silence");
    options.minimumSilenceMs = 250;
    regions = detectSilenceRegions(envelope, options, 0, 120);
    check(regions.size() == 1 && near(regions[0].begin, .2) && near(regions[0].end, .8),
          "minimum silence preserves short pauses");
    options = exactSettings(); options.splitInternal = false;
    regions = detectSilenceRegions(envelope, options, 0, 120);
    check(regions.size() == 1 && near(regions[0].end - regions[0].begin, .6), "edge-only mode keeps inner pauses");
    options = exactSettings(); options.gridBeats = .25;
    regions = detectSilenceRegions(envelope, options, .03, 120);
    check(regions.size() == 2 && near(regions[0].begin, .095) && near(regions[0].end, .47) &&
          near(regions[1].begin, .595) && near(regions[1].end, .845), "grid is timeline-anchored and expands outwards");
    options = exactSettings(); options.preRollMs = options.postRollMs = 150;
    regions = detectSilenceRegions(envelope, options, 0, 120);
    check(regions.size() == 1 && near(regions[0].begin, .05) && near(regions[0].end, .95),
          "overlapping padding merges without double playback");
    SilenceEnvelope noisy = envelope;
    std::fill(noisy.peaks.begin(), noisy.peaks.end(), 0);
    noisy.peaks[40] = .8f;
    options = exactSettings();
    check(detectSilenceRegions(noisy, options, 0, 120).empty(), "minimum sound discards isolated noise");
    for (int i = 200; i < 400; ++i) noisy.peaks[i] = i == 200 ? .3f : .07f;
    regions = detectSilenceRegions(noisy, options, 0, 120);
    check(regions.size() == 1 && near(regions[0].end, .4), "hysteresis retains quiet tails after a loud onset");

    const auto dir = std::filesystem::temp_directory_path() / ("vlt-strip-silence-" + newUuid());
    std::filesystem::create_directories(dir);
    const auto wav = (dir / "Stereo.wav").string();
    audio::AudioBuffer audio(2, 48000);
    for (unsigned i = 0; i < 48000; ++i) {
        const bool sound = (i >= 9600 && i < 19200) || (i >= 28800 && i < 38400);
        audio.getChannel(0)[i] = sound ? .5f : 0;
        audio.getChannel(1)[i] = sound ? -.5f : 0;
    }
    audio::AudioRecorder writer; writer.initialize(48000, 2);
    check(writer.writeWAVFile(wav, audio, 48000).isOk(), "writes antiphase stereo fixture");
    EngineController controller{EngineController::TestRuntime{}};
    check(controller.initialize(48000, 256, false).isOk(), "initializes controller");
    controller.setRecordDirectory(dir.string());
    controller.setTempo(120);
    const auto track = controller.addTrack(TrackKind::Audio, "Stem");
    const auto clip = controller.importAudio(wav, track, 2);
    auto effect = controller.pluginManager().find(plugins::Format::Internal, "daw.equalizer");
    if (effect) check(!controller.addClipFxInsert(track, clip, *effect).empty(), "loaded Clip FX fixture");
    std::vector<EngineController::StripSilenceSource> sources;
    check(controller.prepareStripSilence({{track, clip}, {track, clip}}, sources).isOk() && sources.size() == 1,
          "prepares immutable sources and deduplicates selection");
    auto plan = analyze(sources, options);
    check(plan[0].size() == 2, "stereo cancellation cannot masquerade as silence");
    std::vector<EngineController::ClipAddress> created;
    const auto depth = controller.undoDepth();
    check(controller.applyStripSilence(sources, plan, options, created).isOk() && created.size() == 2,
          "manual apply creates separate clips");
    auto clips = controller.project().findTrack(track)->clips;
    check(clips.size() == 2 && near(clips[0].startSeconds, 2.2) && near(clips[1].startSeconds, 2.6) &&
          near(clips[0].offsetSeconds, .2) && near(clips[1].offsetSeconds, .6), "cuts keep timing and source offsets");
    check(near(clips[0].fadeInSeconds, .003, 1e-9) && near(clips[0].fadeOutSeconds, .003, 1e-9),
          "new edges receive short click-preventing fades");
    check(!effect || (!clips[0].inserts.empty() && clips[0].inserts[0].id != clips[1].inserts[0].id),
          "each fragment keeps independent Clip FX");
    check(controller.undoDepth() == depth + 1, "batch creates one undo item");
    if (effect && clips.size() == 2) {
        auto* first = controller.insertInstance(track, clips[0].inserts[0].id);
        auto* second = controller.insertInstance(track, clips[1].inserts[0].id);
        const auto scheduled = [&](plugins::PluginInstance* instance) {
            const auto graph = controller.routingGraph();
            return std::any_of(graph->nodes.begin(), graph->nodes.end(), [&](const auto& entry) {
                const auto* node = dynamic_cast<const plugins::PluginNode*>(entry.node);
                return node && node->instance() == instance;
            });
        };
        controller.seekSeconds(2.2);
        check(first && second && scheduled(first) && scheduled(second), "fragment effects are initially scheduled");
        check(controller.startRecording(track), "starts the next take over effect-bearing fragments");
        check(!scheduled(first) && !scheduled(second) &&
              !first->isActive() && !second->isActive() &&
              controller.insertInstance(track, clips[0].inserts[0].id) == first &&
              controller.insertInstance(track, clips[1].inserts[0].id) == second,
              "recording retains Clip FX instances with their DSP streams deactivated");
        // A parameter edit must survive dormant processing and the activation
        // on punch-out, without replacing the editor's instance.
        controller.setInsertParameter(track, clips[0].inserts[0].id, "output.gain", -6);
        audio::AudioBuffer input(2, 256), output(2, 256);
        for (unsigned ch = 0; ch < 2; ++ch) std::fill_n(input.getChannel(ch), 256, .125f);
        check(controller.processDeviceBlockForTest(input, output, 256) &&
              std::abs(output.getChannel(0)[255] - .125f) < 1e-5 &&
              std::abs(output.getChannel(1)[255] - .125f) < 1e-5,
              "recording still monitors the live input without previous Clip FX");
        const auto added = controller.addClipFxInsert(track, clips[1].id, *effect);
        auto* addedInstance = controller.insertInstance(track, added);
        check(addedInstance && !addedInstance->isActive() && !scheduled(addedInstance),
              "adding Clip FX during recording creates an editable dormant instance");
        if (!added.empty()) controller.undo();
        controller.finalizeRecordingCapture();
        check(scheduled(first) && scheduled(second) && first->isActive() && second->isActive() &&
              controller.insertInstance(track, clips[0].inserts[0].id) == first,
              "punch-out reschedules the same Clip FX instances");
        controller.processDeviceBlockForTest(input, output, 256);
        check(near(controller.insertParameter(track, clips[0].inserts[0].id, "output.gain"), -6),
              "dormant Clip FX parameter edits survive reactivation");
        controller.stop();
    }
    controller.undo();
    check(controller.project().findTrack(track)->clips.size() == 1 &&
          controller.project().findTrack(track)->clips[0].id == clip, "undo restores exact original clip");
    controller.redo();
    check(controller.project().findTrack(track)->clips[0].id == created[0].clipId,
          "redo preserves fragment identities");
    controller.undo();
    controller.setClipGain(track, clip, .7f);
    check(!controller.applyStripSilence(sources, plan, options, created), "stale analysis cannot overwrite changed audio");
    controller.setClipGain(track, clip, 1);
    controller.setClipTrim(track, clip, 3, .1, .8);
    controller.prepareStripSilence({{track, clip}}, sources);
    plan = analyze(sources, options);
    check(plan[0].size() == 2 && near(plan[0][0].begin, .1), "existing source trim is respected");
    auto bad = plan; bad[0][0].end = 100;
    check(!controller.applyStripSilence(sources, bad, options, created) &&
          controller.project().findTrack(track)->clips.size() == 1, "invalid boundaries leave all clips intact");
    bool cancelled = false;
    const auto stopped = buildSilenceEnvelope(sources[0].placements, sources[0].durationSeconds, 48000,
        [&] { cancelled = true; return false; });
    check(cancelled && stopped.peaks.empty(), "background analysis can be cancelled");
    check(!controller.applyStripSilence({sources[0], sources[0]}, {plan[0], plan[0]}, options, created),
          "duplicate apply addresses cannot corrupt the batch");
    const auto beforeNoop = controller.undoDepth();
    check(controller.applyStripSilence(sources, {{{0, sources[0].durationSeconds}}}, options, created).isOk() &&
          controller.undoDepth() == beforeNoop, "no-op does not create an undo item");

    // Loop phase and tape speed are rendered before slicing, so changing the
    // new clip's origin cannot restart either effect at each retained region.
    for (bool tape : {false, true}) {
        const auto special = controller.importAudio(wav, track, 5);
        if (tape) {
            controller.setClipFade(track, special, .15, .1);
            controller.setClipFadeMode(track, special, true, ClipFadeMode::Tape);
        } else {
            controller.setClipSampleParameter(track, special, "loop.mode", 1);
            controller.setClipTrim(track, special, 5, 0, 2);
        }
        check(controller.prepareStripSilence({{track, special}}, sources).isOk(), "prepares loop/tape source");
        plan = analyze(sources, options);
        check(!plan[0].empty(), "loop/tape source retains audible material");
        check(controller.applyStripSilence(sources, plan, options, created).isOk(), "cuts loop/tape playback without restarting phase");
        const auto* lane = controller.project().findTrack(track);
        for (const auto& address : created) {
            auto piece = std::find_if(lane->clips.begin(), lane->clips.end(), [&](const auto& item) { return item.id == address.clipId; });
            check(piece != lane->clips.end() && piece->filePath != wav && std::filesystem::exists(piece->filePath) &&
                  piece->sampleEdit.loopMode == 0 && piece->fadeInMode == ClipFadeMode::Gain,
                  "phase-sensitive fragments use a preserved rendered source");
        }
        controller.undo();
        check(controller.prepareStripSilence({{track, special}}, sources).isOk() && sources[0].original.filePath == wav,
              "undo restores the original loop/tape source and settings");
    }

    const auto quietFile = (dir / "Silence.wav").string();
    audio.clear(); writer.writeWAVFile(quietFile, audio, 48000);
    const auto quiet = controller.importAudio(quietFile, track, 9);
    const auto sound = controller.importAudio(wav, track, 10);
    controller.prepareStripSilence({{track, quiet}, {track, sound}}, sources);
    plan = analyze(sources, options);
    const auto beforeBatch = controller.project().findTrack(track)->clips.size();
    const auto batchDepth = controller.undoDepth();
    check(plan[0].empty() && controller.applyStripSilence(sources, plan, options, created).isOk() &&
          created.size() == 2 && controller.project().findTrack(track)->clips.size() == beforeBatch,
          "mixed batch removes a silent clip and splits an audible one");
    check(controller.undoDepth() == batchDepth + 1, "multi-clip batch is atomic in undo history");
    controller.undo();
    check(controller.project().findTrack(track)->clips.size() == beforeBatch,
          "undo restores the silent and audible source clips together");

    // Exercise actual recording callbacks and writer finalization, including
    // a loop's second pass and the single Undo for recording plus cleanup.
    for (bool layered : {false, true}) {
        EngineController recorder{EngineController::TestRuntime{}};
        recorder.initialize(48000, 256, false); recorder.setRecordDirectory(dir.string());
        const auto target = recorder.addTrack(TrackKind::Audio, "Recording");
        auto prefs = recorder.recordingPrefs();
        prefs.mode = layered ? RecordMode::Layers : RecordMode::Overwrite;
        prefs.autoSilence = true; prefs.stripSilence = exactSettings();
        recorder.setRecordingPrefs(prefs);
        if (layered) { recorder.setLoopRangeSeconds(0, 1); recorder.setLoopEnabled(true); }
        check(recorder.startRecording(target), "starts Auto Silence recording");
        prefs.autoSilence = false; prefs.stripSilence.thresholdDb = 0;
        recorder.setRecordingPrefs(prefs);
        audio::AudioBuffer input(2, 256), output(2, 256);
        const unsigned total = layered ? 96000 : 48000;
        for (unsigned at = 0; at < total; at += 256) {
            const auto frames = std::min(256u, total - at);
            for (unsigned i = 0; i < frames; ++i) {
                const double t = double(at + i) / 48000;
                const bool sound = layered ? (t >= 1.6 && t < 1.8) : (t >= .2 && t < .4);
                input.getChannel(0)[i] = input.getChannel(1)[i] = sound ? .5f : 0;
            }
            recorder.processDeviceBlockForTest(input, output, frames);
        }
        const auto beforeRecord = recorder.undoDepth();
        const auto beforeRecordBytes = recorder.undoEstimatedBytes();
        recorder.stopRecording();
        const auto landed = recorder.project().findTrack(target)->clips;
        check(recorder.autoSilenceWarning().empty() && landed.size() == 1 &&
              near(landed[0].startSeconds, layered ? .6 : .2) && near(landed[0].durationSeconds, .2),
              layered ? "Auto Silence trims the audible loop take" : "Auto Silence uses settings frozen at recording start");
        check(!layered || (!landed.empty() && landed[0].takes.size() >= 2), "loop take history is retained");
        check(recorder.undoDepth() == beforeRecord + 1, "recording and Auto Silence share one undo");
        check(recorder.undoEstimatedBytes() > beforeRecordBytes + 256,
              "recording accounts for its actual undo snapshot payload");
        recorder.undo();
        check(recorder.project().findTrack(target)->clips.empty(), "undo removes recording and cleanup together");
        recorder.redo();
        check(recorder.project().findTrack(target)->clips.size() == 1 &&
              near(recorder.project().findTrack(target)->clips[0].durationSeconds, .2), "redo restores cleaned recording");
    }

    // Multi-track cleanup is one graph publication, independent of how many
    // target tracks acquire fragments, and one undo operation for the take.
    {
        EngineController recorder{EngineController::TestRuntime{}};
        recorder.initialize(48000, 256, false); recorder.setRecordDirectory(dir.string());
        std::vector<std::string> tracks;
        for (unsigned i = 0; i < 4; ++i) tracks.push_back(recorder.addTrack(TrackKind::Audio, "Batch"));
        auto prefs = recorder.recordingPrefs();
        prefs.autoSilence = true; prefs.stripSilence = exactSettings();
        recorder.setRecordingPrefs(prefs);
        check(recorder.startRecordingTracks(tracks), "starts multitrack Auto Silence capture");
        audio::AudioBuffer input(2, 256), output(2, 256);
        for (unsigned at = 0; at < 48000; at += 256) {
            const auto frames = std::min(256u, 48000 - at);
            for (unsigned i = 0; i < frames; ++i)
                input.getChannel(0)[i] = input.getChannel(1)[i] =
                    (at + i >= 9600 && at + i < 19200) ||
                    (at + i >= 28800 && at + i < 38400) ? .5f : 0.f;
            recorder.processDeviceBlockForTest(input, output, frames);
        }
        const auto rebuilds = recorder.graphRebuildCountForTest();
        const auto depth = recorder.undoDepth();
        recorder.stopRecording();
        check(recorder.autoSilenceWarning().empty() && recorder.graphRebuildCountForTest() - rebuilds == 2,
              "four cleaned tracks require only capture-finalization and landing rebuilds");
        check(recorder.undoDepth() == depth + 1 && std::all_of(tracks.begin(), tracks.end(), [&](const auto& id) {
                  return recorder.project().findTrack(id)->clips.size() == 2;
              }), "all recorded tracks receive their fragments in one undo operation");
        recorder.undo();
        check(std::all_of(tracks.begin(), tracks.end(), [&](const auto& id) {
                  return recorder.project().findTrack(id)->clips.empty();
              }), "multitrack undo restores every target together");
        recorder.redo();
        check(std::all_of(tracks.begin(), tracks.end(), [&](const auto& id) {
                  return recorder.project().findTrack(id)->clips.size() == 2;
              }), "multitrack redo restores every cleaned target");
    }

    // Auto cleanup of a punch must not delete older material outside the take.
    {
        EngineController recorder{EngineController::TestRuntime{}};
        recorder.initialize(48000, 256, false); recorder.setRecordDirectory(dir.string());
        const auto target = recorder.importAudioToNewTrack(wav, 0, "Punch");
        const auto original = recorder.project().findTrack(target)->clips.front().id;
        std::string fx;
        if (effect) {
            fx = recorder.addClipFxInsert(target, original, *effect);
            recorder.setInsertParameter(target, fx, "output.gain", -3);
        }
        auto prefs = recorder.recordingPrefs();
        prefs.mode = RecordMode::Layers; prefs.autoSilence = true; prefs.stripSilence = exactSettings();
        recorder.setRecordingPrefs(prefs); recorder.seekSeconds(.25);
        check(recorder.startRecording(target), "starts a punch into an existing layered clip");
        audio::AudioBuffer input(2, 256), output(2, 256); input.clear();
        for (unsigned at = 0; at < 24000; at += 256)
            recorder.processDeviceBlockForTest(input, output, std::min(256u, 24000 - at));
        recorder.stopRecording();
        const auto parts = recorder.project().findTrack(target)->clips;
        check(recorder.autoSilenceWarning().empty() && parts.size() == 2 &&
              near(parts[0].startSeconds, 0) && near(parts[0].durationSeconds, .25) &&
              near(parts[1].startSeconds, .75) && near(parts[1].durationSeconds, .25),
              "a silent punch preserves all material outside the recorded interval");
        if (effect && parts.size() == 2) {
            recorder.processDeviceBlockForTest(input, output, 256);
            check(near(recorder.insertParameter(target, parts[0].inserts[0].id, "output.gain"), -3) &&
                  near(recorder.insertParameter(target, parts[1].inserts[0].id, "output.gain"), -3),
                  "new fragment effects load their state before publication");
        }
        recorder.undo();
        const auto undone = recorder.project().findTrack(target)->clips;
        check(undone.size() == 1 && undone[0].id == original &&
              (!effect || recorder.insertInstance(target, fx)), "record undo restores the original Clip FX instance");
        recorder.redo();
        const auto redone = recorder.project().findTrack(target)->clips;
        check(redone.size() == 2 && (!effect || (recorder.insertInstance(target, redone[0].inserts[0].id) &&
              recorder.insertInstance(target, redone[1].inserts[0].id))), "record redo restores both fragment Clip FX instances");
    }
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
