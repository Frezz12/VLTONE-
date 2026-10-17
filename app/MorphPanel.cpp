#include "MorphPanel.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Internal/MorphInstance.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStaticText>
#include <QStyledItemDelegate>
#include <QStyleOptionSpinBox>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>

namespace {
using namespace daw::plugins::morph;
const std::array<QColor, 4> accents{QColor("#318b98"), QColor("#8970b5"),
                                  QColor("#c47553"), QColor("#a48532")};
const QColor ink("#292d2e");
const QColor muted("#626a69");
const QColor paper("#f2f2ed");
const QColor panelWhite("#fdfdfa");
const QColor lineColor("#dcded7");
const QColor fieldColor("#eeefe9");
const QColor focusColor("#247d89");
const QPointF padCenter(158, 106);
constexpr double padRadius = 78;
constexpr auto sourceMime = "application/x-vlt-morph-source";
QString englishText(const QString &source) { return source; }
QFont morphFont(int size = 13) {
  // A stable local face, independent of the DAW's theme and language.
  static const QFont base = [] {
    QFont font;
    font.setFamilies({"Segoe UI", "Helvetica Neue", "Noto Sans"});
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
  }();
  QFont font = base;
  font.setPixelSize(size);
  return font;
}
class MorphMenu final : public QMenu {
public:
  explicit MorphMenu(QWidget *parent = nullptr) : QMenu(parent) {
    setFont(morphFont());
    setStyleSheet(
        "QMenu {background:#fdfdfa;color:#292d2e;border:1px solid #dcded7;"
        "border-radius:10px;padding:7px;}"
        "QMenu::item {padding:8px 28px 8px 16px;margin:2px;border-radius:6px;}"
        "QMenu::item:selected {background:#e4eeeb;color:#205e67;}"
        "QMenu::item:disabled {color:#8a908a;}"
        "QMenu::separator {height:1px;background:#dcded7;margin:6px 10px;}");
  }
};
QString valueText(unsigned id, double value) {
  static MorphInstance formatter;
  return QString::fromStdString(formatter.parameterText(id, value));
}
void text(QPainter &p, QRectF r, const QString &s, int size, QColor color = ink,
          double tracking = 0, Qt::Alignment alignment = Qt::AlignCenter) {
  QFont f = morphFont(size);
  f.setWeight(size >= 20 ? QFont::DemiBold : QFont::Medium);
  f.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
  const QString label = englishText(s);
  while (QFontMetricsF(f).horizontalAdvance(label) > r.width() &&
         f.pixelSize() > size - 2)
    f.setPixelSize(f.pixelSize() - 1);
  p.setFont(f);
  p.setPen(color);
  p.drawText(r, alignment, label);
}
QPointF polar(QPointF c, double radius, double degrees) {
  const double a = degrees * std::numbers::pi / 180;
  return c + QPointF(std::cos(a) * radius, std::sin(a) * radius);
}
void well(QPainter &p, QRectF r, QColor accent = {}, double = -1) {
  p.save();
  p.setBrush(panelWhite);
  p.setPen(QPen(lineColor, .8));
  p.drawRoundedRect(r, 18, 18);
  if (accent.isValid()) {
    QColor tint = accent;
    tint.setAlpha(7);
    p.setBrush(tint);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 17, 17);
  }
  p.restore();
}
void controlSurface(QPainter &p, QRectF r, QColor accent, bool pressed,
                    bool selected, bool hovered) {
  QColor fill = pressed ? QColor("#dfe3dc")
                       : hovered ? QColor("#e6e9e1") : fieldColor;
  if (selected) {
    fill = accent;
    fill.setAlpha(pressed ? 40 : 23);
  }
  p.setBrush(fill);
  p.setPen(QPen(selected ? accent : hovered ? QColor("#aeb6ac") : lineColor,
                selected ? 1.1 : .8));
  const double radius = std::min(10., r.height() * .28);
  p.drawRoundedRect(r, radius, radius);
}
enum class Glyph {
  None,
  Power,
  Settings,
  Menu,
  Previous,
  Next,
  Link,
  Assign,
  Heart,
  Compare,
  Flow,
  Image,
  Import,
  Brush,
  Route,
  Variation,
  Scan,
  Record,
  Motion,
  Clip,
  Gain,
  Sine,
  Envelope,
  Matrix,
  Effects,
  Dice,
  Reverse,
  Lock,
  Smooth,
  Clear,
  More,
  Balance
};
Glyph glyphFor(const QString &caption) {
  const std::pair<const char *, Glyph> entries[] = {
      {"⏻", Glyph::Power},
      {"⚙", Glyph::Settings},
      {"≡", Glyph::Menu},
      {"‹", Glyph::Previous},
      {"›", Glyph::Next},
      {"↔", Glyph::Link},
      {"↗", Glyph::Assign},
      {"♡", Glyph::Heart},
      {"A / B", Glyph::Compare},
      {"FLOW", Glyph::Flow},
      {"CANVAS", Glyph::Image},
      {"Import", Glyph::Import},
      {"Brush", Glyph::Brush},
      {"Route", Glyph::Route},
      {"Variation", Glyph::Variation},
      {"Scan", Glyph::Scan},
      {"● Record", Glyph::Record},
      {"Motion", Glyph::Motion},
      {"SOFT CLIP", Glyph::Clip},
      {"AUTO GAIN", Glyph::Gain},
      {"LFO 1", Glyph::Sine},
      {"LFO 2", Glyph::Sine},
      {"ENV FOLLOWER", Glyph::Envelope},
      {"1–4 / 16", Glyph::Matrix},
      {"FX +", Glyph::Effects},
      {"Random", Glyph::Dice},
      {"PING PONG", Glyph::Reverse},
      {"REVERSE", Glyph::Reverse},
      {"Generation locks…", Glyph::Lock},
      {"Smooth route", Glyph::Smooth},
      {"Clear Canvas", Glyph::Clear},
      {"…", Glyph::More},
      {"MOD MATRIX", Glyph::Matrix},
      {"MODE MIX", Glyph::Balance}};
  for (const auto &[name, glyph] : entries)
    if (caption == QString::fromUtf8(name) || caption == englishText(name))
      return glyph;
  return Glyph::None;
}
void drawGlyph(QPainter &p, Glyph icon, bool selected) {
  const auto line = [&](double x, double y, double a, double b) {
    p.drawLine(QPointF(x, y), QPointF(a, b));
  };
  const auto path = [&](std::initializer_list<QPointF> points) {
    QPainterPath shape;
    bool first = true;
    for (auto pt : points) {
      if (first)
        shape.moveTo(pt);
      else
        shape.lineTo(pt);
      first = false;
    }
    p.drawPath(shape);
  };
  switch (icon) {
  case Glyph::Power:
    p.drawArc(QRectF(8, 8, 16, 16), 45 * 16, -270 * 16);
    line(16, 5, 16, 15);
    break;
  case Glyph::Settings:
    p.drawEllipse(QPointF(16, 16), 6.5, 6.5);
    p.drawEllipse(QPointF(16, 16), 2.2, 2.2);
    for (int i = 0; i < 8; ++i)
      p.drawLine(polar({16, 16}, 7, i * 45), polar({16, 16}, 10, i * 45));
    break;
  case Glyph::Menu:
    for (double y : {9., 16., 23.})
      line(7, y, 25, y);
    break;
  case Glyph::Previous:
    path({{19, 8}, {11, 16}, {19, 24}});
    break;
  case Glyph::Next:
    path({{13, 8}, {21, 16}, {13, 24}});
    break;
  case Glyph::Link:
    p.save();
    p.translate(16, 16);
    p.rotate(-42);
    p.drawRoundedRect(QRectF(-10, -4, 13, 8), 4, 4);
    p.drawRoundedRect(QRectF(-3, -4, 13, 8), 4, 4);
    p.restore();
    break;
  case Glyph::Assign:
    line(8, 24, 24, 8);
    path({{13, 8}, {24, 8}, {24, 19}});
    break;
  case Glyph::Heart: {
    QPainterPath h;
    h.moveTo(16, 25);
    h.cubicTo(3, 17, 5, 6, 12, 8);
    h.cubicTo(14, 8, 15, 10, 16, 11);
    h.cubicTo(22, 1, 34, 11, 16, 25);
    if (selected)
      p.setBrush(p.pen().color());
    p.drawPath(h);
    break;
  }
  case Glyph::Compare:
    p.drawRoundedRect(QRectF(6, 7, 20, 18), 3, 3);
    line(16, 7, 16, 25);
    p.fillRect(QRectF(selected ? 18 : 8, 10, 6, 12), p.pen().color());
    break;
  case Glyph::Flow:
  case Glyph::Sine:
  case Glyph::Envelope:
  case Glyph::Smooth: {
    for (int n = 0; n < (icon == Glyph::Flow ? 3 : 1); ++n) {
      QPainterPath wave;
      wave.moveTo(4, 16 + n * 5 - (icon == Glyph::Flow ? 5 : 0));
      wave.cubicTo(10, 2 + n * 5, 20, 30 - n * 2, 28, 12 + n * 5);
      p.drawPath(wave);
    }
    break;
  }
  case Glyph::Image:
    p.drawRoundedRect(QRectF(5, 6, 22, 20), 3, 3);
    p.drawEllipse(QPointF(21, 12), 2, 2);
    path({{6, 23}, {13, 15}, {19, 22}, {23, 18}, {27, 23}});
    break;
  case Glyph::Import:
    path({{5, 19}, {5, 26}, {27, 26}, {27, 19}});
    line(16, 5, 16, 20);
    path({{10, 14}, {16, 20}, {22, 14}});
    break;
  case Glyph::Brush:
    path({{12, 20}, {23, 5}, {27, 9}, {16, 23}, {12, 20}});
    path({{12, 20}, {7, 21}, {5, 27}, {12, 26}, {16, 23}});
    break;
  case Glyph::Route:
    path({{7, 24}, {11, 10}, {25, 7}, {23, 23}});
    for (auto pt :
         {QPointF(7, 24), QPointF(11, 10), QPointF(25, 7), QPointF(23, 23)}) {
      p.setBrush(QColor("#f7f9fa"));
      p.drawEllipse(pt, 2.5, 2.5);
    }
    break;
  case Glyph::Variation:
    line(7, 25, 22, 10);
    path({{19, 7}, {25, 13}});
    for (auto pt : {QPointF(7, 8), QPointF(24, 23)}) {
      p.drawLine(pt - QPointF(3, 0), pt + QPointF(3, 0));
      p.drawLine(pt - QPointF(0, 3), pt + QPointF(0, 3));
    }
    break;
  case Glyph::Scan:
    path({{5, 12}, {5, 5}, {12, 5}});
    path({{20, 5}, {27, 5}, {27, 12}});
    path({{5, 20}, {5, 27}, {12, 27}});
    path({{20, 27}, {27, 27}, {27, 20}});
    line(16, 9, 16, 23);
    break;
  case Glyph::Record:
    p.drawEllipse(QPointF(16, 16), 10, 10);
    p.setBrush(p.pen().color());
    p.drawEllipse(QPointF(16, 16), 5, 5);
    break;
  case Glyph::Motion:
    p.drawArc(QRectF(5, 5, 22, 22), 40 * 16, 280 * 16);
    path({{23, 4}, {27, 10}, {20, 10}});
    path({{13, 10}, {22, 16}, {13, 22}, {13, 10}});
    break;
  case Glyph::Clip:
    path({{4, 23}, {10, 10}, {15, 10}, {22, 23}, {28, 12}});
    line(4, 7, 28, 7);
    break;
  case Glyph::Gain:
    for (int i = 0; i < 3; ++i) {
      line(8 + i * 8, 7, 8 + i * 8, 25);
      line(5 + i * 8, 12 + i * 4, 11 + i * 8, 12 + i * 4);
    }
    break;
  case Glyph::Matrix:
    for (int x = 0; x < 3; ++x)
      for (int y = 0; y < 3; ++y)
        p.drawRoundedRect(QRectF(5 + x * 8, 5 + y * 8, 5, 5), 1, 1);
    break;
  case Glyph::Effects:
    for (int i = 0; i < 3; ++i) {
      line(6, 8 + i * 8, 26, 8 + i * 8);
      p.setBrush(QColor("#f7f9fa"));
      p.drawEllipse(QPointF(i == 1 ? 12 : 21, 8 + i * 8), 2.5, 2.5);
    }
    break;
  case Glyph::Dice:
    p.drawRoundedRect(QRectF(5, 5, 22, 22), 4, 4);
    p.setBrush(p.pen().color());
    for (auto pt : {QPointF(10, 10), QPointF(22, 10), QPointF(16, 16),
                    QPointF(10, 22), QPointF(22, 22)})
      p.drawEllipse(pt, 1, 1);
    break;
  case Glyph::Reverse:
    path({{5, 10}, {26, 10}, {21, 5}});
    path({{27, 22}, {6, 22}, {11, 27}});
    break;
  case Glyph::Lock:
    p.drawRoundedRect(QRectF(7, 14, 18, 13), 3, 3);
    p.drawArc(QRectF(10, 4, 12, 18), 0, 180 * 16);
    line(16, 19, 16, 22);
    break;
  case Glyph::Clear:
    line(5, 9, 27, 9);
    path({{9, 9}, {10, 26}, {22, 26}, {23, 9}});
    path({{12, 9}, {12, 5}, {20, 5}, {20, 9}});
    line(14, 13, 14, 22);
    line(18, 13, 18, 22);
    break;
  case Glyph::More:
    p.setBrush(p.pen().color());
    for (double x : {7., 16., 25.})
      p.drawEllipse(QPointF(x, 16), 1.2, 1.2);
    break;
  case Glyph::Balance:
    path({{16, 4}, {28, 16}, {16, 28}, {4, 16}, {16, 4}});
    line(16, 4, 16, 28);
    line(4, 16, 28, 16);
    break;
  case Glyph::None:
    break;
  }
}
QFont fittedFont(QFont font, const QString &label, double width) {
  const int minimum = std::max(1, qRound(font.pixelSize() * .84));
  while (font.pixelSize() > minimum &&
         QFontMetricsF(font).horizontalAdvance(label) > width)
    font.setPixelSize(font.pixelSize() - 1);
  return font;
}
class MorphButton : public QPushButton {
public:
  MorphButton(const QString &caption, QWidget *parent)
      : QPushButton(englishText(caption), parent), glyph(glyphFor(caption)) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setProperty("morphIcon", glyph != Glyph::None);
  }
  QColor accent = ink;
  Glyph glyph;
  bool flat = false, highlighted = false;
  bool showCaption = false, dropdown = false, primary = false;
  bool segmented = false;
  QString displayCaption;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool active = isChecked() || highlighted;
    const bool hovered = underMouse() || highlighted;
    const bool power = glyph == Glyph::Power || glyph == Glyph::Gain ||
                       glyph == Glyph::Clip;
    const bool compare = glyph == Glyph::Compare;
    QColor foreground = active ? accent.darker(125) : ink;
    if (!isEnabled())
      p.setOpacity(.38);
    const auto r = QRectF(rect()).adjusted(1, 1, -1, -1);
    if (power) {
      const double u = std::min(r.width() / 44., r.height() / 26.);
      const QRectF track(r.center().x() - 22 * u,
                         r.center().y() - 12 * u, 44 * u, 24 * u);
      p.setPen(Qt::NoPen);
      p.setBrush(active ? accent : QColor("#c7cdc3"));
      p.drawRoundedRect(track, 12 * u, 12 * u);
      const QPointF thumb(track.left() + (active ? 32 : 12) * u,
                          track.center().y());
      p.setBrush(panelWhite);
      p.drawEllipse(thumb, (isDown() ? 8 : 9) * u,
                    (isDown() ? 8 : 9) * u);
      // A mark and the thumb position both communicate the on/off state.
      p.setPen(QPen(active ? accent.darker(125) : muted, 1.4 * u,
                    Qt::SolidLine, Qt::RoundCap));
      p.drawLine(thumb + QPointF(0, -3) * u, thumb + QPointF(0, 3) * u);
    } else if (compare) {
      controlSurface(p, r, accent, isDown(), false, hovered);
      const double u = height() / 44.;
      const double half = r.width() * .5;
      const bool sideB = text().contains("[B]");
      auto f = font();
      f.setWeight(QFont::DemiBold);
      p.setFont(f);
      for (unsigned side = 0; side < 2; ++side) {
        const QRectF segment(r.left() + half * side + 4 * u,
                             r.top() + 4 * u, half - 8 * u,
                             r.height() - 8 * u);
        if (side == unsigned(sideB)) {
          p.setPen(Qt::NoPen);
          p.setBrush(ink);
          p.drawRoundedRect(segment, 7 * u, 7 * u);
        }
        p.setPen(side == unsigned(sideB) ? panelWhite : muted);
        p.drawText(segment, Qt::AlignCenter, side ? "B" : "A");
      }
    } else if (primary || (segmented && active)) {
      p.setPen(Qt::NoPen);
      p.setBrush(isDown() ? QColor("#131918")
                         : hovered ? QColor("#404947") : ink);
      p.drawRoundedRect(r, r.height() * .28, r.height() * .28);
      foreground = panelWhite;
    } else if (!flat || hovered || isDown() || isChecked()) {
      controlSurface(p, r, accent, isDown(), active, hovered);
    }
    const QString caption = displayCaption.isEmpty() ? text() : displayCaption;
    const bool labeled = showCaption || glyph == Glyph::None;
    const double iconSize = std::min(width(), height()) * .52;
    if (glyph != Glyph::None && !power && !compare) {
      p.save();
      p.translate(labeled ? height() * .55 : width() / 2.,
                  height() / 2. + (isDown() ? .7 : 0));
      p.scale(iconSize / 32., iconSize / 32.);
      p.translate(-16, -16);
      p.setPen(
          QPen(foreground, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.setBrush(Qt::NoBrush);
      drawGlyph(p, glyph,
                active || text() == QString::fromUtf8("♥") ||
                    text().contains("[B]"));
      p.restore();
      if (glyph == Glyph::Sine && !labeled) {
        auto f = font();
        f.setPixelSize(std::max(8, height() / 3));
        p.setFont(f);
        p.setPen(foreground);
        p.drawText(rect().adjusted(0, 1, -3, 0), Qt::AlignRight | Qt::AlignTop,
                   text().endsWith('2') ? "2" : "1");
      }
    }
    if (labeled && !power) {
      const auto label = rect().adjusted(
          glyph == Glyph::None ? 12 : qRound(height() * .98),
          1 + int(isDown()), dropdown ? -qRound(height() * .85) : -12, -1);
      p.setFont(fittedFont(font(), caption, label.width()));
      p.setPen(foreground);
      p.drawText(
          label, (dropdown || showCaption ? Qt::AlignLeft : Qt::AlignHCenter) |
                     Qt::AlignVCenter,
          p.fontMetrics().elidedText(caption, Qt::ElideRight, label.width()));
    }
    if (dropdown) {
      const QPointF c(width() - height() * .43, height() * .5);
      const double d = height() * .09;
      QPainterPath chevron;
      const double direction = isCheckable() && active ? -1. : 1.;
      chevron.moveTo(c + QPointF(-d, -d * .45 * direction));
      chevron.lineTo(c + QPointF(0, d * .55 * direction));
      chevron.lineTo(c + QPointF(d, -d * .45 * direction));
      p.setPen(QPen(foreground, std::max(1., height() * .035),
                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.setBrush(Qt::NoBrush);
      p.drawPath(chevron);
    }
    if (hasFocus()) {
      p.setPen(QPen(focusColor, 1.5));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 8, 8);
    }
  }
};

class MorphChoiceDelegate final : public QStyledItemDelegate {
public:
  explicit MorphChoiceDelegate(QComboBox *owner)
      : QStyledItemDelegate(owner), combo(owner) {}
  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    const QFontMetrics metrics(option.font);
    return {metrics.horizontalAdvance(index.data().toString()) + 56,
            std::max(32, metrics.height() + 16)};
  }
  void paint(QPainter *p, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const auto r = QRectF(option.rect).adjusted(4, 2, -4, -2);
    const bool selected = option.state & QStyle::State_Selected;
    const bool current = index.row() == combo->currentIndex();
    const double u = std::max(1., option.font.pixelSize() / 13.);
    p->setPen(Qt::NoPen);
    p->setBrush(selected ? QColor("#e4eeeb") : panelWhite);
    p->drawRoundedRect(r, 6 * u, 6 * u);
    p->setFont(option.font);
    p->setPen(index.flags() & Qt::ItemIsEnabled ? ink : muted);
    const auto label = r.adjusted(12 * u, 0, -32 * u, 0);
    p->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(option.font).elidedText(
                    index.data().toString(), Qt::ElideRight, int(label.width())));
    if (current) {
      const QPointF c(r.right() - 16 * u, r.center().y());
      QPainterPath check;
      check.moveTo(c + QPointF(-4, 0) * u);
      check.lineTo(c + QPointF(-1, 3) * u);
      check.lineTo(c + QPointF(5, -3) * u);
      p->setPen(QPen(focusColor, 1.7 * u, Qt::SolidLine,
                    Qt::RoundCap, Qt::RoundJoin));
      p->drawPath(check);
    }
    p->restore();
  }

private:
  QComboBox *combo;
};

class MorphCombo final : public QComboBox {
public:
  explicit MorphCombo(QWidget *parent = nullptr) : QComboBox(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    auto *list = new QListView(this);
    list->setObjectName("morphChoiceList");
    list->setUniformItemSizes(true);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setStyleSheet(
        "QListView {background:#fdfdfa;color:#292d2e;border:1px solid #dcded7;"
        "border-radius:9px;padding:5px;outline:0;}"
        "QScrollBar:vertical {background:#fdfdfa;width:8px;margin:4px 0;}"
        "QScrollBar::handle:vertical {background:#b9c1b6;min-height:24px;"
        "border-radius:4px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical {height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical "
        "{background:transparent;}");
    setView(list);
    setItemDelegate(new MorphChoiceDelegate(this));
    setMaxVisibleItems(10);
  }
  void showPopup() override {
    view()->setFont(font());
    const double u = std::max(1., font().pixelSize() / 13.);
    int contentWidth = width();
    for (int row = 0; row < count(); ++row)
      contentWidth = std::max(contentWidth,
          fontMetrics().horizontalAdvance(itemText(row)) + qRound(56 * u));
    view()->setMinimumWidth(std::min(contentWidth, qRound(420 * u)));
    QComboBox::showPopup();
    update();
  }
  void hidePopup() override {
    QComboBox::hidePopup();
    update();
  }

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
      update();
    return QComboBox::event(event);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
      p.setOpacity(.45);
    const bool open = view()->isVisible();
    controlSurface(p, QRectF(rect()).adjusted(1, 1, -1, -1), focusColor,
                   open, open, underMouse());
    const double u = height() / 30.;
    const QRectF label(10 * u, 0, width() - 36 * u, height() - 1);
    p.setPen(ink);
    p.setFont(fittedFont(font(), currentText(), label.width()));
    p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(currentText(), Qt::ElideRight,
                                          int(label.width())));
    QPainterPath arrow;
    const QPointF c(width() - 12 * u, height() / 2.);
    const double direction = open ? -1. : 1.;
    arrow.moveTo(c + QPointF(-3.5, -1.5 * direction) * u);
    arrow.lineTo(c + QPointF(0, 2 * direction) * u);
    arrow.lineTo(c + QPointF(3.5, -1.5 * direction) * u);
    p.setPen(QPen(ink, 1.3 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(arrow);
    if (hasFocus()) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(focusColor, 1.5));
      p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 7, 7);
    }
  }
};
class MorphValueBox final : public QDoubleSpinBox {
public:
  using QDoubleSpinBox::QDoubleSpinBox;

protected:
  void paintEvent(QPaintEvent *e) override {
    QDoubleSpinBox::paintEvent(e);
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (auto part : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
      const auto r =
          style()->subControlRect(QStyle::CC_SpinBox, &option, part, this);
      const auto c = QRectF(r).center();
      const double d = std::min(r.width(), r.height()) * .22;
      const double sign = part == QStyle::SC_SpinBoxUp ? -1 : 1;
      QPainterPath arrow;
      arrow.moveTo(c + QPointF(-d, -sign * d / 2));
      arrow.lineTo(c + QPointF(0, sign * d / 2));
      arrow.lineTo(c + QPointF(d, -sign * d / 2));
      p.drawPath(arrow);
    }
  }
};
// Keep long gestures within the fixed audio program without dropping their
// ending. Remove the least significant point in time/position space, retaining
// pauses and changes in speed as well as bends in the route.
void appendPoint(MorphCanvasProgram &c, float x, float y, float time) {
  if (c.pointCount == MorphCanvasProgram::maxPoints) {
    unsigned remove = 1;
    double least = std::numeric_limits<double>::max();
    for (unsigned i = 1; i + 1 < c.pointCount; ++i) {
      const auto &a = c.path[i - 1], &b = c.path[i], &d = c.path[i + 1];
      const double t = std::clamp(double(b.time - a.time) /
                                      std::max(1.e-9, double(d.time - a.time)),
                                  0., 1.);
      const double dx = b.x - (a.x + (d.x - a.x) * t),
                   dy = b.y - (a.y + (d.y - a.y) * t);
      const double error = dx * dx + dy * dy;
      if (error < least) {
        least = error;
        remove = i;
      }
    }
    for (unsigned i = remove; i + 1 < c.pointCount; ++i)
      c.path[i] = c.path[i + 1];
    --c.pointCount;
  }
  c.path[c.pointCount++] = {x, y, time};
}
class MorphKnob final : public ui::Knob {
public:
  MorphKnob(unsigned id, QString caption, QColor color, QWidget *parent)
      : ui::Knob(englishText(caption), parent), index(id),
        label(englishText(caption)), accent(color) {
    if (label != "HPF" && label != "LPF") {
      label = label.toLower();
      if (!label.isEmpty())
        label[0] = label[0].toUpper();
    }
    const auto &info = parameterTable()[id];
    setRange(info.minValue, info.maxValue);
    setDefaultValue(info.defaultValue);
    setStepped(info.isStepped);
    setLogarithmic(info.unit == "Hz" || id == EchoTime);
    setAutomatable(true);
    setFormatter([id](double v) { return valueText(id, v); });
    setArcColor(color);
    setAccessibleName(englishText(QString::fromStdString(info.name)));
    setObjectName(QString::fromStdString(info.id));
    setAcceptDrops(modulationTarget(id));
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  }
  unsigned index;
  QString label;
  QColor accent;
  double modulation = 0;
  bool highlighted = false;
  std::function<void(unsigned, unsigned)> assign;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const bool hero = index == Interaction;
    const double heightUnits = hero ? 150. : 126.;
    const double scale = std::min(width() / 100., height() / heightUnits);
    p.translate((width() - 100 * scale) / 2.,
                (height() - heightUnits * scale) / 2.);
    p.scale(scale, scale);
    const QPointF c(50, hero ? 103 : 44);
    const double resolution = scale * devicePixelRatioF();
    if (face.isNull() || !qFuzzyCompare(face.devicePixelRatio(), resolution)) {
      face = QPixmap(int(std::ceil(100 * resolution)),
                     int(std::ceil(heightUnits * resolution)));
      face.setDevicePixelRatio(resolution);
      face.fill(Qt::transparent);
      QPainter q(&face);
      q.setRenderHint(QPainter::Antialiasing);
      q.setPen(Qt::NoPen);
      q.setBrush(QColor(34, 44, 37, 12));
      q.drawEllipse(c + QPointF(0, 3), 35, 35);
      q.setBrush(Qt::NoBrush);
      q.setPen(QPen(QColor("#dfe3da"), 3.5, Qt::SolidLine, Qt::RoundCap));
      q.drawArc(QRectF(c.x() - 42, c.y() - 42, 84, 84), 225 * 16, -270 * 16);
      QLinearGradient cap(c - QPointF(0, 34), c + QPointF(0, 34));
      cap.setColorAt(0, QColor("#454e4b"));
      cap.setColorAt(1, QColor("#282f2c"));
      q.setBrush(cap);
      q.setPen(QPen(QColor("#202824"), .8));
      q.drawEllipse(c, 34, 34);
      q.setBrush(Qt::NoBrush);
      q.setPen(QPen(QColor(255, 255, 255, 22), 1));
      q.drawEllipse(c, 31.5, 31.5);
      ::text(q, hero ? QRectF(0, 0, 100, 20) : QRectF(0, 109, 100, 17), label,
             hero ? 12 : 13, muted);
    }
    p.drawPixmap(0, 0, face);
    const bool log = parameterTable()[index].unit == "Hz" || index == EchoTime;
    const double t = std::clamp(
        log ? std::log(std::max(minimumValue(), value()) / minimumValue()) /
                  std::log(maximumValue() / minimumValue())
            : (value() - minimumValue()) / (maximumValue() - minimumValue()),
        0., 1.);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(accent, 3.5, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(c.x() - 42, c.y() - 42, 84, 84), 225 * 16,
              int(-270 * t * 16));
    if (modulation != 0 || highlighted) {
      p.setPen(QPen(highlighted ? ink : accent, 1.7, Qt::DashLine));
      p.drawArc(
          QRectF(c.x() - 46, c.y() - 46, 92, 92),
          int((225 - 270 * std::clamp(t - std::abs(modulation), 0., 1.)) * 16),
          int(-270 * std::min(1., std::abs(modulation) * 2) * 16));
    }
    const auto inner = polar(c, 20, 135 + t * 270),
               outer = polar(c, 28, 135 + t * 270);
    p.setPen(QPen(panelWhite, 3.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(inner, outer);
    p.setPen(QPen(panelWhite, 1.2));
    p.setBrush(accent);
    p.drawEllipse(polar(c, 42, 135 + t * 270), 3.4, 3.4);
    if (cachedValue != value() || cachedIndex != index) {
      cachedValue = value();
      cachedIndex = index;
      number.setText(valueText(index, value()));
      number.setTextFormat(Qt::PlainText);
      numberFont = morphFont(hero ? 22 : 15);
      numberFont.setWeight(QFont::DemiBold);
      number.prepare(QTransform(), numberFont);
    }
    p.setFont(numberFont);
    p.setPen(ink);
    p.drawStaticText(
        QPointF((100 - number.size().width()) * .5, hero ? 23 : 91), number);
    if (hasFocus()) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(focusColor, 1.3));
      p.drawRoundedRect(QRectF(1, 1, 98, heightUnits - 2), 10, 10);
    }
  }

  void dragEnterEvent(QDragEnterEvent *e) override {
    if (e->mimeData()->hasFormat(sourceMime)) {
      e->acceptProposedAction();
      highlighted = true;
      update();
    }
  }
  void dragLeaveEvent(QDragLeaveEvent *) override {
    highlighted = false;
    update();
  }
  void dropEvent(QDropEvent *e) override {
    if (assign && e->mimeData()->hasFormat(sourceMime)) {
      assign(e->mimeData()->data(sourceMime).toUInt(), index);
      e->acceptProposedAction();
    }
    highlighted = false;
    update();
  }
  void contextMenuEvent(QContextMenuEvent *e) override {
    MorphMenu menu(this);
    menu.addAction(QString::fromUtf8("Edit value…"), this, [this] {
      bool ok = false;
      double v = QInputDialog::getDouble(
          this, label, QString::fromUtf8("Value"), value(), minimumValue(),
          maximumValue(), 3, &ok);
      if (ok) {
        editValue(v);
        finishEditing();
      }
    });
    menu.addAction(QString::fromUtf8("Reset"), this, [this] {
      editValue(parameterTable()[index].defaultValue);
      finishEditing();
    });
    menu.addAction(QString::fromUtf8("Create automation"), this,
                   [this] { emit automateRequested(); });
    if (modulationTarget(index)) {
      auto *mod = menu.addMenu(QString::fromUtf8("Assign modulation"));
      for (unsigned s = 1; s < sourceNames.size(); ++s)
        mod->addAction(englishText(QString::fromUtf8(sourceNames[s])), this,
                       [this, s] {
                         if (assign)
                           assign(s, index);
                       });
    }
    menu.exec(e->globalPos());
  }

private:
  QPixmap face;
  QStaticText number;
  QFont numberFont;
  double cachedValue = std::numeric_limits<double>::quiet_NaN();
  unsigned cachedIndex = Count;
};
class SourceButton final : public MorphButton {
public:
  using MorphButton::MorphButton;
  unsigned source = 1;

protected:
  void mousePressEvent(QMouseEvent *e) override {
    start = e->pos();
    QPushButton::mousePressEvent(e);
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    if ((e->buttons() & Qt::LeftButton) &&
        (e->pos() - start).manhattanLength() >
            QApplication::startDragDistance()) {
      auto *drag = new QDrag(this);
      auto *mime = new QMimeData;
      mime->setData(sourceMime, QByteArray::number(source));
      drag->setMimeData(mime);
      drag->exec(Qt::CopyAction);
      setDown(false);
    }
  }

private:
  QPoint start;
};
class PaintWidget final : public QWidget {
public:
  using QWidget::QWidget;
  std::function<void(QPainter &)> paint;
  std::function<void(QMouseEvent *, int)> pointer;
  std::function<void(QKeyEvent *)> key;
  std::function<void(QContextMenuEvent *)> menu;
  std::function<void(QString)> fileDrop;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (paint)
      paint(p);
  }
  void mousePressEvent(QMouseEvent *e) override {
    setFocus(Qt::MouseFocusReason);
    if (pointer)
      pointer(e, 0);
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    if (pointer)
      pointer(e, 1);
  }
  void mouseReleaseEvent(QMouseEvent *e) override {
    if (pointer)
      pointer(e, 2);
  }
  void keyPressEvent(QKeyEvent *e) override {
    if (key)
      key(e);
    else
      QWidget::keyPressEvent(e);
  }
  void contextMenuEvent(QContextMenuEvent *e) override {
    if (menu)
      menu(e);
  }
  void dragEnterEvent(QDragEnterEvent *e) override {
    if (fileDrop && e->mimeData()->hasUrls())
      e->acceptProposedAction();
  }
  void dropEvent(QDropEvent *e) override {
    if (fileDrop && e->mimeData()->hasUrls() &&
        !e->mimeData()->urls().empty()) {
      fileDrop(e->mimeData()->urls().front().toLocalFile());
      e->acceptProposedAction();
    }
  }
};
QImage programImage(const MorphCanvasProgram &c) {
  if (!c.previewWidth || !c.previewHeight)
    return {};
  QImage im(int(c.previewWidth), int(c.previewHeight), QImage::Format_ARGB32);
  for (unsigned y = 0; y < c.previewHeight; ++y)
    std::copy_n(c.rgba.data() + y * c.previewSide, c.previewWidth,
                reinterpret_cast<QRgb *>(im.scanLine(int(y))));
  return im;
}
void storeImage(MorphCanvasProgram &c, const QImage &im) {
  c.rgba.fill(0);
  c.previewWidth = unsigned(im.width());
  c.previewHeight = unsigned(im.height());
  for (int y = 0; y < im.height(); ++y)
    std::copy_n(reinterpret_cast<const QRgb *>(im.constScanLine(y)), im.width(),
                c.rgba.data() + y * c.previewSide);
  c.painted = true;
  analyzeCanvas(c);
}
} // namespace

struct MorphPanel::Impl {
  MorphPanel *q;
  daw::EngineController *controller;
  std::string channel, insert;
  State state = factoryState(30), a = state, b = state;
  Telemetry telemetry;
  QScrollArea *scroll;
  PaintWidget *surface;
  PaintWidget *flow;
  PaintWidget *pad;
  PaintWidget *meters;
  PaintWidget *gainReadout;
  PaintWidget *env;
  QTimer timer;
  QPixmap background;
  QImage flowWaves;
  std::array<int, 13> flowKey{};
  double zoom = .75;
  std::array<bool, 4> moduleExpanded{};
  bool detailsExpanded = false;
  bool advanced = false, canvasTab = false, brush = true, reduced = false,
       sideB = false, record = false, refreshing = false;
  QColor brushColor = accents[0];
  double brushRadius = .09, brushOpacity = .7;
  QImage canvasImage;
  unsigned importGeneration = 0;
  std::shared_ptr<std::atomic<unsigned>> importToken =
      std::make_shared<std::atomic<unsigned>>(0);
  bool importing = false;
  QString imageError;
  unsigned matrixPage = 0, lfo = 0;
  QStringList favorites;
  struct Item {
    QWidget *widget;
    QRectF r;
    unsigned section =
        0; // 0 main, 1 effects, 2 modulation, 3..6 module details
  };
  std::vector<Item> items;
  std::vector<MorphKnob *> knobs;
  std::array<std::optional<double>, Count> gestures{};
  std::array<QComboBox *, 4> sources{}, targets{};
  std::array<QDoubleSpinBox *, 4> amounts{};
  struct Choice {
    QComboBox *box;
    unsigned id;
  };
  std::vector<Choice> choices;
  struct Toggle {
    QPushButton *button;
    unsigned id;
  };
  std::vector<Toggle> toggles;
  QPushButton *topPreset, *star, *tabFlow, *tabCanvas, *recordButton,
      *pageButton, *abButton;
  std::optional<State> drawingBefore, padBefore;
  QElapsedTimer gestureClock;
  int selectedPoint = -1;
  QPointF lastBrush;
  std::array<float, 120> envelopeHistory{};
  unsigned envelopeAt = 0;
  std::vector<QWidget *> canvasTools;
  std::array<MorphKnob *, 4> lfoKnobs{};
  explicit Impl(MorphPanel *owner, daw::EngineController *c, QString ch,
                QString in);
  std::array<MorphButton *, 4> moduleButtons{};
  MorphButton *detailsButton = nullptr, *balanceButton = nullptr;
  unsigned moduleHeight() const {
    return std::any_of(moduleExpanded.begin(), moduleExpanded.end(),
                       [](bool open) { return open; })
               ? 172
               : 0;
  }
  void toggleDetails();
  void resizeToContent();
  void build();
  void layout();
  void cache();
  void refresh();
  void sync();
  void finish();
  void write(unsigned id, double value);
  void end(unsigned id);
  bool apply(State next, const QString &label, bool history = true,
             const State *previous = nullptr);
  void add(QWidget *w, QRectF r, unsigned section = 0) {
    items.push_back({w, r, section});
  }
  MorphButton *button(QString title, QRectF r, std::function<void()> action,
                      unsigned section = 0);
  MorphButton *toggle(QString title, unsigned id, QRectF r,
                      unsigned section = 0);
  QComboBox *choice(unsigned id, QRectF r, QStringList labels = {},
                    unsigned section = 0);
  MorphKnob *knob(unsigned id, QString label, QColor color, QRectF r,
                  unsigned section = 0);
  void presetMenu(QWidget *anchor);
  void settingsMenu();
  void assign(unsigned source, unsigned target);
  void paintFlow(QPainter &);
  void paintPad(QPainter &);
  void padPointer(QMouseEvent *, int);
  void canvasPointer(QMouseEvent *, int);
  void brushSettings();
  void pathMenu();
  void importPhoto(const QString &);
  void saveUser();
  void userMenu(QMenu *);
  void persistUsers(const QJsonArray &users) {
    QSettings().setValue("Morph/users.v1",
                         QJsonDocument(users).toJson(QJsonDocument::Compact));
  }
  QJsonArray users() const {
    return QJsonDocument::fromJson(
               QSettings().value("Morph/users.v1").toByteArray())
        .array();
  }
};

MorphPanel::Impl::Impl(MorphPanel *owner, daw::EngineController *c, QString ch,
                       QString in)
    : q(owner), controller(c), channel(ch.toStdString()),
      insert(in.toStdString()) {
  if (auto snapshot = controller->morphSnapshot(channel, insert)) {
    state = snapshot->state;
    telemetry = snapshot->telemetry;
  }
  a = b = state;
  reduced = QSettings().value("Morph/reducedMotion", false).toBool();
  favorites = QSettings().value("Morph/favorites").toStringList();
  scroll = new QScrollArea(q);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(false);
  surface = new PaintWidget;
  surface->setAttribute(Qt::WA_OpaquePaintEvent);
  q->setFont(morphFont());
  surface->setFont(morphFont());
  surface->setObjectName("morphSurface");
  scroll->setWidget(surface);
  QPalette light = surface->palette();
  light.setColor(QPalette::Window, paper);
  light.setColor(QPalette::WindowText, ink);
  light.setColor(QPalette::Base, panelWhite);
  light.setColor(QPalette::Text, ink);
  light.setColor(QPalette::Button, fieldColor);
  light.setColor(QPalette::ButtonText, ink);
  light.setColor(QPalette::Highlight, QColor("#e4eeeb"));
  light.setColor(QPalette::HighlightedText, ink);
  surface->setPalette(light);
  q->setPalette(light);
  scroll->setPalette(light);
  scroll->viewport()->setAutoFillBackground(true);
  scroll->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  surface->setStyleSheet(QStringLiteral(
      "QDoubleSpinBox {color:#292d2e;background:#eeefe9;border:1px solid "
      "#dcded7;border-radius:7px;padding:2px 7px;}"
      "QDoubleSpinBox:focus {border-color:#247d89;}"
      "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button "
      "{border:0;width:16px;}"
      "QDoubleSpinBox::up-arrow,QDoubleSpinBox::down-arrow {image:none;}"
      "QToolTip {color:#292d2e;background:#fdfdfa;border:1px solid "
      "#dcded7;padding:6px;}"));
  surface->paint = [this](QPainter &p) {
    if (background.isNull())
      cache();
    p.drawPixmap(0, 0, background);
  };
  build();
  canvasImage = programImage(state.canvas);
  sync();
  timer.setInterval(33);
  QObject::connect(&timer, &QTimer::timeout, q, [this] { refresh(); });
}
MorphButton *MorphPanel::Impl::button(QString title, QRectF r,
                                      std::function<void()> action,
                                      unsigned section) {
  auto *w = new MorphButton(title, surface);
  w->setAccessibleName(englishText(title));
  if (title == QString::fromUtf8("‹"))
    w->setAccessibleName(QString::fromUtf8("Previous preset"));
  else if (title == QString::fromUtf8("›"))
    w->setAccessibleName(QString::fromUtf8("Next preset"));
  else if (title == QString::fromUtf8("≡"))
    w->setAccessibleName(QString::fromUtf8("Presets"));
  w->setCursor(Qt::PointingHandCursor);
  add(w, r, section);
  QObject::connect(w, &QPushButton::clicked, q, std::move(action));
  return w;
}
MorphButton *MorphPanel::Impl::toggle(QString title, unsigned id, QRectF r,
                                      unsigned section) {
  auto *w = button(
      title, r,
      [this, id] {
        auto next = state;
        next.values[id] = state.values[id] < .5 ? 1 : 0;
        apply(std::move(next), QString::fromUtf8("Change Morph"));
      },
      section);
  w->setCheckable(true);
  w->setObjectName(QString::fromStdString(parameterTable()[id].id));
  w->setAccessibleName(
      englishText(QString::fromStdString(parameterTable()[id].name)));
  w->setToolTip(w->accessibleName());
  toggles.push_back({w, id});
  return w;
}
QComboBox *MorphPanel::Impl::choice(unsigned id, QRectF r, QStringList labels,
                                    unsigned section) {
  auto *w = new MorphCombo(surface);
  w->setAccessibleName(
      englishText(QString::fromStdString(parameterTable()[id].name)));
  w->setObjectName(QString::fromStdString(parameterTable()[id].id));
  if (labels.empty())
    for (int i = int(parameterTable()[id].minValue);
         i <= int(parameterTable()[id].maxValue); ++i)
      labels << valueText(id, i);
  for (const auto &label : labels)
    w->addItem(englishText(label));
  add(w, r, section);
  choices.push_back({w, id});
  QObject::connect(w, &QComboBox::activated, q, [this, id](int n) {
    auto next = state;
    next.values[id] = n + parameterTable()[id].minValue;
    apply(std::move(next), QString::fromUtf8("Change Morph"));
  });
  return w;
}
MorphKnob *MorphPanel::Impl::knob(unsigned id, QString label, QColor color,
                                  QRectF r, unsigned section) {
  auto *k = new MorphKnob(id, label, color, surface);
  add(k, r, section);
  knobs.push_back(k);
  k->assign = [this](unsigned s, unsigned t) { assign(s, t); };
  QObject::connect(k, &ui::Knob::valueChanged, q, [this, k](double v) {
    if (!refreshing)
      write(k->index, v);
  });
  QObject::connect(k, &ui::Knob::editFinished, q, [this, k] { end(k->index); });
  QObject::connect(k, &ui::Knob::automateRequested, q, [this, k] {
    emit q->automationRequested(
        QString::fromStdString(parameterTable()[k->index].id));
  });
  return k;
}
void MorphPanel::Impl::build() {
  button(QString::fromUtf8("‹"), {434, 28, 40, 44}, [this] {
    auto presets = factoryPresets();
    unsigned n = 0;
    for (unsigned i = 0; i < presets.size(); ++i)
      if (state.presetName == presets[i].name)
        n = i;
    q->applyFactoryPreset((n + 29) % 30);
  });
  button(QString::fromUtf8("›"), {480, 28, 40, 44}, [this] {
    auto presets = factoryPresets();
    unsigned n = 29;
    for (unsigned i = 0; i < presets.size(); ++i)
      if (state.presetName == presets[i].name)
        n = i;
    q->applyFactoryPreset((n + 1) % 30);
  });
  topPreset =
      button("Init", {530, 28, 344, 44}, [this] { presetMenu(topPreset); });
  static_cast<MorphButton *>(topPreset)->dropdown = true;
  topPreset->setObjectName("morphPresetMenu");
  topPreset->setAccessibleName(QString::fromUtf8("Presets"));
  star = button(QString::fromUtf8("♡"), {882, 28, 44, 44}, [this] {
    const auto key =
        QString::fromStdString(state.presetKind + ":" + state.presetName);
    if (!favorites.removeOne(key))
      favorites << key;
    QSettings().setValue("Morph/favorites", favorites);
    sync();
  });
  star->setAccessibleName(QString::fromUtf8("Favorite"));
  abButton = button("A / B", {1002, 28, 80, 44}, [this] {
    if (sideB)
      b = state;
    else
      a = state;
    sideB = !sideB;
    apply(sideB ? b : a, QString::fromUtf8("Compare Morph A/B"));
  });
  abButton->setObjectName("morphAB");
  abButton->setAccessibleName(QString::fromUtf8("Compare Morph A/B"));
  button(QString::fromUtf8("⚙"), {1442, 28, 44, 44}, [this] {
    settingsMenu();
  })->setAccessibleName(QString::fromUtf8("Settings"));
  button("≡", {934, 28, 44, 44}, [this] { presetMenu(topPreset); });
  auto *random = button("Random", {1094, 28, 116, 44}, [this] {
    finish();
    apply(randomState(state, QRandomGenerator::global()->generate()),
          QStringLiteral("Randomize Morph parameters"));
  });
  random->setObjectName("morphRandom");
  random->showCaption = true;
  random->primary = true;
  random->setToolTip("New random sound, Mix 100%. Does not save a preset.");
  pad = new PaintWidget(surface);
  pad->setFocusPolicy(Qt::StrongFocus);
  pad->setAccessibleName(QString::fromUtf8(
      "Morph balance. Arrow keys move; menu edits coordinates."));
  pad->setObjectName("morphPad");
  add(pad, {28, 864, 304, 179}, 2);
  pad->paint = [this](QPainter &p) { paintPad(p); };
  pad->pointer = [this](QMouseEvent *e, int phase) { padPointer(e, phase); };
  pad->key = [this](QKeyEvent *e) {
    auto n = state;
    const double step = e->modifiers() & Qt::ShiftModifier ? .01 : .05;
    switch (e->key()) {
    case Qt::Key_Left:
      n.values[MorphX] -= step;
      break;
    case Qt::Key_Right:
      n.values[MorphX] += step;
      break;
    case Qt::Key_Up:
      n.values[MorphY] -= step;
      break;
    case Qt::Key_Down:
      n.values[MorphY] += step;
      break;
    default:
      e->ignore();
      return;
    }
    double norm =
        std::max(1., std::abs(n.values[MorphX]) + std::abs(n.values[MorphY]));
    n.values[MorphX] /= norm;
    n.values[MorphY] /= norm;
    apply(std::move(n), QString::fromUtf8("Move Morph balance"));
  };
  pad->menu = [this](QContextMenuEvent *e) {
    MorphMenu m;
    for (auto id : {MorphX, MorphY})
      m.addAction(
          englishText(QString::fromStdString(parameterTable()[id].name)), q,
          [this, id] {
            bool ok = false;
            double v = QInputDialog::getDouble(
                q, QString::fromUtf8("Morph balance"),
                englishText(QString::fromStdString(parameterTable()[id].name)),
                state.values[id], -1, 1, 3, &ok);
            if (ok) {
              auto n = state;
              n.values[id] = v;
              apply(std::move(n), QString::fromUtf8("Move Morph balance"));
            }
          });
    m.exec(e->globalPos());
  };
  tabFlow = button("FLOW", {50, 114, 96, 34}, [this] {
    canvasTab = false;
    sync();
  });
  tabCanvas = button("CANVAS", {152, 114, 112, 34}, [this] {
    canvasTab = true;
    sync();
  });
  tabFlow->setCheckable(true);
  tabCanvas->setCheckable(true);
  tabFlow->setObjectName("morphFlowTab");
  tabCanvas->setObjectName("morphCanvasTab");
  for (auto *tab : {tabFlow, tabCanvas}) {
    auto *button = static_cast<MorphButton *>(tab);
    button->showCaption = true;
    button->segmented = true;
    button->displayCaption = tab == tabFlow ? "Flow" : "Canvas";
  }
  choice(Routing, {805, 114, 144, 34});
  auto *interaction =
      knob(Interaction, "INTERACTION", ink, {56, 162, 124, 158});
  interaction->setProperty("morphFlowOnly", true);
  balanceButton =
      button("MODE MIX", {961, 114, 114, 34}, [this] { toggleDetails(); });
  balanceButton->showCaption = true;
  balanceButton->displayCaption = "Balance";
  balanceButton->setCheckable(true);
  balanceButton->setObjectName("morphBalanceDetails");
  balanceButton->setAccessibleName(QString::fromUtf8("Morph balance"));
  detailsButton =
      button("MOD MATRIX", {1083, 114, 135, 34}, [this] { toggleDetails(); });
  detailsButton->showCaption = true;
  detailsButton->displayCaption = "Modulation";
  detailsButton->setCheckable(true);
  detailsButton->setObjectName("morphModulationDetails");
  detailsButton->setAccessibleName(QString::fromUtf8("Modulation and macros"));
  flow = new PaintWidget(surface);
  flow->setFocusPolicy(Qt::StrongFocus);
  flow->setAccessibleName(QString::fromUtf8(
      "Flow and Canvas. Drop an image, paint, or edit a route."));
  flow->setObjectName("morphCanvas");
  flow->setAcceptDrops(true);
  add(flow, {224, 166, 994, 159});
  flow->lower();
  flow->paint = [this](QPainter &p) { paintFlow(p); };
  flow->pointer = [this](QMouseEvent *e, int phase) {
    canvasPointer(e, phase);
  };
  flow->fileDrop = [this](QString path) { importPhoto(path); };
  flow->menu = [this](QContextMenuEvent *) { pathMenu(); };
  flow->key = [this](QKeyEvent *e) {
    if (!canvasTab || selectedPoint < 0 ||
        unsigned(selectedPoint) >= state.canvas.pointCount)
      return;
    auto n = state;
    auto &pt = n.canvas.path[unsigned(selectedPoint)];
    double step = e->modifiers() & Qt::ShiftModifier ? .005 : .025;
    switch (e->key()) {
    case Qt::Key_Left:
      pt.x = float(std::max(-1., pt.x - step));
      break;
    case Qt::Key_Right:
      pt.x = float(std::min(1., pt.x + step));
      break;
    case Qt::Key_Up:
      pt.y = float(std::max(-1., pt.y - step));
      break;
    case Qt::Key_Down:
      pt.y = float(std::min(1., pt.y + step));
      break;
    default:
      return;
    }
    apply(std::move(n), QString::fromUtf8("Edit Morph route"));
  };

  canvasTools.push_back(
      button(QString::fromUtf8("Import"), {49, 294, 102, 32}, [this] {
        const auto path = QFileDialog::getOpenFileName(
            q, QString::fromUtf8("Import Canvas image"), {},
            QString::fromUtf8("Images (*.png *.jpg *.jpeg *.webp)"));
        if (!path.isEmpty())
          importPhoto(path);
      }));
  canvasTools.back()->setToolTip(QString::fromUtf8(
      "Create a sound locally from the image's colors, contrast and texture. "
      "Mix stays at 100%."));
  canvasTools.push_back(
      button(QString::fromUtf8("Brush"), {159, 294, 94, 32}, [this] {
        brush = true;
        brushSettings();
      }));
  canvasTools.push_back(
      button(QString::fromUtf8("Route"), {261, 294, 96, 32}, [this] {
        brush = false;
        pathMenu();
      }));
  canvasTools.push_back(
      button(QString::fromUtf8("Variation"), {365, 294, 119, 32}, [this] {
        if (!state.canvas.painted)
          return;
        auto n = state;
        n.canvas.seed = n.canvas.seed * 1664525u + 1013904223u;
        n.values = interpretCanvas(n.canvas, n.values);
        n.presetKind = "custom";
        n.presetName = "Image sound";
        n.category.clear();
        apply(std::move(n), QString::fromUtf8("Vary Morph Canvas"));
      }));
  canvasTools.back()->setToolTip(QString::fromUtf8(
      "Create another sound from this image. Keeps the image and route; does "
      "not save a preset."));
  canvasTools.push_back(
      toggle(QString::fromUtf8("Scan"), CanvasOn, {504, 294, 88, 32}));
  auto *influence = new MorphValueBox(surface);
  influence->setRange(0, 100);
  influence->setSuffix("%");
  influence->setDecimals(0);
  influence->setObjectName("canvas.influence");
  influence->setAccessibleName(QString::fromUtf8("Canvas influence"));
  add(influence, {600, 294, 86, 32});
  canvasTools.push_back(influence);
  QObject::connect(influence, &QDoubleSpinBox::valueChanged, q,
                   [this](double v) {
                     if (!refreshing)
                       write(CanvasInfluence, v * .01);
                   });
  QObject::connect(influence, &QDoubleSpinBox::editingFinished, q,
                   [this] { end(CanvasInfluence); });
  recordButton =
      button(QString::fromUtf8("● Record"), {710, 294, 102, 32}, [this] {
        record = !record;
        sync();
      });
  recordButton->setCheckable(true);
  recordButton->setObjectName("morphRecord");
  canvasTools.push_back(recordButton);
  static_cast<MorphButton *>(recordButton)->displayCaption = "Record";
  canvasTools.push_back(choice(MotionBars, {820, 294, 106, 32}));
  canvasTools.push_back(
      toggle(QString::fromUtf8("Motion"), MotionOn, {934, 294, 96, 32}));
  knob(Mix, "MIX", muted, {1280, 151, 150, 177});
  meters = new PaintWidget(surface);
  add(meters, {1436, 157, 55, 160});
  meters->setAccessibleName(QString::fromUtf8("Stereo output level"));
  meters->paint = [this](QPainter &p) {
    p.scale(meters->width() / 55., meters->height() / 160.);
    for (unsigned ch = 0; ch < 2; ++ch) {
      const double x = 11 + ch * 17;
      const QRectF rail(x, 7, 6, 105);
      p.setPen(QPen(QColor("#d2d9e1"), .8));
      p.setBrush(QColor("#e0e5ea"));
      p.drawRoundedRect(rail, 2, 2);
      const double db = 20 * std::log10(std::max(.001f, ch ? telemetry.right
                                                           : telemetry.left));
      const double level = std::clamp((db + 60) / 60., 0., 1.);
      QLinearGradient color(0, 112, 0, 7);
      color.setColorAt(0, QColor("#69cead"));
      color.setColorAt(.8, QColor("#a9d9bf"));
      color.setColorAt(1, QColor("#e6b869"));
      p.fillRect(QRectF(x + .7, 112 - 104 * level, 4.6, 104 * level), color);
      text(p, {x - 5, 116, 16, 18}, ch ? "R" : "L", 10, muted);
    }
    const double peak =
        20 *
        std::log10(std::max(.001f, std::max(telemetry.left, telemetry.right)));
    text(p, {0, 138, 55, 20},
         QString::number(peak, 'f', 1) + " " + englishText("dB"), 11, muted);
  };
  auto *clip = toggle("SOFT CLIP", SoftClip, {1431, 741, 54, 32});
  clip->setToolTip("Soft Clip: gently limit output peaks above -0.9 dBFS.");
  auto *autoGain = toggle("AUTO GAIN", AutoGain, {1431, 682, 54, 32});
  autoGain->setToolTip(
      "Auto Gain: match wet loudness to the input. Gain adds a manual trim.");
  gainReadout = new PaintWidget(surface);
  gainReadout->setAccessibleName("Automatic gain compensation");
  gainReadout->setObjectName("morphGainReadout");
  add(gainReadout, {1223, 709, 198, 19});
  gainReadout->paint = [this](QPainter &p) {
    const double db = 20 * std::log10(std::max(.25f, telemetry.autoGain));
    const QString label =
        QString("Applied %1%2 dB").arg(db >= 0 ? "+" : "").arg(db, 0, 'f', 1);
    text(p, gainReadout->rect(), label, std::max(9, qRound(11 * zoom)), muted,
         0, Qt::AlignLeft | Qt::AlignVCenter);
  };
  knob(Output, "GAIN", accents[0], {1086, 665, 115, 131})
      ->setToolTip("Output gain: -24 to +12 dB, after Auto Gain and Dry/Wet.");
  const std::array<std::array<const char *, 6>, 4> labels{
      {{"RATE", "DEPTH", "VOICES", "SPREAD", "DETUNE", "TONE"},
       {"RATE", "DEPTH", "FEEDBACK", "DELAY", "COLOR", "STEREO"},
       {"RATE", "DEPTH", "STAGES", "RESONANCE", "SPREAD", "MIX"},
       {"TIME", "DETUNE", "VOICES", "SPREAD", "HUMANIZE", "LEVEL"}}};
  const std::array<std::array<unsigned, 6>, 4> order{{{0, 1, 3, 2, 4, 5},
                                                      {0, 1, 2, 3, 4, 5},
                                                      {0, 1, 3, 2, 4, 5},
                                                      {0, 4, 3, 1, 2, 5}}};
  for (unsigned m = 0; m < 4; ++m) {
    const double x = 26 + 374. * m;
    auto *on = toggle("⏻", moduleOffsets[m], {x + 287, 375, 54, 32});
    on->accent = accents[m];
    auto *more = button("…", {x + 258, 412, 83, 28}, [this, m] {
      finish();
      moduleExpanded[m] = !moduleExpanded[m];
      resizeToContent();
    });
    more->glyph = Glyph::None;
    more->displayCaption = "More";
    more->dropdown = true;
    more->accent = accents[m];
    more->setCheckable(true);
    more->setObjectName(QString("morphModuleDetails%1").arg(m));
    more->setAccessibleName(
        QString::fromUtf8("More %1 controls")
            .arg(englishText(std::array<const char *, 4>{
                "Chorus", "Flanger", "Phaser", "Doubler"}[m])));
    moduleButtons[m] = more;
    auto *link = toggle("↔", linkParameters[m], {x + 303, 614, 32, 30}, 3 + m);
    link->accent = accents[m];
    link->setAccessibleName(QString::fromUtf8("Link modulation phase"));
    for (unsigned j = 0; j < 6; ++j) {
      const unsigned param = order[m][j];
      knob(moduleOffsets[m] + 1 + param, labels[m][param], accents[m],
           {x + 15. + 113 * (j % 3), j < 3 ? 444. : 650., 108,
            j < 3 ? 145. : 114.},
           j < 3 ? 0 : 3 + m);
    }
    if (m < 3)
      choice(std::array<unsigned, 3>{ChorusSync, FlangerSync, PhaserSync}[m],
             {x + 25, 614, 155, 30}, {}, 3 + m);
  }
  for (unsigned i = 0; i < 2; ++i) {
    auto *source = new SourceButton(QString("LFO %1").arg(i + 1), surface);
    source->source = i + 1;
    source->accent = accents[i];
    source->setCheckable(true);
    source->setAccessibleName(
        QString::fromUtf8("LFO %1 — select or drag to assign").arg(i + 1));
    source->segmented = true;
    source->glyph = Glyph::None;
    source->showCaption = false;
    add(source, {372. + i * 72, 633, 66, 28});
    QObject::connect(source, &QPushButton::clicked, q, [this, i] {
      finish();
      lfo = i;
      for (unsigned n = 0; n < 4; ++n) {
        auto *k = lfoKnobs[n];
        k->index = (i ? Lfo2Rate : Lfo1Rate) + n;
        const auto &info = parameterTable()[k->index];
        k->setObjectName(QString::fromStdString(info.id));
        k->setAccessibleName(englishText(QString::fromStdString(info.name)));
        k->setRange(info.minValue, info.maxValue);
        k->setDefaultValue(info.defaultValue);
        k->setFormatter([id = k->index](double v) { return valueText(id, v); });
      }
      sync();
    });
  }
  for (unsigned i = 0; i < 4; ++i)
    lfoKnobs[i] =
        knob(Lfo1Rate + i,
             std::array<const char *, 4>{"RATE", "SHAPE", "PHASE", "SMOOTH"}[i],
             accents[0], {58. + 117 * i, 669, 88, 111});
  choice(Lfo1Sync, {59, 780, 87, 26});
  choice(Lfo2Sync, {59, 780, 87, 26});
  pageButton = button(
      "1–4 / 16", {1068, 837, 32, 26},
      [this] {
        matrixPage = (matrixPage + 1) % 4;
        sync();
      },
      2);
  for (unsigned row = 0; row < 4; ++row) {
    auto *s = new MorphCombo(surface);
    for (auto name : sourceNames)
      s->addItem(englishText(QString::fromUtf8(name)));
    s->setAccessibleName(
        QString::fromUtf8("Modulation source %1").arg(row + 1));
    sources[row] = s;
    add(s, {701., 868. + 37 * row, 137, 30}, 2);
    auto *t = new MorphCombo(surface);
    for (unsigned i = 0; i < MatrixStart; ++i)
      if (modulationTarget(i))
        t->addItem(
            englishText(QString::fromStdString(parameterTable()[i].name)), i);
    t->setAccessibleName(
        QString::fromUtf8("Modulation destination %1").arg(row + 1));
    targets[row] = t;
    add(t, {844., 868. + 37 * row, 170, 30}, 2);
    auto *amount = new MorphValueBox(surface);
    amount->setRange(-100, 100);
    amount->setDecimals(0);
    amount->setSuffix("%");
    amount->setAccessibleName(
        QString::fromUtf8("Modulation depth %1").arg(row + 1));
    amounts[row] = amount;
    add(amount, {1020., 868. + 37 * row, 85, 30}, 2);
    for (auto *w : std::array<QWidget *, 3>{s, t, amount}) {
      w->setProperty("morphRouteRow", row);
      w->installEventFilter(q);
    }
    QObject::connect(s, &QComboBox::activated, q, [this, row](int v) {
      auto n = state;
      n.values[matrixSource(matrixPage * 4 + row)] = v;
      apply(std::move(n), QString::fromUtf8("Edit Morph assignment"));
    });
    QObject::connect(t, &QComboBox::activated, q, [this, row, t](int v) {
      auto n = state;
      n.values[matrixTarget(matrixPage * 4 + row)] = t->itemData(v).toUInt();
      apply(std::move(n), QString::fromUtf8("Edit Morph assignment"));
    });
    QObject::connect(amount, &QDoubleSpinBox::valueChanged, q,
                     [this, row](double v) {
                       if (!refreshing)
                         write(matrixAmount(matrixPage * 4 + row), v * .01);
                     });
    QObject::connect(amount, &QDoubleSpinBox::editingFinished, q,
                     [this, row] { end(matrixAmount(matrixPage * 4 + row)); });
  }
  for (unsigned i = 0; i < 3; ++i)
    knob(EnvSensitivity + i,
         std::array<const char *, 3>{"SENS", "ATTACK", "RELEASE"}[i], ink,
         {1145. + 115 * i, 858, 90, 107}, 2);
  env = new PaintWidget(surface);
  add(env, {1148, 977, 207, 53}, 2);
  env->paint = [this](QPainter &p) {
    const QRectF display = QRectF(env->rect()).adjusted(1, 1, -1, -1);
    QLinearGradient glass(display.topLeft(), display.bottomLeft());
    glass.setColorAt(0, QColor("#eef2f6"));
    glass.setColorAt(1, QColor("#fbfcfd"));
    p.setBrush(glass);
    p.setPen(QPen(QColor("#ccd5df"), .7));
    p.drawRoundedRect(display, 4, 4);
    p.setClipRect(display.adjusted(2, 2, -2, -2));
    p.setPen(QPen(QColor(110, 155, 180, 25), 1));
    for (int x = 1; x < 8; ++x)
      p.drawLine(QPointF(x * env->width() / 8., 1),
                 QPointF(x * env->width() / 8., env->height()));
    for (int y = 1; y < 4; ++y)
      p.drawLine(QPointF(1, y * env->height() / 4.),
                 QPointF(env->width(), y * env->height() / 4.));
    QPainterPath path;
    for (unsigned i = 0; i < envelopeHistory.size(); ++i) {
      auto pt = QPointF(
          double(i) * env->width() / (envelopeHistory.size() - 1),
          env->height() *
              (1 -
               std::clamp(
                   envelopeHistory[(envelopeAt + i) % envelopeHistory.size()],
                   0.f, 1.f)));
      if (i)
        path.lineTo(pt);
      else
        path.moveTo(pt);
    }
    auto fill = path;
    fill.lineTo(env->width(), env->height());
    fill.lineTo(0, env->height());
    fill.closeSubpath();
    p.fillPath(fill, QColor(104, 179, 226, 26));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor("#438bb7"), std::max(1., zoom)));
    p.drawPath(path);
  };
  choice(EnvSource, {1365, 992, 126, 30}, {}, 2);
  auto *envDrag = new SourceButton("ENV FOLLOWER", surface);
  envDrag->source = 3;
  envDrag->flat = true;
  envDrag->setAccessibleName(
      QString::fromUtf8("Envelope follower — drag to assign"));
  add(envDrag, {1459, 837, 32, 26}, 2);
  for (unsigned i = 0; i < 4; ++i) {
    knob(Movement + i,
         std::array<const char *, 4>{"MOVEMENT", "TEXTURE", "CHARACTER",
                                     "SPACE"}[i],
         accents[i], {362. + 76 * i, 884, 70, 105}, 2);
    auto *src = new SourceButton("↗", surface);
    src->source = 5 + i;
    src->flat = true;
    src->accent = accents[i == 3 ? 0 : 1];
    src->setAccessibleName(
        QString::fromUtf8("Drag macro %1 to a control").arg(i + 1));
    add(src, {407. + 76 * i, 864, 24, 24}, 2);
  }
  knob(HighPass, "HPF", muted, {567, 667, 89, 115});
  knob(LowPass, "LPF", muted, {681, 667, 89, 115});
  knob(Mix, "DRY/WET", accents[0], {795, 667, 89, 115});
  knob(Width, "WIDTH", muted, {909, 667, 89, 115});
  auto *fx = button("FX +", {932, 633, 94, 28}, [this] {
    finish();
    advanced = !advanced;
    resizeToContent();
  });
  fx->setCheckable(true);
  fx->showCaption = true;
  fx->displayCaption = "Effects";
  fx->setObjectName("morphAdvanced");
  fx->setAccessibleName(QString::fromUtf8("Drive / Echo / Bloom"));
  knob(Drive, "DRIVE", accents[2], {38, 866, 87, 107}, 1);

  choice(DriveMode, {144, 864, 199, 29}, {}, true);
  choice(DrivePosition, {144, 903, 199, 29}, {}, true);
  for (unsigned i = 0; i < 4; ++i)
    knob(EchoLevel + i,
         std::array<const char *, 4>{"ECHO", "TIME", "FEEDBACK", "DAMPING"}[i],
         accents[2], {384. + 94 * i, 866, 83, 103}, true);
  toggle("PING PONG", EchoPingPong, {392, 974, 32, 30}, true);
  choice(EchoSync, {438, 974, 135, 30}, {}, true);
  for (unsigned i = 0; i < 4; ++i)
    knob(BloomLevel + i,
         std::array<const char *, 4>{"BLOOM", "DECAY", "DENSITY", "TONE"}[i],
         accents[3], {786. + 94 * i, 866, 83, 103}, true);
  knob(Breathing, "BREATHING", accents[0], {1198, 866, 94, 107}, true);
  knob(MotionDepth, "MOTION", accents[1], {1299, 866, 94, 107}, true);
  knob(MotionSmooth, "SMOOTH", ink, {1400, 866, 94, 107}, true);
  toggle("REVERSE", MotionReverse, {1380, 977, 32, 28}, true);

  button(
      QString::fromUtf8("Generation locks…"), {144, 947, 32, 30},
      [this] {
        MorphMenu m;
        const std::array<QString, 7> labels{QString::fromUtf8("Modules"),
                                            QString::fromUtf8("Drive"),
                                            QString::fromUtf8("Echo"),
                                            QString::fromUtf8("Bloom"),
                                            QString::fromUtf8("Output / Mix"),
                                            QString::fromUtf8("Morph balance"),
                                            QString::fromUtf8("Motion")};
        for (unsigned i = 0; i < 7; ++i) {
          auto *action = m.addAction(labels[i]);
          action->setCheckable(true);
          action->setChecked(unsigned(state.values[CanvasLocks]) & (1u << i));
          QObject::connect(action, &QAction::triggered, q, [this, i] {
            auto n = state;
            n.values[CanvasLocks] = unsigned(n.values[CanvasLocks]) ^ (1u << i);
            apply(std::move(n), QString::fromUtf8("Change generation locks"));
          });
        }
        m.exec(QCursor::pos());
      },
      true);
  canvasTools.push_back(button(
      QString::fromUtf8("Smooth route"), {1050, 294, 36, 32},
      [this] {
        auto n = state;
        smoothPath(n.canvas);
        apply(std::move(n), QString::fromUtf8("Smooth Morph route"));
      },
      false));
  canvasTools.push_back(button(
      QString::fromUtf8("Clear Canvas"), {1094, 294, 36, 32},
      [this] {
        auto n = state;
        auto path = n.canvas.path;
        auto count = n.canvas.pointCount;
        n.canvas = {};
        n.canvas.path = path;
        n.canvas.pointCount = count;
        apply(std::move(n), QString::fromUtf8("Clear Morph Canvas"));
      },
      false));
  for (auto *tool : canvasTools)
    if (auto *button = dynamic_cast<MorphButton *>(tool))
      button->showCaption = button->glyph != Glyph::Smooth &&
                            button->glyph != Glyph::Clear;
  for (auto &item : items) {
    if (item.widget->toolTip().isEmpty())
      item.widget->setToolTip(item.widget->accessibleName());
    if (auto *button = dynamic_cast<MorphButton *>(item.widget))
      if (button->glyph != Glyph::None && button->accessibleName().isEmpty())
        button->setAccessibleName(button->text());
  }
}

void MorphPanel::Impl::toggleDetails() {
  finish();
  detailsExpanded = !detailsExpanded;
  resizeToContent();
  if (detailsExpanded)
    scroll->ensureWidgetVisible(pad, 0, 12);
}

void MorphPanel::Impl::resizeToContent() {
  background = {};
  layout();
  sync();
  const double preferred = QSettings().value("Morph/zoom", 0.).toDouble();
  const double scale = preferred > 0 ? preferred
                                     : std::clamp(q->width() / 1536., .75, 2.);
  // Request the full height before scrollbars are needed. The host bounds it
  // to the workspace; a constrained editor can still scroll the entire panel.
  emit q->preferredHeightChanged(qRound(
      (864 + moduleHeight() + (detailsExpanded ? 234 : 0) +
       (advanced ? 204 : 0)) * scale));
}

void MorphPanel::Impl::layout() {
  scroll->setGeometry(q->rect());
  const double preferred = QSettings().value("Morph/zoom", 0.).toDouble();
  const unsigned extraHeight =
      moduleHeight() + (detailsExpanded ? 234 : 0) + (advanced ? 204 : 0);
  const bool vertical = (864 + extraHeight) * q->width() / 1536. > q->height();
  zoom = preferred > 0
             ? preferred
             : std::clamp((q->width() - (vertical ? 18 : 0)) / 1536., .75, 2.);
  const QSize size(qRound(1536 * zoom), qRound((864 + extraHeight) * zoom));
  if (surface->size() != size) {
    surface->resize(size);
    background = {};
  }
  for (auto &item : items) {
    auto r = item.r;
    if (item.section == 1)
      r.translate(0, moduleHeight() + (detailsExpanded ? 234 : 0));
    else if (item.section == 2 || (item.section == 0 && r.y() >= 622))
      r.translate(0, moduleHeight());
    bool visible = item.section == 0 || (item.section == 1 && advanced) ||
                   (item.section == 2 && detailsExpanded) ||
                   (item.section >= 3 && moduleExpanded[item.section - 3]);
    if (item.widget->property("morphFlowOnly").toBool())
      visible &= !canvasTab;
    item.widget->setGeometry(QRect(qRound(r.x() * zoom), qRound(r.y() * zoom),
                                   qRound(r.width() * zoom),
                                   qRound(r.height() * zoom)));
    item.widget->setVisible(visible);
    QFont f = morphFont();
    f.setPixelSize(
        std::max(9, qRound((item.widget == topPreset ? 18 : 13) * zoom)));
    item.widget->setFont(f);
  }
  surface->update();
}
void MorphPanel::Impl::cache() {
  background = QPixmap(surface->size() * surface->devicePixelRatioF());
  background.setDevicePixelRatio(surface->devicePixelRatioF());
  background.fill(paper);
  QPainter p(&background);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(zoom, zoom);
  const double height = surface->height() / zoom;
  p.setPen(Qt::NoPen);
  p.setBrush(ink);
  p.drawRoundedRect(QRectF(28, 28, 44, 44), 14, 14);
  p.save();
  p.translate(34, 34);
  p.setPen(QPen(panelWhite, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  drawGlyph(p, Glyph::Flow, false);
  p.restore();
  text(p, {89, 19, 200, 47}, "morph", 38, ink, -1.3,
       Qt::AlignLeft | Qt::AlignVCenter);
  text(p, {225, 34, 180, 30}, "Modulation effect", 13, muted, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  text(p, {1250, 34, 163, 30}, "VLT / AUDIO", 12, muted, 1.8);
  well(p, {26, 95, 1218, 243});
  well(p, {1262, 95, 248, 243});
  text(p, {730, 114, 66, 34}, "Routing", 12, muted, 0);
  text(p, {1285, 114, 127, 28}, "Output", 17, ink, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  for (unsigned i = 0; i < 4; ++i) {
    const double x = 26 + 374. * i;
    well(p, {x, 356, 364, 254. + (moduleExpanded[i] ? 172 : 0)}, accents[i]);
    QColor tint = accents[i];
    tint.setAlpha(22);
    p.setPen(Qt::NoPen);
    p.setBrush(tint);
    p.drawRoundedRect(QRectF(x + 23, 379, 29, 25), 7, 7);
    text(p, {x + 23, 379, 29, 25}, QString("0%1").arg(i + 1), 11,
         accents[i].darker(140));
    text(p, {x + 64, 373, 202, 36},
         std::array<const char *, 4>{"Chorus", "Flanger", "Phaser",
                                     "Doubler"}[i],
         23, ink, -.3, Qt::AlignLeft | Qt::AlignVCenter);
    text(p, {x + 24, 412, 227, 26},
         std::array<const char *, 4>{"Wide & lush", "Sweep & texture",
                                     "Movement & color", "Space & width"}[i],
         13, muted, 0, Qt::AlignLeft | Qt::AlignVCenter);
    if (moduleExpanded[i]) {
      p.setPen(lineColor);
      p.drawLine(QPointF(x + 22, 603), QPointF(x + 342, 603));
    }
  }
  p.save();
  p.translate(0, moduleHeight());
  well(p, {26, 622, 508, 190});
  well(p, {546, 622, 500, 190});
  well(p, {1058, 622, 452, 190});
  text(p, {49, 634, 298, 26}, "Global modulation", 17, ink, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  text(p, {568, 634, 294, 26}, "Tone & width", 17, ink, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  text(p, {1079, 634, 280, 26}, "Gain & level", 17, ink, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  text(p, {1223, 683, 200, 23}, "Auto gain", 15, ink, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  text(p, {1223, 742, 200, 23}, "Soft clip", 15, ink, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  text(p, {1223, 768, 198, 19}, "Round off loud peaks", 11, muted, 0,
       Qt::AlignLeft | Qt::AlignVCenter);
  if (detailsExpanded) {
    well(p, {26, 832, 310, 214});
    well(p, {348, 832, 330, 214});
    well(p, {690, 832, 430, 214});
    well(p, {1132, 832, 378, 214});
    text(p, {45, 837, 265, 24}, "Mode mix", 15);
    text(p, {366, 838, 210, 23}, "Macros", 15);
    text(p, {705, 838, 211, 23}, "Modulation matrix", 15);
    text(p, {1148, 838, 238, 23}, "Envelope follower", 15);
  }
  if (advanced) {
    p.translate(0, detailsExpanded ? 234 : 0);
    well(p, {26, 832, 335, 182});
    well(p, {373, 832, 392, 182}, accents[2]);
    well(p, {777, 832, 392, 182}, accents[3]);
    well(p, {1181, 832, 329, 182});
    text(p, {45, 839, 287, 23}, "Drive", 15);
    text(p, {389, 839, 357, 23}, "Echo", 15);
    text(p, {793, 839, 357, 23}, "Bloom", 15);
    text(p, {1193, 839, 305, 23}, "Motion & breathing", 15);
  }
  p.restore();
  text(p, {31, height - 35, 260, 22}, "MORPH / VLTONE", 10, muted, 1.3,
       Qt::AlignLeft | Qt::AlignVCenter);
  for (unsigned i = 0; i < accents.size(); ++i) {
    p.setPen(Qt::NoPen);
    p.setBrush(accents[i]);
    p.drawEllipse(QPointF(1450 + i * 16, height - 24), 3, 3);
  }
}
void MorphPanel::Impl::write(unsigned id, double v) {
  if (importing) {
    importToken->store(++importGeneration);
    importing = false;
    emit q->imageImportFinished(false);
  }
  if (id >= Count)
    return;
  if (!gestures[id])
    gestures[id] = state.values[id];
  state.values[id] = v;
  controller->setInsertParameter(channel, insert, parameterTable()[id].id, v);
  for (auto *k : knobs)
    if (k->index == id && !k->isEditing()) {
      QSignalBlocker block(k);
      k->setValue(v);
    }
}
void MorphPanel::Impl::end(unsigned id) {
  if (id >= Count || !gestures[id])
    return;
  double before = *gestures[id];
  gestures[id].reset();
  controller->commitInsertParameterEdit(
      channel, insert, parameterTable()[id].id, before,
      "Change Morph " + parameterTable()[id].name);
  if (before != state.values[id])
    emit q->projectEdited();
}
void MorphPanel::Impl::finish() {
  for (auto *k : knobs)
    k->finishEditing();
  for (unsigned i = 0; i < Count; ++i)
    end(i);
  if (drawingBefore) {
    auto old = *drawingBefore;
    drawingBefore.reset();
    apply(state, QString::fromUtf8("Draw Morph Canvas"), true, &old);
  }
  if (padBefore) {
    auto old = *padBefore;
    if (record) {
      if (state.canvas.pointCount == 1) {
        state.canvas.path[1] = state.canvas.path[0];
        state.canvas.path[1].time = .99999f;
        state.canvas.pointCount = 2;
      }
      state.values[MotionOn] = 1;
      record = false;
    }
    padBefore.reset();
    apply(state, QString::fromUtf8("Move Morph balance"), true, &old);
  }
}
bool MorphPanel::Impl::apply(State next, const QString &label, bool history,
                             const State *previous) {
  if (importing) {
    importToken->store(++importGeneration);
    importing = false;
    emit q->imageImportFinished(false);
  }
  if (!controller->applyMorphState(channel, insert, next, label.toStdString(),
                                   history, previous ? previous : &state))
    return false;
  if (state.canvas.rgba != next.canvas.rgba ||
      state.canvas.previewWidth != next.canvas.previewWidth ||
      state.canvas.previewHeight != next.canvas.previewHeight)
    canvasImage = programImage(next.canvas);
  state = std::move(next);
  imageError.clear();
  sync();
  if (history)
    emit q->projectEdited();
  return true;
}
void MorphPanel::Impl::refresh() {
  auto snapshot = controller->morphSnapshot(channel, insert);
  if (!snapshot) {
    timer.stop();
    surface->setEnabled(false);
    return;
  }
  const auto previous = telemetry;
  telemetry = snapshot->telemetry;
  if (!drawingBefore && !padBefore) {
    bool gesture = false;
    for (auto &g : gestures)
      gesture |= g.has_value();
    if (!gesture && (state.values != snapshot->state.values ||
                     state.presetKind != snapshot->state.presetKind ||
                     state.presetName != snapshot->state.presetName ||
                     state.category != snapshot->state.category ||
                     state.canvas != snapshot->state.canvas)) {
      if (state.canvas.rgba != snapshot->state.canvas.rgba ||
          state.canvas.previewWidth != snapshot->state.canvas.previewWidth ||
          state.canvas.previewHeight != snapshot->state.canvas.previewHeight)
        canvasImage = programImage(snapshot->state.canvas);
      state = std::move(snapshot->state);
      sync();
    }
  }
  envelopeHistory[envelopeAt++ % envelopeHistory.size()] = telemetry.envelope;
  envelopeAt %= unsigned(envelopeHistory.size());
  if (telemetry.left != previous.left || telemetry.right != previous.right)
    meters->update();
  if (qRound(200 * std::log10(std::max(.25f, telemetry.autoGain))) !=
      qRound(200 * std::log10(std::max(.25f, previous.autoGain))))
    gainReadout->update();
  if (env->isVisible())
    env->update();
  const bool moved = telemetry.x != previous.x || telemetry.y != previous.y;
  if (pad->isVisible() && moved)
    pad->update();
  if (canvasTab ? moved
                : ((!reduced && telemetry.motion != previous.motion) ||
                   telemetry.weights != previous.weights))
    flow->update();
}
void MorphPanel::Impl::sync() {
  refreshing = true;
  std::array<double, Count> depths{};
  for (unsigned s = 0; s < 16; ++s)
    if (state.values[matrixSource(s)])
      depths[unsigned(state.values[matrixTarget(s)])] +=
          std::abs(state.values[matrixAmount(s)]);
  for (auto *k : knobs) {
    if (!k->isEditing())
      k->setValue(state.values[k->index]);
    if (k->modulation != depths[k->index]) {
      k->modulation = depths[k->index];
      k->update();
    }
  }
  for (auto item : choices)
    item.box->setCurrentIndex(int(std::round(
        state.values[item.id] - parameterTable()[item.id].minValue)));
  for (auto item : toggles)
    item.button->setChecked(state.values[item.id] > .5);
  for (unsigned m = 0; m < 4; ++m)
    moduleButtons[m]->setChecked(moduleExpanded[m]);
  detailsButton->setChecked(detailsExpanded);
  balanceButton->setChecked(detailsExpanded);
  surface->findChild<QPushButton *>("morphAdvanced")->setChecked(advanced);
  for (auto item : choices)
    if (item.id == Lfo1Sync || item.id == Lfo2Sync)
      item.box->setVisible(item.id == (lfo ? Lfo2Sync : Lfo1Sync));
  for (auto *k : knobs)
    if (k->property("morphFlowOnly").toBool())
      k->setVisible(!canvasTab);
  for (unsigned r = 0; r < 4; ++r) {
    unsigned slot = matrixPage * 4 + r;
    sources[r]->setCurrentIndex(int(state.values[matrixSource(slot)]));
    targets[r]->setCurrentIndex(
        targets[r]->findData(unsigned(state.values[matrixTarget(slot)])));
    amounts[r]->setValue(state.values[matrixAmount(slot)] * 100);
  }
  pageButton->setText(
      QString("%1–%2 / 16").arg(matrixPage * 4 + 1).arg(matrixPage * 4 + 4));
  for (const auto [name, id] : std::array<std::pair<const char *, unsigned>, 2>{
           {{"morph.interaction", Interaction},
            {"canvas.influence", CanvasInfluence}}})
    if (auto *spin = surface->findChild<QDoubleSpinBox *>(name))
      spin->setValue(state.values[id] * 100);
  auto title =
      state.presetKind == "factory" && state.presetName == "Init"
          ? QString::fromUtf8("Init")
      : state.presetKind == "custom" && state.presetName == "Randomized"
          ? QString::fromUtf8("Randomized")
      : state.presetKind == "custom" && state.presetName == "Image sound"
          ? QString::fromUtf8("Image sound")
          : QString::fromStdString(state.presetName);
  topPreset->setText(title);

  topPreset->setAccessibleDescription(title);

  star->setEnabled(
      state.presetKind == "user" ||
      (state.presetKind == "factory" && state.presetName != "Init"));
  star->setText(favorites.contains(QString::fromStdString(
                    state.presetKind + ":" + state.presetName))
                    ? QString::fromUtf8("♥")
                    : QString::fromUtf8("♡"));
  pageButton->setToolTip(QString::fromUtf8("Modulation assignments %1–%2 of 16")
                             .arg(matrixPage * 4 + 1)
                             .arg(matrixPage * 4 + 4));
  tabFlow->setChecked(!canvasTab);
  tabCanvas->setChecked(canvasTab);
  for (auto *w : surface->findChildren<QPushButton *>())
    if (auto *source = dynamic_cast<SourceButton *>(w);
        source && source->source <= 2)
      source->setChecked(source->source == lfo + 1);
  for (auto *w : canvasTools)
    w->setVisible(canvasTab);
  const QRectF flowRect = canvasTab ? QRectF(49, 159, 1169, 124)
                                    : QRectF(224, 166, 994, 159);
  flow->setGeometry(qRound(flowRect.x() * zoom), qRound(flowRect.y() * zoom),
                    qRound(flowRect.width() * zoom),
                    qRound(flowRect.height() * zoom));
  recordButton->setChecked(record);
  abButton->setText(sideB ? "A / [B]" : "[A] / B");
  refreshing = false;
  flow->update();
  pad->update();
}
void MorphPanel::Impl::assign(unsigned source, unsigned target) {
  for (unsigned s = 0; s < 16; ++s)
    if (state.values[matrixSource(s)] == 0) {
      auto n = state;
      n.values[matrixSource(s)] = source;
      n.values[matrixTarget(s)] = target;
      n.values[matrixAmount(s)] = .25;
      matrixPage = s / 4;
      apply(std::move(n), QString::fromUtf8("Assign Morph modulation"));
      return;
    }
  QMessageBox::information(
      q, QString::fromUtf8("Morph"),
      QString::fromUtf8("All 16 assignments are in use. Clear a "
                        "source in the matrix first."));
}

void MorphPanel::Impl::paintPad(QPainter &p) {
  p.scale(pad->width() / 316., pad->height() / 209.);
  const QPointF center = padCenter;
  constexpr double r = padRadius;
  p.setPen(QPen(QColor("#cbd5de"), .7));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(center, 96, 96);
  const std::array<QPointF, 4> vertices{
      center + QPointF(0, -r), center + QPointF(-r, 0), center + QPointF(r, 0),
      center + QPointF(0, r)};
  const QPointF base =
      center + QPointF(state.values[MorphX] * r, state.values[MorphY] * r);
  const double projection =
      std::max(1.f, std::abs(telemetry.x) + std::abs(telemetry.y));
  const QPointF actual = center + QPointF(telemetry.x / projection * r,
                                          telemetry.y / projection * r);
  for (unsigned i = 0; i < 4; ++i) {
    unsigned next = std::array<unsigned, 4>{2, 0, 3, 1}[i];
    QPainterPath wedge;
    wedge.moveTo(center);
    wedge.lineTo(vertices[i]);
    wedge.lineTo(vertices[next]);
    wedge.closeSubpath();
    QRadialGradient g(center, r);
    QColor color = accents[i];
    color.setAlpha(75);
    g.setColorAt(0, color);
    color.setAlpha(10);
    g.setColorAt(1, color);
    p.setBrush(g);
    p.setPen(QPen(accents[i].darker(150), .8));
    p.drawPath(wedge);
    p.setPen(QPen(accents[i], .8));
    p.drawLine(base, vertices[i]);
    p.setBrush(accents[i]);
    p.drawEllipse(vertices[i], 2, 2);
  }
  text(p, {111, 0, 94, 23}, "CHORUS", 13);
  text(p, {0, 94, 74, 25}, "FLANGER", 12);
  text(p, {242, 94, 74, 25}, "PHASER", 12);
  text(p, {113, 186, 90, 22}, "DOUBLER", 13);
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(QColor(65, 101, 137, 170), 1.3, Qt::DashLine));
  p.drawEllipse(actual, 9, 9);
  QRadialGradient glow(base, 22);
  glow.setColorAt(0, QColor(144, 223, 255, 125));
  glow.setColorAt(1, Qt::transparent);
  p.setPen(Qt::NoPen);
  p.setBrush(glow);
  p.drawEllipse(base, 22, 22);
  p.setBrush(QColor("#ffffff"));
  p.setPen(QPen(accents[0], 1.3));
  p.drawEllipse(base, 7, 7);
  if (pad->hasFocus()) {
    p.setPen(QPen(ink, 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(1, 1, 314, 206), 5, 5);
  }
}
void MorphPanel::Impl::paintFlow(QPainter &p) {
  const double w = flow->width(), h = flow->height();
  if (canvasTab) {
    QRectF imageRect(0, 0, w, h);
    if (!canvasImage.isNull()) {
      const QSizeF fit =
          canvasImage.size().scaled(int(w), int(h), Qt::KeepAspectRatio);
      imageRect = QRectF((w - fit.width()) * .5, (h - fit.height()) * .5,
                         fit.width(), fit.height());
      p.setRenderHint(QPainter::SmoothPixmapTransform);
      p.drawImage(imageRect, canvasImage);
      p.fillRect(imageRect, QColor(0, 0, 0, 30));
    } else {
      p.fillRect(flow->rect(), QColor("#edf2f6"));
      text(p, flow->rect(),
           QString::fromUtf8("Paint a color • Draw a route • Drop a photo"),
           std::max(10, int(14 * zoom)), muted);
    }
    const auto point = [&](float x, float y) {
      return QPointF(imageRect.center().x() + x * imageRect.width() * .5,
                     imageRect.center().y() + y * imageRect.height() * .5);
    };
    QPainterPath path;
    for (unsigned i = 0; i < state.canvas.pointCount; ++i) {
      auto pt = point(state.canvas.path[i].x, state.canvas.path[i].y);
      if (i)
        path.lineTo(pt);
      else
        path.moveTo(pt);
      if (!brush) {
        p.setBrush(i == unsigned(selectedPoint) ? accents[3] : ink);
        p.setPen(Qt::NoPen);
        p.drawEllipse(pt, 2.6, 2.6);
      }
    }
    if (state.canvas.pointCount > 1)
      path.closeSubpath();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 160), 3));
    p.drawPath(path);
    p.setPen(QPen(QColor(206, 238, 255, 210), 1));
    p.drawPath(path);
    auto at = point(telemetry.x, telemetry.y);
    p.setBrush(QColor("#edfaff"));
    p.setPen(QPen(accents[0], 2));
    p.drawEllipse(at, 4.5, 4.5);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ink, 1, Qt::DashLine));
    p.drawEllipse(
        point(float(state.values[MorphX]), float(state.values[MorphY])), 6, 6);
  } else {
    // Expensive shape math is invariant. Only the phase and contribution
    // change during playback; text and controls retain native DPI resolution.
    static const auto basis = [] {
      std::array<std::array<std::array<QPointF, 65>, 12>, 4> result;
      for (unsigned m = 0; m < 4; ++m)
        for (unsigned strand = 0; strand < 12; ++strand)
          for (unsigned i = 0; i <= 64; ++i) {
            const double x = i / 64., center = .14 + .24 * m;
            const double fan = strand / 11.;
            const double shape = std::exp(-std::pow((x - center) / .27, 2)) *
                                 std::array<double, 4>{.40, .48, .47, .28}[m] *
                                 (1 - fan * .64);
            const double angle = (x - center) * 12 + fan * .8;
            result[m][strand][i] = {shape * std::cos(angle),
                                    shape * std::sin(angle)};
          }
      return result;
    }();
    std::array<int, 13> key{};
    key[12] = reduced;
    for (unsigned m = 0; m < 4; ++m) {
      key[m * 3] = reduced ? 0 : qRound(telemetry.motion[m] * 200);
      key[m * 3 + 1] = qRound(telemetry.weights[m] * 100);
      key[m * 3 + 2] = state.values[moduleOffsets[m]] > .5;
    }
    // Bound decorative raster work at high DPI. Never downsample controls,
    // values, the imported image or its editing handles.
    const double rasterScale = std::min(1., 960. / std::max(1., w));
    const QSize rasterSize(qCeil(w * rasterScale), qCeil(h * rasterScale));
    if (flowWaves.size() != rasterSize || key != flowKey) {
      flowKey = key;
      flowWaves = QImage(rasterSize, QImage::Format_ARGB32_Premultiplied);
      flowWaves.fill(Qt::transparent);
      QPainter waves(&flowWaves);
      waves.setRenderHint(QPainter::Antialiasing);
      waves.scale(rasterScale, rasterScale);
      for (unsigned m = 0; m < 4; ++m) {
        const bool enabled = key[m * 3 + 2];
        const double phase = key[m * 3] / 200. * .35;
        const double strength =
            .82 + .18 * std::clamp(key[m * 3 + 1] / 100., 0., 1.);
        const double cosine = std::cos(phase) * strength;
        const double sine = std::sin(phase) * strength;
        QPainterPath outside, inside;
        const unsigned strands = reduced ? 4 : 12;
        for (unsigned strand = 0; strand < strands; ++strand) {
          QPainterPath curve;
          const auto &points = basis[m][strand * 11 / (strands - 1)];
          for (unsigned i = 0; i <= 64; ++i) {
            const double y =
                h * (.56 - points[i].x() * cosine + points[i].y() * sine);
            if (i)
              curve.lineTo(i * w / 64., y);
            else
              curve.moveTo(0, y);
          }
          QColor color = accents[m];
          color.setAlpha(enabled ? (strand == 0 ? 220 : 35) : 18);
          waves.setPen(QPen(color, (strand == 0 ? 1.8 : .7) * zoom));
          waves.setBrush(Qt::NoBrush);
          waves.drawPath(curve);
          if (!strand)
            outside = curve;
          if (strand == strands - 1)
            inside = curve;
        }
        outside.connectPath(inside.toReversed());
        outside.closeSubpath();
        QColor tint = accents[m];
        tint.setAlpha(enabled ? 16 : 3);
        waves.fillPath(outside, tint);
      }
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRectF(0, 0, w, h), flowWaves);
    for (unsigned m = 0; m < 4; ++m) {
      const bool enabled = state.values[moduleOffsets[m]] > .5;
      const QPointF at((.14 + .24 * m) * w, h * .80);
      p.setPen(Qt::NoPen);
      QColor c = enabled ? accents[m] : QColor("#b9c1b6");
      p.setBrush(c);
      p.drawEllipse(at, 4 * zoom, 4 * zoom);
      text(p, {at.x() - 80 * zoom, h * .87, 160 * zoom, 22 * zoom},
           std::array<const char *, 4>{"Chorus", "Flanger", "Phaser",
                                       "Doubler"}[m],
           std::max(9, qRound(13 * zoom)), muted);
    }
  }
  if (flow->hasFocus()) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ink, 1, Qt::DashLine));
    p.drawRect(flow->rect().adjusted(1, 1, -2, -2));
  }
  if (canvasTab && !imageError.isEmpty()) {
    p.fillRect(QRectF(0, 0, w, 30 * zoom), QColor(255, 235, 228, 245));
    text(p, {5, 0, w - 10, 30 * zoom}, imageError, std::max(10, int(12 * zoom)),
         QColor("#9a4029"));
  }
}
void MorphPanel::Impl::padPointer(QMouseEvent *e, int phase) {
  if (e->button() != Qt::LeftButton && !(e->buttons() & Qt::LeftButton))
    return;
  if (phase == 0) {
    padBefore = state;
    gestureClock.start();
    if (record) {
      state.canvas.pointCount = 0;
      state.canvas.recorded = true;
      // The held point owns the sound until release. Playing the growing
      // route here would make recording fight the user's hand.
      state.values[MotionOn] = 0;
    }
  }
  if (!padBefore)
    return;
  double x = (e->position().x() / pad->width() * 316 - padCenter.x()) /
             padRadius,
         y = (e->position().y() / pad->height() * 209 - padCenter.y()) /
             padRadius;
  const double scale = std::max(1., std::abs(x) + std::abs(y));
  x /= scale;
  y /= scale;
  state.values[MorphX] = x;
  state.values[MorphY] = y;
  if (record) {
    auto &c = state.canvas;
    const double beatsPerBar = 4. * controller->timeSigNumerator() /
                               std::max(1, controller->timeSigDenominator());
    const double seconds =
        (beatsPerBar * (1u << unsigned(state.values[MotionBars]))) * 60 /
        std::max(1., controller->tempo());
    const float t =
        float(std::clamp(gestureClock.elapsed() / 1000. / seconds, 0., .99999));
    if (c.pointCount == 0 || t > c.path[c.pointCount - 1].time + .001)
      appendPoint(c, float(x), float(y), t);
  }
  if (phase == 2) {
    if (record) {
      auto &c = state.canvas;
      if (c.pointCount == 1)
        c.path[c.pointCount++] = {float(x), float(y), .99999f};
      state.values[MotionOn] = 1;
      record = false;
    }
    auto before = *padBefore;
    padBefore.reset();
    apply(state, QString::fromUtf8("Record Morph motion"), true, &before);
  } else
    apply(state, QString::fromUtf8("Move Morph balance"), false);
  pad->update();
}
void MorphPanel::Impl::canvasPointer(QMouseEvent *e, int phase) {
  if (!canvasTab ||
      (!(e->buttons() & Qt::LeftButton) && e->button() != Qt::LeftButton))
    return;
  QRectF r = flow->rect();
  if (!canvasImage.isNull()) {
    const QSizeF size =
        canvasImage.size().scaled(flow->size(), Qt::KeepAspectRatio);
    r = QRectF((flow->width() - size.width()) * .5,
               (flow->height() - size.height()) * .5, size.width(),
               size.height());
  }
  const QPointF n(
      std::clamp((e->position().x() - r.left()) / r.width(), 0., 1.),
      std::clamp((e->position().y() - r.top()) / r.height(), 0., 1.));
  if (phase == 0) {
    drawingBefore = state;
    selectedPoint = -1;
    lastBrush = n;
    if (!brush) {
      double best = .035;
      for (unsigned i = 0; i < state.canvas.pointCount; ++i) {
        auto &pt = state.canvas.path[i];
        double dist =
            std::hypot((pt.x + 1) * .5 - n.x(), (pt.y + 1) * .5 - n.y());
        if (dist < best) {
          best = dist;
          selectedPoint = int(i);
        }
      }
      if (selectedPoint < 0) {
        state.canvas.pointCount = 0;
        state.canvas.recorded = false;
      }
    }
  }
  if (!drawingBefore)
    return;
  if (brush) {
    if (canvasImage.isNull()) {
      canvasImage = QImage(128, 128, QImage::Format_ARGB32);
      canvasImage.fill(QColor("#808080"));
    }
    QPainter paint(&canvasImage);
    paint.setRenderHint(QPainter::Antialiasing);
    QColor color = brushColor;
    color.setAlphaF(brushOpacity);
    paint.setPen(
        QPen(color,
             brushRadius * std::min(canvasImage.width(), canvasImage.height()),
             Qt::SolidLine, Qt::RoundCap));
    const auto pixel = [&](QPointF p) {
      return QPointF(p.x() * canvasImage.width(), p.y() * canvasImage.height());
    };
    paint.drawLine(pixel(lastBrush), pixel(n));
    paint.end();
    lastBrush = n;
    storeImage(state.canvas, canvasImage);
    state.values[CanvasOn] = 1;
  } else {
    auto &c = state.canvas;
    if (selectedPoint >= 0) {
      auto &pt = c.path[unsigned(selectedPoint)];
      pt.x = float(n.x() * 2 - 1);
      pt.y = float(n.y() * 2 - 1);
    } else {
      appendPoint(c, float(n.x() * 2 - 1), float(n.y() * 2 - 1), 0);
      for (unsigned i = 0; i < c.pointCount; ++i)
        c.path[i].time =
            c.pointCount > 1 ? float(i) / float(c.pointCount - 1) : 0;
    }
    state.values[MotionOn] = 1;
  }
  if (phase == 2) {
    if (!brush && !state.canvas.recorded) {
      auto &c = state.canvas;
      double length = 0;
      for (unsigned i = 0; i < c.pointCount; ++i) {
        if (i)
          length += std::hypot(c.path[i].x - c.path[i - 1].x,
                               c.path[i].y - c.path[i - 1].y);
        c.path[i].time = float(length);
      }
      if (length > 1.e-8)
        for (unsigned i = 0; i < c.pointCount; ++i)
          c.path[i].time /= float(length);
      else
        for (unsigned i = 0; i < c.pointCount; ++i)
          c.path[i].time = c.pointCount > 1 ? float(i) / (c.pointCount - 1) : 0;
    }
    auto before = *drawingBefore;
    drawingBefore.reset();
    apply(state, QString::fromUtf8("Draw Morph Canvas"), true, &before);
  } else
    apply(state, QString::fromUtf8("Draw Morph Canvas"), false);
  flow->update();
}
void MorphPanel::Impl::brushSettings() {
  MorphMenu m;
  auto *colors = m.addMenu(QString::fromUtf8("Effect color"));
  for (unsigned i = 0; i < 4; ++i) {
    auto *a = colors->addAction(englishText(std::array<const char *, 4>{
        "Chorus", "Flanger", "Phaser", "Doubler"}[i]));
    QObject::connect(a, &QAction::triggered, q,
                     [this, i] { brushColor = accents[i]; });
  }
  m.addAction(QString::fromUtf8("Custom color…"), q, [this] {
    auto color = QColorDialog::getColor(brushColor, q);
    if (color.isValid())
      brushColor = color;
  });
  m.addAction(QString::fromUtf8("Brush size…"), q, [this] {
    bool ok = false;
    double v = QInputDialog::getDouble(q, QString::fromUtf8("Brush size"), "%",
                                       brushRadius * 100, 1, 60, 0, &ok);
    if (ok)
      brushRadius = v * .01;
  });
  m.addAction(QString::fromUtf8("Opacity…"), q, [this] {
    bool ok = false;
    double v = QInputDialog::getDouble(q, QString::fromUtf8("Opacity"), "%",
                                       brushOpacity * 100, 1, 100, 0, &ok);
    if (ok)
      brushOpacity = v * .01;
  });
  m.exec(QCursor::pos());
}
void MorphPanel::Impl::pathMenu() {
  MorphMenu m;
  m.addAction(QString::fromUtf8("Draw / edit route"), q, [this] {
    brush = false;
    flow->setFocus();
    flow->update();
  });
  m.addAction(QString::fromUtf8("Smooth route"), q, [this] {
    auto n = state;
    smoothPath(n.canvas);
    apply(std::move(n), QString::fromUtf8("Smooth Morph route"));
  });
  m.addAction(QString::fromUtf8("Reverse"), q, [this] {
    auto n = state;
    n.values[MotionReverse] = 1 - n.values[MotionReverse];
    apply(std::move(n), QString::fromUtf8("Reverse Morph route"));
  });
  m.addAction(QString::fromUtf8("Orbit"), q, [this] {
    auto n = state;
    makeOrbit(n.canvas);
    n.values[MotionOn] = 1;
    apply(std::move(n), QString::fromUtf8("Draw Morph orbit"));
  });
  m.addAction(QString::fromUtf8("Delete selected point"), q, [this] {
    if (selectedPoint < 0 || state.canvas.pointCount <= 2)
      return;
    auto n = state;
    for (unsigned i = unsigned(selectedPoint); i + 1 < n.canvas.pointCount; ++i)
      n.canvas.path[i] = n.canvas.path[i + 1];
    --n.canvas.pointCount;
    selectedPoint = -1;
    apply(std::move(n), QString::fromUtf8("Edit Morph route"));
  });
  m.exec(QCursor::pos());
}

void MorphPanel::Impl::importPhoto(const QString &path) {
  const auto generation = ++importGeneration;
  importToken->store(generation);
  const auto token = importToken;
  importing = true;
  imageError.clear();
  const auto identity = controller->insertIdentity(channel, insert);
  const auto program = state.canvas;
  QPointer<MorphPanel> guard(q);
  // Bound decoding across editors and skip obsolete queued requests.
  static auto *imagePool = [] {
    auto *pool = new QThreadPool(qApp);
    pool->setMaxThreadCount(1);
    return pool;
  }();
  imagePool->start([guard, generation, token, path, program,
                    identity]() mutable {
    if (token->load() != generation)
      return;
    auto result = std::make_shared<MorphCanvasProgram>(program);
    QString error;
    QFileInfo file(path);
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto format = reader.format().toLower();
    const auto dimensions = reader.size();
    if (!file.isFile() || file.size() > 20 * 1024 * 1024 ||
        !QStringList{"png", "jpeg", "jpg", "webp"}.contains(
            QString::fromLatin1(format)))
      error = QString::fromUtf8(
          "Use a PNG, JPEG or WebP image smaller than 20 MiB.");
    else if (!dimensions.isValid() ||
             qint64(dimensions.width()) * dimensions.height() >
                 16 * 1024 * 1024)
      error = QString::fromUtf8(
          "The decoded image must be no larger than 16 megapixels.");
    else {
      reader.setScaledSize(dimensions.scaled(128, 128, Qt::KeepAspectRatio));
      auto im = reader.read();
      if (im.isNull())
        error = QString::fromUtf8("The image could not be decoded.");
      else {
        im = im.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                 .convertToFormat(QImage::Format_ARGB32);
        storeImage(*result, im);
        result->seed = canvasFingerprint(*result);
        if (result->pointCount < 2)
          makeOrbit(*result, result->seed % 3);
      }
    }
    // Application lifetime is longer than jobs; only dereference the panel
    // on its GUI thread, after checking the weak QObject and request token.
    QMetaObject::invokeMethod(
        qApp,
        [guard, generation, result, error, identity] {
          if (!guard || guard->d->importGeneration != generation)
            return;
          auto &d = *guard->d;
          d.importing = false;
          if (d.controller->insertIdentity(d.channel, d.insert).instance !=
              identity.instance) {
            emit guard->imageImportFinished(false);
            return;
          }
          if (!error.isEmpty()) {
            d.imageError = error;
            d.canvasTab = true;
            d.sync();
            guard->setToolTip(error);
            emit guard->imageImportFinished(false);
            return;
          }
          auto n = d.state;
          n.canvas = *result;
          n.values = interpretCanvas(n.canvas, n.values);
          if (n.canvas.painted) {
            n.presetKind = "custom";
            n.presetName = "Image sound";
            n.category.clear();
          }
          d.canvasTab = true;
          bool ok =
              d.apply(std::move(n), QString::fromUtf8("Import Morph image"));
          emit guard->imageImportFinished(ok);
        },
        Qt::QueuedConnection);
  });
}
void MorphPanel::Impl::presetMenu(QWidget *anchor) {
  MorphMenu m;
  auto *factory = m.addMenu(QString::fromUtf8("Factory"));
  QString last;
  QMenu *category = nullptr;
  for (unsigned i = 0; i < factoryPresets().size(); ++i) {
    auto preset = factoryPresets()[i];
    const QString group = englishText(QString::fromUtf8(preset.category));
    if (group != last) {
      category = factory->addMenu(group);
      last = group;
    }
    auto *action = category->addAction(QString::fromUtf8(preset.name));
    action->setCheckable(true);
    action->setChecked(state.presetName == preset.name &&
                       state.presetKind == "factory");
    QObject::connect(action, &QAction::triggered, q,
                     [this, i] { q->applyFactoryPreset(i); });
  }
  auto *fav = m.addMenu(QString::fromUtf8("Favorites"));
  for (unsigned i = 0; i < factoryPresets().size(); ++i)
    if (favorites.contains("factory:" +
                           QString::fromUtf8(factoryPresets()[i].name)))
      fav->addAction(QString::fromUtf8(factoryPresets()[i].name), q,
                     [this, i] { q->applyFactoryPreset(i); });
  for (auto entry : users()) {
    const auto item = entry.toObject();
    if (favorites.contains("user:" + item["name"].toString()))
      fav->addAction(item["name"].toString(), q, [this, item] {
        const auto bytes =
            QByteArray::fromBase64(item["state"].toString().toLatin1());
        if (auto n = decodeState(
                {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                 size_t(bytes.size())}))
          apply(std::move(*n), QString::fromUtf8("Load Morph preset"));
      });
  }
  fav->setEnabled(!fav->isEmpty());
  auto *user = m.addMenu(QString::fromUtf8("User presets"));
  userMenu(user);
  m.addSeparator();
  m.addAction(QString::fromUtf8("Save user preset…"), q,
              [this] { saveUser(); });
  m.addAction(QString::fromUtf8("Import .vltmorph…"), q, [this] {
    const auto path = QFileDialog::getOpenFileName(
        q, QString::fromUtf8("Import Morph preset"), {}, "Morph (*.vltmorph)");
    if (path.isEmpty())
      return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 2 * 1024 * 1024) {
      QMessageBox::warning(q, "Morph",
                           QString::fromUtf8("Invalid Morph preset."));
      return;
    }
    auto bytes = f.readAll();
    auto n =
        decodeState({reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                     size_t(bytes.size())});
    if (!n) {
      QMessageBox::warning(q, "Morph",
                           QString::fromUtf8("Invalid Morph preset."));
      return;
    }
    n->presetKind = "user";
    auto entries = users();
    std::vector<std::uint8_t> encoded;
    const QString original = QString::fromStdString(n->presetName);
    QString name = original;
    auto exists = [&entries](const QString &candidate) {
      for (auto entry : entries)
        if (entry.toObject()["name"].toString() == candidate)
          return true;
      return false;
    };
    for (unsigned suffix = 2; exists(name); ++suffix)
      name = original + QString(" (%1)").arg(suffix);
    n->presetName = name.toStdString();
    encodeState(*n, encoded);
    QJsonObject item;
    item["name"] = name;
    item["category"] = QString::fromStdString(n->category);
    item["state"] = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(encoded.data()),
                   int(encoded.size()))
            .toBase64());
    entries.append(item);
    persistUsers(entries);
    apply(std::move(*n), QString::fromUtf8("Import Morph preset"));
  });
  m.addAction(QString::fromUtf8("Export .vltmorph…"), q, [this] {
    auto path = QFileDialog::getSaveFileName(
        q, QString::fromUtf8("Export Morph preset"),
        QString::fromStdString(state.presetName) + ".vltmorph",
        "Morph (*.vltmorph)");
    if (path.isEmpty())
      return;
    if (!path.endsWith(".vltmorph", Qt::CaseInsensitive))
      path += ".vltmorph";
    std::vector<std::uint8_t> bytes;
    if (!encodeState(state, bytes))
      return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(reinterpret_cast<const char *>(bytes.data()),
                qint64(bytes.size())) != qint64(bytes.size()) ||
        !f.commit())
      QMessageBox::warning(q, "Morph",
                           QString::fromUtf8("The preset could not be saved."));
  });
  m.addSeparator();
  m.addAction(QString::fromUtf8("Init"), q,
              [this] { q->applyFactoryPreset(30); });
  m.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}
void MorphPanel::Impl::saveUser() {
  bool ok = false;
  QString name =
      QInputDialog::getText(q, QString::fromUtf8("Save Morph preset"),
                            QString::fromUtf8("Name"), QLineEdit::Normal,
                            QString::fromStdString(state.presetName), &ok)
          .trimmed();
  if (!ok || name.isEmpty())
    return;
  QString category =
      QInputDialog::getText(q, QString::fromUtf8("Preset category"),
                            QString::fromUtf8("Category"), QLineEdit::Normal,
                            QString::fromStdString(state.category), &ok)
          .trimmed();
  if (!ok)
    return;
  auto n = state;
  n.presetName = name.toStdString();
  n.presetKind = "user";
  n.category = category.toStdString();
  std::vector<std::uint8_t> bytes;
  if (!encodeState(n, bytes))
    return;
  QJsonObject item;
  item["name"] = name;
  item["category"] = category;
  item["state"] = QString::fromLatin1(
      QByteArray(reinterpret_cast<const char *>(bytes.data()),
                 int(bytes.size()))
          .toBase64());
  auto entries = users();
  bool replaced = false;
  for (int i = 0; i < entries.size(); ++i)
    if (entries[i].toObject()["name"].toString() == name) {
      entries[i] = item;
      replaced = true;
      break;
    }
  if (!replaced)
    entries.append(item);
  persistUsers(entries);
  apply(std::move(n), QString::fromUtf8("Save Morph preset"));
}
void MorphPanel::Impl::userMenu(QMenu *menu) {
  auto entries = users();
  if (entries.empty()) {
    menu->addAction(QString::fromUtf8("No user presets"))->setEnabled(false);
    return;
  }
  for (int i = 0; i < entries.size(); ++i) {
    const auto item = entries[i].toObject();
    const auto name = item["name"].toString();
    auto *submenu = menu->addMenu(item["category"].toString() + " / " + name);
    const auto load = [item]() {
      auto bytes = QByteArray::fromBase64(item["state"].toString().toLatin1());
      return decodeState(
          {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
           size_t(bytes.size())});
    };
    submenu->addAction(QString::fromUtf8("Load"), q, [this, load] {
      if (auto n = load())
        apply(std::move(*n), QString::fromUtf8("Load Morph preset"));
    });
    auto *favorite = submenu->addAction(QString::fromUtf8("Favorite"));
    favorite->setCheckable(true);
    favorite->setChecked(favorites.contains("user:" + name));
    QObject::connect(favorite, &QAction::triggered, q, [this, name] {
      if (!favorites.removeOne("user:" + name))
        favorites << "user:" + name;
      QSettings().setValue("Morph/favorites", favorites);
      sync();
    });
    submenu->addAction(QString::fromUtf8("Rename…"), q, [this, name, i, load] {
      auto n = load();
      if (!n)
        return;
      bool ok = false;
      auto renamed =
          QInputDialog::getText(q, QString::fromUtf8("Rename preset"),
                                QString::fromUtf8("Name"), QLineEdit::Normal,
                                name, &ok)
              .trimmed();
      if (!ok || renamed.isEmpty())
        return;
      auto entries = users();
      for (auto e : entries)
        if (e.toObject()["name"].toString() == renamed)
          return;
      n->presetName = renamed.toStdString();
      std::vector<std::uint8_t> bytes;
      encodeState(*n, bytes);
      auto item = entries[i].toObject();
      item["name"] = renamed;
      item["state"] = QString::fromLatin1(
          QByteArray(reinterpret_cast<const char *>(bytes.data()),
                     int(bytes.size()))
              .toBase64());
      entries[i] = item;
      persistUsers(entries);
      if (favorites.removeOne("user:" + name)) {
        favorites << "user:" + renamed;
        QSettings().setValue("Morph/favorites", favorites);
      }
      if (state.presetKind == "user" && state.presetName == name.toStdString())
        apply(std::move(*n), QString::fromUtf8("Rename Morph preset"));
    });
    submenu->addAction(QString::fromUtf8("Delete"), q, [this, i, name] {
      auto entries = users();
      entries.removeAt(i);
      persistUsers(entries);
      favorites.removeOne("user:" + name);
      QSettings().setValue("Morph/favorites", favorites);
      sync();
    });
  }
}
void MorphPanel::Impl::settingsMenu() {
  MorphMenu m;
  auto *reduce = m.addAction(QString::fromUtf8("Reduce motion"));
  reduce->setCheckable(true);
  reduce->setChecked(reduced);
  QObject::connect(reduce, &QAction::triggered, q, [this](bool checked) {
    reduced = checked;
    QSettings().setValue("Morph/reducedMotion", reduced);
    flow->update();
  });
  auto *size = m.addMenu(QString::fromUtf8("Interface scale"));
  for (double scale : {0., .75, 1., 1.5, 2.})
    size->addAction(scale ? QString::number(scale * 100) + "%"
                          : QString::fromUtf8("Fit width"),
                    q, [this, scale] {
                      QSettings().setValue("Morph/zoom", scale);
                      resizeToContent();
                    });
  m.addAction(QString::fromUtf8("Copy A → B"), q, [this] {
    if (!sideB)
      a = state;
    b = a;
  });
  m.addAction(QString::fromUtf8("Copy B → A"), q, [this] {
    if (sideB)
      b = state;
    a = b;
  });
  m.addAction(QString::fromUtf8("Drive / Echo / Bloom"), q, [this] {
    finish();
    advanced = !advanced;
    resizeToContent();
  });
  m.addAction(QString::fromUtf8("Modulation and macros"), q,
              [this] { toggleDetails(); });
  m.exec(QCursor::pos());
}
MorphPanel::MorphPanel(daw::EngineController *c, QString channel,
                       QString insert, QWidget *parent)
    : QWidget(parent), d(std::make_unique<Impl>(this, c, std::move(channel),
                                                std::move(insert))) {
  setObjectName("MorphPanel");
  setAccessibleName(QStringLiteral("Morph modulation effect"));
  setMinimumSize(800, 450);
  d->layout();
  d->sync();
}
MorphPanel::~MorphPanel() {
  d->importToken->store(++d->importGeneration);
  d->finish();
}
bool MorphPanel::eventFilter(QObject *object, QEvent *event) {
  const auto row = object->property("morphRouteRow");
  if (row.isValid() &&
      (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
       event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
    const bool active =
        event->type() == QEvent::Enter || event->type() == QEvent::FocusIn;
    const unsigned slot = d->matrixPage * 4 + row.toUInt();
    const unsigned target = unsigned(d->state.values[matrixTarget(slot)]),
                   source = unsigned(d->state.values[matrixSource(slot)]);
    for (auto *k : d->knobs) {
      k->highlighted = active && source && k->index == target;
      k->update();
    }
    for (auto *w : d->surface->findChildren<QPushButton *>())
      if (auto *button = dynamic_cast<SourceButton *>(w)) {
        button->highlighted = active && source == button->source;
        button->update();
      }
  }
  return QWidget::eventFilter(object, event);
}
void MorphPanel::resizeEvent(QResizeEvent *e) {
  QWidget::resizeEvent(e);
  d->layout();
  d->sync();
}
void MorphPanel::showEvent(QShowEvent *e) {
  QWidget::showEvent(e);
  d->refresh();
  d->timer.start();
}
void MorphPanel::hideEvent(QHideEvent *e) {
  d->finish();
  d->timer.stop();
  QWidget::hideEvent(e);
}
bool MorphPanel::applyFactoryPreset(unsigned index) {
  d->finish();
  return d->apply(factoryState(index), QStringLiteral("Load Morph preset"));
}
void MorphPanel::importImage(const QString &path) { d->importPhoto(path); }
bool MorphPanel::imageImportPending() const { return d->importing; }
bool MorphPanel::visualUpdatesActive() const { return d->timer.isActive(); }
State MorphPanel::currentState() const { return d->state; }
