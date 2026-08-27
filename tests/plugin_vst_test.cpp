#include "Host/PluginInstance.hpp"
#include "ProjectSerializer.hpp"
#include "model/Document.hpp"
#include "plugins/PluginManager.hpp"
#include "plugins/ScanProcess.hpp"
#include "Vst/VstFactory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace daw;
using namespace daw::plugins;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

bool near(double a, double b, double tolerance = 1.0e-5) {
    return std::abs(a - b) <= tolerance;
}

class Listener final : public PluginListener {
public:
    void onParameterChanged(std::uint32_t, double value) noexcept override {
        ++changes;
        lastValue = value;
    }
    void onParameterGesture(std::uint32_t, bool begin) noexcept override {
        begin ? ++begins : ++ends;
    }
    void onLatencyChanged() noexcept override { ++latencyChanges; }
    void onRestartRequested() noexcept override { ++restarts; }
    void onReloadRequested() noexcept override { ++reloads; }

    int changes = 0;
    int begins = 0;
    int ends = 0;
    int latencyChanges = 0;
    int restarts = 0;
    int reloads = 0;
    double lastValue = 0.0;
};

class Events final : public EventSink {
public:
    void push(const PluginEvent& event) noexcept override { values.push_back(event); }
    std::vector<PluginEvent> values;
};

class Editor final : public PluginEditorHost {
public:
    void onEditorResized(std::uint32_t w, std::uint32_t h) noexcept override {
        width = w;
        height = h;
        ++resizes;
    }
    void onEditorClosed() noexcept override { ++closes; }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int resizes = 0;
    int closes = 0;
};

const PluginDescriptor* findUid(const std::vector<PluginDescriptor>& descriptors,
                                const char* uid) {
    const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                    [&](const PluginDescriptor& descriptor) {
                                        return descriptor.uid == uid;
                                    });
    return found == descriptors.end() ? nullptr : &*found;
}

struct StereoBlock {
    std::vector<float> input[2]{std::vector<float>(64, 1.0f),
                                std::vector<float>(64, 1.0f)};
    std::vector<float> output[2]{std::vector<float>(64, 0.0f),
                                 std::vector<float>(64, 0.0f)};
    const float* inputs[2]{input[0].data(), input[1].data()};
    float* outputs[2]{output[0].data(), output[1].data()};
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string shellPath = DAW_TEST_VST_SHELL_PATH;
    const std::string vst1Path = DAW_TEST_VST1_PATH;
    VstFactory factory;

    const std::vector<std::string> candidates = factory.enumerateCandidates(
        fs::path(shellPath).parent_path().string());
    auto containsPath = [&](const std::string& wanted) {
        std::error_code pathError;
        return std::any_of(candidates.begin(), candidates.end(),
                           [&](const std::string& candidate) {
                               return fs::equivalent(candidate, wanted, pathError);
                           });
    };
    check(containsPath(shellPath),
          "scanner enumerates the VST2 shell module");
    check(containsPath(vst1Path),
          "scanner enumerates the legacy VST1 module");

    const std::vector<PluginDescriptor> shell = factory.inspect(shellPath);
    check(shell.size() == 2, "shell scan creates one descriptor per component");
    const PluginDescriptor* effectDescriptor = findUid(shell, "54465831");
    const PluginDescriptor* instrumentDescriptor = findUid(shell, "54494E31");
    check(effectDescriptor && effectDescriptor->name == "DAW Test Legacy Effect" &&
              effectDescriptor->vendor == "VLT Tests" &&
              effectDescriptor->format == Format::Vst &&
              effectDescriptor->hasEditor && !effectDescriptor->isInstrument,
          "effect UID and VST2 metadata round-trip from the shell");
    check(instrumentDescriptor && instrumentDescriptor->isInstrument &&
              instrumentDescriptor->wantsMidi &&
              instrumentDescriptor->mainInputChannels == 0 &&
              instrumentDescriptor->mainOutputChannels == 2,
          "shell instrument metadata includes MIDI and channel layout");

    const std::vector<PluginDescriptor> legacy = factory.inspect(vst1Path);
    check(legacy.size() == 1 && legacy[0].uid == "56314658" &&
              legacy[0].name == "DawTestVst1",
          "VST1 legacy entry point uses filename metadata fallback");

    check(effectDescriptor != nullptr, "effect descriptor is available for hosting");
    if (effectDescriptor) {
        std::unique_ptr<PluginInstance> effect = factory.create(*effectDescriptor);
        check(bool(effect), "VST2 shell effect instantiates by descriptor UID");
        if (effect) {
            Listener listener;
            effect->setListener(&listener);
            PluginProcessInfo setup{48000.0, 64, false};
            check(effect->activate(setup), "legacy effect activates");
            effect->startProcessing();
            check(effect->isProcessing(), "effStartProcess enters processing");
            check(effect->latencySamples() == 17 && effect->tailSamples() == 256,
                  "initialDelay and effGetTailSize reach the host");

            StereoBlock audio;
            PluginEvent automation[2];
            automation[0].kind = PluginEvent::Kind::ParamValue;
            automation[0].paramIndex = 0;
            automation[0].frameOffset = 0;
            automation[0].value = 0.25;
            automation[1] = automation[0];
            automation[1].frameOffset = 32;
            automation[1].value = 0.75;
            PluginProcessContext context;
            context.inputs = audio.inputs;
            context.inputChannels = 2;
            context.outputs = audio.outputs;
            context.outputChannels = 2;
            context.frames = 64;
            context.inputEvents = automation;
            context.transport.tempo = 150.0;
            context.transport.ppqPosition = 4.0;
            context.transport.timeSigNumerator = 3;
            context.transport.timeSigDenominator = 4;
            context.transport.looping = true;
            context.transport.loopStartPpq = 3.0;
            context.transport.loopEndPpq = 9.0;
            context.sampleTime = 100;
            context.playing = true;
            effect->process(context);
            check(near(audio.output[0][0], 0.25) &&
                      near(audio.output[0][31], 0.25) &&
                      near(audio.output[0][32], 0.75) &&
                      near(audio.output[0][63], 0.75),
                  "mid-block automation splits processReplacing sample-accurately");
            check(near(effect->parameterValue(1), 0.5) &&
                      effect->parameterValue(2) >= 0.25 &&
                      near(effect->parameterValue(3), 1.0),
                  "tempo, PPQ, loop, time signature and playing reach VstTimeInfo");
            check(listener.changes == 1 && listener.begins == 1 &&
                      listener.ends == 1 && near(listener.lastValue, 0.25),
                  "audioMasterAutomate and edit gestures reach the listener");

            effect->setParameterFromHost(0, 0.25);
            std::vector<std::uint8_t> state;
            check(effect->saveState(state) && state.size() == 20 &&
                      std::equal(state.begin(), state.begin() + 4, "VSTL"),
                  "chunk state is wrapped in a VSTL envelope");
            effect->setParameterFromHost(0, 0.9);
            check(effect->loadState(state) && near(effect->parameterValue(0), 0.25),
                  "bank chunk restores through the validated envelope");
            std::vector<std::uint8_t> invalid = state;
            invalid[0] = 'X';
            effect->setParameterFromHost(0, 0.6);
            check(!effect->loadState(invalid) && near(effect->parameterValue(0), 0.6),
                  "invalid VSTL data is rejected before touching the plugin");

            std::uint32_t width = 0;
            std::uint32_t height = 0;
            Editor editor;
            check(effect->editorSize(width, height) && width == 320 && height == 120,
                  "effEditGetRect reports the native editor size");
            check(effect->openEditor(reinterpret_cast<void*>(1), &editor),
                  "effEditOpen accepts a native parent handle");
            effect->pumpMainThread();
            check(editor.resizes == 1 && editor.width == 320 && editor.height == 120,
                  "audioMasterSizeWindow reaches the editor host");
            effect->closeEditor();
            check(!effect->isEditorOpen(), "effEditClose completes editor lifecycle");

            effect->stopProcessing();
            check(!effect->isProcessing(), "effStopProcess leaves processing");
            effect->deactivate();
            check(!effect->isActive(), "mains-off deactivates the effect");
        }
    }

    check(instrumentDescriptor != nullptr, "instrument descriptor is available for hosting");
    if (instrumentDescriptor) {
        std::unique_ptr<PluginInstance> instrument = factory.create(*instrumentDescriptor);
        check(bool(instrument), "VSTi shell component instantiates");
        if (instrument) {
            PluginProcessInfo setup{48000.0, 64, false};
            check(instrument->activate(setup), "VSTi activates");
            instrument->startProcessing();
            StereoBlock audio;
            Events outputEvents;
            PluginEvent note;
            note.kind = PluginEvent::Kind::NoteOn;
            note.channel = 2;
            note.key = 64;
            note.value = 0.8;
            note.frameOffset = 7;
            PluginProcessContext context;
            context.outputs = audio.outputs;
            context.outputChannels = 2;
            context.frames = 64;
            context.inputEvents = std::span<const PluginEvent>(&note, 1);
            context.outputEvents = &outputEvents;
            instrument->process(context);
            check(audio.output[0][0] > 0.0f,
                  "VSTi receives a timestamped note and generates audio");
            check(outputEvents.values.size() == 1 &&
                      outputEvents.values[0].kind == PluginEvent::Kind::NoteOn &&
                      outputEvents.values[0].frameOffset == 7,
                  "VST MIDI output round-trips with deltaFrames");

            const std::array<std::uint32_t, 4> controllers{7, 128, 129, 130};
            std::vector<PluginEvent> midi;
            for (std::size_t i = 0; i < controllers.size(); ++i) {
                PluginEvent event;
                event.kind = PluginEvent::Kind::MidiController;
                event.paramIndex = controllers[i];
                event.channel = 1;
                event.value = 0.2 * double(i + 1);
                event.frameOffset = std::uint32_t(4 + i * 5);
                midi.push_back(event);
            }
            PluginEvent pressure;
            pressure.kind = PluginEvent::Kind::PolyPressure;
            pressure.channel = 1;
            pressure.key = 60;
            pressure.value = 0.7;
            pressure.frameOffset = 29;
            midi.push_back(pressure);
            outputEvents.values.clear();
            context.inputEvents = midi;
            instrument->process(context);
            check(outputEvents.values.size() == midi.size(),
                  "CC, channel pressure, pitch bend, program and poly pressure round-trip");

            instrument->setParameterFromHost(0, 0.3);
            std::vector<std::uint8_t> state;
            check(instrument->saveState(state) && state.size() == 20,
                  "non-chunk VST state stores normalized float32 parameters");
            instrument->setParameterFromHost(0, 0.8);
            check(instrument->loadState(state) && near(instrument->parameterValue(0), 0.3),
                  "parameter-array VSTL state restores exactly");
            instrument->stopProcessing();
            instrument->deactivate();
        }
    }

    if (!legacy.empty()) {
        std::unique_ptr<PluginInstance> vst1 = factory.create(legacy.front());
        check(bool(vst1), "VST1 module creates through the legacy entry point");
        if (vst1) {
            PluginProcessInfo setup{48000.0, 64, false};
            check(vst1->activate(setup), "VST1 activates");
            vst1->startProcessing();
            StereoBlock audio;
            std::fill(audio.output[0].begin(), audio.output[0].end(), 9.0f);
            std::fill(audio.output[1].begin(), audio.output[1].end(), 9.0f);
            PluginProcessContext context;
            context.inputs = audio.inputs;
            context.inputChannels = 2;
            context.outputs = audio.outputs;
            context.outputChannels = 2;
            context.frames = 64;
            vst1->process(context);
            check(near(audio.output[0][0], 0.5) && near(audio.output[1][63], 0.5),
                  "VST1 fallback clears outputs before accumulate-style process");
            vst1->stopProcessing();
            vst1->deactivate();
        }
    }

    // Persisted enum/string and the project serializer use the same `vst`
    // spelling as the cache and scanner protocol.
    const fs::path temporary = fs::temp_directory_path() / "vlt-vst-host-test";
    std::error_code ec;
    fs::remove_all(temporary, ec);
    fs::create_directories(temporary, ec);
    ProjectModel project;
    InsertModel insert;
    insert.id = "legacy-vst";
    insert.name = "Legacy Effect";
    insert.format = PluginFormat::Vst;
    insert.uid = "54465831";
    insert.path = shellPath;
    project.masterInserts.push_back(insert);
    const fs::path document = temporary / "roundtrip.vlt";
    check(ProjectSerializer::saveDocument(project, document.string(),
                                           MediaPaths::Absolute).isOk(),
          "project containing a legacy VST slot saves");
    ProjectModel reloaded;
    check(ProjectSerializer::loadDocument(reloaded, document.string(), "").isOk() &&
              reloaded.masterInserts.size() == 1 &&
              reloaded.masterInserts[0].format == PluginFormat::Vst &&
              reloaded.masterInserts[0].uid == "54465831",
          "project reload preserves VST format and UID");

    const fs::path cachePath = temporary / "plugins.json";
    {
        std::ofstream old(cachePath);
        old << R"({"version":1,"searchPathsInitialized":true,"searchPaths":{"clap":["C:/CustomClap"],"vst3":["C:/CustomVst3"],"au":[]},"entries":[]})";
    }
    PluginManager manager(cachePath.string());
    manager.load();
    check(manager.searchPaths(Format::Clap) ==
              std::vector<std::string>{"C:/CustomClap"} &&
              manager.searchPaths(Format::Vst3) ==
                  std::vector<std::string>{"C:/CustomVst3"} &&
              !manager.searchPaths(Format::Vst).empty(),
          "pre-VST cache migration adds only default VST paths");
    check(manager.save(), "migrated plugin cache saves with searchPaths.vst");

    PluginManager scanned((temporary / "scan-cache.json").string());
    scanned.setScannerPath(DAW_SCAN_PATH);
    scanned.setScanTimeout(std::chrono::milliseconds(20000));
    scanned.setSearchPaths(Format::Vst, {fs::path(shellPath).parent_path().string()});
    scanned.startScan(true);
    scanned.waitForScan();
    check(scanned.find(Format::Vst, "54465831").has_value() &&
              scanned.find(Format::Vst, "54494E31").has_value() &&
              scanned.find(Format::Vst, "56314658").has_value(),
          "scanner subprocess validates and caches VST shell and VST1 components");
    fs::remove_all(temporary, ec);

    if (failures) std::printf("FAILURES PRESENT: %d\n", failures);
    return failures ? 1 : 0;
}
