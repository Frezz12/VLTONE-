#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

/// The instructions the assistant works under, as data rather than as code.
///
/// The main prompt says how to operate the program; a *playbook* says how to do
/// one kind of work — write a bass part, program a beat, process a vocal, mix.
/// The main prompt carries only an index of them, and the model loads the one it
/// needs with `get_playbook`. That keeps the always-sent prompt short, keeps it
/// byte-identical between the steps of a run (so the provider's cache holds),
/// and makes "teach it to write better bass" an edit to one document rather
/// than a change to this program.
///
/// Three sources, in this order of preference: a pack downloaded from the
/// server, the last one cached on disk, and the built-in text compiled in from
/// `prompts/`. The last of those is why the assistant still works on a first run
/// with no network. Nothing here knows about any of that plumbing — it is
/// parsing, validation and lookup, and `app/PromptService` does the fetching.
namespace daw::ai {

/// One document. `useWhen` is the single line the model reads in the index to
/// decide whether this is the playbook it wants, so it matters as much as the
/// body does.
struct Playbook {
    std::string id;
    std::string title;
    std::string useWhen;
    std::string body;
    std::vector<std::string> tags;
};

struct PromptPack {
    /// Identifies the text, not the schema: it goes back to the server as an
    /// ETag and is shown in Settings so a user can say which one they are on.
    std::string version;
    std::string main;
    std::vector<Playbook> playbooks;

    bool empty() const { return main.empty() && playbooks.empty(); }
};

/// The text compiled into this build, generated from `prompts/` by
/// `scripts/gen_prompts.py`. Never empty.
const PromptPack& builtinPrompts();

/// Limits a served pack has to stay inside. A prompt is sent on every request
/// and paid for by the token: a server that has been tampered with, or an admin
/// who pasted a novel, must not be able to make every request enormous.
inline constexpr std::size_t kMaxMainPromptBytes = 64 * 1024;
inline constexpr std::size_t kMaxPlaybookBytes = 64 * 1024;
inline constexpr std::size_t kMaxPlaybooks = 128;

/// Read a pack the server sent.
///
/// Returns an empty pack and sets `error` when the JSON is not a pack, an id is
/// malformed, a body is empty or something is over the limits above — the
/// caller then keeps whatever it was already using. Never throws.
PromptPack parsePromptPack(const nlohmann::json& body, std::string* error);

/// The same shape `parsePromptPack` reads, for the on-disk cache.
nlohmann::json toJson(const PromptPack& pack);

/// The lines that go into the system prompt under PLAYBOOKS: one per playbook,
/// `id — useWhen`. Empty when the pack has none.
std::string playbookIndex(const PromptPack& pack);

const Playbook* findPlaybook(const PromptPack& pack, std::string_view id);

/// Every id, in pack order — what a failed lookup tells the model to choose
/// from.
std::vector<std::string> playbookIds(const PromptPack& pack);

} // namespace daw::ai
