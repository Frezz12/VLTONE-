#pragma once

#include "Transport/InputClock.hpp"
#include "Transport/AudioPresentationClock.hpp"

#include <memory>

namespace daw::audioipc {

inline constexpr std::size_t kInputClockSlots = 32;

/// Input timestamps and DAC history share a session identity. Retire generation
/// first, detach/drain the previous writer, initialize, then publish identity.
struct alignas(64) InputClockSlot {
    std::atomic<std::uint64_t> sessionId{0}, generation{0};
    engine::InputClock clock;
    engine::AudioPresentationClock presentation;
};

} // namespace daw::audioipc

namespace daw {

/// Acquire on the broker after session acknowledgment, then retain on MIDI/UI
/// readers. The aliasing owner pins only OS mapping lifetime, never native DSP.
/// Input timestamps and audible display positions need no RPC, allocation,
/// mutex or system call, independently of the UI telemetry polling interval.
class AudioInputClockReader final {
public:
    AudioInputClockReader() = default;
    AudioInputClockReader(std::shared_ptr<const audioipc::InputClockSlot> slot,
        std::uint64_t sessionId, std::uint64_t generation)
        : m_slot(std::move(slot)), m_sessionId(sessionId), m_generation(generation) {}

    bool valid() const noexcept {
        return m_slot && m_generation && m_slot->generation.load(std::memory_order_acquire) == m_generation &&
            m_slot->sessionId.load(std::memory_order_relaxed) == m_sessionId;
    }
    bool read(std::uint64_t ns, double& beats) const noexcept {
        if (!valid()) return false;
        const double value = m_slot->clock.inputBeatsAt(ns);
        // A close/replacement can retire this identity during the bounded
        // scalar read. Never return the clock of a reused slot or generation.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!valid()) return false;
        beats = value;
        return true;
    }
    double inputBeatsAt(std::uint64_t ns) const noexcept {
        double beats = 0;
        (void)read(ns, beats);
        return beats;
    }
    bool readPresentation(std::int64_t ns, double& seconds) const noexcept {
        if (!valid() || !m_slot->clock.isPlaying()) return false;
        const auto generation = m_slot->presentation.generation();
        engine::AudioPresentationSnapshot snapshot;
        if (!m_slot->presentation.readAt(ns, generation, snapshot)) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!valid() || !m_slot->clock.isPlaying() || m_slot->presentation.generation() != generation) return false;
        seconds = snapshot.secondsAt(ns);
        return true;
    }
    std::uint64_t generation() const noexcept { return m_generation; }
    std::uint64_t sessionId() const noexcept { return m_sessionId; }
private:
    std::shared_ptr<const audioipc::InputClockSlot> m_slot;
    std::uint64_t m_sessionId = 0, m_generation = 0;
};

} // namespace daw
