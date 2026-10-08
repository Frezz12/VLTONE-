#include "CreatorCommands.hpp"
#include "Creator/CodeUtilities.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include <QRectF>
#include <QUuid>
#include <cmath>
#include <map>
#include <set>
namespace ui {
using namespace daw::plugins::mini;
using json = nlohmann::json;
std::string connectionError(const MiniModuleDefinition &graph,
                            const Connection &candidate) {
  const auto node = [&](const std::string &id) -> const NodeDefinition * {
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                    [&](const auto &n) { return n.id == id; });
    return found == graph.nodes.end() ? nullptr : &*found;
  };
  const auto type = [&](const Connection &edge) -> std::optional<PortType> {
    const auto *source = node(edge.from), *target = node(edge.to);
    if (!source || !target)
      return {};
    const auto outputs = outputPorts(*source, graph),
               inputs = inputPorts(*target, &graph);
    const auto a =
        std::find_if(outputs.begin(), outputs.end(),
                     [&](const auto &p) { return p.id == edge.fromPort; });
    const auto b =
        std::find_if(inputs.begin(), inputs.end(),
                     [&](const auto &p) { return p.id == edge.toPort; });
    if (a == outputs.end() || b == inputs.end() || !compatiblePorts(*a, *b))
      return {};
    return a->type;
  };
  const auto portType = type(candidate);
  if (!portType)
    return "Incompatible or missing ports";
  if (graph.connections.size() > kMaxEdges)
    return "Too many connections";
  const bool function = *portType == PortType::Function;
  if (!function && graph.version >= 5) {
    MiniModuleDefinition flat;
    std::string why;
    if (!expandSubgraphs(graph, flat, why))
      return why;
    std::vector<std::vector<unsigned>> islands;
    if (!programmingIslands(flat, islands, why))
      return why;
    std::map<std::string, unsigned> ids;
    for (unsigned i = 0; i < flat.nodes.size(); ++i)
      ids[flat.nodes[i].id] = i;
    std::vector<unsigned> degree(flat.nodes.size()), ready;
    std::vector<std::vector<unsigned>> next(flat.nodes.size());
    for (const auto &e : flat.connections)
      if (e.fromPort != "function" && ids.contains(e.from) &&
          ids.contains(e.to) &&
          !isMemoryWrite(flat.nodes[ids[e.to]], e.toPort)) {
        next[ids[e.from]].push_back(ids[e.to]);
        ++degree[ids[e.to]];
      }
    for (unsigned i = 0; i < degree.size(); ++i)
      if (!degree[i])
        ready.push_back(i);
    for (unsigned i = 0; i < ready.size(); ++i)
      for (auto child : next[ready[i]])
        if (!--degree[child])
          ready.push_back(child);
    if (ready.size() != flat.nodes.size()) {
      why = "Feedback needs previous-sample memory: ";
      for (unsigned i = 0; i < degree.size(); ++i)
        if (degree[i])
          why += flat.nodes[i].id + " ";
      return why;
    }
    return {};
  }
  std::vector<std::string> pending{candidate.to}, visited;
  while (!pending.empty()) {
    const auto current = std::move(pending.back());
    pending.pop_back();
    if (current == candidate.from)
      return function ? "Function dependencies contain a recursive cycle"
                      : "Graph contains a feedback cycle";
    if (std::find(visited.begin(), visited.end(), current) != visited.end())
      continue;
    visited.push_back(current);
    for (const auto &edge : graph.connections)
      if (edge.from == current) {
        const auto other = type(edge);
        if (!function && graph.version >= 5)
          if (const auto *target = node(edge.to);
              target && isMemoryWrite(*target, edge.toPort))
            continue;
        if (other && (*other == PortType::Function) == function)
          pending.push_back(edge.to);
      }
  }
  return {};
}
QPointF creatorFreePosition(const CreatorProject &project, const QString &id) {
  const auto graph = project.graph();
  auto position = QPointF(340, 80);
  const auto size = [&](const NodeDefinition &n) {
    const auto d = describeNode(n, &graph);
    return QSizeF(240, 100 + 28 * (d.inputs.size() + d.outputs.size() +
                                   d.parameters.size()));
  };
  auto own = QSizeF(240, 300);
  for (const auto &n : graph.nodes)
    if (n.id == id.toStdString())
      own = size(n);
  const auto positions = project.positions.value(project.layoutKey());
  for (unsigned pass = 0; pass < kMaxNodes; ++pass) {
    bool moved = false;
    for (const auto &n : graph.nodes) {
      const auto key = QString::fromStdString(n.id);
      if (key == id || !positions.contains(key))
        continue;
      auto occupied =
          QRectF(positions.value(key), size(n)).adjusted(-24, -24, 24, 24);
      if (QRectF(position, own).intersects(occupied)) {
        position.setY(occupied.bottom() + 24);
        moved = true;
      }
    }
    if (!moved)
      break;
  }
  return position;
}
json creatorNodeDescription(const NodeDefinition &node,
                            const MiniModuleDefinition &graph) {
  const auto d = describeNode(node, &graph);
  json result{{"type", d.id},
              {"name", d.name},
              {"category", d.category},
              {"description", d.description},
              {"inputs", json::array()},
              {"outputs", json::array()},
              {"parameters", json::array()}};
  for (const auto &p : inputPorts(node, &graph))
    result["inputs"].push_back({{"id", p.id},
                                {"type", portTypeId(p.type)},
                                {"required", p.required},
                                {"signature", p.signature}});
  for (const auto &p : outputPorts(node, graph))
    result["outputs"].push_back({{"id", p.id},
                                 {"type", portTypeId(p.type)},
                                 {"signature", p.signature}});
  for (const auto &p : d.parameters)
    result["parameters"].push_back({{"id", p.id},
                                    {"minimum", p.minimum},
                                    {"maximum", p.maximum},
                                    {"default", p.initial},
                                    {"unit", p.unit},
                                    {"choices", p.choices}});
  return result;
}
bool creatorApplyFunction(CreatorProject &p, const QString &id,
                          const QString &operation, const json &reply,
                          QString &error) {
  if (!reply.value("ok", false)) {
    error = QString::fromStdString(
        reply.value("error", std::string("C++ check failed")));
    return false;
  }
  try {
    auto g = p.graph();
    auto n = std::find_if(g.nodes.begin(), g.nodes.end(), [&](const auto &n) {
      return n.id == id.toStdString();
    });
    if (n == g.nodes.end() || !n->function)
      throw std::runtime_error("C++ node not found");
    n->function = functionFromJson(
        reply.at(operation == "analyze" ? "function" : "parent"));
    const auto d = describeNode(*n);
    std::erase_if(n->parameters, [&](auto &v) {
      auto it = std::find_if(d.parameters.begin(), d.parameters.end(),
                             [&](const auto &p) { return p.id == v.id; });
      if (it == d.parameters.end())
        return true;
      v.value = std::clamp(v.value, it->minimum, it->maximum);
      return false;
    });
    QString childId;
    if (operation != "analyze") {
      if (g.nodes.size() >= kMaxNodes || g.connections.size() >= kMaxEdges)
        throw std::runtime_error("Graph limit reached");
      childId = QUuid::createUuid().toString(QUuid::WithoutBraces);
      auto child = makeNode("cpp_function", childId.toStdString());
      child.function = functionFromJson(reply.at("child"));
      g.nodes.push_back(std::move(child));
      g.connections.push_back({childId.toStdString(), id.toStdString(),
                               "function",
                               reply.at("port").get<std::string>()});
    }
    p.setGraph(g);
    if (!childId.isEmpty())
      p.positions[p.layoutKey()][childId] = creatorFreePosition(p, childId);
    return true;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return false;
  }
}
bool creatorApplyBatch(CreatorProject &destination, const json &operations,
                       QString &error) {
  auto p = destination;
  try {
    if (!operations.is_array() || operations.size() > 128 ||
        operations.dump().size() > 2 * 1024 * 1024)
      throw std::runtime_error(
          "Expected at most 128 operations and 2 MiB per batch");
    for (const auto &op : operations) {
      const auto action = op.at("op").get<std::string>();
      const auto id = op.value("id", std::string{});
      if (action == "scope") {
        QStringList path;
        const auto ids = op.at("path").get<std::vector<std::string>>();
        if (ids.size() > kMaxGraphDepth)
          throw std::runtime_error("Subgraph nesting limit");
        auto graph = resolved(p.definition, p.activeMode.toStdString());
        for (const auto &key : ids) {
          if (std::none_of(graph.nodes.begin(), graph.nodes.end(),
                           [&](const auto &n) { return n.subgraph == key; }))
            throw std::runtime_error("Subgraph is not in the requested path");
          auto it = std::find_if(p.definition.subgraphs.begin(),
                                 p.definition.subgraphs.end(),
                                 [&](const auto &g) { return g.id == key; });
          if (it == p.definition.subgraphs.end())
            throw std::runtime_error("Subgraph definition missing");
          graph.nodes = it->nodes;
          path << QString::fromStdString(key);
        }
        p.graphPath = path;
        continue;
      }
      if (action == "subgraph") {
        auto it = std::find_if(p.definition.subgraphs.begin(),
                               p.definition.subgraphs.end(),
                               [&](const auto &g) { return g.id == id; });
        if (it == p.definition.subgraphs.end())
          throw std::runtime_error("Subgraph definition missing");
        auto j = subgraphToJson(*it);
        for (const char *key : {"name", "inputs", "outputs", "oversampling"})
          if (op.contains(key))
            j[key] = op.at(key);
        *it = subgraphFromJson(j);
        continue;
      }
      if (action == "pack" || action == "unpack" || action == "independent") {
        QString why;
        bool ok;
        if (action == "pack") {
          QStringList ids;
          for (const auto &i : op.at("nodes"))
            ids << QString::fromStdString(i.get<std::string>());
          ok = !p.pack(ids,
                       QString::fromStdString(op.at("name").get<std::string>()),
                       why)
                    .isEmpty();
        } else
          ok = action == "unpack"
                   ? p.unpack(QString::fromStdString(id), why)
                   : p.makeIndependent(QString::fromStdString(id), why);
        if (!ok)
          throw std::runtime_error(why.toStdString());
        continue;
      }
      if (action == "mode") {
        const auto modeAction = op.at("action").get<std::string>();
        if (modeAction == "add") {
          if (p.addMode(
                   QString::fromStdString(op.at("name").get<std::string>()))
                  .isEmpty())
            throw std::runtime_error("Mode limit reached");
        } else if (modeAction == "remove")
          p.removeMode(QString::fromStdString(id));
        else if (modeAction == "select") {
          if (!id.empty() &&
              std::none_of(p.definition.modes.begin(), p.definition.modes.end(),
                           [&](const auto &m) { return m.id == id; }))
            throw std::runtime_error("Unknown mode");
          p.activeMode = QString::fromStdString(id);
          p.graphPath.clear();
        } else if (modeAction == "rename" || modeAction == "default") {
          auto it =
              std::find_if(p.definition.modes.begin(), p.definition.modes.end(),
                           [&](const auto &m) { return m.id == id; });
          if (it == p.definition.modes.end())
            throw std::runtime_error("Unknown mode");
          if (modeAction == "rename")
            it->name = op.at("name").get<std::string>();
          else {
            p.definition.defaultMode = id;
            p.definition.nodes = it->nodes;
            p.definition.connections = it->connections;
            p.definition.code = it->code;
          }
        } else
          throw std::runtime_error("Unknown mode action");
        continue;
      }
      if (action == "controls" || action == "appearance") {
        auto j = toJson(p.definition);
        if (action == "appearance") {
          if (!op.at("value").is_object()) throw std::runtime_error("Appearance must be an object");
          for (const char* field : {"theme", "controlStyle", "backgroundColor", "backgroundImage"})
            if (op.at("value").contains(field)) j[action][field] = op.at("value").at(field);
        } else j[action] = op.at("value");
        auto d = fromJson(j);
        if (!d.unavailableSource.empty() || d.controls.size() > 2)
          throw std::runtime_error(
              "Invalid controls or appearance; at most two external controls");
        for (const auto &c : d.controls)
          if (!std::isfinite(c.minimum) || !std::isfinite(c.maximum) ||
              c.minimum >= c.maximum || c.initial < c.minimum ||
              c.initial > c.maximum)
            throw std::runtime_error("Invalid external control range");
        p.definition.controls = d.controls;
        p.definition.appearance = d.appearance;
        p.syncControls();
        continue;
      }
      auto g = p.graph();
      const auto find = [&]() -> NodeDefinition & {
        auto at = std::find_if(g.nodes.begin(), g.nodes.end(),
                               [&](const auto &n) { return n.id == id; });
        if (at == g.nodes.end())
          throw std::runtime_error("Node not found: " + id);
        return *at;
      };
      if (action == "add") {
        if (id.empty() || id.size() > 128 || g.nodes.size() >= kMaxNodes ||
            std::any_of(g.nodes.begin(), g.nodes.end(),
                        [&](const auto &n) { return n.id == id; }))
          throw std::runtime_error(
              "Missing, duplicate or invalid node ID / node limit");
        const auto type = op.at("type").get<std::string>();
        if (!nodeDescription(type))
          throw std::runtime_error("Unknown node type; use creator_catalog");
        auto node = makeNode(type, id);
        if (type == "cpp_function") {
          node.function = op.contains("function")
                              ? functionFromJson(op.at("function"))
                              : FunctionDefinition{};
          if (node.function->source.empty())
            node.function->source =
                functionScaffold(node.function->entry, node.function->inputs,
                                 node.function->outputs);
          if (node.function->source.size() > kMaxFunctionSourceBytes)
            throw std::runtime_error("C++ source exceeds 256 KiB");
        }
        g.nodes.push_back(std::move(node));
        p.setGraph(g);
        p.positions[p.layoutKey()][QString::fromStdString(id)] =
            creatorFreePosition(p, QString::fromStdString(id));
        continue;
      } else if (action == "remove") {
        find();
        std::erase_if(g.nodes, [&](const auto &n) { return n.id == id; });
        std::erase_if(g.connections, [&](const auto &c) {
          return c.from == id || c.to == id;
        });
        p.positions[p.layoutKey()].remove(QString::fromStdString(id));
      } else if (action == "parameter") {
        auto &n = find();
        const auto key = op.at("parameter").get<std::string>();
        const double value = op.at("value").get<double>();
        const auto d = describeNode(n, &g);
        const auto parameter =
            std::find_if(d.parameters.begin(), d.parameters.end(),
                         [&](const auto &p) { return p.id == key; });
        if (parameter == d.parameters.end() || !std::isfinite(value) ||
            value < parameter->minimum || value > parameter->maximum)
          throw std::runtime_error("Unknown parameter or value out of range");
        auto it = std::find_if(n.parameters.begin(), n.parameters.end(),
                               [&](const auto &p) { return p.id == key; });
        if (it == n.parameters.end())
          n.parameters.push_back({key, value});
        else
          it->value = value;
      } else if (action == "source") {
        auto &n = find();
        if (!n.function)
          throw std::runtime_error("Not a C++ node");
        auto source = op.at("source").get<std::string>();
        if (source.size() > kMaxFunctionSourceBytes)
          throw std::runtime_error("C++ source exceeds 256 KiB");
        n.function->source = source;
        n.function->entry = op.value("entry", n.function->entry);
      } else if (action == "configure") {
        auto &n = find();
        if (op.contains("value_type")) {
          const auto type = op.at("value_type").get<std::string>();
          if (!parsePortType(type))
            throw std::runtime_error("Unknown value type");
          n.valueType = type;
        }
        if (op.contains("capacity")) {
          n.capacity = op.at("capacity").get<unsigned>();
          if (!n.capacity || n.capacity > kMaxCollection)
            throw std::runtime_error("Collection capacity out of range");
        }
        if (op.contains("values")) {
          auto values = op.at("values").get<std::vector<double>>();
          if (values.size() > kMaxCollection)
            throw std::runtime_error("Too many values");
          for (double v : values)
            if (!std::isfinite(v))
              throw std::runtime_error("Nonfinite value");
          n.values = std::move(values);
        }
        if (op.contains("label"))
          n.label = op.at("label").get<std::string>();
      } else if (action == "connect" || action == "disconnect") {
        Connection edge{op.at("from").get<std::string>(),
                        op.at("to").get<std::string>(),
                        op.at("from_port").get<std::string>(),
                        op.at("to_port").get<std::string>()};
        if (action == "disconnect")
          std::erase(g.connections, edge);
        else {
          if (std::find(g.connections.begin(), g.connections.end(), edge) !=
              g.connections.end())
            continue;
          if (std::any_of(g.connections.begin(), g.connections.end(),
                          [&](const auto &e) {
                            return e.to == edge.to && e.toPort == edge.toPort;
                          }))
            throw std::runtime_error(
                "Input already connected; disconnect explicitly first");
          g.connections.push_back(edge);
          auto why = connectionError(g, edge);
          if (!why.empty())
            throw std::runtime_error(why);
        }
      } else
        throw std::runtime_error("Unknown graph operation: " + action);
      p.setGraph(g);
    }
    destination = std::move(p);
    return true;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return false;
  }
}
} // namespace ui
