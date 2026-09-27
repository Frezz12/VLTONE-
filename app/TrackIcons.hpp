#pragma once

#include "Icons.hpp"
#include <QColor>
#include <QIcon>
#include <QString>
#include <QVector>

namespace ui::trackicons {
struct Entry {
    QString id;
    QString name;
    QString category;
};

QVector<Entry> builtins();
QVector<Entry> customIcons();
/// Returns a null icon for unknown keys or missing custom files.
QIcon icon(const QString& id, const QColor& color);
QString customPath(const QString& id);
QString importFile(const QString& path, QString* error = nullptr);
bool removeCustom(const QString& id, QString* error = nullptr);
/// Theme packages restore the encoded bytes without recompressing them, so a
/// project's content-addressed icon key stays the same across computers.
bool restoreCustom(const QString& id, const QString& name, const QString& path,
                   QString* error = nullptr);
}
