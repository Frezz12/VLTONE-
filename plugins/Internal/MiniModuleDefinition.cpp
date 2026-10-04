#include "MiniModuleDefinition.hpp"
#include "ChannelColorInstance.hpp"
#include "MiniNodeRegistry.hpp"
#include "ModulationInstance.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace daw::plugins::mini {
using json = nlohmann::json;
const std::vector<NodeType> &nodeTypes() {
  static const std::vector<NodeType> types{
      {"input", 1}, {"output", 1},  {"gain", 1},  {"mix", 1},
      {"color", 1}, {"doubler", 1}, {"chorus", 1}};
  return types;
}
namespace {
bool identifier(const std::string &s) { return !s.empty() && s.size() <= 128; }
bool parameter(const NodeDefinition &n, std::string_view id, double lo,
               double hi) {
  if (n.type == "gain")
    return id == "gain" && lo >= 0 && hi <= 2;
  if (n.type == "mix")
    return id == "mix" && lo >= 0 && hi <= 1;
  std::span<const ParameterInfo> table;
  if (n.type == "color")
    table = channel_color::parameterTable();
  if (n.type == "doubler")
    table = modulation::parameterTable(modulation::Kind::Doubler);
  if (n.type == "chorus")
    table = modulation::parameterTable(modulation::Kind::Chorus);
  for (const auto &p : table)
    if (p.id == id)
      return lo >= p.minValue && hi <= p.maxValue;
  return false;
}
} // namespace
bool validControlStyle(std::string_view s) {
  return s == "machined" || s == "rubber" || s == "glass" || s == "fader";
}
MiniModuleDefinition resolved(const MiniModuleDefinition &d,
                              std::string_view mode) {
  auto result = d;
  const auto selected = mode.empty() ? std::string_view(d.defaultMode) : mode;
  for (const auto &m : d.modes)
    if (m.id == selected) {
      result.nodes = m.nodes;
      result.connections = m.connections;
      result.controls = m.controls;
      result.code = m.code;
      // Appearance belongs to the controls, independently of the selected
      // sound.
      for (std::size_t i = 0;
           i < result.controls.size() && i < d.controls.size(); ++i)
        result.controls[i].style = d.controls[i].style;
      break;
    }
  result.modes.clear();
  result.defaultMode.clear();
  result.appearance = {};
  return result;
}
std::string validate(const MiniModuleDefinition &d,
                     std::string_view selectedMode) {
  if (!d.unavailableSource.empty())
    return "Invalid module definition";
  if (d.version < 1 || d.version > 4)
    return "Unsupported module version";
  const auto &look = d.appearance;
  if (look.theme != "studio" && look.theme != "graphite" &&
      look.theme != "ivory" && look.theme != "copper")
    return "Unsupported module theme";
  if (!validControlStyle(look.controlStyle))
    return "Unsupported control style";
  if (!look.backgroundColor.empty() &&
      (look.backgroundColor.size() != 7 || look.backgroundColor[0] != '#' ||
       look.backgroundColor.find_first_not_of("0123456789abcdefABCDEF", 1) !=
           std::string::npos))
    return "Invalid background color";
  if (!look.backgroundImage.empty()) {
    constexpr std::string_view prefix = "data:image/png;base64,";
    if (!look.backgroundImage.starts_with(prefix) ||
        look.backgroundImage.size() > 1024 * 1024 ||
        look.backgroundImage.size() == prefix.size() ||
        look.backgroundImage.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=",
            prefix.size()) != std::string::npos)
      return "Invalid embedded background image";
  }
  if (d.modes.size() > 8)
    return "A module supports at most eight modes";
  std::set<std::string> modeIds;
  for (const auto &m : d.modes) {
    if (!identifier(m.id) || m.name.empty() || m.name.size() > 128 ||
        !modeIds.insert(m.id).second)
      return "Invalid or duplicate mode";
    if (m.controls.size() != d.controls.size())
      return "Modes must keep the same external controls";
    for (std::size_t i = 0; i < m.controls.size(); ++i) {
      const auto &a = m.controls[i], &b = d.controls[i];
      if (a.id != b.id || a.name != b.name || a.unit != b.unit ||
          a.minimum != b.minimum || a.maximum != b.maximum ||
          a.logarithmic != b.logarithmic || a.initial != b.initial)
        return "Modes must keep the same external controls";
    }
    auto graph = resolved(d, m.id);
    if (const auto error = validate(graph); !error.empty())
      return "Mode " + m.name + ": " + error;
  }
  if ((!d.modes.empty() && !modeIds.contains(d.defaultMode)) ||
      (d.modes.empty() && !d.defaultMode.empty()) ||
      (!selectedMode.empty() && !modeIds.contains(std::string(selectedMode))))
    return "Unavailable module mode";
  if (!identifier(d.id) || d.name.empty() || d.name.size() > 256)
    return "Invalid module identity";
  if (d.nodes.size() < 2 || d.nodes.size() > kMaxNodes ||
      d.connections.size() > kMaxEdges || d.controls.size() > 2)
    return "Module exceeds its node, connection or control limit";
  if (d.version >= 3)
    return validateTypedGraph(d);
  std::map<std::string, unsigned> indices;
  unsigned inputs = 0, outputs = 0, sink = 0;
  for (unsigned i = 0; i < d.nodes.size(); ++i) {
    const auto &n = d.nodes[i];
    if (!identifier(n.id) || !indices.emplace(n.id, i).second)
      return "Duplicate or invalid node ID";
    if (std::none_of(nodeTypes().begin(), nodeTypes().end(),
                     [&](const auto &t) {
                       return t.id == n.type && t.version == n.version;
                     }))
      return "Unavailable node: " + n.type;
    inputs += n.type == "input";
    if (n.type == "output") {
      ++outputs;
      sink = i;
    }
    std::set<std::string> ids;
    for (const auto &p : n.parameters)
      if (!std::isfinite(p.value) || !ids.insert(p.id).second ||
          !parameter(n, p.id, p.value, p.value))
        return "Invalid node parameter";
  }
  if (inputs != 1 || outputs != 1)
    return "A module needs one Input and one Output";
  std::vector<unsigned> degree(d.nodes.size()), visited(d.nodes.size());
  std::vector<std::vector<unsigned>> next(d.nodes.size()),
      previous(d.nodes.size());
  std::set<std::pair<std::string, std::string>> edges;
  for (const auto &e : d.connections) {
    if (!indices.contains(e.from) || !indices.contains(e.to) ||
        !edges.emplace(e.from, e.to).second)
      return "Invalid connection";
    auto a = indices[e.from], b = indices[e.to];
    if (d.nodes[a].type == "output" || d.nodes[b].type == "input")
      return "Invalid input/output connection";
    next[a].push_back(b);
    previous[b].push_back(a);
    ++degree[b];
  }
  std::vector<unsigned> ready;
  for (unsigned i = 0; i < degree.size(); ++i) {
    const auto &type = d.nodes[i].type;
    if (degree[i] != (type == "input" ? 0u : type == "mix" ? 2u : 1u))
      return "Invalid number of node inputs";
    if (!degree[i])
      ready.push_back(i);
  }
  for (std::size_t i = 0; i < ready.size(); ++i)
    for (auto n : next[ready[i]])
      if (!--degree[n])
        ready.push_back(n);
  if (ready.size() != d.nodes.size())
    return "Module contains a feedback cycle";
  ready = {sink};
  visited[sink] = 1;
  for (std::size_t i = 0; i < ready.size(); ++i)
    for (auto n : previous[ready[i]])
      if (!visited[n]) {
        visited[n] = 1;
        ready.push_back(n);
      }
  if (ready.size() != d.nodes.size())
    return "Disconnected node";
  std::set<std::string> controls;
  std::set<std::pair<std::string, std::string>> destinations;
  for (const auto &c : d.controls) {
    if (!identifier(c.id) || !controls.insert(c.id).second || c.name.empty() ||
        c.name.size() > 128 || c.unit.size() > 32 ||
        !std::isfinite(c.minimum) || !std::isfinite(c.maximum) ||
        !std::isfinite(c.initial) || c.minimum >= c.maximum ||
        c.initial < c.minimum || c.initial > c.maximum ||
        (c.logarithmic && c.minimum <= 0) || c.bindings.empty() ||
        c.bindings.size() > 64 ||
        (!c.style.empty() && !validControlStyle(c.style)))
      return "Invalid external control";
    for (const auto &b : c.bindings) {
      if (!indices.contains(b.node) || !std::isfinite(b.minimum) ||
          !std::isfinite(b.maximum) ||
          (b.logarithmic && (b.minimum <= 0 || b.maximum <= 0)) ||
          !parameter(d.nodes[indices[b.node]], b.parameter,
                     std::min(b.minimum, b.maximum),
                     std::max(b.minimum, b.maximum)) ||
          !destinations.emplace(b.node, b.parameter).second)
        return "Invalid or multiply bound parameter";
    }
  }
  return {};
}
bool sameAudioGraph(const MiniModuleDefinition &a, std::string_view am,
                    const MiniModuleDefinition &b, std::string_view bm) {
  const auto normalized = [](const auto &d, std::string_view mode) {
    auto graph = resolved(d, mode);
    if (graph.version < 3) graph.version = 1;
    graph.id.clear(); graph.name.clear();
    for (auto &c : graph.controls) { c.name.clear(); c.unit.clear(); c.style.clear(); }
    return graph;
  };
  return normalized(a, am) == normalized(b, bm);
}
MiniModuleDefinition builtin(std::string_view id) {
  MiniModuleDefinition d;
  d.version = 2;
  d.id = std::string(id);
  if (id != "color" && id != "doubler" && id != "chorus")
    return d;
  d.name = id == "color" ? "Color" : id == "doubler" ? "Doubler" : "Chorus";
  d.nodes = {{"in", "input"}, {"fx", std::string(id)}, {"out", "output"}};
  d.connections = {{"in", "fx"}, {"fx", "out"}};
  const auto add = [&](std::string key, std::string name, std::string unit,
                       double lo, double hi, double initial, bool log = false) {
    d.controls.push_back(
        {key, name, unit, lo, hi, initial, log, {{"fx", key, lo, hi, log}}});
  };
  if (id == "color") {
    add("drive", "Drive", "", -100, 100, -20);
    add("tone", "Tone", "", -100, 100, 0);
  }
  if (id == "doubler") {
    add("width", "Width", "%", 0, 1, .45);
    add("humanize", "Humanize", "%", 0, 1, .35);
    d.nodes[1].parameters = {{"softness", .65}};
  }
  if (id == "chorus") {
    add("amount", "Amount", "%", 0, 1, .25);
    add("rate", "Rate", "Hz", .05, 1.5, .22, true);
    d.nodes[1].parameters = {{"depth", .30}, {"softness", .65}};
  }
  const auto addMode = [&](std::string key, std::string name) -> Mode & {
    d.modes.push_back(
        {std::move(key), std::move(name), d.nodes, d.connections, d.controls});
    return d.modes.back();
  };
  if (id == "color") {
    d.defaultMode = "analog";
    addMode("analog", "Analog");
    auto &tape = addMode("tape", "Tape");
    tape.controls[0].bindings[0] = {"fx", "drive", 0, -100, false, true};
    tape.controls[1].bindings[0].minimum = -80;
    tape.controls[1].bindings[0].maximum = 60;
    auto &tube = addMode("tube", "Tube");
    tube.controls[0].bindings[0] = {"fx", "drive", 0, 100, false, true};
  } else if (id == "doubler") {
    d.defaultMode = "natural";
    addMode("natural", "Natural");
    auto &tight = addMode("tight", "Tight");
    tight.nodes[1].parameters = {{"softness", .85}};
    tight.controls[1].bindings[0].maximum = .35;
    auto &wide = addMode("wide", "Wide");
    wide.nodes[1].parameters = {{"softness", .35}};
    wide.controls[1].bindings[0].minimum = .2;
  } else if (id == "chorus") {
    d.defaultMode = "classic";
    addMode("classic", "Classic");
    addMode("ensemble", "Ensemble").nodes[1].parameters = {{"depth", .55},
                                                           {"softness", .85}};
    addMode("deep", "Deep").nodes[1].parameters = {{"depth", .75},
                                                   {"softness", .45}};
  }
  return d;
}
json toJson(const MiniModuleDefinition &d) {
  if (!d.unavailableSource.empty())
    return json::parse(d.unavailableSource);
  json j{{"id", d.id},
         {"name", d.name},
         {"version", d.version},
         {"nodes", json::array()},
         {"connections", json::array()},
         {"controls", json::array()}};
  for (const auto &n : d.nodes) {
    json params = json::object();
    for (const auto &p : n.parameters)
      params[p.id] = p.value;
    j["nodes"].push_back({{"id", n.id},
                          {"type", n.type},
                          {"version", n.version},
                          {"parameters", params}});
    if (n.function)
      j["nodes"].back()["function"] = functionToJson(*n.function);
  }
  if (d.version >= 4)
    j["code"] = {{"abi", d.code.abi}, {"wasm", d.code.wasm},
                  {"sourceHash", d.code.sourceHash}};
  for (const auto &e : d.connections) {
    json edge{{"from", e.from}, {"to", e.to}};
    if (d.version >= 3) {
      edge["fromPort"] = e.fromPort;
      edge["toPort"] = e.toPort;
    }
    j["connections"].push_back(std::move(edge));
  }
  for (const auto &c : d.controls) {
    json bindings = json::array();
    for (const auto &b : c.bindings)
      bindings.push_back({{"node", b.node},
                          {"parameter", b.parameter},
                          {"min", b.minimum},
                          {"max", b.maximum},
                          {"log", b.logarithmic},
                          {"bipolarMagnitude", b.bipolarMagnitude}});
    j["controls"].push_back({{"id", c.id},
                             {"name", c.name},
                             {"unit", c.unit},
                             {"min", c.minimum},
                             {"max", c.maximum},
                             {"default", c.initial},
                             {"log", c.logarithmic},
                             {"bindings", bindings},
                             {"style", c.style}});
  }
  if (d.version >= 2) {
    j["appearance"] = {{"theme", d.appearance.theme},
                       {"controlStyle", d.appearance.controlStyle},
                       {"backgroundColor", d.appearance.backgroundColor},
                       {"backgroundImage", d.appearance.backgroundImage}};
    j["defaultMode"] = d.defaultMode;
    j["modes"] = json::array();
    for (const auto &m : d.modes) {
      MiniModuleDefinition graph;
      graph.id = m.id;
      graph.name = m.name;
      graph.version = d.version >= 3 ? d.version : 1;
      graph.nodes = m.nodes;
      graph.connections = m.connections;
      graph.controls = m.controls;
      graph.code = m.code;
      auto mode = toJson(graph);
      mode.erase("appearance");
      mode.erase("modes");
      mode.erase("defaultMode");
      j["modes"].push_back(std::move(mode));
    }
  }
  return j;
}
MiniModuleDefinition fromJson(const json &j) {
  MiniModuleDefinition d;
  try {
    d.id = j.at("id").get<std::string>();
    d.name = j.at("name").get<std::string>();
    d.version = j.at("version").get<unsigned>();
    if (d.version < 1 || d.version > 4)
      throw std::runtime_error("version");
    if (!j.at("nodes").is_array() || j.at("nodes").size() > kMaxNodes ||
        !j.at("connections").is_array() ||
        j.at("connections").size() > kMaxEdges ||
        !j.at("controls").is_array() || j.at("controls").size() > 2)
      throw std::runtime_error("size");
    for (const auto &n : j.at("nodes")) {
      NodeDefinition v{n.at("id").get<std::string>(),
                       n.at("type").get<std::string>(),
                       n.at("version").get<unsigned>()};
      if (!n.at("parameters").is_object() || n.at("parameters").size() > 64)
        throw std::runtime_error("parameters");
      for (const auto &[key, value] : n.at("parameters").items())
        v.parameters.push_back({key, value.get<double>()});
      if (n.contains("function")) {
        if (d.version < 4) throw std::runtime_error("function version");
        v.function = functionFromJson(n.at("function"));
      }
      d.nodes.push_back(std::move(v));
    }
    if (d.version >= 4 && j.contains("code")) {
      const auto &c = j.at("code");
      d.code = {c.at("abi").get<unsigned>(), c.at("wasm").get<std::string>(),
                c.at("sourceHash").get<std::string>()};
      if (d.code.abi != 1)
        throw std::runtime_error("unavailable code ABI");
      if (d.code.wasm.size() > kMaxCodeArtifactBytes * 2 ||
          d.code.sourceHash.size() > 128)
        throw std::runtime_error("code artifact size");
    }
    for (const auto &e : j.at("connections"))
      d.connections.push_back({e.at("from").get<std::string>(),
                               e.at("to").get<std::string>(),
                               e.value("fromPort", std::string{}),
                               e.value("toPort", std::string{})});
    for (const auto &c : j.at("controls")) {
      Control v{
          c.at("id").get<std::string>(),   c.at("name").get<std::string>(),
          c.at("unit").get<std::string>(), c.at("min").get<double>(),
          c.at("max").get<double>(),       c.at("default").get<double>(),
          c.at("log").get<bool>()};
      if (!c.at("bindings").is_array() || c.at("bindings").size() > 64)
        throw std::runtime_error("bindings");
      for (const auto &b : c.at("bindings"))
        v.bindings.push_back(
            {b.at("node").get<std::string>(),
             b.at("parameter").get<std::string>(), b.at("min").get<double>(),
             b.at("max").get<double>(), b.at("log").get<bool>(),
             b.value("bipolarMagnitude", false)});
      v.style = c.value("style", std::string{});
      d.controls.push_back(std::move(v));
    }
    if (j.contains("appearance")) {
      const auto &a = j.at("appearance");
      d.appearance = {
          a.value("theme", "studio"), a.value("controlStyle", "machined"),
          a.value("backgroundColor", ""), a.value("backgroundImage", "")};
    }
    d.defaultMode = j.value("defaultMode", std::string{});
    if (j.contains("modes")) {
      if (!j.at("modes").is_array() || j.at("modes").size() > 8)
        throw std::runtime_error("modes");
      for (const auto &m : j.at("modes")) {
        // Modes are flat graphs; recursive variants cannot consume parser stack
        // or memory.
        if (m.contains("modes") || m.contains("appearance") ||
            m.contains("defaultMode"))
          throw std::runtime_error("nested mode");
        auto graph = fromJson(m);
        if (!graph.unavailableSource.empty())
          throw std::runtime_error("mode graph");
        d.modes.push_back({graph.id, graph.name, graph.nodes, graph.connections,
                           graph.controls, graph.code});
      }
    }
  } catch (const std::exception &) {
    d = {};
    d.unavailableSource = j.dump();
  }
  // Only the exact shipped v1 graphs receive new modes; portable custom graphs
  // stay unchanged.
  if (d.unavailableSource.empty() && d.version == 1 &&
      (d.id == "color" || d.id == "doubler" || d.id == "chorus")) {
    auto current = builtin(d.id);
    auto legacy = resolved(current);
    legacy.version = 1;
    if (legacy == d)
      d = std::move(current);
  }
  return d;
}
} // namespace daw::plugins::mini
