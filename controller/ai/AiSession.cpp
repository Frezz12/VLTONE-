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

constexpr const char* kStaleProject =
    "the project changed while this request was being planned; inspect the "
    "fresh CURRENT PROJECT and retry against its current ids and selection";

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
    m_context.mode = inferInteractionMode(prompt);
    m_running = true;
    m_cancelled = false;
    m_iterations = 0;
    m_lastError.clear();
    m_runStartMessage = m_messages.size();
    // A stable undo group survives a full history stack. It is collapsed only
    // if revision tracking proves no user edit landed during the network waits.
    m_undoGroup = m_controller.beginUndoGroup();
    m_expectedRevision = m_controller.projectRevision();
    m_interleaved = false;
    m_hadAiEdits = false;
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
    if (calls.empty() && m_context.mode != InteractionMode::Help &&
        m_context.mode != InteractionMode::Teach)
        calls = toolCallsInText(text);

    m_messages.push_back(Message{Role::Assistant, text, calls, {}});

    // The provider worked from an earlier snapshot. If the user edited while
    // it was thinking, return a structured stale result instead of applying a
    // now-mis-targeted call; the next request receives the fresh project and
    // can re-plan. History remains separate for the rest of this run.
    const bool staleAtReply =
        m_controller.projectRevision() != m_expectedRevision;
    if (staleAtReply) {
        m_interleaved = true;
        m_expectedRevision = m_controller.projectRevision();
    }

    if (calls.empty()) {
        if (staleAtReply && !m_cancelled) {
            ++m_iterations;
            if (m_iterations > m_maxIterations) {
                m_lastError = "the project kept changing while the assistant "
                              "was answering";
                finish();
                return Step::Failed;
            }
            // Do not display prose based on a snapshot we already know is old.
            m_messages.back().text.clear();
            ToolResult stale;
            stale.error = kStaleProject;
            Message update;
            update.role = Role::Tool;
            update.outcomes.push_back(ToolOutcome{
                "stale-project", "project_state", false, stale.toJson(),
                /*fromText=*/true});
            m_messages.push_back(std::move(update));
            return Step::NeedsRequest;
        }
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
    bool staleBatch = staleAtReply;
    for (const ToolCall& call : calls) {
        if (m_controller.projectRevision() != m_expectedRevision) {
            m_interleaved = true;
            m_expectedRevision = m_controller.projectRevision();
            staleBatch = true;
        }
        if (staleBatch) {
            ToolResult stale;
            stale.error = kStaleProject;
            results.outcomes.push_back(ToolOutcome{
                call.id, call.name, false, stale.toJson(), call.fromText});
            continue;
        }
        const std::uint64_t before = m_controller.projectRevision();
        const ToolResult result =
            callTool(m_controller, call.name, call.args, m_context);
        const std::uint64_t after = m_controller.projectRevision();
        if (after != before) m_hadAiEdits = true;
        m_expectedRevision = after;
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
    // A non-interleaved run becomes one entry, including when it was stopped.
    // Once the user edited during a wait, release the group instead: folding
    // it would silently absorb their work into the assistant's undo.
    if (m_interleaved)
        m_controller.releaseUndoGroup(m_undoGroup);
    else
        m_controller.collapseUndo(m_undoGroup, m_undoLabel);
    m_undoGroup = {};

    if (m_hadAiEdits && !m_interleaved) {
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
