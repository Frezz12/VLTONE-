#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace daw::engine {

enum class TransportState : std::uint8_t { Stopped, Playing, Paused, Recording };

/// One audio writer publishes the unwrapped input clock. Playback state is a
/// separate control-thread atomic: control edits never enter the audio seqlock.
/// Scalars and the bounded reader are identical for local and mapped clocks.
class alignas(64) InputClock final {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
        std::atomic<double>::is_always_lock_free && std::atomic<TransportState>::is_always_lock_free);

    void setState(TransportState state) noexcept { m_state.store(state, std::memory_order_release); }
    TransportState state() const noexcept { return m_state.load(std::memory_order_acquire); }
    bool isPlaying() const noexcept {
        const auto value = state();
        return value == TransportState::Playing || value == TransportState::Recording;
    }
    double endBeat() const noexcept { return m_endBeat.load(std::memory_order_acquire); }

    void publish(std::uint64_t ns, double begin, double duration, double tempo) noexcept {
        m_sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        m_ns.store(ns, std::memory_order_relaxed);
        m_beat.store(begin, std::memory_order_relaxed);
        m_duration.store(duration, std::memory_order_relaxed);
        m_tempo.store(tempo, std::memory_order_relaxed);
        m_endBeat.store(begin + duration * tempo / 60.0, std::memory_order_release);
        m_sequence.fetch_add(1, std::memory_order_release);
    }

    double inputBeatsAt(std::uint64_t ns) const noexcept {
        for (int attempt = 0; attempt != 3; ++attempt) {
            const auto before = m_sequence.load(std::memory_order_acquire);
            if (before & 1) continue;
            const auto at = m_ns.load(std::memory_order_relaxed);
            const double begin = m_beat.load(std::memory_order_relaxed);
            const double duration = m_duration.load(std::memory_order_relaxed);
            const double bpm = m_tempo.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before != m_sequence.load(std::memory_order_relaxed)) continue;
            if (!isPlaying()) return begin + duration * bpm / 60.0;
            const double elapsed = ns > at ? double(ns - at) / 1e9 : 0.0;
            return begin + std::clamp(elapsed, 0.0, duration) * bpm / 60.0;
        }
        return endBeat();
    }

    /// Control thread, with the source audio writer drained by RenderGate.
    /// Destination readers remain safe while a retired slot is initialized.
    void copyFrom(const InputClock& source) noexcept {
        if (this == &source) return;
        setState(source.state());
        publish(source.m_ns.load(std::memory_order_relaxed), source.m_beat.load(std::memory_order_relaxed),
            source.m_duration.load(std::memory_order_relaxed), source.m_tempo.load(std::memory_order_relaxed));
    }
private:
    std::atomic<std::uint64_t> m_sequence{0}, m_ns{0};
    std::atomic<double> m_beat{0}, m_duration{0}, m_tempo{120}, m_endBeat{0};
    std::atomic<TransportState> m_state{TransportState::Stopped};
};

} // namespace daw::engine
