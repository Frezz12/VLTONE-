#pragma once

#include "Host/PluginTypes.hpp"

#include <cstdint>
#include <span>
#include <string>

/// The built-in sampler's parameter surface.
///
/// One table, built once, is the single definition of every knob: the host's
/// generic panel, the project file's parameter fallback, the automation lanes
/// and the sampler's own editor all read it, so a parameter cannot exist in one
/// place and not another. Ids are stable strings — that is what a project and
/// an automation lane store, and renumbering the table must never move an
/// existing knob's meaning.
namespace daw::plugins::sampler {

/// The SMP page: everything that is not part of the INS modulation matrix.
enum class Param : std::uint32_t {
    // ── Main ──
    Volume,
    Pan,
    Pitch,          ///< −1 … 1, scaled by PitchRange
    PitchRange,     ///< semitones the Pitch knob spans
    ModX,           ///< filter cutoff, the default MOD X target
    ModY,           ///< filter resonance, the default MOD Y target
    RootNote,       ///< the key that plays the sample untransposed

    // ── Volume envelope (the orange LED gates it) ──
    AmpEnvOn,
    AmpDelay,
    AmpAttack,
    AmpHold,
    AmpDecay,
    AmpSustain,
    AmpRelease,
    AmpAttackTension,
    AmpDecayTension,
    AmpReleaseTension,

    // ── Playback ──
    StartOffset,    ///< fraction of the sample skipped before the first sample
    FadeIn,
    FadeOut,
    LoopMode,       ///< 0 off, 1 forward, 2 ping-pong
    LoopStart,
    LoopEnd,
    KeepOnDisk,     ///< skips the precomputed stage, exactly as FL documents it

    // ── Time stretching ──
    StretchMode,    ///< 0 resample (pitch follows rate), 1 stretch (independent)
    StretchTime,    ///< playback-length multiplier in stretch mode
    StretchPitch,   ///< semitones, independent of time in stretch mode

    // ── Precomputed effects: applied to the sample once, on load ──
    PreBoost,
    PreEqLow,
    PreEqMid,
    PreEqHigh,
    PreRingMix,
    PreRingFreq,
    PreCut,
    PreRes,
    PreReverbType,  ///< 0 room, 1 hall
    PreReverb,
    PreStereoDelay,
    PrePogo,
    PreRemoveDc,
    PreReversePolarity,
    PreNormalize,
    PreFadeStereo,
    PreReverse,
    PreSwapStereo,

    // ── Voice behaviour ──
    // Appended after every existing SMP parameter so their numeric indices
    // stay stable for live automation/event streams. State itself is stored
    // by string id, so older chunks simply leave this at its default.
    CutItself,       ///< every new note immediately chokes older voices
    EndOffset,       ///< normalized end marker, after Start
    Formant,         ///< timbre shift for formant-capable stretch modes

    kMainCount
};

/// What an INS-page envelope/LFO pair drives.
enum class ModTarget : std::uint32_t { Pan, Volume, Cutoff, Resonance, Pitch, kCount };

/// The controls every INS target carries. The envelope half mirrors the volume
/// envelope on the SMP page — same stages, same tensions — because that is what
/// makes one shape editor serve both pages.
enum class ModParam : std::uint32_t {
    EnvOn,
    EnvAmount,
    EnvDelay,
    EnvAttack,
    EnvHold,
    EnvDecay,
    EnvSustain,
    EnvRelease,
    EnvAttackTension,
    EnvDecayTension,
    EnvReleaseTension,
    LfoAmount,
    LfoSpeed,
    LfoDelay,
    LfoAttack,
    LfoTempo,       ///< speed in beats instead of hertz
    LfoGlobal,      ///< free-running instead of retriggered per note
    LfoShape,       ///< 0 sine, 1 triangle, 2 square, 3 saw
    kCount
};

inline constexpr std::uint32_t kMainCount = std::uint32_t(Param::kMainCount);
inline constexpr std::uint32_t kModTargetCount = std::uint32_t(ModTarget::kCount);
inline constexpr std::uint32_t kModParamCount = std::uint32_t(ModParam::kCount);
inline constexpr std::uint32_t kParameterCount =
    kMainCount + kModTargetCount * kModParamCount;

/// Flat index of one INS-page control. The matrix is laid out target-major, so
/// a target's controls are contiguous and the voice can read them as a run.
constexpr std::uint32_t indexOf(ModTarget target, ModParam parameter) noexcept {
    return kMainCount + std::uint32_t(target) * kModParamCount +
           std::uint32_t(parameter);
}
constexpr std::uint32_t indexOf(Param parameter) noexcept {
    return std::uint32_t(parameter);
}

/// Every parameter, in index order. Built once on first use.
std::span<const ParameterInfo> parameterTable() noexcept;

/// True for the knobs that are baked into the sample rather than applied while
/// it plays. They are not automatable — changing one re-renders the sample on
/// the control thread, which an automation curve cannot ask for per block.
bool isPrecomputed(std::uint32_t index) noexcept;

/// Human-readable value, used by the host's readouts.
std::string parameterText(std::uint32_t index, double plainValue);

} // namespace daw::plugins::sampler
