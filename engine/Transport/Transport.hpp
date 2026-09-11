#pragma once

#include "Common/Types.hpp"
#include "AudioPresentationClock.hpp"
#include "Job/BackgroundExecutor.hpp"

#include <atomic>
#include <chrono>
#include <cmath>

namespace daw::engine {

enum class TransportState : std::uint8_t { Stopped, Playing, Paused, Recording };

/// Sample-accurate transport shared between the control thread and the audio
/// thread. Every field is a plain atomic — the audio thread reads the state at
/// the top of each block and advances the playhead at the bottom, so a seek or
/// a tempo change never blocks and never tears a block in half.
class Transport {
public:
    void setSampleRate(SampleRate rate) noexcept {
        // prepare() also runs for a replacement device with the same rate.
        // Its old DAC history must not survive that stream restart.
        m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_sampleRate.store(rate, std::memory_order_relaxed);
    }
    SampleRate sampleRate() const noexcept {
        return m_sampleRate.load(std::memory_order_relaxed);
    }

    void play() noexcept { m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel); m_backgroundLease.setPlaying(true); m_state.store(TransportState::Playing, std::memory_order_release); }
    void pause() noexcept { m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel); m_backgroundLease.setPlaying(false); m_state.store(TransportState::Paused, std::memory_order_release); }
    void stop() noexcept { m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel); m_backgroundLease.setPlaying(false); m_state.store(TransportState::Stopped, std::memory_order_release); }
    void startRecording() noexcept {
        m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_backgroundLease.setPlaying(true);
        m_state.store(TransportState::Recording, std::memory_order_release);
    }

    TransportState state() const noexcept {
        return m_state.load(std::memory_order_acquire);
    }
    bool isPlaying() const noexcept {
        const TransportState s = state();
        return s == TransportState::Playing || s == TransportState::Recording;
    }
    bool isRecording() const noexcept { return state() == TransportState::Recording; }

    void seek(SamplePos position) noexcept {
        m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_position.store(position < 0 ? 0 : position, std::memory_order_release);
    }
    void seekSeconds(double seconds) noexcept {
        // Rounded, not truncated. Truncation puts the playhead one sample
        // *before* the time asked for on almost every value that is not an
        // exact multiple of the sample period — so a seek to a position this
        // same transport reported did not come back to it, and a loop boundary
        // landed a sample early every pass.
        seek(SamplePos(std::llround(seconds * sampleRate())));
    }
    SamplePos position() const noexcept {
        return m_position.load(std::memory_order_acquire);
    }
    double positionSeconds() const noexcept {
        const SampleRate rate = sampleRate();
        return rate > 0.0 ? double(position()) / rate : 0.0;
    }

    // Audio thread only. The device maps its DAC clock into steady-clock time.
    // Engine-only/offline callers leave this unset and retain an estimate.
    void setPresentationTiming(std::int64_t outputNs, PresentationClockSource source) noexcept {
        m_outputTimeNs = outputNs;
        m_outputFrameOffset = 0;
        m_clockSource = source;
        m_outputGeneration = presentationGeneration();
    }
    std::uint64_t presentationGeneration() const noexcept {
        return m_presentationGeneration.load(std::memory_order_acquire);
    }
    bool presentationSnapshot(std::int64_t atNs, AudioPresentationSnapshot& snapshot) const noexcept {
        // Fixed reader-local storage: no locks, allocations or retry loops
        // when a GUI/render read collides with the audio writer.
        thread_local std::array<AudioPresentationReader, 4> readers;
        auto& reader = readers[m_presentationClock.identity() % readers.size()];
        return isPlaying() && reader.readAt(m_presentationClock, atNs, presentationGeneration(), snapshot);
    }
    double presentationPositionSeconds() const noexcept {
        const auto now = PresentationFrameTime::now();
        AudioPresentationSnapshot snapshot;
        return presentationSnapshot(now, snapshot) ? snapshot.secondsAt(now) : positionSeconds();
    }

    void setDuration(SamplePos samples) noexcept {
        m_duration.store(samples, std::memory_order_relaxed);
    }
    SamplePos duration() const noexcept {
        return m_duration.load(std::memory_order_relaxed);
    }

    void setTempo(double bpm) noexcept {
        m_tempo.store(bpm, std::memory_order_relaxed);
    }
    double tempo() const noexcept { return m_tempo.load(std::memory_order_relaxed); }

    void setTimeSignature(int numerator, int denominator) noexcept {
        m_timeSigNumerator.store(numerator > 0 ? numerator : 4,
                                 std::memory_order_relaxed);
        m_timeSigDenominator.store(denominator > 0 ? denominator : 4,
                                   std::memory_order_relaxed);
    }
    int timeSigNumerator() const noexcept {
        return m_timeSigNumerator.load(std::memory_order_relaxed);
    }
    int timeSigDenominator() const noexcept {
        return m_timeSigDenominator.load(std::memory_order_relaxed);
    }

    /// Samples in one quarter note at the current tempo and rate.
    double samplesPerBeat() const noexcept {
        const double bpm = tempo();
        return bpm > 0.0 ? 60.0 / bpm * sampleRate() : 0.0;
    }

    /// Convert a sample position to quarter notes from the origin.
    double ppqAt(SamplePos position) const noexcept {
        const double perBeat = samplesPerBeat();
        return perBeat > 0.0 ? double(position) / perBeat : 0.0;
    }

    /// One bar in quarter notes. 4/4 → 4, 3/4 → 3, 6/8 → 3: the denominator
    /// says what note gets the beat, and ppq counts quarters either way.
    double beatsPerBar() const noexcept {
        const int denominator = timeSigDenominator();
        return denominator > 0
                   ? double(timeSigNumerator()) * 4.0 / double(denominator)
                   : 4.0;
    }

    /// Everything a node — or a hosted plugin — needs to know about musical
    /// time for the block starting at `position`. Built once per block on the
    /// audio thread, so a whole graph sees one consistent view.
    TransportInfo infoAt(SamplePos position) const noexcept {
        TransportInfo info;
        info.tempo = tempo();
        info.timeSigNumerator = timeSigNumerator();
        info.timeSigDenominator = timeSigDenominator();
        info.ppqPosition = ppqAt(position);
        const double barLength = beatsPerBar();
        info.barStartPpq =
            barLength > 0.0
                ? std::floor(info.ppqPosition / barLength) * barLength
                : 0.0;
        info.loopStartPpq = ppqAt(loopStart());
        info.loopEndPpq = ppqAt(loopEnd());
        info.looping = isLoopEnabled();
        info.recording = isRecording();
        return info;
    }

    void setLoopEnabled(bool enabled) noexcept {
        if (m_loopEnabled.exchange(enabled, std::memory_order_relaxed) != enabled)
            m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    bool isLoopEnabled() const noexcept {
        return m_loopEnabled.load(std::memory_order_relaxed);
    }
    void setLoopRange(SamplePos start, SamplePos end) noexcept {
        const bool changed = loopStart() != start || loopEnd() != end;
        m_loopStart.store(start, std::memory_order_relaxed);
        m_loopEnd.store(end, std::memory_order_relaxed);
        if (changed) m_presentationGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    SamplePos loopStart() const noexcept {
        return m_loopStart.load(std::memory_order_relaxed);
    }
    SamplePos loopEnd() const noexcept {
        return m_loopEnd.load(std::memory_order_relaxed);
    }

    /// Audio thread: advance by one block, wrapping at the loop end. Playback
    /// never auto-stops at the end of the arrangement — it keeps running
    /// forward through empty space, so Space is the only thing that pauses it.
    /// Returns the position the block that just rendered started at.
    SamplePos advance(FrameCount frames, bool deferPresentation = false) noexcept {
        const SamplePos start = position();
        SamplePos next = start + SamplePos(frames);

        if (isLoopEnabled()) {
            const SamplePos end = loopEnd();
            const SamplePos begin = loopStart();
            if (end > begin && next >= end) next = begin + (next - begin) % (end - begin);
        }
        const double rate = sampleRate();
        const auto timestamp = m_outputTimeNs > 0 && rate > 0
            ? m_outputTimeNs + std::int64_t(double(m_outputFrameOffset) * 1e9 / rate)
            : presentationNowNs();
        m_pendingPresentation = {start, frames, rate, timestamp,
            m_outputTimeNs > 0 ? m_outputGeneration : presentationGeneration(),
            loopStart(), loopEnd(), isLoopEnabled(),
            m_outputTimeNs > 0 ? m_clockSource : PresentationClockSource::RenderEstimate};
        if (!deferPresentation) finishPresentationBlock(0);
        m_outputFrameOffset += frames;
        m_position.store(next, std::memory_order_release);
        return start;
    }

    // Audio thread, after output has been rendered. A timeline sample emerges
    // from a delayed graph later than this device block's first output sample.
    // Move the DAC timestamp forward exactly once; consumers never subtract
    // either graph or device latency from the timeline position again.
    void finishPresentationBlock(FrameCount graphLatencyFrames) noexcept {
        auto snapshot = m_pendingPresentation;
        if (snapshot.sampleRate > 0 && snapshot.source != PresentationClockSource::RenderEstimate)
            snapshot.outputTimeNs += std::int64_t(double(graphLatencyFrames) * 1e9 / snapshot.sampleRate);
        m_presentationClock.publish(snapshot);
    }

private:
    BackgroundPlaybackLease m_backgroundLease;
    std::atomic<TransportState> m_state{TransportState::Stopped};
    std::atomic<SamplePos> m_position{0};
    std::atomic<SamplePos> m_duration{0};
    std::atomic<SamplePos> m_loopStart{0};
    std::atomic<SamplePos> m_loopEnd{0};
    std::atomic<SampleRate> m_sampleRate{48000.0};
    std::atomic<double> m_tempo{120.0};
    std::atomic<int> m_timeSigNumerator{4};
    std::atomic<int> m_timeSigDenominator{4};
    std::atomic<bool> m_loopEnabled{false};
    std::atomic<std::uint64_t> m_presentationGeneration{0};
    AudioPresentationClock m_presentationClock;
    AudioPresentationSnapshot m_pendingPresentation; // audio-thread-owned
    std::int64_t m_outputTimeNs = 0; // audio-thread-owned
    std::uint64_t m_outputFrameOffset = 0;
    std::uint64_t m_outputGeneration = 0;
    PresentationClockSource m_clockSource = PresentationClockSource::RenderEstimate;

};

} // namespace daw::engine
