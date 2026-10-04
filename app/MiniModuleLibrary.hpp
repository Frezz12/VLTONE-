#pragma once
#include "Internal/MiniModuleDefinition.hpp"
#include <QString>
#include <vector>

namespace ui {
struct MiniModuleFile {
  daw::plugins::mini::MiniModuleDefinition definition;
  QString path;
  QString error;
};
/// Files contain declarative audio graphs only. Projects embed their own copy.
class MiniModuleLibrary {
public:
  static QString directory();
  static std::vector<MiniModuleFile> entries(const QString &folder = {});
  static MiniModuleFile read(const QString &path);
  static bool write(const QString &path,
                    const daw::plugins::mini::MiniModuleDefinition &,
                    QString &error);
};
} // namespace ui
