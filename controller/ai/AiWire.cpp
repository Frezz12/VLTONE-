#include "ai/AiWire.hpp"

#include "ai/AiTools.hpp"

#include <algorithm>

using json = nlohmann::json;

namespace daw::ai::wire {

namespace {

/// A result recovered from prose has no `tool_use` on the provider's side, so
/// its result must go back as ordinary text. Referring to a call id the
/// provider never issued has the whole request rejected.
std::string proseResult(const ToolOutcome& out) {
    return "Result of " + out.name + ": " + out.result.dump() + "\n";
}

constexpr const char* kUseTheInterface =
    "(Use the tool interface rather than writing calls as text.)";

json anthropicBody(const std::string& model, int maxTokens,
                   const std::string& system,
                   const std::vector<Message>& messages, bool stream,
                   bool vendorExtensions) {
    json tools = json::array();
    for (const ToolSpec& spec : toolSpecs())
        tools.push_back(json{{"name", spec.name},
                             {"description", spec.description},
                             {"input_schema", spec.inputSchema}});
    if (vendorExtensions && !tools.empty())
        tools.back()["cache_control"] = json{{"type", "ephemeral"}};

    json wire = json::array();
    for (const Message& message : messages) {
        switch (message.role) {
            case Role::User:
                wire.push_back(json{{"role", "user"}, {"content", message.text}});
                break;

            case Role::Assistant: {
                json content = json::array();
                if (!message.text.empty())
                    content.push_back(json{{"type", "text"}, {"text", message.text}});
                for (const ToolCall& call : message.calls) {
                    if (call.fromText) continue;
                    content.push_back(
                        json{{"type", "tool_use"},
                             {"id", call.id},
                             {"name", call.name},
                             {"input", call.args.is_object() ? call.args
                                                             : json::object()}});
                }
                // An assistant turn with no content at all is rejected outright,
                // and a stopped run can leave exactly that behind.
                if (content.empty()) continue;
                wire.push_back(json{{"role", "assistant"}, {"content", content}});
                break;
            }

            case Role::Tool: {
                // Results are a *user* turn here — that is the shape the API
                // wants, however odd it reads.
                json content = json::array();
                std::string prose;
                for (const ToolOutcome& out : message.outcomes) {
                    if (out.fromText) {
                        prose += proseResult(out);
                        continue;
                    }
                    content.push_back(json{{"type", "tool_result"},
                                           {"tool_use_id", out.callId},
                                           {"is_error", !out.ok},
                                           {"content", out.result.dump()}});
                }
                if (!prose.empty())
                    content.push_back(json{{"type", "text"},
                                           {"text", prose + kUseTheInterface}});
                if (content.empty()) continue;
                wire.push_back(json{{"role", "user"}, {"content", content}});
                break;
            }
        }
    }

    json systemField = system;
    if (vendorExtensions)
        systemField = json::array({json{{"type", "text"},
                                        {"text", system},
                                        {"cache_control",
                                         json{{"type", "ephemeral"}}}}});

    json body{{"model", model},
              {"max_tokens", maxTokens},
              {"system", std::move(systemField)},
              {"tools", std::move(tools)},
              {"messages", std::move(wire)}};
    if (stream) body["stream"] = true;
    return body;
}

json openAiBody(const std::string& model, const std::string& system,
                const std::vector<Message>& messages, bool stream,
                bool vendorExtensions) {
    json tools = json::array();
    for (const ToolSpec& spec : toolSpecs())
        tools.push_back(json{{"type", "function"},
                             {"function", json{{"name", spec.name},
                                               {"description", spec.description},
                                               {"parameters", spec.inputSchema}}}});

    json wire = json::array();
    wire.push_back(json{{"role", "system"}, {"content", system}});

    for (const Message& message : messages) {
        switch (message.role) {
            case Role::User:
                wire.push_back(json{{"role", "user"}, {"content", message.text}});
                break;

            case Role::Assistant: {
                json turn{{"role", "assistant"}, {"content", message.text}};
                json calls = json::array();
                for (const ToolCall& call : message.calls) {
                    if (call.fromText) continue;
                    // Arguments travel as a *string* of JSON here, not as an
                    // object — the one real difference from Anthropic's shape.
                    calls.push_back(json{{"id", call.id},
                                         {"type", "function"},
                                         {"function",
                                          json{{"name", call.name},
                                               {"arguments", call.args.dump()}}}});
                }
                if (!calls.empty()) turn["tool_calls"] = std::move(calls);
                wire.push_back(std::move(turn));
                break;
            }

            case Role::Tool: {
                std::string prose;
                for (const ToolOutcome& out : message.outcomes) {
                    if (out.fromText) {
                        prose += proseResult(out);
                        continue;
                    }
                    wire.push_back(json{{"role", "tool"},
                                        {"tool_call_id", out.callId},
                                        {"content", out.result.dump()}});
                }
                if (!prose.empty())
                    wire.push_back(json{{"role", "user"},
                                        {"content", prose + kUseTheInterface}});
                break;
            }
        }
    }

    json body{{"model", model},
              {"messages", std::move(wire)},
              {"tools", std::move(tools)}};
    if (stream) {
        body["stream"] = true;
        // Without this OpenAI reports no usage at all for a streamed request —
        // but a compatible server may not know the field, and losing the token
        // count is better than losing the request.
        if (vendorExtensions)
            body["stream_options"] = json{{"include_usage", true}};
    }
    return body;
}

/// Arguments that arrived as a string of JSON. A model that emitted something
/// malformed should hear about it from the tool layer, so the raw string is
/// passed through rather than dropped.
json argsFromString(const std::string& raw) {
    if (raw.empty()) return json::object();
    json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    return parsed.is_discarded() ? json(raw) : parsed;
}

} // namespace

json requestBody(Provider provider, const std::string& model, int maxTokens,
                 const std::string& system, const std::vector<Message>& messages,
                 bool stream, bool vendorExtensions) {
    return provider == Provider::Anthropic
               ? anthropicBody(model, maxTokens, system, messages, stream,
                               vendorExtensions)
               : openAiBody(model, system, messages, stream, vendorExtensions);
}

ModelReply parseReply(Provider provider, const json& body) {
    ModelReply reply;
    if (provider == Provider::Anthropic) {
        for (const json& block : body.value("content", json::array())) {
            const std::string type = block.value("type", "");
            if (type == "text") {
                reply.text += block.value("text", "");
            } else if (type == "tool_use") {
                reply.calls.push_back({block.value("id", ""),
                                       block.value("name", ""),
                                       block.value("input", json::object())});
            }
        }
        return reply;
    }

    const json& choices = body.value("choices", json::array());
    if (choices.empty()) {
        reply.error = "the model answered with no choices";
        return reply;
    }
    const json& message = choices[0].value("message", json::object());
    if (message.contains("content") && message["content"].is_string())
        reply.text = message["content"].get<std::string>();
    for (const json& call : message.value("tool_calls", json::array())) {
        const json& fn = call.value("function", json::object());
        reply.calls.push_back({call.value("id", ""), fn.value("name", ""),
                               argsFromString(fn.value("arguments", ""))});
    }
    return reply;
}

AiSession::Usage parseUsage(Provider provider, const json& body) {
    const json& usage = body.value("usage", json::object());
    if (provider == Provider::Anthropic)
        return {usage.value("input_tokens", std::uint64_t(0)),
                usage.value("output_tokens", std::uint64_t(0)),
                usage.value("cache_read_input_tokens", std::uint64_t(0)),
                usage.value("cache_creation_input_tokens", std::uint64_t(0))};

    const json& details = usage.value("prompt_tokens_details", json::object());
    return {usage.value("prompt_tokens", std::uint64_t(0)),
            usage.value("completion_tokens", std::uint64_t(0)),
            details.value("cached_tokens", std::uint64_t(0)), 0};
}

std::string errorMessage(Provider, const json& body) {
    // Providers use error.message. The VLT proxy deliberately has a flatter
    // APIError envelope so account/quota/configuration failures are readable
    // by every desktop client as well.
    if (body.contains("error") && body["error"].is_object())
        return body["error"].value("message", std::string());
    if (body.contains("message") && body["message"].is_string())
        return body["message"].get<std::string>();
    return {};
}

// ── Streaming ───────────────────────────────────────────────────────────────

StreamDecoder::Building& StreamDecoder::building(int index) {
    if (index < 0) index = 0;
    if (std::size_t(index) >= m_building.size())
        m_building.resize(std::size_t(index) + 1);
    return m_building[std::size_t(index)];
}

std::string StreamDecoder::takeText() {
    std::string out;
    out.swap(m_pending);
    return out;
}

bool StreamDecoder::feed(std::string_view bytes) {
    m_buffer.append(bytes);

    // Events are separated by a blank line. Anything after the last one is a
    // partial event and stays in the buffer for the next chunk.
    std::size_t start = 0;
    while (true) {
        const std::size_t end = m_buffer.find("\n\n", start);
        if (end == std::string::npos) break;
        const std::string block = m_buffer.substr(start, end - start);
        start = end + 2;

        std::string eventName;
        std::string data;
        std::size_t lineStart = 0;
        while (lineStart <= block.size()) {
            std::size_t lineEnd = block.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = block.size();
            std::string line = block.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.rfind("event:", 0) == 0) {
                eventName = line.substr(6);
            } else if (line.rfind("data:", 0) == 0) {
                // Multiple data: lines in one event concatenate.
                if (!data.empty()) data += "\n";
                data += line.substr(5);
            }
        }
        const auto trim = [](std::string& text) {
            const auto first = text.find_first_not_of(" \t");
            if (first == std::string::npos) { text.clear(); return; }
            text = text.substr(first, text.find_last_not_of(" \t") - first + 1);
        };
        trim(eventName);
        trim(data);
        if (data.empty()) continue;

        // OpenAI ends with a literal sentinel rather than an event.
        if (data == "[DONE]") {
            m_done = true;
            continue;
        }
        json parsed = json::parse(data, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded()) handle(eventName, parsed);
    }
    m_buffer.erase(0, start);
    return m_done;
}

void StreamDecoder::closeBlock(int index) {
    if (index < 0 || std::size_t(index) >= m_building.size()) return;
    Building& block = m_building[std::size_t(index)];
    if (block.name.empty()) return;
    m_reply.calls.push_back(
        {block.id, block.name, argsFromString(block.args)});
    block = {};
}

void StreamDecoder::handle(const std::string& eventName, const json& data) {
    if (m_provider == Provider::Anthropic) {
        const std::string type =
            eventName.empty() ? data.value("type", std::string()) : eventName;

        if (type == "message_start") {
            m_usage = parseUsage(m_provider, data.value("message", json::object()));
        } else if (type == "content_block_start") {
            const json& block = data.value("content_block", json::object());
            if (block.value("type", "") == "tool_use") {
                Building& made = building(data.value("index", 0));
                made.id = block.value("id", "");
                made.name = block.value("name", "");
            }
        } else if (type == "content_block_delta") {
            const json& delta = data.value("delta", json::object());
            const std::string kind = delta.value("type", "");
            if (kind == "text_delta") {
                const std::string chunk = delta.value("text", "");
                m_reply.text += chunk;
                m_pending += chunk;
            } else if (kind == "input_json_delta") {
                building(data.value("index", 0)).args +=
                    delta.value("partial_json", "");
            }
        } else if (type == "content_block_stop") {
            closeBlock(data.value("index", 0));
        } else if (type == "message_delta") {
            // The output count only arrives at the end, and it is the half
            // that costs the most.
            const json& usage = data.value("usage", json::object());
            m_usage.outputTokens += usage.value("output_tokens", std::uint64_t(0));
        } else if (type == "message_stop") {
            m_done = true;
        } else if (type == "error") {
            m_reply.error = errorMessage(m_provider, data);
            if (m_reply.error.empty()) m_reply.error = "the stream failed";
            m_done = true;
        }
        return;
    }

    // ── OpenAI ──
    if (data.contains("error")) {
        m_reply.error = errorMessage(m_provider, data);
        if (m_reply.error.empty()) m_reply.error = "the stream failed";
        m_done = true;
        return;
    }
    if (data.contains("usage") && data["usage"].is_object())
        m_usage = parseUsage(m_provider, data);

    for (const json& choice : data.value("choices", json::array())) {
        const json& delta = choice.value("delta", json::object());
        if (delta.contains("content") && delta["content"].is_string()) {
            const std::string chunk = delta["content"].get<std::string>();
            m_reply.text += chunk;
            m_pending += chunk;
        }
        for (const json& call : delta.value("tool_calls", json::array())) {
            Building& made = building(call.value("index", 0));
            if (call.contains("id")) made.id = call.value("id", "");
            const json& fn = call.value("function", json::object());
            if (fn.contains("name")) made.name = fn.value("name", "");
            made.args += fn.value("arguments", "");
        }
        // A finish reason means every block is complete; OpenAI has no
        // per-block stop event.
        if (choice.contains("finish_reason") &&
            !choice["finish_reason"].is_null()) {
            for (std::size_t i = 0; i < m_building.size(); ++i)
                closeBlock(int(i));
        }
    }
}

} // namespace daw::ai::wire
