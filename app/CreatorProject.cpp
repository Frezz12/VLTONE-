#include "CreatorProject.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include <QFile>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace ui {
using namespace daw::plugins::mini;
using json = nlohmann::json;
static QString uuid() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
CreatorProject CreatorProject::create(const QString &projectName,
                                      const QString &moduleName) {
  CreatorProject p;
  p.name = projectName;
  auto &d = p.definition;
  d.version = 4;
  d.id = "creator." + uuid().toStdString();
  d.name = moduleName.toStdString();
  d.nodes = {makeNode("input", "input"), makeNode("output", "output"),
             makeNode("interface", "interface")};
  d.connections = {{"input", "output", "out", "in"}};
  d.controls = {{"control_1", "Control 1", "", 0, 1, .5},
                {"control_2", "Control 2", "", 0, 1, .5}};
  p.positions[{}] = {
      {"input", {40, 80}}, {"output", {680, 80}}, {"interface", {40, 280}}};
  return p;
}
MiniModuleDefinition CreatorProject::graph() const {
  auto d = resolved(definition, activeMode.toStdString());
  d.appearance = definition.appearance;
  return d;
}
void CreatorProject::setGraph(const MiniModuleDefinition &d) {
  definition.version = std::max(definition.version, d.version);
  for (auto &mode : definition.modes)
    if (mode.id == activeMode.toStdString()) {
      mode.nodes = d.nodes;
      mode.connections = d.connections;
      mode.controls = definition.controls;
      mode.code = d.code;
      if (mode.id == definition.defaultMode) {
        definition.nodes = d.nodes;
        definition.connections = d.connections;
        definition.code = d.code;
      }
      return;
    }
  definition.nodes = d.nodes;
  definition.connections = d.connections;
  definition.code = d.code;
}
void CreatorProject::syncControls() {
  for (auto &mode : definition.modes)
    mode.controls = definition.controls;
  const auto trim = [&](auto &connections, const auto &nodes) {
    std::erase_if(connections, [&](const auto &e) {
      const auto source =
          std::find_if(nodes.begin(), nodes.end(),
                       [&](const auto &n) { return n.id == e.from; });
      return source != nodes.end() && source->type == "interface" &&
             std::none_of(definition.controls.begin(),
                          definition.controls.end(),
                          [&](const auto &c) { return c.id == e.fromPort; });
    });
  };
  trim(definition.connections, definition.nodes);
  for (auto &mode : definition.modes)
    trim(mode.connections, mode.nodes);
}
QString CreatorProject::addMode(const QString &name) {
  if (definition.modes.size() >= 8)
    return {};
  auto current = graph();
  if (definition.modes.empty()) {
    const auto first = uuid();
    definition.defaultMode = first.toStdString();
    definition.modes.push_back({first.toStdString(), "Main", definition.nodes,
                                definition.connections, definition.controls, definition.code});
    positions[first] = positions.take({});
    viewports[first] = viewports.take({});
    activeMode = first;
  }
  const auto id = uuid();
  definition.modes.push_back({id.toStdString(), name.toStdString(),
                              current.nodes, current.connections,
                              definition.controls, current.code});
  positions[id] = positions[activeMode];
  viewports[id] = viewports[activeMode];
  activeMode = id;
  return id;
}
void CreatorProject::removeMode(const QString &id) {
  if (definition.modes.size() < 2)
    return;
  std::erase_if(definition.modes,
                [&](const auto &m) { return m.id == id.toStdString(); });
  positions.remove(id);
  viewports.remove(id);
  if (definition.defaultMode == id.toStdString())
    definition.defaultMode = definition.modes.front().id;
  if (activeMode == id)
    activeMode = QString::fromStdString(definition.defaultMode);
  const auto d = resolved(definition);
  definition.nodes = d.nodes;
  definition.connections = d.connections;
  definition.code = d.code;
  if (definition.modes.size() == 1) {
    positions[{}] = positions.take(activeMode);
    viewports[{}] = viewports.take(activeMode);
    activeMode.clear();
    definition.defaultMode.clear();
    definition.modes.clear();
  }
}
json CreatorProject::toJson() const {
  json result{{"format", "vltcreator"},
              {"version", 2},
              {"name", name.toStdString()},
              {"activeMode", activeMode.toStdString()},
              {"definition", daw::plugins::mini::toJson(definition)},
              {"positions", json::object()},
              {"viewports", json::object()}};
  result["codeNode"] = codeNode.toStdString();
  result["codeCursors"] = json::object();
  for (auto it=codeCursors.begin();it!=codeCursors.end();++it)
    result["codeCursors"][it.key().toStdString()]=it.value();
  for (auto it = positions.begin(); it != positions.end(); ++it)
    for (auto pos = it->begin(); pos != it->end(); ++pos)
      result["positions"][it.key().toStdString()][pos.key().toStdString()] = {
          pos->x(), pos->y()};
  for (auto it = viewports.begin(); it != viewports.end(); ++it)
    result["viewports"][it.key().toStdString()] = {
        {"center", {it->center.x(), it->center.y()}}, {"zoom", it->zoom}};
  return result;
}
bool CreatorProject::fromJson(const json &j, CreatorProject &result,
                              QString &error) {
  try {
    if (!j.is_object() || j.at("format") != "vltcreator" ||
        (j.at("version") != 1 && j.at("version") != 2))
      throw std::runtime_error("Unsupported Creator project");
    CreatorProject p;
    p.name = QString::fromStdString(j.at("name").get<std::string>());
    p.activeMode = QString::fromStdString(j.value("activeMode", std::string{}));
    p.codeNode=QString::fromStdString(j.value("codeNode",std::string{}));
    if(j.contains("codeCursors") && j.at("codeCursors").is_object() && j.at("codeCursors").size()<=576)
      for(const auto &[id,value]:j.at("codeCursors").items())
        if(value.is_number_integer() && id.size()<=300)
          p.codeCursors[QString::fromStdString(id)]=std::clamp(value.get<int>(),0,int(kMaxFunctionSourceBytes));
    p.definition = daw::plugins::mini::fromJson(j.at("definition"));
    if (p.name.size() > 256 || !p.definition.unavailableSource.empty() ||
        (p.definition.version != 3 && p.definition.version != 4))
      throw std::runtime_error("Invalid Creator project definition");
    if (!p.activeMode.isEmpty() &&
        std::none_of(
            p.definition.modes.begin(), p.definition.modes.end(),
            [&](const auto &m) { return m.id == p.activeMode.toStdString(); }))
      p.activeMode = QString::fromStdString(p.definition.defaultMode);
    if (j.contains("positions")) {
      if (!j.at("positions").is_object() || j.at("positions").size() > 9)
        throw std::runtime_error("Too many layouts");
      for (const auto &[mode, nodes] : j.at("positions").items()) {
        if (!nodes.is_object() || nodes.size() > kMaxNodes)
          throw std::runtime_error("Invalid node layout");
        for (const auto &[id, point] : nodes.items()) {
          const double x = point.at(0).get<double>(),
                       y = point.at(1).get<double>();
          if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e6 ||
              std::abs(y) > 1e6)
            throw std::runtime_error("Invalid node position");
          p.positions[QString::fromStdString(mode)]
                     [QString::fromStdString(id)] = {x, y};
        }
      }
    }
    if (j.contains("viewports"))
      for (const auto &[mode, view] : j.at("viewports").items()) {
        const double x = view.at("center").at(0).get<double>(),
                     y = view.at("center").at(1).get<double>(),
                     zoom = view.at("zoom").get<double>();
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(zoom))
          p.viewports[QString::fromStdString(mode)] = {
              {std::clamp(x, -1e6, 1e6), std::clamp(y, -1e6, 1e6)},
              std::clamp(zoom, .25, 2.5)};
      }
    result = std::move(p);
    error.clear();
    return true;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return false;
  }
}
bool CreatorProject::save(const QString &path, QString &error) const {
  const auto text = toJson().dump(2);
  if (text.size() > 32 * 1024 * 1024) {
    error = QStringLiteral("Creator project exceeds 32 MiB");
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(text.data(), qint64(text.size())) != qint64(text.size()) ||
      !file.commit()) {
    error = file.errorString();
    return false;
  }
  error.clear();
  return true;
}
bool CreatorProject::open(const QString &path, CreatorProject &p,
                          QString &error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) {
    error = QStringLiteral("Cannot read Creator project (maximum 32 MiB)");
    return false;
  }
  const auto bytes = file.readAll();
  return fromJson(json::parse(bytes.constData(),
                              bytes.constData() + bytes.size(), nullptr, false),
                  p, error);
}
} // namespace ui
