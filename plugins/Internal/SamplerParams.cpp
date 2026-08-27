#include "Internal/SamplerParams.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace daw::plugins::sampler {
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

/// The SMP page. Times are seconds, fractions are 0…1, and anything that reads
/// as a switch is a stepped 0…1.
constexpr std::array<Row, kMainCount> kMain{{
    {Param::Volume,     "vol",        "Volume",          "",     0.0,  1.0,  0.55, false},
    {Param::Pan,        "pan",        "Pan",             "",    -1.0,  1.0,  0.0,  false},
    {Param::Pitch,      "pitch",      "Pitch",           "",    -1.0,  1.0,  0.0,  false},
    {Param::PitchRange, "pitchrange", "Pitch Range",     "st",   0.0, 24.0,  2.0,  true},
    {Param::ModX,       "modx",       "Mod X (Cutoff)",  "",     0.0,  1.0,  1.0,  false},
    {Param::ModY,       "mody",       "Mod Y (Res)",     "",     0.0,  1.0,  0.0,  false},
    {Param::RootNote,   "rootnote",   "Root Note",       "",     0.0, 127.0, 60.0, true},

    {Param::AmpEnvOn,          "amp.on",      "Amp Env",           "",   0.0,  1.0, 0.0,  true},
    {Param::AmpDelay,          "amp.delay",   "Amp Delay",         "s",  0.0, 10.0, 0.0,  false},
    {Param::AmpAttack,         "amp.att",     "Amp Attack",        "s",  0.0, 10.0, 0.002, false},
    {Param::AmpHold,           "amp.hold",    "Amp Hold",          "s",  0.0, 10.0, 0.0,  false},
    {Param::AmpDecay,          "amp.dec",     "Amp Decay",         "s",  0.0, 10.0, 0.5,  false},
    {Param::AmpSustain,        "amp.sus",     "Amp Sustain",       "",   0.0,  1.0, 1.0,  false},
    {Param::AmpRelease,        "amp.rel",     "Amp Release",       "s",  0.0, 10.0, 0.1,  false},
    {Param::AmpAttackTension,  "amp.atttens", "Amp Attack Tension","",  -1.0,  1.0, 0.0,  false},
    {Param::AmpDecayTension,   "amp.dectens", "Amp Decay Tension", "",  -1.0,  1.0, 0.0,  false},
    {Param::AmpReleaseTension, "amp.reltens", "Amp Release Tension","", -1.0,  1.0, 0.0,  false},

    {Param::StartOffset, "startoffset", "Start Offset", "", 0.0, 1.0, 0.0, false},
    {Param::FadeIn,      "fadein",      "Fade In",      "", 0.0, 1.0, 0.0, false},
    {Param::FadeOut,     "fadeout",     "Fade Out",     "", 0.0, 1.0, 0.0, false},
    {Param::LoopMode,    "loop.mode",   "Loop Mode",    "", 0.0, 2.0, 0.0, true},
    {Param::LoopStart,   "loop.start",  "Loop Start",   "", 0.0, 1.0, 0.0, false},
    {Param::LoopEnd,     "loop.end",    "Loop End",     "", 0.0, 1.0, 1.0, false},
    {Param::KeepOnDisk,  "keepondisk",  "Keep On Disk", "", 0.0, 1.0, 0.0, true},

    {Param::StretchMode,  "stretch.mode",  "Stretch Mode",  "",   0.0,  4.0, 0.0, true},
    {Param::StretchTime,  "stretch.time",  "Stretch Time",  "x",  0.25, 4.0, 1.0, false},
    {Param::StretchPitch, "stretch.pitch", "Stretch Pitch", "st", -24.0, 24.0, 0.0, false},

    {Param::PreBoost,           "pre.boost",     "Boost",          "",  0.0, 1.0, 0.0, false},
    {Param::PreEqLow,           "pre.eq.low",    "EQ Low",         "",  -1.0, 1.0, 0.0, false},
    {Param::PreEqMid,           "pre.eq.mid",    "EQ Mid",         "",  -1.0, 1.0, 0.0, false},
    {Param::PreEqHigh,          "pre.eq.high",   "EQ High",        "",  -1.0, 1.0, 0.0, false},
    {Param::PreRingMix,         "pre.rm.mix",    "RM Mix",         "",  0.0, 1.0, 0.0, false},
    {Param::PreRingFreq,        "pre.rm.freq",   "RM Freq",        "Hz", 0.0, 1.0, 0.5, false},
    {Param::PreCut,             "pre.cut",       "Pre Cutoff",     "",  0.0, 1.0, 1.0, false},
    {Param::PreRes,             "pre.res",       "Pre Resonance",  "",  0.0, 1.0, 0.0, false},
    {Param::PreReverbType,      "pre.rev.type",  "Reverb Type",    "",  0.0, 1.0, 0.0, true},
    {Param::PreReverb,          "pre.rev",       "Reverb",         "",  0.0, 1.0, 0.0, false},
    {Param::PreStereoDelay,     "pre.delay",     "Stereo Delay",   "",  0.0, 1.0, 0.0, false},
    {Param::PrePogo,            "pre.pogo",      "Pogo",           "",  -1.0, 1.0, 0.0, false},
    {Param::PreRemoveDc,        "pre.dc",        "Remove DC",      "",  0.0, 1.0, 0.0, true},
    {Param::PreReversePolarity, "pre.polarity",  "Reverse Polarity","", 0.0, 1.0, 0.0, true},
    {Param::PreNormalize,       "pre.normalize", "Normalize",      "",  0.0, 1.0, 0.0, true},
    {Param::PreFadeStereo,      "pre.fadestereo","Fade Stereo",    "",  0.0, 1.0, 0.0, true},
    {Param::PreReverse,         "pre.reverse",   "Reverse",        "",  0.0, 1.0, 0.0, true},
    {Param::PreSwapStereo,      "pre.swap",      "Swap Stereo",    "",  0.0, 1.0, 0.0, true},
    {Param::CutItself,          "cutitself",     "Cut Itself",     "",  0.0, 1.0, 0.0, true},
    {Param::EndOffset,          "endoffset",     "End Offset",     "",  0.0, 1.0, 1.0, false},
    {Param::Formant,            "formant",       "Formant",       "st", -12.0, 12.0, 0.0, false},
}};

/// The INS page, written once and instantiated per target.
struct ModRow {
    ModParam parameter;
    const char* id;
    const char* name;
    const char* unit;
    double minimum;
    double maximum;
    double defaultValue;
    bool stepped;
};

constexpr std::array<ModRow, kModParamCount> kMod{{
    {ModParam::EnvOn,             "env.on",      "Env",             "",   0.0,  1.0, 0.0,  true},
    {ModParam::EnvAmount,         "env.amt",     "Env Amount",      "",  -1.0,  1.0, 0.0,  false},
    {ModParam::EnvDelay,          "env.delay",   "Env Delay",       "s",  0.0, 10.0, 0.0,  false},
    {ModParam::EnvAttack,         "env.att",     "Env Attack",      "s",  0.0, 10.0, 0.05, false},
    {ModParam::EnvHold,           "env.hold",    "Env Hold",        "s",  0.0, 10.0, 0.0,  false},
    {ModParam::EnvDecay,          "env.dec",     "Env Decay",       "s",  0.0, 10.0, 0.5,  false},
    {ModParam::EnvSustain,        "env.sus",     "Env Sustain",     "",   0.0,  1.0, 1.0,  false},
    {ModParam::EnvRelease,        "env.rel",     "Env Release",     "s",  0.0, 10.0, 0.2,  false},
    {ModParam::EnvAttackTension,  "env.atttens", "Env Att Tension", "",  -1.0,  1.0, 0.0,  false},
    {ModParam::EnvDecayTension,   "env.dectens", "Env Dec Tension", "",  -1.0,  1.0, 0.0,  false},
    {ModParam::EnvReleaseTension, "env.reltens", "Env Rel Tension", "",  -1.0,  1.0, 0.0,  false},
    {ModParam::LfoAmount,         "lfo.amt",     "LFO Amount",      "",  -1.0,  1.0, 0.0,  false},
    {ModParam::LfoSpeed,          "lfo.speed",   "LFO Speed",       "",   0.01, 40.0, 2.0, false},
    {ModParam::LfoDelay,          "lfo.delay",   "LFO Delay",       "s",  0.0, 10.0, 0.0,  false},
    {ModParam::LfoAttack,         "lfo.att",     "LFO Attack",      "s",  0.0, 10.0, 0.0,  false},
    {ModParam::LfoTempo,          "lfo.tempo",   "LFO Tempo Sync",  "",   0.0,  1.0, 0.0,  true},
    {ModParam::LfoGlobal,         "lfo.global",  "LFO Global",      "",   0.0,  1.0, 0.0,  true},
    {ModParam::LfoShape,          "lfo.shape",   "LFO Shape",       "",   0.0,  3.0, 0.0,  true},
}};

constexpr std::array<const char*, kModTargetCount> kTargetId{
    {"pan", "vol", "cut", "res", "pitch"}};
constexpr std::array<const char*, kModTargetCount> kTargetName{
    {"Pan", "Volume", "Cutoff", "Resonance", "Pitch"}};

/// The precomputed block is contiguous in the enum, which is what lets one
/// range check answer "does changing this re-render the sample".
constexpr bool inPrecomputedRange(std::uint32_t index) noexcept {
    return index >= std::uint32_t(Param::PreBoost) &&
           index <= std::uint32_t(Param::PreSwapStereo);
}

const std::vector<ParameterInfo>& table() {
    static const std::vector<ParameterInfo> built = [] {
        std::vector<ParameterInfo> out;
        out.reserve(kParameterCount);
        for (std::uint32_t i = 0; i < kMainCount; ++i) {
            const Row& row = kMain[i];
            // The row's position *is* its index; a row written out of order
            // would silently give a knob another knob's automation.
            ParameterInfo info;
            info.index = i;
            info.id = row.id;
            info.name = row.name;
            info.unit = row.unit;
            info.minValue = row.minimum;
            info.maxValue = row.maximum;
            info.defaultValue = row.defaultValue;
            info.isStepped = row.stepped;
            info.isAutomatable = !inPrecomputedRange(i);
            out.push_back(std::move(info));
        }
        for (std::uint32_t t = 0; t < kModTargetCount; ++t) {
            for (std::uint32_t p = 0; p < kModParamCount; ++p) {
                const ModRow& row = kMod[p];
                ParameterInfo info;
                info.index = kMainCount + t * kModParamCount + p;
                info.id = std::string("ins.") + kTargetId[t] + "." + row.id;
                info.name = std::string(kTargetName[t]) + " " + row.name;
                info.unit = row.unit;
                info.minValue = row.minimum;
                info.maxValue = row.maximum;
                info.defaultValue = row.defaultValue;
                info.isStepped = row.stepped;
                out.push_back(std::move(info));
            }
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

std::string noteName(int midi) {
    static const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};
    if (midi < 0 || midi > 127) return "—";
    // Middle C is C5 here (FL Studio's numbering), matching the piano roll's
    // own labelling — see daw::miditools::pitchName.
    return std::string(kNames[midi % 12]) + std::to_string(midi / 12);
}

} // namespace

std::span<const ParameterInfo> parameterTable() noexcept { return table(); }

bool isPrecomputed(std::uint32_t index) noexcept { return inPrecomputedRange(index); }

std::string parameterText(std::uint32_t index, double value) {
    if (index >= kParameterCount) return {};

    switch (Param(index)) {
        case Param::Volume: {
            if (value <= 0.0001) return "-inf dB";
            return decimals(20.0 * std::log10(value), 1, " dB");
        }
        case Param::Pan: {
            if (std::abs(value) < 0.005) return "C";
            const double percent = std::abs(value) * 100.0;
            return decimals(percent, 0, value < 0.0 ? "% L" : "% R");
        }
        case Param::Pitch:
            return decimals(value * 100.0, 0, "%");
        case Param::PitchRange:
        case Param::StretchPitch:
            return decimals(value, 0, " st");
        case Param::RootNote:
            return noteName(int(std::lround(value)));
        case Param::LoopMode: {
            const int mode = int(std::lround(value));
            return mode == 1 ? "Forward" : mode == 2 ? "Ping-Pong" : "Off";
        }
        case Param::StretchMode:
            switch (int(std::lround(value))) {
                case 1: return "Drums";
                case 2: return "Loop";
                case 3: return "Vocal";
                case 4: return "Complex";
                default: return "Resample";
            }
        case Param::Formant:
            return decimals(value, 1, " st");
        case Param::PreReverbType:
            return int(std::lround(value)) == 1 ? "Hall" : "Room";
        case Param::PreRingFreq:
            // The knob is 0…1; what it means is a frequency, so say so.
            return decimals(20.0 * std::pow(400.0, value), 0, " Hz");
        case Param::ModX:
        case Param::PreCut:
            return decimals(20.0 * std::pow(1000.0, value), 0, " Hz");
        default:
            break;
    }

    const ParameterInfo& info = table()[index];
    if (info.isStepped && info.minValue == 0.0 && info.maxValue == 1.0) {
        return value >= 0.5 ? "On" : "Off";
    }
    if (index >= kMainCount) {
        const std::uint32_t modParam = (index - kMainCount) % kModParamCount;
        if (ModParam(modParam) == ModParam::LfoShape) {
            switch (int(std::lround(value))) {
                case 1: return "Triangle";
                case 2: return "Square";
                case 3: return "Saw";
                default: return "Sine";
            }
        }
        if (ModParam(modParam) == ModParam::LfoSpeed) {
            // Reads as a rate either way; whether it is hertz or cycles per
            // beat is the Tempo switch's business, and the panel says which.
            return decimals(value, 2);
        }
    }
    if (info.unit == "s") {
        return value < 1.0 ? decimals(value * 1000.0, 0, " ms") : decimals(value, 2, " s");
    }
    if (info.unit == "x") return decimals(value, 2, "x");
    if (info.minValue >= 0.0 && info.maxValue <= 1.0) {
        return decimals(value * 100.0, 0, "%");
    }
    return decimals(value, 2);
}

} // namespace daw::plugins::sampler
