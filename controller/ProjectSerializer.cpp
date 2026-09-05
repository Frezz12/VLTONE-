#include "ProjectSerializer.hpp"
#include "collaboration/CollaborationState.hpp"
#include "platform/PathUtils.hpp"
#include "serialization/AssetJson.hpp"
#include "serialization/InsertJson.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace daw {

std::string ProjectSerializer::mediaPath(const std::string& packageDir) {
    const fs::path package = platform::pathFromUtf8(packageDir);
    const fs::path preferred = package / kMediaDir;
    const fs::path legacy = package / "media";
    std::error_code ec;
    // Keep old .dawp projects openable. A new package has neither directory,
    // so it always takes the VLT layout; an old package keeps resolving the
    // media it already owns instead of silently appearing empty.
    if (!fs::exists(preferred, ec) && fs::exists(legacy, ec))
        return platform::pathToUtf8(legacy);
    return platform::pathToUtf8(preferred);
}

std::string ProjectSerializer::statePath(const std::string& packageDir) {
    const fs::path package = platform::pathFromUtf8(packageDir);
    const fs::path preferred = package / kStateDir;
    const fs::path legacy = package / "state";
    std::error_code ec;
    if (!fs::exists(preferred, ec) && fs::exists(legacy, ec))
        return platform::pathToUtf8(legacy);
    return platform::pathToUtf8(preferred);
}

namespace {

fs::path preferredManifestPath(const fs::path& package) {
    std::string extension = package.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return extension == ".vlt" ? package / package.filename()
                               : package / ProjectSerializer::kProjectFile;
}

using serialization::insertFromJson;
using serialization::insertToJson;
using serialization::insertsFromJson;
using serialization::insertsToJson;
using serialization::assetRefFromJson;
using serialization::assetRefToJson;

json reservedArray(std::size_t capacity) {
    json result = json::array();
    result.get_ref<json::array_t&>().reserve(capacity);
    return result;
}

std::uint64_t fileContentHash(const fs::path& source) {
    std::ifstream stream(source, std::ios::binary);
    std::array<char, 65536> buffer{};
    std::uint64_t hash = 1469598103934665603ull;
    while (stream) {
        stream.read(buffer.data(), std::streamsize(buffer.size()));
        const std::streamsize count = stream.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[std::size_t(i)]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

bool filesEqual(const fs::path& left, const fs::path& right) {
    std::error_code ec;
    const auto leftSize = fs::file_size(left, ec);
    if (ec) return false;
    const auto rightSize = fs::file_size(right, ec);
    if (ec || leftSize != rightSize) return false;

    std::ifstream a(left, std::ios::binary);
    std::ifstream b(right, std::ios::binary);
    if (!a || !b) return false;
    std::array<char, 65536> aBuffer{};
    std::array<char, 65536> bBuffer{};
    while (a && b) {
        a.read(aBuffer.data(), std::streamsize(aBuffer.size()));
        b.read(bBuffer.data(), std::streamsize(bBuffer.size()));
        const auto aCount = a.gcount();
        const auto bCount = b.gcount();
        if (aCount != bCount ||
            !std::equal(aBuffer.begin(), aBuffer.begin() + aCount, bBuffer.begin())) {
            return false;
        }
    }
    return true;
}

std::string uniqueMediaName(const fs::path& source) {
    std::ostringstream prefix;
    prefix << std::hex << std::setw(16) << std::setfill('0')
           << fileContentHash(source);
    return prefix.str() + "-" + platform::pathToUtf8(source.filename());
}

json noteToJson(const NoteModel& n) {
    return json{
        {"id", n.id},
        {"pitch", n.pitch},
        {"startBeats", n.startBeats},
        {"lengthBeats", n.lengthBeats},
        {"velocity", n.velocity},
        {"muted", n.muted},
        {"color", n.color},
        {"pan", n.pan},
    };
}

NoteModel noteFromJson(const json& j) {
    NoteModel n;
    n.id = j.value("id", newUuid());
    n.pitch = j.value("pitch", 60);
    n.startBeats = j.value("startBeats", 0.0);
    n.lengthBeats = j.value("lengthBeats", 1.0);
    n.velocity = j.value("velocity", 100);
    // Defaults match a pre-mute/colour project: audible, colour inherited.
    n.muted = j.value("muted", false);
    n.color = j.value("color", 0u);
    n.pan = j.value("pan", 0.0f);
    return n;
}

json pointToJson(const AutomationPoint& point) {
    json out{{"beats", point.beats}, {"value", point.value}};
    if (!point.id.empty()) out["id"] = point.id;
    // Written only when it has something to say. A straight segment is what
    // every point saved before shapes existed was, and what most points still
    // are — spelling that out on each one would double the size of a curve.
    if (point.shape != AutomationSegment::Linear)
        out["shape"] = toString(point.shape);
    if (std::abs(point.curve) > 1e-9) out["curve"] = point.curve;
    return out;
}

AutomationPoint pointFromJson(const json& j) {
    AutomationPoint point;
    point.id = j.value("id", std::string());
    point.beats = j.value("beats", 0.0);
    point.value = j.value("value", 0.0);
    point.shape = automationSegmentFromString(j.value("shape", std::string("linear")));
    point.curve = std::clamp(j.value("curve", 0.0), -1.0, 1.0);
    return point;
}

json automationToJson(const ClipAutomationModel& automation) {
    json points = reservedArray(automation.points.size());
    for (const auto& pt : automation.points) points.push_back(pointToJson(pt));
    return json{
        {"kind", toString(automation.target.kind)},
        {"channelId", automation.target.channelId},
        {"slotId", automation.target.slotId},
        {"parameterId", automation.target.parameterId},
        {"sendId", automation.target.sendId},
        {"defaultValue", automation.defaultValue},
        {"active", automation.active},
        {"points", std::move(points)},
    };
}

ClipAutomationModel automationFromJson(const json& j) {
    ClipAutomationModel automation;
    automation.target.kind =
        automationTargetKindFromString(j.value("kind", std::string("volume")));
    automation.target.channelId = j.value("channelId", std::string());
    automation.target.slotId = j.value("slotId", std::string());
    automation.target.parameterId = j.value("parameterId", std::string());
    automation.target.sendId = j.value("sendId", std::string());
    automation.defaultValue = std::clamp(j.value("defaultValue", 0.0), 0.0, 1.0);
    // Curves saved before passive automation existed were always applied.
    automation.active = j.value("active", true);
    if (j.contains("points") && j.at("points").is_array()) {
        const auto& points = j.at("points");
        automation.points.reserve(points.size());
        for (const auto& jp : points)
            automation.points.push_back(pointFromJson(jp));
    }
    normalizeAutomation(automation.points);
    return automation;
}

json laneToJson(const ControllerLane& lane) {
    json points = reservedArray(lane.points.size());
    for (const auto& pt : lane.points) points.push_back(pointToJson(pt));
    return json{
        {"id", lane.id},
        {"name", lane.name},
        {"cc", lane.cc},
        {"parameterId", lane.parameterId},
        {"slotId", lane.slotId},
        {"defaultValue", lane.defaultValue},
        {"points", std::move(points)},
    };
}

ControllerLane laneFromJson(const json& j) {
    ControllerLane lane;
    lane.id = j.value("id", newUuid());
    lane.name = j.value("name", "CC");
    lane.cc = j.value("cc", 1);
    lane.parameterId = j.value("parameterId", "");
    lane.slotId = j.value("slotId", "");
    lane.defaultValue = j.value("defaultValue", 0.0);
    if (j.contains("points") && j.at("points").is_array()) {
        const auto& points = j.at("points");
        lane.points.reserve(points.size());
        for (const auto& jp : points) lane.points.push_back(pointFromJson(jp));
    }
    // Sorted and de-duplicated on the way in, like every in-memory path already
    // guarantees. Trusting the file's ordering meant one reader — the one that
    // loaded a project rather than editing one — could see points a curve
    // evaluator is entitled to assume cannot exist.
    normalizeAutomation(lane.points);
    return lane;
}

/// A media reference as it should be written: the basename for a packaged
/// project, the path itself for a journal that does not copy anything.
std::string mediaReference(const std::string& path, MediaPaths media) {
    if (path.empty()) return {};
    if (media == MediaPaths::Absolute) return path;
    return platform::pathToUtf8(platform::pathFromUtf8(path).filename());
}

json takeToJson(const TakeModel& t, MediaPaths media) {
    std::string file = mediaReference(t.filePath, media);
    json notes = reservedArray(t.notes.size());
    for (const auto& n : t.notes) notes.push_back(noteToJson(n));
    json result{
        {"id", t.id},
        {"name", t.name},
        {"file", std::move(file)},
        {"offsetSeconds", t.offsetSeconds},
        {"lengthSeconds", t.lengthSeconds},
        {"clipOffsetSeconds", t.clipOffsetSeconds},
        {"gain", t.gain},
        {"muted", t.muted},
        {"channels", t.channels},
        {"color", t.color},
        {"notes", std::move(notes)},
    };
    if (!t.asset.empty()) result["asset"] = assetRefToJson(t.asset);
    return result;
}

TakeModel takeFromJson(const json& j, const std::string& mediaDir) {
    TakeModel t;
    t.id = j.value("id", newUuid());
    t.name = j.value("name", "");
    const std::string file = j.value("file", "");
    if (!file.empty()) {
        t.filePath = platform::pathToUtf8(platform::pathFromUtf8(mediaDir) /
                                          platform::pathFromUtf8(file));
    }
    t.offsetSeconds = j.value("offsetSeconds", 0.0);
    t.lengthSeconds = j.value("lengthSeconds", 0.0);
    t.clipOffsetSeconds = j.value("clipOffsetSeconds", 0.0);
    t.gain = j.value("gain", 1.0f);
    t.muted = j.value("muted", false);
    t.channels = j.value("channels", 0);
    t.color = j.value("color", 0x4A90D9u);
    if (j.contains("asset")) t.asset = assetRefFromJson(j.at("asset"));
    if (j.contains("notes") && j.at("notes").is_array()) {
        const auto& notes = j.at("notes");
        t.notes.reserve(notes.size());
        for (const auto& jn : notes) t.notes.push_back(noteFromJson(jn));
    }
    return t;
}

json compSegmentToJson(const CompSegment& s) {
    return json{
        {"id", s.id},
        {"takeId", s.takeId},
        {"startSeconds", s.startSeconds},
        {"endSeconds", s.endSeconds},
    };
}

CompSegment compSegmentFromJson(const json& j) {
    CompSegment s;
    s.id = j.value("id", std::string());
    s.takeId = j.value("takeId", "");
    s.startSeconds = j.value("startSeconds", 0.0);
    s.endSeconds = j.value("endSeconds", 0.0);
    return s;
}

json clipSampleEditToJson(const ClipSampleEditModel& s) {
    return json{
        {"loopMode", s.loopMode}, {"loopStart", s.loopStart},
        {"loopEnd", s.loopEnd}, {"stretchMode", int(s.stretchMode)},
        {"stretchTime", s.stretchTime}, {"stretchPitch", s.stretchPitch},
        {"formant", s.formant}, {"rootNote", s.rootNote},
        {"boost", s.boost}, {"eqLow", s.eqLow},
        {"eqMid", s.eqMid}, {"eqHigh", s.eqHigh}, {"ringMix", s.ringMix},
        {"ringFreq", s.ringFreq}, {"cut", s.cut}, {"res", s.res},
        {"reverbType", s.reverbType}, {"reverb", s.reverb},
        {"stereoDelay", s.stereoDelay}, {"pogo", s.pogo},
        {"removeDc", s.removeDc}, {"reversePolarity", s.reversePolarity},
        {"normalize", s.normalize}, {"fadeStereo", s.fadeStereo},
        {"reverse", s.reverse}, {"swapStereo", s.swapStereo},
    };
}

ClipSampleEditModel clipSampleEditFromJson(const json& j) {
    ClipSampleEditModel s;
    s.loopMode = std::clamp(j.value("loopMode", 0), 0, 2);
    s.loopStart = std::clamp(j.value("loopStart", 0.0), 0.0, 1.0);
    s.loopEnd = std::clamp(j.value("loopEnd", 1.0), 0.0, 1.0);
    s.stretchMode = ClipStretchMode(std::clamp(j.value("stretchMode", 0), 0, 4));
    s.stretchTime = std::clamp(j.value("stretchTime", 1.0), 0.25, 4.0);
    s.stretchPitch = std::clamp(j.value("stretchPitch", 0.0), -24.0, 24.0);
    s.formant = std::clamp(j.value("formant", 0.0), -12.0, 12.0);
    s.rootNote = std::clamp(j.value("rootNote", 60), 0, 127);
    s.boost = std::clamp(j.value("boost", 0.0), 0.0, 1.0);
    s.eqLow = std::clamp(j.value("eqLow", 0.0), -1.0, 1.0);
    s.eqMid = std::clamp(j.value("eqMid", 0.0), -1.0, 1.0);
    s.eqHigh = std::clamp(j.value("eqHigh", 0.0), -1.0, 1.0);
    s.ringMix = std::clamp(j.value("ringMix", 0.0), 0.0, 1.0);
    s.ringFreq = std::clamp(j.value("ringFreq", 0.5), 0.0, 1.0);
    s.cut = std::clamp(j.value("cut", 1.0), 0.0, 1.0);
    s.res = std::clamp(j.value("res", 0.0), 0.0, 1.0);
    s.reverbType = std::clamp(j.value("reverbType", 0), 0, 1);
    s.reverb = std::clamp(j.value("reverb", 0.0), 0.0, 1.0);
    s.stereoDelay = std::clamp(j.value("stereoDelay", 0.0), 0.0, 1.0);
    s.pogo = std::clamp(j.value("pogo", 0.0), -1.0, 1.0);
    s.removeDc = j.value("removeDc", false);
    s.reversePolarity = j.value("reversePolarity", false);
    s.normalize = j.value("normalize", false);
    s.fadeStereo = j.value("fadeStereo", false);
    s.reverse = j.value("reverse", false);
    s.swapStereo = j.value("swapStereo", false);
    return s;
}

json musicalAnalysisToJson(const ClipMusicalAnalysisModel& analysis) {
    json alternatives = reservedArray(analysis.tempo.alternatives.size());
    for (double bpm : analysis.tempo.alternatives)
        alternatives.push_back(bpm);
    return json{
        {"version", analysis.algorithmVersion},
        {"offsetSeconds", analysis.analyzedOffsetSeconds},
        {"durationSeconds", analysis.analyzedDurationSeconds},
        {"tempo", {
            {"status", int(analysis.tempo.status)},
            {"bpm", analysis.tempo.bpm},
            {"confidence", analysis.tempo.confidence},
            {"stability", analysis.tempo.stability},
            {"alternatives", std::move(alternatives)},
            {"variable", analysis.tempo.variable},
        }},
        {"key", {
            {"status", int(analysis.key.status)},
            {"root", analysis.key.root},
            {"scale", analysis.key.scale},
            {"confidence", analysis.key.confidence},
            {"alternateRoot", analysis.key.alternateRoot},
            {"alternateScale", analysis.key.alternateScale},
            {"tuningCents", analysis.key.tuningCents},
        }},
    };
}

ClipMusicalAnalysisModel musicalAnalysisFromJson(const json& j) {
    ClipMusicalAnalysisModel analysis;
    analysis.algorithmVersion = std::max(0, j.value("version", 0));
    analysis.analyzedOffsetSeconds =
        std::max(0.0, j.value("offsetSeconds", 0.0));
    analysis.analyzedDurationSeconds =
        std::max(0.0, j.value("durationSeconds", 0.0));
    if (j.contains("tempo") && j.at("tempo").is_object()) {
        const auto& tempo = j.at("tempo");
        analysis.tempo.status = MusicalAnalysisStatus(std::clamp(
            tempo.value("status", 0), 0, 2));
        analysis.tempo.bpm = std::clamp(tempo.value("bpm", 0.0), 0.0, 300.0);
        analysis.tempo.confidence =
            std::clamp(tempo.value("confidence", 0.0), 0.0, 1.0);
        analysis.tempo.stability =
            std::clamp(tempo.value("stability", 0.0), 0.0, 1.0);
        analysis.tempo.variable = tempo.value("variable", false);
        if (tempo.contains("alternatives") && tempo.at("alternatives").is_array()) {
            for (const auto& value : tempo.at("alternatives")) {
                if (!value.is_number()) continue;
                const double bpm = std::clamp(value.get<double>(), 0.0, 300.0);
                if (bpm > 0.0 && analysis.tempo.alternatives.size() < 3)
                    analysis.tempo.alternatives.push_back(bpm);
            }
        }
    }
    if (j.contains("key") && j.at("key").is_object()) {
        const auto& key = j.at("key");
        analysis.key.status = MusicalAnalysisStatus(std::clamp(
            key.value("status", 0), 0, 2));
        analysis.key.root = std::clamp(key.value("root", -1), -1, 11);
        analysis.key.scale = key.value("scale", std::string());
        analysis.key.confidence =
            std::clamp(key.value("confidence", 0.0), 0.0, 1.0);
        analysis.key.alternateRoot =
            std::clamp(key.value("alternateRoot", -1), -1, 11);
        analysis.key.alternateScale =
            key.value("alternateScale", std::string());
        analysis.key.tuningCents =
            std::clamp(key.value("tuningCents", 0.0), -50.0, 50.0);
    }
    return analysis;
}

json clipToJson(const ClipModel& c, MediaPaths media) {
    std::string name = mediaReference(c.filePath, media);
    json notes = reservedArray(c.notes.size());
    for (const auto& n : c.notes) notes.push_back(noteToJson(n));
    json takes = reservedArray(c.takes.size());
    for (const auto& t : c.takes) takes.push_back(takeToJson(t, media));
    json comp = reservedArray(c.comp.size());
    for (const auto& s : c.comp) comp.push_back(compSegmentToJson(s));
    json lanes = reservedArray(c.lanes.size());
    for (const auto& lane : c.lanes) lanes.push_back(laneToJson(lane));
    json result{
        {"id", c.id},
        {"name", c.name},
        {"file", name},
        {"startSeconds", c.startSeconds},
        {"durationSeconds", c.durationSeconds},
        {"offsetSeconds", c.offsetSeconds},
        {"fadeInSeconds", c.fadeInSeconds},
        {"fadeOutSeconds", c.fadeOutSeconds},
        {"fadeInCurve", c.fadeInCurve},
        {"fadeOutCurve", c.fadeOutCurve},
        {"fadeInMode", c.fadeInMode == ClipFadeMode::Tape ? "tape" : "gain"},
        {"fadeOutMode", c.fadeOutMode == ClipFadeMode::Tape ? "tape" : "gain"},
        {"gain", c.gain},
        {"pan", c.pan},
        {"muted", c.muted},
        {"channels", c.channels},
        {"color", c.color},
        // Always written, so the format says what a clip is rather than leaving
        // the reader to guess. Costs 16 bytes on an audio clip.
        {"kind", toString(c.kind)},
        {"patternClipId", c.patternClipId},
        {"notes", std::move(notes)},
        {"takes", std::move(takes)},
        {"comp", std::move(comp)},
        {"compCrossfadeMs", c.compCrossfadeMs},
        {"lanes", std::move(lanes)},
        // Only on the kind that has one — an "automation" object on an audio
        // clip would be a field that means nothing, written on every clip in
        // every project.
        {"automation", c.kind == ClipKind::Automation
                           ? automationToJson(c.automation)
                           : json(nullptr)},
        {"sampleEdit", clipSampleEditToJson(c.sampleEdit)},
        {"inserts", insertsToJson(c.inserts)},
        {"expanded", c.expanded},
    };
    if (!c.musicalAnalysis.empty())
        result["musicalAnalysis"] = musicalAnalysisToJson(c.musicalAnalysis);
    if (!c.asset.empty()) result["asset"] = assetRefToJson(c.asset);
    if (c.playbackInjection.active()) {
        result["playbackInjection"] = json{
            {"stage", toString(c.playbackInjection.stage)},
            {"anchorChannelId", c.playbackInjection.anchorChannelId},
        };
    }
    if (!c.offlineProcess.empty()) {
        json offline{
            {"chain", insertsToJson(c.offlineProcess.chain)},
            {"renderedFile",
             mediaReference(c.offlineProcess.renderedFilePath, media)},
            {"sourceFingerprint", c.offlineProcess.sourceFingerprint},
            {"sourceDurationSeconds",
             c.offlineProcess.sourceDurationSeconds},
            {"renderedDurationSeconds",
             c.offlineProcess.renderedDurationSeconds},
            {"includeTail", c.offlineProcess.includeTail},
            {"tailSilenceDb", c.offlineProcess.tailSilenceDb},
            {"tailHoldSeconds", c.offlineProcess.tailHoldSeconds},
            {"tailMaxSeconds", c.offlineProcess.tailMaxSeconds},
        };
        if (!c.offlineProcess.renderedAsset.empty()) {
            offline["renderedAsset"] =
                assetRefToJson(c.offlineProcess.renderedAsset);
        }
        result["offlineProcess"] = std::move(offline);
    }
    return result;
}

ClipModel clipFromJson(const json& j, const std::string& mediaDir) {
    ClipModel c;
    c.id = j.value("id", newUuid());
    c.name = j.value("name", "");
    const std::string file = j.value("file", "");
    if (!file.empty()) {
        c.filePath = platform::pathToUtf8(platform::pathFromUtf8(mediaDir) /
                                          platform::pathFromUtf8(file));
    }
    c.startSeconds = j.value("startSeconds", 0.0);
    c.durationSeconds = j.value("durationSeconds", 0.0);
    c.offsetSeconds = j.value("offsetSeconds", 0.0);
    c.fadeInSeconds = j.value("fadeInSeconds", 0.0);
    c.fadeOutSeconds = j.value("fadeOutSeconds", 0.0);
    c.fadeInCurve = std::clamp(j.value("fadeInCurve", 0.0), -1.0, 1.0);
    c.fadeOutCurve = std::clamp(j.value("fadeOutCurve", 0.0), -1.0, 1.0);
    c.fadeInMode = j.value("fadeInMode", "gain") == "tape"
                       ? ClipFadeMode::Tape
                       : ClipFadeMode::Gain;
    c.fadeOutMode = j.value("fadeOutMode", "gain") == "tape"
                        ? ClipFadeMode::Tape
                        : ClipFadeMode::Gain;
    c.gain = j.value("gain", 1.0f);
    c.pan = j.value("pan", 0.0f);
    c.muted = j.value("muted", false);
    c.channels = j.value("channels", 0);
    c.color = j.value("color", 0x4A90D9u);
    if (j.contains("asset")) c.asset = assetRefFromJson(j.at("asset"));
    // A project written before MIDI existed has neither key, so it reads back
    // as the audio clip it is — no migration step needed.
    c.kind = clipKindFromString(j.value("kind", "audio"));
    if (j.contains("playbackInjection") &&
        j.at("playbackInjection").is_object()) {
        const auto& injection = j.at("playbackInjection");
        c.playbackInjection.stage = playbackInjectionStageFromString(
            injection.value("stage", std::string("none")));
        c.playbackInjection.anchorChannelId =
            injection.value("anchorChannelId", std::string());
    }
    if (j.contains("offlineProcess") &&
        j.at("offlineProcess").is_object()) {
        const auto& offline = j.at("offlineProcess");
        c.offlineProcess.chain = insertsFromJson(offline, "chain");
        const std::string rendered =
            offline.value("renderedFile", std::string());
        if (!rendered.empty()) {
            c.offlineProcess.renderedFilePath = platform::pathToUtf8(
                platform::pathFromUtf8(mediaDir) /
                platform::pathFromUtf8(rendered));
        }
        if (offline.contains("renderedAsset")) {
            c.offlineProcess.renderedAsset =
                assetRefFromJson(offline.at("renderedAsset"));
        }
        c.offlineProcess.sourceFingerprint =
            offline.value("sourceFingerprint", std::string());
        c.offlineProcess.sourceDurationSeconds =
            offline.value("sourceDurationSeconds", c.durationSeconds);
        c.offlineProcess.renderedDurationSeconds =
            offline.value("renderedDurationSeconds", c.durationSeconds);
        c.offlineProcess.includeTail = offline.value("includeTail", false);
        c.offlineProcess.tailSilenceDb =
            offline.value("tailSilenceDb", -96.0);
        c.offlineProcess.tailHoldSeconds =
            offline.value("tailHoldSeconds", 0.3);
        c.offlineProcess.tailMaxSeconds =
            offline.value("tailMaxSeconds", 30.0);
    }
    c.patternClipId = j.value("patternClipId", "");
    if (j.contains("notes") && j.at("notes").is_array()) {
        const auto& notes = j.at("notes");
        c.notes.reserve(notes.size());
        for (const auto& jn : notes) c.notes.push_back(noteFromJson(jn));
    }
    // A project written before layer recording has neither key, so it loads as
    // the plain single-layer clip it is.
    if (j.contains("takes") && j.at("takes").is_array()) {
        const auto& takes = j.at("takes");
        c.takes.reserve(takes.size());
        for (const auto& jt : takes) {
            c.takes.push_back(takeFromJson(jt, mediaDir));
        }
    }
    if (j.contains("comp") && j.at("comp").is_array()) {
        const auto& comp = j.at("comp");
        c.comp.reserve(comp.size());
        for (const auto& js : comp) {
            c.comp.push_back(compSegmentFromJson(js));
        }
    }
    c.compCrossfadeMs =
        std::clamp(j.value("compCrossfadeMs", 5.0), 0.0, 20.0);
    if (j.contains("automation") && j.at("automation").is_object()) {
        c.automation = automationFromJson(j.at("automation"));
    }
    if (j.contains("lanes") && j.at("lanes").is_array()) {
        const auto& lanes = j.at("lanes");
        c.lanes.reserve(lanes.size());
        for (const auto& jl : lanes) c.lanes.push_back(laneFromJson(jl));
    }
    if (j.contains("sampleEdit") && j.at("sampleEdit").is_object()) {
        c.sampleEdit = clipSampleEditFromJson(j.at("sampleEdit"));
    }
    if (j.contains("musicalAnalysis") &&
        j.at("musicalAnalysis").is_object()) {
        c.musicalAnalysis =
            musicalAnalysisFromJson(j.at("musicalAnalysis"));
    }
    c.inserts = insertsFromJson(j, "inserts");
    if (c.inserts.size() > 8) c.inserts.resize(8);
    c.expanded = j.value("expanded", false);
    // Repairs a comp that names a take the file no longer has, so a
    // hand-edited or half-written project can't produce silent segments.
    normalizeComp(c);
    return c;
}

json sendToJson(const SendModel& s) {
    return json{
        {"id", s.id},
        {"destination", s.destinationTrackId},
        {"level", s.level},
        {"preFader", s.preFader},
        {"enabled", s.enabled},
    };
}

SendModel sendFromJson(const json& j) {
    SendModel s;
    s.id = j.value("id", newUuid());
    s.destinationTrackId = j.value("destination", "");
    s.level = j.value("level", 0.5f);
    s.preFader = j.value("preFader", false);
    s.enabled = j.value("enabled", true);
    return s;
}

json trackToJson(const TrackModel& t, MediaPaths media) {
    json clips = reservedArray(t.clips.size());
    for (const auto& c : t.clips) clips.push_back(clipToJson(c, media));
    json sends = reservedArray(t.sends.size());
    for (const auto& s : t.sends) sends.push_back(sendToJson(s));
    json track{
        {"id", t.id},
        {"kind", toString(t.kind)},
        {"name", t.name},
        {"color", t.color},
        {"volume", t.volume},
        {"pan", t.pan},
        {"muted", t.muted},
        {"soloed", t.soloed},
        {"armed", t.armed},
        {"monitor", t.monitor},
        {"mono", t.mono},
        {"height", t.height},
        {"expanded", t.expanded},
        {"automationExpanded", t.automationExpanded},
        {"summing", t.summing},
        {"parentId", t.parentId},
        {"inputChannel", t.inputChannel},
        {"inputChannelCount", t.inputChannelCount},
        {"outputBusId", t.outputBusId},
        {"inputEnabled", t.inputEnabled},
        {"recordMode", toString(t.recordMode)},
        {"instrument", insertToJson(t.instrument)},
        {"sends", std::move(sends)},
        {"inserts", insertsToJson(t.inserts)},
        {"clips", std::move(clips)},
    };
    if (!t.samplerFx.ownerInstrumentId.empty() || !t.samplerFx.inserts.empty() ||
        std::abs(t.samplerFx.volume - 1.0f) > 1e-6f ||
        std::abs(t.samplerFx.pan) > 1e-6f) {
        track["samplerFx"] = json{
            {"ownerInstrumentId", t.samplerFx.ownerInstrumentId},
            {"volume", t.samplerFx.volume},
            {"pan", t.samplerFx.pan},
            {"inserts", insertsToJson(t.samplerFx.inserts)},
        };
    }
    return track;
}

TrackModel trackFromJson(const json& j, const std::string& mediaDir) {
    TrackModel t;
    t.id = j.value("id", newUuid());
    t.kind = trackKindFromString(j.value("kind", "audio"));
    t.name = j.value("name", "");
    t.color = j.value("color", 0x4A90D9u);
    t.volume = j.value("volume", 1.0f);
    t.pan = j.value("pan", 0.0f);
    t.muted = j.value("muted", false);
    t.soloed = j.value("soloed", false);
    t.armed = j.value("armed", false);
    t.monitor = j.value("monitor", false);
    t.mono = j.value("mono", false);
    t.height = j.value("height", 84.0);
    t.expanded = j.value("expanded", true);
    // Before automation had its own disclosure it borrowed `expanded`; use
    // that value when opening an older project so its visible lanes do not
    // silently change on migration.
    t.automationExpanded =
        j.value("automationExpanded", j.value("expanded", true));
    // Absent in every project written before summing folders existed, and a
    // plain folder is exactly what those were.
    t.summing = j.value("summing", false);
    t.parentId = j.value("parentId", "");
    t.inputChannel = j.value("inputChannel", 0u);
    // Absent in projects written before the capture width was a choice. Those
    // always took a pair, but a pair is what was wrong with them — a track that
    // is reopened and re-armed should record the mono source it was pointed at.
    t.inputChannelCount = std::clamp(j.value("inputChannelCount", 1u), 1u, 2u);
    t.outputBusId = j.value("outputBusId", "");
    t.inputEnabled = j.value("inputEnabled", false);
    t.recordMode = trackRecordModeFromString(j.value("recordMode", "global"));
    if (j.contains("instrument")) t.instrument = insertFromJson(j.at("instrument"));
    if (j.contains("samplerFx") && j.at("samplerFx").is_object()) {
        const auto& fx = j.at("samplerFx");
        t.samplerFx.ownerInstrumentId = fx.value("ownerInstrumentId", "");
        t.samplerFx.volume = std::clamp(fx.value("volume", 1.0f), 0.0f, 2.0f);
        t.samplerFx.pan = std::clamp(fx.value("pan", 0.0f), -1.0f, 1.0f);
        t.samplerFx.inserts = insertsFromJson(fx, "inserts");
        if (t.samplerFx.inserts.size() > 8) t.samplerFx.inserts.resize(8);
    }
    // v1/v2 projects did not carry samplerFx. They reopen with an empty chain,
    // but the fresh strip still belongs to the sampler already in the slot.
    if (t.instrument.uid == "daw.sampler" &&
        t.samplerFx.ownerInstrumentId.empty()) {
        t.samplerFx.ownerInstrumentId = t.instrument.id;
    }
    if (j.contains("sends")) {
        const auto& sends = j.at("sends");
        if (sends.is_array()) t.sends.reserve(sends.size());
        for (const auto& js : sends) t.sends.push_back(sendFromJson(js));
    }
    t.inserts = insertsFromJson(j, "inserts");
    if (j.contains("clips")) {
        const auto& clips = j.at("clips");
        if (clips.is_array()) t.clips.reserve(clips.size());
        for (const auto& jc : clips) {
            t.clips.push_back(clipFromJson(jc, mediaDir));
        }
    }
    return t;
}

json documentToJson(const ProjectModel& project, MediaPaths media) {
    json root;
    root["format"] = "vlt-project";
    root["version"] = ProjectSerializer::kFormatVersion;
    root["name"] = project.name;
    if (!project.author.empty()) root["author"] = project.author;
    if (!project.coverImagePath.empty())
        root["cover"] = mediaReference(project.coverImagePath, media);
    root["tempo"] = project.tempo;
    root["timeSigNumerator"] = project.timeSigNumerator;
    root["timeSigDenominator"] = project.timeSigDenominator;
    root["keyRoot"] = project.keyRoot;
    root["scale"] = project.scale;
    root["aiInstructions"] = project.aiInstructions;
    root["loopStart"] = project.loopStartSeconds;
    root["loopEnd"] = project.loopEndSeconds;
    root["loopEnabled"] = project.loopEnabled;
    root["renderSampleRate"] = project.sampleRate;
    root["masterVolume"] = project.masterVolume;
    root["masterPan"] = project.masterPan;
    root["masterInserts"] = insertsToJson(project.masterInserts);
    json tracks = reservedArray(project.tracks.size());
    for (const auto& t : project.tracks) tracks.push_back(trackToJson(t, media));
    root["tracks"] = std::move(tracks);
    return root;
}

audio::Result documentFromJson(ProjectModel& out, const json& root,
                               const std::string& mediaDir) {
    if (!root.is_object()) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   "project document is not a JSON object");
    }
    const std::string format = root.value("format", "");
    if (format != "vlt-project" && format != "daw-project") {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   "not a VLT project");
    }

    out = ProjectModel{};
    try {
        out.name = root.value("name", "Untitled");
        out.author = root.value("author", std::string());
        const std::string cover = root.value("cover", std::string());
        if (!cover.empty()) {
            out.coverImagePath = platform::pathToUtf8(
                platform::pathFromUtf8(mediaDir) / platform::pathFromUtf8(cover));
        }
        out.tempo = root.value("tempo", 120.0);
        out.timeSigNumerator = root.value("timeSigNumerator", 4);
        out.timeSigDenominator = root.value("timeSigDenominator", 4);
        out.keyRoot = root.value("keyRoot", 0);
        out.scale = root.value("scale", std::string("major"));
        out.aiInstructions = root.value("aiInstructions", std::string());
        out.loopStartSeconds = std::max(0.0, root.value("loopStart", 0.0));
        out.loopEndSeconds = std::max(0.0, root.value("loopEnd", 0.0));
        out.loopEnabled = root.value("loopEnabled", false);
        // v1-v6 called this durable render target `sampleRate`. The engine's
        // actual device rate is session state and is not represented here.
        out.sampleRate = root.value("renderSampleRate",
                                    root.value("sampleRate", 48000.0));
        out.masterVolume = root.value("masterVolume", 1.0f);
        out.masterPan = root.value("masterPan", 0.0f);
        out.masterInserts = insertsFromJson(root, "masterInserts");
        if (root.contains("tracks")) {
            const auto& tracks = root.at("tracks");
            if (!tracks.is_array()) {
                return audio::Result::fail(
                    audio::EngineError::UnsupportedFormat,
                    "project tracks are not an array");
            }
            out.tracks.reserve(tracks.size());
            for (const auto& track : tracks)
                out.tracks.push_back(trackFromJson(track, mediaDir));
        }
    } catch (const std::exception& error) {
        out = ProjectModel{};
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   std::string("bad project data: ") +
                                       error.what());
    }
    collab::ensureStableCollaborationIds(out);
    return audio::Result::ok();
}

/// Write `root` to `file` without ever leaving a partial file in its place: a
/// temporary sibling is written in full, then renamed over the target.
audio::Result writeJsonAtomically(const json& root, const fs::path& file) {
    std::error_code ec;
    fs::path temporary = file;
    temporary += ".tmp-" + newUuid();
    std::ofstream os(temporary, std::ios::binary | std::ios::trunc);
    if (!os) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot open " +
                                       platform::pathToUtf8(temporary));
    }
    // Stream the DOM directly.  dump(2) materialises a second project-sized
    // string before the first byte reaches disk, which is especially costly for
    // MIDI-heavy documents whose note arrays already occupy the JSON tree.
    os << std::setw(2) << root;
    os.flush();
    if (!os.good()) {
        os.close();
        fs::remove(temporary, ec);
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "write failed");
    }
    os.close();

    // POSIX rename replaces atomically. Some standard library/filesystem
    // combinations reject replacement; retain a checked fallback there.
    ec.clear();
    fs::rename(temporary, file, ec);
    if (ec) {
        std::error_code removeError;
        fs::remove(file, removeError);
        ec.clear();
        fs::rename(temporary, file, ec);
    }
    if (ec) {
        const std::string renameFailure = ec.message();
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot replace " +
                                       platform::pathToUtf8(file.filename()) +
                                       ": " + renameFailure);
    }
    return audio::Result::ok();
}

} // namespace

audio::Result ProjectSerializer::copyContentFile(const std::string& sourcePath,
                                                 const std::string& packageDir,
                                                 std::string& outFileName) {
    outFileName.clear();
    if (sourcePath.empty()) return audio::Result::ok();

    std::error_code ec;
    fs::path source = platform::pathFromUtf8(sourcePath);
    const fs::path canonical = fs::weakly_canonical(source, ec);
    if (!ec) source = canonical;
    ec.clear();
    if (!fs::is_regular_file(source, ec) || ec) {
        return audio::Result::fail(
            audio::EngineError::FileWriteError,
            "referenced content is missing or unreadable: " + sourcePath);
    }

    const fs::path content = platform::pathFromUtf8(mediaPath(packageDir));
    fs::create_directories(content, ec);
    if (ec) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot create Content directory: " +
                                       ec.message());
    }

    fs::path destination = content / source.filename();
    ec.clear();
    const bool alreadyPackaged =
        fs::equivalent(source.parent_path(), content, ec) && !ec;
    if (alreadyPackaged) {
        destination = content / source.filename();
    } else {
        ec.clear();
        const bool destinationExists =
            fs::is_regular_file(destination, ec) && !ec;
        if (destinationExists && !filesEqual(source, destination))
            destination = content / uniqueMediaName(source);
    }

    ec.clear();
    const bool destinationExists =
        fs::is_regular_file(destination, ec) && !ec;
    const bool sameFile = destinationExists &&
                          fs::equivalent(source, destination, ec) && !ec;
    if (!destinationExists || (!sameFile && !filesEqual(source, destination))) {
        ec.clear();
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing,
                      ec);
        if (ec) {
            return audio::Result::fail(
                audio::EngineError::FileWriteError,
                "cannot copy content '" + platform::pathToUtf8(source) +
                    "': " + ec.message());
        }
    }

    outFileName = platform::pathToUtf8(destination.filename());
    return audio::Result::ok();
}

audio::Result ProjectSerializer::saveDocument(const ProjectModel& project,
                                              const std::string& filePath,
                                              MediaPaths media) {
    const fs::path file = platform::pathFromUtf8(filePath);
    if (file.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            return audio::Result::fail(audio::EngineError::FileWriteError,
                                       "cannot create " +
                                           platform::pathToUtf8(file.parent_path()) +
                                           ": " +
                                           ec.message());
        }
    }
    ProjectModel persisted = project;
    collab::ensureStableCollaborationIds(persisted);
    return writeJsonAtomically(documentToJson(persisted, media), file);
}

audio::Result ProjectSerializer::loadDocument(ProjectModel& out,
                                              const std::string& filePath,
                                              const std::string& mediaDir) {
    std::ifstream is(platform::pathFromUtf8(filePath));
    if (!is) {
        return audio::Result::fail(audio::EngineError::FileNotFound, filePath);
    }

    json root;
    try {
        is >> root;
    } catch (const std::exception& e) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   std::string("bad JSON: ") + e.what());
    }

    return documentFromJson(out, root, mediaDir);
}

audio::Result ProjectSerializer::serializeDocument(const ProjectModel& project,
                                                    std::string& out,
                                                    MediaPaths media) {
    try {
        ProjectModel persisted = project;
        collab::ensureStableCollaborationIds(persisted);
        out = documentToJson(persisted, media).dump();
    } catch (const std::exception& error) {
        out.clear();
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   std::string("cannot encode project: ") +
                                       error.what());
    }
    return audio::Result::ok();
}

audio::Result ProjectSerializer::deserializeDocument(
    ProjectModel& out, std::string_view bytes, const std::string& mediaDir) {
    json root;
    try {
        root = json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   std::string("bad JSON: ") + error.what());
    }
    return documentFromJson(out, root, mediaDir);
}

audio::Result ProjectSerializer::save(const ProjectModel& project,
                                      const std::string& packageDir) {
    std::error_code ec;
    fs::create_directories(platform::pathFromUtf8(packageDir), ec);
    if (ec) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot create project directory: " + ec.message());
    }
    const fs::path media = platform::pathFromUtf8(mediaPath(packageDir));
    fs::create_directories(media, ec);
    if (ec) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot create media directory: " + ec.message());
    }
    // Plugin state chunks are written by EngineController before it calls this
    // (only it holds the live instances); the directory is created here so the
    // package layout is defined in exactly one place.
    fs::create_directories(platform::pathFromUtf8(statePath(packageDir)), ec);
    if (ec) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot create state directory: " + ec.message());
    }

    // Serialize a copy whose paths name the files inside Content/. A stable hash
    // prefix prevents two unrelated "take.wav" files from overwriting each
    // other. Files already in this package retain their name, so repeated saves
    // do not create a new generation on every pass.
    ProjectModel persisted = project;
    std::unordered_map<std::string, fs::path> copied;
    std::unordered_map<std::string, std::string> destinationOwners;
    std::string copyFailure;
    auto copyMedia = [&](std::string& path) {
        if (path.empty() || !copyFailure.empty()) return;

        fs::path source = platform::pathFromUtf8(path);
        ec.clear();
        const fs::path canonical = fs::weakly_canonical(source, ec);
        if (!ec) source = canonical;
        const std::string key = platform::pathToUtf8(source.lexically_normal());
        if (const auto found = copied.find(key); found != copied.end()) {
            path = platform::pathToUtf8(found->second);
            return;
        }

        ec.clear();
        if (!fs::is_regular_file(source, ec) || ec) {
            copyFailure = "referenced media is missing or unreadable: " + path;
            return;
        }

        fs::path destination;
        ec.clear();
        const bool alreadyPackaged = fs::equivalent(source.parent_path(), media, ec);
        if (!ec && alreadyPackaged) {
            destination = media / source.filename();
        } else {
            ec.clear();
            const std::string basename = platform::pathToUtf8(source.filename());
            const auto owner = destinationOwners.find(basename);
            const fs::path plainDestination = media / source.filename();
            const bool plainExists = fs::is_regular_file(plainDestination, ec) && !ec;
            const bool plainMatches = plainExists && filesEqual(source, plainDestination);
            const bool claimedByOther = owner != destinationOwners.end() &&
                                        owner->second != key;
            destination = (!claimedByOther && (!plainExists || plainMatches))
                              ? plainDestination
                              : media / uniqueMediaName(source);
        }
        destinationOwners.emplace(platform::pathToUtf8(destination.filename()), key);

        ec.clear();
        const bool destinationExists = fs::is_regular_file(destination, ec) && !ec;
        const bool sameFile = destinationExists &&
                              fs::equivalent(source, destination, ec) && !ec;
        const bool sameContent = destinationExists && (sameFile || filesEqual(source, destination));
        if (!sameContent) {
            ec.clear();
            fs::copy_file(source, destination,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                copyFailure = "cannot copy media '" +
                              platform::pathToUtf8(source) + "': " +
                              ec.message();
                return;
            }
        }
        copied.emplace(key, destination);
        path = platform::pathToUtf8(destination);
    };
    for (auto& t : persisted.tracks) {
        for (auto& c : t.clips) {
            copyMedia(c.filePath);
            copyMedia(c.offlineProcess.renderedFilePath);
            for (auto& take : c.takes) copyMedia(take.filePath);
        }
    }
    copyMedia(persisted.coverImagePath);
    if (!copyFailure.empty()) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   std::move(copyFailure));
    }

    const fs::path manifest =
        preferredManifestPath(platform::pathFromUtf8(packageDir));
    const audio::Result saved = saveDocument(
        persisted, platform::pathToUtf8(manifest), MediaPaths::Basenames);
    if (saved && manifest.filename() != fs::path(kProjectFile)) {
        ec.clear();
        fs::remove(platform::pathFromUtf8(packageDir) / kProjectFile, ec);
    }
    return saved;
}

std::string ProjectSerializer::manifestPath(const std::string& packageDir) {
    const fs::path package = platform::pathFromUtf8(packageDir);
    const fs::path preferred = preferredManifestPath(package);
    std::error_code ec;
    if (fs::is_regular_file(preferred, ec))
        return platform::pathToUtf8(preferred);

    const fs::path legacy = package / kProjectFile;
    ec.clear();
    if (fs::is_regular_file(legacy, ec))
        return platform::pathToUtf8(legacy);

    const fs::path legacyJson = package / "project.json";
    ec.clear();
    if (fs::is_regular_file(legacyJson, ec))
        return platform::pathToUtf8(legacyJson);

    ec.clear();
    for (fs::directory_iterator it(package, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (extension == ".vlt")
            return platform::pathToUtf8(it->path());
    }
    return platform::pathToUtf8(preferred);
}

audio::Result ProjectSerializer::load(ProjectModel& out,
                                      const std::string& packageDir) {
    return loadDocument(out, manifestPath(packageDir),
                        mediaPath(packageDir));
}

} // namespace daw
