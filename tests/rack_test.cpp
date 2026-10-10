#include "ChannelStripPreset.hpp"
#include "EngineController.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/SamplerInstance.hpp"
#include "ProjectSerializer.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>

using Controller = daw::EngineController;
namespace fs = std::filesystem;
static int failures = 0;
static bool check(bool condition, const char* text) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", text);
    if (!condition)
        ++failures;
    return condition;
}
static std::string document(const Controller& c) {
    std::string text;
    daw::ProjectSerializer::serializeDocument(c.project(), text);
    return text;
}
static bool near(double a, double b) {
    return std::abs(a - b) < 1e-5;
}
static daw::AutomationTarget targetOf(const Controller& c, const std::string& lane, const std::string& id) {
    if (const auto* track = c.project().findTrack(lane))
        for (const auto& clip : track->clips)
            if (clip.id == id)
                return clip.automation.target;
    return {};
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Controller c;
    if (!check(c.initialize(48000, 256, false).isOk(), "headless controller"))
        return 1;
    const auto a = c.addTrack(daw::TrackKind::Audio, "A"), b = c.addTrack(daw::TrackKind::Audio, "B");
    const auto& eq = daw::plugins::equalizer::EqualizerInstance::staticDescriptor();
    const auto one = c.addInsert(a, eq), two = c.addInsert(a, eq), three = c.addInsert(a, eq);
    check(!one.empty() && !two.empty() && !three.empty(), "three independent devices");
    c.setInsertParameter(a, one, "output.gain", -6.);
    c.setInsertChannelMode(a, one, daw::PluginChannelMode::DualMono);
    c.setInsertEditorChannel(a, one, daw::PluginEditorChannel::Right);
    c.setInsertParameter(a, one, "output.gain", -12.);
    c.pumpPreviewPluginEvents();
    check(c.setRackParameters(a, one, {"output.gain", "band.0.frequency", ""}),
          "rack cells configured by stable parameter ID");
    const auto identity = c.insertIdentity(a, one);
    std::string group;
    const auto depth = c.undoDepth();
    check(c.createRackGroup(a, {one, two}, "Vocal", &group), "adjacent effects form a group");
    check(c.undoDepth() == depth + 1 && c.insertIdentity(a, one) == identity,
          "group is metadata and one undo step, without a new processor");
    check(!c.createRackGroup(a, {one, three}, "Invalid"), "disjoint and overlapping groups rejected");
    check(!c.createRackGroup(a, {three}, "Invalid"), "a new group requires two effects");
    daw::AutomationTarget target;
    target.kind = daw::AutomationTargetKind::PluginParameter;
    target.channelId = a;
    target.slotId = one;
    target.parameterId = "output.gain";
    const auto lane = c.addAutomationLane(a, target), clip = c.addAutomationClip(lane, target, 0, 2);
    Controller::ChannelSnapshot beforeRack;
    c.captureRackSelection(a, {one, two, three}, beforeRack);
    const auto beforeDepth = c.undoDepth();
    std::vector<std::string> landed;
    check(c.transferRackSelection(a, {two, one}, b, 0, false, {}, &landed).isOk(),
          "moves whole group between channels in source order");
    check(landed == std::vector<std::string>{one, two} && c.project().findTrack(a)->inserts.size() == 1,
          "real move retains IDs");
    check(c.rackGroups(b).size() == 1 && c.rackGroups(b)[0].id == group && c.rackGroups(a).empty(),
          "group follows moved effects");
    check(targetOf(c, lane, clip).channelId == b, "automation retargeted without copying its curve");
    check(c.undoDepth() == beforeDepth + 1, "transfer creates exactly one undo step");
    c.undo();
    Controller::ChannelSnapshot undone;
    c.captureRackSelection(a, {one, two, three}, undone);
    check(undone.inserts.size() == beforeRack.inserts.size()
        && std::equal(undone.inserts.begin(), undone.inserts.end(), beforeRack.inserts.begin(),
            [](const auto& lhs, const auto& rhs) { return lhs.model.id == rhs.model.id
                && lhs.state == rhs.state && lhs.rightState == rhs.rightState; })
        && undone.rackGroups == beforeRack.rackGroups && c.project().findTrack(b)->inserts.empty()
        && targetOf(c, lane, clip).channelId == a,
          "undo restores both chains, both mono states, groups and automation");
    c.redo();
    check(c.rackGroups(b).size() == 1 && targetOf(c, lane, clip).channelId == b,
          "redo restores destination and automation");
    Controller::ChannelSnapshot copy;
    check(c.captureRackSelection(b, {one, two}, copy).isOk() && copy.rackGroups.size() == 1,
          "selection snapshot contains whole group");
    check(!copy.inserts[0].state.empty() && !copy.inserts[0].rightState.empty(),
          "both dual-mono states captured");
    check(c.pasteRackSelection(a, copy, 0, {}, &landed).isOk(), "group pasted at exact chain position");
    check(landed.size() == 2 && landed[0] != one && c.rackGroups(a)[0].id != group,
          "copy receives independent device and group IDs");
    const auto copied = landed[0];
    check(c.insertModel(a, copied)->rackParameterIds == c.insertModel(b, one)->rackParameterIds,
          "copy retains assigned rack cells");
    c.setInsertEditorChannel(a, copied, daw::PluginEditorChannel::Left);
    check(near(c.insertParameter(a, copied, "output.gain"), -6), "copy restores left state");
    c.setInsertEditorChannel(a, copied, daw::PluginEditorChannel::Right);
    check(near(c.insertParameter(a, copied, "output.gain"), -12), "copy restores right state");
    check(targetOf(c, lane, clip).channelId == b, "copy leaves automation at source");
    Controller::ChannelSnapshot partial;
    c.captureRackSelection(b, {one}, partial);
    check(partial.rackGroups.empty(), "partial selection never copies a partial group");
    check(c.transferRackSelection(b, {one}, a, c.project().findTrack(a)->inserts.size(), false).isOk(),
          "individual effect moves out of group");
    check(c.rackGroups(b).size() == 1 && c.rackGroups(b)[0].insertIds == std::vector<std::string>{two},
          "one-member group remains");
    check(c.transferRackSelection(a, {one}, b, 0, false, group).isOk(),
          "effect moves back into an explicit group");
    const auto originalB = c.project().findTrack(b)->inserts;
    const auto stable = document(c);
    const auto stableDepth = c.undoDepth();
    auto broken = copy;
    broken.inserts[0].model.uid = "missing.rack.test";
    broken.inserts[0].model.path = "missing.rack.test";
    check(!c.pasteRackSelection(b, broken, 0), "failed plugin load rejects complete paste");
    check(document(c) == stable && c.undoDepth() == stableDepth,
          "failed paste changes neither chains nor undo history");
    auto corrupt = copy;
    corrupt.inserts[0].state = {0xff, 0xff, 0xff};
    check(!c.pasteRackSelection(b, corrupt, 0), "invalid opaque state rejects complete paste");
    check(document(c) == stable, "failed restoration leaves originals untouched");
    check(!c.pasteRackSelection(b, copy, 1, group), "nested group insertion rejected atomically");
    check(document(c) == stable, "nested group rejection leaves chain unchanged");
    const auto added = c.addInsert(b, eq, 1);
    check(!added.empty() && c.rackGroups(b)[0].insertIds.size() == 3,
          "adding an effect between members joins the group");
    c.undo();
    check(c.rackGroups(b)[0].insertIds.size() == 2, "undo insertion restores group membership");
    c.redo();
    check(c.rackGroups(b)[0].insertIds.size() == 3, "redo insertion restores group membership");
    check(c.removeRackSelection(b, c.rackGroups(b)[0].insertIds), "whole group deletion is atomic");
    check(c.rackGroups(b).empty() && c.project().findTrack(b)->inserts.empty(), "empty group disappears");
    c.undo();
    check(c.rackGroups(b).size() == 1 && c.project().findTrack(b)->inserts.size() == 3,
          "undo deletion restores group and devices");
    Controller::ChannelSnapshot missing;
    auto unavailable = copy.inserts[0];
    unavailable.model.id = "unavailable-source";
    unavailable.model.uid = "unavailable.rack.fixture";
    unavailable.model.path = unavailable.model.uid;
    unavailable.state.clear();
    unavailable.rightState.clear();
    unavailable.preserveUnavailable = true;
    missing.inserts = {unavailable};
    check(c.pasteRackSelection(b, missing, 0, {}, &landed).isOk(),
          "unavailable device can retain its placeholder");
    check(!landed.empty() && !c.hasInsert(b, landed[0]), "placeholder never creates a substitute processor");
    check(c.transferRackSelection(b, {landed[0]}, a, 0, false).isOk(),
          "unavailable device can move without losing saved fields");
    const auto directory = fs::temp_directory_path() / "vlt-rack-test";
    fs::create_directories(directory);
    const auto preset = (directory / "group.vlts").string();
    auto presetCopy = copy;
    presetCopy.hasSettings = true;
    const auto savedPreset = daw::ChannelStripPreset::save(presetCopy, preset);
    if (!savedPreset)
        std::fprintf(stderr, "preset: %s\n", savedPreset.message().c_str());
    check(savedPreset.isOk(), "strip preset saves group");
    Controller::ChannelSnapshot loaded;
    check(daw::ChannelStripPreset::load(loaded, preset).isOk() && loaded.rackGroups == copy.rackGroups &&
              loaded.inserts[0].model.rackParameterIds == copy.inserts[0].model.rackParameterIds,
          "strip preset round trip retains groups and rack cells");
    const auto package = (directory / "project.vlt").string();
    check(c.saveProject(package).isOk(), "local project saves");
    Controller reopened;
    reopened.initialize(48000, 256, false);
    check(reopened.openProject(package).isOk(), "local project reopens with unavailable device");
    check(reopened.rackGroups(a) == c.rackGroups(a) && reopened.rackGroups(b) == c.rackGroups(b),
          "version 18 persists per-channel groups");
    auto json = nlohmann::json::parse(document(c));
    check(json["version"] == 18 || json["formatVersion"] == 18, "local format version is 18");
    json["version"] = 17;
    json.erase("masterRackGroups");
    for (auto& track : json["tracks"]) {
        track.erase("rackGroups");
        for (auto& slot : track["inserts"])
            slot.erase("rackParameters");
    }
    daw::ProjectModel legacy;
    check(daw::ProjectSerializer::deserializeDocument(legacy, json.dump()).isOk(),
          "version 17 documents remain readable");
    check(legacy.findTrack(a)->rackGroups.empty(), "old project starts with no groups");
    const auto clone = c.duplicateTrack(a);
    check(!clone.empty() && !c.rackGroups(clone).empty() &&
              c.rackGroups(clone)[0].id != c.rackGroups(a)[0].id,
          "track duplication remaps group identities");
    check(c.pasteRackSelection(Controller::kMasterChannelId, copy, 0).isOk() &&
              c.rackGroups(Controller::kMasterChannelId).size() == 1,
          "master supports rack groups");
    const auto instrumentTrack = c.addTrack(daw::TrackKind::Instrument, "Sampler source");
    const auto instrumentTarget = c.addTrack(daw::TrackKind::Instrument, "Sampler destination");
    check(c.setTrackInstrumentPlugin(instrumentTrack,
                                     daw::plugins::sampler::SamplerInstance::staticDescriptor()),
          "rack instrument source");
    const auto instrumentId = c.project().findTrack(instrumentTrack)->instrument.id;
    const auto internalFx = c.addSamplerFxInsert(instrumentTrack, instrumentId, eq);
    c.setInsertParameter(instrumentTrack, internalFx, "output.gain", -9.);
    Controller::ChannelSnapshot instrumentCopy;
    check(c.captureRackSelection(instrumentTrack, {instrumentId}, instrumentCopy).isOk() &&
              instrumentCopy.instrument && instrumentCopy.instrumentFx.size() == 1,
          "instrument snapshot includes private FX without channel inserts");
    check(c.pasteRackSelection(instrumentTarget, instrumentCopy, 0, {}, &landed).isOk(),
          "instrument copies to another compatible channel");
    const auto* instrumentDestination = c.project().findTrack(instrumentTarget);
    check(instrumentDestination && instrumentDestination->instrument.id != instrumentId &&
              instrumentDestination->inserts.empty() &&
              instrumentDestination->samplerFx.inserts.size() == 1 &&
              near(c.insertParameter(instrumentTarget, instrumentDestination->samplerFx.inserts[0].id,
                                     "output.gain"),
                   -9),
          "instrument copy retains its private processing and fresh identities");
    check(!c.pasteRackSelection(a, instrumentCopy, 0), "instrument cannot be inserted on an audio channel");
    check(c.removeRackSelection(instrumentTarget, landed), "instrument deletion is undoable");
    c.undo();
    check(c.project().findTrack(instrumentTarget)->instrument.isLoaded() &&
              c.project().findTrack(instrumentTarget)->samplerFx.inserts.size() == 1,
          "undo restores instrument and private FX together");
    daw::AutomationTarget instrumentAutomation;
    instrumentAutomation.kind = daw::AutomationTargetKind::PluginParameter;
    instrumentAutomation.channelId = instrumentTrack;
    instrumentAutomation.parameterId = "vol";
    const auto instrumentLane = c.addAutomationLane(instrumentTrack, instrumentAutomation);
    const auto instrumentCurve = c.addAutomationClip(instrumentLane, instrumentAutomation, 0, 1);
    check(c.transferRackSelection(instrumentTrack, {instrumentId}, instrumentTarget, 0, false).isOk()
        && targetOf(c, instrumentLane, instrumentCurve).channelId == instrumentTarget,
        "instrument move preserves automation addressed by the empty instrument slot");
    c.undo();
    check(c.project().findTrack(instrumentTrack)->instrument.id == instrumentId
        && targetOf(c, instrumentLane, instrumentCurve).channelId == instrumentTrack,
        "instrument move undo restores both channels and its automation");
    Controller::ChannelSnapshot cut;
    c.captureRackSelection(b, {one}, cut);
    cut.rackCutSource = b;
    c.removeRackSelection(b, {one});
    check(c.pasteRackSelection(a, cut, 0, {}, &landed).isOk() && landed == std::vector<std::string>{one}
        && targetOf(c, lane, clip).channelId == a, "Cut/Paste keeps device identity and moves automation");
    c.undo(); c.undo();
    check(c.insertModel(b, one) && targetOf(c, lane, clip).channelId == b, "Undo Cut/Paste returns the original address");
    const auto send = c.addSend(a, b);
    check(!send.empty(), "rack send added");
    c.setSendPreFader(a, send, true);
    c.undo();
    check(!c.project().findTrack(a)->sends.back().preFader, "send tap has undo");
    c.setSendEnabled(a, send, false);
    c.undo();
    check(c.project().findTrack(a)->sends.back().enabled, "send enable has undo");
    c.removeSend(a, send);
    c.undo();
    check(c.project().findTrack(a)->sends.back().id == send, "send removal restores stable address");
    return failures ? 1 : 0;
}
