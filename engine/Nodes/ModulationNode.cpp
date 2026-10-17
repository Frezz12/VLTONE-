#include "Nodes/ModulationNode.hpp"
#include <cmath>

namespace daw::engine {
void ModulationNode::reset() {
    m_envelope = m_freePhase = 0.0; m_smoothed = 0.5;
    m_low = {}; m_high = {}; m_initialized = m_wasPlaying = false;
    m_value.store(0.0f, std::memory_order_relaxed);
}
void ModulationNode::process(const ProcessContext& c) {
    const auto settings = m_settings.read();
    for (ChannelCount ch = 0; ch < c.output.numChannels(); ++ch)
        std::fill_n(c.output.data(ch), c.frames, 0.0f);
    if (!settings || !c.output.numChannels() || c.sampleRate <= 0.0) return;
    const auto& s = settings->source;
    const auto coefficient = [&](double ms) { return ms <= 0.0 ? 0.0 : std::exp(-1000.0 / (ms * c.sampleRate)); };
    const double attack = coefficient(s.attackMs), release = coefficient(s.releaseMs);
    const double smooth = coefficient(s.smoothMs), gain = std::pow(10.0, s.gainDb / 20.0);
    const double low = std::exp(-6.283185307179586 * std::min(s.lowHz, c.sampleRate * 0.45) / c.sampleRate);
    const double high = std::exp(-6.283185307179586 * std::min(s.highHz, c.sampleRate * 0.49) / c.sampleRate);
    const bool discontinuity = !m_initialized || (c.playing && (!m_wasPlaying || c.timelinePosition != m_next));
    if (discontinuity && s.kind == modulation::Kind::Follower) { m_envelope = 0; m_low = {}; m_high = {}; }
    double previousBeat = c.transport.ppqPosition;
    for (FrameCount frame = 0; frame < c.frames; ++frame) {
        const double beat = c.playing ? c.ppqAtOffset(frame) : c.transport.ppqPosition;
        const bool wrapped = c.playing && (beat < previousBeat ||
            (frame == 0 && c.transport.looping && std::abs(beat - c.transport.loopStartPpq) < 1e-10));
        previousBeat = beat;
        if (wrapped && s.kind == modulation::Kind::Follower) { m_envelope = 0; m_low = {}; m_high = {}; }
        double value = 0.0;
        if (s.kind == modulation::Kind::Follower) {
            for (ChannelCount ch = 0; ch < 2; ++ch) {
                double sample = 0.0;
                for (const auto& input : c.inputs) if (ch < input.numChannels()) sample += input.data(ch)[frame];
                if (!std::isfinite(sample)) sample = 0.0;
                if (s.filter) {
                    m_low[ch] = low * m_low[ch] + (1.0 - low) * sample;
                    sample -= m_low[ch];
                    m_high[ch] = high * m_high[ch] + (1.0 - high) * sample;
                    sample = m_high[ch];
                }
                value = std::max(value, std::abs(sample) * gain);
            }
            const double a = value > m_envelope ? attack : release;
            m_envelope = a * m_envelope + (1.0 - a) * value;
            value = std::clamp(m_envelope, 0.0, 1.0);
        } else {
            // Synced oscillators freeze at the cursor while stopped.
            double phase = 0.0;
            if (s.sync) phase = settings->rate.integral(beat);
            else if (c.playing || c.offline) {
                const double unwrappedBeat = c.transport.ppqPosition + frame * c.transport.tempo / (c.sampleRate * 60.0);
                const double seconds = (double(c.timelinePosition) + frame) / c.sampleRate +
                    (beat - unwrappedBeat) * 60.0 / std::max(1.0, c.transport.tempo);
                phase = settings->rate.points.empty()
                    ? seconds * settings->rate.fallback
                    : settings->rate.integral(beat) * 60.0 / std::max(1.0, c.transport.tempo);
                m_freePhase = phase;
            } else {
                phase = m_freePhase;
                m_freePhase += settings->rate.at(beat) / c.sampleRate;
            }
            value = modulation::waveValue(s.wave, phase + s.phase);
            if ((discontinuity && frame == 0) || wrapped || s.smoothMs == 0) m_smoothed = value;
            else m_smoothed = smooth * m_smoothed + (1.0 - smooth) * value;
            value = m_smoothed;
        }
        c.output.data(0)[frame] = float(value);
    }
    if (c.frames) m_value.store(c.output.data(0)[c.frames - 1], std::memory_order_relaxed);
    m_initialized = true; m_wasPlaying = c.playing;
    m_next = c.timelinePosition + (c.playing ? c.frames : 0);
    if (c.playing && !c.offline && c.transport.looping && c.transport.tempo > 0) {
        const double unwrappedBeat = c.transport.ppqPosition + c.frames * c.transport.tempo / (c.sampleRate * 60.0);
        m_next += SamplePos(std::llround((c.ppqAtOffset(c.frames) - unwrappedBeat) * c.sampleRate * 60.0 / c.transport.tempo));
    }
}
} // namespace daw::engine
