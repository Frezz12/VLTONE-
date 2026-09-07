#include "DSP/SimdDispatch.hpp"
#include <immintrin.h>
namespace daw::engine::dsp {
namespace {
void gain(float* d, std::size_t n, float value) noexcept {
    const auto g = _mm256_set1_ps(value);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(d + i, _mm256_mul_ps(_mm256_loadu_ps(d + i), g));
    for (; i < n; ++i) d[i] *= value;
}
void accumulate(float* d, const float* s, std::size_t n, float value) noexcept {
    const auto g = _mm256_set1_ps(value);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(d + i,
        _mm256_add_ps(_mm256_loadu_ps(d + i), _mm256_mul_ps(_mm256_loadu_ps(s + i), g)));
    for (; i < n; ++i) d[i] += s[i] * value;
}
void scaled(float* d, const float* s, std::size_t n, float value) noexcept {
    const auto g = _mm256_set1_ps(value);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(d + i, _mm256_mul_ps(_mm256_loadu_ps(s + i), g));
    for (; i < n; ++i) d[i] = s[i] * value;
}
}
extern const Avx2Kernels kAvx2Implementation{gain, accumulate, scaled};
}
