#pragma once

#include "ai/AiTools.hpp"
#include "model/Document.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw {
class EngineController;
}

namespace daw::ai {

/// One tool the model asked for. `id` is the provider's own call id, which has
/// to be echoed back with the result — both APIs match calls to results by it.
struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json args;
    /// True when the call was recovered from the assistant's prose rather than
    /// arriving through the provider's tool channel. Its result must then go
    /// back as ordinary text: there is no `tool_use` on the provider's side for
    /// a `tool_result` to point at, and referring to one it never sent has the
    /// whole request rejected.
    bool fromText = false;
};

/// What running one of those produced, kept beside the call so the transcript
/// can show the pair and the next request can carry the result back.
struct ToolOutcome {
    std::string callId;
    std::string name;
    bool ok = false;
    nlohmann::json result;
    bool fromText = false;   ///< see `ToolCall::fromText`
};

enum class Role { User, Assistant, Tool };

/// The project as it stood before one request ran.
///
/// A request is one undo entry, but `undo()` only reaches it while it is still
/// on top of the stack — and the user will usually have kept working. A
/// checkpoint reaches any past request at any time.
///
/// It restores the **whole document**, so anything done after that request is
/// undone with it. There is no way around that with a snapshot, and pretending
/// otherwise would be worse than saying it: the UI asks before using one, and
/// the restore is itself a single undo entry, so the way back is one Ctrl+Z.
struct Checkpoint {
    std::size_t messageIndex = 0;   ///< the user turn that started the run
    std::string prompt;
    ProjectModel before;
};

/// A turn in the conversation, in a shape neither provider owns. The clients in
/// `app/` translate this into their own wire format and back.
struct Message {
    Role role = Role::User;
    std::string text;                     ///< prose, for User and Assistant
    std::vector<ToolCall> calls;          ///< Assistant only
    std::vector<ToolOutcome> outcomes;    ///< Tool only
};

/// What a provider answered with. `error` set means the request itself failed
/// (no network, bad key, a refusal) and the run ends.
struct ModelReply {
    std::string text;
    std::vector<ToolCall> calls;
    std::string error;
};

/// Tool calls a model wrote into its prose instead of using the tool channel.
///
/// Not every model reaches for the native channel every time; some answer with
/// `{"name": "...", "parameters": {...}}` as text, and dropping that on the
/// floor loses work the model has already done. Only an object whose `name` is
/// a tool that actually exists is taken, so ordinary prose containing JSON is
/// left alone. The spans that were taken are cut from `text`.
std::vector<ToolCall> toolCallsInText(std::string& text);

/// The agent loop, as pure state.
///
/// It holds the conversation, runs the tool calls against the document, and
/// says whether another request has to go out — but it never speaks HTTP. That
/// is what lets the whole loop be driven from a test with scripted replies, and
/// it is where the undo bookkeeping lives: a run marks the undo stack when it
/// begins and folds everything it did into one entry when it ends, so one
/// request is one Ctrl+Z.
class AiSession {
public:
    explicit AiSession(EngineController& controller);

    AiSession(const AiSession&) = delete;
    AiSession& operator=(const AiSession&) = delete;

    /// How many times the model may come back asking for more tools before the
    /// run is cut off. The guard against a loop that spends money forever.
    void setMaxIterations(int iterations);
    int maxIterations() const { return m_maxIterations; }

    /// Everything the tools need that is not in the document: the selection,
    /// the sample folders, the attached files. Set before each run — the
    /// selection moves while the user types.
    void setContext(ToolContext context) { m_context = std::move(context); }
    const ToolContext& context() const { return m_context; }

    /// Open a turn with the user's request. False when one is already running.
    bool begin(const std::string& prompt);

    enum class Step {
        NeedsRequest,  ///< tools ran; send the conversation again
        Finished,      ///< the model is done, or the user stopped it
        Failed,        ///< the request errored, or the cap was reached
    };

    /// Feed a provider's answer in. Runs whatever tools it asked for against the
    /// document, appends both to the transcript, and reports what happens next.
    Step applyReply(const ModelReply& reply);

    /// Stop after the step in flight. The edits already made stay, and still
    /// collapse into the single undo entry.
    void cancel();

    bool running() const { return m_running; }
    bool cancelled() const { return m_cancelled; }
    int iterations() const { return m_iterations; }

    const std::vector<Message>& messages() const { return m_messages; }

    /// What actually goes on the wire: the recent turns, with older ones
    /// dropped. The transcript the user reads keeps everything; the request
    /// does not, or a long session pays for its whole history on every step.
    ///
    /// Cut on a user turn, so a request never begins with an orphaned tool
    /// result — a `tool_result` whose `tool_use` was trimmed away has the whole
    /// request rejected.
    std::vector<Message> wireMessages() const;

    /// How many turns back a request carries. 0 = everything.
    void setHistoryLimit(std::size_t turns) { m_historyLimit = turns; }
    /// Everything since the current run began, for a panel that renders as it goes.
    std::size_t runStartMessage() const { return m_runStartMessage; }

    /// Rebuilt from the live document on every call — the project the model is
    /// told about must be the one its last tool call left behind.
    std::string systemPrompt() const;

    const std::string& lastError() const { return m_lastError; }

    /// What the last request cost, as the provider reported it. Zero when the
    /// provider said nothing.
    struct Usage {
        std::uint64_t inputTokens = 0;
        std::uint64_t outputTokens = 0;
        std::uint64_t cachedTokens = 0;   ///< read from cache, billed cheaply
        std::uint64_t cacheCreationTokens = 0;
    };
    void addUsage(const Usage& usage);
    const Usage& usage() const { return m_usage; }

    /// One per request that changed anything, oldest first. Capped, because a
    /// snapshot of a big project is not small.
    const std::vector<Checkpoint>& checkpoints() const { return m_checkpoints; }

    /// Put the project back to how it was before the request that started at
    /// `messageIndex`, as one undo entry. Everything done since goes with it —
    /// see `Checkpoint`. False when there is no such checkpoint, or a run is in
    /// flight.
    bool revertTo(std::size_t messageIndex);

    /// Drop the conversation. Does nothing while a run is in flight.
    void clear();

private:
    /// Close the run and fold every edit it made into one undo entry.
    void finish();

    EngineController& m_controller;
    std::vector<Message> m_messages;
    ToolContext m_context;
    std::vector<Checkpoint> m_checkpoints;
    Checkpoint m_pendingCheckpoint;

    bool m_running = false;
    bool m_cancelled = false;
    int m_iterations = 0;
    int m_maxIterations = 24;
    std::size_t m_runStartMessage = 0;
    std::size_t m_undoMark = 0;
    std::string m_undoLabel;
    std::string m_lastError;
    Usage m_usage;
    std::size_t m_historyLimit = 12;
};

} // namespace daw::ai
