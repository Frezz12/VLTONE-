#pragma once

#include "ai/AiTools.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace daw {
class EngineController;
}

/// Text-to-music generation, as pure data.
///
/// The second mode of the assistant panel does not run an agent: one request
/// goes to a music model (MiniMax Music and anything that speaks its shape) and
/// one audio file comes back. Everything about that exchange that is not a
/// socket lives here — the brief the project is turned into, the request body,
/// and the reply parser — so all of it is testable with hand-written JSON and
/// no network at all, exactly like `AiWire`.
///
/// Deliberately **no language model in the loop**. The brief is read off the
/// document, so the mode works with nothing configured but a music endpoint.
namespace daw::ai {

/// What is asked of the music model.
struct MusicBrief {
    std::string prompt;       ///< style, mood and the project's musical facts
    std::string lyrics;       ///< empty = let the model write them
    bool instrumental = false;
    /// How long the result should be, when the project implies a length (a
    /// loop is set). Zero means "the model decides". Advisory: MiniMax has no
    /// duration field, so this only reaches the model as words in `prompt`.
    double seconds = 0.0;
};

/// Turn the user's sentence plus the open project into a brief.
///
/// This is the whole of what "it understands the context" means in this mode:
/// tempo, time signature, key, what tracks already exist and which one is
/// selected all go into the prompt, so the generated audio arrives in the same
/// tempo and key as the session rather than needing to be fought into it.
///
/// Lyrics: a request that contains a bracketed section line (`[Verse]`,
/// `[Chorus]`, …) is split — the prose before the first such line is the style,
/// the rest is taken verbatim as lyrics. Anything else leaves `lyrics` empty.
MusicBrief buildBrief(const EngineController& controller,
                      const std::string& request, const ToolContext& context,
                      bool instrumental);

/// The request body, in MiniMax's `music_generation` shape.
nlohmann::json requestJson(const MusicBrief& brief, const std::string& model,
                           const std::string& format, int sampleRate,
                           int bitrate);

/// What came back. Exactly one of `audioUrl` / `audioBytes` is set on success.
struct MusicResult {
    std::string audioUrl;     ///< a link to fetch
    std::string audioBytes;   ///< the audio itself, already decoded
    std::string format;       ///< "mp3", "wav", … when the reply named one
    double seconds = 0.0;
    std::string error;        ///< set = the request failed; a sentence to show
};

/// Read a reply, forgivingly.
///
/// The endpoint is the user's own server, so the exact shape is not
/// guaranteed: the audio is looked for at `data.audio` first and then at the
/// other names these APIs use, and the string is recognised as a URL, as hex or
/// as base64 by its own contents rather than by a field saying which it is.
/// Never throws — a reply that makes no sense comes back as `error`.
MusicResult parseReply(const nlohmann::json& reply);

/// A file name stem for a request: lower-case, words joined by dashes, cut
/// short. Shared by the client (naming the download) and the panel (naming the
/// track), so both agree.
std::string slug(const std::string& text, std::size_t maxLength = 40);

} // namespace daw::ai
