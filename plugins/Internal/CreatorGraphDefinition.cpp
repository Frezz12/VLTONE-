#include "MiniNodeRegistry.hpp"
#include "Creator/CodeUtilities.hpp"
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace daw::plugins::mini {
using json = nlohmann::json;
json subgraphToJson(const SubgraphDefinition &g) {
  MiniModuleDefinition d;
  d.id = g.id; d.name = g.name; d.version = 5; d.nodes = g.nodes; d.connections = g.connections;
  auto j = toJson(d);
  for (const auto *key : {"appearance", "modes", "defaultMode", "subgraphs", "controls", "code"}) j.erase(key);
  const auto ports = [](const auto &list) {
    auto a = json::array();
    for (const auto &p : list) a.push_back({{"id", p.id}, {"name", p.name}, {"type", p.type},
      {"unit", p.unit}, {"min", p.minimum}, {"max", p.maximum}, {"default", p.initial},
      {"capacity", p.capacity}, {"log", p.logarithmic}, {"signature", p.signature}});
    return a;
  };
  j["inputs"] = ports(g.inputs); j["outputs"] = ports(g.outputs); j["oversampling"] = g.oversampling;
  return j;
}
SubgraphDefinition subgraphFromJson(const json &source) {
  if (!source.is_object() || source.contains("subgraphs") || source.contains("modes"))
    throw std::runtime_error("Nested subgraph document");
  auto j = source;
  j["controls"] = json::array(); j["version"] = 5;
  auto d = fromJson(j);
  if (!d.unavailableSource.empty()) throw std::runtime_error("Invalid subgraph document");
  SubgraphDefinition g;
  g.id = d.id; g.name = d.name; g.nodes = std::move(d.nodes); g.connections = std::move(d.connections);
  g.oversampling = j.value("oversampling", 1u);
  const auto ports = [](const json &a) {
    if (!a.is_array() || a.size() > 16) throw std::runtime_error("A custom node supports at most 16 ports per side");
    std::vector<GraphPort> result;
    for (const auto &p : a) {
      GraphPort v;
      v.id = p.at("id"); v.name = p.at("name"); v.type = p.at("type"); v.unit = p.value("unit", "");
      v.signature = p.value("signature", ""); v.minimum = p.value("min", -1e6);
      v.maximum = p.value("max", 1e6); v.initial = p.value("default", 0.);
      v.capacity = p.value("capacity", 64u); v.logarithmic = p.value("log", false);
      result.push_back(std::move(v));
    }
    return result;
  };
  g.inputs = ports(j.at("inputs")); g.outputs = ports(j.at("outputs"));
  return g;
}
namespace {
const SubgraphDefinition *findGroup(const MiniModuleDefinition &d, const std::string &id) {
  for (const auto &g : d.subgraphs) if (g.id == id) return &g;
  return nullptr;
}
std::string instanceId(const std::string &parent, const std::string &node) {
  return "g_" + codeHash(parent + "/" + node).substr(0, 32);
}
bool expand(MiniModuleDefinition &d, unsigned depth, std::string &error) {
  if (depth > kMaxGraphDepth) { error = "Subgraphs exceed eight levels"; return false; }
  std::vector<NodeDefinition> nodes;
  auto edges = d.connections;
  for (const auto &n : d.nodes) {
    if (n.type != "subgraph") { nodes.push_back(n); continue; }
    const auto *g = findGroup(d, n.subgraph);
    if (!g) { error = "Unavailable custom node: " + n.subgraph; return false; }
    if (g->oversampling != 1) { nodes.push_back(n); continue; }
    MiniModuleDefinition child;
    child.version = 5; child.nodes = g->nodes; child.connections = g->connections; child.subgraphs = d.subgraphs;
    std::map<std::string, std::string> ids, inputs, outputs;
    for (auto &v : child.nodes) {
      if (v.stateKey.empty()) v.stateKey = v.id;
      auto id = instanceId(n.id, v.id); ids[v.id] = id; v.id = id;
      if (v.type == "subgraph_input" || v.type == "subgraph_output") {
        const auto &ports = v.type == "subgraph_input" ? g->inputs : g->outputs;
        auto p = std::find_if(ports.begin(), ports.end(), [&](const auto &p) { return p.id == v.port; });
        if (p == ports.end()) { error = "Missing custom port: " + v.port; return false; }
        (v.type == "subgraph_input" ? inputs : outputs)[v.port] = id;
        v.valueType = p->type; v.capacity = p->capacity; v.signature = p->signature;
        double fallback = p->initial;
        for (const auto &parameter : n.parameters) if (parameter.id == p->id) fallback = parameter.value;
        v.parameters = {{"default", fallback}};
        v.type = "wire";
      }
    }
    for (auto &e : child.connections) {
      if (!ids.contains(e.from) || !ids.contains(e.to)) { error = "Missing node inside " + g->name; return false; }
      e.from = ids.at(e.from); e.to = ids.at(e.to);
    }
    if (!expand(child, depth + 1, error)) return false;
    for (auto &e : edges) {
      if (e.from == n.id) {
        if (!outputs.contains(e.fromPort)) { error = "Missing custom output: " + e.fromPort; return false; }
        e.from = outputs.at(e.fromPort); e.fromPort = "out";
      }
      if (e.to == n.id) {
        if (!inputs.contains(e.toPort)) { error = "Missing custom input: " + e.toPort; return false; }
        e.to = inputs.at(e.toPort); e.toPort = "in";
      }
    }
    nodes.insert(nodes.end(), child.nodes.begin(), child.nodes.end());
    edges.insert(edges.end(), child.connections.begin(), child.connections.end());
    if (nodes.size() > kMaxNodes || edges.size() > kMaxEdges) { error = "Expanded graph exceeds 512 nodes or 2048 connections"; return false; }
  }
  d.nodes = std::move(nodes); d.connections = std::move(edges);
  // A callable boundary is a compile-time alias, never a scheduled audio wire.
  bool removed;
  do {
    removed = false;
    for (auto it = d.nodes.begin(); it != d.nodes.end(); ++it) if (it->type == "wire" && it->valueType == "function") {
      auto input = std::find_if(d.connections.begin(), d.connections.end(), [&](const auto &e) { return e.to == it->id; });
      if (input == d.connections.end()) { error = "Connect custom function input"; return false; }
      const auto source = *input;
      for (auto &e : d.connections) if (e.from == it->id) { e.from = source.from; e.fromPort = source.fromPort; }
      const auto id = it->id;
      std::erase_if(d.connections, [&](const auto &e) { return e.to == id; });
      d.nodes.erase(it); removed = true; break;
    }
  } while (removed);
  return true;
}
}
bool expandSubgraphs(const MiniModuleDefinition &d, MiniModuleDefinition &out, std::string &error) {
  out = d;
  return d.version < 5 || expand(out, 0, error);
}
bool programmingIslands(const MiniModuleDefinition &d, std::vector<std::vector<unsigned>> &result, std::string &error) {
  const unsigned size = unsigned(d.nodes.size());
  std::map<std::string, unsigned> ids;
  for (unsigned i = 0; i < size; ++i) ids[d.nodes[i].id] = i;
  std::vector<std::vector<unsigned>> next(size);
  std::vector<std::pair<unsigned, unsigned>> edges;
  for (const auto &e : d.connections) if (e.fromPort != "function") {
    if (!ids.contains(e.from) || !ids.contains(e.to)) { error = "Missing connected node"; return false; }
    auto a = ids[e.from], b = ids[e.to]; next[a].push_back(b); edges.emplace_back(a, b);
  }
  std::vector<int> index(size, -1), low(size), stack;
  std::vector<bool> onStack(size);
  std::vector<unsigned> group(size);
  unsigned serial = 0, groups = 0;
  std::function<void(unsigned)> visit = [&](unsigned i) {
    index[i] = low[i] = int(serial++); stack.push_back(int(i)); onStack[i] = true;
    for (auto n : next[i]) {
      if (index[n] < 0) { visit(n); low[i] = std::min(low[i], low[n]); }
      else if (onStack[n]) low[i] = std::min(low[i], index[n]);
    }
    if (low[i] == index[i]) {
      unsigned n;
      do { n = unsigned(stack.back()); stack.pop_back(); onStack[n] = false; group[n] = groups; } while (n != i);
      ++groups;
    }
  };
  for (unsigned i = 0; i < size; ++i) if (index[i] < 0) visit(i);
  std::vector<unsigned> counts(groups), parent(groups);
  std::vector<bool> block(groups), self(groups);
  std::iota(parent.begin(), parent.end(), 0u);
  for (unsigned i = 0; i < size; ++i) { ++counts[group[i]]; block[group[i]] = block[group[i]] || isBlockNode(d.nodes[i]); }
  for (auto [a, b] : edges) if (a == b) self[group[a]] = true;
  for (unsigned i = 0; i < size; ++i) if ((counts[group[i]] > 1 || self[group[i]]) && block[group[i]]) {
    error = "Feedback must stay inside elementary nodes at one sample rate: " + d.nodes[i].id;
    return false;
  }
  const auto root = [&](unsigned g) { while (parent[g] != g) g = parent[g]; return g; };
  const auto acyclic = [&] {
    std::vector<unsigned> degree(groups); std::vector<std::vector<unsigned>> successors(groups);
    std::set<std::pair<unsigned, unsigned>> unique;
    for (auto [a, b] : edges) {
      a = root(group[a]); b = root(group[b]);
      if (a != b && unique.emplace(a, b).second) { successors[a].push_back(b); ++degree[b]; }
    }
    std::vector<unsigned> ready;
    unsigned total = 0;
    for (unsigned i = 0; i < groups; ++i) if (root(i) == i) { ++total; if (!degree[i]) ready.push_back(i); }
    for (unsigned i = 0; i < ready.size(); ++i) for (auto j : successors[ready[i]]) if (!--degree[j]) ready.push_back(j);
    return ready.size() == total;
  };
  // Convex fusion: never merge across a path containing a block processor.
  for (auto [a, b] : edges) {
    a = root(group[a]); b = root(group[b]);
    if (a == b || block[a] || block[b]) continue;
    parent[b] = a;
    if (!acyclic()) parent[b] = b;
  }
  std::map<unsigned, unsigned> finalIds;
  result.clear();
  for (unsigned i = 0; i < size; ++i) {
    auto [it, added] = finalIds.emplace(root(group[i]), unsigned(result.size()));
    if (added) result.emplace_back();
    result[it->second].push_back(i);
  }
  return true;
}
std::string validateProgrammingGraph(const MiniModuleDefinition &d) {
  std::set<std::string> definitions;
  for (const auto &g : d.subgraphs) {
    if (g.id.empty() || g.id.size() > 128 || g.name.empty() || g.name.size() > 128 || !definitions.insert(g.id).second ||
        g.inputs.size() > 16 || g.outputs.empty() || g.outputs.size() > 16 ||
        (g.oversampling != 1 && g.oversampling != 2 && g.oversampling != 4)) return "Invalid custom node: " + g.name;
    for (bool output : {false, true}) {
      std::set<std::string> ports;
      for (const auto &p : output ? g.outputs : g.inputs) {
        if (p.id.empty() || p.id.size() > 128 || p.name.empty() || p.name.size() > 128 || !ports.insert(p.id).second ||
            !parsePortType(p.type) || p.capacity < 1 || p.capacity > kMaxCollection || !std::isfinite(p.minimum) ||
            !std::isfinite(p.maximum) || !std::isfinite(p.initial) || p.minimum > p.maximum ||
            p.initial < p.minimum || p.initial > p.maximum || (p.logarithmic && p.minimum <= 0) ||
            (p.type == "integer" && std::trunc(p.initial) != p.initial) ||
            (p.type == "gate" && p.initial != 0 && p.initial != 1)) return "Invalid custom port: " + p.name;
        if (std::count_if(g.nodes.begin(), g.nodes.end(), [&](const auto &n) {
              return n.type == (output ? "subgraph_output" : "subgraph_input") && n.port == p.id && n.valueType == p.type;
            }) != 1) return "Custom port needs one matching boundary node: " + p.name;
      }
    }
  }
  std::function<std::string(const std::vector<NodeDefinition> &, std::set<std::string>, unsigned)> dependencies;
  unsigned expanded = 0;
  unsigned expandedEdges = 0;
  dependencies = [&](const auto &nodes, std::set<std::string> path, unsigned depth) -> std::string {
    if (depth > kMaxGraphDepth) return "Subgraphs exceed eight levels";
    expanded += unsigned(nodes.size());
    if (expanded > kMaxNodes) return "Expanded graph exceeds 512 nodes";
    for (const auto &n : nodes) {
      if (!parsePortType(n.valueType) || n.stateKey.size() > 128 || n.capacity < 1 || n.capacity > kMaxCollection || n.values.size() > kMaxCollection * 2 ||
          !std::all_of(n.values.begin(), n.values.end(), [](double v) { return std::isfinite(v); })) return "Invalid node data: " + n.id;
      if (n.type == "curve") {
        if (n.values.size() < 4 || n.values.size() % 2) return "Curve needs at least two x/y points: " + n.id;
        for (unsigned i = 2; i < n.values.size(); i += 2) if (n.values[i] <= n.values[i - 2]) return "Curve points must have increasing X: " + n.id;
      }
      if ((n.type == "array" || n.type == "list") && n.values.size() > n.capacity) return "Initial values exceed collection capacity: " + n.id;
      if (n.type == "history" && (n.valueType == "buffer" || n.valueType == "function")) return "History stores values, not buffer or function handles: " + n.id;
      if (n.type == "history" || n.type == "wire" || n.type == "subgraph_input" || n.type == "subgraph_output")
        for (const auto &p : n.parameters)
          if ((n.valueType == "integer" && std::trunc(p.value) != p.value) ||
              (n.valueType == "gate" && p.value != 0 && p.value != 1)) return "Integer/Gate state needs a value of its exact type: " + n.id;
      if (n.type == "subgraph" || n.type == "map" || n.type == "reduce") {
        const auto *g = findGroup(d, n.subgraph);
        if (!g) return "Choose a custom node definition: " + n.id;
        expandedEdges += unsigned(g->connections.size());
        if (expandedEdges + d.connections.size() > kMaxEdges) return "Expanded graph exceeds 2048 connections";
        if (path.contains(g->id)) return "Recursive custom node: " + g->name;
        auto childPath = path; childPath.insert(g->id);
        if (auto error = dependencies(g->nodes, std::move(childPath), depth + 1); !error.empty()) return error;
        MiniModuleDefinition child; child.version = 5; child.nodes = g->nodes; child.connections = g->connections; child.subgraphs = d.subgraphs;
        if (auto error = validateTypedGraph(child, true); !error.empty()) return g->name + ": " + error;
      }
    }
    return {};
  };
  if (auto error = dependencies(d.nodes, {}, 0); !error.empty()) return error;
  MiniModuleDefinition flat; std::string error;
  if (!expandSubgraphs(d, flat, error)) return error;
  if (auto error = validateTypedGraph(flat); !error.empty()) return error;
  std::vector<std::vector<unsigned>> islands;
  if (!programmingIslands(flat, islands, error)) return error;
  const auto functions = std::count_if(flat.nodes.begin(), flat.nodes.end(), [](const auto &n) { return n.function.has_value(); });
  if (functions > 64) return "A graph supports at most 64 C++ functions";
  return {};
}
} // namespace daw::plugins::mini
