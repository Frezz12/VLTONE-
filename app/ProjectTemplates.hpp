#pragma once

#include <QString>
#include <QStringList>

namespace ui::projecttemplates {

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

} // namespace ui::projecttemplates
