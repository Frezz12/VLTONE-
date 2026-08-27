#include "ChannelStripPreset.hpp"
#include "EngineController.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
int failures = 0;

bool check(bool condition, const char* label) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
    return condition;
}

bool near(float left, float right) {
    return std::abs(left - right) < 1.0e-6f;
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir =
        fs::temp_directory_path() / "daw_channel_strip_preset_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // The serializer carries the full host/plugin description and both opaque
    // state chunks, while deliberately dropping project-specific routing.
    daw::EngineController::ChannelSnapshot snapshot;
    snapshot.sourceName = "Lead Vocal";
    snapshot.hasSettings = true;
    snapshot.volume = 0.42f;
    snapshot.pan = -0.25f;
    snapshot.muted = true;
    snapshot.outputBusId = "project-specific-bus";
    snapshot.sends.push_back({"send", "bus", 0.75f, true, true});

    daw::EngineController::ChainSlotSnapshot slot;
    slot.model.id = "old-slot";
    slot.model.name = "Test Compressor";
    slot.model.format = daw::PluginFormat::Vst3;
    slot.model.uid = "com.example.compressor";
    slot.model.path = "/moved/plugin.vst3";
    slot.model.vendor = "Example";
    slot.model.mix = 0.67f;
    slot.model.bypassed = true;
    slot.model.channelMode = daw::PluginChannelMode::DualMono;
    slot.model.sidechainTrackId = "old-sidechain";
    slot.model.stateFile = "project-state.bin";
    slot.model.rightStateFile = "project-right-state.bin";
    slot.model.windowWidth = 900;
    slot.model.windowOpen = true;
    slot.model.parameters.push_back({"threshold", -12.5});
    slot.state = {0, 1, 2, 127, 128, 255};
    slot.rightState = {9, 8, 7};
    snapshot.inserts.push_back(slot);

    const fs::path roundTrip = dir / "Lead Vocal.vlts";
    check(daw::ChannelStripPreset::save(snapshot, roundTrip.string()).isOk(),
          "writes a VLTS file");
    check(fs::is_regular_file(roundTrip), "VLTS is one portable file");
    {
        std::ifstream stream(roundTrip, std::ios::binary);
        char first = 0;
        stream.get(first);
        check(first != '{', "VLTS stores plugin chunks in compact binary CBOR");
    }

    daw::EngineController::ChannelSnapshot loaded;
    check(daw::ChannelStripPreset::load(loaded, roundTrip.string()).isOk(),
          "reads the VLTS file back");
    check(loaded.hasSettings && near(loaded.volume, 0.42f) &&
              near(loaded.pan, -0.25f),
          "round-trips volume and pan");
    check(loaded.inserts.size() == 1 &&
              loaded.inserts.front().model.uid == "com.example.compressor" &&
              near(loaded.inserts.front().model.mix, 0.67f) &&
              loaded.inserts.front().model.bypassed,
          "round-trips the plugin and its host settings");
    check(loaded.inserts.front().state == slot.state &&
              loaded.inserts.front().rightState == slot.rightState,
          "round-trips both opaque plugin state chunks");
    check(loaded.sends.empty() && loaded.outputBusId.empty() && !loaded.muted &&
              loaded.inserts.front().model.sidechainTrackId.empty() &&
              loaded.inserts.front().model.stateFile.empty() &&
              !loaded.inserts.front().model.windowOpen,
          "omits sends, routing and project/window-specific state");

    daw::EngineController controller;
    check(controller.initialize(48000, 256, /*openDevice=*/false).isOk(),
          "controller starts headless");
    const std::string source =
        controller.addTrack(daw::TrackKind::Audio, "Preset Source");
    const std::string bus = controller.addTrack(daw::TrackKind::Bus, "Bus");
    const std::string target =
        controller.addTrack(daw::TrackKind::Audio, "Preset Target");

    controller.setTrackVolume(source, 0.35f);
    controller.setTrackPan(source, 0.4f);
    controller.setTrackVolume(target, 1.25f);
    controller.setTrackPan(target, -0.6f);
    controller.setTrackMuted(target, true);
    controller.setTrackMono(target, true);
    controller.setTrackOutputBus(target, bus);
    const std::string send = controller.addSend(target, bus);
    controller.setSendLevel(target, send, 0.8f);
    controller.setSendPreFader(target, send, true);

    const fs::path mixPreset = dir / "Portable Mix.vlts";
    check(controller.saveChannelStripPreset(source, mixPreset.string()).isOk(),
          "captures a preset from a normal channel");
    check(controller.applyChannelStripPreset(target, mixPreset.string()).isOk(),
          "applies that preset to another channel");
    const daw::TrackModel* applied = controller.project().findTrack(target);
    check(applied && near(applied->volume, 0.35f) && near(applied->pan, 0.4f),
          "application changes the destination volume and pan");
    check(applied && applied->muted && applied->mono &&
              applied->outputBusId == bus && applied->sends.size() == 1 &&
              applied->sends.front().id == send &&
              near(applied->sends.front().level, 0.8f) &&
              applied->sends.front().preFader,
          "application preserves destination flags, routing and sends");

    controller.undo();
    applied = controller.project().findTrack(target);
    check(applied && near(applied->volume, 1.25f) && near(applied->pan, -0.6f) &&
              applied->sends.size() == 1 && applied->sends.front().id == send,
          "one undo restores the destination strip without touching sends");

    check(controller.applyChannelStripPreset(
              daw::EngineController::kMasterChannelId, mixPreset.string()).isOk() &&
              near(controller.masterVolume(), 0.35f) &&
              near(controller.masterPan(), 0.4f),
          "a preset made on a track also applies to the master channel");

    {
        std::ofstream broken(dir / "Broken.vlts", std::ios::binary);
        broken << "not a preset";
    }
    daw::EngineController::ChannelSnapshot ignored;
    check(daw::ChannelStripPreset::load(ignored, (dir / "Broken.vlts").string())
              .isError(),
          "rejects a corrupt VLTS file cleanly");

    fs::remove_all(dir, ec);
    if (failures) std::printf("\nFAILURES PRESENT: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
