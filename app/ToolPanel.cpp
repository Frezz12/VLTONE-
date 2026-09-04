#include "ToolPanel.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

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
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_startY = event->globalPosition().y();
            m_startValue = m_value;
            setCursor(Qt::SizeVerCursor);
        }
        ui::IconButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_dragging) {
            ui::IconButton::mouseMoveEvent(event);
            return;
        }
        setValue(m_startValue *
                 std::pow(2.0, (m_startY - event->globalPosition().y()) / 80.0));
        ui::ValueBubble::showFor(
            this, rect().center(),
            QObject::tr("Waveform %1%").arg(int(std::lround(m_value * 100.0))));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        const bool wasDragging = m_dragging;
        m_dragging = false;
        if (wasDragging) {
            ui::ValueBubble::dismiss();
            setCursor(Qt::PointingHandCursor);
        }
        ui::IconButton::mouseReleaseEvent(event);
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
    void setValue(double value) {
        const double next = std::clamp(value, 0.25, 4.0);
        if (std::abs(next - m_value) < 1.0e-9) return;
        m_value = next;
        if (m_change) m_change(m_value);
    }

    Change m_change;
    double m_value = 1.0;
    double m_startValue = 1.0;
    double m_startY = 0.0;
    bool m_dragging = false;
};

}  // namespace

ToolPanel::ToolPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("ToolPanel");
    // Tall enough to hold the context-panel island with its shadow: a child
    // can't paint outside its parent, so the strip has to make room.
    setFixedHeight(44);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 2, 8, 2);
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
    setInspectorZoneWidth(152);
    row->addWidget(m_inspectorZone);

    row->addWidget(ui::separatorLine(Qt::Vertical, 18, this));

    // Tracks zone: lines up with the track-header column.
    m_trackZone = new QWidget(this);
    auto* tz = new QHBoxLayout(m_trackZone);
    tz->setContentsMargins(2, 0, 2, 0);
    tz->setSpacing(2);
    auto* addTrack = new ui::IconButton(
        icons::Glyph::Plus,
        tr("Add an audio track — right-click for MIDI, instrument, bus and "
           "folder tracks"),
        m_trackZone);
    addTrack->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(addTrack, &QAbstractButton::clicked, this,
            &ToolPanel::addTrackRequested);
    connect(addTrack, &QWidget::customContextMenuRequested, this,
            [this, addTrack](const QPoint& pos) {
                emit addTrackMenuRequested(addTrack->mapToGlobal(pos));
            });
    tz->addWidget(addTrack);

    // The two playback switches live here, next to the add-track button and
    // directly above the header column's own M and S chips — *not* over the
    // arrangement. They used to stand at the timeline's near corner, which is
    // exactly where the context island travels: a switch you cannot reach
    // because a plate is parked on it is not a switch.
    m_restart = new ui::IconButton(
        icons::Glyph::Restart,
        tr("Restart: Space starts from the anchored spot"), m_trackZone);
    m_restart->setCheckable(true);
    connect(m_restart, &QAbstractButton::toggled, this,
            &ToolPanel::restartModeToggled);

    m_playFromClip = new ui::IconButton(
        icons::Glyph::ClipLoop,
        tr("From clip: Space plays the selected clip over and over"), m_trackZone);
    m_playFromClip->setCheckable(true);
    connect(m_playFromClip, &QAbstractButton::toggled, this,
            &ToolPanel::playFromClipToggled);

    m_createAutomation = new ui::IconButton(
        icons::Glyph::AutomationCreate,
        tr("Create automation: hold Alt/Option, or click to latch; then "
           "double-click an automatable control"),
        m_trackZone);
    m_createAutomation->setObjectName(QStringLiteral("AutomationCreateMode"));
    m_createAutomation->setCheckable(true);
    m_createAutomation->setAccessibleName(tr("Create automation clips"));
    connect(m_createAutomation, &QAbstractButton::toggled, this,
            &ToolPanel::automationCreationModeToggled);

    m_showAutomation = new ui::IconButton(
        icons::Glyph::Automation,
        tr("Show or hide automation lanes for all tracks"), m_trackZone);
    m_showAutomation->setCheckable(true);
    m_showAutomation->setAccessibleName(
        tr("Show or hide automation lanes for all tracks"));
    connect(m_showAutomation, &QAbstractButton::toggled, this,
            &ToolPanel::automationVisibilityToggled);

    m_followPlayhead = new ui::IconButton(
        icons::Glyph::SkipEnd,
        tr("Follow the playhead — P centres it; press again to navigate freely"),
        m_trackZone);
    m_followPlayhead->setObjectName(QStringLiteral("FollowPlayheadButton"));
    m_followPlayhead->setCheckable(true);
    m_followPlayhead->setAccessibleName(tr("Follow the playhead"));
    connect(m_followPlayhead, &QAbstractButton::toggled, this,
            &ToolPanel::followPlayheadToggled);

    tz->addWidget(m_restart);
    tz->addWidget(m_playFromClip);
    tz->addWidget(m_createAutomation);
    tz->addWidget(m_showAutomation);
    tz->addWidget(m_followPlayhead);
    tz->addStretch(1);
    m_trackZone->setFixedWidth(ui::kTrackHeaderWidth);
    row->addWidget(m_trackZone);

    row->addWidget(ui::separatorLine(Qt::Vertical, 18, this));

    // Timeline zone: the context island travels through its centre. The far
    // edge holds a display-only vertical zoom for audio waveforms.
    auto* timelineZone = new QWidget(this);
    auto* timelineLayout = new QHBoxLayout(timelineZone);
    timelineLayout->setContentsMargins(2, 0, 2, 0);
    timelineLayout->setSpacing(2);
    timelineLayout->addStretch(1);
    m_waveformScale = new WaveformScaleButton(
        [this](double scale) { emit waveformScaleChanged(scale); }, timelineZone);
    timelineLayout->addWidget(m_waveformScale);
    row->addWidget(timelineZone, 1);

    // Past the stretch, so it sits over the assistant column at the far right.
    moveAiZoneLast();
    setAiVisible(false);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ToolPanel::applyTheme);
    applyTheme();
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

void ToolPanel::paintEvent(QPaintEvent*) {
    // The header is a plate above this strip, not a band printed on the same
    // sheet. A short cast shadow along the top edge is what makes the workspace
    // read as sitting *under* the transport: without it the two fuse into one
    // field of the same grey and the window loses its top storey.
    QPainter p(this);
    const Theme& t = th();
    constexpr int kDepth = 10;
    QColor ink = t.dark ? QColor(0, 0, 0)
                        : mixColors(t.surface, QColor(10, 14, 22), 0.86);
    QLinearGradient cast(0, 0, 0, kDepth);
    ink.setAlpha(t.dark ? 82 : 66);
    cast.setColorAt(0.0, ink);
    ink.setAlpha(t.dark ? 24 : 20);
    cast.setColorAt(0.45, ink);
    ink.setAlpha(0);
    cast.setColorAt(1.0, ink);
    p.fillRect(QRect(0, 0, width(), kDepth), cast);
}

void ToolPanel::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    emit resized();
}

void ToolPanel::applyTheme() {
    const Theme& t = th();
    if (m_followPlayhead) {
        m_followPlayhead->setActiveColor(t.cursor);
        m_followPlayhead->setIcon(
            icons::svgIcon(QStringLiteral("signpost.svg"), t.textPrimary, 18));
    }
    setStyleSheet(QString(
        "#ToolPanel { background: %1; border-bottom: 1px solid %2; }")
                      .arg(t.headerBackground.name(), t.sectionDivider().name()));
}
