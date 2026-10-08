#include "AiChatShared.hpp"
#include "AccountService.hpp"
#include "Theme.hpp"
namespace ui {
LlmConfig aiModelConfig(const aiprefs::ModelConnection &c) {
  LlmConfig config;
  config.connectionId = c.id;
  config.displayName = c.displayName;
  config.model = c.model;
  config.stream = aiprefs::streaming();
  config.timeoutSeconds = aiprefs::timeoutSeconds();
  config.maxRetries = aiprefs::maxRetries();
  if (c.source == aiprefs::ModelSource::Managed) {
    config.transport = LlmConfig::Transport::Managed;
    if (auto *a = account::Service::instance())
      config.accessToken = a->accessToken();
  } else {
    config.transport = LlmConfig::Transport::Direct;
    config.endpoint = c.endpoint;
    config.apiKey = aiprefs::customApiKey(c.id);
  }
  return config;
}
QToolButton *aiMessageAction(QWidget *parent, icons::Glyph glyph,
                             const QString &label) {
  auto *b = new QToolButton(parent);
  b->setObjectName("AiMessageAction");
  b->setProperty("aiGlyph", int(glyph));
  b->setIcon(icons::icon(glyph, th().textSecondary, 16));
  b->setIconSize({16, 16});
  b->setToolButtonStyle(Qt::ToolButtonIconOnly);
  b->setFixedSize(28, 28);
  b->setAutoRaise(true);
  b->setCursor(Qt::PointingHandCursor);
  b->setFocusPolicy(Qt::StrongFocus);
  b->setAccessibleName(label);
  b->setToolTip(label);
  return b;
}
QLabel *aiCardText(QWidget *parent, const QString &text, const char *name,
                   bool secondary) {
  auto *label = new QLabel(text, parent);
  label->setObjectName(name);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::TextSelectableByKeyboard);
  label->setFocusPolicy(Qt::ClickFocus);
  auto policy = label->sizePolicy();
  policy.setHorizontalPolicy(QSizePolicy::Ignored);
  label->setSizePolicy(policy);
  if (secondary)
    label->setAccessibleDescription(QObject::tr("Secondary message text"));
  return label;
}
} // namespace ui
