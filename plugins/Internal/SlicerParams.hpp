#pragma once

#include "Common/Types.hpp"
#include "Host/PluginTypes.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace daw::engine { class SampleBuffer; }

/// Shared Slicer data for playback, analysis and editing. Global sound controls
/// are automatable parameters; boundaries, MIDI layout and per-slice settings
/// belong to immutable tables edited through controller transactions.
namespace daw::plugins::slicer {

/// A chop table is read once per note and played until the next one, so the
/// ceiling is about what a keyboard can reach rather than about memory. 128 is
/// also what the host's own note polyphony budgets to.
inline constexpr std::uint32_t kMaxSlices = 128;
inline constexpr int kStateVersion = 3;

enum class SliceMode : int { Transients, Random, Grid, Manual };

struct AnalysisSettings {
    SliceMode mode = SliceMode::Transients;
    int targetCount = 16;
    double sensitivity = 0.5;
    std::uint64_t seed = 1;
    double preAttackMs = 2.0;
    int rootNote = 48;
    int scale = 0;
    bool descending = false;
    double minimumMs = 0.0;
    bool zeroCrossing = false;
    double randomSpread = 0.45;
    double sourceBpm = 120.0;
    double gridBeats = 0.0; // zero selects equal-count grid
    engine::FrameCount rangeStart = 0;
    engine::FrameCount rangeEnd = 0; // zero selects the full source
    friend bool operator==(const AnalysisSettings&, const AnalysisSettings&) = default;
};

enum class Param : std::uint32_t {
    Volume,          ///< instrument output level, linear
    Drive,           ///< soft saturation on the summed output
    Attack,          ///< seconds, fades the front of every chop up
    Release,         ///< seconds, fades a released chop down
    Gate,            ///< fraction of the chop played before the release
    KeyTrack,        ///< 0 = every chop sounds as recorded, 1 = an octave up per octave played
    RootNote,        ///< the key a chop sounds untransposed by Key Track, and the key the layout starts from
    ChokeMode,       ///< 0 off, 1 all voices, 2 by choke group
    VelocityDepth,   ///< how much velocity affects the level

    Transpose,
    FineTune,
    Pan,
    Decay,
    Sustain,
    PlayMode,       ///< 0 gate, 1 one shot (looping notes still obey note off)
    Polyphony,
    BendRange,
    CrushBits,
    CrushRate,
    CrushMix,

    kCount
};

inline constexpr std::uint32_t kParameterCount = std::uint32_t(Param::kCount);

constexpr std::uint32_t indexOf(Param parameter) noexcept {
    return std::uint32_t(parameter);
}

/// One chop of the loaded sample. Frame indices are absolute in the decoded
/// buffer, so a chop outlives every knob setting around it.
struct Slice {
    engine::FrameCount start = 0;
    engine::FrameCount end = 0;
    std::int16_t key = 48;      ///< MIDI key this chop is parked on
    std::int16_t transpose = 0; ///< semitones of permanent pitch shift
    float gain = 1.0f;          ///< linear
    float pan = 0.0f;           ///< −1…1
    float cutoff = 1.0f;        ///< 0…1, mapped 20 Hz…20 kHz
    float resonance = 0.0f;     ///< 0…1
    std::uint8_t filter = 0;    ///< 0 off, 1 lowpass, 2 highpass, 3 bandpass
    std::uint8_t chokeGroup = 0;
    std::uint8_t flags = 0;

    std::uint32_t id = 0;
    float fineTune = 0.0f;
    float normalization = 1.0f;
    float fadeInMs = 0.0f;
    float fadeOutMs = -1.0f; // automatic legacy taper
    float crossfadeMs = 0.0f;
    std::uint8_t loopMode = 0; // 0 off, 1 forward, 2 ping-pong
    bool locked = false;
    bool useGlobalEnvelope = true;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.05f;
    // The filter keeps its legacy fields. Additional effects run per voice,
    // so another slice (including an overlapping note) keeps its own sound.
    std::uint8_t effect = 0; // 0 off, 1 saturation, 2 crusher, 3 ring modulation
    float effectX = 0.5f;
    float effectY = 0.5f;
    float effectMix = 1.0f;
    friend bool operator==(const Slice&, const Slice&) = default;

    /// Guarded against the sample length, never trusted from a file alone.
    bool valid(engine::FrameCount frames) const noexcept {
        return start < frames && end <= frames && end > start;
    }
};

/// A chop can be run backwards without the analysis having to know. These live
/// on the chop rather than in the state's free-form JSON because they change
/// what the audio thread does with a slice, exactly like its start index does.
inline constexpr std::uint8_t kSliceReverse = 1u << 0;
inline constexpr std::uint8_t kSliceLoop = 1u << 1;
inline constexpr std::uint8_t kSliceMuted = 1u << 2;

/// What the analysis published and what playback reads. Immutable once it has
/// been handed to the audio thread: a note must not be able to catch a table
/// halfway through a re-slice, so every change replaces the whole thing.
struct SliceTable {
    std::array<Slice, kMaxSlices> slices{};
    /// Key → index into `slices`, built by `rebuild` so a note-on is a lookup
    /// rather than a linear scan over a hundred and twenty-eight entries.
    std::array<std::int16_t, 128> keyToIndex{};
    std::uint32_t count = 0;
    engine::FrameCount frames = 0;
    std::uint32_t nextId = 1;
    bool chromaticFallback = false;

    SliceTable() { keyToIndex.fill(-1); }

    /// Recomputes the key lookup. Called by every producer — the analysis, the
    /// state loader, the tests — so none of them can disagree about it.
    void rebuild() noexcept;
    int indexForId(std::uint32_t id) const noexcept;

    const Slice* forIndex(std::uint32_t index) const noexcept {
        return index < count ? &slices[index] : nullptr;
    }
    /// −1 when no chop is parked on that key.
    std::int32_t indexForKey(int key) const noexcept {
        return key >= 0 && key < 128 ? std::int32_t(keyToIndex[key]) : -1;
    }
    const Slice* forKey(int key) const noexcept {
        const std::int32_t index = indexForKey(key);
        return index < 0 ? nullptr : forIndex(std::uint32_t(index));
    }
    /// Every chop parked on a key, in table order. Used by the choke search so
    /// a table can hold two chops on one key without a second index.
    std::span<const Slice> forKeySpan(int key) const noexcept;
};

/// The scales the chop layout can walk. Degree 0 is the scale root in every
/// one of them, and each list covers exactly one octave so `degreeToKey` can
/// add the carry instead of holding a long table. Nothing in the plugin's
/// playback path reads these — a chop is already parked on a key by the time
/// it plays — but the analysis and the panel do, and keeping them beside the
/// chop model is what stops the two from disagreeing about which degrees exist.
inline constexpr int kScaleCount = 12;
std::span<const std::uint8_t> scaleDegrees(int scale) noexcept;
/// English, untranslated — the panel owns the wording, this is the fallback.
const char* scaleName(int scale) noexcept;
/// Semitones above the scale root for a degree. Unbounded and unclamped: the
/// chop layout wraps this into the MIDI range itself, because a run of more
/// than a hundred and twenty-eight degrees has to come out *somewhere* and a
/// clamp would pile every overflow chop onto G9.
int degreeToSemitone(int degree, int scale) noexcept;
/// MIDI key of a scale degree counted from `root`, clamped into range.
int degreeToKey(int degree, int root, int scale) noexcept;
/// `C-1`…`G9`, the same numbering the piano roll labels.
std::string noteName(int midi);

struct ControlState {
    std::string path;
    std::shared_ptr<const engine::SampleBuffer> audio;
    std::shared_ptr<const SliceTable> table;
    AnalysisSettings analysis;
    std::array<double, kParameterCount> parameters{};
};

/// Every knob, in index order. Built once on first use.
std::span<const ParameterInfo> parameterTable() noexcept;

/// Human-readable value, used by the host's readouts.
std::string parameterText(std::uint32_t index, double plainValue);

} // namespace daw::plugins::slicer
