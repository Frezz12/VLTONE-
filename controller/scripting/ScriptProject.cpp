#include "ScriptProject.hpp"
#include "plugins/PluginManager.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace daw::scripting {
namespace {
// Bound the response while constructing it, before IPC serialization can allocate
// a complete project. Leave room for the protocol envelope and callback handles.
class ResponseBudget {
  public:
    size_t bytes = 0;
    void account(const Json &value, size_t children = 0) {
        bytes += value.dump().size() + 1 - children;
        if (bytes > 1536 * 1024)
            throw std::runtime_error("SDK result exceeds 1.5 MiB; use smaller pages or filters");
    }
    void append(Json &array, Json value, size_t children = 0) {
        account(value, children);
        array.push_back(std::move(value));
    }
};
double number(const Json &j, const char *key, double low, double high) {
    const double value = j.at(key).get<double>();
    if (!std::isfinite(value) || value < low || value > high)
        throw std::runtime_error(std::string("Out of range: ") + key);
    return value;
}
int integer(const Json &j, const char *key, int low, int high) {
    const auto value = number(j, key, low, high);
    if (std::floor(value) != value)
        throw std::runtime_error(std::string("Expected integer: ") + key);
    return int(value);
}
std::uint64_t eventOrder(const Json &j, const char *key) {
    if (!j.contains(key))
        return 0;
    const auto &value = j[key];
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::runtime_error(std::string("Expected nonnegative integer: ") + key);
    return value.get<std::uint64_t>();
}
const TrackModel &track(EngineController &c, const std::string &id) {
    const auto *t = c.project().findTrack(id);
    if (!t)
        throw std::runtime_error("Track no longer exists");
    return *t;
}
const ClipModel &clip(EngineController &c, const std::string &t, const std::string &id) {
    for (const auto &item : track(c, t).clips)
        if (item.id == id)
            return item;
    throw std::runtime_error("Clip no longer exists");
}
Json noteJson(const NoteModel &n) {
    return {{"id", n.id},
            {"pitch", n.pitch},
            {"start", n.startBeats},
            {"length", n.lengthBeats},
            {"velocity", n.velocity},
            {"muted", n.muted},
            {"pan", n.pan},
            {"channel", n.channel},
            {"color", n.color},
            {"releaseVelocity", n.releaseVelocity},
            {"startOrder", n.startOrder},
            {"endOrder", n.endOrder}};
}
} // namespace
std::string parameterIdentity(EngineController &c, const std::string &t, const std::string &d) {
    const auto id = c.insertIdentity(t, d);
    return std::to_string(id.project) + ":" + std::to_string(id.instance);
}
Json projectSnapshot(EngineController &c, const Json &query) {
    const auto selectedTrack = query.value("track", ""), selectedClip = query.value("clip", "");
    const auto bounded = [&](const char *name, size_t fallback, size_t maximum) {
        if (!query.contains(name))
            return fallback;
        const auto n = query.at(name).get<std::int64_t>();
        if (n < 0 || std::uint64_t(n) > maximum)
            throw std::runtime_error("Invalid snapshot range");
        return size_t(n);
    };
    const auto trackOffset = bounded("trackOffset", 0, 1000000),
               trackLimit = bounded("trackLimit", 128, 4096);
    const auto clipOffset = bounded("clipOffset", 0, 1000000),
               clipLimit = bounded("clipLimit", 256, 10000);
    const auto eventOffset = bounded("eventOffset", 0, 100000000),
               eventLimit = bounded("eventLimit", 4096, 10000);
    Json result{{"name", c.projectName()},
                {"revision", c.projectRevision()},
                {"generation", c.projectGeneration()},
                {"tempo", c.tempo()},
                {"sampleRate", c.sampleRate()},
                {"minimumClipLength", EngineController::minimumClipDurationSeconds * c.tempo() / 60.0},
                {"readOnly", c.hasCloudProjectBinding()},
                {"trackCount", c.project().tracks.size()},
                {"trackOffset", trackOffset},
                {"tracks", Json::array()}};
    const auto beats = [&](double seconds) { return seconds * c.tempo() / 60.0; };
    ResponseBudget budget;
    budget.account(result);
    size_t trackIndex = 0;
    for (const auto &t : c.project().tracks) {
        const auto index = trackIndex++;
        if (!selectedTrack.empty() ? t.id != selectedTrack
                                   : (index < trackOffset || index >= trackOffset + trackLimit))
            continue;
        const auto trackBytes = budget.bytes;
        Json devices = Json::array();
        const auto device = [&](const InsertModel &d, bool instrument, const char *scope = "insert",
                                const std::string &clipId = "") {
            if (!d.id.empty())
                budget.append(devices, {{"id", d.id},
                                        {"name", d.name},
                                        {"instrument", instrument},
                                        {"scope", scope},
                                        {"clip", clipId},
                                        {"identity", parameterIdentity(c, t.id, d.id)},
                                        {"bypassed", d.bypassed}});
        };
        device(t.instrument, true, "instrument");
        for (const auto &d : t.inserts)
            device(d, false);
        if (t.channelColor)
            device(*t.channelColor, false, "color");
        for (const auto &d : t.miniModules)
            device(d, false, "mini");
        for (const auto &d : t.samplerFx.inserts)
            device(d, false, "samplerFx");
        Json clips = Json::array(), sends = Json::array();
        for (const auto &s : t.sends)
            budget.append(sends, {{"id", s.id},
                                  {"destination", s.destinationTrackId},
                                  {"level", s.level},
                                  {"enabled", s.enabled},
                                  {"preFader", s.preFader}});
        size_t clipIndex = 0;
        for (const auto &item : t.clips) {
            for (const auto &d : item.inserts)
                device(d, false, "clipFx", item.id);
            const auto index = clipIndex++;
            if (!selectedClip.empty() ? item.id != selectedClip
                                      : (index < clipOffset || index >= clipOffset + clipLimit))
                continue;
            const auto eventBytes = budget.bytes;
            Json notes = Json::array(), points = Json::array();
            if (query.value("includeEvents", true)) {
                for (size_t i = eventOffset;
                     i < std::min(item.notes.size(), eventOffset + eventLimit); ++i)
                    budget.append(notes, noteJson(item.notes[i]));
                for (size_t i = eventOffset;
                     i < std::min(item.automation.points.size(), eventOffset + eventLimit); ++i) {
                    const auto &p = item.automation.points[i];
                    budget.append(points,
                                  {{"id", p.id},
                                   {"beat", p.beats},
                                   {"value", c.automationToPlain(item.automation.target, p.value)},
                                   {"curve", p.curve},
                                   {"shape", toString(p.shape)},
                                   {"eventOrder", p.eventOrder}});
                }
            }
            budget.append(clips,
                          {{"id", item.id},
                           {"name", item.name},
                           {"kind", item.kind == ClipKind::Midi         ? "midi"
                                    : item.kind == ClipKind::Automation ? "automation"
                                                                        : "audio"},
                           {"start", beats(item.startSeconds)},
                           {"length", beats(item.durationSeconds)},
                           {"offset", beats(item.offsetSeconds)},
                           {"gain", item.gain},
                           {"muted", item.muted},
                           {"notes", notes},
                           {"points", points},
                           {"noteCount", item.notes.size()},
                           {"pointCount", item.automation.points.size()},
                           {"eventOffset", eventOffset},
                           {"source", item.filePath},
                           {"reversed", item.sampleEdit.reverse},
                           {"looped", item.sampleEdit.loopMode != 0},
                           {"stretched", item.warp.enabled ||
                                             std::abs(item.sampleEdit.stretchTime - 1.0) > 1e-9 ||
                                             std::abs(item.sampleEdit.stretchPitch) > 1e-9},
                           {"composite", !item.takes.empty() || item.audioEdit.initialized}},
                          budget.bytes - eventBytes);
        }
        budget.append(result["tracks"],
                      {{"id", t.id},
                       {"name", t.name},
                       {"kind", int(t.kind)},
                       {"volume", t.volume},
                       {"pan", t.pan},
                       {"muted", t.muted},
                       {"solo", t.soloed},
                       {"locked", c.isTrackEditLocked(t.id)},
                       {"parent", t.parentId},
                       {"output", t.outputBusId},
                       {"clips", clips},
                       {"clipCount", t.clips.size()},
                       {"clipOffset", clipOffset},
                       {"devices", devices},
                       {"sends", sends}},
                      budget.bytes - trackBytes);
    }
    const auto masterBytes = budget.bytes;
    Json masterDevices = Json::array();
    for (const auto &d : c.project().masterInserts)
        if (!d.id.empty())
            budget.append(
                masterDevices,
                {{"id", d.id},
                 {"name", d.name},
                 {"instrument", false},
                 {"identity", parameterIdentity(c, EngineController::kMasterChannelId, d.id)},
                 {"bypassed", d.bypassed}});
    for (const auto &d : c.project().masterMiniModules)
        if (!d.id.empty())
            budget.append(
                masterDevices,
                {{"id", d.id},
                 {"name", d.name},
                 {"instrument", false},
                 {"scope", "mini"},
                 {"identity", parameterIdentity(c, EngineController::kMasterChannelId, d.id)},
                 {"bypassed", d.bypassed}});
    result["master"] = {{"id", EngineController::kMasterChannelId},
                        {"volume", c.masterVolume()},
                        {"pan", c.masterPan()},
                        {"devices", masterDevices}};
    budget.account(result["master"], budget.bytes - masterBytes);
    return result;
}
Json parameters(EngineController &c, const Json &a) {
    const auto t = a.at("track").get<std::string>(), d = a.at("device").get<std::string>();
    if (!c.hasInsert(t, d))
        throw std::runtime_error("Device no longer exists");
    Json result = Json::array();
    ResponseBudget budget;
    const auto metadata = c.insertParameters(t, d);
    const auto offset = a.value("offset", std::int64_t(0));
    const auto limit = a.value("limit", std::int64_t(256));
    if (offset < 0 || limit < 1 || limit > 4096)
        throw std::runtime_error("Invalid parameter page");
    size_t matched = 0;
    for (const auto &p : metadata) {
        if (a.contains("parameter") && a["parameter"] != p.id)
            continue;
        if (matched++ < size_t(offset))
            continue;
        if (result.size() >= size_t(limit))
            break;
        if (!std::isfinite(p.minValue) || !std::isfinite(p.maxValue) || p.minValue > p.maxValue)
            throw std::runtime_error("Plugin supplied an invalid parameter range");
        Json choices = Json::array();
        if (p.isStepped && p.maxValue - p.minValue <= 128) {
            const double first = std::ceil(p.minValue);
            const int count = int(std::floor(p.maxValue) - first);
            for (int step = 0; step <= count; ++step) {
                const double value = first + step;
                choices.push_back(
                    {{"value", value}, {"label", c.insertParameterText(t, d, p.id, value)}});
            }
        }
        budget.append(result, {{"id", p.id},
                               {"name", p.name},
                               {"unit", p.unit},
                               {"min", p.minValue},
                               {"max", p.maxValue},
                               {"default", p.defaultValue},
                               {"value", c.insertParameter(t, d, p.id)},
                               {"stepped", p.isStepped},
                               {"readOnly", p.isReadOnly},
                               {"choices", choices},
                               {"identity", parameterIdentity(c, t, d)}});
    }
    return result;
}
Json deviceCatalog(EngineController &c, const Json &a) {
    Json result = Json::array();
    const auto query = a.value("query", "");
    ResponseBudget budget;
    const auto offset = a.value("offset", std::int64_t(0));
    const auto limit = a.value("limit", std::int64_t(256));
    if (offset < 0 || limit < 1 || limit > 4096)
        throw std::runtime_error("Invalid device page");
    size_t matched = 0;
    for (const auto &p : c.pluginManager().plugins()) {
        if (!query.empty() && p.name.find(query) == std::string::npos &&
            p.vendor.find(query) == std::string::npos)
            continue;
        if (matched++ < size_t(offset))
            continue;
        if (result.size() >= size_t(limit))
            break;
        budget.append(result, {{"uid", p.uid},
                               {"format", std::string(plugins::toString(p.format))},
                               {"name", p.name},
                               {"vendor", p.vendor},
                               {"instrument", p.isInstrument}});
    }
    return result;
}
Json parameter(EngineController &c, const Json &a, bool write) {
    const auto t = a.at("track").get<std::string>(), d = a.at("device").get<std::string>(),
               p = a.at("parameter").get<std::string>();
    if (!c.hasInsert(t, d) || a.at("identity") != parameterIdentity(c, t, d))
        throw std::runtime_error("Device was replaced or removed");
    const auto infos = c.insertParameters(t, d);
    const auto info =
        std::find_if(infos.begin(), infos.end(), [&](const auto &i) { return i.id == p; });
    if (info == infos.end())
        throw std::runtime_error("Unknown parameter");
    if (write) {
        if (c.hasCloudProjectBinding())
            throw std::runtime_error("Collaborative projects are read-only for scripts");
        if (info->isReadOnly)
            throw std::runtime_error("Parameter is read-only");
        const double value = number(a, "value", info->minValue, info->maxValue);
        const auto before = c.insertParameter(t, d, p);
        c.setInsertParameter(t, d, p, value);
        c.commitInsertParameterEdit(t, d, p, before, a.value("label", "Script parameter"));
    }
    return c.insertParameter(t, d, p);
}
Json applyEdits(EngineController &c, const Json &a) {
    const auto &operations = a.at("operations");
    if (!operations.is_array() || operations.size() > 4096)
        throw std::runtime_error("Invalid edit batch");
    Json created = Json::array();
    bool deviceState = false;
    for (const auto &op : operations) {
        const auto action = op.value("op", "");
        deviceState |=
            action.starts_with("device.") || action == "track.remove" || action == "clip.remove";
    }
    const auto result = c.applyScriptTransaction(
        a.at("revision").get<std::uint64_t>(), a.at("label").get<std::string>(),
        [&] {
            for (const auto &op : operations) {
                const auto action = op.at("op").get<std::string>();
                const auto t = op.value("track", ""), id = op.value("clip", "");
                const auto seconds = [&](const char *key) {
                    return number(op, key, 0, 1e9) * 60.0 / c.tempo();
                };
                if (action != "track.add" && t.empty())
                    throw std::runtime_error("Operation requires a track");
                if ((action.starts_with("clip.") && action != "clip.addMidi" &&
                     action != "clip.importAudio") ||
                    action.starts_with("notes.") || action == "automation.set")
                    if (id.empty())
                        throw std::runtime_error("Operation requires a clip");
                const bool master = t == EngineController::kMasterChannelId;
                if (master && action != "track.mix" && !action.starts_with("device."))
                    throw std::runtime_error("Operation is unavailable on master");
                if (!t.empty() && !master) {
                    track(c, t);
                    if (c.isTrackEditLocked(t))
                        throw std::runtime_error("Track is locked");
                }
                if (!id.empty()) {
                    clip(c, t, id);
                    if (c.isClipContentEditLocked(t, id))
                        throw std::runtime_error("Clip is locked");
                }
                if (action == "track.add") {
                    const auto kind = op.value("kind", "audio");
                    if (kind != "midi" && kind != "instrument" && kind != "bus" && kind != "audio")
                        throw std::runtime_error("Unknown track kind");
                    const auto k = kind == "midi"         ? TrackKind::Midi
                                   : kind == "instrument" ? TrackKind::Instrument
                                   : kind == "bus"        ? TrackKind::Bus
                                                          : TrackKind::Audio;
                    created.push_back(c.addTrack(k, op.value("name", "Script track")));
                } else if (action == "track.remove")
                    c.removeTrack(t);
                else if (action == "track.rename")
                    c.renameTrack(t, op.at("name"));
                else if (action == "track.mix") {
                    if (master) {
                        if (op.contains("volume"))
                            c.setMasterVolume(float(number(op, "volume", 0, 4)));
                        if (op.contains("pan"))
                            c.setMasterPan(float(number(op, "pan", -1, 1)));
                        if (op.contains("muted") || op.contains("solo") || op.contains("output"))
                            throw std::runtime_error("Master supports volume and pan");
                        continue;
                    }
                    if (op.contains("volume"))
                        c.setTrackVolume(t, float(number(op, "volume", 0, 4)));
                    if (op.contains("pan"))
                        c.setTrackPan(t, float(number(op, "pan", -1, 1)));
                    if (op.contains("muted"))
                        c.setTrackMuted(t, op.at("muted"));
                    if (op.contains("solo"))
                        c.setTrackSoloed(t, op.at("solo"));
                    if (op.contains("output") && !c.setTrackOutputBus(t, op.at("output")))
                        throw std::runtime_error("Invalid routing");
                } else if (action == "track.move") {
                    if (!c.moveTrack(t, op.at("index").get<size_t>(), op.value("parent", "")))
                        throw std::runtime_error("Cannot move track");
                } else if (action == "send.add") {
                    const auto s = c.addSend(t, op.at("destination"));
                    if (s.empty())
                        throw std::runtime_error("Cannot create send");
                    created.push_back(s);
                } else if (action == "send.remove")
                    c.removeSend(t, op.at("send"));
                else if (action == "send.set") {
                    const auto s = op.at("send").get<std::string>();
                    if (op.contains("level"))
                        c.setSendLevel(
                            t, s, float(number(op, "level", 0, EngineController::kMaxSendLevel)));
                    if (op.contains("enabled"))
                        c.setSendEnabled(t, s, op.at("enabled"));
                    if (op.contains("preFader"))
                        c.setSendPreFader(t, s, op.at("preFader"));
                } else if (action == "clip.importAudio") {
                    const auto imported = c.importAudio(op.at("path"), t, seconds("start"));
                    if (imported.empty())
                        throw std::runtime_error("Audio could not be imported");
                    created.push_back(imported);
                } else if (action == "clip.addMidi")
                    created.push_back(c.addMidiClip(t, seconds("start"), seconds("length")));
                else if (action == "clip.remove")
                    c.removeClip(t, id);
                else if (action == "clip.rename")
                    c.setClipName(t, id, op.at("name"));
                else if (action == "clip.move")
                    c.setClipStartSeconds(t, id, seconds("start"));
                else if (action == "clip.trim")
                    c.setClipTrim(t, id, seconds("start"), seconds("offset"), seconds("length"));
                else if (action == "clip.gain")
                    c.setClipGain(t, id, float(number(op, "gain", 0, 64)));
                else if (action == "clip.fade")
                    c.setClipFade(t, id, seconds("in"), seconds("out"));
                else if (action == "clip.mute")
                    c.setClipMuted(t, id, op.at("muted"));
                else if (action == "clip.copy")
                    created.push_back(c.duplicateClipAt(t, id, seconds("start")));
                else if (action == "clip.split") {
                    const auto newId = c.splitClip(t, id, seconds("at"));
                    if (newId.empty())
                        throw std::runtime_error("Split must leave more than 20 ms on each side of the clip");
                    created.push_back(newId);
                } else if (action == "notes.set" || action == "notes.update") {
                    if (clip(c, t, id).kind != ClipKind::Midi)
                        throw std::runtime_error("Expected MIDI clip");
                    const auto &values = op.at("notes");
                    if (!values.is_array() || values.size() > 100000)
                        throw std::runtime_error("Invalid notes");
                    std::vector<NoteModel> notes =
                        action == "notes.update"
                            ? std::vector<NoteModel>(clip(c, t, id).notes.begin(),
                                                     clip(c, t, id).notes.end())
                            : std::vector<NoteModel>{};
                    if (action == "notes.update" && op.contains("remove")) {
                        const auto removed = op["remove"].get<std::unordered_set<std::string>>();
                        std::erase_if(notes, [&](const auto &n) { return removed.contains(n.id); });
                    }
                    std::unordered_map<std::string, size_t> noteIndices;
                    for (size_t i = 0; i < notes.size(); ++i)
                        noteIndices.emplace(notes[i].id, i);
                    std::unordered_set<std::string> ids;
                    for (const auto &v : values) {
                        NoteModel n;
                        n.id = v.at("id");
                        if (n.id.empty() || !ids.insert(n.id).second)
                            throw std::runtime_error("Duplicate note identity");
                        n.pitch = integer(v, "pitch", 0, 127);
                        n.startBeats = number(v, "start", 0, 1e9);
                        n.lengthBeats = number(v, "length", 1e-6, 1e9);
                        n.velocity = integer(v, "velocity", 1, 127);
                        n.muted = v.value("muted", false);
                        n.pan = v.contains("pan") ? float(number(v, "pan", -1, 1)) : 0;
                        n.channel = v.contains("channel") ? integer(v, "channel", 0, 15) : 0;
                        n.color = v.value("color", 0u);
                        n.releaseVelocity = v.contains("releaseVelocity")
                                                ? integer(v, "releaseVelocity", 0, 127)
                                                : 0;
                        n.startOrder = eventOrder(v, "startOrder");
                        n.endOrder = eventOrder(v, "endOrder");
                        const auto existing = noteIndices.find(n.id);
                        if (existing != noteIndices.end())
                            notes[existing->second] = n;
                        else {
                            noteIndices.emplace(n.id, notes.size());
                            notes.push_back(n);
                        }
                    }
                    c.setClipNotes(t, id, std::move(notes), a.at("label"));
                } else if (action == "automation.create") {
                    const auto &value = op.at("target");
                    AutomationTarget target;
                    target.channelId = t;
                    const auto kind = value.at("kind").get<std::string>();
                    if (kind == "volume")
                        target.kind = AutomationTargetKind::TrackVolume;
                    else if (kind == "pan")
                        target.kind = AutomationTargetKind::TrackPan;
                    else if (kind == "mute")
                        target.kind = AutomationTargetKind::TrackMute;
                    else if (kind == "send") {
                        target.kind = AutomationTargetKind::SendLevel;
                        target.sendId = value.at("send");
                    } else if (kind == "parameter") {
                        parameter(c, value, false);
                        target.kind = AutomationTargetKind::PluginParameter;
                        target.channelId = value.at("track");
                        target.slotId = value.at("device");
                        target.parameterId = value.at("parameter");
                    } else
                        throw std::runtime_error("Unknown automation target kind");
                    const auto added = c.ensureAutomation(target);
                    if (added.first.empty() || added.second.empty())
                        throw std::runtime_error("Automation could not be created");
                    created.push_back({{"track", added.first}, {"clip", added.second}});
                } else if (action == "automation.set") {
                    if (clip(c, t, id).kind != ClipKind::Automation)
                        throw std::runtime_error("Expected automation clip");
                    const auto &target = clip(c, t, id).automation.target;
                    if (!op.at("points").is_array() || op["points"].size() > 100000)
                        throw std::runtime_error("Invalid automation points");
                    std::vector<AutomationPoint> points;
                    std::unordered_set<std::string> pointIds;
                    for (const auto &v : op.at("points")) {
                        AutomationPoint p;
                        p.id = v.at("id");
                        if (p.id.empty() || !pointIds.insert(p.id).second)
                            throw std::runtime_error("Duplicate automation point identity");
                        const auto shape = v.value("shape", "linear");
                        if (shape != "linear" && shape != "hold" && shape != "scurve")
                            throw std::runtime_error("Unknown automation segment shape");
                        p.shape = automationSegmentFromString(shape);
                        p.eventOrder = eventOrder(v, "eventOrder");
                        p.beats = number(v, "beat", 0, 1e9);
                        p.value = c.plainToAutomation(target, number(v, "value", -1e12, 1e12));
                        p.curve = v.contains("curve") ? number(v, "curve", -1, 1) : 0;
                        points.push_back(p);
                    }
                    c.setAutomationPoints(t, id, std::move(points));
                } else if (action == "device.add" || action == "device.replace" ||
                           action == "device.instrument") {
                    const auto descriptor = c.pluginManager().find(
                        plugins::formatFromString(op.at("format").get<std::string>()),
                        op.at("uid"));
                    if (!descriptor)
                        throw std::runtime_error("Device is not in the installed catalog");
                    if (action == "device.instrument") {
                        if (master)
                            throw std::runtime_error("Master cannot contain an instrument");
                        if (!c.setTrackInstrumentPlugin(t, *descriptor))
                            throw std::runtime_error("Instrument could not be loaded");
                        created.push_back(track(c, t).instrument.id);
                    } else if (action == "device.replace") {
                        if (op.at("identity") != parameterIdentity(c, t, op.at("device")))
                            throw std::runtime_error("Device identity changed");
                        if (!c.replaceInsert(t, op.at("device"), *descriptor))
                            throw std::runtime_error("Device could not be replaced");
                    } else {
                        const auto added =
                            c.addInsert(t, *descriptor, op.value("index", size_t(-1)));
                        if (added.empty())
                            throw std::runtime_error("Device could not be loaded");
                        created.push_back(added);
                    }
                } else if (action == "device.bypass" || action == "device.remove" ||
                           action == "device.move") {
                    const auto device = op.at("device").get<std::string>();
                    if (!c.hasInsert(t, device) ||
                        op.at("identity") != parameterIdentity(c, t, device))
                        throw std::runtime_error("Device identity changed");
                    if (action == "device.bypass")
                        c.setInsertBypassed(t, device, op.at("bypassed"));
                    else if (action == "device.move")
                        c.moveInsert(t, device, op.at("index").get<size_t>());
                    else
                        c.removeInsert(t, device);
                } else
                    throw std::runtime_error("Unknown edit operation: " + action);
            }
        },
        deviceState);
    if (!result)
        throw std::runtime_error(result.message());
    return {{"revision", c.projectRevision()}, {"created", created}};
}
} // namespace daw::scripting
