#include "MidiContentJson.hpp"
#include "ProjectSerializer.hpp"
#include "serialization/LegacyClipContent.hpp"
#include "SlideJson.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_set>
namespace daw::collab {
using json = nlohmann::json;
namespace {
json wrapper(const ClipModel &clip) {
    ProjectModel project;
    TrackModel track;
    track.id = "00000000-0000-4000-8000-000000000001";
    track.kind = TrackKind::Midi;
    track.clips.push_back(clip);
    project.tracks.push_back(std::move(track));
    std::string bytes;
    ProjectSerializer::serializeDocument(project, bytes);
    auto root = json::parse(bytes);
    serialization::expandLegacyClipContent(root);
    return root;
}
bool keys(const json &j, std::initializer_list<const char *> allowed) {
    if (!j.is_object())
        return false;
    for (const auto &[k, v] : j.items())
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char *a) { return k == a; }))
            return false;
    return true;
}
} // namespace
json midiContentToJson(const ClipModel &content) {
    auto root = wrapper(content);
    const auto &clip = root["tracks"][0]["clips"][0];
    json out{{"notes", clip.value("notes", json::array())},
             {"lanes", clip.value("lanes", json::array())},
             {"takes", clip.value("takes", json::array())},
             {"comp", clip.value("comp", json::array())},
             {"expanded", content.expanded}};
    if (!content.slideNotes.empty()) out["slideNotes"] = slides::toJson(content.slideNotes);
    const auto points = [](json &lanes) {
        for (auto &lane : lanes)
            for (auto &point : lane["points"]) {
                if (!point.contains("shape"))
                    point["shape"] = "linear";
                if (!point.contains("curve"))
                    point["curve"] = 0.0;
            }
    };
    points(out["lanes"]);
    for (auto &take : out["takes"]) {
        take.erase("file");
        take.erase("asset");
        if (take.contains("lanes"))
            points(take["lanes"]);
    }
    return out;
}
bool midiContentFromJson(const json &value, ClipModel &out) {
    if (!keys(value, {"notes", "lanes", "takes", "comp", "expanded", "slideNotes"}) || (value.size() != 5 && value.size() != 6))
        return false;
    for (auto key : {"notes", "lanes", "takes", "comp"})
        if (!value.at(key).is_array())
            return false;
    if (!value.at("expanded").is_boolean())
        return false;
    try {
        if (value.contains("slideNotes")) (void)slides::fromArray(value.at("slideNotes"));
        // Validate the wire values before the project reader's compatibility
        // defaults/clamping. Parts deliberately need not contain every take.
        const auto number = [](const json &j, const char *k, double lo, double hi) {
            if (!j.contains(k) || !j.at(k).is_number())
                return false;
            const double v = j.at(k).get<double>();
            return std::isfinite(v) && v >= lo && v <= hi;
        };
        const auto integer = [&](const json &j, const char *k, double lo, double hi) {
            return j.contains(k) && j.at(k).is_number_integer() && number(j, k, lo, hi);
        };
        const auto optionalInteger = [&](const json &j, const char *k, double lo, double hi) {
            return !j.contains(k) || integer(j, k, lo, hi);
        };
        const auto id = [](const json &j, const char *k) {
            return j.contains(k) && j.at(k).is_string() && isUuid(j.at(k).get<std::string>());
        };
        const auto valid = [&](const json &notes, const json &lanes) {
            if (!notes.is_array() || !lanes.is_array())
                return false;
            for (const auto &n : notes) {
                if (!keys(n,
                          {"id", "pitch", "startBeats", "lengthBeats", "velocity", "muted", "color",
                           "pan", "channel", "releaseVelocity", "startOrder", "endOrder"}) ||
                    !id(n, "id") || !integer(n, "pitch", 0, 127) ||
                    !integer(n, "velocity", 1, 127) || !number(n, "startBeats", 0, 1e100) ||
                    !number(n, "lengthBeats", std::numeric_limits<double>::min(), 1e100) ||
                    !number(n, "pan", -1, 1) || !n.at("muted").is_boolean() ||
                    !integer(n, "color", 0, 4294967295.0) ||
                    !optionalInteger(n, "channel", 0, 15) ||
                    !optionalInteger(n, "releaseVelocity", 0, 127) ||
                    !optionalInteger(n, "startOrder", 0, 18446744073709551615.0) ||
                    !optionalInteger(n, "endOrder", 0, 18446744073709551615.0))
                    return false;
            }
            for (const auto &l : lanes) {
                if (!keys(l, {"id", "name", "cc", "channel", "key", "parameterId", "slotId",
                              "defaultValue", "points"}) ||
                    !id(l, "id") || !integer(l, "cc", -5, 127) || !integer(l, "channel", 0, 15) ||
                    !integer(l, "key", 0, 127) || !number(l, "defaultValue", 0, 1) ||
                    !l.at("name").is_string() || l.at("name").get<std::string>().size() > 4096 ||
                    !l.at("parameterId").is_string() || !l.at("slotId").is_string() ||
                    !l.at("points").is_array())
                    return false;
                if (l.at("cc") == -1 && l.at("parameterId").get<std::string>().empty())
                    return false;
                if (!l.at("slotId").get<std::string>().empty() && !id(l, "slotId"))
                    return false;
                for (const auto &p : l.at("points")) {
                    if (!keys(p, {"id", "beats", "value", "shape", "curve", "eventOrder"}) ||
                        !id(p, "id") || !number(p, "beats", 0, 1e100) ||
                        !number(p, "value", 0, 1) ||
                        (p.contains("curve") && !number(p, "curve", -1, 1)) ||
                        !optionalInteger(p, "eventOrder", 0, 18446744073709551615.0))
                        return false;
                    if (p.contains("shape")) {
                        const auto shape = p.at("shape").get<std::string>();
                        if (shape != "linear" && shape != "hold" && shape != "scurve")
                            return false;
                    }
                }
            }
            return true;
        };
        if (!valid(value.at("notes"), value.at("lanes")))
            return false;
        for (const auto &t : value.at("takes")) {
            if (t.contains("slideNotes")) (void)slides::fromArray(t.at("slideNotes"));
            if (!keys(t, {"id", "name", "offsetSeconds", "lengthSeconds", "clipOffsetSeconds",
                          "gain", "muted", "channels", "color", "notes", "lanes", "slideNotes"}) ||
                !id(t, "id") || !t.at("name").is_string() ||
                t.at("name").get<std::string>().size() > 4096 ||
                !number(t, "offsetSeconds", 0, 1e100) || !number(t, "lengthSeconds", 0, 1e100) ||
                !number(t, "clipOffsetSeconds", 0, 1e100) || !number(t, "gain", 0, 4) ||
                !t.at("muted").is_boolean() || !integer(t, "channels", 0, 1024) ||
                !integer(t, "color", 0, 4294967295.0) ||
                !valid(t.at("notes"), t.value("lanes", json::array())))
                return false;
        }
        std::vector<CompSegment> comp;
        for (const auto &c : value.at("comp")) {
            if (!keys(c, {"id", "takeId", "startSeconds", "endSeconds"}) || !id(c, "id") ||
                !id(c, "takeId") || !number(c, "startSeconds", 0, 1e100) ||
                !number(c, "endSeconds", 0, 1e100) ||
                c.at("endSeconds").get<double>() <= c.at("startSeconds").get<double>())
                return false;
            CompSegment segment;
            segment.id = c.at("id");
            segment.takeId = c.at("takeId");
            segment.startSeconds = c.at("startSeconds");
            segment.endSeconds = c.at("endSeconds");
            comp.push_back(std::move(segment));
        }
        ClipModel base;
        base.id = "00000000-0000-4000-8000-000000000002";
        base.kind = ClipKind::Midi;
        auto root = wrapper(base);
        auto &clip = root["tracks"][0]["clips"][0];
        for (const auto &[k, v] : value.items())
            if (k != "comp")
                clip[k] = v;
        ProjectModel project;
        if (!ProjectSerializer::deserializeDocument(project, root.dump()).isOk() ||
            project.tracks.empty() || project.tracks[0].clips.empty())
            return false;
        out = std::move(project.tracks[0].clips[0]);
        out.comp = std::move(comp);
        return true;
    } catch (...) {
        return false;
    }
}
std::vector<PrepareMidiPart> prepareMidiContent(const std::string &recordingId,
                                                const std::string &contentId,
                                                const ClipModel &clip) {
    std::vector<PrepareMidiPart> parts;
    auto add = [&](ClipModel content) {
        parts.push_back(
            {recordingId, contentId, std::uint32_t(parts.size()), 0, std::move(content)});
    };
    ClipModel header;
    header.kind = ClipKind::Midi;
    header.expanded = clip.expanded;
    for (const auto &take : clip.takes) {
        auto t = take;
        t.notes.clear();
        t.slideNotes.clear();
        t.lanes.clear();
        header.takes.push_back(std::move(t));
        if (header.takes.size() == 64) {
            add(std::move(header));
            header = {};
            header.kind = ClipKind::Midi;
        }
    }
    for (const auto &segment : clip.comp) {
        header.comp.push_back(segment);
        if (header.comp.size() == 256) {
            add(std::move(header));
            header = {};
            header.kind = ClipKind::Midi;
        }
    }
    add(std::move(header));
    const auto append = [&](const std::vector<NoteModel> &notes,
                            const std::vector<ControllerLane> &lanes, const TakeModel *take) {
        const auto emit = [&](std::vector<NoteModel> ns, std::vector<ControllerLane> ls) {
            ClipModel part;
            part.kind = ClipKind::Midi;
            if (take) {
                auto t = *take;
                t.slideNotes.clear();
                t.notes = std::move(ns);
                t.lanes = std::move(ls);
                part.takes.push_back(std::move(t));
            } else {
                part.notes = std::move(ns);
                part.lanes = std::move(ls);
            }
            add(std::move(part));
        };
        constexpr std::size_t size = 256;
        for (std::size_t i = 0; i < notes.size(); i += size)
            emit({notes.begin() + i, notes.begin() + std::min(notes.size(), i + size)}, {});
        for (const auto &lane : lanes) {
            if (lane.points.empty()) {
                emit({}, {lane});
                continue;
            }
            for (std::size_t i = 0; i < lane.points.size(); i += size) {
                auto l = lane;
                l.points.assign(lane.points.begin() + i,
                                lane.points.begin() + std::min(lane.points.size(), i + size));
                emit({}, {std::move(l)});
            }
        }
    };
    append(clip.notes, clip.lanes, nullptr);
    for (const auto &t : clip.takes)
        append(t.notes, t.lanes, &t);
    auto appendSlides=[&](const auto& list,const TakeModel* take){
        for(std::size_t i=0;i<list.size();i+=16){ClipModel content;content.kind=ClipKind::Midi;
            std::vector<SlideNoteModel> group(list.begin()+i,list.begin()+std::min(list.size(),i+16));
            if(take){auto t=*take;t.notes.clear();t.lanes.clear();t.slideNotes=std::move(group);content.takes.push_back(std::move(t));}
            else content.slideNotes=std::move(group);
            add(std::move(content));
        }
    };
    appendSlides(clip.slideNotes,nullptr);for(const auto& t:clip.takes)appendSlides(t.slideNotes,&t);
    for (auto &part : parts)
        part.count = std::uint32_t(parts.size());
    return parts;
}
bool validMidiContentIdentities(const ClipModel &clip) {
    std::unordered_set<std::string> ids, takes;
    const auto content = [&](const auto &notes, const auto &lanes) {
        for (const auto &note : notes)
            if (!ids.insert(note.id).second)
                return false;
        for (const auto &lane : lanes) {
            if (!ids.insert(lane.id).second)
                return false;
            for (const auto &point : lane.points)
                if (!ids.insert(point.id).second)
                    return false;
        }
        return true;
    };
    for (const auto& s:clip.slideNotes) if(!ids.insert(s.id).second||!slides::valid(s))return false;
    for (const auto& t:clip.takes)for(const auto& s:t.slideNotes)if(!ids.insert(s.id).second||!slides::valid(s))return false;
    if (!content(clip.notes, clip.lanes))
        return false;
    for (const auto &take : clip.takes)
        if (!takes.insert(take.id).second || !ids.insert(take.id).second ||
            !content(take.notes, take.lanes))
            return false;
    for (const auto &c : clip.comp)
        if (!ids.insert(c.id).second || !takes.contains(c.takeId))
            return false;
    return true;
}
ClipModel joinMidiContent(const std::vector<PrepareMidiPart> &parts) {
    ClipModel out;
    out.kind = ClipKind::Midi;
    const auto merge = [](auto &notes, auto &lanes, const auto &ns, const auto &ls) {
        notes.insert(notes.end(), ns.begin(), ns.end());
        for (const auto &lane : ls) {
            auto it = std::find_if(lanes.begin(), lanes.end(),
                                   [&](const auto &l) { return l.id == lane.id; });
            if (it == lanes.end())
                lanes.push_back(lane);
            else
                it->points.insert(it->points.end(), lane.points.begin(), lane.points.end());
        }
    };
    for (const auto &part : parts) {
        out.expanded = out.expanded || part.content.expanded;
        out.comp.insert(out.comp.end(), part.content.comp.begin(), part.content.comp.end());
        merge(out.notes, out.lanes, part.content.notes, part.content.lanes);
        out.slideNotes.insert(out.slideNotes.end(),part.content.slideNotes.begin(),part.content.slideNotes.end());
        for (const auto &take : part.content.takes) {
            auto it = std::find_if(out.takes.begin(), out.takes.end(),
                                   [&](const auto &t) { return t.id == take.id; });
            if (it == out.takes.end())
                out.takes.push_back(take);
            else {
                merge(it->notes, it->lanes, take.notes, take.lanes);
                it->slideNotes.insert(it->slideNotes.end(),take.slideNotes.begin(),take.slideNotes.end());
            }
        }
    }
    return out;
}
} // namespace daw::collab
