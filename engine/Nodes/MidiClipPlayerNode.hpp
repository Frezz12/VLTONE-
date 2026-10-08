#pragma once

#include "DSP/Simd.hpp"
#include "DSP/Curve.hpp"
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
    float pan = 0.0f;             ///< per-note stereo position, -1 ... 1
    std::uint8_t releaseVelocity = 0;
    std::uint64_t startOrder = 0, endOrder = 0;
    std::int32_t noteId = -1;
    std::vector<curve::Point> pitch;
};

/// Plays MIDI clips: turns a list of notes into note-on and note-off events at
/// the right sample offsets.
///
/// The note list is published as an immutable snapshot, like `ClipPlayerNode`'s
/// clip list, so editing in the piano roll never locks the audio thread.
class MidiClipPlayerNode : public Node {
public:
    using NoteList = std::vector<MidiNote>;
    struct ControlPoint {
        double beats = 0, value = 0;
        curve::Shape shape = curve::Shape::Linear;
        double curve = 0;
        std::uint64_t order = 0;
    };
    struct ControlCurve {
        double startBeats = 0, endBeats = 0, defaultValue = 0;
        int cc = 1, channel = 0, key = 0;
        std::vector<ControlPoint> points;
    };
    using ControlCurves = std::vector<ControlCurve>;
    void setControllers(std::shared_ptr<const ControlCurves> curves) { m_controllers.publish(std::move(curves)); }

private:
    /// Control-thread-prepared interval index. `subtreeMaxEnd[mid]` is the
    /// latest note end in the balanced subtree whose root is `mid`. It lets a
    /// seek chase prune whole runs of expired notes without allocating or
    /// locking in the audio callback.
    struct NoteSchedule {
        std::shared_ptr<const NoteList> notes;
        std::vector<double> subtreeMaxEnd;
        std::uint64_t revision = 0;
        std::vector<std::pair<std::int32_t, std::size_t>> identities;
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
        for (std::size_t i = 0; i < schedule->notes->size(); ++i)
            if ((*schedule->notes)[i].noteId >= 0) schedule->identities.emplace_back((*schedule->notes)[i].noteId, i);
        std::sort(schedule->identities.begin(), schedule->identities.end());
        return schedule;
    }

public:
    explicit MidiClipPlayerNode(std::string name = "MIDI Clips")
        : m_name(std::move(name)),
          m_schedule(makeSchedule(std::make_shared<const NoteList>(), 0)) {}

    std::string_view name() const noexcept override { return m_name; }
    OfflineNodePolicy offlineNodePolicy() const noexcept override { return OfflineNodePolicy::Ordered; }
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
    /// Control thread: a replacement take monitors live input while the old
    /// performance stays saved. Only the audio thread releases timeline voices.
    void setTimelineSuppressed(bool suppressed) noexcept {
        m_timelineSuppressed.store(suppressed, std::memory_order_relaxed);
    }
    struct ControlState {
        std::shared_ptr<const NoteSchedule> notes;
        std::shared_ptr<const ControlCurves> controllers;
        bool timelineSuppressed;
    };
    ControlState controlState() const {
        return {m_schedule.controlCopy(), m_controllers.controlCopy(), m_timelineSuppressed.load(std::memory_order_relaxed)};
    }
    void restoreControlState(const ControlState& state) {
        m_schedule.publish(state.notes); m_controllers.publish(state.controllers);
        setTimelineSuppressed(state.timelineSuppressed);
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

        if (m_timelineSuppressed.load(std::memory_order_relaxed)) {
            // Release the previous performance before live input, so punching
            // in on the same key cannot cut off the new note. Never panic the
            // whole instrument: live notes and controllers belong to the player.
            releaseControllers(*context.midiOutput);
            releaseAll(*context.midiOutput, 0);
            m_hasPosition = false;
            drainLiveEvents(*context.midiOutput);
            context.midiOutput->sort();
            return;
        }

        // Live keys first, and whatever the transport is doing: playing a note
        // from the keyboard has nothing to do with the playhead, and this is
        // the only path that sounds at all while stopped.
        drainLiveEvents(*context.midiOutput);

        const double tempo = context.transport.tempo > 0.0 ? context.transport.tempo : 120.0;
        const double samplesPerBeat = context.sampleRate * 60.0 / tempo;
        if (!(samplesPerBeat > 0.0)) return;

        if (!context.playing) {
            releaseControllers(*context.midiOutput);
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
        playControllers(context, blockStartBeats, blockEndBeats, samplesPerBeat);

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
            if (jumped || !m_hasPosition) releaseAll(*context.midiOutput, 0);
            else reconcile(*schedule, blockStartBeats, *context.midiOutput);
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
                if (sounding.noteIndex < notes.size()) emitPitch(notes[sounding.noteIndex], blockStartBeats, blockEndBeats, samplesPerBeat, context.frames, *context.midiOutput,snapshotChanged);
                const auto offset = sounding.endBeats <= blockStartBeats
                                        ? FrameCount(0)
                                        : FrameCount((sounding.endBeats -
                                                      blockStartBeats) *
                                                     samplesPerBeat);
                if (pushOrdered(*context.midiOutput, sounding.endOrder, MidiEvent::noteOff(
                        std::min(offset, context.frames - 1), sounding.channel,
                        sounding.key, sounding.releaseVelocity, sounding.noteId))) {
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

            const bool started = pushOrdered(*context.midiOutput, note.startOrder, MidiEvent::noteOn(
                std::min(onOffset, context.frames - 1), note.channel, note.key,
                note.velocity, note.pan, note.noteId));
            if (!started) continue;
            if (endsThisBlock) {
                const auto offOffset =
                    FrameCount((endBeats - blockStartBeats) * samplesPerBeat);
                pushOrdered(*context.midiOutput, note.endOrder, MidiEvent::noteOff(
                    std::min(offOffset, context.frames - 1), note.channel, note.key, note.releaseVelocity, note.noteId));
                emitPitch(note, blockStartBeats, blockEndBeats, samplesPerBeat, context.frames, *context.midiOutput,snapshotChanged);
            } else {
                m_sounding.push_back(Sounding{note.key, note.channel, endBeats, note.releaseVelocity, note.endOrder, note.noteId, m_noteCursor - 1, !note.pitch.empty()});
            }
        }
        for (const auto& voice : m_sounding) if (voice.noteIndex < notes.size()) emitPitch(notes[voice.noteIndex], blockStartBeats, blockEndBeats, samplesPerBeat, context.frames, *context.midiOutput,snapshotChanged);
        context.midiOutput->sort();
    }

private:
    static bool pushOrdered(MidiBuffer& out, std::uint64_t order, MidiEvent event) noexcept {
        event.musicalOrder = order;
        return out.push(event);
    }
    static constexpr std::size_t kMaxSounding = 128;

    struct Sounding {
        std::uint8_t key;
        std::uint8_t channel;
        double endBeats;
        std::uint8_t releaseVelocity = 0;
        std::uint64_t endOrder = 0;
        std::int32_t noteId = -1;
        std::size_t noteIndex = 0;
        bool hadPitch = false;
    };

    void reconcile(const NoteSchedule& schedule, double beat, MidiBuffer& out) {
        for (std::size_t i = 0; i < m_sounding.size();) {
            auto& voice = m_sounding[i];
            const auto it = std::lower_bound(schedule.identities.begin(), schedule.identities.end(),
                std::pair{voice.noteId, std::size_t(0)});
            const MidiNote* n = it != schedule.identities.end() && it->first == voice.noteId ? &(*schedule.notes)[it->second] : nullptr;
            if (!n || voice.noteId < 0 || n->key != voice.key || n->channel != voice.channel ||
                n->startBeats > beat || n->startBeats+n->lengthBeats <= beat) {
                if (!out.push(MidiEvent::noteOff(0, voice.channel, voice.key, voice.releaseVelocity, voice.noteId))) { voice.noteIndex = schedule.notes->size(); voice.endBeats = beat; ++i; continue; }
                m_sounding[i] = m_sounding.back(); m_sounding.pop_back(); continue;
            }
            voice.noteIndex = it->second; voice.endBeats = n->startBeats+n->lengthBeats;
            voice.releaseVelocity = n->releaseVelocity; voice.endOrder = n->endOrder;
            if (voice.hadPitch && n->pitch.empty()) {
                MidiEvent reset; reset.noteId = n->noteId; reset.status = n->channel;
                reset.data1 = n->key; reset.isPitchExpression = true;reset.pitch.edited=true;
                out.push(reset);
            }
            voice.hadPitch = !n->pitch.empty();
            ++i;
        }
    }

    static void emitPitch(const MidiNote& note, double begin, double end, double spb,
                          FrameCount frames, MidiBuffer& out, bool edited = false) {
        if(note.pitch.empty())return;
        const double start=std::max(begin,note.startBeats),stop=std::min(end,note.startBeats+note.lengthBeats);
        if(!(stop>start)||!frames)return;
        FrameCount frame=FrameCount(std::clamp((start-begin)*spb,0.,double(frames-1)));
        const FrameCount limit=stop==end?frames:FrameCount(std::clamp((stop-begin)*spb,0.,double(frames)));
        bool first=true;
        while(frame<limit){
            const double at=begin+frame/spb;
            auto next=std::upper_bound(note.pitch.begin(),note.pitch.end(),at,[](double t,const auto& p){return t<p.beats;});
            FrameCount until=limit;
            if(next!=note.pitch.end())until=FrameCount(std::clamp(std::ceil((next->beats-begin)*spb-1e-8),double(frame+1),double(limit)));
            MidiEvent e;e.status=note.channel;e.data1=note.key;e.noteId=note.noteId;e.isPitchExpression=true;e.musicalOrder=note.startOrder;e.frameOffset=frame;e.pitch.frames=until-frame;e.pitch.edited=edited&&first;
            if(next!=note.pitch.begin()){
                const auto& prev=*std::prev(next);e.pitch.active=true;e.pitch.from=prev.value;e.pitch.to=next==note.pitch.end()?prev.value:next->value;
                e.pitch.segmentStart=prev.beats;e.pitch.shape=prev.shape;e.pitch.tension=prev.curve;e.pitch.priority=prev.priority;
                e.pitch.shapeFrom=curve::shapeT(prev.phaseFrom,prev.shape,prev.curve);e.pitch.shapeTo=curve::shapeT(prev.phaseTo,prev.shape,prev.curve);
                if(next!=note.pitch.end()){
                    const double scale=(prev.phaseTo-prev.phaseFrom)/(next->beats-prev.beats);
                    e.pitch.phaseFrom=prev.phaseFrom+(at-prev.beats)*scale;
                    e.pitch.phaseTo=prev.phaseFrom+(begin+until/spb-prev.beats)*scale;
                }
            }
            out.push(e);frame=until;first=false;
        }
    }

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
            if (endBeats > blockStartBeats && !std::any_of(m_sounding.begin(), m_sounding.end(), [&](const Sounding& s) { return note.noteId >= 0 && s.noteId == note.noteId; })) {
                const bool endsThisBlock = endBeats < blockEndBeats;
                if (endsThisBlock || m_sounding.size() < kMaxSounding) {
                    const bool started = out.push(MidiEvent::noteOn(
                        0, note.channel, note.key, note.velocity, note.pan, note.noteId));
                    // Do not remember or release a voice the destination never
                    // received.
                    if (started) {
                        if (endsThisBlock) {
                            const auto offOffset = FrameCount(
                                (endBeats - blockStartBeats) * samplesPerBeat);
                            out.push(MidiEvent::noteOff(
                                std::min(offOffset, frames - 1), note.channel,
                                note.key, note.releaseVelocity, note.noteId));
                            emitPitch(note, blockStartBeats, blockEndBeats, samplesPerBeat, frames, out);
                        } else {
                            m_sounding.push_back(
                                Sounding{note.key, note.channel, endBeats, note.releaseVelocity, note.endOrder, note.noteId, mid, !note.pitch.empty()});
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
                auto release = MidiEvent::noteOff(0, channel, key);
                // This is the overflow fallback: also terminate one-shots
                // whose ordinary note-off would leave them sounding.
                release.isNoteChoke = true;
                if (!out.push(release)) {
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
            if (out.push(MidiEvent::noteOff(offset, note.channel, note.key, 0, note.noteId)))
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
    RealtimeSnapshot<ControlCurves> m_controllers;
    const ControlCurves* m_controllerSnapshot = nullptr;
    std::array<int, 8192> m_controllerValues{};
    std::array<bool, 16> m_sustainChannels{};
    std::array<bool, 16> m_bendChannels{};

    void releaseControllers(MidiBuffer& out) noexcept {
        for (int ch = 0; ch < 16; ++ch) {
            if (m_sustainChannels[ch] && out.push({0, std::uint8_t(0xb0 | ch), 64, 0}))
                m_sustainChannels[ch] = false;
            if (m_bendChannels[ch] && out.push({0, std::uint8_t(0xe0 | ch), 0, 64}))
                m_bendChannels[ch] = false;
        }
        m_controllerSnapshot = nullptr;
    }

    void playControllers(const ProcessContext& context, double begin, double end, double samplesPerBeat) {
        auto curves = m_controllers.read();
        const bool discontinuity = !m_hasPosition || std::abs(begin - m_expectedBeats) > 1e-6 ||
                                   m_controllerSnapshot != curves.get();
        if (discontinuity) { releaseControllers(*context.midiOutput); m_controllerValues.fill(-1); }
        m_controllerSnapshot = curves.get();
        if (!curves) return;
        for (std::size_t i = 0; i < std::min(curves->size(), m_controllerValues.size()); ++i) {
            const auto& c = (*curves)[i];
            if (c.channel < 0 || c.channel > 15 || c.points.empty()) continue;
            const auto send = [&](double beat, double value, std::uint64_t order, bool force) {
                const int v = int(std::lround(std::clamp(value, 0.0, 1.0) * (c.cc == -2 ? 16383 : 127)));
                if (!force && m_controllerValues[i] == v) return;
                MidiEvent event;
                event.frameOffset = FrameCount(std::clamp((beat - begin) * samplesPerBeat, 0.0, double(context.frames - 1)));
                event.musicalOrder = order;
                if (c.cc >= 0) { event.status = std::uint8_t(0xb0 | c.channel); event.data1 = std::uint8_t(c.cc); event.data2 = std::uint8_t(v); }
                else if (c.cc == -2) { event.status = std::uint8_t(0xe0 | c.channel); event.data1 = v & 127; event.data2 = (v >> 7) & 127; }
                else if (c.cc == -3) { event.status = std::uint8_t(0xd0 | c.channel); event.data1 = std::uint8_t(v); }
                else if (c.cc == -4) { event.status = std::uint8_t(0xa0 | c.channel); event.data1 = std::uint8_t(c.key); event.data2 = std::uint8_t(v); }
                else if (c.cc == -5) { event.status = std::uint8_t(0xc0 | c.channel); event.data1 = std::uint8_t(v); }
                else return;
                if (context.midiOutput->push(event)) {
                    m_controllerValues[i] = v;
                    if (c.cc == 64) m_sustainChannels[c.channel] = v >= 64;
                    if (c.cc == -2) m_bendChannels[c.channel] = v != 8192;
                }
            };
            if (begin >= c.endBeats && m_controllerValues[i] >= 0) {
                if (c.cc == 64) send(begin, 0, 0, true);
                if (c.cc == -2) send(begin, 8192.0 / 16383.0, 0, true);
                m_controllerValues[i] = -1;
            }
            if (end <= c.startBeats || begin >= c.endBeats) continue;
            const double at = std::max(begin, c.startBeats);
            auto next = std::lower_bound(c.points.begin(), c.points.end(), at,
                [](const auto& p, double beat) { return p.beats < beat; });
            if (next == c.points.end() || next->beats > at) {
                if (next != c.points.begin()) {
                    const auto& previous = *std::prev(next);
                    double value = previous.value;
                    if (next != c.points.end() && next->beats > previous.beats)
                        value = previous.value + (next->value - previous.value) * curve::shapeT(
                            (at - previous.beats) / (next->beats - previous.beats), previous.shape, previous.curve);
                    send(at, value, 0, discontinuity);
                } else if (!c.points.front().order) send(at, c.defaultValue, 0, discontinuity);
            }
            while (next != c.points.end() && next->beats < std::min(end, c.endBeats)) {
                send(next->beats, next->value, next->order, true); ++next;
            }
            if (c.endBeats < end) {
                if (c.cc == 64) send(c.endBeats, 0, 0, true);
                if (c.cc == -2) send(c.endBeats, 8192.0 / 16383.0, 0, true);
                m_controllerValues[i] = -1;
            }
        }
    }
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
    std::atomic<bool> m_timelineSuppressed{false};

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
