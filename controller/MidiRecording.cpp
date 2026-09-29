#include "MidiRecording.hpp"
#include "SlideNotes.hpp"

#include <algorithm>
#include <cmath>

namespace daw {

bool sameMidiLaneTarget(const ControllerLane &a, const ControllerLane &b) {
    return a.cc == b.cc && a.channel == b.channel && a.key == b.key && a.slotId == b.slotId &&
           a.parameterId == b.parameterId;
}

void MidiRecording::event(std::uint64_t source, int status, int d1, int d2, double beat,
                          std::uint64_t order) {
    if (!std::isfinite(beat))
        return;
    beat = std::max(0.0, beat);
    const int type = status & 0xf0, channel = status & 15;
    const auto key = std::tuple{source, channel, d1};
    const auto end = [&](auto it, int velocity) {
        auto &note = data.notes[it->second];
        note.lengthBeats = std::max(1e-9, beat - note.startBeats);
        note.releaseVelocity = velocity;
        note.endOrder = order;
        m_held.erase(it);
    };
    if (type == 0x80 || (type == 0x90 && !d2)) {
        if (auto it = m_held.find(key); it != m_held.end())
            end(it, d2);
    } else if (type == 0x90) {
        if (auto it = m_held.find(key); it != m_held.end())
            end(it, 0);
        NoteModel note;
        note.id = newUuid();
        note.pitch = d1;
        note.velocity = d2;
        note.channel = channel;
        note.startBeats = beat;
        note.lengthBeats = 1e-9;
        note.startOrder = order;
        m_held[key] = data.notes.size();
        data.notes.push_back(std::move(note));
    } else {
        int cc = -1, pitch = 0;
        double value = double(d1) / 127.0;
        std::string name;
        if (type == 0xb0) {
            cc = d1;
            value = double(d2) / 127.0;
            name = cc == 64 ? "Sustain" : "CC " + std::to_string(cc);
            if (d1 == 120 || d1 == 123)
                for (auto it = m_held.begin(); it != m_held.end();) {
                    auto current = it++;
                    if (std::get<0>(current->first) == source &&
                        std::get<1>(current->first) == channel)
                        end(current, 0);
                }
        } else if (type == 0xe0) {
            cc = -2;
            value = double(d1 | (d2 << 7)) / 16383.0;
            name = "Pitch Bend";
        } else if (type == 0xd0) {
            cc = -3;
            name = "Channel Pressure";
        } else if (type == 0xa0) {
            cc = -4;
            pitch = d1;
            value = double(d2) / 127.0;
            name = "Poly Pressure " + std::to_string(pitch);
        } else if (type == 0xc0) {
            cc = -5;
            name = "Program Change";
        }
        if (cc != -1)
            point(cc, channel, pitch, {}, {}, name, value, beat, order);
    }
}

void MidiRecording::point(int cc, int channel, int key, const std::string &slot,
                          const std::string &parameter, const std::string &name, double value,
                          double beat, std::uint64_t order) {
    ControllerLane target;
    target.cc = cc;
    target.channel = channel;
    target.key = key;
    target.slotId = slot;
    target.parameterId = parameter;
    auto lane = std::find_if(data.lanes.begin(), data.lanes.end(),
                             [&](const auto &l) { return sameMidiLaneTarget(l, target); });
    if (lane == data.lanes.end()) {
        target.id = newUuid();
        target.name = name;
        target.defaultValue = cc == -2 ? 8192.0 / 16383.0 : value;
        data.lanes.push_back(std::move(target));
        lane = std::prev(data.lanes.end());
    }
    AutomationPoint p;
    p.id = newUuid();
    p.beats = std::max(0.0, beat);
    p.value = std::clamp(value, 0.0, 1.0);
    p.shape = AutomationSegment::Hold;
    p.eventOrder = order;
    // Preserve repeated same-time messages (bank/program/RPN ordering matters).
    if (lane->points.empty() || lane->points.back().beats <= p.beats)
        lane->points.push_back(std::move(p));
    else {
        const auto position =
            std::upper_bound(lane->points.begin(), lane->points.end(), p.beats,
                             [](double beat, const auto &point) { return beat < point.beats; });
        lane->points.insert(position, std::move(p));
    }
}

void MidiRecording::parameter(const std::string &slot, const std::string &parameter,
                              const std::string &name, double value, double beat,
                              std::uint64_t order) {
    point(-1, 0, 0, slot, parameter, name, value, beat, order);
}

void MidiRecording::extendHeld(double beat) {
    for (const auto &[key, index] : m_held)
        data.notes[index].lengthBeats = std::max(1e-9, beat - data.notes[index].startBeats);
}

void MidiRecording::close(double beat) {
    extendHeld(beat);
    m_held.clear();
}

MidiPerformance MidiRecording::finished(double endBeat) const {
    MidiRecording copy = *this;
    copy.close(endBeat);
    return std::move(copy.data);
}

MidiPerformance sliceMidiPerformance(const MidiPerformance &data, double from, double to,
                                     bool newIds) {
    MidiPerformance result;
    if (!(to > from))
        return result;
    for (const auto &source : data.notes) {
        const double begin = std::max(from, source.startBeats);
        const double end = std::min(to, source.startBeats + source.lengthBeats);
        if (end <= begin)
            continue;
        auto note = source;
        if (newIds)
            note.id = newUuid();
        note.startBeats = begin - from;
        note.lengthBeats = end - begin;
        if (end < source.startBeats + source.lengthBeats)
            note.releaseVelocity = 0;
        result.notes.push_back(std::move(note));
    }
    result.slideNotes=slides::crop(data.notes,data.slideNotes,result.notes,from,to);
    for (const auto &source : data.lanes) {
        if (source.points.empty() || source.points.front().beats >= to)
            continue;
        auto lane = source;
        if (newIds)
            lane.id = newUuid();
        lane.points.clear();
        lane.defaultValue = automationValueAt(source.points, from, source.defaultValue);
        // Establish the value at a trim/loop boundary before any incoming notes.
        if (source.points.front().beats < from) {
            AutomationPoint p{0, lane.defaultValue, AutomationSegment::Hold};
            p.id = newUuid();
            lane.points.push_back(std::move(p));
        }
        for (const auto &sourcePoint : source.points) {
            if (sourcePoint.beats < from || sourcePoint.beats >= to)
                continue;
            auto p = sourcePoint;
            if (newIds)
                p.id = newUuid();
            p.beats -= from;
            lane.points.push_back(std::move(p));
        }
        result.lanes.push_back(std::move(lane));
    }
    return result;
}

void mergeMidiPerformance(ClipModel &clip, MidiPerformance data, double offset, double end) {
    for(auto& s:data.slideNotes){s.startBeats+=offset;clip.slideNotes.push_back(std::move(s));}
    for (auto &n : data.notes) {
        n.startBeats += offset;
        clip.notes.push_back(std::move(n));
    }
    std::stable_sort(clip.notes.begin(), clip.notes.end(),
                     [](const auto &a, const auto &b) { return a.startBeats < b.startBeats; });
    for (auto &lane : data.lanes) {
        for (auto &p : lane.points)
            p.beats += offset;
        auto found = std::find_if(clip.lanes.begin(), clip.lanes.end(),
                                  [&](const auto &l) { return sameMidiLaneTarget(l, lane); });
        if (found == clip.lanes.end()) {
            clip.lanes.push_back(std::move(lane));
            continue;
        }
        const double after = automationValueAt(found->points, end, found->defaultValue);
        std::erase_if(found->points,
                      [&](const auto &p) { return p.beats >= offset && p.beats < end; });
        found->points.insert(found->points.end(), lane.points.begin(), lane.points.end());
        AutomationPoint restore{end, after, AutomationSegment::Hold};
        restore.id = newUuid();
        found->points.push_back(std::move(restore));
        normalizeAutomation(found->points);
    }
}

void sliceMidiClipContent(ClipModel &clip, double from, double to, double tempo, bool newIds) {
    const double bps = tempo / 60.0;
    // Notes and comp windows keep their source origin through non-destructive
    // edge trims. Materialize a requested range relative to the visible head.
    from += clip.offsetSeconds;
    to += clip.offsetSeconds;
    auto data = sliceMidiPerformance({clip.notes, clip.lanes, clip.slideNotes}, from * bps, to * bps, newIds);
    if (!newIds) {
        std::vector<ControllerLane> lanes;
        for (const auto &original : clip.lanes) {
            auto found = std::find_if(data.lanes.begin(), data.lanes.end(),
                                      [&](const auto &lane) { return lane.id == original.id; });
            if (found != data.lanes.end())
                lanes.push_back(std::move(*found));
            else {
                auto lane = original;
                lane.points.clear();
                lanes.push_back(std::move(lane));
            }
        }
        data.lanes = std::move(lanes);
    }
    clip.notes = std::move(data.notes);
    clip.slideNotes = std::move(data.slideNotes);
    clip.lanes = std::move(data.lanes);
    clip.offsetSeconds = 0;
    for (auto &take : clip.takes) {
        auto data = sliceMidiPerformance(
            {take.notes, take.lanes, take.slideNotes}, (from - take.clipOffsetSeconds + take.offsetSeconds) * bps,
            (to - take.clipOffsetSeconds + take.offsetSeconds) * bps, newIds);
        take.notes = std::move(data.notes);
        take.slideNotes = std::move(data.slideNotes);
        take.lanes = std::move(data.lanes);
        take.clipOffsetSeconds = 0;
        take.offsetSeconds = 0;
        take.lengthSeconds = to - from;
        if (newIds) {
            auto old = take.id;
            take.id = newUuid();
            for (auto &c : clip.comp)
                if (c.takeId == old)
                    c.takeId = take.id;
        }
    }
    for (auto &c : clip.comp) {
        c.startSeconds = std::max(c.startSeconds, from) - from;
        c.endSeconds = std::min(c.endSeconds, to) - from;
        if (newIds)
            c.id = newUuid();
    }
    std::erase_if(clip.comp, [](const auto &c) { return c.endSeconds <= c.startSeconds; });
}

std::vector<ClipModel> midiPlaybackClips(const TrackModel &track, double tempo) {
    std::vector<ClipModel> result;
    const double bps = tempo / 60.0;
    for (const auto &clip : track.clips) {
        if (clip.kind != ClipKind::Midi)
            continue;
        if (clip.takes.empty()) {
            result.push_back(clip);
            if (clip.offsetSeconds > 0.0) {
                auto& part = result.back();
                auto data = sliceMidiPerformance(
                    {std::move(part.notes), std::move(part.lanes), std::move(part.slideNotes)}, clip.offsetSeconds * bps,
                    (clip.offsetSeconds + clip.durationSeconds) * bps, false);
                part.notes = std::move(data.notes);
                part.lanes = std::move(data.lanes);
                part.slideNotes = std::move(data.slideNotes);
                part.offsetSeconds = 0.0;
            }
            continue;
        }
        for (const auto &segment : clip.comp) {
            const auto take = std::find_if(clip.takes.begin(), clip.takes.end(),
                                           [&](const auto &t) { return t.id == segment.takeId; });
            if (take == clip.takes.end() || take->muted)
                continue;
            const double from = std::max({clip.offsetSeconds, segment.startSeconds, take->clipOffsetSeconds});
            const double to = std::min({clip.offsetSeconds + clip.durationSeconds, segment.endSeconds,
                                        take->clipOffsetSeconds + take->lengthSeconds});
            auto data = sliceMidiPerformance(
                {take->notes, take->lanes, take->slideNotes},
                (from - take->clipOffsetSeconds + take->offsetSeconds) * bps,
                (to - take->clipOffsetSeconds + take->offsetSeconds) * bps, false);
            if (!(to > from))
                continue;
            ClipModel part;
            part.id = clip.id;
            part.kind = ClipKind::Midi;
            part.muted = clip.muted;
            part.patternClipId = clip.patternClipId;
            part.startSeconds = clip.startSeconds + from - clip.offsetSeconds;
            part.durationSeconds = to - from;
            part.notes = std::move(data.notes);
            part.slideNotes = std::move(data.slideNotes);
            part.lanes = std::move(data.lanes);
            result.push_back(std::move(part));
        }
    }
    return result;
}

} // namespace daw
