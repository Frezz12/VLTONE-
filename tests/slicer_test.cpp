// The built-in Slicer: analysis and manual editing, state compatibility,
// realtime playback, portable media and end-to-end controller integration.
//
// The audio under test is synthesised wherever the file layer is not the thing
// being checked. A ramp makes direction and gating readable as a single
// comparison, a constant tone makes Key Track readable as one frequency, and a
// train of bursts gives the transient detector something unambiguous to find —
// all of which real recordings would hide behind their own shape.
#include "SliceAnalysis.hpp"
#include "SlicerTools.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "recovery/RecoveryJournal.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include "WarpAnalysis.hpp"
#include "Internal/InternalFactory.hpp"
#include "Internal/SlicerInstance.hpp"
#include "Internal/SlicerParams.hpp"
#include "Audio/SampleBuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>


namespace allocationAudit { thread_local bool active=false; thread_local std::size_t count=0; }
void* operator new(std::size_t size) {
    if(allocationAudit::active) ++allocationAudit::count;
    if(auto* p=std::malloc(std::max(std::size_t(1),size))) return p; throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t size,std::align_val_t alignment) {
    if(allocationAudit::active) ++allocationAudit::count;
    const auto align=std::size_t(alignment);
#if defined(_WIN32)
    auto* p=_aligned_malloc(std::max(std::size_t(1),size),align);
#else
    auto* p=std::aligned_alloc(align,((std::max(std::size_t(1),size)+align-1)/align)*align);
#endif
    if(p) return p; throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void* operator new[](std::size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete[](void* p,std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete(void* p,std::size_t,std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete[](void* p,std::size_t,std::align_val_t a) noexcept { ::operator delete(p,a); }

namespace {
namespace fs = std::filesystem;
namespace p = daw::plugins::slicer;
namespace slicing = daw::slicer;
using daw::plugins::PluginEvent;
using daw::plugins::PluginProcessContext;
using p::Param;
using p::Slice;
using p::SliceTable;

int failures = 0;
bool check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
    return ok;
}

constexpr double kRate = 48000.0;
constexpr std::uint32_t kBlock = 512;

using SamplePtr = std::shared_ptr<const daw::engine::SampleBuffer>;

SamplePtr makeSample(daw::engine::FrameCount frames,
                     const std::function<float(daw::engine::FrameCount)>& gen) {
    auto buffer = std::make_shared<daw::engine::SampleBuffer>(2, frames, kRate);
    for (daw::engine::ChannelCount ch = 0; ch < buffer->channels(); ++ch) {
        float* plane = buffer->writableChannel(ch);
        for (daw::engine::FrameCount i = 0; i < frames; ++i) plane[i] = gen(i);
    }
    return buffer;
}

SamplePtr tone(daw::engine::FrameCount frames, double hz, float amplitude = 0.5f) {
    return makeSample(frames, [&](daw::engine::FrameCount i) {
        return float(amplitude * std::sin(2.0 * std::numbers::pi * hz * double(i) / kRate));
    });
}

SamplePtr ramp(daw::engine::FrameCount frames) {
    return makeSample(frames, [&](daw::engine::FrameCount i) {
        return frames > 1 ? float(double(i) / double(frames - 1)) : 0.0f;
    });
}

/// Eight bursts of a 2 kHz tone at deliberately uneven levels, separated by
/// silence. The uneven levels are what give `sensitivity` something to bite on.
SamplePtr clickTrain() {
    const daw::engine::FrameCount frames = daw::engine::FrameCount(kRate * 2.0);
    const std::array<float, 8> levels{1.0f, 0.02f, 0.9f, 0.03f, 0.95f, 0.02f, 0.85f, 0.04f};
    return makeSample(frames, [levels](daw::engine::FrameCount i) {
        const std::size_t burst = std::size_t(i) / std::size_t(kRate * 0.25);
        if (burst >= levels.size()) return 0.0f;
        const double within = double(std::size_t(i) % std::size_t(kRate * 0.25));
        if (within >= 400.0) return 0.0f;
        return float(levels[burst] *
                     std::sin(2.0 * std::numbers::pi * 2000.0 * within / kRate));
    });
}

/// A table of `count` equal chops over `frames`, keys ascending from `root`.
SliceTable evenTable(daw::engine::FrameCount frames, int count, int root) {
    slicing::SliceSettings settings;
    settings.mode = slicing::SliceMode::Grid;
    settings.targetCount = count;
    settings.rootNote = root;
    return slicing::cut(*tone(frames, 1000.0), settings);
}

// ── Rendering harness ───────────────────────────────────────────────────────

struct Block {
    std::vector<float> left, right, outL, outR;
    const float* in[2];
    float* out[2];
    explicit Block(std::uint32_t n)
        : left(n), right(n), outL(n), outR(n),
          in{left.data(), right.data()}, out{outL.data(), outR.data()} {}
    void run(p::SlicerInstance& instance, std::uint32_t frames,
             std::span<const PluginEvent> events = {}) {
        PluginProcessContext context;
        context.inputs = in;
        context.inputChannels = 2;
        context.outputs = out;
        context.outputChannels = 2;
        context.frames = frames;
        context.inputEvents = events;
        instance.process(context);
    }
};

/// Drives `instance` for `total` frames from a clean slate, delivering
/// `events` at their absolute frame offsets, and returns the left channel.
std::vector<float> render(p::SlicerInstance& instance, std::uint32_t total,
                          const std::vector<PluginEvent>& events = {}) {
    instance.reset();
    Block block(kBlock);
    std::vector<float> out(total, 0.0f);
    std::size_t next = 0;
    std::vector<PluginEvent> here;
    for (std::uint32_t start = 0; start < total; start += kBlock) {
        const std::uint32_t frames = std::min(kBlock, total - start);
        here.clear();
        while (next < events.size() && events[next].frameOffset < start + frames) {
            PluginEvent event = events[next++];
            event.frameOffset = event.frameOffset - start;
            here.push_back(event);
        }
        block.run(instance, frames, std::span<const PluginEvent>(here.data(), here.size()));
        std::copy_n(block.outL.begin(), frames, out.begin() + start);
    }
    return out;
}

PluginEvent noteOn(std::uint32_t at, int key, double velocity = 1.0) {
    PluginEvent event;
    event.kind = PluginEvent::Kind::NoteOn;
    event.frameOffset = at;
    event.key = std::int16_t(key);
    event.channel = 0;
    event.value = velocity;
    return event;
}

PluginEvent noteOff(std::uint32_t at, int key) {
    PluginEvent event;
    event.kind = PluginEvent::Kind::NoteOff;
    event.frameOffset = at;
    event.key = std::int16_t(key);
    event.channel = 0;
    return event;
}

double rms(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    if (to <= from || to > samples.size()) return 0.0;
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) sum += double(samples[i]) * double(samples[i]);
    return std::sqrt(sum / double(to - from));
}

/// DFT magnitude at `hz` over the last `count` frames. `count` is chosen so
/// `hz` and its second harmonic both land on a whole number of cycles, which
/// is what keeps one out of leaking into the other's bin.
double amplitude(const std::vector<float>& samples, double hz, std::size_t count) {
    if (samples.size() < count) return 0.0;
    const std::size_t start = samples.size() - count;
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double angle = 2.0 * std::numbers::pi * hz * double(i) / kRate;
        re += double(samples[start + i]) * std::cos(angle);
        im += double(samples[start + i]) * std::sin(angle);
    }
    return 2.0 * std::hypot(re, im) / double(count);
}

/// Every chop starts where the one before it ended, and the pair covers the
/// sample exactly once. The contract the panel, the project file and playback
/// all assume, whatever mode produced the table.
bool tilesExactly(const SliceTable& table, daw::engine::FrameCount frames) {
    if (table.count == 0 || table.frames != frames) return false;
    if (table.slices[0].start != 0) return false;
    for (std::uint32_t i = 0; i < table.count; ++i) {
        const Slice& slice = table.slices[i];
        if (slice.end <= slice.start || slice.end > frames) return false;
        if (i > 0 && slice.start != table.slices[i - 1].end) return false;
        if (i + 1 == table.count && slice.end != frames) return false;
    }
    return true;
}

bool ascendingKeys(const SliceTable& table) {
    for (std::uint32_t i = 1; i < table.count; ++i)
        if (table.slices[i].key <= table.slices[i - 1].key) return false;
    return table.count > 1;
}

// ── Knobs ───────────────────────────────────────────────────────────────────

void testParameters() {
    const std::span<const daw::plugins::ParameterInfo> table = p::parameterTable();
    check(table.size() == p::kParameterCount, "one row per knob");
    check(p::kParameterCount == 20, "twenty automated sound parameters");
    const std::array<const char*, 9> originalIds{"vol", "drive", "att", "rel", "gate", "keytrack", "root", "choke", "veld"};
    bool stable = true; for (std::size_t i = 0; i < originalIds.size(); ++i) stable &= table[i].id == originalIds[i];
    check(stable, "the original nine IDs and indices remain unchanged");

    bool ordered = true;
    for (std::uint32_t i = 0; i < table.size(); ++i) ordered &= table[i].index == i;
    check(ordered, "every row carries its own index");

    p::SlicerInstance instance;
    bool resolvable = true;
    for (const daw::plugins::ParameterInfo& info : table)
        resolvable &= instance.parameterIndexForId(info.id) == std::int32_t(info.index);
    check(resolvable, "every id resolves back to its index");

    check(instance.parameterIndexForId("scale") < 0, "no Scale knob");
    check(instance.parameterIndexForId("dir") < 0, "no Direction knob");
    check(instance.parameterIndexForId("scamt") < 0, "no Scale Amount knob");

    check(instance.parameterValue(p::indexOf(Param::Volume)) == 1.0, "Volume defaults to unity");
    check(instance.parameterValue(p::indexOf(Param::Drive)) == 0.0, "Drive defaults to clean");
    check(instance.parameterValue(p::indexOf(Param::KeyTrack)) == 0.0,
          "Key Track defaults off — a chop sounds as recorded");
    check(instance.parameterValue(p::indexOf(Param::Gate)) == 1.0, "Gate defaults to the whole chop");
    check(instance.parameterValue(p::indexOf(Param::ChokeMode)) == 0.0, "Choke defaults to off");

    bool readable = true, consistent = true;
    for (const daw::plugins::ParameterInfo& info : table) {
        for (const double value : {info.minValue, info.defaultValue, info.maxValue}) {
            const std::string text = p::parameterText(info.index, value);
            readable &= !text.empty();
            consistent &= instance.parameterText(info.index, value) == text;
        }
    }
    check(readable, "every knob reads as something at each end of its range");
    check(consistent, "the instance readout reaches the free function instead of itself");
}

// ── Scales and the key layout ───────────────────────────────────────────────

void testScales() {
    bool sane = true, named = true;
    for (int scale = 0; scale < p::kScaleCount; ++scale) {
        const std::span<const std::uint8_t> degrees = p::scaleDegrees(scale);
        sane &= !degrees.empty() && degrees[0] == 0;
        sane &= p::degreeToSemitone(0, scale) == 0;
        named &= p::scaleName(scale) != nullptr && *p::scaleName(scale) != '\0';
    }
    check(sane, "every scale starts on its root");
    check(named, "every scale has a name to show");

    bool chromatic = true;
    for (int degree = 0; degree < 128; ++degree)
        chromatic &= p::degreeToSemitone(degree, 0) == degree;
    check(chromatic, "the chromatic scale walks one semitone per degree");

    check(p::degreeToSemitone(7, 1) == 12, "a major scale's eighth degree is an octave");
    check(p::degreeToKey(7, 60, 1) == 72, "an octave up from C is C");
    check(p::degreeToKey(-1, 60, 1) == 59, "a degree below the root steps down");
    check(p::degreeToSemitone(200, 0) == 200, "the semitone offset does not clamp");
    check(p::degreeToKey(4000, 0, 0) == 127, "the key does, at the top of the keyboard");
    check(p::degreeToKey(-4000, 60, 0) == 0, "and at the bottom");
}

// ── The chop table ──────────────────────────────────────────────────────────

void testTable() {
    SliceTable chops;
    chops.frames = 1000;
    chops.count = 3;
    chops.slices[0].start = 0;
    chops.slices[0].end = 300;
    chops.slices[0].key = 60;
    chops.slices[1].start = 300;
    chops.slices[1].end = 700;
    chops.slices[1].key = 60;
    chops.slices[2].start = 700;
    chops.slices[2].end = 1000;
    chops.slices[2].key = 64;
    chops.rebuild();

    check(chops.indexForKey(60) == 0, "a duplicated key reports its first chop");
    check(chops.indexForKey(64) == 2, "an occupied key reports its chop");
    check(chops.forKey(64) != nullptr && chops.forKey(64)->start == 700,
          "the lookup hands back the chop itself");
    check(chops.forKey(61) == nullptr, "an unoccupied key is nothing");
    check(chops.indexForKey(-1) == -1 && chops.indexForKey(128) == -1,
          "a key outside the keyboard is nothing");
    check(chops.forKeySpan(60).size() == 2, "both chops parked on the same key are reported");
    check(chops.forKeySpan(61).empty(), "an unoccupied key spans nothing");
    check(chops.forIndex(3) == nullptr, "beyond the count is nothing");
    check(chops.forIndex(2) != nullptr, "within the count is a chop");

    const Slice slice = chops.slices[0];
    check(slice.valid(1000), "a chop inside the sample is valid");
    check(!Slice{0, 0, 60}.valid(1000), "an empty range is not");
    check(!Slice{900, 1001, 60}.valid(1000), "a chop running past the end is not");
}

// ── Grid ────────────────────────────────────────────────────────────────────

void testGrid() {
    const SamplePtr audio = tone(daw::engine::FrameCount(kRate), 1000.0);
    const daw::engine::FrameCount frames = audio->frames();

    slicing::SliceSettings settings;
    settings.mode = slicing::SliceMode::Grid;
    settings.targetCount = 16;
    settings.rootNote = 48;

    const SliceTable table = slicing::cut(*audio, settings);
    check(table.count == 16, "the grid cuts exactly what was asked for");
    check(tilesExactly(table, frames), "grid chops tile the sample exactly");
    check(ascendingKeys(table), "chops are laid out in ascending order");
    check(table.indexForKey(48) == 0, "the first chop sits on the root");
    check(table.indexForKey(63) == 15, "sixteen chromatic chops end fifteen semitones up");
    check(table.forKey(47) == nullptr, "nothing sits below the root");

    // A length that divides by nothing: integer boundaries must still meet.
    const SamplePtr odd = tone(daw::engine::FrameCount(1001), 1000.0);
    settings.targetCount = 7;
    const SliceTable uneven = slicing::cut(*odd, settings);
    check(uneven.count == 7, "an awkward length still gives the requested count");
    check(tilesExactly(uneven, odd->frames()),
          "integer boundaries tile an awkward length without a gap or an overlap");

    settings.targetCount = 400;
    const SliceTable capped = slicing::cut(*audio, settings);
    check(capped.count == p::kMaxSlices, "an absurd count is capped at the keyboard's ceiling");
    check(tilesExactly(capped, frames), "the capped table still tiles exactly");
}

// ── Transients ──────────────────────────────────────────────────────────────

void testTransients() {
    const SamplePtr audio = clickTrain();
    const double seconds = double(audio->frames()) / kRate;
    const std::vector<daw::analysis::WarpTransient> onsets =
        daw::analysis::detectWarpTransients(*audio, 0.0, seconds);
    check(onsets.size() >= 4, "the burst train reports its attacks");

    slicing::SliceSettings settings;
    settings.mode = slicing::SliceMode::Transients;
    settings.targetCount = 16;
    settings.sensitivity = 0.0;
    settings.preAttackMs = 0.0;
    const SliceTable loose = slicing::cut(*audio, settings);
    check(loose.count >= 4 && loose.count <= 16,
          "transient cuts land between the attacks and the requested ceiling");
    check(tilesExactly(loose, audio->frames()), "transient chops tile the sample exactly");
    check(loose.slices[0].start == 0, "the first chop starts at the beginning of the sample");

    settings.sensitivity = 1.0;
    const SliceTable strict = slicing::cut(*audio, settings);
    check(strict.count < loose.count, "turning sensitivity up drops the quieter attacks");

    // The detector reports a point *inside* the attack, so a chop that began
    // there would clip the front of the hit. The margin is what keeps it.
    settings.sensitivity = 0.0;
    settings.preAttackMs = 0.0;
    const SliceTable none = slicing::cut(*audio, settings);
    settings.preAttackMs = 10.0;
    const SliceTable margin = slicing::cut(*audio, settings);
    bool matches = none.count == margin.count && none.count > 1;
    bool earlier = false;
    for (std::uint32_t i = 0; i < margin.count && matches; ++i) {
        if (margin.slices[i].start > none.slices[i].start) matches = false;
        earlier |= margin.slices[i].start < none.slices[i].start;
    }
    check(matches && earlier, "a pre-attack margin moves every chop start back");

    settings.preAttackMs = 0.0;
    const SliceTable held = slicing::cut(*audio, settings, [] { return true; });
    const SliceTable abandoned = slicing::cut(*audio, settings, [] { return false; });
    check(held.count > 0, "a run that is allowed to finish returns a table");
    check(abandoned.count == 0, "a cancelled run returns nothing rather than an invented split");
}

void testFallback() {
    // Silence: no flux anywhere, so the detector has nothing to report and the
    // even split has to take over — an empty table would leave the instrument
    // mute with no way to say why.
    const SamplePtr silence = makeSample(daw::engine::FrameCount(kRate),
                                         [](daw::engine::FrameCount) { return 0.0f; });
    slicing::SliceSettings settings;
    settings.mode = slicing::SliceMode::Transients;
    settings.targetCount = 8;
    const SliceTable silent = slicing::cut(*silence, settings);
    check(silent.count == 8, "a sample with no attacks falls back to an even split");
    check(tilesExactly(silent, silence->frames()), "the fallback still tiles exactly");

    const SamplePtr steady = tone(daw::engine::FrameCount(kRate), 1000.0);
    const SliceTable stationary = slicing::cut(*steady, settings);
    check(stationary.count >= 1, "a steady tone still comes out sliced");
    check(tilesExactly(stationary, steady->frames()), "and still tiles exactly");
}

// ── Random ──────────────────────────────────────────────────────────────────

void testRandom() {
    const SamplePtr audio = tone(daw::engine::FrameCount(kRate), 1000.0);
    slicing::SliceSettings settings;
    settings.mode = slicing::SliceMode::Random;
    settings.targetCount = 16;
    settings.seed = 42;

    const SliceTable first = slicing::cut(*audio, settings);
    const SliceTable second = slicing::cut(*audio, settings);
    bool identical = first.count == second.count;
    for (std::uint32_t i = 0; i < first.count && identical; ++i)
        identical &= first.slices[i].start == second.slices[i].start;
    check(identical, "the same seed rebuilds the same chops");
    check(first.count == 16, "jitter never collapses two boundaries together");
    check(tilesExactly(first, audio->frames()), "random chops tile the sample exactly");

    settings.seed = 43;
    const SliceTable other = slicing::cut(*audio, settings);
    bool same = other.count == first.count;
    for (std::uint32_t i = 0; i < first.count && same; ++i)
        same &= other.slices[i].start == first.slices[i].start;
    check(!same, "a different seed gives a different cut");
}

// ── The layout ──────────────────────────────────────────────────────────────

void testLayout() {
    const SamplePtr audio = tone(daw::engine::FrameCount(kRate), 1000.0);
    slicing::SliceSettings settings;
    settings.mode = slicing::SliceMode::Grid;
    settings.targetCount = 24;
    settings.rootNote = 48;

    settings.descending = false;
    const SliceTable up = slicing::cut(*audio, settings);
    settings.descending = true;
    const SliceTable down = slicing::cut(*audio, settings);

    check(up.count == down.count && up.count == 24, "both directions cut the same number");
    check(up.slices[0].key == 48 && up.slices[23].key == 71,
          "an ascending run starts on the root");
    check(down.slices[0].key == 71 && down.slices[23].key == 48,
          "a descending run starts high and lands back on the root");

    bool sameKeys = true;
    for (int key = 0; key < 128; ++key)
        sameKeys &= (up.indexForKey(key) >= 0) == (down.indexForKey(key) >= 0);
    check(sameKeys, "both directions cover the same keys");

    // 128 chromatic chops from C3 would run off the top of the keyboard; the
    // layout wraps rather than piling the overflow onto one unreachable note.
    settings.targetCount = 128;
    const SliceTable wrap = slicing::cut(*audio, settings);
    bool covered = wrap.count == 128;
    for (int key = 0; key < 128 && covered; ++key) covered &= wrap.indexForKey(key) >= 0;
    check(covered, "a run that overflows the keyboard wraps so every note is reachable");

    settings.scale = 1;
    settings.descending = false;
    settings.targetCount = 24;
    const SliceTable major = slicing::cut(*audio, settings);
    check(major.slices[0].key == 48 && major.slices[1].key == 50,
          "a major layout skips the semitones");
    check(ascendingKeys(major), "and stays in ascending order");
}

// ── Registration and state ──────────────────────────────────────────────────

void testFactory() {
    const std::vector<daw::plugins::PluginDescriptor> builtins = daw::plugins::builtinPlugins();
    const auto found = std::find_if(builtins.begin(), builtins.end(),
                                    [](const daw::plugins::PluginDescriptor& descriptor) {
                                        return descriptor.uid == "daw.slicer";
                                    });
    check(found != builtins.end(), "the factory lists daw.slicer");
    if (found == builtins.end()) return;

    daw::plugins::InternalFactory factory;
    std::unique_ptr<daw::plugins::PluginInstance> created = factory.create(*found);
    check(created != nullptr, "the factory builds one");
    if (!created) return;
    check(created->descriptor().uid == "daw.slicer", "with the right identity");
    check(created->descriptor().isInstrument, "and as an instrument");
    check(!created->parameters().empty(), "wearing the parameter surface");
    check(!created->hasEditor(), "the host draws its own editor for this one");
    const daw::plugins::PitchCapabilities pitch = created->pitchCapabilities();
    check(pitch.pitchBend && !pitch.perNote && !pitch.mpe && !pitch.continuous,
          "channel bend only — nothing a chop has nowhere to go");
}

void testState() {
    p::SlicerInstance source;
    const SamplePtr audio = tone(daw::engine::FrameCount(kRate), 1000.0);
    check(source.adoptSample("/samples/kick.wav", audio), "a decode is adopted");
    source.setParameter(p::indexOf(Param::Volume), 0.25);
    source.setParameter(p::indexOf(Param::Drive), 0.5);
    source.setParameter(p::indexOf(Param::Attack), 0.01);
    source.setParameter(p::indexOf(Param::KeyTrack), 1.0);
    source.setParameter(p::indexOf(Param::RootNote), 60.0);
    source.setSliceTable(std::make_shared<SliceTable>(evenTable(audio->frames(), 16, 60)));

    // A project package carries the sample as a name beside its own state; the
    // plugin's ordinary state chunk carries the path it was loaded from.
    std::vector<std::uint8_t> project;
    check(source.saveProjectState(project, "kick.wav"), "the project state is written");
    std::vector<std::uint8_t> chunk;
    check(source.saveState(chunk), "the plugin state chunk is written");

    p::SlicerInstance restored;
    check(restored.loadState(chunk), "and read back");
    bool sameParams = true;
    for (std::uint32_t i = 0; i < p::kParameterCount; ++i)
        sameParams &= restored.parameterValue(i) == source.parameterValue(i);
    check(sameParams, "every knob comes back as it was saved");
    check(restored.samplePath() == "/samples/kick.wav", "the sample path survives");

    const std::shared_ptr<const SliceTable> chops = restored.sliceTable();
    check(chops != nullptr && chops->count == 16, "and so does the chop table");
    const std::shared_ptr<const SliceTable> original = source.sliceTable();
    if (chops && chops->count == 16 && original) {
        bool sameChops = true;
        for (std::uint32_t i = 0; i < 16; ++i) {
            sameChops &= chops->slices[i].start == original->slices[i].start;
            sameChops &= chops->slices[i].end == original->slices[i].end;
            sameChops &= chops->slices[i].key == original->slices[i].key;
            sameChops &= chops->slices[i].flags == original->slices[i].flags;
        }
        check(sameChops, "every chop comes back with its range, key and flags");
    }

    p::SlicerInstance packaged;
    check(packaged.loadProjectState(project, {}), "the project state is read");
    check(packaged.samplePath() == "kick.wav", "a bare sample name is kept as it was written");
    check(packaged.sampleName() == "kick.wav", "with the name the panel shows");

    // A project stores the sample inside its Content directory; the loader puts
    // it back against that directory rather than against the process cwd.
    const fs::path content = fs::temp_directory_path() / "vlt-slicer-test";
    p::SlicerInstance contained;
    check(contained.loadProjectState(project, content.string()),
          "a packaged state is read against a content directory");
    const fs::path resolved = contained.samplePath();
    check(resolved.filename() == "kick.wav" && resolved.parent_path() == content,
          "the bare name resolves inside the package");

    // Absent means empty: this state describes the whole instrument, so a
    // second project must not inherit the first one's chops.
    const std::string bare = R"({"version":1,"sample":"","params":{},"slices":[]})";
    std::vector<std::uint8_t> empty(bare.begin(), bare.end());
    p::SlicerInstance cleared;
    cleared.adoptSample("/samples/kick.wav", audio);
    cleared.setSliceTable(std::make_shared<SliceTable>(evenTable(audio->frames(), 4, 48)));
    check(cleared.loadProjectState(empty, {}) && cleared.sliceTable() &&
              cleared.sliceTable()->count == 0,
          "a state with no chops clears the ones before it");

    const std::string broken = "not json at all";
    std::vector<std::uint8_t> garbage(broken.begin(), broken.end());
    p::SlicerInstance refused;
    check(!refused.loadProjectState(garbage, {}), "malformed state is refused");
}

void testSampleAndTable() {
    p::SlicerInstance instance;
    const SamplePtr first = tone(daw::engine::FrameCount(kRate), 1000.0);
    const SamplePtr second = tone(daw::engine::FrameCount(kRate / 4), 1000.0);

    check(instance.adoptSample("/a.wav", first), "the first decode is adopted");
    instance.setSliceTable(std::make_shared<SliceTable>(evenTable(first->frames(), 8, 48)));
    check(instance.sliceTable() != nullptr, "with a table");

    check(instance.adoptSample("/a.wav", first), "the same file is re-adopted");
    check(instance.sliceTable() != nullptr, "and its table is kept — it still describes the audio");

    check(instance.adoptSample("/b.wav", second), "a different file is adopted");
    check(!instance.sliceTable() || instance.sliceTable()->count == 0,
          "and drops the old table, whose ranges belong to audio it no longer has");
    check(instance.rawSample() && instance.rawSample()->frames() == second->frames(),
          "while carrying the new decode");

    instance.clearSample();
    check(!instance.sample(), "clearing drops the audio");
    check(!instance.sliceTable(), "and the table with it");
    check(instance.samplePath().empty(), "and the reference");
    check(!instance.adoptSample("/a.wav", {}), "an empty decode is refused");

    // A table bigger than the keyboard: published trimmed, never mutated under
    // a note that may be reading it right now.
    auto oversized = std::make_shared<SliceTable>();
    oversized->count = p::kMaxSlices + 40;
    oversized->frames = 1000;
    for (std::uint32_t i = 0; i < oversized->count; ++i)
        oversized->slices[i % p::kMaxSlices].key = std::int16_t(i % 128);
    instance.setSliceTable(oversized);
    check(instance.sliceTable() && instance.sliceTable()->count == p::kMaxSlices,
          "an oversized table is trimmed before it is published");
    check(oversized->count == p::kMaxSlices + 40, "and the caller's own copy is left alone");

    instance.activate({kRate, kBlock, true, false});
    check(instance.isActive(), "activation succeeds");
    check(instance.busLayout().outputs.size() == 1 && instance.busLayout().outputs[0] == 2,
          "one stereo bus out");
    check(instance.tailSamples() >= std::uint32_t(0.003 * kRate),
          "the tail covers at least the shortest possible release");
    instance.setParameter(p::indexOf(Param::Release), 0.5);
    check(instance.tailSamples() >= std::uint32_t(0.5 * kRate), "and follows the Release knob");
    instance.reset();
    instance.deactivate();
    check(!instance.isActive(), "deactivation succeeds");
}

// ── Playback ────────────────────────────────────────────────────────────────

struct Rig {
    p::SlicerInstance instance;
    SamplePtr audio;

    explicit Rig(SamplePtr source, int chops = 2, int root = 48) : audio(std::move(source)) {
        instance.adoptSample("/synth.wav", audio);
        instance.setSliceTable(std::make_shared<SliceTable>(evenTable(audio->frames(), chops, root)));
        instance.setParameter(p::indexOf(Param::Attack), 0.0);
        instance.setParameter(p::indexOf(Param::Release), 0.05);
        instance.setParameter(p::indexOf(Param::RootNote), double(root));
        instance.activate({kRate, kBlock, true, false});
    }
};

void testSliceEffects() {
    const auto source=tone(48000,733);
    Rig clean(source,2);
    const auto dry=render(clean.instance,8192,{noteOn(0,48)});
    const auto neighbor=render(clean.instance,8192,{noteOn(0,49)});
    for(std::uint8_t effect=1;effect<=3;++effect) {
        Rig rig(source,2); auto table=std::make_shared<SliceTable>(*rig.instance.sliceTable());
        table->slices[0].effect=effect; table->slices[0].effectX=.72f; table->slices[0].effectY=.4f;
        table->slices[0].effectMix=.65f; rig.instance.setSliceTable(table);
        const auto wet=render(rig.instance,8192,{noteOn(0,48)});
        const auto untouched=render(rig.instance,8192,{noteOn(0,49)});
        check(wet!=dry && std::all_of(wet.begin(),wet.end(),[](float v){return std::isfinite(v) && std::abs(v)<=1.01f;}),
            "per-slice saturation, crusher and ring modulation produce finite processed audio");
        check(untouched==neighbor,"an effect on one slice leaves its neighbor unchanged");
        const auto chord=render(rig.instance,8192,{noteOn(0,48),noteOn(0,49)});
        bool independent=true; for(std::size_t i=0;i<chord.size();++i) independent &= std::abs(chord[i]-wet[i]-neighbor[i])<2e-6f;
        check(independent,"overlapping slices retain independent effects");
        table=std::make_shared<SliceTable>(*table); table->slices[0].effectMix=0;
        rig.instance.setSliceTable(table);
        check(render(rig.instance,8192,{noteOn(0,48)})==dry,"zero effect mix returns the exact dry signal");
    }
    Rig filtered(source,2); auto table=std::make_shared<SliceTable>(*filtered.instance.sliceTable());
    table->slices[0].filter=1; table->slices[0].cutoff=.3f; table->slices[0].effectMix=0;
    filtered.instance.setSliceTable(table);
    check(render(filtered.instance,8192,{noteOn(0,48)})==dry,"filter XY effects also respect the dry/wet control");
}

void testPlayback() {
    const SamplePtr audio = tone(daw::engine::FrameCount(kRate), 1000.0);

    {
        Rig rig(audio);
        const std::vector<float> out = render(rig.instance, 11500, {noteOn(0, 48)});
        check(rms(out, 2000, 11000) > 1e-3, "a note on a sliced key makes sound");
    }
    {
        Rig rig(audio);
        const std::vector<float> out = render(rig.instance, 4096, {noteOn(0, 90)});
        check(rms(out, 0, 4096) < 1e-9, "a note on an unsliced key makes none");
    }
    {
        // Key Track off is the default contract: the chop sounds exactly as
        // recorded, whatever key is played.
        Rig rig(audio);
        const std::vector<float> held = render(rig.instance, 11500, {noteOn(0, 48)});
        check(amplitude(held, 1000.0, 4800) > 0.3, "with Key Track off the source pitch survives");
        check(amplitude(held, 2000.0, 4800) < 0.05, "and nothing is transposed onto its octave");
    }
    {
        // The layout must hold a chop on the key played, or there is nothing
        // to transpose. Key 60 with a root of 48 is exactly one octave.
        Rig rig(audio, 1, 60);
        rig.instance.setParameter(p::indexOf(Param::RootNote), 48.0);
        rig.instance.setParameter(p::indexOf(Param::KeyTrack), 1.0);
        const std::vector<float> up = render(rig.instance, 11500, {noteOn(0, 60)});
        check(amplitude(up, 2000.0, 4800) > 0.3, "with Key Track on the octave is played");
        check(amplitude(up, 1000.0, 4800) < 0.05, "and the original pitch is gone");
    }
    {
        Rig rig(audio);
        auto chops = std::make_shared<SliceTable>(evenTable(audio->frames(), 2, 48));
        chops->slices[0].flags |= p::kSliceMuted;
        rig.instance.setSliceTable(chops);
        const std::vector<float> out = render(rig.instance, 4096, {noteOn(0, 48)});
        check(rms(out, 0, 4096) < 1e-9, "a muted chop keeps its key but makes no sound");
    }
    {
        // Level and Drive act on the summed output, so halving one halves what
        // leaves the instrument.
        Rig rig(audio);
        const std::vector<float> unity = render(rig.instance, 4096, {noteOn(0, 48)});
        rig.instance.setParameter(p::indexOf(Param::Volume), 0.5);
        const std::vector<float> quiet = render(rig.instance, 4096, {noteOn(0, 48)});
        const double ratio = rms(quiet, 0, 4096) / std::max(rms(unity, 0, 4096), 1e-12);
        check(std::abs(ratio - 0.5) < 0.01, "Volume scales the output exactly");
    }
    {
        Rig rig(audio);
        const std::vector<float> clean = render(rig.instance, 4096, {noteOn(0, 48)});
        rig.instance.setParameter(p::indexOf(Param::Drive), 1.0);
        const std::vector<float> driven = render(rig.instance, 4096, {noteOn(0, 48)});
        check(rms(driven, 0, 4096) > rms(clean, 0, 4096),
              "Drive at full pushes the sum harder rather than pulling it down");
    }
    {
        // Gate trims from the end, so the chop still starts on its own attack
        // and simply stops early.
        const SamplePtr longSample = tone(daw::engine::FrameCount(kRate), 1000.0);
        Rig rig(longSample, 1, 48);
        const std::vector<float> whole = render(rig.instance, 12000, {noteOn(0, 48)});
        rig.instance.setParameter(p::indexOf(Param::Gate), 0.1);
        const std::vector<float> cut = render(rig.instance, 12000, {noteOn(0, 48)});
        check(rms(whole, 4000, 11500) > 1e-3, "an ungated chop plays through");
        check(rms(cut, 0, 400) > 1e-3, "a gated chop still starts on its attack");
        check(rms(cut, 6000, 11500) < 1e-9, "and stops when the gate runs out");
    }
    {
        // Direction: a ramp read forwards rises, the same ramp read backwards
        // falls, and both are the same chop.
        const SamplePtr ramps = ramp(1000);
        SliceTable one;
        one.frames = 1000;
        one.count = 1;
        one.slices[0].end = 1000;
        one.slices[0].key = 48;
        one.rebuild();

        p::SlicerInstance forwards;
        forwards.adoptSample("/ramp.wav", ramps);
        forwards.setSliceTable(std::make_shared<SliceTable>(one));
        forwards.activate({kRate, kBlock, true, false});
        const std::vector<float> up = render(forwards, 1000, {noteOn(0, 48)});

        one.slices[0].flags |= p::kSliceReverse;
        one.rebuild();
        p::SlicerInstance backwards;
        backwards.adoptSample("/ramp.wav", ramps);
        backwards.setSliceTable(std::make_shared<SliceTable>(one));
        backwards.activate({kRate, kBlock, true, false});
        const std::vector<float> down = render(backwards, 1000, {noteOn(0, 48)});

        check(rms(up, 600, 950) > rms(up, 50, 400), "a forward chop rises");
        check(rms(down, 50, 400) > rms(down, 600, 950), "a reversed chop falls");
    }
    {
        // Choke mode one cuts the sounding voice the moment the next one starts;
        // with it off, the first note simply carries on under the second. Key 49
        // is the layout's second chop — an unsliced key has no note to choke with.
        const std::vector<PluginEvent> sequence{noteOn(0, 48), noteOn(kBlock, 49),
                                                noteOff(2 * kBlock, 49)};
        Rig off(audio);
        off.instance.setParameter(p::indexOf(Param::ChokeMode), 0.0);
        const std::vector<float> open = render(off.instance, 10240, sequence);
        Rig on(audio);
        on.instance.setParameter(p::indexOf(Param::ChokeMode), 1.0);
        const std::vector<float> cut = render(on.instance, 10240, sequence);
        check(rms(open, 8192, 10240) > 1e-4, "with choke off the first note keeps sounding");
        check(rms(cut, 8192, 10240) < 1e-9, "with choke on every voice has been cut");
    }
    {
        // A host that hands over a plugin with nowhere to write is not a crash.
        p::SlicerInstance idle;
        PluginProcessContext context;
        const daw::plugins::PluginProcessDisposition result = idle.process(context);
        check(result == daw::plugins::PluginProcessDisposition::Continue,
              "processing with no outputs is harmless");
    }
}


void testEditingV2() {
    const auto audio = tone(48000, 1000);
    slicing::SliceSettings settings; settings.mode = slicing::SliceMode::Manual;
    settings.rangeStart = 500; settings.rangeEnd = 45000;
    auto manual = slicing::cut(*audio, settings);
    check(manual.count == 1 && manual.slices[0].start == 500 && manual.slices[0].end == 45000, "manual mode keeps the selected range");
    const auto id = manual.slices[0].id;
    auto identity = manual; slicing::split(identity,24000); const auto deletedId=identity.slices[1].id; slicing::merge(identity,1); slicing::split(identity,25000);
    check(identity.slices[1].id!=deletedId,"a deleted slice ID is never reused by the next split");
    check(slicing::split(manual, 24000) && manual.slices[0].id == id && manual.slices[1].id != id, "split preserves the left ID and creates a distinct right ID");
    check(slicing::moveBoundary(manual, 1, 0) && manual.slices[0].end == 501 && manual.slices[1].start == 501, "manual boundary clamps to nonempty adjacent slices");
    check(!slicing::split(manual, 501) && slicing::merge(manual, 1) && manual.count == 1 && manual.slices[0].id == id, "duplicate boundary rejected and merge preserves identity");
    settings.mode = slicing::SliceMode::Grid; settings.rangeStart = 0; settings.rangeEnd = 0; settings.targetCount = 128;
    settings.scale = 8; settings.rootNote = 120;
    const auto maximum = slicing::cut(*audio, settings);
    std::array<bool, 128> used{}; bool unique = true, ids = true;
    for (std::uint32_t i = 0; i < maximum.count; ++i) { unique &= !used[maximum.slices[i].key]; used[maximum.slices[i].key] = true; ids &= maximum.slices[i].id != 0; }
    check(maximum.count == 128 && unique && ids && maximum.chromaticFallback, "128 slices have distinct IDs and unique notes with chromatic fallback");
    auto full = maximum; check(!slicing::split(full, 100), "a full 128-slice table refuses another split");
    settings.minimumMs = 100; auto minimum = slicing::cut(*audio, settings); bool longEnough = true;
    for (std::uint32_t i = 0; i < minimum.count; ++i) longEnough &= minimum.slices[i].end - minimum.slices[i].start >= 4800;
    check(minimum.count == 10 && longEnough, "minimum slice length constrains requested count");
    settings.minimumMs = 0; settings.gridBeats = .5; settings.sourceBpm = 120;
    auto beats = slicing::cut(*audio, settings); check(beats.count == 4 && beats.slices[1].start == 12000, "musical grid uses source BPM");
    settings.gridBeats = 0; settings.mode = slicing::SliceMode::Random; settings.randomSpread = 0; settings.targetCount = 16;
    const auto random = slicing::cut(*audio, settings); settings.mode = slicing::SliceMode::Grid; const auto grid = slicing::cut(*audio, settings);
    check(random.slices[7].start == grid.slices[7].start, "zero random spread is the equal-parts grid");
    settings.zeroCrossing = true; settings.targetCount = 7;
    const auto snap = slicing::cut(*audio, settings); bool zero = true;
    for (std::uint32_t i = 1; i < snap.count; ++i) zero &= std::abs(audio->readSample(0, snap.slices[i].start)) < .07f;
    check(zero, "zero-crossing snapping finds low-amplitude boundaries");
}
void testNewState() {
    Rig rig(tone(48000, 440), 4); auto table = std::make_shared<SliceTable>(*rig.instance.sliceTable());
    auto& s = table->slices[1]; s.fineTune = 37.5f; s.normalization = 1.5f; s.locked = true; s.loopMode = 2;
    s.fadeInMs = 4; s.fadeOutMs = 9; s.crossfadeMs = 2; s.useGlobalEnvelope = false;
    s.attack = .2f; s.decay = .3f; s.sustain = .4f; s.release = .5f;
    s.effect=2; s.effectX=.73f; s.effectY=.21f; s.effectMix=.64f;
    rig.instance.setSliceTable(table); auto analysis = rig.instance.analysisSettings(); analysis.minimumMs = 7; analysis.sourceBpm = 92; analysis.seed = 123456;
    analysis.rangeStart = 20; analysis.rangeEnd = 45000; rig.instance.setAnalysisSettings(analysis);
    rig.instance.setParameter(p::indexOf(Param::CrushMix), .5);
    std::vector<std::uint8_t> bytes; rig.instance.saveState(bytes);
    p::SlicerInstance staged; staged.adoptSample("/synth.wav", rig.audio); check(staged.loadState(bytes), "v3 state loads");
    check(staged.analysisSettings() == analysis && staged.sliceTable()->slices[1] == s, "all analysis, sound and per-slice XY effects survive save/load");
    auto v2=nlohmann::json::parse(bytes); v2["version"]=2;
    for(auto& slice:v2["slices"])for(const auto* field:{"effect","effectX","effectY","effectMix"})slice.erase(field);
    v2["slices"][1]["filter"]=2; const auto oldText=v2.dump();
    check(staged.loadState({reinterpret_cast<const std::uint8_t*>(oldText.data()),oldText.size()}) &&
        staged.sliceTable()->slices[1].effect==0 && staged.sliceTable()->slices[1].effectMix==1 && staged.sliceTable()->slices[1].filter==2,
        "v2 keeps its original filter and receives neutral additional effects");
    v2["version"]=3; v2["slices"][1]["effect"]=999; v2["slices"][1]["effectX"]=-5; v2["slices"][1]["effectY"]=5; v2["slices"][1]["effectMix"]="bad";
    const auto bounded=v2.dump(); staged.loadState({reinterpret_cast<const std::uint8_t*>(bounded.data()),bounded.size()});
    const auto validated=staged.sliceTable()->slices[1];
    check(validated.effect==3 && validated.effectX==0 && validated.effectY==1 && validated.effectMix==1,"untrusted effect fields are bounded and type checked");
    auto legacy = nlohmann::json{{"v", 1}, {"sample", "/synth.wav"}, {"params", {{"vol", .7}, {"rel", .2}}},
        {"slices", nlohmann::json::array({nlohmann::json::array({0, 48000, 48, 2, 1, 0, 1, 0, 0, 0, p::kSliceLoop})})}};
    // Use the legacy serializer's field names.
    legacy["version"] = 1; legacy["frames"] = 48000;
    const auto text = legacy.dump(); check(staged.loadState({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}), "v1 slice arrays remain readable");
    const auto old = staged.sliceTable()->slices[0];
    check(old.id != 0 && old.normalization == 1 && old.fineTune == 0 && old.useGlobalEnvelope && old.loopMode == 1 && old.fadeOutMs == -1,
        "v1 fields receive neutral defaults and retain loop semantics");
    legacy["version"] = 99; const auto future = legacy.dump(); const auto previous = staged.captureState();
    check(!staged.loadState({reinterpret_cast<const std::uint8_t*>(future.data()), future.size()}) && staged.sliceTable() == previous.table, "unknown future state is rejected without partial mutation");
}
void testAdvancedDSP() {
    const auto audio = tone(48000, 1000);
    Rig tuned(audio, 1); tuned.instance.setParameter(p::indexOf(Param::Transpose), 12);
    const auto pitch = render(tuned.instance, 10000, {noteOn(0, 48)});
    check(amplitude(pitch, 2000, 4800) > .3, "global transpose reaches the voice");
    Rig fine(audio, 1); fine.instance.setParameter(p::indexOf(Param::FineTune), 100);
    const auto cents = render(fine.instance, 10000, {noteOn(0, 48)});
    check(amplitude(cents, 1000 * std::pow(2.0, 1.0 / 12), 4800) > .3, "fine tune is measured in cents");
    Rig panned(audio, 1); panned.instance.setParameter(p::indexOf(Param::Pan), 1);
    check(rms(render(panned.instance, 10000, {noteOn(0,48)}), 1000,9000) < 1e-8, "global pan reaches the output balance");
    Rig gate(audio, 1), shot(audio, 1); shot.instance.setParameter(p::indexOf(Param::PlayMode), 1);
    const auto a = render(gate.instance, 10000, {noteOn(0,48),noteOff(512,48)});
    const auto b = render(shot.instance, 10000, {noteOn(0,48),noteOff(512,48)});
    check(rms(a,6000,9000)<1e-8 && rms(b,6000,9000)>.1, "one shot survives Note Off while gate releases");
    for (int loop : {1, 2}) {
        Rig rig(tone(1200, 1000), 1); auto table = std::make_shared<SliceTable>(*rig.instance.sliceTable());
        table->slices[0].loopMode = std::uint8_t(loop); table->slices[0].crossfadeMs = 2; rig.instance.setSliceTable(table);
        rig.instance.setParameter(p::indexOf(Param::PlayMode), 1);
        const auto out = render(rig.instance, 10000, {noteOn(0,48),noteOff(6000,48)});
        check(rms(out,3000,5500)>.1 && rms(out,9000,10000)<1e-8 && std::all_of(out.begin(),out.end(),[](float v){return std::isfinite(v);}), "forward/ping-pong loop repeats finitely and releases even in one-shot mode");
    }
    Rig plain(audio,1), crusher(audio,1); crusher.instance.setParameter(p::indexOf(Param::CrushBits),4); crusher.instance.setParameter(p::indexOf(Param::CrushRate),8);
    const auto neutral=render(crusher.instance,10000,{noteOn(0,48)}); const auto dry=render(plain.instance,10000,{noteOn(0,48)});
    check(neutral==dry, "crusher bit depth and rate are neutral with zero mix");
    crusher.instance.setParameter(p::indexOf(Param::CrushMix),1); const auto wet=render(crusher.instance,10000,{noteOn(0,48)});
    check(wet!=dry && wet[9000]==wet[9001] && std::all_of(wet.begin(),wet.end(),[](float v){return std::isfinite(v);}), "crusher quantizes and holds samples without invalid output");
    Rig adsr(audio,1); auto table=std::make_shared<SliceTable>(*adsr.instance.sliceTable());
    table->slices[0].useGlobalEnvelope=false; table->slices[0].decay=.005f; table->slices[0].sustain=.2f; table->slices[0].release=.1f;
    adsr.instance.setSliceTable(table); const auto env=render(adsr.instance,10000,{noteOn(0,48)});
    check(rms(env,4000,9000)<rms(dry,4000,9000)*.25 && adsr.instance.tailSamples()>=4800, "local ADSR and release tail override the global envelope");
    Rig bounds(audio,1); Block block(512); const auto on=noteOn(0,48); block.run(bounds.instance,512,{&on,1});
    auto moved=std::make_shared<SliceTable>(*bounds.instance.sliceTable()); moved->slices[0].end=1024; bounds.instance.setSliceTable(moved);
    block.run(bounds.instance,512); block.run(bounds.instance,512);
    check(rms(block.outL,0,512)>.1, "active voice keeps its original bounds after a boundary edit");
    const auto revision=bounds.instance.sourceRevision();auto restoredState=bounds.instance.captureState();restoredState.table=moved;
    bounds.instance.restoreState(restoredState);block.run(bounds.instance,512);
    check(bounds.instance.sourceRevision()==revision && rms(block.outL,0,512)>.1,"restoring an edit with the same source preserves active voice boundaries");
    const float oldOutput=block.outL.back();
    bounds.instance.adoptSample("/new.wav",makeSample(48000,[](auto){return 0.0f;}));
    allocationAudit::count=0; allocationAudit::active=true; block.run(bounds.instance,512);
    allocationAudit::active=false;
    check(std::abs(block.outL.front()-oldOutput)<1e-7f && rms(block.outL,240,512)<1e-8 &&
              allocationAudit::count==0,
          "source replacement retires voices without a click or realtime allocation");
}
void testRealtimeAndProcessing() {
    const auto source=tone(48000,1000); Rig rig(source,128); auto table=std::make_shared<SliceTable>(*rig.instance.sliceTable());
    for(std::uint32_t i=0;i<table->count;++i) { table->slices[i].loopMode=1; table->slices[i].crossfadeMs=1; table->slices[i].filter=std::uint8_t(i%4); table->slices[i].effect=std::uint8_t(i%4); table->slices[i].gain=.01f; }
    rig.instance.setSliceTable(table); rig.instance.setParameter(p::indexOf(Param::CrushMix),.5); rig.instance.setParameter(p::indexOf(Param::Drive),.1);
    std::array<PluginEvent,128> events{}; for(int i=0;i<128;++i) events[i]=noteOn(0,table->slices[i].key);
    Block block(512); allocationAudit::count=0; allocationAudit::active=true;
    block.run(rig.instance,512,events); for(int i=0;i<64;++i) block.run(rig.instance,512);
    allocationAudit::active=false; check(allocationAudit::count==0,"process performs no allocations with 128 triggers, loops, filters and crusher");
    Rig dry(source,1),filtered(source,1); auto t=std::make_shared<SliceTable>(*filtered.instance.sliceTable());t->slices[0].filter=1;t->slices[0].cutoff=float(std::log(200.0/20)/std::log(1000.0));filtered.instance.setSliceTable(t);
    const auto off=render(dry.instance,10000,{noteOn(0,48)}),lp=render(filtered.instance,10000,{noteOn(0,48)});
    check(rms(lp,4000,9000)<rms(off,4000,9000)*.15,"per-slice low-pass attenuates above its cutoff");
    Rig fade(source,1);t=std::make_shared<SliceTable>(*fade.instance.sliceTable());t->slices[0].fadeInMs=100;fade.instance.setSliceTable(t);const auto onset=render(fade.instance,10000,{noteOn(0,48)});
    check(rms(onset,0,1000)<rms(off,0,1000)*.2 && rms(onset,6000,9000)>rms(off,6000,9000)*.9,"fade-in preserves the later slice level");
    Rig bend(source,1);bend.instance.setParameter(p::indexOf(Param::BendRange),12);PluginEvent wheel;wheel.kind=PluginEvent::Kind::MidiController;wheel.channel=0;wheel.paramIndex=129;wheel.value=1;wheel.frameOffset=512;
    const auto bent=render(bend.instance,14000,{noteOn(0,48),wheel});check(amplitude(bent,2000,4800)>.3,"pitch-bend range controls the smoothed channel bend");
    Rig groups(source,3);t=std::make_shared<SliceTable>(*groups.instance.sliceTable());t->slices[0].chokeGroup=t->slices[1].chokeGroup=1;t->slices[2].chokeGroup=2;groups.instance.setSliceTable(t);groups.instance.setParameter(p::indexOf(Param::ChokeMode),2);
    const auto choked=render(groups.instance,10000,{noteOn(0,48),noteOn(512,49),noteOff(1024,49)});
    check(rms(choked,7000,10000)<1e-8,"choke by group cuts only the previous matching group");
    const auto separate=render(groups.instance,10000,{noteOn(0,48),noteOn(512,50),noteOff(1024,50)});
    check(rms(separate,7000,10000)>.1,"a different choke group does not cut the held slice");

    Rig smooth(makeSample(48000,[](auto){return .25f;}),1); Block transition(512); const auto on=noteOn(0,48);
    transition.run(smooth.instance,512,{&on,1}); smooth.instance.setParameter(p::indexOf(Param::Volume),0); transition.run(smooth.instance,512);
    check(transition.outL.front()>.24f && transition.outL.back()<.035f,"live volume changes settle smoothly instead of stepping at the block boundary");
    smooth.instance.setParameter(p::indexOf(Param::Volume),1); transition.run(smooth.instance,512); transition.run(smooth.instance,512);
    t=std::make_shared<SliceTable>(*smooth.instance.sliceTable()); t->slices[0].filter=1; t->slices[0].cutoff=0; smooth.instance.setSliceTable(t); transition.run(smooth.instance,512);
    check(transition.outL.front()>.24f && std::all_of(transition.outL.begin(),transition.outL.end(),[](float v){return std::isfinite(v);}),"enabling a live filter crossfades from the prior sound");

    const auto ramp=makeSample(128,[](auto i){return .8f-float(i)*1.6f/127;}); Rig abrupt(ramp,1),crossfaded(ramp,1);
    t=std::make_shared<SliceTable>(*abrupt.instance.sliceTable());t->slices[0].loopMode=1;abrupt.instance.setSliceTable(t);
    t=std::make_shared<SliceTable>(*t);t->slices[0].crossfadeMs=.67f;crossfaded.instance.setSliceTable(t);
    const auto hard=render(abrupt.instance,2048,{noteOn(0,48)}),soft=render(crossfaded.instance,2048,{noteOn(0,48)});
    const auto largestJump=[](const auto& data){double peak=0;for(std::size_t i=513;i<data.size();++i)peak=std::max(peak,std::abs(double(data[i]-data[i-1])));return peak;};
    check(largestJump(soft)<largestJump(hard)*.1,"forward-loop crossfade removes the wrap discontinuity");

    Rig mono(makeSample(48000,[](auto){return .05f;}),1),poly(makeSample(48000,[](auto){return .05f;}),1);
    mono.instance.setParameter(p::indexOf(Param::Polyphony),1);poly.instance.setParameter(p::indexOf(Param::Polyphony),32);
    std::vector<PluginEvent> chord(32,noteOn(0,48));const auto single=render(mono.instance,512,chord),many=render(poly.instance,512,chord);
    check(std::abs(single[400]-.05f)<1e-6 && std::abs(many[400]-1.6f)<1e-5,"polyphony enforces one voice and permits all 32 voices");
    Rig invalid(makeSample(1024,[](auto){return std::numeric_limits<float>::quiet_NaN();}),1);
    const auto clean=render(invalid.instance,512,{noteOn(0,48)});check(std::all_of(clean.begin(),clean.end(),[](float v){return v==0;}),"non-finite source PCM cannot poison the voice or filter");
}

void testSlicerTools() {
    Rig rig(tone(48000,440,.25f),4); auto original=*rig.instance.sliceTable(); original.slices[1].locked=true;
    slicing::RandomSettings options; options.seed=123; options.pitch=options.gain=options.pan=options.reverse=options.filter=options.keys=true;
    auto a=original,b=original; slicing::randomize(a,{},options); slicing::randomize(b,{},options);
    bool same=true; for(std::uint32_t i=0;i<a.count;++i) same &= a.slices[i]==b.slices[i];
    check(same && a.slices[1]==original.slices[1],"randomization is seeded and excludes locked slices");
    std::array<bool,128> keys{}; bool unique=true; for(std::uint32_t i=0;i<a.count;++i){unique &= !keys[a.slices[i].key];keys[a.slices[i].key]=true;}
    check(unique,"MIDI shuffle preserves unique assignments");
    options.seed=124; slicing::randomize(b,{},options); check(a.slices[0]!=b.slices[0],"another randomization seed changes sound settings");
    auto normalized=original; const std::uint32_t selected=normalized.slices[0].id;
    check(slicing::normalize(*rig.audio,normalized,{&selected,1}) && std::abs(normalized.slices[0].normalization*.25-std::pow(10.0,-1.0/20))<1e-5 && normalized.slices[1].normalization==1,"normalization is non-destructive and selection-scoped");
    auto silent=original; silent.slices[0].normalization=2; slicing::normalize(*makeSample(48000,[](auto){return 0.0f;}),silent,{});
    check(silent.slices[0].normalization==2,"normalizing silence leaves its coefficient unchanged");
    const auto untouched=normalized; check(!slicing::normalize(*rig.audio,normalized,{},[]{return false;}) && normalized.slices[0]==untouched.slices[0],"cancelled normalization preserves its input");
    auto state=rig.instance.captureState(); auto table=std::make_shared<SliceTable>(original); table->slices[1].flags |= p::kSliceMuted; state.table=table; state.analysis.sourceBpm=120;
    const auto phrase=slicing::midiPhrase(state,slicing::PhraseOrder::Source);
    check(phrase.notes.size()==3 && phrase.notes[1].startBeats==1 && phrase.lengthBeats==2,"MIDI duration uses source BPM and muted slices leave pauses");
    const auto reverse=slicing::midiPhrase(state,slicing::PhraseOrder::Reverse);
    check(reverse.notes.front().pitch==original.slices[3].key,"reverse MIDI order starts with the last source slice");
    const auto shuffled=slicing::midiPhrase(state,slicing::PhraseOrder::Shuffle,100),repeat=slicing::midiPhrase(state,slicing::PhraseOrder::Shuffle,100);
    check(shuffled.notes[0].pitch==repeat.notes[0].pitch && shuffled.notes[1].startBeats==repeat.notes[1].startBeats,"MIDI shuffle seed is repeatable");
    std::vector<std::uint8_t> bytes;std::string error;daw::midifile::encode(phrase,bytes,error);daw::midifile::File parsed;
    check(daw::midifile::parseBytes(bytes.data(),bytes.size(),parsed,error) && std::abs(parsed.lengthBeats-2)<1e-6,"exported MIDI round-trip preserves timing including trailing silence");
}
void testPortableAndController() {
    const auto root=fs::temp_directory_path()/fs::path("vlt-slicer-"+daw::newUuid());fs::create_directories(root);
    const auto source=root/"source.wav", preset=root/"portable.vltslicer", cache=root/"cache";
    const auto audio=tone(48000,440,.25f); const float* channels[]{audio->channel(0),audio->channel(1)};
    audio::platform::AudioFileWriter writer;check(writer.open(source.string(),48000,2).isOk() && writer.write(channels,48000).isOk() && writer.close().isOk(),"portable fixture WAV written");
    daw::EngineController c;check(c.initialize(48000,512,false).isOk(),"slicer controller initialized");
    const auto descriptor=c.pluginManager().find(daw::plugins::Format::Internal,"daw.slicer"); if(!descriptor){check(false,"slicer descriptor");return;}
    const auto track=c.addTrack(daw::TrackKind::Instrument,"Slicer");c.setTrackInstrumentPlugin(track,*descriptor);
    const auto slot=c.project().findTrack(track)->instrument.id;auto* instance=c.slicerInstance(track,slot);
    check(c.loadSlicerSample(track,slot,source.string()),"controller loads source with project BPM");
    auto table=std::make_shared<SliceTable>(evenTable(48000,4,48)); table->slices[0].gain=.5;table->slices[0].normalization=2;table->slices[1].loopMode=1;table->slices[1].crossfadeMs=3;
    c.publishSlicerTable(track,slot,table);auto original=instance->captureState();const auto depth=c.undoDepth();
    check(c.beginSlicerEdit(track,slot),"controller begins a boundary gesture");
    auto edit=std::make_shared<SliceTable>(*table);slicing::moveBoundary(*edit,1,11000);c.updateSlicerEdit(edit,original.analysis);
    edit=std::make_shared<SliceTable>(*edit);slicing::moveBoundary(*edit,1,10000);c.updateSlicerEdit(edit,original.analysis);c.commitSlicerEdit("Drag");
    check(c.undoDepth()==depth+1,"many drag updates produce one controller undo");c.undo();check(instance->sliceTable()->slices[0].end==12000,"undo restores table boundaries");c.redo();
    c.beginSlicerEdit(track,slot);edit=std::make_shared<SliceTable>(*edit);slicing::moveBoundary(*edit,1,9000);c.updateSlicerEdit(edit,original.analysis);c.cancelSlicerEdit();check(instance->sliceTable()->slices[0].end==10000,"cancel restores full prior table");
    const auto beforeClear=instance->captureState();c.clearSlicerSample(track,slot);check(!instance->rawSample(),"clear unloads source");c.undo();
    check(instance->rawSample()==beforeClear.audio && instance->sliceTable()==beforeClear.table && instance->analysisSettings()==beforeClear.analysis,"undo clear restores the complete instrument snapshot");
    std::string error;check(slicing::savePreset(preset.string(),beforeClear,error),"single-file CBOR preset embeds audio");
    const auto package=root/"song.vlt";check(c.saveProject(package.string()).isOk(),"project packaging includes Slicer state and source");
    const auto recovery=c.captureRecoverySnapshot();bool captured=false;
    for(const auto& chunk:recovery.pluginStates){auto j=nlohmann::json::parse(chunk.bytes,nullptr,false);captured |= j.is_object() && j.contains("slices");}
    check(captured,"recovery captures Slicer v2 state");
    {
        daw::recovery::RecoveryJournal journal;check(journal.start((root/"journal").string(),"slicer-test"),"Slicer recovery journal starts");
        journal.requestWrite(recovery);journal.flush();daw::ProjectModel recoveredDocument;
        const auto session=fs::path(journal.sessionDir());daw::EngineController recovered;recovered.initialize(48000,512,false);
        const auto documentResult=daw::ProjectSerializer::loadDocument(recoveredDocument,(session/"project.json").string(),"");
        check(documentResult.isOk() && recovered.restoreRecoveryProject(std::move(recoveredDocument),session.string(),package.string()).isOk(),"recovery journal reopens a Slicer instrument");
        auto* recoveredSlicer=recovered.slicerInstance(track,slot);
        check(recoveredSlicer && recoveredSlicer->rawSample() && recoveredSlicer->sliceTable()->slices[0].end==10000 && recoveredSlicer->sliceTable()->slices[0].normalization==2 && recoveredSlicer->analysisSettings()==beforeClear.analysis,"recovery restores source, boundaries, analysis and slice processing");
        recovered.shutdown();journal.stop();
    }
    fs::remove(source);p::ControlState imported;check(slicing::loadPreset(preset.string(),cache.string(),imported,error),"portable preset works after original source deletion");
    check(imported.audio && fs::exists(imported.path) && imported.table->slices[0].gain==.5f,"preset decodes into durable managed media cache");
    const auto oldState=imported;const auto damaged=root/"broken.vltslicer";{std::ofstream f(damaged,std::ios::binary);f<<"broken";}
    check(!slicing::loadPreset(damaged.string(),cache.string(),imported,error) && imported.audio==oldState.audio && !slicing::loadPreset(preset.string(),cache.string(),imported,error,[]{return false;}),"damaged and cancelled presets preserve prior state");
    daw::EngineController reopened;reopened.initialize(48000,512,false);check(reopened.openProject(package.string()).isOk(),"portable project opens after source deletion");
    auto* restored=reopened.slicerInstance(track,slot);check(restored && restored->rawSample() && restored->sliceTable()->count==4 && restored->sliceTable()->slices[0].end==10000,"project restores audio, boundaries and sound settings");
    const auto raw=root/"raw.wav",processed=root/"processed.wav",loop=root/"loop.wav";
    auto processedState=oldState;processedState.parameters[p::indexOf(Param::Volume)]=.5;
    check(slicing::renderWav(raw.string(),oldState,oldState.table->slices[0].id,false,error) && slicing::renderWav(processed.string(),processedState,oldState.table->slices[0].id,true,error) && slicing::renderWav(loop.string(),oldState,oldState.table->slices[1].id,true,error),"raw, processed and finite loop WAV export");
    audio::platform::DecodedAudio rawAudio,wetAudio,loopAudio;audio::platform::decodeAudioFile(raw.string(),rawAudio);audio::platform::decodeAudioFile(processed.string(),wetAudio);audio::platform::decodeAudioFile(loop.string(),loopAudio);
    check(rawAudio.frames==10000 && rawAudio.sampleRate==48000 && std::abs(rawAudio.interleaved[500]-oldState.audio->readSample(0,250))<1e-7,"raw WAV contains the exact source frames at its rate");
    check(wetAudio.frames==rawAudio.frames && loopAudio.frames<20000 && wetAudio.sampleRate==48000 && std::abs(wetAudio.interleaved[500]-rawAudio.interleaved[500]*.5)<1e-6,"processed WAV applies sound settings and only loops add an envelope release");
    c.applySlicerState(track,slot,oldState,"Cached source");
    const auto tempo=c.tempo();const auto midi=c.addMidiClip(track,0,1);daw::midifile::File phrase=slicing::midiPhrase(oldState,slicing::PhraseOrder::Source);c.replaceMidiClipFromFile(track,midi,phrase);
    check(c.tempo()==tempo && !c.project().findTrack(track)->clips[0].notes.empty(),"MIDI clip uses the Slicer channel without changing tempo");
    daw::rendering::Spec spec;spec.outputDir=root.string();spec.baseName="offline-slicer";spec.range=daw::rendering::Range::Custom;spec.customEndSeconds=1;spec.tail=daw::rendering::Tail::None;
    daw::rendering::Report report;const auto renderResult=c.renderProject(spec,{},report);check(renderResult.isOk() && fs::exists(root/"offline-slicer.wav"),"local offline-render clone restores the full Slicer state");
    const auto pattern=c.addPattern("Slicer pattern");const auto child=c.addPatternInstrument(pattern,*descriptor);const auto childSlot=c.project().findTrack(child)->instrument.id;
    c.applySlicerState(child,childSlot,oldState,"Slicer pattern fixture");
    const auto patternClip=c.project().findTrack(pattern)->clips.empty()?c.addPatternClip(pattern,0,1):c.project().findTrack(pattern)->clips.front().id;std::string entry;
    check(c.saveClipToLibrary({pattern,patternClip},entry).isOk(),"pattern clip library captures Slicer source and opaque state");
    daw::EngineController::ClipAddress restoredClip;check(c.restoreLibraryClip(entry,{},2,restoredClip).isOk(),"library restoration reinstantiates Slicer channels");
    const auto dropped=c.addTrack(daw::TrackKind::Audio,"WAV drop"); const auto importedClip=c.importAudio(processed.string(),dropped,0);
    check(!importedClip.empty(),"durable exported WAV imports through the arrangement file path");
    const auto dragProject=root/"drag.vlt";c.applySlicerState(track,slot,oldState,"Cached source");check(c.saveProject(dragProject.string()).isOk(),"project saves permanent exported WAV and cached preset media");
    fs::remove(processed);fs::remove(oldState.path); reopened.shutdown();reopened.initialize(48000,512,false);
    check(reopened.openProject(dragProject.string()).isOk(),"project reopens after the dragged WAV cache file is removed");
    bool hasExport=false;for(const auto& row:reopened.project().tracks)for(const auto& clip:row.clips)hasExport |= clip.kind==daw::ClipKind::Audio && fs::exists(clip.filePath);
    check(hasExport,"dragged WAV survives inside project Content");
    check(reopened.restoreLibraryClip(entry,{},4,restoredClip).isOk(),"packaged Slicer library state restores after the external cache is removed");
    reopened.shutdown();c.shutdown();std::error_code ignored;fs::remove_all(root,ignored);
}

} // namespace

int main() {
    testParameters();
    testScales();
    testTable();
    testGrid();
    testTransients();
    testFallback();
    testRandom();
    testLayout();
    testFactory();
    testState();
    testSampleAndTable();
    testPlayback();
    testSliceEffects();
    testEditingV2();
    testNewState();
    testAdvancedDSP();
    testRealtimeAndProcessing();
    testSlicerTools();
    testPortableAndController();

    if (failures != 0) std::printf("FAILURES PRESENT: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
