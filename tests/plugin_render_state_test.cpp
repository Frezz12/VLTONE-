#include "EngineController.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Host/PluginNode.hpp"
#include "platform/AudioFileDecoder.hpp"
#if DAW_ENABLE_CLAP
#include "Clap/ClapFactory.hpp"
#endif
#if DAW_ENABLE_VST3
#include "Vst3/Vst3Factory.hpp"
#endif
#if DAW_ENABLE_AU
#include "Au/AuFactory.hpp"
#include <AudioUnit/AudioUnitParameters.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace daw;
using namespace daw::plugins;
namespace fs = std::filesystem;
namespace ap = audio::platform;
namespace {
constexpr unsigned kBlock = 128;
void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}
struct TempDirectory {
    fs::path path = fs::temp_directory_path() / ("daw-plugin-render-state-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { fs::create_directories(path); }
    ~TempDirectory() { std::error_code error; fs::remove_all(path, error); }
};

void checkFormat(PluginFactory& factory, const PluginDescriptor& descriptor,
                 const std::string& parameterId, double dryValue,
                 double tunedValue, double editedValue, float editedGain) {
    TempDirectory temp;
    const auto source = (temp.path / "voice.wav").string();
    ap::WriteSpec format;
    format.encoding = ap::Encoding::Float32;
    ap::AudioFileWriter writer;
    std::vector<float> samples(48000, 0.2f);
    const float* inputs[]{samples.data(), samples.data()};
    require(bool(writer.open(source, format, 48000, 2, 48000)), "open source");
    require(bool(writer.write(inputs, 48000)) && bool(writer.close()), "write source");
    float left[kBlock]{}, right[kBlock]{};
    float* outputs[]{left, right};

    // A real format-native preset with gain 0.5, independent of any project
    // parameter mirror. AU uses the system EQ's global gain in dB.
    auto preset = factory.create(descriptor);
    require(preset && preset->activate({48000, kBlock, false}), "activate preset");
    preset->startProcessing();
    const auto index = preset->parameterIndexForId(parameterId);
    require(index >= 0, "find gain parameter");
    PluginEvent edit;
    edit.kind = PluginEvent::Kind::ParamValue;
    edit.paramIndex = unsigned(index); edit.value = tunedValue;
    PluginProcessContext context;
    context.inputs = inputs; context.inputChannels = 2;
    context.outputs = outputs; context.outputChannels = 2; context.frames = kBlock;
    context.inputEvents = std::span(&edit, 1);
    require(preset->process(context) != PluginProcessDisposition::Error, "process preset");
    preset->setParameterFromHost(unsigned(index), tunedValue);
    std::vector<std::uint8_t> state;
    require(preset->saveState(state) && !state.empty(), "save native preset");
    preset->stopProcessing(); preset->deactivate();

    for (const std::string scope : {"track", "group", "master", "clip", "dual-mono"}) {
        EngineController controller;
        require(bool(controller.initialize(48000, kBlock, false)), "initialize controller");
        const auto track = controller.addTrack(TrackKind::Audio, "Voice");
        const auto clip = controller.importAudio(source, track, 0);
        require(!clip.empty(), "import source");
        const std::string channel = scope == "master" ? EngineController::kMasterChannelId
            : scope == "group" ? controller.addTrack(TrackKind::Group, "Vocals") : track;
        if (scope == "group") require(controller.setTrackOutputBus(track, channel), "route group");
        const auto slot = scope == "clip" ? controller.addClipFxInsert(track, clip, descriptor)
                                         : controller.addInsert(channel, descriptor);
        require(!slot.empty(), "insert plugin");
        const bool dual = scope == "dual-mono";
        if (dual) require(controller.setInsertChannelMode(channel, slot, PluginChannelMode::DualMono), "dual mono");
        const auto eachSide = [&](const auto& action) {
            controller.setInsertEditorChannel(channel, slot, PluginEditorChannel::Left);
            action();
            if (dual) {
                controller.setInsertEditorChannel(channel, slot, PluginEditorChannel::Right);
                action();
            }
            controller.setInsertEditorChannel(channel, slot, PluginEditorChannel::Left);
        };
        eachSide([&] { controller.setInsertParameter(channel, slot, parameterId, dryValue); });
        engine::GraphProcessor live(2);
        live.setGraph(controller.routingGraph());
        engine::AudioBlock block(outputs, 2, kBlock);
        const auto play = [&] {
            for (unsigned i = 0; i < 4; ++i)
                require(bool(live.process(block, kBlock, i * kBlock, true)), "live DSP");
        };
        play(); // consume startup/host events before loading the native preset
        eachSide([&] { require(controller.insertInstance(channel, slot)->loadState(state), "load preset"); });
        play();
        require(std::abs(left[kBlock - 1] - 0.1f) < 1e-4f &&
                std::abs(right[kBlock - 1] - 0.1f) < 1e-4f, "live preset halves both channels");
        require(controller.insertModel(channel, slot)->parameters.front().value == dryValue,
                "document deliberately retains stale dry parameter");

        rendering::Spec spec;
        spec.outputDir = temp.path.string(); spec.baseName = scope;
        spec.file.encoding = ap::Encoding::Float32;
        const auto render = [&](float expectedLeft, float expectedRight) {
            rendering::Report report;
            const auto result = controller.renderProject(spec, {}, report);
            require(bool(result) && report.files.size() == 1, "render: " + result.message());
            ap::DecodedAudio decoded;
            require(bool(ap::decodeAudioFile(report.files.front(), decoded)), "decode export");
            require(decoded.frames == 48000 && decoded.channels == 2, "export shape");
            require(std::abs(decoded.interleaved[24000] - expectedLeft) < 1e-4f &&
                    std::abs(decoded.interleaved[24001] - expectedRight) < 1e-4f,
                    descriptor.name + " / " + scope + ": export retains audible settings");
        };
        render(0.1f, 0.1f);
        // Two edits before the next audio callback: the last value must win.
        // In dual mono edit only the right instance, to catch swapped snapshots.
        if (dual) controller.setInsertEditorChannel(channel, slot, PluginEditorChannel::Right);
        controller.setInsertParameter(channel, slot, parameterId, dryValue);
        controller.setInsertParameter(channel, slot, parameterId, editedValue);
        render(dual ? 0.1f : 0.2f * editedGain, 0.2f * editedGain);
        // Capturing/cancelling an export must leave those same live events queued.
        rendering::Report cancelled;
        require(bool(controller.renderProject(spec, [](const auto& progress) {
            return progress.stage == rendering::Progress::Stage::Preparing;
        }, cancelled)) && cancelled.cancelled, "cancel render");
        play();
        require(std::abs(left[kBlock - 1] - (dual ? 0.1f : 0.2f * editedGain)) < 1e-4f &&
                std::abs(right[kBlock - 1] - 0.2f * editedGain) < 1e-4f,
                "export leaves live edits intact");
        std::cout << "PASS " << descriptor.name << " / " << scope
                  << ": preset, pending edits, cancellation, live audio\n";
    }
}
}

int main() try {
#if DAW_ENABLE_CLAP
    ClapFactory clap;
    const auto clapPlugins = clap.inspect(DAW_TEST_CLAP_PATH);
    require(!clapPlugins.empty(), "CLAP fixture");
    auto clapInstance = clap.create(clapPlugins.front());
    require(clapInstance && !clapInstance->parameters().empty(), "CLAP gain parameter");
    checkFormat(clap, clapPlugins.front(), clapInstance->parameters().front().id,
                1.0, 0.5, 0.75, 0.75f);
#endif
#if DAW_ENABLE_VST3
    Vst3Factory vst3;
    const auto vst3Plugins = vst3.inspect(DAW_TEST_VST3_PATH);
    require(!vst3Plugins.empty(), "VST3 fixture");
    checkFormat(vst3, vst3Plugins.front(), "100", 1.0, 0.5, 0.75, 0.75f);
#endif
#if DAW_ENABLE_AU
    AuFactory au;
    const auto units = au.inspect("/System/Library/Components/CoreAudio.component");
    if (units.empty()) std::cout << "SKIP AU: system AudioComponent registry is unavailable\n";
    else {
        const auto eq = std::find_if(units.begin(), units.end(),
            [](const auto& unit) { return unit.name == "AUNBandEQ"; });
        require(eq != units.end(), "system AUNBandEQ");
        checkFormat(au, *eq, std::to_string(kAUNBandEQParam_GlobalGain),
                    0.0, 20 * std::log10(0.5), 20 * std::log10(0.75), 0.75f);
    }
#endif
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n'; return 1;
}
