#include "EngineController.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace daw {
namespace {
template <class Queue, class Event> void appendTimedMidi(Queue &queue, Event event) {
    if (queue.empty() || queue.back().stamp.timeNs <= event.stamp.timeNs) {
        queue.push_back(std::move(event));
        return;
    }
    // Hardware can arrive through Qt after a newer computer-keyboard gesture.
    const auto position = std::upper_bound(
        queue.begin(), queue.end(), event.stamp.timeNs,
        [](std::uint64_t time, const auto &item) { return time < item.stamp.timeNs; });
    queue.insert(position, std::move(event));
}
} // namespace

MidiInputStamp EngineController::midiInputStamp() const noexcept {
    const auto ns = std::uint64_t(engine::presentationNowNs());
    return {ns, m_runtime.inputBeatsAt(ns), m_runtime.transportSnapshot().tempo};
}

void EngineController::resetMidiInput() {
    m_liveMidiControllers.clear();
    m_retrospectivePendingThrough = 0;
    m_retrospectiveMidi.clear();
    m_retrospectiveState.clear();
    m_heldMidiInput.clear();
    m_midiLearnBindings.clear();
    m_loadedMidiBindings.clear();
    cancelMidiLearn();
    m_midiCaptureError.clear();
}

void EngineController::ensureMidiBindings(const std::string &track) {
    if (!m_loadedMidiBindings.insert(track).second || !loadLocalMidiBindings)
        return;
    auto bindings = loadLocalMidiBindings(track);
    for (auto &b : bindings)
        if (b.trackId == track && b.channel >= 0 && b.channel < 16 && b.cc >= 0 && b.cc < 128)
            m_midiLearnBindings.push_back(std::move(b));
}
void EngineController::persistMidiBindings(const std::string &track) {
    if (!saveLocalMidiBindings)
        return;
    std::vector<MidiLearnBinding> bindings;
    for (const auto &b : m_midiLearnBindings)
        if (b.trackId == track)
            bindings.push_back(b);
    saveLocalMidiBindings(track, bindings);
}

void EngineController::beginMidiLearn(const std::string &trackId, const std::string &parameterId) {
    const auto *track = m_project.findTrack(trackId);
    if (!track || !trackAccepts(track->kind, ClipKind::Midi))
        return;
    const auto params = insertParameters(trackId, track->instrument.id);
    if (std::none_of(params.begin(), params.end(),
                     [&](const auto &p) { return p.id == parameterId && p.isAutomatable; }))
        return;
    ensureMidiBindings(trackId);
    m_midiLearnTrack = trackId;
    m_midiLearnParameter = parameterId;
}

void EngineController::cancelMidiLearn() {
    m_midiLearnTrack.clear();
    m_midiLearnParameter.clear();
}

void EngineController::removeMidiLearn(const std::string &track, const std::string &parameter) {
    ensureMidiBindings(track);
    std::erase_if(m_midiLearnBindings,
                  [&](const auto &b) { return b.trackId == track && b.parameterId == parameter; });
    persistMidiBindings(track);
}

bool EngineController::liveMidiInput(const std::string &trackId, int status, int d1, int d2,
                                     std::uint64_t source, MidiInputStamp stamp,
                                     LiveMidiOrigin origin) {
    if (status < 0x80 || status > 0xef || d1 < 0 || d1 > 127 || d2 < 0 || d2 > 127 ||
        !std::isfinite(stamp.transportBeats) || !std::isfinite(stamp.tempo) || stamp.tempo <= 0)
        return false;
    if (!stamp.timeNs)
        stamp = midiInputStamp();
    if (origin == LiveMidiOrigin::Audition)
        return sendLiveMidiEvent(trackId, status, d1, d2, /*audition=*/true);

    std::vector<std::string> targets;
    if (origin != LiveMidiOrigin::Audition) {
        for (const auto &c : m_captures)
            if (c.midi)
                targets.push_back(c.trackId);
    }
    if (targets.empty())
        targets.push_back(trackId);
    bool accepted = false;
    for (const auto &target : targets) {
        const auto *track = m_project.findTrack(target);
        if (!track || !trackAccepts(track->kind, ClipKind::Midi))
            continue;
        if ((status & 0xf0) == 0xb0 && origin == LiveMidiOrigin::Performance) {
            ensureMidiBindings(target);
            if (m_midiLearnTrack == target && !m_midiLearnParameter.empty()) {
                const auto parameters = insertParameters(target, track->instrument.id);
                const auto parameter =
                    std::find_if(parameters.begin(), parameters.end(), [&](const auto &p) {
                        return p.id == m_midiLearnParameter && p.isAutomatable;
                    });
                if (parameter != parameters.end()) {
                    removeMidiLearn(target, parameter->id);
                    std::erase_if(m_midiLearnBindings, [&](const auto &b) {
                        return b.trackId == target && b.channel == (status & 15) && b.cc == d1;
                    });
                    m_midiLearnBindings.push_back({target, track->instrument.uid, parameter->id,
                                                   parameter->name, status & 15, d1});
                    persistMidiBindings(target);
                }
                cancelMidiLearn();
            }
            const auto binding = std::find_if(
                m_midiLearnBindings.begin(), m_midiLearnBindings.end(), [&](const auto &b) {
                    return b.trackId == target && b.instrumentUid == track->instrument.uid &&
                           b.channel == (status & 15) && b.cc == d1;
                });
            if (binding != m_midiLearnBindings.end()) {
                const auto parameters = insertParameters(target, track->instrument.id);
                const auto parameter =
                    std::find_if(parameters.begin(), parameters.end(), [&](const auto &p) {
                        return p.id == binding->parameterId && p.isAutomatable;
                    });
                if (parameter != parameters.end()) {
                    double value = parameter->minValue +
                                   double(d2) / 127.0 * (parameter->maxValue - parameter->minValue);
                    if (parameter->isStepped)
                        value = std::round(value);
                    captureMidiParameter(target, track->instrument.id, parameter->id, value, stamp,
                                         true);
                    m_applyingMidiLearn = true;
                    setInsertParameter(target, track->instrument.id, parameter->id, value);
                    m_applyingMidiLearn = false;
                    if (!isRecording())
                        writeAutomationPoint(target, track->instrument.id, parameter->id, value);
                    accepted = true;
                    continue;
                }
            }
        }
        sendLiveMidiEvent(target, status, d1, d2);
        captureMidiEvent(target, status, d1, d2, source, stamp, origin);
        accepted = true;
    }
    return accepted;
}

void EngineController::captureMidiEvent(const std::string &track, int status, int d1, int d2,
                                        std::uint64_t source, MidiInputStamp stamp,
                                        LiveMidiOrigin origin) {
    const int type = status & 0xf0, channel = status & 15;
    const bool on = type == 0x90 && d2 > 0;
    const bool off = type == 0x80 || (type == 0x90 && !d2);
    auto held = std::find_if(m_heldMidiInput.begin(), m_heldMidiInput.end(), [&](const auto &h) {
        return h.trackId == track && h.source == source && h.channel == channel && h.pitch == d1;
    });
    const bool closesHeld = off && held != m_heldMidiInput.end();
    if (off && held != m_heldMidiInput.end())
        m_heldMidiInput.erase(held);
    if (on) {
        if (held != m_heldMidiInput.end())
            m_heldMidiInput.erase(held);
        m_heldMidiInput.push_back({track, source, channel, d1, d2});
    }
    if (type == 0xb0 && (d1 == 120 || d1 == 123))
        std::erase_if(m_heldMidiInput, [&](const auto &h) {
            return h.trackId == track && h.source == source && h.channel == channel;
        });
    const auto controlKey = track + ":" + std::to_string(status) + ":" +
                            std::to_string(type == 0xb0 || type == 0xa0 ? d1 : 0);
    bool resetChangedController = false;
    if (type == 0xb0 && d1 == 121) {
        std::erase_if(m_liveMidiControllers, [&](const auto &item) {
            const auto &e = item.second;
            const int kind = e.status & 0xf0;
            if (e.trackId != track || !e.parameterId.empty() || (e.status & 15) != channel ||
                kind == 0xc0 || (kind == 0xb0 && (e.d1 == 0 || e.d1 == 32)))
                return false;
            resetChangedController |= kind == 0xe0 ? (e.d1 | (e.d2 << 7)) != 8192
                                                   : (kind == 0xd0 ? e.d1 != 0 : e.d2 != 0);
            return true;
        });
    }
    if (origin == LiveMidiOrigin::Cleanup && !closesHeld && !resetChangedController) {
        const auto previous = m_liveMidiControllers.find(controlKey);
        if (on || off || previous == m_liveMidiControllers.end() ||
            (previous->second.d1 == d1 && previous->second.d2 == d2))
            return;
    }
    const auto order = ++m_midiEventOrder;
    RetrospectiveMidiEvent event{track, {}, {}, {}, source, order, stamp, status, d1, d2};
    if (!on && !off) {
        auto &previous = m_liveMidiControllers[controlKey];
        if (previous.stamp.timeNs <= stamp.timeNs)
            previous = event;
    }
    appendTimedMidi(m_retrospectiveMidi, std::move(event));
    pruneRetrospectiveMidi(stamp.timeNs);
    for (auto &capture : m_captures) {
        if (!capture.midi || capture.trackId != track || stamp.timeNs < capture.midiStart.timeNs)
            continue;
        const double beat = std::max(0.0, stamp.transportBeats - capture.midiStart.transportBeats);
        capture.midiLastBeat = std::max(capture.midiLastBeat, beat);
        capture.midiInputReceived = true;
        capture.midiRecording.event(source, status, d1, d2, beat, order);
    }
}

void EngineController::captureMidiParameter(const std::string &trackId, const std::string &slot,
                                            const std::string &parameter, double plain,
                                            MidiInputStamp stamp, bool retrospective) {
    const auto *track = m_project.findTrack(trackId);
    if (!track || track->instrument.id != slot || !std::isfinite(plain))
        return;
    const auto parameters = insertParameters(trackId, slot);
    const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const auto &p) {
        return p.id == parameter && p.isAutomatable;
    });
    if (found == parameters.end())
        return;
    const double span = found->maxValue - found->minValue;
    const double value = span > 0 ? std::clamp((plain - found->minValue) / span, 0.0, 1.0) : 0.0;
    const auto order = ++m_midiEventOrder;
    {
        RetrospectiveMidiEvent e;
        e.trackId = trackId;
        e.parameterId = parameter;
        e.parameterName = found->name;
        e.stamp = stamp;
        e.order = order;
        e.value = value;
        m_liveMidiControllers[trackId + ":" + parameter] = e;
        if (retrospective) {
            appendTimedMidi(m_retrospectiveMidi, std::move(e));
            pruneRetrospectiveMidi(stamp.timeNs);
        }
    }
    for (auto &capture : m_captures) {
        if (!capture.midi || capture.trackId != trackId || stamp.timeNs < capture.midiStart.timeNs)
            continue;
        const double beat = std::max(0.0, stamp.transportBeats - capture.midiStart.transportBeats);
        capture.midiLastBeat = std::max(capture.midiLastBeat, beat);
        capture.midiInputReceived = true;
        const bool firstTouch = std::none_of(
            capture.midiRecording.data.lanes.begin(), capture.midiRecording.data.lanes.end(),
            [&](const auto &lane) { return lane.cc == -1 && lane.parameterId == parameter; });
        const auto initial = capture.midiInitialParameters.find(parameter);
        if (firstTouch && initial != capture.midiInitialParameters.end())
            capture.midiRecording.parameter({}, parameter, found->name, initial->second, 0, 0);
        capture.midiRecording.parameter({}, parameter, found->name, value, beat, order);
        m_runtime.setPluginAutomationOverride(pluginAddress(trackId, slot), parameter);
    }
}

void EngineController::seedMidiControllers(const std::string &track, MidiRecording &recording) {
    const auto feed = [&](const RetrospectiveMidiEvent &e) {
        if (e.trackId != track)
            return;
        if (!e.parameterId.empty())
            recording.parameter(e.slotId, e.parameterId, e.parameterName, e.value, 0, 0);
        else if ((e.status & 0xf0) != 0x90 && (e.status & 0xf0) != 0x80 &&
                 !((e.status & 0xf0) == 0xb0 && e.d1 >= 120))
            recording.event(e.source, e.status, e.d1, e.d2, 0, 0);
    };
    for (const auto &[key, e] : m_liveMidiControllers)
        feed(e);
    for (auto &lane : recording.data.lanes)
        if (!lane.points.empty()) {
            const auto last = lane.points.back();
            lane.points = {last};
            lane.defaultValue = last.value;
        }
}

void EngineController::pruneRetrospectiveMidi(std::uint64_t now) {
    constexpr std::uint64_t window = 600ULL * 1000000000ULL;
    const auto cutoff = now > window ? now - window : 0;
    while (!m_retrospectiveMidi.empty() && m_retrospectiveMidi.front().stamp.timeNs < cutoff) {
        auto e = std::move(m_retrospectiveMidi.front());
        m_retrospectiveMidi.pop_front();
        const int type = e.status & 0xf0;
        const bool note = type == 0x80 || type == 0x90;
        if (type == 0xb0 && (e.d1 == 120 || e.d1 == 123))
            std::erase_if(m_retrospectiveState, [&](const auto &item) {
                const auto &h = item.second;
                return h.trackId == e.trackId && h.source == e.source &&
                       (h.status & 15) == (e.status & 15) && (h.status & 0xf0) == 0x90;
            });
        const auto key = e.trackId + ":" +
                         (e.parameterId.empty()
                              ? std::to_string(note ? 0x90 | (e.status & 15) : e.status) + ":" +
                                    std::to_string(e.d1 * (note || type == 0xb0 || type == 0xa0)) +
                                    ":" + std::to_string(note ? e.source : 0)
                              : e.parameterId);
        if (note && (type == 0x80 || e.d2 == 0))
            m_retrospectiveState.erase(key);
        else
            m_retrospectiveState[key] = std::move(e);
    }
}

bool EngineController::hasRetrospectiveMidi() const {
    if (isRecording() || isCountingIn() || m_retrospectivePendingThrough)
        return false;
    const auto now = midiInputStamp().timeNs;
    const_cast<EngineController *>(this)->pruneRetrospectiveMidi(now);
    if (std::any_of(m_retrospectiveState.begin(), m_retrospectiveState.end(),
                    [](const auto &entry) { return (entry.second.status & 0xf0) == 0x90; }))
        return true;
    return std::any_of(m_retrospectiveMidi.begin(), m_retrospectiveMidi.end(), [&](const auto &e) {
        return e.stamp.timeNs + 600ULL * 1000000000ULL >= now &&
               (!e.parameterId.empty() || (e.status & 0xf0) != 0x80) &&
               !((e.status & 0xf0) == 0x90 && !e.d2);
    });
}

void EngineController::clearRetrospectiveMidi(std::uint64_t through) {
    if (through >= m_retrospectivePendingThrough)
        m_retrospectivePendingThrough = 0;
    std::erase_if(m_retrospectiveMidi, [&](const auto &e) { return e.stamp.timeNs <= through; });
    std::erase_if(m_retrospectiveState,
                  [&](const auto &entry) { return entry.second.stamp.timeNs <= through; });
}

bool EngineController::restoreRetrospectiveMidi(double startSeconds) {
    m_midiCaptureError.clear();
    if (!hasRetrospectiveMidi() || !std::isfinite(startSeconds))
        return false;

    const auto through = midiInputStamp().timeNs;
    const auto cutoff = through > 600ULL * 1000000000ULL ? through - 600ULL * 1000000000ULL : 0;
    pruneRetrospectiveMidi(through);
    auto first = std::find_if(m_retrospectiveMidi.begin(), m_retrospectiveMidi.end(),
                              [&](const auto &e) { return e.stamp.timeNs >= cutoff; });
    if (first == m_retrospectiveMidi.end() && m_retrospectiveState.empty())
        return false;
    std::map<std::string, MidiRecording> recordings;
    const double bps = m_project.tempo / 60.0;
    const bool heldAtBoundary =
        std::any_of(m_retrospectiveState.begin(), m_retrospectiveState.end(),
                    [](const auto &entry) { return (entry.second.status & 0xf0) == 0x90; });
    if (!heldAtBoundary)
        first = std::find_if(first, m_retrospectiveMidi.end(), [](const auto &e) {
            return !e.parameterId.empty() ||
                   ((e.status & 0xf0) != 0x80 && !((e.status & 0xf0) == 0x90 && !e.d2));
        });
    if (!heldAtBoundary && first == m_retrospectiveMidi.end())
        return false;
    const auto begin = heldAtBoundary ? cutoff : first->stamp.timeNs;
    std::unordered_set<std::string> activeTracks;
    for (auto it = first; it != m_retrospectiveMidi.end(); ++it)
        activeTracks.insert(it->trackId);
    for (const auto &[key, e] : m_retrospectiveState)
        if ((e.status & 0xf0) == 0x90)
            activeTracks.insert(e.trackId);
    for (const auto &[key, e] : m_retrospectiveState) {
        if (!activeTracks.contains(e.trackId))
            continue;
        const auto *track = m_project.findTrack(e.trackId);
        if (!track || !trackAccepts(track->kind, ClipKind::Midi)) {
            m_midiCaptureError = "Исходный MIDI-трек недоступен. Сыгранное сохранено в буфере.";
            return false;
        }
        if (!e.parameterId.empty())
            recordings[e.trackId].parameter(e.slotId, e.parameterId, e.parameterName, e.value, 0,
                                            e.order);
        else
            recordings[e.trackId].event(e.source, e.status, e.d1, e.d2, 0, e.order);
    }
    double lastBeat = 0.0;
    for (auto it = first; it != m_retrospectiveMidi.end(); ++it) {
        const auto *track = m_project.findTrack(it->trackId);
        if (!track || !trackAccepts(track->kind, ClipKind::Midi)) {
            m_midiCaptureError = "Исходный MIDI-трек недоступен. Сыгранное сохранено в буфере.";
            return false;
        }
        const double beat = double(it->stamp.timeNs - begin) / 1e9 * bps;
        lastBeat = std::max(lastBeat, beat);
        auto &recording = recordings[it->trackId];
        if (!it->parameterId.empty())
            recording.parameter(it->slotId, it->parameterId, it->parameterName, it->value, beat,
                                it->order);
        else
            recording.event(it->source, it->status, it->d1, it->d2, beat, it->order);
    }
    for (const auto &[id, recording] : recordings)
        if (recording.hasHeldNotes())
            lastBeat = std::max(lastBeat, double(through - begin) / 1e9 * bps);
    // Ignore unmatched releases when trimming the tail. They carry no playable
    // material (for example a key held before the last successful recovery).
    std::map<std::string, MidiPerformance> performances;
    double contentEnd = 0.0;
    for (const auto &[id, recording] : recordings) {
        auto data = recording.finished(std::max(lastBeat, bps / m_sampleRate));
        for (const auto &note : data.notes)
            contentEnd = std::max(contentEnd, note.startBeats + note.lengthBeats);
        for (const auto &lane : data.lanes)
            for (const auto &point : lane.points)
                contentEnd = std::max(contentEnd, point.beats);
        performances.emplace(id, std::move(data));
    }
    lastBeat = contentEnd;
    struct Landing {
        std::string track;
        std::vector<ClipModel> before, after;
    };
    std::vector<Landing> landings;
    std::vector<std::pair<std::string, ClipModel>> recovered;
    for (auto &[id, data] : performances) {
        if (data.empty())
            continue;
        auto *track = m_project.findTrack(id);
        Landing landing{id, track->clips, {}};
        ClipModel clip;
        clip.id = newUuid();
        clip.kind = ClipKind::Midi;
        clip.name = "Recovered MIDI";
        clip.startSeconds = std::max(0.0, startSeconds);
        clip.durationSeconds =
            std::nextafter(std::max(1.0 / m_sampleRate, lastBeat / bps), INFINITY);
        clip.color = track->color;
        clip.notes = std::move(data.notes);
        clip.lanes = std::move(data.lanes);
        if (cloudProjectBound()) {
            recovered.emplace_back(id, std::move(clip));
            continue;
        }
        track->clips.push_back(std::move(clip));
        landing.after = track->clips;
        landings.push_back(std::move(landing));
    }
    if (cloudProjectBound()) {
        if (recovered.empty())
            return false;
        if (submitRecoveredMidi && submitRecoveredMidi(recovered, through)) {
            markRetrospectiveMidiPending(through);
            return true;
        }
        m_midiCaptureError = "Восстановление MIDI ожидает доступного совместного проекта версии 4.";
        return false;
    }
    if (landings.empty())
        return false;
    const auto apply = [this, landings](bool after) {
        for (const auto &landing : landings)
            if (auto *track = m_project.findTrack(landing.track))
                track->clips = after ? landing.after : landing.before;
        rebuildGraph();
    };
    m_undo.push("Restore played MIDI", [apply] { apply(false); }, [apply] { apply(true); });
    clearRetrospectiveMidi(through);
    rebuildGraph();
    return true;
}

std::vector<ClipModel>
EngineController::midiRecordingLanding(const TrackModel &before,
                                       const FinalizedRecordingTrack &recording) {
    TrackModel track = before;
    landMidiCapture(track, recording);
    return std::move(track.clips);
}

void EngineController::landMidiCapture(TrackModel &track,
                                       const FinalizedRecordingTrack &recording) {
    const double bps = recording.midiTempo / 60.0;
    const auto patternAt = [&](double seconds) {
        for (auto parent = track.parentId; !parent.empty();) {
            const auto *owner = m_project.findTrack(parent);
            if (!owner)
                break;
            if (owner->kind == TrackKind::Pattern) {
                for (const auto &c : owner->clips)
                    if (c.kind == ClipKind::Pattern && seconds >= c.startSeconds &&
                        seconds < c.startSeconds + c.durationSeconds)
                        return c.id;
                break;
            }
            parent = owner->parentId;
        }
        return std::string();
    };
    for (const auto &pass : recording.passes) {
        const double length = pass.endSeconds - pass.startSeconds;
        const double from = pass.captureOffsetSeconds * bps,
                     to = (pass.captureOffsetSeconds + length) * bps;
        // A carried controller value alone does not make a new loop attempt.
        const bool played =
            std::any_of(recording.performance.notes.begin(), recording.performance.notes.end(),
                        [&](const auto &n) {
                            return n.startBeats < to && n.startBeats + n.lengthBeats > from;
                        }) ||
            std::any_of(recording.performance.lanes.begin(), recording.performance.lanes.end(),
                        [&](const auto &lane) {
                            return std::any_of(
                                lane.points.begin(), lane.points.end(), [&](const auto &p) {
                                    return p.eventOrder != 0 && p.beats >= from && p.beats < to;
                                });
                        });
        if (!played)
            continue;
        auto data = sliceMidiPerformance(recording.performance, pass.captureOffsetSeconds * bps,
                                         (pass.captureOffsetSeconds + length) * bps);
        if (data.empty())
            continue;
        ClipModel *target = nullptr;
        for (auto &clip : track.clips)
            if (clip.kind == ClipKind::Midi && clip.startSeconds < pass.endSeconds &&
                clip.startSeconds + clip.durationSeconds > pass.startSeconds) {
                target = &clip;
                break;
            }
        // A newly recorded layer uses the visible clip origin. Rebase the old
        // audible range once before merging, rather than mixing source and
        // visible coordinates in notes, take placements and comp windows.
        if (target && target->offsetSeconds > 0.0)
            sliceMidiClipContent(*target, 0.0, target->durationSeconds, recording.midiTempo, false);
        if (recording.semantics.midiOverdubMerge && target) {
            ClipModel merged = *target;
            merged.notes.clear();
            merged.slideNotes.clear();
            merged.lanes.clear();
            merged.takes.clear();
            merged.comp.clear();
            TrackModel audible;
            audible.clips.push_back(*target);
            for (const auto &part : midiPlaybackClips(audible, recording.midiTempo)) {
                auto content =
                    sliceMidiPerformance({part.notes, part.lanes,part.slideNotes}, 0, part.durationSeconds * bps);
                mergeMidiPerformance(
                    merged, std::move(content), (part.startSeconds - target->startSeconds) * bps,
                    (part.startSeconds + part.durationSeconds - target->startSeconds) * bps);
            }
            const double earlier = std::min(pass.startSeconds, target->startSeconds);
            const double shift = (target->startSeconds - earlier) * bps;
            for (auto &n : merged.notes)
                n.startBeats += shift;
            for (auto &lane : merged.lanes)
                for (auto &p : lane.points)
                    p.beats += shift;
            for(auto& slide:merged.slideNotes)slide.startBeats+=shift;
            merged.startSeconds = earlier;
            merged.offsetSeconds = 0;
            merged.durationSeconds =
                std::max(target->startSeconds + target->durationSeconds, pass.endSeconds) - earlier;
            // Chased state is needed when a lane is new, but does not mean the
            // user touched an existing automation lane in this overdub pass.
            std::erase_if(data.lanes, [&](const auto &lane) {
                return std::none_of(lane.points.begin(), lane.points.end(),
                                    [](const auto &point) { return point.eventOrder != 0; }) &&
                       std::any_of(merged.lanes.begin(), merged.lanes.end(),
                                   [&](const auto &existing) {
                                       return sameMidiLaneTarget(existing, lane);
                                   });
            });
            mergeMidiPerformance(merged, std::move(data), (pass.startSeconds - earlier) * bps,
                                 (pass.endSeconds - earlier) * bps);
            if (target->takes.empty()) {
                *target = std::move(merged);
                continue;
            }
            // Existing layers remain available; the new comp is the audible material plus this
            // overdub.
            const double oldShift = target->startSeconds - earlier;
            for (auto &take : target->takes) {
                for (auto &n : take.notes)
                    n.startBeats += oldShift * bps;
                for (auto &lane : take.lanes)
                    for (auto &p : lane.points)
                        p.beats += oldShift * bps;
                take.lengthSeconds += oldShift;
            }
            for (auto &segment : target->comp) {
                segment.startSeconds += oldShift;
                segment.endSeconds += oldShift;
            }
            target->startSeconds = earlier;
            target->durationSeconds = merged.durationSeconds;
            TakeModel take;
            take.id = newUuid();
            take.name = "MIDI overdub";
            take.notes = std::move(merged.notes);
            take.slideNotes=std::move(merged.slideNotes);
            take.lanes = std::move(merged.lanes);
            take.lengthSeconds = merged.durationSeconds;
            take.color = takeColor(track.color, target->takes.size());
            const auto id = take.id;
            target->takes.push_back(std::move(take));
            selectWholeTake(*target, id);
            continue;
        }
        const bool layers =
            recording.semantics.mode == RecordMode::Layers ||
            (recording.semantics.loopEnabled && recording.semantics.loopCreatesTakes);
        if (!layers) {
            clearTrackRange(track, pass.startSeconds, pass.endSeconds);
            ClipModel clip;
            clip.id = newUuid();
            clip.name = "Recorded MIDI";
            clip.kind = ClipKind::Midi;
            clip.startSeconds = pass.startSeconds;
            clip.durationSeconds = length;
            clip.color = track.color;
            clip.patternClipId = patternAt(pass.startSeconds);
            clip.notes = std::move(data.notes);
            clip.lanes = std::move(data.lanes);
            track.clips.push_back(std::move(clip));
            continue;
        }
        if (!target) {
            ClipModel clip;
            clip.id = newUuid();
            clip.name = "Recorded MIDI";
            clip.kind = ClipKind::Midi;
            clip.startSeconds = pass.startSeconds;
            clip.durationSeconds = length;
            clip.color = track.color;
            clip.patternClipId = patternAt(pass.startSeconds);
            track.clips.push_back(std::move(clip));
            target = &track.clips.back();
        } else
            promoteToTake(*target);
        if (pass.startSeconds < target->startSeconds) {
            const double shift = target->startSeconds - pass.startSeconds;
            target->startSeconds -= shift;
            target->durationSeconds += shift;
            for (auto &take : target->takes) {
                for (auto &n : take.notes)
                    n.startBeats += shift * bps;
                for (auto &lane : take.lanes)
                    for (auto &p : lane.points)
                        p.beats += shift * bps;
                take.lengthSeconds += shift;
            }
            for (auto &segment : target->comp) {
                segment.startSeconds += shift;
                segment.endSeconds += shift;
            }
        }
        target->durationSeconds =
            std::max(target->durationSeconds, pass.endSeconds - target->startSeconds);
        TakeModel take;
        take.id = newUuid();
        take.name = "Take " + std::to_string(target->takes.size() + 1);
        take.notes = std::move(data.notes);
        take.lanes = std::move(data.lanes);
        take.lengthSeconds = target->durationSeconds;
        const double placement = (pass.startSeconds - target->startSeconds) * bps;
        for (auto &n : take.notes)
            n.startBeats += placement;
        for (auto &lane : take.lanes)
            for (auto &p : lane.points)
                p.beats += placement;
        take.color = takeColor(track.color, target->takes.size());
        const auto id = take.id;
        target->takes.push_back(std::move(take));
        if (recording.semantics.trimTakesToRegion)
            setCompRange(*target, id, pass.startSeconds - target->startSeconds,
                         pass.endSeconds - target->startSeconds);
        else
            selectWholeTake(*target, id);
        if (recording.semantics.autoExpandAfterRecord)
            target->expanded = true;
    }
}

} // namespace daw
