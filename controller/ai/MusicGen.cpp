#include "ai/MusicGen.hpp"

#include "EngineController.hpp"
#include "MidiTools.hpp"
#include "model/Document.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace daw::ai {

namespace {

/// MiniMax caps the style prompt; the exact number is not the point, but a
/// prompt that grows with the project would eventually be refused, so the
/// musical facts are kept short and the whole thing is clamped.
constexpr std::size_t kMaxPrompt = 300;
constexpr std::size_t kMaxTracksNamed = 5;
/// Below this an inline "audio" field is prose that happened to decode, not a
/// track. See `parseReply`.
constexpr std::size_t kMinInlineAudio = 64;

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

/// Cut at a word boundary rather than mid-word, and never mid-UTF-8: a byte
/// with the high bit set is part of a multi-byte character, and a request in
/// Russian is one long run of them.
std::string clampWords(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t cut = text.rfind(' ', limit);
    if (cut == std::string::npos || cut < limit / 2) {
        cut = limit;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
    }
    return trim(text.substr(0, cut));
}

std::string noteName(int pitchClass) {
    static const std::array<const char*, 12> names = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[std::size_t(((pitchClass % 12) + 12) % 12)];
}

std::string round1(double value) {
    std::ostringstream out;
    out.precision(value == std::floor(value) ? 0 : 1);
    out << std::fixed << value;
    return out.str();
}

/// The first line that is a bracketed section marker — `[Verse]`, `[Chorus]`,
/// `[Припев]`. Where the style stops and the lyrics begin.
std::size_t firstSectionLine(const std::string& text) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t lineEnd = text.find('\n', at);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const std::string line = trim(text.substr(at, lineEnd - at));
        if (line.size() >= 3 && line.front() == '[' && line.back() == ']')
            return at;
        at = lineEnd + 1;
    }
    return std::string::npos;
}

bool isHexString(const std::string& text) {
    if (text.size() < 32 || (text.size() % 2) != 0) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

std::string decodeHex(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    std::string out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(char((value(text[i]) << 4) | value(text[i + 1])));
    return out;
}

/// Base64, tolerant of whitespace and of the URL-safe alphabet. Empty when the
/// string is not base64 at all, which is how the caller tells the two apart.
std::string decodeBase64(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    std::uint32_t bits = 0;
    int have = 0;
    for (char c : text) {
        if (c == '=' ) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int digit = value(c);
        if (digit < 0) return {};
        bits = (bits << 6) | std::uint32_t(digit);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out.push_back(char((bits >> have) & 0xFF));
        }
    }
    return out;
}

bool isUrl(const std::string& text) {
    auto starts = [&text](const char* prefix) {
        const std::size_t length = std::char_traits<char>::length(prefix);
        if (text.size() < length) return false;
        for (std::size_t i = 0; i < length; ++i)
            if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i])
                return false;
        return true;
    };
    return starts("http://") || starts("https://");
}

using Path = std::vector<const char*>;

/// The node at `path` ("data" then "audio"), or null. Never throws on a wrong
/// type, and never walks into a non-object.
const json* nodeAt(const json& root, const Path& path) {
    const json* at = &root;
    for (const char* step : path) {
        if (!at->is_object() || !at->contains(step)) return nullptr;
        at = &(*at)[step];
    }
    return at;
}

std::string stringAt(const json& root, const Path& path) {
    const json* at = nodeAt(root, path);
    return at && at->is_string() ? at->get<std::string>() : std::string();
}

double numberAt(const json& root, const Path& path) {
    const json* at = nodeAt(root, path);
    if (!at) return 0.0;
    if (at->is_number()) return at->get<double>();
    if (at->is_string()) {
        try {
            return std::stod(at->get<std::string>());
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

} // namespace

std::string slug(const std::string& text, std::size_t maxLength) {
    std::string out;
    bool pendingDash = false;
    bool cut = false;
    for (unsigned char c : text) {
        if (out.size() >= maxLength) {
            cut = true;
            break;
        }
        if (std::isalnum(c) && c < 0x80) {
            if (pendingDash && !out.empty()) out.push_back('-');
            pendingDash = false;
            out.push_back(char(std::tolower(c)));
        } else {
            pendingDash = true;
        }
    }
    // A name cut mid-word ("…-brushed-drum") reads as a typo on a track
    // header, so a truncated name loses its last word rather than half of it.
    if (cut) {
        const std::size_t lastDash = out.rfind('-');
        if (lastDash != std::string::npos && lastDash > maxLength / 2)
            out.erase(lastDash);
    }
    return out;
}

MusicBrief buildBrief(const EngineController& c, const std::string& request,
                      const ToolContext& context, bool instrumental) {
    MusicBrief brief;
    brief.instrumental = instrumental;

    std::string style = trim(request);
    const std::size_t sectionAt = firstSectionLine(style);
    if (sectionAt != std::string::npos) {
        const std::string lyrics = trim(style.substr(sectionAt));
        style = trim(style.substr(0, sectionAt));
        // Lyrics on an instrumental would be silently ignored by the model;
        // dropping them here keeps the request honest about what was asked.
        if (!instrumental) brief.lyrics = lyrics;
    }
    if (style.empty())
        style = instrumental ? "instrumental music that fits this project"
                             : "a song that fits this project";

    std::ostringstream facts;
    facts << " Tempo " << round1(c.tempo()) << " BPM, " << c.timeSigNumerator()
          << "/" << c.timeSigDenominator() << ", key " << noteName(c.keyRoot())
          << " " << miditools::scaleName(miditools::scaleFromId(c.projectScale()))
          << ".";

    // What is already in the session, so the result arrives as a part of this
    // arrangement rather than as a song of its own.
    std::vector<std::string> named;
    for (const TrackModel& t : c.project().tracks) {
        if (t.kind == TrackKind::Folder) continue;
        if (named.size() >= kMaxTracksNamed) break;
        named.push_back(t.name + " (" + toString(t.kind) + ")");
    }
    if (!named.empty()) {
        facts << " It plays alongside: ";
        for (std::size_t i = 0; i < named.size(); ++i)
            facts << (i ? ", " : "") << named[i];
        facts << ".";
    }
    if (!context.focus.trackId.empty())
        if (const TrackModel* t = c.project().findTrack(context.focus.trackId))
            facts << " The user is working on \"" << t->name << "\".";

    if (c.isLoopEnabled() && c.loopEndSeconds() > c.loopStartSeconds()) {
        brief.seconds = c.loopEndSeconds() - c.loopStartSeconds();
        facts << " About " << round1(brief.seconds) << " seconds long.";
    }
    if (instrumental) facts << " Instrumental, no vocals.";

    const std::string tail = facts.str();
    // The user's own words come first and are never cut for the sake of the
    // facts: what they asked for outranks what the project implies.
    const std::size_t room =
        tail.size() < kMaxPrompt ? kMaxPrompt - tail.size() : kMaxPrompt / 2;
    brief.prompt = clampWords(style, room) + tail;
    return brief;
}

json requestJson(const MusicBrief& brief, const std::string& model,
                 const std::string& format, int sampleRate, int bitrate) {
    json body{{"model", model},
              {"prompt", brief.prompt},
              {"audio_setting",
               json{{"sample_rate", sampleRate},
                    {"bitrate", bitrate},
                    {"format", format.empty() ? std::string("mp3") : format}}},
              {"output_format", "url"}};
    if (brief.instrumental) {
        body["is_instrumental"] = true;
    } else if (!brief.lyrics.empty()) {
        body["lyrics"] = brief.lyrics;
    } else {
        // No words were given and vocals were not refused: let the model write
        // them, rather than sending an empty `lyrics` the server would reject.
        body["lyrics_optimizer"] = true;
    }
    return body;
}

MusicResult parseReply(const json& reply) {
    MusicResult out;
    if (!reply.is_object()) {
        out.error = "the music server did not return a JSON object.";
        return out;
    }

    // The provider's own error channel comes first: a failed request still
    // answers 200 with a non-zero code in here.
    if (reply.contains("base_resp") && reply["base_resp"].is_object()) {
        const json& base = reply["base_resp"];
        const double code = numberAt(base, {"status_code"});
        if (code != 0.0) {
            const std::string message = stringAt(base, {"status_msg"});
            out.error = message.empty()
                            ? "the music server reported error " +
                                  std::to_string(int(code)) + "."
                            : message;
            return out;
        }
    }
    for (const char* field : {"error", "message", "detail"}) {
        if (reply.contains(field) && reply[field].is_string() &&
            !reply[field].get<std::string>().empty()) {
            out.error = reply[field].get<std::string>();
            return out;
        }
    }

    // `data.audio` is where MiniMax puts it; the rest are the names the other
    // servers wrapping this model use, tried in turn.
    static const std::vector<Path> kAudioPaths = {
        {"data", "audio"},     {"data", "audio_url"}, {"data", "url"},
        {"data", "audio_hex"}, {"audio"},             {"audio_url"},
        {"url"},               {"output", "audio"},   {"output", "audio_url"}};
    std::string audio;
    for (const Path& path : kAudioPaths) {
        audio = stringAt(reply, path);
        if (!audio.empty()) break;
    }
    if (audio.empty()) {
        out.error =
            "the music server's reply contained no audio (expected it at "
            "data.audio).";
        return out;
    }

    out.format = stringAt(reply, {"data", "format"});
    if (out.format.empty()) out.format = stringAt(reply, {"extra_info", "audio_format"});
    if (out.format.empty()) out.format = stringAt(reply, {"audio_setting", "format"});

    double seconds = numberAt(reply, {"extra_info", "music_duration"});
    if (seconds == 0.0) seconds = numberAt(reply, {"extra_info", "audio_length"});
    if (seconds == 0.0) seconds = numberAt(reply, {"data", "duration"});
    // These APIs report milliseconds about as often as seconds, and nothing
    // this model makes is ten minutes long.
    out.seconds = seconds > 600.0 ? seconds / 1000.0 : seconds;

    if (isUrl(audio)) {
        out.audioUrl = audio;
        return out;
    }
    out.audioBytes = isHexString(audio) ? decodeHex(audio) : decodeBase64(audio);
    // Short prose happens to be valid base64 — "not audio" decodes to six
    // bytes of nothing. Nothing this model produces is under a kilobyte, so a
    // tiny result means the field was never audio in the first place, and
    // saying so beats writing an unplayable file to the user's disk.
    if (out.audioBytes.size() < kMinInlineAudio) {
        out.audioBytes.clear();
        out.error =
            "the music server returned an audio field that is neither a URL "
            "nor decodable audio.";
    }
    return out;
}

} // namespace daw::ai
