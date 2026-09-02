#include "EngineController.hpp"
#include "collaboration/SharedMutationSink.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

bool check(bool condition, const char* description) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) ++failures;
    return condition;
}

class FakeSharedMutationSink final : public daw::collab::SharedMutationSink {
public:
    daw::collab::SharedMutationResult result =
        daw::collab::SharedMutationResult::Submitted;

    int timeSignatureCalls = 0;
    int numerator = 0;
    int denominator = 0;

    int projectKeyCalls = 0;
    int keyRoot = 0;
    std::string scale;

    int aiInstructionsCalls = 0;
    std::string aiInstructions;

    int renameTrackCalls = 0;
    std::string renamedTrackId;
    std::string trackName;

    int trackMutedCalls = 0;
    std::string mutedTrackId;
    bool muted = false;

    int tracksMutedCalls = 0;
    std::vector<std::string> mutedTrackIds;
    bool tracksMuted = false;

    int clearAllMutesCalls = 0;
    std::vector<std::string> clearedTrackIds;

    daw::collab::SharedMutationResult setTimeSignature(
        int nextNumerator, int nextDenominator) override {
        ++timeSignatureCalls;
        numerator = nextNumerator;
        denominator = nextDenominator;
        return result;
    }

    daw::collab::SharedMutationResult setProjectKey(
        int root, std::string_view scaleId) override {
        ++projectKeyCalls;
        keyRoot = root;
        scale = scaleId;
        return result;
    }

    daw::collab::SharedMutationResult setAiInstructions(
        std::string_view text) override {
        ++aiInstructionsCalls;
        aiInstructions = text;
        return result;
    }

    daw::collab::SharedMutationResult renameTrack(
        std::string_view trackId, std::string_view name) override {
        ++renameTrackCalls;
        renamedTrackId = trackId;
        trackName = name;
        return result;
    }

    daw::collab::SharedMutationResult setTrackMuted(
        std::string_view trackId, bool nextMuted) override {
        ++trackMutedCalls;
        mutedTrackId = trackId;
        muted = nextMuted;
        return result;
    }

    daw::collab::SharedMutationResult setTracksMuted(
        std::span<const std::string> trackIds, bool nextMuted) override {
        ++tracksMutedCalls;
        mutedTrackIds.assign(trackIds.begin(), trackIds.end());
        tracksMuted = nextMuted;
        return result;
    }

    daw::collab::SharedMutationResult clearAllMutes(
        std::span<const std::string> mutedTrackIds) override {
        ++clearAllMutesCalls;
        clearedTrackIds.assign(mutedTrackIds.begin(), mutedTrackIds.end());
        return result;
    }
};

void verifyConsumedMutation(daw::collab::SharedMutationResult result) {
    const bool submitted =
        result == daw::collab::SharedMutationResult::Submitted;
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          submitted ? "submitted fixture initializes"
                    : "blocked fixture initializes");
    controller.setProjectKey(2, "minor");
    controller.setAiInstructions("before");
    const std::string trackId =
        controller.addTrack(daw::TrackKind::Audio, "Before");
    controller.setTrackMuted(trackId, true);
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    check(controller.sharedMutationSink() == &sink,
          submitted ? "submitted sink attaches" : "blocked sink attaches");

    check(controller.setTimeSignature(7, 8) == result &&
              controller.setProjectKey(-1, "dorian") == result &&
              controller.setAiInstructions("after") == result &&
              controller.renameTrack(trackId, "After") == result &&
              controller.setTrackMuted(trackId, false) == result &&
              controller.clearAllMutes() == result,
          submitted ? "submitted outcomes reach the initiating UI"
                    : "blocked outcomes reach the initiating UI");

    check(sink.timeSignatureCalls == 1 && sink.numerator == 7 &&
              sink.denominator == 8,
          submitted ? "submitted time-signature payload is exact"
                    : "blocked time-signature payload is exact");
    check(sink.projectKeyCalls == 1 && sink.keyRoot == 11 &&
              sink.scale == "dorian",
          submitted ? "submitted project-key payload is canonical"
                    : "blocked project-key payload is canonical");
    check(sink.aiInstructionsCalls == 1 && sink.aiInstructions == "after",
          submitted ? "submitted AI-instructions payload is exact"
                    : "blocked AI-instructions payload is exact");
    check(sink.renameTrackCalls == 1 && sink.renamedTrackId == trackId &&
              sink.trackName == "After",
          submitted ? "submitted rename payload is exact"
                    : "blocked rename payload is exact");
    check(sink.trackMutedCalls == 1 && sink.mutedTrackId == trackId &&
              !sink.muted,
          submitted ? "submitted mute payload is exact"
                    : "blocked mute payload is exact");
    check(sink.clearAllMutesCalls == 1 &&
              sink.clearedTrackIds == std::vector<std::string>{trackId},
          submitted ? "submitted clear-mutes batch is exact"
                    : "blocked clear-mutes batch is exact");

    const auto* track = controller.project().findTrack(trackId);
    check(controller.timeSigNumerator() == 4 &&
              controller.timeSigDenominator() == 4 &&
              controller.keyRoot() == 2 &&
              controller.projectScale() == "minor" &&
              controller.aiInstructions() == "before" && track &&
              track->name == "Before" && track->muted,
          submitted ? "submitted commands never mutate the local document"
                    : "blocked commands never mutate the local document");
    check(controller.undoDepth() == undoDepth,
          submitted ? "submitted commands never enter legacy undo"
                    : "blocked commands never enter legacy undo");

    {
        FakeSharedMutationSink replacement;
        controller.attachSharedMutationSink(replacement);
        check(!controller.detachSharedMutationSink(sink) &&
                  controller.sharedMutationSink() == &replacement,
              "stale sink cannot detach its replacement");
        check(controller.detachSharedMutationSink(replacement) &&
                  controller.sharedMutationSink() == nullptr,
              "active sink detaches explicitly");
    }
    controller.setAiInstructions("after detach");
    check(controller.aiInstructions() == "after detach",
          "local mutation is safe after the detached sink is destroyed");
}

void verifyAtomicMuteGesture(daw::collab::SharedMutationResult result) {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "atomic-mute fixture initializes");
    const std::string folder = controller.addFolder(false, "Folder");
    const std::string child =
        controller.addTrack(daw::TrackKind::Audio, "Child");
    const std::string peer =
        controller.addTrack(daw::TrackKind::Audio, "Peer");
    controller.moveTrackToFolder(child, folder);
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    const std::array<std::string, 4> selected{folder, child, folder, peer};
    const auto outcome = controller.setTracksMuted(selected, true);

    check(outcome == result && sink.tracksMutedCalls == 1 &&
              sink.tracksMuted &&
              sink.mutedTrackIds ==
                  std::vector<std::string>({folder, child, peer}),
          "multi/folder mute expands descendants and deduplicates once");
    const bool locallyApplied =
        result == daw::collab::SharedMutationResult::LocalFallback;
    check(controller.project().findTrack(folder)->muted == locallyApplied &&
              controller.project().findTrack(child)->muted == locallyApplied &&
              controller.project().findTrack(peer)->muted == locallyApplied &&
              controller.undoDepth() == undoDepth,
          locallyApplied
              ? "atomic fallback preserves local mute without legacy undo"
              : "submitted/blocked atomic mute avoids local mutation and undo");
    check(controller.detachSharedMutationSink(sink),
          "atomic-mute sink detaches before its lifetime ends");
}

void verifyLocalFallback() {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "fallback fixture initializes");
    const std::string first =
        controller.addTrack(daw::TrackKind::Audio, "First");
    const std::string second =
        controller.addTrack(daw::TrackKind::Audio, "Second");

    FakeSharedMutationSink sink;
    sink.result = daw::collab::SharedMutationResult::LocalFallback;
    controller.attachSharedMutationSink(sink);

    std::size_t undoDepth = controller.undoDepth();
    controller.setTimeSignature(7, 8);
    check(controller.timeSigNumerator() == 7 &&
              controller.timeSigDenominator() == 8 &&
              controller.undoDepth() == undoDepth + 1,
          "time signature keeps local mutation and undo on fallback");
    controller.undo();
    check(controller.timeSigNumerator() == 4 &&
              controller.timeSigDenominator() == 4,
          "fallback time-signature undo restores the old value");
    controller.redo();
    check(controller.timeSigNumerator() == 7 &&
              controller.timeSigDenominator() == 8,
          "fallback time-signature redo restores the new value");

    undoDepth = controller.undoDepth();
    controller.setProjectKey(-1, "dorian");
    check(controller.keyRoot() == 11 && controller.projectScale() == "dorian" &&
              controller.undoDepth() == undoDepth + 1 && sink.keyRoot == 11,
          "project key keeps canonical local value and sink payload");
    controller.undo();
    check(controller.keyRoot() == 0 && controller.projectScale() == "major",
          "fallback project-key undo restores the old value");
    controller.redo();
    check(controller.keyRoot() == 11 && controller.projectScale() == "dorian",
          "fallback project-key redo restores the new value");

    undoDepth = controller.undoDepth();
    controller.setAiInstructions("mix quietly");
    check(controller.aiInstructions() == "mix quietly" &&
              controller.undoDepth() == undoDepth,
          "AI instructions keep their non-undoable local fallback semantics");
    const int aiCalls = sink.aiInstructionsCalls;
    controller.setAiInstructions("mix quietly");
    check(sink.aiInstructionsCalls == aiCalls,
          "unchanged AI instructions do not reach the command sink");

    undoDepth = controller.undoDepth();
    controller.renameTrack(first, "Lead");
    check(controller.project().findTrack(first)->name == "Lead" &&
              controller.undoDepth() == undoDepth + 1,
          "track rename keeps local mutation and undo on fallback");
    controller.undo();
    check(controller.project().findTrack(first)->name == "First",
          "fallback rename undo restores the old name");
    controller.redo();
    check(controller.project().findTrack(first)->name == "Lead",
          "fallback rename redo restores the new name");

    undoDepth = controller.undoDepth();
    controller.setTrackMuted(first, true);
    controller.setTrackMuted(second, true);
    check(controller.project().findTrack(first)->muted &&
              controller.project().findTrack(second)->muted &&
              controller.undoDepth() == undoDepth,
          "track mute keeps its non-undoable local fallback semantics");
    const int muteCalls = sink.trackMutedCalls;
    controller.setTrackMuted(first, true);
    check(sink.trackMutedCalls == muteCalls,
          "unchanged track mute does not reach the command sink");
    controller.clearAllMutes();
    check(!controller.project().findTrack(first)->muted &&
              !controller.project().findTrack(second)->muted &&
              controller.undoDepth() == undoDepth &&
              sink.clearAllMutesCalls == 1 &&
              sink.clearedTrackIds ==
                  std::vector<std::string>({first, second}),
          "clear-all-mutes falls back atomically without legacy undo");

    check(controller.detachSharedMutationSink(sink),
          "fallback sink detaches before its lifetime ends");
}

} // namespace

int main() {
    check(daw::collab::marksLocalFileDirty(
              daw::collab::SharedMutationResult::LocalFallback) &&
              !daw::collab::marksLocalFileDirty(
                  daw::collab::SharedMutationResult::Submitted) &&
              !daw::collab::marksLocalFileDirty(
                  daw::collab::SharedMutationResult::Blocked),
          "only LocalFallback enters legacy file-dirty handling");
    verifyConsumedMutation(daw::collab::SharedMutationResult::Submitted);
    verifyConsumedMutation(daw::collab::SharedMutationResult::Blocked);
    verifyLocalFallback();
    verifyAtomicMuteGesture(daw::collab::SharedMutationResult::Submitted);
    verifyAtomicMuteGesture(daw::collab::SharedMutationResult::Blocked);
    verifyAtomicMuteGesture(daw::collab::SharedMutationResult::LocalFallback);

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
