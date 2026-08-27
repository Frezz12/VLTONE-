// VST3 hosting, end to end against a real VST3 plugin.
//
// The fixture (tests/fixtures/test_vst3) is a genuine bundle on disk with a
// *separate* processor and controller, so these exercise the module entry
// point, the factory, the two-object initialisation and the connection between
// the halves — not a mock of our own interface calling itself.
//
// It computes out[i] = (in[i - 64] + sidechain[i - 64]) * gain + offset and
// reports 64 samples of latency, deliberately the same arithmetic as the CLAP
// fixture so both formats can be held to identical numbers.
#include "Host/PluginNode.hpp"
#include "Graph/AudioGraph.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Vst3/Vst3Factory.hpp"
#include "Vst3/Vst3Instance.hpp"
#include "Vst3/Vst3Support.hpp"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace daw;
using namespace daw::plugins;

static int failures = 0;
static void check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

namespace {

constexpr engine::FrameCount kBlock = 128;
constexpr std::uint32_t kPluginLatency = 64;

struct OutputBuffer {
    OutputBuffer(engine::ChannelCount channels, engine::FrameCount frames)
        : storage(std::size_t(channels) * frames, 0.0f), pointers(channels),
          channels(channels), frames(frames) {
        for (engine::ChannelCount ch = 0; ch < channels; ++ch) {
            pointers[ch] = storage.data() + std::size_t(ch) * frames;
        }
    }
    engine::AudioBlock block() {
        return engine::AudioBlock(pointers.data(), channels, frames);
    }
    std::vector<float> storage;
    std::vector<float*> pointers;
    engine::ChannelCount channels;
    engine::FrameCount frames;
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string pluginPath = DAW_TEST_VST3_PATH;

    Vst3Factory factory;
    PluginDescriptor descriptor;

    // ── The class id round-trips ──
    //
    // This is what a project file stores, so a lossy conversion would mean
    // every saved plugin fails to reload — silently, and only on reopen.
    {
        DECLARE_UID(raw, 0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF);
        const std::string text = vst3::uidToString(raw);
        check(text == "00112233445566778899AABBCCDDEEFF",
              "a class id formats as 32 hex characters");
        char back[16] = {};
        check(vst3::uidFromString(text, back), "and parses back");
        bool same = true;
        for (int i = 0; i < 16; ++i) {
            if (back[i] != raw[i]) same = false;
        }
        check(same, "byte for byte");
        char ignored[16] = {};
        check(!vst3::uidFromString("tooshort", ignored),
              "a malformed id is refused rather than half-parsed");
    }

    // ── Host COM objects ──
    //
    // The pointer handed back from `createInstance` must be the *interface*
    // pointer. `U::Implements` lists its own base before the interface, so the
    // most-derived pointer is a different address; handing that over puts every
    // call one vtable slot off and kills the plugin somewhere unrelated. That
    // is exactly how a stock FabFilter plugin took this host down.
    {
        vst3::HostApplication host;
        void* raw = nullptr;
        const auto result = host.createInstance(
            const_cast<char*>(static_cast<const char*>(Steinberg::Vst::IMessage::iid)),
            const_cast<char*>(static_cast<const char*>(Steinberg::Vst::IMessage::iid)),
            &raw);
        check(result == Steinberg::kResultOk && raw != nullptr,
              "the host allocates an IMessage for the plugin");
        if (raw) {
            auto* message = static_cast<Steinberg::Vst::IMessage*>(raw);
            message->setMessageID("hello");
            check(std::string(message->getMessageID()) == "hello",
                  "the message pointer is the interface pointer, not the object's");
            Steinberg::Vst::IAttributeList* attributes = message->getAttributes();
            check(attributes != nullptr, "the message carries an attribute list");
            if (attributes) {
                const std::uint8_t payload[3] = {1, 2, 3};
                attributes->setBinary("blob", payload, 3);
                const void* data = nullptr;
                Steinberg::uint32 size = 0;
                check(attributes->getBinary("blob", data, size) == Steinberg::kResultOk &&
                          size == 3 && static_cast<const std::uint8_t*>(data)[2] == 3,
                      "binary attributes round-trip");
                double number = 0.0;
                check(attributes->getFloat("blob", number) != Steinberg::kResultOk,
                      "reading an attribute as the wrong type fails rather than lying");
            }
            message->release();
        }
    }

    // ── The stream plugins read and write their state through ──
    {
        auto stream = Steinberg::owned(new vst3::MemoryStream);
        const std::uint8_t payload[4] = {9, 8, 7, 6};
        Steinberg::int32 written = 0;
        stream->write(const_cast<std::uint8_t*>(payload), 4, &written);
        check(written == 4, "a stream write reports what it took");
        Steinberg::int64 position = 0;
        stream->tell(&position);
        check(position == 4, "and leaves the cursor at the end");
        stream->seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
        std::uint8_t back[8] = {};
        Steinberg::int32 read = 0;
        stream->read(back, 8, &read);
        check(read == 4 && back[0] == 9 && back[3] == 6,
              "a read past the end returns what there was, not a failure");
        stream->seek(64, Steinberg::IBStream::kIBSeekSet, nullptr);
        read = -1;
        check(stream->read(back, 8, &read) == Steinberg::kResultOk && read == 0,
              "a read after seeking past EOF is empty and stays in bounds");
    }

    // ── Scanning ──
    {
        const std::string directory =
            pluginPath.substr(0, pluginPath.find_last_of("/\\"));
        const std::vector<std::string> candidates =
            factory.enumerateCandidates(directory);
        bool foundCandidate = false;
        std::error_code pathError;
        for (const std::string& candidate : candidates) {
            if (std::filesystem::equivalent(candidate, pluginPath, pathError))
                foundCandidate = true;
        }
        check(foundCandidate, "the scanner finds the .vst3 by walking a directory");

        const std::vector<PluginDescriptor> found = factory.inspect(pluginPath);
        // Three classes are advertised — two audio modules and the controller
        // they share — and only the audio modules are plugins. A host that
        // lists the controller offers the user a silent, editorless object.
        check(found.size() == 3,
              "the controller class is not offered as a plugin of its own");
        if (found.empty()) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        descriptor = found.front();
        check(descriptor.name == "DAW Test Gain VST3", "the class name is read");
        check(descriptor.vendor == "DAW", "the vendor is read");
        check(descriptor.version == "1.0.0", "the version is read");
        check(!descriptor.isInstrument, "an Fx sub-category is not an instrument");
        check(descriptor.format == Format::Vst3, "the format is recorded");
        check(descriptor.uid == "11112222333344445555666677778888",
              "the class id is the plugin's, in our hex form");

        check(factory.inspect("/nowhere/at/all.vst3").empty(),
              "inspecting something that is not a plugin reports nothing, and does not throw");
    }

    // ── Instantiate, and look at the two halves ──
    {
        auto instance = factory.create(descriptor);
        check(instance != nullptr, "the plugin instantiates");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }

        // The parameters come from the *controller*, which is a separate
        // object; a host that never created or connected it sees none.
        const std::span<const ParameterInfo> parameters = instance->parameters();
        check(parameters.size() == 2,
              "parameters come from the separate controller object");
        if (parameters.size() == 2) {
            check(parameters[0].name == "Gain" && parameters[0].unit == "x",
                  "the parameter's name and unit are read");
            // VST3 speaks normalised 0…1; the host's API is in plain units, and
            // getting that conversion backwards is invisible until a value is
            // saved and reloaded.
            check(std::fabs(parameters[0].minValue) < 1e-9 &&
                      std::fabs(parameters[0].maxValue - 2.0) < 1e-9,
                  "the range is converted from normalised to plain units");
            check(std::fabs(parameters[1].minValue + 1.0) < 1e-9 &&
                      std::fabs(parameters[1].maxValue - 1.0) < 1e-9,
                  "including a range that does not start at zero");
            check(std::fabs(instance->parameterValue(0) - 1.0) < 1e-9,
                  "the default reads back in plain units");
            check(instance->parameterIndexForId(parameters[1].id) == 1,
                  "a parameter can be found by its stable id");
        }

        check(instance->latencySamples() == kPluginLatency,
              "the plugin's reported latency is read");
        check(!instance->hasEditor(),
              "a plugin whose createView returns null reports no editor");

        PluginProcessInfo processInfo;
        processInfo.sampleRate = 48000.0;
        processInfo.maxBlockSize = kBlock;
        check(instance->activate(processInfo), "the plugin activates");
        instance->startProcessing();

        std::vector<float> inputLeft(kBlock, 1.0f);
        std::vector<float> inputRight(kBlock, 1.0f);
        std::vector<float> outputLeft(kBlock, -1.0f);
        std::vector<float> outputRight(kBlock, -1.0f);
        const float* inputs[2] = {inputLeft.data(), inputRight.data()};
        float* outputs[2] = {outputLeft.data(), outputRight.data()};

        PluginProcessContext context;
        context.inputs = inputs;
        context.inputChannels = 2;
        context.outputs = outputs;
        context.outputChannels = 2;
        context.frames = kBlock;
        context.playing = true;

        instance->process(context);
        bool sentinel = false;
        for (float sample : outputLeft) {
            if (sample < -900.0f) sentinel = true;
        }
        check(!sentinel,
              "a plugin declaring a sidechain is handed every bus it declared");
        check(std::fabs(outputLeft[0]) < 1e-6f,
              "the plugin's latency shows up as leading silence");
        check(std::fabs(outputLeft[kPluginLatency] - 1.0f) < 1e-6f,
              "the signal arrives after the reported latency, at unity gain, "
              "with the unconnected sidechain reading as silence");

        std::vector<float> sidechainLeft(kBlock, 0.25f);
        std::vector<float> sidechainRight(kBlock, 0.25f);
        const float* sidechain[2] = {sidechainLeft.data(), sidechainRight.data()};
        context.sidechainInputs = sidechain;
        context.sidechainInputChannels = 2;
        instance->process(context);
        check(std::fabs(outputLeft[kPluginLatency] - 1.25f) < 1e-6f,
              "a connected sidechain reaches VST3 auxiliary input bus 1");
        context.sidechainInputs = nullptr;
        context.sidechainInputChannels = 0;
        // Flush the fixture's latency ring so the following parameter-only
        // assertion is not still rendering the preceding sidechain block.
        instance->process(context);

        // ── A parameter change, in plain units, timed inside the block ──
        PluginEvent gainEvent;
        gainEvent.kind = PluginEvent::Kind::ParamValue;
        gainEvent.paramIndex = 0;
        gainEvent.value = 0.5;          // plain, not normalised
        gainEvent.frameOffset = 0;
        const PluginEvent events[1] = {gainEvent};
        context.inputEvents = events;

        instance->process(context);
        check(std::fabs(outputLeft[0] - 0.5f) < 1e-6f,
              "a parameter event is converted to normalised and applied to its block");
        context.inputEvents = {};

        // ── A plugin that declares its output silent and writes nothing ──
        //
        // The buffers the host hands over are recycled and still hold the
        // previous block. A host that reads them anyway plays that block again,
        // and again, for as long as the plugin stays quiet — a few milliseconds
        // of audio buzzing at the block rate, which starts the moment the
        // signal does (pressing Space, in the application).
        {
            PluginEvent quiet;
            quiet.kind = PluginEvent::Kind::ParamValue;
            quiet.paramIndex = 0;
            quiet.value = 0.0;
            const PluginEvent silentEvents[1] = {quiet};
            context.inputEvents = silentEvents;
            std::fill(outputLeft.begin(), outputLeft.end(), 7.0f);
            std::fill(outputRight.begin(), outputRight.end(), 7.0f);
            instance->process(context);
            const bool leftover =
                std::any_of(outputLeft.begin(), outputLeft.end(),
                            [](float sample) { return std::fabs(sample) > 1e-6f; }) ||
                std::any_of(outputRight.begin(), outputRight.end(),
                            [](float sample) { return std::fabs(sample) > 1e-6f; });
            check(!leftover,
                  "a bus the plugin flagged silent is cleared, not replayed");
            context.inputEvents = {};

            // Back to the value the block above left it on, so nothing that
            // follows can tell this check ever ran.
            PluginEvent loud = quiet;
            loud.value = 0.5;
            const PluginEvent loudEvents[1] = {loud};
            context.inputEvents = loudEvents;
            instance->process(context);
            context.inputEvents = {};
        }

        // The processor took the event, but in VST3 the controller is a
        // different object and does not see it. The host tells it separately,
        // on the control thread; without that the editor keeps showing the old
        // value and the saved state disagrees with what is heard.
        check(std::fabs(instance->parameterValue(0) - 1.0) < 1e-9,
              "the event alone leaves the controller on its old value");
        instance->setParameterFromHost(0, 0.5);
        check(std::fabs(instance->parameterValue(0) - 0.5) < 1e-9,
              "and the host's control-thread update brings it into step");

        // ── State round trip ──
        std::vector<std::uint8_t> saved;
        check(instance->saveState(saved) && !saved.empty(),
              "the plugin saves its state");

        auto fresh = factory.create(descriptor);
        check(fresh != nullptr, "a second instance is created");
        if (fresh) {
            check(std::fabs(fresh->parameterValue(0) - 1.0) < 1e-9,
                  "the fresh instance starts at the default gain");
            check(fresh->loadState(saved), "the saved state loads into it");
            // Both halves have to be restored: the processor to make it sound
            // right, the controller to make the editor show it.
            check(std::fabs(fresh->parameterValue(0) - 0.5) < 1e-9,
                  "the controller was restored along with the processor");
        }
        check(!instance->loadState(std::span<const std::uint8_t>()),
              "an empty state is refused rather than half-applied");

        instance->stopProcessing();
        instance->deactivate();
    }

    // ── Preset-wide controller changes ──
    // FabFilter-style preset browsers update the controller and send only
    // kParamValuesChanged. The host must copy that complete snapshot into the
    // processor on the next block, not merely repaint its generic controls.
    {
#if defined(_WIN32)
        ::_putenv_s("DAW_TEST_VST3_PRESET_RESTART", "1");
#else
        ::setenv("DAW_TEST_VST3_PRESET_RESTART", "1", 1);
#endif
        auto preset = factory.create(descriptor);
#if defined(_WIN32)
        ::_putenv_s("DAW_TEST_VST3_PRESET_RESTART", "");
#else
        ::unsetenv("DAW_TEST_VST3_PRESET_RESTART");
#endif
        check(preset != nullptr, "the preset-restart fixture instantiates");
        if (preset) {
            PluginProcessInfo setup{48000.0, kBlock, false};
            check(preset->activate(setup), "the preset-restart fixture activates");
            preset->startProcessing();
            std::vector<float> inputLeft(kBlock, 1.0f);
            std::vector<float> inputRight(kBlock, 1.0f);
            std::vector<float> outputLeft(kBlock, 0.0f);
            std::vector<float> outputRight(kBlock, 0.0f);
            const float* inputs[2] = {inputLeft.data(), inputRight.data()};
            float* outputs[2] = {outputLeft.data(), outputRight.data()};
            PluginProcessContext context;
            context.inputs = inputs;
            context.inputChannels = 2;
            context.outputs = outputs;
            context.outputChannels = 2;
            context.frames = kBlock;
            preset->process(context);
            check(std::fabs(outputLeft[0] - 0.5f) < 1e-6f &&
                      std::fabs(outputLeft[kPluginLatency] - 0.75f) < 1e-6f,
                  "kParamValuesChanged synchronizes the preset from controller to DSP");
            preset->stopProcessing();
            preset->deactivate();
        }
    }

    // ── A real VST3 instrument event bus ──
    {
        const std::vector<PluginDescriptor> all = factory.inspect(pluginPath);
        const PluginDescriptor* instrument = nullptr;
        for (const PluginDescriptor& candidate : all) {
            if (candidate.isInstrument) instrument = &candidate;
        }
        check(instrument != nullptr, "the VSTi class is identified as an instrument");
        if (instrument) {
            auto instance = factory.create(*instrument);
            check(instance != nullptr, "the VSTi initializes");
            PluginProcessInfo setup;
            setup.sampleRate = 48000.0;
            setup.maxBlockSize = kBlock;
            check(instance && instance->activate(setup),
                  "audio and event buses are activated before setActive");
            if (instance && instance->isActive()) {
                instance->startProcessing();
                std::vector<float> left(kBlock, 0.0f);
                std::vector<float> right(kBlock, 0.0f);
                float* outputs[2] = {left.data(), right.data()};
                PluginEvent note;
                note.kind = PluginEvent::Kind::NoteOn;
                note.key = 60;
                note.value = 0.75;
                PluginProcessContext context;
                context.outputs = outputs;
                context.outputChannels = 2;
                context.frames = kBlock;
                context.inputEvents = std::span<const PluginEvent>(&note, 1);
                instance->process(context);
                check(std::fabs(left[0] - 0.75f) < 1e-6f,
                      "a note reaches the activated VST3 event input and produces audio");

                PluginEvent modWheel;
                modWheel.kind = PluginEvent::Kind::MidiController;
                modWheel.channel = 0;
                modWheel.paramIndex = Steinberg::Vst::kCtrlModWheel;
                modWheel.value = 0.25;  // maps to gain 0.5 on the fixture
                context.inputEvents = std::span<const PluginEvent>(&modWheel, 1);
                instance->process(context);
                check(std::fabs(left[0] - 0.375f) < 1e-6f,
                      "IMidiMapping turns CC/mod-wheel data into a processor parameter");
                instance->stopProcessing();
                instance->deactivate();
            }
        }
    }

    // ── Exact input silence reaches VST3 silenceFlags ──
    {
        engine::PrepareInfo info;
        info.sampleRate = 48000.0;
        info.maxBlockSize = kBlock;
        info.channels = 2;

        float sourceValue = 0.0f;
        auto node = std::make_shared<PluginNode>("vst3-silence-flags",
                                                factory.create(descriptor));
        engine::AudioGraph graph;
        const engine::NodeId sourceId = graph.addNode(
            std::make_unique<engine::SourceNode>(
                "src",
                [](void* context, const engine::AudioBlock& output,
                   engine::FrameCount frames, engine::SamplePos) {
                    const float value = *static_cast<const float*>(context);
                    for (engine::ChannelCount ch = 0;
                         ch < output.numChannels(); ++ch) {
                        std::fill_n(output.data(ch), frames, value);
                    }
                },
                &sourceValue));
        const engine::NodeId pluginId = graph.adoptNode(node);
        graph.connect(sourceId, pluginId);
        graph.setSink(pluginId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "the VST3 silence-flag graph compiles");
        if (compiled) {
            PluginEvent diagnostic;
            diagnostic.kind = PluginEvent::Kind::ParamValue;
            diagnostic.paramIndex = 0;
            diagnostic.value = 0.0625;
            check(node->pushEvent(diagnostic),
                  "the VST3 silence-flag diagnostic queues");

            engine::GraphProcessor processor(2);
            processor.setGraph(*compiled);
            OutputBuffer output(2, kBlock);
            processor.process(output.block(), kBlock, 0, true);
            check(std::fabs(output.storage[0] - 0.25f) < 1e-6f,
                  "exact-zero main and sidechain buses set VST3 silenceFlags");

            sourceValue = 1.0f;
            processor.process(output.block(), kBlock, kBlock, true);
            check(std::fabs(output.storage[0] - 0.5f) < 1e-6f,
                  "a non-zero sample clears the VST3 main-bus silence flags");
        }
    }

    // ── In the graph, with delay compensation ──
    {
        engine::PrepareInfo info;
        info.sampleRate = 48000.0;
        info.maxBlockSize = kBlock;
        info.channels = 2;

        auto instance = factory.create(descriptor);
        check(instance != nullptr, "an instance for the graph");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        auto node = std::make_shared<PluginNode>("vst3", std::move(instance));

        engine::AudioGraph graph;
        const engine::NodeId sourceId = graph.addNode(std::make_unique<engine::SourceNode>(
            "src",
            [](void*, const engine::AudioBlock& output, engine::FrameCount frames,
               engine::SamplePos) {
                for (engine::ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
                    float* data = output.data(ch);
                    for (engine::FrameCount i = 0; i < frames; ++i) data[i] = 1.0f;
                }
            },
            nullptr));
        const engine::NodeId pluginId = graph.adoptNode(node);
        graph.connect(sourceId, pluginId);
        graph.setSink(pluginId);

        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a graph containing a VST3 plugin compiles");
        check(node->latencySamples() == kPluginLatency,
              "the node reports the plugin's latency to the graph");
        if (compiled) {
            check((*compiled)->totalLatency == kPluginLatency,
                  "and the graph totals it");
        }

        engine::GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        for (float& sample : output.storage) sample = 123.0f;   // stale contents
        processor.process(output.block(), kBlock, 0, true);

        check(std::fabs(output.storage[0]) < 1e-6f,
              "the plugin's latency is audible as leading silence in the graph");
        check(std::fabs(output.storage[kPluginLatency] - 1.0f) < 1e-6f,
              "audio flows through the VST3 plugin inside the graph");
    }

    // ── A plugin wider than the buffer arena ──
    //
    // The arena is two channels; a 5.1 plugin writes six whatever the host
    // wants. The four it has nowhere to put still have to land in memory this
    // host owns — writing them into the buffer sized for the *input* is a heap
    // overflow on the audio thread, and that is what a stock Waves surround
    // variant did to this host.
    {
        const std::vector<PluginDescriptor> all = factory.inspect(pluginPath);
        const PluginDescriptor* surround = nullptr;
        for (const PluginDescriptor& candidate : all) {
            if (candidate.name.find("5.1") != std::string::npos) surround = &candidate;
        }
        check(surround != nullptr, "the surround class is scanned");
        if (surround) {
            auto instance = factory.create(*surround);
            check(instance != nullptr, "a 5.1 plugin instantiates");
            if (instance) {
                // The scan could not know this — it never opened the plugin.
                // The live instance is where the real width comes from.
                check(instance->descriptor().mainOutputChannels == 6,
                      "the live instance reports six output channels, not the scan's guess");

                auto node = std::make_shared<PluginNode>("surround", std::move(instance));
                engine::PrepareInfo info;
                info.sampleRate = 48000.0;
                info.maxBlockSize = kBlock;
                info.channels = 2;

                engine::AudioGraph graph;
                const engine::NodeId sourceId =
                    graph.addNode(std::make_unique<engine::SourceNode>(
                        "src",
                        [](void*, const engine::AudioBlock& output,
                           engine::FrameCount frames, engine::SamplePos) {
                            for (engine::ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
                                float* data = output.data(ch);
                                for (engine::FrameCount i = 0; i < frames; ++i) data[i] = 1.0f;
                            }
                        },
                        nullptr));
                const engine::NodeId pluginId = graph.adoptNode(node);
                graph.connect(sourceId, pluginId);
                graph.setSink(pluginId);

                auto compiled = graph.compile(info);
                check(compiled.has_value(), "a 5.1 plugin compiles into a stereo graph");
                if (compiled) {
                    engine::GraphProcessor processor(4);
                    processor.setGraph(*compiled);
                    OutputBuffer output(2, kBlock);
                    // Several blocks: the overflow this guards against corrupts
                    // whatever the allocator put next, which one block might
                    // survive by luck.
                    for (int pass = 0; pass < 8; ++pass) {
                        processor.process(output.block(), kBlock, pass * kBlock, true);
                    }
                    check(std::fabs(output.storage[0] - 1.0f) < 1e-6f,
                          "and the two channels the arena has still carry the plugin's audio");
                }
            }
        }
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
