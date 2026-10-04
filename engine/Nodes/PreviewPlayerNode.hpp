#pragma once

#include "Audio/SampleBuffer.hpp"
#include "DSP/Simd.hpp"
#include "DSP/DeClick.hpp"
#include "Graph/Node.hpp"
#include "Common/RealtimeSnapshot.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace daw::engine {

/// Auditions a file: plays a decoded buffer on demand, whatever the transport
/// is doing.
///
/// This is the one thing `ClipPlayerNode` cannot do. That node returns early on
/// `!context.playing` and positions everything against the timeline, which is
/// right for a clip and useless for "let me hear this sample". `MetronomeNode`
/// already solved the same problem for a count-in — render above the transport
/// guard, driven by an atomic request from the UI thread — and this follows it.
///
/// One instance lives permanently in the graph, summed into the master, so an
/// audition survives a graph rebuild and needs no routing of its own. It is
/// silent in an offline render: an export must contain the project, not
/// whatever the user happened to be listening to.
class PreviewPlayerNode : public Node {
public:
    explicit PreviewPlayerNode(std::string name = "Preview")
        : m_name(std::move(name)) {}

    std::string_view name() const noexcept override { return m_name; }
    OfflineNodePolicy offlineNodePolicy() const noexcept override { return OfflineNodePolicy::Ordered; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    // ── Control thread ──

    /// Publish source and restart serial together. A stop has its own serial,
    /// so rapid browser changes cannot pair a new buffer with an old playhead.
    void start(std::shared_ptr<const SampleBuffer> audio) {
        if (audio) audio->prepareRead();
        m_seek.store(-1, std::memory_order_release);
        const auto serial = m_serial.fetch_add(1, std::memory_order_relaxed) + 1;
        m_audio.publish(std::make_shared<const Request>(Request{std::move(audio), serial}));
    }

    void stop() noexcept {
        m_seek.store(-1, std::memory_order_release);
        m_stopSerial.store(m_serial.fetch_add(1, std::memory_order_relaxed) + 1,
                           std::memory_order_release);
    }

    /// Loop at the end of the source instead of stopping. Takes effect on the
    /// block that reaches the end, so it can be flipped mid-audition.
    void setLoop(bool loop) noexcept {
        m_loop.store(loop, std::memory_order_relaxed);
    }
    bool loop() const noexcept { return m_loop.load(std::memory_order_relaxed); }

    void setGain(float gain) noexcept {
        m_gain.store(std::clamp(gain, 0.0f, 4.0f), std::memory_order_relaxed);
    }
    float gain() const noexcept { return m_gain.load(std::memory_order_relaxed); }

    /// Audition playback-rate multiplier. One is the source's original pitch;
    /// the clip editor uses 2^(semitones/12) for keyboard audition.
    void setRate(double rate) noexcept {
        m_rate.store(std::clamp(rate, 0.125, 8.0), std::memory_order_relaxed);
    }

    /// Jump to a source frame. −1 means "nothing posted", which is why the
    /// queue slot is signed.
    void seekFrames(std::int64_t frame) {
        const auto request = m_audio.controlCopy();
        if (request && request->audio)
            request->audio->prepareRead(FrameCount(std::clamp<std::int64_t>(
                frame, 0, request->audio->frames())));
        m_seek.store(std::max<std::int64_t>(0, frame), std::memory_order_release);
    }

    bool playing() const noexcept { return m_playing.load(std::memory_order_relaxed); }

    /// Where the audition head is, as a source frame index. Published by the
    /// audio thread for a UI that wants to draw a playhead; a plain integer, so
    /// it is lock-free on every target we build for.
    std::uint64_t positionFrames() const noexcept {
        return m_position.load(std::memory_order_relaxed);
    }
    /// Rate of the armed buffer, so the UI can turn frames into seconds.
    SampleRate sourceRate() const noexcept {
        return m_sourceRate.load(std::memory_order_relaxed);
    }
    /// Length of the armed buffer in frames (0 when nothing is armed).
    std::uint64_t sourceFrames() const noexcept {
        return m_sourceFrames.load(std::memory_order_relaxed);
    }

    // ── Audio thread ──

    void prepare(const PrepareInfo& info) override { m_sampleRate = info.sampleRate; }

    void reset() override {
        // An offline render resets every node before it starts, so this is also
        // what keeps an audition out of an export.
        m_playing.store(false, std::memory_order_relaxed);
        m_position.store(0, std::memory_order_relaxed);
        const auto request = m_audio.read();
        m_appliedSerial = std::max(request ? request->serial : 0,
            m_stopSerial.load(std::memory_order_acquire));
        m_seek.store(-1, std::memory_order_relaxed);
        m_readPosition = 0.0;
        m_declick.reset();
    }

    void process(const ProcessContext& context) override {
        const ChannelCount channels = context.output.numChannels();
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            dsp::clear(context.output.channel(ch));
        }
        // Never in an export or a freeze. `reset()` above already stops an
        // audition before an offline pass; this is the guard that holds even
        // for a future path that renders without resetting.
        if (context.offline || context.frames == 0) return;

        auto request = m_audio.read();
        const auto stop = m_stopSerial.load(std::memory_order_acquire);
        const auto serial = std::max(request ? request->serial : 0, stop);
        if (serial != m_appliedSerial) {
            m_appliedSerial = serial;
            m_readPosition = 0.0;
            m_position.store(0, std::memory_order_relaxed);
            m_playing.store(request && request->serial > stop &&
                request->audio && request->audio->frames(), std::memory_order_relaxed);
            m_declick.begin(m_sampleRate);
        }

        const std::int64_t seek = m_seek.exchange(-1, std::memory_order_acquire);
        if (seek >= 0) {
            m_readPosition = double(seek);
            m_declick.begin(m_sampleRate);
        }
        // A stopped audition still drains its short correction tail.
        if (!m_playing.load(std::memory_order_relaxed) ||
            !request || !request->audio) {
            m_declick.process(context.output, context.frames);
            return;
        }
        const auto* audio = request->audio.get();
        if (!context.offline && loop()) audio->hintRead();
        m_sourceRate.store(audio->sampleRate(), std::memory_order_relaxed);
        m_sourceFrames.store(audio->frames(), std::memory_order_relaxed);

        // A 44.1 kHz file on a 48 kHz device is read with a step, so it plays
        // at its own pitch rather than transposed.
        const double step = ((audio->sampleRate() > 0.0 && m_sampleRate > 0.0)
                                 ? audio->sampleRate() / m_sampleRate
                                 : 1.0) *
                            m_rate.load(std::memory_order_relaxed);
        const auto sourceFrames = std::int64_t(audio->frames());
        const float gain = m_gain.load(std::memory_order_relaxed);
        const bool looping = m_loop.load(std::memory_order_relaxed);
        if (gain != m_previousGain || step != m_previousStep)
            m_declick.begin(m_sampleRate);
        m_previousGain = gain;
        m_previousStep = step;

        FrameCount written = 0;
        while (written < context.frames) {
            if (m_readPosition >= double(sourceFrames)) {
                if (!looping) break;
                m_declick.begin(m_sampleRate, std::max<FrameCount>(
                    1, FrameCount(double(sourceFrames) / step / 2.0)));
                // Keep the fractional part across the wrap: with a non-integer
                // step, snapping to 0 would drift the loop a little each time.
                m_readPosition -= double(sourceFrames);
                if (m_readPosition < 0.0 || m_readPosition >= double(sourceFrames))
                    m_readPosition = 0.0;
            }

            // How many frames fit before the end of the source (or the block).
            const double left = double(sourceFrames) - m_readPosition;
            const auto room =
                FrameCount(std::max<double>(0.0, std::floor(left / step)));
            FrameCount count = std::min<FrameCount>(context.frames - written, room);
            if (count == 0) {
                // Less than one step of source left: treat it as the end.
                m_readPosition = double(sourceFrames);
                if (!looping) break;
                // A source shorter than a single step can never produce a
                // frame, and wrapping it forever would spin the audio thread
                // inside this block — which the device hears as the last buffer
                // repeating. Stop the audition instead.
                if (double(sourceFrames) < step) {
                    m_playing.store(false, std::memory_order_relaxed);
                    break;
                }
                continue;
            }

            for (ChannelCount ch = 0; ch < channels; ++ch) {

                float* destination = context.output.data(ch) + written;
                if (step == 1.0) {
                    const auto base = std::int64_t(m_readPosition);
                    for (FrameCount done = 0; done < count;) {
                        const auto part = audio->readSpan(ch, FrameCount(base + done), count - done);
                        if (part.empty()) break;
                        dsp::addScaled({destination + done, part.size()}, part, gain);
                        done += FrameCount(part.size());
                    }
                    continue;
                }
                double position = m_readPosition;
                for (FrameCount i = 0; i < count; ++i, position += step) {
                    const auto index = std::int64_t(position);
                    if (index + 1 >= sourceFrames) break;
                    const float fraction = float(position - double(index));
                    const float a = audio->readSample(ch, FrameCount(index));
                    const float b = audio->readSample(ch, FrameCount(index + 1));
                    destination[i] += (a + (b - a) * fraction) * gain;
                }
            }
            if (!looping) {
                // A known file end can fade before it runs out, preserving its
                // exact duration instead of appending a held-sample tail.
                const double fadeFrames = std::max(1.0, m_sampleRate * 0.005);
                const double remaining = std::floor((double(sourceFrames) - m_readPosition) / step);
                if (remaining - count < fadeFrames) {
                    for (ChannelCount ch = 0; ch < channels; ++ch)
                        for (FrameCount i = 0; i < count; ++i)
                            context.output.data(ch)[written + i] *= float(std::clamp(
                                (remaining - i - 1.0) / fadeFrames, 0.0, 1.0));
                }
            }
            m_declick.process(context.output, count, written);
            m_readPosition += double(count) * step;
            written += count;
        }

        if (written < context.frames)
            m_declick.process(context.output, context.frames - written, written);
        if (m_readPosition >= double(sourceFrames) && !looping) {
            m_playing.store(false, std::memory_order_relaxed);
            m_position.store(0, std::memory_order_relaxed);
            m_readPosition = 0.0;
            return;
        }
        m_position.store(std::uint64_t(std::max(0.0, m_readPosition)),
                         std::memory_order_relaxed);
    }

private:
    struct Request {
        std::shared_ptr<const SampleBuffer> audio;
        std::uint64_t serial;
    };
    std::string m_name;
    RealtimeSnapshot<Request> m_audio;

    std::atomic<std::uint64_t> m_serial{0}, m_stopSerial{0};
    std::atomic<std::int64_t> m_seek{-1};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_loop{false};
    std::atomic<float> m_gain{1.0f};
    std::atomic<double> m_rate{1.0};
    std::atomic<std::uint64_t> m_position{0};
    std::atomic<SampleRate> m_sourceRate{0.0};
    std::atomic<std::uint64_t> m_sourceFrames{0};

    /// Audio thread only: the fractional read head, in source frames.
    double m_readPosition = 0.0;
    std::uint64_t m_appliedSerial = 0;
    float m_previousGain = 1.0f;
    double m_previousStep = 1.0;
    dsp::DeClick m_declick;
    SampleRate m_sampleRate = 48000.0;
};

} // namespace daw::engine
