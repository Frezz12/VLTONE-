#include "NativePluginView.hpp"
#include "Controls.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/DelayInstance.hpp"
#include "Internal/ModulationRackInstance.hpp"
#include "Internal/PitchCorrectorInstance.hpp"
#include "PluginStyle.hpp"
#include "Theme.hpp"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyleFactory>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <memory>
#include <numbers>
#include <utility>

namespace {
using Rack = daw::plugins::modulation::ModulationRackInstance;
constexpr unsigned rackEq(unsigned band, unsigned field) {
  return Rack::eqOffset + band * 4 + field;
}
constexpr double pi = std::numbers::pi;
const QStringList moduleNames{"Chorus", "Doubler", "Flanger", "Phaser",
                              "Doubler Pro"};
const QStringList moduleCaptions{"LAYERED VOICES", "HUMAN DOUBLES",
                                 "COMB MOTION", "PHASE ROTATION",
                                 "VOCAL DIMENSION"};
const std::array<QColor, 5> moduleColors{QColor("#9779e7"), QColor("#d45a9b"),
                                         QColor("#3cbb97"), QColor("#e3a448"),
                                         QColor("#d45a9b")};

QLabel *label(const QString &text, QWidget *parent, int size = 11) {
  auto *l = new QLabel(text, parent);
  auto f = l->font();
  f.setPixelSize(size);
  l->setFont(f);
  const auto restyle = [l, size] {
    l->setStyleSheet(
        QStringLiteral("font-size: %1px; color: %2; background: transparent;")
            .arg(size)
            .arg(th().textPrimary.name()));
  };
  QObject::connect(&ThemeManager::instance(), &ThemeManager::changed, l,
                   restyle);
  restyle();
  l->setAlignment(Qt::AlignCenter);
  l->setTextFormat(Qt::PlainText);
  return l;
}

class Button : public QAbstractButton {
public:
  Button(const QString &text, QWidget *parent) : QAbstractButton(parent) {
    setText(text);
    setAccessibleName(text);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumHeight(28);
  }
  QSize sizeHint() const override {
    return {fontMetrics().horizontalAdvance(text()) + 24, 30};
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (property("pianoKey").toBool()) {
      const bool black = text().contains('#');
      QLinearGradient key(0, 0, width(), height());
      const QColor base =
          black ? th().well()
                : mixColors(th().textPrimary, pluginStyle::shell(), .15);
      key.setColorAt(0, th().edgeLight(base));
      key.setColorAt(.7, base);
      key.setColorAt(1, th().edgeDark(base));
      p.setPen(QPen(hasFocus() ? pluginStyle::accent() : th().separator(),
                    hasFocus() ? 2 : 1));
      p.setBrush(key);
      p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -2), 4, 4);
      if (!isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 85));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -2), 4, 4);
      }
      if (property("targetNote").toBool())
        p.fillRect(QRect(5, height() - 6, width() - 10, 3),
                   pluginStyle::accent());
      auto f = font();
      f.setStrikeOut(!isChecked());
      f.setPixelSize(10);
      p.setFont(f);
      p.setPen(black ? th().textPrimary : th().background);
      p.drawText(rect().adjusted(0, 0, 0, -9),
                 Qt::AlignHCenter | Qt::AlignBottom, text());
      return;
    }
    p.setOpacity(isEnabled() ? 1 : .45);
    pluginStyle::surface(p, QRectF(rect()).adjusted(1, 1, -1, -3),
                         isDown() || isChecked(), 8);
    if (hasFocus() || isChecked()) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(pluginStyle::accent(), hasFocus() ? 2 : 1));
      p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -4), 8, 8);
    }
    p.setPen(isChecked() ? pluginStyle::accent() : th().textPrimary);
    p.drawText(rect().adjusted(5, 0, -5, 0), Qt::AlignCenter, text());
  }
};

class ReorderButton final : public Button {
public:
  using Button::Button;
  std::function<void(QPoint)> drop;
  std::function<void(int)> step;

protected:
  void mousePressEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton) {
      m_start = e->globalPosition().toPoint();
      m_dragging = false;
    }
    Button::mousePressEvent(e);
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    if ((e->buttons() & Qt::LeftButton) &&
        (e->globalPosition().toPoint() - m_start).manhattanLength() > 5)
      m_dragging = true;
    if (!m_dragging)
      Button::mouseMoveEvent(e);
  }
  void mouseReleaseEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton && m_dragging) {
      setDown(false);
      m_dragging = false;
      if (drop)
        drop(e->globalPosition().toPoint());
      e->accept();
    } else
      Button::mouseReleaseEvent(e);
  }
  void keyPressEvent(QKeyEvent *e) override {
    if (e->modifiers().testFlag(Qt::AltModifier) &&
        (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right)) {
      if (step)
        step(e->key() == Qt::Key_Left ? -1 : 1);
      e->accept();
    } else
      Button::keyPressEvent(e);
  }

private:
  QPoint m_start;
  bool m_dragging = false;
};

class Choice final : public QComboBox {
public:
  using QComboBox::QComboBox;

protected:
  void paintEvent(QPaintEvent *e) override {
    if (!property("dialReadout").toBool()) {
      QComboBox::paintEvent(e);
      return;
    }
    QPainter p(this);
    auto f = font();
    f.setPixelSize(27);
    p.setFont(f);
    p.setPen(palette().text().color());
    p.drawText(rect(), Qt::AlignCenter, currentText());
    if (hasFocus()) {
      p.setPen(QPen(palette().highlight().color(), 2));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
  }
};

// Slicer face, with the DAW's accessible parameter gestures.
class Dial final : public ui::Knob {
public:
  Dial(const QString &name, int diameter, QWidget *parent)
      : Knob(name, parent) {
    setBare(diameter);
  }
  bool logarithmic = false;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    const double n = logarithmic && minimumValue() > 0
                         ? std::log(value() / minimumValue()) /
                               std::log(maximumValue() / minimumValue())
                         : (value() - minimumValue()) /
                               std::max(1e-9, maximumValue() - minimumValue());
    pluginStyle::knob(p, rect(), n, isEditing(), isEnabled(), hasFocus());
  }
};

class Drawing final : public QWidget {
public:
  explicit Drawing(QWidget *p) : QWidget(p) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  std::function<void(QPainter &, QRectF)> paint;
  std::function<void(QMouseEvent *)> press, move, release;
  std::function<void(QWheelEvent *)> wheel;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (paint)
      paint(p, rect());
  }
  void mousePressEvent(QMouseEvent *e) override {
    if (press)
      press(e);
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    if (move)
      move(e);
  }
  void mouseReleaseEvent(QMouseEvent *e) override {
    if (release)
      release(e);
  }
  void wheelEvent(QWheelEvent *e) override {
    if (wheel)
      wheel(e);
    else
      e->ignore();
  }
};

// A parameter canvas, updated by the panel's existing telemetry cadence.
// Each drag owns one parameter and one undo gesture, including when the time
// source is changed externally while the pointer is held.
class EchoDisplay final : public QWidget {
public:
  explicit EchoDisplay(QWidget *parent) : QWidget(parent) {
    setObjectName("echo-display");
    setMinimumSize(240, 180);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(tr("Echo controls"));
    setAccessibleDescription(
        tr("Drag Time horizontally, Feedback vertically, or Mix horizontally. "
           "Left and Right change Time; Up and Down change Feedback; Shift "
           "Left and Right change Mix."));
    setToolTip(accessibleDescription());
  }
  std::function<void(int, double, bool)> edit;
  std::function<void(int)> finish, automate;
  void snapshot(const QVariantMap &state) {
    const bool reduced = QSettings().value("ui/reduceMotion", false).toBool();
    const bool running = isVisible() && state["playing"].toBool() &&
                         state["active"].toBool() && !reduced;
    if (running) {
      if (m_clock.isValid())
        m_phase = std::fmod(
            m_phase + m_clock.restart() /
                          std::max(80., state["milliseconds"].toDouble()),
            8.);
      else
        m_clock.start();
    } else {
      m_clock.invalidate();
      m_phase = 0;
    }
    const auto audible = [](const QVariantMap &s) {
      return std::max(s["input"].toDouble(), s["wet"].toDouble()) > .0001;
    };
    const bool changed = m_state["values"] != state["values"] ||
                         running != m_running ||
                         (audible(m_state) && !audible(state));
    m_state = state;
    m_running = running;
    setProperty("echoAnimating", running);
    // Silence and stopped meters do not repaint an unchanged envelope.
    if (changed || (running && audible(state)))
      update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    pluginStyle::surface(p, QRectF(rect()).adjusted(1, 1, -1, -3), true);
    const auto a = area();
    auto font = p.font();
    font.setPixelSize(9);
    p.setFont(font);
    p.setPen(th().textSecondary);
    p.drawText(QRectF(a.left(), 6, a.width(), 14), Qt::AlignLeft,
               tr("TIME  ↔"));
    p.drawText(QRectF(a.left(), height() - 18, a.width(), 14), Qt::AlignLeft,
               tr("MIX  %1%  ↔").arg(v(6), 0, 'f', 0));
    p.drawText(QRectF(a.left(), 6, a.width(), 14), Qt::AlignRight,
               tr("FB %1%  ↕").arg(v(5), 0, 'f', 0));
    p.setPen(QPen(th().separator(), 1));
    p.drawLine(QPointF(a.left(), 27), QPointF(a.right(), 27));
    p.drawLine(QPointF(a.left(), height() - 25),
               QPointF(a.right(), height() - 25));
    p.drawLine(QPointF(width() - 18, a.top()),
               QPointF(width() - 18, a.bottom()));
    const double spacing = a.width() * (.13 + .15 * timeFraction());
    const double feedback = v(5) / 100.;
    for (int lane = 0; lane < 2; ++lane) {
      const double y = a.top() + a.height() * (lane ? .76 : .26);
      p.setPen(th().textSecondary);
      p.drawText(QRectF(5, y - 7, 15, 14), Qt::AlignCenter, lane ? "R" : "L");
      p.setPen(QPen(th().separator(), 1));
      p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
      for (int i = 0; i < 8; ++i) {
        const double x = a.left() + i * spacing;
        if (x > a.right())
          break;
        const bool routed = v(4) < .5 || i == 0 || (i % 2 == lane);
        const double envelope = std::pow(feedback, i);
        const double h = std::max(2., a.height() * .20 * envelope);
        auto colour = pluginStyle::accent();
        colour.setAlphaF(routed ? .25 + .6 * envelope : .10);
        p.setPen(QPen(colour, i ? 3 : 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(x, y - h), QPointF(x, y + h));
        if (m_running && routed) {
          const double distance = std::abs(m_phase - i);
          const double pulse = std::max(0., 1. - distance * 2.5);
          auto glow = pluginStyle::accent();
          const double level =
              std::max(m_state["input"].toDouble(), m_state["wet"].toDouble());
          glow.setAlphaF(pulse * std::clamp(level * 3., 0., 1.) *
                         std::pow(feedback, i * .35));
          p.setPen(Qt::NoPen);
          p.setBrush(glow);
          p.drawEllipse(QPointF(x, y), 4 + 5 * pulse, 4 + 5 * pulse);
        }
      }
    }
    const auto handle = [&](QPointF point, bool selected) {
      p.setPen(QPen(selected && hasFocus() ? th().textPrimary
                                           : pluginStyle::accent(),
                    1.5));
      p.setBrush(pluginStyle::shell());
      p.drawEllipse(point, 5, 5);
      p.setPen(Qt::NoPen);
      p.setBrush(pluginStyle::accent());
      p.drawEllipse(point, 2, 2);
    };
    handle(QPointF(a.left() + timeFraction() * a.width(), 27), m_selected == 1);
    handle(QPointF(width() - 18, a.bottom() - v(5) / 95. * a.height()),
           m_selected == 5);
    handle(QPointF(a.left() + v(6) / 100. * a.width(), height() - 25),
           m_selected == 6);
  }
  void mousePressEvent(QMouseEvent *e) override {
    endGesture();
    m_selected = e->position().y() > height() - 42  ? 6
                 : e->position().x() > width() - 34 ? 5
                                                    : 1;
    setFocus(Qt::MouseFocusReason);
    if (e->button() == Qt::RightButton) {
      QMenu menu(this);
      auto *reset = menu.addAction(tr("Reset parameter"));
      auto *automation = menu.addAction(tr("Create automation"));
      const int index = target();
      const auto *chosen = menu.exec(e->globalPosition().toPoint());
      if (chosen == reset && edit)
        edit(index, daw::plugins::delay::parameterTable()[index].defaultValue,
             true);
      if (chosen == automation && automate)
        automate(index);
    } else if (e->button() == Qt::LeftButton) {
      m_drag = target();
      apply(e->position());
    }
    update();
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    if (m_drag >= 0)
      apply(e->position());
    else
      setCursor(e->position().x() > width() - 34 &&
                        e->position().y() < height() - 42
                    ? Qt::SizeVerCursor
                    : Qt::SizeHorCursor);
  }
  void mouseReleaseEvent(QMouseEvent *e) override {
    if (e->button() == Qt::LeftButton)
      endGesture();
  }
  void keyPressEvent(QKeyEvent *e) override {
    const bool horizontal =
        e->key() == Qt::Key_Left || e->key() == Qt::Key_Right;
    const bool vertical = e->key() == Qt::Key_Up || e->key() == Qt::Key_Down;
    if (!horizontal && !vertical) {
      QWidget::keyPressEvent(e);
      return;
    }
    endGesture();
    m_selected = vertical                                     ? 5
                 : e->modifiers().testFlag(Qt::ShiftModifier) ? 6
                                                              : 1;
    const int index = target();
    const int sign =
        e->key() == Qt::Key_Left || e->key() == Qt::Key_Down ? -1 : 1;
    const double step = index == 1 ? std::max(1., v(1) * .02) : 1.;
    if (edit)
      edit(index, bounded(index, v(index) + sign * step), true);
    update();
    e->accept();
  }
  void hideEvent(QHideEvent *e) override {
    endGesture();
    m_clock.invalidate();
    m_running = false;
    setProperty("echoAnimating", false);
    QWidget::hideEvent(e);
  }
  bool event(QEvent *e) override {
    if (e->type() == QEvent::WindowDeactivate ||
        e->type() == QEvent::UngrabMouse)
      endGesture();
    if (e->type() == QEvent::EnabledChange && !isEnabled()) {
      endGesture();
      m_clock.invalidate();
      m_running = false;
      setProperty("echoAnimating", false);
    }
    return QWidget::event(e);
  }

private:
  QRectF area() const { return QRectF(rect()).adjusted(25, 46, -40, -47); }
  double v(int i) const {
    return m_state["values"].toList().value(i).toDouble();
  }
  int target() const { return m_selected == 1 && v(0) != 2 ? 2 : m_selected; }
  double timeFraction() const {
    return v(0) == 2 ? std::log(std::max(1., v(1))) / std::log(8000.)
                     : v(2) / 22.;
  }
  static double bounded(int i, double v) {
    const auto &p = daw::plugins::delay::parameterTable()[i];
    return std::clamp(p.isStepped ? std::round(v) : v, p.minValue, p.maxValue);
  }
  void apply(QPointF point) {
    const auto a = area();
    const double x = std::clamp((point.x() - a.left()) / a.width(), 0., 1.);
    const double y = std::clamp((a.bottom() - point.y()) / a.height(), 0., 1.);
    const double value = m_drag == 5   ? y * 95
                         : m_drag == 6 ? x * 100
                         : m_drag == 2 ? x * 22
                                       : std::pow(8000., x);
    if (edit)
      edit(m_drag, bounded(m_drag, value), false);
  }
  void endGesture() {
    const int index = std::exchange(m_drag, -1);
    if (index >= 0 && finish)
      finish(index);
  }
  QVariantMap m_state;
  QElapsedTimer m_clock;
  double m_phase = 0;
  int m_drag = -1, m_selected = 1;
  bool m_running = false;
};

QWidget *section(QWidget *parent, QLayout *layout) {
  auto *card = new Drawing(parent);
  card->paint = [](QPainter &p, QRectF r) {
    pluginStyle::surface(p, r.adjusted(1, 1, -1, -3));
  };
  layout->setContentsMargins(10, 10, 10, 10);
  card->setLayout(layout);
  return card;
}

QVariantList metadata(std::span<const daw::plugins::ParameterInfo> params) {
  QVariantList result;
  for (const auto &p : params)
    result.append(QVariantMap{{"id", QString::fromStdString(p.id)},
                              {"name", QString::fromStdString(p.name)},
                              {"unit", QString::fromStdString(p.unit)},
                              {"min", p.minValue},
                              {"max", p.maxValue},
                              {"default", p.defaultValue},
                              {"stepped", p.isStepped},
                              {"automatable", p.isAutomatable}});
  return result;
}
} // namespace

NativePluginView::NativePluginView(Kind kind, QWidget *parent)
    : QWidget(parent), m_kind(kind) {
  setObjectName("NativePluginView");
  setAttribute(Qt::WA_OpaquePaintEvent);
  auto controlFont = font();
  controlFont.setPixelSize(11);
  setFont(controlFont);
  pluginStyle::bind(this);
  auto *style = QStyleFactory::create("Fusion");
  style->setParent(this);
  setStyle(style);
  if (kind == Kind::Delay) {
    m_parameters = metadata(daw::plugins::delay::parameterTable());
    buildDelay();
  }
  if (kind == Kind::Compressor) {
    m_parameters = metadata(daw::plugins::compressor::parameterTable());
    buildCompressor();
  }
  if (kind == Kind::Pitch) {
    m_parameters = metadata(daw::plugins::pitch::parameterTable());
    auto retune = m_parameters[0].toMap();
    retune["min"] = 0.;
    retune["max"] = 200.;
    retune["unit"] = "ms";
    retune["default"] =
        daw::plugins::pitch::retuneMilliseconds(retune["default"].toDouble());
    m_parameters[0] = retune;
    buildPitch();
  }
  m_built = kind != Kind::Modulation;
}

double NativePluginView::value(int index) const {
  return m_state.value("values").toList().value(index).toDouble();
}
void NativePluginView::change(int index, double v, bool finished) {
  if (!std::isfinite(v))
    return;
  auto values = m_state.value("values").toList();
  if (index < 0 || index >= values.size())
    return;
  values[index] = v;
  m_state["values"] = values;
  emit edit(index, v, finished);
}
void NativePluginView::context(QWidget *w, int index) {
  w->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(w, &QWidget::customContextMenuRequested, this,
          [this, w, index](QPoint point) {
            QMenu menu(w);
            auto *reset = menu.addAction(tr("Reset to default"));
            auto *automation =
                m_parameters[index].toMap().value("automatable", true).toBool()
                    ? menu.addAction(tr("Create automation"))
                    : nullptr;
            auto *chosen = menu.exec(w->mapToGlobal(point));
            if (chosen == reset)
              change(index, m_parameters[index].toMap()["default"].toDouble());
            else if (automation && chosen == automation)
              emit automate(index);
          });
}
QDoubleSpinBox *NativePluginView::number(int index) {
  const auto p = m_parameters[index].toMap();
  auto *box = new QDoubleSpinBox(this);
  const double scale = m_kind == Kind::Modulation && p["unit"] == "%" ? 100 : 1;
  box->setObjectName(p["id"].toString() + "-value");
  box->setAccessibleName(p["name"].toString());
  box->setRange(p["min"].toDouble() * scale, p["max"].toDouble() * scale);
  box->setDecimals(p["stepped"].toBool()                           ? 0
                   : p["unit"] == "Hz" && p["max"].toDouble() < 20 ? 2
                                                                   : 1);
  box->setSingleStep(p["stepped"].toBool() ? 1 : .1);
  box->setKeyboardTracking(false);
  box->setButtonSymbols(QAbstractSpinBox::NoButtons);
  box->setAlignment(Qt::AlignCenter);
  box->setFrame(false);
  // Compact native readouts must not inherit the host form's 7px input padding.
  const auto restyle = [box] {
    box->setStyleSheet(
        QStringLiteral(
            "QDoubleSpinBox, QLineEdit { font-size: 11px; color: %1; padding: "
            "0; border: none; background: transparent; }")
            .arg(th().textPrimary.name()));
  };
  connect(&ThemeManager::instance(), &ThemeManager::changed, box, restyle);
  restyle();
  QPalette transparent;
  transparent.setColor(QPalette::Base, Qt::transparent);
  box->setPalette(transparent);
  box->setSuffix(" " + p["unit"].toString());
  box->setMinimumHeight(24);
  box->setMaximumWidth(112);
  box->setValue(p["default"].toDouble() * scale);
  connect(box, &QDoubleSpinBox::valueChanged, this,
          [this, index, scale](double v) { change(index, v / scale); });
  context(box, index);
  m_numbers[index] = box;
  return box;
}
QWidget *NativePluginView::knob(int index, int diameter, const QColor &face) {
  const auto p = m_parameters[index].toMap();
  auto *group = new QWidget(this);
  auto *l = new QVBoxLayout(group);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(3);
  l->setAlignment(Qt::AlignVCenter);
  Q_UNUSED(face);
  auto *dial = new Dial(p["name"].toString(), diameter, group);
  dial->setObjectName(p["id"].toString());
  dial->setAccessibleName(p["name"].toString());
  dial->setRange(p["min"].toDouble(), p["max"].toDouble());
  dial->setDefaultValue(p["default"].toDouble());
  dial->setValue(p["default"].toDouble());
  dial->setStepped(p["stepped"].toBool());
  dial->setAutomatable(p.value("automatable", true).toBool());
  dial->logarithmic =
      p["min"].toDouble() > 0 && (p["unit"] == "Hz" || p["id"] == "timeMs" ||
                                  p["id"] == "attack" || p["id"] == "release");
  dial->setLogarithmic(dial->logarithmic);
  dial->setFormatter([p, this](double v) {
    const double scale =
        m_kind == Kind::Modulation && p["unit"] == "%" ? 100 : 1;
    return QString::number(v * scale, 'f', 1) + " " + p["unit"].toString();
  });
  const auto target = [this, index] {
    return m_kind == Kind::Delay && index == 1 && value(0) != 2 ? 2 : index;
  };
  connect(dial, &ui::Knob::valueChanged, this,
          [this, target](double v) { change(target(), v, false); });
  connect(dial, &ui::Knob::editFinished, this,
          [this, target] { emit finish(target()); });
  connect(dial, &ui::Knob::automateRequested, this,
          [this, target] { emit automate(target()); });
  l->addWidget(dial, 0, Qt::AlignHCenter);
  auto name = p["name"].toString();
  if (m_kind == Kind::Modulation && m_state["rack"].toBool())
    name = name.section(' ', 1);
  auto *caption =
      label(name.toUpper(), group, m_kind == Kind::Modulation ? 10 : 11);
  caption->setSizePolicy(m_kind == Kind::Modulation ? QSizePolicy::Ignored
                                                    : QSizePolicy::Preferred,
                         QSizePolicy::Preferred);
  l->addWidget(caption);
  auto *numeric = number(index);
  numeric->setFixedWidth(m_kind == Kind::Modulation ? std::max(50, diameter + 4)
                                                    : std::max(72, diameter));
  l->addWidget(numeric, 0, Qt::AlignHCenter);
  m_knobs[index] = dial;
  return group;
}
QComboBox *NativePluginView::choice(int index, const QStringList &labels) {
  auto *box = new Choice(this);
  box->addItems(labels);
  const auto p = m_parameters[index].toMap();
  box->setObjectName(p["id"].toString());
  box->setAccessibleName(p["name"].toString());
  box->setMinimumHeight(28);
  box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  box->setMinimumContentsLength(4);
  connect(box, &QComboBox::activated, this,
          [this, index](int v) { change(index, v); });
  context(box, index);
  m_choices[index] = box;
  return box;
}
QAbstractButton *NativePluginView::toggle(int index, const QString &text) {
  auto *b = new Button(text, this);
  b->setCheckable(true);
  b->setObjectName(m_parameters[index].toMap()["id"].toString());
  connect(b, &QAbstractButton::clicked, this,
          [this, index](bool on) { change(index, on ? 1 : 0); });
  context(b, index);
  m_toggles[index] = b;
  return b;
}
QWidget *NativePluginView::segments(int index, const QStringList &texts) {
  auto *w = new QWidget(this);
  auto *l = new QHBoxLayout(w);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(3);
  for (int i = 0; i < texts.size(); ++i) {
    auto *b = new Button(texts[i], w);
    b->setCheckable(true);
    b->setObjectName(m_parameters[index].toMap()["id"].toString() +
                     QString::number(i));
    connect(b, &QAbstractButton::clicked, this,
            [this, index, i] { change(index, i); });
    context(b, index);
    l->addWidget(b);
    m_segments[index].append(b);
  }
  return w;
}

void NativePluginView::buildDelay() {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(25, 12, 25, 8);
  root->setSpacing(8);
  auto *head = new QHBoxLayout;
  auto *logo = new Drawing(this);
  logo->setFixedSize(42, 42);
  logo->paint = [](QPainter &p, QRectF r) {
    p.setPen(QPen(pluginStyle::accent(), 2, Qt::SolidLine, Qt::RoundCap));
    for (int i = 0; i < 5; ++i) {
      const double h = 22 * std::pow(.7, i);
      double x = r.left() + 8 + i * 6;
      p.drawLine(QPointF(x, r.center().y() - h / 2),
                 QPointF(x, r.center().y() + h / 2));
    }
  };
  head->addWidget(logo);
  head->addWidget(label("Classic Delay", this, 27));
  head->addStretch();
  head->addWidget(label("CHARACTER", this));
  QStringList characters, divisions;
  for (unsigned i = 0; i < 7; ++i)
    characters << QString::fromUtf8(daw::plugins::delay::characterName(i));
  for (unsigned i = 0; i < 23; ++i)
    divisions << QString::fromUtf8(daw::plugins::delay::divisionName(i));
  auto *character = choice(8, characters);
  character->setMinimumWidth(155);
  head->addWidget(character);
  auto *amount = knob(9, 38, QColor("#c3aed8"));
  static_cast<QBoxLayout *>(amount->layout())
      ->setDirection(QBoxLayout::LeftToRight);
  head->addWidget(amount);
  root->addLayout(head);
  auto *body = new QHBoxLayout;
  body->setSpacing(8);
  root->addLayout(body, 1);
  auto *tapCol = new QVBoxLayout;
  tapCol->addStretch();
  m_tap = new Button("TAP", this);
  m_tap->setObjectName("tap");
  m_tap->setAccessibleName(tr("Tap tempo"));
  m_tap->setFixedSize(68, 28);
  auto f = m_tap->font();
  f.setPixelSize(14);
  m_tap->setFont(f);
  tapCol->addWidget(m_tap, 0, Qt::AlignHCenter);
  tapCol->addStretch();

  connect(m_tap, &QAbstractButton::clicked, this, [this] {
    const auto now = m_tapClock.isValid() ? m_tapClock.elapsed() : 0;
    if (!m_tapClock.isValid())
      m_tapClock.start();
    if (!m_taps.empty() && now - m_taps.back() > 2000)
      m_taps.clear();
    m_taps.append(now);
    while (m_taps.size() > 5)
      m_taps.removeFirst();
    if (m_taps.size() > 1) {
      const double ms =
          double(m_taps.back() - m_taps.front()) / (m_taps.size() - 1);
      if (ms > 0)
        change(value(0) == 2 ? 1 : 3, value(0) == 2
                                          ? std::clamp(ms, 1., 8000.)
                                          : std::clamp(60000. / ms, 30., 300.));
    }
  });
  auto *time = new QVBoxLayout;
  time->addLayout(tapCol);
  auto *timeKnob = knob(1, 118);
  time->addWidget(timeKnob);
  m_divisionReadout = label("1/8 D", timeKnob);
  timeKnob->layout()->addWidget(m_divisionReadout);
  time->addWidget(label("MODULATION", this));
  auto *mods = new QHBoxLayout;
  mods->addWidget(knob(12, 60, QColor("#99cbd7")));
  mods->addWidget(knob(13, 60, QColor("#cdd783")));
  time->addLayout(mods);
  time->addStretch();
  body->addWidget(section(this, time), 2);
  auto *center = new QVBoxLayout;
  center->addWidget(segments(4, {"STEREO", "PING PONG"}), 0, Qt::AlignHCenter);
  auto *readouts = new QHBoxLayout;
  auto *division = choice(2, divisions);
  division->setMinimumWidth(70);
  readouts->addWidget(division);
  m_timing = label("375.0 ms", this, 13);
  m_timing->setObjectName("time-display");
  readouts->addWidget(m_timing, 1);
  readouts->addWidget(number(3));
  center->addLayout(readouts);
  auto *orbit = new EchoDisplay(this);
  orbit->edit = [this](int i, double v, bool done) { change(i, v, done); };
  orbit->finish = [this](int i) { emit finish(i); };
  orbit->automate = [this](int i) { emit automate(i); };
  m_displays.append(orbit);
  center->addWidget(orbit, 1);
  center->addWidget(segments(0, {"HOST", "BPM", "ms"}), 0, Qt::AlignHCenter);
  body->addLayout(center, 3);
  auto *feedback = new QVBoxLayout;
  feedback->addStretch();
  feedback->addWidget(knob(5, 118));
  feedback->addWidget(label("FILTERS", this));
  auto *filters = new QHBoxLayout;
  filters->addWidget(knob(10, 60, QColor("#c3aed8")));
  filters->addWidget(knob(11, 60, QColor("#99d6c4")));
  feedback->addLayout(filters);
  feedback->addStretch();
  body->addWidget(section(this, feedback), 2);
  auto *levels = new QVBoxLayout;
  levels->addStretch();
  levels->addWidget(knob(6, 76, QColor("#99d6c4")));
  levels->addStretch();
  levels->addWidget(knob(7, 68, QColor("#f19c9f")));
  levels->addStretch();
  body->addWidget(section(this, levels));
  m_status = label("STEREO ECHO PROCESSOR                         READY", this);
  root->addWidget(m_status);
}

void NativePluginView::buildCompressor() {
  auto *root = new QHBoxLayout(this);
  root->setContentsMargins(20, 20, 20, 16);
  root->setSpacing(22);
  auto *transfer = new QVBoxLayout;
  transfer->addWidget(segments(7, {"SOFT", "PUNCH"}));
  auto *graph = new Drawing(this);
  graph->setObjectName("transfer-curve");
  graph->setAccessibleName(tr("Compression curve; drag threshold or ratio"));
  graph->setMinimumSize(195, 210);
  m_displays.append(graph);
  graph->paint = [this](QPainter &p, QRectF r) {
    auto plot = r.adjusted(22, 12, -12, -30);
    auto xy = [&](double x, double y) {
      return QPointF(plot.left() + (x + 60) / 60 * plot.width(),
                     plot.bottom() - (y + 60) / 60 * plot.height());
    };
    p.fillRect(plot, th().well());
    p.setPen(QPen(th().separator(), .7));
    for (int i = 0; i <= 6; ++i) {
      p.drawLine(xy(-60 + i * 10, -60), xy(-60 + i * 10, 0));
      p.drawLine(xy(-60, -60 + i * 10), xy(0, -60 + i * 10));
    }
    p.setPen(QPen(th().textSecondary, 1, Qt::DashLine));
    p.drawLine(plot.bottomLeft(), plot.topRight());
    QPainterPath path;
    for (int i = 0; i <= 120; ++i) {
      const double x = -60 + i * .5;
      auto point = xy(x, x - daw::plugins::compressor::reductionDb(
                                 x, value(1), value(0), value(5)));
      if (!i)
        path.moveTo(point);
      else
        path.lineTo(point);
    }
    p.save();
    p.setClipRect(plot);
    p.setPen(QPen(pluginStyle::accent(), 3));
    p.drawPath(path);
    p.setBrush(th().textPrimary);
    p.drawEllipse(xy(std::clamp(m_state["input"].toDouble(), -60., 0.),
                     m_state["marker"].toDouble()),
                  4, 4);
    p.restore();
    p.setPen(QPen(pluginStyle::accent(), 2));
    p.setBrush(th().surfaceElevated);
    p.drawEllipse(xy(value(1), value(1)), 6, 6);
    p.drawEllipse(xy(0, -daw::plugins::compressor::reductionDb(
                            0, value(1), value(0), value(5))),
                  6, 6);
    p.setPen(th().textSecondary);
    p.drawText(r.adjusted(0, r.height() - 24, 0, 0), Qt::AlignCenter,
               "INPUT / OUTPUT  dB");
  };
  auto dragIndex = std::make_shared<int>(-1);
  graph->press = [this, graph, dragIndex](QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
      return;
    *dragIndex = e->position().x() > graph->width() * .78 ? 0 : 1;
  };
  graph->move = [this, graph, dragIndex](QMouseEvent *e) {
    if (*dragIndex < 0)
      return;
    if (*dragIndex == 1)
      change(
          1,
          std::clamp((e->position().x() - 22) / (graph->width() - 34) * 60 - 60,
                     -60., 0.),
          false);
    else {
      const double y = std::clamp((graph->height() - 30 - e->position().y()) /
                                          (graph->height() - 42) * 60 -
                                      60,
                                  value(1) + .01, 0.);
      change(0, std::clamp(-value(1) / (y - value(1)), 1., 20.), false);
    }
    graph->update();
  };
  graph->release = [this, dragIndex](QMouseEvent *) {
    if (*dragIndex >= 0)
      emit finish(*dragIndex);
    *dragIndex = -1;
  };
  transfer->addWidget(graph, 1);
  root->addLayout(transfer, 3);
  auto *controls = new QVBoxLayout;
  controls->addStretch();
  auto *primary = new QHBoxLayout;
  for (int i : {0, 1, 2})
    primary->addWidget(knob(i, 86));
  primary->addWidget(toggle(8, "AUTO GAIN"));
  controls->addLayout(primary);
  controls->addSpacing(16);
  auto *secondary = new QHBoxLayout;
  for (int i : {3, 4, 5, 6})
    secondary->addWidget(knob(i, 68));
  controls->addLayout(secondary);
  controls->addStretch();
  root->addLayout(controls, 6);
  auto *meters = new Drawing(this);
  meters->setObjectName("signal-meters");
  meters->setAccessibleName(tr("Gain reduction, input and output levels"));
  meters->setMinimumWidth(90);
  m_displays.append(meters);
  meters->paint = [this](QPainter &p, QRectF r) {
    auto font = p.font();
    font.setPixelSize(10);
    p.setFont(font);
    const QStringList names{"GR", "IN", "OUT"};
    const double vals[]{m_state["reduction"].toDouble(),
                        m_state["input"].toDouble(),
                        m_state["output"].toDouble()};
    for (int col = 0; col < 3; ++col) {
      const double x = r.width() * col / 3;
      const double level = col == 0 ? vals[col] / 30 : (vals[col] + 60) / 66;
      p.setPen(palette().text().color());
      p.drawText(QRectF(x, 8, r.width() / 3, 22), Qt::AlignCenter, names[col]);
      for (int i = 0; i < 25; ++i) {
        const auto color = col == 0 ? pluginStyle::accent()
                           : i > 21 ? QColor("#e77366")
                                    : pluginStyle::accent();
        p.fillRect(QRectF(x + 6,
                          r.height() - 44 - (i + 1) * (r.height() - 80) / 25,
                          r.width() / 3 - 12, (r.height() - 80) / 25 - 3),
                   i / 25. < level ? color : th().separator());
      }
      p.drawText(QRectF(x, r.height() - 33, r.width() / 3, 22), Qt::AlignCenter,
                 QString::number(vals[col], 'f', 1));
    }
  };
  root->addWidget(meters, 1);
}

void NativePluginView::presetHeader(QBoxLayout *root, const QString &title) {
  const bool single = m_kind == Kind::Modulation && !m_state["rack"].toBool();
  auto *row = new QHBoxLayout;
  auto *brand = label(title, this,
                      single                  ? 12
                      : m_kind == Kind::Pitch ? 20
                                              : 18);
  brand->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  row->addWidget(brand, 1);
  m_preset = new QComboBox(this);
  m_preset->setObjectName("preset");
  m_preset->setAccessibleName(tr("Preset"));
  m_preset->setFixedWidth(single ? 105 : 125);
  m_preset->setMinimumHeight(28);
  row->addWidget(m_preset);
  connect(m_preset, &QComboBox::activated, this, [this](int i) {
    const auto data = m_preset->itemData(i).toString();
    if (data.startsWith("factory:"))
      emit factoryPreset(data.mid(8).toInt());
    else if (data.startsWith("user:"))
      emit loadPreset(data.mid(5));
  });
  if (m_kind == Kind::Pitch) {
    auto *send = new Button(tr("Send to all"), this);
    send->setObjectName("send-to-all");
    connect(send, &QAbstractButton::clicked, this,
            &NativePluginView::sendToAll);
    row->addWidget(send);
  }
  auto *library = new Button(m_kind == Kind::Pitch ? tr("Settings")
                             : single              ? QStringLiteral("…")
                                                   : tr("Presets"),
                             this);
  library->setAccessibleName(tr("Preset library"));
  library->setObjectName("settings");
  connect(library, &QAbstractButton::clicked, this,
          &NativePluginView::presetDialog);
  row->addWidget(library);
  m_active = new Button(tr("On"), this);
  m_active->setObjectName("active");
  m_active->setCheckable(true);
  m_active->setAccessibleName(tr("Enable plugin"));
  connect(m_active, &QAbstractButton::clicked, this,
          &NativePluginView::toggleBypass);
  row->addWidget(m_active);
  root->addLayout(row);
}
void NativePluginView::buildPitch() {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(20, 14, 20, 9);
  root->setSpacing(12);
  presetHeader(root, "VLT  PITCH");
  auto *row = new QHBoxLayout;
  row->addWidget(choice(
      4, {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}));
  row->addWidget(choice(5, {"Chromatic", "Major", "Natural Minor",
                            "Harmonic Minor", "Melodic Minor",
                            "Major Pentatonic", "Minor Pentatonic", "Custom"}),
                 1);
  row->addWidget(label("A4", this));
  row->addWidget(number(3));
  auto *quality = choice(11, {"Real-Time", "HD"});
  quality->setMinimumContentsLength(9);
  row->addWidget(quality);
  root->addLayout(row);
  auto *controls = new QHBoxLayout;
  controls->setSpacing(10);
  auto *correction = new QVBoxLayout;
  correction->addWidget(label("CORRECTION", this));
  auto *correctionKnobs = new QHBoxLayout;
  correctionKnobs->setSpacing(0);
  correctionKnobs->addWidget(knob(0, 100));
  correctionKnobs->addWidget(knob(1, 76));
  correctionKnobs->addWidget(knob(2, 76));
  correction->addLayout(correctionKnobs, 1);
  controls->addWidget(section(this, correction), 3);
  auto *readout = new QVBoxLayout;
  readout->addStretch();
  readout->addWidget(label("PITCH TRACKING", this));
  m_timing = label("—  ›  —", this, 24);
  m_timing->setObjectName("pitch-readout");
  readout->addWidget(m_timing);
  readout->addWidget(toggle(7, "FORMANTS"));
  readout->addStretch();
  controls->addWidget(section(this, readout), 2);
  root->addLayout(controls, 1);
  auto *piano = new Drawing(this);
  piano->setObjectName("piano");
  piano->setMinimumHeight(64);
  piano->setMaximumHeight(94);
  const QStringList names{"C",  "C#", "D",  "D#", "E",  "F",
                          "F#", "G",  "G#", "A",  "A#", "B"};
  for (int i = 0; i < 12; ++i) {
    auto *key = new Button(names[i], piano);
    key->setObjectName("note-" + QString::number(i));
    key->setProperty("pianoKey", true);
    key->setAccessibleName(tr("Allow note %1").arg(names[i]));
    key->setCheckable(true);
    m_notes.append(key);
    connect(key, &QAbstractButton::clicked, this,
            [this, i] { emit toggleNote(i); });
  }
  // Position keys in paint as well as on snapshots, so resizing keeps the
  // familiar seven white/five black key geometry at every scale.
  piano->paint = [this](QPainter &, QRectF r) {
    int white = 0;
    for (int i = 0; i < 12; ++i) {
      const bool black =
          QStringList{"1", "3", "6", "8", "10"}.contains(QString::number(i));
      auto *key = m_notes[i];
      if (!black) {
        key->setGeometry(qRound(white * r.width() / 7), 0,
                         qRound(r.width() / 7) - 2, qRound(r.height()));
        ++white;
      } else {
        key->setGeometry(qRound((white - .32) * r.width() / 7), 0,
                         qRound(r.width() / 7 * .64), qRound(r.height() * .64));
        key->raise();
      }
      auto pal = key->palette();
      pal.setColor(QPalette::Button,
                   black ? QColor("#242930") : QColor("#b8c4d0"));
      pal.setColor(QPalette::ButtonText,
                   black ? QColor("#dce5ee") : QColor("#24303c"));
      key->setPalette(pal);
    }
  };
  root->addWidget(piano);
  m_status = label(tr("Click keys to set your scale"), this);
  m_status->setObjectName("status");
  root->addWidget(m_status);
}

void NativePluginView::presetDialog() {
  if (m_settings) {
    m_settings->show();
    m_settings->raise();
    return;
  }
  m_settings = new QDialog(this);
  m_settings->setWindowTitle(m_kind == Kind::Pitch ? tr("Fine tuning")
                                                   : tr("Preset library"));
  pluginStyle::bind(m_settings);
  auto *root = new QVBoxLayout(m_settings);
  if (m_kind == Kind::Pitch) {
    auto *form = new QFormLayout;
    form->addRow(tr("Voice range"), choice(6, {"Auto", "Low", "Mid", "High"}));
    form->addRow(tr("Correction amount"), number(8));
    form->addRow(tr("Output level"), number(9));
    root->addLayout(form);
  }
  auto *name = new QLineEdit(m_state["preset"].toString(), m_settings);
  name->setObjectName("preset-name");
  name->setAccessibleName(tr("Preset name"));
  name->setMaxLength(48);
  root->addWidget(name);
  auto *message = label({}, m_settings);
  message->setWordWrap(true);
  auto *buttons = new QHBoxLayout;
  for (const auto &text :
       {tr("Save"), tr("Replace"), tr("Rename"), tr("Delete")}) {
    if (text == tr("Rename") && !renamePreset)
      continue;
    auto *b = new Button(text, m_settings);
    buttons->addWidget(b);
    connect(b, &QAbstractButton::clicked, this, [this, text, name, message] {
      QString error;
      if (text == tr("Save") || text == tr("Replace")) {
        if (savePreset)
          error = savePreset(name->text(), text == tr("Replace"));
      } else if (text == tr("Rename")) {
        if (renamePreset)
          error = renamePreset(m_state["preset"].toString(), name->text());
      } else
        emit deletePreset(m_state["preset"].toString());
      message->setText(error.isEmpty() ? tr("Saved")
                       : error == "replace"
                           ? tr("A preset with this name exists. Use Replace "
                                "to overwrite it.")
                           : error);
    });
  }
  root->addLayout(buttons);
  root->addWidget(message);
  m_settings->show();
  setSnapshot(m_state);
}

void NativePluginView::buildModulation() {
  const bool rack = m_state["rack"].toBool();
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(14, 12, 14, 8);
  root->setSpacing(8);
  presetHeader(root, m_state["title"].toString().toUpper());
  m_cardLayout = new QHBoxLayout;
  m_cardLayout->setSpacing(10);
  root->addLayout(m_cardLayout, 1);
  for (const auto &entry : m_state["order"].toList()) {
    const int module = entry.toInt();
    auto *card = new Drawing(this);
    card->setObjectName("module-" + QString::number(module));
    card->setProperty("module", module);
    card->paint = [](QPainter &p, QRectF r) {
      pluginStyle::surface(p, r.adjusted(1, 1, -1, -3));
    };
    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(7, 10, 7, 10);
    col->setSpacing(4);
    auto *head = new QHBoxLayout;
    if (rack) {
      auto *move = new ReorderButton("↔  STEREO", card);
      move->setObjectName("move-" + QString::number(module));
      move->setAccessibleName(tr("Move %1").arg(moduleNames[module]));
      move->setToolTip(tr("Drag to reorder; Alt + Left/Right"));
      head->addWidget(move);
      move->step = [this, module](int delta) {
        const int from = m_state["order"].toList().indexOf(module);
        emit reorder(from, std::clamp(from + delta, 0, 3));
      };
      move->drop = [this, module](QPoint global) {
        const int from = m_state["order"].toList().indexOf(module);
        int to = 0;
        double nearest = 1.e12;
        for (int i = 0; i < m_cardLayout->count(); ++i) {
          auto *target = m_cardLayout->itemAt(i)->widget();
          const double distance = std::abs(
              target->mapToGlobal(target->rect().center()).x() - global.x());
          if (distance < nearest) {
            nearest = distance;
            to = i;
          }
        }
        if (from != to)
          emit reorder(from, to);
      };
      connect(move, &QAbstractButton::clicked, this, [this, move, module] {
        QMenu menu(move);
        auto order = m_state["order"].toList();
        int from = order.indexOf(module);
        auto *left = menu.addAction(tr("Move left"));
        auto *right = menu.addAction(tr("Move right"));
        left->setEnabled(from > 0);
        right->setEnabled(from < 3);
        auto *selected =
            menu.exec(move->mapToGlobal(QPoint(0, move->height())));
        if (selected == left)
          emit reorder(from, from - 1);
        if (selected == right)
          emit reorder(from, from + 1);
      });
      head->addWidget(toggle(Rack::offsets[module], "On"));
    } else
      head->addWidget(label("STEREO", card));
    col->addLayout(head);
    auto *art = new Drawing(card);
    art->setMinimumHeight(85);
    art->setMaximumHeight(155);
    art->paint = [module](QPainter &p, QRectF r) {
      p.translate(r.center() - QPointF(80, 66) *
                                   std::min(r.width() / 160, r.height() / 132));
      p.scale(std::min(r.width() / 160, r.height() / 132),
              std::min(r.width() / 160, r.height() / 132));
      QColor tint = mixColors(pluginStyle::accent(), moduleColors[module], .18);
      QLinearGradient g(0, 25, 0, 108);
      auto transparent = tint;
      transparent.setAlpha(20);
      g.setColorAt(0, transparent);
      g.setColorAt(1, tint.darker(130));
      p.setBrush(g);
      p.setPen(QPen(tint.lighter(145), 1));
      if (module == 1 || module == 4) {
        for (int x : {30, 78}) {
          int y = x == 30 ? 48 : 36;
          p.drawRoundedRect(QRectF(x, y, 52, 52), 12, 8);
          p.drawEllipse(QRectF(x, y - 9, 52, 18));
          p.drawArc(QRectF(x, y + 30, 52, 18), 180 * 16, 180 * 16);
        }
      } else {
        QPolygonF shape{QPointF(40, 42),  QPointF(80, 25),  QPointF(120, 42),
                        QPointF(120, 88), QPointF(80, 106), QPointF(40, 88)};
        p.drawPolygon(shape);
        p.drawLine(QPointF(40, 42), QPointF(80, 60));
        p.drawLine(QPointF(80, 60), QPointF(120, 42));
        p.drawLine(QPointF(80, 60), QPointF(80, 106));
        if (module == 3) {
          p.setBrush(Qt::NoBrush);
          p.drawEllipse(QRectF(53, 61, 54, 22));
          p.drawEllipse(QRectF(69, 45, 22, 54));
        } else
          for (int j = 0; j < 3; ++j) {
            QPainterPath wave;
            wave.moveTo(41, 70 + j * 7);
            wave.cubicTo(60, 51 + j * 7, 85, 93 + j * 7, 119, 65 + j * 7);
            p.drawPath(wave);
          }
      }
    };
    col->addWidget(art, 1);
    col->addWidget(label(moduleNames[module].toUpper(), card, 20));
    col->addWidget(label(moduleCaptions[module], card, 10));
    const int base = rack ? int(Rack::offsets[module]) + 1 : 0,
              count = rack ? (module == 1 ? 3 : 4) : m_parameters.size();
    auto *controls = new QHBoxLayout;
    controls->setSpacing(2);
    controls->addWidget(knob(base + 1, rack ? 34 : 42));
    controls->addWidget(knob(base, rack ? 60 : 76));
    controls->addWidget(knob(base + 2, rack ? 34 : 42));
    col->addLayout(controls);
    if (count > 3) {
      for (int i = 3; i < count; ++i) {
        auto *extra = knob(base + i, 26);
        extra->findChild<QLabel *>()->setSizePolicy(QSizePolicy::Preferred,
                                                    QSizePolicy::Preferred);
        static_cast<QBoxLayout *>(extra->layout())
            ->setDirection(QBoxLayout::LeftToRight);
        extra->setMaximumWidth(220);
        col->addWidget(extra, 0, Qt::AlignHCenter);
      }
    } else if (rack)
      col->addSpacing(30);
    m_cards.append(card);
    m_cardLayout->addWidget(card, 1);
  }
  if (rack)
    buildEq(root);
  m_status = label({}, this);
  m_status->setObjectName("status");
  root->addWidget(m_status);
}

void NativePluginView::buildEq(QBoxLayout *root) {
  auto *heading = new QHBoxLayout;
  heading->addWidget(toggle(Rack::eqEnabledParameter, "EQ On"));
  auto *expand =
      new Button("FILTER & EQ     ·     4 BANDS · LOW CUT · HIGH CUT", this);
  expand->setCheckable(true);
  expand->setObjectName("eq-toggle");
  heading->addWidget(expand, 1);
  root->addLayout(heading);
  auto *content = new QWidget(this);
  content->setObjectName("eq-content");
  auto *col = new QVBoxLayout(content);
  col->setContentsMargins(0, 0, 0, 0);
  auto *graph = new Drawing(content);
  graph->setObjectName("eq-graph");
  graph->setMinimumHeight(155);
  m_displays.append(graph);
  graph->paint = [this](QPainter &p, QRectF r) {
    auto plot = r.adjusted(20, 12, -20, -20);
    auto xy = [&](double hz, double db) {
      return QPointF(plot.left() +
                         std::log(hz / 20) / std::log(1000) * plot.width(),
                     plot.center().y() - db / 36 * plot.height());
    };
    p.setPen(th().separator());
    for (double hz :
         {20., 50., 100., 200., 500., 1000., 2000., 5000., 10000., 20000.})
      p.drawLine(xy(hz, -18), xy(hz, 18));
    for (int db : {-18, -12, -6, 0, 6, 12, 18})
      p.drawLine(xy(20, db), xy(20000, db));
    auto response = m_state["response"].toList();
    QPainterPath path;
    for (int i = 0; i < response.size(); ++i) {
      auto pt = QPointF(
          plot.left() +
              double(i) / std::max(1, int(response.size()) - 1) * plot.width(),
          plot.center().y() - std::clamp(response[i].toDouble(), -24., 24.) /
                                  36 * plot.height());
      if (!i)
        path.moveTo(pt);
      else
        path.lineTo(pt);
    }
    p.setPen(QPen(pluginStyle::accent(), 2));
    p.drawPath(path);
    for (unsigned band = 0; band < 6; ++band) {
      p.setBrush(value(rackEq(band, 0)) > .5 ? pluginStyle::accent()
                                             : th().textSecondary);
      p.drawEllipse(xy(value(rackEq(band, 1)),
                       band == 0 || band == 5 ? 0 : value(rackEq(band, 2))),
                    5, 5);
    }
  };
  auto dragging = std::make_shared<bool>(false);
  auto anchor = std::make_shared<QPointF>();
  graph->press = [this, graph, dragging, anchor](QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
      return;
    int nearest = 0;
    double distance = 1.e12;
    for (int band = 0; band < 6; ++band) {
      const double x = 20 + std::log(value(rackEq(band, 1)) / 20) /
                                std::log(1000) * (graph->width() - 40);
      const double d = std::abs(x - e->position().x());
      if (d < distance) {
        distance = d;
        nearest = band;
      }
    }
    m_eqBand->setCurrentIndex(nearest);
    *anchor = e->position();
    *dragging = true;
  };
  graph->move = [this, graph, dragging, anchor](QMouseEvent *e) {
    if (!*dragging)
      return;
    const int band = m_eqBand->currentIndex();
    const auto delta = (e->position() - *anchor) *
                       (e->modifiers().testFlag(Qt::ShiftModifier) ? .15 : 1.);
    *anchor = e->position();
    change(rackEq(band, 1),
           std::clamp(value(rackEq(band, 1)) *
                          std::pow(1000, delta.x() / (graph->width() - 40)),
                      20., 20000.),
           false);
    if (band > 0 && band < 5)
      change(rackEq(band, 2),
             std::clamp(value(rackEq(band, 2)) -
                            delta.y() / (graph->height() - 32) * 36,
                        -18., 18.),
             false);
    syncEq();
    graph->update();
  };
  graph->release = [this, dragging](QMouseEvent *) {
    *dragging = false;
    emit finish(-1);
  };
  graph->wheel = [this](QWheelEvent *e) {
    const int index = rackEq(m_eqBand->currentIndex(), 3);
    change(index,
           std::clamp(value(index) + e->angleDelta().y() / 120. * .1, .1, 30.));
    e->accept();
  };
  col->addWidget(graph);
  auto *row = new QHBoxLayout;
  m_eqBand = new QComboBox(content);
  m_eqBand->setObjectName("eq-band");
  m_eqBand->setAccessibleName(tr("Selected EQ band"));
  m_eqBand->addItems(
      {"Low Cut", "Low", "Low Mid", "High Mid", "High", "High Cut"});
  row->addWidget(m_eqBand);
  m_eqOn = new Button(tr("On"), content);
  m_eqOn->setCheckable(true);
  m_eqOn->setObjectName("band-power");
  row->addWidget(m_eqOn);
  const auto field = [&](const QString &name, double lo, double hi,
                         const QString &suffix, int field) {
    auto *box = new QDoubleSpinBox(content);
    box->setRange(lo, hi);
    box->setDecimals(field == 1 ? 0 : 2);
    box->setSuffix(suffix);
    box->setAccessibleName(name);
    box->setObjectName("eq-" + name.toLower());
    box->setKeyboardTracking(false);
    row->addWidget(box);
    connect(box, &QDoubleSpinBox::valueChanged, this, [this, field](double v) {
      change(rackEq(m_eqBand->currentIndex(), field), v);
    });
    box->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(box, &QWidget::customContextMenuRequested, this,
            [this, box, field](QPoint pos) {
              QMenu menu;
              auto *a = menu.addAction(tr("Create automation"));
              if (menu.exec(box->mapToGlobal(pos)) == a)
                emit automate(rackEq(m_eqBand->currentIndex(), field));
            });
    return box;
  };
  m_eqFrequency = field("Frequency", 20, 20000, " Hz", 1);
  m_eqGain = field("Gain", -18, 18, " dB", 2);
  m_eqQ = field("Q", .1, 30, " Q", 3);
  connect(m_eqBand, &QComboBox::currentIndexChanged, this,
          [this] { syncEq(); });
  connect(m_eqOn, &QAbstractButton::clicked, this, [this](bool on) {
    change(rackEq(m_eqBand->currentIndex(), 0), on ? 1 : 0);
  });
  col->addLayout(row);
  content->hide();
  root->addWidget(content);
  connect(expand, &QAbstractButton::clicked, this, [this, content](bool on) {
    content->setVisible(on);
    emit eqExpanded(on);
  });
}
void NativePluginView::syncEq() {
  if (!m_eqBand)
    return;
  const int band = m_eqBand->currentIndex();
  QSignalBlocker a(m_eqFrequency), b(m_eqGain), c(m_eqQ), d(m_eqOn);
  if (!m_eqFrequency->hasFocus())
    m_eqFrequency->setValue(value(rackEq(band, 1)));
  if (!m_eqGain->hasFocus())
    m_eqGain->setValue(value(rackEq(band, 2)));
  if (!m_eqQ->hasFocus())
    m_eqQ->setValue(value(rackEq(band, 3)));
  m_eqOn->setChecked(value(rackEq(band, 0)) > .5);
  m_eqGain->setEnabled(band > 0 && band < 5);
}

void NativePluginView::setSnapshot(const QVariantMap &state) {
  if (!state.value("available", true).toBool()) {
    setEnabled(false);
    return;
  }
  const auto previous = m_state;
  m_state = state;
  if (!m_built && m_kind == Kind::Modulation &&
      state.value("available").toBool()) {
    m_parameters = state["params"].toList();
    buildModulation();
    m_built = true;
  }
  setEnabled(state.value("available", true).toBool());
  if (!m_built)
    return;
  for (auto it = m_knobs.begin(); it != m_knobs.end(); ++it)
    if (!it.value()->isEditing() && !(m_kind == Kind::Delay && it.key() == 1))
      it.value()->setValue(value(it.key()));
  for (auto it = m_numbers.begin(); it != m_numbers.end(); ++it) {
    const double scale = m_kind == Kind::Modulation &&
                                 m_parameters[it.key()].toMap()["unit"] == "%"
                             ? 100
                             : 1;
    QSignalBlocker block(it.value());
    if (!it.value()->hasFocus())
      it.value()->setValue(value(it.key()) * scale);
  }
  for (auto it = m_choices.begin(); it != m_choices.end(); ++it) {
    QSignalBlocker block(it.value());
    it.value()->setCurrentIndex(int(value(it.key())));
  }
  for (auto it = m_toggles.begin(); it != m_toggles.end(); ++it) {
    QSignalBlocker block(it.value());
    it.value()->setChecked(value(it.key()) > .5);
  }
  for (auto it = m_segments.begin(); it != m_segments.end(); ++it)
    for (int i = 0; i < it.value().size(); ++i)
      it.value()[i]->setChecked(i == int(value(it.key())));
  if (m_active)
    m_active->setChecked(state["active"].toBool());
  if (m_preset) {
    QStringList factories;
    if (m_kind == Kind::Pitch)
      factories = {"Natural", "Pop", "Tight", "Hard"};
    else
      for (const auto &v : state["factories"].toList())
        factories << v.toString();
    QStringList users;
    if (m_kind == Kind::Pitch)
      users = state["presets"].toStringList();
    else
      for (const auto &v : state["presets"].toList())
        users << v.toMap()["name"].toString();
    if (previous["presets"] != state["presets"] || m_preset->count() == 0) {
      QSignalBlocker b(m_preset);
      m_preset->clear();
      m_preset->addItem("");
      for (int i = 0; i < factories.size(); ++i)
        m_preset->addItem(factories[i], "factory:" + QString::number(i));
      for (const auto &name : users)
        m_preset->addItem(name, "user:" + name);
    }
    m_preset->setItemText(0, state["preset"].toString() +
                                 (state["modified"].toBool() ? " *" : ""));
    if (!m_preset->view()->isVisible())
      m_preset->setCurrentIndex(0);
  }
  if (m_kind == Kind::Delay) {
    const int mode = int(value(0));
    m_divisionReadout->setVisible(mode != 2);
    m_numbers[1]->setVisible(mode == 2);
    m_divisionReadout->setText(QString::fromUtf8(
        daw::plugins::delay::divisionName(unsigned(value(2)))));
    m_tap->setEnabled(mode != 0);
    m_choices[2]->setEnabled(mode != 2);
    m_choices[2]->setVisible(mode != 2);
    m_numbers[3]->setEnabled(mode == 1);
    m_numbers[1]->setEnabled(mode == 2);
    auto *time = static_cast<Dial *>(m_knobs[1]);
    if (!time->isEditing()) {
      const bool ms = mode == 2;
      const int index = ms ? 1 : 2;
      auto p = m_parameters[index].toMap();
      if (time->maximumValue() != p["max"].toDouble()) {
        time->setRange(p["min"].toDouble(), p["max"].toDouble());
        time->setDefaultValue(p["default"].toDouble());
        time->setStepped(!ms);
        time->setLogarithmic(ms);
        time->logarithmic = ms;
        time->setFormatter([ms](double v) {
          return ms ? QString::number(v, 'f', 1) + " ms"
                    : QString::fromUtf8(
                          daw::plugins::delay::divisionName(unsigned(v)));
        });
      }
      time->setValue(value(index));
    }
    if (mode != 2) {
      QSignalBlocker b(m_numbers[1]);
      m_numbers[1]->setValue(state["milliseconds"].toDouble());
    }
    if (mode != 1) {
      QSignalBlocker block(m_numbers[3]);
      m_numbers[3]->setValue(state["bpm"].toDouble());
    }
    m_timing->setText(
        QString::number(state["milliseconds"].toDouble(), 'f', 1) + " ms");
    m_status->setText(state["limited"].toBool()
                          ? tr("Delay limited to 8 seconds")
                          : QStringLiteral("STEREO ECHO PROCESSOR     ·     ") +
                                (state["wet"].toDouble() > .0001
                                     ? tr("SIGNAL")
                                     : tr("READY")));
  }
  if (m_kind == Kind::Pitch) {
    for (int i = 0; i < m_notes.size(); ++i) {
      m_notes[i]->setChecked((state["mask"].toInt() & (1 << i)) != 0);
      const bool target = state["targetNote"].toInt() == i;
      if (m_notes[i]->property("targetNote").toBool() != target) {
        m_notes[i]->setProperty("targetNote", target);
        m_notes[i]->update();
      }
    }
    m_choices[11]->setEnabled(!state["busy"].toBool());
    m_timing->setText(
        state["input"].toString() + "  ›  " + state["target"].toString() +
        "\n" + QString::number(state["cents"].toDouble(), 'f', 0) + " ct");
    m_status->setText(
        (state["status"].toString().isEmpty()
             ? tr("Click keys to set your scale")
             : state["status"].toString()) +
        tr("   ·   %1 ms latency").arg(state["latency"].toDouble(), 0, 'f', 1));
  }
  if (m_kind == Kind::Modulation) {
    const auto order = state["order"].toList();
    if (previous["order"] != order)
      for (const auto &module : order)
        for (auto *card : m_cards)
          if (card->property("module") == module) {
            m_cardLayout->removeWidget(card);
            m_cardLayout->addWidget(card, 1);
          }
    QStringList chain;
    for (const auto &v : order)
      chain << moduleNames[v.toInt()].toUpper();
    m_status->setText(
        chain.join("  →  ") + "     ·     " +
        (state["active"].toBool()
             ? state["playing"].toBool() ? tr("PLAYING") : tr("READY")
             : tr("BYPASSED")));
    syncEq();
  }
  // Echo motion shares the visible panel's telemetry tick; other drawings
  // repaint only when their state changes. No independent animation timer.
  for (auto *display : m_displays) {
    if (auto *echo = dynamic_cast<EchoDisplay *>(display))
      echo->snapshot(state);
    else if (previous != state)
      display->update();
  }
}
void NativePluginView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), th().background);
  pluginStyle::surface(p, QRectF(rect()).adjusted(1, 1, -1, -3));
}
