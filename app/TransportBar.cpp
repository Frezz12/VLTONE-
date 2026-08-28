#include "TransportBar.hpp"
#include "Controls.hpp"
#include "GlassPanel.hpp"
#include "Icons.hpp"
#include "SpectrumMeter.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include <QSettings>

#include <algorithm>

#include "EngineController.hpp"

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace {

QFont monoFont(int pixelSize, bool bold = false) {
#ifdef Q_OS_MACOS
    // Qt 6 reports its macOS FixedFont family as the generic "Monospace" and
    // spends ~65 ms populating aliases before discovering it is not a real
    // installed family. Menlo is the platform fixed UI font.
    QFont f(QStringLiteral("Menlo"));
#else
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#endif
    f.setPixelSize(pixelSize);
    f.setBold(bold);
    return f;
}

/// A tempo number with the interaction used by DAWs and graphics tools: drag
/// vertically for quick relative changes, or double-click to turn it into a
/// normal text editor. It remains a real QLineEdit for accessibility and for
/// keyboard entry; the gesture is an additional path, not the only one.
class TempoScrubEdit final : public QLineEdit {
public:
    using ScrubCallback = std::function<void(double, bool)>;

    explicit TempoScrubEdit(const QString& value, QWidget* parent = nullptr)
        : QLineEdit(value, parent) {
        setReadOnly(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
        setMouseTracking(true);
    }

    void setScrubCallback(ScrubCallback callback) {
        m_callback = std::move(callback);
    }

    void endTextEditing() {
        if (isReadOnly()) return;
        setReadOnly(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
        style()->unpolish(this);
        style()->polish(this);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (!isReadOnly()) {
            QLineEdit::mousePressEvent(event);
            return;
        }
        if (event->button() != Qt::LeftButton) {
            QLineEdit::mousePressEvent(event);
            return;
        }
        bool ok = false;
        m_startValue = text().replace(',', '.').toDouble(&ok);
        if (!ok) m_startValue = 120.0;
        m_currentValue = m_startValue;
        m_pressGlobalY = event->globalPosition().y();
        m_pressed = true;
        m_dragging = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_pressed || !(event->buttons() & Qt::LeftButton)) {
            QLineEdit::mouseMoveEvent(event);
            return;
        }
        const qreal delta = m_pressGlobalY - event->globalPosition().y();
        if (!m_dragging && std::abs(delta) < 3.0) {
            event->accept();
            return;
        }
        m_dragging = true;
        // One pixel is a tenth of a BPM; Shift gives a gentler five-pixel
        // throw per tenth for detailed tempo matching.
        const double perPixel = event->modifiers() & Qt::ShiftModifier ? 0.02 : 0.1;
        const double raw = std::clamp(m_startValue + delta * perPixel, 20.0, 300.0);
        const double value = std::round(raw * 10.0) / 10.0;
        if (value != m_currentValue) {
            m_currentValue = value;
            if (m_callback) m_callback(value, false);
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!m_pressed || event->button() != Qt::LeftButton) {
            QLineEdit::mouseReleaseEvent(event);
            return;
        }
        const bool changed = m_dragging;
        m_pressed = false;
        m_dragging = false;
        setCursor(Qt::SizeVerCursor);
        if (changed && m_callback) m_callback(m_currentValue, true);
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QLineEdit::mouseDoubleClickEvent(event);
            return;
        }
        m_pressed = false;
        m_dragging = false;
        m_textBeforeEdit = text();
        setReadOnly(false);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::IBeamCursor);
        setFocus(Qt::MouseFocusReason);
        selectAll();
        style()->unpolish(this);
        style()->polish(this);
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!isReadOnly() && event->key() == Qt::Key_Escape) {
            setText(m_textBeforeEdit);
            clearFocus();
            // An inactive top-level window may reject focus even though it
            // still receives a synthetic/assistive key event, so do not rely
            // on focusOut to restore scrub mode.
            endTextEditing();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    ScrubCallback m_callback;
    QString m_textBeforeEdit;
    qreal m_pressGlobalY = 0.0;
    double m_startValue = 120.0;
    double m_currentValue = 120.0;
    bool m_pressed = false;
    bool m_dragging = false;
};

// The readout plate: its inner height, plus the margin its shadow needs inside
// its own rect (a child can't paint outside itself).
constexpr int kLcdHeight = 38;
constexpr int kLcdShadow = 6;

struct ToolDef { icons::Glyph glyph; const char* name; };
const ToolDef kTools[] = {
    {icons::Glyph::Pointer, QT_TRANSLATE_NOOP("TransportBar", "Select")},
    {icons::Glyph::Knife, QT_TRANSLATE_NOOP("TransportBar", "Knife")},
    {icons::Glyph::Eraser, QT_TRANSLATE_NOOP("TransportBar", "Eraser")},
    {icons::Glyph::Crosshair, QT_TRANSLATE_NOOP("TransportBar", "Region")},
    {icons::Glyph::Power, QT_TRANSLATE_NOOP("TransportBar", "Mute")},
    {icons::Glyph::Brush, QT_TRANSLATE_NOOP("TransportBar", "Draw")},
};
constexpr int kToolCount = int(sizeof(kTools) / sizeof(kTools[0]));

QString translatedToolName(int index) {
    return QCoreApplication::translate("TransportBar", kTools[index].name);
}

QLabel* capLabel(const QString& text) {
    auto* l = new QLabel(text);
    QFont f = l->font();
    f.setPixelSize(9);
    f.setBold(true);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    l->setFont(f);
    return l;
}

/// A shallow well punched into the header. The left instance can crop its
/// action row down to one disclosure button; animating the well itself (rather
/// than translating loose buttons over the header) keeps the control feeling
/// like part of the chrome throughout the transition.
class HeaderInsetPanel final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(HeaderInsetPanel)
public:
    explicit HeaderInsetPanel(bool collapsible, QWidget* parent = nullptr)
        : QWidget(parent), m_collapsible(collapsible) {
        setFixedHeight(34);
        setAttribute(Qt::WA_StyledBackground, false);

        m_row = new QHBoxLayout(this);
        m_row->setContentsMargins(4, 3, 4, 3);
        m_row->setSpacing(2);

        if (m_collapsible) {
            m_reveal = new ui::IconButton(
                icons::Glyph::Layers, tr("Show workspace controls"), this);
            m_reveal->setObjectName(QStringLiteral("HeaderDockReveal"));
            m_reveal->setAccessibleName(tr("Workspace controls"));
            m_reveal->setFocusPolicy(Qt::StrongFocus);
            m_reveal->setCheckable(true);
            m_reveal->setButtonSize(28, 28);
            m_row->addWidget(m_reveal);
            connect(m_reveal, &QAbstractButton::toggled, this,
                    [this](bool expanded) { setExpanded(expanded, true); });
        }

        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                QOverload<>::of(&QWidget::update));
    }

    void addAction(QWidget* action) {
        if (!action) return;
        m_actions.push_back(action);
        m_row->addWidget(action);
    }

    void finish() {
        m_row->invalidate();
        m_row->activate();
        const QMargins margins = m_row->contentsMargins();
        m_collapsedWidth = m_collapsible && m_reveal
                               ? margins.left() + m_reveal->width() +
                                     margins.right()
                               : m_row->sizeHint().width();
        m_expandedWidth = std::max(m_collapsedWidth, m_row->sizeHint().width());
        setFixedWidth(m_expandedWidth);
        if (m_collapsible) {
            m_expanded = true;
            setExpanded(false, false);
        }
    }

    void setGeometryChangedCallback(std::function<void()> callback) {
        m_geometryChanged = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const Theme& t = th();
        const QRectF panel = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath shape;
        shape.addRoundedRect(panel, 9, 9);

        QLinearGradient well(0, panel.top(), 0, panel.bottom());
        well.setColorAt(0.0, mixColors(t.well(), t.headerBackground, 0.18));
        well.setColorAt(1.0, mixColors(t.well(), t.surfaceElevated, 0.16));
        painter.fillPath(shape, well);

        // Dark at the upper edge and a hairline reflected edge below are the
        // two cues that make this read as inset, not as another floating pill.
        const QColor rim = mixColors(t.separator(), t.well(), 0.34);
        painter.setPen(QPen(rim, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(shape);

        painter.save();
        painter.setClipPath(shape);
        QColor upper = t.background;
        upper.setAlpha(150);
        painter.setPen(QPen(upper, 1));
        painter.drawLine(QPointF(panel.left() + 8, panel.top() + 1),
                         QPointF(panel.right() - 8, panel.top() + 1));
        QColor lower = t.textPrimary;
        lower.setAlpha(18);
        painter.setPen(QPen(lower, 1));
        painter.drawLine(QPointF(panel.left() + 8, panel.bottom() - 1),
                         QPointF(panel.right() - 8, panel.bottom() - 1));
        painter.restore();
    }

private:
    void setExpanded(bool expanded, bool animate) {
        if (!m_collapsible || m_expanded == expanded) return;
        m_expanded = expanded;

        if (m_reveal && m_reveal->isChecked() != expanded) {
            QSignalBlocker block(m_reveal);
            m_reveal->setChecked(expanded);
        }
        if (m_reveal) {
            const QString tip = expanded ? tr("Hide workspace controls")
                                         : tr("Show workspace controls");
            m_reveal->setToolTip(tip);
            m_reveal->setAccessibleName(tip);
        }

        if (expanded) {
            for (QWidget* action : m_actions) action->show();
            m_row->invalidate();
            m_row->activate();
            m_expandedWidth =
                std::max(m_collapsedWidth, m_row->sizeHint().width());
        }

        const int target = expanded ? m_expandedWidth : m_collapsedWidth;
        if (!animate) {
            setFixedWidth(target);
            if (!expanded)
                for (QWidget* action : m_actions) action->hide();
            if (m_geometryChanged) m_geometryChanged();
            return;
        }

        if (!m_animation) {
            m_animation = new QVariantAnimation(this);
            m_animation->setDuration(180);
            connect(m_animation, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& value) {
                        setFixedWidth(value.toInt());
                        if (m_geometryChanged) m_geometryChanged();
                    });
            connect(m_animation, &QVariantAnimation::finished, this, [this] {
                if (!m_expanded)
                    for (QWidget* action : m_actions) action->hide();
                if (m_geometryChanged) m_geometryChanged();
            });
        }
        m_animation->stop();
        m_animation->setStartValue(width());
        m_animation->setEndValue(target);
        m_animation->setEasingCurve(expanded ? QEasingCurve::OutCubic
                                             : QEasingCurve::InOutCubic);
        m_animation->start();
    }

    bool m_collapsible = false;
    bool m_expanded = false;
    int m_collapsedWidth = 0;
    int m_expandedWidth = 0;
    QHBoxLayout* m_row = nullptr;
    ui::IconButton* m_reveal = nullptr;
    QList<QWidget*> m_actions;
    QVariantAnimation* m_animation = nullptr;
    std::function<void()> m_geometryChanged;
};

} // namespace

TransportBar::TransportBar(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    // Read the remembered choices before anything is built, so the menus come
    // up already ticked on what the user last picked rather than on 1/16 and
    // the Select tool.
    m_gridIndex = std::clamp(QSettings().value(ui::kGridIndexSetting, 5).toInt(),
                             0, int(ui::gridDivisions().size()) - 1);
    m_toolIndex = std::clamp(QSettings().value(ui::kEditToolSetting, 0).toInt(),
                             0, kToolCount - 1);
    m_altToolIndex =
        std::clamp(QSettings().value(ui::kAltEditToolSetting, 1).toInt(),
                   0, kToolCount - 1);
    setFixedHeight(ui::kTransportHeight);
    setAttribute(Qt::WA_StyledBackground, false);

    // Build the trailing controls first, then place all three parts into one
    // floating cluster. Keeping them under one layout is what guarantees that
    // the gaps around the LCD stay equal and the whole header reads centred.
    m_rightGroup = buildRightGroup();
    m_pill = buildPill();
    m_pill->setParent(this);
    m_pill->raise();

    // Window controls used to consume a whole strip at the bottom of the
    // workspace. They now sit in shallow header wells at the outer edges: the
    // left one discloses the less frequent commands, while Web and AI remain
    // one-click actions on the right.
    m_leftDock = buildLeftDock();
    m_rightDock = buildRightDock();
    m_leftDock->raise();
    m_rightDock->raise();

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &TransportBar::applyTheme);
    applyTheme();
    syncTempo();
    updateResponsiveLayout();
    if (m_controller) m_controller->addMasterSpectrumConsumer();
}

TransportBar::~TransportBar() {
    if (m_controller) m_controller->removeMasterSpectrumConsumer();
}

QWidget* TransportBar::buildLeftDock() {
    auto* panel = new HeaderInsetPanel(/*collapsible=*/true, this);
    panel->setObjectName(QStringLiteral("HeaderLeftDock"));
    panel->setAccessibleName(tr("Workspace controls"));

    const auto panelButton = [panel](icons::Glyph glyph, const QString& tip,
                                     const char* objectName) {
        auto* button = new ui::IconButton(glyph, tip, panel);
        button->setObjectName(QString::fromLatin1(objectName));
        button->setAccessibleName(tip);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setButtonSize(28, 28);
        return button;
    };

    m_browserPanelButton = panelButton(
        icons::Glyph::Folder, tr("Show or hide the browser"),
        "HeaderBrowserButton");
    m_browserPanelButton->setCheckable(true);
    m_browserPanelButton->setChecked(true);
    connect(m_browserPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::browserToggled);
    panel->addAction(m_browserPanelButton);

    m_inspectorPanelButton = panelButton(
        icons::Glyph::Sidebar, tr("Show or hide the inspector"),
        "HeaderInspectorButton");
    m_inspectorPanelButton->setCheckable(true);
    m_inspectorPanelButton->setChecked(true);
    connect(m_inspectorPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::inspectorToggled);
    panel->addAction(m_inspectorPanelButton);

    panel->addAction(ui::separatorLine(Qt::Vertical, 14, panel));

    m_mixerPanelButton = panelButton(
        icons::Glyph::Mixer, tr("Show or hide the mixer (X)"),
        "HeaderMixerButton");
    m_mixerPanelButton->setCheckable(true);
    m_mixerPanelButton->setChecked(true);
    connect(m_mixerPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::mixerToggled);
    panel->addAction(m_mixerPanelButton);

    m_detachMixerButton = panelButton(
        icons::Glyph::Detach, tr("Open the mixer in its own window"),
        "HeaderDetachMixerButton");
    connect(m_detachMixerButton, &QAbstractButton::clicked, this,
            &TransportBar::detachMixerRequested);
    panel->addAction(m_detachMixerButton);

    panel->addAction(ui::separatorLine(Qt::Vertical, 14, panel));

    auto* addTrack = panelButton(icons::Glyph::Plus, tr("Add audio track"),
                                 "HeaderAddTrackButton");
    connect(addTrack, &QAbstractButton::clicked, this,
            &TransportBar::addTrackRequested);
    panel->addAction(addTrack);

    auto* settings = panelButton(icons::Glyph::Gear, tr("Audio settings"),
                                 "HeaderSettingsButton");
    connect(settings, &QAbstractButton::clicked, this,
            &TransportBar::settingsRequested);
    panel->addAction(settings);

    panel->setGeometryChangedCallback(
        [this] { updateResponsiveLayout(); });
    panel->finish();
    return panel;
}

QWidget* TransportBar::buildRightDock() {
    auto* panel = new HeaderInsetPanel(/*collapsible=*/false, this);
    panel->setObjectName(QStringLiteral("HeaderRightDock"));
    panel->setAccessibleName(tr("Connected panels"));

    m_webPanelButton = new ui::IconButton(
        icons::Glyph::Globe, tr("Open the integrated web browser (Alt+W)"),
        panel);
    m_webPanelButton->setObjectName(QStringLiteral("HeaderWebButton"));
    m_webPanelButton->setAccessibleName(tr("Web browser"));
    m_webPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_webPanelButton->setCheckable(true);
    m_webPanelButton->setButtonSize(28, 28);
    connect(m_webPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::webToggled);
    panel->addAction(m_webPanelButton);

    m_aiPanelButton = new ui::IconButton(
        icons::Glyph::Assistant, tr("Open the AI assistant"), panel);
    m_aiPanelButton->setObjectName(QStringLiteral("HeaderAiButton"));
    m_aiPanelButton->setAccessibleName(tr("AI assistant"));
    m_aiPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_aiPanelButton->setCheckable(true);
    m_aiPanelButton->setButtonSize(28, 28);
    connect(m_aiPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::aiToggled);
    panel->addAction(m_aiPanelButton);

    panel->finish();
    return panel;
}

QWidget* TransportBar::buildRightGroup() {
    auto* box = new QWidget(this);
    box->setObjectName("HeaderToolGroup");
    box->setFixedHeight(32);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(2);

    // Time format and grid stay closest to the transport, but now read as
    // compact icon commands. Their checked menu items and tooltips carry the
    // current value without making the header continually reserve text width.
    m_timeFormatButton = new QToolButton(box);
    m_timeFormatButton->setObjectName("GridChip");
    m_timeFormatButton->setPopupMode(QToolButton::InstantPopup);
    m_timeFormatButton->setCursor(Qt::PointingHandCursor);
    m_timeFormatButton->setFixedSize(28, 24);
    m_timeFormatButton->setIconSize(QSize(14, 14));
    m_timeFormatButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_timeFormatButton->setIcon(
        icons::icon(icons::Glyph::TimeFormat, th().textSecondary, 14));

    auto* timeMenu = new QMenu(m_timeFormatButton);
    auto* timeGroup = new QActionGroup(timeMenu);
    timeGroup->setExclusive(true);
    const struct { const char* label; bool bars; } kFormats[] = {
        {QT_TRANSLATE_NOOP("TransportBar", "Bars"), true},
        {QT_TRANSLATE_NOOP("TransportBar", "Time"), false}};
    for (const auto& fmt : kFormats) {
        QAction* action = timeMenu->addAction(
            QCoreApplication::translate("TransportBar", fmt.label));
        action->setCheckable(true);
        action->setChecked(fmt.bars == m_showBars);
        action->setData(fmt.bars);
        timeGroup->addAction(action);
        const bool bars = fmt.bars;
        connect(action, &QAction::triggered, this,
                [this, bars] { setTimeDisplayBars(bars); });
    }
    m_timeFormatButton->setMenu(timeMenu);
    const QString timeDescription =
        tr("Time display — %1").arg(m_showBars ? tr("Bars") : tr("Time"));
    m_timeFormatButton->setToolTip(timeDescription);
    m_timeFormatButton->setAccessibleName(timeDescription);

    m_gridButton = new QToolButton(box);
    m_gridButton->setObjectName("GridChip");
    m_gridButton->setPopupMode(QToolButton::InstantPopup);
    m_gridButton->setCursor(Qt::PointingHandCursor);
    m_gridButton->setFixedSize(28, 24);
    m_gridButton->setIconSize(QSize(14, 14));
    m_gridButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_gridButton->setIcon(
        icons::icon(icons::Glyph::GridDivision, th().textSecondary, 14));

    auto* gridMenu = new QMenu(m_gridButton);
    auto* gridGroup = new QActionGroup(gridMenu);
    gridGroup->setExclusive(true);
    const auto& divisions = ui::gridDivisions();
    for (int i = 0; i < divisions.size(); ++i) {
        QAction* action = gridMenu->addAction(divisions[i].name);
        action->setCheckable(true);
        action->setChecked(i == m_gridIndex);
        action->setData(i);
        gridGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, i] { setGridIndex(i); });
    }
    m_gridButton->setMenu(gridMenu);
    const QString gridDescription =
        tr("Grid division — %1").arg(divisions[m_gridIndex].name);
    m_gridButton->setToolTip(gridDescription);
    m_gridButton->setAccessibleName(gridDescription);

    m_snapButton = new ui::IconButton(icons::Glyph::Magnet, tr("Snap to grid"), box);
    m_snapButton->setAccessibleName(tr("Snap to grid"));
    m_snapButton->setCheckable(true);
    m_snapButton->setChecked(m_snapEnabled);
    connect(m_snapButton, &QAbstractButton::toggled, this, [this](bool on) {
        m_snapEnabled = on;
        emit snapChanged(on);
    });

    m_typingKeysButton = new ui::IconButton(icons::Glyph::MidiKeys,
                                            QString(), box);
    m_typingKeysButton->setCheckable(true);
    setTypingKeyboardOctave(m_typingOctave);   // writes the tooltip
    connect(m_typingKeysButton, &QAbstractButton::toggled, this,
            &TransportBar::typingKeyboardToggled);

    // Two tools, the way a Logic user works: the one on the pointer, and the
    // one the modifier borrows. Both are icon chips — a tool is a picture, and
    // spelling it out cost more width than it explained. The name is in the
    // menu and in the tooltip.
    auto buildToolChip = [this, box](bool primary) {
        auto* chip = new QToolButton(box);
        chip->setObjectName(primary ? "PrimaryToolChip" : "SecondaryToolChip");
        chip->setPopupMode(QToolButton::InstantPopup);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFixedSize(28, 24);
        chip->setToolButtonStyle(Qt::ToolButtonIconOnly);
        chip->setIconSize(QSize(15, 15));

        auto* menu = new QMenu(chip);
        auto* group = new QActionGroup(menu);
        group->setExclusive(true);
        for (int i = 0; i < kToolCount; ++i) {
            QAction* action = menu->addAction(
                icons::icon(kTools[i].glyph, th().textSecondary, 14),
                translatedToolName(i));
            action->setCheckable(true);
            group->addAction(action);
            (primary ? m_toolActions : m_altToolActions).push_back(action);
            connect(action, &QAction::triggered, this, [this, i, primary] {
                if (primary) setToolIndex(i);
                else setSecondaryToolIndex(i);
            });
        }
        chip->setMenu(menu);
        return chip;
    };

    m_toolButton = buildToolChip(/*primary=*/true);
    m_altToolButton = buildToolChip(/*primary=*/false);
    setToolIndex(m_toolIndex);
    setSecondaryToolIndex(m_altToolIndex);

    row->addWidget(m_timeFormatButton);
    row->addWidget(m_gridButton);
    row->addWidget(ui::separatorLine(Qt::Vertical, 22, box));
    row->addWidget(m_snapButton);
    row->addWidget(m_typingKeysButton);
    row->addWidget(ui::separatorLine(Qt::Vertical, 22, box));
    row->addWidget(m_toolButton);
    row->addWidget(m_altToolButton);
    return box;
}

QWidget* TransportBar::buildPill() {
    auto* pill = new QWidget(this);
    pill->setObjectName("TransportPill");
    pill->setFixedHeight(56);

    auto* row = new QHBoxLayout(pill);
    row->setContentsMargins(12, 0, 12, 0);
    row->setSpacing(10);

    constexpr int kBtn = 24;
    m_toStartButton = new ui::IconButton(icons::Glyph::SkipStart,
                                         tr("Return to start"), pill);
    m_toStartButton->setButtonSize(kBtn, kBtn);
    connect(m_toStartButton, &QAbstractButton::clicked, this,
            &TransportBar::returnToStartRequested);

    m_rewindButton = new ui::IconButton(icons::Glyph::Rewind,
                                        tr("Rewind one bar"), pill);
    m_rewindButton->setButtonSize(kBtn, kBtn);
    connect(m_rewindButton, &QAbstractButton::clicked, this,
            [this] { emit nudgeRequested(-1); });

    m_stopButton = new ui::IconButton(icons::Glyph::Stop, tr("Stop"), pill);
    m_stopButton->setButtonSize(kBtn, kBtn);
    connect(m_stopButton, &QAbstractButton::clicked, this,
            &TransportBar::stopRequested);

    // Play stays the primary action but flat — an accent-tinted glyph, not a
    // filled circle, so the transport block reads slim.
    m_playButton = new ui::IconButton(icons::Glyph::Play, tr("Play"), pill);
    m_playButton->setAccentTint(true);
    m_playButton->setButtonSize(kBtn, kBtn);
    connect(m_playButton, &QAbstractButton::clicked, this,
            &TransportBar::playPauseRequested);

    m_forwardButton = new ui::IconButton(icons::Glyph::Forward,
                                         tr("Forward one bar"), pill);
    m_forwardButton->setButtonSize(kBtn, kBtn);
    connect(m_forwardButton, &QAbstractButton::clicked, this,
            [this] { emit nudgeRequested(1); });

    m_recordButton = new ui::IconButton(icons::Glyph::Record, tr("Record"), pill);
    m_recordButton->setCheckable(true);
    m_recordButton->setActiveColor(Theme::record());
    m_recordButton->setButtonSize(kBtn, kBtn);
    connect(m_recordButton, &QAbstractButton::clicked, this,
            &TransportBar::recordRequested);

    m_loopButton = new ui::IconButton(icons::Glyph::Loop, tr("Cycle / loop"), pill);
    m_loopButton->setCheckable(true);
    m_loopButton->setButtonSize(kBtn, kBtn);
    connect(m_loopButton, &QAbstractButton::toggled, this,
            &TransportBar::loopToggled);

    m_metroButton = new ui::IconButton(icons::Glyph::Metronome,
                                       tr("Metronome"), pill);
    m_metroButton->setCheckable(true);
    m_metroButton->setButtonSize(kBtn, kBtn);
    connect(m_metroButton, &QAbstractButton::toggled, this,
            &TransportBar::metronomeToggled);

    // The transport buttons (return-to-start … metronome) live in their own
    // rounded sub-block — a nested panel on the header, like Logic's transport.
    m_transportGroup = new QWidget(pill);
    m_transportGroup->setObjectName("TransportGroup");
    m_transportGroup->setFixedHeight(28);   // match the Bars / grid chips
    auto* btnRow = new QHBoxLayout(m_transportGroup);
    btnRow->setContentsMargins(8, 0, 8, 0);
    btnRow->setSpacing(3);
    btnRow->addWidget(m_toStartButton);
    btnRow->addWidget(m_rewindButton);
    btnRow->addWidget(m_stopButton);
    btnRow->addWidget(m_playButton);
    btnRow->addWidget(m_forwardButton);
    btnRow->addWidget(m_recordButton);
    btnRow->addWidget(m_loopButton);
    btnRow->addWidget(m_metroButton);

    row->addWidget(m_transportGroup);
    m_lcdScreen = buildLcd();
    row->addWidget(m_lcdScreen);
    row->addWidget(m_rightGroup);
    pill->adjustSize();
    return pill;
}

QWidget* TransportBar::buildLcd() {
    // The readout: bar.beat.ticks and the tempo, side by side on one baseline,
    // in a well the same height as the transport buttons and the grid chips it
    // sits between. No captions — a monospaced "3.2.480" next to "124.0 BPM"
    // says what each one is, and the two stacked caption/value columns this
    // replaced were what made the whole pill look misaligned.
    // Liquid glass, the same plate the context island is made of: the readout
    // is the one thing in the header that should look like it is floating over
    // the interface rather than stamped into it.
    auto* screen = new ui::GlassPanel(m_pill ? m_pill : this);
    screen->setObjectName("LcdScreen");
    screen->setShadowMargin(kLcdShadow);
    screen->setCornerRadius(9);
    screen->setSubtleVerticalGradient(true);
    screen->setFixedHeight(kLcdHeight + 2 * kLcdShadow);
    auto* row = new QHBoxLayout(screen);
    row->setContentsMargins(kLcdShadow + 13, kLcdShadow, kLcdShadow + 9,
                            kLcdShadow);
    row->setSpacing(10);

    m_positionValue = new QLabel("1.1.000", screen);
    m_positionValue->setObjectName("LcdPosition");
    m_positionValue->setFont(monoFont(15, true));
    m_positionValue->setFixedSize(112, 28);
    m_positionValue->setAlignment(Qt::AlignCenter);
    m_positionValue->setAccessibleName(tr("Playhead position"));
    m_positionValue->setToolTip(tr("Playhead position — bar.beat.ticks"));
    row->addWidget(m_positionValue);

    row->addWidget(ui::separatorLine(Qt::Vertical, 22, screen));

    // Between the position and the tempo: a compact frequency view of what the
    // master bus is actually doing. It moves only when there is sound.
    m_spectrum = new SpectrumMeter(screen);
    row->addWidget(m_spectrum);

    row->addWidget(ui::separatorLine(Qt::Vertical, 22, screen));

    auto* tempoEdit = new TempoScrubEdit("120.0", screen);
    m_tempoEdit = tempoEdit;
    m_tempoEdit->setObjectName("TempoField");
    m_tempoEdit->setFont(monoFont(16, true));
    m_tempoEdit->setFixedSize(64, 28);
    m_tempoEdit->setFrame(false);
    m_tempoEdit->setAlignment(Qt::AlignCenter);
    m_tempoEdit->setAccessibleName(tr("Tempo in BPM"));
    m_tempoEdit->setAccessibleDescription(
        tr("Drag up or down to change tempo. Double-click to type a value."));
    m_tempoEdit->setToolTip(
        tr("Drag up/down to change tempo · Double-click to type"));
    tempoEdit->setScrubCallback([this](double bpm, bool finished) {
        const QString text = QString::number(bpm, 'f', 1);
        if (m_tempoEdit->text() != text) m_tempoEdit->setText(text);
        previewTempo(text);
        if (finished) commitTempo();
    });
    connect(m_tempoEdit, &QLineEdit::textEdited, this,
            &TransportBar::previewTempo);
    connect(m_tempoEdit, &QLineEdit::editingFinished, this,
            [this, tempoEdit] {
                commitTempo();
                tempoEdit->endTextEditing();
            });
    row->addWidget(m_tempoEdit);

    auto* unit = new QLabel(tr("BPM"), screen);
    unit->setObjectName("LcdUnit");
    QFont unitFont = unit->font();
    unitFont.setPixelSize(9);
    unitFont.setBold(true);
    unitFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    unit->setFont(unitFont);
    unit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    row->addWidget(unit);

    return screen;
}

double TransportBar::gridBeats() const {
    return ui::gridDivisions()[m_gridIndex].beats;
}

void TransportBar::setGridIndex(int index) {
    const auto& divisions = ui::gridDivisions();
    if (index < 0 || index >= divisions.size()) return;
    const bool changed = index != m_gridIndex;
    m_gridIndex = index;
    if (m_gridButton) {
        const QString description =
            tr("Grid division — %1").arg(divisions[index].name);
        m_gridButton->setToolTip(description);
        m_gridButton->setAccessibleName(description);
        if (m_gridButton->menu()) {
            for (QAction* action : m_gridButton->menu()->actions()) {
                if (action->data().isValid())
                    action->setChecked(action->data().toInt() == index);
            }
        }
    }
    if (!changed) return;
    QSettings().setValue(ui::kGridIndexSetting, index);
    emit gridChanged();
}

void TransportBar::setSnapEnabled(bool enabled) {
    if (m_snapButton) {
        if (m_snapButton->isChecked() != enabled)
            m_snapButton->setChecked(enabled);
        return;
    }
    if (m_snapEnabled == enabled) return;
    m_snapEnabled = enabled;
    emit snapChanged(enabled);
}

void TransportBar::setTimeDisplayBars(bool bars) {
    if (m_showBars == bars) return;
    m_showBars = bars;
    if (m_timeFormatButton) {
        const QString description =
            tr("Time display — %1").arg(bars ? tr("Bars") : tr("Time"));
        m_timeFormatButton->setToolTip(description);
        m_timeFormatButton->setAccessibleName(description);
        if (m_timeFormatButton->menu()) {
            for (QAction* action : m_timeFormatButton->menu()->actions()) {
                if (action->data().isValid())
                    action->setChecked(action->data().toBool() == bars);
            }
        }
    }
    emit timeFormatChanged();
    refresh();
}

void TransportBar::setToolIndex(int index) {
    // Only a real change is written. The constructor calls this to apply the
    // stored value, and a launch that re-saves what it just read is a launch
    // that can only ever write over a good setting with a worse one.
    if (index != m_toolIndex) QSettings().setValue(ui::kEditToolSetting, index);
    if (index < 0 || index >= kToolCount) return;
    m_toolIndex = index;
    if (m_toolButton) {
        m_toolButton->setIcon(icons::icon(kTools[index].glyph, th().accent, 15));
        const QString description =
            tr("%1 tool — on the pointer. 1…6 switch it.")
                .arg(translatedToolName(index));
        m_toolButton->setToolTip(description);
        m_toolButton->setAccessibleName(description);
    }
    if (index < m_toolActions.size() && m_toolActions[index])
        m_toolActions[index]->setChecked(true);
    emit toolChanged(index);
}

void TransportBar::setSecondaryToolIndex(int index) {
    if (index != m_altToolIndex)
        QSettings().setValue(ui::kAltEditToolSetting, index);
    if (index < 0 || index >= kToolCount) return;
    m_altToolIndex = index;
    if (m_altToolButton) {
        // Drawn in the secondary ink, so the pair reads as "this one, and this
        // one while you hold the key" rather than as two equal tools.
        m_altToolButton->setIcon(
            icons::icon(kTools[index].glyph, th().textSecondary, 15));
        const QString description =
            tr("%1 tool — while %2 is held.")
                .arg(translatedToolName(index),
                     QKeySequence(Qt::ControlModifier)
                         .toString(QKeySequence::NativeText));
        m_altToolButton->setToolTip(description);
        m_altToolButton->setAccessibleName(description);
    }
    if (index < m_altToolActions.size() && m_altToolActions[index])
        m_altToolActions[index]->setChecked(true);
    emit secondaryToolChanged(index);
}

void TransportBar::toggleCycle() {
    if (m_loopButton) m_loopButton->toggle();
}

void TransportBar::setCycleEnabled(bool on) {
    if (!m_loopButton || m_loopButton->isChecked() == on) return;
    QSignalBlocker block(m_loopButton);
    m_loopButton->setChecked(on);
}

void TransportBar::toggleMetronome() {
    if (m_metroButton) m_metroButton->toggle();
}

void TransportBar::applyTheme() {
    const Theme& t = th();

    if (m_pill) {
        // The header keeps its own colour; the transport buttons and the
        // trailing tool group are matching nested panels. The LCD is a touch
        // darker so the counters read as an inset screen.
        const QColor nested =
            mixColors(t.surfaceElevated, t.headerBackground, 0.45);
        const QColor pillBorder = mixColors(t.separator(), t.textPrimary, 0.08);
        const QColor hover = mixColors(t.surfaceElevated, t.textPrimary, 0.14);
        setStyleSheet(QString(R"(
#TransportPill { background: transparent; }
#TransportPill QLabel { color: %3; }
#TransportGroup { background: %1; border: 1px solid %2; border-radius: 9px; }
#HeaderToolGroup { background: %1; border: 1px solid %2; border-radius: 9px; }
#LcdScreen QLabel { color: %4; background: transparent; }
#LcdScreen #LcdUnit { color: %3; }
#TempoField { background: %5; border: 1px solid %2; border-radius: 6px;
              padding: 0 5px; color: %4; selection-background-color: %7; }
#TempoField:hover { background: %6; border-color: %8; }
#TempoField:focus { background: %5; border-color: %8; }
#GridChip { background: transparent; border: 1px solid transparent;
            border-radius: 6px; padding: 0; }
#GridChip:hover { background: %6; border: 1px solid %6; }
#GridChip:focus { background: %5; border: 1px solid %8; }
#GridChip::menu-indicator { image: none; width: 0; }
#PrimaryToolChip, #SecondaryToolChip {
    background: %5; border: 1px solid %2; border-radius: 8px; padding: 0;
}
#PrimaryToolChip { background: %7; border-color: %8; }
#PrimaryToolChip:hover, #SecondaryToolChip:hover { background: %6; }
#PrimaryToolChip:focus, #SecondaryToolChip:focus { border-color: %8; }
#PrimaryToolChip::menu-indicator, #SecondaryToolChip::menu-indicator {
    image: none; width: 0;
}
)")
            .arg(nested.name(),
                 pillBorder.name(),
                 t.textSecondary.name(),
                 t.textPrimary.name(),
                 t.well().name(),
                 hover.name(),
                 QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 42).name(QColor::HexArgb),
                 QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 150).name(QColor::HexArgb)));
    }
    updatePositionStyle();
    if (m_timeFormatButton)
        m_timeFormatButton->setIcon(
            icons::icon(icons::Glyph::TimeFormat, t.textSecondary, 14));
    if (m_gridButton)
        m_gridButton->setIcon(
            icons::icon(icons::Glyph::GridDivision, t.textSecondary, 14));
    if (m_toolButton && m_toolIndex >= 0 && m_toolIndex < kToolCount)
        m_toolButton->setIcon(
            icons::icon(kTools[m_toolIndex].glyph, t.accent, 16));
    if (m_altToolButton && m_altToolIndex >= 0 && m_altToolIndex < kToolCount)
        m_altToolButton->setIcon(
            icons::icon(kTools[m_altToolIndex].glyph, t.textSecondary, 16));
    for (int i = 0; i < m_toolActions.size() && i < kToolCount; ++i) {
        if (m_toolActions[i])
            m_toolActions[i]->setIcon(
                icons::icon(kTools[i].glyph, t.textSecondary, 14));
    }
    for (int i = 0; i < m_altToolActions.size() && i < kToolCount; ++i) {
        if (m_altToolActions[i])
            m_altToolActions[i]->setIcon(
                icons::icon(kTools[i].glyph, t.textSecondary, 14));
    }
    if (m_statusText) m_statusText->setStyleSheet(
        QString("color: %1;").arg(t.textSecondary.name()));
    if (m_deviceText) m_deviceText->setStyleSheet(
        QString("color: %1;").arg(t.textSecondary.name()));
    update();
}

void TransportBar::updatePositionStyle() {
    if (!m_positionValue) return;
    const Theme& t = th();
    const QColor background = mixColors(t.well(), t.surfaceElevated, 0.26);
    const QColor border = m_positionRecording
                              ? Theme::record()
                              : mixColors(t.separator(), t.textPrimary, 0.10);
    const QColor text = m_positionRecording ? Theme::record() : t.textPrimary;
    // A flat, typographic timecode chip replaces the novelty seven-segment
    // display. Tabular system digits stay steady as the playhead advances, and
    // the border only gains semantic colour while recording.
    m_positionValue->setStyleSheet(
        QString("QLabel#LcdPosition { background: %1; border: 1px solid %2; "
                "border-radius: 6px; color: %3; padding: 0 7px; }")
            .arg(background.name(), border.name(), text.name()));
}

void TransportBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    // The header has its own colour (per-theme `headerBackground`); the control
    // blocks are nested flat panels on top of it — no drop shadow.
    p.fillRect(rect(), t.headerBackground);

    p.setPen(QPen(t.sectionDivider(), 1));
    p.drawLine(0, height() - 1, width(), height() - 1);
}

void TransportBar::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    updateResponsiveLayout();
}

int TransportBar::minimumResponsiveWidth() const {
    if (!m_pill || !m_transportGroup || !m_lcdScreen || !m_rightGroup)
        return 0;

    const auto widgetWidth = [](const QWidget* widget) {
        if (!widget) return 0;
        int width = std::max(widget->sizeHint().width(),
                             widget->minimumSizeHint().width());
        width = std::max(width, widget->minimumWidth());
        if (widget->layout()) {
            width = std::max(width, widget->layout()->sizeHint().width());
            width = std::max(width, widget->layout()->minimumSize().width());
        }
        return std::max(0, width);
    };
    const auto layoutWidth = [&widgetWidth](const QLayout* layout,
                                           const QList<QWidget*>& widgets) {
        if (!layout) return 0;
        const QMargins margins = layout->contentsMargins();
        int width = margins.left() + margins.right();
        int visible = 0;
        for (QWidget* widget : widgets) {
            if (!widget) continue;
            width += widgetWidth(widget);
            ++visible;
        }
        if (visible > 1) width += layout->spacing() * (visible - 1);
        return width;
    };

    // The transport group's minimum state is deliberately just these three
    // core actions. Compute it without hiding live widgets, so querying the
    // panel budget cannot make the header flash during a resize.
    const int coreTransport = layoutWidth(
        m_transportGroup->layout(), {m_stopButton, m_playButton, m_recordButton});
    const int completeCluster = layoutWidth(
        m_pill->layout(), {m_transportGroup, m_lcdScreen, m_rightGroup}) -
        widgetWidth(m_transportGroup) + coreTransport;

    // The fixed right well is broader than the left well's collapsed state,
    // so it defines the symmetric edge reserve without making a temporarily
    // expanded drawer inflate the application's permanent minimum width.
    constexpr int kOuterMargin = 14;
    constexpr int kDockGap = 10;
    const int edgeDock = m_rightDock ? widgetWidth(m_rightDock) : 0;
    return 2 * (kOuterMargin + kDockGap + edgeDock) + completeCluster;
}

void TransportBar::updateResponsiveLayout() {
    if (!m_pill) return;

    const auto fitTransportGroup = [this] {
        if (!m_transportGroup || !m_transportGroup->layout()) return;
        QLayout* layout = m_transportGroup->layout();
        const QMargins margins = layout->contentsMargins();
        int width = margins.left() + margins.right();
        int visible = 0;
        for (int i = 0; i < layout->count(); ++i) {
            QWidget* widget = layout->itemAt(i)->widget();
            if (!widget || widget->isHidden()) continue;
            width += std::max(widget->sizeHint().width(),
                              widget->minimumWidth());
            ++visible;
        }
        if (visible > 1) width += layout->spacing() * (visible - 1);
        // QBoxLayout keeps the old group's broad size hint cached while child
        // buttons are hidden. Fixing the group to the width of what is really
        // visible is what lets the readout move left instead of being laid out
        // below the fixed trailing controls.
        m_transportGroup->setFixedWidth(std::max(1, width));
    };

    // Restore everything before measuring: enlarging the window must bring
    // controls back in the same deterministic priority order used to hide
    // them. The three core actions are never candidates.
    QWidget* const optional[] = {
        m_forwardButton, m_rewindButton, m_loopButton, m_metroButton,
        m_toStartButton};
    for (QWidget* item : optional)
        if (item) item->show();
    fitTransportGroup();

    constexpr int kOuterMargin = 14;
    constexpr int kDockGap = 10;
    const auto dockY = [this](QWidget* dock) {
        return dock ? (height() - dock->height()) / 2 : 0;
    };
    if (m_leftDock)
        m_leftDock->move(kOuterMargin, dockY(m_leftDock));
    if (m_rightDock)
        m_rightDock->move(std::max(kOuterMargin,
                                  width() - kOuterMargin - m_rightDock->width()),
                          dockY(m_rightDock));

    // Reserve the broader edge on both sides. The transport/LCD/edit cluster
    // therefore remains mathematically centred while the left well opens,
    // instead of being pushed sideways by the drawer animation.
    const int edgeDock = std::max(m_leftDock ? m_leftDock->width() : 0,
                                  m_rightDock ? m_rightDock->width() : 0);
    const int safeLeft = kOuterMargin + edgeDock + kDockGap;
    const int safeRight = width() - safeLeft;
    const int available = std::max(0, safeRight - safeLeft);

    const auto measuredWidth = [this] {
        if (m_pill->layout()) {
            m_pill->layout()->invalidate();
            m_pill->layout()->activate();
        }
        m_pill->adjustSize();
        return m_pill->sizeHint().width();
    };

    int pillWidth = measuredWidth();
    for (QWidget* item : optional) {
        if (pillWidth <= available) break;
        if (!item) continue;
        item->hide();
        fitTransportGroup();
        pillWidth = measuredWidth();
    }

    // This is the complete cluster, not only the LCD. Its centre therefore is
    // the visual centre of all three header elements at every usable width.
    int x = (width() - pillWidth) / 2;
    if (pillWidth <= available) {
        const int maxX = std::max(safeLeft, safeRight - pillWidth);
        x = std::clamp(x, safeLeft, maxX);
    } else {
        // At the application's deliberately squeezed Web+AI test width there
        // is not enough horizontal space for the opened drawer and even the
        // irreducible transport readout at once. Keep the transport centred
        // and let the short-lived drawer overlay its far-left controls; moving
        // the timeline's principal readout would be the more disruptive
        // failure. At normal workspace widths the reserved wells never meet.
        x = std::max(0, x);
    }
    m_pill->setGeometry(x, (height() - m_pill->height()) / 2, pillWidth,
                        m_pill->height());
}

QString TransportBar::positionText() const {
    const double seconds = m_controller->presentationPositionSeconds();
    if (!m_showBars) {
        const int mins = int(seconds) / 60;
        const double rest = seconds - mins * 60;
        return QString::asprintf("%d:%06.3f", mins, rest);
    }
    const double tempo = std::max(1.0, m_controller->tempo());
    const double beats = seconds * tempo / 60.0;
    const int beatsPerBar = std::max(1, m_controller->timeSigNumerator());
    const int bar = int(beats) / beatsPerBar + 1;
    const int beat = int(beats) % beatsPerBar + 1;
    const int ticks = int((beats - std::floor(beats)) * 1000.0);
    return QString::asprintf("%d.%d.%03d", bar, beat, ticks);
}

void TransportBar::refreshPosition() {
    if (!m_positionValue) return;
    const QString position = positionText();
    if (m_positionValue->text() != position) m_positionValue->setText(position);
}

void TransportBar::refresh() {
    const bool playing = m_controller->isPlaying();
    const bool recording = m_controller->isRecording();

    m_playButton->setGlyph(playing ? icons::Glyph::Pause : icons::Glyph::Play);
    m_playButton->setToolTip(playing ? tr("Pause") : tr("Play"));
    // Engaged and rolling both light the button; only the shade differs, so the
    // two states are never mistaken for each other.
    if (recording) m_recordEngaged = true;
    const bool lit = recording || m_recordEngaged;
    if (m_recordButton->isChecked() != lit) {
        QSignalBlocker block(m_recordButton);
        m_recordButton->setChecked(lit);
    }
    m_recordButton->setActiveColor(Theme::record());
    // Engaged glows: the take is set up in the context panel and starts on the
    // next press. Rolling is steady red — a light that breathes would be
    // saying "waiting" while the machine is already recording.
    m_recordButton->setPulse(m_recordEngaged && !recording);
    m_recordButton->setToolTip(recording  ? tr("Stop recording")
                               : m_recordEngaged
                                   ? tr("Record engaged — start from the panel "
                                        "or press R")
                                   : tr("Record"));

    // While rolling the dedicated lightweight playhead clock owns this at
    // display cadence. Avoid formatting the same position again on the slower
    // meter/plugin UI tick.
    if (!playing) refreshPosition();
    if (m_positionRecording != recording) {
        m_positionRecording = recording;
        updatePositionStyle();
    }

    const bool online = m_controller->isDeviceOpen();
    const QColor dot = !online ? QColor(0xE0, 0x9B, 0x3B)
                              : recording ? Theme::record()
                              : playing ? QColor(0x4C, 0xC4, 0x8A)
                                        : th().textSecondary;
    if (m_statusDot) {
        const QString style =
            QString("background: %1; border-radius: 3px;").arg(dot.name());
        if (m_statusDot->styleSheet() != style) m_statusDot->setStyleSheet(style);
    }
    if (m_statusText) {
        const QString status = !online ? tr("Offline")
                               : recording ? tr("Recording")
                               : playing ? tr("Playing")
                                         : tr("Stopped");
        if (m_statusText->text() != status) m_statusText->setText(status);
    }
    if (m_spectrum) {
        // Real log-spaced master-bus bands. The device stays live while the
        // transport is parked so previews, keyboard notes and effect tails are
        // represented rather than being mistaken for silence.
        m_spectrum->setAccent(recording ? Theme::record() : th().accent);
        m_spectrum->push(m_controller->masterSpectrum(), online);
    }

    if (m_deviceText) {
        const QString device = QString("%1k  %2")
                                   .arg(m_controller->sampleRate() / 1000.0, 0, 'g', 3)
                                   .arg(m_controller->bufferSizeFrames());
        if (m_deviceText->text() != device) m_deviceText->setText(device);
    }
}

void TransportBar::setRecordEngaged(bool engaged) {
    if (m_recordEngaged == engaged) return;
    m_recordEngaged = engaged;
    refresh();   // the button's shade and tooltip both follow from the state
}

void TransportBar::setTypingKeyboardActive(bool active) {
    if (!m_typingKeysButton || m_typingKeysButton->isChecked() == active) return;
    // The signal is what MainWindow drives the keyboard with, and this setter
    // exists precisely to reflect a change that came from there.
    QSignalBlocker block(m_typingKeysButton);
    m_typingKeysButton->setChecked(active);
}

void TransportBar::setTypingKeyboardOctave(int octave) {
    m_typingOctave = octave;
    if (!m_typingKeysButton) return;
    const QString description =
        tr("Typing keyboard — the computer keys play notes\n"
           "Z…M and Q…P, two octaves from C%1  ·  [ and ] shift the octave")
            .arg(octave);
    m_typingKeysButton->setToolTip(description);
    m_typingKeysButton->setAccessibleName(description);
}

namespace {
void reflectToggle(ui::IconButton* button, bool checked) {
    if (!button || button->isChecked() == checked) return;
    QSignalBlocker block(button);
    button->setChecked(checked);
}
} // namespace

void TransportBar::setMixerVisible(bool visible) {
    reflectToggle(m_mixerPanelButton, visible);
}

void TransportBar::setInspectorVisible(bool visible) {
    reflectToggle(m_inspectorPanelButton, visible);
}

void TransportBar::setBrowserVisible(bool visible) {
    reflectToggle(m_browserPanelButton, visible);
}

void TransportBar::setWebVisible(bool visible) {
    reflectToggle(m_webPanelButton, visible);
}

void TransportBar::setAiVisible(bool visible) {
    reflectToggle(m_aiPanelButton, visible);
}

void TransportBar::setMixerDetached(bool detached) {
    if (!m_detachMixerButton) return;
    m_detachMixerButton->setEnabled(!detached);
    const QString description = detached
        ? tr("Mixer is already open in its own window")
        : tr("Open the mixer in its own window");
    m_detachMixerButton->setToolTip(description);
    m_detachMixerButton->setAccessibleName(description);
}

void TransportBar::syncTempo() {
    if (!m_tempoEdit) return;
    m_tempoEditing = false;
    m_tempoEdit->setText(QString::number(m_controller->tempo(), 'f', 1));
}

void TransportBar::previewTempo(const QString& text) {
    bool ok = false;
    const double bpm = QString(text).replace(',', '.').toDouble(&ok);
    // Partial input (empty, "1", "12.") is allowed to remain in the editor,
    // but only a complete project-valid value can drive the timeline.
    if (!ok || bpm < 20.0 || bpm > 300.0 || bpm == m_controller->tempo()) return;
    if (!m_tempoEditing) {
        m_tempoEditing = true;
        m_tempoUndoDepth = m_controller->undoDepth();
    }
    emit tempoChanged(bpm);
}

void TransportBar::commitTempo() {
    // Hand the keyboard back as soon as the value is in: the transport keys
    // matter more than the field.
    m_tempoEdit->clearFocus();
    bool ok = false;
    const double bpm = m_tempoEdit->text().replace(',', '.').toDouble(&ok);
    if (!ok || bpm < 20.0 || bpm > 300.0) {
        if (m_tempoEditing)
            m_controller->collapseUndo(m_tempoUndoDepth, "Set Tempo");
        syncTempo();
        return;
    }
    m_tempoEdit->setText(QString::number(bpm, 'f', 1));
    if (bpm != m_controller->tempo()) emit tempoChanged(bpm);
    if (m_tempoEditing) {
        m_controller->collapseUndo(m_tempoUndoDepth, "Set Tempo");
        m_tempoEditing = false;
    }
}
