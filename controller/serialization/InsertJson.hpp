#pragma once

#include "model/Document.hpp"

#include <nlohmann/json_fwd.hpp>

#include <vector>

namespace daw::serialization {

/// The canonical JSON shape for a hosted plugin slot. Projects and portable
/// channel-strip presets both use these helpers so an added slot field cannot
/// silently be saved by one format and dropped by the other.
nlohmann::json insertToJson(const InsertModel& insert);
InsertModel insertFromJson(const nlohmann::json& json);

nlohmann::json insertsToJson(const std::vector<InsertModel>& inserts);
std::vector<InsertModel> insertsFromJson(const nlohmann::json& parent,
                                         const char* key);

} // namespace daw::serialization
