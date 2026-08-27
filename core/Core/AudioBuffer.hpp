#pragma once

#include "Types.hpp"
#include <memory>
#include <cstring>
#include <cassert>
#include <algorithm>

namespace audio {

class AudioBuffer {
public:
    AudioBuffer() = default;

    AudioBuffer(ChannelCount channels, BufferSize frames)
        : m_numChannels(channels)
        , m_numFrames(frames) {
        allocate();
    }

    AudioBuffer(const AudioBuffer& other) {
        copyFrom(other);
    }

    AudioBuffer& operator=(const AudioBuffer& other) {
        if (this != &other) {
            copyFrom(other);
        }
        return *this;
    }

    AudioBuffer(AudioBuffer&& other) noexcept
        : m_channels(other.m_channels)
        , m_numChannels(other.m_numChannels)
        , m_numFrames(other.m_numFrames)
        , m_ownsMemory(other.m_ownsMemory) {
        other.m_channels = nullptr;
        other.m_numChannels = 0;
        other.m_numFrames = 0;
        other.m_ownsMemory = false;
    }

    AudioBuffer& operator=(AudioBuffer&& other) noexcept {
        if (this != &other) {
            deallocate();
            m_channels = other.m_channels;
            m_numChannels = other.m_numChannels;
            m_numFrames = other.m_numFrames;
            m_ownsMemory = other.m_ownsMemory;
            other.m_channels = nullptr;
            other.m_numChannels = 0;
            other.m_numFrames = 0;
            other.m_ownsMemory = false;
        }
        return *this;
    }

    ~AudioBuffer() { deallocate(); }

    void resize(ChannelCount channels, BufferSize frames) {
        if (m_numChannels == channels && m_numFrames == frames) {
            clear();
            return;
        }
        deallocate();
        m_numChannels = channels;
        m_numFrames = frames;
        allocate();
    }

    void clear() { clear(m_numFrames); }

    /// Zero only the first `frames` samples. Realtime-safe: never allocates.
    void clear(BufferSize frames) {
        if (!m_channels) return;
        const BufferSize n = std::min(frames, m_numFrames);
        if (n == 0) return;
        for (ChannelCount ch = 0; ch < m_numChannels; ++ch) {
            if (m_channels[ch]) {
                std::memset(m_channels[ch], 0, n * sizeof(Sample));
            }
        }
    }

    void copyFrom(const AudioBuffer& other) {
        resize(other.m_numChannels, other.m_numFrames);
        for (ChannelCount ch = 0; ch < m_numChannels; ++ch) {
            std::memcpy(m_channels[ch], other.m_channels[ch],
                        m_numFrames * sizeof(Sample));
        }
    }

    /// Copy `frames` samples without ever reallocating. Channels and frames
    /// beyond this buffer's capacity are ignored, so it is safe to call from
    /// the audio thread with a preallocated destination.
    void copyFromInPlace(const AudioBuffer& source, BufferSize frames) {
        const ChannelCount ch = std::min(m_numChannels, source.m_numChannels);
        const BufferSize n = std::min({frames, m_numFrames, source.m_numFrames});
        for (ChannelCount c = 0; c < ch; ++c) {
            if (m_channels[c] && source.m_channels[c]) {
                std::memcpy(m_channels[c], source.m_channels[c],
                            n * sizeof(Sample));
            }
        }
    }

    void copyFromInterleaved(const Sample* data, ChannelCount channels,
                              BufferSize frames) {
        resize(channels, frames);
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            for (BufferSize f = 0; f < frames; ++f) {
                m_channels[ch][f] = data[f * channels + ch];
            }
        }
    }

    void copyToInterleaved(Sample* dest) const {
        for (ChannelCount ch = 0; ch < m_numChannels; ++ch) {
            for (BufferSize f = 0; f < m_numFrames; ++f) {
                dest[f * m_numChannels + ch] = m_channels[ch][f];
            }
        }
    }

    void addFrom(const AudioBuffer& source, float gain = 1.0f) {
        addFrom(source, gain, m_numFrames);
    }

    void addFrom(const AudioBuffer& source, float gain, BufferSize frames) {
        const ChannelCount ch = std::min(m_numChannels, source.m_numChannels);
        const BufferSize fr = std::min({frames, m_numFrames, source.m_numFrames});
        for (ChannelCount c = 0; c < ch; ++c) {
            Sample* dst = m_channels[c];
            const Sample* src = source.m_channels[c];
            if (!dst || !src) continue;
            for (BufferSize f = 0; f < fr; ++f) {
                dst[f] += src[f] * gain;
            }
        }
    }

    void applyGain(float gain) { applyGain(gain, m_numFrames); }

    void applyGain(float gain, BufferSize frames) {
        const BufferSize n = std::min(frames, m_numFrames);
        for (ChannelCount ch = 0; ch < m_numChannels; ++ch) {
            Sample* data = m_channels[ch];
            if (!data) continue;
            for (BufferSize f = 0; f < n; ++f) {
                data[f] *= gain;
            }
        }
    }

    [[nodiscard]] Sample* getChannel(ChannelCount idx) const {
        return (idx < m_numChannels) ? m_channels[idx] : nullptr;
    }

    [[nodiscard]] Sample** getChannels() const { return m_channels; }
    [[nodiscard]] ChannelCount numChannels() const { return m_numChannels; }
    [[nodiscard]] BufferSize numFrames() const { return m_numFrames; }
    [[nodiscard]] bool isValid() const { return m_channels != nullptr; }

    void setNonOwning(Sample** chs, ChannelCount channels, BufferSize frames) {
        deallocate();
        m_channels = chs;
        m_numChannels = channels;
        m_numFrames = frames;
        m_ownsMemory = false;
    }

private:
    void allocate() {
        if (m_numChannels == 0 || m_numFrames == 0) return;
        m_channels = new Sample*[m_numChannels];
        for (ChannelCount ch = 0; ch < m_numChannels; ++ch) {
            m_channels[ch] = new Sample[m_numFrames]();
        }
        m_ownsMemory = true;
    }

    void deallocate() {
        if (m_ownsMemory && m_channels) {
            for (ChannelCount ch = 0; ch < m_numChannels; ++ch) {
                delete[] m_channels[ch];
            }
            delete[] m_channels;
        }
        m_channels = nullptr;
        m_numChannels = 0;
        m_numFrames = 0;
        m_ownsMemory = false;
    }

    Sample** m_channels = nullptr;
    ChannelCount m_numChannels = 0;
    BufferSize m_numFrames = 0;
    bool m_ownsMemory = false;
};

using AudioBufferPtr = std::shared_ptr<AudioBuffer>;

} // namespace audio
