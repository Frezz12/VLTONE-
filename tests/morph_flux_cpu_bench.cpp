#include "Internal/FluxInstance.hpp"
#include "Internal/MorphInstance.hpp"
#include "ScopedNoDenormals.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace daw::plugins;

// Run in Release with no other build/benchmark running. Percentages are the
// fraction of one audio callback's deadline, not whole-machine CPU usage.
// --rate 44100 --cpu 4 pins a comparable run on Windows. --render out.f32
// writes stereo blocks for an A/B null test; --automate 1 exercises DSP edits.
int main(int argc, char **argv) {
  const char *renderPath = nullptr;
  unsigned rate = 48000;
  bool automate = false;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view option = argv[i];
    if (option == "--render")
      renderPath = argv[i + 1];
    else if (option == "--rate")
      rate = unsigned(std::strtoul(argv[i + 1], nullptr, 10));
    else if (option == "--automate")
      automate = std::string_view(argv[i + 1]) == "1";
#ifdef _WIN32
    else if (option == "--cpu") {
      const auto cpu = std::strtoul(argv[i + 1], nullptr, 10);
      if (cpu >= sizeof(DWORD_PTR) * 8 ||
          !SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu))
        return 1;
    }
#endif
    else
      return 1;
  }
  if (rate < 8000 || rate > 192000 || argc % 2 == 0)
    return 1;
  std::FILE *reference = renderPath ? std::fopen(renderPath, "wb") : nullptr;
  if (renderPath && !reference)
    return 1;
  const daw::rt::ScopedNoDenormals noDenormals;
  constexpr unsigned frames = 128;
  const unsigned blocks = (rate * 2 + frames - 1) / frames;
  std::printf("Stereo / %u Hz / %u frames / Release benchmark\n", rate, frames);
  std::array<std::array<float, frames>, 2> input{}, output{};
  const float *ins[]{input[0].data(), input[1].data()};
  float *outs[]{output[0].data(), output[1].data()};
  PluginProcessContext context;
  context.inputs = ins;
  context.outputs = outs;
  context.inputChannels = context.outputChannels = 2;
  context.frames = frames;
  context.playing = true;
  context.transport.tempo = 120;
  bool valid = true;
  auto run = [&](PluginInstance &plugin, const char *name, unsigned preset,
                 std::span<const unsigned> automated) {
    valid &= plugin.activate({double(rate), frames});
    const auto parameters = plugin.parameters();
    std::vector<double> times;
    times.reserve(blocks);
    for (unsigned b = 0; b < blocks; ++b) {
      context.sampleTime = b * frames;
      context.transport.ppqPosition = double(b * frames) * 2 / rate;
      for (unsigned f = 0; f < frames; ++f) {
        const double t = double(b * frames + f) / rate;
        input[0][f] = float(.18 * std::sin(t * 1382.300767579509) +
                            .05 * std::sin(t * 23316.90067539464));
        input[1][f] = float(input[0][f] * .8 + .03 * std::sin(t * 2079.734336676443));
      }
      std::array<PluginEvent, 2> events{};
      context.inputEvents = {};
      if (automate && b % 16 == 7 && !automated.empty()) {
        for (unsigned e = 0; e < events.size(); ++e) {
          const unsigned index = b / 16 * 2 + e;
          const auto &p = parameters[automated[index % automated.size()]];
          events[e].kind = PluginEvent::Kind::ParamValue;
          events[e].paramIndex = p.index;
          events[e].frameOffset = 17 + e * 62;
          const double value = p.minValue + (p.maxValue - p.minValue) *
              (index % 3 == 1 ? .75 : .25);
          events[e].value = p.isStepped ? std::round(value) : value;
        }
        context.inputEvents = events;
      }
      const auto start = std::chrono::steady_clock::now();
      plugin.process(context);
      const double ms = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start).count();
      if (b >= blocks / 4)
        times.push_back(ms);
      for (const auto &channel : output)
        for (float sample : channel)
          valid &= std::isfinite(sample);
      if (reference)
        valid &= std::fwrite(output.data(), sizeof(output), 1, reference) == 1;
    }
    std::sort(times.begin(), times.end());
    std::printf("%s preset=%u median=%.3f%% p99=%.3f%%\n", name, preset,
                times[times.size() / 2] * rate / frames / 10,
                times[times.size() * 99 / 100] * rate / frames / 10);
  };
  for (unsigned i = 0; i <= morph::factoryPresets().size(); ++i) {
    morph::MorphInstance plugin;
    std::vector<std::uint8_t> bytes;
    valid &= morph::encodeState(morph::factoryState(i), bytes);
    valid &= plugin.loadState(bytes);
    std::vector<unsigned> automated;
    for (unsigned p = 0; p < morph::MatrixStart; ++p)
      automated.push_back(p);
    run(plugin, "Morph", i, automated);
  }
  for (unsigned i = 0; i <= flux::factoryPresets().size(); ++i) {
    flux::FluxInstance plugin;
    const auto state = i < flux::factoryPresets().size()
                           ? flux::factoryState(i) : flux::initialState();
    valid &= plugin.setState(state);
    std::vector<unsigned> automated{flux::Input, flux::Output, flux::Mix};
    for (unsigned slot = 0; slot < flux::slotCount; ++slot)
      if (state.modules[slot].kind != flux::Kind::Empty) {
        const auto kind = state.modules[slot].kind;
        for (unsigned p = 0; p < flux::fields(kind).size(); ++p)
          automated.push_back(flux::effectParam(slot, kind, p));
        for (unsigned p = 0; p < flux::CommonCount; ++p)
          automated.push_back(flux::commonParam(slot, p));
      }
    run(plugin, "Flux", i, automated);
  }
  if (reference)
    valid &= std::fclose(reference) == 0;
  return valid ? 0 : 1;
}
