#pragma once

#include "Internal/SamplerVoice.hpp"
#include "Internal/SlicerParams.hpp"

#include <cstdint>

/// The slicer's per-note DSP: one chop, played forwards or backwards, through
/// the same amplitude envelope and state-variable filter the sampler uses.
///
/// Everything here is realtime-safe and allocation-free. Nothing knows about
/// the plugin API — the instance resolves the knobs into a `SlicerSettings`
/// once per block and hands the same copy to every voice, which is what keeps
/// parameter loads out of the per-voice inner loop.
///
/// What this is not: a sampler voice. There is no stretch engine, no modulation
/// matrix and no loop points expressed as fractions — a chop is an absolute
/// frame range with a fixed pitch, and the whole playback path is one resample
/// loop. That is the point of the plugin.
namespace daw::plugins::slicer {

/// The sampler already carries the path and name a project round-trip needs,
/// and a second shape for it would be a second answer to "what does playback
/// need to know about this file".
using SampleData = sampler::SampleData;

/// Every knob that matters to a voice, resolved once per block. Level and Drive
/// are deliberately absent: they act on the summed instrument output, so
/// putting them here would ask every voice to saturate its own copy of a
/// signal that has not been mixed yet.
struct SlicerSettings {
    double attack = 0.0;          ///< seconds
    double release = 0.05;        ///< seconds, clamped to a click-free floor
    double gate = 1.0;            ///< fraction of the chop played before release
    double keyTrack = 0.0;        ///< 0…1
    int rootNote = 48;
    double velocityDepth = 1.0;
    double transpose = 0.0;
    double pan = 0.0;
    bool oneShot = false;

    /// The amplitude envelope every voice runs. Built by the instance from the
    /// Attack/Release knobs rather than exposed on its own, so a slicer
    /// parameter cannot end up describing a stage the panel never shows.
    sampler::EnvSettings ampEnv;
};

/// One sounding chop.
class Voice {
public:
    /// Hands everything over for one note. Returns false when the chop cannot
    /// sound — no audio, a muted chop, or one whose range no longer fits the
    /// buffer — so the instance never has to bookkeep a voice that never starts.
    bool start(const Slice& slice, int key, int channel, float velocity,
               float notePan, const SlicerSettings& settings,
               const SampleData& sample, double sampleRate,
               std::uint32_t sliceIndex, std::int32_t noteId) noexcept;

    /// Fade out over the Release knob. Calling it twice is a no-op: a retrigger
    /// must not restart a tail that is already on its way down.
    void release() noexcept;
    /// End with a short ramp regardless of the Release knob, for choke groups
    /// and for All Voices mode.
    void choke() noexcept;
    /// Cut immediately, for a transport stop or reset.
    void kill() noexcept;

    bool active() const noexcept { return m_active; }
    bool releasing() const noexcept { return m_amp.released(); }
    int key() const noexcept { return m_key; }
    int channel() const noexcept { return m_channel; }
    std::int32_t noteId() const noexcept { return m_noteId; }
    std::uint64_t startedAt() const noexcept { return m_startedAt; }
    void setStartedAt(std::uint64_t stamp) noexcept { m_startedAt = stamp; }
    std::uint32_t sliceIndex() const noexcept { return m_sliceIndex; }
    std::uint8_t chokeGroup() const noexcept { return m_chokeGroup; }
    std::uint32_t sliceId() const noexcept { return m_sliceId; }
    bool oneShot() const noexcept { return m_oneShot && !m_loop; }
    void updateSound(const Slice&, const SlicerSettings&, double sampleRate) noexcept;

    /// Pitch bend and expression. A chop's base rate is fixed at `start`, so
    /// this only ever offsets it.
    void setBend(double semitones, double smoothingMs) noexcept;

    /// Adds this voice into `out`.
    void render(const SampleData& sample, const SlicerSettings& settings,
                float* const* out, engine::ChannelCount channels,
                engine::FrameCount frames, double sampleRate) noexcept;

private:
    /// Source position → one interpolated sample. 4-point Hermite, the same
    /// interpolation the sampler uses: linear is audibly grainy an octave up,
    /// and an octave up is what Key Track exists to do.
    static float readSample(const engine::SampleBuffer& audio,
                            engine::ChannelCount channel, double position) noexcept;

    bool m_active = false;
    int m_key = 48;
    int m_channel = 0;
    std::int32_t m_noteId = -1;
    std::uint64_t m_startedAt = 0;
    std::uint8_t m_chokeGroup = 0;
    std::uint32_t m_sliceIndex = 0;
    std::uint32_t m_sliceId = 0;
    bool m_oneShot = false;
    bool m_pingpong = false;
    bool m_globalEnvelope = true;
    sampler::EnvSettings m_localEnvelope;
    sampler::EnvSettings m_envelopeSettings;

    double m_position = 0.0;
    double m_start = 0.0;        ///< source frames
    double m_end = 0.0;
    double m_step = 0.0;         ///< source frames per output frame, signed
    double m_fadeOut = 0.0;      ///< frames of taper at the end of the chop
    bool m_forward = true;
    bool m_loop = false;
    double m_fadeIn = 0.0;
    double m_crossfade = 0.0;
    double m_travel = 0.0;
    double m_tune = 0.0, m_tuneTarget = 0.0;
    double m_velocity = 1.0, m_notePan = 0.0;

    double m_gain = 1.0;         ///< chop gain × velocity, folded here
    double m_gainLeft = 1.0;
    double m_gainRight = 1.0;
    double m_gainTarget = 1.0, m_leftTarget = 1.0, m_rightTarget = 1.0;
    double m_filterMix = 0.0;

    double m_bend = 0.0;         ///< semitones, eased toward the target
    double m_bendTarget = 0.0;
    double m_bendSmoothingMs = 8.0;

    float m_cutoff = 1.0f;
    float m_resonance = 0.0f;
    float m_cutoffTarget = 1.0f, m_resonanceTarget = 0.0f;
    std::uint8_t m_previousFilter = 0;
    double m_filterBlend = 1.0;
    sampler::Svf m_previousFilterDsp;
    std::uint8_t m_filter = 0;

    sampler::Envelope m_amp;
    sampler::Svf m_filterDsp;

    std::uint8_t m_effect = 0;
    double m_effectX = .5, m_effectY = .5, m_effectMix = 1.0;
    double m_effectXTarget = .5, m_effectYTarget = .5, m_effectMixTarget = 1.0;
    std::array<double, 3> m_effectWeights{};
    double m_toneState[2]{}, m_crushHeld[2]{};
    double m_ringPhase = 0.0;
    int m_crushCounter = 0;

    /// Frames left of a forced short fade. Negative means "not cutting".
    int m_cutRemaining = -1;
    int m_cutLength = 1;
};

} // namespace daw::plugins::slicer
