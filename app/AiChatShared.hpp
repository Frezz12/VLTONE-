#pragma once
#include "AiPrefs.hpp"
#include "Icons.hpp"
#include "LlmClient.hpp"
#include <QLabel>
#include <QToolButton>
namespace ui {
LlmConfig aiModelConfig(const aiprefs::ModelConnection &);
QToolButton *aiMessageAction(QWidget *, icons::Glyph, const QString &);
QLabel *aiCardText(QWidget *, const QString &, const char *objectName,
                   bool secondary = false);
} // namespace ui
