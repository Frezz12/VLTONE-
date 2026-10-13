#include "Internal/InternalFactory.hpp"
#include "Internal/MorphInstance.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

static thread_local bool countAllocations = false;
static thread_local unsigned allocations = 0;
static thread_local unsigned releases = 0;
void *operator new(std::size_t n) {
  if (countAllocations)
    ++allocations;
  if (auto *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
  if (countAllocations && p)
    ++releases;
  std::free(p);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
using namespace daw::plugins;
using namespace daw::plugins::morph;
namespace {
int failures = 0;
void check(bool ok, const char *label) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
  failures += !ok;
}
struct Audio {
  std::vector<float> l, r;
  explicit Audio(unsigned n) : l(n), r(n) {}
};
Audio signal(unsigned n, double sr) {
  Audio a(n);
  for (unsigned i = 0; i < n; ++i) {
    a.l[i] = float(.2 * std::sin(2 * 3.141592653589793 * 231 * i / sr) +
                   .03 * std::sin(2 * 3.141592653589793 * 4311 * i / sr));
    a.r[i] = a.l[i] * .8f;
  }
  return a;
}
void run(MorphInstance &p, const Audio &in, Audio &out, unsigned block,
         double sr = 48000, std::span<const PluginEvent> events = {},
         unsigned channels = 2) {
  for (unsigned at = 0; at < in.l.size(); at += block) {
    const unsigned n = std::min<unsigned>(block, unsigned(in.l.size()) - at);
    const float *inputs[]{in.l.data() + at, in.r.data() + at};
    float *outputs[]{out.l.data() + at, out.r.data() + at};
    std::array<PluginEvent, 8> local{};
    unsigned num = 0;
    for (auto e : events)
      if (e.frameOffset >= at && e.frameOffset < at + n) {
        e.frameOffset -= at;
        local[num++] = e;
      }
    PluginProcessContext c;
    c.inputs = inputs;
    c.outputs = outputs;
    c.inputChannels = c.outputChannels = channels;
    c.frames = n;
    c.sampleTime = at;
    c.playing = true;
    c.transport.tempo = 120;
    c.transport.ppqPosition = at / (sr * .5);
    c.inputEvents = {local.data(), num};
    countAllocations = true;
    p.process(c);
    countAllocations = false;
  }
}
double diff(const Audio &a, const Audio &b) {
  double d = 0;
  for (unsigned i = 0; i < a.l.size(); ++i)
    d = std::max({d, std::abs(double(a.l[i] - b.l[i])),
                  std::abs(double(a.r[i] - b.r[i]))});
  return d;
}
bool bounded(const Audio &a) {
  for (unsigned i = 0; i < a.l.size(); ++i)
    if (!std::isfinite(a.l[i]) || !std::isfinite(a.r[i]) ||
        std::abs(a.l[i]) > 16 || std::abs(a.r[i]) > 16)
      return false;
  return true;
}
void load(MorphInstance &p, const State &s) {
  std::vector<std::uint8_t> b;
  encodeState(s, b);
  if (!p.loadState(b))
    std::abort();
}
bool gainCheck() {
  bool matched = true;
  for (double sr : {48000., 192000.}) {
    Audio vowel(unsigned(sr * 3));
    double phase = 0;
    for (unsigned f = 0; f < vowel.l.size(); ++f) {
      phase += 2 * 3.141592653589793 * (143 + 3 * std::sin(f / sr * 2.3)) / sr;
      double sample = 0;
      for (unsigned harmonic = 1; harmonic <= 18; ++harmonic)
        sample += .09 * std::sin(phase * harmonic) / harmonic;
      vowel.l[f] = vowel.r[f] = float(sample);
    }
    const auto differenceDb = [&](const Audio &out, double begin, double end) {
      double dry = 0, wet = 0;
      for (unsigned f = unsigned(begin * sr); f < unsigned(end * sr); ++f) {
        dry +=
            double(vowel.l[f]) * vowel.l[f] + double(vowel.r[f]) * vowel.r[f];
        wet += double(out.l[f]) * out.l[f] + double(out.r[f]) * out.r[f];
      }
      return 10 * std::log10(std::max(1.e-20, wet) / dry);
    };
    for (unsigned preset = 0; preset < 5; ++preset) {
      auto state = factoryState(preset);
      state.values[SoftClip] = 0;
      state.values[Output] = 0;
      Audio compensated(unsigned(vowel.l.size())),
          raw(unsigned(vowel.l.size()));
      MorphInstance processor;
      load(processor, state);
      processor.activate({sr, 128, false});
      run(processor, vowel, compensated, 128, sr);
      state.values[AutoGain] = 0;
      load(processor, state);
      processor.reset();
      run(processor, vowel, raw, 128, sr);
      const double early = differenceDb(compensated, .5, 1);
      const double late = differenceDb(compensated, 2, 3);
      const double uncompensated = differenceDb(raw, 2, 3);
      std::printf("GAIN preset=%s rate=%.0f first_second_db=%.3f "
                  "settled_db=%.3f auto_off_db=%.3f\n",
                  factoryPresets()[preset].name, sr, early, late,
                  uncompensated);
      matched &= std::abs(late) < 1.0 && std::abs(early) < 1.8 &&
                 std::abs(late) <= std::abs(uncompensated) + .3;
    }
  }
  return matched;
}
void bench() {
  constexpr unsigned block = 128;
  auto input = signal(block, 48000);
  Audio output(block);
  std::vector<std::unique_ptr<MorphInstance>> instances;
  for (unsigned i = 0; i < 16; ++i) {
    auto p = std::make_unique<MorphInstance>();
    auto s = factoryState(29);
    s.values[ChorusVoices] = s.values[DoublerVoices] = 4;
    s.values[PhaserStages] = 4;
    s.values[DriveMode] = 2;
    for (unsigned slot = 0; slot < 16; ++slot) {
      s.values[matrixSource(slot)] = 1 + slot % 15;
      s.values[matrixTarget(slot)] = ChorusDepth;
      s.values[matrixAmount(slot)] = .01;
    }
    load(*p, s);
    p->activate({48000, block, false});
    instances.push_back(std::move(p));
  }
  for (unsigned count : {1u, 16u}) {
    std::vector<double> times;
    const unsigned measurements = count == 1 ? 12000 : 5000;
    times.reserve(measurements);
    std::int64_t pos = 0;
    const float *ins[]{input.l.data(), input.r.data()};
    float *outs[]{output.l.data(), output.r.data()};
    PluginProcessContext c;
    c.inputs = ins;
    c.outputs = outs;
    c.inputChannels = c.outputChannels = 2;
    c.frames = block;
    c.playing = true;
    for (unsigned i = 0; i < measurements + 1000; ++i) {
      c.sampleTime = pos;
      c.transport.ppqPosition = pos / 24000.;
      pos += block;
      const auto start = std::chrono::steady_clock::now();
      for (unsigned j = 0; j < count; ++j)
        instances[j]->process(c);
      const auto end = std::chrono::steady_clock::now();
      if (i >= 1000)
        times.push_back(
            std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(times.begin(), times.end());
    const double p99 = times[times.size() * 99 / 100];
    std::printf("BENCH instances=%u p50_ms=%.6f p99_ms=%.6f max_ms=%.6f "
                "deadline_percent=%.2f memory_192k_target=16MiB\n",
                count, times[times.size() / 2], p99, times.back(),
                100 * p99 / (block / 48.));
  }
}
} // namespace
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1 && std::string_view(argv[1]) == "--gain-bench") {
    gainCheck();
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--bench") {
#ifdef _WIN32
    if (argc > 3 && std::string_view(argv[2]) == "--cpu") {
      const unsigned cpu = unsigned(std::strtoul(argv[3], nullptr, 10));
      if (cpu < 64) {
        const auto mask =
            SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu);
        const auto priority =
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        std::printf("BENCH cpu=%u affinity_applied=%d above_normal=%d\n", cpu,
                    mask != 0, priority != 0);
      }
    }
#endif
    bench();
    return 0;
  }
  check(factoryPresets().size() == 30, "exactly 30 factory presets");
  bool fullyWet = defaults()[Mix] == 1 && factoryState(30).values[Mix] == 1;
  for (unsigned i = 0; i < 30; ++i)
    fullyWet &= factoryState(i).values[Mix] == 1 &&
                factoryState(i).values[PhaserMix] == 1;
  check(fullyWet, "Init and all 30 factory presets use 100% mix");
  MorphInstance p;
  auto in = signal(8192, 48000);
  Audio a(8192), b(8192);
  auto neutral = factoryState(30);
  neutral.values[Mix] = 0;
  neutral.values[AutoGain] = 0;
  load(p, neutral);
  p.activate({48000, 128, false});
  run(p, in, a, 128);
  check(diff(in, a) == 0 && p.latencySamples() == 0,
        "dry is bit exact with zero reported latency");
  neutral.values[Mix] = 1;
  for (auto i : moduleOffsets)
    neutral.values[i] = 0;
  load(p, neutral);
  p.reset();
  run(p, in, a, 128);
  check(diff(in, a) == 0, "all sound sections off is transparent");
  check(gainCheck(),
        "fully wet vocal presets match input level at 48 and 192 kHz");
  {
    auto level = factoryState(0);
    level.values[Mix] = 0;
    level.values[SoftClip] = 0;
    load(p, level);
    p.reset();
    run(p, in, a, 128);
    check(diff(in, a) == 0,
          "Auto Gain leaves the zero-latency dry signal bit exact");
    level.values[Output] = 6;
    load(p, level);
    p.reset();
    run(p, in, a, 128);
    double gainError = 0;
    for (unsigned f = 0; f < in.l.size(); ++f)
      gainError = std::max(gainError,
                           std::abs(a.l[f] - in.l[f] * std::pow(10., 6. / 20)));
    check(gainError < 1.e-6,
          "manual Output gain supplies the requested 6 dB trim");
    level.values[Output] = 0;
    level.values[Mix] = 1;
    load(p, level);
    p.reset();
    auto sustained = signal(96000, 48000);
    Audio gained(96000), silence(48000), tail(48000);
    run(p, sustained, gained, 128);
    const float settled = p.telemetry().autoGain;
    run(p, silence, tail, 128);
    check(bounded(tail) && p.telemetry().autoGain >= .25 &&
              p.telemetry().autoGain <= 4 &&
              std::abs(p.telemetry().autoGain - settled) < .4,
          "Auto Gain holds a bounded correction through silence and tails");
    p.setParameterFromHost(AutoGain, 0);
    run(p, silence, tail, 128);
    check(std::abs(p.telemetry().autoGain - 1) < .001,
          "disabling Auto Gain smoothly returns to unity");
  }
  bool factory = true, roundtrip = true, unique = true;
  std::array<double, 30> fingerprints{};
  for (unsigned n = 0; n < 30; ++n) {
    auto s = factoryState(n);
    std::vector<std::uint8_t> bytes;
    factory &= encodeState(s, bytes);
    auto restored = decodeState(bytes);
    roundtrip &= restored && restored->values == s.values &&
                 restored->canvas.seed == s.canvas.seed &&
                 restored->presetName == s.presetName;
    load(p, s);
    p.reset();
    run(p, in, a, 257);
    factory &= bounded(a);
    for (unsigned i = 0; i < a.l.size(); ++i)
      fingerprints[n] += a.l[i] * (1 + (i % 31));
    if (n) {
      for (unsigned j = 0; j < n; ++j)
        unique &= std::abs(fingerprints[n] - fingerprints[j]) > 1.e-6;
    }
  }
  check(factory && unique, "all 30 presets produce bounded distinct audio");
  check(roundtrip, "factory state, Canvas, seed and parameters round-trip");
  auto randomBase = factoryState(27);
  randomBase.values[Mix] = .2;
  randomBase.values[Output] = -3;
  randomBase.values[matrixSource(15)] = 1;
  randomBase.values[matrixTarget(15)] = Mix;
  randomBase.values[matrixAmount(15)] = -.8;
  auto randomized = randomState(randomBase, 42);
  bool randomValid = randomized.values == randomState(randomBase, 42).values &&
                     randomized.values != randomState(randomBase, 43).values;
  for (unsigned seed = 0; seed < 64; ++seed) {
    const auto s = randomState(randomBase, seed);
    randomValid &= s.values[Mix] == 1 && s.values[PhaserMix] == 1 &&
                   s.values[Output] == -3 && s.presetKind == "custom" &&
                   s.canvas.rgba == randomBase.canvas.rgba &&
                   s.canvas.seed == randomBase.canvas.seed &&
                   validProgram(s.canvas);
    for (const auto &info : parameterTable()) {
      const auto value = s.values[info.index];
      randomValid &= std::isfinite(value) && value >= info.minValue &&
                     value <= info.maxValue &&
                     (!info.isStepped || value == std::round(value));
    }
    for (unsigned slot = 0; slot < 16; ++slot)
      randomValid &= s.values[matrixSource(slot)] == 0 ||
                     (s.values[matrixTarget(slot)] != Mix &&
                      s.values[matrixTarget(slot)] != PhaserMix);
    load(p, s);
    p.reset();
    run(p, in, a, 257);
    randomValid &= bounded(a);
  }
  check(randomValid,
        "random sounds are reproducible, varied, bounded and fully wet");
  auto moving = factoryState(27);
  load(p, moving);
  p.reset();
  run(p, in, a, 1);
  p.reset();
  run(p, in, b, 257);
  check(diff(a, b) < 1.e-6,
        "Canvas and modulation are independent of host block size");
  std::vector<std::uint8_t> saved;
  p.saveState(saved);
  MorphInstance clone;
  clone.loadState(saved);
  clone.activate({48000, 1024, true});
  run(clone, in, b, 1024);
  check(diff(a, b) < 1.e-6, "offline clone reproduces saved program");
  auto malformed = saved;
  malformed.resize(17);
  const auto old = p.state();
  check(!p.loadState(malformed) && p.state().values == old.values,
        "malformed state rejected without partial mutation");
  auto image = moving.canvas;
  image.cells[0].light = std::numeric_limits<float>::quiet_NaN();
  check(!p.setCanvas(image), "invalid prepared Canvas rejected");
  auto canvas = moving.canvas;
  const auto generated = interpretCanvas(canvas, defaults());
  check(generated == interpretCanvas(canvas, defaults()),
        "image interpretation is deterministic");
  canvas.seed++;
  check(generated != interpretCanvas(canvas, defaults()),
        "variation seed changes interpretation");
  check(generated[Mix] == defaults()[Mix] && generated[Output] == 0,
        "generation protects output and mix by default");
  {
    // Mirrored photos have identical global colors/contrast. Their spatial
    // fingerprint must still produce different initial sounds, without a
    // running scanner being responsible for the difference.
    auto photo = factoryState(30);
    auto &c = photo.canvas;
    c.previewWidth = 72;
    c.previewHeight = 128;
    for (unsigned y = 0; y < c.previewHeight; ++y)
      for (unsigned x = 0; x < c.previewWidth; ++x)
        c.rgba[y * c.previewSide + x] = x < 36 ? 0xff35b9ef : 0xffad5020;
    analyzeCanvas(c);
    c.seed = canvasFingerprint(c);
    auto mirrored = c;
    for (unsigned y = 0; y < c.previewHeight; ++y)
      for (unsigned x = 0; x < c.previewWidth; ++x)
        mirrored.rgba[y * c.previewSide + x] =
            c.rgba[y * c.previewSide + c.previewWidth - x - 1];
    analyzeCanvas(mirrored);
    mirrored.seed = canvasFingerprint(mirrored);
    check(validProgram(c) && validProgram(mirrored) &&
              c.seed != mirrored.seed && c.seed == canvasFingerprint(c),
          "image fingerprint is repeatable and reflects spatial content");
    auto padded = c;
    padded.rgba[100] = 0xff00ff00; // Outside a portrait's visible width.
    check(c.seed == canvasFingerprint(padded),
          "image fingerprint ignores unused row padding");

    photo.values[Output] = -4;
    photo.values[Mix] = .1;
    for (auto id : moduleOffsets)
      photo.values[id] = 0;
    auto values = interpretCanvas(c, photo.values);
    check(values == interpretCanvas(c, values) && values[Mix] == 1 &&
              values[Output] == -4 && values[ChorusOn] == 1 &&
              values[FlangerOn] == 1 && values[PhaserOn] == 1 &&
              values[DoublerOn] == 1,
          "photo generation is idempotent, enables effects and keeps full mix");
    auto locks = photo.values;
    locks[CanvasLocks] = 127;
    const auto locked = interpretCanvas(c, locks);
    locks[Mix] = locks[CanvasOn] = 1;
    check(locked == locks, "photo generation honors all seven group locks");
    locks[CanvasLocks] = 0;
    locks[matrixSource(15)] = 1;
    locks[matrixTarget(15)] = Mix;
    locks[matrixAmount(15)] = -.5;
    const auto unprotected = interpretCanvas(c, locks);
    check(unprotected[Mix] == 1 && unprotected[Output] == -4 &&
              unprotected[matrixSource(15)] == 0,
          "photo generation never randomizes output or modulates overall mix");
    bool valid = true;
    for (unsigned i = 0; i < 64; ++i) {
      auto varied = c;
      varied.seed += i * 7919;
      const auto result = interpretCanvas(varied, defaults());
      for (const auto &info : parameterTable())
        valid &= std::isfinite(result[info.index]) &&
                 result[info.index] >= info.minValue &&
                 result[info.index] <= info.maxValue &&
                 (!info.isStepped ||
                  result[info.index] == std::round(result[info.index]));
    }
    check(valid,
          "image interpretations stay within continuous and discrete ranges");

    auto source = signal(48000, 48000);
    Audio first(48000), second(48000);
    photo.values = values;
    photo.values[CanvasOn] = photo.values[MotionOn] = 0;
    load(p, photo);
    p.reset();
    run(p, source, first, 257);
    photo.canvas = mirrored;
    photo.values = interpretCanvas(mirrored, values);
    photo.values[CanvasOn] = photo.values[MotionOn] = 0;
    load(p, photo);
    p.reset();
    run(p, source, second, 257);
    double energy = 0, difference = 0;
    for (unsigned i = 0; i < source.l.size(); ++i) {
      energy += source.l[i] * source.l[i];
      const double delta = first.l[i] - second.l[i];
      difference += delta * delta;
    }
    const double relative = std::sqrt(difference / energy);
    std::printf("MEASURE photo_static_difference_rms_ratio=%.6f\n", relative);
    check(bounded(first) && bounded(second) && relative > .08,
          "different photos change audio substantially with scanner stopped");
    std::vector<std::uint8_t> bytes;
    encodeState(photo, bytes);
    const auto restored = decodeState(bytes);
    check(restored && restored->canvas.rgba == photo.canvas.rgba &&
              restored->values == photo.values &&
              restored->canvas.seed == photo.canvas.seed,
          "portrait pixels, image seed and generated sound survive "
          "serialization");
  }
  bool matrix = true;
  std::size_t maximumMemory = 0;
  for (unsigned channels : {1u, 2u})
    for (double sr : {44100., 48000., 96000., 192000.})
      for (unsigned block : {1u, 32u, 64u, 128u, 257u, 1024u}) {
        MorphInstance test;
        PluginBusLayout accepted;
        matrix &= test.setBusLayout(
            {{std::uint16_t(channels)}, {std::uint16_t(channels)}}, accepted);
        auto extreme = factoryState(29);
        for (const auto &info : parameterTable())
          if (info.index < MatrixStart && info.index != Output)
            extreme.values[info.index] = info.maxValue;
        load(test, extreme);
        matrix &= test.activate({sr, block, false});
        auto source = signal(2048, sr);
        Audio dest(2048);
        run(test, source, dest, block, sr, {}, channels);
        matrix &= bounded(dest);
        matrix &= test.dspMemoryBytes() < 16 * 1024 * 1024;
        maximumMemory = std::max(maximumMemory, test.dspMemoryBytes());
      }
  std::printf("MEASURE DSP_memory_bytes_192k=%zu\n", maximumMemory);
  check(matrix, "mono/stereo, rates, block sizes, maximum controls and 16 MiB "
                "memory budget");
  load(p, neutral);
  p.setParameterFromHost(ChorusOn, 1);
  p.setParameterFromHost(Mix, 0);
  p.reset();
  PluginEvent event;
  event.paramIndex = Mix;
  event.value = 1;
  event.frameOffset = 500;
  run(p, in, a, 128, 48000, {&event, 1});
  bool exact = true;
  for (unsigned i = 0; i < 500; ++i)
    exact &= a.l[i] == in.l[i];
  check(exact && diff(in, a) > .001,
        "automation starts at the requested sample offset");
  Audio silence(8192);
  load(p, moving);
  p.reset();
  run(p, silence, a, 128);
  check(diff(silence, a) == 0, "silence stays silent");
  {
    MorphInstance mono;
    PluginBusLayout wanted{{1}, {1}}, accepted;
    check(mono.setBusLayout(wanted, accepted) &&
              mono.activate({48000, 128, false}),
          "mono bus supported");
    mono.setParameterFromHost(Mix, 0);
    mono.setParameterFromHost(Output, 0);
    mono.setParameterFromHost(SoftClip, 0);
    mono.reset();
    std::array<float, 128> input{}, output{};
    for (unsigned i = 0; i < input.size(); ++i)
      input[i] = i % 2 ? -32.f : 32.f;
    const float *ins[]{input.data()};
    float *outs[]{output.data()};
    PluginProcessContext ctx;
    ctx.inputs = ins;
    ctx.outputs = outs;
    ctx.inputChannels = ctx.outputChannels = 1;
    ctx.frames = 128;
    mono.process(ctx);
    check(input == output, "mono dry path remains exact above full scale");
    mono.setParameterFromHost(Mix, 1);
    input[0] = std::numeric_limits<float>::infinity();
    input[1] = std::numeric_limits<float>::quiet_NaN();
    mono.process(ctx);
    check(std::all_of(output.begin(), output.end(),
                      [](float f) { return std::isfinite(f); }),
          "NaN and infinity do not escape");
  }
  {
    auto gray = moving.canvas;
    gray.rgba.fill(0xff808080);
    gray.previewWidth = gray.previewHeight = 128;
    gray.painted = true;
    analyzeCanvas(gray);
    bool neutral = true;
    for (const auto &c : gray.cells)
      for (float weight : c.weights)
        neutral &= std::abs(weight - .25f) < 1.e-5;
    check(neutral, "gray image has neutral effect balance");
    MorphCanvasProgram empty;
    check(!empty.painted && sampleCanvas(empty, 0, 0).weights ==
                                std::array<float, 4>{.25, .25, .25, .25},
          "empty Canvas is neutral");
  }
  {
    auto motion = factoryState(27);
    motion.values[MotionBars] = 1;
    motion.values[MotionOn] = 1;
    load(p, motion);
    p.reset();
    std::array<float, 16> left{}, right{};
    const float *ins[]{left.data(), right.data()};
    float *outs[]{left.data(), right.data()};
    PluginProcessContext ctx;
    ctx.inputs = ins;
    ctx.outputs = outs;
    ctx.inputChannels = ctx.outputChannels = 2;
    ctx.frames = 16;
    ctx.playing = true;
    ctx.transport.tempo = 120;
    ctx.transport.timeSigNumerator = 4;
    ctx.transport.timeSigDenominator = 4;
    ctx.transport.ppqPosition = 3;
    p.process(ctx);
    const auto first = p.telemetry().phase;
    ctx.transport.ppqPosition = 19;
    p.process(ctx);
    check(std::abs(first - .375) < 1.e-6 && first == p.telemetry().phase,
          "seek and repeated musical sections use the same motion phase");
    p.setParameterFromHost(MotionReverse, 1);
    p.process(ctx);
    check(std::abs(p.telemetry().phase - .625) < 1.e-6,
          "reverse uses the complementary musical phase");
    p.setParameterFromHost(MotionReverse, 0);
    ctx.playing = false;
    ctx.transport.tempo = std::numeric_limits<double>::quiet_NaN();
    p.process(ctx);
    const auto stopped = p.telemetry().phase;
    p.process(ctx);
    check(p.telemetry().phase > stopped &&
              std::abs((p.telemetry().phase - stopped) - 16. / (24000 * 8)) <
                  1.e-6,
          "stopped transport keeps moving with the last valid tempo");
  }
  {
    auto drive = factoryState(30);
    for (auto id : moduleOffsets)
      drive.values[id] = 0;
    drive.values[Drive] = .5;
    drive.values[Mix] = 1;
    drive.values[LowPass] = 20000;
    drive.values[AutoGain] = drive.values[Breathing] = 0;
    load(p, drive);
    p.reset();
    auto source = signal(65536, 48000);
    Audio rendered(65536), naive(65536);
    for (unsigned i = 0; i < source.l.size(); ++i) {
      const double x = .12 * std::sin(2 * 3.141592653589793 * 9000 * i / 48000),
                   v = x * 4.5;
      source.l[i] = source.r[i] = float(x);
      naive.l[i] = float((v - v * v * v / 3) / std::sqrt(4.5));
    }
    run(p, source, rendered, 128);
    const auto bin = [](const Audio &a, double frequency) {
      double real = 0, imag = 0;
      for (unsigned i = 57344; i < 65536; ++i) {
        double phase = 2 * 3.141592653589793 * frequency * i / 48000;
        real += a.l[i] * std::cos(phase);
        imag += a.l[i] * std::sin(phase);
      }
      return std::hypot(real, imag);
    };
    const double ratio = (bin(rendered, 21000) / bin(rendered, 9000)) /
                         (bin(naive, 21000) / bin(naive, 9000));
    std::printf("MEASURE ADAA_alias_relative=%.6f\n", ratio);
    check(ratio < .8,
          "ADAA lowers aliased third harmonic relative to fundamental");
    drive.values[Drive] = 0;
    load(p, drive);
    p.reset();
    PluginEvent onset;
    onset.frameOffset = 512;
    onset.paramIndex = Drive;
    onset.value = .5;
    run(p, source, rendered, 128, 48000, {&onset, 1});
    double onsetDifference = 0;
    for (unsigned i = 512; i < 528; ++i)
      onsetDifference = std::max(onsetDifference,
                                 std::abs(double(rendered.l[i]) - source.l[i]));
    std::printf("MEASURE Drive_onset_difference=%.9g\n", onsetDifference);
    check(onsetDifference < .006, "Drive from zero fades in ADAA phase without "
                                  "an abrupt half-sample jump");
  }
  {
    load(p, moving);
    p.reset();
    std::array<PluginEvent, 4> changes{};
    for (unsigned i = 0; i < 4; ++i) {
      changes[i].frameOffset = 1500 + i * 1400;
      changes[i].paramIndex = i % 2 ? DriveMode : Routing;
      changes[i].value = i % 2 ? 2 : 0;
    }
    run(p, in, a, 128, 48000, changes);
    check(bounded(a),
          "routing and Drive mode changes stay finite during playback");
    auto stable = factoryState(30);
    stable.values[EchoLevel] = .4;
    stable.values[EchoTime] = 5;
    stable.values[EchoFeedback] = .88;
    stable.values[Drive] = 1;
    stable.values[DrivePosition] = 1;
    stable.values[DriveMode] = 1;
    stable.values[Mix] = 1;
    stable.values[AutoGain] = 0;
    load(p, stable);
    p.reset();
    Audio impulse(480000), tail(480000);
    impulse.l[0] = impulse.r[0] = .5;
    run(p, impulse, tail, 1024);
    double last = 0;
    for (unsigned i = 470000; i < 480000; ++i)
      last = std::max(last, std::abs(double(tail.l[i])));
    check(bounded(tail) && last < 1.e-6,
          "saturated feedback rejects DC and settles to silence");
  }
  auto program = moving.canvas;
  std::atomic<bool> done = false;
  std::thread producer([&] {
    for (unsigned i = 0; i < 500; ++i) {
      program.seed = i + 1;
      p.setCanvas(program);
    }
    done.store(true);
  });
  unsigned processed = 0;
  while (!done.load() || processed < 8) {
    run(p, in, a, 128);
    ++processed;
  }
  producer.join();
  check(bounded(a), "concurrent Canvas publication preserves audio ownership");
  check(allocations == 0,
        "no allocation in process including Canvas publication");
  check(releases == 0,
        "no deallocation in process including Canvas publication");
  std::printf("Morph failures: %d\n", failures);
  return failures ? 1 : 0;
}
