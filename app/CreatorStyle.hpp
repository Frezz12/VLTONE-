#pragma once

#include "Controls.hpp"
#include "Icons.hpp"
#include <QDoubleSpinBox>
#include <string_view>

class QAction;

namespace ui {
// Canvas and proxied controls use the active application theme, including
// custom palettes. Semantic port colours remain distinguishable in both modes.
struct CreatorColors {
  QColor canvas, panel, card, field, hover, border, grid;
  QColor text, muted, accent, accentFill, onAccent;
  QColor audio, number, gate, function;
  QColor integer, array, list, buffer;
};
CreatorColors creatorColors();
void styleCreator(QWidget *widget);
void creatorIcon(QAction *action, icons::Glyph glyph, bool primary = false);
icons::Glyph creatorCategoryIcon(std::string_view category);
int creatorCategoryOrder(std::string_view category);

class CreatorNumberField final : public QDoubleSpinBox {
  Q_OBJECT
public:
  explicit CreatorNumberField(QWidget *parent = nullptr);
  ~CreatorNumberField() override;
  void setLogarithmic(bool on) { m_logarithmic = on; }
  void setDefaultValue(double value) { m_default = value; }

protected:
  bool event(QEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;

private:
  bool input(QEvent *);
  void beginText();
  void finish();
  void endInteraction();
  void scrub(QMouseEvent *);
  double m_start = 0, m_scrub = 0, m_default = 0;
  qreal m_pending = 0.0;
  bool m_pressed = false, m_dragged = false, m_logarithmic = false;
  LockedCursorDrag m_cursor;
};
} // namespace ui
