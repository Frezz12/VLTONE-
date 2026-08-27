#pragma once

#include "Common/RealtimeSnapshot.hpp"
#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"
#include "Nodes/MeterNode.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace daw::engine {

/// Sums every input. Buses, groups, folders and the master are all this node —
/// routing shape lives in the graph, not in a class hierarchy.
class SumNode : public Node {
public:
    explicit SumNode(std::string name = "Sum") : m_name(std::move(name)) {}

    std::string_view name() const noexcept override { return m_name; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    void process(const ProcessContext& context) override {
        dsp::sumInto(context.output, context.inputs);
    }

private:
    std::string m_name;
};

/// One automated level, as the audio thread sees it: already in the target's
/// own units, already broken into straight pieces.
///
/// The shaping and the taper are done once, on the control thread, while the
/// graph is compiled — the same bargain the plugin automation path strikes. A
/// node that understood curve shapes would be evaluating `std::pow` for every
/// channel on every block to produce a number the ramp below then straightens
/// anyway.
struct LevelCurve {
    /// (beat, value), sorted. Beats are timeline-absolute.
    std::vector<std::pair<double, double>> points;
    /// Held before the first point — never the first point's value, which is
    /// the rule the whole application plays by.
    double defaultValue = 0.0;
    /// False when nothing automates this level, which is the common case and
    /// must cost nothing.
    bool active = false;
};

/// A channel's automated volume, pan and mute.
struct LevelAutomation {
    LevelCurve gain;
    LevelCurve pan;
    LevelCurve mute;
};

/// Walk a curve forward to `beats` and read it. `cursor` is kept between blocks
/// so an ordinary block costs a comparison rather than a search; the caller
/// resets it when the playhead jumps backwards.
inline double levelAt(const LevelCurve& curve, double beats,
                      std::size_t& cursor) noexcept {
    const auto& points = curve.points;
    if (points.empty()) return curve.defaultValue;
    if (beats < points.front().first) return curve.defaultValue;
    if (beats >= points.back().first) return points.back().second;

    std::size_t i = std::min(cursor, points.size() - 1);
    while (i + 1 < points.size() && points[i + 1].first <= beats) ++i;
    cursor = i;
    if (i + 1 >= points.size()) return points[i].second;

    const double span = points[i + 1].first - points[i].first;
    if (!(span > 0.0)) return points[i + 1].second;
    const double t = (beats - points[i].first) / span;
    return points[i].second + (points[i + 1].second - points[i].second) * t;
}

/// Gain + pan, with a per-block ramp so parameter moves don't zipper. This is
/// the channel fader; mute and solo are just gain going to zero.
class GainNode : public Node {
public:
    explicit GainNode(std::string name = "Gain") : m_name(std::move(name)) {}

    std::string_view name() const noexcept override { return m_name; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    /// Control thread; the audio thread reads the target atomically.
    void setGain(float gain) noexcept { m_targetGain.store(gain, std::memory_order_relaxed); }
    void setPan(float pan) noexcept { m_targetPan.store(pan, std::memory_order_relaxed); }
    /// Manual mute/solo gate, deliberately separate from the static fader
    /// value. A volume automation curve must be able to raise a track whose
    /// stored fader happens to be at zero without bypassing mute or solo.
    void setSilent(bool silent) noexcept {
        m_silent.store(silent, std::memory_order_relaxed);
    }
    /// When on, the track's inputs are summed to a single mono signal and spread
    /// to every output channel — a stereo clip is heard as mono.
    void setMono(bool mono) noexcept { m_mono.store(mono, std::memory_order_relaxed); }
    /// Publish a new automation snapshot. Control thread; the audio thread
    /// picks it up on its next block and notices the change through the pointer
    /// identity, which is what tells it to drop its cursors.
    void setAutomation(std::shared_ptr<const LevelAutomation> automation) {
        m_automation.publish(std::move(automation));
    }
    float gain() const noexcept { return m_targetGain.load(std::memory_order_relaxed); }
    bool mono() const noexcept { return m_mono.load(std::memory_order_relaxed); }

    void prepare(const PrepareInfo& info) override {
        // Scratch for the mono fold. Allocated here so the audio thread never
        // has to; the mono path used to work sample-by-sample precisely because
        // it had nowhere to put an intermediate result.
        m_monoScratch.assign(info.maxBlockSize, 0.0f);
    }

    void reset() override {
        m_currentGain = m_silent.load(std::memory_order_relaxed)
                            ? 0.0f
                            : m_targetGain.load(std::memory_order_relaxed);
        m_currentPan = m_targetPan.load(std::memory_order_relaxed);
    }

    void process(const ProcessContext& context) override {
        float targetGain = m_targetGain.load(std::memory_order_relaxed);
        float targetPan = m_targetPan.load(std::memory_order_relaxed);
        const bool mono = m_mono.load(std::memory_order_relaxed);
        const ChannelCount outChannels = context.output.numChannels();

        // ── Automation ──
        //
        // The node already ramps from where it was to where it is going across
        // the block. Automation simply supplies both ends instead of one, so a
        // curve is played at block-rate interpolation with no new machinery —
        // and with better resolution than a value that only changes on block
        // boundaries.
        auto reader = m_automation.read();
        if (const LevelAutomation* automation = reader.get()) {
            if (automation != m_automationFor ||
                context.transport.ppqPosition < m_lastBlockBeats) {
                // A new snapshot, or the playhead went backwards — a seek or a
                // cycle wrap. The forward-only cursors are meaningless now.
                m_gainCursor = m_panCursor = m_muteCursor = 0;
                m_automationFor = automation;
            }
            const double startBeats = context.transport.ppqPosition;
            const double endBeats =
                startBeats + beatsIn(context.frames, context);
            m_lastBlockBeats = startBeats;

            if (automation->gain.active) {
                m_currentGain = float(levelAt(automation->gain, startBeats,
                                              m_gainCursor));
                std::size_t cursor = m_gainCursor;
                targetGain = float(levelAt(automation->gain, endBeats, cursor));
            }
            if (automation->pan.active) {
                m_currentPan = float(levelAt(automation->pan, startBeats,
                                             m_panCursor));
                std::size_t cursor = m_panCursor;
                targetPan = float(levelAt(automation->pan, endBeats, cursor));
            }
            if (automation->mute.active) {
                // A switch: the block is muted or it is not. Ramping between
                // the two would put the fader through levels the user never
                // drew, and the mute curve is stepped for exactly that reason.
                const double muted =
                    levelAt(automation->mute, startBeats, m_muteCursor);
                if (muted >= 0.5) {
                    m_currentGain = 0.0f;
                    targetGain = 0.0f;
                }
            }
        }

        // Manual mute and solo stay a gate over the top: a soloed-away track
        // is silent whatever its curve says. This cannot be inferred from a
        // zero static gain — zero is a legitimate fader position automation
        // must be able to leave.
        if (m_silent.load(std::memory_order_relaxed)) {
            m_currentGain = 0.0f;
            targetGain = 0.0f;
        }

        if (mono && outChannels >= 2 && context.frames <= m_monoScratch.size()) {
            // Fold every input down once into scratch, then spread that one
            // signal across the outputs. Folding per output channel — or worse,
            // per sample — repeated the same sum for every channel and put a
            // pan-law evaluation inside the innermost loop.
            const std::span<float> folded(m_monoScratch.data(), context.frames);
            dsp::clear(folded);
            for (const AudioBlock& input : context.inputs) {
                const ChannelCount inChannels = input.numChannels();
                if (inChannels == 0) continue;
                const float norm = 1.0f / float(inChannels);
                for (ChannelCount ch = 0; ch < inChannels; ++ch) {
                    dsp::addScaled(folded, input.channel(ch), norm);
                }
            }
            for (ChannelCount ch = 0; ch < outChannels; ++ch) {
                const float startGain = m_currentGain * panGain(ch, m_currentPan);
                const float endGain = targetGain * panGain(ch, targetPan);
                if (startGain == endGain) {
                    dsp::copyScaled(context.output.channel(ch), folded, endGain);
                } else {
                    dsp::copyScaledRamp(context.output.channel(ch), folded,
                                        startGain, endGain);
                }
            }
            m_currentGain = targetGain;
            m_currentPan = targetPan;
            return;
        }

        // Channel-outer, so the pan law is evaluated once per channel per block
        // instead of once per input, and the destination stays in cache while
        // its contributors are summed into it.
        for (ChannelCount ch = 0; ch < outChannels; ++ch) {
            const float startGain = m_currentGain * panGain(ch, m_currentPan);
            const float endGain = targetGain * panGain(ch, targetPan);
            const std::span<float> destination = context.output.channel(ch);
            const bool ramping = startGain != endGain;
            bool written = false;
            for (const AudioBlock& input : context.inputs) {
                if (ch >= input.numChannels()) continue;
                const std::span<const float> source = input.channel(ch);
                if (written) {
                    ramping ? dsp::addScaledRamp(destination, source, startGain, endGain)
                            : dsp::addScaled(destination, source, endGain);
                } else {
                    ramping ? dsp::copyScaledRamp(destination, source, startGain, endGain)
                            : dsp::copyScaled(destination, source, endGain);
                    written = true;
                }
            }
            if (!written) dsp::clear(destination);
        }
        m_currentGain = targetGain;
        m_currentPan = targetPan;
    }

private:
    /// Stereo balance law: centre passes both sides at unity and panning
    /// attenuates the far side. A constant-power law would drop a centred
    /// channel by 3 dB, and stacking that on every track and the master makes
    /// the whole mix quieter than the material.
    static float panGain(ChannelCount channel, float pan) noexcept {
        if (channel > 1) return 1.0f;
        if (channel == 0) return pan <= 0.0f ? 1.0f : 1.0f - pan;
        return pan >= 0.0f ? 1.0f : 1.0f + pan;
    }

    /// How many beats one block covers at this tempo and rate. The rate is the
    /// context's — `TransportInfo` carries musical time only.
    static double beatsIn(FrameCount frames, const ProcessContext& context) noexcept {
        const double samplesPerBeat =
            context.sampleRate * 60.0 / std::max(1.0, context.transport.tempo);
        return samplesPerBeat > 0.0 ? double(frames) / samplesPerBeat : 0.0;
    }

    std::string m_name;
    std::atomic<float> m_targetGain{1.0f};
    std::atomic<float> m_targetPan{0.0f};
    std::atomic<bool> m_silent{false};
    std::atomic<bool> m_mono{false};
    float m_currentGain = 1.0f;
    float m_currentPan = 0.0f;
    std::vector<float> m_monoScratch;

    RealtimeSnapshot<LevelAutomation> m_automation;
    const LevelAutomation* m_automationFor = nullptr;
    std::size_t m_gainCursor = 0;
    std::size_t m_panCursor = 0;
    std::size_t m_muteCursor = 0;
    double m_lastBlockBeats = 0.0;
};

/// Taps its input at a scaled level — the send. Its output goes to whatever bus
/// the graph connects it to, pre or post fader depending on where it is wired.
class SendNode : public Node {
public:
    explicit SendNode(std::string name = "Send") : m_name(std::move(name)) {}
    std::string_view name() const noexcept override { return m_name; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    void setLevel(float level) noexcept { m_level.store(level, std::memory_order_relaxed); }
    /// A curve for the send amount, on the same terms as `GainNode`'s.
    void setAutomation(std::shared_ptr<const LevelCurve> curve) {
        m_automation.publish(std::move(curve));
    }

    void process(const ProcessContext& context) override {
        float level = m_level.load(std::memory_order_relaxed);
        auto reader = m_automation.read();
        if (const LevelCurve* curve = reader.get(); curve && curve->active) {
            if (curve != m_automationFor ||
                context.transport.ppqPosition < m_lastBlockBeats) {
                m_cursor = 0;
                m_automationFor = curve;
            }
            m_lastBlockBeats = context.transport.ppqPosition;
            // One value for the block. A send is a tap, not the signal path —
            // its own ramp would only smooth what the destination bus already
            // smooths, and a disabled send is still held at zero by the level
            // the control thread pushed.
            if (level > 0.0f) {
                level = float(levelAt(*curve, context.transport.ppqPosition,
                                      m_cursor));
            }
        }
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            const std::span<float> destination = context.output.channel(ch);
            bool written = false;
            for (const AudioBlock& input : context.inputs) {
                if (ch >= input.numChannels()) continue;
                if (written) {
                    dsp::addScaled(destination, input.channel(ch), level);
                } else {
                    // One pass for the single-input case, which is every send
                    // that is not fed by a group.
                    dsp::copyScaled(destination, input.channel(ch), level);
                    written = true;
                }
            }
            if (!written) dsp::clear(destination);
        }
    }

private:
    std::string m_name;
    std::atomic<float> m_level{0.5f};

    RealtimeSnapshot<LevelCurve> m_automation;
    const LevelCurve* m_automationFor = nullptr;
    std::size_t m_cursor = 0;
    double m_lastBlockBeats = 0.0;
};

/// A cascade of biquad sections — the EQ node, and a realistic unit of DSP load
/// for scheduling measurements. State is per channel and per section, kept in
/// one contiguous block so a channel's coefficients and state share cache lines.
class BiquadNode : public Node {
public:
    BiquadNode(std::string name, int sections = 4)
        : m_name(std::move(name)), m_sections(sections) {}

    std::string_view name() const noexcept override { return m_name; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    /// Peaking EQ at `frequency` with gain in dB, applied to every section.
    void setPeaking(double frequency, double q, double gainDb) noexcept {
        m_frequency.store(frequency, std::memory_order_relaxed);
        m_q.store(q, std::memory_order_relaxed);
        m_gainDb.store(gainDb, std::memory_order_relaxed);
        m_coefficientsDirty.store(true, std::memory_order_release);
    }

    void prepare(const PrepareInfo& info) override {
        m_sampleRate = info.sampleRate;
        m_state.assign(std::size_t(m_sections) * info.channels * 4, 0.0f);
        m_channels = info.channels;
        m_coefficientsDirty.store(true, std::memory_order_release);
    }

    void reset() override { std::fill(m_state.begin(), m_state.end(), 0.0f); }

    void process(const ProcessContext& context) override {
        if (m_coefficientsDirty.exchange(false, std::memory_order_acq_rel)) {
            updateCoefficients(context.sampleRate > 0.0 ? context.sampleRate
                                                        : m_sampleRate);
        }
        dsp::sumInto(context.output, context.inputs);

        for (ChannelCount ch = 0; ch < std::min(m_channels, context.output.numChannels());
             ++ch) {
            float* samples = context.output.data(ch);
            for (int section = 0; section < m_sections; ++section) {
                float* state = &m_state[(std::size_t(section) * m_channels + ch) * 4];
                for (FrameCount i = 0; i < context.frames; ++i) {
                    // Direct Form II transposed: two states, no history buffer.
                    const float in = samples[i];
                    const float out = m_b0 * in + state[0];
                    state[0] = m_b1 * in - m_a1 * out + state[1];
                    state[1] = m_b2 * in - m_a2 * out;
                    samples[i] = out;
                }
            }
        }
    }

private:
    void updateCoefficients(SampleRate sampleRate) noexcept {
        const double frequency = m_frequency.load(std::memory_order_relaxed);
        const double q = std::max(m_q.load(std::memory_order_relaxed), 1e-6);
        const double gainDb = m_gainDb.load(std::memory_order_relaxed);
        const double a = std::pow(10.0, gainDb / 40.0);
        const double omega = 2.0 * 3.14159265358979 * frequency / sampleRate;
        const double sn = std::sin(omega);
        const double cs = std::cos(omega);
        const double alpha = sn / (2.0 * q);

        const double b0 = 1.0 + alpha * a;
        const double b1 = -2.0 * cs;
        const double b2 = 1.0 - alpha * a;
        const double a0 = 1.0 + alpha / a;
        const double a1 = -2.0 * cs;
        const double a2 = 1.0 - alpha / a;

        m_b0 = float(b0 / a0);
        m_b1 = float(b1 / a0);
        m_b2 = float(b2 / a0);
        m_a1 = float(a1 / a0);
        m_a2 = float(a2 / a0);
    }

    std::string m_name;
    int m_sections = 4;
    ChannelCount m_channels = 2;
    SampleRate m_sampleRate = 48000.0;
    std::atomic<double> m_frequency{1000.0};
    std::atomic<double> m_q{0.707};
    std::atomic<double> m_gainDb{3.0};
    std::atomic<bool> m_coefficientsDirty{true};
    float m_b0 = 1.0f, m_b1 = 0.0f, m_b2 = 0.0f, m_a1 = 0.0f, m_a2 = 0.0f;
    std::vector<float> m_state;
};

/// Declares a processing latency without changing the signal — a stand-in for a
/// plugin until the host lands, and the node the PDC tests drive.
class LatencyNode : public Node {
public:
    LatencyNode(std::string name, FrameCount latency)
        : m_name(std::move(name)), m_latency(latency) {}

    std::string_view name() const noexcept override { return m_name; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }
    FrameCount latencySamples() const noexcept override { return m_latency; }

    void prepare(const PrepareInfo& info) override {
        m_history.assign(std::size_t(m_latency + info.maxBlockSize + 1) *
                             info.channels, 0.0f);
        m_stride = std::size_t(m_latency) + info.maxBlockSize + 1;
        m_channels = info.channels;
        m_writePos = 0;
    }

    void process(const ProcessContext& context) override {
        dsp::sumInto(context.output, context.inputs);
        if (m_latency == 0 || m_stride == 0) return;

        // Actually delay the signal by the latency we advertise, so the tests
        // measure real misalignment rather than a bookkeeping number.
        for (ChannelCount ch = 0; ch < m_channels; ++ch) {
            float* ring = m_history.data() + std::size_t(ch) * m_stride;
            float* signal = context.output.data(ch);
            std::size_t write = m_writePos;
            std::size_t read = (write + m_stride - m_latency) % m_stride;
            for (FrameCount i = 0; i < context.frames; ++i) {
                ring[write] = signal[i];
                signal[i] = ring[read];
                if (++write >= m_stride) write = 0;
                if (++read >= m_stride) read = 0;
            }
            if (ch + 1 == m_channels) m_writePos = write;
        }
    }

private:
    std::string m_name;
    FrameCount m_latency = 0;
    std::vector<float> m_history;
    std::size_t m_stride = 0;
    std::size_t m_writePos = 0;
    ChannelCount m_channels = 2;
};

/// Generates a signal from a callback — the seam clip playback, instruments and
/// the test tone all plug into.
class SourceNode : public Node {
public:
    using Renderer = void (*)(void* context, const AudioBlock& output,
                              FrameCount frames, SamplePos position);

    SourceNode(std::string name, Renderer renderer, void* context)
        : m_name(std::move(name)), m_renderer(renderer), m_context(context) {}

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    void process(const ProcessContext& context) override {
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            dsp::clear(context.output.channel(ch));
        }
        if (m_renderer) {
            m_renderer(m_context, context.output, context.frames,
                       context.timelinePosition);
        }
    }

private:
    std::string m_name;
    Renderer m_renderer = nullptr;
    void* m_context = nullptr;
};

} // namespace daw::engine
