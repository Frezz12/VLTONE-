#pragma once
#include "SlideNotes.hpp"
#include "collaboration/ProjectCommand.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace daw::slides {
inline nlohmann::json toJson(const SlideNoteModel &s) {
    auto points = nlohmann::json::array();
    for (const auto &p : s.points)
        points.push_back(
            {{"time", p.beats}, {"pitch", p.value}, {"shape", int(p.shape)}, {"curve", p.curve}});
    nlohmann::json out = {{"id", s.id},
                          {"startBeats", s.startBeats},
                          {"lengthBeats", s.lengthBeats},
                          {"referenceNoteId", s.referenceNoteId},
                          {"targetNoteIds", s.targetNoteIds},
                          {"chord", s.chord},
                          {"muted", s.muted},
                          {"points", points}};
    if (s.resumed)
        out["resumed"] = true;
    return out;
}
inline SlideNoteModel fromJson(const nlohmann::json &j) {
    if (!j.is_object() || (j.size() != 8 && !(j.size() == 9 && j.contains("resumed"))))
        throw std::invalid_argument("Invalid slide fields");
    SlideNoteModel s;
    s.id = j.at("id").get<std::string>();
    s.startBeats = j.at("startBeats").get<double>();
    s.lengthBeats = j.at("lengthBeats").get<double>();
    s.referenceNoteId = j.at("referenceNoteId").get<std::string>();
    s.targetNoteIds = j.at("targetNoteIds").get<std::vector<std::string>>();
    s.resumed = j.value("resumed", false);
    s.chord = j.at("chord").get<bool>();
    s.muted = j.at("muted").get<bool>();
    if (!j.at("points").is_array() || j.at("points").size() > maxPoints)
        throw std::invalid_argument("Invalid slide curve");
    for (const auto &p : j.at("points")) {
        if (!p.is_object() || p.size() != 4 || !p.at("shape").is_number_integer())
            throw std::invalid_argument("Invalid slide point");
        const int shape = p.at("shape").get<int>();
        if (shape < 0 || shape > 2)
            throw std::invalid_argument("Invalid slide shape");
        s.points.push_back({p.at("time").get<double>(), p.at("pitch").get<double>(),
                            AutomationSegment(shape), p.at("curve").get<double>()});
    }
    if (!collab::isUuid(s.id) || (!s.referenceNoteId.empty() && !collab::isUuid(s.referenceNoteId)))
        throw std::invalid_argument("Invalid slide identity");
    std::unordered_set<std::string> ids;
    for (const auto &id : s.targetNoteIds)
        if (!collab::isUuid(id) || !ids.insert(id).second)
            throw std::invalid_argument("Invalid slide target");
    if (!valid(s))
        throw std::invalid_argument("Invalid slide note");
    return s;
}
inline nlohmann::json toJson(const std::vector<SlideNoteModel> &slides) {
    auto out = nlohmann::json::array();
    for (const auto &s : slides)
        out.push_back(toJson(s));
    return out;
}
inline std::vector<SlideNoteModel> fromArray(const nlohmann::json &j) {
    if (!j.is_array())
        throw std::invalid_argument("Invalid slide notes");
    std::vector<SlideNoteModel> out;
    out.reserve(j.size());
    for (const auto &s : j)
        out.push_back(fromJson(s));
    return out;
}
} // namespace daw::slides
