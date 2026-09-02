#pragma once

#include "collaboration/ProjectCommand.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace daw::collab {

nlohmann::json projectCommandToJson(const ProjectCommand& command);
std::optional<ProjectCommand> projectCommandFromJson(
    const nlohmann::json& value, std::string* error = nullptr);

/// Canonical compact JSON. nlohmann::json uses sorted object keys, so the same
/// command produces byte-identical output on every client.
std::string serializeProjectCommand(const ProjectCommand& command);
std::optional<ProjectCommand> deserializeProjectCommand(
    std::string_view bytes, std::string* error = nullptr);

} // namespace daw::collab
