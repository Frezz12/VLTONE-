#pragma once
#include "Controls.hpp"
#include "Internal/FluxInstance.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <functional>

namespace fluxui {
using namespace daw::plugins::flux;
inline const QColor accent{"#318b98"}, teal{"#318b98"}, gold{"#a48532"},
    purple{"#8970b5"}, olive{"#74846a"}, clay{"#c47553"}, ink{"#292d2e"},
    muted{"#626a69"}, border{"#dcded7"}, paper{"#f2f2ed"},
    panelWhite{"#fdfdfa"}, field{"#eeefe9"}, focus{"#247d89"};
QFont controlFont(int size = 13);
QColor color(Kind);
QString label(const char *en, const char *ru);
QString localize(const QString &);
void well(QPainter &, QRectF, QColor accent = {});
void text(QPainter &, QRectF, const QString &, int size = 12,
          QColor color = ink, int align = Qt::AlignCenter);
class Surface : public QWidget {
public:
  using QWidget::QWidget;
  std::function<void(QPainter &)> draw;

protected:
  void paintEvent(QPaintEvent *) override;
};
class Button : public QPushButton {
public:
  explicit Button(QWidget *parent = nullptr);
  Button(const QString &text, QWidget *parent = nullptr);

protected:
  void paintEvent(QPaintEvent *) override;
};
class ComboBox final : public QComboBox {
public:
  explicit ComboBox(QWidget *parent = nullptr);
  void showPopup() override;
  void hidePopup() override;

protected:
  bool event(QEvent *) override;
  void paintEvent(QPaintEvent *) override;
};
class Toggle final : public QCheckBox {
public:
  using QCheckBox::QCheckBox;
  QSize sizeHint() const override {
    const auto size = QCheckBox::sizeHint();
    return {size.width() + 18, size.height()};
  }

protected:
  void paintEvent(QPaintEvent *) override;
  bool hitButton(const QPoint &point) const override {
    return rect().contains(point);
  }
};
class NumberBox final : public QDoubleSpinBox {
public:
  using QDoubleSpinBox::QDoubleSpinBox;

protected:
  void paintEvent(QPaintEvent *) override;
};
class Knob final : public ui::Knob {
public:
  Knob(unsigned id, QString label, QColor accent, QWidget *parent);
  unsigned index;
  QString caption;
  QColor accent;
  double effective = 0;
  bool logarithmic = false, showEffective = false;
  std::function<void(unsigned, unsigned)> assign;
  std::function<void(unsigned)> learn;
  std::function<void(unsigned)> lock;
  bool locked = false;

protected:
  void paintEvent(QPaintEvent *) override;
  void dragEnterEvent(QDragEnterEvent *) override;
  void dropEvent(QDropEvent *) override;
  void contextMenuEvent(QContextMenuEvent *) override;

private:
  QPixmap face;
};
class SourceButton final : public Button {
public:
  using Button::Button;
  unsigned source = 0;

protected:
  void mouseMoveEvent(QMouseEvent *) override;
};
class CurveEditor final : public QWidget {
public:
  explicit CurveEditor(QWidget *parent);
  State *state = nullptr;
  FluxTelemetry *telemetry = nullptr;
  unsigned slot = 0, curve = 0, tool = 0, grid = 16;
  double gridMultiplier = 1;
  bool snap = true, showOther = true, waveInput = true, waveOutput = false,
       waveSidechain = false;
  std::function<void()> begin, changed, finish;
  void command(unsigned);
  void fit();

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void contextMenuEvent(QContextMenuEvent *) override;

private:
  Curve &data();
  QRectF area() const;
  QPointF toView(const Point &) const;
  Point fromView(QPointF, Qt::KeyboardModifiers) const;
  void drawAt(QPointF, Qt::KeyboardModifiers);
  QSet<int> selection;
  std::vector<Point> copied, original;
  double zoom = 1, offset = 0;
  QPointF last, down;
  int active = -1;
  bool dragging = false, panning = false, marquee = false;
};
class SequenceEditor final : public QWidget {
public:
  explicit SequenceEditor(QWidget *);
  State *state = nullptr;
  FluxTelemetry *telemetry = nullptr;
  unsigned selectedSlot = 0;
  bool compact = false;
  std::function<void()> begin, changed, finish;
  void copy(bool pattern = false);
  void paste(bool pattern = false);

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void contextMenuEvent(QContextMenuEvent *) override;

private:
  std::vector<unsigned> rows() const;
  void edit(QPointF, bool first);
  QPoint cellAt(QPointF) const;
  int focusedRow = 0, focusedStep = 0, anchorRow = 0, anchorStep = 0;
  bool dragging = false, paintOn = true, selecting = false,
       hasSelection = false;
};
class Pad final : public QWidget {
public:
  explicit Pad(QWidget *);
  State *state = nullptr;
  FluxTelemetry *telemetry = nullptr;
  std::function<void()> begin, changed, finish;

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void keyPressEvent(QKeyEvent *) override;

private:
  void edit(QPointF);
  bool dragging = false;
};
class RoutingEditor final : public QWidget {
public:
  explicit RoutingEditor(QWidget *);
  State *state = nullptr;
  unsigned selected = 0;
  std::function<void()> begin, changed, finish;
  std::function<void(unsigned)> select;

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void dragEnterEvent(QDragEnterEvent *) override;
  void dropEvent(QDropEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void keyPressEvent(QKeyEvent *) override;

private:
  QPoint press;
  int dragged = -1;
  double horizontal = 0;
  QRectF card(unsigned) const;
};
class BandEditor final : public QWidget {
public:
  explicit BandEditor(QWidget *);
  State *state = nullptr;
  std::function<void()> begin, changed, finish;
  std::function<void(unsigned)> automate;

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void contextMenuEvent(QContextMenuEvent *) override;

private:
  QRectF area() const;
  double position(double) const;
  void move(double);
  unsigned selected = 0;
  bool dragging = false;
};
} // namespace fluxui
