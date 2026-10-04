#include "MiniModuleLibrary.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <nlohmann/json.hpp>

namespace ui {
QString MiniModuleLibrary::directory() {
  return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
         QStringLiteral("/VLTONE/MiniModules");
}
MiniModuleFile MiniModuleLibrary::read(const QString &path) {
  MiniModuleFile result;
  result.path = path;
  result.definition.name = QFileInfo(path).completeBaseName().toStdString();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) {
    result.error = QStringLiteral("Cannot read mini module (maximum 32 MiB)");
    return result;
  }
  const auto bytes = file.readAll();
  const auto json = nlohmann::json::parse(
      bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
  if (!json.is_object() || !json.contains("format") ||
      !json["format"].is_string() || json["format"] != "vltmini" ||
      !json.contains("version") ||
      (json["version"] != 1 && json["version"] != 2 && json["version"] != 3 && json["version"] != 4) ||
      !json.contains("definition")) {
    result.error = QStringLiteral("Unsupported .vltmini file");
    return result;
  }
  result.definition = daw::plugins::mini::fromJson(json["definition"]);
  result.error =
      QString::fromStdString(daw::plugins::mini::validate(result.definition));
  if (result.definition.name.empty())
    result.definition.name = QFileInfo(path).completeBaseName().toStdString();
  return result;
}
std::vector<MiniModuleFile> MiniModuleLibrary::entries(const QString &folder) {
  std::vector<MiniModuleFile> result;
  for (const auto &path : QDir(folder.isEmpty() ? directory() : folder)
                              .entryInfoList({QStringLiteral("*.vltmini")},
                                             QDir::Files, QDir::Name))
    result.push_back(read(path.absoluteFilePath()));
  return result;
}
bool MiniModuleLibrary::write(
    const QString &path,
    const daw::plugins::mini::MiniModuleDefinition &definition,
    QString &error) {
  error = QString::fromStdString(daw::plugins::mini::validate(definition));
  if (!error.isEmpty())
    return false;
  const auto bytes =
      nlohmann::json{{"format", "vltmini"},
                     {"version", 4},
                     {"definition", daw::plugins::mini::toJson(definition)}}
          .dump(2);
  QSaveFile file(path);
  if (bytes.size() > 32 * 1024 * 1024) {
    error = QStringLiteral("Mini module exceeds 32 MiB");
    return false;
  }
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) ||
      !file.commit()) {
    error = file.errorString();
    return false;
  }
  return true;
}
} // namespace ui
