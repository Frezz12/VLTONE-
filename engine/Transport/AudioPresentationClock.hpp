#pragma once

#include "Common/Types.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace daw::engine {
enum class PresentationClockSource : std::uint8_t { RenderEstimate, DeviceLatency, DeviceTimestamp };

inline std::int64_t presentationNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// GUI/render preparation scope: all editors sample the same point in time.
// Thread-local state never crosses into, or synchronizes with, the callback.
class PresentationFrameTime {
public:
    PresentationFrameTime() noexcept : previous(value) { value = presentationNowNs(); }
    ~PresentationFrameTime() { value = previous; }
    PresentationFrameTime(const PresentationFrameTime&) = delete;
    PresentationFrameTime& operator=(const PresentationFrameTime&) = delete;
    static std::int64_t now() noexcept { return value ? value : presentationNowNs(); }
private:
    inline static thread_local std::int64_t value = 0;
    std::int64_t previous;
};

struct AudioPresentationSnapshot {
    SamplePos blockStart = 0;
    FrameCount frames = 0;
    double sampleRate = 48000;
    std::int64_t outputTimeNs = 0;
    std::uint64_t generation = 0;
    SamplePos loopStart = 0, loopEnd = 0;
    bool looping = false;
    PresentationClockSource source = PresentationClockSource::RenderEstimate;

    double secondsAt(std::int64_t timeNs) const noexcept {
        if (!(sampleRate > 0) || !std::isfinite(sampleRate)) return 0;
        double elapsed = std::clamp(double(timeNs - outputTimeNs) * sampleRate / 1e9,
                                    0., double(frames));
        double sample = double(blockStart) + elapsed;
        if (looping && loopEnd > loopStart && sample >= double(loopEnd))
            sample = double(loopStart) + std::fmod(sample - double(loopStart), double(loopEnd - loopStart));
        return sample / sampleRate;
    }
};

// One audio writer, any number of readers. Fixed storage and bounded reads:
// neither the callback nor a render thread can wait for the other. Keep past
// blocks because the newest rendered block may not have reached the DAC yet.
class AudioPresentationClock {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
                  std::atomic<SamplePos>::is_always_lock_free && std::atomic<double>::is_always_lock_free,
                  "Audio presentation history requires lock-free scalar atomics");
    static constexpr std::size_t capacity = 512;
    std::uint64_t identity() const noexcept { return m_identity; }
    void publish(const AudioPresentationSnapshot& value) noexcept {
        const auto ticket = m_written.load(std::memory_order_relaxed) + 1;
        auto& s = m_slots[ticket % capacity];
        s.sequence.fetch_add(1, std::memory_order_acq_rel);
        // Pairs with the reader's acquire fence when it observes ANY new
        // field. Its final sequence read must then also observe the odd stamp
        // (or a later one), including on weakly ordered architectures.
        std::atomic_thread_fence(std::memory_order_release);
        s.ticket.store(ticket, std::memory_order_relaxed);
        s.start.store(value.blockStart, std::memory_order_relaxed);
        s.frames.store(value.frames, std::memory_order_relaxed);
        s.rate.store(value.sampleRate, std::memory_order_relaxed);
        s.time.store(value.outputTimeNs, std::memory_order_relaxed);
        s.generation.store(value.generation, std::memory_order_relaxed);
        s.loopStart.store(value.loopStart, std::memory_order_relaxed);
        s.loopEnd.store(value.loopEnd, std::memory_order_relaxed);
        s.looping.store(value.looping, std::memory_order_relaxed);
        s.source.store(value.source, std::memory_order_relaxed);
        s.sequence.fetch_add(1, std::memory_order_release);
        m_written.store(ticket, std::memory_order_release);
    }

    bool readAt(std::int64_t timeNs, std::uint64_t generation,
                AudioPresentationSnapshot& result) const noexcept {
        const auto latest = m_written.load(std::memory_order_acquire);
        const auto count = std::min<std::uint64_t>(latest, capacity);
        bool found = false;
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            const auto ticket = latest - offset;
            AudioPresentationSnapshot value;
            if (!read(ticket, value)) continue;
            if (value.generation != generation) break;
            result = value;
            found = true;
            if (value.outputTimeNs <= timeNs) break;
        }
        // If all queued audio is still in the future, return the earliest
        // available start, never a future block or extrapolated stale audio.
        return found;
    }
private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{0}, ticket{0}, generation{0};
        std::atomic<SamplePos> start{0}, loopStart{0}, loopEnd{0};
        std::atomic<FrameCount> frames{0};
        std::atomic<double> rate{48000};
        std::atomic<std::int64_t> time{0};
        std::atomic<bool> looping{false};
        std::atomic<PresentationClockSource> source{PresentationClockSource::RenderEstimate};
    };
    bool read(std::uint64_t ticket, AudioPresentationSnapshot& value) const noexcept {
        const auto& s = m_slots[ticket % capacity];
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto before = s.sequence.load(std::memory_order_acquire);
            if (before & 1) continue;
            const auto actualTicket = s.ticket.load(std::memory_order_relaxed);
            value.blockStart = s.start.load(std::memory_order_relaxed);
            value.frames = s.frames.load(std::memory_order_relaxed);
            value.sampleRate = s.rate.load(std::memory_order_relaxed);
            value.outputTimeNs = s.time.load(std::memory_order_relaxed);
            value.generation = s.generation.load(std::memory_order_relaxed);
            value.loopStart = s.loopStart.load(std::memory_order_relaxed);
            value.loopEnd = s.loopEnd.load(std::memory_order_relaxed);
            value.looping = s.looping.load(std::memory_order_relaxed);
            value.source = s.source.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == s.sequence.load(std::memory_order_relaxed) && actualTicket == ticket)
                return true;
        }
        return false;
    }
    std::array<Slot, capacity> m_slots;
    std::atomic<std::uint64_t> m_written{0};
    inline static std::atomic<std::uint64_t> s_nextIdentity{1};
    const std::uint64_t m_identity = s_nextIdentity.fetch_add(1, std::memory_order_relaxed);
};

// Reader-owned fallback, never shared with the audio writer. On a bounded
// read conflict retain the last complete tuple, but never across seek/restart
// generations or a new clock allocated at the same memory address.
class AudioPresentationReader {
public:
    bool readAt(const AudioPresentationClock& clock, std::int64_t timeNs,
                std::uint64_t generation, AudioPresentationSnapshot& result) noexcept {
        if (m_identity != clock.identity() || m_generation != generation) {
            m_valid = false; m_identity = clock.identity(); m_generation = generation;
        }
        // A frame scope supplies the exact same time to all editors. Reuse
        // its tuple even if another audio block is published during painting.
        if (!m_valid || m_atNs != timeNs) {
            AudioPresentationSnapshot value;
            if (clock.readAt(timeNs, generation, value)) { m_last = value; m_valid = true; }
            m_atNs = timeNs;
        }
        if (m_valid) result = m_last;
        return m_valid;
    }
private:
    AudioPresentationSnapshot m_last;
    std::uint64_t m_identity = 0, m_generation = 0;
    std::int64_t m_atNs = 0;
    bool m_valid = false;
};
} // namespace daw::engine
