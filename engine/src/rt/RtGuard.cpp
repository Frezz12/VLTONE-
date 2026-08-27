#include "daw/rt/RtGuard.h"

#include <cstdio>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define DAW_SSE 1
    #include <pmmintrin.h>
    #include <xmmintrin.h>
#elif defined(__aarch64__)
    #define DAW_NEON 1
#endif

namespace daw::rt {
namespace {

// thread_local, а не atomic: флаг принадлежит потоку и никем больше не читается.
thread_local bool tlsIsAudioThread = false;

std::atomic<std::uint64_t> gViolationCount{0};
std::atomic<const char*>   gLastViolation{nullptr};
std::atomic<bool>          gAbortOnViolation{false};

} // namespace

bool isAudioThread() noexcept { return tlsIsAudioThread; }
void setAudioThread(bool value) noexcept { tlsIsAudioThread = value; }

void reportViolation(const char* what) noexcept {
    gViolationCount.fetch_add(1, std::memory_order_relaxed);
    gLastViolation.store(what, std::memory_order_relaxed);

    if (gAbortOnViolation.load(std::memory_order_relaxed)) {
        // Уже роняем процесс, так что можно и написать в stderr.
        std::fprintf(stderr, "\n[RT] Нарушение реального времени в аудио-потоке: %s\n", what);
        std::fflush(stderr);
        std::abort();
    }
}

Violations violations() noexcept {
    Violations v;
    v.count = gViolationCount.load(std::memory_order_relaxed);
    v.last  = gLastViolation.load(std::memory_order_relaxed);
    return v;
}

void resetViolations() noexcept {
    gViolationCount.store(0, std::memory_order_relaxed);
    gLastViolation.store(nullptr, std::memory_order_relaxed);
}

void setAbortOnViolation(bool value) noexcept {
    gAbortOnViolation.store(value, std::memory_order_relaxed);
}

bool checksEnabled() noexcept {
#if defined(DAW_RT_CHECKS)
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// ScopedNoDenormals
// ---------------------------------------------------------------------------

ScopedNoDenormals::ScopedNoDenormals() noexcept : saved_(0) {
#if defined(DAW_SSE)
    saved_ = _mm_getcsr();
    // 0x8000 = flush-to-zero, 0x0040 = denormals-are-zero
    _mm_setcsr(static_cast<unsigned int>(saved_) | 0x8040u);
#elif defined(DAW_NEON)
    std::uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    saved_ = fpcr;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));  // FZ
#endif
}

ScopedNoDenormals::~ScopedNoDenormals() noexcept {
#if defined(DAW_SSE)
    _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(DAW_NEON)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_));
#endif
}

} // namespace daw::rt

// ---------------------------------------------------------------------------
// Подмена глобальных operator new/delete.
//
// Работает потому, что этот .obj гарантированно попадает в линковку: Engine.cpp
// вызывает setAudioThread() из этого же TU. Если бы TU был «сиротой», линкер
// взял бы реализацию из CRT и проверка молча не работала бы.
// ---------------------------------------------------------------------------
#if defined(DAW_RT_CHECKS)

#include <cstddef>
#include <new>

namespace {

inline void* dawAlloc(std::size_t size, const char* what) noexcept {
    if (daw::rt::isAudioThread())
        daw::rt::reportViolation(what);
    if (size == 0) size = 1;
    return std::malloc(size);
}

inline void* dawAllocAligned(std::size_t size, std::size_t align, const char* what) noexcept {
    if (daw::rt::isAudioThread())
        daw::rt::reportViolation(what);
    if (size == 0) size = 1;
#if defined(_MSC_VER)
    return _aligned_malloc(size, align);
#else
    void* p = nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    if (posix_memalign(&p, align, size) != 0) return nullptr;
    return p;
#endif
}

inline void dawFree(void* p) noexcept {
    if (daw::rt::isAudioThread())
        daw::rt::reportViolation("operator delete");
    std::free(p);
}

inline void dawFreeAligned(void* p) noexcept {
    if (daw::rt::isAudioThread())
        daw::rt::reportViolation("operator delete (aligned)");
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

void* operator new(std::size_t n) {
    void* p = dawAlloc(n, "operator new");
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    void* p = dawAlloc(n, "operator new[]");
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return dawAlloc(n, "operator new (nothrow)");
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return dawAlloc(n, "operator new[] (nothrow)");
}
void* operator new(std::size_t n, std::align_val_t a) {
    void* p = dawAllocAligned(n, static_cast<std::size_t>(a), "operator new (aligned)");
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
    void* p = dawAllocAligned(n, static_cast<std::size_t>(a), "operator new[] (aligned)");
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return dawAllocAligned(n, static_cast<std::size_t>(a), "operator new (aligned, nothrow)");
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return dawAllocAligned(n, static_cast<std::size_t>(a), "operator new[] (aligned, nothrow)");
}

void operator delete(void* p) noexcept                        { dawFree(p); }
void operator delete[](void* p) noexcept                      { dawFree(p); }
void operator delete(void* p, std::size_t) noexcept           { dawFree(p); }
void operator delete[](void* p, std::size_t) noexcept         { dawFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { dawFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { dawFree(p); }

void operator delete(void* p, std::align_val_t) noexcept                        { dawFreeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept                      { dawFreeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept           { dawFreeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept         { dawFreeAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { dawFreeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { dawFreeAligned(p); }

#endif // DAW_RT_CHECKS
