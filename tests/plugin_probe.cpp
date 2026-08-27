// A hand-run diagnostic: load one real plugin and watch what it does.
//
// Not a ctest target — it needs plugins that only exist on the user's machine.
// It is here because "the plugin opens but nothing happens inside it" is a
// question no unit test can answer: what is needed is the plugin's own view of
// the host — the buses it accepted, the transport it was handed, whether it
// asked for anything back — printed block by block.
//
//   plugin_probe <vst3|vst|au|clap> <path> [uid] [--blocks N] [--editor]
//
// With no uid the first plugin the module advertises is used.
#include "Host/PluginInstance.hpp"

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
                     "[--blocks N] [--editor]\n");
        return 2;
    }
    const plugins::Format format = plugins::formatFromString(argv[1]);
    const std::string path = argv[2];
    std::string uid;
    int blocks = 16;
    bool wantEditor = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) blocks = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--editor") == 0) wantEditor = true;
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
        std::printf("  %-40s uid=%s%s\n", d.name.c_str(), d.uid.c_str(),
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
    std::unique_ptr<plugins::PluginInstance> plugin = factory->create(descriptor);
    if (!plugin) {
        std::fprintf(stderr, "create failed\n");
        return 1;
    }
    Listener listener;
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

    plugins::PluginProcessInfo info;
    info.sampleRate = 48000.0;
    info.maxBlockSize = 512;
    if (!plugin->activate(info)) {
        std::fprintf(stderr, "activate failed\n");
        return 1;
    }
    plugin->startProcessing();
    std::printf("  active     %s, processing %s, latency %u\n",
                plugin->isActive() ? "yes" : "no",
                plugin->isProcessing() ? "yes" : "no",
                plugin->latencySamples());

    if (wantEditor) {
        std::printf("  openEditor(nullptr) → %s (no window: expect false)\n",
                    plugin->openEditor(nullptr, nullptr) ? "true" : "false");
        plugin->closeEditor();
    }

    // ── Run blocks with a rolling transport ──
    //
    // A tempo-synced plugin (a step sequencer, a synced delay) only does
    // anything when the host tells it the transport is moving, so the probe
    // moves it exactly as the engine would: ppq advancing with sample time.
    const std::uint32_t frames = 512;
    std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
    std::vector<float> outLeft(frames, 0.0f), outRight(frames, 0.0f);
    const float* inputs[2] = {left.data(), right.data()};
    float* outputs[2] = {outLeft.data(), outRight.data()};

    std::printf("\n── %d blocks of %u frames, transport rolling at 120 BPM ──\n",
                blocks, frames);
    std::int64_t sampleTime = 0;
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
        context.sampleTime = sampleTime;
        context.playing = true;
        context.transport.tempo = 120.0;
        context.transport.ppqPosition =
            double(sampleTime) / info.sampleRate * (120.0 / 60.0);
        context.transport.barStartPpq =
            std::floor(context.transport.ppqPosition / 4.0) * 4.0;
        plugin->process(context);
        plugin->pumpMainThread();

        std::printf("  block %2d  ppq %6.3f  in %.4f  out %.4f\n", block,
                    context.transport.ppqPosition, rms(left), rms(outLeft));
        sampleTime += frames;
    }

    plugin->stopProcessing();
    plugin->deactivate();
    std::printf("\n  plugin asked for: %d parameter changes, %d latency changes, "
                "%d restarts, %d reloads\n",
                listener.parameterChanges, listener.latencyChanges,
                listener.restarts, listener.reloads);
    return 0;
}
