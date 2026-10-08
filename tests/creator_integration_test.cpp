#include "Creator/CreatorCompilerClient.hpp"
#include "EngineController.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "MiniModuleUpdate.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <numbers>

using namespace daw;
using namespace daw::plugins::mini;
namespace {
int failures = 0;
bool cppMode = false;
bool programmingMode = false;
bool check(bool ok, const char *message) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
  failures += !ok;
  return ok;
}
MiniModuleDefinition definition(bool revised = false) {
  MiniModuleDefinition d;
  d.version = 3;
  d.id = "creator.integration";
  d.name = "Creator gain";
  d.controls = {{"level", "Level", "", 0, 2, 1}};
  d.nodes = {makeNode("input", "in"), makeNode("output", "out"),
             makeNode("interface", "ui"), makeNode("gain", "gain")};
  d.connections = {{"in", "gain", "out", "in"},
                   {"gain", "out", "out", "in"},
                   {"ui", "gain", "level", "gain"}};
  if (revised) {
    d.nodes.push_back(makeNode("multiply", "half"));
    for (auto &p : d.nodes.back().parameters)
      if (p.id == "b")
        p.value = .5;
    d.connections.back() = {"ui", "half", "level", "a"};
    d.connections.push_back({"half", "gain", "out", "gain"});
  }
  if (programmingMode) {
    d.version = 5;
    d.nodes[3] = makeNode("audio_scale", "gain");
  }
  if (cppMode) {
    d.version = 4;
    FunctionDefinition function;
    function.source =
        "#include <vlt/creator.hpp>\nvlt::AudioFrame process(VLT_PORT(\"in\") "
        "vlt::AudioFrame input,VLT_PORT(\"gain\") float gain=1){return "
        "input*gain;}";
    const auto analysis = analyzeFunction(function);
    if (!analysis)
      throw std::runtime_error(analysis.error);
    d.nodes[3] = makeNode("cpp_function", "gain");
    d.nodes[3].function = analysis.function;
    std::string error;
    std::vector<CodeDiagnostic> diagnostics;
    if (!compileCppGraph(d, error, diagnostics))
      throw std::runtime_error(error);
  }
  return d;
}
double difference(const std::vector<float> &a, const std::vector<float> &b,
                  unsigned skipFrames = 0) {
  if (a.empty() || a.size() != b.size())
    return 1e9;
  double error = 0;
  unsigned worst = 0;
  for (unsigned i = skipFrames * 2; i < a.size(); ++i)
    if (std::abs(double(a[i]) - b[i]) > error) {
      error = std::abs(double(a[i]) - b[i]);
      worst = i;
    }
  if (error > 1e-6)
    std::printf("MEASURE difference %.9g at %u: %.9g / %.9g\n", error, worst,
                a[worst], b[worst]);
  return error;
}
std::vector<float> playback(EngineController &c, unsigned frames = 12000) {
  c.stop();
  c.seekSeconds(0);
  c.pumpPluginEvents();
  for (const auto &node : c.routingGraph()->nodes)
    node.node->reset();
  c.play();
  audio::AudioBuffer in(2, 256), out(2, 256);
  in.clear();
  std::vector<float> result(frames * 2);
  for (unsigned at = 0; at < frames; at += 256) {
    const unsigned count = std::min(256u, frames - at);
    c.processDeviceBlockForTest(in, out, count);
    for (unsigned i = 0; i < count; ++i)
      for (unsigned ch = 0; ch < 2; ++ch)
        result[(at + i) * 2 + ch] = out.getChannel(ch)[i];
  }
  c.stop();
  return result;
}
} // namespace
int main(int argc, char **argv) {
  cppMode = argc > 1 && std::string_view(argv[1]) == "--cpp";
  programmingMode = argc > 1 && std::string_view(argv[1]) == "--programming";
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  ProjectModel future;
  check(!ProjectSerializer::deserializeDocument(
            future, std::string("{\"format\":\"vlt-project\",\"version\":") + std::to_string(ProjectSerializer::kFormatVersion + 1) + "}"),
        "future project versions are rejected instead of silently dropping "
        "fields");
  const auto temporary = std::filesystem::temp_directory_path() /
                         ("vlt-creator-test-" + newUuid());
  std::filesystem::create_directories(temporary);
  audio::AudioBuffer source(2, 48000);
  std::vector<float> original(96000);
  for (unsigned i = 0; i < 48000; ++i)
    for (unsigned ch = 0; ch < 2; ++ch)
      original[i * 2 + ch] = source.getChannel(ch)[i] = float(
          .2 * std::sin(2 * std::numbers::pi * (ch ? 421 : 631) * i / 48000));
  const auto wav = (temporary / "source.wav").string();
  audio::AudioRecorder writer;
  writer.initialize(48000, 2);
  check(bool(writer.writeWAVFile(wav, source, 48000)), "fixture written");
  const auto before = definition(), after = definition(true);
  {
    EngineController c{EngineController::TestRuntime{}};
    c.initialize(48000, 256, false);
    const auto track = c.importAudioToNewTrack(wav, 0);
    const auto slot = c.addMiniModule(track, before);
    const auto repeated = c.addMiniModule(track, before);
    const auto master = c.addMiniModule({}, before);
    c.setInsertParameter(track, slot, "level", .8);
    c.setMiniModulePostFx(track, repeated, true);
    c.setInsertBypassed(track, repeated, true);
    const auto seed = c.miniModules(track).front().profileSeed;
    const auto dry = playback(c);
    auto update = c.planMiniModuleUpdate(after);
    auto preparing =
        std::async(std::launch::async, [update] { return update->prepare(); });
    audio::AudioBuffer in(2, 256), out(2, 256);
    in.clear();
    c.play();
    bool renders = true;
    do {
      renders &= c.processDeviceBlockForTest(in, out, 256);
    } while (preparing.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready);
    check(preparing.get() && renders && c.isPlaying(),
          "background compilation leaves device playback running");
    // A gesture committed while compilation ran must win over its snapshot.
    c.setInsertParameter(track, slot, "level", .65);
    const auto depth = c.undoDepth();
    std::string error;
    check(c.applyMiniModuleUpdate(update, error) && c.isPlaying() &&
              c.undoDepth() == depth + 1,
          "publication during playback is one Undo");
    for (unsigned i = 0; i < 8; ++i)
      renders &= c.processDeviceBlockForTest(in, out, 256);
    c.stop();
    c.pumpPluginEvents();
    const auto &rack = c.miniModules(track);
    check(renders && rack[0].id == slot && rack[1].id == repeated &&
              rack[0].miniModule == after && rack[1].miniModule == after &&
              c.miniModules({})[0].id == master &&
              c.miniModules({})[0].miniModule == after &&
              rack[0].profileSeed == seed && rack[1].bypassed &&
              rack[1].miniModulePostFx &&
              c.insertParameter(track, slot, "level") == .65,
          "all repeated and Master instances retain IDs, seed, latest controls "
          "and route");
    const auto revised = playback(c);
    check(difference(dry, revised) > .01,
          "compiled algorithm changes real device output");
    c.undo();
    check(c.miniModules(track)[0].miniModule == before &&
              c.miniModules({})[0].miniModule == before &&
              c.insertParameter(track, slot, "level") == .65,
          "Undo restores all algorithms and keeps the latest gesture");
    c.redo();
    // The device intentionally crossfades its previous output over a seek for
    // 5 ms. Compare the algorithm after that transport-only transition.
    check(difference(playback(c), revised, 256) < 1e-6,
          "Redo restores the same sound");
    auto invalid = after;
    invalid.connections.push_back({"gain", "gain", "out", "in"});
    const auto *live = c.insertInstance(track, slot);
    check(!c.updateMiniModuleDefinition(invalid, error) && !error.empty() &&
              live == c.insertInstance(track, slot) &&
              difference(playback(c), revised, 256) < 1e-6,
          "failed compile keeps live DSP and sound");
    auto stale = c.planMiniModuleUpdate(before);
    stale->prepare();
    c.moveMiniModule(track, slot, 1);
    check(!c.applyMiniModuleUpdate(stale, error) && !error.empty(),
          "stale compilation is rejected after the rack changes");
    // File operations restore the complete typed definition, without the
    // library.
    const auto file = (temporary / "creator.vlt").string();
    EngineController reopened{EngineController::TestRuntime{}};
    reopened.initialize(48000, 256, false);
    check(bool(c.saveProject(file)) && bool(reopened.openProject(file)) &&
              reopened.miniModules(track)[1].miniModule == after &&
              difference(playback(reopened), revised, 256) < 1e-6,
          "DAW project embeds and restores the typed graph and sound");
    const auto target = c.addTrack(TrackKind::Audio, "Preset");
    const auto preset = (temporary / "creator.vlts").string();
    check(bool(c.saveChannelStripPreset(track, preset)) &&
              bool(c.applyChannelStripPreset(target, preset)) &&
              c.miniModules(target)[1].miniModule == after &&
              c.miniModules(target)[1].id != slot,
          "strip preset transfers the complete Creator definition with fresh "
          "slot IDs");
  }
  {
    EngineController c{EngineController::TestRuntime{}};
    c.initialize(48000, 256, false);
    const auto track = c.importAudioToNewTrack(wav, 0);
    c.setRecordDirectory(temporary.string());
    const auto slot = c.addMiniModule(track, after);
    rendering::Spec spec;
    spec.outputDir = temporary.string();
    spec.file.container = audio::platform::Container::Wav;
    spec.file.encoding = audio::platform::Encoding::Float32;
    spec.range = rendering::Range::Custom;
    spec.customEndSeconds = 1;
    spec.blockSize = 256;
    spec.stemChannelIds = {track};
    const auto render = [&](const char *name) {
      spec.baseName = name;
      rendering::Report report;
      check(bool(c.renderProject(spec, {}, report)) && !report.files.empty(),
            "typed graph export completes");
      std::vector<std::vector<float>> result;
      for (const auto &path : report.files) {
        audio::platform::DecodedAudio decoded;
        check(bool(audio::platform::decodeAudioFile(path, decoded)),
              "render decodes");
        result.push_back(std::move(decoded.interleaved));
      }
      return result;
    };
    const auto live = playback(c, 48000);
    const auto wet = render("creator-wet");
    check(!wet.empty() && difference(live, wet[0]) < 1e-6 &&
              difference(original, wet[0]) > .09,
          "typed graph playback and export agree sample for sample");
    spec.bypassTrackInserts = true;
    const auto dry = render("creator-dry");
    spec.bypassTrackInserts = false;
    spec.stemsAtSource = true;
    const auto atSource = render("creator-source");
    spec.stemsAtSource = false;
    check(!dry.empty() && difference(original, dry[0]) < 1e-6 &&
              atSource.size() == 2 && difference(original, atSource[1]) < 1e-6,
          "dry export and source stems exclude Creator processing");
    rendering::Report report;
    check(bool(c.freezeTrack(track, {}, report)) && c.isTrackFrozen(track),
          "typed module freezes");
    const auto baked = render("creator-frozen");
    check(!baked.empty() && difference(wet[0], baked[0]) < 1e-6,
          "Freeze applies the typed algorithm exactly once");
    auto renamed = after;
    renamed.name = "Renamed";
    renamed.controls[0].name = "Volume";
    renamed.appearance.backgroundColor = "#334455";
    std::string error;
    check(c.updateMiniModuleDefinition(renamed, error) &&
              c.isTrackFrozen(track),
          "name and card appearance updates preserve Freeze");
    auto louder = before;
    louder.name = renamed.name;
    check(c.updateMiniModuleDefinition(louder, error) &&
              !c.isTrackFrozen(track),
          "algorithm recompilation invalidates affected Freeze");
    auto pending = c.planMiniModuleUpdate(after);
    pending->prepare();
    bool guarded = false, observed = false;
    spec.baseName = "creator-export-guard";
    const auto status = c.renderProject(
        spec,
        [&](const auto &) {
          if (c.offlineRenderInProgress()) {
            observed = true;
            guarded = !c.applyMiniModuleUpdate(pending, error);
          }
          return true;
        },
        report);
    check(
        bool(status) && observed && guarded &&
            c.miniModules(track)[0].miniModule == louder,
        "offline export cannot publish a pending compile into the live model");
    AutomationTarget target;
    target.kind = AutomationTargetKind::PluginParameter;
    target.channelId = track;
    target.slotId = slot;
    target.parameterId = "level";
    const auto lane = c.addAutomationLane(track, target);
    const auto clip = c.addAutomationClip(lane, target, 0, 2);
    c.setAutomationPoints(lane, clip,
                          {{0, .25}, {.67, .8}, {1.51, .4}, {2, .6}});
    const auto automated = playback(c);
    const auto another = c.addMiniModule(track, before);
    c.setInsertBypassed(track, another, true);
    c.moveMiniModule(track, slot, 1);
    check(difference(automated, playback(c), 256) < 1e-6,
          "automation addresses the external parameter after rack reorder");
    check(c.updateMiniModuleDefinition(after, error) &&
              difference(automated, playback(c), 256) > .01,
          "existing automation drives the recompiled algorithm by stable "
          "parameter ID");
    auto delayed = after;
    delayed.nodes.push_back(makeNode("color", "color"));
    for (auto &p : delayed.nodes.back().parameters)
      if (p.id == "drive")
        p.value = 0;
    delayed.connections[1] = {"gain", "color", "out", "in"};
    delayed.connections.push_back({"color", "out", "out", "in"});
    auto update = c.planMiniModuleUpdate(delayed);
    update->prepare();
    c.play();
    c.fadeMiniModuleUpdate(*update);
    audio::AudioBuffer in(2, 256), out(2, 256);
    in.clear();
    for (unsigned i = 0; i < 5; ++i)
      c.processDeviceBlockForTest(in, out, 256);
    check(c.miniModuleUpdateFaded(*update) &&
              c.applyMiniModuleUpdate(update, error) && c.isPlaying() &&
              c.routingGraph()->totalLatency == 96,
          "latency-changing compilation fades then publishes updated PDC "
          "during playback");
    c.stop();
    if (cppMode) {
      auto fault = before;
      auto &function = *fault.nodes[3].function;
      const auto body = function.source.find("return input*gain;");
      function.source.replace(
          body, std::string("return input*gain;").size(),
          "volatile unsigned n=0;for(;;){n=n+1;}return input;");
      const auto analyzed = analyzeFunction(function);
      check(bool(analyzed), "runtime-fault fixture passes C++ type checking");
      function = analyzed.function;
      std::vector<CodeDiagnostic> diagnostics;
      check(compileCppGraph(fault, error, diagnostics) &&
                c.updateMiniModuleDefinition(fault, error),
            "runtime-fault fixture installs without running audio in "
            "preparation");
      spec.baseName = "creator-cpp-fault";
      rendering::Report failedReport;
      check(!c.renderProject(spec, {}, failedReport) &&
                failedReport.files.empty(),
            "C++ runtime failure aborts export without publishing substitute "
            "audio");
    }
  }
  // This unique directory is owned by this harness.
  std::filesystem::remove_all(temporary);
  return failures ? 1 : 0;
}
