#include "Internal/SlicerParams.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace daw::plugins::slicer {
namespace {

/// One row of the static description the table is built from. Kept separate
/// from `ParameterInfo` so the table can be written as a plain initialiser
/// list; the index is filled in when the table is materialised, which is the
/// one field that must never disagree with the row's position.
struct Row {
    Param parameter;
    const char* id;
    const char* name;
    const char* unit;
    double minimum;
    double maximum;
    double defaultValue;
    bool stepped;
};

/// Times are seconds, fractions are 0…1, and anything that reads as a switch is
/// a stepped 0…1. Nothing here bakes audio, so every knob is automatable.
constexpr std::array<Row, kParameterCount> kRows{{
    {Param::Volume,       "vol",     "Volume",        "",     0.0,   1.0,   1.0, false},
    {Param::Drive,        "drive",   "Drive",         "",     0.0,   1.0,   0.0, false},
    {Param::Attack,       "att",     "Attack",        "",     0.0,   1.0,   0.0, false},
    {Param::Release,      "rel",     "Release",       "",     0.0,   5.0,   0.05,false},
    {Param::Gate,         "gate",    "Gate",          "",     0.02,  1.0,   1.0, false},
    {Param::KeyTrack,     "keytrack","Key Track",     "",     0.0,   1.0,   0.0, false},
    {Param::RootNote,     "root",    "Root Note",     "",     0.0, 127.0,  48.0, true},
    {Param::ChokeMode,    "choke",   "Choke Mode",    "",     0.0,   2.0,   0.0, true},
    {Param::VelocityDepth,"veld",    "Velocity",      "",     0.0,   1.0,   1.0, false},
    {Param::Transpose,    "tune",    "Transpose",     "st", -48.0, 48.0, 0.0, true},
    {Param::FineTune,     "finepitch","Fine Tune",    "ct", -100.0,100.0,0.0, false},
    {Param::Pan,          "pan",     "Pan",           "", -1.0, 1.0, 0.0, false},
    {Param::Decay,        "dec",     "Decay",         "", 0.0, 5.0, 0.0, false},
    {Param::Sustain,      "sus",     "Sustain",       "", 0.0, 1.0, 1.0, false},
    {Param::PlayMode,     "playmode","Playback",      "", 0.0, 1.0, 0.0, true},
    {Param::Polyphony,    "poly",    "Polyphony",     "", 1.0, 32.0,32.0, true},
    {Param::BendRange,    "bend",    "Bend Range",    "st",0.0,48.0,2.0, true},
    {Param::CrushBits,    "bits",    "Bit Depth",     "bit",4.0,24.0,24.0, true},
    {Param::CrushRate,    "downsample","Downsample",  "", 1.0,64.0,1.0, true},
    {Param::CrushMix,     "crushmix","Crusher Mix",   "", 0.0,1.0,0.0, false},
}};

/// Semitone offsets from the root, one octave each. Ordering is harmonic — the
/// triads sit where a player expects them — rather than alphabetical.
constexpr std::array<std::uint8_t, 12> kChromatic{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<std::uint8_t, 7>  kMajor{0, 2, 4, 5, 7, 9, 11};
constexpr std::array<std::uint8_t, 7>  kMinor{0, 2, 3, 5, 7, 8, 10};
constexpr std::array<std::uint8_t, 7>  kHarmonicMinor{0, 2, 3, 5, 7, 8, 11};
constexpr std::array<std::uint8_t, 7>  kDorian{0, 2, 3, 5, 7, 9, 10};
constexpr std::array<std::uint8_t, 7>  kPhrygian{0, 1, 3, 5, 7, 8, 10};
constexpr std::array<std::uint8_t, 7>  kLydian{0, 2, 4, 6, 7, 9, 11};
constexpr std::array<std::uint8_t, 7>  kMixolydian{0, 2, 4, 5, 7, 9, 10};
constexpr std::array<std::uint8_t, 5>  kMajorPentatonic{0, 2, 4, 7, 9};
constexpr std::array<std::uint8_t, 5>  kMinorPentatonic{0, 3, 5, 7, 10};
constexpr std::array<std::uint8_t, 6>  kBlues{0, 3, 5, 6, 7, 10};
constexpr std::array<std::uint8_t, 6>  kWholeTone{0, 2, 4, 6, 8, 10};

/// The degree lists, address-stable, so `scaleDegrees` can hand out a span
/// without owning anything. The index *is* the scale id stored in a project.
struct ScaleEntry {
    const char* name;
    const std::uint8_t* degrees;
    std::uint8_t count;
};

constexpr std::array<ScaleEntry, kScaleCount> kScales{{
    {"Chromatic",        kChromatic.data(), std::size(kChromatic)},
    {"Major",            kMajor.data(), std::size(kMajor)},
    {"Natural Minor",    kMinor.data(), std::size(kMinor)},
    {"Harmonic Minor",   kHarmonicMinor.data(), std::size(kHarmonicMinor)},
    {"Dorian",           kDorian.data(), std::size(kDorian)},
    {"Phrygian",         kPhrygian.data(), std::size(kPhrygian)},
    {"Lydian",           kLydian.data(), std::size(kLydian)},
    {"Mixolydian",       kMixolydian.data(), std::size(kMixolydian)},
    {"Major Pentatonic", kMajorPentatonic.data(), std::size(kMajorPentatonic)},
    {"Minor Pentatonic", kMinorPentatonic.data(), std::size(kMinorPentatonic)},
    {"Blues",            kBlues.data(), std::size(kBlues)},
    {"Whole Tone",       kWholeTone.data(), std::size(kWholeTone)},
}};

const ScaleEntry& scaleEntry(int scale) noexcept {
    return kScales[std::clamp(scale, 0, kScaleCount - 1)];
}

std::span<const ParameterInfo> table() {
    static const std::vector<ParameterInfo> built = [] {
        std::vector<ParameterInfo> out;
        out.reserve(kParameterCount);
        for (const Row& row : kRows) {
            ParameterInfo info;
            info.index = indexOf(row.parameter);
            info.id = row.id;
            info.name = row.name;
            info.unit = row.unit;
            info.minValue = row.minimum;
            info.maxValue = row.maximum;
            info.defaultValue = row.defaultValue;
            info.isStepped = row.stepped;
            out.push_back(std::move(info));
        }
        return out;
    }();
    return built;
}

std::string decimals(double value, int places, const char* suffix = "") {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f%s", places, value, suffix);
    return buffer;
}

} // namespace

void SliceTable::rebuild() noexcept {
    keyToIndex.fill(-1);
    if (count > kMaxSlices) count = kMaxSlices;
    if (nextId == 0) nextId = 1;
    for (std::uint32_t i = 0; i < count; ++i)
        if (slices[i].id < UINT32_MAX) nextId = std::max(nextId, slices[i].id + 1);
    for (std::uint32_t i = 0; i < count; ++i) {
        bool duplicate = slices[i].id == 0;
        for (std::uint32_t j = 0; j < i; ++j) duplicate |= slices[j].id == slices[i].id;
        if (duplicate) {
            // A state may contain duplicate or exhausted ids. There are only
            // 128 slices, so a small unused id always exists.
            while (nextId == 0 || indexForId(nextId) >= 0) ++nextId;
            slices[i].id = nextId++;
        }
        const int key = slices[i].key;
        if (key >= 0 && key < 128 && keyToIndex[key] < 0)
            keyToIndex[key] = std::int16_t(i);
    }
}

int SliceTable::indexForId(std::uint32_t id) const noexcept {
    if (!id) return -1;
    for (std::uint32_t i = 0; i < count; ++i)
        if (slices[i].id == id) return int(i);
    return -1;
}

std::span<const Slice> SliceTable::forKeySpan(int key) const noexcept {
    if (key < 0 || key >= 128) return {};
    // Tables are built with one chop per key today, but a hand-edited project
    // can park two on one key, so the lookup reports the first and a scan is
    // only worth it in that case.
    const std::int16_t first = keyToIndex[key];
    if (first < 0) return {};
    std::uint32_t end = std::uint32_t(first) + 1;
    while (end < count && slices[end].key == key) ++end;
    return {&slices[std::uint32_t(first)], end - std::uint32_t(first)};
}

std::span<const std::uint8_t> scaleDegrees(int scale) noexcept {
    const ScaleEntry& entry = scaleEntry(scale);
    return {entry.degrees, entry.count};
}

const char* scaleName(int scale) noexcept { return scaleEntry(scale).name; }

int degreeToSemitone(int degree, int scale) noexcept {
    const std::span<const std::uint8_t> steps = scaleDegrees(scale);
    if (steps.empty()) return degree;
    // Division on a negative degree floors away from zero, which is what a
    // walk downward past the scale root needs.
    const int n = int(steps.size());
    const int octave = degree >= 0 ? degree / n : -((-degree + n - 1) / n);
    const int index = degree - octave * n;
    return 12 * octave + int(steps[std::size_t(index)]);
}

int degreeToKey(int degree, int root, int scale) noexcept {
    return std::clamp(root + degreeToSemitone(degree, scale), 0, 127);
}

std::string noteName(int midi) {
    static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};
    if (midi < 0 || midi > 127) return "—";
    // Middle C is C5 here (FL Studio's numbering), matching the piano roll's
    // own labelling — see daw::miditools::pitchName.
    return std::string(kNames[midi % 12]) + std::to_string(midi / 12);
}

std::span<const ParameterInfo> parameterTable() noexcept { return table(); }

std::string parameterText(std::uint32_t index, double value) {
    if (index >= kParameterCount) return {};
    switch (Param(index)) {
        case Param::Volume:
            if (value <= 0.0001) return "-inf dB";
            return decimals(20.0 * std::log10(value), 1, " dB");
        case Param::Drive:
        case Param::Gate:
        case Param::VelocityDepth:
        case Param::Sustain:
        case Param::CrushMix:
            return decimals(value * 100.0, 0, "%");
        case Param::Attack:
        case Param::Release:
        case Param::Decay:
            // Reads as a rate either way; under a second it is the fade length
            // the envelope actually uses, so say it in milliseconds.
            return value < 1.0 ? decimals(value * 1000.0, 0, " ms")
                               : decimals(value, 2, " s");
        case Param::KeyTrack:
            return value <= 0.005 ? "Off" : decimals(value * 100.0, 0, "%");
        case Param::RootNote:
            return noteName(int(std::lround(value)));
        case Param::ChokeMode:
            switch (int(std::lround(value))) {
                case 1: return "All Voices";
                case 2: return "By Group";
                default: return "Off";
            }
        case Param::PlayMode: return value >= 0.5 ? "One Shot" : "Gate";
        case Param::Transpose:
        case Param::BendRange: return decimals(value, 0, " st");
        case Param::FineTune: return decimals(value, 1, " ct");
        case Param::Pan: return decimals(value * 100.0, 0, "%");
        case Param::Polyphony: return decimals(value, 0, " voices");
        case Param::CrushBits: return decimals(value, 0, " bit");
        case Param::CrushRate: return decimals(value, 0, "x");
        default:
            break;
    }
    const ParameterInfo& info = table()[index];
    if (info.isStepped && info.minValue == 0.0 && info.maxValue == 1.0)
        return value >= 0.5 ? "On" : "Off";
    return decimals(value, 2);
}

} // namespace daw::plugins::slicer
