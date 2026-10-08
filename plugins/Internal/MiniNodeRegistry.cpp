#include "MiniNodeRegistry.hpp"
#include "ChannelColorInstance.hpp"
#include "CompressorInstance.hpp"
#include "DelayInstance.hpp"
#include "EqualizerInstance.hpp"
#include "ModulationInstance.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace daw::plugins::mini {
const char *portTypeName(PortType t) noexcept {
  constexpr const char *names[] = {"Audio", "Number", "Gate", "Function", "Integer", "Array", "List", "Buffer"};
  return names[unsigned(t)];
}
std::span<const NodeDescription> nodeRegistry() {
  static const auto registry = [] {
    std::vector<NodeDescription> r;
    const auto add = [&](const char *id, const char *name, const char *category,
                         Operation op,
                         PortType output =
                             PortType::Number) -> NodeDescription & {
      r.push_back({id, name, category, {}, 1, op});
      r.back().outputs.push_back({"out", "Output", output});
      return r.back();
    };
    const auto value =
        [](NodeDescription &n, const char *id, const char *name, double lo,
           double hi, double initial, const char *unit = "", bool log = false,
           bool mod = true, std::vector<std::string> choices = {}) {
          const int index = int(n.parameters.size());
          n.parameters.push_back(
              {id, name, unit, lo, hi, initial, log, mod, std::move(choices)});
          if (mod)
            n.inputs.push_back({id, name, PortType::Number, false, index});
        };
    const auto audio = [](NodeDescription &n, const char *id = "in",
                          const char *name = "Audio") {
      n.inputs.push_back({id, name, PortType::Audio, true});
    };
    add("input", "Input", "Routing", Operation::Input, PortType::Audio);
    auto &out =
        add("output", "Output", "Routing", Operation::Output, PortType::Audio);
    audio(out);
    out.outputs.clear();
    add("interface", "Interface", "Routing", Operation::Interface)
        .outputs.clear();
    add("cpp_function", "C++ Function", "Code", Operation::CppFunction,
        PortType::Function).description = "Write a portable C++ audio function";
    auto &constant = add("constant", "Constant", "Math", Operation::Constant);
    value(constant, "value", "Value", -1e6, 1e6, 0);
    auto &gain =
        add("gain", "Gain", "Routing", Operation::Gain, PortType::Audio);
    audio(gain);
    value(gain, "gain", "Gain", 0, 2, 1);
    auto &mix = add("mix", "Mix", "Routing", Operation::Mix, PortType::Audio);
    audio(mix, "a", "A");
    audio(mix, "b", "B");
    value(mix, "mix", "Mix", 0, 1, .5);
    struct Math {
      const char *id, *name;
      Operation op;
    };
    for (auto m : {Math{"add", "Add", Operation::Add},
                   {"subtract", "Subtract", Operation::Subtract},
                   {"multiply", "Multiply", Operation::Multiply},
                   {"divide", "Divide", Operation::Divide},
                   {"min", "Minimum", Operation::Minimum},
                   {"max", "Maximum", Operation::Maximum},
                   {"power", "Power", Operation::Power}}) {
      auto &n = add(m.id, m.name, "Math", m.op);
      value(n, "a", "A", -1e6, 1e6, 0);
      value(n, "b", "B", -1e6, 1e6, 1);
    }
    for (auto m : {Math{"abs", "Absolute", Operation::Abs},
                   {"negate", "Negate", Operation::Negate},
                   {"sqrt", "Square root", Operation::Sqrt},
                   {"sin", "Sine", Operation::Sin},
                   {"cos", "Cosine", Operation::Cos}})
      value(add(m.id, m.name, "Math", m.op), "value", "Value", -1e6, 1e6, 0);
    auto &clamp = add("clamp", "Clamp", "Math", Operation::Clamp);
    value(clamp, "value", "Value", -1e6, 1e6, 0);
    value(clamp, "min", "Minimum", -1e6, 1e6, 0);
    value(clamp, "max", "Maximum", -1e6, 1e6, 1);
    auto &map = add("map_range", "Map Range", "Math", Operation::MapRange);
    value(map, "value", "Value", -1e6, 1e6, 0);
    value(map, "in_min", "Input min", -1e6, 1e6, -1);
    value(map, "in_max", "Input max", -1e6, 1e6, 1);
    value(map, "out_min", "Output min", -1e6, 1e6, 0);
    value(map, "out_max", "Output max", -1e6, 1e6, 1);
    auto &compare =
        add("compare", "Compare", "Logic", Operation::Compare, PortType::Gate);
    value(compare, "a", "A", -1e6, 1e6, 0);
    value(compare, "b", "B", -1e6, 1e6, 0);
    value(compare, "operation", "Operation", 0, 4, 0, "", false, false,
          {">", "<", "=", ">=", "<="});
    auto &select = add("select", "Select", "Logic", Operation::Select);
    select.inputs.push_back({"gate", "Select B", PortType::Gate});
    value(select, "a", "A", -1e6, 1e6, 0);
    value(select, "b", "B", -1e6, 1e6, 1);
    auto &smooth = add("smooth", "Smooth", "Modulation", Operation::Smooth);
    value(smooth, "value", "Value", -1e6, 1e6, 0);
    value(smooth, "time", "Time", .1, 10000, 20, "ms", true);
    auto &hold = add("sample_hold", "Sample & Hold", "Modulation",
                     Operation::SampleHold);
    value(hold, "value", "Value", -1e6, 1e6, 0);
    hold.inputs.push_back({"gate", "Trigger", PortType::Gate});
    auto &lfo = add("lfo", "LFO", "Modulation", Operation::Lfo);
    value(lfo, "rate", "Rate", .01, 40, 1, "Hz", true);
    value(lfo, "amount", "Amount", 0, 1000, 1);
    value(lfo, "offset", "Offset", -1000, 1000, 0);
    value(lfo, "shape", "Shape", 0, 3, 0, "", false, false,
          {"Sine", "Triangle", "Saw", "Square"});
    value(lfo, "sync", "Clock", 0, 1, 0, "", false, false, {"Hz", "Tempo"});
    value(lfo, "beats", "Cycle length", .03125, 32, 1, "beats", true, false);
    lfo.inputs.push_back({"reset", "Reset", PortType::Gate});
    auto &random = add("random", "Random", "Modulation", Operation::Random);
    value(random, "rate", "Rate", .01, 100, 1, "Hz", true);
    value(random, "smooth", "Smooth", 0, 1, 0);
    value(random, "seed", "Seed", 0, 2147483647, 1, "", false, false);
    random.inputs.push_back({"reset", "Reset", PortType::Gate});
    auto &env =
        add("envelope", "Envelope Follower", "Analysis", Operation::Envelope);
    audio(env);
    value(env, "attack", "Attack", .1, 1000, 10, "ms", true);
    value(env, "release", "Release", 1, 5000, 100, "ms", true);
    value(env, "mode", "Detector", 0, 1, 0, "", false, false, {"Peak", "RMS"});
    auto &a2n = add("audio_to_number", "Audio to Number", "Analysis",
                    Operation::AudioToNumber);
    audio(a2n);
    value(a2n, "channel", "Channel", 0, 2, 0, "", false, false,
          {"Mono sum", "Left", "Right"});
    value(add("number_to_audio", "Number to Audio", "Routing",
              Operation::NumberToAudio, PortType::Audio),
          "value", "Value", -1e6, 1e6, 0);
    auto &osc = add("oscillator", "Oscillator", "Generators",
                    Operation::Oscillator, PortType::Audio);
    value(osc, "frequency", "Frequency", 1, 20000, 440, "Hz", true);
    value(osc, "level", "Level", 0, 1, .1);
    value(osc, "shape", "Shape", 0, 3, 0, "", false, false,
          {"Sine", "Triangle", "Saw", "Square"});
    osc.inputs.push_back({"reset", "Reset", PortType::Gate});
    auto &noise = add("noise", "White Noise", "Generators", Operation::Noise,
                      PortType::Audio);
    value(noise, "level", "Level", 0, 1, .1);
    value(noise, "seed", "Seed", 0, 2147483647, 1, "", false, false);
    const auto effect = [&](const char *id, const char *name,
                            std::span<const ParameterInfo> table) {
      auto &n = add(id, name, "Effects", Operation::Effect, PortType::Audio);
      audio(n);
      for (const auto &p : table) {
        value(n, p.id.c_str(), p.name.c_str(), p.minValue, p.maxValue,
              p.defaultValue, p.unit.c_str(),
              p.minValue > 0 && p.maxValue / p.minValue > 50,
              p.isAutomatable && !p.isStepped && !p.isBypass);
        n.parameters.back().nativeIndex = int(p.index);
      }
    };
    effect("color", "Color", channel_color::parameterTable());
    effect("doubler", "Doubler",
           modulation::parameterTable(modulation::Kind::Doubler));
    effect("chorus", "Chorus",
           modulation::parameterTable(modulation::Kind::Chorus));
    effect("flanger", "Flanger",
           modulation::parameterTable(modulation::Kind::Flanger));
    effect("phaser", "Phaser",
           modulation::parameterTable(modulation::Kind::Phaser));
    effect("delay", "Delay", delay::parameterTable());
    {
      delay::DelayInstance labels;
      for (auto &p : r.back().parameters)
        if (delay::parameterTable()[unsigned(p.nativeIndex)].isStepped)
          for (double v = p.minimum; v <= p.maximum; ++v)
            p.choices.push_back(
                labels.parameterText(unsigned(p.nativeIndex), v));
    }
    effect("compressor", "Compressor", compressor::parameterTable());
    {
      compressor::CompressorInstance labels;
      for (auto &p : r.back().parameters)
        if (compressor::parameterTable()[unsigned(p.nativeIndex)].isStepped)
          for (double v = p.minimum; v <= p.maximum; ++v)
            p.choices.push_back(
                labels.parameterText(unsigned(p.nativeIndex), v));
    }
    auto &eq = add("eq_band", "EQ Band", "Effects", Operation::Effect,
                   PortType::Audio);
    audio(eq);
    using equalizer::BandParam;
    for (auto bp : {BandParam::Type, BandParam::Frequency, BandParam::Gain,
                    BandParam::Q}) {
      const auto &p =
          equalizer::parameterTable()[equalizer::bandParameter(0, bp)];
      const char *key = bp == BandParam::Type        ? "type"
                        : bp == BandParam::Frequency ? "frequency"
                        : bp == BandParam::Gain      ? "gain"
                                                     : "q";
      const char *label = bp == BandParam::Type        ? "Filter type"
                          : bp == BandParam::Frequency ? "Frequency"
                          : bp == BandParam::Gain      ? "Gain"
                                                       : "Q";
      value(eq, key, label, p.minValue, p.maxValue, p.defaultValue,
            p.unit.c_str(), bp == BandParam::Frequency || bp == BandParam::Q,
            bp != BandParam::Type);
      eq.parameters.back().nativeIndex = int(p.index);
      if (bp == BandParam::Type)
        eq.parameters.back().choices = {"Bell",      "Low Shelf", "High Shelf",
                                        "Low Cut",   "High Cut",  "Notch",
                                        "Band Pass", "Tilt",      "All Pass"};
    }
    auto &reverb =
        add("reverb", "Reverb", "Effects", Operation::Reverb, PortType::Audio);
    audio(reverb);
    value(reverb, "mix", "Mix", 0, 1, .25);
    value(reverb, "decay", "Decay", .1, 12, 2, "s", true);
    value(reverb, "damping", "Damping", 0, 1, .5);
    value(reverb, "room", "Space", 0, 1, 0, "", false, false, {"Room", "Hall"});
    appendProgrammingNodes(r);
    return r;
  }();
  return registry;
}
const NodeDescription *nodeDescription(std::string_view id, unsigned version) {
  for (const auto &n : nodeRegistry())
    if (n.id == id && n.version == version)
      return &n;
  return nullptr;
}
NodeDescription describeNode(const NodeDefinition &n, const MiniModuleDefinition *graph) {
  const auto *registered = nodeDescription(n.type, n.version);
  if (!registered) return {};
  auto d = *registered;
  if (!n.label.empty()) d.name = n.label;
  const auto valueType = parsePortType(n.valueType).value_or(PortType::Number);
  const auto collectionType = valueType == PortType::List ? PortType::List : PortType::Array;
  if (n.type == "subgraph" && graph) {
    for (const auto &g : graph->subgraphs) if (g.id == n.subgraph) {
      d.name = n.label.empty() ? g.name : n.label;
      const auto port = [](const GraphPort &p) {
        return PortDescription{p.id, p.name, parsePortType(p.type).value_or(PortType::Number), false, -1, p.signature, p.capacity};
      };
      for (const auto &p : g.inputs) {
        auto v = port(p);
        if (v.type == PortType::Number || v.type == PortType::Integer || v.type == PortType::Gate) {
          v.parameter = int(d.parameters.size());
          d.parameters.push_back({p.id, p.name, p.unit, p.minimum, p.maximum, p.initial, p.logarithmic});
        }
        d.inputs.push_back(v);
      }
      for (const auto &p : g.outputs) d.outputs.push_back(port(p));
      return d;
    }
    return {};
  }
  if (n.type == "history") {
    d.outputs[0].type = valueType;
    d.outputs[0].capacity = n.capacity;
    for (auto &p : d.inputs) if (p.id == "next") p.type = valueType;
  }
  if (n.type == "wire" || n.type == "subgraph_input" || n.type == "subgraph_output") {
    d.outputs[0].type = valueType;
    d.outputs[0].signature = n.signature;
    d.outputs[0].capacity = n.capacity;
    d.parameters.push_back({"default", "Default", "", -1e6, 1e6, 0});
    if (!d.inputs.empty()) { d.inputs[0].type = valueType; d.inputs[0].parameter = 0; d.inputs[0].signature = n.signature; }
  }
  for (auto &p : d.outputs) if (p.type == PortType::Array || p.type == PortType::List) p.capacity = n.capacity;
  if (n.type == "biquad_coefficients") d.outputs[0].capacity = 5;
  if (n.type == "length" || n.type == "get" || n.type == "set" || n.type == "clear" ||
      n.type == "sum" || n.type == "collection_min" || n.type == "collection_max" || n.type == "map" || n.type == "reduce") {
    d.inputs[0].type = collectionType;
    if (n.type == "set" || n.type == "clear" || n.type == "map") d.outputs[0].type = collectionType;
  }
  if (n.type != "cpp_function" || !n.function) return d;
  const auto &f = *n.function;
  d.name = f.entry;
  d.inputs.clear();
  d.outputs.clear();
  const auto kind = [](const std::string &s) {
    return s == "audio" ? PortType::Audio : s == "gate" ? PortType::Gate
         : s == "function" ? PortType::Function : PortType::Number;
  };
  for (const auto &p : f.inputs) {
    int index = -1;
    if (p.type == "number" || p.type == "gate") {
      index = int(d.parameters.size());
      d.parameters.push_back({p.id, p.name, "", p.minimum, p.maximum, p.initial});
    }
    d.inputs.push_back({p.id, p.name, kind(p.type),
                        p.type == "audio" || p.type == "function", index, p.signature});
  }
  for (const auto &p : f.outputs)
    d.outputs.push_back({p.id, p.name, kind(p.type), false, -1});
  d.outputs.push_back({"function", f.entry + "()", PortType::Function,
                       false, -1, callableSignature(f)});
  return d;
}
std::vector<PortDescription> inputPorts(const NodeDefinition &n, const MiniModuleDefinition *d) {
  return describeNode(n, d).inputs;
}
bool compatiblePorts(const PortDescription &a, const PortDescription &b) {
  return a.type == b.type &&
         (a.type != PortType::Function || a.signature == b.signature);
}
std::vector<PortDescription> outputPorts(const NodeDefinition &n,
                                         const MiniModuleDefinition &d) {
  if (n.type == "interface") {
    std::vector<PortDescription> result;
    for (const auto &c : d.controls)
      result.push_back({c.id, c.name, PortType::Number});
    return result;
  }
  return describeNode(n, &d).outputs;
}
NodeDefinition makeNode(std::string_view type, std::string id) {
  NodeDefinition n{std::move(id), std::string(type)};
  if (const auto *d = nodeDescription(type))
    for (const auto &p : d->parameters)
      n.parameters.push_back({p.id, p.initial});
  if (type == "curve") n.values = {-1, -1, 0, 0, 1, 1};
  if (type == "array") n.values = {1};
  if (type == "list" || type == "append" || type == "remove") n.valueType = "list";
  return n;
}
std::vector<bool> reachableNodes(const MiniModuleDefinition &d) {
  std::vector<bool> reachable(d.nodes.size());
  for (unsigned i = 0; i < d.nodes.size(); ++i)
    reachable[i] = d.nodes[i].type == "output" || d.nodes[i].type == "subgraph_output";
  for (unsigned pass = 0; pass < d.nodes.size(); ++pass) {
    bool changed = false;
    for (const auto &e : d.connections) {
      if (e.fromPort == "function") continue;
      auto a = std::find_if(d.nodes.begin(), d.nodes.end(),
                            [&](const auto &n) { return n.id == e.from; });
      auto b = std::find_if(d.nodes.begin(), d.nodes.end(),
                            [&](const auto &n) { return n.id == e.to; });
      if (a != d.nodes.end() && b != d.nodes.end() &&
          reachable[b - d.nodes.begin()] && !reachable[a - d.nodes.begin()]) {
        reachable[a - d.nodes.begin()] = true;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
  return reachable;
}
std::string validateTypedGraph(const MiniModuleDefinition &d, bool nested) {
  if (d.version < 5) {
    if (d.nodes.size() > 64) return "Legacy Creator graphs support 64 nodes";
    for (const auto &n : d.nodes) if (const auto *description = nodeDescription(n.type, n.version))
      if (description->operation >= Operation::Wire) return "Programming nodes require mini-module format 5";
  }
  std::map<std::string, unsigned> ids;
  unsigned inputs = 0, outputs = 0, interfaces = 0;
  for (unsigned i = 0; i < d.nodes.size(); ++i) {
    const auto &n = d.nodes[i];
    const auto description = describeNode(n, &d);
    const auto *desc = description.id.empty() ? nullptr : &description;
    if (n.id.empty() || n.id.size() > 128 || !ids.emplace(n.id, i).second)
      return "Invalid or duplicate node ID: " + n.id;
    if (!desc)
      return "Unavailable node: " + n.type;
    if (n.type == "cpp_function") {
      if (d.version < 4 || !n.function ||
          n.function->sdkVersion != kCreatorSdkVersion)
        return "Unavailable C++ function SDK: " + n.id;
      if (!isCppIdentifier(n.function->entry) || n.function->source.empty() ||
          n.function->analyzedHash.empty() || n.function->outputs.empty())
        return "Update C++ function ports: " + n.id;
      for (const auto &p : n.function->inputs)
        if (p.type != "audio" && p.type != "number" && p.type != "gate" &&
            p.type != "function") return "Unsupported C++ port: " + p.name;
      for (const auto &p : n.function->outputs)
        if (p.type != "audio" && p.type != "number" && p.type != "gate")
          return "Unsupported C++ output: " + p.name;
    }
    inputs += n.type == "input";
    outputs += n.type == "output";
    interfaces += n.type == "interface";
    std::set<std::string> params;
    for (const auto &p : n.parameters) {
      auto it = std::find_if(desc->parameters.begin(), desc->parameters.end(),
                             [&](const auto &v) { return v.id == p.id; });
      if (!params.insert(p.id).second || it == desc->parameters.end() ||
          !std::isfinite(p.value) || p.value < it->minimum ||
          p.value > it->maximum)
        return "Invalid parameter: " + n.id + "." + p.id;
    }
  }
  if ((!nested && (inputs != 1 || outputs != 1 || interfaces > 1)) ||
      (nested && (inputs || outputs || interfaces)))
    return "A graph needs one Input, one Output and at most one Interface";
  std::set<std::pair<std::string, std::string>> destinations;
  std::vector<unsigned> degree(d.nodes.size());
  std::vector<std::vector<unsigned>> next(d.nodes.size());
  std::vector<unsigned> functionDegree(d.nodes.size());
  std::vector<std::vector<unsigned>> functionNext(d.nodes.size());
  for (const auto &e : d.connections) {
    if (!ids.contains(e.from) || !ids.contains(e.to))
      return "Connection refers to a missing node";
    const auto &a = d.nodes[ids[e.from]], &b = d.nodes[ids[e.to]];
    const auto ports = outputPorts(a, d);
    const auto description = describeNode(b, &d);
    const auto *desc = &description;
    auto src = std::find_if(ports.begin(), ports.end(),
                            [&](const auto &p) { return p.id == e.fromPort; });
    auto dst = std::find_if(desc->inputs.begin(), desc->inputs.end(),
                            [&](const auto &p) { return p.id == e.toPort; });
    if (src == ports.end() || dst == desc->inputs.end())
      return "Unknown port: " + e.from + "." + e.fromPort + " → " + e.to + "." +
             e.toPort;
    if (!compatiblePorts(*src, *dst))
      return "Incompatible port types: " +
             std::string(portTypeName(src->type)) + " → " +
             portTypeName(dst->type);
    if (!destinations.emplace(e.to, e.toPort).second)
      return "Input already connected: " + e.to + "." + e.toPort;
    if (src->type == PortType::Function) {
      functionNext[ids[e.from]].push_back(ids[e.to]);
      ++functionDegree[ids[e.to]];
    } else if (d.version < 5 || !isMemoryWrite(b, e.toPort)) {
      next[ids[e.from]].push_back(ids[e.to]);
      ++degree[ids[e.to]];
    }
  }
  std::vector<unsigned> ready;
  for (unsigned i = 0; i < degree.size(); ++i)
    if (!degree[i])
      ready.push_back(i);
  for (unsigned i = 0; i < ready.size(); ++i)
    for (auto n : next[ready[i]])
      if (!--degree[n])
        ready.push_back(n);
  if (ready.size() != d.nodes.size())
    return "Graph contains a feedback cycle without an explicit previous-sample memory";
  ready.clear();
  for (unsigned i = 0; i < functionDegree.size(); ++i)
    if (!functionDegree[i]) ready.push_back(i);
  for (unsigned i = 0; i < ready.size(); ++i)
    for (auto n : functionNext[ready[i]])
      if (!--functionDegree[n]) ready.push_back(n);
  if (ready.size() != d.nodes.size())
    return "Function dependencies contain a recursive cycle";
  const auto reachable = reachableNodes(d);
  for (unsigned i = 0; i < d.nodes.size(); ++i)
    if (reachable[i]) {
      const auto &n = d.nodes[i];
      for (const auto &p : inputPorts(n, &d))
        if (p.required && !destinations.contains({n.id, p.id}))
          return "Connect required input: " + n.id + "." + p.id;
    }
  std::set<std::string> controls;
  for (const auto &c : d.controls) {
    if (c.id.empty() || c.id.size() > 128 || !controls.insert(c.id).second ||
        c.name.empty() || c.name.size() > 128 || c.unit.size() > 32 ||
        !std::isfinite(c.minimum) || !std::isfinite(c.maximum) ||
        !std::isfinite(c.initial) || c.minimum >= c.maximum ||
        c.initial < c.minimum || c.initial > c.maximum ||
        (c.logarithmic && c.minimum <= 0) ||
        (!c.style.empty() && !validControlStyle(c.style)))
      return "Invalid external control: " + c.id;
    // In v3 Interface output wires express all modulation, including fan-out.
    if (!c.bindings.empty())
      return "Typed graphs use Interface ports instead of legacy bindings";
  }
  return {};
}
} // namespace daw::plugins::mini
