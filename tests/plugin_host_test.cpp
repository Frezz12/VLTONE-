// Plugin hosting, end to end against a real CLAP plugin.
//
// The fixture (tests/fixtures/test_clap) is a genuine bundle on disk, so these
// exercise dlopen, the clap_entry symbol, the factory, activation, events and
// state — not a mock of our own interface calling itself.
//
// The plugin computes out[i] = in[i - 64] * gain + offset and reports 64
// samples of latency, which is what makes the compensation assertions real.
#include "Clap/ClapFactory.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Host/PluginNode.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Common/LockFreeQueue.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <thread>
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

/// Writes a constant into every channel, so what the plugin did to it is
/// obvious in the output.
struct ConstantSource {
    float value = 1.0f;
    static void render(void* context, const engine::AudioBlock& output,
                       engine::FrameCount frames, engine::SamplePos) {
        auto* self = static_cast<ConstantSource*>(context);
        for (engine::ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
            float* data = output.data(ch);
            for (engine::FrameCount i = 0; i < frames; ++i) data[i] = self->value;
        }
    }
};

/// Sample value encodes timeline position, so a misaligned path is visible.
struct RampSource {
    static void render(void*, const engine::AudioBlock& output,
                       engine::FrameCount frames, engine::SamplePos position) {
        for (engine::ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
            float* data = output.data(ch);
            for (engine::FrameCount i = 0; i < frames; ++i) {
                data[i] = float(position + engine::SamplePos(i));
            }
        }
    }
};

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

engine::PrepareInfo makeInfo() {
    engine::PrepareInfo info;
    info.sampleRate = 48000.0;
    info.maxBlockSize = kBlock;
    info.channels = 2;
    return info;
}

/// Models plugins that return successfully but stop writing audio as soon as
/// the host parks the transport. Real plugins should explicitly report a
/// silent output; some do not, which used to expose PluginNode's recycled
/// output buffer and repeat its last block forever.
class SilentOnStoppedInstance final : public PluginInstance {
public:
    explicit SilentOnStoppedInstance(bool acceptsLayout = true,
                                     bool wantsMidi = false,
                                     bool returnsSleep = false,
                                     bool transposeMidi = false)
        : m_acceptsLayout(acceptsLayout), m_returnsSleep(returnsSleep),
          m_transposeMidi(transposeMidi) {
        m_descriptor.format = Format::Clap;
        m_descriptor.name = "Silent on stop";
        m_descriptor.mainInputChannels = 2;
        m_descriptor.mainOutputChannels = 2;
        m_descriptor.wantsMidi = wantsMidi;
        m_parameter.id = "value";
        m_parameter.name = "Value";
    }

    const PluginDescriptor& descriptor() const noexcept override {
        return m_descriptor;
    }
    void setListener(PluginListener* listener) noexcept override {
        m_listener = listener;
    }
    void reportParameter(double value) noexcept {
        if (m_listener) m_listener->onParameterChanged(0, value);
    }
    void reportLatencyChanged() noexcept {
        if (m_listener) m_listener->onLatencyChanged();
    }
    void reportRestartRequested() noexcept {
        if (m_listener) m_listener->onRestartRequested();
    }
    void reportReloadRequested() noexcept {
        if (m_listener) m_listener->onReloadRequested();
    }

    bool setBusLayout(const PluginBusLayout& wanted,
                      PluginBusLayout& accepted) override {
        accepted = m_acceptsLayout ? wanted : busLayout();
        return m_acceptsLayout;
    }
    PluginBusLayout busLayout() const override { return {{2}, {2}}; }

    bool activate(const PluginProcessInfo&) override {
        m_active = true;
        return true;
    }
    void deactivate() override {
        m_processing = false;
        m_active = false;
    }
    bool isActive() const noexcept override { return m_active; }
    bool isProcessing() const noexcept override { return m_processing; }
    void startProcessing() override {
        startCalls.fetch_add(1, std::memory_order_relaxed);
        m_processing = m_active;
    }
    void stopProcessing() override {
        stopCalls.fetch_add(1, std::memory_order_relaxed);
        m_processing = false;
    }

    std::span<const ParameterInfo> parameters() const noexcept override {
        return std::span<const ParameterInfo>(&m_parameter, 1);
    }
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override {
        return id == m_parameter.id ? 0 : -1;
    }
    double parameterValue(std::uint32_t) const noexcept override {
        return seenParameterValue.load(std::memory_order_relaxed);
    }
    std::string parameterText(std::uint32_t, double) const override { return {}; }
    bool saveState(std::vector<std::uint8_t>& out) const override {
        out.clear();
        return true;
    }
    bool loadState(std::span<const std::uint8_t>) override { return true; }

    bool hasEditor() const noexcept override { return false; }
    bool openEditor(void*, PluginEditorHost*) override { return false; }
    void closeEditor() override {}
    bool isEditorOpen() const noexcept override { return false; }
    bool editorSize(std::uint32_t&, std::uint32_t&) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t&, std::uint32_t&) override { return false; }

    PluginProcessDisposition process(
        const PluginProcessContext& context) noexcept override {
        processCalls.fetch_add(1, std::memory_order_relaxed);
        int heldKey60 = 0;
        for (const PluginEvent& event : context.inputEvents) {
            if (event.kind == PluginEvent::Kind::ParamValue &&
                event.paramIndex == 0) {
                seenParameterValue.store(event.value, std::memory_order_relaxed);
            } else {
                midiEventsSeen.fetch_add(1, std::memory_order_relaxed);
            }
            if (event.kind == PluginEvent::Kind::NoteOn && event.key == 60) {
                ++heldKey60;
                noteOnsSeen.fetch_add(1, std::memory_order_relaxed);
            } else if ((event.kind == PluginEvent::Kind::NoteOff ||
                        event.kind == PluginEvent::Kind::NoteChoke) &&
                       event.key == 60) {
                heldKey60 = std::max(heldKey60 - 1, 0);
                noteOffsSeen.fetch_add(1, std::memory_order_relaxed);
            }
            if (m_transposeMidi && context.outputEvents &&
                (event.kind == PluginEvent::Kind::NoteOn ||
                 event.kind == PluginEvent::Kind::NoteOff ||
                 event.kind == PluginEvent::Kind::NoteChoke)) {
                PluginEvent transposed = event;
                transposed.key = std::int16_t(std::min<int>(event.key + 12, 127));
                context.outputEvents->push(transposed);
            }
        }
        heldKey60AfterBlock.store(heldKey60, std::memory_order_relaxed);
        if (!m_processing) {
            return PluginProcessDisposition::Continue;
        }
        if (!context.playing) {
            return m_returnsSleep ? PluginProcessDisposition::Sleep
                                  : PluginProcessDisposition::Continue;
        }
        for (std::uint16_t channel = 0; channel < context.outputChannels; ++channel) {
            float* output = context.outputs[channel];
            const float* input =
                context.inputChannels > 0
                    ? context.inputs[std::min<std::uint16_t>(
                          channel, context.inputChannels - 1)]
                    : nullptr;
            if (input) {
                std::copy_n(input, context.frames, output);
            } else {
                std::fill_n(output, context.frames, 0.0f);
            }
        }
        return m_returnsSleep ? PluginProcessDisposition::Sleep
                              : PluginProcessDisposition::Continue;
    }
    void reset() noexcept override {
        resetCalls.fetch_add(1, std::memory_order_relaxed);
    }
    void sleepProcessing() noexcept override { stopProcessing(); }
    bool wakeProcessing() noexcept override {
        startProcessing();
        return m_processing;
    }
    bool takeProcessWakeRequest() noexcept override {
        return processWakeRequested.exchange(false, std::memory_order_acq_rel);
    }
    void requestProcessWake() noexcept {
        processWakeRequested.store(true, std::memory_order_release);
    }
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override { return 0; }

    std::atomic<unsigned> processCalls{0};
    std::atomic<unsigned> resetCalls{0};
    std::atomic<unsigned> startCalls{0};
    std::atomic<unsigned> stopCalls{0};
    std::atomic<unsigned> midiEventsSeen{0};
    std::atomic<unsigned> noteOnsSeen{0};
    std::atomic<unsigned> noteOffsSeen{0};
    std::atomic<int> heldKey60AfterBlock{0};
    std::atomic<double> seenParameterValue{-1.0};

private:
    PluginDescriptor m_descriptor;
    ParameterInfo m_parameter;
    bool m_acceptsLayout = true;
    bool m_returnsSleep = false;
    bool m_transposeMidi = false;
    bool m_active = false;
    bool m_processing = false;
    std::atomic<bool> processWakeRequested{false};
    PluginListener* m_listener = nullptr;
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Plugin callbacks may originate from editor and audio threads at once.
    {
        constexpr std::size_t kProducers = 4;
        constexpr std::size_t kPerProducer = 2000;
        constexpr std::size_t kTotal = kProducers * kPerProducer;
        engine::LockFreeMPSCQueue<std::uint64_t, 1024> queue;
        std::array<std::thread, kProducers> producers;
        for (std::size_t producer = 0; producer < kProducers; ++producer) {
            producers[producer] = std::thread([&, producer] {
                for (std::size_t item = 0; item < kPerProducer; ++item) {
                    const std::uint64_t value = producer * kPerProducer + item;
                    while (!queue.push(value)) std::this_thread::yield();
                }
            });
        }
        std::vector<bool> seen(kTotal, false);
        std::size_t received = 0;
        std::size_t popped = 0;
        bool valid = true;
        while (popped < kTotal) {
            std::uint64_t value = 0;
            if (!queue.pop(value)) {
                std::this_thread::yield();
                continue;
            }
            ++popped;
            if (value >= seen.size() || seen[std::size_t(value)]) {
                valid = false;
                continue;
            }
            seen[std::size_t(value)] = true;
            ++received;
        }
        for (auto& producer : producers) producer.join();
        check(valid && received == kTotal,
              "the MPSC queue preserves concurrent callbacks");
    }

    // A burst of plugin notifications publishes one process-wide wake, then
    // the node's next controller drain re-arms it. This is the contract that
    // lets EngineController skip every plugin list on no-work UI ticks.
    {
        auto instance = std::make_unique<SilentOnStoppedInstance>();
        auto* reporter = instance.get();
        const std::uint64_t beforeConstruction =
            PluginMainThreadWork::generation();
        auto node = std::make_shared<PluginNode>("wake-coalescing",
                                                std::move(instance));
        check(PluginMainThreadWork::generation() != beforeConstruction,
              "a newly hosted plugin requests its first main-thread turn");

        node->beginMainThreadPump();
        const std::uint64_t beforeBurst = PluginMainThreadWork::generation();
        for (int i = 0; i < 100; ++i) reporter->reportParameter(double(i));
        check(PluginMainThreadWork::generation() == beforeBurst + 1,
              "a notification burst coalesces to one global wake generation");

        PluginEvent notification;
        int drained = 0;
        while (node->popNotification(notification)) ++drained;
        node->beginMainThreadPump();
        reporter->reportParameter(101.0);
        check(drained == 100 &&
                  PluginMainThreadWork::generation() == beforeBurst + 2,
              "draining re-arms the wake without losing queued notifications");

        node->beginMainThreadPump();
        const std::uint64_t beforeLifecycleSignals =
            PluginMainThreadWork::generation();
        reporter->reportLatencyChanged();
        reporter->reportRestartRequested();
        reporter->reportReloadRequested();
        check(PluginMainThreadWork::generation() == beforeLifecycleSignals + 1 &&
                  node->takeLatencyChanged() &&
                  node->takeRestartRequested() &&
                  node->takeReloadRequested(),
              "latency, restart and reload signals coalesce without losing flags");
    }

    const std::string pluginPath = DAW_TEST_CLAP_PATH;
    const engine::PrepareInfo info = makeInfo();

    ClapFactory factory;
    PluginDescriptor descriptor;

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
        check(foundCandidate, "the scanner finds the .clap by walking a directory");

        // The fixture holds two: the effect these tests use, and an instrument
        // the MIDI tests use. One bundle, several plugins, like every real
        // shell.
        const std::vector<PluginDescriptor> found = factory.inspect(pluginPath);
        check(found.size() == 2, "inspecting the bundle reports both of its plugins");
        if (found.empty()) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        for (const PluginDescriptor& candidate : found) {
            if (candidate.uid == "com.daw.test.gain") descriptor = candidate;
        }
        check(descriptor.uid == "com.daw.test.gain", "the plugin id is read");
        check(descriptor.name == "DAW Test Gain", "the display name is read");
        check(descriptor.vendor == "DAW", "the vendor is read");
        check(!descriptor.isInstrument, "an audio effect is not an instrument");
        check(descriptor.format == Format::Clap, "the format is recorded");
    }

    // ── Instantiate, activate, and look at the parameters ──
    {
        auto instance = factory.create(descriptor);
        check(instance != nullptr, "the plugin instantiates");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }

        // Channel counts come from the plugin's own audio-ports extension, not
        // from what the descriptor was scanned with.
        check(instance->descriptor().mainInputChannels == 2 &&
                  instance->descriptor().mainOutputChannels == 2,
              "the plugin reports a stereo in and out bus");

        const std::span<const ParameterInfo> parameters = instance->parameters();
        check(parameters.size() == 2, "both parameters are enumerated");
        if (parameters.size() == 2) {
            check(parameters[0].name == "Gain" && parameters[0].maxValue == 2.0,
                  "the gain parameter's name and range are read");
            check(parameters[1].name == "Offset" && parameters[1].minValue == -1.0,
                  "the offset parameter's range is read");
            check(instance->parameterIndexForId(parameters[1].id) == 1,
                  "a parameter can be found by its stable id");
        }

        check(instance->latencySamples() == kPluginLatency,
              "the plugin's reported latency is read");

        PluginProcessInfo processInfo;
        processInfo.sampleRate = 48000.0;
        processInfo.maxBlockSize = kBlock;
        check(instance->activate(processInfo), "the plugin activates");
        instance->startProcessing();

        // ── One block through it ──
        std::vector<float> inputLeft(kBlock, 1.0f);
        std::vector<float> inputRight(kBlock, 1.0f);
        std::vector<float> outputLeft(kBlock, -99.0f);
        std::vector<float> outputRight(kBlock, -99.0f);
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
        // The first 64 samples are the delay line's initial silence; after that
        // the constant 1.0 arrives at unity gain.
        check(std::fabs(outputLeft[0]) < 1e-6f,
              "the plugin's latency shows up as leading silence");
        check(std::fabs(outputLeft[kPluginLatency] - 1.0f) < 1e-6f,
              "the signal arrives after the reported latency, at unity gain");

        // ── A parameter change, timed inside the block ──
        PluginEvent gainEvent;
        gainEvent.kind = PluginEvent::Kind::ParamValue;
        gainEvent.paramIndex = 0;
        gainEvent.value = 0.5;
        gainEvent.frameOffset = 0;
        const PluginEvent events[1] = {gainEvent};
        context.inputEvents = events;

        instance->process(context);
        check(std::fabs(outputLeft[0] - 0.5f) < 1e-6f,
              "a parameter event applies to the block it was sent for");
        check(std::fabs(instance->parameterValue(0) - 0.5) < 1e-9,
              "the plugin reports the new parameter value back");

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
            check(std::fabs(fresh->parameterValue(0) - 0.5) < 1e-9,
                  "the loaded state restored the changed parameter");
        }

        instance->stopProcessing();

        // A plugin that is not processing writes nothing, and the buffer it was
        // handed is recycled — it still holds the last block that played. The
        // caller has to get silence, not that block back again.
        {
            std::fill(outputLeft.begin(), outputLeft.end(), 7.0f);
            std::fill(outputRight.begin(), outputRight.end(), 7.0f);
            instance->process(context);
            const bool leftover =
                std::any_of(outputLeft.begin(), outputLeft.end(),
                            [](float sample) { return std::fabs(sample) > 1e-6f; }) ||
                std::any_of(outputRight.begin(), outputRight.end(),
                            [](float sample) { return std::fabs(sample) > 1e-6f; });
            check(!leftover,
                  "a plugin that is not processing hands back silence, not the "
                  "previous block");
        }

        instance->deactivate();
    }

    // ── PluginNode in the graph ──
    {
        auto instance = factory.create(descriptor);
        auto node = std::make_shared<PluginNode>("gain", std::move(instance));

        engine::AudioGraph graph;
        ConstantSource source{1.0f};
        const engine::NodeId sourceId = graph.addNode(
            std::make_unique<engine::SourceNode>("src", &ConstantSource::render,
                                                 &source));
        const engine::NodeId pluginId = graph.adoptNode(node);
        graph.connect(sourceId, pluginId);
        graph.setSink(pluginId);

        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a graph containing a plugin compiles");
        check(node->latencySamples() == kPluginLatency,
              "the node reports the plugin's latency to the graph");
        check((*compiled)->totalLatency == kPluginLatency,
              "the graph's total latency includes the plugin");

        engine::GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        processor.process(output.block(), kBlock, 0, true);
        check(std::fabs(output.storage[kPluginLatency] - 1.0f) < 1e-6f,
              "audio flows through the plugin inside the graph");

        // Bypass hands the dry signal through, and must not move the latency —
        // changing it would force a recompile every time the button is clicked.
        node->setBypassed(true);
        processor.process(output.block(), kBlock, 0, true);
        processor.process(output.block(), kBlock, 0, true);
        check(std::fabs(output.storage[kBlock - 1] - 1.0f) < 1e-6f,
              "a bypassed plugin passes the dry signal");
        check(node->latencySamples() == kPluginLatency,
              "bypass does not change the reported latency");
        node->setBypassed(false);
    }

    // ── A plugin that does not define its stopped output ──
    //
    // This is deliberately exercised at PluginNode rather than in one format
    // adapter: the guard must cover VST3, AU and CLAP plugins equally.
    {
        auto instance = std::make_unique<SilentOnStoppedInstance>();
        SilentOnStoppedInstance* counter = instance.get();
        auto node = std::make_shared<PluginNode>("silent-on-stop",
                                                std::move(instance));
        node->prepare(info);

        OutputBuffer input(2, kBlock);
        OutputBuffer output(2, kBlock);
        std::fill(input.storage.begin(), input.storage.end(), 0.75f);
        std::array<engine::AudioBlock, 1> inputs{input.block()};
        engine::MidiBuffer midiInput;
        engine::MidiBuffer midiOutput;
        midiInput.reserve(engine::kMidiEventsPerBlock);
        midiOutput.reserve(engine::kMidiEventsPerBlock);
        midiInput.push(engine::MidiEvent::noteOn(11, 0, 64, 100));
        std::array<const engine::MidiBuffer*, 1> midiInputs{&midiInput};

        engine::ProcessContext context;
        context.output = output.block();
        context.inputs = inputs;
        context.frames = kBlock;
        context.sampleRate = info.sampleRate;
        context.playing = true;
        context.midiInputs = midiInputs;
        context.midiOutput = &midiOutput;

        node->process(context);
        check(std::fabs(output.storage[0] - 0.75f) < 1e-6f,
              "the nonconforming plugin writes normally while transport plays");
        check(counter->midiEventsSeen.load(std::memory_order_relaxed) == 0,
              "an audio-only plugin does not receive converted MIDI events");
        check(midiOutput.size() == 1 &&
                  midiOutput.events().front().frameOffset == 11,
              "the skipped conversion leaves audio-effect MIDI pass-through intact");

        // A stopped ClipPlayerNode feeds silence. The fake plugin returns
        // without touching output, leaving the previous 0.75 block in memory.
        std::fill(input.storage.begin(), input.storage.end(), 0.0f);
        context.playing = false;
        node->process(context);
        const bool repeatedPreviousBlock =
            std::any_of(output.storage.begin(), output.storage.end(),
                        [](float sample) { return std::fabs(sample) > 1e-6f; });
        check(!repeatedPreviousBlock,
              "parking transport cannot repeat a plugin's previous audio block");
    }

    // ── Releases survive a saturated plugin event block ──
    // 2047 host events fit in the inbound SPSC ring. One note-on then fills the
    // prepared 2048-event block vector; the matching off must evict lower-value
    // traffic and remain after its on at the same frame.
    {
        auto instance = std::make_unique<SilentOnStoppedInstance>(true, true);
        SilentOnStoppedInstance* observer = instance.get();
        auto node = std::make_shared<PluginNode>("event-overflow",
                                                std::move(instance));
        node->prepare(makeInfo());

        PluginEvent parameter;
        parameter.kind = PluginEvent::Kind::ParamValue;
        parameter.paramIndex = 0;
        int queued = 0;
        while (node->pushEvent(parameter)) ++queued;

        OutputBuffer input(2, kBlock);
        OutputBuffer output(2, kBlock);
        std::array<engine::AudioBlock, 1> inputs{input.block()};
        engine::MidiBuffer midiInput;
        engine::MidiBuffer midiOutput;
        midiInput.reserve(4);
        midiOutput.reserve(engine::kMidiEventsPerBlock);
        midiInput.push(engine::MidiEvent::noteOn(0, 0, 60, 100));
        midiInput.push(engine::MidiEvent::noteOff(0, 0, 60));
        std::array<const engine::MidiBuffer*, 1> midiInputs{&midiInput};

        engine::ProcessContext context;
        context.output = output.block();
        context.inputs = inputs;
        context.midiInputs = midiInputs;
        context.midiOutput = &midiOutput;
        context.frames = kBlock;
        context.sampleRate = makeInfo().sampleRate;
        context.playing = true;
        node->process(context);

        check(queued == 2047, "the plugin host-event ring reaches its fixed limit");
        check(observer->noteOnsSeen.load(std::memory_order_relaxed) == 1 &&
                  observer->noteOffsSeen.load(std::memory_order_relaxed) == 1,
              "a saturated plugin event block still delivers the matching release");
        check(observer->heldKey60AfterBlock.load(std::memory_order_relaxed) == 0,
              "same-frame overflow keeps note-on before its rescued note-off");
    }

    // ── Bypassing a MIDI transformer releases transformed voices ──
    {
        auto instance =
            std::make_unique<SilentOnStoppedInstance>(true, true, false, true);
        auto node = std::make_shared<PluginNode>("transpose", std::move(instance));
        node->prepare(makeInfo());

        OutputBuffer input(2, kBlock);
        OutputBuffer output(2, kBlock);
        std::array<engine::AudioBlock, 1> inputs{input.block()};
        engine::MidiBuffer midiInput;
        engine::MidiBuffer midiOutput;
        midiInput.reserve(4);
        midiOutput.reserve(engine::kMidiEventsPerBlock);
        std::array<const engine::MidiBuffer*, 1> midiInputs{&midiInput};

        engine::ProcessContext context;
        context.output = output.block();
        context.inputs = inputs;
        context.midiInputs = midiInputs;
        context.midiOutput = &midiOutput;
        context.frames = kBlock;
        context.sampleRate = makeInfo().sampleRate;
        context.playing = true;

        midiInput.push(engine::MidiEvent::noteOn(0, 0, 60, 100));
        node->process(context);
        check(midiOutput.size() == 1 && midiOutput.events().front().isNoteOn() &&
                  midiOutput.events().front().data1 == 72,
              "a MIDI-owning plugin emits its transformed note");

        node->setBypassed(true);
        midiInput.clear();
        midiInput.push(engine::MidiEvent::noteOff(0, 0, 60));
        midiOutput.clear();
        node->process(context);
        bool releasedTransformed = false;
        bool forwardedOriginal = false;
        for (const engine::MidiEvent& event : midiOutput.events()) {
            if (!event.isNoteOff()) continue;
            releasedTransformed |= event.data1 == 72;
            forwardedOriginal |= event.data1 == 60;
        }
        check(releasedTransformed && forwardedOriginal,
              "bypass releases transformed voices before forwarding the dry stream");
    }

    // ── Permanent bypass sleeps third-party DSP ──
    //
    // The transition block is still processed for its wet-to-dry crossfade.
    // Once it reaches fully dry, subsequent blocks must keep the host-owned
    // latency/MIDI path alive without entering the plugin again.
    {
        auto instance = std::make_unique<SilentOnStoppedInstance>(true, true);
        SilentOnStoppedInstance* counter = instance.get();
        auto node = std::make_shared<PluginNode>("counting-bypass",
                                                std::move(instance));
        node->prepare(info);

        OutputBuffer input(2, kBlock);
        OutputBuffer output(2, kBlock);
        std::fill(input.storage.begin(), input.storage.end(), 0.625f);
        std::array<engine::AudioBlock, 1> inputs{input.block()};

        engine::MidiBuffer midiInput;
        engine::MidiBuffer midiOutput;
        midiInput.reserve(engine::kMidiEventsPerBlock);
        midiOutput.reserve(engine::kMidiEventsPerBlock);
        midiInput.push(engine::MidiEvent::noteOn(7, 2, 64, 100));
        std::array<const engine::MidiBuffer*, 1> midiInputs{&midiInput};

        engine::ProcessContext context;
        context.output = output.block();
        context.inputs = inputs;
        context.midiInputs = midiInputs;
        context.frames = kBlock;
        context.sampleRate = info.sampleRate;
        context.playing = true;
        context.midiOutput = &midiOutput;

        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 1,
              "an enabled plugin processes its block");
        check(midiOutput.empty(),
              "a MIDI-aware enabled plugin owns, rather than duplicates, MIDI");

        node->setBypassed(true);
        midiOutput.clear();
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 2,
              "the bypass transition still processes one crossfade block");
        check(midiOutput.size() == 1 &&
                  midiOutput.events().front().frameOffset == 7,
              "MIDI becomes transparent as soon as bypass engages");

        midiOutput.clear();
        std::fill(output.storage.begin(), output.storage.end(), -99.0f);
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 2,
              "steady bypass skips the plugin process callback");
        check(counter->resetCalls.load(std::memory_order_relaxed) == 1,
              "steady bypass clears a frozen plugin tail exactly once");
        check(std::all_of(output.storage.begin(), output.storage.end(),
                          [](float sample) {
                              return std::fabs(sample - 0.625f) < 1e-6f;
                          }),
              "sleeping bypass still fully writes latency-aligned dry audio");
        check(midiOutput.size() == 1,
              "sleeping bypass continues forwarding MIDI");

        midiOutput.clear();
        auto curves = std::make_shared<PluginNode::AutomationCurves>();
        PluginNode::AutomationCurve curve;
        curve.parameterIndex = 0;
        curve.defaultValue = 0.2;
        curve.points.emplace_back(4.0, 0.8);
        curves->push_back(std::move(curve));
        node->setAutomation(curves);
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 2,
              "automation does not wake permanently bypassed DSP");
        node->setAutomation(nullptr);

        PluginEvent bypassedEdit;
        bypassedEdit.kind = PluginEvent::Kind::ParamValue;
        bypassedEdit.paramIndex = 0;
        bypassedEdit.value = 0.375;
        check(node->pushEvent(bypassedEdit),
              "a parameter edit queues while the plugin is bypassed");
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 3 &&
                  counter->resetCalls.load(std::memory_order_relaxed) == 1,
              "a bypassed parameter edit wakes exactly one processor block");
        check(std::fabs(counter->seenParameterValue.load(
                            std::memory_order_relaxed) - 0.375) < 1e-9,
              "the bypassed edit reaches processor state for state saving");

        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 3 &&
                  counter->resetCalls.load(std::memory_order_relaxed) == 2,
              "the plugin sleeps again after the parameter event block");

        node->setBypassed(false);
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 4,
              "releasing bypass wakes the plugin for its crossfade");
    }

    // ── Explicit format sleep wakes on every host-visible stimulus ──
    //
    // This fake returns the same explicit Sleep disposition as CLAP, but also
    // exposes transition counters so every format-independent wake condition
    // can be checked independently of the dynamically loaded fixture below.
    {
        auto instance = std::make_unique<SilentOnStoppedInstance>(true, true, true);
        SilentOnStoppedInstance* counter = instance.get();
        auto node = std::make_shared<PluginNode>("contract-sleep",
                                                std::move(instance));
        node->prepare(info);

        OutputBuffer input(2, kBlock);
        OutputBuffer output(2, kBlock);
        std::array<engine::AudioBlock, 1> inputs{input.block()};
        engine::MidiBuffer midiInput;
        engine::MidiBuffer midiOutput;
        midiInput.reserve(engine::kMidiEventsPerBlock);
        midiOutput.reserve(engine::kMidiEventsPerBlock);
        std::array<const engine::MidiBuffer*, 1> midiInputs{&midiInput};

        engine::ProcessContext context;
        context.output = output.block();
        context.inputs = inputs;
        context.midiInputs = midiInputs;
        context.frames = kBlock;
        context.sampleRate = info.sampleRate;
        context.playing = false;
        context.midiOutput = &midiOutput;

        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 1 &&
                  counter->stopCalls.load(std::memory_order_relaxed) == 1,
              "an explicit Sleep disposition stops processing after its block");

        std::fill(output.storage.begin(), output.storage.end(), -99.0f);
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 1 &&
                  std::all_of(output.storage.begin(), output.storage.end(),
                              [](float sample) { return sample == 0.0f; }),
              "steady explicit sleep skips DSP and fully defines silent output");

        midiInput.push(engine::MidiEvent::noteOn(3, 0, 60, 100));
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 2 &&
                  counter->startCalls.load(std::memory_order_relaxed) == 2,
              "MIDI wakes an explicitly sleeping processor before the callback");
        midiInput.clear();

        PluginEvent parameter;
        parameter.kind = PluginEvent::Kind::ParamValue;
        parameter.paramIndex = 0;
        parameter.value = 0.375;
        check(node->pushEvent(parameter), "a sleep-wake parameter event queues");
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 3 &&
                  std::fabs(counter->seenParameterValue.load(
                                std::memory_order_relaxed) - 0.375) < 1e-9,
              "a host parameter event wakes and reaches sleeping DSP");

        auto curves = std::make_shared<PluginNode::AutomationCurves>();
        PluginNode::AutomationCurve curve;
        curve.parameterIndex = 0;
        curve.defaultValue = 0.25;
        curves->push_back(std::move(curve));
        node->setAutomation(curves);
        node->process(context);
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) == 5 &&
                  counter->startCalls.load(std::memory_order_relaxed) == 4,
              "active automation wakes once and keeps processing every block");
        node->setAutomation(nullptr);
        node->process(context);   // one block observes no automation, then sleeps

        context.playing = true;
        context.timelinePosition = 0;
        node->process(context);
        const unsigned afterPlayWake =
            counter->processCalls.load(std::memory_order_relaxed);
        context.timelinePosition = kBlock;
        context.transport.ppqPosition = 0.125;
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) ==
                  afterPlayWake,
              "continuous transport advance does not spuriously wake sleeping DSP");

        context.timelinePosition = 2 * kBlock;
        context.transport.tempo = 130.0;
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) ==
                  afterPlayWake + 1,
              "a transport state change wakes sleeping DSP");

        std::fill(input.storage.begin(), input.storage.end(), 0.25f);
        context.timelinePosition = 3 * kBlock;
        node->process(context);
        context.timelinePosition = 4 * kBlock;
        node->process(context);
        const unsigned afterTwoSignalBlocks =
            counter->processCalls.load(std::memory_order_relaxed);
        check(afterTwoSignalBlocks == afterPlayWake + 3,
              "non-zero input wakes and prevents sleep while signal continues");

        std::fill(input.storage.begin(), input.storage.end(), 0.0f);
        context.timelinePosition = 5 * kBlock;
        node->process(context);   // explicit Sleep after the first quiet block
        context.timelinePosition = 6 * kBlock;
        node->process(context);   // skipped
        check(counter->processCalls.load(std::memory_order_relaxed) ==
                  afterTwoSignalBlocks + 1,
              "DSP sleeps again after the input becomes exactly silent");

        counter->requestProcessWake();
        context.timelinePosition = 7 * kBlock;
        node->process(context);
        check(counter->processCalls.load(std::memory_order_relaxed) ==
                  afterTwoSignalBlocks + 2,
              "a plugin-originated process request wakes sleeping DSP");
    }

    // ── Real CLAP SLEEP / CONTINUE_IF_NOT_QUIET / TAIL contract ──
    {
        auto node = std::make_shared<PluginNode>("clap-sleep",
                                                factory.create(descriptor));
        node->prepare(info);

        OutputBuffer input(2, kBlock);
        OutputBuffer output(2, kBlock);
        std::array<engine::AudioBlock, 1> inputs{input.block()};
        engine::MidiBuffer midiOutput;
        engine::ProcessContext context;
        context.output = output.block();
        context.inputs = inputs;
        context.frames = kBlock;
        context.sampleRate = info.sampleRate;
        context.playing = false;
        context.midiOutput = &midiOutput;

        auto processCount = [&]() -> std::uint64_t {
            std::vector<std::uint8_t> state;
            if (!node->instance()->saveState(state) ||
                state.size() < 3 * sizeof(double)) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            double encoded = 0.0;
            std::memcpy(&encoded, state.data() + 2 * sizeof(double),
                        sizeof(encoded));
            return std::uint64_t(encoded);
        };
        auto setGain = [&](double value) {
            PluginEvent event;
            event.kind = PluginEvent::Kind::ParamValue;
            event.paramIndex = 0;
            event.value = value;
            return node->pushEvent(event);
        };

        check(setGain(0.0), "the real CLAP sleep-mode parameter queues");
        node->process(context);
        const std::uint64_t afterExplicitSleep = processCount();
        std::fill(output.storage.begin(), output.storage.end(), -99.0f);
        node->process(context);
        check(afterExplicitSleep == 1 && processCount() == afterExplicitSleep &&
                  std::all_of(output.storage.begin(), output.storage.end(),
                              [](float sample) { return sample == 0.0f; }),
              "CLAP_PROCESS_SLEEP skips the next real plugin callback");

        std::fill(input.storage.begin(), input.storage.end(), 1.0f);
        node->process(context);
        node->process(context);
        check(processCount() == afterExplicitSleep + 2,
              "a real CLAP plugin restarts and processes every non-zero block");
        std::fill(input.storage.begin(), input.storage.end(), 0.0f);
        node->process(context);
        node->process(context);
        check(processCount() == afterExplicitSleep + 3,
              "the real CLAP processor returns to sleep after input silence");

        check(setGain(0.0625),
              "the real CLAP continue-if-not-quiet mode queues");
        node->process(context);
        const std::uint64_t afterQuietDisposition = processCount();
        node->process(context);
        check(processCount() == afterQuietDisposition,
              "CLAP_CONTINUE_IF_NOT_QUIET sleeps only after exact-zero output");

        check(setGain(0.125), "the real CLAP tail mode queues");
        node->process(context);
        check(node->tailSamples() == 256,
              "CLAP_PROCESS_TAIL refreshes the plugin's dynamic tail length");
        const std::uint64_t atTailStart = processCount();
        node->process(context);
        node->process(context);
        node->process(context);
        check(processCount() == atTailStart + 2,
              "a 256-sample CLAP tail runs exactly two 128-frame quiet callbacks");

        auto mainThreadCount = [&]() -> std::uint64_t {
            std::vector<std::uint8_t> state;
            if (!node->instance()->saveState(state) ||
                state.size() < 4 * sizeof(double)) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            double encoded = 0.0;
            std::memcpy(&encoded, state.data() + 3 * sizeof(double),
                        sizeof(encoded));
            return std::uint64_t(encoded);
        };
        PluginEvent callbackRequest;
        callbackRequest.kind = PluginEvent::Kind::ParamValue;
        callbackRequest.paramIndex = 1;
        callbackRequest.value = 0.875;  // fixture requests host callback
        const std::uint64_t beforeCallbackWake =
            PluginMainThreadWork::generation();
        check(node->pushEvent(callbackRequest),
              "the CLAP callback-request fixture parameter queues");
        node->process(context);
        check(PluginMainThreadWork::generation() != beforeCallbackWake,
              "CLAP request_callback publishes global pending work");
        node->instance()->pumpMainThread();
        check(mainThreadCount() == 1,
              "the requested CLAP on_main_thread callback runs exactly once");
    }

    // ── Automatic mono/stereo plugin layout ──
    {
        auto render = [&](std::uint16_t preferred, bool acceptsLayout = true) {
            auto node = std::make_shared<PluginNode>(
                "layout-aware",
                std::make_unique<SilentOnStoppedInstance>(acceptsLayout));
            node->setPreferredChannelCount(preferred);
            node->prepare(info);

            OutputBuffer input(2, kBlock);
            OutputBuffer output(2, kBlock);
            for (engine::FrameCount frame = 0; frame < kBlock; ++frame) {
                input.pointers[0][frame] = 1.0f;
                input.pointers[1][frame] = 0.0f;
            }
            std::array<engine::AudioBlock, 1> inputs{input.block()};
            engine::MidiBuffer midiOutput;
            engine::ProcessContext context;
            context.output = output.block();
            context.inputs = inputs;
            context.frames = kBlock;
            context.sampleRate = info.sampleRate;
            context.playing = true;
            context.midiOutput = &midiOutput;
            node->process(context);
            return output.storage;
        };

        const std::vector<float> stereo = render(2);
        check(std::fabs(stereo[0] - 1.0f) < 1e-6f &&
                  std::fabs(stereo[kBlock]) < 1e-6f,
              "a stereo track asks for and preserves the stereo plugin layout");

        const std::vector<float> mono = render(1);
        check(std::fabs(mono[0] - 0.5f) < 1e-6f &&
                  std::fabs(mono[kBlock] - 0.5f) < 1e-6f,
              "a mono track folds before its mono plugin and returns dual mono");

        const std::vector<float> stereoOnlyFallback = render(1, false);
        check(std::fabs(stereoOnlyFallback[0] - 0.5f) < 1e-6f &&
                  std::fabs(stereoOnlyFallback[kBlock] - 0.5f) < 1e-6f,
              "a stereo-only plugin on a mono track receives identical L/R");
    }

    // ── Delay compensation around a plugin ──
    //
    // Two ramps into one bus, one of them through the plugin. Without PDC the
    // sum would comb; with it both arrive aligned and the sum is exactly twice
    // one path.
    {
        auto instance = factory.create(descriptor);
        auto node = std::make_shared<PluginNode>("gain", std::move(instance));

        engine::AudioGraph graph;
        const engine::NodeId direct = graph.addNode(std::make_unique<engine::SourceNode>(
            "direct", &RampSource::render, nullptr));
        const engine::NodeId through = graph.addNode(std::make_unique<engine::SourceNode>(
            "through", &RampSource::render, nullptr));
        const engine::NodeId pluginId = graph.adoptNode(node);
        const engine::NodeId bus = graph.addNode(std::make_unique<engine::SumNode>("bus"));
        graph.connect(through, pluginId);
        graph.connect(pluginId, bus);
        graph.connect(direct, bus);
        graph.setSink(bus);

        auto compiled = graph.compile(info);
        check(compiled.has_value() && !(*compiled)->delays.empty(),
              "a compensation delay is inserted on the path around the plugin");

        engine::GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        engine::SamplePos position = 0;
        for (int block = 0; block < 8; ++block) {
            processor.process(output.block(), kBlock, position, true);
            position += kBlock;
        }
        // Both paths now carry the same ramp, delayed by the plugin's latency.
        const float expected = 2.0f * float(position - kBlock - kPluginLatency);
        check(std::fabs(output.storage[0] - expected) < 1e-2f,
              "both paths line up once the plugin's latency is compensated");
    }

    // ── Parallel and serial must agree with plugins in the graph ──
    //
    // Two independently built graphs with their own plugin instances, rendered
    // over the same positions — one on the pool, one on the audio thread. They
    // must not merely settle to the same steady state: comparing one graph's
    // output before and after switching schedulers would pass even if the two
    // disagreed on every transient, because a constant input washes that out.
    // Hence a ramp source, separate instances, and every block compared.
    {
        auto build = [&](engine::AudioGraph& graph) {
            const engine::NodeId master =
                graph.addNode(std::make_unique<engine::SumNode>("master"));
            for (int i = 0; i < 24; ++i) {
                const engine::NodeId sourceId =
                    graph.addNode(std::make_unique<engine::SourceNode>(
                        "src", &RampSource::render, nullptr));
                auto node =
                    std::make_shared<PluginNode>("gain", factory.create(descriptor));
                const engine::NodeId pluginId = graph.adoptNode(node);
                graph.connect(sourceId, pluginId);
                graph.connect(pluginId, master);
            }
            graph.setSink(master);
        };

        engine::AudioGraph parallelGraph;
        engine::AudioGraph serialGraph;
        build(parallelGraph);
        build(serialGraph);

        auto parallelCompiled = parallelGraph.compile(info);
        auto serialCompiled = serialGraph.compile(info);
        check(parallelCompiled.has_value() && serialCompiled.has_value(),
              "two 24-plugin graphs compile");

        engine::GraphProcessor parallelProcessor(4);
        engine::GraphProcessor serialProcessor(4);
        parallelProcessor.setGraph(*parallelCompiled);
        serialProcessor.setGraph(*serialCompiled);
        parallelProcessor.setParallelThreshold(1);   // force the pool

        OutputBuffer parallel(2, kBlock);
        OutputBuffer serial(2, kBlock);
        bool identical = true;
        bool sawSignal = false;
        engine::SamplePos position = 0;
        for (int block = 0; block < 8; ++block) {
            parallelProcessor.process(parallel.block(), kBlock, position, true);
            serialProcessor.processSerial(serial.block(), kBlock, position, true);
            for (std::size_t i = 0; i < parallel.storage.size(); ++i) {
                if (parallel.storage[i] != serial.storage[i]) identical = false;
                if (parallel.storage[i] != 0.0f) sawSignal = true;
            }
            position += kBlock;
        }
        check(sawSignal, "the comparison ran on actual signal, not on silence");
        check(identical,
              "parallel and serial renders are bit-identical, block for block, "
              "with plugins");
    }

    // ── A node with no plugin still fully defines its output ──
    //
    // The arena recycles buffers and hands them over holding the previous
    // owner's audio, so "did nothing" must still mean "wrote something".
    {
        auto node = std::make_shared<PluginNode>("empty", nullptr);
        engine::AudioGraph graph;
        const engine::NodeId id = graph.adoptNode(node);
        graph.setSink(id);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a node with no plugin compiles");

        engine::GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        for (float& sample : output.storage) sample = 123.0f;   // stale contents
        processor.process(output.block(), kBlock, 0, true);

        bool cleared = true;
        for (float sample : output.storage) {
            if (sample != 0.0f) cleared = false;
        }
        check(cleared, "a plugin node with nothing to render writes silence, not stale audio");
    }

    // ── Every declared bus reaches the plugin ──
    //
    // The fixture declares a main input *and* a sidechain, like most real
    // effects do, and reads both. A host that hands over only the main bus
    // makes the plugin index past the end of `audio_inputs` — which is how a
    // stock FabFilter insert took the whole application down. The plugin
    // answers a short bus array with a sentinel rather than an out-of-bounds
    // read, so the failure is a wrong number here instead of a segfault that
    // reproduces only sometimes.
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(pluginPath);
        auto instance = found.empty() ? nullptr : factory.create(found.front());
        check(instance != nullptr, "the sidechain fixture instantiates");
        if (instance) {
            check(instance->activate(PluginProcessInfo{48000.0, kBlock, false}),
                  "it activates");
            instance->startProcessing();

            std::vector<float> inputLeft(kBlock, 1.0f);
            std::vector<float> inputRight(kBlock, 1.0f);
            std::vector<float> outputLeft(kBlock, -99.0f);
            std::vector<float> outputRight(kBlock, -99.0f);
            const float* inputs[2] = {inputLeft.data(), inputRight.data()};
            float* outputs[2] = {outputLeft.data(), outputRight.data()};

            PluginProcessContext context;
            context.inputs = inputs;
            context.inputChannels = 2;
            context.outputs = outputs;
            context.outputChannels = 2;
            context.frames = kBlock;

            instance->process(context);
            bool sentinel = false;
            for (float sample : outputLeft) {
                if (sample < -900.0f) sentinel = true;
            }
            check(!sentinel,
                  "a plugin declaring a sidechain is handed every bus it declared");
            // The sidechain is silent, so it must contribute exactly nothing —
            // a bus pointed at uninitialised memory would fail this even when
            // the count is right.
            check(std::fabs(outputLeft[kPluginLatency] - 1.0f) < 1e-6f,
                  "the unconnected sidechain reads as silence, not as garbage");

            // Twice, because the buffers are reused: a plugin is allowed to
            // write into an input it was given, and last block's silence must
            // not be assumed to have survived.
            instance->process(context);
            check(std::fabs(outputLeft[kPluginLatency] - 1.0f) < 1e-6f,
                  "the silent sidechain is still silent on the next block");

            std::vector<float> sidechainLeft(kBlock, 0.25f);
            std::vector<float> sidechainRight(kBlock, 0.25f);
            const float* sidechain[2] = {sidechainLeft.data(),
                                         sidechainRight.data()};
            context.sidechainInputs = sidechain;
            context.sidechainInputChannels = 2;
            instance->process(context);
            check(std::fabs(outputLeft[kPluginLatency] - 1.25f) < 1e-6f,
                  "a connected sidechain reaches CLAP auxiliary input bus 1");

            instance->stopProcessing();
            instance->deactivate();
        }
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
