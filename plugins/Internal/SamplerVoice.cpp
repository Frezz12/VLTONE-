#include "Internal/SamplerVoice.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw::plugins::sampler {
namespace {

constexpr double kPi = std::numbers::pi;

/// How often modulation is recomputed. The amplitude envelope runs per sample —
/// a stepped attack is audible as a click — while the filter, pan and pitch
/// modulation move once per 32 frames, which is 0.7 ms at 48 kHz and costs one
/// tan() per voice per sub-block instead of one per sample.
constexpr engine::FrameCount kModBlock = 32;

/// The granular engine's grain length in output frames, and its hop. Hann at
/// 50 % overlap sums to exactly one, so two grains reconstruct a continuous
/// signal with no ripple.
constexpr double kDefaultGrainLength = 2048.0;

/// 0…1 knob → 20 Hz … 20 kHz, the range a filter knob is expected to sweep.
double cutoffHz(double knob) noexcept {
    return 20.0 * std::pow(1000.0, std::clamp(knob, 0.0, 1.0));
}

double semitonesToRatio(double semitones) noexcept {
    return std::pow(2.0, semitones / 12.0);
}

} // namespace

double applyTension(double t, double tension) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    if (std::abs(tension) < 1e-4) return t;
    // exp-shaped bend with the endpoints pinned: t(0) = 0 and t(1) = 1 for any
    // tension, so changing the curve never changes where the segment ends up.
    // The sign is negated so that *positive* tension starts fast and eases out,
    // which is the direction the header promises and the one a knob turned up
    // is expected to mean.
    const double k = -tension * 6.0;
    return (1.0 - std::exp(k * t)) / (1.0 - std::exp(k));
}

// ── Envelope ───────────────────────────────────────────────────────────────

void Envelope::noteOn() noexcept {
    m_stage = Stage::Delay;
    m_time = 0.0;
    m_value = 0.0;
    m_releaseFrom = 0.0;
}

void Envelope::noteOff() noexcept {
    if (m_stage == Stage::Idle || m_stage == Stage::Done) return;
    m_releaseFrom = m_value;
    m_stage = Stage::Release;
    m_time = 0.0;
}

void Envelope::kill() noexcept {
    m_stage = Stage::Idle;
    m_time = 0.0;
    m_value = 0.0;
}

double Envelope::advance(double dt, const EnvSettings& s) noexcept {
    if (m_stage == Stage::Idle) return 0.0;
    if (m_stage == Stage::Done) return 0.0;

    m_time += dt;
    // A while loop rather than a chain of ifs: with a long block and short
    // stages a single advance can cross several of them, and a per-call single
    // transition would stretch a 1 ms attack into one attack per block.
    for (;;) {
        switch (m_stage) {
            case Stage::Delay:
                if (m_time < s.delay) { m_value = 0.0; return m_value; }
                m_time -= s.delay;
                m_stage = Stage::Attack;
                continue;
            case Stage::Attack:
                if (s.attack > 0.0 && m_time < s.attack) {
                    m_value = applyTension(m_time / s.attack, s.attackTension);
                    return m_value;
                }
                m_time -= s.attack;
                m_stage = Stage::Hold;
                continue;
            case Stage::Hold:
                if (m_time < s.hold) { m_value = 1.0; return m_value; }
                m_time -= s.hold;
                m_stage = Stage::Decay;
                continue;
            case Stage::Decay:
                if (s.decay > 0.0 && m_time < s.decay) {
                    m_value = 1.0 - (1.0 - s.sustain) *
                                        applyTension(m_time / s.decay, s.decayTension);
                    return m_value;
                }
                m_time -= s.decay;
                m_stage = Stage::Sustain;
                continue;
            case Stage::Sustain:
                m_value = s.sustain;
                return m_value;
            case Stage::Release:
                if (s.release > 0.0 && m_time < s.release) {
                    m_value = m_releaseFrom *
                              (1.0 - applyTension(m_time / s.release, s.releaseTension));
                    return m_value;
                }
                m_stage = Stage::Done;
                m_value = 0.0;
                return m_value;
            case Stage::Idle:
            case Stage::Done:
                m_value = 0.0;
                return m_value;
        }
    }
}

// ── LFO ────────────────────────────────────────────────────────────────────

double Lfo::advance(double dt, double rateHz, int shape, double delay, double attack,
                    const double* globalPhase) noexcept {
    m_time += dt;
    // A global LFO reads the instance's free-running phase, so every voice sees
    // the same sweep; a retriggered one owns its phase and starts at zero.
    if (globalPhase) {
        m_phase = *globalPhase;
    } else {
        m_phase += rateHz * dt;
        if (m_phase >= 1e6) m_phase = std::fmod(m_phase, 1.0);
    }
    const double phase = m_phase - std::floor(m_phase);

    double wave = 0.0;
    switch (shape) {
        case 1: wave = 4.0 * std::abs(phase - 0.5) - 1.0; break;      // triangle
        case 2: wave = phase < 0.5 ? 1.0 : -1.0; break;               // square
        case 3: wave = 2.0 * phase - 1.0; break;                      // saw
        default: wave = std::sin(2.0 * kPi * phase); break;           // sine
    }

    // Delay then attack, both measured from the note — that is what the two
    // knobs mean on the INS page, global LFO or not.
    if (m_time < delay) return 0.0;
    if (attack > 0.0) {
        const double since = m_time - delay;
        if (since < attack) wave *= since / attack;
    }
    return wave;
}

// ── Filter ─────────────────────────────────────────────────────────────────

void Svf::reset() noexcept {
    m_ic1[0] = m_ic1[1] = 0.0;
    m_ic2[0] = m_ic2[1] = 0.0;
}

void Svf::setCoefficients(double cutoff, double resonance, double sampleRate) noexcept {
    const double nyquist = sampleRate * 0.5;
    const double clamped = std::clamp(cutoff, 10.0, nyquist * 0.99);
    m_g = std::tan(kPi * clamped / sampleRate);
    // Resonance 0…1 → Q 0.5…12. Never let k reach zero: the filter would
    // self-oscillate and a knob at its top would blow up.
    const double q = 0.5 + std::clamp(resonance, 0.0, 1.0) * 11.5;
    m_k = std::max(1.0 / q, 0.05);
    m_a1 = 1.0 / (1.0 + m_g * (m_g + m_k));
    m_a2 = m_g * m_a1;
    m_a3 = m_g * m_a2;
}

float Svf::processLowpass(int channel, float input) noexcept {
    const double v0 = input;
    const double v3 = v0 - m_ic2[channel];
    const double v1 = m_a1 * m_ic1[channel] + m_a2 * v3;
    const double v2 = m_ic2[channel] + m_a2 * m_ic1[channel] + m_a3 * v3;
    m_ic1[channel] = 2.0 * v1 - m_ic1[channel];
    m_ic2[channel] = 2.0 * v2 - m_ic2[channel];
    return float(v2);
}

// ── Voice ──────────────────────────────────────────────────────────────────

void Voice::start(int key, int channel, float velocity, float notePan,
                  const SamplerSettings& settings, const SampleData& sample,
                  double sampleRate) noexcept {
    m_active = true;
    m_key = key;
    m_channel = channel;
    m_velocity = velocity;
    m_notePan = std::clamp(notePan, -1.0f, 1.0f);
    m_cutRemaining = -1;
    m_cutLength = std::max(1, int(sampleRate * 0.005));

    const Region region = regionFor(settings, sample);
    m_position = region.start;
    m_forward = true;
    m_grainSource = region.start;
    m_grainTimer = 0.0;
    for (Grain& grain : m_grains) grain = Grain{};

    m_amp.noteOn();
    for (std::uint32_t t = 0; t < kModTargetCount; ++t) {
        m_modEnv[t].noteOn();
        m_lfo[t].noteOn();
    }
    m_filter.reset();
    m_formantLow[0] = m_formantLow[1] = 0.0;
}

void Voice::release(bool cutWhenEnvelopeOff) noexcept {
    if (!m_active) return;
    m_amp.noteOff();
    for (Envelope& env : m_modEnv) env.noteOff();
    // With the amplitude envelope switched off a one-shot is meant to play to
    // its end after the key is released — but a *looping* one never reaches an
    // end, so it gets a short ramp instead of playing forever.
    if (cutWhenEnvelopeOff && m_cutRemaining < 0) m_cutRemaining = m_cutLength;
}

void Voice::kill() noexcept {
    m_active = false;
    m_amp.kill();
    for (Envelope& env : m_modEnv) env.kill();
    m_cutRemaining = -1;
}

Voice::Region Voice::regionFor(const SamplerSettings& settings,
                               const SampleData& sample) noexcept {
    Region region;
    if (!sample.audio) return region;
    const double total = double(sample.audio->frames());
    const double base = sample.baseFrames > 0 ? double(sample.baseFrames) : total;

    region.total = total;
    region.base = base;
    region.start = std::clamp(settings.startOffset, 0.0, 1.0) * base;
    const double endFraction = std::clamp(settings.endOffset, 0.0, 1.0);
    region.end = endFraction >= 0.999999 ? total : endFraction * base;
    if (region.end - region.start < 16.0)
        region.end = std::min(total, region.start + 16.0);
    region.loopMode = settings.loopMode;
    region.loopStart = std::clamp(
        std::clamp(settings.loopStart, 0.0, 1.0) * base,
        region.start, region.end);
    region.loopEnd = std::clamp(
        std::clamp(settings.loopEnd, 0.0, 1.0) * base,
        region.start, region.end);
    // A loop shorter than a handful of frames is a mistake, not a request:
    // honouring it would turn the note into a click at the sample rate.
    if (region.loopEnd - region.loopStart < 16.0) region.loopMode = 0;
    return region;
}

float Voice::readSample(const engine::SampleBuffer& audio, engine::ChannelCount channel,
                        double position) noexcept {
    const engine::FrameCount frames = audio.frames();
    if (frames == 0) return 0.0f;
    const double clamped = std::clamp(position, 0.0, double(frames - 1));
    const std::int64_t i = std::int64_t(clamped);
    const float fraction = float(clamped - double(i));
    const float* data = audio.channel(channel);

    const auto at = [&](std::int64_t index) -> float {
        return data[std::clamp<std::int64_t>(index, 0, std::int64_t(frames) - 1)];
    };
    const float y0 = at(i - 1);
    const float y1 = at(i);
    const float y2 = at(i + 1);
    const float y3 = at(i + 2);

    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * fraction + c2) * fraction + c1) * fraction + c0;
}

double Voice::fadeGain(const SamplerSettings& settings, const Region& region,
                       double position) const noexcept {
    double gain = 1.0;
    const double span = std::max(region.base - region.start, 1.0);
    if (settings.fadeIn > 0.0) {
        const double length = settings.fadeIn * span;
        const double into = position - region.start;
        if (into < length) gain *= std::clamp(into / std::max(length, 1.0), 0.0, 1.0);
    }
    if (settings.fadeOut > 0.0) {
        const double length = settings.fadeOut * span;
        const double left = region.end - position;
        if (left < length) gain *= std::clamp(left / std::max(length, 1.0), 0.0, 1.0);
    }
    return gain;
}

engine::FrameCount Voice::fillResampled(const SampleData& sample,
                                        const SamplerSettings& settings,
                                        const Region& region, float* left, float* right,
                                        engine::FrameCount count, double rate) noexcept {
    const engine::SampleBuffer& audio = *sample.audio;
    const bool stereo = audio.channels() > 1;
    const double step = std::abs(rate);

    for (engine::FrameCount i = 0; i < count; ++i) {
        const double fade = fadeGain(settings, region, m_position);
        left[i] = readSample(audio, 0, m_position) * float(fade);
        right[i] = stereo ? readSample(audio, 1, m_position) * float(fade) : left[i];

        m_position += m_forward ? step : -step;

        switch (region.loopMode) {
            case 1:   // forward
                if (m_position >= region.loopEnd) {
                    const double length = region.loopEnd - region.loopStart;
                    m_position -= length * std::floor((m_position - region.loopStart) / length);
                }
                break;
            case 2:   // ping-pong
                if (m_forward && m_position >= region.loopEnd) {
                    m_position = region.loopEnd - (m_position - region.loopEnd);
                    m_forward = false;
                } else if (!m_forward && m_position <= region.loopStart) {
                    m_position = region.loopStart + (region.loopStart - m_position);
                    m_forward = true;
                }
                break;
            default:
                // Past the end — the tail the precomputed reverb added
                // included, which is why this tests `total` and not `base`.
                if (m_position >= region.end) return i + 1;
                break;
        }
    }
    return count;
}

engine::FrameCount Voice::fillGranular(const SampleData& sample,
                                       const SamplerSettings& settings,
                                       const Region& region, float* left, float* right,
                                       engine::FrameCount count, double pitchRatio,
                                       double timeRate, double grainLength) noexcept {
    const engine::SampleBuffer& audio = *sample.audio;
    const bool stereo = audio.channels() > 1;
    const bool looping = region.loopMode != 0;
    const double loopLength = region.loopEnd - region.loopStart;
    grainLength = std::clamp(grainLength, 128.0, 8192.0);
    const double grainHop = grainLength * 0.5;

    // Both the playhead and each grain's read pointer stay inside the loop; a
    // grain that walked past it would play material the loop excludes.
    const auto wrap = [&](double position) {
        if (!looping || loopLength <= 0.0) return position;
        if (position < region.loopEnd) return position;
        return region.loopStart +
               std::fmod(position - region.loopStart, loopLength);
    };

    for (engine::FrameCount i = 0; i < count; ++i) {
        if (m_grainTimer <= 0.0) {
            Grain* slot = nullptr;
            for (Grain& grain : m_grains) {
                if (!grain.active) { slot = &grain; break; }
            }
            // No free slot means the hop and the grain length disagree, which
            // cannot happen with a fixed 50 % overlap — but stealing the older
            // grain keeps a rounding error from silencing the voice.
            if (!slot) {
                slot = &m_grains[0];
                for (Grain& grain : m_grains) {
                    if (grain.phase > slot->phase) slot = &grain;
                }
            }
            slot->active = true;
            slot->read = m_grainSource;
            slot->phase = 0.0;
            m_grainTimer += grainHop;
        }

        double sumLeft = 0.0;
        double sumRight = 0.0;
        for (Grain& grain : m_grains) {
            if (!grain.active) continue;
            const double window =
                0.5 - 0.5 * std::cos(2.0 * kPi * grain.phase / grainLength);
            sumLeft += window * readSample(audio, 0, grain.read);
            if (stereo) sumRight += window * readSample(audio, 1, grain.read);
            grain.read = wrap(grain.read + pitchRatio);
            grain.phase += 1.0;
            if (grain.phase >= grainLength) grain.active = false;
        }

        const double fade = fadeGain(settings, region, m_grainSource);
        left[i] = float(sumLeft * fade);
        right[i] = stereo ? float(sumRight * fade) : left[i];

        m_grainTimer -= 1.0;
        m_grainSource += timeRate;
        if (looping) {
            m_grainSource = wrap(m_grainSource);
        } else if (m_grainSource >= region.end) {
            return i + 1;
        }
    }
    return count;
}

void Voice::render(const SampleData& sample, const SamplerSettings& settings,
                   float* const* out, engine::ChannelCount channels,
                   engine::FrameCount frames, double sampleRate, double tempo,
                   const double* globalPhase) noexcept {
    if (!m_active || !sample.audio || sample.audio->frames() == 0 || channels == 0) {
        return;
    }

    const Region region = regionFor(settings, sample);
    const double sourceRate = sample.audio->sampleRate() > 0.0
                                  ? sample.audio->sampleRate()
                                  : sampleRate;
    const double rateScale = sourceRate / std::max(sampleRate, 1.0);
    const double keySemitones = double(m_key - settings.rootNote);

    float sourceLeft[kModBlock];
    float sourceRight[kModBlock];

    engine::FrameCount done = 0;
    while (done < frames && m_active) {
        const engine::FrameCount count = std::min<engine::FrameCount>(kModBlock, frames - done);
        const double dt = double(count) / sampleRate;

        // ── Modulation for this sub-block ──
        double modulation[kModTargetCount] = {};
        for (std::uint32_t t = 0; t < kModTargetCount; ++t) {
            const ModSettings& mod = settings.mod[t];
            double value = 0.0;
            if (mod.envOn) {
                const double env = m_modEnv[t].advance(dt, mod.env);
                value += mod.envAmount * env;
            }
            if (mod.lfoAmount != 0.0) {
                const double rate = mod.lfoTempo ? mod.lfoSpeed * tempo / 60.0 : mod.lfoSpeed;
                value += mod.lfoAmount *
                         m_lfo[t].advance(dt, rate, mod.lfoShape, mod.lfoDelay,
                                          mod.lfoAttack,
                                          mod.lfoGlobal && globalPhase ? &globalPhase[t]
                                                                       : nullptr);
            }
            modulation[t] = value;
        }

        // ── Pitch, filter and pan, held across the sub-block ──
        const double semitones = keySemitones + settings.pitchSemitones +
                                 modulation[std::uint32_t(ModTarget::Pitch)] *
                                     settings.pitchRange;
        const double pan = std::clamp(
            settings.pan + double(m_notePan) +
                modulation[std::uint32_t(ModTarget::Pan)],
            -1.0, 1.0);
        const double angle = (pan + 1.0) * kPi * 0.25;
        const double gainLeft = std::cos(angle);
        const double gainRight = std::sin(angle);

        const double cut = std::clamp(settings.modX + modulation[std::uint32_t(ModTarget::Cutoff)],
                                      0.0, 1.0);
        const double res = std::clamp(settings.modY + modulation[std::uint32_t(ModTarget::Resonance)],
                                      0.0, 1.0);
        const bool filtering = cut < 0.999 || res > 0.001;
        if (filtering) m_filter.setCoefficients(cutoffHz(cut), res, sampleRate);

        const double volumeMod =
            std::clamp(1.0 + modulation[std::uint32_t(ModTarget::Volume)], 0.0, 2.0);
        const double staticGain = settings.volume * double(m_velocity) * volumeMod;

        // ── Source ──
        engine::FrameCount produced = 0;
        if (settings.stretchMode != 0) {
            const double pitchRatio =
                rateScale * semitonesToRatio(semitones + settings.stretchPitch);
            const double timeRate = rateScale / std::max(settings.stretchTime, 0.01);
            double grainLength = kDefaultGrainLength;
            switch (settings.stretchMode) {
                case 1: grainLength = 384.0; break;    // transient priority
                case 2: grainLength = 1024.0; break;   // loop/body priority
                case 3: grainLength = 2048.0; break;   // periodic voice body
                case 4: grainLength = 4096.0; break;   // dense/low-frequency phase
                default: break;
            }
            produced = fillGranular(sample, settings, region, sourceLeft, sourceRight,
                                    count, pitchRatio, timeRate, grainLength);
        } else {
            produced = fillResampled(sample, settings, region, sourceLeft, sourceRight,
                                     count, rateScale * semitonesToRatio(semitones));
        }

        // ── Amplitude, filter, pan ──
        float* outLeft = out[0];
        float* outRight = channels > 1 ? out[1] : nullptr;

        // Formant coefficients depend on the sample rate and one knob, so they
        // are resolved here rather than inside the sample loop — an exp() and a
        // tanh() per sample per voice is a transcendental bill nobody ordered.
        // Every mode, not only the granular ones. The control is a spectral
        // tilt — a filter on whatever the source stage produced — and there was
        // never a technical reason it could not run after a plain resample.
        // Gating it left a knob sitting there doing nothing in the mode the
        // sampler starts in, which reads as a broken control, not as a hint.
        const bool shifting = std::abs(settings.formant) > 0.001;
        const double formantPole = std::exp(-2.0 * kPi * 1200.0 / sampleRate);
        const double formantTilt = std::tanh(settings.formant / 12.0);

        for (engine::FrameCount i = 0; i < produced; ++i) {
            double gain = staticGain;
            if (settings.ampEnvOn) gain *= m_amp.advance(1.0 / sampleRate, settings.ampEnv);
            if (m_cutRemaining > 0) {
                gain *= double(m_cutRemaining) / double(m_cutLength);
                --m_cutRemaining;
            }

            float l = sourceLeft[i];
            float r = sourceRight[i];
            if (shifting) {
                // Realtime spectral-envelope preview. Both channels use the
                // same coefficient, preserving their phase relationship while
                // the low/high balance follows the formant control.
                auto shift = [&](int channel, float input) {
                    m_formantLow[channel] = formantPole * m_formantLow[channel] +
                                            (1.0 - formantPole) * input;
                    const double low = m_formantLow[channel];
                    const double high = double(input) - low;
                    return float(low * (1.0 - 0.45 * formantTilt) +
                                 high * (1.0 + 0.75 * formantTilt));
                };
                l = shift(0, l);
                r = shift(1, r);
            }
            if (filtering) {
                l = m_filter.processLowpass(0, l);
                r = m_filter.processLowpass(1, r);
            }

            outLeft[done + i] += float(double(l) * gain * gainLeft);
            if (outRight) {
                outRight[done + i] += float(double(r) * gain * gainRight);
            } else {
                // A mono output still hears both channels of a stereo sample.
                outLeft[done + i] += float(double(r) * gain * gainRight);
            }
        }

        done += count;

        // Three ways a voice ends: the sample ran out, the amplitude envelope
        // finished its release, or the short cut ramp completed.
        const bool sampleEnded = produced < count;
        const bool envelopeEnded = settings.ampEnvOn && !m_amp.active();
        const bool cutEnded = m_cutRemaining == 0;
        if (sampleEnded || envelopeEnded || cutEnded) {
            m_active = false;
            m_cutRemaining = -1;
            return;
        }
    }
}

} // namespace daw::plugins::sampler
