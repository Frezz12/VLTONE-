#include "EngineController.hpp"
#include "MidiTools.hpp"
#include "Internal/SamplerInstance.hpp"
#include "collaboration/SharedMutationSink.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

namespace dc = daw::collab;
static int failures = 0;
static bool check(bool value, const char* message) {
    std::printf("%s  %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures; return value;
}
struct Sink final : dc::SharedMutationSink {
    int calls = 0;
    dc::SharedMutationResult result = dc::SharedMutationResult::Submitted;
    std::shared_ptr<dc::BatchCommand> batch;
    bool handlesCloudBinding() override { return true; }
    dc::SharedMutationResult submit(dc::SharedMutationRequest request) override {
        ++calls;
        if (auto* value = std::get_if<std::shared_ptr<dc::BatchCommand>>(&request.body)) batch = *value;
        return result;
    }
    dc::SharedMutationResult setTimeSignature(int, int) override { return result; }
    dc::SharedMutationResult setProjectKey(int, std::string_view) override { return result; }
    dc::SharedMutationResult setAiInstructions(std::string_view) override { return result; }
    dc::SharedMutationResult renameTrack(std::string_view, std::string_view) override { return result; }
    dc::SharedMutationResult setTrackMuted(std::string_view, bool) override { return result; }
    dc::SharedMutationResult setTracksMuted(std::span<const std::string>, bool) override { return result; }
    dc::SharedMutationResult clearAllMutes(std::span<const std::string>) override { return result; }
};

int main() {
    using daw::NoteModel;
    using Notes = daw::miditools::Notes;
    const Notes source{{"held", 60, 0, 40, 80}, {"inside", 62, 5, 1, 90}, {"after", 67, 40, 2, 99}};
    const Notes generated{{{}, 64, 0, 3, 100}};
    const auto replaced = daw::miditools::insertGeneratedNotes(source, generated, 4, 36, false);
    check(replaced.size() == 4 && replaced[0].id == "held" && replaced[0].lengthBeats == 4 &&
          replaced[1].startBeats == 4 && replaced[2].id.empty() && replaced[2].startBeats == 36 &&
          replaced[2].lengthBeats == 4 && replaced[3] == source[2],
          "replacement preserves both exterior tails and unaffected notes");
    const auto merged = daw::miditools::insertGeneratedNotes(source, generated, 4, 36, true);
    check(merged.size() == 4 && merged.front() == source.front() && merged.back() == source.back(),
          "add mode preserves complete existing notes");
    daw::EngineController controller;
    if (!check(controller.initialize(48000, 256, false).isOk(), "controller initializes")) return failures;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Generated");
    const auto clipId = controller.addMidiClip(track, 6, 2);
    const auto clip = [&]() -> const daw::ClipModel& { return controller.project().findTrack(track)->clips.front(); };
    const auto depth = controller.undoDepth();
    check(controller.applyGeneratedMidi(track, clipId, generated, 32) && clip().durationSeconds == 16 &&
          !daw::midiNotes(clip()).front().id.empty() && controller.undoDepth() == depth + 1,
          "eight bars extend a short clip and insert notes in one undo");
    const auto applied = daw::midiNotes(clip());
    controller.undo();
    check(clip().durationSeconds == 2 && daw::midiNotes(clip()).empty(), "undo restores notes and duration together");
    controller.redo();
    check(daw::midiNotes(clip()) == applied && clip().durationSeconds == 16, "redo restores exact note identities");
    controller.beginClipTrimEdit({{track, clipId}});
    controller.setClipTrim(track, clipId, 8, 2, 2); controller.endClipTrimEdit("Trim fixture");
    check(controller.applyGeneratedMidi(track, clipId, generated, 12) && clip().durationSeconds == 4 &&
          clip().offsetSeconds == 2 && clip().startSeconds == 8,
          "trimmed clips extend using source-note coordinates without moving the visible start");
    const auto validDepth = controller.undoDepth();
    auto invalid = generated; invalid[0].startBeats = std::numeric_limits<double>::quiet_NaN();
    check(!controller.applyGeneratedMidi(track, clipId, invalid, 32) && controller.undoDepth() == validDepth,
          "invalid generated data makes no partial edit");
    const auto pattern = controller.addPattern("Phrase");
    const auto child = controller.addPatternInstrument(pattern, daw::plugins::sampler::SamplerInstance::staticDescriptor());
    if (check(!child.empty(), "pattern fixture loads an internal instrument")) {
        const auto member = controller.project().findTrack(child)->clips.front().id;
        const auto beforeDuration = controller.project().findTrack(pattern)->clips.front().durationSeconds;
        const auto patternDepth = controller.undoDepth();
        check(controller.applyGeneratedMidi(child, member, generated, 32) &&
              controller.project().findTrack(pattern)->clips.front().durationSeconds == 16 &&
              controller.undoDepth() == patternDepth + 1, "pattern parent grows in the same undo entry");
        controller.undo();
        check(controller.project().findTrack(pattern)->clips.front().durationSeconds == beforeDuration &&
              daw::midiNotes(controller.project().findTrack(child)->clips.front()).empty(),
              "pattern undo restores its member and owner");
    }
    Sink sink; controller.attachSharedMutationSink(sink);
    controller.setSharedEditingAllowed(false);
    check(!controller.applyGeneratedMidi(track, clipId, generated, 32) && sink.calls == 0, "read-only cloud sessions reject application");
    controller.setSharedEditingAllowed(true);
    const auto cloudDepth = controller.undoDepth(); const auto cloudNotes = daw::midiNotes(clip()); const auto cloudDuration = clip().durationSeconds;
    check(controller.applyGeneratedMidi(track, clipId, generated, 64) && sink.calls == 1 && sink.batch &&
          controller.undoDepth() == cloudDepth && daw::midiNotes(clip()) == cloudNotes && clip().durationSeconds == cloudDuration,
          "cloud application submits one atomic batch without a parallel local edit");
    sink.result = dc::SharedMutationResult::Blocked;
    check(!controller.applyGeneratedMidi(track, clipId, generated, 64) && sink.calls == 2 && daw::midiNotes(clip()) == cloudNotes,
          "blocked cloud application leaves the target untouched");
    controller.detachSharedMutationSink(sink);
    std::printf("\n%s\n", failures ? "FAILURES PRESENT" : "ALL PASSED");
    return failures;
}
