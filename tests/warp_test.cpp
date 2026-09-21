#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "WarpAnalysis.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "cloud/PublishPreflight.hpp"
#include "DSP/WarpPlayback.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
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
    auto broken = map; broken.markers[1].targetBeats = 5;
    check(!validWarp(broken), "crossed Warp markers are rejected");
    broken = map; broken.markers[1].sourceSeconds = std::numeric_limits<double>::quiet_NaN();
    check(!validWarp(broken), "nonfinite Warp coordinates are rejected");
    auto slice = sliceWarp(map, 1, 3);
    check(validWarp(slice) && near(warpSourceAt(slice, .5), .5) && near(slice.markers.back().targetBeats, 2), "trim inserts mapped boundary anchors");

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
    EngineController controller;
    if (!controller.initialize(48000, 256, false)) return 1;
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
    std::string encoded; ProjectModel decoded;
    check(ProjectSerializer::serializeDocument(controller.project(), encoded).isOk() &&
          ProjectSerializer::deserializeDocument(decoded, encoded).isOk() && decoded.tracks[0].clips[0].warp == map, "Warp project serialization round-trip");
    check(!cloud::inspectForPublishV1(controller.project()).canPublish(), "cloud publication refuses local Warp data");
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
    check(controller.exportMixdown(output, false).isOk(), "Warp exports through the ordinary mixdown pipeline");
    audio::platform::DecodedAudio exported; audio::platform::decodeAudioFile(output, exported);
    double loudest = 0; int peakFrame = 0;
    for (int i = 35000; i < 37000 && i < int(exported.frames); ++i)
        if (std::abs(exported.interleaved[i * exported.channels]) > loudest) { loudest = std::abs(exported.interleaved[i * exported.channels]); peakFrame = i; }
    check(std::abs(peakFrame - 36000) < 480 && loudest > .1, "export contains the warped attack at its musical position");
    EngineController rack; rack.initialize(48000, 256, false);
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
    check(!cloud::inspectForPublishV1(controller.project()).canPublish(), "cloud publication also protects maps in audio history");
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
