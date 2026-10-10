#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;
static int failures = 0;
static void check(bool value, const char* label) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    failures += !value;
}
static const daw::ClipModel* clip(const daw::EngineController& c, const std::string& track, const std::string& id) {
    const auto* t = c.project().findTrack(track);
    if (t) for (const auto& item : t->clips) if (item.id == id) return &item;
    return nullptr;
}
static std::shared_ptr<const daw::engine::ClipPlayerNode::ClipList>
placements(const daw::EngineController& c, const std::string& track) {
    const auto graph = c.routingGraph();
    const auto* ids = c.trackNodes(track);
    if (graph && ids) for (const auto& entry : graph->nodes) if (entry.id == ids->clips)
        if (const auto* player = dynamic_cast<const daw::engine::ClipPlayerNode*>(entry.node)) return player->clips();
    return {};
}
static bool sameAudio(const daw::engine::ClipPlayerNode::ClipList& a,
                      const daw::engine::ClipPlayerNode::ClipList& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i]; const auto& y = b[i];
        if (x.audio != y.audio || x.startSample != y.startSample || x.lengthSamples != y.lengthSamples ||
            std::abs(x.sourceStartFrame - y.sourceStartFrame) > 1.e-6 ||
            std::abs(x.sourceEndFrame - y.sourceEndFrame) > 1.e-6 ||
            x.gain != y.gain || x.pan != y.pan || x.muted != y.muted ||
            x.fadeInSamples != y.fadeInSamples || x.fadeOutSamples != y.fadeOutSamples ||
            x.fadeInCurve != y.fadeInCurve || x.fadeOutCurve != y.fadeOutCurve) return false;
    }
    return true;
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto dir = fs::temp_directory_path() / ("vlt-glue-test-" + daw::newUuid());
    fs::create_directories(dir);
    const auto file = (dir / "sample.wav").string();
    audio::AudioBuffer audio(2, 24000);
    for (unsigned i = 0; i < 24000; ++i) {
        audio.getChannel(0)[i] = 0.25f;
        audio.getChannel(1)[i] = -0.1f;
    }
    audio::AudioRecorder writer; writer.initialize(48000, 2);
    writer.writeWAVFile(file, audio, 48000);
    daw::EngineController c;
    check(c.initialize(48000, 512, false).isOk(), "initialize");
    const auto track = c.addTrack(daw::TrackKind::Audio, "Glue");
    const auto a = c.importAudio(file, track, 0.0);
    const auto b = c.importAudio(file, track, 3600.0); // Sparse hour: no hour-long render/allocation.
    c.setClipGain(track, b, 0.4f);
    c.setClipTrim(track, b, 3600.0, 0.1, 0.3);
    const auto before = placements(c, track);
    check(before && before->size() == 2, "source placements available");
    const auto rebuilds = c.graphRebuildCountForTest();
    int renders = 0;
    const daw::EngineController::RenderExecutor cancelRender = [&](const auto& session, auto& report) {
        check(session.valid(), "fallback receives an immutable session");
        ++renders; report.cancelled = true; return audio::Result::ok();
    };
    std::string joined;
    const auto start = std::chrono::steady_clock::now();
    check(c.glueClips({{track, a}, {track, b}}, joined, cancelRender).isOk(), "glue sparse samples");
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("Glue elapsed: %.3f ms\n", elapsed);
    const auto* merged = clip(c, track, joined);
    check(renders == 0 && c.graphRebuildCountForTest() == rebuilds, "glue needs neither render nor graph rebuild");
    check(merged && merged->takes.size() == 2 && merged->compCrossfadeMs == 0.0,
          "different trims use non-destructive source references");
    if (merged) for (const auto& take : merged->takes) check(take.filePath == file, "source paths retained");
    const auto after = placements(c, track);
    check(before && after && sameAudio(*before, *after), "playback preserves gap, trim and independent gain exactly");
    c.undo();
    check(clip(c, track, a) && clip(c, track, b) && !clip(c, track, joined), "undo restores original identities");
    c.redo();
    check(clip(c, track, joined) && c.graphRebuildCountForTest() == rebuilds, "redo restores glue without graph rebuild");
    const auto third = c.importAudio(file, track, 3601.0);
    const auto beforeAgain = placements(c, track);
    std::string again;
    check(c.glueClips({{track, joined}, {track, third}}, again, cancelRender).isOk(), "glue a previously glued clip");
    const auto afterAgain = placements(c, track);
    check(beforeAgain && afterAgain && sameAudio(*beforeAgain, *afterAgain) && renders == 0,
          "repeated glue preserves all original placements");
    const auto project = (dir / "glued.vlt").string();
    check(c.saveProject(project).isOk(), "save non-destructive glue");
    daw::EngineController reopened;
    reopened.initialize(48000, 512, false);
    check(reopened.openProject(project).isOk(), "reload non-destructive glue");
    const auto reloaded = placements(reopened, track);
    check(reloaded && reloaded->size() == 3 && (*reloaded)[1].gain == 0.4f &&
        (*reloaded)[1].sourceStartFrame == 4800, "reload preserves all source regions and offsets");

    // A contiguous split is reunited as one ordinary source view, with outer fades.
    const auto splitTrack = c.addTrack(daw::TrackKind::Audio, "Split");
    const auto left = c.importAudio(file, splitTrack, 0.0);
    c.setClipFade(splitTrack, left, 0.03, 0.04);
    const auto right = c.splitClip(splitTrack, left, 0.2);
    std::string whole;
    check(c.glueClips({{splitTrack, left}, {splitTrack, right}}, whole, cancelRender).isOk(), "rejoin a split");
    const auto* restored = clip(c, splitTrack, whole);
    check(restored && restored->takes.empty() && restored->filePath == file &&
        restored->durationSeconds == 0.5 && restored->fadeInSeconds == 0.03 && restored->fadeOutSeconds == 0.04,
        "split glue preserves original source and outside fades");

    // Overlap must retain the exact-render fallback; cancelling never mutates.
    const auto overlap = c.importAudio(file, splitTrack, 0.1);
    std::string cancelled;
    check(c.glueClips({{splitTrack, whole}, {splitTrack, overlap}}, cancelled, cancelRender).isOk() &&
        cancelled.empty() && renders == 1 && clip(c, splitTrack, whole) && clip(c, splitTrack, overlap),
        "overlap fallback cancellation leaves both clips untouched");
    check(!c.offlineRenderInProgress(), "cancel releases render guard");
    c.shutdown(); reopened.shutdown();
    fs::remove_all(dir);
    return failures ? 1 : 0;
}
