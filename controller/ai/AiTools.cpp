#include "ai/AiTools.hpp"
#include "ai/ContentCatalog.hpp"
#include "ai/CompositionEngine.hpp"
#include "ai/ProjectMusicContext.hpp"

#include "EngineController.hpp"
#include "MidiTools.hpp"
#include "model/Document.hpp"
#include "plugins/PluginManager.hpp"

#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <optional>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace daw::ai {

namespace {

// ── Units ───────────────────────────────────────────────────────────────────
// The tools speak bars and decibels because that is what a producer — and a
// model trained on how producers talk — thinks in. The document speaks seconds
// and linear gain. All of that conversion happens here and nowhere else.

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

double gainToDb(double gain) {
    return gain > 1e-6 ? 20.0 * std::log10(gain) : -96.0;
}

/// Rounded to two places: a model that reads back `0.9999999` will helpfully
/// "fix" it on the next call and burn a round trip doing nothing.
double round2(double v) { return std::round(v * 100.0) / 100.0; }

std::string privateFileLabel(const std::string& path) {
    const std::string name = fs::path(path).filename().string();
    return name.empty() ? "selected file" : name;
}

// ── Argument reading ────────────────────────────────────────────────────────
// Every failure has to reach the model as a sentence, so these accumulate into
// one error string rather than throwing. The first failure wins: telling the
// model three things at once about one bad call is noise.

bool wantString(const json& args, const char* key, std::string& out,
                std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        error = std::string("missing required argument '") + key + "'";
        return false;
    }
    if (!it->is_string()) {
        error = std::string("argument '") + key + "' must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool wantNumber(const json& args, const char* key, double& out,
                std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        error = std::string("missing required argument '") + key + "'";
        return false;
    }
    if (!it->is_number()) {
        error = std::string("argument '") + key + "' must be a number";
        return false;
    }
    out = it->get<double>();
    return true;
}

/// Absent leaves `out` alone, so the caller's current value is the default.
/// Present-but-wrong-type is still an error: a model that sent `"0.5"` needs to
/// hear about it rather than have it silently ignored.
bool optNumber(const json& args, const char* key, double& out,
               std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return false;
    if (!it->is_number()) {
        error = std::string("argument '") + key + "' must be a number";
        return false;
    }
    out = it->get<double>();
    return true;
}

bool optBool(const json& args, const char* key, bool& out,
             std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return false;
    if (!it->is_boolean()) {
        error = std::string("argument '") + key + "' must be true or false";
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool optString(const json& args, const char* key, std::string& out,
               std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return false;
    if (!it->is_string()) {
        error = std::string("argument '") + key + "' must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

/// A model that cannot nest JSON inside a tool call sends the container as a
/// *string* of JSON instead — most often the note list, which is the one
/// argument big enough to tempt it. Accepting that costs nothing and saves a
/// whole round trip; refusing it lost the part the model had already written.
bool wantArray(const json& args, const char* key, json& out,
               std::string& error) {
    if (!error.empty()) return false;
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        error = std::string("missing required argument '") + key + "'";
        return false;
    }
    if (it->is_array()) {
        out = *it;
        return true;
    }
    if (it->is_string()) {
        json parsed = json::parse(it->get<std::string>(), nullptr,
                                  /*allow_exceptions=*/false);
        if (parsed.is_array()) {
            out = std::move(parsed);
            return true;
        }
    }
    error = std::string("argument '") + key + "' must be an array";
    return false;
}

/// What the model is told when the user says no. Phrased so it stops and asks
/// rather than trying the same thing a different way.
constexpr const char* kRefused =
    "the user declined that. Do not try it again — tell them what you wanted "
    "to do and why, and let them decide.";

bool allowed(const ToolContext& ctx, const std::string& what) {
    return !ctx.confirmDestructive || ctx.confirmDestructive(what);
}

ToolResult fail(std::string message) {
    ToolResult r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

ToolResult done(json value = json::object()) {
    ToolResult r;
    r.ok = true;
    r.value = std::move(value);
    return r;
}

/// A comma-separated list, for the "what you could have said instead" half of
/// an error message.
std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += ", ";
        out += part;
    }
    return out.empty() ? std::string("none") : out;
}

/// "#30D158", "30D158" or "0x30D158" — every spelling a model reaches for.
bool parseHexColor(const std::string& text, std::uint32_t& out) {
    std::string digits = text;
    if (!digits.empty() && digits.front() == '#') digits.erase(0, 1);
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        digits.erase(0, 2);
    if (digits.size() != 6) return false;
    std::uint32_t value = 0;
    for (const char ch : digits) {
        int digit = 0;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else return false;
        value = value * 16 + std::uint32_t(digit);
    }
    out = value;
    return true;
}

/// Track ids are uuids the model copied from a snapshot; getting one wrong is
/// its most likely mistake, so the message lists what does exist.
std::string unknownTrack(const EngineController& c, const std::string& id) {
    std::string known;
    for (const TrackModel& t : c.project().tracks) {
        if (!known.empty()) known += ", ";
        known += t.name + " (" + t.id + ")";
    }
    return "no track with id '" + id + "'. Tracks in this project: " +
           (known.empty() ? std::string("none") : known);
}

// ── Snapshot pieces ─────────────────────────────────────────────────────────

json insertJson(const InsertModel& slot) {
    return json{{"id", slot.id},
                {"name", slot.name},
                {"format", toString(slot.format)},
                {"uid", slot.uid},
                {"bypassed", slot.bypassed}};
}

json clipJson(const EngineController& c, const ClipModel& clip) {
    json j{{"id", clip.id},
           {"name", clip.name},
           {"kind", toString(clip.kind)},
           {"startBar", round2(secondsToBars(c, clip.startSeconds) + 1.0)},
           {"lengthBars", round2(secondsToBars(c, clip.durationSeconds))}};
    if (clip.kind == ClipKind::Midi) j["noteCount"] = clip.notes.size();
    if (clip.muted) j["muted"] = true;
    return j;
}

json trackJson(const EngineController& c, const TrackModel& t) {
    json j{{"id", t.id},
           {"name", t.name},
           {"kind", toString(t.kind)},
           {"volumeDb", round2(gainToDb(t.volume))},
           {"pan", round2(t.pan)}};
    if (t.muted) j["muted"] = true;
    if (t.soloed) j["soloed"] = true;
    if (t.instrument.isLoaded()) j["instrument"] = insertJson(t.instrument);

    json inserts = json::array();
    for (const InsertModel& slot : t.inserts)
        if (slot.isLoaded()) inserts.push_back(insertJson(slot));
    if (!inserts.empty()) j["inserts"] = std::move(inserts);

    json sends = json::array();
    for (const SendModel& s : t.sends)
        sends.push_back(json{{"id", s.id},
                             {"to", s.destinationTrackId},
                             {"level", round2(s.level)},
                             {"preFader", s.preFader}});
    if (!sends.empty()) j["sends"] = std::move(sends);

    json clips = json::array();
    for (const ClipModel& clip : t.clips) clips.push_back(clipJson(c, clip));
    if (!clips.empty()) j["clips"] = std::move(clips);
    return j;
}

// ── Lookups shared by several tools ─────────────────────────────────────────

const ClipModel* findClip(const EngineController& c, const std::string& trackId,
                          const std::string& clipId) {
    const TrackModel* track = c.project().findTrack(trackId);
    if (!track) return nullptr;
    for (const ClipModel& clip : track->clips)
        if (clip.id == clipId) return &clip;
    return nullptr;
}

/// A channel is a track id or the literal "master". Returns false with a filled
/// error when it is neither.
bool channelExists(const EngineController& c, const std::string& channelId) {
    return channelId == EngineController::kMasterChannelId ||
           c.project().findTrack(channelId) != nullptr;
}

/// Resolve a plugin the model named by format + uid, or explain what is there.
std::optional<plugins::PluginDescriptor> findPlugin(EngineController& c,
                                                    const std::string& format,
                                                    const std::string& uid,
                                                    std::string& error) {
    const plugins::Format fmt = plugins::formatFromString(format);
    if (fmt == plugins::Format::Unknown) {
        error = "unknown plugin format '" + format +
                "'. Use one of: clap, vst3, vst, au, internal";
        return std::nullopt;
    }
    if (const auto found = c.pluginManager().find(fmt, uid)) return found;
    error = "no " + format + " plugin with uid '" + uid +
            "' is installed. Call list_plugins and use a uid from that list "
            "exactly as given — uids cannot be guessed from a plugin's name.";
    return std::nullopt;
}

// The catalog enforces its own hard cap; the tool keeps the public response
// bounded too so a broad query cannot consume the model's whole context.
constexpr std::size_t kSearchLimit = 200;

/// Model-facing metadata deliberately omits the trusted native path.
json contentJson(const ContentItem& item) {
    json out{{"contentId", item.contentId},
             {"name", item.name},
             {"type", toString(item.type)},
             {"sizeBytes", item.sizeBytes}};
    if (item.audio) {
        out["seconds"] = round2(item.audio->durationSeconds);
        out["sampleRate"] = round2(item.audio->sampleRate);
        out["channels"] = item.audio->channels;
        if (item.audio->timbre) {
            const AudioTimbreMetadata& timbre = *item.audio->timbre;
            out["timbre"] =
                json{{"rmsDbfs", round2(timbre.rmsDbfs)},
                     {"peakDbfs", round2(timbre.peakDbfs)},
                     {"crestFactorDb", round2(timbre.crestFactorDb)},
                     {"brightness", round2(timbre.brightness)},
                     {"transientness", round2(timbre.transientness)},
                     {"stereoWidth", round2(timbre.stereoWidth)},
                     {"zeroCrossingRate", round2(timbre.zeroCrossingRate)},
                     {"sampledSeconds", round2(timbre.sampledSeconds)}};
        }
    }
    if (item.midi) {
        out["noteCount"] = item.midi->noteCount;
        out["lengthBeats"] = round2(item.midi->lengthBeats);
        out["trackCount"] = item.midi->trackCount;
        if (item.midi->firstTempoBpm > 0.0)
            out["tempo"] = round2(item.midi->firstTempoBpm);
        out["tempoChanges"] = item.midi->hasTempoChanges;
    }
    return out;
}

std::optional<CompositionRole> compositionRole(const std::string& name) {
    if (name == "melody") return CompositionRole::Melody;
    if (name == "bass") return CompositionRole::Bass;
    if (name == "chords") return CompositionRole::Chords;
    if (name == "drums") return CompositionRole::Drums;
    return std::nullopt;
}

json scoreJson(const CompositionCandidateScore& score) {
    const auto component = [](const CompositionScoreComponent& value) {
        return json{{"value", round2(value.value)},
                    {"explanation", value.explanation}};
    };
    return json{{"total", round2(score.total)},
                {"harmony", component(score.harmony)},
                {"rhythm", component(score.rhythm)},
                {"register", component(score.registerFit)},
                {"repetition", component(score.repetition)},
                {"voiceLeading", component(score.voiceLeading)}};
}

json candidateJson(const CompositionCandidate& candidate) {
    json notes = json::array();
    for (const CompositionNoteEvent& note : candidate.notes)
        notes.push_back(json{{"pitch", note.pitch},
                             {"start", round2(note.startBeats)},
                             {"length", round2(note.lengthBeats)},
                             {"velocity", note.velocity}});
    return json{{"candidateId", candidate.id},
                {"variation", candidate.variationIndex + 1},
                {"noteCount", candidate.notes.size()},
                {"score", scoreJson(candidate.score)},
                {"notes", std::move(notes)}};
}

std::string attachmentId(const Attachment& attachment, std::size_t index) {
    return attachment.contentId.empty()
               ? "attachment_" + std::to_string(index + 1)
               : attachment.contentId;
}

bool pathWithin(const fs::path& candidate, const fs::path& root) {
    std::error_code ec;
    const fs::path relative = fs::relative(candidate, root, ec);
    if (ec || relative.empty() || relative.is_absolute()) return false;
    for (const fs::path& component : relative)
        if (component == "..") return false;
    return true;
}

std::optional<std::string> authorizedInputPath(const json& args,
                                               const ToolContext& ctx,
                                               std::string& error) {
    std::string contentId, legacyPath;
    optString(args, "contentId", contentId, error);
    optString(args, "filePath", legacyPath, error);
    if (!error.empty()) return std::nullopt;

    if (!contentId.empty()) {
        for (std::size_t i = 0; i < ctx.attachments.size(); ++i)
            if (attachmentId(ctx.attachments[i], i) == contentId)
                return ctx.attachments[i].path;
        if (ctx.contentCatalog) {
            if (const auto resolved = ctx.contentCatalog->resolvePath(contentId))
                return resolved;
        }
        error = "contentId '" + contentId +
                "' is unknown or its folder permission was revoked; call "
                "search_files again";
        return std::nullopt;
    }

    if (legacyPath.empty()) {
        error = "send contentId from search_files or from FILES THE USER "
                "ATTACHED";
        return std::nullopt;
    }

    std::error_code ec;
    const fs::path candidate =
        fs::canonical(platform::pathFromUtf8(legacyPath), ec);
    if (ec) {
        error = "file '" +
                platform::pathToUtf8(platform::pathFromUtf8(legacyPath).filename()) +
                "' does not exist or cannot be read";
        return std::nullopt;
    }
    for (const Attachment& attachment : ctx.attachments) {
        const fs::path attached =
            fs::canonical(platform::pathFromUtf8(attachment.path), ec);
        if (!ec && candidate == attached)
            return platform::pathToUtf8(candidate);
        ec.clear();
    }
    for (const std::string& rootText : ctx.sampleFolders) {
        const fs::path root =
            fs::canonical(platform::pathFromUtf8(rootText), ec);
        if (!ec && pathWithin(candidate, root))
            return platform::pathToUtf8(candidate);
        ec.clear();
    }
    error = "that path is outside the files the user granted to the browser "
            "or attached to this chat";
    return std::nullopt;
}

/// Pitch-class names for reporting a key back. Sharps, because that is what
/// `pitchName` uses and the two should not disagree.
const std::vector<std::string>& noteNames() {
    static const std::vector<std::string> names = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names;
}

/// "C", "F#", "Bb" → a pitch class. Empty when it is not a note name.
int pitchClassFromName(const std::string& name, bool& ok) {
    static const std::string letters = "CDEFGAB";
    static const int offsets[] = {0, 2, 4, 5, 7, 9, 11};
    ok = false;
    if (name.empty()) return 0;
    const auto letter = letters.find(char(std::toupper(name[0])));
    if (letter == std::string::npos) return 0;
    int value = offsets[letter];
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] == '#') ++value;
        else if (name[i] == 'b' || name[i] == 'B') --value;
        else return 0;
    }
    ok = true;
    return ((value % 12) + 12) % 12;
}

miditools::ChordParams::Type chordTypeFromId(const std::string& id, bool& ok) {
    using T = miditools::ChordParams::Type;
    static const std::pair<const char*, T> table[] = {
        {"major", T::Major},         {"minor", T::Minor},
        {"diminished", T::Diminished}, {"augmented", T::Augmented},
        {"major7", T::Major7},       {"minor7", T::Minor7},
        {"dominant7", T::Dominant7}, {"minor7b5", T::Minor7b5},
        {"diminished7", T::Diminished7}, {"sus2", T::Sus2},
        {"sus4", T::Sus4},           {"add9", T::Add9},
        {"major9", T::Major9},       {"minor9", T::Minor9},
        {"power", T::Power},
    };
    for (const auto& [name, type] : table)
        if (id == name) { ok = true; return type; }
    ok = false;
    return T::Major;
}

} // namespace

// ── Units, public ───────────────────────────────────────────────────────────

double beatsPerBar(const EngineController& c) {
    const int num = std::max(1, c.timeSigNumerator());
    const int den = std::max(1, c.timeSigDenominator());
    return num * 4.0 / den;
}

double barsToSeconds(const EngineController& c, double bars) {
    return beatsToSeconds(bars * beatsPerBar(c), c.tempo());
}

double secondsToBars(const EngineController& c, double seconds) {
    const double perBar = beatsPerBar(c);
    return perBar > 0.0 ? secondsToBeats(seconds, c.tempo()) / perBar : 0.0;
}

json ToolResult::toJson() const {
    if (ok) {
        json out = value.is_null() ? json::object() : value;
        if (!out.is_object()) out = json{{"result", value}};
        out["ok"] = true;
        return out;
    }
    return json{{"ok", false}, {"error", error}};
}

json projectSnapshot(const EngineController& c, const ToolContext& ctx) {
    json tracks = json::array();
    for (const TrackModel& t : c.project().tracks) tracks.push_back(trackJson(c, t));

    json master{{"id", EngineController::kMasterChannelId},
                {"volumeDb", round2(gainToDb(c.masterVolume()))}};
    json inserts = json::array();
    for (const InsertModel& slot : c.project().masterInserts)
        if (slot.isLoaded()) inserts.push_back(insertJson(slot));
    if (!inserts.empty()) master["inserts"] = std::move(inserts);

    // Where the user is, in the units the tools take.
    json here{{"playheadBar", round2(secondsToBars(c, c.positionSeconds()) + 1.0)}};
    if (c.isLoopEnabled())
        here["loop"] = json{
            {"fromBar", round2(secondsToBars(c, c.loopStartSeconds()) + 1.0)},
            {"toBar", round2(secondsToBars(c, c.loopEndSeconds()) + 1.0)}};
    if (!ctx.focus.trackId.empty()) {
        here["selectedTrackId"] = ctx.focus.trackId;
        if (const TrackModel* t = c.project().findTrack(ctx.focus.trackId))
            here["selectedTrackName"] = t->name;
    }
    if (!ctx.focus.clipId.empty()) here["selectedClipId"] = ctx.focus.clipId;
    if (!ctx.focus.trackIds.empty())
        here["selectedTrackIds"] = ctx.focus.trackIds;
    if (!ctx.focus.clipIds.empty())
        here["selectedClipIds"] = ctx.focus.clipIds;

    return json{{"name", c.projectName()},
                {"focus", std::move(here)},
                {"tempo", round2(c.tempo())},
                {"assistantMode", interactionModeName(ctx.mode)},
                {"key", noteNames()[c.keyRoot()] + " " +
                            miditools::scaleName(
                                miditools::scaleFromId(c.projectScale()))},
                {"timeSignature", std::to_string(c.timeSigNumerator()) + "/" +
                                      std::to_string(c.timeSigDenominator())},
                {"master", std::move(master)},
                {"tracks", std::move(tracks)}};
}

// ── The registry ────────────────────────────────────────────────────────────

namespace {

/// Built with a loop rather than a json initializer-list: a braced list of
/// pairs is ambiguous with nlohmann's array construction, and this reads the
/// same at the call sites.
json obj(std::initializer_list<std::pair<const char*, json>> fields) {
    json out = json::object();
    for (const auto& [key, value] : fields) out[key] = value;
    return out;
}

json prop(const char* type, const char* description) {
    return json{{"type", type}, {"description", description}};
}

json schema(json properties, std::vector<std::string> required) {
    return json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)}};
}

std::vector<ToolSpec> buildSpecs() {
    std::vector<ToolSpec> specs;
    auto add = [&specs](const char* name, const char* description, json s) {
        specs.push_back({name, description, std::move(s)});
    };

    // ── Instructions ──
    add("get_playbook",
        "How to do one kind of work well: writing a bass part, programming a "
        "beat, voicing chords, processing a vocal, mixing, arranging, "
        "automating. The ids are listed under PLAYBOOKS in your instructions. "
        "Load the matching one BEFORE you start that work and follow it — it "
        "carries the ranges, grids and orderings this program's users expect, "
        "and they are not repeated anywhere else. A request that spans two "
        "areas takes both.",
        schema(obj({{"id",
                     prop("string",
                          "the playbook's id, exactly as listed under "
                          "PLAYBOOKS, e.g. bass or drums")}}),
               {"id"}));

    add("search_commands",
        "Find real program actions by id, label, category or description. Use "
        "this to answer where/how questions with the current shortcut and to "
        "discover the stable commandId before run_command.",
        schema(obj({{"query", prop("string", "words describing the action; empty lists commands")}}),
               {"query"}));

    add("run_command",
        "Invoke one currently visible and enabled program action by the exact "
        "commandId returned by search_commands. Unknown, destructive and "
        "external actions require confirmation in the application.",
        schema(obj({{"commandId", prop("string", "exact stable command id")}}),
               {"commandId"}));

    // ── Reading ──
    add("get_project",
        "The whole project: tempo, time signature, and every track with its "
        "id, kind, level, instrument, inserts, sends and clips. The ids in the "
        "reply are what every other tool takes.",
        schema(json::object(), {}));

    add("inspect_music_context",
        "Read the musical context before composing: key with source and "
        "confidence, detected MIDI key, stored audio tempo/key analysis, chord "
        "timeline and chord tones, plus each part's rhythmic activity, density, "
        "register, polyphony, instrument and effects.",
        schema(obj({{"fromBar", prop("number", "start bar; default 1")},
                    {"toBar", prop("number", "exclusive end bar; omit for project end")},
                    {"segmentBeats", prop("number", "optional chord-analysis window in beats")}}),
               {}));

    add("compose_candidates",
        "Generate and validate three to five deterministic MIDI alternatives "
        "against the project's current key and chord timeline. Each candidate "
        "has an opaque id and explainable harmony, rhythm, register, repetition "
        "and voice-leading scores. Present the alternatives before applying one.",
        schema(obj({{"role",
                     json{{"type", "string"},
                          {"enum", json::array({"melody", "bass", "chords", "drums"})}}},
                    {"fromBar", prop("number", "where the part will start; default playhead")},
                    {"bars", prop("integer", "length in bars; default 4")},
                    {"variations", prop("integer", "three to five; default 3")},
                    {"seed", prop("integer", "repeatable variation seed; change it to regenerate")},
                    {"creativity", prop("number", "0 stable to 1 adventurous; default 0.5")},
                    {"density", prop("number", "0 sparse to 1 busy; default 0.5")},
                    {"keyRoot", prop("integer", "optional pitch class 0 to 11")},
                    {"scale", prop("string", "optional scale id")},
                    {"lowestPitch", prop("integer", "optional MIDI register floor")},
                    {"highestPitch", prop("integer", "optional MIDI register ceiling")}}),
               {"role"}));

    add("apply_composition_candidate",
        "Insert one previously generated, validated composition candidate into "
        "a new MIDI clip. The stored notes are applied by candidateId, so they "
        "cannot change between scoring and insertion.",
        schema(obj({{"candidateId", prop("string", "id from compose_candidates")},
                    {"trackId", prop("string", "target instrument or MIDI track")},
                    {"startBar", prop("number", "clip start; default playhead")}}),
               {"candidateId", "trackId"}));

    add("get_clip_notes",
        "Every note in a MIDI clip, as pitch, start and length in beats from "
        "the clip's start. Read this before changing notes you did not write.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")}}),
               {"trackId", "clipId"}));

    add("list_plugins",
        "The plugins installed on this machine. Returns format and uid for "
        "each; those two together name a plugin for set_track_instrument and "
        "add_insert. Never invent a uid — call this first.",
        schema(obj({{"kind",
                     json{{"type", "string"},
                          {"enum", json::array({"instrument", "effect", "all"})},
                          {"description",
                           "instrument for something that makes sound from "
                           "MIDI, effect for something that processes audio"}}},
                    {"nameContains",
                     prop("string",
                          "optional case-insensitive filter on the name")}}),
               {"kind"}));

    add("search_files",
        "Search the user's explicitly granted browser folders for audio or "
        "MIDI. Audio results include lightweight loudness, brightness, "
        "transient and stereo-width descriptors for choosing a sound by role. "
        "Results contain an opaque contentId, never a path. Pass that "
        "contentId to analyze_sample, load_sampler or import_audio.",
        schema(obj({{"query",
                     prop("string",
                          "part of a file name, or an extension such as "
                          "\".wav\". Empty lists what is there.")},
                    {"type",
                     json{{"type", "string"},
                          {"enum", json::array({"audio", "midi", "all"})},
                          {"description", "optional type; default all"}}},
                    {"limit",
                     prop("integer", "how many to return at most; default 40")}}),
               {"query"}));

    add("list_plugin_parameters",
        "The parameters of one loaded plugin, with each parameter's id, "
        "range, unit and current value. set_insert_parameter takes those ids, "
        "and they cannot be guessed — read them here first.",
        schema(obj({{"channelId",
                     prop("string", "a track id, or \"master\"")},
                    {"insertId",
                     prop("string",
                          "id of the insert slot, or of the track's "
                          "instrument slot")}}),
               {"channelId", "insertId"}));

    add("analyze_track",
        "Render a channel on its own and measure it: level, headroom, whether "
        "it clips, and how its energy splits across low, mid and high. Do this "
        "BEFORE deciding a level or an EQ move — you cannot hear the project, "
        "and these numbers are the only thing standing between a real mix "
        "decision and a guess. Measure the master too, to see where the mix "
        "stands as a whole.",
        schema(obj({{"channelId",
                     prop("string", "a track id, or \"master\" for the mix")},
                    {"fromBar", prop("number", "start of the range; default 1")},
                    {"toBar",
                     prop("number",
                          "end of the range; omit to measure to the end of "
                          "the project")}}),
               {"channelId"}));

    add("analyze_sample",
        "Decode an audio file and describe it: how long, how loud, how fast it "
        "attacks, whether it holds a pitch, and a guess at what it is. Use it "
        "when a file's name does not tell you enough — you cannot listen, and "
        "this is the closest thing to it.",
        schema(obj({{"contentId",
                     prop("string", "id from search_files or an attachment")}}),
               {"contentId"}));

    // ── Project ──
    add("set_tempo", "Set the project tempo in BPM (20–300).",
        schema(obj({{"bpm", prop("number", "beats per minute")}}), {"bpm"}));

    add("set_project_key",
        "Record the key the project is in. Nothing is forced into it, but "
        "transform_notes with snap_to_scale uses it, and stating a key keeps "
        "everything you write afterwards consistent. Set it once when you "
        "choose a key, and tell the user which one.",
        schema(obj({{"root",
                     prop("string",
                          "the tonic: C, C#, Db, D, Eb, E, F, F#, G, Ab, A, "
                          "Bb or B")},
                    {"scale",
                     prop("string",
                          "major, natural_minor, harmonic_minor, "
                          "melodic_minor, dorian, phrygian, lydian, "
                          "mixolydian, locrian, pentatonic_major, "
                          "pentatonic_minor, blues, whole_tone or chromatic")}}),
               {"root", "scale"}));

    add("transform_notes",
        "Reshape the notes already in a clip. This is how a part stops "
        "sounding typed in: write the notes, then quantize and humanize them. "
        "Prefer these over recomputing a note list by hand — they are exact, "
        "and they are what the program's own editing menu uses.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"operation",
                     json{{"type", "string"},
                          {"enum", json::array({"quantize", "humanize",
                                                "legato", "articulate",
                                                "strum", "transpose",
                                                "transpose_in_scale",
                                                "snap_to_scale", "limit_pitch",
                                                "invert_pitch", "reverse_time",
                                                "ramp_velocity",
                                                "scale_velocity",
                                                "set_velocity", "add_velocity",
                                                "scale_length", "set_length",
                                                "nudge", "rotate",
                                                "arpeggiate", "glue",
                                                "randomize", "split_at_grid",
                                                "build_chords"})},
                          {"description",
                           "quantize pulls starts to a grid; humanize adds "
                           "small timing and velocity spread; legato runs "
                           "each note into the next; articulate shortens and "
                           "accents; strum spreads a chord in time; "
                           "arpeggiate turns held chords into a picked "
                           "pattern; glue joins touching notes on a pitch; "
                           "randomize scatters timing and velocity; "
                           "split_at_grid cuts long notes at the grid; "
                           "snap_to_scale pulls every note into the project's "
                           "key; build_chords stacks a chord on every note"}}},
                    {"amount",
                     prop("number",
                          "how much, 0 to 1, for quantize, humanize, "
                          "articulate and snap. Defaults to 1 (all the way). "
                          "0.6 to 0.8 keeps a part's feel.")},
                    {"grid",
                     prop("integer",
                          "for quantize: the note value, 4 = quarter, 8 = "
                          "eighth, 16 = sixteenth. Defaults to 16.")},
                    {"triplet", prop("boolean", "for quantize: use a triplet grid")},
                    {"semitones", prop("integer", "for transpose")},
                    {"low", prop("integer", "for limit_pitch: lowest MIDI note")},
                    {"high", prop("integer", "for limit_pitch: highest MIDI note")},
                    {"from", prop("integer", "for ramp_velocity: first note's velocity")},
                    {"to", prop("integer", "for ramp_velocity: last note's velocity")},
                    {"factor",
                     prop("number", "for scale_velocity and scale_length")},
                    {"beats",
                     prop("number", "for nudge, rotate and strum: how far, in beats")},
                    {"chord",
                     prop("string",
                          "for build_chords: major, minor, diminished, "
                          "augmented, major7, minor7, dominant7, sus2, sus4, "
                          "add9, major9, minor9 or power")},
                    {"velocity",
                     prop("integer", "for set_velocity (1-127) and add_velocity "
                                     "(a signed amount)")},
                    {"degrees",
                     prop("integer",
                          "for transpose_in_scale: how many scale steps, so a "
                          "part stays in key")}}),
               {"trackId", "clipId", "operation"}));

    add("set_time_signature", "Set the project time signature.",
        schema(obj({{"numerator", prop("integer", "beats per bar, 1–32")},
                    {"denominator",
                     prop("integer", "note value: 1, 2, 4, 8, 16 or 32")}}),
               {"numerator", "denominator"}));

    // ── Tracks and mix ──
    add("add_track",
        "Create a track and return its id. An instrument track plays MIDI "
        "through a plugin; an audio track holds recorded or imported audio; a "
        "bus is a destination for sends.",
        schema(obj({{"kind",
                     json{{"type", "string"},
                          {"enum", json::array({"instrument", "audio", "midi",
                                                "bus", "aux", "group",
                                                "folder"})},
                          {"description", "what the track is for"}}},
                    {"name", prop("string", "what to call it")}}),
               {"kind", "name"}));

    add("remove_track",
        "Delete a track and everything on it. Only when the user asked for it.",
        schema(obj({{"trackId", prop("string", "id of the track")}}),
               {"trackId"}));

    add("rename_track", "Change a track's name.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"name", prop("string", "the new name")}}),
               {"trackId", "name"}));

    add("set_track_mix",
        "Set any of a track's mixer controls. Every field is optional — send "
        "only what you want to change.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"volumeDb",
                     prop("number",
                          "fader level in dB; 0 is unity, -6 is half as loud, "
                          "+6 is the maximum")},
                    {"pan", prop("number", "-1 hard left, 0 centre, 1 hard right")},
                    {"muted", prop("boolean", "silence the track")},
                    {"soloed", prop("boolean", "silence everything else")}}),
               {"trackId"}));

    add("add_send",
        "Route a copy of a track's signal to a bus, for a shared reverb or "
        "delay. Create the bus with add_track first.",
        schema(obj({{"trackId", prop("string", "the track that sends")},
                    {"destinationTrackId", prop("string", "the bus receiving it")},
                    {"level", prop("number", "how much, 0 to 1")},
                    {"preFader",
                     prop("boolean",
                          "take the signal before the fader; defaults to "
                          "false")}}),
               {"trackId", "destinationTrackId", "level"}));

    // ── Instruments ──
    add("set_track_instrument",
        "Put a plugin instrument in a track's instrument slot, so its MIDI "
        "clips make sound. Use format and uid from list_plugins.",
        schema(obj({{"trackId", prop("string", "an instrument or MIDI track")},
                    {"format", prop("string", "clap, vst3, vst, au or internal")},
                    {"uid", prop("string", "the plugin's uid from list_plugins")}}),
               {"trackId", "format", "uid"}));

    add("load_sampler",
        "Put the built-in sampler on a track and load an audio file into it, "
        "in one step. The fastest way to make a drum or a one-shot playable "
        "from MIDI. Use a contentId from search_files or an attachment.",
        schema(obj({{"trackId", prop("string", "an instrument or MIDI track")},
                    {"contentId",
                     prop("string", "id from search_files or an attachment")}}),
               {"trackId", "contentId"}));

    // ── Clips and notes ──
    add("add_midi_clip",
        "Create an empty MIDI clip on a track and return its id. Write the "
        "notes into it with set_clip_notes.",
        schema(obj({{"trackId", prop("string", "an instrument or MIDI track")},
                    {"startBar", prop("number", "bar it starts on; bar 1 is the "
                                                "start of the project")},
                    {"lengthBars", prop("number", "how many bars long")}}),
               {"trackId", "startBar", "lengthBars"}));

    add("set_clip_notes",
        "Write notes into a MIDI clip. This is the main tool for composing: "
        "send a whole part in one call rather than a note at a time.",
        schema(obj(
                   {{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"mode",
                     json{{"type", "string"},
                          {"enum", json::array({"replace", "append"})},
                          {"description",
                           "replace discards the clip's existing notes; append "
                           "keeps them. Defaults to replace."}}},
                    {"notes",
                     json{{"type", "array"},
                          {"description", "the notes to write"},
                          {"items",
                           schema(obj({{"pitch",
                                        prop("integer",
                                             "MIDI note number, 0-127. 60 is "
                                             "middle C, shown as C5 in this "
                                             "program")},
                                       {"start",
                                        prop("number",
                                             "beats from the start of the "
                                             "clip; 0 is the clip's first beat")},
                                       {"length", prop("number", "length in beats")},
                                       {"velocity",
                                        prop("integer",
                                             "how hard, 1-127; defaults to "
                                             "100. Vary it — a part where "
                                             "every note is the same velocity "
                                             "sounds mechanical")}}),
                                  {"pitch", "start", "length"})}}}}),
               {"trackId", "clipId", "notes"}));

    add("set_clip_start", "Move a clip to another bar.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"startBar", prop("number", "the bar to move it to")}}),
               {"trackId", "clipId", "startBar"}));

    add("duplicate_clip",
        "Copy a clip and place the copy straight after the original. The way "
        "to repeat a bar or a section.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")}}),
               {"trackId", "clipId"}));

    add("remove_clip", "Delete a clip. Only when the user asked for it.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")}}),
               {"trackId", "clipId"}));

    // ── Effects ──
    add("add_insert",
        "Add an effect plugin to a channel's insert chain and return the new "
        "slot's id. Order matters: inserts process in the order they are "
        "added, so EQ before compression before reverb is the usual shape.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"format", prop("string", "clap, vst3, vst, au or internal")},
                    {"uid", prop("string", "the plugin's uid from list_plugins")},
                    {"index",
                     prop("integer",
                          "position in the chain; omit to append at the end")}}),
               {"channelId", "format", "uid"}));

    add("remove_insert", "Remove an effect from a channel's chain.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"insertId", prop("string", "id of the insert slot")}}),
               {"channelId", "insertId"}));

    add("set_insert_bypassed", "Switch an effect off without removing it.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"insertId", prop("string", "id of the insert slot")},
                    {"bypassed", prop("boolean", "true to bypass")}}),
               {"channelId", "insertId", "bypassed"}));

    add("set_insert_parameter",
        "Set one parameter of a loaded plugin, in the parameter's own units. "
        "Call list_plugin_parameters first: the ids and the ranges differ for "
        "every plugin and cannot be guessed.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"insertId",
                     prop("string", "id of the insert or instrument slot")},
                    {"parameterId",
                     prop("string", "id from list_plugin_parameters")},
                    {"value",
                     prop("number",
                          "the new value, between the parameter's min and max")}}),
               {"channelId", "insertId", "parameterId", "value"}));

    add("set_insert_parameters",
        "Set several parameters of one plugin at once. Same rules as "
        "set_insert_parameter — always list_plugin_parameters first — but one "
        "call instead of six, which is how a plugin should be set up.",
        schema(obj({{"channelId", prop("string", "a track id, or \"master\"")},
                    {"insertId",
                     prop("string", "id of the insert or instrument slot")},
                    {"parameters",
                     json{{"type", "array"},
                          {"description",
                           "objects of {parameterId, value}, values in the "
                           "parameter's own units"},
                          {"items",
                           schema(obj({{"parameterId", prop("string", "")},
                                       {"value", prop("number", "")}}),
                                  {"parameterId", "value"})}}}}),
               {"channelId", "insertId", "parameters"}));

    // ── Harmony and measurement ──
    add("analyze_harmony",
        "What is actually playing: the key, and the chord in every bar with "
        "its root note. CALL THIS BEFORE ADDING A PART TO EXISTING MATERIAL — "
        "a bass part follows the root of each chord, and a melody has to land "
        "on chord tones. Reads the MIDI notes; give it a clip, a track, or "
        "nothing at all to take every MIDI track over a bar range.",
        schema(obj({{"trackId",
                     prop("string",
                          "optional: read only this track. Omit to read every "
                          "MIDI track at once")},
                    {"clipId",
                     prop("string",
                          "optional: read only this clip. Needs trackId")},
                    {"fromBar", prop("number", "first bar, default 1")},
                    {"toBar",
                     prop("number",
                          "last bar, exclusive. Default: to the end of the "
                          "material")},
                    {"segmentBeats",
                     prop("number",
                          "how long one chord is assumed to last, in beats. "
                          "Default: one bar. Use 2 in 4/4 for music that "
                          "changes chord twice a bar")}}),
               {}));

    add("analyze_mix",
        "Measure every channel and the master in one pass: peak, RMS, "
        "headroom, and how the energy splits low/mid/high. The first thing to "
        "call before any mixing decision — you cannot hear the project, and "
        "the numbers are what tells you which channel is eating the mix.",
        schema(obj({{"fromBar", prop("number", "first bar, default 1")},
                    {"toBar",
                     prop("number", "last bar. Default: the whole project")}}),
               {}));

    // ── Transport and project ──
    add("transport",
        "Drive the transport: start or stop playback, move the playhead, "
        "switch the metronome. Only when the user asked — playback is their "
        "hands, not yours.",
        schema(obj({{"action",
                     json{{"type", "string"},
                          {"enum", json::array({"play", "stop", "pause", "seek",
                                                "metronome_on", "metronome_off"})},
                          {"description", "what to do"}}},
                    {"bar",
                     prop("number", "for seek: which bar to move the playhead to")}}),
               {"action"}));

    add("set_loop",
        "Set or clear the loop range, in bars. A loop is also how the user "
        "says \"this part\" — reading it back is often more useful than "
        "setting it.",
        schema(obj({{"enabled", prop("boolean", "turn looping on or off")},
                    {"fromBar", prop("number", "first bar of the loop")},
                    {"toBar", prop("number", "bar the loop ends at, exclusive")}}),
               {"enabled"}));

    add("undo",
        "Undo the last change. Only when the user asked to undo something — "
        "never to clean up after yourself, because it may take their own work "
        "with it.",
        schema(json::object(), {}));

    add("redo", "Redo what was just undone. Only when the user asked.",
        schema(json::object(), {}));

    add("save_project",
        "Save the project. Only when the user asked; they choose when their "
        "work is written to disk.",
        schema(obj({{"path",
                     prop("string",
                          "optional: where to save. Omit to save where the "
                          "project already lives")}}),
               {}));

    add("export_audio",
        "Render the project to an audio file. Only when the user asked for a "
        "bounce, an export or a file to send somebody. Confirm the path first "
        "if something is already there.",
        schema(obj({{"path",
                     prop("string",
                          "the file to write, ending in .wav")},
                    {"normalize",
                     prop("boolean",
                          "bring the peak up to full scale. Default false; "
                          "leave it off for anything already mastered")}}),
               {"path"}));

    add("import_audio",
        "Put an audio file into the project as a clip. Use the contentId from "
        "search_files or an attachment.",
        schema(obj({{"contentId", prop("string", "id of the audio file")},
                    {"trackId",
                     prop("string",
                          "optional: an existing audio track. Omit to make a "
                          "new track for it")},
                    {"atBar", prop("number", "where it starts, default 1")},
                    {"trackName",
                     prop("string", "name for the new track, when making one")}}),
               {"contentId"}));

    // ── The action tools. Their full tables are in the tools-reference
    // playbook, so the schema here stays small enough to send every time. ──
    add("edit_clip",
        "Change a clip that already exists: move it, rename it, mute it, fade "
        "it, trim it, split it, copy it to another bar, or remove it. ALWAYS "
        "prefer this to rewriting a part — the user's material is theirs. Read "
        "the tools-reference playbook for the full table of actions.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"action",
                     json{{"type", "string"},
                          {"enum", json::array({"move", "move_to_track",
                                                "rename", "mute", "unmute",
                                                "gain", "fade", "trim", "split",
                                                "duplicate_at", "remove"})},
                          {"description",
                           "move takes atBar; move_to_track takes toTrackId; "
                           "rename takes name; gain takes gainDb; fade takes "
                           "fadeInBeats and fadeOutBeats; trim takes atBar, "
                           "offsetBeats and lengthBars; split and duplicate_at "
                           "take atBar"}}},
                    {"atBar", prop("number", "bar, for move, split, trim and duplicate_at")},
                    {"toTrackId", prop("string", "for move_to_track")},
                    {"name", prop("string", "for rename")},
                    {"gainDb", prop("number", "for gain, -60 to 12")},
                    {"fadeInBeats", prop("number", "for fade")},
                    {"fadeOutBeats", prop("number", "for fade")},
                    {"offsetBeats",
                     prop("number", "for trim: how far into the material to start")},
                    {"lengthBars", prop("number", "for trim: how long it plays")}}),
               {"trackId", "clipId", "action"}));

    add("edit_notes",
        "Change part of a MIDI clip without rewriting all of it. Use this "
        "rather than set_clip_notes whenever the clip already has notes you "
        "are keeping.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"action",
                     json{{"type", "string"},
                          {"enum", json::array({"add", "remove", "replace_range"})},
                          {"description",
                           "add appends notes; remove deletes the notes inside "
                           "the beat range; replace_range deletes them and puts "
                           "`notes` in their place"}}},
                    {"notes",
                     json{{"type", "array"},
                          {"description",
                           "for add and replace_range: {pitch, start, length, "
                           "velocity}, start and length in beats from the "
                           "clip's start"},
                          {"items",
                           schema(obj({{"pitch", prop("integer", "0-127")},
                                       {"start", prop("number", "beats")},
                                       {"length", prop("number", "beats")},
                                       {"velocity", prop("integer", "1-127")}}),
                                  {"pitch", "start", "length"})}}},
                    {"fromBeat",
                     prop("number", "for remove and replace_range, default 0")},
                    {"toBeat",
                     prop("number",
                          "for remove and replace_range. Default: the end of "
                          "the clip")},
                    {"lowPitch",
                     prop("integer",
                          "optional: only touch notes at or above this pitch — "
                          "how you edit the hats without touching the kick")},
                    {"highPitch", prop("integer", "optional: and at or below this")}}),
               {"trackId", "clipId", "action"}));

    add("mix",
        "Levels, pan, mutes, solos and sends — the mixer, in one tool. Measure "
        "with analyze_mix first; the mixing playbook says in what order.",
        schema(obj({{"action",
                     json{{"type", "string"},
                          {"enum", json::array({"set_level", "set_pan", "mute",
                                                "unmute", "solo", "unsolo",
                                                "clear_solos", "set_send",
                                                "remove_send",
                                                "set_master_volume"})},
                          {"description",
                           "set_level takes levelDb; set_pan takes pan; "
                           "set_send takes toTrackId and levelDb (it creates "
                           "the send if there is none); remove_send takes "
                           "sendId; set_master_volume takes levelDb"}}},
                    {"channelId",
                     prop("string",
                          "the track, or \"master\". Not needed for "
                          "clear_solos or set_master_volume")},
                    {"levelDb", prop("number", "-60 to 6")},
                    {"pan", prop("number", "-1 left to 1 right")},
                    {"toTrackId", prop("string", "for set_send: the bus to feed")},
                    {"sendId", prop("string", "for remove_send")}}),
               {"action"}));

    add("arrange_tracks",
        "The arrangement itself: order, folders and colours. Use it to keep a "
        "project readable once it has more than a handful of tracks.",
        schema(obj({{"action",
                     json{{"type", "string"},
                          {"enum", json::array({"reorder", "create_folder",
                                                "move_to_folder", "set_color",
                                                "duplicate_track"})},
                          {"description",
                           "reorder takes trackId and toIndex; create_folder "
                           "takes name and summing and returns its id; "
                           "move_to_folder takes trackId and folderId; "
                           "set_color takes trackId and color"}}},
                    {"trackId", prop("string", "the track being moved or coloured")},
                    {"toIndex", prop("integer", "for reorder: 0 is the top")},
                    {"folderId",
                     prop("string",
                          "for move_to_folder. Empty string moves it back out "
                          "to the top level")},
                    {"name", prop("string", "for create_folder")},
                    {"summing",
                     prop("boolean",
                          "for create_folder: true makes it a bus everything "
                          "inside is mixed through, so it can carry effects. "
                          "False just groups them")},
                    {"color",
                     prop("string", "for set_color: a hex colour like \"#30D158\"")}}),
               {"action"}));

    add("channel_strip",
        "Copy a whole chain of effects from one channel onto another, or empty "
        "one. Faster and more consistent than adding the same four plugins "
        "twice — and the plugins arrive holding the settings they were copied "
        "at.",
        schema(obj({{"action",
                     json{{"type", "string"},
                          {"enum", json::array({"copy_to", "clear"})},
                          {"description",
                           "copy_to copies channelId's chain onto toChannelId; "
                           "clear removes every effect from channelId"}}},
                    {"channelId",
                     prop("string",
                          "the source, or the channel being cleared. A track "
                          "id, or \"master\"")},
                    {"toChannelId", prop("string", "for copy_to: the destination")},
                    {"withSettings",
                     prop("boolean",
                          "for copy_to: bring the fader, pan and sends across "
                          "as well as the plugins. Default false")}}),
               {"action", "channelId"}));

    add("automation",
        "Make a parameter move over time: fades, filter sweeps, ducking, a "
        "volume ride. Read the automation playbook — values are normalised 0 "
        "to 1, not the parameter's own units.",
        schema(obj({{"action",
                     json{{"type", "string"},
                          {"enum", json::array({"list_targets", "set_points",
                                                "remove"})},
                          {"description",
                           "list_targets shows what can be automated on a "
                           "channel; set_points writes a curve, creating the "
                           "lane if there is none; remove deletes the lane"}}},
                    {"channelId", prop("string", "the track, or \"master\"")},
                    {"target",
                     json{{"type", "string"},
                          {"enum", json::array({"volume", "pan", "mute",
                                                "send", "plugin"})},
                          {"description",
                           "what is being automated. plugin also needs "
                           "insertId and parameterId; send also needs sendId"}}},
                    {"insertId",
                     prop("string",
                          "for target plugin: the slot id, or an empty string "
                          "for the track's instrument")},
                    {"parameterId",
                     prop("string", "for target plugin: from list_plugin_parameters")},
                    {"sendId", prop("string", "for target send")},
                    {"points",
                     json{{"type", "array"},
                          {"description",
                           "the curve: {bar, value} pairs, value 0 to 1. Land "
                           "the important ones exactly on a bar line"},
                          {"items",
                           schema(obj({{"bar", prop("number", "position, in bars")},
                                       {"value", prop("number", "0 to 1")},
                                       {"shape",
                                        json{{"type", "string"},
                                             {"enum", json::array({"linear",
                                                                   "hold",
                                                                   "smooth"})},
                                             {"description",
                                              "how it travels to the next "
                                              "point; default linear"}}}}),
                                  {"bar", "value"})}}}}),
               {"action", "channelId"}));

    add("apply_groove",
        "Push a part's timing onto a groove — swing, shuffle, or one of the "
        "program's own templates. What makes programmed drums stop sounding "
        "square. Call with no groove name to see the list.",
        schema(obj({{"trackId", prop("string", "id of the track")},
                    {"clipId", prop("string", "id of the clip")},
                    {"groove",
                     prop("string",
                          "name of the groove, exactly as listed. Omit to list "
                          "them")},
                    {"amount",
                     prop("number",
                          "how far towards it, 0 to 1. 0.5-0.8 is a feel; 1 is "
                          "a machine copy")}}),
               {"trackId", "clipId"}));

    for (ToolSpec& spec : specs) {
        const std::string& name = spec.name;
        if (name.starts_with("get_") || name.starts_with("list_") ||
            name.starts_with("search_") || name.starts_with("analyze_") ||
            name.starts_with("inspect_") || name.starts_with("describe_")) {
            spec.effect = ToolSpec::Effect::ReadOnly;
        } else if (name.starts_with("remove_")) {
            spec.effect = ToolSpec::Effect::DestructiveEdit;
        } else if (name == "undo" || name == "redo") {
            spec.effect = ToolSpec::Effect::History;
        } else if (name == "transport" || name == "set_loop") {
            spec.effect = ToolSpec::Effect::LiveControl;
        } else if (name == "save_project" || name == "export_audio" ||
                   name == "run_command") {
            spec.effect = ToolSpec::Effect::ExternalSideEffect;
        } else {
            spec.effect = ToolSpec::Effect::ReversibleEdit;
        }
    }
    return specs;
}

} // namespace

const std::vector<ToolSpec>& toolSpecs() {
    static const std::vector<ToolSpec> specs = buildSpecs();
    return specs;
}

const char* interactionModeName(InteractionMode mode) {
    switch (mode) {
        case InteractionMode::Help: return "HELP";
        case InteractionMode::Teach: return "TEACH";
        case InteractionMode::Compose: return "COMPOSE";
        case InteractionMode::Do: return "DO";
    }
    return "DO";
}

bool toolAllowedInMode(const ToolSpec& tool, InteractionMode mode) {
    if (mode == InteractionMode::Do) return true;
    if (mode == InteractionMode::Help || mode == InteractionMode::Teach)
        return tool.effect == ToolSpec::Effect::ReadOnly;
    return tool.effect == ToolSpec::Effect::ReadOnly ||
           tool.effect == ToolSpec::Effect::ReversibleEdit ||
           tool.effect == ToolSpec::Effect::LiveControl;
}

std::vector<ToolSpec> toolSpecsForMode(InteractionMode mode) {
    std::vector<ToolSpec> allowed;
    for (const ToolSpec& spec : toolSpecs())
        if (toolAllowedInMode(spec, mode)) allowed.push_back(spec);
    return allowed;
}

InteractionMode inferInteractionMode(const std::string& prompt) {
    std::string text = prompt;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) text.erase(0, first);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return ch < 128 ? static_cast<char>(std::tolower(ch))
                        : static_cast<char>(ch);
    });

    const auto has = [&text](const char* word) {
        return text.find(word) != std::string::npos;
    };
    if (text.starts_with("/help")) return InteractionMode::Help;
    if (text.starts_with("/teach")) return InteractionMode::Teach;
    if (text.starts_with("/do")) return InteractionMode::Do;
    if (text.starts_with("/compose")) return InteractionMode::Compose;

    const bool asksHow = has("how do ") || has("how can ") ||
                         has("show me how") || has("teach me") ||
                         has("как ") || has("покажи, как") ||
                         has("покажи как") || has("научи");
    if (asksHow) return InteractionMode::Teach;

    const bool asksWhat = has("what is ") || has("what does ") ||
                          has("where is ") || has("explain ") ||
                          has("что такое") || has("что делает") ||
                          has("где находится") || has("объясни");
    if (asksWhat) return InteractionMode::Help;

    const bool music = has("melod") || has("bass") || has("chord") ||
                       has("drum") || has("beat") || has("harmony") ||
                       has("music") || has("мелод") || has("бас") ||
                       has("аккорд") || has("барабан") || has("бит") ||
                       has("гармон") || has("музык");
    return music ? InteractionMode::Compose : InteractionMode::Do;
}

// ── Dispatch ────────────────────────────────────────────────────────────────

ToolResult callTool(EngineController& c, const std::string& name,
                    const json& rawArgs, const ToolContext& ctx) {
    const auto spec = std::find_if(
        toolSpecs().begin(), toolSpecs().end(),
        [&name](const ToolSpec& candidate) { return candidate.name == name; });
    if (spec != toolSpecs().end() && !toolAllowedInMode(*spec, ctx.mode)) {
        return fail("tool '" + name + "' is not available in " +
                    interactionModeName(ctx.mode) +
                    " mode; answer without changing the project or ask the "
                    "user to switch modes");
    }

    // A model occasionally sends the arguments as a JSON *string*, or sends
    // nothing at all for a no-argument tool. Both are cheap to accept.
    json args = json::object();
    if (rawArgs.is_object()) {
        args = rawArgs;
    } else if (rawArgs.is_string()) {
        args = json::parse(rawArgs.get<std::string>(), nullptr, false);
        if (args.is_discarded() || !args.is_object())
            return fail("arguments were not a JSON object");
    } else if (!rawArgs.is_null()) {
        return fail("arguments must be a JSON object");
    }

    std::string err;

    // ── Instructions ──
    if (name == "get_playbook") {
        std::string id;
        wantString(args, "id", id, err);
        if (!err.empty()) return fail(err);
        const PromptPack& pack = promptsFor(ctx);
        const Playbook* book = findPlaybook(pack, id);
        if (!book) {
            return fail("no playbook '" + id + "'. The ones that exist are: " +
                        join(playbookIds(pack)));
        }
        return done(json{{"id", book->id},
                         {"title", book->title},
                         {"use_when", book->useWhen},
                         {"body", book->body}});
    }

    if (name == "search_commands") {
        std::string query;
        wantString(args, "query", query, err);
        if (!err.empty()) return fail(err);
        if (!ctx.searchCommands)
            return fail("the program command catalog is unavailable");
        return done(ctx.searchCommands(query, ctx.mode));
    }

    if (name == "run_command") {
        std::string commandId;
        wantString(args, "commandId", commandId, err);
        if (!err.empty()) return fail(err);
        if (!ctx.invokeCommand)
            return fail("program command execution is unavailable");
        std::string commandError;
        if (!ctx.invokeCommand(commandId, ctx.mode, commandError))
            return fail(commandError.empty() ? "the command could not run"
                                             : commandError);
        return done(json{{"commandId", commandId}, {"invoked", true}});
    }

    // ── Reading ──
    if (name == "get_project") return done(projectSnapshot(c, ctx));

    if (name == "inspect_music_context") {
        double fromBar = 1.0, toBar = 0.0, segmentBeats = 0.0;
        optNumber(args, "fromBar", fromBar, err);
        optNumber(args, "toBar", toBar, err);
        optNumber(args, "segmentBeats", segmentBeats, err);
        if (!err.empty()) return fail(err);
        if (fromBar < 1.0) return fail("bars start at 1, not 0");
        if (toBar > 0.0 && toBar <= fromBar)
            return fail("toBar must be after fromBar");
        if (segmentBeats < 0.0)
            return fail("segmentBeats cannot be negative");
        return done(buildProjectMusicContext(c, fromBar, toBar, segmentBeats)
                        .toJson());
    }

    if (name == "compose_candidates") {
        std::string roleName, requestedScale;
        double fromBar = secondsToBars(c, c.positionSeconds()) + 1.0;
        double barsValue = 4.0, variations = 3.0, seed = 0.0;
        double creativity = 0.5, density = 0.5;
        double keyRoot = -1.0, lowest = -1.0, highest = -1.0;
        wantString(args, "role", roleName, err);
        optString(args, "scale", requestedScale, err);
        optNumber(args, "fromBar", fromBar, err);
        optNumber(args, "bars", barsValue, err);
        optNumber(args, "variations", variations, err);
        optNumber(args, "seed", seed, err);
        optNumber(args, "creativity", creativity, err);
        optNumber(args, "density", density, err);
        optNumber(args, "keyRoot", keyRoot, err);
        optNumber(args, "lowestPitch", lowest, err);
        optNumber(args, "highestPitch", highest, err);
        if (!err.empty()) return fail(err);
        const std::optional<CompositionRole> role = compositionRole(roleName);
        if (!role) return fail("role must be melody, bass, chords or drums");
        if (fromBar < 1.0) return fail("bars start at 1, not 0");
        if (std::abs(barsValue - std::round(barsValue)) > 1e-6)
            return fail("bars must be a whole number");
        if (seed < 0.0) return fail("seed cannot be negative");
        if ((lowest >= 0.0) != (highest >= 0.0))
            return fail("send both lowestPitch and highestPitch, or neither");
        if (!ctx.compositionCandidates)
            return fail("the composition candidate store is unavailable; "
                        "reopen the AI panel and try again");

        CompositionRequest request;
        request.role = *role;
        request.bars = int(std::llround(barsValue));
        request.variationCount = int(std::llround(variations));
        request.seed = args.contains("seed")
                           ? std::uint64_t(std::llround(seed))
                           : ctx.compositionCandidates->nextSeed(
                                 c.projectRevision() + 1);
        request.creativity = creativity;
        request.rhythmicDensity = density;
        request.beatsPerBar = beatsPerBar(c);
        if (lowest >= 0.0)
            request.pitchRange = CompositionPitchRange{
                int(std::llround(lowest)), int(std::llround(highest))};

        const ProjectMusicContext music = buildProjectMusicContext(
            c, fromBar, fromBar + std::max(1, request.bars));
        const MusicKeySummary& inferred =
            music.detectedMidiKey.available() ? music.detectedMidiKey
                                              : music.globalKey;
        if (*role == CompositionRole::Melody) {
            request.avoidOnsetProfile16.assign(16, 0.0);
            for (const MusicTrackSummary& track : music.tracks) {
                if (track.muted || track.activity.onsetProfile16.size() != 16)
                    continue;
                for (std::size_t slot = 0; slot < 16; ++slot)
                    request.avoidOnsetProfile16[slot] = std::max(
                        request.avoidOnsetProfile16[slot],
                        track.activity.onsetProfile16[slot]);
            }
        }
        if (keyRoot >= 0.0)
            request.keyRoot = int(std::llround(keyRoot));
        else if (inferred.available())
            request.keyRoot = inferred.root;
        request.scale = !requestedScale.empty()
                            ? std::optional<std::string>(requestedScale)
                            : inferred.available()
                                  ? std::optional<std::string>(inferred.scale)
                                  : std::nullopt;
        for (const MusicChordSummary& chord : music.chords) {
            CompositionHarmonySegment segment;
            segment.startBeats =
                std::max(0.0, (chord.startBar - fromBar) * request.beatsPerBar);
            segment.lengthBeats = chord.lengthBeats;
            segment.root = chord.root;
            segment.chordTonePitchClasses = chord.chordTonePitchClasses;
            const double total = request.bars * request.beatsPerBar;
            if (segment.startBeats >= total) continue;
            segment.lengthBeats =
                std::min(segment.lengthBeats, total - segment.startBeats);
            request.harmony.push_back(std::move(segment));
        }

        const CompositionValidation validation =
            validateCompositionRequest(request);
        if (!validation.valid()) return fail(join(validation.errors));
        const std::vector<CompositionCandidate> candidates =
            generateCompositionCandidates(request);
        if (candidates.empty())
            return fail("no valid composition candidates were generated");
        ctx.compositionCandidates->replace(request, candidates);

        json alternatives = json::array();
        for (const CompositionCandidate& candidate : candidates)
            alternatives.push_back(candidateJson(candidate));
        return done(json{{"role", roleName},
                         {"fromBar", round2(fromBar)},
                         {"bars", request.bars},
                         {"seed", request.seed},
                         {"keyRoot", request.keyRoot.value_or(0)},
                         {"scale", request.scale.value_or("major")},
                         {"keySource", inferred.source},
                         {"harmonySegments", request.harmony.size()},
                         {"candidates", std::move(alternatives)},
                         {"next", "Present these alternatives, then apply only the candidate the user chooses."}});
    }

    if (name == "apply_composition_candidate") {
        std::string candidateId, trackId;
        double startBar = secondsToBars(c, c.positionSeconds()) + 1.0;
        wantString(args, "candidateId", candidateId, err);
        wantString(args, "trackId", trackId, err);
        optNumber(args, "startBar", startBar, err);
        if (!err.empty()) return fail(err);
        if (!ctx.compositionCandidates)
            return fail("there are no composition candidates in this panel");
        const std::optional<StoredCompositionCandidate> stored =
            ctx.compositionCandidates->find(candidateId);
        if (!stored)
            return fail("candidateId '" + candidateId +
                        "' is unknown or was replaced by a newer generation");
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!trackAccepts(track->kind, ClipKind::Midi))
            return fail("track '" + track->name + "' cannot hold MIDI clips");
        if (startBar < 1.0) return fail("bars start at 1, not 0");
        const CompositionValidation validation = validateCompositionCandidate(
            stored->request, stored->candidate);
        if (!validation.valid())
            return fail("stored candidate failed validation: " +
                        join(validation.errors));

        const std::string clipId = c.addMidiClip(
            trackId, barsToSeconds(c, startBar - 1.0),
            barsToSeconds(c, stored->request.bars));
        if (clipId.empty()) return fail("the MIDI clip could not be created");
        std::vector<NoteModel> notes;
        notes.reserve(stored->candidate.notes.size());
        for (const CompositionNoteEvent& event : stored->candidate.notes) {
            NoteModel note;
            note.pitch = event.pitch;
            note.startBeats = event.startBeats;
            note.lengthBeats = event.lengthBeats;
            note.velocity = event.velocity;
            notes.push_back(std::move(note));
        }
        c.setClipNotes(trackId, clipId, std::move(notes),
                       "AI: Apply composition candidate");
        c.setClipName(trackId, clipId,
                      "AI " + std::string(compositionRoleName(stored->request.role)) +
                          " " + std::to_string(stored->candidate.variationIndex + 1));
        return done(json{{"candidateId", candidateId},
                         {"trackId", trackId},
                         {"clipId", clipId},
                         {"startBar", round2(startBar)},
                         {"bars", stored->request.bars},
                         {"noteCount", stored->candidate.notes.size()},
                         {"score", scoreJson(stored->candidate.score)}});
    }

    if (name == "get_clip_notes") {
        std::string trackId, clipId;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        if (!err.empty()) return fail(err);
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) return fail("no clip with id '" + clipId + "' on that track");

        json notes = json::array();
        for (const NoteModel& n : clip->notes)
            notes.push_back(json{{"pitch", n.pitch},
                                 {"name", miditools::pitchName(n.pitch)},
                                 {"start", round2(n.startBeats)},
                                 {"length", round2(n.lengthBeats)},
                                 {"velocity", n.velocity}});
        return done(json{{"clipId", clipId},
                         {"lengthBeats",
                          round2(secondsToBeats(clip->durationSeconds, c.tempo()))},
                         {"notes", std::move(notes)}});
    }

    if (name == "list_plugins") {
        std::string kind, filter;
        wantString(args, "kind", kind, err);
        optString(args, "nameContains", filter, err);
        if (!err.empty()) return fail(err);
        if (kind != "instrument" && kind != "effect" && kind != "all")
            return fail("'kind' must be instrument, effect or all");

        std::vector<plugins::PluginDescriptor> found =
            kind == "instrument" ? c.pluginManager().instruments()
            : kind == "effect"   ? c.pluginManager().effects()
                                 : c.pluginManager().plugins();

        std::string lowered = filter;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });

        json list = json::array();
        for (const plugins::PluginDescriptor& d : found) {
            if (!lowered.empty()) {
                std::string lowerName = d.name;
                std::transform(
                    lowerName.begin(), lowerName.end(), lowerName.begin(),
                    [](unsigned char ch) { return char(std::tolower(ch)); });
                if (lowerName.find(lowered) == std::string::npos) continue;
            }
            list.push_back(json{{"format", toString(d.format)},
                                {"uid", d.uid},
                                {"name", d.name},
                                {"vendor", d.vendor},
                                {"isInstrument", d.isInstrument}});
        }
        json out{{"plugins", std::move(list)}};
        if (out["plugins"].empty())
            out["note"] =
                "nothing is installed for that filter. The built-in sampler "
                "(format \"internal\", uid \"daw.sampler\") is always "
                "available, and load_sampler puts it on a track with a file "
                "in one step.";
        return done(std::move(out));
    }

    if (name == "search_files") {
        std::string query;
        std::string type = "all";
        double limit = 40.0;
        wantString(args, "query", query, err);
        optString(args, "type", type, err);
        optNumber(args, "limit", limit, err);
        if (!err.empty()) return fail(err);
        if (type != "all" && type != "audio" && type != "midi")
            return fail("type must be audio, midi or all");
        if (ctx.sampleFolders.empty())
            return fail("the user has not added any folders to the browser, so "
                        "there is nowhere to search. Ask them to add one, or "
                        "use an installed instrument plugin instead.");
        if (!ctx.contentCatalog)
            return fail("the content index is unavailable; reopen the AI panel "
                        "and try again");

        const std::size_t want =
            std::size_t(std::clamp(limit, 1.0, double(kSearchLimit)));
        std::optional<ContentType> filter;
        if (type == "audio") filter = ContentType::Audio;
        if (type == "midi") filter = ContentType::Midi;
        std::vector<ContentItem> matches =
            ctx.contentCatalog->search(query, filter, want + 1);
        const CatalogIndexStatus status = ctx.contentCatalog->status();
        const bool truncated = matches.size() > want;
        if (truncated) matches.resize(want);
        json files = json::array();
        for (const ContentItem& item : matches)
            files.push_back(contentJson(item));
        json indexed{{"files", std::move(files)},
                     {"indexState", toString(status.state)},
                     {"indexedFiles", status.filesPublished}};
        if (const std::optional<double> progress = status.progress())
            indexed["indexProgress"] = round2(*progress);
        if (status.running()) indexed["partial"] = true;
        if (truncated) indexed["truncated"] = true;
        if (indexed["files"].empty() && status.running())
            indexed["note"] =
                "the library index is still building and no published item "
                "matches yet. Tell the user it is indexing, or continue with "
                "an installed instrument.";
        else if (indexed["files"].empty())
            indexed["note"] =
                "nothing matched. Try a shorter query, an extension like "
                "'.wav', or type 'all'.";
        return done(std::move(indexed));

    }

    if (name == "list_plugin_parameters") {
        std::string channelId, insertId;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId))
            return fail(unknownTrack(c, channelId));

        const std::vector<plugins::ParameterInfo> params =
            c.insertParameters(channelId, insertId);
        if (params.empty())
            return fail("no loaded plugin with slot id '" + insertId +
                        "' on that channel, or it has no parameters. Slot ids "
                        "come from get_project.");

        json list = json::array();
        for (const plugins::ParameterInfo& p : params)
            list.push_back(
                json{{"id", p.id},
                     {"name", p.name},
                     {"min", round2(p.minValue)},
                     {"max", round2(p.maxValue)},
                     {"unit", p.unit},
                     {"current", round2(c.insertParameter(channelId, insertId, p.id))}});
        return done(json{{"parameters", std::move(list)}});
    }

    if (name == "analyze_track") {
        std::string channelId;
        double fromBar = 1.0, toBar = 0.0;
        wantString(args, "channelId", channelId, err);
        optNumber(args, "fromBar", fromBar, err);
        optNumber(args, "toBar", toBar, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        analysis::Metrics metrics;
        const audio::Result result = c.analyzeChannel(
            channelId, barsToSeconds(c, std::max(0.0, fromBar - 1.0)),
            toBar > 0.0 ? barsToSeconds(c, toBar - 1.0) : 0.0, metrics);
        if (!result.isOk())
            return fail(std::string("could not measure that channel: ") +
                        result.message());
        if (metrics.silent)
            return done(json{{"silent", true},
                             {"note",
                              "that channel makes no sound over the range — "
                              "check it has clips, an instrument, and is not "
                              "muted"}});

        json out{{"peakDb", round2(metrics.peakDb)},
                 {"rmsDb", round2(metrics.rmsDb)},
                 {"headroomDb", round2(-metrics.peakDb)},
                 {"lowShare", round2(metrics.lowFraction)},
                 {"midShare", round2(metrics.midFraction)},
                 {"highShare", round2(metrics.highFraction)},
                 {"seconds", round2(metrics.seconds)}};
        if (metrics.clipped > 0) {
            out["clippedSamples"] = metrics.clipped;
            out["warning"] =
                "this channel goes past full scale and is being damaged — "
                "bring its level down, or the levels feeding it";
        }
        return done(std::move(out));
    }

    if (name == "analyze_sample") {
        const std::optional<std::string> input =
            authorizedInputPath(args, ctx, err);
        if (!input) return fail(err);
        const std::string& filePath = *input;

        analysis::SampleTraits traits;
        const audio::Result result = c.analyzeSampleFile(filePath, traits);
        if (!result.isOk())
            return fail("could not read '" + privateFileLabel(filePath) +
                        "': " + result.message());

        json out{{"seconds", round2(traits.level.seconds)},
                 {"peakDb", round2(traits.level.peakDb)},
                 {"character", traits.character},
                 {"attackSeconds", round2(traits.attackSeconds)},
                 {"lowShare", round2(traits.level.lowFraction)},
                 {"midShare", round2(traits.level.midFraction)},
                 {"highShare", round2(traits.level.highFraction)}};
        if (traits.tonal) {
            out["pitch"] = traits.pitch;
            out["pitchName"] = miditools::pitchName(traits.pitch);
            out["note"] =
                "it holds a pitch, so set the sampler's root note to " +
                std::to_string(traits.pitch) +
                " if you want it to play in tune from the keyboard";
        }
        return done(std::move(out));
    }

    // ── Project ──
    if (name == "set_tempo") {
        double bpm = 0.0;
        wantNumber(args, "bpm", bpm, err);
        if (!err.empty()) return fail(err);
        if (bpm < 20.0 || bpm > 300.0)
            return fail("tempo must be between 20 and 300 BPM");
        c.setTempo(bpm);
        return done(json{{"tempo", round2(c.tempo())}});
    }

    if (name == "set_project_key") {
        std::string root, scale;
        wantString(args, "root", root, err);
        wantString(args, "scale", scale, err);
        if (!err.empty()) return fail(err);

        bool ok = false;
        const int pitchClass = pitchClassFromName(root, ok);
        if (!ok)
            return fail("'" + root +
                        "' is not a note name. Use C, C#, Db, D, Eb, E, F, "
                        "F#, G, Ab, A, Bb or B.");
        const miditools::Scale parsed = miditools::scaleFromId(scale);
        if (miditools::scaleId(parsed) != scale && scale != "minor")
            return fail("'" + scale +
                        "' is not a scale. Use major, natural_minor, "
                        "harmonic_minor, melodic_minor, dorian, phrygian, "
                        "lydian, mixolydian, locrian, pentatonic_major, "
                        "pentatonic_minor, blues, whole_tone or chromatic.");
        c.setProjectKey(pitchClass, miditools::scaleId(parsed));
        return done(json{{"key", root + " " + miditools::scaleName(parsed)}});
    }

    if (name == "transform_notes") {
        std::string trackId, clipId, operation;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        wantString(args, "operation", operation, err);
        if (!err.empty()) return fail(err);
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) return fail("no clip with id '" + clipId + "' on that track");
        if (clip->kind != ClipKind::Midi)
            return fail("clip '" + clip->name + "' holds audio, not notes");
        if (clip->notes.empty())
            return fail("that clip has no notes yet — write them with "
                        "set_clip_notes first");

        double amount = 1.0, factor = 1.0, beats = 0.25;
        double grid = 16.0, semitones = 0.0, low = 0.0, high = 127.0;
        double from = 40.0, to = 110.0, velocity = 100.0, degrees = 0.0;
        bool triplet = false;
        std::string chord = "major";
        optNumber(args, "amount", amount, err);
        optNumber(args, "factor", factor, err);
        optNumber(args, "beats", beats, err);
        optNumber(args, "grid", grid, err);
        optNumber(args, "semitones", semitones, err);
        optNumber(args, "low", low, err);
        optNumber(args, "high", high, err);
        optNumber(args, "from", from, err);
        optNumber(args, "to", to, err);
        optBool(args, "triplet", triplet, err);
        optString(args, "chord", chord, err);
        optNumber(args, "velocity", velocity, err);
        optNumber(args, "degrees", degrees, err);
        if (!err.empty()) return fail(err);
        amount = std::clamp(amount, 0.0, 1.0);

        const miditools::Scale scale = miditools::scaleFromId(c.projectScale());
        const double clipBeats = secondsToBeats(clip->durationSeconds, c.tempo());
        miditools::Notes notes = clip->notes;

        if (operation == "quantize") {
            miditools::QuantizeParams params;
            params.gridBeats = miditools::gridBeatsFor(
                int(grid), triplet ? miditools::GridFlavour::Triplet
                                   : miditools::GridFlavour::Straight);
            params.strength = amount;
            notes = miditools::quantize(std::move(notes), params);
        } else if (operation == "humanize") {
            // Scaled off `amount` so one number covers it: at 1 this is a
            // 32nd of timing spread and ±14 of velocity, which is about as far
            // as "played rather than typed" goes before it sounds sloppy.
            notes = miditools::humanize(std::move(notes), 0.03 * amount,
                                        int(14 * amount), 1);
        } else if (operation == "legato") {
            notes = miditools::legato(std::move(notes), clip->notes);
        } else if (operation == "articulate") {
            miditools::ArticulateParams params;
            params.amount = amount;
            params.accentEvery = 4;
            notes = miditools::articulate(std::move(notes), params);
        } else if (operation == "strum") {
            miditools::StrumParams params;
            params.spanBeats = beats > 0.0 ? beats : 0.125;
            notes = miditools::strum(std::move(notes), params);
        } else if (operation == "transpose") {
            notes = miditools::transpose(std::move(notes), int(semitones));
        } else if (operation == "snap_to_scale") {
            notes = miditools::snapToScale(std::move(notes), c.keyRoot(), scale);
        } else if (operation == "limit_pitch") {
            if (low >= high) return fail("'low' must be below 'high'");
            notes = miditools::limitPitch(std::move(notes), int(low), int(high));
        } else if (operation == "invert_pitch") {
            notes = miditools::invertPitch(std::move(notes));
        } else if (operation == "reverse_time") {
            notes = miditools::reverseTime(std::move(notes));
        } else if (operation == "ramp_velocity") {
            notes = miditools::rampVelocity(std::move(notes), int(from), int(to));
        } else if (operation == "scale_velocity") {
            notes = miditools::scaleVelocity(std::move(notes), factor);
        } else if (operation == "scale_length") {
            notes = miditools::scaleLength(std::move(notes), factor);
        } else if (operation == "nudge") {
            notes = miditools::nudge(std::move(notes), beats);
        } else if (operation == "rotate") {
            notes = miditools::rotate(std::move(notes), beats, clipBeats);
        } else if (operation == "transpose_in_scale") {
            notes = miditools::transposeInScale(std::move(notes), int(degrees),
                                                c.keyRoot(), scale);
        } else if (operation == "set_velocity") {
            notes = miditools::setVelocity(std::move(notes), int(velocity));
        } else if (operation == "add_velocity") {
            notes = miditools::addVelocity(std::move(notes), int(velocity));
        } else if (operation == "set_length") {
            if (!(beats > 0.0))
                return fail("set_length needs 'beats' above 0");
            notes = miditools::setLength(std::move(notes), beats);
        } else if (operation == "arpeggiate") {
            miditools::ArpParams params;
            // Sixteenths unless asked otherwise: the rate an arp is written at
            // far more often than any other.
            params.rateBeats = beats > 0.0 ? beats : 0.25;
            notes = miditools::arpeggiate(notes, params, clipBeats);
        } else if (operation == "glue") {
            notes = miditools::glue(std::move(notes), {});
        } else if (operation == "randomize") {
            miditools::RandomParams params;
            params.timing = true;
            params.timingBeats = 0.05 * amount;
            params.velocity = true;
            params.velocityAmount = int(20 * amount);
            notes = miditools::randomize(std::move(notes), params);
        } else if (operation == "split_at_grid") {
            const double gridBeats = miditools::gridBeatsFor(
                int(grid), triplet ? miditools::GridFlavour::Triplet
                                   : miditools::GridFlavour::Straight);
            notes = miditools::splitAtGrid(std::move(notes), gridBeats);
        } else if (operation == "build_chords") {
            bool ok = false;
            miditools::ChordParams params;
            params.type = chordTypeFromId(chord, ok);
            if (!ok)
                return fail("'" + chord +
                            "' is not a chord type. Use major, minor, "
                            "diminished, augmented, major7, minor7, "
                            "dominant7, sus2, sus4, add9, major9, minor9 or "
                            "power.");
            notes = miditools::buildChords(notes, params);
        } else {
            return fail("'" + operation + "' is not a transform");
        }

        const std::size_t count = notes.size();
        c.setClipNotes(trackId, clipId, std::move(notes),
                       "AI: " + operation);
        return done(json{{"operation", operation}, {"noteCount", count}});
    }

    if (name == "set_time_signature") {
        double num = 0.0, den = 0.0;
        wantNumber(args, "numerator", num, err);
        wantNumber(args, "denominator", den, err);
        if (!err.empty()) return fail(err);
        const int n = int(num), d = int(den);
        if (n < 1 || n > 32) return fail("numerator must be between 1 and 32");
        if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32)
            return fail("denominator must be 1, 2, 4, 8, 16 or 32");
        c.setTimeSignature(n, d);
        return done(json{{"timeSignature",
                          std::to_string(n) + "/" + std::to_string(d)}});
    }

    // ── Tracks ──
    if (name == "add_track") {
        std::string kind, trackName;
        wantString(args, "kind", kind, err);
        wantString(args, "name", trackName, err);
        if (!err.empty()) return fail(err);
        if (kind == "master")
            return fail("the master channel already exists; use channelId "
                        "\"master\" to work on it");
        const TrackKind parsed = trackKindFromString(kind);
        // `trackKindFromString` falls back to Audio, so an unknown word would
        // silently make the wrong kind of track — check the round trip.
        if (toString(parsed) != kind)
            return fail("unknown track kind '" + kind +
                        "'. Use instrument, audio, midi, bus, aux, group or "
                        "folder.");
        const std::string id = c.addTrack(parsed, trackName);
        if (id.empty()) return fail("the track could not be created");
        return done(json{{"trackId", id}, {"name", trackName}, {"kind", kind}});
    }

    if (name == "remove_track") {
        std::string trackId;
        wantString(args, "trackId", trackId, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!allowed(ctx, "delete the track \"" + track->name + "\" and " +
                              std::to_string(track->clips.size()) +
                              " clip(s) on it"))
            return fail(kRefused);
        c.removeTrack(trackId);
        return done();
    }

    if (name == "rename_track") {
        std::string trackId, trackName;
        wantString(args, "trackId", trackId, err);
        wantString(args, "name", trackName, err);
        if (!err.empty()) return fail(err);
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        c.renameTrack(trackId, trackName);
        return done(json{{"name", trackName}});
    }

    if (name == "set_track_mix") {
        std::string trackId;
        wantString(args, "trackId", trackId, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));

        json applied = json::object();
        double db = 0.0;
        if (optNumber(args, "volumeDb", db, err)) {
            if (db > 6.0) return fail("volumeDb cannot exceed +6");
            c.setTrackVolume(trackId, float(dbToGain(db)));
            applied["volumeDb"] = round2(db);
        }
        double pan = 0.0;
        if (optNumber(args, "pan", pan, err)) {
            if (pan < -1.0 || pan > 1.0)
                return fail("pan must be between -1 and 1");
            c.setTrackPan(trackId, float(pan));
            applied["pan"] = round2(pan);
        }
        bool flag = false;
        if (optBool(args, "muted", flag, err)) {
            c.setTrackMuted(trackId, flag);
            applied["muted"] = flag;
        }
        if (optBool(args, "soloed", flag, err)) {
            c.setTrackSoloed(trackId, flag);
            applied["soloed"] = flag;
        }
        if (!err.empty()) return fail(err);
        if (applied.empty())
            return fail("nothing to change: send at least one of volumeDb, "
                        "pan, muted or soloed");
        return done(std::move(applied));
    }

    if (name == "add_send") {
        std::string trackId, destId;
        double level = 0.0;
        bool preFader = false;
        wantString(args, "trackId", trackId, err);
        wantString(args, "destinationTrackId", destId, err);
        wantNumber(args, "level", level, err);
        optBool(args, "preFader", preFader, err);
        if (!err.empty()) return fail(err);
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        if (!c.project().findTrack(destId)) return fail(unknownTrack(c, destId));
        if (level < 0.0 || level > 1.0)
            return fail("level must be between 0 and 1");

        const std::string sendId = c.addSend(trackId, destId);
        if (sendId.empty())
            return fail("that send would feed a signal back into itself");
        c.setSendLevel(trackId, sendId, float(level));
        if (preFader) c.setSendPreFader(trackId, sendId, true);
        return done(json{{"sendId", sendId}});
    }

    // ── Instruments ──
    if (name == "set_track_instrument") {
        std::string trackId, format, uid;
        wantString(args, "trackId", trackId, err);
        wantString(args, "format", format, err);
        wantString(args, "uid", uid, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!trackAccepts(track->kind, ClipKind::Midi))
            return fail("track '" + track->name + "' is " +
                        toString(track->kind) +
                        " and cannot hold an instrument. Make an instrument "
                        "track with add_track.");

        const auto descriptor =
            findPlugin(c, format, uid, err);
        if (!descriptor) return fail(err);
        c.setTrackInstrumentPlugin(trackId, *descriptor);

        const TrackModel* after = c.project().findTrack(trackId);
        if (!after || !after->instrument.isLoaded())
            return fail("'" + descriptor->name +
                        "' failed to load — it may be broken or blacklisted");
        return done(json{{"insertId", after->instrument.id},
                         {"name", after->instrument.name}});
    }

    if (name == "load_sampler") {
        std::string trackId;
        wantString(args, "trackId", trackId, err);
        if (!err.empty()) return fail(err);
        const std::optional<std::string> input =
            authorizedInputPath(args, ctx, err);
        if (!input) return fail(err);
        const std::string& filePath = *input;
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!trackAccepts(track->kind, ClipKind::Midi))
            return fail("track '" + track->name +
                        "' cannot hold an instrument. Make an instrument track "
                        "with add_track.");
        if (!c.loadInstrumentSampler(trackId, filePath))
            return fail("could not load '" + privateFileLabel(filePath) +
                        "' — check the path, and that it is an audio file this "
                        "program can decode");

        const TrackModel* after = c.project().findTrack(trackId);
        return done(json{{"insertId", after ? after->instrument.id : ""}});
    }

    // ── Clips ──
    if (name == "add_midi_clip") {
        std::string trackId;
        double startBar = 1.0, lengthBars = 0.0;
        wantString(args, "trackId", trackId, err);
        wantNumber(args, "startBar", startBar, err);
        wantNumber(args, "lengthBars", lengthBars, err);
        if (!err.empty()) return fail(err);
        const TrackModel* track = c.project().findTrack(trackId);
        if (!track) return fail(unknownTrack(c, trackId));
        if (!trackAccepts(track->kind, ClipKind::Midi))
            return fail("track '" + track->name + "' is " +
                        toString(track->kind) +
                        " and cannot hold MIDI clips");
        if (startBar < 1.0) return fail("startBar starts at 1, not 0");
        if (lengthBars <= 0.0) return fail("lengthBars must be greater than 0");

        const std::string clipId =
            c.addMidiClip(trackId, barsToSeconds(c, startBar - 1.0),
                          barsToSeconds(c, lengthBars));
        if (clipId.empty()) return fail("the clip could not be created");
        return done(json{{"clipId", clipId},
                         {"lengthBeats", round2(lengthBars * beatsPerBar(c))}});
    }

    if (name == "set_clip_notes") {
        std::string trackId, clipId, mode = "replace";
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        optString(args, "mode", mode, err);
        if (!err.empty()) return fail(err);
        if (mode != "replace" && mode != "append")
            return fail("'mode' must be replace or append");
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) return fail("no clip with id '" + clipId + "' on that track");
        if (clip->kind != ClipKind::Midi)
            return fail("clip '" + clip->name + "' holds audio, not notes");

        json noteList;
        wantArray(args, "notes", noteList, err);
        if (!err.empty()) return fail(err + " of notes");

        const double clipBeats =
            secondsToBeats(clip->durationSeconds, c.tempo());
        std::vector<NoteModel> notes;
        if (mode == "append") notes = clip->notes;

        double lastEnd = 0.0;
        for (const json& raw : noteList) {
            if (!raw.is_object()) return fail("every note must be an object");
            std::string noteErr;
            double pitch = 0.0, start = 0.0, length = 0.0, velocity = 100.0;
            wantNumber(raw, "pitch", pitch, noteErr);
            wantNumber(raw, "start", start, noteErr);
            wantNumber(raw, "length", length, noteErr);
            optNumber(raw, "velocity", velocity, noteErr);
            if (!noteErr.empty()) return fail("in one of the notes: " + noteErr);
            if (pitch < 0.0 || pitch > 127.0)
                return fail("pitch must be between 0 and 127");
            if (start < 0.0) return fail("a note cannot start before the clip");
            if (length <= 0.0) return fail("a note's length must be above 0");

            NoteModel note;
            note.pitch = int(pitch);
            note.startBeats = start;
            note.lengthBeats = length;
            note.velocity = std::clamp(int(velocity), 1, 127);
            lastEnd = std::max(lastEnd, start + length);
            notes.push_back(note);
        }

        // A part longer than the clip it was written into is the commonest way
        // for the assistant's work to vanish — notes at or past the end never
        // sound. The notes are the intent, so the clip grows to hold them,
        // rounded up to a whole bar and never shrunk. Same reasoning as
        // `importMidiFile`, and it rides the same single undo entry.
        double grewToBars = 0.0;
        if (lastEnd > clipBeats) {
            const double perBar = beatsPerBar(c);
            const double bars = std::ceil(lastEnd / perBar);
            grewToBars = std::max(1.0, bars);
            c.setClipTrim(trackId, clipId, clip->startSeconds, clip->offsetSeconds,
                          barsToSeconds(c, grewToBars));
        }

        const std::size_t count = notes.size();
        c.setClipNotes(trackId, clipId, std::move(notes), "AI: Write Notes");

        json out{{"noteCount", count},
                 {"clipLengthBeats",
                  round2(grewToBars > 0.0 ? grewToBars * beatsPerBar(c) : clipBeats)}};
        if (grewToBars > 0.0)
            out["note"] = "the clip was lengthened to " +
                          std::to_string(int(grewToBars)) +
                          " bar(s) so the whole part sounds";
        return done(std::move(out));
    }

    if (name == "set_clip_start") {
        std::string trackId, clipId;
        double startBar = 1.0;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        wantNumber(args, "startBar", startBar, err);
        if (!err.empty()) return fail(err);
        if (!findClip(c, trackId, clipId))
            return fail("no clip with id '" + clipId + "' on track '" + trackId + "'");
        if (startBar < 1.0) return fail("startBar starts at 1, not 0");
        c.setClipStartSeconds(trackId, clipId, barsToSeconds(c, startBar - 1.0));
        return done(json{{"startBar", round2(startBar)}});
    }

    if (name == "duplicate_clip") {
        std::string trackId, clipId;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        if (!err.empty()) return fail(err);
        if (!findClip(c, trackId, clipId))
            return fail("no clip with id '" + clipId + "' on track '" + trackId + "'");
        const std::string copy = c.duplicateClip(trackId, clipId);
        if (copy.empty()) return fail("the clip could not be duplicated");
        const ClipModel* made = findClip(c, trackId, copy);
        return done(json{{"clipId", copy},
                         {"startBar",
                          made ? round2(secondsToBars(c, made->startSeconds) + 1.0)
                               : 0.0}});
    }

    if (name == "remove_clip") {
        std::string trackId, clipId;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        if (!err.empty()) return fail(err);
        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip)
            return fail("no clip with id '" + clipId + "' on track '" + trackId + "'");
        if (!allowed(ctx, "delete the clip \"" + clip->name + "\""))
            return fail(kRefused);
        c.removeClip(trackId, clipId);
        return done();
    }

    // ── Effects ──
    if (name == "add_insert") {
        std::string channelId, format, uid;
        double index = -1.0;
        wantString(args, "channelId", channelId, err);
        wantString(args, "format", format, err);
        wantString(args, "uid", uid, err);
        optNumber(args, "index", index, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        const auto descriptor =
            findPlugin(c, format, uid, err);
        if (!descriptor) return fail(err);
        if (descriptor->isInstrument)
            return fail("'" + descriptor->name +
                        "' is an instrument, not an effect — put it in the "
                        "track's instrument slot with set_track_instrument");

        const std::size_t at =
            index >= 0.0 ? std::size_t(index) : std::size_t(-1);
        const std::string insertId = c.addInsert(channelId, *descriptor, at);
        if (insertId.empty())
            return fail("'" + descriptor->name +
                        "' failed to load — it may be broken or blacklisted");
        return done(json{{"insertId", insertId}, {"name", descriptor->name}});
    }

    if (name == "remove_insert") {
        std::string channelId, insertId;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));
        if (!allowed(ctx, "remove an effect from that channel"))
            return fail(kRefused);
        c.removeInsert(channelId, insertId);
        return done();
    }

    if (name == "set_insert_bypassed") {
        std::string channelId, insertId;
        bool bypassed = false;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        if (!args.contains("bypassed")) return fail("missing required argument 'bypassed'");
        optBool(args, "bypassed", bypassed, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));
        c.setInsertBypassed(channelId, insertId, bypassed);
        return done(json{{"bypassed", bypassed}});
    }

    if (name == "set_insert_parameter") {
        std::string channelId, insertId, parameterId;
        double value = 0.0;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        wantString(args, "parameterId", parameterId, err);
        wantNumber(args, "value", value, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        const std::vector<plugins::ParameterInfo> params =
            c.insertParameters(channelId, insertId);
        if (params.empty())
            return fail("no loaded plugin with slot id '" + insertId +
                        "' on that channel");

        const auto found = std::find_if(
            params.begin(), params.end(),
            [&](const plugins::ParameterInfo& p) { return p.id == parameterId; });
        if (found == params.end()) {
            // Name the alternatives: this is the error the model is most likely
            // to hit, and the one it can most usefully correct itself from.
            std::string known;
            for (const plugins::ParameterInfo& p : params) {
                if (known.size() > 800) { known += ", …"; break; }
                if (!known.empty()) known += ", ";
                known += p.id;
            }
            return fail("'" + parameterId +
                        "' is not a parameter of that plugin. Its parameters "
                        "are: " + known);
        }
        if (value < found->minValue || value > found->maxValue)
            return fail("'" + parameterId + "' takes " +
                        std::to_string(found->minValue) + " to " +
                        std::to_string(found->maxValue));

        const double before = c.insertParameter(channelId, insertId, parameterId);
        c.setInsertParameter(channelId, insertId, parameterId, value);
        // The live setter is deliberately not undoable — it is what a knob drag
        // calls — so the edit is committed here, or the assistant's changes
        // would not come back with Ctrl+Z.
        c.commitInsertParameterEdit(channelId, insertId, parameterId, before,
                                    "AI: " + found->name);
        return done(json{{"parameterId", parameterId}, {"value", value}});
    }

    if (name == "set_insert_parameters") {
        std::string channelId, insertId;
        wantString(args, "channelId", channelId, err);
        wantString(args, "insertId", insertId, err);
        json list;
        wantArray(args, "parameters", list, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        const std::vector<plugins::ParameterInfo> params =
            c.insertParameters(channelId, insertId);
        if (params.empty())
            return fail("no loaded plugin with slot id '" + insertId +
                        "' on that channel");

        // Checked in full before anything is written: a half-applied setup is
        // worse than a refused one, because the model cannot see which half.
        struct Pending { std::string id; double value; std::string label; };
        std::vector<Pending> pending;
        for (const json& raw : list) {
            if (!raw.is_object()) return fail("every parameter must be an object");
            std::string paramErr, parameterId;
            double value = 0.0;
            wantString(raw, "parameterId", parameterId, paramErr);
            wantNumber(raw, "value", value, paramErr);
            if (!paramErr.empty()) return fail("in one of the parameters: " + paramErr);
            const auto found = std::find_if(
                params.begin(), params.end(),
                [&](const plugins::ParameterInfo& p) { return p.id == parameterId; });
            if (found == params.end()) {
                std::string known;
                for (const plugins::ParameterInfo& p : params) {
                    if (known.size() > 800) { known += ", …"; break; }
                    if (!known.empty()) known += ", ";
                    known += p.id;
                }
                return fail("'" + parameterId +
                            "' is not a parameter of that plugin. Its "
                            "parameters are: " + known);
            }
            if (value < found->minValue || value > found->maxValue)
                return fail("'" + parameterId + "' takes " +
                            std::to_string(found->minValue) + " to " +
                            std::to_string(found->maxValue));
            pending.push_back({parameterId, value, found->name});
        }

        json applied = json::array();
        for (const Pending& p : pending) {
            const double before = c.insertParameter(channelId, insertId, p.id);
            c.setInsertParameter(channelId, insertId, p.id, p.value);
            c.commitInsertParameterEdit(channelId, insertId, p.id, before,
                                        "AI: " + p.label);
            applied.push_back(json{{"parameterId", p.id}, {"value", p.value}});
        }
        return done(json{{"applied", std::move(applied)}});
    }

    // ── Harmony and measurement ──
    if (name == "analyze_harmony") {
        std::string trackId, clipId;
        double fromBar = 1.0, toBar = 0.0, segmentBeats = 0.0;
        optString(args, "trackId", trackId, err);
        optString(args, "clipId", clipId, err);
        optNumber(args, "fromBar", fromBar, err);
        optNumber(args, "toBar", toBar, err);
        optNumber(args, "segmentBeats", segmentBeats, err);
        if (!err.empty()) return fail(err);
        if (!clipId.empty() && trackId.empty())
            return fail("clipId needs the trackId it is on");
        if (!trackId.empty() && !c.project().findTrack(trackId))
            return fail(unknownTrack(c, trackId));

        // Everything is gathered on ONE timeline — beats from the start of the
        // project, not from each clip — or two parts written into clips that
        // start at different bars would be read as sounding together.
        const double perBar = beatsPerBar(c);
        const double fromBeat = std::max(0.0, (fromBar - 1.0) * perBar);
        const double toBeat = toBar > 0.0 ? (toBar - 1.0) * perBar : 0.0;

        std::vector<NoteModel> gathered;
        int clipsRead = 0;
        for (const TrackModel& track : c.project().tracks) {
            if (!trackId.empty() && track.id != trackId) continue;
            for (const ClipModel& clip : track.clips) {
                if (clip.kind != ClipKind::Midi || clip.muted) continue;
                if (!clipId.empty() && clip.id != clipId) continue;
                const double clipStart =
                    secondsToBeats(clip.startSeconds, c.tempo());
                const double clipLength =
                    secondsToBeats(clip.durationSeconds, c.tempo());
                bool used = false;
                for (const NoteModel& note : clip.notes) {
                    if (note.muted) continue;
                    // Notes past the clip's end do not sound, so they must not
                    // colour the reading either.
                    if (note.startBeats >= clipLength) continue;
                    NoteModel moved = note;
                    moved.startBeats = clipStart + note.startBeats;
                    moved.lengthBeats =
                        std::min(note.lengthBeats, clipLength - note.startBeats);
                    const double end = moved.startBeats + moved.lengthBeats;
                    if (end <= fromBeat) continue;
                    if (toBeat > 0.0 && moved.startBeats >= toBeat) continue;
                    gathered.push_back(moved);
                    used = true;
                }
                if (used) ++clipsRead;
            }
        }
        if (gathered.empty()) {
            return done(json{
                {"segments", json::array()},
                {"note",
                 "no MIDI notes sound in that range, so there is no harmony to "
                 "follow — you are choosing the key yourself. Say which one."}});
        }

        const std::vector<miditools::HarmonySegment> segments =
            miditools::analyzeHarmony(gathered, perBar, segmentBeats);
        const miditools::KeyGuess key = miditools::estimateKey(gathered);

        json out = json::array();
        for (const miditools::HarmonySegment& segment : segments) {
            json entry{{"bar", round2(segment.startBeats / perBar + 1.0)},
                       {"lengthBeats", round2(segment.lengthBeats)},
                       {"quality", segment.quality}};
            entry["pitchClasses"] = segment.pitchClasses;
            json pitchClassNames = json::array();
            for (int pitchClass : segment.pitchClasses)
                pitchClassNames.push_back(
                    miditools::pitchClassName(pitchClass));
            entry["pitchClassNames"] = std::move(pitchClassNames);
            if (segment.root >= 0) {
                entry["root"] = miditools::pitchClassName(segment.root);
                entry["rootPitchClass"] = segment.root;
                // The octave a bass part actually lives in, so the model does
                // not have to work it out and get it wrong.
                int bass = segment.root + 36;
                while (bass < 28) bass += 12;
                while (bass > 55) bass -= 12;
                entry["suggestedBassPitch"] = bass;
                entry["suggestedBassNote"] = miditools::pitchName(bass);
            }
            if (segment.bassPitch >= 0) {
                entry["lowestSoundingPitch"] = segment.bassPitch;
                entry["lowestSoundingNote"] = miditools::pitchName(segment.bassPitch);
            }
            entry["confidence"] = round2(segment.confidence);
            out.push_back(std::move(entry));
        }

        json reply{{"clipsRead", clipsRead},
                   {"beatsPerBar", round2(perBar)},
                   {"segments", std::move(out)},
                   {"howToUse",
                    "Write the bass on the root of each segment "
                    "(suggestedBassPitch is that root in bass range), and land "
                    "melody notes on chord tones over each segment."}};
        if (key.root >= 0) {
            reply["key"] = miditools::pitchClassName(key.root) + " " +
                           miditools::scaleName(key.scale);
            reply["keyRoot"] = key.root;
            reply["keyScale"] = miditools::scaleId(key.scale);
            reply["keyConfidence"] = round2(key.confidence);
        }
        return done(std::move(reply));
    }

    if (name == "analyze_mix") {
        double fromBar = 1.0, toBar = 0.0;
        optNumber(args, "fromBar", fromBar, err);
        optNumber(args, "toBar", toBar, err);
        if (!err.empty()) return fail(err);
        const double from = barsToSeconds(c, std::max(0.0, fromBar - 1.0));
        const double to = toBar > 0.0 ? barsToSeconds(c, toBar - 1.0) : 0.0;

        const auto measure = [&](const std::string& channelId,
                                 const std::string& label) -> json {
            analysis::Metrics metrics;
            const audio::Result result =
                c.analyzeChannel(channelId, from, to, metrics);
            json entry{{"channelId", channelId}, {"name", label}};
            if (!result.isOk()) {
                entry["error"] = result.message();
                return entry;
            }
            if (metrics.silent) {
                entry["silent"] = true;
                return entry;
            }
            entry["peakDb"] = round2(metrics.peakDb);
            entry["rmsDb"] = round2(metrics.rmsDb);
            entry["headroomDb"] = round2(-metrics.peakDb);
            entry["lowShare"] = round2(metrics.lowFraction);
            entry["midShare"] = round2(metrics.midFraction);
            entry["highShare"] = round2(metrics.highFraction);
            if (metrics.clipped > 0) entry["clippedSamples"] = metrics.clipped;
            return entry;
        };

        json channels = json::array();
        for (const TrackModel& track : c.project().tracks) {
            // Folders that do not sum and automation lanes carry no audio of
            // their own; measuring them returns silence and wastes the reply.
            if (track.kind == TrackKind::Automation) continue;
            if (track.kind == TrackKind::Folder && !track.summing) continue;
            channels.push_back(measure(track.id, track.name));
        }
        return done(json{
            {"master", measure(EngineController::kMasterChannelId, "Master")},
            {"channels", std::move(channels)},
            {"howToRead",
             "peakDb above -1 is clipping. A master above -3 before mastering "
             "has no headroom left. lowShare above 0.6 on anything that is not "
             "a kick or a bass is what makes a mix muddy."}});
    }

    // ── Transport and project ──
    if (name == "transport") {
        std::string action;
        double bar = 1.0;
        wantString(args, "action", action, err);
        optNumber(args, "bar", bar, err);
        if (!err.empty()) return fail(err);
        if (action == "play") c.play();
        else if (action == "stop") c.stop();
        else if (action == "pause") c.pause();
        else if (action == "metronome_on") c.setMetronomeEnabled(true);
        else if (action == "metronome_off") c.setMetronomeEnabled(false);
        else if (action == "seek") {
            if (bar < 1.0) return fail("bars start at 1, not 0");
            c.seekSeconds(barsToSeconds(c, bar - 1.0));
        } else {
            return fail("'" + action +
                        "' is not a transport action. Use play, stop, pause, "
                        "seek, metronome_on or metronome_off");
        }
        return done(json{{"playing", c.isPlaying()},
                         {"bar", round2(secondsToBars(c, c.positionSeconds()) + 1.0)},
                         {"metronome", c.isMetronomeEnabled()}});
    }

    if (name == "set_loop") {
        bool enabled = false;
        double fromBar = 0.0, toBar = 0.0;
        std::string boolErr;
        optBool(args, "enabled", enabled, boolErr);
        if (args.find("enabled") == args.end())
            return fail("missing required argument 'enabled'");
        if (!boolErr.empty()) return fail(boolErr);
        optNumber(args, "fromBar", fromBar, err);
        optNumber(args, "toBar", toBar, err);
        if (!err.empty()) return fail(err);
        if (enabled && (fromBar > 0.0 || toBar > 0.0)) {
            if (fromBar < 1.0 || toBar <= fromBar)
                return fail("the loop needs fromBar of 1 or more and a toBar "
                            "after it");
            c.setLoopRangeSeconds(barsToSeconds(c, fromBar - 1.0),
                                  barsToSeconds(c, toBar - 1.0));
        }
        c.setLoopEnabled(enabled);
        return done(json{{"enabled", c.isLoopEnabled()},
                         {"fromBar", round2(secondsToBars(c, c.loopStartSeconds()) + 1.0)},
                         {"toBar", round2(secondsToBars(c, c.loopEndSeconds()) + 1.0)}});
    }

    if (name == "undo") {
        if (!c.canUndo()) return fail("there is nothing to undo");
        const std::string label = c.undoLabel();
        c.undo();
        return done(json{{"undone", label}});
    }

    if (name == "redo") {
        if (!c.canRedo()) return fail("there is nothing to redo");
        const std::string label = c.redoLabel();
        c.redo();
        return done(json{{"redone", label}});
    }

    if (name == "save_project") {
        std::string path;
        optString(args, "path", path, err);
        if (!err.empty()) return fail(err);
        if (path.empty()) path = ctx.projectPath;
        if (path.empty())
            return fail("this project has never been saved, so there is "
                        "nowhere to save it to. Ask the user where it should "
                        "go, or let them use File > Save");
        const audio::Result result = c.saveProject(path);
        if (!result.isOk())
            return fail(std::string("the project could not be saved: ") +
                        result.message());
        return done(json{{"path", path}});
    }

    if (name == "export_audio") {
        std::string path;
        bool normalize = false;
        wantString(args, "path", path, err);
        optBool(args, "normalize", normalize, err);
        if (!err.empty()) return fail(err);
        if (!allowed(ctx, "export the project to \"" + path + "\""))
            return fail(kRefused);
        const audio::Result result = c.exportMixdown(path, normalize);
        if (!result.isOk())
            return fail(std::string("the export failed: ") + result.message());
        return done(json{{"path", path}, {"normalized", normalize}});
    }

    if (name == "import_audio") {
        std::string trackId, trackName;
        double atBar = 1.0;
        optString(args, "trackId", trackId, err);
        optString(args, "trackName", trackName, err);
        optNumber(args, "atBar", atBar, err);
        if (!err.empty()) return fail(err);
        const std::optional<std::string> input =
            authorizedInputPath(args, ctx, err);
        if (!input) return fail(err);
        const std::string& filePath = *input;
        if (atBar < 1.0) return fail("bars start at 1, not 0");
        if (!trackId.empty() && !c.project().findTrack(trackId))
            return fail(unknownTrack(c, trackId));
        const double at = barsToSeconds(c, atBar - 1.0);

        std::string clipId;
        if (trackId.empty()) {
            trackId = c.importAudioToNewTrack(filePath, at, trackName);
            if (trackId.empty())
                return fail("that file could not be read as audio: " +
                            privateFileLabel(filePath));
            const TrackModel* made = c.project().findTrack(trackId);
            if (made && !made->clips.empty()) clipId = made->clips.front().id;
        } else {
            clipId = c.importAudio(filePath, trackId, at);
            if (clipId.empty())
                return fail("that file could not be read as audio: " +
                            privateFileLabel(filePath));
        }
        return done(json{{"trackId", trackId},
                         {"clipId", clipId},
                         {"atBar", round2(atBar)}});
    }

    // ── The action tools ──
    if (name == "edit_clip") {
        std::string trackId, clipId, action, toTrackId, clipName;
        double atBar = 0.0, gainDb = 0.0, fadeIn = 0.0, fadeOut = 0.0;
        double offsetBeats = 0.0, lengthBars = 0.0;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        wantString(args, "action", action, err);
        optString(args, "toTrackId", toTrackId, err);
        optString(args, "name", clipName, err);
        optNumber(args, "atBar", atBar, err);
        optNumber(args, "gainDb", gainDb, err);
        optNumber(args, "fadeInBeats", fadeIn, err);
        optNumber(args, "fadeOutBeats", fadeOut, err);
        optNumber(args, "offsetBeats", offsetBeats, err);
        optNumber(args, "lengthBars", lengthBars, err);
        if (!err.empty()) return fail(err);

        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) {
            if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
            return fail("no clip with id '" + clipId + "' on that track");
        }
        // Held by value: several of these edit the document, and the pointer
        // above dangles the moment a clip vector reallocates.
        const std::string clipLabel = clip->name;
        const double clipStart = clip->startSeconds;
        const double clipOffset = clip->offsetSeconds;
        const double clipLength = clip->durationSeconds;

        const auto needsBar = [&](double& bar) {
            if (atBar < 1.0) return false;
            bar = atBar;
            return true;
        };

        if (action == "move") {
            double bar = 0.0;
            if (!needsBar(bar)) return fail("move needs 'atBar' of 1 or more");
            c.setClipStartSeconds(trackId, clipId, barsToSeconds(c, bar - 1.0));
            return done(json{{"startBar", round2(bar)}});
        }
        if (action == "move_to_track") {
            if (toTrackId.empty()) return fail("move_to_track needs 'toTrackId'");
            if (!c.project().findTrack(toTrackId))
                return fail(unknownTrack(c, toTrackId));
            c.moveClipToTrack(trackId, clipId, toTrackId);
            return done(json{{"trackId", toTrackId}, {"clipId", clipId}});
        }
        if (action == "rename") {
            if (clipName.empty()) return fail("rename needs 'name'");
            c.setClipName(trackId, clipId, clipName);
            return done(json{{"name", clipName}});
        }
        if (action == "mute" || action == "unmute") {
            c.setClipMuted(trackId, clipId, action == "mute");
            return done(json{{"muted", action == "mute"}});
        }
        if (action == "gain") {
            if (gainDb < -60.0 || gainDb > 12.0)
                return fail("clip gain runs from -60 to +12 dB");
            c.setClipGain(trackId, clipId, float(dbToGain(gainDb)));
            return done(json{{"gainDb", round2(gainDb)}});
        }
        if (action == "fade") {
            if (fadeIn < 0.0 || fadeOut < 0.0)
                return fail("a fade cannot be negative");
            c.setClipFade(trackId, clipId, beatsToSeconds(fadeIn, c.tempo()),
                          beatsToSeconds(fadeOut, c.tempo()));
            return done(json{{"fadeInBeats", round2(fadeIn)},
                             {"fadeOutBeats", round2(fadeOut)}});
        }
        if (action == "trim") {
            const double start = atBar >= 1.0 ? barsToSeconds(c, atBar - 1.0)
                                              : clipStart;
            const double offset = args.contains("offsetBeats")
                                      ? beatsToSeconds(offsetBeats, c.tempo())
                                      : clipOffset;
            const double length = lengthBars > 0.0 ? barsToSeconds(c, lengthBars)
                                                   : clipLength;
            if (length <= 0.0) return fail("a clip has to be longer than nothing");
            c.setClipTrim(trackId, clipId, start, offset, length);
            return done(json{{"startBar", round2(secondsToBars(c, start) + 1.0)},
                             {"lengthBars", round2(secondsToBars(c, length))}});
        }
        if (action == "split") {
            double bar = 0.0;
            if (!needsBar(bar)) return fail("split needs 'atBar' of 1 or more");
            const double at = barsToSeconds(c, bar - 1.0);
            if (at <= clipStart || at >= clipStart + clipLength)
                return fail("bar " + std::to_string(bar) +
                            " is not inside that clip, so there is nothing to "
                            "split there");
            const std::string tail = c.splitClip(trackId, clipId, at);
            if (tail.empty()) return fail("that clip could not be split there");
            return done(json{{"firstClipId", clipId}, {"secondClipId", tail}});
        }
        if (action == "duplicate_at") {
            double bar = 0.0;
            if (!needsBar(bar)) return fail("duplicate_at needs 'atBar' of 1 or more");
            const std::string copy =
                c.duplicateClipAt(trackId, clipId, barsToSeconds(c, bar - 1.0));
            if (copy.empty()) return fail("the clip could not be duplicated");
            return done(json{{"clipId", copy}, {"startBar", round2(bar)}});
        }
        if (action == "remove") {
            if (!allowed(ctx, "delete the clip \"" + clipLabel + "\""))
                return fail(kRefused);
            c.removeClip(trackId, clipId);
            return done();
        }
        return fail("'" + action +
                    "' is not an edit_clip action. Use move, move_to_track, "
                    "rename, mute, unmute, gain, fade, trim, split, "
                    "duplicate_at or remove");
    }

    if (name == "edit_notes") {
        std::string trackId, clipId, action;
        double fromBeat = 0.0, toBeat = 0.0;
        double lowPitch = 0.0, highPitch = 127.0;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        wantString(args, "action", action, err);
        optNumber(args, "fromBeat", fromBeat, err);
        optNumber(args, "toBeat", toBeat, err);
        optNumber(args, "lowPitch", lowPitch, err);
        optNumber(args, "highPitch", highPitch, err);
        if (!err.empty()) return fail(err);
        if (action != "add" && action != "remove" && action != "replace_range")
            return fail("'" + action +
                        "' is not an edit_notes action. Use add, remove or "
                        "replace_range");

        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) {
            if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
            return fail("no clip with id '" + clipId + "' on that track");
        }
        if (clip->kind != ClipKind::Midi)
            return fail("clip '" + clip->name + "' holds audio, not notes");

        std::vector<NoteModel> incoming;
        if (action != "remove") {
            json noteList;
            wantArray(args, "notes", noteList, err);
            if (!err.empty()) return fail(err + " of notes");
            for (const json& raw : noteList) {
                if (!raw.is_object()) return fail("every note must be an object");
                std::string noteErr;
                double pitch = 0.0, start = 0.0, length = 0.0, velocity = 100.0;
                wantNumber(raw, "pitch", pitch, noteErr);
                wantNumber(raw, "start", start, noteErr);
                wantNumber(raw, "length", length, noteErr);
                optNumber(raw, "velocity", velocity, noteErr);
                if (!noteErr.empty()) return fail("in one of the notes: " + noteErr);
                if (pitch < 0.0 || pitch > 127.0)
                    return fail("pitch must be between 0 and 127");
                if (start < 0.0) return fail("a note cannot start before the clip");
                if (length <= 0.0) return fail("a note's length must be above 0");
                NoteModel note;
                note.pitch = int(pitch);
                note.startBeats = start;
                note.lengthBeats = length;
                note.velocity = std::clamp(int(velocity), 1, 127);
                incoming.push_back(note);
            }
        }

        const double clipBeats = secondsToBeats(clip->durationSeconds, c.tempo());
        const double until = toBeat > 0.0 ? toBeat : std::max(clipBeats, 1e9);
        if (until <= fromBeat && action != "add")
            return fail("'toBeat' has to be after 'fromBeat'");

        std::vector<NoteModel> notes;
        std::size_t removed = 0;
        for (const NoteModel& note : clip->notes) {
            const bool inRange =
                action != "add" && note.startBeats + note.lengthBeats > fromBeat &&
                note.startBeats < until && note.pitch >= int(lowPitch) &&
                note.pitch <= int(highPitch);
            if (inRange) {
                ++removed;
                continue;
            }
            notes.push_back(note);
        }
        if (action != "remove")
            notes.insert(notes.end(), incoming.begin(), incoming.end());

        const std::string label = action == "add"     ? "AI: Add Notes"
                                  : action == "remove" ? "AI: Remove Notes"
                                                       : "AI: Edit Notes";
        c.setClipNotes(trackId, clipId, notes, label);
        return done(json{{"removed", removed},
                         {"added", incoming.size()},
                         {"noteCount", notes.size()}});
    }

    if (name == "mix") {
        std::string action, channelId, toTrackId, sendId;
        double levelDb = 0.0, pan = 0.0;
        wantString(args, "action", action, err);
        optString(args, "channelId", channelId, err);
        optString(args, "toTrackId", toTrackId, err);
        optString(args, "sendId", sendId, err);
        optNumber(args, "levelDb", levelDb, err);
        optNumber(args, "pan", pan, err);
        if (!err.empty()) return fail(err);

        if (action == "clear_solos") {
            c.clearAllSolos();
            return done();
        }
        if (action == "set_master_volume") {
            if (levelDb < -60.0 || levelDb > 6.0)
                return fail("levelDb runs from -60 to +6");
            c.setMasterVolume(float(dbToGain(levelDb)));
            return done(json{{"levelDb", round2(levelDb)}});
        }

        if (channelId.empty())
            return fail("that action needs 'channelId'");
        // Only the master's own volume can be set, and that is its own action:
        // everything below edits a track.
        const TrackModel* track = c.project().findTrack(channelId);
        if (!track) return fail(unknownTrack(c, channelId));

        if (action == "set_level") {
            if (levelDb < -60.0 || levelDb > 6.0)
                return fail("levelDb runs from -60 to +6");
            c.setTrackVolume(channelId, float(dbToGain(levelDb)));
            return done(json{{"levelDb", round2(levelDb)}});
        }
        if (action == "set_pan") {
            if (pan < -1.0 || pan > 1.0) return fail("pan runs from -1 to 1");
            c.setTrackPan(channelId, float(pan));
            return done(json{{"pan", round2(pan)}});
        }
        if (action == "mute" || action == "unmute") {
            c.setTrackMuted(channelId, action == "mute");
            return done(json{{"muted", action == "mute"}});
        }
        if (action == "solo" || action == "unsolo") {
            c.setTrackSoloed(channelId, action == "solo");
            return done(json{{"soloed", action == "solo"}});
        }
        if (action == "set_send") {
            if (toTrackId.empty())
                return fail("set_send needs 'toTrackId' — the bus being fed");
            if (!c.project().findTrack(toTrackId))
                return fail(unknownTrack(c, toTrackId));
            if (toTrackId == channelId)
                return fail("a channel cannot send to itself");
            std::string id = sendId;
            if (id.empty()) {
                for (const SendModel& send : track->sends)
                    if (send.destinationTrackId == toTrackId) id = send.id;
            }
            if (id.empty()) {
                id = c.addSend(channelId, toTrackId);
                if (id.empty())
                    return fail("that send could not be made — a send has to "
                                "reach a bus, and it cannot loop back");
            }
            const double level = args.contains("levelDb") ? dbToGain(levelDb) : 0.5;
            c.setSendLevel(channelId, id,
                           std::clamp(float(level), 0.0f,
                                      EngineController::kMaxSendLevel));
            return done(json{{"sendId", id}, {"to", toTrackId}});
        }
        if (action == "remove_send") {
            if (sendId.empty()) return fail("remove_send needs 'sendId'");
            c.removeSend(channelId, sendId);
            return done();
        }
        return fail("'" + action +
                    "' is not a mix action. Use set_level, set_pan, mute, "
                    "unmute, solo, unsolo, clear_solos, set_send, remove_send "
                    "or set_master_volume");
    }

    if (name == "arrange_tracks") {
        std::string action, trackId, folderId, trackName, color;
        double toIndex = 0.0;
        bool summing = false;
        wantString(args, "action", action, err);
        optString(args, "trackId", trackId, err);
        optString(args, "folderId", folderId, err);
        optString(args, "name", trackName, err);
        optString(args, "color", color, err);
        optNumber(args, "toIndex", toIndex, err);
        optBool(args, "summing", summing, err);
        if (!err.empty()) return fail(err);

        if (action == "create_folder") {
            const std::string id = c.addFolder(summing, trackName);
            if (id.empty()) return fail("that folder could not be made");
            return done(json{{"folderId", id}, {"summing", summing}});
        }

        if (trackId.empty()) return fail("that action needs 'trackId'");
        if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));

        if (action == "reorder") {
            if (toIndex < 0.0) return fail("toIndex starts at 0");
            if (!c.moveTrack(trackId, std::size_t(toIndex), std::string()))
                return fail("that track could not be moved there");
            return done(json{{"toIndex", int(toIndex)}});
        }
        if (action == "move_to_folder") {
            if (!folderId.empty() && !c.project().findTrack(folderId))
                return fail(unknownTrack(c, folderId));
            c.moveTrackToFolder(trackId, folderId);
            return done(json{{"trackId", trackId}, {"folderId", folderId}});
        }
        if (action == "set_color") {
            std::uint32_t rgb = 0;
            if (!parseHexColor(color, rgb))
                return fail("'" + color +
                            "' is not a colour. Use a hex value like "
                            "\"#30D158\"");
            c.setTrackColor(trackId, rgb);
            return done(json{{"color", color}});
        }
        if (action == "duplicate_track") {
            const std::string copy = c.duplicateTrack(trackId, true);
            if (copy.empty()) return fail("that track could not be duplicated");
            return done(json{{"trackId", copy}});
        }
        return fail("'" + action +
                    "' is not an arrange_tracks action. Use reorder, "
                    "create_folder, move_to_folder, set_color or "
                    "duplicate_track");
    }

    if (name == "channel_strip") {
        std::string action, channelId, toChannelId;
        bool withSettings = false;
        wantString(args, "action", action, err);
        wantString(args, "channelId", channelId, err);
        optString(args, "toChannelId", toChannelId, err);
        optBool(args, "withSettings", withSettings, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        if (action == "copy_to") {
            if (toChannelId.empty()) return fail("copy_to needs 'toChannelId'");
            if (!channelExists(c, toChannelId))
                return fail(unknownTrack(c, toChannelId));
            if (toChannelId == channelId)
                return fail("that is the same channel");
            const EngineController::ChannelSnapshot strip =
                c.copyChannelStrip(channelId, withSettings);
            if (strip.empty())
                return fail("that channel has no effects to copy");
            const bool ok = withSettings ? c.pasteChannelStrip(toChannelId, strip)
                                         : c.pasteChannelInserts(toChannelId, strip);
            if (!ok) return fail("that chain could not be pasted");
            return done(json{{"from", channelId},
                             {"to", toChannelId},
                             {"inserts", strip.inserts.size()}});
        }
        if (action == "clear") {
            std::vector<std::string> ids;
            if (const std::vector<InsertModel>* inserts = c.channelInserts(channelId))
                for (const InsertModel& slot : *inserts)
                    if (slot.isLoaded()) ids.push_back(slot.id);
            if (ids.empty()) return fail("that channel has no effects on it");
            if (!allowed(ctx, "remove every effect from that channel"))
                return fail(kRefused);
            for (const std::string& id : ids) c.removeInsert(channelId, id);
            return done(json{{"removed", ids.size()}});
        }
        return fail("'" + action +
                    "' is not a channel_strip action. Use copy_to or clear");
    }

    if (name == "automation") {
        std::string action, channelId, target, insertId, parameterId, sendId;
        wantString(args, "action", action, err);
        wantString(args, "channelId", channelId, err);
        optString(args, "target", target, err);
        optString(args, "insertId", insertId, err);
        optString(args, "parameterId", parameterId, err);
        optString(args, "sendId", sendId, err);
        if (!err.empty()) return fail(err);
        if (!channelExists(c, channelId)) return fail(unknownTrack(c, channelId));

        if (action == "list_targets") {
            json targets = json::array();
            targets.push_back(json{{"target", "volume"}});
            targets.push_back(json{{"target", "pan"}});
            if (const TrackModel* track = c.project().findTrack(channelId)) {
                for (const SendModel& send : track->sends)
                    targets.push_back(json{{"target", "send"},
                                           {"sendId", send.id},
                                           {"to", send.destinationTrackId}});
                const auto describe = [&](const InsertModel& slot, bool instrument) {
                    if (!slot.isLoaded()) return;
                    json params = json::array();
                    for (const plugins::ParameterInfo& p :
                         c.insertParameters(channelId, slot.id)) {
                        if (params.size() >= 60) break;
                        params.push_back(json{{"parameterId", p.id},
                                              {"name", p.name}});
                    }
                    targets.push_back(json{
                        {"target", "plugin"},
                        // The instrument slot is spelled as an empty insertId
                        // everywhere automation is concerned; saying so here is
                        // what stops the model from sending its uuid instead.
                        {"insertId", instrument ? std::string() : slot.id},
                        {"plugin", slot.name},
                        {"parameters", std::move(params)}});
                };
                describe(track->instrument, true);
                for (const InsertModel& slot : track->inserts) describe(slot, false);
            }
            return done(json{{"channelId", channelId},
                             {"targets", std::move(targets)},
                             {"note",
                              "values in set_points are normalised 0 to 1, not "
                              "the parameter's own units"}});
        }

        AutomationTarget wanted;
        wanted.channelId = channelId;
        if (target == "volume") {
            wanted.kind = AutomationTargetKind::TrackVolume;
        } else if (target == "pan") {
            wanted.kind = AutomationTargetKind::TrackPan;
        } else if (target == "mute") {
            wanted.kind = AutomationTargetKind::TrackMute;
        } else if (target == "send") {
            if (sendId.empty()) return fail("automating a send needs 'sendId'");
            wanted.kind = AutomationTargetKind::SendLevel;
            wanted.sendId = sendId;
        } else if (target == "plugin") {
            if (parameterId.empty())
                return fail("automating a plugin needs 'parameterId' — call "
                            "automation list_targets or list_plugin_parameters "
                            "first");
            wanted.kind = AutomationTargetKind::PluginParameter;
            wanted.slotId = insertId;
            wanted.parameterId = parameterId;
        } else {
            return fail("'" + target +
                        "' is not an automation target. Use volume, pan, mute, "
                        "send or plugin");
        }

        if (action == "remove") {
            const auto [lane, clip] = c.findAutomation(wanted);
            if (lane.empty()) return fail("nothing automates that yet");
            if (!allowed(ctx, "delete the automation on that parameter"))
                return fail(kRefused);
            c.removeAutomationLane(lane);
            return done();
        }

        if (action != "set_points")
            return fail("'" + action +
                        "' is not an automation action. Use list_targets, "
                        "set_points or remove");

        json pointList;
        wantArray(args, "points", pointList, err);
        if (!err.empty()) return fail(err + " of points");
        if (pointList.empty())
            return fail("a curve needs at least one point");

        const auto [laneId, clipId] = c.ensureAutomation(wanted);
        if (laneId.empty() || clipId.empty())
            return fail("that parameter could not be automated");
        const ClipModel* curve = findClip(c, laneId, clipId);
        if (!curve) return fail("that automation clip went missing");
        const double clipStartBars = secondsToBars(c, curve->startSeconds);
        const std::vector<AutomationPoint> before = curve->automation.points;
        const bool activeBefore = curve->automation.active;

        std::vector<AutomationPoint> points;
        double lastBeat = -1.0;
        double furthest = 0.0;
        for (const json& raw : pointList) {
            if (!raw.is_object()) return fail("every point must be an object");
            std::string pointErr;
            double bar = 0.0, value = 0.0;
            std::string shape = "linear";
            wantNumber(raw, "bar", bar, pointErr);
            wantNumber(raw, "value", value, pointErr);
            optString(raw, "shape", shape, pointErr);
            if (!pointErr.empty()) return fail("in one of the points: " + pointErr);
            if (bar < 1.0) return fail("bars start at 1, not 0");
            if (value < 0.0 || value > 1.0)
                return fail("automation values are normalised: 0 to 1, not the "
                            "parameter's own units");
            AutomationPoint point;
            // Points are measured from the *clip's* start, like notes.
            point.beats = (bar - 1.0 - clipStartBars) * beatsPerBar(c);
            if (point.beats < 0.0)
                return fail("bar " + std::to_string(bar) +
                            " is before the automation clip starts");
            if (point.beats <= lastBeat)
                return fail("the points have to run forwards in time");
            lastBeat = point.beats;
            point.value = value;
            if (shape == "hold") point.shape = AutomationSegment::Hold;
            else if (shape == "smooth") point.shape = AutomationSegment::SCurve;
            else if (shape != "linear")
                return fail("'" + shape +
                            "' is not a point shape. Use linear, hold or smooth");
            furthest = std::max(furthest, point.beats);
            points.push_back(point);
        }

        // A curve reaching past the clip it lives in simply stops, which looks
        // exactly like the automation not working. Same reasoning as the clip
        // growing to hold its notes in set_clip_notes.
        const double curveBeats = secondsToBeats(curve->durationSeconds, c.tempo());
        if (furthest > curveBeats) {
            const double perBar = beatsPerBar(c);
            c.setClipTrim(laneId, clipId, curve->startSeconds, curve->offsetSeconds,
                          barsToSeconds(c, std::ceil(furthest / perBar)));
        }
        c.setAutomationPoints(laneId, clipId, points);
        c.commitAutomationEdit(laneId, clipId, before,
                               "AI: " + c.automationTargetName(wanted),
                               activeBefore);
        return done(json{{"laneTrackId", laneId},
                         {"clipId", clipId},
                         {"points", points.size()},
                         {"automating", c.automationTargetName(wanted)}});
    }

    if (name == "apply_groove") {
        std::string trackId, clipId, groove;
        double amount = 0.7;
        wantString(args, "trackId", trackId, err);
        wantString(args, "clipId", clipId, err);
        optString(args, "groove", groove, err);
        optNumber(args, "amount", amount, err);
        if (!err.empty()) return fail(err);

        const std::vector<miditools::Groove>& presets = miditools::groovePresets();
        if (groove.empty()) {
            std::vector<std::string> names;
            for (const miditools::Groove& preset : presets)
                names.push_back(preset.name);
            return done(json{{"grooves", names},
                             {"note",
                              "call again with one of these as 'groove'"}});
        }
        const auto found = std::find_if(
            presets.begin(), presets.end(),
            [&](const miditools::Groove& preset) { return preset.name == groove; });
        if (found == presets.end()) {
            std::vector<std::string> names;
            for (const miditools::Groove& preset : presets)
                names.push_back(preset.name);
            return fail("'" + groove + "' is not a groove. The ones here are: " +
                        join(names));
        }

        const ClipModel* clip = findClip(c, trackId, clipId);
        if (!clip) {
            if (!c.project().findTrack(trackId)) return fail(unknownTrack(c, trackId));
            return fail("no clip with id '" + clipId + "' on that track");
        }
        if (clip->kind != ClipKind::Midi)
            return fail("clip '" + clip->name + "' holds audio, not notes");
        if (clip->notes.empty())
            return fail("that clip has no notes yet");

        amount = std::clamp(amount, 0.0, 1.0);
        miditools::QuantizeParams params;
        params.gridBeats = 0.25;
        params.strength = amount;
        params.groove = *found;
        params.grooveTiming = amount;
        params.grooveVelocity = amount;
        miditools::Notes notes = miditools::quantize(clip->notes, params);
        c.setClipNotes(trackId, clipId, notes, "AI: " + groove);
        return done(json{{"groove", groove},
                         {"amount", round2(amount)},
                         {"noteCount", notes.size()}});
    }

    return fail("there is no tool called '" + name + "'");
}

// ── The system prompt ───────────────────────────────────────────────────────

const PromptPack& promptsFor(const ToolContext& ctx) {
    return ctx.prompts && !ctx.prompts->empty() ? *ctx.prompts : builtinPrompts();
}

std::string systemPrompt(const EngineController& c, const ToolContext& ctx) {
    const PromptPack& pack = promptsFor(ctx);

    std::string out = pack.main;

    // The index, not the playbooks themselves. Their bodies arrive as tool
    // results, which leaves this prompt byte-identical between the steps of a
    // run — and that is what the provider's cache is keyed on.
    const std::string index = playbookIndex(pack);
    if (!index.empty()) {
        out += "\n\nPLAYBOOKS AVAILABLE\n";
        out += index;
        out +=
            "Load one with get_playbook before doing that kind of work. Do not "
            "guess at what a playbook says.";
    }

    out += "\n\nCURRENT PROJECT\n";
    out += projectSnapshot(c, ctx).dump(2);
    out +=
        "\n\nThe \"focus\" block is what the user is looking at. When they say "
        "\"this\", \"that track\" or \"the clip\", they mean what is selected "
        "there; when they say \"here\" or \"at this point\", they mean the "
        "playhead's bar, or the loop range if one is set. Work on the focus "
        "unless the request names something else. If nothing is selected and "
        "the request needs a target, say which track you chose.";

    if (!c.aiInstructions().empty()) {
        // Last, and framed as overriding: standing instructions are the user's
        // own rules for this project, and they beat the defaults above.
        out += "\n\nSTANDING INSTRUCTIONS FOR THIS PROJECT\n";
        out += "These come from the user and override anything above that "
               "conflicts with them.\n";
        out += c.aiInstructions();
    }

    out += "\n\nACTIVE INTERACTION MODE: ";
    out += interactionModeName(ctx.mode);
    switch (ctx.mode) {
        case InteractionMode::Help:
            out +=
                "\nAnswer the question from inspected project state. This is "
                "read-only: do not change, audition, save, or export anything.";
            break;
        case InteractionMode::Teach:
            out +=
                "\nTeach in short, concrete steps using this project's real "
                "names and values. Inspect when useful, but do not perform the "
                "steps for the user.";
            break;
        case InteractionMode::Do:
            out +=
                "\nCarry out the requested operation. Prefer reversible actions, "
                "inspect before editing, and report the exact result.";
            break;
        case InteractionMode::Compose:
            out +=
                "\nCompose against the existing musical context. Inspect key, "
                "harmony, rhythm, register and instrumentation first; create "
                "reversible material and audition it, but do not delete work, "
                "rewrite history, save, or export.";
            break;
    }

    out += "\n\nFILES THE USER ATTACHED\n";
    if (ctx.attachments.empty()) {
        out +=
            "None. If a request needs a sample you have not been given, use an "
            "installed instrument plugin instead.";
    } else {
        out +=
            "You cannot listen to these directly. Judge them by their name, "
            "length and channel count; inspect audio with analyze_sample, then "
            "pass the opaque contentId to load_sampler or import_audio.\n";
        for (std::size_t i = 0; i < ctx.attachments.size(); ++i) {
            const Attachment& a = ctx.attachments[i];
            char line[1024];
            std::snprintf(line, sizeof(line),
                          "- %s  (%.2f s, %d Hz, %d ch)\n  contentId: %s\n",
                          a.name.c_str(), a.seconds, a.sampleRate, a.channels,
                          attachmentId(a, i).c_str());
            out += line;
        }
    }
    return out;
}

} // namespace daw::ai
