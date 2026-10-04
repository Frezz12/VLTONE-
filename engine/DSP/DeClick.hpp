#pragma once

#include "Common/Types.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace daw::engine::dsp {

/// Audio-thread-owned, finite crossfade over a discontinuous signal edge.
/// The first changed sample meets the previous output; the correction reaches
/// zero in 5 ms. No source delay, allocation, locking or ownership reclamation.
class DeClick {
public:
    void reset() noexcept {
        m_last.fill(0.0f);
        m_rawLast.fill(0.0f);
        m_from.fill(0.0f);
        m_remaining = 0;
        m_pending = false;
    }

    void begin(SampleRate rate, FrameCount maximum = kMaxBlockSize) noexcept {
        const double frames = (rate > 0.0 ? rate : 48000.0) * 0.005;
        m_pendingLength = std::max<FrameCount>(1, std::min<FrameCount>(
            maximum, FrameCount(std::clamp(frames, 1.0, double(kMaxBlockSize)))));
        m_pending = true;
    }

    void process(const AudioBlock& output, FrameCount frames,
                 FrameCount offset = 0) noexcept {
        if (!frames) return;
        const auto channels = std::min(output.numChannels(), kMaxChannels);
        if (m_pending) {
            bool changed = false;
            for (ChannelCount ch = 0; ch < channels; ++ch)
                changed |= output.data(ch)[offset] != m_rawLast[ch];
            // A constant one-frame loop has no seam. Restarting its unfinished
            // fade on every wrap would prevent it ever reaching its level.
            if (changed) {
                for (ChannelCount ch = 0; ch < channels; ++ch)
                    m_from[ch] = m_last[ch];
                m_length = m_pendingLength;
                m_remaining = m_length;
            }
            m_pending = false;
        }
        const auto count = std::min(frames, m_remaining);
        const float inverse = 1.0f / float(m_length);
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            auto* samples = output.data(ch) + offset;
            m_rawLast[ch] = samples[frames - 1];
            for (FrameCount i = 0; i < count; ++i) {
                const float t = float(m_remaining - i) * inverse;
                const float fade = t * t * (3.0f - 2.0f * t);
                samples[i] = samples[i] * (1.0f - fade) + m_from[ch] * fade;
            }
            m_last[ch] = samples[frames - 1];
        }
        m_remaining -= count;
    }

private:
    std::array<float, kMaxChannels> m_last{}, m_rawLast{}, m_from{};
    FrameCount m_length = 240, m_pendingLength = 240, m_remaining = 0;
    bool m_pending = false;
};

} // namespace daw::engine::dsp
