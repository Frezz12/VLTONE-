#pragma once

#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"
#include "Midi/MidiEvent.hpp"
#include "Common/RealtimeSnapshot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace daw::engine {

/// One note on the timeline, in musical time.
///
/// Beats, not samples, and deliberately so: a tempo change has to move the
/// notes, and the only way that happens for free is if the note never held a
/// sample position to begin with. The conversion happens per block, against the
/// transport the whole graph agrees on.
struct MidiNote {
    double startBeats = 0.0;    ///< from the start of the timeline
    double lengthBeats = 1.0;
    std::uint8_t key = 60;
    std::uint8_t velocity = 100;
    std::uint8_t channel = 0;
};

/// Plays MIDI clips: turns a list of notes into note-on and note-off events at
/// the right sample offsets.
///
/// The note list is published as an immutable snapshot, like `ClipPlayerNode`'s
/// clip list, so editing in the piano roll never locks the audio thread.
class MidiClipPlayerNode : public Node {
public:
    using NoteList = std::vector<MidiNote>;

private:
    /// Control-thread-prepared interval index. `subtreeMaxEnd[mid]` is the
    /// latest note end in the balanced subtree whose root is `mid`. It lets a
    /// seek chase prune whole runs of expired notes without allocating or
    /// locking in the audio callback.
    struct NoteSchedule {
        std::shared_ptr<const NoteList> notes;
        std::vector<double> subtreeMaxEnd;
        std::uint64_t revision = 0;
    };

    static double buildSubtreeMax(NoteSchedule& schedule, std::size_t first,
                                  std::size_t last) {
        if (first >= last) return -std::numeric_limits<double>::infinity();
        const std::size_t mid = first + (last - first) / 2;
        const MidiNote& note = (*schedule.notes)[mid];
        double maxEnd = note.startBeats + note.lengthBeats;
        if (std::isnan(maxEnd))
            maxEnd = -std::numeric_limits<double>::infinity();
        maxEnd = std::max(maxEnd, buildSubtreeMax(schedule, first, mid));
        maxEnd =
            std::max(maxEnd, buildSubtreeMax(schedule, mid + 1, last));
        schedule.subtreeMaxEnd[mid] = maxEnd;
        return maxEnd;
    }

    static std::shared_ptr<const NoteSchedule> makeSchedule(
        std::shared_ptr<const NoteList> notes, std::uint64_t revision) {
        if (!notes) notes = std::make_shared<const NoteList>();
        auto schedule = std::make_shared<NoteSchedule>();
        schedule->notes = std::move(notes);
        schedule->revision = revision;
        schedule->subtreeMaxEnd.resize(schedule->notes->size());
        buildSubtreeMax(*schedule, 0, schedule->notes->size());
        return schedule;
    }

public:
    explicit MidiClipPlayerNode(std::string name = "MIDI Clips")
        : m_name(std::move(name)),
          m_schedule(makeSchedule(std::make_shared<const NoteList>(), 0)) {}

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::Output; }

    /// Control thread: swap in a new set of notes.
    void setNotes(std::shared_ptr<const NoteList> notes) {
        m_schedule.publish(
            makeSchedule(std::move(notes), ++m_scheduleRevision));
    }
    std::shared_ptr<const NoteList> notes() const {
        const auto schedule = m_schedule.controlCopy();
        return schedule ? schedule->notes : nullptr;
    }
    /// Deterministic performance-test hook: indexed subtrees inspected by the
    /// most recent discontinuity chase, including branches rejected at root.
    std::size_t lastChaseSubtreesVisitedForTest() const noexcept {
        return m_lastChaseVisits.load(std::memory_order_relaxed);
    }

    /// Control thread: sound a note *now*, outside the timeline — a key on the
    /// piano roll's keyboard, the typing keyboard, later a MIDI controller.
    ///
    /// The event is queued rather than pushed straight at the synth: only the
    /// audio thread may touch a MIDI buffer, and it is the one that decides
    /// which block the note lands in. A single-producer queue is enough because
    /// every caller is the UI thread. Full means the audio thread has stopped
    /// draining (no device). Ordinary events are dropped in that case; releases
    /// fall back to a coalescing atomic mailbox so an accepted live note can never
    /// be left sounding merely because the ring filled. Returns false only when
    /// an ordinary event was dropped.
    bool sendLiveEvent(const MidiEvent& event) noexcept {
        const std::uint32_t write = m_liveWrite.load(std::memory_order_relaxed);
        const std::uint32_t next = (write + 1) % kLiveQueue;
        if (next == m_liveRead.load(std::memory_order_acquire)) {
            if (event.isNoteOff()) {
                queueEmergencyRelease(event.channel(), event.data1);
                return true;
            }
            return false;
        }
        m_liveEvents[write] = event;
        m_liveWrite.store(next, std::memory_order_release);
        return true;
    }

    bool sendLiveNoteOn(std::uint8_t key, std::uint8_t velocity,
                        std::uint8_t channel = 0) noexcept {
        return sendLiveEvent(MidiEvent::noteOn(0, channel, key, velocity));
    }
    bool sendLiveNoteOff(std::uint8_t key, std::uint8_t channel = 0) noexcept {
        return sendLiveEvent(MidiEvent::noteOff(0, channel, key));
    }

    void prepare(const PrepareInfo&) override {
        // Reserved here so `process` never grows it: 128 simultaneous notes is
        // past any keyboard and any chord, and refusing beyond that is the only
        // realtime-safe answer.
        m_sounding.reserve(kMaxSounding);
        m_sounding.clear();
        m_noteCursor = 0;
        m_hasPosition = false;
    }

    void reset() override {
        m_sounding.clear();
        m_noteCursor = 0;
        m_hasPosition = false;
    }

    void process(const ProcessContext& context) override {
        // A MIDI source writes no audio, but the buffer it was handed is
        // recycled and still holds the previous owner's signal.
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            dsp::clear(context.output.channel(ch));
        }
        if (!context.midiOutput || context.frames == 0) return;

        // Live keys first, and whatever the transport is doing: playing a note
        // from the keyboard has nothing to do with the playhead, and this is
        // the only path that sounds at all while stopped.
        drainLiveEvents(*context.midiOutput);

        const double tempo = context.transport.tempo > 0.0 ? context.transport.tempo : 120.0;
        const double samplesPerBeat = context.sampleRate * 60.0 / tempo;
        if (!(samplesPerBeat > 0.0)) return;

        if (!context.playing) {
            // Stopped mid-note: release whatever was sounding, or the synth
            // holds it forever. This is why the node tracks what it started.
            // Live notes are not in that list — they end when the key is let
            // go, not when the transport stops.
            releaseAll(*context.midiOutput, 0);
            // The next playing block is a fresh transport start even when the
            // playhead has not moved. It must rebuild/chase active notes instead
            // of continuing with the cursor left behind by the previous run.
            m_hasPosition = false;
            context.midiOutput->sort();
            return;
        }

        const double blockStartBeats = context.transport.ppqPosition;
        const double blockEndBeats =
            blockStartBeats + double(context.frames) / samplesPerBeat;

        auto schedule = m_schedule.read();
        if (!schedule || !schedule->notes) return;
        const NoteList& notes = *schedule->notes;

        const bool snapshotChanged =
            m_scheduleRevisionFor != schedule->revision;
        const bool jumped =
            m_hasPosition && std::abs(blockStartBeats - m_expectedBeats) > 1e-6;
        // A locate, loop wrap or edited note set invalidates both the active
        // voices and the monotonic start cursor.
        if (snapshotChanged || jumped || !m_hasPosition) {
            releaseAll(*context.midiOutput, 0);
            m_noteCursor = std::size_t(std::lower_bound(
                notes.begin(), notes.end(), blockStartBeats,
                [](const MidiNote& note, double beat) {
                    return note.startBeats < beat;
                }) - notes.begin());
            m_scheduleRevisionFor = schedule->revision;

            // MIDI note chase: starting or locating into the held portion of a
            // note must sound immediately. Rebuild only on a discontinuity; the
            // normal per-block path remains cursor-based and O(polyphony).
            std::size_t chaseVisits = 0;
            chaseActiveNotes(*schedule, 0, notes.size(), m_noteCursor,
                             blockStartBeats, blockEndBeats, samplesPerBeat,
                             context.frames, *context.midiOutput, chaseVisits);
            m_lastChaseVisits.store(chaseVisits, std::memory_order_relaxed);
        }
        m_expectedBeats = blockEndBeats;
        m_hasPosition = true;

        // End only voices that were active before this block. Note-offs are
        // therefore O(polyphony), not a scan of every note in the project.
        for (std::size_t i = 0; i < m_sounding.size();) {
            const Sounding sounding = m_sounding[i];
            if (sounding.endBeats < blockEndBeats) {
                const auto offset = sounding.endBeats <= blockStartBeats
                                        ? FrameCount(0)
                                        : FrameCount((sounding.endBeats -
                                                      blockStartBeats) *
                                                     samplesPerBeat);
                if (context.midiOutput->push(MidiEvent::noteOff(
                        std::min(offset, context.frames - 1), sounding.channel,
                        sounding.key))) {
                    m_sounding[i] = m_sounding.back();
                    m_sounding.pop_back();
                } else {
                    // An all-note-off overflow is extraordinarily rare, but
                    // forgetting the voice here would make the loss permanent.
                    // Keep it and retry at offset zero next block.
                    ++i;
                }
            } else {
                ++i;
            }
        }

        // Notes are sorted by start. The cursor advances once over the entire
        // arrangement during normal playback and is repositioned by binary
        // search only after a seek/edit.
        while (m_noteCursor < notes.size() &&
               notes[m_noteCursor].startBeats < blockEndBeats) {
            const MidiNote& note = notes[m_noteCursor++];
            if (note.startBeats < blockStartBeats) continue;
            const double endBeats = note.startBeats + note.lengthBeats;
            const auto onOffset = FrameCount(
                (note.startBeats - blockStartBeats) * samplesPerBeat);
            const bool endsThisBlock = endBeats < blockEndBeats;
            if (!endsThisBlock && m_sounding.size() >= kMaxSounding) continue;

            const bool started = context.midiOutput->push(MidiEvent::noteOn(
                std::min(onOffset, context.frames - 1), note.channel, note.key,
                note.velocity));
            if (!started) continue;
            if (endsThisBlock) {
                const auto offOffset =
                    FrameCount((endBeats - blockStartBeats) * samplesPerBeat);
                context.midiOutput->push(MidiEvent::noteOff(
                    std::min(offOffset, context.frames - 1), note.channel, note.key));
            } else {
                m_sounding.push_back(Sounding{note.key, note.channel, endBeats});
            }
        }
        context.midiOutput->sort();
    }

private:
    static constexpr std::size_t kMaxSounding = 128;

    struct Sounding {
        std::uint8_t key;
        std::uint8_t channel;
        double endBeats;
    };

    void chaseActiveNotes(const NoteSchedule& schedule, std::size_t first,
                          std::size_t last, std::size_t startLimit,
                          double blockStartBeats, double blockEndBeats,
                          double samplesPerBeat, FrameCount frames,
                          MidiBuffer& out, std::size_t& visited) {
        if (first >= last || first >= startLimit) return;
        ++visited;
        const std::size_t mid = first + (last - first) / 2;
        if (!(schedule.subtreeMaxEnd[mid] > blockStartBeats)) return;

        // In-order traversal preserves the previous start-time event order.
        chaseActiveNotes(schedule, first, mid, startLimit, blockStartBeats,
                         blockEndBeats, samplesPerBeat, frames, out, visited);
        if (mid < startLimit) {
            const MidiNote& note = (*schedule.notes)[mid];
            const double endBeats = note.startBeats + note.lengthBeats;
            if (endBeats > blockStartBeats) {
                const bool endsThisBlock = endBeats < blockEndBeats;
                if (endsThisBlock || m_sounding.size() < kMaxSounding) {
                    const bool started = out.push(MidiEvent::noteOn(
                        0, note.channel, note.key, note.velocity));
                    // Do not remember or release a voice the destination never
                    // received.
                    if (started) {
                        if (endsThisBlock) {
                            const auto offOffset = FrameCount(
                                (endBeats - blockStartBeats) * samplesPerBeat);
                            out.push(MidiEvent::noteOff(
                                std::min(offOffset, frames - 1), note.channel,
                                note.key));
                        } else {
                            m_sounding.push_back(
                                Sounding{note.key, note.channel, endBeats});
                        }
                    }
                }
            }
        }
        if (mid + 1 < startLimit) {
            chaseActiveNotes(schedule, mid + 1, last, startLimit,
                             blockStartBeats, blockEndBeats, samplesPerBeat,
                             frames, out, visited);
        }
    }

    /// Audio thread: move everything the UI queued into this block, at offset 0.
    /// The queue holds a handful of key presses, so it empties in one pass.
    void drainLiveEvents(MidiBuffer& out) noexcept {
        std::uint32_t read = m_liveRead.load(std::memory_order_relaxed);
        const std::uint32_t write = m_liveWrite.load(std::memory_order_acquire);
        while (read != write) {
            const MidiEvent event = m_liveEvents[read];
            if (!out.push(event) && event.isNoteOff()) {
                queueEmergencyRelease(event.channel(), event.data1);
            }
            read = (read + 1) % kLiveQueue;
        }
        m_liveRead.store(read, std::memory_order_release);

        // Drain releases after the FIFO so an on already queued at the same
        // frame remains before its off. `exchange` gives this thread one stable
        // batch; a concurrent producer either lands in that batch or remains set
        // for the next block. If the block itself contains only releases and is
        // full, put the unconsumed bits back instead of forgetting them.
        for (std::size_t wordIndex = 0; wordIndex < kLiveReleaseWords; ++wordIndex) {
            std::uint64_t releases =
                m_emergencyLiveReleases[wordIndex].exchange(
                    0, std::memory_order_acq_rel);
            while (releases != 0) {
                const unsigned bit = std::countr_zero(releases);
                const std::uint64_t mask = std::uint64_t{1} << bit;
                releases &= ~mask;
                const std::size_t identity = wordIndex * 64 + bit;
                const auto channel = std::uint8_t(identity / 128);
                const auto key = std::uint8_t(identity % 128);
                if (!out.push(MidiEvent::noteOff(0, channel, key))) {
                    m_emergencyLiveReleases[wordIndex].fetch_or(
                        releases | mask, std::memory_order_release);
                    break;
                }
            }
        }
    }

    void queueEmergencyRelease(std::uint8_t channel, std::uint8_t key) noexcept {
        const std::size_t identity = std::size_t(channel & 0x0F) * 128 +
                                     std::size_t(key & 0x7F);
        m_emergencyLiveReleases[identity / 64].fetch_or(
            std::uint64_t{1} << (identity % 64), std::memory_order_release);
    }

    void releaseAll(MidiBuffer& out, FrameCount offset) noexcept {
        std::size_t keep = 0;
        for (Sounding note : m_sounding) {
            if (out.push(MidiEvent::noteOff(offset, note.channel, note.key)))
                continue;
            // A block already filled entirely with releases cannot sacrifice
            // one of them for this one. Keep the voice and make it immediately
            // overdue so a playing block retries too; clearing it here would
            // turn a transient overflow during stop/seek into a stuck note.
            note.endBeats = -std::numeric_limits<double>::infinity();
            m_sounding[keep++] = note;
        }
        m_sounding.resize(keep);
    }

    std::string m_name;
    RealtimeSnapshot<NoteSchedule> m_schedule;
    /// What this node has started and not yet ended. A vector, not a set: it
    /// holds a handful of entries and `process` must not allocate, so the
    /// capacity is reserved once and the search is a scan of a few elements.
    std::vector<Sounding> m_sounding;
    std::size_t m_noteCursor = 0;
    std::uint64_t m_scheduleRevision = 0;
    std::uint64_t m_scheduleRevisionFor =
        std::numeric_limits<std::uint64_t>::max();
    double m_expectedBeats = 0.0;
    bool m_hasPosition = false;

    /// The live-key queue: fixed storage, one producer (the UI), one consumer
    /// (the audio thread). One slot is always left empty so a full queue is
    /// distinguishable from an empty one without a third counter.
    static constexpr std::uint32_t kLiveQueue = 256;
    static constexpr std::size_t kLiveReleaseWords = (16 * 128) / 64;
    std::array<MidiEvent, kLiveQueue> m_liveEvents{};
    std::atomic<std::uint32_t> m_liveWrite{0};
    std::atomic<std::uint32_t> m_liveRead{0};
    std::array<std::atomic<std::uint64_t>, kLiveReleaseWords>
        m_emergencyLiveReleases{};
    std::atomic<std::size_t> m_lastChaseVisits{0};
};

} // namespace daw::engine
