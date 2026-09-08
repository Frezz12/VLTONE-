#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace ui::projecttemplates {

struct AudioTarget {
    QString trackId;
    QString displayPath;
};

inline constexpr const char* kExtension = "vltt";

/// The application-managed `Presets/Templates` folder. Calling this creates it.
QString folder();

/// Saved project template packages, ordered by display name.
QStringList files();
QString displayName(const QString& packagePath);

/// Resolve a user-entered name inside the managed folder. Empty means the name
/// is not portable across the desktop platforms supported by the application.
QString filePathForName(const QString& name);

/// Extension check used by dialogs, the browser and drop targets. A template
/// package is a directory on disk, but acts as one document everywhere in UI.
bool isTemplatePackage(const QString& path);

/// Read only the template document and return lanes that can own audio clips.
/// Plugin instances and their state are deliberately never constructed here.
QVector<AudioTarget> audioTargets(const QString& packagePath,
                                  QString* error = nullptr);

} // namespace ui::projecttemplates
