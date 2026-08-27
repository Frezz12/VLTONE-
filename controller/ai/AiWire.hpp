#pragma once

#include "ai/AiSession.hpp"

#include <string>
#include <string_view>
#include <vector>

/// The two providers' wire formats, as pure functions.
///
/// This used to live inside the Qt clients, where it could only be checked by
/// sending a real request. It is JSON in and JSON out — none of it needs a
/// network — so it belongs here, next to the rest of the assistant, where a
/// test can drive it with recorded bytes. `app/LlmClient` is left with the part
/// that genuinely is HTTP: the URL, the headers, and the socket.
namespace daw::ai::wire {

enum class Provider { Anthropic, OpenAi };

/// The request body, in the provider's own schema.
///
/// Anthropic marks the tools and the system prompt as cacheable: they are
/// several kilobytes and identical on every step, and a twenty-step run would
/// otherwise pay for them twenty times over.
/// `vendorExtensions` turns on the fields only the real endpoints understand:
/// Anthropic's `cache_control` and OpenAI's `stream_options`. A third-party
/// server that speaks the same API may reject an unknown field outright and
/// fail the whole request, so anything behind a custom base URL gets the plain
/// body and gives up the caching rather than the connection.
nlohmann::json requestBody(Provider provider, const std::string& model,
                           int maxTokens, const std::string& system,
                           const std::vector<Message>& messages, bool stream,
                           bool vendorExtensions = true);

/// A complete (non-streamed) answer.
ModelReply parseReply(Provider provider, const nlohmann::json& body);

/// What the request cost, when the provider says so.
AiSession::Usage parseUsage(Provider provider, const nlohmann::json& body);

/// The provider's own error message, which is always more use than the
/// transport's. Empty when the body carries none.
std::string errorMessage(Provider provider, const nlohmann::json& body);

/// Server-sent events, decoded as they arrive.
///
/// Both providers stream the same way — `data:` lines separated by blank
/// lines — and differ only in what the events say. Feeding bytes in as the
/// socket delivers them is what lets the panel show an answer being written
/// instead of appearing all at once.
class StreamDecoder {
public:
    explicit StreamDecoder(Provider provider) : m_provider(provider) {}

    /// Feed whatever arrived. True once the stream has ended.
    bool feed(std::string_view bytes);

    /// Everything decoded so far. Complete once `feed` has returned true.
    const ModelReply& reply() const { return m_reply; }
    const AiSession::Usage& usage() const { return m_usage; }

    /// Prose added since the last call, so a caller can append rather than
    /// redraw. Clears what it returns.
    std::string takeText();

    bool done() const { return m_done; }

private:
    void handle(const std::string& eventName, const nlohmann::json& data);
    /// A tool call's arguments arrive as a run of JSON fragments; they are only
    /// parseable once the block closes.
    void closeBlock(int index);

    Provider m_provider;
    std::string m_buffer;       ///< bytes not yet a whole event
    std::string m_pending;      ///< text not yet taken
    ModelReply m_reply;
    AiSession::Usage m_usage;
    bool m_done = false;

    /// Partial tool calls by their index in the stream.
    struct Building {
        std::string id;
        std::string name;
        std::string args;
    };
    std::vector<Building> m_building;
    Building& building(int index);
};

} // namespace daw::ai::wire
