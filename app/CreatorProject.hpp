#pragma once
#include "Internal/MiniModuleDefinition.hpp"
#include <QMap>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <nlohmann/json_fwd.hpp>

namespace ui {
struct CreatorViewport {
  QPointF center{350, 220};
  double zoom = 1;
  bool operator==(const CreatorViewport &) const = default;
};
struct CreatorProject {
  QString name, activeMode;
  QStringList graphPath;
  daw::plugins::mini::MiniModuleDefinition definition;
  QMap<QString, QMap<QString, QPointF>> positions;
  QMap<QString, CreatorViewport> viewports;
  QString codeNode;
  QMap<QString, int> codeCursors;
  static CreatorProject create(const QString &projectName,
                               const QString &moduleName);
  daw::plugins::mini::MiniModuleDefinition graph() const;
  void setGraph(const daw::plugins::mini::MiniModuleDefinition &);
  QString layoutKey() const;
  QString pack(const QStringList &nodes, const QString &name, QString &error);
  bool unpack(const QString &node, QString &error);
  bool makeIndependent(const QString &node, QString &error);
  void syncControls();
  QString addMode(const QString &name);
  void removeMode(const QString &id);
  nlohmann::json toJson() const;
  static bool fromJson(const nlohmann::json &, CreatorProject &,
                       QString &error);
  bool save(const QString &path, QString &error) const;
  static bool open(const QString &path, CreatorProject &, QString &error);
  bool operator==(const CreatorProject &) const = default;
};
} // namespace ui
