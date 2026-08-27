#pragma once

#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <string>

namespace daw::engine {

/// A click track locked to the transport. It is a source node summed into the
/// master, generating a short sine "tick" at every beat — a brighter, louder
/// accent on the bar's downbeat.
///
/// Tempo and time signature come from `context.transport`, not from setters:
/// they used to be pushed in from the control thread, which meant the same
/// numbers lived in two places and a tempo change reached the click and the
/// rest of the graph at different moments. Everything else is derived from the
/// block's timeline position, so the clicks stay sample-accurate however the
/// buffer is chunked.
class MetronomeNode : public Node {
public:
    explicit MetronomeNode(std::string name = "Metronome")
        : m_name(std::move(name)) {}

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    void setEnabled(bool on) noexcept {
        m_enabled.store(on, std::memory_order_relaxed);
    }
    bool enabled() const noexcept {
        return m_enabled.load(std::memory_order_relaxed);
    }

    /// Fire `beats` count-in clicks starting at the next block, at the
    /// transport's tempo and whatever the transport is doing. A count-in has to
    /// be heard with the transport parked — which is exactly the case the
    /// beat-locked clicks above produce nothing for — and it is heard even when
    /// the metronome itself is off, because asking for a count-in is asking for
    /// clicks. Passing 0 cancels one in flight.
    void requestCountIn(int beats) noexcept {
        m_countInRequest.store(beats, std::memory_order_release);
    }

    void prepare(const PrepareInfo& info) override {
        m_sampleRate = info.sampleRate;
    }

    void process(const ProcessContext& context) override {
        const ChannelCount channels = context.output.numChannels();
        for (ChannelCount ch = 0; ch < channels; ++ch)
            dsp::clear(context.output.channel(ch));

        if (m_sampleRate <= 0.0) return;

        const double tempo =
            context.transport.tempo > 0.0 ? context.transport.tempo : 120.0;

        renderCountIn(context, tempo);

        if (!context.playing || !m_enabled.load(std::memory_order_relaxed))
            return;
        // The accent falls on the downbeat, so the count is bar length in
        // quarter notes — 3 in 6/8, not 6.
        const int denominator = context.transport.timeSigDenominator > 0
                                    ? context.transport.timeSigDenominator
                                    : 4;
        const int beatsPerBar = std::max(
            1, int(std::lround(double(context.transport.timeSigNumerator) * 4.0 /
                               double(denominator))));
        const double samplesPerBeat = 60.0 / tempo * m_sampleRate;
        if (samplesPerBeat <= 0.0) return;

        const SamplePos blockStart = context.timelinePosition;
        const SamplePos blockEnd = blockStart + SamplePos(context.frames);
        const SamplePos clickLen = SamplePos(0.035 * m_sampleRate);  // 35 ms

        // The first beat whose click could reach into this block.
        long beat = long(std::floor(
            (double(blockStart) - double(clickLen)) / samplesPerBeat));
        if (beat < 0) beat = 0;

        for (;; ++beat) {
            const double onset = double(beat) * samplesPerBeat;
            if (onset >= double(blockEnd)) break;
            const bool accent = (beat % beatsPerBar) == 0;
            // mixClick clamps to the overlap with this block up front. Walking
            // from the click's onset and skipping meant iterating the whole
            // 35 ms tail sample by sample for every beat that started before it.
            mixClick(context, onset, blockStart, clickLen,
                     accent ? 1600.0 : 1000.0, accent ? 0.5f : 0.32f);
        }
    }

private:
    /// One click of `length` samples starting at `onset`, measured from the
    /// same origin as `blockStart`, mixed into every output channel.
    void mixClick(const ProcessContext& context, double onset, SamplePos blockStart,
                  SamplePos length, double freq, float amp) const {
        const SamplePos blockEnd = blockStart + SamplePos(context.frames);
        const SamplePos start = SamplePos(onset);
        const SamplePos from = std::max(start, blockStart);
        const SamplePos to = std::min(start + length, blockEnd);
        const ChannelCount channels = context.output.numChannels();
        for (SamplePos s = from; s < to; ++s) {
            const SamplePos idx = s - blockStart;
            const double tt = double(s - start) / m_sampleRate;
            const float env = std::exp(float(-tt) * 40.0f);
            const float val =
                amp * env *
                std::sin(float(2.0 * std::numbers::pi_v<double> * freq * tt));
            for (ChannelCount ch = 0; ch < channels; ++ch)
                context.output.data(ch)[idx] += val;
        }
    }

    /// The count-in clicks, on the node's own sample clock rather than the
    /// timeline's: the transport is parked while they play, so there is no
    /// timeline position to lock them to.
    void renderCountIn(const ProcessContext& context, double tempo) {
        const int request = m_countInRequest.exchange(-1, std::memory_order_acquire);
        if (request >= 0) {
            m_countInBeats = request;
            m_countInPos = 0;
        }
        if (m_countInBeats <= 0) return;

        const double samplesPerBeat = 60.0 / tempo * m_sampleRate;
        if (samplesPerBeat <= 0.0) {
            m_countInBeats = 0;
            return;
        }
        const SamplePos clickLen = SamplePos(0.035 * m_sampleRate);
        for (int beat = 0; beat < m_countInBeats; ++beat) {
            const double onset = double(beat) * samplesPerBeat;
            if (onset >= double(m_countInPos) + double(context.frames)) break;
            if (onset + double(clickLen) <= double(m_countInPos)) continue;
            // The first click is the accent, so "three, two, one" has a top to
            // count down from.
            const bool accent = beat == 0;
            mixClick(context, onset, m_countInPos, clickLen,
                     accent ? 1600.0 : 1000.0, accent ? 0.5f : 0.32f);
        }
        m_countInPos += SamplePos(context.frames);
        if (double(m_countInPos) >=
            double(m_countInBeats) * samplesPerBeat + double(clickLen)) {
            m_countInBeats = 0;
        }
    }

    std::string m_name;
    std::atomic<bool> m_enabled{false};
    SampleRate m_sampleRate = 48000.0;

    // Count-in: the control thread posts a beat count, the audio thread owns
    // everything else. −1 means "nothing posted".
    std::atomic<int> m_countInRequest{-1};
    int m_countInBeats = 0;        // audio thread only
    SamplePos m_countInPos = 0;    // audio thread only
};

} // namespace daw::engine
