#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace daw::serialization {
// The collaboration v7 codec still transports independent, inline entities.
// Keep that wire shape stable while the local document uses a content registry.
// Unsupported shared data must fail explicitly, never be silently flattened.
inline void expandLegacyClipContent(nlohmann::json& root) {
    if (!root.contains("clipContents")) return;
    std::unordered_set<std::string> seen;
    const auto& contents = root.at("clipContents");
    for (auto& track : root["tracks"]) {
        if (track.contains("instrument") && track["instrument"].contains("audioEdit"))
            throw std::runtime_error("sample editor requires a newer collaboration protocol");
        for (auto& clip : track["clips"]) {
            const auto id = clip.at("contentId").get<std::string>();
            if (!seen.insert(id).second || clip.value("contentOffsetBeats", 0.0) != 0.0)
                throw std::runtime_error("linked content requires a newer collaboration protocol");
            const auto& content = contents.at(id);
            if (content.contains("audioEdit"))
                throw std::runtime_error("sample editor requires a newer collaboration protocol");
            const auto destination = clip.value("automation", nlohmann::json::object());
            for (auto item = content.begin(); item != content.end(); ++item) clip[item.key()] = item.value();
            if (destination.contains("target")) clip["automation"]["target"] = destination["target"];
            clip.erase("contentId"); clip.erase("contentOffsetBeats");
            clip.erase("patternParts"); clip.erase("patternPartId");
        }
    }
    root.erase("clipContents");
}
} // namespace daw::serialization
