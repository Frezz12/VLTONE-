#include "McpProtocol.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vlt::mcp {
Json error(const Json &id, int code, const std::string &message, Json data) {
    Json e{{"code", code}, {"message", message}};
    if (!data.is_null()) e["data"] = std::move(data);
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(e)}};
}
Json toolResult(bool ok, Json value, const std::string &message) {
    Json out = value.is_object() ? std::move(value) : Json{{"value", std::move(value)}};
    out["ok"] = ok;
    if (!ok) out["error"] = message;
    return {{"isError", !ok}, {"structuredContent", out},
            {"content", Json::array({{{"type", "text"}, {"text", out.dump()}}})}};
}

void validate(const Json &s, const Json &v, const std::string &path) {
    const auto fail = [&](const std::string &why) { throw std::invalid_argument(path + ": " + why); };
    const auto type = s.value("type", "");
    if ((type == "object" && !v.is_object()) || (type == "array" && !v.is_array()) ||
        (type == "string" && !v.is_string()) || (type == "boolean" && !v.is_boolean()) ||
        (type == "integer" && !v.is_number_integer()) || (type == "number" && !v.is_number()))
        fail("expected " + type);
    if (s.contains("enum") && std::find(s["enum"].begin(), s["enum"].end(), v) == s["enum"].end())
        fail("value is not in enum");
    if (v.is_number()) {
        const double n = v.get<double>();
        if (!std::isfinite(n)) fail("number must be finite");
        if (s.contains("minimum") && n < s["minimum"].get<double>()) fail("below minimum");
        if (s.contains("maximum") && n > s["maximum"].get<double>()) fail("above maximum");
    }
    if (v.is_object()) {
        for (const auto &key : s.value("required", Json::array()))
            if (!v.contains(key.get<std::string>())) fail("missing " + key.get<std::string>());
        const auto props = s.value("properties", Json::object());
        for (const auto &[key, item] : v.items()) {
            if (props.contains(key)) validate(props[key], item, path + "." + key);
            else if (s.contains("additionalProperties") && s["additionalProperties"] == false)
                fail("unknown field " + key);
        }
    }
    if (v.is_array()) {
        if (v.size() > s.value("maxItems", std::size_t(65536))) fail("too many items");
        if (s.contains("items"))
            for (const auto &item : v) validate(s["items"], item, path + "[]");
    }
    if (v.is_string() && v.get_ref<const std::string &>().size() > s.value("maxLength", maxMessageBytes))
        fail("string exceeds limit");
}

std::optional<Json> Protocol::receive(const Json &request, const Dispatch &dispatch, const Cancel &cancel) {
    Json id = request.is_object() ? request.value("id", Json()) : Json();
    if (!request.is_object() || request.value("jsonrpc", Json()) != "2.0" ||
        !request.contains("method") || !request["method"].is_string() ||
        (request.contains("id") && !id.is_string() && !id.is_number_integer()) ||
        (request.contains("params") && !request["params"].is_object()))
        return error(nullptr, -32600, "Invalid JSON-RPC request");
    const auto method = request["method"].get<std::string>();
    const auto params = request.value("params", Json::object());
    if (!request.contains("id")) {
        if (method == "notifications/initialized" && initialized) ready = true;
        if (method == "notifications/cancelled" && cancel && params.contains("requestId"))
            cancel(params["requestId"]);
        return std::nullopt;
    }
    const auto meta = params.value("_meta", Json::object());
    if (!meta.is_object()) return error(id, -32602, "_meta must be an object");
    const auto version = meta.value("io.modelcontextprotocol/protocolVersion", Json());
    const bool modern = !version.is_null();
    const Json versions = {modernVersion, legacyVersion, "2025-06-18"};
    const Json server{{"name", "VLTONE"}, {"version", "1.0.0"}};
    const Json capabilities{{"tools", Json::object()}, {"resources", Json::object()}};
    const std::string instructions =
        "Control the running VLTONE app. Start with workspace_projects action=list. "
        "Pass explicit projectId and generation; use the current revision for edits. "
        "Creator/Lua calls also require documentId and their document revision. "
        "Jobs return jobId: poll get_job until complete. Edits are visible and undoable. "
        "Read SDK resources before authoring code. Never claim a build/render succeeded before its job does.";
    if (modern) {
        if (version != modernVersion)
            return error(id, -32022, "Unsupported protocol version", {{"requested", version}, {"supported", versions}});
        if (!meta.contains("io.modelcontextprotocol/clientCapabilities") ||
            !meta["io.modelcontextprotocol/clientCapabilities"].is_object())
            return error(id, -32602, "Missing clientCapabilities metadata");
        if (meta.contains("io.modelcontextprotocol/clientInfo")) {
            const auto &info = meta["io.modelcontextprotocol/clientInfo"];
            if (!info.is_object() || !info.value("name", Json()).is_string())
                return error(id, -32602, "Invalid clientInfo");
            clientName = info["name"].get<std::string>().substr(0, 128);
        }
    }
    Json result;
    try {
        if (method == "initialize" && !modern) {
            if (initialized) return error(id, -32600, "Already initialized");
            const auto preferred = params.at("protocolVersion").get<std::string>();
            negotiatedVersion = preferred == "2025-06-18" ? preferred : legacyVersion;
            if (!params.at("capabilities").is_object()) throw std::invalid_argument("Invalid capabilities");
            clientName = params.at("clientInfo").at("name").get<std::string>().substr(0, 128);
            initialized = true;
            result = {{"protocolVersion", negotiatedVersion}, {"capabilities", capabilities},
                      {"serverInfo", server}, {"instructions", instructions}};
        } else if (method == "server/discover") {
            if (!modern) return error(id, -32602, "server/discover requires modern request metadata");
            result = {{"supportedVersions", versions}, {"capabilities", capabilities},
                      {"_meta", {{"io.modelcontextprotocol/serverInfo", server}}}, {"instructions", instructions}};
        } else {
            if (!modern && !ready) return error(id, -32600, "Initialize the connection first");
            if (method == "ping") result = Json::object();
            else if (method == "tools/list" || method == "tools/call" ||
                     method == "resources/list" || method == "resources/read" || method == "resources/templates/list")
                result = dispatch(method, params);
            else return error(id, -32601, "Method not found");
        }
    } catch (const std::exception &e) {
        return error(id, -32602, e.what());
    }
    if (modern) result["resultType"] = "complete";
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}
}
