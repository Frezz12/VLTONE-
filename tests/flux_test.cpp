#include "Internal/FluxDsp.hpp"
#include "Internal/FluxInstance.hpp"
#include "Internal/InternalFactory.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

static thread_local bool audit = false;
static thread_local unsigned allocations = 0, releases = 0;
void *operator new(std::size_t n) {
  if (audit)
    ++allocations;
  if (auto *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
  if (audit && p)
    ++releases;
  std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
using namespace daw::plugins;
using namespace daw::plugins::flux;
namespace {
unsigned failures = 0;
void check(bool value, const char *label) {
  std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
  failures += !value;
}
struct Audio {
  std::vector<float> l, r;
  explicit Audio(unsigned n) : l(n), r(n) {};
};
Audio signal(unsigned n, double rate) {
  Audio a(n);
  for (unsigned i = 0; i < n; ++i) {
    double t = i / rate;
    double beat = std::fmod(t, .5);
    double transient = std::exp(-beat * 55);
    a.l[i] = float(.15 * std::sin(2 * 3.14159265359 * 220 * t) +
                   .04 * std::sin(2 * 3.14159265359 * 3711 * t) +
                   transient * (randomAt(31, i) * 2 - 1) * .18);
    a.r[i] = a.l[i] * .8f + float(.04 * std::sin(2 * 3.14159265359 * 331 * t));
  }
  return a;
}
void run(FluxInstance &plugin, const Audio &input, Audio &output,
         unsigned block, double rate = 48000, double tempo = 120,
         unsigned numerator = 4, unsigned denominator = 4,
         std::span<const PluginEvent> events = {}) {
  for (unsigned start = 0; start < input.l.size(); start += block) {
    unsigned n = std::min<unsigned>(block, unsigned(input.l.size()) - start);
    const float *in[]{input.l.data() + start, input.r.data() + start};
    float *out[]{output.l.data() + start, output.r.data() + start};
    std::array<PluginEvent, 16> local{};
    unsigned count = 0;
    for (auto e : events)
      if (e.frameOffset >= start && e.frameOffset < start + n) {
        e.frameOffset -= start;
        local[count++] = e;
      }
    PluginProcessContext c;
    c.inputs = in;
    c.outputs = out;
    c.inputChannels = c.outputChannels = 2;
    c.frames = n;
    c.playing = true;
    c.sampleTime = start;
    c.transport.tempo = tempo;
    c.transport.ppqPosition = start * tempo / (60 * rate);
    c.transport.timeSigNumerator = numerator;
    c.transport.timeSigDenominator = denominator;
    c.inputEvents = {local.data(), count};
    audit = true;
    plugin.process(c);
    audit = false;
  }
}
double difference(const Audio &a, const Audio &b, unsigned start = 0) {
  double d = 0;
  for (unsigned i = start; i < a.l.size(); ++i)
    d = std::max(d, std::max(std::abs(double(a.l[i] - b.l[i])),
                             std::abs(double(a.r[i] - b.r[i]))));
  return d;
}
bool finite(const Audio &a) {
  for (unsigned i = 0; i < a.l.size(); ++i)
    if (!std::isfinite(a.l[i]) || !std::isfinite(a.r[i]) ||
        std::abs(a.l[i]) > 32 || std::abs(a.r[i]) > 32)
      return false;
  return true;
}
void benchmark(bool profile) {
  struct Scenario {
    unsigned rate, block, copies;
    bool studio;
  };
  const Scenario cases[]{{48000, 64, 1, false},
                         {48000, 257, 1, true},
                         {96000, 128, 1, false},
                         {192000, 1024, 1, true},
                         {48000, 128, 3, false}};
  constexpr Kind kinds[]{Kind::Dynamics, Kind::Filter,    Kind::Drive,
                         Kind::Time,     Kind::Pitch,     Kind::Granular,
                         Kind::Spectral, Kind::Resonator, Kind::Chorus,
                         Kind::Delay,    Kind::Reverb,    Kind::Vocoder};
  for (const auto scenario : cases) {
    State s = initialState();
    s.studio = scenario.studio;
    for (unsigned i = 0; i < slotCount; ++i)
      setKind(s, i, kinds[i]);
    auto &modules = s.scenes[0].modules;
    modules[3].values[1] = .75;
    modules[3].values[2] = 6;
    modules[3].values[3] = .25;
    modules[4].values[0] = 7;
    modules[5].values[1] = 500;
    modules[5].values[2] = 200;
    modules[5].values[8] = .25;
    modules[7].values[3] = 8;
    modules[8].values[2] = 4;
    std::vector<std::unique_ptr<FluxInstance>> processors;
    std::size_t memory = 0;
    for (unsigned i = 0; i < scenario.copies; ++i) {
      auto p = std::make_unique<FluxInstance>();
      p->setState(s);
      p->activate({double(scenario.rate), scenario.block});
      memory += p->dspMemoryBytes();
      processors.push_back(std::move(p));
    }
    auto input = signal(scenario.rate * 2, scenario.rate);
    Audio output(input.l.size());
    std::vector<double> durations;
    durations.reserve(input.l.size() / scenario.block + 1);
    for (unsigned at = 0; at < input.l.size(); at += scenario.block) {
      const float *in[]{input.l.data() + at, input.r.data() + at};
      float *out[]{output.l.data() + at, output.r.data() + at};
      PluginProcessContext c;
      c.inputs = in;
      c.outputs = out;
      c.inputChannels = c.outputChannels = 2;
      c.frames = std::min(scenario.block, unsigned(input.l.size()) - at);
      c.playing = true;
      c.sampleTime = at;
      c.transport.tempo = 120;
      c.transport.ppqPosition = at * 2. / scenario.rate;
      const auto start = std::chrono::steady_clock::now();
      audit = true;
      for (auto &p : processors)
        p->process(c);
      audit = false;
      const double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
      if (at >= scenario.rate / 2)
        durations.push_back(seconds * 1000);
    }
    std::sort(durations.begin(), durations.end());
    double total = 0;
    for (auto d : durations)
      total += d;
    const double budget = 1000. * scenario.block / scenario.rate;
    std::printf("BENCH %u Hz / %u frames / %s / %u x 12 FX: mean %.3f ms "
                "(%.1f%%), p99 %.3f ms, max %.3f ms, DSP %.2f MiB\n",
                scenario.rate, scenario.block,
                scenario.studio ? "Studio" : "Live", scenario.copies,
                total / durations.size(),
                total / durations.size() / budget * 100,
                durations[std::min(durations.size() - 1,
                                   std::size_t(durations.size() * .99))],
                durations.back(), memory / 1048576.);
    check(finite(output), "loaded twelve-module processing remains finite");
    std::fflush(stdout);
    if (profile && scenario.rate == 192000) {
      for (unsigned i = 0; i <= slotCount; ++i) {
        auto one = initialState();
        one.studio = true;
        if (i < slotCount) {
          setKind(one, 0, kinds[i]);
          one.scenes[0].modules[0] = modules[i];
        }
        FluxInstance p;
        p.setState(one);
        p.activate({double(scenario.rate), scenario.block});
        auto start = std::chrono::steady_clock::now();
        run(p, input, output, scenario.block, scenario.rate);
        auto elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        std::printf("PROFILE %s: %.2f%% of real time\n",
                    i < slotCount ? kindName(kinds[i]) : "Empty",
                    elapsed / 2 * 100);
        std::fflush(stdout);
      }
    }
  }
}
} // namespace
int main(int argc, char **argv) {
  {
    double error = 0;
    for (int i = -100000; i <= 100000; ++i) {
      const double cycles = i / 13731.;
      error = std::max(error, std::abs(dsp::sine(cycles) -
          std::sin(2 * 3.14159265358979323846 * cycles)));
    }
    for (double cycles : {-1e-20, 0., 1., 1000.})
      error = std::max(error, std::abs(dsp::sine(cycles) -
          std::sin(2 * 3.14159265358979323846 * cycles)));
    check(error < 7.4e-8, "modulation oscillator error stays below 7.4e-8");
  }
  {
    bool historyOk = true;
    for (unsigned capacity : {4u, 7u, 257u}) {
      dsp::History history;
      history.prepare(capacity);
      for (unsigned n = 0; n < capacity * 4; ++n) {
        history.push({double(n + 1), -double(n + 1)});
        for (double delay : {0., .5, 1., double(capacity - 1)}) {
          const auto sample = history.ago(delay);
          const double expected = delay <= std::min(n, capacity - 1)
                                      ? n + 1 - delay : 0;
          historyOk &= sample[0] == expected && sample[1] == -expected;
        }
      }
      history.reset();
      historyOk &= history.ago(0) == dsp::Stereo{};
      history.push({7, -7});
      historyOk &= history.ago(0) == dsp::Stereo{7, -7} &&
                   history.ago(.5) == dsp::Stereo{};
    }
    check(historyOk, "history preserves fractional taps across wrap and reset");
  }
  const bool catalog = argc > 1 && std::string_view(argv[1]) == "--catalog";
  auto begin = std::chrono::steady_clock::now();
  auto list = builtinPlugins();
  check(std::count_if(list.begin(), list.end(),
                      [](auto &d) { return d.uid == "daw.flux"; }) == 1,
        "registered exactly once");
  std::set<std::string> names, states, folders;
  bool valid = true, roundtrip = true;
  for (unsigned i = 0; i < 100; ++i) {
    auto state = factoryState(i);
    names.insert(state.name);
    folders.insert(state.category);
    std::vector<std::uint8_t> bytes;
    bool ok = validState(state) && encodeState(state, bytes);
    if (!ok)
      std::printf("INVALID PRESET %u %s\n", i + 1, state.name.c_str());
    valid &= ok;
    auto restored = decodeState(bytes);
    std::vector<std::uint8_t> again;
    roundtrip &= restored && encodeState(*restored, again) && again == bytes;
    state.name = state.description = state.category = state.tags =
        state.presetId = "";
    state.seed = 1;
    encodeState(state, bytes);
    states.emplace(bytes.begin(), bytes.end());
  }
  check(valid, "all 100 factory recipes validate");
  check(names.size() == 100 && states.size() == 100 && folders.size() == 12,
        "100 unique names and audio states / 12 folders");
  check(roundtrip, "factory JSON round trips without changes");
  auto broken = initialState();
  setKind(broken, 0, Kind::Filter);
  broken.scenes[0].modules[0].values[0] = std::nan("");
  check(!validState(broken), "nonfinite parameter rejected");
  FluxInstance neutral;
  neutral.activate({48000, 512});
  std::vector<std::uint8_t> original, after;
  neutral.saveState(original);
  auto json = nlohmann::json::parse(original);
  json["state"]["order"][0] = 99;
  auto bad = json.dump();
  check(!neutral.loadState(
            {reinterpret_cast<const std::uint8_t *>(bad.data()), bad.size()}),
        "corrupt preset rejected atomically");
  neutral.saveState(after);
  check(after == original, "rejection preserves the entire state");
  auto input = signal(48000, 48000);
  Audio out(input.l.size());
  run(neutral, input, out, 257);
  check(difference(input, out, 500) < 1e-6, "neutral stereo path");
  neutral.setParameterFromHost(LowSplit, 4000);
  neutral.setParameterFromHost(HighSplit, 200);
  std::vector<std::uint8_t> automatedSplit;
  check(neutral.saveState(automatedSplit) &&
            decodeState(automatedSplit).has_value(),
        "crossing crossover automation saves a valid phase-matched split");
  auto parallelState = initialState();
  parallelState.routing = Routing::Parallel;
  parallelState.globals[HighSolo] = 1;
  FluxInstance parallelSolo;
  parallelSolo.setState(parallelState);
  parallelSolo.activate({48000, 257});
  run(parallelSolo, input, out, 257);
  check(difference(input, out, 500) < 1e-6,
        "an unused third-band Solo cannot mute parallel branches");
  bool effects = true, deterministic = true;
  for (unsigned k = 1; k < unsigned(Kind::Count); ++k) {
    auto s = initialState();
    setKind(s, 0, Kind(k));
    auto &v = s.scenes[0].modules[0].values;
    switch (Kind(k)) {
    case Kind::Volume:
      v[0] = .3;
      break;
    case Kind::Pitch:
      v[0] = 7;
      break;
    case Kind::Stereo:
      v[0] = .7;
      break;
    case Kind::Time:
      v[3] = .25;
      v[2] = 1;
      break;
    case Kind::Tape:
      v[5] = .25;
      v[0] = .6;
      break;
    case Kind::Repeat:
      v[1] = .25;
      v[0] = .125;
      break;
    case Kind::Granular:
      v[8] = .25;
      break;
    default:
      break;
    }
    FluxInstance p, q;
    p.setState(s);
    q.setState(s);
    p.activate({48000, 512});
    q.activate({48000, 257});
    Audio a(input.l.size()), b(input.l.size());
    run(p, input, a, 512);
    run(q, input, b, 257);
    bool changed = difference(input, a, 24000) > 1e-4 && finite(a);
    bool same = difference(a, b) < 1e-6;
    if (!changed || !same)
      std::printf("DSP %s changed=%d block-independent=%d diff=%g\n",
                  kindName(Kind(k)), changed, same, difference(a, b));
    effects &= changed;
    deterministic &= same;
  }
  check(effects, "all 22 modules perform finite non-neutral processing");
  check(deterministic, "all 22 modules independent of block partition");
  auto random = factoryState(40);
  random.scenes[0].modules[0].values[1] = .25;
  FluxInstance a, b;
  a.setState(random);
  b.setState(random);
  a.activate({48000, 64});
  b.activate({48000, 1024});
  Audio aa(input.l.size()), bb(input.l.size());
  run(a, input, aa, 64);
  run(b, input, bb, 1024);
  check(difference(aa, bb) < 1e-6,
        "probability uses musical position, not block size");
  bool rates = true;
  for (double sr : {44100., 48000., 96000., 192000.})
    for (unsigned block : {64u, 128u, 257u, 512u, 1024u}) {
      FluxInstance p;
      auto s = factoryState(94);
      p.setState(s);
      p.activate({sr, block});
      auto in = signal(unsigned(sr * .06), sr);
      Audio o(in.l.size());
      run(p, in, o, block, sr);
      rates &= finite(o);
    }
  check(rates,
        "44.1 / 48 / 96 / 192 kHz and 64 / 128 / 257 / 512 / 1024 frames");
  bool tempos = true;
  for (double bpm : {60., 90., 120., 174., 240.})
    for (auto signature : std::array<std::pair<unsigned, unsigned>, 3>{
             {{3, 4}, {4, 4}, {6, 8}}}) {
      FluxInstance p;
      p.setState(factoryState(0));
      p.activate({48000, 257});
      auto in = signal(12000, 48000);
      Audio o(12000);
      run(p, in, o, 257, 48000, bpm, signature.first, signature.second);
      tempos &= finite(o);
    }
  check(tempos, "five tempos and 3/4, 4/4, 6/8");
  bool quarter = true;
  for (auto sig :
       std::array<std::pair<unsigned, unsigned>, 3>{{{3, 4}, {4, 4}, {6, 8}}}) {
    FluxInstance p;
    p.setState(factoryState(0));
    p.activate({48000, 257});
    auto in = signal(24064, 48000);
    Audio o(24064);
    run(p, in, o, 257, 48000, 120, sig.first, sig.second);
    quarter &= p.telemetry().phases[0][0] < .01;
  }
  check(quarter,
        "quarter-note motion remains one beat in every time signature");
  dsp::Crossover crossover;
  crossover.tune(160, 3000, 48000);
  double powerIn = 0, powerOut = 0;
  for (unsigned i = 0; i < 48000; ++i) {
    double x = std::sin(i * .17);
    auto bands = crossover.split({x, x});
    double y = bands[0][0] + bands[1][0] + bands[2][0];
    if (i > 5000) {
      powerIn += x * x;
      powerOut += y * y;
    }
  }
  check(std::abs(10 * std::log10(powerOut / powerIn)) < .02,
        "three-way LR4 sum is an all-pass");
  auto scenes = initialState();
  setKind(scenes, 0, Kind::Volume);
  scenes.scenes[0].modules[0].values[0] = 1;
  ensureScene(scenes, 1).modules[0].values[0] = .2;
  scenes.globals[Quantize] = 1;
  FluxInstance sp;
  sp.setState(scenes);
  sp.activate({48000, 257});
  PluginEvent e;
  e.kind = PluginEvent::Kind::ParamValue;
  e.paramIndex = SceneSelect;
  e.value = 1;
  e.frameOffset = 100;
  Audio so(48000);
  run(sp, input, so, 257, 48000, 120, 4, 4, {&e, 1});
  check(sp.telemetry().playingScene == 1 &&
            sp.telemetry().queuedScene == sceneCount,
        "timestamped quantized scene event is consumed");
  bool allPresets = true;
  double maxPeak = 0;
  std::size_t maxMemory = 0;
  const unsigned catalogFrames = catalog ? 960000 : 16000;
  const auto catalogInput = signal(catalogFrames, 48000);
  for (unsigned i = 0; i < 100; ++i) {
    FluxInstance p;
    p.setState(factoryState(i));
    p.activate({48000, 512});
    Audio o(catalogFrames);
    run(p, catalogInput, o, 512);
    maxMemory = std::max(maxMemory, p.dspMemoryBytes());
    allPresets &= finite(o);
    for (float v : o.l)
      maxPeak = std::max(maxPeak, std::abs(double(v)));
    if (catalog && (i + 1) % 10 == 0) {
      std::printf("Catalog render: %u / 100, 20 seconds per preset\n", i + 1);
      std::fflush(stdout);
    }
  }
  check(allPresets && maxPeak <= 1.001, "all 100 factory states render safely");
  if (catalog)
    std::printf("Catalog: peak %.4f, largest DSP buffers %.2f MiB\n", maxPeak,
                maxMemory / 1048576.);
  bool pdc = true;
  for (bool studio : {false, true})
    for (auto kind : {Kind::Pitch, Kind::Spectral}) {
      auto s = initialState();
      setKind(s, 0, kind);
      s.studio = studio;
      s.globals[Protect] = 0;
      if (kind == Kind::Spectral)
        s.scenes[0].modules[0].values[1] = 0;
      FluxInstance p;
      p.setState(s);
      p.activate({48000, 257});
      Audio impulse(24000), o(24000);
      impulse.l[8000] = impulse.r[8000] = .2f;
      run(p, impulse, o, 257);
      unsigned peak =
          unsigned(std::max_element(o.l.begin(), o.l.end()) - o.l.begin());
      bool ok = std::abs(int(peak) - int(8000 + p.latencySamples())) <= 1;
      pdc &= ok;
      if (!ok)
        std::printf("LATENCY %s studio=%d reported=%u measured=%u\n",
                    kindName(kind), studio, p.latencySamples(), peak - 8000);
    }
  check(pdc, "Live and Studio impulse latency matches host PDC");
  auto frequency = [](const Audio &audio, unsigned first, unsigned last) {
    unsigned crossings = 0;
    for (unsigned i = first + 1; i < last; ++i)
      crossings += audio.l[i - 1] <= 0 && audio.l[i] > 0;
    return crossings * 48000. / (last - first);
  };
  Audio tone(144000);
  for (unsigned i = 0; i < tone.l.size(); ++i)
    tone.l[i] = tone.r[i] =
        float(.2 * std::sin(2 * 3.14159265359 * 440 * i / 48000));
  bool tuning = true;
  for (int mode = 0; mode < 5; ++mode) {
    auto s = initialState();
    auto kind = mode < 2 ? Kind::Pitch : Kind::Time;
    setKind(s, 0, kind);
    auto &v = s.scenes[0].modules[0].values;
    double expected = mode == 0   ? 220
                      : mode == 1 ? 880
                      : mode == 2 ? 220
                      : mode == 3 ? 110
                                  : 440;
    if (mode < 2)
      v[0] = mode == 0 ? -12 : 12;
    else {
      v[2] = mode == 2 ? 1 : mode == 3 ? 2 : 6;
      v[3] = 1;
      v[1] = .5;
    }
    FluxInstance p;
    p.setState(s);
    p.activate({48000, 257});
    Audio o(tone.l.size());
    run(p, tone, o, 257);
    double hz = frequency(o, 60000, 140000);
    bool ok = std::abs(hz - expected) < expected * .035;
    tuning &= ok;
    if (!ok)
      std::printf("PITCH mode=%d expected=%g measured=%g\n", mode, expected,
                  hz);
  }
  check(tuning,
        "octaves / half-time / quarter-time / pitch-preserving stretch");
  auto parallel = initialState();
  setKind(parallel, 0, Kind::Pitch);
  parallel.routing = Routing::Parallel;
  parallel.globals[Protect] = 0;
  FluxInstance parallelFx;
  parallelFx.setState(parallel);
  parallelFx.activate({48000, 257});
  Audio parallelOut(input.l.size());
  run(parallelFx, input, parallelOut, 257);
  double alignError = 0;
  for (unsigned i = 12000; i < input.l.size(); ++i)
    alignError = std::max(alignError,
                          std::abs(double(parallelOut.l[i]) -
                                   input.l[i - parallelFx.latencySamples()]));
  check(alignError < .003,
        "parallel dry branch aligns with Signalsmith pitch latency");
  auto multi = initialState();
  multi.routing = Routing::Multiband;
  FluxInstance wet, dry;
  wet.setState(multi);
  multi.globals[Mix] = 0;
  dry.setState(multi);
  wet.activate({48000, 512});
  dry.activate({48000, 512});
  Audio mw(input.l.size()), md(input.l.size());
  run(wet, input, mw, 512);
  run(dry, input, md, 512);
  check(difference(mw, md) < 1e-6, "multiband dry/wet is phase matched");
  // The host may mirror a scheduled automation value before delivering its
  // timestamped event.
  FluxInstance automated;
  automated.activate({48000, 512});
  Audio constant(512), warm(512);
  std::fill(constant.l.begin(), constant.l.end(), .25f);
  constant.r = constant.l;
  run(automated, constant, warm, 512);
  automated.setParameterFromHost(Output, -12);
  PluginEvent timed;
  timed.kind = PluginEvent::Kind::ParamValue;
  timed.paramIndex = Output;
  timed.value = -12;
  timed.frameOffset = 128;
  const float *ip[]{constant.l.data(), constant.r.data()};
  float *op[]{warm.l.data(), warm.r.data()};
  PluginProcessContext context;
  context.inputs = ip;
  context.outputs = op;
  context.inputChannels = context.outputChannels = 2;
  context.frames = 512;
  context.sampleTime = 512;
  context.playing = true;
  context.transport.tempo = 120;
  context.transport.ppqPosition = 512. / 24000;
  context.inputEvents = {&timed, 1};
  audit = true;
  automated.process(context);
  audit = false;
  check(std::all_of(warm.l.begin(), warm.l.begin() + 128,
                    [](float v) { return std::abs(v - .25f) < 1e-6; }) &&
            warm.l[511] < .2,
        "timestamped automation is not applied early");
  FluxInstance virtualScene;
  auto midiState = initialState();
  midiState.midiScenes = true;
  virtualScene.setState(midiState);
  virtualScene.activate({48000, 512});
  PluginEvent midi;
  midi.kind = PluginEvent::Kind::NoteOn;
  midi.channel = 15;
  midi.key = 51;
  midi.value = 1;
  midi.frameOffset = 0;
  run(virtualScene, constant, warm, 512, 48000, 120, 4, 4, {&midi, 1});
  auto vs = virtualScene.state();
  check(virtualScene.telemetry().playingScene == 15 && vs.scenes.size() == 16 &&
            validState(vs),
        "all 16 scenes can be triggered before opening the editor");
  auto pre = initialState();
  setKind(pre, 0, Kind::Volume);
  ensureScene(pre, 2).modules[0].values[0] = .3;
  pre.globals[SceneSelect] = 2;
  FluxInstance pending;
  pending.setState(pre);
  check(pending.state().scenes[2].modules[0].values[0] == .3 &&
            pending.state().scenes[0].modules[0].values[0] == 1,
        "saving a prepared scene before audio adoption preserves other scenes");
  auto rt = initialState();
  setKind(rt, 0, Kind::Volume);
  auto &rs = rt.scenes[0].modules[0];
  makeShape(rs.curves[0], 1);
  rs.curves[0].depth = 1;
  rs.curves[0].bars = 1;
  rs.sequence.enabled = true;
  for (auto &step : rs.sequence.steps)
    step.retrigger = true;
  FluxInstance retrigger;
  retrigger.setState(rt);
  retrigger.activate({48000, 257});
  Audio rtIn = signal(6500, 48000), rtOut(6500);
  run(retrigger, rtIn, rtOut, 257);
  check(retrigger.telemetry().phases[0][0] < .01,
        "sequence retrigger restarts the module motion envelope");
  auto frozen = initialState();
  setKind(frozen, 0, Kind::Granular);
  frozen.scenes[0].modules[0].values[7] = 1;
  frozen.scenes[0].modules[0].values[8] = .25;
  FluxInstance panic;
  panic.setState(frozen);
  panic.activate({48000, 512});
  Audio filled(12000);
  run(panic, signal(12000, 48000), filled, 512);
  panic.setParameterFromHost(Panic, 1);
  context.sampleTime = 12000;
  context.transport.ppqPosition = .5;
  context.inputEvents = {};
  context.inputs = ip;
  context.outputs = op;
  context.frames = 512;
  panic.process(context);
  check(panic.parameterValue(effectParam(0, Kind::Granular, 7)) == 0 &&
            panic.telemetry().fill[0] < .1,
        "Panic releases freeze and clears audio history");
  auto loopState = initialState();
  setKind(loopState, 0, Kind::Repeat);
  loopState.scenes[0].modules[0].values[1] = .25;
  loopState.sceneMode = 2;
  loopState.globals[Quantize] = 0;
  ensureScene(loopState, 1);
  FluxInstance looping;
  looping.setState(loopState);
  looping.activate({48000, 512});
  PluginEvent fillEvent;
  fillEvent.kind = PluginEvent::Kind::ParamValue;
  fillEvent.paramIndex = SceneSelect;
  fillEvent.value = 1;
  fillEvent.frameOffset = 0;
  bool keptHistory = true;
  for (unsigned elapsed = 0; elapsed < 96064;) {
    unsigned at = elapsed % 12000;
    context.frames = std::min({64u, 12000 - at, 96064 - elapsed});
    context.sampleTime = at;
    context.transport.ppqPosition = at / 24000.;
    context.transport.looping = true;
    context.transport.loopStartPpq = 0;
    context.transport.loopEndPpq = .5;
    context.inputEvents = elapsed == 0
                              ? std::span<const PluginEvent>(&fillEvent, 1)
                              : std::span<const PluginEvent>();
    audit = true;
    looping.process(context);
    audit = false;
    elapsed += context.frames;
    if (elapsed > 12000)
      keptHistory &= looping.telemetry().fill[0] == 1;
  }
  check(keptHistory && looping.telemetry().playingScene == 0,
        "transport loops preserve capture history and one-bar Fill returns");
  context.sampleTime = 500000;
  context.transport.ppqPosition = 500000. / 24000;
  context.inputEvents = {};
  looping.process(context);
  check(looping.telemetry().fill[0] < .02,
        "seek invalidates old capture history");
  auto oneShot = initialState();
  setKind(oneShot, 0, Kind::Volume);
  auto &one = oneShot.scenes[0].modules[0].curves[0];
  makeShape(one, 1);
  one.play = PlayMode::OneShot;
  one.depth = 1;
  one.bars = 1;
  FluxInstance shot;
  shot.setState(oneShot);
  shot.activate({48000, 512});
  context.transport.looping = false;
  context.sampleTime = 480000;
  context.transport.ppqPosition = 20;
  shot.process(context);
  check(shot.telemetry().phases[0][0] < .01,
        "Host One Shot starts correctly when loaded in the middle of a song");
  auto sideState = initialState();
  setKind(sideState, 0, Kind::Dynamics);
  sideState.scenes[0].modules[0].values[8] = 1;
  sideState.scenes[0].modules[0].values[0] = -30;
  FluxInstance side;
  side.setState(sideState);
  side.activate({48000, 512});
  daw::plugins::PluginBusLayout accepted;
  check(side.setBusLayout({{1, 1}, {1}}, accepted) && accepted.outputs[0] == 1,
        "mono processing with a mono sidechain is supported");
  context.inputs = ip;
  context.inputChannels = context.outputChannels = 1;
  context.sidechainInputs = ip;
  context.sidechainInputChannels = 1;
  context.frames = 512;
  context.sampleTime = 0;
  context.transport.ppqPosition = 0;
  for (unsigned block = 0; block < 64; ++block) {
    context.sampleTime = block * 512;
    context.transport.ppqPosition = context.sampleTime / 24000.;
    audit = true;
    side.process(context);
    audit = false;
  }
  check(warm.l.back() < .08 && std::isfinite(warm.l.back()),
        "external sidechain drives the mono compressor");
  bool randomStates = true;
  for (unsigned i = 0; i < 100; ++i) {
    auto s = factoryState(i);
    auto mix = s.globals[Mix], output = s.globals[Output];
    randomize(s, hash(i + 12), 7);
    randomStates &=
        validState(s) && s.globals[Mix] == mix && s.globals[Output] == output;
  }
  check(randomStates, "all factory chains randomize within valid limits "
                      "without changing global gain or Mix");
  std::set<Kind> generatedKinds;
  std::set<Routing> generatedRoutings;
  bool fullValid = true, seeded = true;
  for (unsigned i = 0; i < 100; ++i) {
    auto s = factoryState(i), sameSeed = s;
    randomize(s, hash(i + 991), randomAll);
    randomize(sameSeed, hash(i + 991), randomAll);
    fullValid &= validState(s) && s.scenes.size() >= 4 && !s.path.empty() &&
                 !s.scenes[0].routes.empty();
    seeded &= s == sameSeed;
    for (auto slot : s.modules)
      if (slot.kind != Kind::Empty)
        generatedKinds.insert(slot.kind);
    generatedRoutings.insert(s.routing);
  }
  check(fullValid && seeded, "full randomization is valid and reproducible for "
                             "all 100 factory starting states");
  check(generatedKinds.size() == unsigned(Kind::Count) - 1 &&
            generatedRoutings.size() == 3,
        "complete Random reaches all 22 effects and all routing modes");
  auto pinned = factoryState(94);
  pinned.modules[0].locked = true;
  pinned.parameterLocks = {effectParam(1, Kind::Repeat, 0), Mix, Output};
  auto pinnedBefore = pinned;
  randomize(pinned, 2345, randomAll);
  check(
      pinned.modules[0] == pinnedBefore.modules[0] &&
          pinned.order[0] == pinnedBefore.order[0] &&
          pinned.scenes[0].modules[0] == pinnedBefore.scenes[0].modules[0] &&
          pinned.modules[1].kind == Kind::Repeat &&
          pinned.scenes[0].modules[1].values[0] ==
              pinnedBefore.scenes[0].modules[1].values[0] &&
          pinned.globals[Mix] == pinnedBefore.globals[Mix] &&
          pinned.globals[Output] == pinnedBefore.globals[Output],
      "complete Random respects module, position, parameter and global locks");
  bool randomAudio = true;
  auto shortInput = signal(12000, 48000);
  Audio randomOutput(shortInput.l.size());
  for (unsigned i = 0; i < 8; ++i) {
    auto s = initialState();
    randomize(s, 301 + i, randomAll);
    FluxInstance processor;
    randomAudio &= processor.setState(s) && processor.activate({48000, 257});
    run(processor, shortInput, randomOutput, 257);
    randomAudio &= finite(randomOutput);
  }
  check(randomAudio, "generated multi-effect chains process finite audio "
                     "without real-time allocations");
  auto revision = neutral.telemetry().parameterRevision;
  neutral.setParameterFromHost(Mix, .123);
  check(neutral.telemetry().parameterRevision != revision,
        "parameter revision reports host changes");
  revision = neutral.telemetry().parameterRevision;
  neutral.setParameterFromHost(Mix, .123);
  check(neutral.telemetry().parameterRevision == revision,
        "unchanged parameters do not invalidate the editor");
  if (argc > 1 && (std::string_view(argv[1]) == "--benchmark" ||
                   std::string_view(argv[1]) == "--profile"))
    benchmark(std::string_view(argv[1]) == "--profile");
  check(allocations == 0 && releases == 0,
        "process contains no allocations or reclamation");
  if (allocations || releases)
    std::printf("allocations=%u releases=%u\n", allocations, releases);
  std::printf(
      "Flux validation: %u failures, %.2f seconds\n", failures,
      std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
          .count());
  return failures ? 1 : 0;
}
