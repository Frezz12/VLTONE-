#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "WarpAnalysis.hpp"
#include "WarpTools.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "cloud/PublishPreflight.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include "DSP/WarpPlayback.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstdio>
#include <numbers>

namespace {
int failures = 0;
void check(bool ok, const char* label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
bool near(double a, double b, double eps = 1e-7) { return std::abs(a - b) < eps; }
std::vector<float> render(const daw::ClipWarpModel& map, const daw::engine::SampleBuffer& audio, int block) {
    daw::engine::dsp::WarpPlayback player(map, 120, 48000);
    std::vector<float> result(96000), right(96000);
    for (int p = 0; p < 96000; p += block)
        player.render(audio, p / 48000., 0, 0, result.data() + p, right.data() + p, std::min(block, 96000 - p));
    double error = 0;
    for (int p = 0; p < 96000; ++p) error = std::max(error, double(std::abs(right[p] + .7f * result[p])));
    check(error < .003, "Warp preserves stereo phase and balance");
    if (error >= .003) std::printf("stereo max error %.9f\n", error);
    return result;
}
}
int main() {
    using namespace daw;
    ClipWarpModel map; map.enabled = true; map.baselineDurationSeconds = 2;
    map.markers = {{"start", 0, 0, true}, {"middle", .5, 1.5, false}, {"end", 2, 4, true}};
    check(validWarp(map), "valid monotonic Warp map");
    check(near(warpSourceAt(map, 1.5), .5) && near(warpBeatAt(map, .5), 1.5), "forward and inverse mapping agree");
    auto timing = map; timing.markers[1].targetBeats = 1.13;
    warptools::AlignParams params; params.timing.gridBeats = .5;
    params.timing.strength = 0;
    check(warptools::align(timing, {}, params).map == timing, "zero strength is the exact original map");
    params.timing.strength = .5;
    const auto half = warptools::align(timing, {}, params);
    check(near(half.map.markers[1].targetBeats, 1.065), "assisted 50 percent timing halves the error");
    params.timing.strength = 1;
    check(near(warptools::align(timing, {}, params).map.markers[1].targetBeats, 1), "assisted 100 percent timing lands on the grid");
    params.timing.strength = .5;
    check(warptools::align(timing, {}, params).map == half.map, "strength recomputation does not accumulate corrections");
    params.timing.toleranceBeats = .14;
    check(warptools::align(timing, {}, params).map == timing, "tolerance preserves intentional microtiming");
    params.timing.toleranceBeats = 0; params.timing.strength = 1; params.timing.swing = .66;
    params.timing.swingUnitBeats = .5; timing.markers[1].targetBeats = 1.49;
    const double sharedTarget = miditools::gridTarget(1.49, params.timing);
    auto swing = warptools::align(timing, {}, params);
    check(near(swing.map.markers[1].targetBeats, sharedTarget), "audio and MIDI use the same Swing target");
    params.timing.groove = miditools::groovePresets().back(); params.originBeats = 3.5;
    swing = warptools::align(timing, {}, params);
    check(near(swing.map.markers[1].targetBeats, miditools::gridTarget(4.99, params.timing) - 3.5), "groove phase follows project beats, including odd meter origins");
    params = {}; params.addTransients = true;
    const std::vector<analysis::WarpTransient> candidates{{.31, .9, .95}, {.81, .8, .2}};
    const auto proposed = warptools::align(timing, candidates, params);
    check(proposed.added == 1 && proposed.uncertain == 1 && validWarp(proposed.map), "only confident attacks become assisted markers");
    params.includeUncertain = true;
    check(warptools::align(timing, candidates, params).added == 2, "uncertain attacks require explicit inclusion");
    timing.markers[1].locked = true;
    check(warptools::align(timing, candidates, params).map.markers[2] == timing.markers[1], "assistant preserves a manual locked anchor and its id");
    params.beginBeats = 2; params.endBeats = 3;
    const auto ranged = warptools::align(timing, candidates, params);
    check(near(warpSourceAt(ranged.map, .3), warpSourceAt(timing, .3)) && near(warpSourceAt(ranged.map, 3.7), warpSourceAt(timing, 3.7)), "range boundary anchors keep outside audio unchanged");
    const auto groove = warptools::extractGroove({.01, .28, .49, .78}, 3.5, .25, "Human 7/8");
    check(groove.offsets.size() == 14 && near(groove.offsets[1], .03) && groove.velocities.empty(), "audio groove extraction captures timing without velocity");
    auto broken = map; broken.markers[1].targetBeats = 5;
    check(!validWarp(broken), "crossed Warp markers are rejected");
    broken = map; broken.markers[1].sourceSeconds = std::numeric_limits<double>::quiet_NaN();
    check(!validWarp(broken), "nonfinite Warp coordinates are rejected");
    auto slice = sliceWarp(map, 1, 3);
    check(validWarp(slice) && near(warpSourceAt(slice, .5), .5) && near(slice.markers.back().targetBeats, 2), "trim inserts mapped boundary anchors");
    auto boundary = map;
    boundary.markers = {{"start", .1, 0, true}, {"end", 1, .6180038246658313, true}};
    const auto expanded = sliceWarp(boundary, warpBeatAt(boundary, 0), .5);
    check(expanded.markers.front().sourceSeconds == 0 && validWarp(expanded),
          "extending a trimmed Warp clip to source zero does not create negative roundoff");
    check(warpSourceAt(boundary, -1) < -1e-9,
          "Warp mapping preserves genuinely negative extrapolation for caller bounds checks");

    auto tone = std::make_shared<engine::SampleBuffer>(2, 96000, 48000);
    for (unsigned i = 0; i < tone->frames(); ++i) {
        tone->writableChannel(0)[i] = float(.5 * std::sin(2 * std::numbers::pi * 220 * i / 48000.));
        tone->writableChannel(1)[i] = -.7f * tone->channel(0)[i];
    }
    auto identity = map; identity.markers[1].targetBeats = 1;
    const auto plain = render(identity, *tone, 257);
    check(std::equal(plain.begin(), plain.end(), tone->channel(0)), "enabling identity Warp is sample-exact");
    const auto a = render(map, *tone, 512), b = render(map, *tone, 157);
    double error = 0, jump = 0;
    for (std::size_t i = 1; i < a.size(); ++i) {
        error = std::max(error, double(std::abs(a[i] - b[i])));
        if (i > 5000 && i + 5000 < a.size()) jump = std::max(jump, double(std::abs(a[i] - a[i - 1])));
    }
    check(error < 1e-6, "Warp realtime and export do not depend on block size");
    check(jump < .12, "Warp segment boundaries do not click");
    int crossings = 0;
    for (int i = 12001; i < 24000; ++i) if (a[i - 1] <= 0 && a[i] > 0) ++crossings;
    check(std::abs(crossings * 4 - 220) <= 4, "nonlinear Warp preserves pitch");

    auto hits = std::make_shared<engine::SampleBuffer>(2, 96000, 48000);
    for (unsigned i = 0; i < hits->frames(); ++i) {
        const int phase = int(i) % 24000;
        const float value = phase < 400 ? float(.8 * std::exp(-phase / 70.)) : 0.f;
        hits->writableChannel(0)[i] = value; hits->writableChannel(1)[i] = -.7f * value;
    }
    auto transients = analysis::detectWarpTransients(*hits, 0, 2);
    check(std::any_of(transients.begin(), transients.end(), [](const auto& t) { return std::abs(t.sourceSeconds - .5) < .012; }), "transient analysis locates attacks in antiphase stereo");
    check(analysis::detectWarpTransients(*hits, 0, 2, [] { return false; }).empty(), "transient analysis can be cancelled");
    auto warpedHits = render(map, *hits, 157);
    const auto peak = std::max_element(warpedHits.begin() + 35000, warpedHits.begin() + 37000);
    check(std::abs((peak - warpedHits.begin()) - 36000) < 480 && *peak > .3f, "Warp moves an attack to its marker within 10ms");

    const auto dir = std::filesystem::temp_directory_path() / ("vlt-warp-" + newUuid());
    std::filesystem::create_directories(dir);
    const auto file = (dir / "source.wav").string();
    audio::AudioBuffer buffer(2, 96000);
    for (int ch = 0; ch < 2; ++ch) std::copy_n(hits->channel(ch), 96000, buffer.getChannel(ch));
    audio::AudioRecorder recorder; recorder.initialize(48000, 2); recorder.writeWAVFile(file, buffer, 48000);
    EngineController controller{};
    if (!controller.initialize(48000, 256, false)) return 1;
    {
        controller.setTempo(120);
        const auto importedTrack = controller.addTrack(TrackKind::Audio, "Auto Warp");
        ClipMusicalAnalysisModel analysis;
        analysis.algorithmVersion = analysis.tempo.algorithmVersion = 2;
        analysis.tempo.status = MusicalAnalysisStatus::Available;
        analysis.tempo.bpm = 160.01;
        analysis.tempo.stability = 1;
        analysis.analyzedDurationSeconds = 2;
        const auto beforeImport = controller.undoDepth();
        const auto imported = controller.importAudio(file, importedTrack, 3.25, analysis, true);
        const auto current = [&] { return controller.audioClip(importedTrack, imported); };
        check(!imported.empty() && current()->warp.enabled && validWarp(current()->warp) &&
              current()->warp.preservePitch && current()->warp.mode == 4 &&
              near(current()->durationSeconds, 8.0 / 3) && near(current()->startSeconds, 3.25) &&
              near(current()->warp.baselineDurationSeconds, 2),
              "Auto Warp fits the source tempo to the project without moving the clip or changing pitch");
        const auto fitted = current()->warp;
        check(controller.undoDepth() == beforeImport + 1, "auto-fit and import share one Undo entry");
        controller.undo(); check(!current(), "Undo removes the automatically fitted clip");
        controller.redo(); check(current() && current()->warp == fitted, "Redo restores the exact fitted map and stable ids");
        auto off = fitted; off.enabled = false;
        controller.setClipWarp(importedTrack, imported, off);
        check(near(current()->durationSeconds, 2) && near(current()->startSeconds, 3.25) &&
              current()->warp.markers == fitted.markers && near(current()->sampleEdit.stretchTime, 1),
              "Warp Off restores source duration and playback while keeping the map and placement");
        controller.undo(); check(current()->warp == fitted && near(current()->durationSeconds, 8.0 / 3),
                                  "Undo of Warp Off restores automatic fitting");
        controller.redo();
        controller.setTempo(80);
        check(near(current()->durationSeconds, 2), "a disabled auto-fit keeps source duration when BPM changes");
        controller.setClipWarp(importedTrack, imported, fitted);
        check(near(current()->durationSeconds, 4), "reenabling auto-fit follows the current project BPM");
        std::string saved;
        ProjectModel restored;
        check(ProjectSerializer::serializeDocument(controller.project(), saved).isOk() &&
              ProjectSerializer::deserializeDocument(restored, saved).isOk(), "auto-fit project round-trip succeeds");
        const auto* savedTrack = restored.findTrack(importedTrack);
        check(savedTrack && savedTrack->clips.back().warp == fitted,
              "project persistence retains the fitted map and original duration");

        const auto plain = controller.importAudio(file, importedTrack, 0, analysis);
        check(controller.audioClip(importedTrack, plain)->warp.empty(),
              "imports that did not request Auto Warp retain their existing behavior");
        for (int invalid = 0; invalid < 5; ++invalid) {
            auto uncertain = analysis;
            if (invalid == 0) uncertain.tempo.status = MusicalAnalysisStatus::Unavailable;
            if (invalid == 1) uncertain.tempo.status = MusicalAnalysisStatus::Ambiguous;
            if (invalid == 2) uncertain.tempo.variable = true;
            if (invalid == 3) uncertain.tempo.bpm = std::numeric_limits<double>::quiet_NaN();
            if (invalid == 4) uncertain.tempo.stability = 0;
            const auto id = controller.importAudio(file, importedTrack, 7, uncertain, true);
            const auto* unwarped = controller.audioClip(importedTrack, id);
            check(unwarped && unwarped->warp.empty() && near(unwarped->durationSeconds, 2),
                  "unreliable or missing tempo imports at its original duration");
        }
        const auto beforeTrack = controller.undoDepth();
        const auto newTrack = controller.importAudioToNewTrack(file, 5, "Fitted track", analysis, true);
        check(!newTrack.empty() && controller.project().findTrack(newTrack)->clips.front().warp.enabled &&
              controller.undoDepth() == beforeTrack + 1, "new-track auto-fit is one complete import transaction");
        controller.undo(); check(!controller.project().findTrack(newTrack), "Undo removes the new fitted track atomically");
    }
    ProjectModel project; project.tempo = 120;
    TrackModel track; track.id = newUuid(); track.name = "Drums"; track.kind = TrackKind::Audio;
    ClipModel clip; clip.id = newUuid(); clip.kind = ClipKind::Audio; clip.filePath = file; clip.durationSeconds = 2; clip.channels = 2;
    track.clips.push_back(clip); project.tracks.push_back(track); controller.restoreProject(project, "Test");
    check(controller.initializeClipWarp(track.id, clip.id), "initialize Warp through controller");
    const auto baseline = controller.audioClip(track.id, clip.id)->warp;
    const auto depth = controller.undoDepth();
    controller.beginWarpEdit(track.id, clip.id);
    controller.setClipWarp(track.id, clip.id, map);
    auto moved = map; moved.markers[1].targetBeats = 1.6; controller.setClipWarp(track.id, clip.id, moved);
    controller.commitWarpEdit();
    check(controller.undoDepth() == depth + 1, "a whole marker drag creates one Undo entry");
    controller.undo(); check(controller.audioClip(track.id, clip.id)->warp == baseline, "Undo restores the complete map");
    controller.redo(); check(controller.audioClip(track.id, clip.id)->warp == moved, "Redo restores the edited map");
    controller.beginWarpEdit(track.id, clip.id); controller.setClipWarp(track.id, clip.id, map); controller.cancelWarpEdit();
    check(controller.audioClip(track.id, clip.id)->warp == moved, "Escape restores map without history");
    auto off = moved; off.enabled = false; controller.setClipWarp(track.id, clip.id, off);
    check(controller.initializeClipWarp(track.id, clip.id) && !controller.audioClip(track.id, clip.id)->warp.enabled, "reopening respects Warp Off");
    controller.setClipWarp(track.id, clip.id, map);
    controller.setTempo(150);
    check(near(controller.audioClip(track.id, clip.id)->durationSeconds, 1.6) && controller.audioClip(track.id, clip.id)->warp == map, "tempo edits preserve musical marker positions");
    controller.setTempo(120);
    const auto previewDepth = controller.undoDepth();
    auto proposal = map; proposal.markers[1].targetBeats = 1.25;
    check(controller.beginWarpPreview(track.id, clip.id) && controller.updateWarpPreview(proposal), "controller starts an immutable Warp preview");
    check(controller.audioClip(track.id, clip.id)->warp == map && controller.undoDepth() == previewDepth,
          "preview leaves the document and history untouched");
    const auto savedDuringPreview = controller.prepareProjectSave();
    check(savedDuringPreview.project.tracks.front().clips.front().warp == map, "autosave snapshot excludes preview timing");
    check(controller.auditionWarpPreview(false) && controller.auditionWarpPreview(true) && controller.warpPreviewActive(), "Before After switches the live audition");
    const auto livePeak = [&] {
        audio::AudioBuffer input(2, 256), output(2, 256);
        input.clear(); controller.seekSeconds(0); controller.play();
        float loudest = 0; int frame = 0;
        for (int offset = 0; offset < 43000; offset += 256) {
            controller.processDeviceBlockForTest(input, output, 256);
            for (int i = 0; i < 256; ++i) if (offset + i > 26000 && offset + i < 40000 && std::abs(output.getChannel(0)[i]) > loudest) {
                loudest = std::abs(output.getChannel(0)[i]); frame = offset + i;
            }
        }
        controller.pause(); return frame;
    };
    check(std::abs(livePeak() - 30000) < 500, "After audition moves the attack through the real device callback");
    controller.auditionWarpPreview(false);
    check(std::abs(livePeak() - 36000) < 500, "Before audition restores confirmed attack timing in the real callback");
    controller.auditionWarpPreview(true);
    auto invalidProposal = proposal; invalidProposal.markers.front().targetBeats = .1;
    check(!controller.updateWarpPreview(invalidProposal) && *controller.warpPreviewMap() == proposal, "preview rejects altered clip boundaries without losing the valid proposal");
    controller.cancelWarpPreview();
    check(!controller.warpPreviewActive() && controller.audioClip(track.id, clip.id)->warp == map && controller.undoDepth() == previewDepth, "Cancel restores confirmed playback without history");
    controller.beginWarpPreview(track.id, clip.id); controller.updateWarpPreview(proposal); controller.commitWarpPreview();
    check(controller.audioClip(track.id, clip.id)->warp == proposal && controller.undoDepth() == previewDepth + 1, "Apply stores one undoable Warp operation");
    controller.undo(); check(controller.audioClip(track.id, clip.id)->warp == map, "Undo restores the pre-assistant map");
    controller.redo(); check(controller.audioClip(track.id, clip.id)->warp == proposal, "Redo restores accepted assistant timing"); controller.undo();
    controller.beginWarpPreview(track.id, clip.id); controller.updateWarpPreview(proposal); controller.setTempo(121);
    check(!controller.warpPreviewActive(), "tempo edits invalidate a pending proposal"); controller.undo();
    controller.beginWarpPreview(track.id, clip.id); controller.updateWarpPreview(proposal); controller.setClipStartSeconds(track.id, clip.id, .25);
    check(!controller.warpPreviewActive(), "clip movement invalidates the proposal grid origin");
    // Placement updates are live gesture primitives, without their own undo.
    controller.setClipStartSeconds(track.id, clip.id, 0);
    std::string encoded; ProjectModel decoded;
    check(ProjectSerializer::serializeDocument(controller.project(), encoded).isOk() &&
          ProjectSerializer::deserializeDocument(decoded, encoded).isOk() && decoded.tracks[0].clips[0].warp == map, "Warp project serialization round-trip");
    auto roundedDocument = nlohmann::json::parse(encoded);
    auto& roundedWarp = roundedDocument["tracks"][0]["clips"][0]["warp"];
    roundedWarp["markers"][0]["sourceSeconds"] = -2.7755575615628914e-17;
    check(ProjectSerializer::deserializeDocument(decoded, roundedDocument.dump()).isOk() &&
          decoded.tracks[0].clips[0].warp == map,
          "legacy projects with negative Warp roundoff load with an exact zero source anchor");
    roundedWarp["enabled"] = false;
    auto disabledMap = map; disabledMap.enabled = false;
    check(ProjectSerializer::deserializeDocument(decoded, roundedDocument.dump()).isOk() &&
          decoded.tracks[0].clips[0].warp == disabledMap,
          "disabled Warp maps also recover source-zero roundoff");
    roundedWarp["markers"][0]["sourceSeconds"] = -1e-6;
    check(!ProjectSerializer::deserializeDocument(decoded, roundedDocument.dump()).isOk(),
          "project recovery still rejects genuinely negative Warp coordinates");
    auto roundedProject = controller.project();
    roundedProject.tracks[0].clips[0].warp.markers[0].sourceSeconds = -2.7755575615628914e-17;
    std::string normalizedDocument;
    check(ProjectSerializer::serializeDocument(roundedProject, normalizedDocument).isOk() &&
          nlohmann::json::parse(normalizedDocument)["tracks"][0]["clips"][0]["warp"]["markers"][0]["sourceSeconds"] == 0,
          "saving a Warp map never persists negative source-zero roundoff");
    check(cloud::inspectForPublishV1(controller.project()).canPublish(),
          "cloud publication accepts supported Warp data");
    check(cloud::projectForCloudSnapshotV1(controller.project()).document.tracks[0].clips[0].warp == map,
          "cloud snapshot preserves the nonlinear Warp map");
    const auto copied = controller.duplicateClip(track.id, clip.id);
    auto other = map; other.markers[1].targetBeats = 1.2; controller.setClipWarp(track.id, copied, other);
    check(controller.audioClip(track.id, clip.id)->warp == map, "duplicate clips have independent maps");
    controller.removeClip(track.id, copied);
    const auto right = controller.splitClip(track.id, clip.id, .75);
    check(!right.empty() && near(controller.audioClip(track.id, right)->offsetSeconds, .5), "split uses the nonlinear source position");
    controller.undo();
    controller.beginClipTrimEdit(track.id, clip.id);
    const auto trimGeometry = controller.clipGeometryRevision();
    controller.setClipTrim(track.id, clip.id, .75, .75, 1.25);
    check(controller.clipGeometryRevision() > trimGeometry, "Warp trim updates visual geometry before release");
    controller.endClipTrimEdit("Trim Warp");
    check(near(controller.audioClip(track.id, clip.id)->offsetSeconds, .5), "trim maps its timeline edge into source time");
    controller.undo(); check(controller.audioClip(track.id, clip.id)->warp == map, "trim Undo restores discarded markers");
    const double oldEnd = controller.clipSampleParameter(track.id, clip.id, "endoffset");
    controller.setClipSampleParameter(track.id, clip.id, "endoffset", .2);
    controller.commitClipSampleParameterEdit(track.id, clip.id, "endoffset", oldEnd, "Trim source");
    check(controller.audioClip(track.id, clip.id)->warp.markers.size() == 2, "sample trim removes markers outside the source range");
    controller.undo(); check(controller.audioClip(track.id, clip.id)->warp == map, "sample trim Undo restores the full Warp map");
    const auto version = captureClipAudioVersion(*controller.audioClip(track.id, clip.id));
    ClipModel restored; applyClipAudioVersion(restored, version);
    check(restored.warp == map, "audio version restoration includes Warp");
    ProjectModel legacy = project; std::string oldDocument;
    ProjectSerializer::serializeDocument(legacy, oldDocument);
    ProjectSerializer::deserializeDocument(decoded, oldDocument);
    check(decoded.tracks.front().clips.front().warp.empty(), "projects without Warp load with Warp Off");
    const auto output = (dir / "warped.wav").string();
    const auto confirmedOutput = (dir / "confirmed.wav").string();
    check(controller.exportMixdown(confirmedOutput, false).isOk(), "confirmed timing exports before an audition");
    controller.beginWarpPreview(track.id, clip.id); controller.updateWarpPreview(proposal);
    check(controller.exportMixdown(output, false).isOk(), "Warp exports through the ordinary mixdown pipeline");
    audio::platform::DecodedAudio exported; audio::platform::decodeAudioFile(output, exported);
    double loudest = 0; int peakFrame = 0;
    for (int i = 35000; i < 37000 && i < int(exported.frames); ++i)
        if (std::abs(exported.interleaved[i * exported.channels]) > loudest) { loudest = std::abs(exported.interleaved[i * exported.channels]); peakFrame = i; }
    check(std::abs(peakFrame - 36000) < 480 && loudest > .1, "export contains the warped attack at its musical position");
    check(controller.audioClip(track.id, clip.id)->warp == map, "mixdown during preview exports only committed timing");
    audio::platform::DecodedAudio confirmed; audio::platform::decodeAudioFile(confirmedOutput, confirmed);
    check(confirmed.frames == exported.frames && confirmed.interleaved == exported.interleaved,
          "preview and confirmed exports are sample-identical, including the initial transition");
    check(controller.warpPreviewActive() && std::abs(livePeak() - 30000) < 500,
          "mixdown restores the pending After audition on return");
    controller.cancelWarpPreview();
    EngineController rack{}; rack.initialize(48000, 256, false);
    const auto rackTrack = rack.addTrack(TrackKind::Audio, "Offline rack");
    rack.addInsert(rackTrack, plugins::equalizer::EqualizerInstance::staticDescriptor());
    controller.setRecordDirectory(dir.string());
    EngineController::OfflineRenderReport rendered;
    const EngineController::ClipAddress address{track.id, clip.id};
    check(controller.renderClipsOffline({address}, rack.copyChannelStrip(rackTrack, false), false, {}, rendered).isOk(),
          "Offline Render processes warped audio");
    const auto* baked = controller.audioClip(track.id, clip.id);
    check(baked->warp.empty() && baked->offlineHistory.front().source.warp == map,
          "baked Warp is not applied twice and the original map remains in version history");
    check(cloud::inspectForPublishV1(controller.project()).canPublish(),
          "cloud publication accepts Warp maps in processed audio history");
    check(cloud::projectForCloudSnapshotV1(controller.project()).document.tracks[0].clips[0].offlineHistory.front().source.warp == map,
          "cloud snapshot preserves the original Warp map in processed audio history");
    check(controller.restoreOfflineRenderOriginal(address).isOk() && controller.audioClip(track.id, clip.id)->warp == map,
          "restoring original audio restores the nonlinear map");
    auto missingProject = project;
    auto& missingClip = missingProject.tracks.front().clips.front();
    missingClip.filePath = (dir / "missing.wav").string(); missingClip.warp = map;
    controller.restoreProject(missingProject, "Missing source");
    auto disabled = map; disabled.enabled = false;
    check(controller.initializeClipWarp(track.id, clip.id) && controller.setClipWarp(track.id, clip.id, disabled),
          "a saved map opens and can be disabled when its source is missing");
    controller.newProject(false);
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
