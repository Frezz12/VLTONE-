#include "ToolPanel.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "InspectorWidget.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include <QAbstractButton>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStyleOption>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kCompactZoomSteps = 200;
double timelineZoomForSlider(int value) {
    const double position = std::clamp(value, 0, kCompactZoomSteps) /
                            double(kCompactZoomSteps);
    return ui::kMinTimelineZoom *
           std::pow(ui::kMaxTimelineZoom / ui::kMinTimelineZoom, position);
}

int sliderForTimelineZoom(double pixelsPerSecond) {
    const double zoom = std::clamp(pixelsPerSecond, ui::kMinTimelineZoom,
                                   ui::kMaxTimelineZoom);
    const double position =
        std::log(zoom / ui::kMinTimelineZoom) /
        std::log(ui::kMaxTimelineZoom / ui::kMinTimelineZoom);
    return int(std::lround(position * kCompactZoomSteps));
}

using ui::kViewControlWidth;
using ui::kViewControlHeight;
using ui::paintViewControl;

class WaveformScaleButton final : public ui::IconButton {
public:
    using Change = std::function<void(double)>;

    explicit WaveformScaleButton(Change change, QWidget* parent)
        : ui::IconButton(
              icons::Glyph::Waveform,
              QObject::tr("Waveform height: drag or use Up/Down; double-click or Home resets"),
              parent),
          m_change(std::move(change)) {
        setObjectName(QStringLiteral("WaveformScaleButton"));
        setAccessibleName(QObject::tr("Waveform display height"));
        setFocusPolicy(Qt::StrongFocus);
        setButtonSize(kViewControlWidth, kViewControlHeight);
        setCursor(Qt::SizeVerCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintViewControl(painter, this, icons::Glyph::Waveform,
                         m_dragging || isDown());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_cursorDrag.begin(event->globalPosition());
        }
        ui::IconButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_dragging) {
            ui::IconButton::mouseMoveEvent(event);
            return;
        }
        if (!(event->buttons() & Qt::LeftButton)) {
            m_cursorDrag.cancel();
            m_dragging = false;
            setDown(false);
            ui::ValueBubble::dismiss();
            update();
            return;
        }
        const bool moved =
            applyPointerDelta(m_cursorDrag.takeDelta(event->globalPosition()));
        if (!moved) {
            event->accept();
            return;
        }
        ui::ValueBubble::showFor(
            this, rect().center(),
            QObject::tr("Waveform %1%").arg(int(std::lround(m_value * 100.0))));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        const bool wasDragging = m_dragging;
        if (wasDragging)
            applyPointerDelta(m_cursorDrag.finish(event->globalPosition()));
        m_dragging = false;
        if (wasDragging) {
            ui::ValueBubble::dismiss();
            setCursor(Qt::SizeVerCursor);
        }
        ui::IconButton::mouseReleaseEvent(event);
    }

    void hideEvent(QHideEvent* event) override {
        m_cursorDrag.cancel();
        m_dragging = false;
        ui::ValueBubble::dismiss();
        ui::IconButton::hideEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            ui::IconButton::mouseDoubleClickEvent(event);
            return;
        }
        setValue(1.0);
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Up) {
            setValue(m_value * std::pow(2.0, 0.125));
        } else if (event->key() == Qt::Key_Down) {
            setValue(m_value / std::pow(2.0, 0.125));
        } else if (event->key() == Qt::Key_Home) {
            setValue(1.0);
        } else {
            ui::IconButton::keyPressEvent(event);
            return;
        }
        event->accept();
    }

private:
    bool applyPointerDelta(const QPointF& delta) {
        if (std::abs(delta.y()) < 1.0e-9) return false;
        return setValue(m_value * std::pow(2.0, -delta.y() / 80.0));
    }

    bool setValue(double value) {
        const double next = std::clamp(value, 0.25, 4.0);
        if (std::abs(next - m_value) < 1.0e-9) return false;
        m_value = next;
        if (m_change) m_change(m_value);
        return true;
    }

    Change m_change;
    double m_value = 1.0;
    ui::LockedCursorDrag m_cursorDrag;
    bool m_dragging = false;
};

}  // namespace

ToolPanel::ToolPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("ToolPanel");
    // Child QWidget backgrounds are not stylesheet-painted consistently by
    // the Windows backend unless the styled-background attribute is explicit.
    setAttribute(Qt::WA_StyledBackground, true);
    // The moving context island and the strip share one compact height.
    setFixedHeight(ui::kToolPanelHeight);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 0, 12, 1);
    row->setSpacing(4);
    m_row = row;

    // Browser zone: lines up with the browser column, wherever it is. Built
    // first because the browser is left of the inspector by default; moving it
    // to the other side re-inserts this zone at the end (see setBrowserOnLeft).
    // The browser and the inspector are opened from the header drawer, which is
    // where every panel toggle in the application now lives. Their zones stay:
    // they are what makes the strip line up with the columns beneath it, and
    // without them the playback switches drift into the middle of the track
    // headers instead of standing at the timeline's left edge.
    m_browserZone = new QWidget(this);
    auto* bz = new QHBoxLayout(m_browserZone);
    bz->setContentsMargins(2, 0, 2, 0);
    bz->setSpacing(2);
    bz->addStretch(1);
    row->addWidget(m_browserZone);
    m_browserSeparator = ui::separatorLine(Qt::Vertical, 18, this);
    row->addWidget(m_browserSeparator);

    // Assistant zone: always the far right, matching the column it labels. Set
    // up here but appended by moveAiZoneLast() once the row is complete.
    m_aiZone = new QWidget(this);
    auto* az = new QHBoxLayout(m_aiZone);
    az->setContentsMargins(2, 0, 2, 0);
    az->setSpacing(2);
    az->addStretch(1);
    m_aiSeparator = ui::separatorLine(Qt::Vertical, 18, this);

    // Inspector zone: lines up with the inspector column on the left.
    m_inspectorZone = new QWidget(this);
    auto* iz = new QHBoxLayout(m_inspectorZone);
    iz->setContentsMargins(2, 0, 2, 0);
    iz->setSpacing(2);
    iz->addStretch(1);
    setInspectorZoneWidth(InspectorWidget::kExpandedWidth);
    row->addWidget(m_inspectorZone);

    row->addWidget(ui::separatorLine(Qt::Vertical, 18, this));

    // Tracks zone: lines up with the track-header column.
    m_trackZone = new QWidget(this);
    auto* tz = new QHBoxLayout(m_trackZone);
    tz->setContentsMargins(2, 0, 2, 0);
    tz->setSpacing(2);
    m_trackActions = new QWidget(m_trackZone);
    m_trackActions->setObjectName(QStringLiteral("TrackRulerActions"));
    m_trackActions->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* actions = new QHBoxLayout(m_trackActions);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(1);

    m_createTrack = new ui::IconButton(icons::Glyph::Plus, tr("Create tracks…"), m_trackActions);
    m_createTrack->setObjectName(QStringLiteral("TrackCreateButton"));
    m_createTrack->setFixedSize(22, 22);
    m_createTrack->setFocusPolicy(Qt::StrongFocus);
    m_createTrack->setAccessibleName(tr("Create tracks"));
    connect(m_createTrack, &QAbstractButton::clicked, this, &ToolPanel::createTracksRequested);
    actions->addWidget(m_createTrack);

    // The two playback switches live here, next to the add-track button and
    // directly above the header column's own M and S chips — *not* over the
    // arrangement. They used to stand at the timeline's near corner, which is
    // exactly where the context island travels: a switch you cannot reach
    // because a plate is parked on it is not a switch.
    m_restart = new ui::IconButton(
        icons::Glyph::Restart,
        tr("Restart: Space starts from the anchored spot"), m_trackActions);
    m_restart->setCheckable(true);
    connect(m_restart, &QAbstractButton::toggled, this,
            &ToolPanel::restartModeToggled);

    m_playFromClip = new ui::IconButton(
        icons::Glyph::ClipLoop,
        tr("From clip: Space plays the selected clip over and over"), m_trackActions);
    m_playFromClip->setCheckable(true);
    connect(m_playFromClip, &QAbstractButton::toggled, this,
            &ToolPanel::playFromClipToggled);

    m_createAutomation = new ui::IconButton(
        icons::Glyph::AutomationCreate,
        tr("Create automation: click to enable, then double-click a parameter"),
        m_trackActions);
    m_createAutomation->setObjectName(QStringLiteral("AutomationCreateMode"));
    m_createAutomation->setCheckable(true);
    m_createAutomation->setAccessibleName(tr("Create automation clips"));
    connect(m_createAutomation, &QAbstractButton::toggled, this,
            &ToolPanel::automationCreationModeToggled);

    m_showAutomation = new ui::IconButton(
        icons::Glyph::Automation,
        tr("Show or hide automation lanes for selected tracks"), m_trackActions);
    m_showAutomation->setCheckable(true);
    m_showAutomation->setAccessibleName(
        tr("Show or hide automation lanes for selected tracks"));
    connect(m_showAutomation, &QAbstractButton::toggled, this,
            &ToolPanel::automationVisibilityToggled);

    m_followPlayhead = new ui::IconButton(
        icons::Glyph::SkipEnd,
        tr("Follow the playhead — P centres it; press again to navigate freely"),
        m_trackActions);
    m_followPlayhead->setObjectName(QStringLiteral("FollowPlayheadButton"));
    m_followPlayhead->setCheckable(true);
    m_followPlayhead->setAccessibleName(tr("Follow the playhead"));
    connect(m_followPlayhead, &QAbstractButton::toggled, this,
            &ToolPanel::followPlayheadToggled);

    for (ui::IconButton* button : {m_restart, m_playFromClip,
                                   m_createAutomation, m_showAutomation,
                                   m_followPlayhead}) {
        button->setButtonSize(22, 22);
        actions->addWidget(button);
    }
    actions->addStretch(1);
    tz->addWidget(m_trackActions);
    tz->addStretch(1);
    m_trackZone->setFixedWidth(ui::kTrackHeaderWidth);
    row->addWidget(m_trackZone);

    row->addWidget(ui::separatorLine(Qt::Vertical, 18, this));

    // Keep the context travel area aligned with the arrangement. The view
    // controls are handed to its right rail once MainWindow creates the timeline.
    m_timelineZone = new QWidget(this);
    m_timelineZone->setFixedHeight(ui::kToolPanelHeight - 1);
    auto* timelineLayout = new QHBoxLayout(m_timelineZone);
    timelineLayout->setContentsMargins(2, 0, 2, 0);
    timelineLayout->setSpacing(0);
    timelineLayout->addStretch(1);
    auto* viewControls = new ui::ViewControlStrip(m_timelineZone);
    viewControls->setObjectName(QStringLiteral("TimelineViewControls"));
    m_timelineViewControls = viewControls;
    auto* viewRow = new QVBoxLayout(viewControls);
    viewRow->setContentsMargins(0, 0, 0, 0);
    viewRow->setSpacing(0);
    m_waveformScale = new WaveformScaleButton(
        [this](double scale) { emit waveformScaleChanged(scale); }, viewControls);
    viewRow->addWidget(m_waveformScale);

    m_trackHeightSlider = new ui::ViewScrubSlider(
        icons::Glyph::ResizeVertical, ui::ViewScrubSlider::Axis::Vertical,
        ui::kLaneHeight, viewControls);
    m_trackHeightSlider->setObjectName(QStringLiteral("TimelineTrackHeightSlider"));
    m_trackHeightSlider->setRange(ui::kMinLaneHeight, 180);
    m_trackHeightSlider->setValue(ui::kLaneHeight);
    m_trackHeightSlider->setSingleStep(2);
    m_trackHeightSlider->setPageStep(12);
    m_trackHeightSlider->setAccessibleName(tr("Timeline track height"));
    m_trackHeightSlider->setToolTip(
        tr("Track height: drag the icon up or down; double-click resets"));

    const int defaultZoomValue = sliderForTimelineZoom(80.0);
    m_timelineZoomSlider = new ui::ViewScrubSlider(
        icons::Glyph::ResizeHorizontal, ui::ViewScrubSlider::Axis::Horizontal,
        defaultZoomValue, viewControls);
    m_timelineZoomSlider->setObjectName(QStringLiteral("TimelineZoomSlider"));
    m_timelineZoomSlider->setRange(0, kCompactZoomSteps);
    m_timelineZoomSlider->setValue(defaultZoomValue);
    m_timelineZoomSlider->setSingleStep(1);
    m_timelineZoomSlider->setPageStep(8);
    m_timelineZoomSlider->setAccessibleName(tr("Timeline horizontal zoom"));
    m_timelineZoomSlider->setToolTip(
        tr("Timeline zoom: drag the icon left or right; double-click resets"));
    viewRow->addWidget(m_timelineZoomSlider);
    viewRow->addWidget(m_trackHeightSlider);
    QWidget::setTabOrder(m_waveformScale, m_timelineZoomSlider);
    QWidget::setTabOrder(m_timelineZoomSlider, m_trackHeightSlider);
    timelineLayout->addWidget(viewControls, 0, Qt::AlignVCenter);
    row->addWidget(m_timelineZone, 1, Qt::AlignTop);

    connect(m_trackHeightSlider, &QSlider::sliderPressed, this, [this] {
        m_trackHeightDragging = true;
        emit trackHeightEditStarted();
    });
    connect(m_trackHeightSlider, &QSlider::valueChanged, this, [this](int value) {
        const bool oneShot = !m_trackHeightDragging;
        if (oneShot) emit trackHeightEditStarted();
        emit trackHeightChanged(value);
        if (oneShot) emit trackHeightEditFinished();
    });
    connect(m_trackHeightSlider, &QSlider::sliderMoved, this, [this](int value) {
        ui::ValueBubble::showFor(
            m_trackHeightSlider, m_trackHeightSlider->rect().center(),
            tr("Tracks %1 px").arg(value));
    });
    connect(m_trackHeightSlider, &QSlider::sliderReleased, this, [this] {
        m_trackHeightDragging = false;
        ui::ValueBubble::dismiss();
        emit trackHeightEditFinished();
    });
    connect(m_timelineZoomSlider, &QSlider::valueChanged, this, [this](int value) {
        emit timelineZoomChanged(timelineZoomForSlider(value));
    });
    connect(m_timelineZoomSlider, &QSlider::sliderMoved, this, [this](int value) {
        const double relative = timelineZoomForSlider(value) / 80.0;
        ui::ValueBubble::showFor(
            m_timelineZoomSlider, m_timelineZoomSlider->rect().center(),
            tr("Zoom %1×").arg(relative, 0, 'f', relative < 1.0 ? 2 : 1));
    });
    connect(m_timelineZoomSlider, &QSlider::sliderReleased,
            this, [] { ui::ValueBubble::dismiss(); });

    // Past the stretch, so it sits over the assistant column at the far right.
    moveAiZoneLast();
    setAiVisible(false);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ToolPanel::applyTheme);
    applyTheme();
}

QWidget* ToolPanel::takeTrackActions() {
    if (!m_trackActions) return nullptr;
    if (m_trackZone && m_trackZone->layout())
        m_trackZone->layout()->removeWidget(m_trackActions);
    QWidget* actions = m_trackActions;
    actions->setParent(nullptr);
    return actions;
}

QWidget* ToolPanel::takeTimelineViewControls() {
    QWidget* controls = m_timelineViewControls;
    if (!controls) return nullptr;
    m_timelineZone->layout()->removeWidget(controls);
    controls->setParent(nullptr);
    m_timelineViewControls = nullptr;
    return controls;
}

void ToolPanel::setRestartMode(bool on) {
    if (m_restart && m_restart->isChecked() != on) m_restart->setChecked(on);
}

void ToolPanel::setPlayFromClip(bool on) {
    if (m_playFromClip && m_playFromClip->isChecked() != on)
        m_playFromClip->setChecked(on);
}

void ToolPanel::setFollowPlayhead(bool on) {
    if (!m_followPlayhead || m_followPlayhead->isChecked() == on) return;
    QSignalBlocker blocker(m_followPlayhead);
    m_followPlayhead->setChecked(on);
}

void ToolPanel::setAutomationVisible(bool visible) {
    if (!m_showAutomation || m_showAutomation->isChecked() == visible) return;
    QSignalBlocker blocker(m_showAutomation);
    m_showAutomation->setChecked(visible);
}

void ToolPanel::setAutomationCreationActive(bool active) {
    if (!m_createAutomation || m_createAutomation->isChecked() == active) return;
    QSignalBlocker blocker(m_createAutomation);
    m_createAutomation->setChecked(active);
}

void ToolPanel::setAutomationCreationShortcut(const QString& shortcut) {
    QString tip = tr("Create automation: click to enable, then double-click a parameter");
    if (!shortcut.isEmpty()) tip += QStringLiteral(" (%1)").arg(shortcut);
    m_createAutomation->setToolTip(tip);
}

void ToolPanel::setTrackHeightValue(int height) {
    if (!m_trackHeightSlider) return;
    const QSignalBlocker blocker(m_trackHeightSlider);
    m_trackHeightSlider->setValue(height);
}

void ToolPanel::setTimelineZoom(double pixelsPerSecond) {
    if (!m_timelineZoomSlider) return;
    const QSignalBlocker blocker(m_timelineZoomSlider);
    m_timelineZoomSlider->setValue(sliderForTimelineZoom(pixelsPerSecond));
}

void ToolPanel::setInspectorVisible(bool) {}

void ToolPanel::setInspectorZoneWidth(int width) {
    if (m_inspectorZone) m_inspectorZone->setFixedWidth(std::max(30, width));
}

void ToolPanel::setAiVisible(bool visible) {
    if (m_aiZone) m_aiZone->setVisible(visible);
    if (m_aiSeparator) m_aiSeparator->setVisible(visible);
}

void ToolPanel::moveAiZoneLast() {
    if (!m_row || !m_aiZone) return;
    m_row->removeWidget(m_aiZone);
    if (m_aiSeparator) m_row->removeWidget(m_aiSeparator);
    if (m_aiSeparator) m_row->addWidget(m_aiSeparator);
    m_row->addWidget(m_aiZone);
    m_aiZone->show();
    if (m_aiSeparator) m_aiSeparator->show();
}

void ToolPanel::setBrowserZoneWidth(int width) {
    if (m_browserZone) m_browserZone->setFixedWidth(std::max(30, width));
}

void ToolPanel::setTrackZoneWidth(int width) {
    if (m_trackZone) m_trackZone->setFixedWidth(
        std::max(ui::kMinTrackHeaderWidth, width));
}

void ToolPanel::setBrowserVisible(bool visible) {
    // The zone follows the column: with no browser there is nothing to line up
    // with, and an empty gap on the left would look like a missing panel.
    if (m_browserZone) m_browserZone->setVisible(visible);
    if (m_browserSeparator) m_browserSeparator->setVisible(visible);
}

void ToolPanel::setBrowserOnLeft(bool onLeft) {
    if (!m_row || !m_browserZone) return;
    m_row->removeWidget(m_browserZone);
    if (m_browserSeparator) m_row->removeWidget(m_browserSeparator);
    if (onLeft) {
        // Ahead of everything, matching the column order below.
        m_row->insertWidget(0, m_browserZone);
        if (m_browserSeparator) m_row->insertWidget(1, m_browserSeparator);
    } else {
        // After the stretch that the timeline zone ends with, so the button
        // sits over the panel at the right edge.
        if (m_browserSeparator) m_row->addWidget(m_browserSeparator);
        m_row->addWidget(m_browserZone);
    }
    m_browserZone->show();
    if (m_browserSeparator) m_browserSeparator->show();
    // The browser may have just been appended past the assistant; put the
    // assistant back on the outside, where its panel is.
    moveAiZoneLast();
}

void ToolPanel::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    emit resized();
}

int ToolPanel::contextLeftEdge() const {
    int edge = 12;
    if (!m_trackZone) return edge;
    for (auto* button : m_trackZone->findChildren<QAbstractButton*>()) {
        if (!button->isHidden())
            edge = std::max(edge, button->mapTo(this, QPoint(button->width(), 0)).x() + 8);
    }
    return edge;
}

int ToolPanel::contextRightEdge() const {
    if (!m_timelineZone) return width() - 12;
    return std::min(width() - 12,
                    m_timelineZone->mapTo(this, QPoint(m_timelineZone->width(), 0)).x());
}

bool ToolPanel::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest) {
        // Sidebar/track-zone widths can change without resizing this strip.
        // Notify after Qt has positioned the available context area.
        if (m_row) m_row->activate();
        emit resized();
    }
    return handled;
}

void ToolPanel::applyTheme() {
    const Theme& t = th();
    // Recess the travel rail below the header-coloured context plate. Derive
    // it from that same colour so custom palettes retain the separation too.
    const QColor rail = mixColors(t.headerBackground, QColor(0, 0, 0),
                                  t.dark ? 0.28 : 0.12);
    if (m_followPlayhead) {
        m_followPlayhead->setActiveColor(t.cursor);
        m_followPlayhead->setIcon(
            icons::svgIcon(QStringLiteral("signpost.svg"), t.textPrimary, 18));
    }
    setStyleSheet(QString(R"(
#ToolPanel { background: %1; border-bottom: 1px solid %2; }
)")
        .arg(rail.name(),
             t.sectionDivider().name()));
}
