#pragma once

#include "ai/Prompts.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace daw {
class EngineController;
}

/// The surface a language model is given over the program.
///
/// Everything here is pure: no Qt, no network, no widgets. A tool call is a
/// name and a JSON object in, a JSON object out, applied to a real
/// `EngineController` — which means the whole agent can be driven from a test
/// with hand-written JSON and never touch a provider. `app/` supplies only the
/// HTTP and the chat window.
///
/// The tool set does not mirror the controller's several hundred methods one
/// for one: a model picks well from a short list of well-named tools and badly
/// from a long one. What is reached for constantly gets a tool of its own; the
/// long tails of near-identical operations — every way to edit a clip, every
/// way to change a mix — are one tool each with an `action`, and the table of
/// actions lives in the `tools-reference` playbook instead of in the schema
/// that is sent on every single request.
namespace daw::ai {

/// One tool as the model sees it. `inputSchema` is JSON Schema, which both
/// providers accept — Anthropic as `input_schema`, OpenAI as
/// `function.parameters`.
struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
};

/// The registry, built once. Stable order, so a cached prompt stays cached.
const std::vector<ToolSpec>& toolSpecs();

/// What a tool call produced. A failure is *not* an exception and not an error
/// the user sees: it goes back to the model as text it can read and retry
/// against, which is most of what makes the feature work at all. So the message
/// names what was wrong and, where it can, what the valid values were.
struct ToolResult {
    bool ok = false;
    nlohmann::json value;   ///< on success; whatever the model needs next
    std::string error;      ///< on failure; a sentence addressed to the model

    /// The single JSON blob handed back as the tool result.
    nlohmann::json toJson() const;
};

/// A file the user dropped into the chat. The model cannot hear it, so it gets
/// the name and the numbers and decides from those.
struct Attachment {
    std::string name;
    std::string path;
    double seconds = 0.0;
    int sampleRate = 0;
    int channels = 0;
};

/// What the user is looking at.
///
/// Without this the model has the whole project and no idea which part of it
/// the user means by "this" or "here" — the commonest way a perfectly good
/// request goes to the wrong track. The playhead and the loop come from the
/// controller; only the selection has to be handed in, because it lives in the
/// UI layer.
struct Focus {
    std::string trackId;
    std::string clipId;
};

/// Everything a tool call needs that is not in the document.
///
/// One struct rather than a growing parameter list, and it keeps this layer
/// Qt-free: the selection and the sample folders both live in the UI, and are
/// handed down rather than reached for.
struct ToolContext {
    Focus focus;
    /// Asked before anything that destroys work — deleting a track, a clip or
    /// an effect. Returning false refuses the call, and the model is told why.
    /// Unset means allow, which is what a test wants and what the panel falls
    /// back to when it has no window to ask in.
    std::function<bool(const std::string& what)> confirmDestructive;
    /// Where `search_files` may look — the browser's folders. Nothing outside
    /// them is ever listed, which is the same promise the browser makes.
    std::vector<std::string> sampleFolders;
    std::vector<Attachment> attachments;
    /// The instructions in force: the main prompt and the playbooks the model
    /// may load. Null means the text compiled into this build, which is what a
    /// test wants and what the panel falls back to when nothing was downloaded.
    const PromptPack* prompts = nullptr;
    /// Where the open project lives on disk, so `save_project` can save it
    /// without the model inventing a path. Empty for a project that has never
    /// been saved — the tool then says so rather than guessing.
    std::string projectPath;
};

/// The pack a context is working from — its own, or the built-in one.
const PromptPack& promptsFor(const ToolContext& context);

/// Run one tool against the document. Never throws: a malformed `args`, an
/// unknown tool name and a rejected value all come back as `ok == false`.
ToolResult callTool(EngineController& controller, const std::string& name,
                    const nlohmann::json& args, const ToolContext& context = {});

/// The project as compact JSON, injected into the prompt so the model does not
/// spend a round trip on `get_project` before every request. Same shape the
/// `get_project` tool returns.
nlohmann::json projectSnapshot(const EngineController& controller,
                               const ToolContext& context = {});

/// The instructions the model works under: the pack's main prompt, the index of
/// playbooks it can load, and the current project and attachments folded in.
std::string systemPrompt(const EngineController& controller,
                         const ToolContext& context = {});

/// Bars are what the tools speak, beats are what the document stores.
/// Bar 1 is the start of the project.
double beatsPerBar(const EngineController& controller);
double barsToSeconds(const EngineController& controller, double bars);
double secondsToBars(const EngineController& controller, double seconds);

} // namespace daw::ai
