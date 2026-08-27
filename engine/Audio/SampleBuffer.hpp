#pragma once

#include "Common/Types.hpp"

#include <memory>
#include <vector>

namespace daw::engine {

/// Decoded audio held in memory, planar and immutable once built.
///
/// Clips reference these through shared_ptr, so the same file used by twenty
/// clips is decoded once, and the audio thread can keep reading a buffer while
/// the control thread removes the clip that owned it — the shared_ptr keeps it
/// alive until the last reader is gone.
class SampleBuffer {
public:
    SampleBuffer(ChannelCount channels, FrameCount frames, SampleRate rate)
        : m_storage(std::size_t(channels) * frames, 0.0f),
          m_channels(channels), m_frames(frames), m_sampleRate(rate) {}

    /// Build from interleaved decoder output.
    static std::shared_ptr<const SampleBuffer> fromInterleaved(
        const std::vector<float>& interleaved, ChannelCount channels,
        FrameCount frames, SampleRate rate) {
        auto buffer = std::make_shared<SampleBuffer>(channels, frames, rate);
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            float* destination = buffer->m_storage.data() + std::size_t(ch) * frames;
            for (FrameCount i = 0; i < frames; ++i) {
                destination[i] = interleaved[std::size_t(i) * channels + ch];
            }
        }
        return buffer;
    }

    ChannelCount channels() const noexcept { return m_channels; }
    FrameCount frames() const noexcept { return m_frames; }
    SampleRate sampleRate() const noexcept { return m_sampleRate; }

    const float* channel(ChannelCount index) const noexcept {
        // Mono sources feed every output channel rather than only the left.
        const ChannelCount ch = index < m_channels ? index : 0;
        return m_storage.data() + std::size_t(ch) * m_frames;
    }

    float* writableChannel(ChannelCount index) noexcept {
        return m_storage.data() + std::size_t(index) * m_frames;
    }

private:
    std::vector<float> m_storage;
    ChannelCount m_channels;
    FrameCount m_frames;
    SampleRate m_sampleRate;
};

} // namespace daw::engine
