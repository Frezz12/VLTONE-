#include "EngineController.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>

int main() {
    namespace fs = std::filesystem;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const auto directory = fs::temp_directory_path() / ("vlt-sampler-duplicate-" + daw::newUuid());
    fs::create_directories(directory);
    const auto sample = (directory / "source.wav").string();
    audio::AudioBuffer tone(2, 4800);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 4800; ++i) tone.getChannel(ch)[i] = .2f;
    audio::AudioRecorder recorder;
    recorder.initialize(48000, 2);
    recorder.writeWAVFile(sample, tone, 48000);
    for (const bool opaque : {false, true}) {
        for (const bool withClips : {false, true}) {
            daw::EngineController controller;
            if (!controller.initialize(48000, 512, false)) return 2;
            const auto track = controller.addTrack(daw::TrackKind::Instrument, "Source");
            if (!controller.loadInstrumentSampler(track, sample)) return 2;
            const auto slot = controller.project().findTrack(track)->instrument.id;
            const auto clip = controller.addMidiClip(track, 1, 2);
            controller.addNote(track, clip, 60, 0, 1);
            const std::vector<std::pair<std::string, double>> settings{
                {"vol", .43}, {"pan", -.35}, {"rootnote", 48}, {"startoffset", .12},
                {"amp.on", 1}, {"amp.att", .04}, {"amp.rel", .3},
                {"loop.mode", 1}, {"loop.start", .2}, {"loop.end", .8},
                {"stretch.time", 1.25}, {"stretch.pitch", -3}};
            if (opaque) {
                // Presets/native state can precede the document's mirror.
                controller.setInsertParameter(track, slot, "vol", 1.0);
                controller.setInsertParameter(track, slot, "rootnote", 60);
                controller.play();
                audio::AudioBuffer input(2, 512), output(2, 512);
                input.clear();
                check(controller.processDeviceBlockForTest(input, output, 512),
                      "previous knob events reach the processor before loading a preset");
                controller.stop();
                auto* instance = controller.samplerInstance(track, slot);
                std::vector<std::uint8_t> bytes;
                instance->saveState(bytes);
                auto state = nlohmann::json::parse(bytes);
                for (const auto& [id, value] : settings) state["params"][id] = value;
                const auto text = state.dump();
                bytes.assign(text.begin(), text.end());
                check(instance->loadState(bytes), "source accepts a Sampler preset");
            } else {
                // Duplicate immediately, before pending knob events render.
                for (const auto& [id, value] : settings)
                    controller.setInsertParameter(track, slot, id, value);
            }
            const auto depth = controller.undoDepth();
            const auto copy = controller.duplicateTrack(track, true, withClips);
            const auto matches = [&] {
                const auto* model = controller.project().findTrack(copy);
                if (!model || model->instrument.id == slot ||
                    model->clips.size() != (withClips ? 1u : 0u)) return false;
                auto* instance = controller.samplerInstance(copy, model->instrument.id);
                if (!instance || instance->samplePath() != sample) return false;
                for (const auto& [id, value] : settings)
                    if (std::abs(instance->parameterValue(instance->parameterIndexForId(id)) - value) > 1e-6) {
                        std::printf("  %s: expected %.6f, got %.6f\n", id.c_str(), value,
                            instance->parameterValue(instance->parameterIndexForId(id)));
                        return false;
                    }
                return true;
            };
            check(matches(), opaque ? "duplicate preserves live preset settings" : "duplicate preserves pending knob edits");
            check(controller.undoDepth() == depth + 1, "duplicate is one undo step");
            controller.undo();
            check(!controller.project().findTrack(copy), "undo removes the copied track");
            controller.redo();
            check(matches(), "redo restores Sampler settings and sample");
            controller.addTrack(daw::TrackKind::Audio, "Unrelated");
            check(matches(), "later graph rebuild retains copied settings");
            controller.setInsertParameter(track, slot, "vol", .8);
            check(matches(), "copy remains independent of source edits");
            const auto package = (directory / (std::string(opaque ? "preset" : "knobs") +
                (withClips ? "-clips.vlt" : "-empty.vlt"))).string();
            check(controller.saveProject(package).isOk(), "duplicated Sampler project saves");
            daw::EngineController reopened;
            if (!reopened.initialize(48000, 512, false)) return 2;
            check(reopened.openProject(package).isOk(), "duplicated Sampler project reopens");
            const auto* saved = reopened.project().findTrack(copy);
            auto* loaded = saved ? reopened.samplerInstance(copy, saved->instrument.id) : nullptr;
            bool preserved = loaded && loaded->rawSample();
            if (loaded) for (const auto& [id, value] : settings)
                preserved &= std::abs(loaded->parameterValue(loaded->parameterIndexForId(id)) - value) < 1e-6;
            check(preserved, "save and reopen retain all copied Sampler settings");
        }
    }
    fs::remove_all(directory);
    return failures ? 1 : 0;
}
