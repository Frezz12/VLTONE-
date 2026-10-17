#pragma once

#include "Common/Types.hpp"

#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace daw::engine {

/// One cache-aligned arena holding every audio buffer the graph needs.
///
/// All allocation happens on the control thread when the graph is compiled; the
/// realtime path only ever indexes into it. Channels are stored channel-major
/// with each channel padded to a cache line, so two nodes writing neighbouring
/// buffers never share a line (false sharing would otherwise show up as random
/// stalls once many workers run in parallel).
class BufferArena {
public:
    BufferArena() = default;

    /// Reserve `bufferCount` buffers of `channels × frames`. Control thread.
    void allocate(std::size_t bufferCount, ChannelCount channels,
                  FrameCount frames) {
        m_bufferCount = bufferCount;
        m_channels = channels;
        m_frames = frames;
        // Pad each channel so a channel start always lands on a cache line.
        m_strideFloats = ((std::size_t(frames) * sizeof(float) + kCacheLine - 1) /
                          kCacheLine) * kCacheLine / sizeof(float);

        const std::size_t total = bufferCount * channels * m_strideFloats;
        m_storage = makeAligned(total);
        m_channelPointers.assign(bufferCount * channels, nullptr);
        for (std::size_t buffer = 0; buffer < bufferCount; ++buffer) {
            for (ChannelCount ch = 0; ch < channels; ++ch) {
                m_channelPointers[buffer * channels + ch] =
                    m_storage.get() + (buffer * channels + ch) * m_strideFloats;
            }
        }
        clearAll();
    }

    std::size_t bufferCount() const noexcept { return m_bufferCount; }
    ChannelCount channels() const noexcept { return m_channels; }
    FrameCount frames() const noexcept { return m_frames; }

    /// View onto buffer `index`, limited to `frames` (≤ the prepared size).
    AudioBlock block(std::size_t index, FrameCount frames) const noexcept {
        return AudioBlock(&m_channelPointers[index * m_channels], m_channels,
                          frames);
    }

    void clearAll() noexcept {
        if (m_storage) {
            std::memset(m_storage.get(), 0,
                        m_bufferCount * m_channels * m_strideFloats * sizeof(float));
        }
    }

private:
    // Aligned `operator new` rather than posix_memalign: the latter does not
    // exist on MSVC, and its _aligned_malloc counterpart needs a matching
    // _aligned_free, which would put a platform branch in both the allocator
    // and the deleter. This is one expression on every toolchain.
    struct AlignedDeleter {
        void operator()(float* p) const noexcept {
            ::operator delete(p, std::align_val_t{kCacheLine});
        }
    };
    using Storage = std::unique_ptr<float[], AlignedDeleter>;

    static Storage makeAligned(std::size_t floats) {
        const std::size_t bytes =
            ((floats * sizeof(float) + kCacheLine - 1) / kCacheLine) * kCacheLine;
        return Storage(static_cast<float*>(
            ::operator new(bytes, std::align_val_t{kCacheLine})));
    }

    Storage m_storage;
    std::vector<float*> m_channelPointers;
    std::size_t m_bufferCount = 0;
    std::size_t m_strideFloats = 0;
    ChannelCount m_channels = 0;
    FrameCount m_frames = 0;
};

} // namespace daw::engine
