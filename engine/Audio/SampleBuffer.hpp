#pragma once

#include "Common/Types.hpp"
#include "Memory/SampleStorage.hpp"
#include "Memory/PcmReadCache.hpp"

#include <memory>
#include <algorithm>
#include <cstring>
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
        : m_storage(std::size_t(channels) * frames),
          m_channels(channels), m_frames(frames), m_sampleRate(rate) {
        if (m_storage.fileBacked()) {
            m_readCache = &PcmReadCache::instance();
            m_sourceId = m_readCache->addSource(m_storage.data(), std::size_t(channels) * frames);
        }
    }
    ~SampleBuffer() { if (m_readCache) m_readCache->removeSource(m_sourceId); }

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
    bool fileBacked() const noexcept { return m_storage.fileBacked(); }
    // May return fewer samples at a cache-page boundary. The view is pinned
    // until the current node's PcmReadScope ends or evicts its cursor.
    std::span<const float> readSpan(ChannelCount channelIndex, FrameCount first, FrameCount count) const noexcept {
        if (first >= m_frames || m_channels == 0) return {};
        count = std::min(count, m_frames - first);
        const auto channel = channelIndex < m_channels ? channelIndex : ChannelCount(0);
        const auto offset = std::size_t(channel) * m_frames + first;
        if (m_readCache && PcmReadScope::current())
            return PcmReadScope::current()->view(*m_readCache, m_sourceId, offset, count);
        return {m_storage.data() + offset, count};
    }
    float readSample(ChannelCount channelIndex, FrameCount frame) const noexcept {
        const auto samples = readSpan(channelIndex, frame, 1);
        return samples.empty() ? 0.f : samples.front();
    }
    void hintRead(FrameCount first = 0) const noexcept {
        if (!m_readCache || first >= m_frames) return;
        for (ChannelCount ch = 0; ch < m_channels; ++ch)
            m_readCache->hint(m_sourceId, std::size_t(ch) * m_frames + first);
    }
    // Bounded synchronous pre-roll, control thread only. Warm both planes and
    // no more than four pages per channel regardless of recording duration.
    void prepareRead(FrameCount first = 0) const {
        if (!m_readCache || first >= m_frames) return;
        for (ChannelCount ch = 0; ch < m_channels; ++ch)
            m_readCache->warm(m_sourceId, std::size_t(ch) * m_frames + first,
                std::min<std::size_t>(m_frames - first, 4 * PcmReadCache::kPageSamples));
    }

    /// Finalize a decoded prefix before publishing the immutable buffer. Move
    /// the channel planes in place so a short MPEG stream needs no second PCM
    /// allocation and does not acquire a silent tail from its length estimate.
    void trimFrames(FrameCount frames) noexcept {
        frames = std::min(frames, m_frames);
        if (frames == m_frames) return;
        for (ChannelCount ch = 1; ch < m_channels; ++ch)
            std::memmove(m_storage.data() + std::size_t(ch) * frames,
                         m_storage.data() + std::size_t(ch) * m_frames,
                         std::size_t(frames) * sizeof(float));
        m_frames = frames;
    }

    const float* channel(ChannelCount index) const noexcept {
        // Mono sources feed every output channel rather than only the left.
        const ChannelCount ch = index < m_channels ? index : 0;
        return m_storage.data() + std::size_t(ch) * m_frames;
    }

    float* writableChannel(ChannelCount index) noexcept {
        return m_storage.data() + std::size_t(index) * m_frames;
    }

private:
    SampleStorage m_storage;
    ChannelCount m_channels;
    FrameCount m_frames;
    SampleRate m_sampleRate;
    PcmReadCache* m_readCache = nullptr;
    std::uint64_t m_sourceId = 0;
};

} // namespace daw::engine
