#include <QDir>
#include "PluginStyle.hpp"
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

constexpr double kPi = std::numbers::pi_v<double>;
QColor graphitCopper() { return QColor(th().dark ? "#d6a47e" : "#975e39"); }

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
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(isEnabled() ? 1. : .4);
        p.translate(rect().center());
        const double scale = std::min(width(), height()) / 220.;
        p.scale(scale, scale);
        const auto point = [](double f, double radius) {
            const double a = (225. - f * 270.) * kPi / 180.;
            return QPointF(std::cos(a) * radius, -std::sin(a) * radius);
        };
        for (int i = 0; i <= 40; ++i) {
            p.setPen(QPen(i / 40. <= value() ? graphitCopper() : th().textSecondary,
                          i % 5 == 0 ? 1.7 : .7));
            p.drawLine(point(i / 40., i % 5 == 0 ? 96 : 100), point(i / 40., 104));
        }
        p.setPen(Qt::NoPen);
        if (!ui::paintMetalKnob(p, QRectF(-87, -87, 174, 174), 225 - value() * 270)) {
        for (int i = 5; i > 0; --i) {
            p.setBrush(QColor(0, 0, 0, 16));
            p.drawEllipse(QPointF(0, 3), 86 + i, 86 + i);
        }
        QLinearGradient rim(-55, -85, 65, 85);
        rim.setColorAt(0, QColor("#777b7c"));
        rim.setColorAt(.4, QColor("#343839"));
        rim.setColorAt(1, QColor("#101314"));
        p.setBrush(rim);
        p.drawEllipse(QPointF(), 87, 87);
        for (int i = 0; i < 60; ++i) {
            const double a = i * 2 * kPi / 60.;
            const QPointF d(std::cos(a), std::sin(a));
            p.setPen(QPen(QColor(0, 0, 0, 100), 1));
            p.drawLine(d * 81, d * 86);
        }
        QRadialGradient carbon(QPointF(-35, -46), 145);
        carbon.setColorAt(0, QColor("#45494a"));
        carbon.setColorAt(.6, QColor("#262a2b"));
        carbon.setColorAt(1, QColor("#171a1b"));
        p.setBrush(carbon);
        p.setPen(QPen(QColor("#555a5b"), 1));
        p.drawEllipse(QPointF(), 79, 79);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 50), .8));
        for (int r = 20; r < 75; r += 3) p.drawEllipse(QPointF(), r, r);
        p.setPen(QPen(graphitCopper(), 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(point(value(), 56), point(value(), 72));
        }
    }
};

class PrioritySlider final : public ui::GlassSlider {
public:
    explicit PrioritySlider(QWidget* parent)
        : ui::GlassSlider(Qt::Horizontal, parent) {
        setRange(-100, 100);
        setSingleStep(5);
        setPageStep(25);
        setFillFrom(0.5);
        setDetent(0.5);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
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
    : ui::FrameWidget(parent),
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
        "font-size:18px;font-weight:600;letter-spacing:1px;"));
    layout->addWidget(m_amountReadout);
    layout->addLayout(modeRow);
    layout->addStretch(1);

    auto* footer = new QLabel(tr("SATURATION  ·  EQUALIZATION  ·  DYNAMICS"), this);
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet(QStringLiteral(
        "font-size:10px;letter-spacing:1px;"));
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

    pluginStyle::bind(this);
    const auto tintControls = [this] {
        auto colours = palette();
        colours.setColor(QPalette::Highlight, graphitCopper());
        setPalette(colours);
        setStyleSheet(QStringLiteral(
            "#GraphitModeButton, #GraphitActiveButton { color: %1; background: %2;"
            " border: 1px solid %3; border-radius: 6px; font-size: 12px; }"
            "#GraphitModeButton:checked, #GraphitActiveButton:checked { color: %4;"
            " background: %5; }")
            .arg(th().textPrimary.name(), pluginStyle::shell().name(), th().separator().name(),
                 graphitCopper().name(), mixColors(th().well(), graphitCopper(), .10).name()));
    };
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, tintControls);
    tintControls();


    dial->setProperty("parameterId", QStringLiteral("amount"));
    priority->setProperty("parameterId", QStringLiteral("priority"));
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

    m_visualTimer = new ui::FrameTimer(this);
    connect(m_visualTimer, &ui::FrameTimer::timeout, this, &GraphitPanel::refreshTelemetry);
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, [this] {
        refresh();
        if (isVisible() && !m_visualTimer->isActive()) refreshTelemetry();
    });
    refresh();
}

bool GraphitPanel::available() const {
    return m_controller && m_controller->hasInsert(m_channelKey, m_insertKey, graphit::GraphitInstance::uid());
}

double GraphitPanel::readParameter(const char* parameterId) const {
    if (!m_controller) return 0.0;
    return m_controller->insertParameter(m_channelKey, m_insertKey, parameterId);
}

void GraphitPanel::writeParameter(const char* parameterId, double value) {
    m_controlsValid = false;
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
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
            (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
             key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
            event->accept();
            return true;
        }
    }
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
    const double amount = readParameter("amount");
    const double priority = std::clamp(readParameter("priority"), -1.0, 1.0);
    const int mode = std::clamp(int(std::lround(readParameter("mode"))), 0, 4);
    const auto* model = m_controller->insertModel(m_channelKey, m_insertKey);
    const bool bypassed = model ? model->bypassed : m_bypassed;
    m_reducedMotion = QSettings().value(QStringLiteral("ui/reduceMotion"), false).toBool();
    if (m_controlsValid && amount == m_amountValue && priority == m_priorityValue &&
        mode == m_mode && bypassed == m_bypassed) return;
    m_controlsValid = true;
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

void GraphitPanel::refreshTelemetry() {
    const auto telemetry = available()
        ? m_controller->effectMeterSnapshot(m_channelKey, m_insertKey)
        : daw::EffectMeterSnapshot{};
    const float peak = telemetry.output;
    const float level = peak > 1.0e-6f
        ? std::clamp((20.0f * std::log10(peak) + 54.0f) / 54.0f, 0.0f, 1.0f)
        : 0.0f;
    const double dt = std::clamp(m_visualTimer->deltaSeconds(), 0.0, 0.25);
    const float previousLevel = m_meterLevel, previousReduction = m_gainReduction;
    const bool hadHistory = std::any_of(m_history.begin(), m_history.end(),
                                       [](float level) { return level > 0.0f; });
    m_meterLevel = std::max(level, m_meterLevel * float(std::pow(0.92, dt / 0.033)));
    m_gainReduction = std::max(telemetry.reduction,
                               m_gainReduction * float(std::pow(0.90, dt / 0.033)));
    if (m_meterLevel < 1e-5f) m_meterLevel = 0.0f;
    if (m_gainReduction < 1e-5f) m_gainReduction = 0.0f;
    if (m_reducedMotion) m_history.fill(m_meterLevel);
    else {
        m_historyTime += dt;
        const auto steps = std::min<std::size_t>(m_history.size(), std::size_t(m_historyTime / 0.033));
        if (steps) {
            std::move(m_history.begin() + steps, m_history.end(), m_history.begin());
            std::fill(m_history.end() - steps, m_history.end(), m_meterLevel);
            m_historyTime -= double(steps) * 0.033;
        }
    }
    const bool active = m_meterLevel > 0 || m_gainReduction > 0 ||
        std::any_of(m_history.begin(), m_history.end(), [](float level) { return level > 0.0f; });
    if (hadHistory || active || previousLevel != m_meterLevel || previousReduction != m_gainReduction)
        update(QRect(20, 35, width() - 40, 68));
    if (active && isVisible()) m_visualTimer->start();
    else m_visualTimer->stop();
}

void GraphitPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    paintScene(painter, QRegion(rect()));
}

void GraphitPanel::paintScene(QPainter& painter, const QRegion&) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(),th().background);
    const auto base = mixColors(pluginStyle::shell(), graphitCopper(), .035);
    QLinearGradient graphite(0, 0, width(), height());
    graphite.setColorAt(0, th().edgeLight(base));
    graphite.setColorAt(.6, base);
    graphite.setColorAt(1, th().edgeDark(base));
    painter.setPen(QPen(th().separator(), 1));
    painter.setBrush(graphite);
    painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -3), 14, 14);

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

    painter.setBrush(th().well());
    painter.setPen(QPen(th().separator(),1));
    painter.drawPath(headerShape);

    QFont header = painter.font();
    header.setPixelSize(14);
    header.setWeight(QFont::DemiBold);
    header.setLetterSpacing(QFont::PercentageSpacing, 180.0);
    painter.setFont(header);
    painter.setPen(th().textPrimary);
    painter.drawText(QRectF(28.0, 15.0, 180.0, 20.0),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("GRAPHIT"));
    header.setLetterSpacing(QFont::PercentageSpacing, 115.0);
    painter.setFont(header);
    painter.setPen(th().textSecondary);
    painter.drawText(QRectF(width() - 100.0, 15.0, 72.0, 20.0),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("VLTONE"));

    QFont small = painter.font();
    small.setPixelSize(9);
    small.setWeight(QFont::Medium);
    small.setLetterSpacing(QFont::PercentageSpacing, 135.0);
    painter.setFont(small);
    painter.setPen(th().textSecondary);
    painter.drawText(QRectF(30.0, 39.0, 120.0, 14.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     tr("AMOUNT %1%").arg(int(std::lround(m_amountValue * 100.0))));
    painter.drawText(QRectF(width() - 150.0, 39.0, 120.0, 14.0),
                     Qt::AlignRight | Qt::AlignVCenter,
                     tr("GR %1 dB").arg(m_gainReduction, 0, 'f', 1));

    const QRectF graph(44.0, 57.0, width() - 88.0, 42.0);
    painter.setPen(QPen(th().separator(), 1.0, Qt::DotLine));
    painter.drawLine(graph.bottomLeft(), graph.bottomRight());
    const double spacing = graph.width() / double(m_history.size() - 1);
    for (std::size_t index = 0; index < m_history.size(); ++index) {
        const double x = graph.left() + double(index) * spacing;
        const double height = 2.0 + m_history[index] * (graph.height() - 3.0);
        QColor bar = graphitCopper();
        bar.setAlpha(int(55 + m_history[index] * 150.0));
        painter.setPen(QPen(bar, 1.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(x, graph.bottom()),
                         QPointF(x, graph.bottom() - height));
    }

    painter.setPen(th().textSecondary);
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
    if (m_visualTimer) m_visualTimer->start();
    refresh();
}

void GraphitPanel::hideEvent(QHideEvent* event) {
    if (m_timer) m_timer->stop();
    if (m_visualTimer) m_visualTimer->stop();
    QWidget::hideEvent(event);
}

bool GraphitPanel::checkForTest() {
    const auto shots = qEnvironmentVariable("VLT_NATIVE_SCREENSHOTS");
    if (!shots.isEmpty()) { QDir().mkpath(shots); grab().save(shots + "/graphit.png"); }

    if (!m_controller || !available()) return false;
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
