#pragma once

#include "Audio/SampleBuffer.hpp"
#include "Internal/SamplerParams.hpp"

#include <cstdint>
#include <memory>
#include <string>

/// The sampler's per-note DSP: envelopes, LFOs, the filter and the two
/// playback engines (plain resampling and a granular stretch).
///
/// Everything here is realtime-safe and allocation-free once `prepare` has run.
/// Nothing in this file knows about the plugin API — the instance builds a
/// `SamplerSettings` snapshot from its parameter array once per block and hands
/// it to every voice, which is what keeps 136 parameter loads out of the
/// per-voice inner loop.
namespace daw::plugins::sampler {

/// FL's "tension": bends a 0…1 ramp without changing its endpoints. Positive
/// values make it start fast and ease out, negative the other way; zero is a
/// straight line.
double applyTension(double t, double tension) noexcept;

struct EnvSettings {
    double delay = 0.0;
    double attack = 0.0;
    double hold = 0.0;
    double decay = 0.0;
    double sustain = 1.0;
    double release = 0.0;
    double attackTension = 0.0;
    double decayTension = 0.0;
    double releaseTension = 0.0;
};

/// DAHDSR with per-segment tension. Advanced by an explicit `dt` so the same
/// class can run per sample for the amplitude and per sub-block for the
/// modulation targets.
class Envelope {
public:
    void noteOn() noexcept;
    void noteOff() noexcept;
    void kill() noexcept;

    /// True until the release has finished — the amplitude envelope's answer to
    /// "may this voice be retired".
    bool active() const noexcept { return m_stage != Stage::Idle && m_stage != Stage::Done; }
    bool released() const noexcept { return m_stage == Stage::Release || m_stage == Stage::Done; }
    double value() const noexcept { return m_value; }

    double advance(double dt, const EnvSettings& settings) noexcept;

private:
    enum class Stage : std::uint8_t { Idle, Delay, Attack, Hold, Decay, Sustain, Release, Done };

    Stage m_stage = Stage::Idle;
    double m_time = 0.0;        ///< seconds inside the current stage
    double m_value = 0.0;
    double m_releaseFrom = 0.0;
};

/// One modulation oscillator. `global` LFOs read a phase the instance keeps
/// running across notes, so this only owns the retriggered case — plus the
/// delay/attack ramp, which is per note either way.
class Lfo {
public:
    void noteOn() noexcept { m_phase = 0.0; m_time = 0.0; }
    /// Returns −1 … 1, already scaled by the delay/attack ramp.
    double advance(double dt, double rateHz, int shape, double delay,
                   double attack, const double* globalPhase) noexcept;

private:
    double m_phase = 0.0;
    double m_time = 0.0;
};

/// Zavalishin's TPT state-variable filter, one per channel. Stable at any
/// cutoff and cheap to modulate, which is what a per-note filter needs.
class Svf {
public:
    void reset() noexcept;
    /// `cutoffHz` and `resonance` (0…1) are recomputed per sub-block, not per
    /// sample — the coefficients cost a tan() each.
    void setCoefficients(double cutoffHz, double resonance, double sampleRate) noexcept;
    float processLowpass(int channel, float input) noexcept;

private:
    double m_g = 0.0;
    double m_k = 2.0;
    double m_a1 = 0.0, m_a2 = 0.0, m_a3 = 0.0;
    double m_ic1[2] = {0.0, 0.0};
    double m_ic2[2] = {0.0, 0.0};
};

/// One INS-page target's settings, resolved for a block.
struct ModSettings {
    bool envOn = false;
    double envAmount = 0.0;
    EnvSettings env;
    double lfoAmount = 0.0;
    double lfoSpeed = 2.0;
    double lfoDelay = 0.0;
    double lfoAttack = 0.0;
    bool lfoTempo = false;
    bool lfoGlobal = false;
    int lfoShape = 0;

    /// Nothing to compute when neither half is doing anything — checked once
    /// per block rather than per sub-block per voice.
    bool idle() const noexcept {
        return (!envOn || envAmount == 0.0) && lfoAmount == 0.0;
    }
};

/// Every knob that matters to a voice, resolved once per block.
struct SamplerSettings {
    double volume = 0.55;
    double pan = 0.0;
    double pitchSemitones = 0.0;   ///< the Pitch knob already scaled by Range
    /// The Range knob itself. It also scales the INS pitch envelope and LFO —
    /// the same thing FL does, and the reason a pitch modulation with Range at
    /// zero is deliberately silent rather than secretly an octave.
    double pitchRange = 2.0;
    double modX = 1.0;             ///< base cutoff, 0…1
    double modY = 0.0;             ///< base resonance, 0…1
    int rootNote = 60;

    bool ampEnvOn = false;
    EnvSettings ampEnv;

    double startOffset = 0.0;
    double endOffset = 1.0;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    int loopMode = 0;
    double loopStart = 0.0;
    double loopEnd = 1.0;

    int stretchMode = 0;
    double stretchTime = 1.0;
    double stretchPitch = 0.0;
    double formant = 0.0;

    ModSettings mod[kModTargetCount];
};

/// Decoded audio plus the two things playback needs to know about it that the
/// buffer itself cannot say.
struct SampleData {
    std::shared_ptr<const engine::SampleBuffer> audio;
    /// Frames of the original sample. The precomputed reverb appends a tail, so
    /// this — not `audio->frames()` — is what the start, loop and fade
    /// fractions are measured against; the tail is only ever played out.
    engine::FrameCount baseFrames = 0;
    std::string path;
    std::string name;
};

/// One sounding note.
class Voice {
public:
    void start(int key, int channel, float velocity, float notePan,
               const SamplerSettings& settings, const SampleData& sample,
               double sampleRate) noexcept;
    /// `cutWhenEnvelopeOff` is the instance's answer to "would this note ever
    /// stop on its own" — true for a looping sample whose amplitude envelope is
    /// switched off, which otherwise sounds forever.
    void release(bool cutWhenEnvelopeOff) noexcept;
    /// Cut immediately, for a stolen voice or a transport stop.
    void kill() noexcept;

    bool active() const noexcept { return m_active; }
    bool releasing() const noexcept { return m_amp.released(); }
    int key() const noexcept { return m_key; }
    int channel() const noexcept { return m_channel; }
    std::uint64_t startedAt() const noexcept { return m_startedAt; }
    void setStartedAt(std::uint64_t stamp) noexcept { m_startedAt = stamp; }

    /// Adds this voice into `out`. `globalPhase` is the instance's free-running
    /// LFO phase per target, in cycles.
    void render(const SampleData& sample, const SamplerSettings& settings,
                float* const* out, engine::ChannelCount channels,
                engine::FrameCount frames, double sampleRate, double tempo,
                const double* globalPhase) noexcept;

private:
    /// Two overlapping grains is the smallest overlap-add that reconstructs a
    /// continuous signal from a Hann window, and enough for a sampler's modest
    /// stretch ratios.
    struct Grain {
        bool active = false;
        double read = 0.0;    ///< source position
        double phase = 0.0;   ///< 0 … grain length, in output frames
    };

    /// Where playback may go: the loop if there is one, the whole sample if not.
    struct Region {
        double start = 0.0;      ///< the START OFFSET position
        double loopStart = 0.0;
        double loopEnd = 0.0;
        double total = 0.0;      ///< including the precomputed reverb tail
        double base = 0.0;       ///< the original sample's length
        double end = 0.0;        ///< end marker, with tail when set to 100 %
        int loopMode = 0;
    };
    static Region regionFor(const SamplerSettings& settings,
                            const SampleData& sample) noexcept;

    /// Source position → one interpolated sample. 4-point Hermite: linear
    /// interpolation is audibly grainy an octave up, which is an ordinary thing
    /// to ask a sampler for.
    static float readSample(const engine::SampleBuffer& audio,
                            engine::ChannelCount channel, double position) noexcept;

    /// Fill `left`/`right` with `count` frames of raw source, fades included,
    /// and return how many frames were actually produced — fewer when the
    /// sample ran out. Both playback engines share the gain, filter and pan
    /// pass that follows, which is why they only produce source here.
    engine::FrameCount fillResampled(const SampleData& sample,
                                     const SamplerSettings& settings,
                                     const Region& region, float* left, float* right,
                                     engine::FrameCount count, double rate) noexcept;
    engine::FrameCount fillGranular(const SampleData& sample,
                                    const SamplerSettings& settings,
                                    const Region& region, float* left, float* right,
                                    engine::FrameCount count, double pitchRatio,
                                    double timeRate, double grainLength) noexcept;

    /// Where the fade-in/fade-out envelope stands at a source position.
    double fadeGain(const SamplerSettings& settings, const Region& region,
                    double position) const noexcept;

    bool m_active = false;
    int m_key = 60;
    int m_channel = 0;
    float m_velocity = 1.0f;
    float m_notePan = 0.0f;
    std::uint64_t m_startedAt = 0;

    double m_position = 0.0;
    bool m_forward = true;
    /// Where the granular engine's playhead sits; the grains chase it.
    double m_grainSource = 0.0;
    double m_grainTimer = 0.0;
    Grain m_grains[2];

    /// Frames left of the short fade that ends a looping note whose amplitude
    /// envelope is switched off. Negative means "not cutting" — with the
    /// envelope off a one-shot is meant to play to its end even after the key
    /// is released, but a *loop* would then never stop.
    int m_cutRemaining = -1;
    int m_cutLength = 1;

    Envelope m_amp;
    Envelope m_modEnv[kModTargetCount];
    Lfo m_lfo[kModTargetCount];
    Svf m_filter;
    double m_formantLow[2] = {0.0, 0.0};
};

} // namespace daw::plugins::sampler
