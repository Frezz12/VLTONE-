#include "ai/AiSession.hpp"

#include "EngineController.hpp"

#include <algorithm>

using json = nlohmann::json;

namespace daw::ai {

namespace {

/// The undo entry is named after the request, so the Edit menu says
/// "Undo AI: make a piano part" rather than "Undo AI".
std::string labelFor(const std::string& prompt) {
    std::string line = prompt.substr(0, prompt.find('\n'));
    // Trim, then keep it short enough for a menu item.
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) return "AI";
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    if (line.size() > 48) line = line.substr(0, 45) + "…";
    return "AI: " + line;
}

/// The end of the balanced JSON object starting at `open`, or npos.
///
/// A plain `find('}')` would stop at the first brace inside a string — and a
/// note list is full of them — so this tracks strings and their escapes.
std::size_t objectEnd(const std::string& text, std::size_t open) {
    int depth = 0;
    bool inString = false, escaped = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char ch = text[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') inString = true;
        else if (ch == '{') ++depth;
        else if (ch == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

constexpr std::size_t kMaxCheckpoints = 10;

bool isToolName(const std::string& name) {
    for (const ToolSpec& spec : toolSpecs())
        if (spec.name == name) return true;
    return false;
}

} // namespace

std::vector<ToolCall> toolCallsInText(std::string& text) {
    std::vector<ToolCall> calls;
    std::string kept;
    std::size_t cursor = 0;

    while (true) {
        const std::size_t open = text.find('{', cursor);
        if (open == std::string::npos) break;
        const std::size_t close = objectEnd(text, open);
        if (close == std::string::npos) break;

        const std::string span = text.substr(open, close - open + 1);
        json parsed = json::parse(span, nullptr, /*allow_exceptions=*/false);
        const std::string name =
            parsed.is_object() ? parsed.value("name", std::string()) : std::string();

        if (!name.empty() && isToolName(name)) {
            // The argument object has been seen under all three names, so all
            // three are accepted rather than guessing which model is talking.
            json args = json::object();
            for (const char* key : {"parameters", "arguments", "input"})
                if (parsed.contains(key)) { args = parsed[key]; break; }
            calls.push_back(ToolCall{"text-" + std::to_string(calls.size()), name,
                                     std::move(args), /*fromText=*/true});
            kept += text.substr(cursor, open - cursor);
        } else {
            kept += text.substr(cursor, close - cursor + 1);
        }
        cursor = close + 1;
    }
    if (calls.empty()) return calls;

    kept += text.substr(cursor);
    // Whatever prose surrounded the call is worth keeping; the JSON itself is
    // not something the user should have to read.
    const auto first = kept.find_first_not_of(" \t\r\n");
    text = first == std::string::npos
               ? std::string()
               : kept.substr(first, kept.find_last_not_of(" \t\r\n") - first + 1);
    return calls;
}

AiSession::AiSession(EngineController& controller) : m_controller(controller) {}

void AiSession::setMaxIterations(int iterations) {
    m_maxIterations = std::clamp(iterations, 1, 200);
}

std::string AiSession::systemPrompt() const {
    return ai::systemPrompt(m_controller, m_context);
}

bool AiSession::begin(const std::string& prompt) {
    if (m_running) return false;
    m_running = true;
    m_cancelled = false;
    m_iterations = 0;
    m_lastError.clear();
    m_runStartMessage = m_messages.size();
    // Where the undo stack stood before the assistant touched anything. Taken
    // here rather than held as an UndoStack::Suspend because the run spans
    // network waits the user can keep editing through, and a suspend held that
    // long would silently swallow their edits too.
    m_undoMark = m_controller.undoDepth();
    m_undoLabel = labelFor(prompt);

    m_messages.push_back(Message{Role::User, prompt, {}, {}});

    // Snapshotted before anything runs. Kept only if the run turns out to have
    // changed something, so a conversation of questions costs nothing.
    m_pendingCheckpoint =
        Checkpoint{m_messages.size() - 1, prompt, m_controller.project()};
    return true;
}

AiSession::Step AiSession::applyReply(const ModelReply& reply) {
    if (!m_running) return Step::Finished;

    if (!reply.error.empty()) {
        m_lastError = reply.error;
        finish();
        return Step::Failed;
    }

    // A model that answered with a tool call in its prose has still done the
    // work; recovering it is the difference between the part being written and
    // the user seeing raw JSON in the chat.
    std::string text = reply.text;
    std::vector<ToolCall> calls = reply.calls;
    if (calls.empty()) calls = toolCallsInText(text);

    m_messages.push_back(Message{Role::Assistant, text, calls, {}});

    if (calls.empty()) {
        finish();
        return Step::Finished;
    }
    if (m_cancelled) {
        // Stopped between requests: the calls it just asked for are not run.
        m_messages.back().calls.clear();
        finish();
        return Step::Finished;
    }

    ++m_iterations;
    if (m_iterations > m_maxIterations) {
        m_messages.back().calls.clear();
        m_lastError = "stopped after " + std::to_string(m_maxIterations) +
                      " rounds of tool calls";
        finish();
        return Step::Failed;
    }

    Message results;
    results.role = Role::Tool;
    results.outcomes.reserve(calls.size());
    for (const ToolCall& call : calls) {
        const ToolResult result =
            callTool(m_controller, call.name, call.args, m_context);
        results.outcomes.push_back(ToolOutcome{call.id, call.name, result.ok,
                                               result.toJson(), call.fromText});
    }
    m_messages.push_back(std::move(results));
    return Step::NeedsRequest;
}

void AiSession::cancel() {
    if (m_running) m_cancelled = true;
}

std::vector<Message> AiSession::wireMessages() const {
    if (m_historyLimit == 0 || m_messages.size() <= m_historyLimit)
        return m_messages;

    // Walk back the wanted number of user turns, then take everything from
    // there. Starting anywhere else risks a tool result with no call in front
    // of it, which both providers reject outright.
    std::size_t turns = 0;
    std::size_t from = 0;
    for (std::size_t i = m_messages.size(); i-- > 0;) {
        if (m_messages[i].role != Role::User) continue;
        if (++turns >= m_historyLimit) {
            from = i;
            break;
        }
    }
    if (from == 0) return m_messages;
    return std::vector<Message>(m_messages.begin() + std::ptrdiff_t(from),
                                m_messages.end());
}

void AiSession::addUsage(const Usage& usage) {
    m_usage.inputTokens += usage.inputTokens;
    m_usage.outputTokens += usage.outputTokens;
    m_usage.cachedTokens += usage.cachedTokens;
    m_usage.cacheCreationTokens += usage.cacheCreationTokens;
}

bool AiSession::revertTo(std::size_t messageIndex) {
    if (m_running) return false;
    for (const Checkpoint& point : m_checkpoints) {
        if (point.messageIndex != messageIndex) continue;
        m_controller.restoreProject(point.before, labelFor(point.prompt) +
                                                      " (reverted)");
        return true;
    }
    return false;
}

void AiSession::finish() {
    m_running = false;
    // Whatever the run managed to change becomes one entry, including when it
    // failed or was stopped part way: the user still wants those edits back
    // with a single Ctrl+Z.
    m_controller.collapseUndo(m_undoMark, m_undoLabel);

    if (m_controller.undoDepth() > m_undoMark) {
        m_checkpoints.push_back(std::move(m_pendingCheckpoint));
        // A snapshot of a large project is megabytes; ten deep is enough to
        // undo a session's worth of assistant work without holding the lot.
        if (m_checkpoints.size() > kMaxCheckpoints)
            m_checkpoints.erase(m_checkpoints.begin());
    }
    m_pendingCheckpoint = {};
}

void AiSession::clear() {
    if (m_running) return;
    m_messages.clear();
    m_checkpoints.clear();
    m_usage = {};
    m_runStartMessage = 0;
    m_lastError.clear();
    m_iterations = 0;
}

} // namespace daw::ai
