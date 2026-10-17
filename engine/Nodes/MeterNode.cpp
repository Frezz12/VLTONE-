#include "Nodes/MeterNode.hpp"

#include "DSP/Simd.hpp"

#include <algorithm>
#include <cstddef>

namespace daw::engine {

namespace {

/// Copy a channel while reducing the magnitude of exactly the samples written.
/// This is the common one-input meter path: one source read and one destination
/// write instead of memcpy followed by a second read for dsp::peak().
template <bool Add>
float writeAndPeak(std::span<float> destination,
                   std::span<const float> source) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    if constexpr (!Add)
        if (destination.data() == source.data()) return dsp::peak(source.first(n));
    float* output = destination.data();
    const float* input = source.data();
    std::size_t i = 0;
    float highest = 0.0f;

#if DAW_SIMD_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i + 4 <= n; i += 4) {
        float32x4_t samples = vld1q_f32(input + i);
        if constexpr (Add) samples = vaddq_f32(vld1q_f32(output + i), samples);
        vst1q_f32(output + i, samples);
        acc = vmaxq_f32(acc, vabsq_f32(samples));
    }
    highest = vmaxvq_f32(acc);
#elif DAW_SIMD_X86 && defined(__AVX__)
    const __m256 mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m256 acc = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) {
        __m256 samples = _mm256_loadu_ps(input + i);
        if constexpr (Add) samples = _mm256_add_ps(_mm256_loadu_ps(output + i), samples);
        _mm256_storeu_ps(output + i, samples);
        acc = _mm256_max_ps(acc, _mm256_and_ps(samples, mask));
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, acc);
    for (float lane : lanes) highest = lane > highest ? lane : highest;
#elif DAW_SIMD_X86
    const __m128 mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    __m128 acc = _mm_setzero_ps();
    for (; i + 4 <= n; i += 4) {
        __m128 samples = _mm_loadu_ps(input + i);
        if constexpr (Add) samples = _mm_add_ps(_mm_loadu_ps(output + i), samples);
        _mm_storeu_ps(output + i, samples);
        acc = _mm_max_ps(acc, _mm_and_ps(samples, mask));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, acc);
    for (float lane : lanes) highest = lane > highest ? lane : highest;
#endif
    for (; i < n; ++i) {
        float sample = input[i];
        if constexpr (Add) sample = output[i] + sample;
        output[i] = sample;
        const float magnitude = sample < 0.0f ? -sample : sample;
        if (magnitude > highest) highest = magnitude;
    }
    return highest;
}

void sumChannel(std::span<float> destination,
                std::span<const AudioBlock> inputs,
                ChannelCount channel) noexcept {
    bool written = false;
    for (const AudioBlock& input : inputs) {
        if (channel >= input.numChannels()) continue;
        if (written) {
            dsp::add(destination, input.channel(channel));
        } else {
            dsp::copy(destination, input.channel(channel));
            written = true;
        }
    }
    if (!written) dsp::clear(destination);
}

float sumChannelAndPeak(std::span<float> destination,
                        std::span<const AudioBlock> inputs,
                        ChannelCount channel) noexcept {
    const AudioBlock* first = nullptr;
    const AudioBlock* last = nullptr;
    for (const AudioBlock& input : inputs) {
        if (channel >= input.numChannels()) continue;
        if (!first) first = &input;
        last = &input;
    }
    if (!first) {
        dsp::clear(destination);
        return 0.0f;
    }
    if (first == last) {
        return writeAndPeak<false>(destination, first->channel(channel));
    }

    dsp::copy(destination, first->channel(channel));
    for (const AudioBlock& input : inputs) {
        if (&input == first || channel >= input.numChannels()) continue;
        if (&input == last) return writeAndPeak<true>(destination, input.channel(channel));
        dsp::add(destination, input.channel(channel));
    }
    return 0.0f; // first != last guarantees an earlier return
}

} // namespace

void MeterNode::process(const ProcessContext& context) noexcept {
    // Export has no live meter consumer. Preserve the identical audio sum
    // without scanning samples or publishing UI peaks on every block.
    if (context.offline) {
        for (ChannelCount channel = 0; channel < context.output.numChannels(); ++channel)
            sumChannel(context.output.channel(channel), context.inputs, channel);
        return;
    }
    float left = 0.0f;
    float right = 0.0f;
    for (ChannelCount channel = 0; channel < context.output.numChannels(); ++channel) {
        if (channel < 2) {
            const float peak = sumChannelAndPeak(
                context.output.channel(channel), context.inputs, channel);
            if (channel == 0) left = peak;
            else right = peak;
        } else {
            sumChannel(context.output.channel(channel), context.inputs, channel);
        }
    }
    // Always publish both values. A graph recompiled from stereo to mono must
    // not leave the old right-channel peak visible in the UI.
    m_peakL.store(left, std::memory_order_relaxed);
    m_peakR.store(right, std::memory_order_relaxed);
    m_peakHold.observe(std::max(left, right));
}

} // namespace daw::engine
