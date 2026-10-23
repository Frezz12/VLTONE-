#pragma once
#include "Api.hpp"
class QWidget;

namespace ui::scriptstyle {
using Json = vlt::script::Json;
// All strings and numbers are validated before they can become local Qt styles.
void validateTheme(const Json &theme);
void validateDefinitions(const Json &styles);
Json resolve(const Json &style, const Json &styles);
void remember(QWidget *widget, const Json &resolved);
Json read(const QWidget *widget);
void applyWindow(QWidget *window, QWidget *content);
} // namespace ui::scriptstyle
