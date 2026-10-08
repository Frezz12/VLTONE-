#include "CreatorProject.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "Creator/CodeUtilities.hpp"
#include <QUuid>
#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace ui {
using namespace daw::plugins::mini;
namespace {
std::string newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
bool boundary(const NodeDefinition &n) {
  return n.type == "input" || n.type == "output" || n.type == "interface" || n.type == "subgraph_input" || n.type == "subgraph_output";
}
}
QString CreatorProject::pack(const QStringList &selection, const QString &name, QString &error) {
  auto d = graph(); std::set<std::string> chosen;
  for (const auto &n : d.nodes) if (selection.contains(QString::fromStdString(n.id)) && !boundary(n)) chosen.insert(n.id);
  if (chosen.empty()) { error = "Select processing nodes to create a custom node"; return {}; }
  SubgraphDefinition group; group.id = newId(); group.name = name.toStdString();
  NodeDefinition instance = makeNode("subgraph", newId()); instance.subgraph = group.id;
  std::vector<Connection> outside;
  using Key = std::pair<std::string, std::string>;
  std::map<Key, std::pair<std::string, std::string>> inputPorts, outputPorts;
  const auto nodeById = [&](const std::string &id) -> const NodeDefinition & {
    return *std::find_if(d.nodes.begin(), d.nodes.end(), [&](const auto &n) { return n.id == id; });
  };
  const auto portFor = [&](const Connection &e, bool output) -> std::optional<GraphPort> {
    const auto &node = nodeById(output ? e.from : e.to);
    const auto desc = describeNode(node, &d);
    const auto ports = output ? daw::plugins::mini::outputPorts(node, d) : desc.inputs;
    auto p = std::find_if(ports.begin(), ports.end(), [&](const auto &p) { return p.id == (output ? e.fromPort : e.toPort); });
    if (p == ports.end()) return {};
    GraphPort result; result.id = newId(); result.name = p->name; result.type = portTypeId(p->type);
    result.signature = p->signature; result.capacity = p->capacity ? p->capacity : node.capacity;
    if (!output && p->parameter >= 0) {
      const auto &param = desc.parameters[p->parameter];
      result.minimum = param.minimum; result.maximum = param.maximum; result.initial = param.initial;
      result.unit = param.unit; result.logarithmic = param.logarithmic;
      for (const auto &value : node.parameters) if (value.id == param.id) result.initial = value.value;
    }
    return result;
  };
  for (const auto &n : d.nodes) if (chosen.contains(n.id)) group.nodes.push_back(n);
  for (const auto &e : d.connections) {
    const bool from = chosen.contains(e.from), to = chosen.contains(e.to);
    if (from && to) { group.connections.push_back(e); continue; }
    if (!from && !to) { outside.push_back(e); continue; }
    auto &ports = from ? outputPorts : inputPorts;
    const Key key{e.from, e.fromPort};
    auto found = ports.find(key);
    if (found == ports.end()) {
      auto port = portFor(e, from);
      if (!port) { error = "Repair missing ports before grouping"; return {}; }
      auto b = makeNode(from ? "subgraph_output" : "subgraph_input", newId());
      b.port = port->id; b.valueType = port->type; b.capacity = port->capacity; b.label = port->name; b.signature = port->signature;
      b.parameters = {{"default", port->initial}};
      found = ports.emplace(key, std::pair{port->id, b.id}).first;
      if (from) { group.outputs.push_back(*port); group.connections.push_back({e.from, b.id, e.fromPort, "in"}); }
      else { group.inputs.push_back(*port); outside.push_back({e.from, instance.id, e.fromPort, port->id}); }
      group.nodes.push_back(std::move(b));
    }
    if (from) outside.push_back({instance.id, e.to, found->second.first, e.toPort});
    else group.connections.push_back({found->second.second, e.to, "out", e.toPort});
  }
  if (group.outputs.empty() || group.inputs.size() > 16 || group.outputs.size() > 16) {
    error = "Custom nodes need an outgoing connection and support up to 16 ports per side"; return {};
  }
  const QString childKey = "node/" + QString::fromStdString(group.id);
  const auto key = layoutKey();
  const QPointF anchor = positions[key].value(QString::fromStdString(*chosen.begin()));
  unsigned in = 0, out = 0;
  for (const auto &n : group.nodes) {
    const auto id = QString::fromStdString(n.id);
    positions[childKey][id] = n.type == "subgraph_input" ? QPointF(-280, in++ * 120.) : n.type == "subgraph_output" ? QPointF(580, out++ * 120.) : positions[key].value(id) - anchor;
    positions[key].remove(id);
  }
  positions[key][QString::fromStdString(instance.id)] = anchor;
  std::erase_if(d.nodes, [&](const auto &n) { return chosen.contains(n.id); });
  d.nodes.push_back(instance); d.connections = std::move(outside); d.subgraphs.push_back(std::move(group)); d.version = 5; d.code = {};
  setGraph(d); error.clear(); return QString::fromStdString(instance.id);
}
bool CreatorProject::unpack(const QString &id, QString &error) {
  auto d = graph();
  auto n = std::find_if(d.nodes.begin(), d.nodes.end(), [&](const auto &n) { return n.id == id.toStdString() && n.type == "subgraph"; });
  if (n == d.nodes.end()) { error = "Select a custom node"; return false; }
  const auto groupId = n->subgraph;
  auto group = std::find_if(d.subgraphs.begin(), d.subgraphs.end(), [&](const auto &g) { return g.id == groupId; });
  if (group == d.subgraphs.end()) { error = "Missing custom node definition"; return false; }
  if (group->oversampling != 1) { error = "An oversampled node keeps its sample-rate boundary; select 1x before expanding"; return false; }
  const auto anchor = positions[layoutKey()].value(id);
  const auto layout = positions.value("node/" + QString::fromStdString(groupId));
  // Only this instance expands. Other shared instances retain their identity.
  const auto unique = newId(); auto copy = *group; copy.id = unique; n->subgraph = unique;
  for (auto &g : d.subgraphs) g.oversampling = 2;
  d.subgraphs.push_back(copy);
  MiniModuleDefinition flat;
  std::string expansionError;
  if (!expandSubgraphs(d, flat, expansionError)) {
    error = QString::fromStdString(expansionError);
    return false;
  }
  flat.subgraphs = definition.subgraphs;
  for (const auto &child : copy.nodes) {
    const auto expandedId = "g_" + codeHash(id.toStdString() + "/" + child.id).substr(0, 32);
    positions[layoutKey()][QString::fromStdString(expandedId)] = anchor + layout.value(QString::fromStdString(child.id));
  }
  positions[layoutKey()].remove(id);
  flat.code = {}; setGraph(flat); error.clear(); return true;
}
bool CreatorProject::makeIndependent(const QString &id, QString &error) {
  auto d = graph();
  auto node = std::find_if(d.nodes.begin(), d.nodes.end(), [&](const auto &n) { return n.id == id.toStdString() && !n.subgraph.empty(); });
  if (node == d.nodes.end()) { error = "Select a custom node"; return false; }
  std::map<std::string, std::string> copies;
  std::function<std::string(const std::string &)> clone = [&](const std::string &source) {
    if (copies.contains(source)) return copies[source];
    auto found = std::find_if(definition.subgraphs.begin(), definition.subgraphs.end(), [&](const auto &g) { return g.id == source; });
    if (found == definition.subgraphs.end()) return source;
    auto copy = *found; copy.id = newId(); copies[source] = copy.id;
    for (auto &n : copy.nodes) if (!n.subgraph.empty()) n.subgraph = clone(n.subgraph);
    positions["node/" + QString::fromStdString(copy.id)] = positions.value("node/" + QString::fromStdString(source));
    d.subgraphs.push_back(copy); return copy.id;
  };
  node->subgraph = clone(node->subgraph); d.code = {}; setGraph(d); error.clear(); return true;
}
} // namespace ui
