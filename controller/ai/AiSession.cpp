#include "ai/AiSession.hpp"

#include "EngineController.hpp"

#include <algorithm>
#include <set>

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

bool isToolName(const std::string& name, const std::vector<ToolSpec>& tools) {
    for (const ToolSpec& spec : tools)
        if (spec.name == name) return true;
    return false;
}

} // namespace

std::vector<ToolCall> toolCallsInText(std::string& text) {
    return toolCallsInText(text, toolSpecs());
}
std::vector<ToolCall> toolCallsInText(std::string& text, const std::vector<ToolSpec>& tools) {
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

        if (!name.empty() && isToolName(name, tools)) {
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

namespace {
class DawWorkspace final : public AiWorkspace {
public:
    explicit DawWorkspace(EngineController& c) : controller(c) {}
    std::uint64_t revision() const override { return controller.projectRevision(); }
    std::string systemPrompt(const ToolContext& c) const override { return ai::systemPrompt(controller, c); }
    std::vector<ToolSpec> tools(InteractionMode mode) const override { return toolSpecsForMode(mode); }
    std::optional<ToolResult> execute(const ToolCall& call, const ToolContext& c, std::uint64_t) override {
        return callTool(controller, call.name, call.args, c);
    }
    void begin(std::uint64_t, const std::string& prompt, std::size_t index) override {
        group = controller.beginUndoGroup(); label = labelFor(prompt);
        pending = {index, prompt, controller.project()};
    }
    void finish(bool changed, bool interleaved) override {
        if (interleaved) controller.releaseUndoGroup(group);
        else controller.collapseUndo(group, label);
        group = {};
        if (changed && !interleaved) {
            points.push_back(std::move(pending));
            if (points.size() > kMaxCheckpoints) points.erase(points.begin());
        }
        pending = {};
    }
    const std::vector<Checkpoint>& checkpoints() const override { return points; }
    bool revertTo(std::size_t index) override {
        for (const auto& p : points) if (p.messageIndex == index) {
            controller.restoreProject(p.before, labelFor(p.prompt) + " (reverted)"); return true;
        }
        return false;
    }
    void clear() override { points.clear(); }
private:
    EngineController& controller;
    UndoStack::Group group;
    std::string label;
    Checkpoint pending;
    std::vector<Checkpoint> points;
};
}
const std::vector<Checkpoint>& AiWorkspace::checkpoints() const {
    static const std::vector<Checkpoint> empty;
    return empty;
}
AiSession::AiSession(EngineController& controller)
    : m_ownedWorkspace(std::make_unique<DawWorkspace>(controller)), m_workspace(m_ownedWorkspace.get()) {}
AiSession::AiSession(AiWorkspace& workspace) : m_workspace(&workspace) {}
AiSession::~AiSession() { if (m_running) { cancel(); if (m_running) finish(); } }

void AiSession::setMaxIterations(int iterations) {
    m_maxIterations = std::clamp(iterations, 1, 200);
}

std::string AiSession::systemPrompt() const {
    return m_workspace->systemPrompt(m_context);
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
    ++m_runId;
    m_pendingCalls.clear(); m_pendingResults = {}; m_waitingId.clear();
    m_workspace->begin(m_runId, prompt, m_messages.size());
    m_expectedRevision = m_workspace->revision();
    m_interleaved = false;
    m_hadAiEdits = false;

    m_messages.push_back(Message{Role::User, prompt, {}, {}});

    // Snapshotted before anything runs. Kept only if the run turns out to have
    // changed something, so a conversation of questions costs nothing.
    return true;
}

bool AiSession::resume() {
    if (m_running || m_lastError.empty() || m_messages.empty()) return false;
    const auto mode = m_context.mode;
    const bool started = begin("Continue the previous request from the current project state. "
                              "Completed tool results above are already applied; do not repeat them. "
                              "Finish the missing response and any remaining work.");
    m_context.mode = mode;
    return started;
}

AiSession::Step AiSession::applyReply(const ModelReply& reply) {
    if (!m_running) return Step::Finished;
    if (waitingForTool()) return Step::WaitingForTool;

    if (!reply.error.empty()) {
        // Keep partial prose visible, but never recover or execute commands
        // from a response that failed or was interrupted.
        if (!reply.text.empty())
            m_messages.push_back(Message{Role::Assistant, reply.text, {}, {}});
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
        calls = toolCallsInText(text, availableTools());
    std::set<std::string> ids;
    for (const auto& call : calls) if (call.id.empty() || !ids.insert(call.id).second) {
        m_lastError = "Provider returned missing or duplicate tool call IDs";
        finish(); return Step::Failed;
    }

    m_messages.push_back(Message{Role::Assistant, text, calls, {}});

    // The provider worked from an earlier snapshot. If the user edited while
    // it was thinking, return a structured stale result instead of applying a
    // now-mis-targeted call; the next request receives the fresh project and
    // can re-plan. History remains separate for the rest of this run.
    const bool staleAtReply =
        m_workspace->revision() != m_expectedRevision;
    if (staleAtReply) {
        m_interleaved = true;
        m_expectedRevision = m_workspace->revision();
    }

    if (calls.empty()) {
        const auto incomplete = !m_cancelled ? m_workspace->completionIssue() : std::string{};
        if (!incomplete.empty() && !staleAtReply) {
            m_messages.back().text.clear();
            if (++m_iterations > m_maxIterations) {
                m_lastError = "Iteration limit reached. Draft saved; continue to finish compilation and installation.";
                finish(); return Step::Failed;
            }
            Message update; update.role = Role::Tool;
            update.outcomes.push_back({"workspace-incomplete", "workspace_state", false,
                {{"ok", false}, {"error", incomplete}}, true});
            m_messages.push_back(std::move(update)); return Step::NeedsRequest;
        }
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

    m_pendingCalls = std::move(calls);
    m_nextCall = 0;
    m_pendingResults = {};
    m_pendingResults.role = Role::Tool;
    m_staleBatch = staleAtReply;
    return executePending();
}
AiSession::Step AiSession::executePending() {
    while (m_nextCall < m_pendingCalls.size()) {
        const auto& call = m_pendingCalls[m_nextCall];
        if (m_workspace->revision() != m_expectedRevision) {
            m_interleaved = true;
            m_expectedRevision = m_workspace->revision();
            m_staleBatch = true;
        }
        ToolResult result;
        if (m_staleBatch) result.error = kStaleProject;
        else if (!isToolName(call.name, availableTools())) result.error = "Tool is unavailable in this workspace or interaction mode";
        else {
            const auto before = m_workspace->revision();
            std::optional<ToolResult> answer;
            try { answer = m_workspace->execute(call, m_context, m_runId); }
            catch (const std::exception& e) { answer = ToolResult{false, {}, e.what()}; }
            if (m_workspace->revision() != before) m_hadAiEdits = true;
            m_expectedRevision = m_workspace->revision();
            if (!answer) { m_waitingId = call.id; return Step::WaitingForTool; }
            result = std::move(*answer);
        }
        m_pendingResults.outcomes.push_back(ToolOutcome{call.id, call.name, result.ok,
                                               result.toJson(), call.fromText});
        ++m_nextCall;
    }
    m_messages.push_back(std::move(m_pendingResults));
    m_pendingCalls.clear();
    return Step::NeedsRequest;
}
AiSession::Step AiSession::completeTool(std::uint64_t run, const std::string& callId, const ToolResult& result) {
    if (!m_running) return Step::Finished;
    if (run != m_runId || m_waitingId.empty() || callId != m_waitingId)
        return waitingForTool() ? Step::WaitingForTool : Step::NeedsRequest;
    const auto& call = m_pendingCalls[m_nextCall++];
    m_pendingResults.outcomes.push_back({call.id, call.name, result.ok, result.toJson(), call.fromText});
    m_waitingId.clear();
    if (m_expectedRevision != m_workspace->revision()) m_hadAiEdits = true;
    m_expectedRevision = m_workspace->revision();
    return executePending();
}

void AiSession::cancel() {
    if (!m_running) return;
    m_cancelled = true;
    m_workspace->cancel();
    if (waitingForTool()) {
        ToolResult stopped{false, {}, "Stopped. You can continue this request."};
        for (; m_nextCall < m_pendingCalls.size(); ++m_nextCall) {
            const auto& c = m_pendingCalls[m_nextCall];
            m_pendingResults.outcomes.push_back({c.id, c.name, false, stopped.toJson(), c.fromText});
        }
        m_messages.push_back(std::move(m_pendingResults));
        m_waitingId.clear(); m_pendingCalls.clear(); m_lastError = stopped.error;
        finish();
    }
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
    return m_workspace->revertTo(messageIndex);
}

void AiSession::finish() {
    m_running = false;
    // A non-interleaved run becomes one entry, including when it was stopped.
    // Once the user edited during a wait, release the group instead: folding
    // it would silently absorb their work into the assistant's undo.
    m_workspace->finish(m_hadAiEdits, m_interleaved);
}
void AiSession::restoreMessages(std::vector<Message> messages, std::string error, InteractionMode mode) {
    if (m_running) return;
    clear(); m_messages = std::move(messages); m_lastError = std::move(error); m_context.mode = mode;
    // Recover a local transcript saved during an interrupted tool batch.
    for (std::size_t i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].role != Role::Assistant || m_messages[i].calls.empty()) continue;
        if (i + 1 < m_messages.size() && m_messages[i + 1].role == Role::Tool) continue;
        Message interrupted; interrupted.role = Role::Tool;
        for (const auto& c : m_messages[i].calls)
            interrupted.outcomes.push_back({c.id, c.name, false, {{"error", "Previous run was interrupted; inspect current state before retrying."}}, c.fromText});
        m_messages.insert(m_messages.begin() + std::ptrdiff_t(++i), std::move(interrupted));
    }
}

void AiSession::clear() {
    if (m_running) return;
    m_messages.clear();
    m_workspace->clear();
    m_usage = {};
    m_runStartMessage = 0;
    m_lastError.clear();
    m_iterations = 0;
}

} // namespace daw::ai
