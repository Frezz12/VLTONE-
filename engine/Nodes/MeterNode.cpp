#include "Nodes/MeterNode.hpp"

#include "DSP/Simd.hpp"

#include <algorithm>
#include <cstddef>

namespace daw::engine {

namespace {

/// Copy a channel while reducing the magnitude of exactly the samples written.
/// This is the common one-input meter path: one source read and one destination
/// write instead of memcpy followed by a second read for dsp::peak().
float copyAndPeak(std::span<float> destination,
                  std::span<const float> source) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    float* output = destination.data();
    const float* input = source.data();
    std::size_t i = 0;
    float highest = 0.0f;

#if DAW_SIMD_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t samples = vld1q_f32(input + i);
        vst1q_f32(output + i, samples);
        acc = vmaxq_f32(acc, vabsq_f32(samples));
    }
    highest = vmaxvq_f32(acc);
#elif DAW_SIMD_X86 && defined(__AVX__)
    const __m256 mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m256 acc = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) {
        const __m256 samples = _mm256_loadu_ps(input + i);
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
        const __m128 samples = _mm_loadu_ps(input + i);
        _mm_storeu_ps(output + i, samples);
        acc = _mm_max_ps(acc, _mm_and_ps(samples, mask));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, acc);
    for (float lane : lanes) highest = lane > highest ? lane : highest;
#endif
    for (; i < n; ++i) {
        const float sample = input[i];
        output[i] = sample;
        const float magnitude = sample < 0.0f ? -sample : sample;
        if (magnitude > highest) highest = magnitude;
    }
    return highest;
}

/// Add the final contributor and reduce the finished sum in the same pass.
/// Earlier contributors still use the normal SIMD kernels, preserving their
/// exact accumulation order and avoiding a frame-major input loop.
float addAndPeak(std::span<float> destination,
                 std::span<const float> source) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    float* output = destination.data();
    const float* input = source.data();
    std::size_t i = 0;
    float highest = 0.0f;

#if DAW_SIMD_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t sum =
            vaddq_f32(vld1q_f32(output + i), vld1q_f32(input + i));
        vst1q_f32(output + i, sum);
        acc = vmaxq_f32(acc, vabsq_f32(sum));
    }
    highest = vmaxvq_f32(acc);
#elif DAW_SIMD_X86 && defined(__AVX__)
    const __m256 mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m256 acc = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) {
        const __m256 sum = _mm256_add_ps(_mm256_loadu_ps(output + i),
                                         _mm256_loadu_ps(input + i));
        _mm256_storeu_ps(output + i, sum);
        acc = _mm256_max_ps(acc, _mm256_and_ps(sum, mask));
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, acc);
    for (float lane : lanes) highest = lane > highest ? lane : highest;
#elif DAW_SIMD_X86
    const __m128 mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    __m128 acc = _mm_setzero_ps();
    for (; i + 4 <= n; i += 4) {
        const __m128 sum = _mm_add_ps(_mm_loadu_ps(output + i),
                                      _mm_loadu_ps(input + i));
        _mm_storeu_ps(output + i, sum);
        acc = _mm_max_ps(acc, _mm_and_ps(sum, mask));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, acc);
    for (float lane : lanes) highest = lane > highest ? lane : highest;
#endif
    for (; i < n; ++i) {
        const float sample = output[i] + input[i];
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
        return copyAndPeak(destination, first->channel(channel));
    }

    dsp::copy(destination, first->channel(channel));
    for (const AudioBlock& input : inputs) {
        if (&input == first || channel >= input.numChannels()) continue;
        if (&input == last) return addAndPeak(destination, input.channel(channel));
        dsp::add(destination, input.channel(channel));
    }
    return 0.0f; // first != last guarantees an earlier return
}

} // namespace

void MeterNode::process(const ProcessContext& context) noexcept {
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
}

} // namespace daw::engine
