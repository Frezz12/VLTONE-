#include "MiniModuleRack.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "model/MiniModules.hpp"
#include "MiniModuleLibrary.hpp"
#include "model/MiniModules.hpp"
#include "Theme.hpp"
#include <QApplication>
#include <QBoxLayout>
#include <QBuffer>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <optional>

namespace ui {
namespace {
constexpr auto kDragType = "application/x-vlt-mini-module";
using Definition = daw::plugins::mini::MiniModuleDefinition;
using Control = daw::plugins::mini::Control;
using Appearance = daw::plugins::mini::Appearance;
QString caption(const std::string &name) {
  return QCoreApplication::translate("MiniModuleRack", name.c_str());
}
QString formatted(const Control &c, double value) {
  if (c.unit == "%")
    return QString::number(value * (c.minimum >= 0 && c.maximum <= 1 ? 100 : 1), 'f', 0) + QStringLiteral("%");
  if (c.unit == "Hz")
    return QString::number(value, 'f', 2) + QStringLiteral(" Hz");
  return QString::number(value, 'f', 1) + (c.unit.empty() ? QString{} : " " + QString::fromStdString(c.unit));
}
QColor faceColor(const Appearance &a) {
  if (!a.backgroundColor.empty())
    return QColor(QString::fromStdString(a.backgroundColor));
  if (a.theme == "graphite")
    return QColor("#303640");
  if (a.theme == "ivory")
    return QColor("#d9d3c5");
  if (a.theme == "copper")
    return QColor("#765447");
  return ThemeManager::instance().theme().surface;
}
QColor inkColor(const Appearance &a) {
  if (a.theme == "studio" && a.backgroundColor.empty())
    return ThemeManager::instance().theme().textPrimary;
  const auto c = faceColor(a);
  return (c.redF() * .2126 + c.greenF() * .7152 + c.blueF() * .0722) > .57
             ? QColor("#20242b")
             : QColor("#f2f3f5");
}
QColor accentColor(const Appearance &a) {
  const auto accent = ThemeManager::instance().theme().accent;
  return faceColor(a).lightnessF() > .57 ? accent.darker(135) : accent;
}
int cardHeight(const daw::InsertModel &m, int width) {
  if (!m.miniModule || !m.miniModule->unavailableSource.empty())
    return 86;
  const int count = std::min(2, int(m.miniModule->controls.size()));
  return 34 + (count ? 64 * (width < 100 ? count : 1) : 0) +
         (m.miniModule->modes.empty() ? 0 : 24) + 24;
}
class ModuleKnob final : public Knob {
public:
  ModuleKnob(const Control &c, const Appearance &look, QWidget *parent)
      : Knob({}, parent), m_control(c), m_look(look) {
    setBare(36);
    setRange(c.minimum, c.maximum);
    setDefaultValue(c.initial);
    setLogarithmic(c.logarithmic);
    setBipolar(c.minimum < 0 && c.maximum > 0);
    setAutomatable(true);
    setFocusPolicy(Qt::StrongFocus);
    setFormatter([c](double v) { return formatted(c, v); });
  }

protected:
  void paintEvent(QPaintEvent *) override {
    const auto &t = ThemeManager::instance().theme();
    const auto ink = inkColor(m_look);
    const QString style = QString::fromStdString(
        m_control.style.empty() ? m_look.controlStyle : m_control.style);
    const qreal dpi = devicePixelRatioF();
    const QString key =
        faceColor(m_look).name() + ink.name() + t.accent.name() + style;
    if (m_face.isNull() || m_dpi != dpi || key != m_key ||
        m_face.size() != size() * dpi) {
      m_dpi = dpi;
      m_key = key;
      m_face = QPixmap(size() * dpi);
      m_face.setDevicePixelRatio(dpi);
      m_face.fill(Qt::transparent);
      QPainter p(&m_face);
      p.setRenderHint(QPainter::Antialiasing);
      const auto face = faceColor(m_look);
      const auto rail = mixColors(face, ink, .18);
      p.setPen(QPen(rail, 2, Qt::SolidLine, Qt::RoundCap));
      if (style == "fader") {
        p.drawLine(QPointF(18, 5), QPointF(18, 31));
        p.setPen(QPen(rail, 1));
        for (int i = 0; i < 5; ++i)
          p.drawLine(QPointF(8, 5 + i * 6.5), QPointF(11, 5 + i * 6.5));
      } else {
        p.setBrush(Qt::NoBrush);
        // Keep the persisted style IDs while giving every variant the same
        // digital vocabulary as the mixer: ring, disc, segments and fader.
        if (style == "glass") {
          for (int i = 0; i <= 18; ++i) {
            const double a = (135 + i * 15) * 3.141592653589793 / 180;
            const QPointF d(std::cos(a), std::sin(a));
            p.drawLine(QPointF(18, 18) + 13.5 * d, QPointF(18, 18) + 15.5 * d);
          }
        } else {
          p.drawArc(QRectF(4, 4, 28, 28), 225 * 16, -270 * 16);
          p.setPen(QPen(mixColors(face, ink, .4), 1));
          for (int i = 0; i < 3; ++i) {
            const double a = (135 + i * 135) * 3.141592653589793 / 180;
            const QPointF d(std::cos(a), std::sin(a));
            p.drawLine(QPointF(18, 18) + 16.5 * d, QPointF(18, 18) + 17.5 * d);
          }
        }
        p.setPen(QPen(mixColors(face, ink, .12), 1));
        p.setBrush(mixColors(face, ink, style == "rubber" ? .15 : .035));
        p.drawEllipse(QRectF(7.5, 7.5, 21, 21));
      }
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.drawPixmap(0, 0, m_face);
    const auto &c = m_control;
    const double f = std::clamp(
        c.logarithmic ? std::log(std::max(value(), c.minimum) / c.minimum) /
                            std::log(c.maximum / c.minimum)
                      : (value() - c.minimum) / (c.maximum - c.minimum),
        0., 1.);
    const auto accent = accentColor(m_look);
    const double origin = c.minimum < 0 && c.maximum > 0
                              ? -c.minimum / (c.maximum - c.minimum)
                              : 0.;
    p.setPen(QPen(accent, 2.2, Qt::SolidLine, Qt::RoundCap));
    if (style == "fader") {
      const double y = 31 - 26 * f;
      p.drawLine(QPointF(18, 31 - 26 * origin), QPointF(18, y));
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawRoundedRect(QRectF(10, y - 2, 16, 4), 1.5, 1.5);
    } else {
      p.setBrush(Qt::NoBrush);
      if (style == "glass") {
        for (int i = 0; i <= 18; ++i) {
          const double at = i / 18.;
          if (at < std::min(origin, f) || at > std::max(origin, f))
            continue;
          const double a = (135 + i * 15) * 3.141592653589793 / 180;
          const QPointF d(std::cos(a), std::sin(a));
          p.drawLine(QPointF(18, 18) + 13.5 * d, QPointF(18, 18) + 15.5 * d);
        }
      } else {
        p.drawArc(QRectF(4, 4, 28, 28), qRound((225 - 270 * origin) * 16),
                  qRound(-270 * (f - origin) * 16));
      }
      const double a = (135 + 270 * f) * 3.141592653589793 / 180;
      const QPointF dir(std::cos(a), std::sin(a));
      p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(18, 18) + 4 * dir, QPointF(18, 18) + 9 * dir);
    }
    if (hasFocus()) {
      p.setPen(QPen(t.accent, 1.5));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(.75, .75, 34.5, 34.5), 4, 4);
    }
  }

private:
  Control m_control;
  Appearance m_look;
  QPixmap m_face;
  QString m_key;
  qreal m_dpi = 0;
};
class ModulePower final : public QPushButton {
public:
  ModulePower(const Appearance &look, QWidget *parent)
      : QPushButton(parent), m_look(look) {
    setObjectName("MiniModulePower");
    setCheckable(true);
    setFixedSize(24, 24);
    setFocusPolicy(Qt::StrongFocus);
    setAutoDefault(false);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    const auto &t = ThemeManager::instance().theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto face = faceColor(m_look);
    const auto ink = inkColor(m_look);
    const auto accent = accentColor(m_look);
    const auto lamp = isChecked() ? accent : mixColors(face, ink, .55);
    p.setPen(QPen(mixColors(face, lamp, isChecked() ? .35 : .15), 1));
    p.setBrush(mixColors(face, lamp,
                         isDown()       ? .25
                         : underMouse() ? .16
                                        : .07));
    p.drawRoundedRect(QRectF(3, 3, 18, 18), 4, 4);
    p.setPen(QPen(lamp, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(7.5, 7.5, 9, 9), 130 * 16, 280 * 16);
    p.drawLine(QPointF(12, 6.5), QPointF(12, 11));
    if (hasFocus()) {
      p.setPen(QPen(t.accent, 1.3));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
    }
  }

private:
  Appearance m_look;
};
class ModuleHeader final : public QPushButton {
public:
  ModuleHeader(QString title, QString token, QColor ink, QWidget *parent)
      : QPushButton(title, parent), m_token(token), m_ink(ink) {
    setObjectName("MiniModuleTitle");
    setFixedHeight(24);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setFocusPolicy(Qt::StrongFocus);
    setAutoDefault(false);
    QFont f = font();
    f.setPixelSize(10);
    f.setWeight(QFont::DemiBold);
    setFont(f);
    setToolTip(title);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setPen(m_ink);
    p.drawText(rect(), Qt::AlignCenter,
               fontMetrics().elidedText(text(), Qt::ElideRight, width()));
    if (hasFocus()) {
      p.setPen(ThemeManager::instance().theme().accent);
      p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
  }
  void mousePressEvent(QMouseEvent *e) override {
    m_start = e->pos();
    QPushButton::mousePressEvent(e);
  }
  void mouseMoveEvent(QMouseEvent *e) override {
    if (e->buttons().testFlag(Qt::LeftButton) &&
        (e->pos() - m_start).manhattanLength() >=
            QApplication::startDragDistance()) {
      setDown(false);
      auto *drag = new QDrag(this);
      auto *mime = new QMimeData;
      mime->setData(kDragType, m_token.toUtf8());
      drag->setMimeData(mime);
      drag->exec(Qt::MoveAction);
    } else
      QPushButton::mouseMoveEvent(e);
  }

private:
  QString m_token;
  QPoint m_start;
  QColor m_ink;
};
class ControlLabel final : public QLabel {
public:
  ControlLabel(const QString &name, QWidget *parent)
      : QLabel(name, parent), m_full(name) {
    setTextFormat(Qt::PlainText);
    setToolTip(name);
    setAccessibleName(name);
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QLabel::resizeEvent(event);
    const auto metrics = fontMetrics();
    const int available = std::max(0, width() - 2);
    QString label = m_full;
    if (metrics.horizontalAdvance(label) > available &&
        label == caption("Humanize"))
      label = caption("Human.");
    setText(metrics.elidedText(label, Qt::ElideRight, available));
  }

private:
  QString m_full;
};
class ModuleSelector final : public QComboBox {
public:
  using QComboBox::QComboBox;

protected:
  void paintEvent(QPaintEvent *) override {
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const auto field = style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
    option.currentText = fontMetrics().elidedText(
        currentText(), Qt::ElideRight, std::max(0, field.width() - 2));
    QStylePainter p(this);
    p.drawComplexControl(QStyle::CC_ComboBox, option);
    p.drawControl(QStyle::CE_ComboBoxLabel, option);
  }
};
QComboBox *selector(QWidget *parent, const QString &name) {
  auto *box = new ModuleSelector(parent);
  box->setObjectName(name);
  box->setFixedHeight(22);
  box->setMinimumWidth(0);
  box->setMinimumContentsLength(1);
  box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  box->setFocusPolicy(Qt::StrongFocus);
  return box;
}
} // namespace

class MiniModuleCard final : public QWidget {
  Q_DECLARE_TR_FUNCTIONS(MiniModuleCard)
public:
  struct Dial {
    Control control;
    ModuleKnob *knob;
    QLabel *value;
    std::shared_ptr<std::optional<double>> before;
  };
  MiniModuleCard(daw::EngineController *controller, QString channel,
                 QString project, const daw::InsertModel &model,
                 QWidget *parent)
      : QWidget(parent), m_controller(controller), m_channel(channel),
        m_model(model) {
    Q_UNUSED(project);
    if (model.miniModule)
      m_look = model.miniModule->appearance;
    const auto ink = inkColor(m_look);
    setObjectName("MiniModuleCard");
    setAccessibleName(QString::fromStdString(model.name));
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(1, 2, 1, 2);
    column->setSpacing(0);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(0);
    m_power = new ModulePower(m_look, this);
    m_power->setAccessibleName(tr("Enable module"));
    auto *title = new ModuleHeader(
        QString::fromStdString(model.name),
        channel + "\n" + QString::fromStdString(model.id), ink, this);
    title->setToolTip(
        tr("Drag to reorder; click to expand or collapse all racks"));
    auto *menu = new IconButton(icons::Glyph::Gear, tr("Module menu"), this);
    menu->setObjectName("MiniModuleMenu");
    menu->setIdleColor(ink);
    menu->setButtonSize(18, 24);
    menu->setFocusPolicy(Qt::StrongFocus);
    bar->addWidget(m_power);
    bar->addWidget(title, 1);
    bar->addWidget(menu);
    column->addLayout(bar);
    m_body = new QWidget(this);
    auto *body = new QVBoxLayout(m_body);
    body->setContentsMargins(0, 4, 0, 2);
    body->setSpacing(2);
    m_controls = new QBoxLayout(QBoxLayout::LeftToRight);
    m_controls->setContentsMargins(0, 0, 0, 0);
    m_controls->setSpacing(0);
    body->addLayout(m_controls);
    column->addWidget(m_body);
    const auto error = model.miniModule
                           ? daw::plugins::mini::validate(*model.miniModule,
                                                          model.miniModuleMode)
                           : "Missing module definition";
    if (!error.empty()) {
      auto *label = new QLabel(tr("Unavailable"), m_body);
      label->setToolTip(QString::fromStdString(error));
      label->setWordWrap(true);
      m_controls->addWidget(label);
    } else
      for (const auto &c : model.miniModule->controls) {
        auto *group = new QWidget(m_body);
        group->setMinimumWidth(0);
        group->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto *stack = new QVBoxLayout(group);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->setSpacing(0);
        auto *name = new ControlLabel(caption(c.name), group);
        name->setAlignment(Qt::AlignCenter);
        name->setFixedHeight(14);
        name->setMinimumWidth(0);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        name->setToolTip(caption(c.name));
        name->setStyleSheet("font-size:10px;background:transparent;color:" +
                            mixColors(faceColor(m_look), ink, .78).name() +
                            ";");
        auto *knob = new ModuleKnob(c, m_look, group);
        knob->setObjectName(QString::fromStdString(c.id));
        knob->setAccessibleName(QString::fromStdString(model.name) + " " +
                                caption(c.name));
        auto *value = new QLabel(group);
        value->setAlignment(Qt::AlignCenter);
        value->setFixedHeight(14);
        value->setMinimumWidth(0);
        value->setStyleSheet(
            "font-size:10px;background:transparent;color:" + ink.name() + ";");
        stack->addWidget(name);
        stack->addWidget(knob, 0, Qt::AlignHCenter);
        stack->addWidget(value);
        group->setFixedHeight(64);
        m_controls->addWidget(group, 1);
        auto before = std::make_shared<std::optional<double>>();
        m_dials.push_back({c, knob, value, before});
        connect(knob, &Knob::valueChanged, this,
                [this, c, before, value](double next) {
                  if (!*before)
                    *before = m_controller->insertParameter(
                        m_channel.toStdString(), m_model.id, c.id);
                  m_controller->setInsertParameter(m_channel.toStdString(),
                                                   m_model.id, c.id, next);
                  value->setText(formatted(c, next));
                  if (edited)
                    edited(false);
                });
        connect(knob, &Knob::editFinished, this, [this, c, before] {
          if (!*before)
            return;
          const double old = **before;
          before->reset();
          if (!m_controller->insertModel(m_channel.toStdString(), m_model.id))
            return;
          m_controller->commitInsertParameterEdit(
              m_channel.toStdString(), m_model.id, c.id, old,
              "Change " + m_model.name + " " + c.name);
          if (edited)
            edited(true);
        });
        connect(knob, &Knob::automateRequested, this, [this, c] {
          if (automate)
            automate(QString::fromStdString(c.id));
        });
      }
    if (error.empty() && !model.miniModule->modes.empty()) {
      m_mode = selector(m_body, "MiniModuleMode");
      m_mode->setAccessibleName(tr("Module mode"));
      m_mode->setToolTip(tr("Sound mode"));
      for (const auto &mode : model.miniModule->modes)
        m_mode->addItem(caption(mode.name), QString::fromStdString(mode.id));
      body->addWidget(m_mode);
      connect(m_mode, qOverload<int>(&QComboBox::activated), this,
              [this](int i) {
                finish();
                m_controller->setMiniModuleMode(
                    m_channel.toStdString(), m_model.id,
                    m_mode->itemData(i).toString().toStdString());
                if (edited)
                  edited(true);
              });
    }
    m_route = selector(m_body, "MiniModulePosition");
    m_route->setAccessibleName(tr("Position relative to Audio FX"));
    m_route->addItem(tr("Pre FX"));
    m_route->addItem(tr("Post FX"));
    m_route->setToolTip(tr("Before or after Audio FX; both before the fader"));
    body->addWidget(m_route);
    connect(m_route, qOverload<int>(&QComboBox::activated), this,
            [this](int i) {
              finish();
              m_controller->setMiniModulePostFx(m_channel.toStdString(),
                                                m_model.id, i == 1);
              if (edited)
                edited(true);
            });
    const QString comboStyle =
        "QComboBox{font-size:10px;padding:0 3px;border:1px solid " +
        mixColors(faceColor(m_look), ink, .16).name() +
        ";border-radius:3px;background:" +
        mixColors(faceColor(m_look), ink, .035).name() +
        ";color:" + ink.name() +
        ";}"
        "QComboBox:hover{border-color:" +
        mixColors(faceColor(m_look), ink, .32).name() +
        ";}"
        "QComboBox:focus{border-color:" +
        ThemeManager::instance().theme().accent.name() +
        ";} QComboBox::drop-down{width:13px;border:0;}";
    m_route->setStyleSheet(comboStyle);
    if (m_mode)
      m_mode->setStyleSheet(comboStyle);
    connect(m_power, &QPushButton::clicked, this, [this](bool on) {
      m_controller->setInsertBypassed(m_channel.toStdString(), m_model.id, !on);
      if (edited)
        edited(true);
    });
    connect(menu, &QAbstractButton::clicked, this, [this, menu] {
      finish();
      if (openMenu)
        openMenu(menu->mapToGlobal(QPoint(0, 24)));
    });
    title->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(title, &QWidget::customContextMenuRequested, this,
            [this, title](QPoint p) {
              finish();
              if (openMenu)
                openMenu(title->mapToGlobal(p));
            });
    if (!m_look.backgroundImage.empty()) {
      const auto bytes = QByteArray::fromBase64(
          QByteArray::fromStdString(m_look.backgroundImage.substr(22)));
      QBuffer source;
      source.setData(bytes);
      source.open(QIODevice::ReadOnly);
      QImageReader reader(&source, "png");
      const auto size = reader.size();
      if (size.isValid() && size.width() <= 2048 && size.height() <= 2048) {
        reader.setScaledSize(size.scaled(512, 512, Qt::KeepAspectRatio));
        m_background = reader.read();
      }
    }
    sync(model, false);
  }
  ~MiniModuleCard() override { finish(); }
  void finish() {
    for (auto &d : m_dials)
      d.knob->finishEditing();
  }
  void sync(const daw::InsertModel &model, bool automation) {
    m_model.parameters = model.parameters;
    m_model.bypassed = model.bypassed;
    m_model.miniModuleMode = model.miniModuleMode;
    m_model.miniModulePostFx = model.miniModulePostFx;
    {
      QSignalBlocker b(m_power);
      m_power->setChecked(!model.bypassed);
      m_power->setToolTip(model.bypassed ? tr("Enable module")
                                         : tr("Bypass module"));
    }
    {
      QSignalBlocker b(m_route);
      m_route->setCurrentIndex(model.miniModulePostFx ? 1 : 0);
    }
    if (m_mode && model.miniModule) {
      QSignalBlocker b(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(QString::fromStdString(
          model.miniModuleMode.empty() ? model.miniModule->defaultMode
                                       : model.miniModuleMode)));
      m_mode->setToolTip(tr("Sound mode") + ": " + m_mode->currentText());
    }
    for (auto &d : m_dials)
      if (!d.knob->isEditing()) {
        double value = d.control.initial;
        for (const auto &p : model.parameters)
          if (p.id == d.control.id)
            value = p.value;
        if (automation && m_controller->isPlaying()) {
          daw::AutomationTarget target;
          target.kind = daw::AutomationTargetKind::PluginParameter;
          target.channelId = m_channel.toStdString();
          target.slotId = model.id;
          target.parameterId = d.control.id;
          if (auto at = m_controller->automationValueAtPlayhead(target))
            value = *at;
        }
        const QSignalBlocker b(d.knob);
        d.knob->setValue(value);
        d.value->setText(formatted(d.control, value));
      }
    update();
  }
  void fit(int width) {
    m_controls->setDirection(width < 100 ? QBoxLayout::TopToBottom
                                         : QBoxLayout::LeftToRight);
    setFixedHeight(cardHeight(m_model, width));
  }
  std::function<void(bool)> edited;
  std::function<void(const QString &)> automate;
  std::function<void(const QPoint &)> openMenu;

protected:
  void paintEvent(QPaintEvent *) override {
    const auto face = faceColor(m_look);
    const auto &t = ThemeManager::instance().theme();
    const QString key = face.name() + t.surface.name() +
                        QString::number(width()) + ":" +
                        QString::number(height());
    const auto dpi = devicePixelRatioF();
    if (m_shell.isNull() || m_dpi != dpi || m_key != key) {
      m_dpi = dpi;
      m_key = key;
      m_shell = QPixmap(size() * dpi);
      m_shell.setDevicePixelRatio(dpi);
      m_shell.fill(Qt::transparent);
      QPainter p(&m_shell);
      p.setRenderHint(QPainter::Antialiasing);
      const auto ink = inkColor(m_look);
      const QRectF panel = QRectF(rect()).adjusted(.5, .5, -.5, -1.5);
      p.setPen(QPen(mixColors(face, ink, .16), 1));
      p.setBrush(face);
      p.drawRoundedRect(panel, 3, 3);
      if (!m_background.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(panel.adjusted(1, 1, -1, -1), 2, 2);
        p.save();
        p.setClipPath(clip);
        const auto image =
            m_background.scaled(size() * dpi, Qt::KeepAspectRatioByExpanding,
                                Qt::SmoothTransformation);
        p.drawImage(panel, image,
                    QRectF((image.width() - width() * dpi) / 2,
                           (image.height() - height() * dpi) / 2, width() * dpi,
                           height() * dpi));
        QColor veil = face;
        veil.setAlpha(205);
        p.fillRect(panel, veil);
        p.restore();
      }
      p.setPen(QPen(mixColors(face, ink, .10), 1));
      p.drawLine(QPointF(3, 27.5), QPointF(width() - 3, 27.5));
    }
    QPainter p(this);
    p.drawPixmap(0, 0, m_shell);
  }

private:
  daw::EngineController *m_controller;
  QString m_channel;
  daw::InsertModel m_model;
  Appearance m_look;
  ModulePower *m_power;
  QWidget *m_body;
  QBoxLayout *m_controls;
  QComboBox *m_mode = nullptr;
  QComboBox *m_route;
  std::vector<Dial> m_dials;
  QImage m_background;
  QPixmap m_shell;
  QString m_key;
  qreal m_dpi = 0;
};

MiniModuleRack::MiniModuleRack(daw::EngineController *controller,
                               QString channel, QWidget *parent)
    : QWidget(parent), m_controller(controller), m_channel(channel),
      m_project(
          QString::fromStdString(controller->project().miniModuleProjectId)) {
  setObjectName("MiniModuleRack");
  setAccessibleName(tr("Mini modules"));
  setAcceptDrops(true);
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(2);
  auto *caption = new QLabel(tr("MINI"), this);
  caption->setObjectName("ChannelSectionCaption");
  caption->setProperty("role", "section");
  caption->setFixedHeight(17);
  caption->setToolTip(tr("Mini modules"));
  auto font = caption->font();
  font.setPixelSize(9);
  font.setWeight(QFont::Medium);
  caption->setFont(font);
  outer->addWidget(caption);
  m_well = new QWidget(this);
  m_well->setObjectName("SlotWell");
  m_well->setAttribute(Qt::WA_StyledBackground, true);
  outer->addWidget(m_well, 1);
  m_column = new QVBoxLayout(m_well);
  m_column->setContentsMargins(1, 1, 1, 1);
  m_column->setSpacing(1);
  connect(&ThemeManager::instance(), &ThemeManager::changed, this,
          [this] { rebuild(); });
  sync(false);
}
MiniModuleRack::~MiniModuleRack() { finishEdits(); }
QWidget *MiniModuleRack::createPreview(const Definition &definition, int width, QWidget *parent) {
  // Preview has the exact production card and dimensions. Its controls are
  // display-only and never address a project insert.
  auto model = daw::makeMiniModule(definition);
  auto *card = new MiniModuleCard(nullptr, {}, {}, model, parent);
  card->setAttribute(Qt::WA_TransparentForMouseEvents);
  for (auto *child : card->findChildren<QWidget *>()) child->setFocusPolicy(Qt::NoFocus);
  card->setFixedWidth(width);
  card->fit(width);
  return card;
}
void MiniModuleRack::hideEvent(QHideEvent *e) {
  finishEdits();
  QWidget::hideEvent(e);
}
void MiniModuleRack::finishEdits() {
  for (auto *c : m_cards)
    c->finish();
}
int MiniModuleRack::naturalHeight(const daw::ProjectModel &,
                                  const std::vector<daw::InsertModel> &modules,
                                  int width) {
  int height = 21;
  for (const auto &m : modules)
    height += cardHeight(m, width) + 1;
  if (modules.size() < 3)
    height += 28;
  return height;
}
int MiniModuleRack::naturalHeight() const {
  return naturalHeight(m_controller->project(), m_models, m_stripWidth);
}
void MiniModuleRack::setRackHeight(int height) {
  m_assignedHeight = height;
  setFixedHeight(std::max(height, naturalHeight()));
}
void MiniModuleRack::setStripWidth(int width) {
  m_stripWidth = width;
  for (auto *card : m_cards)
    card->fit(width);
  setFixedHeight(std::max(m_assignedHeight, naturalHeight()));
  updateGeometry();
}
void MiniModuleRack::sync(bool automation) {
  if (m_syncing)
    return;
  QScopedValueRollback<bool> syncing(m_syncing, true);
  const auto &modules = m_controller->miniModules(m_channel.toStdString());
  const bool changed =
      modules.size() != m_models.size() ||
      !std::equal(modules.begin(), modules.end(), m_models.begin(),
                  [](const auto &a, const auto &b) {
                    return a.id == b.id && a.name == b.name &&
                           a.miniModule == b.miniModule;
                  });
  if (changed) {
    finishEdits();
    m_models = modules;
    rebuild();
  }
  for (unsigned i = 0; i < modules.size(); ++i)
    m_cards[i]->sync(modules[i], automation);
  for (unsigned i = 0; i < modules.size(); ++i) {
    m_models[i].parameters = modules[i].parameters;
    m_models[i].bypassed = modules[i].bypassed;
    m_models[i].miniModuleMode = modules[i].miniModuleMode;
    m_models[i].miniModulePostFx = modules[i].miniModulePostFx;
  }
  if (m_column->count() == 0)
    rebuild();
}
void MiniModuleRack::rebuild() {
  finishEdits();
  m_cards.clear();
  while (auto *item = m_column->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  for (const auto &model : m_models) {
    auto *card =
        new MiniModuleCard(m_controller, m_channel, m_project, model, m_well);
    const auto id = QString::fromStdString(model.id);
    card->edited = [this](bool undo) { emit edited(undo); };
    card->automate = [this, id](const QString &p) {
      emit automateRequested(id, p);
    };
    card->openMenu = [this, id](const QPoint &p) { cardMenu(id, p); };
    m_cards.push_back(card);
    m_column->addWidget(card);
  }
  auto *add = new QPushButton(QStringLiteral("+"), m_well);
  add->setObjectName("AddMiniModule");
  add->setMinimumHeight(28);
  add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  add->setStyleSheet(
      "QPushButton{border:0;background:transparent;font-size:14px;} "
      "QPushButton:hover{background:rgba(128,128,128,18);}");
  add->setAccessibleName(tr("Add mini module"));
  add->setToolTip(tr("Add mini module (maximum three)"));
  add->setEnabled(m_models.size() < 3);
  add->setVisible(m_models.size() < 3);
  connect(add, &QPushButton::clicked, this,
          [this, add] { addMenu(add->mapToGlobal(QPoint(0, add->height()))); });
  m_column->addWidget(add, 1);
  m_assignedHeight = 0;
  setStripWidth(m_stripWidth);
  emit layoutChanged();
}
void MiniModuleRack::addMenu(const QPoint &point, const QString &replace) {
  QMenu menu(this);
  menu.setToolTipsVisible(true);
  const auto add = [&](const Definition &definition,
                       const QString &error = {}) {
    auto *action = menu.addAction(QString::fromStdString(definition.name));
    action->setEnabled(error.isEmpty());
    action->setToolTip(error);
    connect(action, &QAction::triggered, this, [this, definition, replace] {
      finishEdits();
      const bool changed = replace.isEmpty()
          ? !m_controller->addMiniModule(m_channel.toStdString(), definition).empty()
          : m_controller->replaceMiniModule(m_channel.toStdString(),
                                             replace.toStdString(), definition);
      if (!changed) {
        QMessageBox::warning(this, tr("Mini modules"),
            m_controller->hasCloudProjectBinding()
                ? tr("Mini modules are not yet available in shared projects.")
                : tr("Could not add the module. Check that the channel still exists and has fewer than three modules."));
        return;
      }
      sync(false);
      emit edited();
    });
  };
  for (const auto *type : {"color", "doubler", "chorus"})
    add(daw::plugins::mini::builtin(type));
  const auto files = MiniModuleLibrary::entries();
  if (!files.empty())
    menu.addSeparator();
  for (const auto &file : files)
    add(file.definition, file.error);
  menu.addSeparator();
  auto *folder = menu.addAction(tr("Open mini modules folder"));
  connect(folder, &QAction::triggered, this, [] {
    QDir().mkpath(MiniModuleLibrary::directory());
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(MiniModuleLibrary::directory()));
  });
  auto *import = menu.addAction(tr("Install module from file…"));
  connect(import, &QAction::triggered, this, [this] {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Install mini module"), {},
        QStringLiteral("Mini modules (*.vltmini)"));
    if (path.isEmpty())
      return;
    const auto file = MiniModuleLibrary::read(path);
    QString error = file.error;
    if (error.isEmpty()) {
      QDir().mkpath(MiniModuleLibrary::directory());
      const QString destination =
          MiniModuleLibrary::directory() + "/" + QFileInfo(path).fileName();
      if (QFileInfo::exists(destination) &&
          QFileInfo(destination).absoluteFilePath() !=
              QFileInfo(path).absoluteFilePath()) {
        QMessageBox::information(
            this, tr("Mini modules"),
            tr("A file with this name is already installed."));
        return;
      }
      MiniModuleLibrary::write(destination, file.definition, error);
    }
    if (!error.isEmpty())
      QMessageBox::warning(this, tr("Mini module unavailable"), error);
  });
  menu.exec(point);
}
void MiniModuleRack::cardMenu(const QString &id, const QPoint &point) {
  sync(false);
  QMenu menu(this);
  const auto at =
      std::find_if(m_models.begin(), m_models.end(),
                   [&](const auto &m) { return m.id == id.toStdString(); });
  if (at == m_models.end())
    return;
  const int index = int(at - m_models.begin());
  for (int direction : {-1, 1}) {
    auto *action =
        menu.addAction(direction < 0 ? tr("Move up") : tr("Move down"));
    action->setEnabled(index + direction >= 0 &&
                       index + direction < int(m_models.size()));
    connect(action, &QAction::triggered, this, [this, id, index, direction] {
      m_controller->moveMiniModule(m_channel.toStdString(), id.toStdString(),
                                   index + direction);
      sync(false);
      emit edited();
    });
  }
  auto *replace = menu.addAction(tr("Replace…"));
  connect(replace, &QAction::triggered, this,
          [this, id, point] { addMenu(point, id); });
  auto *save = menu.addAction(tr("Export module…"));
  save->setEnabled(at->miniModule &&
                   daw::plugins::mini::validate(*at->miniModule).empty());
  const auto model = *at;
  auto *appearance = menu.addAction(tr("Appearance…"));
  appearance->setObjectName("MiniModuleAppearance");
  appearance->setEnabled(model.miniModule.has_value());
  connect(appearance, &QAction::triggered, this,
          [this, id] { appearanceDialog(id); });
  connect(save, &QAction::triggered, this, [this, model] {
    auto definition = *model.miniModule;
    for (auto &c : definition.controls)
      for (const auto &p : model.parameters)
        if (c.id == p.id)
          c.initial = p.value;
    for (auto &mode : definition.modes)
      for (auto &c : mode.controls)
        for (const auto &p : model.parameters)
          if (c.id == p.id)
            c.initial = p.value;
    if (!model.miniModuleMode.empty())
      definition.defaultMode = model.miniModuleMode;
    const auto path = QFileDialog::getSaveFileName(
        this, tr("Export mini module"),
        QString::fromStdString(model.name) + ".vltmini",
        QStringLiteral("Mini modules (*.vltmini)"));
    if (path.isEmpty())
      return;
    QString error;
    if (!MiniModuleLibrary::write(path.endsWith(".vltmini", Qt::CaseInsensitive)
                                      ? path
                                      : path + ".vltmini",
                                  definition, error))
      QMessageBox::warning(this, tr("Cannot export module"), error);
  });
  menu.addSeparator();
  auto *remove = menu.addAction(tr("Remove"));
  connect(remove, &QAction::triggered, this, [this, id] {
    m_controller->removeMiniModule(m_channel.toStdString(), id.toStdString());
    sync(false);
    emit edited();
  });
  menu.exec(point);
}
void MiniModuleRack::appearanceDialog(const QString &id) {
  const auto *slot =
      m_controller->insertModel(m_channel.toStdString(), id.toStdString());
  if (!slot || !slot->miniModule)
    return;
  const auto model = *slot;
  auto look = model.miniModule->appearance;
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Module appearance"));
  dialog.setObjectName("MiniModuleAppearanceDialog");
  auto *column = new QVBoxLayout(&dialog);
  auto *content = new QHBoxLayout;
  column->addLayout(content);
  auto *previewHost = new QWidget(&dialog);
  previewHost->setFixedWidth(150);
  auto *previewLayout = new QVBoxLayout(previewHost);
  previewLayout->setContentsMargins(0, 0, 0, 0);
  previewLayout->addStretch();
  content->addWidget(previewHost, 0, Qt::AlignTop);
  auto *form = new QFormLayout;
  content->addLayout(form, 1);
  auto *theme = new QComboBox(&dialog);
  theme->setObjectName("MiniAppearanceTheme");
  for (const auto &pair : {std::pair{"studio", "Studio"},
                           {"graphite", "Graphite"},
                           {"ivory", "Ivory"},
                           {"copper", "Copper"}})
    theme->addItem(caption(pair.second), QString::fromLatin1(pair.first));
  theme->setCurrentIndex(theme->findData(QString::fromStdString(look.theme)));
  form->addRow(tr("Theme"), theme);
  auto *style = new QComboBox(&dialog);
  style->setObjectName("MiniAppearanceControl");
  for (const auto &pair : {std::pair{"machined", "Ring"},
                           {"rubber", "Disc"},
                           {"glass", "Segments"},
                           {"fader", "Fader"}})
    style->addItem(caption(pair.second), QString::fromLatin1(pair.first));
  style->setCurrentIndex(
      style->findData(QString::fromStdString(look.controlStyle)));
  form->addRow(tr("Controls"), style);
  auto *color = new QPushButton(tr("Choose color…"), &dialog);
  form->addRow(tr("Background"), color);
  auto *image = new QPushButton(tr("Load image…"), &dialog);
  form->addRow(QString(), image);
  auto *clear = new QPushButton(tr("Reset background"), &dialog);
  form->addRow(QString(), clear);
  auto *note =
      new QLabel(tr("The image is embedded in the module file."), &dialog);
  note->setWordWrap(true);
  note->setMaximumWidth(220);
  form->addRow(note);
  MiniModuleCard *preview = nullptr;
  const auto refresh = [&] {
    delete preview;
    auto copy = model;
    copy.miniModule->appearance = look;
    preview = new MiniModuleCard(m_controller, m_channel, m_project, copy,
                                 previewHost);
    preview->setObjectName("MiniModuleAppearancePreview");
    preview->setAttribute(Qt::WA_TransparentForMouseEvents);
    preview->fit(150);
    for (auto *child : preview->findChildren<QWidget *>())
      child->setFocusPolicy(Qt::NoFocus);
    previewLayout->insertWidget(0, preview);
    preview->show();
    previewLayout->activate();
    clear->setEnabled(!look.backgroundColor.empty() ||
                      !look.backgroundImage.empty());
    image->setText(look.backgroundImage.empty() ? tr("Load image…")
                                                : tr("Replace image…"));
  };
  connect(theme, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
          [&](int i) {
            look.theme = theme->itemData(i).toString().toStdString();
            look.backgroundColor.clear();
            look.backgroundImage.clear();
            refresh();
          });
  connect(style, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
          [&](int i) {
            look.controlStyle = style->itemData(i).toString().toStdString();
            refresh();
          });
  connect(color, &QPushButton::clicked, &dialog, [&] {
    const auto chosen = QColorDialog::getColor(faceColor(look), &dialog,
                                               tr("Module background"));
    if (chosen.isValid()) {
      look.backgroundColor = chosen.name().toStdString();
      look.backgroundImage.clear();
      refresh();
    }
  });
  connect(clear, &QPushButton::clicked, &dialog, [&] {
    look.backgroundColor.clear();
    look.backgroundImage.clear();
    refresh();
  });
  connect(image, &QPushButton::clicked, &dialog, [&] {
    const auto path =
        QFileDialog::getOpenFileName(&dialog, tr("Module background"), {},
                                     tr("Images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
      return;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (size.isValid())
      reader.setScaledSize(size.scaled(512, 512, Qt::KeepAspectRatio));
    auto pixels = reader.read();
    if (pixels.isNull()) {
      QMessageBox::warning(&dialog, tr("Cannot load image"),
                           reader.errorString());
      return;
    }
    pixels =
        pixels.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    if (!pixels.save(&output, "PNG")) {
      QMessageBox::warning(&dialog, tr("Cannot load image"),
                           tr("Cannot encode this image."));
      return;
    }
    const auto embedded =
        std::string("data:image/png;base64,") + bytes.toBase64().toStdString();
    if (embedded.size() > 1024 * 1024) {
      QMessageBox::warning(&dialog, tr("Cannot load image"),
                           tr("The image is too large."));
      return;
    }
    look.backgroundImage = embedded;
    look.backgroundColor.clear();
    refresh();
  });
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
  column->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  refresh();
  if (dialog.exec() == QDialog::Accepted &&
      m_controller->setMiniModuleAppearance(m_channel.toStdString(),
                                            id.toStdString(), look)) {
    sync(false);
    emit edited();
  }
}
void MiniModuleRack::dragEnterEvent(QDragEnterEvent *e) {
  if (e->mimeData()->hasFormat(kDragType) &&
      e->mimeData()->data(kDragType).startsWith(m_channel.toUtf8() + "\n"))
    e->acceptProposedAction();
}
void MiniModuleRack::dropEvent(QDropEvent *e) {
  const auto parts =
      QString::fromUtf8(e->mimeData()->data(kDragType)).split('\n');
  if (parts.size() != 2 || parts[0] != m_channel)
    return;
  finishEdits();
  int at = 0;
  for (auto *card : m_cards)
    if (e->position().y() > card->geometry().center().y())
      ++at;
  auto old = std::find_if(m_models.begin(), m_models.end(), [&](const auto &m) {
    return m.id == parts[1].toStdString();
  });
  if (old == m_models.end())
    return;
  if (at > old - m_models.begin())
    --at;
  m_controller->moveMiniModule(m_channel.toStdString(), parts[1].toStdString(),
                               at);
  sync(false);
  emit edited();
  e->acceptProposedAction();
}
} // namespace ui
