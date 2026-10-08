#include "Host/PluginNode.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "Internal/ModulationInstance.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <nlohmann/json.hpp>

static std::atomic<bool> countAllocations{false};
static std::atomic<unsigned> allocations{0};
void *operator new(std::size_t n) {
  if (countAllocations)
    ++allocations;
  if (void *p = std::malloc(std::max(n, std::size_t(1))))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
using namespace daw::plugins;
using namespace daw::plugins::mini;
static int failures = 0;
static void check(bool ok, const char *name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}
static MiniModuleDefinition initial() {
  MiniModuleDefinition d;
  d.version = 3;
  d.id = "test.creator";
  d.name = "Creator test";
  d.nodes = {makeNode("input", "in"), makeNode("output", "out"),
             makeNode("interface", "ui")};
  d.connections = {{"in", "out", "out", "in"}};
  return d;
}
static void set(NodeDefinition &n, std::string_view id, double value) {
  for (auto &p : n.parameters)
    if (p.id == id)
      p.value = value;
}
struct Audio {
  std::vector<float> left, right, outL, outR;
  const float *in[2];
  float *out[2];
  explicit Audio(unsigned size)
      : left(size), right(size), outL(size), outR(size),
        in{left.data(), right.data()}, out{outL.data(), outR.data()} {}
  bool run(MiniModuleInstance &fx, unsigned channels = 2,
           std::span<const PluginEvent> events = {}) {
    PluginProcessContext c;
    c.inputs = in;
    c.outputs = out;
    c.inputChannels = c.outputChannels = std::uint16_t(channels);
    c.frames = unsigned(left.size());
    c.inputEvents = events;
    c.transport.tempo = 120;
    c.playing = true;
    return fx.process(c) == PluginProcessDisposition::Continue;
  }
};
static void benchmark(bool repeatedColor) {
  std::vector<std::unique_ptr<PluginInstance>> direct, typed;
  for (unsigned channel = 0; channel < 64; ++channel)
    for (unsigned slot = 0; slot < 3; ++slot) {
      const std::string type = repeatedColor || slot == 0 ? "color"
                               : slot == 1                ? "doubler"
                                                          : "chorus";
      auto graph = initial();
      graph.nodes.push_back(makeNode(type, "fx"));
      graph.connections = {{"in", "fx", "out", "in"},
                           {"fx", "out", "out", "in"}};
      auto module = std::make_unique<MiniModuleInstance>();
      module->configure(graph);
      module->activate({48000, 256});
      typed.push_back(std::move(module));
      std::unique_ptr<PluginInstance> native;
      if (type == "color")
        native = std::make_unique<channel_color::ChannelColorInstance>();
      else if (type == "doubler")
        native = std::make_unique<modulation::DoublerInstance>();
      else
        native = std::make_unique<modulation::ChorusInstance>();
      for (const auto &p : graph.nodes.back().parameters)
        native->setParameterFromHost(
            unsigned(native->parameterIndexForId(p.id)), p.value);
      native->activate({48000, 256});
      direct.push_back(std::move(native));
    }
  Audio audio(256);
  PluginProcessContext context;
  context.inputs = audio.in;
  context.outputs = audio.out;
  context.inputChannels = context.outputChannels = 2;
  context.frames = 256;
  context.playing = true;
  context.transport.tempo = 120;
  const auto run = [&](auto &modules) {
    double total = 0;
    for (unsigned block = 0; block < 24; ++block) {
      const auto start = std::chrono::steady_clock::now();
      for (unsigned channel = 0; channel < 64; ++channel) {
        for (unsigned i = 0; i < 256; ++i)
          audio.left[i] = audio.right[i] =
              float(.15 * std::sin((block * 256 + i) * .13));
        for (unsigned slot = 0; slot < 3; ++slot) {
          modules[channel * 3 + slot]->process(context);
          std::copy(audio.outL.begin(), audio.outL.end(), audio.left.begin());
          std::copy(audio.outR.begin(), audio.outR.end(), audio.right.begin());
        }
      }
      if (block >= 4)
        total += std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count();
    }
    return total / 20;
  };
  const double native = run(direct), graph = run(typed);
  std::printf("BENCH Creator 64 stereo channels, 48k/256, serial %s: direct "
              "%.3f ms/block, typed graph %.3f ms/block, ratio %.3f\n",
              repeatedColor ? "3x Color" : "Color+Doubler+Chorus", native,
              graph, graph / native);
}
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 3 && std::string_view(argv[1]) == "--validate") {
    try {
      std::ifstream file(argv[2]);
      nlohmann::json document;
      file >> document;
      const auto graph = fromJson(document.at("definition"));
      const auto error = validate(graph);
      if (!error.empty())
        std::printf("%s\n", error.c_str());
      MiniModuleInstance module;
      check(error.empty() && module.configure(graph) &&
                module.activate({48000, 256}),
            "example graph validates and prepares");
      if (!failures) {
        Audio audio(1024);
        std::fill(audio.left.begin(), audio.left.end(), .1f);
        check(audio.run(module) &&
                  std::all_of(audio.outL.begin(), audio.outL.end(),
                              [](float x) { return std::isfinite(x); }),
              "example executes with finite output");
      }
    } catch (const std::exception &error) {
      std::printf("FAIL %s\n", error.what());
      return 1;
    }
    return failures ? 1 : 0;
  }
  if (argc > 1) {
    benchmark(false);
    benchmark(true);
    return 0;
  }
  auto d = initial();
  check(validate(d).empty() && fromJson(toJson(d)) == d,
        "typed graph file round trip");
  bool catalogue = true, rt = true, finite = true, deterministic = true;
  for (const auto &type : nodeRegistry()) {
    if (type.operation >= Operation::Wire) continue; // v5 operations have dedicated typed/state tests.
    if (type.id == "input" || type.id == "output" || type.id == "interface" || type.id == "cpp_function")
      continue;
    auto graph = initial();
    graph.nodes.push_back(makeNode(type.id, "test"));
    graph.connections.clear();
    for (const auto &port : type.inputs)
      if (port.type == PortType::Audio)
        graph.connections.push_back({"in", "test", "out", port.id});
    if (type.outputs.front().type == PortType::Audio)
      graph.connections.push_back({"test", "out", "out", "in"});
    else {
      graph.nodes.push_back(makeNode("number_to_audio", "convert"));
      if (type.outputs.front().type == PortType::Gate) {
        graph.nodes.push_back(makeNode("select", "select"));
        graph.connections.push_back({"test", "select", "out", "gate"});
        graph.connections.push_back({"select", "convert", "out", "value"});
      } else
        graph.connections.push_back({"test", "convert", "out", "value"});
      graph.connections.push_back({"convert", "out", "out", "in"});
    }
    catalogue &= fromJson(toJson(graph)) == graph;
    for (double rate : {44100., 48000., 96000., 192000.})
      for (unsigned channels : {1u, 2u}) {
        MiniModuleInstance fx;
        PluginBusLayout accepted;
        bool ok = fx.configure(graph, 123) &&
                  fx.setBusLayout(
                      {{std::uint16_t(channels)}, {std::uint16_t(channels)}},
                      accepted) &&
                  fx.activate({rate, 257});
        if (!ok)
          std::printf("node %s: %s\n", type.id.c_str(), fx.error().c_str());
        catalogue &= ok;
        if (!ok)
          continue;
        Audio a(257);
        for (unsigned i = 0; i < 257; ++i) {
          a.left[i] = float(.1 * std::sin(i * .05));
          a.right[i] = -.7f * a.left[i];
        }
        countAllocations = true;
        allocations = 0;
        ok = a.run(fx, channels);
        fx.reset();
        ok &= a.run(fx, channels);
        countAllocations = false;
        rt &= allocations == 0;
        if (allocations)
          std::printf("allocations: %s %u\n", type.id.c_str(),
                      allocations.load());
        finite &= ok && std::all_of(a.outL.begin(), a.outL.end(),
                                    [](float x) { return std::isfinite(x); });
        const auto first = a.outL;
        fx.reset();
        a.run(fx, channels);
        deterministic &= first == a.outL;
      }
  }
  check(catalogue,
        "every registry node prepares at 44.1/48/96/192k, mono/stereo");
  check(rt, "all nodes process and reset without allocation");
  check(finite, "every node produces finite audio");
  check(deterministic, "node reset is deterministic");

  d = initial();
  d.nodes.push_back(makeNode("number_to_audio", "convert"));
  d.controls = {{"macro", "Macro", "", -1, 1, .25}};
  d.connections = {{"ui", "convert", "macro", "value"},
                   {"convert", "out", "out", "in"}};
  MiniModuleInstance fx;
  check(fx.configure(d) && fx.activate({48000, 32}),
        "unused Input is pruned for a generator");
  Audio a(97);
  PluginEvent event;
  event.frameOffset = 41;
  event.paramIndex = 0;
  event.value = -.75;
  check(a.run(fx, 2, std::span(&event, 1)) && a.outL[40] == .25f &&
            a.outL[41] == -.75f,
        "macro event keeps exact sample offset across chunk boundaries");
  d.nodes.push_back(makeNode("add", "sum"));
  d.connections = {{"ui", "sum", "macro", "a"},
                   {"ui", "sum", "macro", "b"},
                   {"sum", "convert", "out", "value"},
                   {"convert", "out", "out", "in"}};
  check(fx.configure(d) && a.run(fx) && a.outL[0] == .5f,
        "one macro feeds multiple ports on the same node");
  auto invalid = d;
  invalid.connections[0].toPort = "missing";
  check(!validate(invalid).empty(), "unknown port rejected");
  invalid = d;
  invalid.connections.push_back({"in", "sum", "out", "a"});
  check(!validate(invalid).empty(), "audio cannot connect to numeric input");
  invalid = d;
  invalid.connections[0] = {"sum", "sum", "out", "a"};
  check(!validate(invalid).empty(), "cycles rejected");
  invalid = d;
  invalid.controls.push_back({"second", "Second", "", 0, 1, 0});
  invalid.controls.push_back({"third", "Third", "", 0, 1, 0});
  check(!validate(invalid).empty(), "third external control rejected");

  d = initial();
  d.nodes.push_back(makeNode("color", "color"));
  set(d.nodes.back(), "drive", 0);
  d.nodes.push_back(makeNode("mix", "mix"));
  d.connections = {{"in", "color", "out", "in"},
                   {"in", "mix", "out", "a"},
                   {"color", "mix", "out", "b"},
                   {"mix", "out", "out", "in"}};
  check(fx.configure(d) && fx.latencySamples() == 48,
        "parallel typed graph reports compensated latency");
  a.left.assign(97, 0);
  a.left[0] = 1;
  a.run(fx);
  check(std::abs(a.outL[48] - 1) < 1e-6 && a.outL[0] == 0,
        "dry and Color branches arrive together");
  d.modes = {{"first", "First", d.nodes, d.connections, d.controls},
             {"second", "Second", d.nodes, d.connections, d.controls}};
  d.defaultMode = "first";
  check(validate(d).empty() && fromJson(toJson(d)) == d,
        "typed modes serialize without recursive definitions");

  bool modulated = true, partitioned = true;
  for (const auto &type : nodeRegistry()) {
    if (type.operation != Operation::Effect &&
        type.operation != Operation::Reverb)
      continue;
    auto graph = initial();
    graph.nodes.push_back(makeNode(type.id, "fx"));
    graph.nodes.push_back(makeNode("lfo", "lfo"));
    graph.nodes.push_back(makeNode("map_range", "map"));
    const auto parameter =
        std::find_if(type.parameters.begin(), type.parameters.end(),
                     [](const auto &p) { return p.modulatable; });
    if (parameter == type.parameters.end())
      continue;
    set(graph.nodes.back(), "out_min", parameter->minimum);
    set(graph.nodes.back(), "out_max", parameter->maximum);
    graph.connections = {{"in", "fx", "out", "in"},
                         {"lfo", "map", "out", "value"},
                         {"map", "fx", "out", parameter->id},
                         {"fx", "out", "out", "in"}};
    MiniModuleInstance whole, small;
    modulated &= whole.configure(graph) && small.configure(graph) &&
                 whole.activate({48000, 257}) && small.activate({48000, 31});
    Audio first(2053), second(2053);
    for (unsigned i = 0; i < first.left.size(); ++i) {
      first.left[i] = second.left[i] = float(.1 * std::sin(i * .14));
      first.right[i] = second.right[i] = float(.2 * std::sin(i * .11));
    }
    allocations = 0;
    countAllocations = true;
    const bool ok = first.run(whole) && second.run(small);
    countAllocations = false;
    modulated &= ok && allocations == 0;
    double error = 0;
    for (unsigned i = 0; i < first.outL.size(); ++i) {
      modulated &= std::isfinite(first.outL[i]) && std::isfinite(first.outR[i]);
      error = std::max(error, std::abs(double(first.outL[i] - second.outL[i])));
    }
    if (error > 1e-5)
      std::printf("partition difference %s: %.9f\n", type.id.c_str(), error);
    partitioned &= error < 1e-5;
  }
  check(modulated, "sample-stream modulation of every effect has no "
                   "allocations or nonfinite output");
  check(partitioned,
        "modulated effect graph is independent of block partition");

  auto dc = initial();
  dc.nodes.push_back(makeNode("number_to_audio", "constant"));
  set(dc.nodes.back(), "value", -.5);
  dc.connections = {{"constant", "out", "out", "in"}};
  auto previous = std::make_unique<MiniModuleInstance>();
  previous->configure(dc);
  auto previousNode =
      std::make_shared<PluginNode>("previous", std::move(previous));
  previousNode->prepare({48000, 64, 2});
  set(dc.nodes.back(), "value", .5);
  MiniModuleInstance replacement;
  replacement.configure(dc);
  replacement.activate({48000, 64});
  replacement.transitionFrom(previousNode);
  Audio transition(1500);
  allocations = 0;
  countAllocations = true;
  const bool transitionOk = transition.run(replacement);
  countAllocations = false;
  double maximumStep = 0;
  for (unsigned i = 1; i < transition.outL.size(); ++i)
    maximumStep =
        std::max(maximumStep,
                 std::abs(double(transition.outL[i] - transition.outL[i - 1])));
  check(transitionOk && allocations == 0 && transition.outL.front() == -.5f &&
            transition.outL.back() == .5f && maximumStep < .002,
        "recompiled instance crossfades in 20ms without allocation or a "
        "discontinuity");
  replacement.pumpMainThread();
  replacement.requestUpdateFadeOut();
  transition.run(replacement);
  check(replacement.updateFadeOutFinished() && transition.outL.back() == 0,
        "latency-changing update fades out before graph publication");
  replacement.cancelUpdateFadeOut();
  transition.run(replacement);
  maximumStep = 0;
  for (unsigned i = 1; i < transition.outL.size(); ++i)
    maximumStep =
        std::max(maximumStep,
                 std::abs(double(transition.outL[i] - transition.outL[i - 1])));
  check(transition.outL.front() == 0 && transition.outL.back() == .5f &&
            maximumStep < .002,
        "failed installation restores the previous sound with a fade");
  return failures ? 1 : 0;
}
