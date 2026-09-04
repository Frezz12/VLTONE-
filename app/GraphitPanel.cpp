#include "GraphitPanel.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFontMetrics>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace graphit = daw::plugins::graphit;

namespace {

constexpr QColor kSurface(0x10, 0x11, 0x13);
constexpr QColor kMuted(0x8C, 0x91, 0x96);
constexpr QColor kPrimary(0xF1, 0xF3, 0xF2);
constexpr QColor kAccent(0x55, 0xE0, 0xC8);
constexpr double kPi = std::numbers::pi_v<double>;

const daw::plugins::ParameterInfo* parameterInfo(std::string_view id) {
    for (const auto& info : graphit::parameterTable())
        if (info.id == id) return &info;
    return nullptr;
}

class GraphitDial final : public ui::Knob {
public:
    explicit GraphitDial(QWidget* parent) : ui::Knob({}, parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF ring = QRectF(rect()).adjusted(5.0, 5.0, -5.0, -5.0);
        const QPointF centre = ring.center();
        const double radius = ring.width() * 0.5;
        const double fraction = std::clamp(value(), 0.0, 1.0);

        for (int tick = 0; tick < 29; ++tick) {
            const double position = double(tick) / 28.0;
            const double angle = (225.0 - position * 270.0) * kPi / 180.0;
            const bool active = position <= fraction + 1.0e-9;
            const double outer = radius - 1.5;
            const double inner = outer - (tick % 4 == 0 ? 7.5 : 4.0);
            QColor ink = active ? kPrimary : QColor(0x3B, 0x3E, 0x40);
            ink.setAlpha(active ? 235 : 75);
            painter.setPen(QPen(ink, active ? 1.55 : 0.9,
                                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(
                QPointF(centre.x() + std::cos(angle) * inner,
                        centre.y() - std::sin(angle) * inner),
                QPointF(centre.x() + std::cos(angle) * outer,
                        centre.y() - std::sin(angle) * outer));
        }

        const QRectF collar = ring.adjusted(21.0, 21.0, -21.0, -21.0);
        for (int layer = 0; layer < 6; ++layer) {
            const double spread = double(layer) * 1.7;
            QColor shadow(0, 0, 0, 70 - layer * 9);
            painter.setPen(Qt::NoPen);
            painter.setBrush(shadow);
            painter.drawEllipse(collar.adjusted(-spread, -spread,
                                                 spread, spread)
                                    .translated(0.0, 8.0 + layer * 0.8));
        }

        QRadialGradient bezel(collar.topLeft() +
                                  QPointF(collar.width() * 0.30,
                                          collar.height() * 0.22),
                              collar.width() * 0.82);
        bezel.setColorAt(0.0, QColor(0x58, 0x5A, 0x5C));
        bezel.setColorAt(0.48, QColor(0x2C, 0x2E, 0x30));
        bezel.setColorAt(0.78, QColor(0x15, 0x16, 0x18));
        bezel.setColorAt(1.0, QColor(0x05, 0x06, 0x07));
        painter.setPen(QPen(QColor(0x05, 0x06, 0x07), 2.2));
        painter.setBrush(bezel);
        painter.drawEllipse(collar);

        const QRectF body = collar.adjusted(7.0, 7.0, -7.0, -7.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 165));
        painter.drawEllipse(body.translated(0.0, 3.5));

        QLinearGradient graphite(body.topLeft(), body.bottomLeft());
        graphite.setColorAt(0.0, QColor(0x3D, 0x3F, 0x41));
        graphite.setColorAt(0.40, QColor(0x29, 0x2B, 0x2D));
        graphite.setColorAt(0.72, QColor(0x1C, 0x1E, 0x20));
        graphite.setColorAt(1.0, QColor(0x12, 0x13, 0x15));
        painter.setPen(QPen(QColor(0x09, 0x0A, 0x0B), 1.7));
        painter.setBrush(graphite);
        painter.drawEllipse(body);

        QRadialGradient highlight(body.topLeft() +
                                      QPointF(body.width() * 0.28,
                                              body.height() * 0.20),
                                  body.width() * 0.62);
        highlight.setColorAt(0.0, QColor(255, 255, 255, 34));
        highlight.setColorAt(0.48, QColor(255, 255, 255, 8));
        highlight.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(highlight);
        painter.drawEllipse(body.adjusted(2.0, 2.0, -2.0, -2.0));

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, underMouse() || isEditing()
                                                    ? 62 : 42),
                            1.35, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(collar.adjusted(3.0, 3.0, -3.0, -3.0),
                        28 * 16, 124 * 16);
        painter.setPen(QPen(QColor(0, 0, 0, 150), 2.0,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(collar.adjusted(3.0, 3.0, -3.0, -3.0),
                        205 * 16, 130 * 16);

        const double angle = (225.0 - fraction * 270.0) * kPi / 180.0;
        painter.setPen(QPen(kPrimary, 3.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(
            QPointF(centre.x() + std::cos(angle) * body.width() * 0.10,
                    centre.y() - std::sin(angle) * body.width() * 0.10),
            QPointF(centre.x() + std::cos(angle) * body.width() * 0.40,
                    centre.y() - std::sin(angle) * body.width() * 0.40));

        if (hasFocus()) {
            painter.setPen(QPen(kAccent, 2.0, Qt::SolidLine));
            painter.drawEllipse(ring.adjusted(-2.0, -2.0, 2.0, 2.0));
        }
    }
};

class PrioritySlider final : public QSlider {
public:
    explicit PrioritySlider(QWidget* parent)
        : QSlider(Qt::Horizontal, parent) {
        setRange(-100, 100);
        setSingleStep(5);
        setPageStep(25);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF groove(6.0, height() * 0.5 - 1.25,
                            width() - 12.0, 2.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x31, 0x34, 0x36));
        painter.drawRoundedRect(groove, 1.25, 1.25);

        const double fraction = (double(value()) - minimum()) /
                                double(maximum() - minimum());
        const double x = groove.left() + groove.width() * fraction;
        painter.setBrush(QColor(0x55, 0xE0, 0xC8, 150));
        painter.drawRoundedRect(QRectF(std::min(x, groove.center().x()),
                                      groove.top(),
                                      std::abs(x - groove.center().x()),
                                      groove.height()), 1.25, 1.25);
        painter.setPen(QPen(QColor(0x78, 0x7D, 0x80), 1.0));
        painter.drawLine(QPointF(groove.center().x(), groove.top() - 3.0),
                         QPointF(groove.center().x(), groove.bottom() + 3.0));

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 150));
        painter.drawEllipse(QPointF(x, groove.center().y() + 2.0), 6.0, 6.0);
        QRadialGradient handle(QPointF(x - 2.0, groove.center().y() - 2.0), 8.0);
        handle.setColorAt(0.0, QColor(0xF5, 0xF6, 0xF5));
        handle.setColorAt(0.45, QColor(0xA6, 0xAA, 0xAC));
        handle.setColorAt(1.0, QColor(0x31, 0x34, 0x36));
        painter.setPen(QPen(QColor(0x08, 0x09, 0x0A), 1.0));
        painter.setBrush(handle);
        painter.drawEllipse(QPointF(x, groove.center().y()), 5.5, 5.5);

        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(kAccent, 1.0, Qt::SolidLine));
            painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0,
                                                           -1.0, -1.0),
                                    4.0, 4.0);
        }
    }
};

QString modeDescription(int mode) {
    static const std::array<const char*, 5> names{
        QT_TR_NOOP("Air — soft high-frequency polish"),
        QT_TR_NOOP("Body — warm low-mid weight"),
        QT_TR_NOOP("Punch — transient-focused impact"),
        QT_TR_NOOP("Crunch — firm midrange saturation"),
        QT_TR_NOOP("Extreme — hard compression and clipping"),
    };
    return GraphitPanel::tr(names[std::size_t(std::clamp(mode, 0, 4))]);
}

} // namespace

GraphitPanel::GraphitPanel(daw::EngineController* controller, QString channelId,
                           QString insertId, QWidget* parent)
    : QWidget(parent),
      m_controller(controller),
      m_channelId(std::move(channelId)),
      m_insertId(std::move(insertId)),
      m_channelKey(m_channelId.toStdString()),
      m_insertKey(m_insertId.toStdString()) {
    setObjectName(QStringLiteral("GraphitPanel"));
    setMinimumSize(440, 460);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(tr("Graphit saturation effect"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 10, 28, 16);
    layout->setSpacing(4);
    layout->addSpacing(96);

    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(9);
    modeRow->addStretch(1);
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    static constexpr std::array<char, 5> letters{'A', 'B', 'P', 'C', 'X'};
    for (int mode = 0; mode < int(m_modeButtons.size()); ++mode) {
        auto* button = new QPushButton(QString(QChar(letters[std::size_t(mode)])), this);
        button->setObjectName(QStringLiteral("GraphitModeButton"));
        button->setCheckable(true);
        button->setFixedSize(48, 34);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setToolTip(modeDescription(mode));
        button->setAccessibleName(
            tr("Graphit mode %1: %2").arg(button->text(), modeDescription(mode)));
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        button->installEventFilter(this);
        group->addButton(button, mode);
        modeRow->addWidget(button);
        connect(button, &QPushButton::clicked, this,
                [this, mode] { selectMode(mode); });
        connect(button, &QWidget::customContextMenuRequested, this,
                [this, button](const QPoint& position) {
                    showModeAutomationMenu(button, position);
                });
        m_modeButtons[std::size_t(mode)] = button;
    }
    modeRow->addStretch(1);
    auto* dial = new GraphitDial(this);
    m_amount = dial;
    dial->setBare(220);
    if (const auto* info = parameterInfo("amount")) {
        dial->setRange(info->minValue, info->maxValue);
        dial->setDefaultValue(info->defaultValue);
    }
    dial->setFormatter([](double value) {
        return QString::fromStdString(
            graphit::parameterText(std::uint32_t(graphit::Param::Amount), value));
    });
    dial->setAccessibleName(tr("Graphit amount"));
    dial->setToolTip(tr("Amount — drag vertically, use Shift for fine adjustment"));
    dial->setAutomatable(true);
    dial->setValue(readParameter("amount"));
    layout->addWidget(dial, 0, Qt::AlignHCenter);

    m_amountReadout = new QLabel(this);
    m_amountReadout->setAlignment(Qt::AlignCenter);
    m_amountReadout->setStyleSheet(QStringLiteral(
        "color:#F1F3F2;font-size:11px;font-weight:600;letter-spacing:2px;"));
    layout->addWidget(m_amountReadout);
    layout->addLayout(modeRow);
    layout->addStretch(1);

    auto* footer = new QLabel(tr("SATURATION  ·  EQUALIZATION  ·  DYNAMICS"), this);
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet(QStringLiteral(
        "color:#777C81;font-size:8px;letter-spacing:2px;"));
    layout->addWidget(footer);

    auto* priority = new PrioritySlider(this);
    m_priority = priority;
    priority->setObjectName(QStringLiteral("GraphitPriority"));
    priority->setValue(int(std::lround(readParameter("priority") * 100.0)));
    priority->setAccessibleName(tr("Graphit frequency priority"));
    priority->setToolTip(
        tr("Frequency priority — move left for lows or right for highs"));
    priority->setContextMenuPolicy(Qt::CustomContextMenu);
    priority->installEventFilter(this);

    m_activeButton = new QPushButton(tr("ACTIVE"), this);
    m_activeButton->setObjectName(QStringLiteral("GraphitActiveButton"));
    m_activeButton->setCheckable(true);
    m_activeButton->setFocusPolicy(Qt::StrongFocus);
    m_activeButton->setAccessibleName(tr("Graphit active"));
    m_activeButton->setToolTip(tr("Enable or bypass Graphit"));
    priority->setGeometry(30, 135, 112, 25);
    m_activeButton->setGeometry(width() - 132, 127, 102, 32);

    setStyleSheet(QStringLiteral(
        "QPushButton#GraphitModeButton{color:#92979C;background:#0A0B0D;"
        "border:1px solid #34383B;border-radius:5px;font-size:12px;"
        "font-weight:600;}"
        "QPushButton#GraphitModeButton:hover{color:#F1F3F2;border-color:#697075;}"
        "QPushButton#GraphitModeButton:checked{color:#F1F3F2;background:#153B37;"
        "border:2px solid #55E0C8;}"
        "QPushButton#GraphitModeButton:focus{border:2px solid #F1F3F2;}"
        "QPushButton#GraphitActiveButton{color:#7B8084;background:transparent;"
        "border:1px solid transparent;border-radius:5px;font-size:9px;"
        "font-weight:600;letter-spacing:1px;padding:0 5px;}"
        "QPushButton#GraphitActiveButton:checked{color:#55E0C8;}"
        "QPushButton#GraphitActiveButton:hover{border-color:#34383B;}"
        "QPushButton#GraphitActiveButton:focus{border-color:#F1F3F2;}"));

    connect(dial, &ui::Knob::valueChanged, this, [this](double value) {
        beginAmountGesture();
        writeParameter("amount", value);
        m_amountValue = value;
        update();
    });
    connect(dial, &ui::Knob::editFinished, this,
            &GraphitPanel::endAmountGesture);
    connect(dial, &ui::Knob::automateRequested, this, [this] {
        emit automationRequested(QStringLiteral("amount"));
    });
    connect(priority, &QSlider::valueChanged, this, [this](int value) {
        if (m_refreshing) return;
        beginPriorityGesture();
        m_priorityValue = double(value) / 100.0;
        writeParameter("priority", m_priorityValue);
        update();
    });
    connect(priority, &QSlider::sliderReleased, this,
            &GraphitPanel::endPriorityGesture);
    connect(priority, &QWidget::customContextMenuRequested, this,
            [this, priority](const QPoint& position) {
                QMenu menu(this);
                QAction* create = menu.addAction(
                    tr("Create Priority Automation Clip"));
                if (menu.exec(priority->mapToGlobal(position)) == create)
                    emit automationRequested(QStringLiteral("priority"));
            });
    connect(m_activeButton, &QPushButton::clicked, this,
            &GraphitPanel::toggleActive);

    QWidget::setTabOrder(priority, m_activeButton);
    QWidget::setTabOrder(m_activeButton, dial);
    QWidget::setTabOrder(dial, m_modeButtons.front());
    for (std::size_t index = 1; index < m_modeButtons.size(); ++index)
        QWidget::setTabOrder(m_modeButtons[index - 1], m_modeButtons[index]);

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &GraphitPanel::refresh);
    refresh();
}

graphit::GraphitInstance* GraphitPanel::graphitInstance() const {
    if (!m_controller) return nullptr;
    return dynamic_cast<graphit::GraphitInstance*>(
        m_controller->insertInstance(m_channelKey, m_insertKey));
}

double GraphitPanel::readParameter(const char* parameterId) const {
    if (!m_controller) return 0.0;
    return m_controller->insertParameter(m_channelKey, m_insertKey, parameterId);
}

void GraphitPanel::writeParameter(const char* parameterId, double value) {
    if (m_controller)
        m_controller->setInsertParameter(m_channelKey, m_insertKey,
                                         parameterId, value);
}

void GraphitPanel::beginAmountGesture() {
    if (!m_amountGestureStart) m_amountGestureStart = readParameter("amount");
}

void GraphitPanel::endAmountGesture() {
    if (!m_controller || !m_amountGestureStart) return;
    m_controller->commitInsertParameterEdit(
        m_channelKey, m_insertKey, "amount", *m_amountGestureStart,
        "Change Graphit Amount");
    m_amountGestureStart.reset();
    emit projectEdited();
    refresh();
}

void GraphitPanel::beginPriorityGesture() {
    if (!m_priorityGestureStart)
        m_priorityGestureStart = readParameter("priority");
}

void GraphitPanel::endPriorityGesture() {
    if (!m_controller || !m_priorityGestureStart) return;
    m_controller->commitInsertParameterEdit(
        m_channelKey, m_insertKey, "priority", *m_priorityGestureStart,
        "Change Graphit Priority");
    m_priorityGestureStart.reset();
    emit projectEdited();
    refresh();
}

void GraphitPanel::toggleActive(bool active) {
    if (m_refreshing || !m_controller) return;
    m_controller->setInsertBypassed(m_channelKey, m_insertKey, !active);
    emit projectEdited();
    refresh();
}

void GraphitPanel::selectMode(int mode) {
    if (m_refreshing || !m_controller) return;
    const double before = readParameter("mode");
    if (int(std::lround(before)) == mode) return;
    writeParameter("mode", double(mode));
    m_controller->commitInsertParameterEdit(
        m_channelKey, m_insertKey, "mode", before, "Change Graphit Mode");
    emit projectEdited();
    refresh();
}

void GraphitPanel::showModeAutomationMenu(QPushButton* button,
                                          const QPoint& position) {
    QMenu menu(this);
    QAction* create = menu.addAction(tr("Create Mode Automation Clip"));
    if (menu.exec(button->mapToGlobal(position)) == create)
        emit automationRequested(QStringLiteral("mode"));
}

bool GraphitPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_priority) {
        if (event->type() == QEvent::KeyPress) {
            beginPriorityGesture();
        } else if (event->type() == QEvent::KeyRelease) {
            QTimer::singleShot(0, this, &GraphitPanel::endPriorityGesture);
        } else if (event->type() == QEvent::Wheel) {
            beginPriorityGesture();
            QTimer::singleShot(0, this, &GraphitPanel::endPriorityGesture);
        }
    }
    if (event->type() == QEvent::KeyPress) {
        const auto found = std::find(m_modeButtons.begin(), m_modeButtons.end(),
                                     watched);
        if (found != m_modeButtons.end()) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            const int direction = (key == Qt::Key_Right || key == Qt::Key_Down)
                                      ? 1
                                      : (key == Qt::Key_Left || key == Qt::Key_Up)
                                            ? -1
                                            : 0;
            if (direction != 0) {
                const int current = int(std::distance(m_modeButtons.begin(), found));
                const int next = (current + direction + 5) % 5;
                m_modeButtons[std::size_t(next)]->setFocus();
                m_modeButtons[std::size_t(next)]->click();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GraphitPanel::refresh() {
    if (!m_controller) return;
    m_refreshing = true;
    m_amountValue = readParameter("amount");
    m_priorityValue = std::clamp(readParameter("priority"), -1.0, 1.0);
    m_mode = std::clamp(int(std::lround(readParameter("mode"))), 0, 4);
    if (!m_amount->isEditing() && !m_amountGestureStart)
        m_amount->setValue(m_amountValue);
    for (int mode = 0; mode < int(m_modeButtons.size()); ++mode) {
        const QSignalBlocker blocker(m_modeButtons[std::size_t(mode)]);
        m_modeButtons[std::size_t(mode)]->setChecked(mode == m_mode);
        m_modeButtons[std::size_t(mode)]->setAccessibleDescription(
            mode == m_mode ? tr("Selected") : tr("Not selected"));
    }
    if (!m_priority->isSliderDown() && !m_priorityGestureStart) {
        const QSignalBlocker blocker(m_priority);
        m_priority->setValue(int(std::lround(m_priorityValue * 100.0)));
    }
    if (const daw::InsertModel* model =
            m_controller->insertModel(m_channelKey, m_insertKey))
        m_bypassed = model->bypassed;
    {
        const QSignalBlocker blocker(m_activeButton);
        m_activeButton->setChecked(!m_bypassed);
        m_activeButton->setText(
            QStringLiteral("● ") + (m_bypassed ? tr("BYPASSED") : tr("ACTIVE")));
        m_activeButton->setAccessibleDescription(
            m_bypassed ? tr("Graphit is bypassed") : tr("Graphit is active"));
    }

    graphit::Telemetry telemetry;
    if (graphit::GraphitInstance* instance = graphitInstance())
        telemetry = instance->consumeTelemetry();
    const float peak = std::max(telemetry.outputLeft, telemetry.outputRight);
    const float level = peak > 1.0e-6f
        ? std::clamp((20.0f * std::log10(peak) + 54.0f) / 54.0f, 0.0f, 1.0f)
        : 0.0f;
    m_meterLevel = std::max(level, m_meterLevel * 0.92f);
    m_gainReduction = std::max(telemetry.gainReductionDb,
                               m_gainReduction * 0.90f);
    const bool reducedMotion = QSettings().value(
        QStringLiteral("ui/reduceMotion"), false).toBool();
    if (reducedMotion) {
        m_history.fill(m_meterLevel);
    } else {
        std::move(m_history.begin() + 1, m_history.end(), m_history.begin());
        m_history.back() = m_meterLevel;
    }

    m_amountReadout->setText(
        tr("AMOUNT  ·  %1").arg(QString::fromStdString(graphit::parameterText(
            std::uint32_t(graphit::Param::Amount), m_amountValue))));
    m_amount->setAccessibleDescription(
        tr("Current value %1; mode %2; priority %3")
            .arg(QString::fromStdString(graphit::parameterText(
                     std::uint32_t(graphit::Param::Amount), m_amountValue)),
                 m_modeButtons[std::size_t(m_mode)]->text(),
                 QString::fromStdString(graphit::parameterText(
                     std::uint32_t(graphit::Param::Priority),
                     m_priorityValue))));
    m_priority->setAccessibleDescription(
        QString::fromStdString(graphit::parameterText(
            std::uint32_t(graphit::Param::Priority), m_priorityValue)));
    m_refreshing = false;
    update();
}

void GraphitPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient surface(0.0, 0.0, 0.0, height());
    surface.setColorAt(0.0, QColor(0x1B, 0x1C, 0x1E));
    surface.setColorAt(0.42, kSurface);
    surface.setColorAt(1.0, QColor(0x0D, 0x0E, 0x10));
    painter.fillRect(rect(), surface);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0x2B, 0x2E, 0x30), 1.0));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            11.0, 11.0);

    const double centreX = width() * 0.5;
    QPainterPath headerShape;
    headerShape.moveTo(20.0, 8.0);
    headerShape.quadTo(8.0, 8.0, 8.0, 20.0);
    headerShape.lineTo(8.0, 162.0);
    headerShape.quadTo(8.0, 174.0, 20.0, 174.0);
    headerShape.lineTo(centreX - 116.0, 174.0);
    headerShape.cubicTo(centreX - 94.0, 132.0,
                        centreX - 57.0, 104.0, centreX, 101.0);
    headerShape.cubicTo(centreX + 57.0, 104.0,
                        centreX + 94.0, 132.0, centreX + 116.0, 174.0);
    headerShape.lineTo(width() - 20.0, 174.0);
    headerShape.quadTo(width() - 8.0, 174.0,
                       width() - 8.0, 162.0);
    headerShape.lineTo(width() - 8.0, 20.0);
    headerShape.quadTo(width() - 8.0, 8.0, width() - 20.0, 8.0);
    headerShape.closeSubpath();

    QLinearGradient headerFill(0.0, 8.0, 0.0, 174.0);
    headerFill.setColorAt(0.0, QColor(0x0D, 0x0E, 0x10));
    headerFill.setColorAt(0.72, QColor(0x09, 0x0A, 0x0C));
    headerFill.setColorAt(1.0, QColor(0x07, 0x08, 0x09));
    painter.setBrush(headerFill);
    painter.setPen(QPen(QColor(0x32, 0x35, 0x37), 1.2));
    painter.drawPath(headerShape);

    QFont header = painter.font();
    header.setPixelSize(11);
    header.setWeight(QFont::DemiBold);
    header.setLetterSpacing(QFont::PercentageSpacing, 180.0);
    painter.setFont(header);
    painter.setPen(kPrimary);
    painter.drawText(QRectF(28.0, 15.0, 180.0, 20.0),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("GRAPHIT"));
    header.setLetterSpacing(QFont::PercentageSpacing, 115.0);
    painter.setFont(header);
    painter.setPen(kMuted);
    painter.drawText(QRectF(width() - 100.0, 15.0, 72.0, 20.0),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("VLT"));

    QFont small = painter.font();
    small.setPixelSize(8);
    small.setWeight(QFont::Medium);
    small.setLetterSpacing(QFont::PercentageSpacing, 135.0);
    painter.setFont(small);
    painter.setPen(kMuted);
    painter.drawText(QRectF(30.0, 39.0, 120.0, 14.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     tr("AMOUNT %1%").arg(int(std::lround(m_amountValue * 100.0))));
    painter.drawText(QRectF(width() - 150.0, 39.0, 120.0, 14.0),
                     Qt::AlignRight | Qt::AlignVCenter,
                     tr("GR %1 dB").arg(m_gainReduction, 0, 'f', 1));

    const QRectF graph(44.0, 57.0, width() - 88.0, 42.0);
    painter.setPen(QPen(QColor(0x2E, 0x32, 0x34), 1.0, Qt::DotLine));
    painter.drawLine(graph.bottomLeft(), graph.bottomRight());
    const double spacing = graph.width() / double(m_history.size() - 1);
    for (std::size_t index = 0; index < m_history.size(); ++index) {
        const double x = graph.left() + double(index) * spacing;
        const double height = 2.0 + m_history[index] * (graph.height() - 3.0);
        QColor bar = kAccent;
        bar.setAlpha(int(55 + m_history[index] * 150.0));
        painter.setPen(QPen(bar, 1.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(x, graph.bottom()),
                         QPointF(x, graph.bottom() - height));
    }

    painter.setPen(kMuted);
    painter.drawText(QRectF(30.0, 113.0, 112.0, 16.0),
                     Qt::AlignCenter,
                     tr("PRIORITY %1").arg(QString::fromStdString(
                         graphit::parameterText(
                             std::uint32_t(graphit::Param::Priority),
                             m_priorityValue))));
}

void GraphitPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_priority) m_priority->setGeometry(30, 135, 112, 25);
    if (m_activeButton)
        m_activeButton->setGeometry(width() - 132, 127, 102, 32);
}

void GraphitPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_timer) m_timer->start();
    refresh();
}

void GraphitPanel::hideEvent(QHideEvent* event) {
    if (m_timer) m_timer->stop();
    QWidget::hideEvent(event);
}

bool GraphitPanel::checkForTest() {
    if (!m_controller || !graphitInstance()) return false;
    const auto oneUndoAdvanced = [this](std::size_t before) {
        const std::size_t after = m_controller->undoDepth();
        return before < m_controller->undoLimit() ? after == before + 1
                                                   : after == before;
    };

    const int nextMode = (m_mode + 1) % 5;
    const std::size_t modeDepth = m_controller->undoDepth();
    m_modeButtons[std::size_t(m_mode)]->setFocus();
    QKeyEvent modeKey(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(m_modeButtons[std::size_t(m_mode)], &modeKey);
    const bool modeChanged = oneUndoAdvanced(modeDepth) &&
        int(std::lround(readParameter("mode"))) == nextMode &&
        m_modeButtons[std::size_t(nextMode)]->isChecked();

    const double beforeAmount = readParameter("amount");
    const std::size_t amountDepth = m_controller->undoDepth();
    m_amount->setFocus();
    QKeyEvent amountKey(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(m_amount, &amountKey);
    const bool amountGrouped = oneUndoAdvanced(amountDepth) &&
        readParameter("amount") > beforeAmount;

    const double beforePriority = readParameter("priority");
    const std::size_t priorityDepth = m_controller->undoDepth();
    m_priority->setFocus();
    QKeyEvent priorityPress(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QKeyEvent priorityRelease(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(m_priority, &priorityPress);
    QCoreApplication::sendEvent(m_priority, &priorityRelease);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const bool priorityGrouped = oneUndoAdvanced(priorityDepth) &&
        readParameter("priority") > beforePriority;

    m_activeButton->click();
    const bool bypassed = m_bypassed && !m_activeButton->isChecked();
    m_activeButton->click();
    const bool active = !m_bypassed && m_activeButton->isChecked();

    bool accessible = m_amount->focusPolicy() == Qt::TabFocus &&
                      !m_amount->accessibleName().isEmpty() &&
                      m_priority->focusPolicy() == Qt::TabFocus &&
                      !m_priority->accessibleName().isEmpty() &&
                      m_activeButton->focusPolicy() == Qt::StrongFocus &&
                      !m_activeButton->accessibleName().isEmpty();
    for (QPushButton* button : m_modeButtons) {
        accessible = accessible && button->focusPolicy() != Qt::NoFocus &&
                     !button->accessibleName().isEmpty();
    }
    const bool passed = modeChanged && amountGrouped && priorityGrouped &&
                        bypassed && active && accessible;
    if (!passed) {
        std::fprintf(stderr,
                     "Graphit UI selftest: mode=%d amount=%d priority=%d bypass=%d active=%d a11y=%d\n",
                     modeChanged, amountGrouped, priorityGrouped, bypassed,
                     active, accessible);
    }
    return passed;
}
