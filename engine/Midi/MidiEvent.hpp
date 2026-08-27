#pragma once

#include "Common/RealtimeSort.hpp"

#include "Common/Types.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace daw::engine {

/// One MIDI channel-voice message, timed inside a block.
///
/// Three bytes and an offset — the wire format, not a parsed note. Everything
/// the engine routes fits in that: note on/off, controllers, pitch bend,
/// pressure. SysEx does not, and is deliberately out of scope; nothing in this
/// project produces or consumes it, and carrying it would mean variable-length
/// events in a buffer that must not allocate.
struct MidiEvent {
    FrameCount frameOffset = 0;   ///< from the start of the block
    std::uint8_t status = 0;      ///< status byte, channel included
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    /// Assigned by the destination MidiBuffer. It is host metadata, not part of
    /// the three-byte wire message, and makes equal-frame arrival order explicit
    /// for the dense O(N log N) sorting path.
    std::uint32_t sortOrder = 0;

    static constexpr std::uint8_t kNoteOff = 0x80;
    static constexpr std::uint8_t kNoteOn = 0x90;
    static constexpr std::uint8_t kPolyPressure = 0xA0;
    static constexpr std::uint8_t kControlChange = 0xB0;
    static constexpr std::uint8_t kProgramChange = 0xC0;
    static constexpr std::uint8_t kChannelPressure = 0xD0;
    static constexpr std::uint8_t kPitchBend = 0xE0;

    std::uint8_t type() const noexcept { return std::uint8_t(status & 0xF0); }
    std::uint8_t channel() const noexcept { return std::uint8_t(status & 0x0F); }

    /// A note-on with zero velocity is a note-off — a convention old enough
    /// that plenty of real files and controllers still rely on it, and one
    /// that a synth reading only the status byte would get wrong.
    bool isNoteOn() const noexcept { return type() == kNoteOn && data2 > 0; }
    bool isNoteOff() const noexcept {
        return type() == kNoteOff || (type() == kNoteOn && data2 == 0);
    }

    static MidiEvent noteOn(FrameCount offset, std::uint8_t channel, std::uint8_t key,
                            std::uint8_t velocity) noexcept {
        return {offset, std::uint8_t(kNoteOn | (channel & 0x0F)), key, velocity};
    }
    static MidiEvent noteOff(FrameCount offset, std::uint8_t channel,
                             std::uint8_t key) noexcept {
        return {offset, std::uint8_t(kNoteOff | (channel & 0x0F)), key, 0};
    }
};

/// How many events one node can emit or receive in a block.
///
/// 512 is far past what music produces — a dense 32nd-note chord at 200 BPM is
/// a handful per block — and generous enough that hitting it means something is
/// wrong rather than merely busy.
inline constexpr std::size_t kMidiEventsPerBlock = 512;

/// One block's worth of MIDI, with a fixed capacity.
///
/// `push` never grows: this is written from `process`, and a reallocation there
/// is a dropout. At capacity, ordinary messages are refused. A note-off is more
/// important because losing one can leave a voice sounding forever, so it may
/// evict a lower-priority event already in the block while preserving every
/// release already queued. Both cases are counted for diagnostics.
class MidiBuffer {
public:
    void reserve(std::size_t capacity) { m_events.reserve(capacity); }
    void clear() noexcept {
        m_events.clear();
        m_nextSortOrder = 0;
    }

    bool push(const MidiEvent& event) noexcept {
        MidiEvent queued = event;
        queued.sortOrder = m_nextSortOrder;
        if (m_events.size() >= m_events.capacity()) {
            if (queued.isNoteOff()) {
                // Prefer sacrificing continuous controller traffic. If the
                // buffer contains only note messages, sacrifice a note-on for
                // another key before the matching note-on. That keeps the new
                // release meaningful whenever possible and never trades away
                // a release which is already safe.
                auto replacement = m_events.end();
                for (auto it = m_events.end(); it != m_events.begin();) {
                    --it;
                    if (!it->isNoteOn() && !it->isNoteOff()) {
                        replacement = it;
                        break;
                    }
                }
                if (replacement == m_events.end()) {
                    for (auto it = m_events.end(); it != m_events.begin();) {
                        --it;
                        if (it->isNoteOn() &&
                            (it->channel() != queued.channel() ||
                             it->data1 != queued.data1)) {
                            replacement = it;
                            break;
                        }
                    }
                }
                if (replacement == m_events.end()) {
                    for (auto it = m_events.end(); it != m_events.begin();) {
                        --it;
                        if (it->isNoteOn()) {
                            replacement = it;
                            break;
                        }
                    }
                }
                if (replacement != m_events.end()) {
                    // The incoming release was observed after every event already
                    // in the block. Replacing a victim in place can invert that
                    // order when frame offsets tie (off, then matching on), which
                    // is worse than the overflow we are trying to recover from.
                    // Remove the victim with a bounded shift and append the
                    // release as the newest equal-time event.
                    std::move(replacement + 1, m_events.end(), replacement);
                    m_events.back() = queued;
                    ++m_nextSortOrder;
                    ++m_noteOffRescues;
                    return true;
                }
            }
            ++m_droppedEvents;
            return false;
        }
        m_events.push_back(queued);
        ++m_nextSortOrder;
        return true;
    }

    /// Stable, so two events at the same frame keep the order they were added —
    /// which is what makes a note-off before a note-on at the same offset stay
    /// that way, instead of cutting the note it was meant to precede.
    void sort() noexcept {
        constexpr std::size_t kInsertionSortThreshold = 32;
        if (m_events.size() <= kInsertionSortThreshold) {
            stableRealtimeSort(m_events.begin(), m_events.end(),
                               [](const MidiEvent& a, const MidiEvent& b) {
                                   return a.frameOffset < b.frameOffset;
                               });
            return;
        }
        // std::sort is allocation-free introsort in the supported standard
        // libraries. Explicit arrival order supplies stable semantics without
        // the O(N^2) movement of insertion sort on a reversed 512-event burst.
        std::sort(m_events.begin(), m_events.end(),
                  [](const MidiEvent& a, const MidiEvent& b) {
                      if (a.frameOffset != b.frameOffset) {
                          return a.frameOffset < b.frameOffset;
                      }
                      return a.sortOrder < b.sortOrder;
                  });
    }

    std::span<const MidiEvent> events() const noexcept { return m_events; }
    bool empty() const noexcept { return m_events.empty(); }
    std::size_t size() const noexcept { return m_events.size(); }
    std::uint64_t droppedEvents() const noexcept { return m_droppedEvents; }
    std::uint64_t noteOffRescues() const noexcept { return m_noteOffRescues; }
    void resetDiagnostics() noexcept {
        m_droppedEvents = 0;
        m_noteOffRescues = 0;
    }

private:
    std::vector<MidiEvent> m_events;
    std::uint32_t m_nextSortOrder = 0;
    std::uint64_t m_droppedEvents = 0;
    std::uint64_t m_noteOffRescues = 0;
};

/// Delays MIDI by the same number of samples as the audio on the same edge.
///
/// Without this, latency compensation would move the audio and leave the notes
/// where they were: an instrument behind a compensated effect would be played
/// early by exactly that effect's latency. Events that fall past the end of the
/// block are held and emitted by a later one, which is why this has state.
class MidiDelay {
public:
    void prepare(FrameCount delaySamples, std::size_t capacity) {
        m_delay = delaySamples;
        m_pending.reserve(capacity);
        m_capacity = capacity;
        m_pending.clear();
        m_frameCursor = 0;
        m_sequence = 0;
        m_droppedPending = 0;
        m_noteOffRescues = 0;
    }
    void reset() noexcept {
        m_pending.clear();
        m_frameCursor = 0;
        m_sequence = 0;
    }
    FrameCount delaySamples() const noexcept { return m_delay; }
    std::uint64_t droppedPendingEvents() const noexcept {
        return m_droppedPending;
    }
    std::uint64_t pendingNoteOffRescues() const noexcept {
        return m_noteOffRescues;
    }

    void process(const MidiBuffer& input, MidiBuffer& output, FrameCount frames) noexcept {
        output.clear();

        const std::uint64_t blockEnd = m_frameCursor + frames;
        // Pending events are a fixed-capacity min-heap in absolute sample time.
        // A long compensation delay therefore touches only events due this
        // block (heap operations are O(log P)), instead of rescanning and
        // decrementing every future event each block.
        while (!m_pending.empty()) {
            const Pending& next = m_pending.front();
            if (next.dueFrame >= blockEnd) break;
            std::pop_heap(m_pending.begin(), m_pending.end(), pendingLater);
            Pending due = m_pending.back();
            m_pending.pop_back();
            due.event.frameOffset = FrameCount(due.dueFrame - m_frameCursor);
            output.push(due.event);
        }

        for (MidiEvent event : input.events()) {
            const std::uint64_t dueFrame = m_frameCursor + event.frameOffset +
                                           std::uint64_t(m_delay);
            if (dueFrame < blockEnd) {
                event.frameOffset = FrameCount(dueFrame - m_frameCursor);
                output.push(event);
            } else if (m_pending.size() < m_capacity) {
                m_pending.push_back(Pending{dueFrame, m_sequence++, event});
                std::push_heap(m_pending.begin(), m_pending.end(), pendingLater);
            } else {
                // The delayed queue has the same release guarantee as a block
                // buffer. Rebuilding a fixed 512-item heap is bounded and only
                // occurs during an already exceptional overflow burst.
                auto replacement = m_pending.end();
                if (event.isNoteOff()) {
                    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                        if (!it->event.isNoteOn() && !it->event.isNoteOff()) {
                            replacement = it;
                            break;
                        }
                    }
                    if (replacement == m_pending.end()) {
                        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                            if (it->event.isNoteOn() &&
                                (it->event.channel() != event.channel() ||
                                 it->event.data1 != event.data1)) {
                                replacement = it;
                                break;
                            }
                        }
                    }
                    if (replacement == m_pending.end()) {
                        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                            if (it->event.isNoteOn()) {
                                replacement = it;
                                break;
                            }
                        }
                    }
                }
                if (replacement != m_pending.end()) {
                    *replacement = Pending{dueFrame, m_sequence++, event};
                    std::make_heap(m_pending.begin(), m_pending.end(), pendingLater);
                    ++m_noteOffRescues;
                } else {
                    ++m_droppedPending;
                }
            }
        }
        m_frameCursor = blockEnd;
        output.sort();
    }

private:
    struct Pending {
        std::uint64_t dueFrame = 0;
        std::uint64_t sequence = 0;
        MidiEvent event;
    };

    static bool pendingLater(const Pending& left, const Pending& right) noexcept {
        if (left.dueFrame != right.dueFrame)
            return left.dueFrame > right.dueFrame;
        return left.sequence > right.sequence;
    }

    std::vector<Pending> m_pending;
    std::size_t m_capacity = 0;
    std::uint64_t m_frameCursor = 0;
    std::uint64_t m_sequence = 0;
    std::uint64_t m_droppedPending = 0;
    std::uint64_t m_noteOffRescues = 0;
    FrameCount m_delay = 0;
};

} // namespace daw::engine
