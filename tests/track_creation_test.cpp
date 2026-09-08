#include "EngineController.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace {
int failures = 0;
bool check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
    return ok;
}
double gain(daw::EngineController& controller, const std::string& track) {
    const auto* model = controller.project().findTrack(track);
    auto* plugin = model && !model->inserts.empty()
        ? controller.insertInstance(track, model->inserts.front().id) : nullptr;
    return plugin ? plugin->parameterValue(0) : -100;
}
}

int main() {
    daw::EngineController draft, target;
    if (!check(draft.initialize(48000, 256, false).isOk() && target.initialize(48000, 256, false).isOk(),
               "controllers initialize without audio hardware")) return 1;
    daw::plugins::PluginDescriptor descriptor;
    descriptor.format = daw::plugins::Format::Clap;
    descriptor.uid = "com.daw.test.gain";
    descriptor.path = DAW_TEST_CLAP_PATH;
    descriptor.name = "Test Gain";
    const auto preview = draft.addTrack(daw::TrackKind::Audio, "Draft");
    const auto slot = draft.addInsert(preview, descriptor);
    auto* instance = draft.insertInstance(preview, slot);
    if (!check(instance != nullptr, "real CLAP fixture loads in the draft")) return 1;
    draft.setInsertParameter(preview, slot, instance->parameters()[0].id, 0.29);
    draft.pumpPreviewPluginEvents();
    check(std::abs(instance->parameterValue(0) - 0.29) < 1e-6, "preview applies queued host edits without an audio device");
    std::vector<std::uint8_t> idleBefore, idleAfter;
    instance->saveState(idleBefore);
    for (int i = 0; i < 5; ++i) draft.pumpPreviewPluginEvents();
    instance->saveState(idleAfter);
    check(idleBefore.size() >= sizeof(double) * 3 && idleAfter.size() >= sizeof(double) * 3 &&
          std::memcmp(idleBefore.data() + sizeof(double) * 2, idleAfter.data() + sizeof(double) * 2, sizeof(double)) == 0,
          "idle preview ticks do not keep processing DSP");
    // Direct state change emulates a native GUI. No controller parameter mirror
    // carries the result: only the opaque state can restore this value.
    std::vector<std::uint8_t> nativeState(sizeof(double) * 2);
    const double values[]{0.37, 0.12};
    std::memcpy(nativeState.data(), values, sizeof(values));
    if (!check(instance->loadState(nativeState), "fixture accepts native state")) return 1;
    auto chain = draft.copyChannelStrip(preview, false);
    if (!check(chain.inserts.size() == 1, "draft captures one insert")) return 1;
    chain.inserts[0].model.parameters.clear();
    chain.inserts[0].model.windowOpen = true;
    chain.inserts[0].model.stateFile = "old-state.bin";
    const auto bus = target.addTrack(daw::TrackKind::Bus, "Vocal bus");
    const auto existing = target.addTrack(daw::TrackKind::Audio, "Vocal 1");
    const auto baseline = target.project().tracks.size();
    const auto depth = target.undoDepth();
    daw::EngineController::TrackCreationRequest request;
    request.count = 3; request.name = "Vocal"; request.mono = true;
    request.inputEnabled = true; request.inputChannel = 2; request.inputChannelCount = 1;
    request.outputBusId = bus; request.inserts = chain.inserts;
    std::vector<std::string> ids;
    if (!check(target.createTracks(request, ids).isOk() && ids.size() == 3, "three tracks are created together")) return 1;
    std::set<std::string> slotIds;
    std::set<const void*> instances;
    bool correct = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto* track = target.project().findTrack(ids[i]);
        if (!track || track->inserts.size() != 1) { correct = false; continue; }
        const auto& insert = track->inserts[0];
        slotIds.insert(insert.id);
        instances.insert(target.insertInstance(track->id, insert.id));
        correct &= track->name == "Vocal " + std::to_string(i + 2) && track->mono &&
            track->inputEnabled && track->inputChannel == 2 && track->inputChannelCount == 1 &&
            track->outputBusId == bus && !insert.windowOpen && insert.stateFile.empty() &&
            std::abs(gain(target, track->id) - 0.37) < 1e-6;
    }
    check(correct && slotIds.size() == 3 && instances.size() == 3, "unique names and instances preserve routing and opaque state");
    check(target.undoDepth() == depth + 1, "one undo entry for the whole batch");
    target.undo();
    check(target.project().tracks.size() == baseline && target.project().findTrack(existing), "one undo removes all new tracks and preserves existing tracks");
    target.redo();
    check(target.project().tracks.size() == baseline + 3 && std::abs(gain(target, ids[2]) - 0.37) < 1e-6,
          "redo restores identities and settings");
    auto* first = target.insertInstance(ids[0], target.project().findTrack(ids[0])->inserts[0].id);
    const double changed[]{0.81, 0.12};
    std::memcpy(nativeState.data(), changed, sizeof(changed));
    first->loadState(nativeState);
    check(std::abs(gain(target, ids[0]) - 0.81) < 1e-6 && std::abs(gain(target, ids[1]) - 0.37) < 1e-6,
          "editing one created plugin leaves the other instances unchanged");

    const auto stableSize = target.project().tracks.size();
    const auto stableDepth = target.undoDepth();
    auto reject = [&](auto invalid, const char* name) {
        std::vector<std::string> result;
        check(!target.createTracks(invalid, result) && result.empty() &&
              target.project().tracks.size() == stableSize && target.undoDepth() == stableDepth, name);
    };
    auto invalid = request; invalid.count = 0; reject(invalid, "zero tracks rejected without mutation");
    invalid.count = 65; reject(invalid, "oversized batch rejected without mutation");
    invalid = request; invalid.outputBusId = "missing"; reject(invalid, "missing output rejected without mutation");
    invalid = request; invalid.inserts[0].state = {1, 2, 3}; reject(invalid, "failed state restore rolls back the entire batch");
    invalid = request; invalid.inserts[0].model.uid = "missing.plugin";
    reject(invalid, "failed plugin load rolls back the entire batch");
    for (const auto kind : {daw::TrackKind::Audio, daw::TrackKind::Midi, daw::TrackKind::Pattern,
            daw::TrackKind::Automation, daw::TrackKind::Bus, daw::TrackKind::Aux, daw::TrackKind::Folder}) {
        daw::EngineController::TrackCreationRequest plain; plain.kind = kind;
        check(target.createTracks(plain, ids).isOk() && ids.size() == 1, "track type can be created");
        if (kind == daw::TrackKind::Pattern) {
            const auto* pattern = target.project().findTrack(ids[0]);
            check(pattern && pattern->clips.size() == 1 && pattern->clips[0].durationSeconds > 0,
                  "pattern starts with its editable pattern clip");
        }
        target.undo();
    }
    auto group = daw::EngineController::TrackCreationRequest{};
    group.kind = daw::TrackKind::Folder; group.summing = true;
    check(target.createTracks(group, ids).isOk() && target.project().findTrack(ids[0])->summing,
          "summing folder preserves its audio role");
    target.undo();
    auto dual = request;
    dual.count = 2;
    dual.inserts[0].model.channelMode = daw::PluginChannelMode::DualMono;
    dual.inserts[0].model.bypassed = true;
    dual.inserts[0].rightState = nativeState;
    if (check(target.createTracks(dual, ids).isOk(), "dual mono chain creates successfully")) {
        bool channels = true;
        for (const auto& id : ids) {
            const auto* model = target.project().findTrack(id);
            channels &= model && model->inserts[0].bypassed && std::abs(gain(target, id) - 0.37) < 1e-6;
            target.setInsertEditorChannel(id, model->inserts[0].id, daw::PluginEditorChannel::Right);
            channels &= std::abs(gain(target, id) - 0.81) < 1e-6;
        }
        check(channels, "both independent mono states and bypass survive batch creation");
        target.undo();
    }
    const auto sampler = draft.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
    const auto midi = draft.addTrack(daw::TrackKind::Midi);
    if (check(sampler && draft.setTrackInstrumentPlugin(midi, *sampler), "instrument template loads")) {
        daw::EngineController::TrackCreationRequest synth;
        synth.kind = daw::TrackKind::Midi; synth.count = 2;
        synth.instrument.emplace();
        synth.instrument->model = draft.project().findTrack(midi)->instrument;
        draft.insertInstance(midi, synth.instrument->model.id)->saveState(synth.instrument->state);
        if (check(target.createTracks(synth, ids).isOk(), "instrument tracks create together")) {
            bool ownership = true;
            for (const auto& id : ids) {
                const auto* model = target.project().findTrack(id);
                ownership &= model && model->instrument.isLoaded() &&
                    model->samplerFx.ownerInstrumentId == model->instrument.id &&
                    target.insertInstance(id, model->instrument.id);
            }
            check(ownership, "each sampler belongs to its new instrument slot");
        }
    }
    return failures ? 1 : 0;
}
