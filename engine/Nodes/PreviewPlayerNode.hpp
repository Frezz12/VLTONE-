#pragma once

#include "Audio/SampleBuffer.hpp"
#include "DSP/Simd.hpp"
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
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    // ── Control thread ──

    /// Arm `audio` and play it from the start. A null buffer stops playback.
    ///
    /// The buffer is published before the command, so the audio thread can
    /// never see "play" paired with the previous buffer. `atomic_store` on a
    /// `shared_ptr` is the convention the other nodes use: the audio thread may
    /// still be reading the old buffer, and the shared_ptr keeps it alive until
    /// it is not.
    void start(std::shared_ptr<const SampleBuffer> audio) {
        m_audio.publish(std::move(audio));
        m_command.store(int(Command::Play), std::memory_order_release);
    }

    void stop() noexcept {
        m_command.store(int(Command::Stop), std::memory_order_release);
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
    void seekFrames(std::int64_t frame) noexcept {
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
        m_command.store(int(Command::None), std::memory_order_relaxed);
        m_seek.store(-1, std::memory_order_relaxed);
        m_readPosition = 0.0;
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

        const int command = m_command.exchange(int(Command::None),
                                               std::memory_order_acquire);
        if (command == int(Command::Play)) {
            m_readPosition = 0.0;
            m_playing.store(true, std::memory_order_relaxed);
        } else if (command == int(Command::Stop)) {
            m_playing.store(false, std::memory_order_relaxed);
            m_position.store(0, std::memory_order_relaxed);
        }

        const std::int64_t seek = m_seek.exchange(-1, std::memory_order_acquire);
        if (seek >= 0) m_readPosition = double(seek);

        // Deliberately no `!context.playing` guard: an audition has nothing to
        // do with the playhead.
        if (!m_playing.load(std::memory_order_relaxed)) return;

        auto audio = m_audio.read();
        if (!audio || audio->frames() == 0) {
            m_playing.store(false, std::memory_order_relaxed);
            return;
        }
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

        FrameCount written = 0;
        while (written < context.frames) {
            if (m_readPosition >= double(sourceFrames)) {
                if (!looping) break;
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
                const float* source = audio->channel(ch);
                float* destination = context.output.data(ch) + written;
                if (step == 1.0) {
                    const auto base = std::int64_t(m_readPosition);
                    dsp::addScaled({destination, count},
                                   {source + base, count}, gain);
                    continue;
                }
                double position = m_readPosition;
                for (FrameCount i = 0; i < count; ++i, position += step) {
                    const auto index = std::int64_t(position);
                    if (index + 1 >= sourceFrames) break;
                    const float fraction = float(position - double(index));
                    const float a = source[index];
                    const float b = source[index + 1];
                    destination[i] += (a + (b - a) * fraction) * gain;
                }
            }
            m_readPosition += double(count) * step;
            written += count;
        }

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
    enum class Command { None = 0, Play, Stop };

    std::string m_name;
    RealtimeSnapshot<SampleBuffer> m_audio;

    std::atomic<int> m_command{int(Command::None)};
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
    SampleRate m_sampleRate = 48000.0;
};

} // namespace daw::engine
