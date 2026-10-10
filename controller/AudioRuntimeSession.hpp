#pragma once

#include <cstdint>
#include <utility>

namespace daw {
/// The runtime owns the snapshot; a document operation holds its numeric lease.
template<class Runtime> class ScopedAudioTransaction {
public:
    explicit ScopedAudioTransaction(Runtime& runtime, bool enabled = true)
        : m_runtime(runtime), m_id(enabled ? runtime.captureTransaction() : 0) {}
    ~ScopedAudioTransaction() { if (m_id) m_runtime.releaseTransaction(m_id); }
    ScopedAudioTransaction(const ScopedAudioTransaction&) = delete;
    ScopedAudioTransaction& operator=(const ScopedAudioTransaction&) = delete;
    auto restore() { return m_runtime.restoreTransaction(m_id); }
private:
    Runtime& m_runtime;
    std::uint64_t m_id;
};
} // namespace daw
