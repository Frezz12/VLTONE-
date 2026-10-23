#pragma once
#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <string>

namespace vlt::mcp {
using Json = nlohmann::json;
inline constexpr std::size_t maxMessageBytes = 8 * 1024 * 1024;
inline constexpr auto modernVersion = "2026-07-28";
inline constexpr auto legacyVersion = "2025-11-25";

// One transport connection; modern request metadata never inherits legacy state.
class Protocol {
public:
    using Dispatch = std::function<Json(const std::string &, const Json &)>;
    using Cancel = std::function<void(const Json &)>;
    std::optional<Json> receive(const Json &, const Dispatch &, const Cancel & = {});
    std::string clientName;
private:
    bool initialized = false, ready = false;
    std::string negotiatedVersion;
};
Json error(const Json &id, int code, const std::string &message, Json data = nullptr);
Json toolResult(bool ok, Json value, const std::string &message = {});
// Validates the JSON Schema vocabulary used by the application's tool registries.
void validate(const Json &schema, const Json &value, const std::string &path = "arguments");
}
