#pragma once
#include <cstdint>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace daw::rt {
// Applies to each thread separately. The host restores the caller's FP mode;
// offline rendering and helper threads use exactly the same arithmetic mode.
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        m_saved = _mm_getcsr();
        _mm_setcsr(unsigned(m_saved) | 0x8040u);
#elif defined(__aarch64__) || defined(__arm64__)
        asm volatile("mrs %0, fpcr" : "=r"(m_saved));
        const auto value = m_saved | (std::uint64_t(1) << 24);
        asm volatile("msr fpcr, %0" : : "r"(value));
#endif
    }
    ~ScopedNoDenormals() {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        _mm_setcsr(unsigned(m_saved));
#elif defined(__aarch64__) || defined(__arm64__)
        asm volatile("msr fpcr, %0" : : "r"(m_saved));
#endif
    }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;
private:
    std::uint64_t m_saved = 0;
};
} // namespace daw::rt
