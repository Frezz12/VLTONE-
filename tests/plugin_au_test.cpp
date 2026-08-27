// Audio Unit hosting, end to end against real Audio Units.
//
// The fixture is macOS itself. `/System/Library/Components/CoreAudio.component`
// ships with every Mac and holds Apple's own units — AUNBandEQ among them — so
// these tests need nothing installed and nothing built. That matters more for
// AU than for the other two formats: an AU has to be *registered* with the
// system to be found at all, so a fixture built in the tree would have to be
// installed into a Library folder before it could be tested, which a test suite
// has no business doing.
#include "Au/AuFactory.hpp"
#include "Au/AuTiming.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Host/PluginNode.hpp"
#include "Nodes/BasicNodes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
const char* kSystemComponents = "/System/Library/Components/CoreAudio.component";

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
    AuFactory factory;

    // ── Musical time ──
    //
    // An AU asks the host where the beat is; the answer is what a synced delay
    // or an arpeggiator lines up on, and getting it wrong is a plugin that
    // loads, opens and does nothing musical.
    {
        // 120 BPM at 48 kHz: 24000 samples to the beat.
        check(au::samplesToNextBeat(0.0, 120.0, 48000.0) == 0,
              "exactly on a beat, the next beat is now");
        check(au::samplesToNextBeat(0.5, 120.0, 48000.0) == 12000,
              "half a beat in, half a beat to go");
        check(au::samplesToNextBeat(3.75, 120.0, 48000.0) == 6000,
              "and it counts from within the bar, not from its start");
        check(au::samplesToNextBeat(0.25, 60.0, 48000.0) == 36000,
              "a slower tempo makes the beat longer");
        check(au::samplesToNextBeat(1.0 - 1e-12, 120.0, 48000.0) == 0,
              "a hair short of the beat reads as the beat, not as a whole one");
        check(au::samplesToNextBeat(0.5, 0.0, 48000.0) == 0 &&
                  au::samplesToNextBeat(0.5, 120.0, 0.0) == 0,
              "a tempo or rate of zero answers zero rather than dividing by it");
    }

    // ── The identity round-trips ──
    //
    // An AU is addressed by three four-character codes, and they are written as
    // hex on purpose: the codes are arbitrary 32-bit values, plenty of real
    // ones contain bytes that are not printable, and a project file that stored
    // them as characters would mangle those plugins on save.
    {
        const std::string text = au::identityToString(0x61756678, 0x00204020, 0x7F7F7F7F);
        check(text == "61756678:00204020:7F7F7F7F", "an identity formats as three hex words");
        std::uint32_t type = 0, subtype = 0, manufacturer = 0;
        check(au::identityFromString(text, type, subtype, manufacturer), "and parses back");
        check(type == 0x61756678 && subtype == 0x00204020 && manufacturer == 0x7F7F7F7F,
              "including codes with unprintable bytes in them");
        check(!au::identityFromString("not-an-identity", type, subtype, manufacturer),
              "a malformed identity is refused rather than half-parsed");
    }

    // ── Scanning ──
    PluginDescriptor eq;
    {
        const std::vector<std::string> candidates =
            factory.enumerateCandidates("/Library/Audio/Plug-Ins/Components");
        // Nothing is asserted about how many: the machine's own plugin folder
        // is not the test's to have opinions about. That it walks without
        // throwing is the point.
        check(candidates.size() < 100000, "the scanner walks the components folder");

        const std::vector<PluginDescriptor> found = factory.inspect(kSystemComponents);
        // AudioComponent discovery is a per-user service and is unavailable in
        // some headless/sandboxed CI sessions even though the system bundle and
        // its plist are present. That environment cannot exercise an AU
        // instance; keep the full integration test on normal macOS hosts and
        // report an explicit skip here instead of failing an unrelated build.
        if (found.empty()) {
            std::printf("SKIP  system AudioComponent registry is unavailable\n");
            return failures == 0 ? 0 : 1;
        }
        check(!found.empty(), "Apple's own component bundle inspects");

        bool sawEq = false;
        bool sawOutputUnit = false;
        for (const PluginDescriptor& descriptor : found) {
            if (descriptor.name == "AUNBandEQ") {
                sawEq = true;
                eq = descriptor;
            }
            // `auou` is an output device, `afil` a file component. Neither
            // belongs in an insert slot, and the bundle advertises both.
            if (descriptor.name.find("AudioDeviceOutput") != std::string::npos) {
                sawOutputUnit = true;
            }
        }
        check(sawEq, "AUNBandEQ is found among them");
        check(!sawOutputUnit, "output and file components are filtered out, not offered");
        if (sawEq) {
            check(eq.vendor == "Apple", "the vendor is split off the AU-style name");
            check(!eq.isInstrument, "an aufx is not an instrument");
            check(eq.format == Format::AudioUnit, "the format is recorded");
        }

        check(factory.inspect("/nowhere/at/all.component").empty(),
              "inspecting something that is not a component reports nothing");
    }
    if (eq.uid.empty()) {
        std::printf("\nFAILURES PRESENT\n");
        return 1;
    }

    // ── Instantiate and run a block ──
    {
        auto instance = factory.create(eq);
        check(instance != nullptr, "the unit instantiates from its identity alone");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }

        check(!instance->parameters().empty(), "its parameters are enumerated");
        check(instance->descriptor().mainInputChannels == 2 &&
                  instance->descriptor().mainOutputChannels == 2,
              "the live instance reports its real bus widths");

        PluginProcessInfo info;
        info.sampleRate = 48000.0;
        info.maxBlockSize = kBlock;
        check(instance->activate(info), "it activates");
        instance->startProcessing();

        std::vector<float> inputLeft(kBlock, 0.5f);
        std::vector<float> inputRight(kBlock, 0.5f);
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
        // A flat EQ passes its input through. This is the assertion that the
        // *pull* model is wired up at all: AU does not hand the plugin its
        // input, the plugin asks for it through a render callback, and a host
        // that never installs one gets silence rather than an error.
        check(std::fabs(outputLeft[0] - 0.5f) < 1e-4f &&
                  std::fabs(outputRight[kBlock - 1] - 0.5f) < 1e-4f,
              "the render callback delivers the host's input to the unit");

        // ── Rendering while the transport stands still ──
        //
        // The bug this pins down: monitoring an input with the transport
        // stopped calls `process` over and over with the *same* timeline
        // position. Core Audio's timestamp is a render clock, not a musical
        // one, and a unit handed a sample time that never advances is entitled
        // to treat every block after the first as a repeat of the first —
        // which several do by returning nothing. The whole channel went silent
        // the moment an AU was added to it.
        {
            bool everySilent = true;
            for (int block = 0; block < 8; ++block) {
                std::fill(outputLeft.begin(), outputLeft.end(), -1.0f);
                std::fill(outputRight.begin(), outputRight.end(), -1.0f);
                // Deliberately not advanced: this is what a stopped transport
                // hands the plugin.
                context.sampleTime = 0;
                context.playing = false;
                instance->process(context);
                if (std::fabs(outputLeft[0] - 0.5f) < 1e-4f) everySilent = false;
            }
            check(!everySilent,
                  "a unit keeps passing audio when the transport is not rolling");
        }
        context.playing = true;
        context.sampleTime = 0;

        // ── State round trip ──
        std::vector<std::uint8_t> saved;
        check(instance->saveState(saved) && !saved.empty(), "the unit saves its state");
        check(instance->loadState(saved), "and loads it back");
        check(!instance->loadState(std::span<const std::uint8_t>()),
              "an empty state is refused rather than half-applied");
        std::vector<std::uint8_t> garbage{1, 2, 3, 4};
        check(!instance->loadState(garbage),
              "and so is something that is not a property list");

        instance->stopProcessing();
        instance->deactivate();
    }

    // ── In the graph ──
    {
        engine::PrepareInfo info;
        info.sampleRate = 48000.0;
        info.maxBlockSize = kBlock;
        info.channels = 2;

        auto instance = factory.create(eq);
        check(instance != nullptr, "an instance for the graph");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        auto node = std::make_shared<PluginNode>("au", std::move(instance));

        engine::AudioGraph graph;
        const engine::NodeId sourceId = graph.addNode(std::make_unique<engine::SourceNode>(
            "src",
            [](void*, const engine::AudioBlock& output, engine::FrameCount frames,
               engine::SamplePos) {
                for (engine::ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
                    float* data = output.data(ch);
                    for (engine::FrameCount i = 0; i < frames; ++i) data[i] = 0.25f;
                }
            },
            nullptr));
        const engine::NodeId pluginId = graph.adoptNode(node);
        graph.connect(sourceId, pluginId);
        graph.setSink(pluginId);

        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a graph containing an Audio Unit compiles");

        engine::GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        for (float& sample : output.storage) sample = 123.0f;   // stale contents
        for (int pass = 0; pass < 4; ++pass) {
            processor.process(output.block(), kBlock, pass * kBlock, true);
        }
        check(std::fabs(output.storage[0] - 0.25f) < 1e-4f,
              "audio flows through the Audio Unit inside the graph");

        // The same through the graph, with the position standing still and the
        // transport stopped — a monitored input, which is where this was first
        // reported. Four passes at position zero, the way the engine renders
        // while nothing is rolling.
        for (float& sample : output.storage) sample = 123.0f;
        for (int pass = 0; pass < 4; ++pass) {
            processor.process(output.block(), kBlock, 0, false);
        }
        check(std::fabs(output.storage[0] - 0.25f) < 1e-4f,
              "and it keeps flowing with the transport stopped at one position");

        // Bypass is the host's crossfade, not the unit's — same for all three
        // formats, so a unit with no bypass parameter still bypasses.
        node->setBypassed(true);
        for (int pass = 0; pass < 4; ++pass) {
            processor.process(output.block(), kBlock, pass * kBlock, true);
        }
        check(std::fabs(output.storage[kBlock - 1] - 0.25f) < 1e-4f,
              "and the dry signal survives a bypass");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
