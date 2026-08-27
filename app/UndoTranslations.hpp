#pragma once

#include <QString>

#include <string>

namespace ui {

/// Translate stable engine undo identifiers only at the display boundary.
QString translatedUndoLabel(const std::string& label);

} // namespace ui
