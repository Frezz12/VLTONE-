#include "VocalAlignment.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "DSP/WarpPlayback.hpp"
#include <cstdio>
#include <filesystem>
#include <numbers>

namespace {
int failures = 0;
void check(bool value, const char* label) { std::printf("%s %s\n", value ? "PASS" : "FAIL", label); failures += !value; }
using namespace daw;
using namespace daw::alignment;
AlignmentSource source(const std::vector<double>& events, double duration = 3) {
    auto audio = std::make_shared<engine::SampleBuffer>(2, unsigned(duration * 48000), 48000);
    for (std::size_t k = 0; k < events.size(); ++k) {
        const int start = int(std::llround(events[k] * 48000));
        for (int i = 0; i < 600; ++i) {
            if (start + i < 0 || start + i >= int(audio->frames())) continue;
            const float value = float(.7 * std::exp(-i / 140.) * (1 + .3 * std::sin(2 * std::numbers::pi * (220 + 170 * k) * i / 48000)));
            audio->writableChannel(0)[start+i] = value; audio->writableChannel(1)[start+i] = -.7f * value;
        }
    }
    AlignmentSource result; result.audio = audio; result.startSeconds = 2; result.durationSeconds = duration;
    result.baseline.enabled = true; result.baseline.baselineDurationSeconds = duration;
    result.baseline.markers = {{"start", 0, 0, true}, {"end", duration, duration * 2, true}};
    return result;
}
double destination(const AlignmentProposal& proposal, double sourceSeconds) {
    return proposal.startSeconds + warpBeatAt(proposal.map, sourceSeconds) / 2;
}
}
int main() {
    using namespace daw; using namespace daw::alignment;
    const std::vector<double> events{.5, 1.2, 2.3};
    auto guide = source(events), target = guide;
    target.startSeconds += 503. / 48000;
    AlignmentSettings settings; settings.method = AlignmentMethod::Shift; settings.toleranceSeconds = 0;
    auto shifted = align(guide, {target}, settings).front();
    check(shifted.status == AlignmentStatus::Aligned && shifted.rigidShift && std::abs(shifted.startSeconds-guide.startSeconds) < .5/48000,
          "rigid shift is sample accurate for identical audio");
    check(shifted.map == target.baseline, "rigid shift does not stretch the waveform");
    target.startSeconds = guide.startSeconds + .003; settings.toleranceSeconds = .005;
    check(align(guide, {target}, settings).front().status == AlignmentStatus::Skipped, "tolerance preserves small offsets");
    auto local = source({.58, 1.15, 2.35}); settings = {};
    for (auto method : {AlignmentMethod::Hybrid, AlignmentMethod::Attacks}) {
        settings.method = method;
        const auto proposal = align(guide, {local}, settings).front();
        check(proposal.status != AlignmentStatus::Skipped && validWarp(proposal.map), "local alignment returns a monotonic editable map");
        const std::vector<double> delayed{.58, 1.15, 2.35};
        for (std::size_t i = 0; i < events.size(); ++i) {
            const double error = destination(proposal, delayed[i]) - (guide.startSeconds + events[i]);
            std::printf("method %d event %zu timing error %.3f ms (%zu matched, %zu skipped)\n", int(method), i, error * 1000, proposal.matched, proposal.skipped);
            check(std::abs(error) <= .010, "confident synthetic event lands within 10 ms");
        }
        check(proposal.map.mode == 3 && proposal.map.preservePitch, "vocal profile preserves pitch");
    }
    settings.strength = 0;
    auto identity = align(guide, {local}, settings).front();
    check(identity.map == local.baseline && identity.startSeconds == local.startSeconds, "zero density preserves the exact input map and placement");
    settings = {};
    local.baseline.markers.insert(local.baseline.markers.begin()+1, {"manual", 1.15, 2.30, true});
    const auto locked = align(guide, {local}, settings).front();
    const auto marker = std::find_if(locked.map.markers.begin(), locked.map.markers.end(), [](const auto& p) { return p.id == "manual"; });
    check(marker != locked.map.markers.end() && marker->locked && marker->sourceSeconds == 1.15 &&
          std::abs(locked.startSeconds + marker->targetBeats / 2 - local.startSeconds - 1.15) < 1e-8, "manual locked interior marker survives composition");
    const auto silence = source({});
    check(align(guide, {silence}, settings).front().status == AlignmentStatus::Skipped, "silence is not forced onto a phrase");
    auto unrelated = source({});
    auto noise = std::make_shared<engine::SampleBuffer>(2, 144000, 48000);
    unsigned seed = 17;
    for (unsigned i = 0; i < noise->frames(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        noise->writableChannel(0)[i] = float((double(seed) / 4294967296. - .5) * .15);
        noise->writableChannel(1)[i] = noise->channel(0)[i];
    }
    unrelated.audio = noise;
    check(align(guide, {unrelated}, settings).front().status == AlignmentStatus::Skipped, "unrelated broadband noise is not forced onto vocal events");
    auto far = guide; far.startSeconds += 20;
    check(align(guide, {far}, settings).front().status == AlignmentStatus::Skipped, "search never jumps to distant repetitions");
    bool cancelled = false;
    try { align(guide, std::vector<AlignmentSource>(32, local), settings, [](double p) { return p < .07; }); } catch (const std::exception&) { cancelled = true; }
    check(cancelled, "a batch can cancel without returning partial results");
    settings.method = AlignmentMethod::Shift;
    check(align(guide, std::vector<AlignmentSource>(32, target), settings).size() == 32, "32 targets use a single bounded batch");

    const auto dir = std::filesystem::temp_directory_path() / ("vlt-alignment-" + newUuid()); std::filesystem::create_directories(dir);
    const auto file = (dir / "source.wav").string();
    audio::AudioBuffer buffer(2, guide.audio->frames());
    for (int ch = 0; ch < 2; ++ch) std::copy_n(guide.audio->channel(ch), guide.audio->frames(), buffer.getChannel(ch));
    audio::AudioRecorder recorder; recorder.initialize(48000, 2); recorder.writeWAVFile(file, buffer, 48000);
    EngineController controller; if (!controller.initialize(48000, 256, false)) return 1;
    const auto track = controller.addTrack(TrackKind::Audio, "Vocals");
    const auto a = controller.importAudio(file, track, 2), b = controller.importAudio(file, track, 2.1), c = controller.importAudio(file, track, 2.2);
    std::vector<EngineController::VocalAlignmentClip> snapshots;
    check(bool(controller.prepareVocalAlignment({{track,a},{track,b},{track,c}}, snapshots)), "controller prepares immutable source snapshots");
    settings.method = AlignmentMethod::Shift;
    const auto proposals = align(snapshots[0].source, {snapshots[1].source, snapshots[2].source}, settings);
    const auto depth = controller.undoDepth();
    check(bool(controller.applyVocalAlignment(snapshots, proposals)) && controller.undoDepth() == depth + 1, "all targets apply in one undo operation");
    check(std::abs(controller.audioClip(track,b)->startSeconds-2) < 1./48000 && controller.audioClip(track,a)->startSeconds == 2, "targets move and guide stays untouched");
    check(controller.audioClip(track,b)->warp.empty(), "rigid shift keeps original audio processing");
    controller.undo();
    check(controller.audioClip(track,b)->startSeconds == 2.1 && controller.audioClip(track,c)->startSeconds == 2.2, "one Undo restores every target exactly");
    controller.redo();
    check(std::abs(controller.audioClip(track,c)->startSeconds-2) < 1./48000, "Redo restores the entire batch");
    settings.strength = 0;
    auto zero = align(snapshots[0].source, {snapshots[1].source, snapshots[2].source}, settings);
    check(bool(controller.applyVocalAlignment(snapshots, zero)) && controller.audioClip(track,b)->startSeconds == 2.1, "recalculating at zero restores the saved original state");
    controller.setClipStartSeconds(track,c,2.4);
    const auto beforeStale = controller.undoDepth();
    check(!controller.applyVocalAlignment(snapshots, proposals) && controller.undoDepth() == beforeStale && controller.audioClip(track,b)->startSeconds == 2.1,
          "one stale source rejects the entire batch atomically");
    check(!controller.prepareVocalAlignment({{track,a},{track,a}}, snapshots), "self-alignment is rejected");
    const auto peers = controller.duplicateLinkedClips({{track,b}});
    check(peers.size() == 1, "create a linked peer for source preservation");
    check(bool(controller.prepareVocalAlignment({{track,a},{track,b}}, snapshots)), "capture a cache source fingerprint");
    auto cachedProject = controller.project();
    const auto renderedFile = (dir / "processed.wav").string();
    std::filesystem::copy_file(file, renderedFile);
    for (auto& clip : cachedProject.findTrack(track)->clips) if (clip.id == b) {
        clip.offlineProcess.renderedFilePath = renderedFile;
        clip.offlineProcess.renderedDurationSeconds = clip.durationSeconds;
        clip.offlineProcess.sourceFingerprint = snapshots[1].expectedFingerprint.substr(0, snapshots[1].expectedFingerprint.find(':'));
    }
    check(bool(controller.materializeCollaborationProject(std::move(cachedProject), true)) && controller.offlineProcessCacheValid({track,b}), "legacy active offline cache fixture is valid");
    check(bool(controller.prepareVocalAlignment({{track,a},{track,b}}, snapshots)), "processed audio is prepared without mutating the document");
    settings = {};
    const auto cachedProposal = align(snapshots[0].source, {snapshots[1].source}, settings);
    check(bool(controller.applyVocalAlignment(snapshots,cachedProposal)) && controller.audioClip(track,b)->filePath == renderedFile &&
          controller.audioClip(track,b)->offlineProcess.empty() && controller.audioClip(track,b)->offlineHistory.size() >= 2,
          "Warp promotes cached processing into audio version history exactly once");
    check(!peers.empty() && controller.audioClip(peers[0].trackId,peers[0].clipId)->filePath == file,
          "promoting a target's processed source leaves linked peers untouched");
    controller.undo();
    check(controller.audioClip(track,b)->filePath == file && controller.offlineProcessCacheValid({track,b}), "Undo restores active offline processing and original file");
    check(controller.linkedClipCount({track,b}) == 2, "Undo restores the original content relationship");
    controller.redo();
    const auto savedWarp = controller.audioClip(track,b)->warp;
    const auto package = (dir / "session.vlt").string();
    check(bool(controller.saveProject(package)) && bool(controller.openProject(package)) &&
          controller.audioClip(track,b)->warp == savedWarp && controller.audioClip(track,b)->offlineHistory.size() >= 2,
          "Warp and processed source history survive save and reopen");
    check(std::filesystem::exists(file) && std::filesystem::exists(renderedFile), "alignment never removes original or processed media");
    check(!controller.vocalAlignmentSourcesCurrent(snapshots), "project replacement invalidates an earlier session");
    controller.shutdown(); std::filesystem::remove_all(dir);
    std::printf("%d failures\n", failures); return failures ? 1 : 0;
}
