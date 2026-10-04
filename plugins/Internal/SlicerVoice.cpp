#include "Internal/SlicerVoice.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw::plugins::slicer {
namespace {

/// 0…1 knob → 20 Hz … 20 kHz, the range a filter knob is expected to sweep.
double cutoffHz(double knob) noexcept {
    return 20.0 * std::pow(1000.0, std::clamp(double(knob), 0.0, 1.0));
}

double semitonesToRatio(double semitones) noexcept {
    return std::pow(2.0, semitones / 12.0);
}

/// Frames of taper at the chop's far end. Bounded both ways: long enough to
/// kill the click where a chop stops on a waveform that is still swinging, and
/// short enough that a ten-frame chop keeps most of its length. The *front* of
/// a chop never gets one — that is the transient, and the analysis already
/// left a pre-attack margin of quiet audio in front of it.
double fadeOutFrames(double span, double sourceRate) noexcept {
    return std::min(sourceRate * 0.003, span * 0.25);
}

} // namespace

float Voice::readSample(const engine::SampleBuffer& audio,
                        engine::ChannelCount channel, double position) noexcept {
    const engine::FrameCount frames = audio.frames();
    if (frames == 0) return 0.0f;
    const double clamped = std::clamp(position, 0.0, double(frames - 1));
    const std::int64_t i = std::int64_t(clamped);
    const float fraction = float(clamped - double(i));

    const auto at = [&](std::int64_t index) -> float {
        const auto value = audio.readSample(channel, engine::FrameCount(
            std::clamp<std::int64_t>(index, 0, std::int64_t(frames) - 1)));
        return std::isfinite(value) ? value : 0.0f;
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

bool Voice::start(const Slice& slice, int key, int channel, float velocity,
                  float notePan, const SlicerSettings& settings,
                  const SampleData& sample, double sampleRate,
                  std::uint32_t sliceIndex, std::int32_t noteId) noexcept {
    m_active = false;
    if (!sample.audio || sample.audio->frames() == 0) return false;
    if (slice.flags & kSliceMuted) return false;

    const engine::SampleBuffer& audio = *sample.audio;
    const double total = double(audio.frames());
    const double sourceRate = audio.sampleRate() > 0.0 ? audio.sampleRate()
                                                       : sampleRate;

    const double sliceStart = std::clamp(double(slice.start), 0.0, total);
    const double sliceEnd = std::clamp(double(slice.end), 0.0, total);
    if (sliceEnd - sliceStart < 1.0) return false;

    // Gate limits how much of the chop a held note may play. It trims from the
    // end going forwards and from the start going backwards, so the chop's own
    // onset — the reason it is a chop at all — is what the gate always keeps.
    const double span = sliceEnd - sliceStart;
    const double gate = std::clamp(settings.gate, 0.0, 1.0);
    double played = gate * span;
    if (played < 1.0) played = std::min(1.0, span);

    m_forward = (slice.flags & kSliceReverse) == 0;
    m_loop = slice.loopMode > 0 || (slice.flags & kSliceLoop) != 0;
    m_pingpong = slice.loopMode == 2 && played > 1.0;
    m_oneShot = settings.oneShot;
    m_start = m_forward ? sliceStart : sliceEnd - played;
    m_end = m_forward ? sliceStart + played : sliceEnd;
    m_fadeOut = m_loop ? 0.0 : (slice.fadeOutMs < 0 ? fadeOutFrames(played,sourceRate) : std::min(played,double(slice.fadeOutMs)*sourceRate*0.001));
    m_fadeIn=std::min(played,double(slice.fadeInMs)*sourceRate*0.001);
    m_crossfade=m_loop && !m_pingpong ? std::min(played*0.49,double(slice.crossfadeMs)*sourceRate*0.001) : 0.0;
    m_travel=0.0;

    // Key Track is what makes a chop transpose at all: with the knob at zero
    // the chop sounds as recorded, at one every octave played is an octave
    // heard. Nothing during the note changes it, so the whole thing resolves
    // once here rather than per sample.
    const double rateScale=sourceRate/std::max(sampleRate,1.0);
    m_step=rateScale;
    m_key=key; m_sliceId=slice.id; m_velocity=std::clamp(double(velocity),0.0,1.0); m_notePan=notePan;
    m_filterDsp.reset();
    updateSound(slice,settings,sampleRate);
    m_effectX=m_effectXTarget; m_effectY=m_effectYTarget; m_effectMix=m_effectMixTarget;
    m_effectWeights.fill(0.0);
    if (m_effect > 0) m_effectWeights[m_effect-1]=1.0;
    std::fill_n(m_toneState,2,0.0); std::fill_n(m_crushHeld,2,0.0);
    m_ringPhase=0.0; m_crushCounter=0;
    m_cutoff = m_cutoffTarget; m_resonance = m_resonanceTarget;
    m_filterDsp.setCoefficients(cutoffHz(m_cutoff), m_resonance, sampleRate);
    m_tune=m_tuneTarget; m_gain=m_gainTarget; m_gainLeft=m_leftTarget; m_gainRight=m_rightTarget;
    m_filterMix=m_filter>0 ? 1.0 : 0.0; m_filterBlend=1.0;
    m_envelopeSettings = m_globalEnvelope ? settings.ampEnv : m_localEnvelope;
    const double step=rateScale*semitonesToRatio(m_tune);
    m_position=m_forward ? m_start : std::max(m_start,m_end-step);

    m_key = key;
    m_channel = channel;
    m_noteId = noteId;
    m_sliceIndex = sliceIndex;
    m_chokeGroup = slice.chokeGroup;

    m_cutLength = std::max(1, int(sampleRate * 0.005));
    m_cutRemaining = -1;
    m_bend = 0.0;
    m_bendTarget = 0.0;
    m_bendSmoothingMs = 8.0;

    // `noteOn` resets every field the envelope actually reads, so a reused
    // voice needs nothing else cleared.
    m_amp.noteOn();
    m_active = true;
    return true;
}

void Voice::updateSound(const Slice& s, const SlicerSettings& settings, double sampleRate) noexcept {
    m_tuneTarget=double(s.transpose)+s.fineTune/100.0+settings.transpose+settings.keyTrack*double(m_key-settings.rootNote);
    const double depth=std::clamp(settings.velocityDepth,0.0,1.0);
    m_gainTarget=double(s.gain)*s.normalization*(1.0-depth+depth*m_velocity);
    const double pan=std::clamp(m_notePan+double(s.pan)+settings.pan,-1.0,1.0);
    m_leftTarget=pan<=0?1.0:1.0-pan; m_rightTarget=pan>=0?1.0:1.0+pan;
    m_globalEnvelope=s.useGlobalEnvelope;
    m_localEnvelope.attack=s.attack; m_localEnvelope.decay=s.decay;
    m_localEnvelope.sustain=s.sustain; m_localEnvelope.release=std::max(0.003,double(s.release));
    if (m_active && m_filter != s.filter) {
        m_previousFilter = m_filter; m_previousFilterDsp = m_filterDsp; m_filterBlend = 0.0;
        if (m_filter == 0) m_filterDsp.reset();
    }
    m_filter=s.filter; m_cutoffTarget=s.cutoff; m_resonanceTarget=s.resonance;
    m_effect=std::min<std::uint8_t>(s.effect,3);
    const auto unit=[](float v, double fallback) { return std::isfinite(v) ? std::clamp(double(v),0.0,1.0) : fallback; };
    m_effectXTarget=unit(s.effectX,.5); m_effectYTarget=unit(s.effectY,.5); m_effectMixTarget=unit(s.effectMix,1.0);
    if (m_active && (s.flags & kSliceMuted)) choke();
}

void Voice::release() noexcept {
    if (!m_active || m_amp.released()) return;
    m_amp.noteOff();
}

void Voice::choke() noexcept {
    if (!m_active) return;
    // Repeated triggers must not restart a tail that is already fading out.
    if (m_cutRemaining < 0) {
        m_amp.noteOff();
        m_cutRemaining = m_cutLength;
    }
}

void Voice::kill() noexcept {
    m_active = false;
    m_amp.kill();
    m_cutRemaining = -1;
}

void Voice::setBend(double semitones, double smoothingMs) noexcept {
    m_bendTarget = semitones;
    m_bendSmoothingMs = smoothingMs > 0.0 ? smoothingMs : 8.0;
}

void Voice::render(const SampleData& sample, const SlicerSettings& settings,
                   float* const* out, engine::ChannelCount channels,
                   engine::FrameCount frames, double sampleRate) noexcept {
    if (!m_active || !sample.audio || sample.audio->frames() == 0 || channels == 0)
        return;

    const engine::SampleBuffer& audio = *sample.audio;
    const bool stereo = audio.channels() > 1;
    float* outLeft = out[0];
    float* outRight = channels > 1 ? out[1] : nullptr;
    const double invRate = 1.0 / std::max(sampleRate, 1.0);
    const double loopLength = m_end - m_start;
    const double smoothing=1.0-std::exp(-invRate/0.005);
    const auto& envelope=m_globalEnvelope ? settings.ampEnv : m_localEnvelope;
    double drive=1.0, toneCoefficient=1.0, levels=1.0, ringStep=0.0;
    int holdFrames=1;

    for (engine::FrameCount i = 0; i < frames; ++i) {
        if (m_cutRemaining == 0) {
            m_active = false;
            break;
        }

        if (m_pingpong) {
            const double length=m_end-m_start-1.0;
            if (m_forward && m_position>=m_end-1.0) {
                const double over=std::fmod(m_position-(m_end-1.0),2.0*length);
                m_forward=over>length; m_position=m_forward ? m_start+over-length : m_end-1.0-over;
            } else if (!m_forward && m_position<m_start) {
                const double over=std::fmod(m_start-m_position,2.0*length);
                m_forward=over<=length; m_position=m_forward ? m_start+over : m_end-1.0-(over-length);
            }
        } else if (m_forward ? m_position >= m_end : m_position < m_start) {
            if (m_loop && loopLength > 0.0) {
                const double period=loopLength-m_crossfade;
                if (m_forward) m_position=m_start+m_crossfade+std::fmod(m_position-m_end,period);
                else m_position=m_end-m_crossfade-std::max(1e-9,std::fmod(m_start-m_position,period));
            } else { m_active=false; break; }
        }
        m_gain+=smoothing*(m_gainTarget-m_gain);
        m_gainLeft+=smoothing*(m_leftTarget-m_gainLeft); m_gainRight+=smoothing*(m_rightTarget-m_gainRight);
        m_tune+=smoothing*(m_tuneTarget-m_tune);
        m_envelopeSettings.attack += smoothing * (envelope.attack - m_envelopeSettings.attack);
        m_envelopeSettings.decay += smoothing * (envelope.decay - m_envelopeSettings.decay);
        m_envelopeSettings.sustain += smoothing * (envelope.sustain - m_envelopeSettings.sustain);
        m_envelopeSettings.release += smoothing * (envelope.release - m_envelopeSettings.release);
        const double env = m_amp.advance(invRate, m_envelopeSettings);
        double gain = m_gain * env;
        if (m_cutRemaining > 0) {
            gain *= double(m_cutRemaining) / double(m_cutLength);
            --m_cutRemaining;
        }

        // The taper is measured against whichever end the chop is heading for,
        // so a reversed chop gets it on its own stopping side.
        double taper = 1.0;
        if (m_fadeOut > 0.0) {
            const double left = m_forward ? m_end - m_position
                                          : m_position - m_start;
            if (left < m_fadeOut) taper = std::clamp(left / m_fadeOut, 0.0, 1.0);
        }

        if (m_fadeIn>0.0) taper*=std::clamp(m_travel/m_fadeIn,0.0,1.0);
        float left = readSample(audio, 0, m_position);
        float right = stereo ? readSample(audio, 1, m_position) : left;
        if (m_crossfade>0.0) {
            const double distance=m_forward ? m_end-m_position : m_position-m_start;
            if (distance<m_crossfade) {
                const double blend=std::clamp(1.0-distance/m_crossfade,0.0,1.0);
                const double other=m_forward ? m_start+(m_crossfade-distance) : m_end-1.0-(m_crossfade-distance);
                left=float(left*(1.0-blend)+readSample(audio,0,other)*blend);
                right=float(right*(1.0-blend)+readSample(audio,stereo?1:0,other)*blend);
            }
        }
        const float dryLeft=left, dryRight=right;
        if (m_filter > 0 || m_filterBlend < 1.0) {
            m_cutoff += float(smoothing * (m_cutoffTarget - m_cutoff));
            m_resonance += float(smoothing * (m_resonanceTarget - m_resonance));
            if ((i & 31u) == 0) {
                m_filterDsp.setCoefficients(cutoffHz(m_cutoff),m_resonance,sampleRate);
                m_previousFilterDsp.setCoefficients(cutoffHz(m_cutoff),m_resonance,sampleRate);
            }
            const auto filtered = [](sampler::Svf& filter,int mode,int ch,float value) {
                switch(mode) {
                    case 1: return filter.processLowpass(ch,value);
                    case 2: return filter.processHighpass(ch,value);
                    case 3: return filter.processBandpass(ch,value);
                    default: return value;
                }
            };
            const float newLeft=filtered(m_filterDsp,m_filter,0,left),newRight=filtered(m_filterDsp,m_filter,1,right);
            if (m_filterBlend < 1.0) {
                m_filterBlend += smoothing * (1.0-m_filterBlend);
                const float oldLeft=filtered(m_previousFilterDsp,m_previousFilter,0,left),oldRight=filtered(m_previousFilterDsp,m_previousFilter,1,right);
                left=float(oldLeft+(newLeft-oldLeft)*m_filterBlend);right=float(oldRight+(newRight-oldRight)*m_filterBlend);
                if(m_filterBlend>1.0-1e-5) m_filterBlend=1.0;
            } else { left=newLeft;right=newRight; }
        }

        m_effectX+=smoothing*(m_effectXTarget-m_effectX);
        m_effectY+=smoothing*(m_effectYTarget-m_effectY);
        m_effectMix+=smoothing*(m_effectMixTarget-m_effectMix);
        for (int fx=0;fx<3;++fx) {
            const double target=m_effect==fx+1 ? 1.0 : 0.0;
            m_effectWeights[fx]+=smoothing*(target-m_effectWeights[fx]);
            if (std::abs(target-m_effectWeights[fx])<1e-6) m_effectWeights[fx]=target;
        }
        if ((i & 31u)==0) {
            if(m_effectWeights[0]>0) {
                drive=1.0+31.0*m_effectX;
                toneCoefficient=1.0-std::exp(-2.0*std::numbers::pi*std::min(18000.0,250.0*std::pow(72.0,m_effectY))*invRate);
            }
            if(m_effectWeights[1]>0) { levels=std::pow(2.0,23.0-20.0*m_effectX); holdFrames=1+int(std::round(47.0*m_effectY)); }
            if(m_effectWeights[2]>0) ringStep=20.0*std::pow(100.0,m_effectX)*invRate;
        }
        double wet[2]{left,right};
        const double input[2]{left,right};
        if (m_effectWeights[0]>0.0) for (int ch=0;ch<2;++ch) {
            const double saturated=std::tanh(input[ch]*drive)/std::tanh(drive);
            m_toneState[ch]+=toneCoefficient*(saturated-m_toneState[ch]);
            wet[ch]+=m_effectWeights[0]*(m_toneState[ch]-input[ch]);
        }
        if (m_effectWeights[1]>0.0) {
            if (m_crushCounter<=0) {
                for (int ch=0;ch<2;++ch) m_crushHeld[ch]=std::round(input[ch]*levels)/levels;
                m_crushCounter=holdFrames;
            }
            --m_crushCounter;
            for (int ch=0;ch<2;++ch) wet[ch]+=m_effectWeights[1]*(m_crushHeld[ch]-input[ch]);
        } else m_crushCounter=0;
        if (m_effectWeights[2]>0.0) {
            for (int ch=0;ch<2;++ch) {
                const double carrier=std::cos(2.0*std::numbers::pi*m_ringPhase+ch*std::numbers::pi*m_effectY);
                wet[ch]+=m_effectWeights[2]*(input[ch]*carrier-input[ch]);
            }
            m_ringPhase+=ringStep; m_ringPhase-=std::floor(m_ringPhase);
        }
        left=float(dryLeft+(wet[0]-dryLeft)*m_effectMix);
        right=float(dryRight+(wet[1]-dryRight)*m_effectMix);

        const double scaled = gain * taper;
        if (outRight) {
            outLeft[i] += float(double(left) * scaled * m_gainLeft);
            outRight[i] += float(double(right) * scaled * m_gainRight);
        } else {
            // Average both sides for mono without doubling their level.
            outLeft[i] += float((double(left) * m_gainLeft +
                                 double(right) * m_gainRight) * scaled * 0.5);
        }

        // Bend eases in over a few milliseconds so a wheel sweep does not step
        // the pitch in blocks; the chop's base rate never moves, only this does.
        if (m_bend != m_bendTarget) {
            const double coefficient =
                1.0 - std::exp(-invRate / std::max(1e-4, m_bendSmoothingMs * 1e-3));
            m_bend += coefficient * (m_bendTarget - m_bend);
            if (std::abs(m_bendTarget - m_bend) < 1e-4) m_bend = m_bendTarget;
        }

        const double bendStep = m_step * semitonesToRatio(m_bend+m_tune);
        m_position += m_forward ? bendStep : -bendStep;
        m_travel+=bendStep;

        if (!m_amp.active()) {
            m_active = false;
            break;
        }
    }

    if (!m_active) m_cutRemaining = -1;
}

} // namespace daw::plugins::slicer
