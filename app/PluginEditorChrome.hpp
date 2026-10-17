#pragma once
#include "Theme.hpp"

// Shared by the draggable frame and the plugin's host controls.
inline QColor pluginEditorChromeSurface(const Theme& theme) {
    return theme.headerBackground;
}
