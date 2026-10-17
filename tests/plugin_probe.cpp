// A hand-run diagnostic: load one real plugin and watch what it does.
//
// Not a ctest target — it needs plugins that only exist on the user's machine.
// It is here because "the plugin opens but nothing happens inside it" is a
// question no unit test can answer: what is needed is the plugin's own view of
// the host — the buses it accepted, the transport it was handed, whether it
// asked for anything back — printed block by block.
//
//   plugin_probe <vst3|vst|au|clap> <path> [uid] [--blocks N] [--editor]
//                [--params] [--set id plainValue] [--offline] [--sidechain-silent]
//                [--block-size N] [--sample-rate Hz] [--activations N]
//                [--host-cycles N] [--dense-automation]
//
// With no uid the first plugin the module advertises is used.
#include "Host/PluginInstance.hpp"
#include "Host/PluginNode.hpp"
#include "ScopedNoDenormals.hpp"
#include "PluginProbeMeasurement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace plugins = daw::plugins;

namespace {

class Listener final : public plugins::PluginListener {
public:
    void onParameterChanged(std::uint32_t index, double value) noexcept override {
        if (parameterChanges++ < 8)
            std::printf("    plugin moved parameter %u to %.4f\n", index, value);
    }
    void onParameterGesture(std::uint32_t, bool) noexcept override {}
    void onLatencyChanged() noexcept override { ++latencyChanges; }
    void onRestartRequested() noexcept override { ++restarts; }
    void onReloadRequested() noexcept override { ++reloads; }

    int parameterChanges = 0;
    int latencyChanges = 0;
    int restarts = 0;
    int reloads = 0;
};

double rms(const std::vector<float>& buffer) {
    double sum = 0.0;
    for (float sample : buffer) sum += double(sample) * double(sample);
    return buffer.empty() ? 0.0 : std::sqrt(sum / double(buffer.size()));
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: plugin_probe <vst3|vst|au|clap|internal> <path> [uid] "
                     "[--blocks N] [--editor] [--params] [--set id plainValue] "
                     "[--offline] [--sidechain-silent] [--block-size N] [--sample-rate Hz] "
                     "[--activations N] [--host-cycles N] [--dense-automation]\n");
        return 2;
    }
    const plugins::Format format = plugins::formatFromString(argv[1]);
    const std::string path = argv[2];
    std::string uid;
    int blocks = 16;
    bool wantEditor = false;
    bool listParameters = false;
    bool offline = false, sidechainConnected = false;
    std::vector<std::pair<std::string, double>> edits;
    std::string measurement;
    double sampleRate = 48000, frequency = 0, amplitudeDb = -18, secondFrequency = 0;
    unsigned blockSize = 512;
    int activations = 1;
    int hostCycles = 0;
    bool denseAutomation = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) blocks = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--editor") == 0) wantEditor = true;
        else if (std::strcmp(argv[i], "--params") == 0) listParameters = true;
        else if (std::strcmp(argv[i], "--offline") == 0) offline = true;
        else if (std::strcmp(argv[i], "--sidechain-silent") == 0) sidechainConnected = true;
        else if (std::strcmp(argv[i], "--measure") == 0 && i + 1 < argc) measurement = argv[++i];
        else if (std::strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) sampleRate = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--frequency") == 0 && i + 1 < argc) frequency = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--tone2") == 0 && i + 1 < argc) secondFrequency = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--amplitude-db") == 0 && i + 1 < argc) amplitudeDb = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) blockSize = unsigned(std::stoul(argv[++i]));
        else if (std::strcmp(argv[i], "--activations") == 0 && i + 1 < argc) activations = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--host-cycles") == 0 && i + 1 < argc) hostCycles = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--dense-automation") == 0) denseAutomation = true;
        else if (std::strcmp(argv[i], "--set") == 0 && i + 2 < argc) {
            const std::string id = argv[++i];
            edits.emplace_back(id, std::stod(argv[++i]));
        }
        else if (uid.empty()) uid = argv[i];
    }

    plugins::PluginFactory* factory = plugins::factoryFor(format);
    if (!factory) {
        std::fprintf(stderr, "no factory for '%s' in this build\n", argv[1]);
        return 2;
    }

    std::printf("── inspect ──\n");
    const std::vector<plugins::PluginDescriptor> found = factory->inspect(path);
    if (found.empty()) {
        std::fprintf(stderr, "nothing in %s\n", path.c_str());
        return 1;
    }
    for (const plugins::PluginDescriptor& d : found)
        if (uid.empty() || d.uid == uid) std::printf("  %-40s uid=%s%s\n", d.name.c_str(), d.uid.c_str(),
                    d.isInstrument ? "  [instrument]" : "");

    plugins::PluginDescriptor descriptor = found.front();
    if (!uid.empty()) {
        auto match = std::find_if(found.begin(), found.end(),
                                  [&](const plugins::PluginDescriptor& d) {
                                      return d.uid == uid;
                                  });
        if (match == found.end()) {
            std::fprintf(stderr, "no plugin with uid %s in %s\n", uid.c_str(),
                         path.c_str());
            return 1;
        }
        descriptor = *match;
    }

    std::printf("\n── create '%s' ──\n", descriptor.name.c_str());
    Listener listener; // Must outlive the plugin on every early-return path too.
    std::unique_ptr<plugins::PluginInstance> plugin = factory->create(descriptor);
    if (!plugin) {
        std::fprintf(stderr, "create failed\n");
        return 1;
    }
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000 ||
        !std::isfinite(amplitudeDb) || amplitudeDb < -180 || amplitudeDb > 24 ||
        !std::isfinite(frequency) || !std::isfinite(secondFrequency) || !blockSize || blockSize > 65536) return 2;
    if (hostCycles) {
        plugins::PluginNode node(descriptor.name, std::move(plugin));
        daw::engine::PrepareInfo prepare{sampleRate, blockSize, 2, false};
        node.prepare(prepare);
        int reconfigurations = 0;
        for (int cycle = 0; cycle < hostCycles; ++cycle) {
            const bool latency = node.takeLatencyChanged();
            const bool restart = node.takeRestartRequested();
            node.instance()->pumpMainThread();
            if (latency || restart) { ++reconfigurations; node.prepare(prepare); }
        }
        std::printf("  host service cycles=%d reconfigurations=%d ready=%d latency=%u\n",
            hostCycles, reconfigurations, int(node.isReady()), node.latencySamples());
        return node.isReady() ? 0 : 1;
    }
    plugin->setListener(&listener);

    const plugins::PluginDescriptor& real = plugin->descriptor();
    std::printf("  ports      %u in / %u out (cache said %u / %u)\n",
                real.mainInputChannels, real.mainOutputChannels,
                descriptor.mainInputChannels, descriptor.mainOutputChannels);
    const plugins::PluginBusLayout layout = plugin->busLayout();
    std::printf("  buses      in:");
    for (std::uint16_t channels : layout.inputs) std::printf(" %u", channels);
    std::printf("   out:");
    for (std::uint16_t channels : layout.outputs) std::printf(" %u", channels);
    std::printf("\n  editor     %s\n", plugin->hasEditor() ? "yes" : "no");
    std::printf("  parameters %zu\n", plugin->parameters().size());
    if (listParameters) {
        for (const auto& p : plugin->parameters())
            std::printf("    id=%s name='%s' min=%g max=%g value=%g text='%s' bypass=%d\n",
                p.id.c_str(), p.name.c_str(), p.minValue, p.maxValue,
                plugin->parameterValue(p.index),
                plugin->parameterText(p.index, plugin->parameterValue(p.index)).c_str(), p.isBypass);
    }
    std::vector<plugins::PluginEvent> parameterEvents;
    for (const auto& [id, value] : edits) {
        const auto index = plugin->parameterIndexForId(id);
        if (index < 0 || !std::isfinite(value)) return 2;
        plugins::PluginEvent event;
        event.kind = plugins::PluginEvent::Kind::ParamValue;
        event.paramIndex = unsigned(index); event.value = value;
        parameterEvents.push_back(event);
        plugin->setParameterFromHost(unsigned(index), value);
        std::printf("  set %s = %g (%s)\n", id.c_str(), value,
            plugin->parameterText(unsigned(index), value).c_str());
    }
    if (denseAutomation) {
        if (plugin->parameters().empty()) return 2;
        const auto& parameter = plugin->parameters().front();
        for (unsigned frame = 0; frame < blockSize; ++frame) {
            plugins::PluginEvent event;
            event.paramIndex = parameter.index; event.frameOffset = frame;
            event.value = parameter.minValue + (parameter.maxValue - parameter.minValue) *
                double(frame) / std::max(1u, blockSize - 1);
            parameterEvents.push_back(event);
        }
    }

    plugins::PluginProcessInfo info;
    info.sampleRate = sampleRate;
    info.maxBlockSize = blockSize;
    info.offline = offline || !measurement.empty();
    info.sidechainConnected = sidechainConnected;
    if (!plugin->activate(info)) {
        std::fprintf(stderr, "activate failed\n");
        return 1;
    }
    plugin->startProcessing();
    std::printf("  active     %s, processing %s, latency %u\n",
                plugin->isActive() ? "yes" : "no",
                plugin->isProcessing() ? "yes" : "no",
                plugin->latencySamples());

    for (int cycle = 1; cycle < activations; ++cycle) {
        const int restarts = listener.restarts, latencyChanges = listener.latencyChanges;
        plugin->stopProcessing();
        plugin->deactivate();
        if (!plugin->activate(info)) return 1;
        plugin->startProcessing();
        std::printf("  activation %d: latency=%u, latency notices=%d, restart notices=%d\n",
            cycle + 1, plugin->latencySamples(), listener.latencyChanges - latencyChanges,
            listener.restarts - restarts);
    }

    if (wantEditor) {
        std::printf("  openEditor(nullptr) → %s (no window: expect false)\n",
                    plugin->openEditor(nullptr, nullptr) ? "true" : "false");
        plugin->closeEditor();
    }

    if (!measurement.empty()) {
        const bool measured = measurePlugin(*plugin, measurement, sampleRate, blockSize, amplitudeDb, frequency, secondFrequency, parameterEvents);
        plugin->stopProcessing(); plugin->deactivate(); plugin->setListener(nullptr);
        return measured ? 0 : 1;
    }

    // ── Run blocks with a rolling transport ──
    //
    // A tempo-synced plugin (a step sequencer, a synced delay) only does
    // anything when the host tells it the transport is moving, so the probe
    // moves it exactly as the engine would: ppq advancing with sample time.
    const std::uint32_t frames = blockSize;
    std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
    std::vector<float> outLeft(frames, 0.0f), outRight(frames, 0.0f);
    const float* inputs[2] = {left.data(), right.data()};
    float* outputs[2] = {outLeft.data(), outRight.data()};

    std::printf("\n── %d blocks of %u frames, transport rolling at 120 BPM ──\n",
                blocks, frames);
    std::int64_t sampleTime = 0;
    std::vector<double> processTimes;
    processTimes.reserve(std::max(blocks, 0));
    const daw::rt::ScopedNoDenormals noDenormals;
    for (int block = 0; block < blocks; ++block) {
        // A steady tone in, so anything the plugin does to it shows up as a
        // changing output level rather than as silence either way.
        for (std::uint32_t f = 0; f < frames; ++f) {
            const double t = double(sampleTime + f) / info.sampleRate;
            const float sample = 0.5f * float(std::sin(2.0 * 3.14159265 * 220.0 * t));
            left[f] = sample;
            right[f] = sample;
        }
        std::fill(outLeft.begin(), outLeft.end(), 0.0f);
        std::fill(outRight.begin(), outRight.end(), 0.0f);

        plugins::PluginProcessContext context;
        context.inputs = inputs;
        context.inputChannels = 2;
        context.outputs = outputs;
        context.outputChannels = 2;
        context.frames = frames;
        if (block == 0 || denseAutomation) context.inputEvents = parameterEvents;
        context.sampleTime = sampleTime;
        context.steadyTime = sampleTime;
        context.playing = true;
        context.offline = offline;
        context.transport.tempo = 120.0;
        context.transport.ppqPosition =
            double(sampleTime) / info.sampleRate * (120.0 / 60.0);
        context.transport.barStartPpq =
            std::floor(context.transport.ppqPosition / 4.0) * 4.0;
        const auto processStart = std::chrono::steady_clock::now();
        const auto disposition = plugin->process(context);
        processTimes.push_back(std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - processStart).count());
        if (disposition == plugins::PluginProcessDisposition::Error) {
            std::fprintf(stderr, "process failed\n"); return 1;
        }
        plugin->pumpMainThread();

        if (block < 4 || block == blocks - 1)
            std::printf("  block %2d  ppq %6.3f  in %.6f  out L %.6f R %.6f\n", block,
                        context.transport.ppqPosition, rms(left), rms(outLeft), rms(outRight));
        sampleTime += frames;
    }

    if (!processTimes.empty()) {
        std::sort(processTimes.begin(), processTimes.end());
        const double budget = 1e6 * frames / info.sampleRate;
        std::printf("  process us: median=%.2f p99=%.2f max=%.2f budget=%.2f over-budget=%zu/%zu\n",
            processTimes[processTimes.size() / 2], processTimes[(processTimes.size() - 1) * 99 / 100],
            processTimes.back(), budget,
            std::size_t(std::count_if(processTimes.begin(), processTimes.end(), [=](double t) { return t > budget; })),
            processTimes.size());
    }

    plugin->stopProcessing();
    plugin->deactivate();
    std::printf("\n  plugin asked for: %d parameter changes, %d latency changes, "
                "%d restarts, %d reloads\n",
                listener.parameterChanges, listener.latencyChanges,
                listener.restarts, listener.reloads);
    plugin->setListener(nullptr);
    plugin.reset();
    std::printf("  plugin instance destroyed; probe completed\n");
    return 0;
}
