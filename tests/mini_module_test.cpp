#include "EngineController.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/ModulationInstance.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "collaboration/CommandJson.hpp"
#include "collaboration/ProjectReducer.hpp"
#include "model/ChannelColor.hpp"
#include "model/MiniModules.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <nlohmann/json.hpp>

namespace {
std::atomic<bool> counting{false};
std::atomic<unsigned> allocations{0};
} // namespace
void *operator new(std::size_t n) {
  if (counting)
    ++allocations;
  if (auto *p = std::malloc(std::max(n, std::size_t(1))))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
namespace {
using namespace daw;
using namespace daw::plugins;
using namespace daw::plugins::mini;
int failures = 0;
void check(bool ok, const char *label) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
  failures += !ok;
}
struct Audio {
  std::vector<float> left, right, outL, outR;
  const float *in[2];
  float *out[2];
  Audio(unsigned n)
      : left(n), right(n), outL(n), outR(n), in{left.data(), right.data()},
        out{outL.data(), outR.data()} {}
  bool process(PluginInstance &module, unsigned channels = 2,
               std::span<const PluginEvent> events = {}) {
    PluginProcessContext c;
    c.inputs = in;
    c.outputs = out;
    c.frames = unsigned(left.size());
    c.inputChannels = c.outputChannels = std::uint16_t(channels);
    c.inputEvents = events;
    return module.process(c) != PluginProcessDisposition::Error;
  }
};
MiniModuleDefinition parallel() {
  MiniModuleDefinition d;
  d.id = "parallel";
  d.name = "Parallel tape";
  d.nodes = {{"in", "input"},
             {"tape", "color", 1, {{"drive", 0}}},
             {"gain", "gain", 1, {{"gain", 1}}},
             {"mix", "mix", 1, {{"mix", .5}}},
             {"out", "output"}};
  d.connections = {{"in", "tape"},
                   {"in", "gain"},
                   {"tape", "mix"},
                   {"gain", "mix"},
                   {"mix", "out"}};
  d.controls = {{"level",
                 "Level",
                 "",
                 0,
                 1,
                 .5,
                 false,
                 {{"gain", "gain", 0, 2, false}, {"mix", "mix", 0, 1, false}}}};
  return d;
}
void benchmark(bool repeatedColor) {
  using Clock = std::chrono::steady_clock;
  std::vector<std::unique_ptr<PluginInstance>> wrapped, direct;
  for (unsigned channel = 0; channel < 64; ++channel)
    for (unsigned slot = 0; slot < 3; ++slot) {
      const auto type = repeatedColor || slot == 0 ? "color"
                        : slot == 1                ? "doubler"
                                                   : "chorus";
      auto graph = std::make_unique<MiniModuleInstance>();
      graph->configure(builtin(type));
      graph->activate({48000, 256});
      wrapped.push_back(std::move(graph));
      std::unique_ptr<PluginInstance> core;
      if (std::string_view(type) == "color") {
        core = std::make_unique<channel_color::ChannelColorInstance>();
        core->setParameterFromHost(0, -20);
      } else if (std::string_view(type) == "doubler")
        core = std::make_unique<modulation::DoublerInstance>();
      else {
        core = std::make_unique<modulation::ChorusInstance>();
        core->setParameterFromHost(0, .25);
        core->setParameterFromHost(1, .22);
        core->setParameterFromHost(2, .30);
        core->setParameterFromHost(3, .65);
      }
      core->activate({48000, 256});
      direct.push_back(std::move(core));
    }
  Audio audio(256);
  const auto run = [&](auto &modules) {
    const auto start = Clock::now();
    for (unsigned block = 0; block < 8; ++block)
      for (unsigned channel = 0; channel < 64; ++channel) {
        for (unsigned i = 0; i < 256; ++i)
          audio.left[i] = audio.right[i] =
              float(.15 * std::sin((block * 256 + i) * .13));
        for (unsigned slot = 0; slot < 3; ++slot) {
          audio.process(*modules[channel * 3 + slot]);
          std::copy(audio.outL.begin(), audio.outL.end(), audio.left.begin());
          std::copy(audio.outR.begin(), audio.outR.end(), audio.right.begin());
        }
      }
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
               .count() /
           8;
  };
  const double native = run(direct), graph = run(wrapped);
  std::printf("BENCH 64 stereo channels, 48k/256, %s: direct %.3f ms/block, "
              "graph %.3f ms/block, ratio %.3f\n",
              repeatedColor ? "3x Color" : "Color+Doubler+Chorus", native,
              graph, graph / native);
}
} // namespace
int main(int argc, char **) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1) {
    benchmark(false);
    benchmark(true);
    return 0;
  }
  for (const auto *type : {"color", "doubler", "chorus"}) {
    const auto d = builtin(type);
    check(validate(d).empty(), "built-in graph valid");
    check(fromJson(toJson(d)) == d, "definition JSON round trip");
    check(d.modes.size() == 3 && !d.defaultMode.empty(),
          "built-ins include named optional modes");
    std::vector<float> previous;
    for (const auto &mode : d.modes) {
      MiniModuleInstance variant;
      Audio signal(32768);
      for (unsigned i = 0; i < signal.left.size(); ++i)
        signal.left[i] = signal.right[i] =
            float(.27 * std::sin(i * .08) + .1 * std::sin(i * .021));
      bool ok =
          variant.configure(d, 17, mode.id) && variant.activate({48000, 256});
      allocations = 0;
      counting = true;
      ok &= signal.process(variant);
      counting = false;
      check(ok && allocations == 0,
            "mode graph runs without audio-thread allocation");
      if (!previous.empty()) {
        double difference = 0;
        for (unsigned i = 0; i < signal.outL.size(); ++i)
          difference = std::max(difference,
                                std::abs(double(signal.outL[i]) - previous[i]));
        check(difference > 1e-5,
              "mode selection changes the sound with the same controls");
      }
      previous = signal.outL;
      std::vector<std::uint8_t> saved;
      variant.saveState(saved);
      MiniModuleInstance restored;
      check(restored.loadState(saved) && restored.mode() == mode.id,
            "selected mode survives state round trip");
    }
    auto invalidMode = d;
    invalidMode.modes.back().controls[0].id = "different";
    check(!validate(invalidMode).empty() && !validate(d, "unknown").empty(),
          "mode control mismatch and unknown selection rejected");
    auto legacy = resolved(d);
    legacy.version = 1;
    check(fromJson(toJson(legacy)) == d,
          "exact shipped v1 graphs gain modes without changing their default "
          "sound");
    for (double rate : {44100., 48000., 96000., 192000.})
      for (unsigned channels : {1u, 2u})
        for (unsigned size : {1u, 37u, 256u, 1024u}) {
          MiniModuleInstance instance;
          PluginBusLayout accepted;
          bool ok = instance.configure(d) &&
                    instance.setBusLayout(
                        {{std::uint16_t(channels)}, {std::uint16_t(channels)}},
                        accepted) &&
                    instance.activate({rate, size});
          if (!ok) {
            check(false, "prepare sample rate/block/channel matrix");
            continue;
          }
          Audio audio(size);
          for (unsigned i = 0; i < size; ++i)
            audio.left[i] = audio.right[i] = float(.25 * std::sin(i * .13));
          allocations = 0;
          counting = true;
          for (int n = 0; n < 4; ++n)
            ok &= audio.process(instance, channels);
          instance.reset();
          counting = false;
          ok &= allocations == 0;
          for (float v : audio.outL)
            ok &= std::isfinite(v);
          if (!ok)
            check(false, "finite allocation-free graph process/reset");
        }
  }
  auto definition = parallel();
  {
    auto custom = definition;
    custom.version = 2;
    custom.appearance.theme = "copper";
    custom.appearance.controlStyle = "fader";
    custom.modes = {{"blend", "My blend", custom.nodes, custom.connections,
                     custom.controls}};
    custom.defaultMode = "blend";
    check(validate(custom).empty() && fromJson(toJson(custom)) == custom,
          "user-defined mode graph and appearance are portable");
    custom.appearance.backgroundImage = "file:///local-only.png";
    check(!validate(custom).empty(),
          "background images must be embedded in the module file");
  }
  check(validate(definition).empty(),
        "parallel graph and macro fanout accepted");
  MiniModuleInstance graph;
  check(graph.configure(definition) && graph.activate({48000, 256}),
        "parallel graph prepared");
  check(graph.latencySamples() == 48,
        "parallel branch compensation reports 48 samples");
  Audio audio(256);
  audio.left[0] = audio.right[0] = 1;
  check(audio.process(graph), "parallel graph renders");
  bool exact = true;
  for (unsigned i = 0; i < 256; ++i)
    exact &= audio.outL[i] == (i == 48 ? 1.f : 0.f);
  check(exact, "dry branch delayed to exactly match neutral Color");
  std::vector<std::uint8_t> state;
  graph.setParameterFromHost(0, .7);
  graph.saveState(state);
  MiniModuleInstance restored;
  check(restored.loadState(state) && restored.definition() == definition &&
            restored.parameterValue(0) == .7,
        "graph state and external parameter survive restore");
  restored.activate({48000, 256});
  audio.process(restored);
  check(std::abs(audio.outL[48] - 1.28f) < 1e-6,
        "one macro drives gain and parallel mix simultaneously");
  auto bad = definition;
  bad.controls.push_back(bad.controls[0]);
  bad.controls.push_back(bad.controls[0]);
  check(!validate(bad).empty(), "third external control rejected");
  bad = definition;
  bad.nodes[1].type = "future-node";
  check(!validate(bad).empty(), "unknown node unavailable");
  bad = definition;
  bad.connections[0] = {"gain", "tape"};
  bad.connections[1] = {"tape", "gain"};
  check(!validate(bad).empty(), "cycle rejected");
  // Event offsets must be identical to splitting the same block at that event.
  MiniModuleInstance automated, split;
  automated.configure(builtin("color"));
  split.configure(builtin("color"));
  automated.activate({48000, 256});
  split.activate({48000, 256});
  Audio whole(256), first(73), second(183);
  for (unsigned i = 0; i < 256; ++i) {
    whole.left[i] = whole.right[i] = float(.3 * std::sin(.2 * i));
    if (i < 73)
      first.left[i] = first.right[i] = whole.left[i];
    else
      second.left[i - 73] = second.right[i - 73] = whole.left[i];
  }
  PluginEvent event;
  event.kind = PluginEvent::Kind::ParamValue;
  event.paramIndex = 0;
  event.value = 80;
  event.frameOffset = 73;
  whole.process(automated, 2, std::span(&event, 1));
  first.process(split);
  split.setParameterFromHost(0, 80);
  second.process(split);
  exact = true;
  for (unsigned i = 0; i < 256; ++i)
    exact &= whole.outL[i] == (i < 73 ? first.outL[i] : second.outL[i - 73]);
  check(exact, "sample-offset automation matches split processing exactly");
  EngineController controller;
  check(bool(controller.initialize(48000, 256, false)),
        "controller initialized");
  const auto track = controller.addTrack(TrackKind::Audio, "Mini modules");
  check(controller.miniModules(track).empty(),
        "new audio channel has an empty rack");
  const auto color = controller.addMiniModule(track, builtin("color"));
  const auto doubler = controller.addMiniModule(track, builtin("doubler"));
  const auto duplicate = controller.addMiniModule(track, builtin("color"));
  check(!color.empty() && !doubler.empty() && !duplicate.empty() &&
            controller.addMiniModule(track, builtin("chorus")).empty(),
        "three card limit and repeated effects");
  auto *processor = controller.insertInstance(track, color);
  check(processor != nullptr, "mini module resolves through PluginInstance");
  controller.moveMiniModule(track, color, 2);
  check(controller.miniModules(track)[2].id == color &&
            controller.insertInstance(track, color) == processor,
        "reorder preserves ID and processor history");
  controller.undo();
  check(controller.miniModules(track)[0].id == color, "reorder undo");
  controller.redo();
  check(controller.miniModules(track)[2].id == color, "reorder redo");
  controller.setInsertParameter(track, color, "drive", -37);
  controller.commitInsertParameterEdit(track, color, "drive", -20,
                                       "Change drive");
  controller.undo();
  check(controller.insertParameter(track, color, "drive") == -20,
        "parameter gesture undo");
  controller.redo();
  controller.setInsertBypassed(track, color, true);
  check(controller.miniModules(track)[2].bypassed, "independent bypass");
  check(controller.setMiniModulePostFx(track, color, true) &&
            controller.setMiniModuleMode(track, color, "tube"),
        "each card stores its own route and mode");
  auto look = controller.miniModules(track)[2].miniModule->appearance;
  look.theme = "ivory";
  look.controlStyle = "glass";
  const auto styleUndo = controller.undoDepth();
  check(controller.setMiniModuleAppearance(track, color, look) &&
            controller.undoDepth() == styleUndo + 1,
        "appearance is one Undo action");
  controller.undo();
  check(controller.miniModules(track)[2].miniModule->appearance.theme ==
            "studio",
        "appearance Undo restores prior look");
  controller.redo();
  check(controller.insertParameter(track, color, "drive") == -37 &&
            controller.insertInstance(track, color) == processor,
        "mode and route preserve controls and processor identity");
  check(!controller.addMiniModule("master", builtin("chorus")).empty(),
        "Master accepts mini modules");
  const auto bus = controller.addTrack(TrackKind::Bus, "Bus");
  check(!controller.addMiniModule(bus, builtin("doubler")).empty(),
        "bus accepts mini modules");
  const auto folder = controller.addTrack(TrackKind::Folder, "Folder");
  check(controller.addMiniModule(folder, builtin("color")).empty(),
        "plain folder rejects audio module");
  controller.setFolderSumming(folder, true);
  check(!controller.addMiniModule(folder, builtin("chorus")).empty(),
        "summing folder accepts a mini module");
  const auto snapshot = controller.copyChannelStrip(track, true);
  check(snapshot.miniModules.size() == 3 &&
            controller.pasteChannelStrip(bus, snapshot) &&
            controller.miniModules(bus).size() == 3,
        "channel strip copy includes entire rack");
  const auto directory =
      std::filesystem::temp_directory_path() / ("vlt-mini-test-" + newUuid());
  std::filesystem::create_directories(directory);
  check(bool(controller.saveProject(directory.string())),
        "project saves module graphs");
  EngineController reopened;
  reopened.initialize(48000, 256, false);
  check(bool(reopened.openProject(directory.string())) &&
            reopened.miniModules(track).size() == 3 &&
            reopened.miniModules("master").size() == 1,
        "project restores track and Master racks");
  const auto &reopenedColor = reopened.miniModules(track)[2];
  check(reopenedColor.miniModuleMode == "tube" &&
            reopenedColor.miniModulePostFx &&
            reopenedColor.miniModule->appearance == look,
        "project restores mode route and appearance");
  ProjectModel legacy;
  TrackModel old;
  old.id = newUuid();
  old.kind = TrackKind::Audio;
  old.channelColor = defaultChannelColor(old.id);
  old.channelColor->bypassed = false;
  const auto oldId = old.channelColor->id;
  const auto seed = old.channelColor->profileSeed;
  legacy.tracks.push_back(old);
  migrateMiniModules(legacy);
  check(!legacy.tracks[0].channelColor &&
            legacy.tracks[0].miniModules[0].id == oldId &&
            legacy.tracks[0].miniModules[0].profileSeed == seed &&
            !legacy.tracks[0].miniModules[0].bypassed,
        "legacy Color migration preserves identity seed bypass");
  {
    ProjectModel virtualColor;
    old.channelColor.reset();
    virtualColor.tracks.push_back(old);
    TrackModel lane;
    lane.id = newUuid();
    lane.kind = TrackKind::Automation;
    ClipModel clip;
    clip.id = newUuid();
    clip.kind = ClipKind::Automation;
    clip.automation.target.kind = AutomationTargetKind::PluginParameter;
    clip.automation.target.channelId = old.id;
    clip.automation.target.slotId = oldId;
    clip.automation.target.parameterId = "drive";
    lane.clips.push_back(clip);
    virtualColor.tracks.push_back(lane);
    std::string bytes;
    ProjectSerializer::serializeDocument(virtualColor, bytes);
    auto document = nlohmann::json::parse(bytes);
    document["version"] = 11;
    ProjectModel migrated;
    check(bool(ProjectSerializer::deserializeDocument(migrated,
                                                      document.dump())) &&
              migrated.tracks[0].miniModules.size() == 1 &&
              migrated.tracks[0].miniModules[0].id == oldId,
          "old virtual Color automation restores the missing card");
    migrated.tracks[0].miniModules.clear();
    ProjectSerializer::serializeDocument(migrated, bytes);
    check(bool(ProjectSerializer::deserializeDocument(migrated, bytes)) &&
              migrated.tracks[0].miniModules.empty(),
          "deleted migrated card is not resurrected in version 12");
    document["version"] = 12;
    document["tracks"][0]["miniModules"] = nlohmann::json::array();
    auto invalid = makeMiniModule(builtin("color"));
    invalid.miniModule->nodes[1].type = "future-node";
    virtualColor.tracks[0].miniModules = {invalid};
    ProjectSerializer::serializeDocument(virtualColor, bytes);
    check(bool(ProjectSerializer::deserializeDocument(migrated, bytes)) &&
              migrated.tracks[0].miniModules[0].miniModule ==
                  invalid.miniModule,
          "unknown node definition survives project round trip");
    document = nlohmann::json::parse(bytes);
    for (int i = 0; i < 3; ++i)
      document["tracks"][0]["miniModules"].push_back(
          document["tracks"][0]["miniModules"][0]);
    check(!ProjectSerializer::deserializeDocument(migrated, document.dump()),
          "project loader rejects a fourth card");
    auto unavailable = controller.project();
    unavailable.findTrack(track)->miniModules = {invalid};
    const auto unavailablePath = (directory / "unknown.vlt").string();
    check(bool(ProjectSerializer::save(unavailable, unavailablePath)) &&
              bool(controller.openProject(unavailablePath)) &&
              controller.miniModules(track).front().miniModule ==
                  invalid.miniModule,
          "unavailable module can remain in a local document");
    rendering::Spec spec;
    spec.outputDir = directory.string();
    spec.baseName = "unavailable";
    spec.range = rendering::Range::Custom;
    spec.customEndSeconds = .02;
    rendering::Report report;
    const auto result = controller.renderProject(spec, {}, report);
    check(!result && result.message().find("Mini module unavailable") !=
                         std::string::npos,
          "ordinary export rejects an unavailable module");
  }
  {
    MiniModuleInstance mono;
    PluginBusLayout layout;
    mono.configure(builtin("doubler"));
    mono.setBusLayout({{1}, {1}}, layout);
    mono.activate({48000, 256});
    Audio signal(256);
    for (unsigned i = 0; i < 256; ++i)
      signal.left[i] = float(.2 * std::sin(i * .27));
    signal.process(mono, 1);
    check(signal.outL == signal.left,
          "Doubler preserves a true mono path exactly");
    MiniModuleInstance stereo;
    stereo.configure(builtin("doubler"));
    stereo.activate({48000, 256});
    signal.right = signal.left;
    double width = 0;
    for (int n = 0; n < 32; ++n) {
      signal.process(stereo);
      for (unsigned i = 0; i < 256; ++i)
        width += std::abs(signal.outL[i] - signal.outR[i]);
    }
    check(width > 1, "Doubler widens a mono source on a stereo path");
    auto compound = parallel();
    compound.nodes.push_back(
        {"chorus",
         "chorus",
         1,
         {{"amount", .25}, {"depth", .3}, {"softness", .65}}});
    compound.connections.back() = {"mix", "chorus"};
    compound.connections.push_back({"chorus", "out"});
    MiniModuleInstance multiple;
    check(multiple.configure(compound) && multiple.activate({48000, 256}) &&
              signal.process(multiple),
          "compound graph executes multiple effects and parallel branches");
  }
  std::printf("Mini module failures: %d\n", failures);
  return failures ? 1 : 0;
}
