#pragma once
#include "Api.hpp"
#include <QList>
#include <QString>

namespace ui {
struct ScriptPackage {
    QString directory;
    vlt::script::Json manifest;
    vlt::script::Json sources;
    vlt::script::Json resources = vlt::script::Json::object();
    QString id() const;
    QString name() const;
};
class ScriptLibrary {
  public:
    static QString directory();
    static QString workerPath();
    static vlt::script::Json manifestForLua(const QString &name, const QString &id = {});
    static bool readDirectory(const QString &, ScriptPackage &, QString &error);
    static bool checkSource(const QString &source, const QString &name, QString &error);
    static bool validate(const ScriptPackage &, QString &error, bool syntax = true);
    static bool read(const QString &path, ScriptPackage &, QString &error);
    static bool readCreatorDocument(const QString &path, vlt::script::Json &, QString &error);
    static bool install(const QString &path, ScriptPackage &, QString &error,
                        const QString &expectedId = {});
    static bool install(const ScriptPackage &, QString &error, bool checkSyntax = true);
    static bool exportPackage(const ScriptPackage &, const QString &path, QString &error);
    static QList<ScriptPackage> installed();
    static bool remove(const QString &id, QString &error);
};
} // namespace ui
