#pragma once
#include <cstddef>
namespace daw::engine::dsp {
struct Avx2Kernels {
    void (*applyGain)(float*, std::size_t, float) noexcept;
    void (*addScaled)(float*, const float*, std::size_t, float) noexcept;
    void (*copyScaled)(float*, const float*, std::size_t, float) noexcept;
};
// Selected once from baseline code. A null table leaves the inline SSE2/NEON
// path in place. The accelerated object is built without IPO so its ISA cannot
// spread into baseline callers during link-time optimization.
extern const Avx2Kernels* const kAvx2Kernels;
}
