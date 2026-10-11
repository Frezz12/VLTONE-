// Exercise the AI's Sequence and mixing workflow against the real controller.
#include "EngineController.hpp"
#include "ai/AiSession.hpp"
#include "ai/AiTools.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace ai = daw::ai;
using json = nlohmann::json;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}

struct TestSample {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("ai-studio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    TestSample() {
        audio::AudioBuffer tone(2, audio::BufferSize(4800));
        for (int i = 0; i < 4800; ++i)
            tone.getChannel(0)[i] = tone.getChannel(1)[i] = .1f * std::sin(float(i) * .1f);
        audio::AudioRecorder recorder;
        recorder.initialize(audio::SampleRate(48000), 2);
        recorder.writeWAVFile(path.string(), tone, audio::SampleRate(48000));
    }
    ~TestSample() { std::error_code ec; std::filesystem::remove(path, ec); }
};

int main() try {
    TestSample sample;
    daw::EngineController c;
    c.initialize(48000, 512, false);
    c.setTempo(120);
    ai::ToolContext context;
    const auto call = [&](const char* name, json args = json::object()) {
        return ai::callTool(c, name, args, context);
    };
    const auto execute = [&](const char* name, json args = json::object()) {
        const auto result = call(name, std::move(args));
        if (!result.ok) throw std::runtime_error(std::string(name) + ": " + result.error);
        return result.value;
    };

    const auto beforePattern = c.undoDepth();
    const auto pattern = execute("create_pattern", {{"name", "Drums"}, {"startBar", 5}, {"lengthBars", 2}});
    const auto parent = pattern.at("patternId").get<std::string>();
    const auto owner = pattern.at("patternClipId").get<std::string>();
    check(c.undoDepth() == beforePattern + 1, "Pattern creation is one undo entry");
    const auto lane = execute("add_pattern_lane", {{"patternId", parent}, {"patternClipId", owner},
        {"name", "Kick"}, {"format", "internal"}, {"uid", "daw.sampler"}});
    const auto track = lane.at("trackId").get<std::string>();
    const auto clip = lane.at("clipId").get<std::string>();
    const auto instrument = lane.at("insertId").get<std::string>();
    const auto* created = c.project().findTrack(track);
    check(created && created->parentId == parent && created->clips.front().patternClipId == owner &&
          created->clips.front().midiView.sequence && created->clips.front().durationSeconds == 4,
          "Pattern lane belongs to its arrangement clip and opens in Sequence view");
    c.undo();
    check(!c.project().findTrack(track) && c.project().findTrack(parent), "Undo removes the whole lane only");
    c.redo();
    check(c.project().findTrack(track) && c.project().findTrack(track)->clips.front().midiView.sequence,
          "Redo restores the Pattern lane in Sequence view");

    auto sequence = execute("set_sequence", {{"trackId", track}, {"clipId", clip}, {"pitch", 60}, {"stepBeats", .25},
        {"steps", json::array({{{"step", 1}, {"velocity", 110}}, {{"step", 9}, {"velocity", 90}}})}});
    check(sequence.at("sequence") == true && sequence.at("steps").size() == 2 && sequence.at("lastStep") == 32,
          "Two bars receive source-anchored steps directly");
    execute("set_sequence", {{"trackId", track}, {"clipId", clip}, {"pitch", 42},
        {"steps", json::array({{{"step", 3}, {"velocity", 70}}})}});
    sequence = execute("set_sequence", {{"trackId", track}, {"clipId", clip}, {"pitch", 60}, {"mode", "merge"},
        {"steps", json::array({{{"step", 1}, {"velocity", 0}}, {{"step", 5}, {"velocity", 100}, {"offsetBeats", .02}}})}});
    check(sequence.at("totalNoteCount") == 3 && sequence.at("steps").size() == 2,
          "Merging a rest and a shifted hit preserves other steps and other pitches");
    const auto notesBeforeInvalid = execute("get_clip_notes", {{"trackId", track}, {"clipId", clip}});
    check(!call("set_sequence", {{"trackId", track}, {"clipId", clip},
        {"steps", json::array({{{"step", 2}}, {{"step", 33}}})}}).ok &&
          notesBeforeInvalid == execute("get_clip_notes", {{"trackId", track}, {"clipId", clip}}),
          "An invalid step rejects the complete batch before changing notes");
    check(!call("set_sequence", {{"trackId", track}, {"clipId", clip},
        {"steps", json::array({{{"step", 1}}, {{"step", 1}}})}}).ok,
          "Duplicate cells in one lane are rejected");
    c.setClipTrim(track, clip, 8.5, .5, 3.5);
    sequence = execute("get_sequence", {{"trackId", track}, {"clipId", clip}});
    check(sequence.at("sourceOffsetBeats") == 1 && sequence.at("firstStep") == 5,
          "A trimmed Sequence reports its source grid instead of silently moving the rhythm");
    check(!call("set_sequence", {{"trackId", track}, {"clipId", clip}, {"steps", json::array({{{"step", 1}}})}}).ok,
          "Writing before a trimmed clip's visible range is rejected");
    execute("set_sequence", {{"trackId", track}, {"clipId", clip}, {"pitch", 60}, {"steps", json::array()}});
    check(c.project().findTrack(track)->clips.front().notes.size() == 1,
          "Clearing the visible lane keeps another pitch outside the trimmed window");

    int focusCalls = 0;
    context.focusInApp = [&](const json& target) { ++focusCalls; return target; };
    execute("focus_in_app", {{"action", "pattern"}, {"trackId", parent}, {"clipId", owner}});
    execute("focus_in_app", {{"action", "plugin"}, {"trackId", track}, {"insertId", instrument}});
    check(!call("focus_in_app", {{"action", "sample"}, {"trackId", track}, {"clipId", clip}}).ok && focusCalls == 2,
          "Native editor dispatch validates the target and clip kind");
    context.requestedMode = ai::InteractionMode::Help;
    ai::AiSession session(c);
    session.setContext(context);
    const auto count = c.project().tracks.size();
    session.begin("create a drum track");
    session.applyReply({"", {{"edit", "create_pattern", {{"name", "Must not appear"}}}}, ""});
    check(c.project().tracks.size() == count && !session.messages().back().outcomes.front().ok,
          "Explicit Ask mode prevents edits even for an imperative request");
    session.cancel();
    context.requestedMode.reset();

    ai::Attachment attachment;
    attachment.name = "Drum sample"; attachment.path = sample.path.string(); attachment.contentId = "attached-drum";
    context.attachments.push_back(attachment);
    const auto sampled = execute("add_pattern_lane", {{"patternId", parent}, {"patternClipId", owner},
        {"name", "Snare"}, {"contentId", "attached-drum"}});
    check(c.project().findTrack(sampled.at("trackId"))->instrument.isLoaded(),
          "An authorized audio attachment becomes a playable sampler lane");

    const auto audioTrack = c.importAudioToNewTrack(sample.path.string(), 0, "Clip FX source");
    check(!audioTrack.empty(), "Audio material is available for clip-scoped processing");
    const auto audioClip = c.project().findTrack(audioTrack)->clips.front().id;
    for (const auto& chain : {std::string("sampler_fx"), std::string("clip")}) {
        const auto channel = chain == "clip" ? audioTrack : track;
        const json target{{"channelId", channel}, {"format", "internal"}, {"uid", "daw.equalizer"},
                          {"chain", chain}, {"clipId", audioClip}};
        const auto first = execute("add_insert", target).at("insertId").get<std::string>();
        const auto second = execute("add_insert", target).at("insertId").get<std::string>();
        check(execute("get_plugin_state", {{"channelId", channel}, {"insertId", first}}).at("chain") == chain,
              "Local effect slots resolve to their actual owning chain");
        check(ai::projectSnapshot(c).dump().find(first) != std::string::npos,
              "Project context exposes clip and sampler effect IDs");
        const auto p = execute("list_plugin_parameters", {{"channelId", channel}, {"insertId", first}}).at("parameters").front();
        execute("set_insert_parameter", {{"channelId", channel}, {"insertId", first},
            {"parameterId", p.at("id")}, {"value", p.at("min")}});
        check(c.insertParameter(channel, first, p.at("id")) == p.at("min").get<double>(),
              "Local effect parameters use the same host API");
        execute("configure_plugin", {{"channelId", channel}, {"insertId", second}, {"action", "move"}, {"toIndex", 0}});
        const auto* slots = chain == "clip" ? c.clipFx(channel, audioClip) : &c.samplerFx(channel, instrument)->inserts;
        check(slots->front().id == second, "AI reorders effects inside their local chain");
        execute("remove_insert", {{"channelId", channel}, {"insertId", first}});
        check(!c.insertModel(channel, first), "AI removes the intended local effect");
    }

    const auto bus = c.addTrack(daw::TrackKind::Bus, "Vocal FX");
    const auto vocal = c.addTrack(daw::TrackKind::Audio, "Vocal");
    execute("mix", {{"action", "set_output"}, {"channelId", vocal}, {"toTrackId", bus}});
    check(c.project().findTrack(vocal)->outputBusId == bus, "AI can route a channel to a mix bus");
    check(!call("mix", {{"action", "set_output"}, {"channelId", bus}, {"toTrackId", vocal}}).ok,
          "Output routing refuses a non-bus destination");
    execute("mix", {{"action", "set_output"}, {"channelId", vocal}, {"toTrackId", "master"}});
    const auto send = execute("mix", {{"action", "set_send"}, {"channelId", vocal}, {"toTrackId", bus},
        {"levelDb", -12}, {"preFader", true}, {"enabled", false}}).at("sendId").get<std::string>();
    const auto& routed = c.project().findTrack(vocal)->sends.front();
    check(routed.preFader && !routed.enabled, "AI controls pre/post-fader and send enable state");
    const auto beforeLevel = routed.level;
    execute("mix", {{"action", "set_send"}, {"channelId", vocal}, {"toTrackId", bus}, {"sendId", send}, {"levelDb", -6}});
    c.undo();
    check(std::abs(c.project().findTrack(vocal)->sends.front().level - beforeLevel) < 1e-6,
          "Send level edits participate in Undo");
    execute("mix", {{"action", "set_send"}, {"channelId", vocal}, {"toTrackId", bus}, {"sendId", send}, {"enabled", true}});
    check(c.project().findTrack(vocal)->sends.front().level == beforeLevel,
          "Toggling a send preserves its existing level");
    const auto secondBus = c.addTrack(daw::TrackKind::Bus, "Parallel vocal");
    const auto beforeSend = c.undoDepth();
    execute("add_send", {{"trackId", vocal}, {"destinationTrackId", secondBus}, {"level", .2}, {"preFader", true}});
    check(c.undoDepth() == beforeSend + 1, "Creating a configured send is one undo entry");
    c.undo(); c.redo();
    check(std::abs(c.project().findTrack(vocal)->sends.back().level - .2) < 1e-6 &&
          c.project().findTrack(vocal)->sends.back().preFader,
          "Redo restores a new send's level and pre-fader setting");

#ifdef DAW_TEST_VST3_PATH
    const auto factory = daw::plugins::factoryFor(daw::plugins::Format::Vst3);
    check(bool(factory), "VST3 factory is available");
    const auto plugins = factory->inspect(DAW_TEST_VST3_PATH);
    const auto effect = std::find_if(plugins.begin(), plugins.end(), [](const auto& p) { return !p.isInstrument; });
    check(effect != plugins.end(), "A real VST3 fixture is discovered");
    const auto slot = c.addInsert(vocal, *effect);
    check(!slot.empty(), "The test VST3 loads through the actual host");
    const auto parameters = execute("list_plugin_parameters", {{"channelId", vocal}, {"insertId", slot}}).at("parameters");
    check(parameters.size() >= 2, "AI sees the hosted VST3 parameter catalog");
    json updates = json::array();
    for (const auto& p : parameters) {
        const double low = p.at("min"), high = p.at("max");
        const double value = low + (high - low) * .375;
        updates.push_back({{"parameterId", p.at("id")}, {"value", value}});
        check(p.contains("display") && p.contains("automatable"), "Parameter display and capabilities are exposed");
    }
    execute("set_insert_parameters", {{"channelId", vocal}, {"insertId", slot}, {"parameters", updates}});
    for (const auto& p : updates)
        check(std::abs(c.insertParameter(vocal, slot, p.at("parameterId")) - p.at("value").get<double>()) < 1e-5,
              "AI writes each VST3 parameter in its actual plain units");
    const auto previous = c.insertParameter(vocal, slot, updates.front().at("parameterId"));
    updates.push_back({{"parameterId", "nonexistent"}, {"value", .5}});
    check(!call("set_insert_parameters", {{"channelId", vocal}, {"insertId", slot}, {"parameters", updates}}).ok &&
          c.insertParameter(vocal, slot, updates.front().at("parameterId")) == previous,
          "A bad plugin parameter rejects the whole batch");
    execute("configure_plugin", {{"channelId", vocal}, {"insertId", slot}, {"action", "mix"}, {"mix", .35}});
    check(std::abs(c.insertModel(vocal, slot)->mix - .35) < 1e-6, "AI controls host wet/dry independently of plugin knobs");
    c.undo();
    check(c.insertModel(vocal, slot)->mix == 1, "Plugin wet/dry is undoable");
    const auto second = c.addInsert(vocal, *effect);
    execute("configure_plugin", {{"channelId", vocal}, {"insertId", second}, {"action", "move"}, {"toIndex", 0}});
    check(c.channelInserts(vocal)->front().id == second, "AI can change processing order");
    const auto state = execute("get_plugin_state", {{"channelId", vocal}, {"insertId", slot}});
    execute("configure_plugin", {{"channelId", vocal}, {"insertId", slot}, {"action", "channel_mode"}, {"channelMode", "auto"}});
    check(!state.at("supportedChannelModes").empty() && state.contains("sidechainSources"),
          "Host channel and sidechain capabilities are inspectable");
    check(state.at("supportsSidechain") == true, "The VST3's auxiliary input is available to AI");
    execute("configure_plugin", {{"channelId", vocal}, {"insertId", slot}, {"action", "sidechain"},
        {"sourceTrackIds", json::array({parent})}});
    check(c.insertModel(vocal, slot)->sidechainTrackIds == std::vector<std::string>{parent},
          "AI connects a real sidechain source to the plugin");
    check(!call("configure_plugin", {{"channelId", vocal}, {"insertId", slot}, {"action", "sidechain"},
        {"sourceTrackIds", json::array({vocal})}}).ok,
          "A feedback sidechain is refused");
    execute("configure_plugin", {{"channelId", vocal}, {"insertId", slot}, {"action", "sidechain"},
        {"sourceTrackIds", json::array()}});
    check(c.insertModel(vocal, slot)->sidechainTrackIds.empty(), "AI can remove a sidechain route");
#endif

    ai::PromptPack older = ai::builtinPrompts();
    for (auto& book : older.playbooks) if (book.id == "drums") book.body = "Old server instructions";
    context.prompts = &older;
    const auto playbook = execute("get_playbook", {{"id", "drums"}});
    check(playbook.contains("hostAddendum") && playbook.at("hostAddendum").get<std::string>().find("set_sequence") != std::string::npos,
          "Downloaded prompt packs retain current host workflow instructions");
    std::puts("ALL PASSED");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
}
