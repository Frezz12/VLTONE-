#pragma once

#include <QString>

namespace ui::startupproject {

/// Project template used only for a normal application launch. An empty path
/// means that VLTONE starts with its regular blank project.
QString templatePath();
void setTemplatePath(const QString& path);

/// QSettings round-trip and default-value regression check.
bool checkPreferencesForTest(QString* error = nullptr);

} // namespace ui::startupproject
