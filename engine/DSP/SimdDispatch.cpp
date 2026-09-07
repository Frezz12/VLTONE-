#include "DSP/SimdDispatch.hpp"
#if defined(DAW_RUNTIME_AVX2) && defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#endif
namespace daw::engine::dsp {
#if defined(DAW_RUNTIME_AVX2)
extern const Avx2Kernels kAvx2Implementation;
namespace {
bool supported() noexcept {
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 0);
    if (registers[0] < 7) return false;
    __cpuidex(registers, 1, 0);
    constexpr int required = (1 << 27) | (1 << 28) | (1 << 12) | (1 << 29);
    if ((registers[2] & required) != required || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(registers, 7, 0);
    // /arch:AVX2 may also emit BMI instructions.
    constexpr int leaf7 = (1 << 5) | (1 << 3) | (1 << 8);
    return (registers[1] & leaf7) == leaf7;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#endif
}
}
const Avx2Kernels* const kAvx2Kernels = supported() ? &kAvx2Implementation : nullptr;
#else
const Avx2Kernels* const kAvx2Kernels = nullptr;
#endif
}
