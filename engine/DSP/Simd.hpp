#pragma once

#include "Common/Types.hpp"

#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #include <arm_neon.h>
    #define DAW_SIMD_NEON 1
#elif defined(__AVX__) || defined(__SSE4_1__) || defined(_M_X64)
    #include <immintrin.h>
    #define DAW_SIMD_X86 1
#endif

/// Vectorised kernels for the handful of operations that dominate a mix:
/// clear, copy, gain, and accumulate-with-gain. Each has a scalar reference
/// implementation and a SIMD path chosen at compile time (NEON on Apple
/// Silicon/ARM, AVX/SSE on x86). The loops are written so the compiler can also
/// auto-vectorise the tail.
namespace daw::engine::dsp {

inline void clear(std::span<float> destination) noexcept {
    std::memset(destination.data(), 0, destination.size() * sizeof(float));
}

inline void copy(std::span<float> destination,
                 std::span<const float> source) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    std::memcpy(destination.data(), source.data(), n * sizeof(float));
}

/// destination *= gain
inline void applyGain(std::span<float> destination, float gain) noexcept {
    const std::size_t n = destination.size();
    float* d = destination.data();
    std::size_t i = 0;

#if DAW_SIMD_NEON
    const float32x4_t g = vdupq_n_f32(gain);
    for (; i + 4 <= n; i += 4) {
        vst1q_f32(d + i, vmulq_f32(vld1q_f32(d + i), g));
    }
#elif DAW_SIMD_X86 && defined(__AVX__)
    const __m256 g = _mm256_set1_ps(gain);
    for (; i + 8 <= n; i += 8) {
        _mm256_storeu_ps(d + i, _mm256_mul_ps(_mm256_loadu_ps(d + i), g));
    }
#elif DAW_SIMD_X86
    const __m128 g = _mm_set1_ps(gain);
    for (; i + 4 <= n; i += 4) {
        _mm_storeu_ps(d + i, _mm_mul_ps(_mm_loadu_ps(d + i), g));
    }
#endif
    for (; i < n; ++i) d[i] *= gain;
}

/// destination += source * gain — the mixing workhorse.
inline void addScaled(std::span<float> destination,
                      std::span<const float> source, float gain) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    float* d = destination.data();
    const float* s = source.data();
    std::size_t i = 0;

#if DAW_SIMD_NEON
    const float32x4_t g = vdupq_n_f32(gain);
    for (; i + 4 <= n; i += 4) {
        vst1q_f32(d + i, vmlaq_f32(vld1q_f32(d + i), vld1q_f32(s + i), g));
    }
#elif DAW_SIMD_X86 && defined(__AVX__)
    const __m256 g = _mm256_set1_ps(gain);
    for (; i + 8 <= n; i += 8) {
        const __m256 acc = _mm256_loadu_ps(d + i);
        _mm256_storeu_ps(d + i,
                         _mm256_add_ps(acc, _mm256_mul_ps(_mm256_loadu_ps(s + i), g)));
    }
#elif DAW_SIMD_X86
    const __m128 g = _mm_set1_ps(gain);
    for (; i + 4 <= n; i += 4) {
        const __m128 acc = _mm_loadu_ps(d + i);
        _mm_storeu_ps(d + i, _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(s + i), g)));
    }
#endif
    for (; i < n; ++i) d[i] += s[i] * gain;
}

/// destination += source — the unity-gain accumulate.
///
/// Worth having separately from `addScaled(…, 1.0f)`: summing a bus is the
/// single most executed loop in the mixer, and the multiply is not only a wasted
/// operation per sample but blocks the compiler from folding load-add-store into
/// the tightest form.
inline void add(std::span<float> destination,
                std::span<const float> source) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    float* d = destination.data();
    const float* s = source.data();
    std::size_t i = 0;

#if DAW_SIMD_NEON
    for (; i + 4 <= n; i += 4) {
        vst1q_f32(d + i, vaddq_f32(vld1q_f32(d + i), vld1q_f32(s + i)));
    }
#elif DAW_SIMD_X86 && defined(__AVX__)
    for (; i + 8 <= n; i += 8) {
        _mm256_storeu_ps(d + i,
                         _mm256_add_ps(_mm256_loadu_ps(d + i), _mm256_loadu_ps(s + i)));
    }
#elif DAW_SIMD_X86
    for (; i + 4 <= n; i += 4) {
        _mm_storeu_ps(d + i, _mm_add_ps(_mm_loadu_ps(d + i), _mm_loadu_ps(s + i)));
    }
#endif
    for (; i < n; ++i) d[i] += s[i];
}

/// destination = source * gain, in one pass over the data rather than a copy
/// followed by a scaling pass.
inline void copyScaled(std::span<float> destination,
                       std::span<const float> source, float gain) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    float* d = destination.data();
    const float* s = source.data();
    std::size_t i = 0;

#if DAW_SIMD_NEON
    const float32x4_t g = vdupq_n_f32(gain);
    for (; i + 4 <= n; i += 4) {
        vst1q_f32(d + i, vmulq_f32(vld1q_f32(s + i), g));
    }
#elif DAW_SIMD_X86 && defined(__AVX__)
    const __m256 g = _mm256_set1_ps(gain);
    for (; i + 8 <= n; i += 8) {
        _mm256_storeu_ps(d + i, _mm256_mul_ps(_mm256_loadu_ps(s + i), g));
    }
#elif DAW_SIMD_X86
    const __m128 g = _mm_set1_ps(gain);
    for (; i + 4 <= n; i += 4) {
        _mm_storeu_ps(d + i, _mm_mul_ps(_mm_loadu_ps(s + i), g));
    }
#endif
    for (; i < n; ++i) d[i] = s[i] * gain;
}

/// Peak magnitude of a buffer, used by meters. Every metered channel runs this
/// over every block, so it is worth vectorising even though it produces one
/// float: the scalar version's compare-and-branch per sample defeats the
/// pipeline, while the SIMD form is a straight max reduction.
inline float peak(std::span<const float> source) noexcept {
    const std::size_t n = source.size();
    const float* s = source.data();
    std::size_t i = 0;
    float highest = 0.0f;

#if DAW_SIMD_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i + 4 <= n; i += 4) {
        acc = vmaxq_f32(acc, vabsq_f32(vld1q_f32(s + i)));
    }
    highest = vmaxvq_f32(acc);
#elif DAW_SIMD_X86 && defined(__AVX__)
    // Clearing the sign bit is the branch-free absolute value.
    const __m256 mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m256 acc = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) {
        acc = _mm256_max_ps(acc, _mm256_and_ps(_mm256_loadu_ps(s + i), mask));
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, acc);
    for (float lane : lanes) highest = lane > highest ? lane : highest;
#elif DAW_SIMD_X86
    const __m128 mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    __m128 acc = _mm_setzero_ps();
    for (; i + 4 <= n; i += 4) {
        acc = _mm_max_ps(acc, _mm_and_ps(_mm_loadu_ps(s + i), mask));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, acc);
    for (float lane : lanes) highest = lane > highest ? lane : highest;
#endif
    for (; i < n; ++i) {
        const float magnitude = s[i] < 0.0f ? -s[i] : s[i];
        if (magnitude > highest) highest = magnitude;
    }
    return highest;
}

/// Write the sum of `inputs` into `output` — the mixer's gather step, shared by
/// every node that starts by collecting what feeds it.
///
/// The first contributor to a channel is *copied* instead of being accumulated
/// into a cleared buffer. Almost every node in a real session has exactly one
/// input, and clear-then-accumulate walks the block twice (and multiplies by
/// 1.0 all the way) to produce what a single copy already gives. Channels that
/// no input reaches are cleared, so the output is always fully defined.
inline void sumInto(const AudioBlock& output,
                    std::span<const AudioBlock> inputs) noexcept {
    // Channel-outer: a channel is finished and left behind before the next one
    // is touched, so the destination stays in L1 across its contributors.
    for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
        const std::span<float> destination = output.channel(ch);
        bool written = false;
        for (const AudioBlock& input : inputs) {
            if (ch >= input.numChannels()) continue;
            if (written) {
                add(destination, input.channel(ch));
            } else {
                copy(destination, input.channel(ch));
                written = true;
            }
        }
        if (!written) clear(destination);
    }
}

/// Linear ramp of gain across the block: destination += source * gain(t).
/// Ramping avoids the zipper noise a per-block gain step produces.
inline void addScaledRamp(std::span<float> destination,
                          std::span<const float> source, float startGain,
                          float endGain) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    if (n == 0) return;
    const float step = (endGain - startGain) / float(n);
    float gain = startGain;
    float* d = destination.data();
    const float* s = source.data();
    for (std::size_t i = 0; i < n; ++i, gain += step) d[i] += s[i] * gain;
}

/// destination = source * gain(t) — the ramping counterpart of copyScaled, for
/// the first contributor to a channel.
inline void copyScaledRamp(std::span<float> destination,
                           std::span<const float> source, float startGain,
                           float endGain) noexcept {
    const std::size_t n = std::min(destination.size(), source.size());
    if (n == 0) return;
    const float step = (endGain - startGain) / float(n);
    float gain = startGain;
    float* d = destination.data();
    const float* s = source.data();
    for (std::size_t i = 0; i < n; ++i, gain += step) d[i] = s[i] * gain;
}

} // namespace daw::engine::dsp
