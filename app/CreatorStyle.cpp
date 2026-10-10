#include "CreatorStyle.hpp"
#include "Theme.hpp"
#include <QAction>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace ui {
CreatorColors creatorColors() {
  const auto &t = th();
  return {t.background, t.surface, t.surfaceElevated, t.well(),
          mixColors(t.surfaceElevated, t.textPrimary, .08), t.separator(), t.gridLine,
          t.textPrimary, t.textSecondary, t.accent, t.accent, t.accentText(),
          QColor(t.dark ? "#63d6b6" : "#087c64"), QColor(t.dark ? "#8abaff" : "#2869bb"),
          QColor(t.dark ? "#d7a1ef" : "#8843ad"), QColor(t.dark ? "#efc078" : "#996210"),
          QColor(t.dark ? "#baca74" : "#596816"), QColor(t.dark ? "#e6a878" : "#9a531f"),
          QColor(t.dark ? "#ec95b0" : "#ab3860"), QColor(t.dark ? "#79ced7" : "#177883")};
}

void styleCreator(QWidget *widget) {
  const auto c = creatorColors();
  auto palette = widget->palette();
  palette.setColor(QPalette::Window, c.panel);
  palette.setColor(QPalette::Base, c.field);
  palette.setColor(QPalette::AlternateBase, c.card);
  palette.setColor(QPalette::Text, c.text);
  palette.setColor(QPalette::WindowText, c.text);
  palette.setColor(QPalette::Button, c.card);
  palette.setColor(QPalette::ButtonText, c.text);
  palette.setColor(QPalette::Highlight, c.accentFill);
  palette.setColor(QPalette::HighlightedText, c.onAccent);
  palette.setColor(QPalette::PlaceholderText, c.muted);
  palette.setColor(QPalette::Disabled, QPalette::Text, c.muted);
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
  widget->setPalette(palette);
  // Token replacement keeps the QSS readable and independent of DAW chrome.
  QString css = QStringLiteral(R"(
    QWidget { color: $text; font-size: 12px; }
    QMainWindow, QDialog, QScrollArea, QStatusBar, QWidget#CreatorPropertiesBody { background: $panel; }
    QLabel { background: transparent; border: none; padding: 0; }
    QLabel[creatorRole="muted"] { color: $muted; }
    QLabel[creatorRole="heading"] { font-size: 14px; font-weight: 600; }
    QLabel[creatorRole="brand"] { font-size: 16px; font-weight: 600; padding: 0 10px 0 4px; }
    QLabel[creatorRole="section"] { color: $muted; font-size: 11px; font-weight: 600; padding: 8px 0; }
    QWidget#CreatorAiPanel QToolButton { background: transparent; border: 1px solid transparent; text-align: left; padding: 3px 5px; }
    QWidget#CreatorAiPanel QToolButton:hover { background: $hover; }
    QWidget#CreatorAiPanel QToolButton:focus { border-color: $accent; }
    QWidget#CreatorAiPanel QScrollArea, QWidget#CreatorAiPanel QScrollArea > QWidget > QWidget { background: $panel; }
    QWidget#CreatorAiPanel QPlainTextEdit { background: $field; border: 1px solid $border; border-radius: 7px; padding: 6px; }
    QWidget#CreatorAiPanel QPlainTextEdit:focus { border-color: $accent; }
    QPushButton#CreatorAiRestore { background: transparent; color: $muted; }
    QToolBar { background: $panel; border: none; border-bottom: 1px solid $border; padding: 5px; spacing: 4px; }
    QToolBar::separator { background: $border; width: 1px; margin: 8px 7px; }
    QToolButton, QPushButton { background: $card; border: 1px solid $border; border-radius: 5px; padding: 4px 8px; min-height: 18px; }
    QToolButton { padding: 5px; }
    QToolButton:hover, QPushButton:hover { background: $hover; border-color: $muted; }
    QToolButton:pressed, QPushButton:pressed { background: $field; border-color: $accent; }
    QToolButton:disabled, QPushButton:disabled { color: $muted; background: $panel; border-color: $border; }
    QToolButton[creatorPrimary="true"], QPushButton[creatorPrimary="true"] { color: $onAccent; background: $accentFill; border-color: $accentFill; padding-left: 13px; padding-right: 13px; font-weight: 600; }
    QToolButton[creatorPrimary="true"]:hover { border-color: $accent; }
    QToolButton[creatorPrimary="true"]:disabled { background: $hover; color: $muted; border-color: $border; }
    QToolBar#CreatorToolbar { padding: 7px 10px; spacing: 5px; }
    QToolBar#CreatorToolbar::separator { background: $border; width: 1px; margin: 8px 10px; }
    QToolBar#CreatorToolbar QToolButton { background: transparent; border: 1px solid transparent; padding: 5px; min-width: 20px; min-height: 20px; }
    QToolBar#CreatorToolbar QToolButton:hover { background: $hover; border-color: transparent; }
    QToolBar#CreatorToolbar QToolButton:pressed { background: $field; border-color: transparent; }
    QToolBar#CreatorToolbar QToolButton:disabled { background: transparent; border-color: transparent; }
    QToolBar#CreatorToolbar QToolButton[creatorPrimary="true"] { color: $onAccent; background: $accentFill; padding-left: 13px; padding-right: 13px; font-weight: 600; }
    QToolBar#CreatorToolbar QToolButton[creatorPrimary="true"]:disabled { background: $hover; color: $muted; }
    QLineEdit, QAbstractSpinBox, QComboBox { background: $field; color: $text; border: 1px solid $border; border-radius: 4px; padding: 3px 6px; min-height: 16px; selection-background-color: $accentFill; selection-color: $onAccent; }
    QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled { color: $muted; border-color: transparent; background: $panel; }
    QAbstractSpinBox QLineEdit { border: none; padding: 0; background: transparent; min-height: 0; }
    QLineEdit QToolButton { background: transparent; border: none; border-radius: 0; padding: 0; min-height: 0; min-width: 0; }
    QLineEdit QToolButton:hover { background: transparent; }
    QComboBox { padding-right: 24px; }
    QComboBox::drop-down { width: 20px; border: none; background: transparent; }
    QComboBox::down-arrow { image: url(:/icons/popup-chevron-$appearance.svg); width: 12px; height: 12px; border: none; }
    QComboBox QAbstractItemView { padding: 4px; border: 1px solid $border; background: $card; selection-background-color: $hover; selection-color: $text; }
    QListWidget, QTreeWidget { background: $panel; border: none; outline: none; padding: 0; }
    QListWidget::item { padding: 4px 6px; border: 1px solid transparent; border-radius: 4px; }
    QListWidget::item:hover { background: $hover; }
    QListWidget::item:selected { background: $hover; color: $text; border-color: $accent; }
    QListWidget::item:disabled { background: transparent; color: $muted; padding-top: 14px; padding-bottom: 5px; }
    QTreeWidget#CreatorNodeLibrary::item { padding: 3px 5px; border: 1px solid transparent; border-radius: 4px; }
    QTreeWidget#CreatorNodeLibrary::item:hover { background: $hover; }
    QTreeWidget#CreatorNodeLibrary::item:selected { background: $hover; color: $text; border-color: $accent; }
    QTreeWidget#CreatorNodeLibrary::branch { background: transparent; }
    QTableWidget { background: $field; alternate-background-color: $card; gridline-color: $border; border: 1px solid $border; selection-background-color: $hover; selection-color: $text; }
    QHeaderView::section { background: $panel; color: $muted; border: none; border-bottom: 1px solid $border; padding: 7px; }
    QPlainTextEdit { background: $field; color: $text; border: none; selection-background-color: $accentFill; selection-color: $onAccent; }
    QPlainTextEdit#CreatorCppSource { font-family: "Cascadia Code", "Consolas", "Menlo", monospace; font-size: 13px; }
    QSplitter::handle { background: $border; }
    QSplitter::handle:hover { background: $accent; }
    QStatusBar { border-top: 1px solid $border; padding: 3px 10px; }
    QStatusBar::item { border: none; }
    QStatusBar QLabel { color: $muted; }
    QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
    QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
    QScrollBar::handle { background: $border; border-radius: 3px; min-height: 24px; min-width: 24px; }
    QScrollBar::handle:hover { background: $muted; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; }
    QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    QTabWidget::pane { background: $panel; border: none; border-top: 1px solid $border; }
    QTabBar::tab { background: transparent; color: $muted; border: none; border-bottom: 2px solid transparent; padding: 9px 12px; }
    QTabBar::tab:selected { color: $accent; border-bottom-color: $accent; }
    QTabBar::tab:hover { background: $hover; }
    QGroupBox { background: transparent; border: none; margin-top: 22px; padding: 0; }
    QGroupBox::title { color: $muted; subcontrol-origin: margin; subcontrol-position: top left; }
    QCheckBox { background: transparent; spacing: 8px; padding: 4px 0; }
    QMenu { background: $card; color: $text; border: 1px solid $border; border-radius: 7px; padding: 6px; }
    QMenu::item { padding: 7px 30px 7px 26px; border-radius: 4px; }
    QMenu::item:selected { background: $hover; color: $text; }
    QMenu::item:disabled { color: $muted; }
    QMenu::separator { height: 1px; background: $border; margin: 5px 6px; }
    QToolTip { background: $card; color: $text; border: 1px solid $border; padding: 6px; }
  )");
  css.replace("$appearance", th().dark ? "dark" : "light");
  const std::pair<const char *, QColor> tokens[] = {
      {"onAccent", c.onAccent}, {"accentFill", c.accentFill},
      {"accent", c.accent}, {"text", c.text}, {"muted", c.muted},
      {"panel", c.panel}, {"card", c.card}, {"field", c.field},
      {"hover", c.hover}, {"border", c.border}};
  for (const auto &[name, color] : tokens)
    css.replace("$" + QString::fromLatin1(name), color.name());
  widget->setStyleSheet(css);
}

void creatorIcon(QAction *action, icons::Glyph glyph, bool primary) {
  const auto update = [action, glyph, primary] {
    const auto c = creatorColors();
    action->setIcon(icons::icon(glyph, primary ? c.onAccent : c.text));
  };
  update();
  QObject::connect(&ThemeManager::instance(), &ThemeManager::changed, action, update);
}

icons::Glyph creatorCategoryIcon(std::string_view category) {
  if (category == "Routing") return icons::Glyph::Send;
  if (category == "Effects") return icons::Glyph::Plugin;
  if (category == "Generators") return icons::Glyph::Synth;
  if (category == "Modulation") return icons::Glyph::Automation;
  if (category == "Math") return icons::Glyph::Plus;
  if (category == "Logic") return icons::Glyph::Layers;
  if (category == "Analysis") return icons::Glyph::Waveform;
  return icons::Glyph::Edit;
}

int creatorCategoryOrder(std::string_view category) {
  constexpr std::string_view categories[] = {"Routing", "Signal", "Math", "Levels", "Logic",
      "Memory", "Time", "Filters", "Shaping", "Collections", "Custom", "Effects", "Generators",
      "Modulation", "Analysis", "Code"};
  const auto it = std::find(std::begin(categories), std::end(categories), category);
  return int(it - std::begin(categories));
}

CreatorNumberField::CreatorNumberField(QWidget *parent) : QDoubleSpinBox(parent) {
  setProperty("vlt.customNumericScrub", true);
  setButtonSymbols(NoButtons);
  setKeyboardTracking(false);
  setAlignment(Qt::AlignRight);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  lineEdit()->setMouseTracking(true);
  lineEdit()->installEventFilter(this);
  setAccessibleDescription(tr("Drag up or down to adjust. Shift for fine control. "
                             "Double-click or press Enter to type. Escape cancels."));
  endInteraction();
  connect(this, &QDoubleSpinBox::editingFinished, this, [this] { endInteraction(); });
}
CreatorNumberField::~CreatorNumberField() { m_cursor.cancel(); }
void CreatorNumberField::endInteraction() {
  m_pressed = false;
  if (m_dragged) m_cursor.finish(QCursor::pos());
  else m_cursor.cancel();
  m_dragged = false;
  if (QWidget::mouseGrabber() == this) releaseMouse();
  setReadOnly(true);
  setCursor(Qt::SizeVerCursor);
  lineEdit()->setCursor(Qt::SizeVerCursor);
}
void CreatorNumberField::finish() {
  if (!m_pressed && isReadOnly()) return;
  if (!isReadOnly()) interpretText();
  endInteraction();
  emit editingFinished();
}
void CreatorNumberField::beginText() {
  m_start = value();
  endInteraction();
  setReadOnly(false);
  setCursor(Qt::IBeamCursor);
  lineEdit()->setCursor(Qt::IBeamCursor);
  setFocus(Qt::OtherFocusReason);
  selectAll();
}
void CreatorNumberField::scrub(QMouseEvent *event) {
  const qreal dy = -m_cursor.takeDelta(event->globalPosition()).y();
  if (!m_dragged) {
    m_pending += dy;
    if (std::abs(m_pending) < 3) return;
    m_dragged = true;
    setCursor(Qt::BlankCursor);
    lineEdit()->setCursor(Qt::BlankCursor);
  }
  const double pixels = m_pending ? std::exchange(m_pending, 0.0) : dy;
  const double speed = event->modifiers().testFlag(Qt::ShiftModifier) ? .025 : .25;
  if (m_logarithmic && minimum() > 0 && maximum() > minimum())
    m_scrub *= std::exp(pixels * speed * std::log(maximum() / minimum()) / 100.);
  else
    m_scrub += pixels * speed * singleStep();
  m_scrub = std::clamp(m_scrub, minimum(), maximum());
  setValue(m_scrub);
}
bool CreatorNumberField::event(QEvent *event) {
  if (input(event)) return true;
  if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
      (event->type() == QEvent::UngrabMouse && m_pressed)) finish();
  return QDoubleSpinBox::event(event);
}
bool CreatorNumberField::eventFilter(QObject *watched, QEvent *event) {
  if (watched == lineEdit() && input(event)) return true;
  return QDoubleSpinBox::eventFilter(watched, event);
}
bool CreatorNumberField::input(QEvent *event) {
  if (!isEnabled()) return false;
  if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    const bool numeric = !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
        !key->text().isEmpty() && (key->text().front().isDigit() ||
        QStringLiteral("-+.,").contains(key->text().front()));
    const bool enter = key->key() == Qt::Key_Enter || key->key() == Qt::Key_Return;
    if (event->type() == QEvent::ShortcutOverride) {
      if (enter || numeric || key->key() == Qt::Key_F2 ||
          key->key() == Qt::Key_Up || key->key() == Qt::Key_Down ||
          (key->key() == Qt::Key_Escape && (m_pressed || !isReadOnly()))) {
        event->accept(); return true;
      }
    } else {
      if (key->key() == Qt::Key_Escape && (m_pressed || !isReadOnly())) {
        setValue(m_start); endInteraction(); event->accept(); return true;
      }
      if (enter || key->key() == Qt::Key_F2) {
        if (isReadOnly()) beginText(); else finish();
        event->accept(); return true;
      }
      if (isReadOnly() && numeric) beginText();
      if (isReadOnly() && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
        const double step = singleStep() * (key->modifiers().testFlag(Qt::ShiftModifier) ? .1 : 1);
        setValue(value() + (key->key() == Qt::Key_Up ? step : -step));
        emit editingFinished(); event->accept(); return true;
      }
    }
  }
  if (event->type() == QEvent::Wheel && isReadOnly()) {
    auto *wheel = static_cast<QWheelEvent *>(event);
    if (!hasFocus()) { event->ignore(); return true; }
    const double steps = wheel->angleDelta().y() / 120.;
    setValue(value() + steps * singleStep() * (wheel->modifiers().testFlag(Qt::ShiftModifier) ? .1 : 1));
    emit editingFinished(); event->accept(); return true;
  }
  if (event->type() == QEvent::MouseButtonDblClick) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() == Qt::LeftButton) { beginText(); event->accept(); return true; }
  }
  if (event->type() == QEvent::MouseButtonPress && isReadOnly()) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton) return false;
    setFocus(Qt::MouseFocusReason);
    m_start = m_scrub = value();
    if (mouse->modifiers().testFlag(Qt::ControlModifier)) {
      setValue(m_default); emit editingFinished(); event->accept(); return true;
    }
    m_pending = 0; m_pressed = true; m_dragged = false;
    m_cursor.begin(mouse->globalPosition());
    grabMouse(); event->accept(); return true;
  }
  if (event->type() == QEvent::MouseMove && m_pressed) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->buttons().testFlag(Qt::LeftButton)) scrub(mouse); else finish();
    event->accept(); return true;
  }
  if (event->type() == QEvent::MouseButtonRelease && m_pressed) {
    // The last move already applied the delta; pointer warping can make the
    // release carry the pre-warp position. Never count it a second time.
    finish(); event->accept(); return true;
  }
  return false;
}
} // namespace ui
