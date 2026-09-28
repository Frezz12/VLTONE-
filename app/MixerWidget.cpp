#include "UiPerformance.hpp"
#include "UiFrameClock.hpp"
#include <QElapsedTimer>
#include "MixerWidget.hpp"
#include "ChannelViewState.hpp"
#include <QApplication>
#include <QEvent>
#include <QScopedValueRollback>

#include <algorithm>
#include <cstdio>
#include <functional>
#include "ChannelStrip.hpp"
#include "LoudnessDisplay.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include "SettingsWindow.hpp"
#include "ShortcutManager.hpp"

#include "EngineController.hpp"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QMenu>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
constexpr int kStripGap = 2;
constexpr int kVerticalInset = 6;
int channelStride(int width) { return width + kStripGap; }
int channelsWidth(int count, int width) {
    return count > 0 ? count * channelStride(width) - kStripGap : 0;
}

// The strip keeps its full width while its dock clips the part slid past the
// right edge. Global pointer coordinates keep the moving handle under the hand.
class MasterFoldHandle final : public QAbstractButton {
public:
    explicit MasterFoldHandle(QWidget* parent) : QAbstractButton(parent) {
        setObjectName("MasterFoldHandle"); setFixedWidth(16);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setCursor(Qt::SplitHCursor); setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                qOverload<>(&QWidget::update));
    }
    std::function<void()> started, cancelled;
    std::function<void(int)> moved, finished;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const auto& t = th();
        const bool collapsed = property("collapsed").toBool();
        const bool active = underMouse() || hasFocus() || m_dragging;
        const QColor color = collapsed || active ? t.accent : t.textSecondary;
        const qreal x = width() / 2.;
        p.setPen(QPen(t.separator(), 1));
        p.drawLine(QPointF(x, 8), QPointF(x, height() - 8));
        const QRectF grip(2.5, height() / 2. - 26, width() - 5, 52);
        p.setBrush(active ? t.panelTop() : t.panelBottom());
        p.setPen(QPen(hasFocus() ? t.accent : t.separator(), 1));
        p.drawRoundedRect(grip, 5, 5);
        p.setPen(QPen(mixColors(t.panelBottom(), color, collapsed || active ? .9 : .55), 1.5,
                      Qt::SolidLine, Qt::RoundCap));
        for (qreal offset : {-2.0, 2.0})
            p.drawLine(QPointF(x + offset, height() / 2. - 10),
                       QPointF(x + offset, height() / 2. + 10));
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_pressed = true; m_dragging = false; m_cancelled = false;
            m_origin = e->globalPosition().x();
            if (started) started();
        }
        QAbstractButton::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_pressed) { QAbstractButton::mouseMoveEvent(e); return; }
        if (m_cancelled) return;
        const int delta = qRound(e->globalPosition().x() - m_origin);
        if (!m_dragging && std::abs(delta) < QApplication::startDragDistance()) return;
        m_dragging = true; setDown(false);
        if (moved) moved(delta);
        update(); e->accept();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { QAbstractButton::mouseReleaseEvent(e); return; }
        m_pressed = false;
        if (m_dragging && !m_cancelled && finished)
            finished(qRound(e->globalPosition().x() - m_origin));
        else if (cancelled) cancelled();
        if (m_dragging || m_cancelled) setDown(false);
        m_dragging = false; m_cancelled = false;
        QAbstractButton::mouseReleaseEvent(e); update();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape && m_pressed) {
            cancel(); e->accept(); return;
        }
        QAbstractButton::keyPressEvent(e);
    }
    bool event(QEvent* e) override {
        if (m_pressed && (e->type() == QEvent::UngrabMouse || e->type() == QEvent::Hide ||
                          e->type() == QEvent::WindowDeactivate)) cancel();
        return QAbstractButton::event(e);
    }
private:
    void cancel() {
        if (m_cancelled) return;
        m_cancelled = true; m_dragging = false; setDown(false);
        if (cancelled) cancelled();
        update();
    }
    bool m_pressed = false, m_dragging = false, m_cancelled = false;
    qreal m_origin = 0;
};
} // namespace

MixerWidget::MixerWidget(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    m_channelWidth = ui::MixerPreferences::instance().channelWidth();
    m_masterVisible = ui::MixerPreferences::instance().masterVisible();
    setObjectName("MixerPanel");
    setAttribute(Qt::WA_StyledBackground, true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // A raised command bar above the recessed channel bay.
    m_header = new QWidget(this);
    m_header->setObjectName("MixerHeader");
    m_header->setFixedHeight(30);
    auto* head = new QHBoxLayout(m_header);
    head->setContentsMargins(12, 0, 8, 0);
    head->setSpacing(8);

    m_headerAccent = new QWidget(m_header);
    m_headerAccent->setObjectName(QStringLiteral("MixerHeaderAccent"));
    m_headerAccent->setFixedSize(2, 14);
    m_headerGlyph = new QLabel(m_header);
    m_headerGlyph->setFixedSize(16, 16);
    auto* title = new QLabel(tr("MIXER"), m_header);
    title->setObjectName("MixerTitle");
    m_headerCount = new QLabel(m_header);
    m_headerCount->setObjectName(QStringLiteral("MixerHeaderCount"));
    m_headerCount->setFixedHeight(20);
    m_headerCount->setAlignment(Qt::AlignCenter);
    auto* settings = new ui::IconButton(icons::Glyph::Gear,
                                        tr("Mixer settings"), m_header);
    settings->setObjectName(QStringLiteral("MixerSettingsButton"));
    settings->setButtonSize(24, 24);
    settings->setFocusPolicy(Qt::StrongFocus);
    settings->setAccessibleName(tr("Mixer settings"));
    connect(settings, &QAbstractButton::clicked, this,
            &MixerWidget::settingsRequested);
    m_masterToggle = new QToolButton(m_header);
    m_masterToggle->setObjectName("MasterVisibilityButton");
    m_masterToggle->setText(tr("Master"));
    m_masterToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_masterToggle->setCheckable(true); m_masterToggle->setFixedHeight(24);
    m_masterToggle->setMinimumWidth(72); m_masterToggle->setIconSize(QSize(12, 12));
    m_masterToggle->setFocusPolicy(Qt::StrongFocus);
    m_masterToggle->setAttribute(Qt::WA_MacShowFocusRect, false);
    connect(m_masterToggle, &QAbstractButton::clicked, this, [this] {
        ui::MixerPreferences::instance().setMasterVisible(!m_masterVisible);
    });

    head->addWidget(m_headerAccent);
    head->addWidget(m_headerGlyph);
    head->addWidget(title);
    head->addWidget(m_headerCount);
    head->addStretch(1);
    head->addWidget(m_masterToggle);
    head->addWidget(settings);
    outer->addWidget(m_header);

    // ── Strips: scrolling channels on the left, master pinned right ──
    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("MixerBody"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    auto* bodyRow = new QHBoxLayout(body);
    bodyRow->setContentsMargins(8, kVerticalInset, 8, kVerticalInset);
    bodyRow->setSpacing(0);

    m_stripsHost = new QWidget(body);
    m_stripsHost->setObjectName(QStringLiteral("MixerStripHost"));
    m_stripsLayout = new QHBoxLayout(m_stripsHost);
    m_stripsLayout->setContentsMargins(0, 0, 0, 0);
    m_stripsLayout->setSpacing(0);
    m_stripsLayout->addStretch(1);

    m_scroll = new QScrollArea(body);
    m_scroll->setObjectName(QStringLiteral("MixerChannelsScroll"));
    m_scroll->viewport()->setObjectName(QStringLiteral("MixerChannelsViewport"));
    m_scroll->horizontalScrollBar()->setObjectName(QStringLiteral("MixerNavigationBar"));
    m_scroll->verticalScrollBar()->setObjectName(QStringLiteral("MixerNavigationBar"));
    m_scroll->setWidget(m_stripsHost);
    // The viewport supplies the background. Keeping the page transparent also
    // avoids raster backing-store blits under the retained GPU scene.
    m_stripsHost->setAutoFillBackground(false);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_masterHost = new QWidget(body);
    m_masterHost->setObjectName(QStringLiteral("MixerMasterHost"));
    auto* masterRow = new QHBoxLayout(m_masterHost);
    masterRow->setContentsMargins(0, 0, 0, 0);
    masterRow->setSpacing(0);

    // Master has its own natural rack height and scroll range. A long chain
    // elsewhere must never stretch its slots or push its fader out of view.
    auto* masterScroll = new QScrollArea(body);
    m_masterScroll = masterScroll;
    masterScroll->setObjectName(QStringLiteral("MixerMasterScroll"));
    masterScroll->verticalScrollBar()->setObjectName(QStringLiteral("MixerNavigationBar"));
    masterScroll->setWidget(m_masterHost);
    m_masterHost->setAutoFillBackground(false);
    masterScroll->setWidgetResizable(true);
    masterScroll->setFrameShape(QFrame::NoFrame);
    masterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    masterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_scroll->viewport()->installEventFilter(this);
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this] { syncVisibleStrips(); });
    bodyRow->addWidget(m_scroll, 1);
    auto* handle = new MasterFoldHandle(body);
    m_masterHandle = handle;
    m_masterDock = new QWidget(body);
    m_masterDock->setObjectName("MasterDock");
    m_masterDock->setMinimumWidth(0);
    m_masterDock->installEventFilter(this);
    m_masterColumn = new QWidget(m_masterDock);
    auto* masterColumn = new QVBoxLayout(m_masterColumn);
    masterColumn->setContentsMargins(0, 0, 0, 0);
    masterColumn->setSpacing(0);
    masterColumn->addWidget(masterScroll, 1);
    m_masterBottomGap = new QWidget(body);
    m_masterBottomGap->setFixedHeight(0);
    masterColumn->addWidget(m_masterBottomGap);
    bodyRow->addWidget(handle);
    bodyRow->addWidget(m_masterDock);
    outer->addWidget(body, 1);

    handle->started = [this] {
        m_masterDragging = true; m_masterDragStartWidth = m_masterDock->width();
    };
    handle->moved = [this](int delta) { setMasterRevealWidth(m_masterDragStartWidth - delta); };
    handle->finished = [this](int delta) {
        const bool visible = m_masterDragStartWidth - delta >= masterExpandedWidth() / 2;
        m_masterDragging = false;
        ui::MixerPreferences::instance().setMasterVisible(visible);
        setMasterVisible(visible); // also settle a gesture that retained its state
    };
    handle->cancelled = [this] {
        m_masterDragging = false; updateMasterGeometry();
    };
    connect(handle, &QAbstractButton::clicked, this, [this] {
        ui::MixerPreferences::instance().setMasterVisible(!m_masterVisible);
    });
    connect(masterScroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { updateMasterGeometry(); });

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &MixerWidget::applyTheme);
    connect(&ui::MixerPreferences::instance(), &ui::MixerPreferences::channelWidthChanged,
            this, &MixerWidget::applyChannelWidth);
    connect(&ui::MixerPreferences::instance(), &ui::MixerPreferences::masterVisibleChanged,
            this, &MixerWidget::setMasterVisible);
    m_meterTimer = new ui::FrameTimer(this);
    connect(m_meterTimer, &ui::FrameTimer::timeout, this, &MixerWidget::refreshMeters);
    m_materializeTimer = new ui::FrameTimer(this);
    connect(m_materializeTimer, &ui::FrameTimer::timeout, this, &MixerWidget::syncVisibleStrips);
    applyTheme();
    rebuild();
    updateMasterGeometry();
}

void MixerWidget::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#MixerPanel { background: %BOTTOM%; border: none; }
#MixerHeader { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                          stop:0 %TOP%, stop:1 %BOTTOM%);
               border: 1px solid %SECTION%; border-top-color: %LIGHT%;
               border-top-left-radius: 8px; border-top-right-radius: 8px; }
#MixerTitle { color: %TEXT%; font-size: 11px; font-weight: 600;
              letter-spacing: 0.7px; }
#MixerHeaderCount { color: %TEXT2%; font-size: 10px; background: %WELL%;
                    border: 1px solid %SEP%; border-radius: 6px; padding: 0 7px; }
#MixerBody { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 %BAY_TOP%, stop:1 %WELL%);
             border: 1px solid %SECTION%; border-top: none; }
#MixerChannelsScroll, #MixerMasterScroll, #MixerChannelsViewport,
#MixerStripHost, #MixerMasterHost { background: transparent; border: none; }
#MasterDock { background: %BOTTOM%; border-radius: 8px; }
#MasterVisibilityButton { color: %TEXT2%; background: %WELL%; border: 1px solid %SEP%;
                         border-radius: 6px; padding: 2px 8px; font-size: 11px; }
#MasterVisibilityButton:checked { color: %TEXT%; background: %SELECT%; border-color: %SELECT_EDGE%; }
#MasterVisibilityButton:hover { background: %HOVER%; color: %TEXT%; }
#MasterVisibilityButton:pressed { background: %WELL%; }
#MasterVisibilityButton:focus { border-color: %ACCENT%; }
QScrollBar#MixerNavigationBar { background: %WELL%; }
QScrollBar#MixerNavigationBar:horizontal {
    height: 12px; margin: 2px 0 0 0;
}
QScrollBar#MixerNavigationBar:vertical {
    width: 12px; margin: 0 0 0 2px;
}
QScrollBar#MixerNavigationBar::handle {
    background: %SCROLL%; border: 1px solid %SCROLL_EDGE%; border-radius: 4px;
    min-width: 28px; min-height: 28px;
}
QScrollBar#MixerNavigationBar::handle:hover {
    background: %SCROLL_HOVER%;
}
QScrollBar#MixerNavigationBar::handle:pressed {
    background: %ACCENT%;
}
)")
        .replace("%TOP%", t.panelTop().name())
        .replace("%BOTTOM%", t.panelBottom().name())
        .replace("%LIGHT%", t.edgeLight(t.panelTop()).name())
        .replace("%BAY_TOP%", mixColors(t.well(), t.surface, .35).name())
        .replace("%WELL%", t.well().name())
        .replace("%SELECT%", mixColors(t.panelBottom(), t.accent, .17).name())
        .replace("%SELECT_EDGE%", mixColors(t.separator(), t.accent, .42).name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%SCROLL%", mixColors(t.well(), t.textPrimary, .25).name())
        .replace("%SCROLL_EDGE%", mixColors(t.well(), t.textPrimary, .32).name())
        .replace("%SCROLL_HOVER%", mixColors(t.well(), t.textPrimary, .40).name())
        .replace("%SEP%", t.separator().name())
        .replace("%SECTION%", t.sectionDivider().name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%HOVER%", t.controlTop().name())
        .replace("%TEXT%", t.textPrimary.name()));
    if (m_headerAccent)
        m_headerAccent->setStyleSheet(
            QStringLiteral("background: %1; border-radius: 1px;").arg(t.accent.name()));
    if (m_headerGlyph)
        m_headerGlyph->setPixmap(
            icons::icon(icons::Glyph::Mixer, t.accent, 15).pixmap(15, 15));
    updateMasterToggle();
    update();
}

int MixerWidget::masterExpandedWidth() const {
    const auto* bar = m_masterScroll->verticalScrollBar();
    return m_channelWidth + (bar->maximum() > 0 ? bar->sizeHint().width() : 0);
}

void MixerWidget::updateMasterToggle() {
    const auto action = m_masterVisible ? tr("Hide master channel") : tr("Show master channel");
    const QSignalBlocker block(m_masterToggle);
    m_masterToggle->setChecked(m_masterVisible);
    m_masterToggle->setToolTip(action); m_masterToggle->setAccessibleName(action);
    m_masterToggle->setIcon(icons::icon(m_masterVisible ? icons::Glyph::ArrowRight : icons::Glyph::ArrowLeft,
        m_masterVisible ? th().accent : th().textSecondary, 12));
    if (m_masterHandle) {
        m_masterHandle->setAccessibleName(action);
        m_masterHandle->setToolTip(m_masterVisible
            ? tr("Drag right to hide Master. Click to toggle.")
            : tr("Drag left to show Master. Click to toggle."));
        m_masterHandle->setProperty("collapsed", !m_masterVisible);
        m_masterHandle->update();
    }
}

void MixerWidget::setMasterRevealWidth(int width) {
    if (!m_masterColumn) return;
    width = std::clamp(width, 0, masterExpandedWidth());
    m_masterDock->setFixedWidth(width);
    m_masterColumn->setVisible(width > 0);
    m_masterColumn->setGeometry(0, 0, masterExpandedWidth(), m_masterDock->height());
    // Apply the moving boundary in this input event, including on direction
    // reversal. The strip itself is neither resized nor reconstructed.
    if (m_masterDock->parentWidget()->layout()) m_masterDock->parentWidget()->layout()->activate();
}

void MixerWidget::updateMasterGeometry() {
    if (!m_masterColumn) return;
    m_masterColumn->setGeometry(0, 0, masterExpandedWidth(), m_masterDock->height());
    if (!m_masterDragging) setMasterRevealWidth(m_masterVisible ? masterExpandedWidth() : 0);
}

void MixerWidget::setMasterVisible(bool visible) {
    m_masterVisible = visible;
    if (!visible && m_masterColumn && m_masterColumn->isAncestorOf(QApplication::focusWidget()))
        m_masterToggle->setFocus(Qt::OtherFocusReason);
    updateMasterToggle(); updateMasterGeometry();
    if (visible) refreshMeters();
}

double MixerWidget::faderGainForTest(const QString& trackId) const {
    for (ChannelStrip* strip : m_strips) {
        if (strip->trackId() == trackId) return strip->faderGainForTest();
    }
    return -1.0;
}

void MixerWidget::syncFromModel(const QStringList& trackIds) {
    QSet<QString> affected(trackIds.begin(), trackIds.end());
    for (const auto& id : trackIds) {
        if (const auto* track = m_controller->project().findTrack(id.toStdString());
            track && !track->outputBusId.empty())
            affected.insert(QString::fromStdString(track->outputBusId));
    }
    for (ChannelStrip* strip : m_strips)
        if (trackIds.isEmpty() || affected.contains(strip->trackId())) strip->syncFromModel();
}

void MixerWidget::refreshAutomationValues() {
    if (!isVisible()) return;
    for (ChannelStrip* strip : m_strips) {
        if (stripIsVisible(strip)) strip->refreshAutomationValues();
    }
}

bool MixerWidget::checkCollaborationPresenceForTest(QString* error) {
    auto& preferences = ui::MixerPreferences::instance();
    const bool wasVisible = preferences.masterVisible();
    const auto restoreMaster = qScopeGuard([&] { preferences.setMasterVisible(wasVisible); });
    preferences.setMasterVisible(true);
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    daw::EngineController controller;
    controller.initialize(48000.0, 512, false);
    const QString trackId =
        QString::fromStdString(controller.addTrack(daw::TrackKind::Audio, "A"));
    (void)controller.addTrack(daw::TrackKind::Audio, "B");
    if (trackId.isEmpty())
        return fail(QStringLiteral("mixer presence fixture has no tracks"));

    MixerWidget mixer(&controller);
    auto* settings = mixer.findChild<QAbstractButton*>(
        QStringLiteral("MixerSettingsButton"));
    int settingsRequests = 0;
    QObject::connect(&mixer, &MixerWidget::settingsRequested,
                     [&settingsRequests] { ++settingsRequests; });
    if (!settings)
        return fail(QStringLiteral("mixer header has no settings action"));
    settings->click();
    if (settingsRequests != 1)
        return fail(QStringLiteral("mixer settings action is not connected"));
    // The strips sit inside a scroll area, so their geometry only exists after
    // a real layout pass. WA_DontShowOnScreen gives one without a window.
    mixer.setAttribute(Qt::WA_DontShowOnScreen, true);
    mixer.resize(900, 520);
    mixer.rebuild();
    mixer.show();
    QCoreApplication::sendPostedEvents();
    if (mixer.layout()) mixer.layout()->activate();
    QRect strip = mixer.stripRectForTrackForTest(trackId);
    if (strip.isNull() || strip.height() <= 0)
        return fail(QStringLiteral("mixer presence fixture has no strips"));

    const QPointF source(strip.center().x(),
                         strip.top() + strip.height() * 0.75);
    const collab::SemanticPoint semantic = mixer.collaborationPresenceAt(source);
    if (semantic.trackId != trackId ||
        semantic.targetId != QLatin1String("strip") ||
        std::abs(semantic.laneFraction - 0.75) > 1e-6) {
        return fail(QStringLiteral("mixer presence lost its channel context"));
    }

    // A console sized differently must place the same packet on the same
    // channel, not at the same pixel.
    mixer.resize(700, 760);
    QCoreApplication::sendPostedEvents();
    if (mixer.layout()) mixer.layout()->activate();
    strip = mixer.stripRectForTrackForTest(trackId);
    const auto remapped = mixer.collaborationPositionFor(semantic);
    if (strip.isNull() || !remapped ||
        std::abs(remapped->y() - (strip.top() + strip.height() * 0.75)) > 1e-6) {
        return fail(QStringLiteral("mixer presence did not follow the layout"));
    }

    collab::SemanticPoint absent = semantic;
    absent.trackId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    if (mixer.collaborationPositionFor(absent))
        return fail(QStringLiteral("mixer presence mapped an unknown channel"));

    // The master strip carries no track id, because the wire requires trackId
    // to be a UUID; it must still be addressable.
    collab::SemanticPoint master;
    master.surface = {collab::SurfaceKind::Mixer, QStringLiteral("main"), {}};
    master.targetId = QStringLiteral("master_strip");
    master.laneFraction = 0.5;
    if (!mixer.collaborationPositionFor(master))
        return fail(QStringLiteral("mixer presence lost the master strip"));
    return true;
}

bool MixerWidget::checkLayoutForTest() {
    auto& preferences = ui::MixerPreferences::instance();
    const int originalWidth = preferences.channelWidth();
    const QVariant originalSetting = QSettings().value(ui::MixerPreferences::kWidthSetting);
    const bool originalMasterVisible = preferences.masterVisible();
    const QVariant originalMasterSetting = QSettings().value(ui::MixerPreferences::kMasterVisibleSetting);
    const auto restore = qScopeGuard([&] {
        preferences.setChannelWidth(originalWidth);
        preferences.setMasterVisible(originalMasterVisible);
        if (originalSetting.isValid()) QSettings().setValue(ui::MixerPreferences::kWidthSetting, originalSetting);
        else QSettings().remove(ui::MixerPreferences::kWidthSetting);
        if (originalMasterSetting.isValid()) QSettings().setValue(ui::MixerPreferences::kMasterVisibleSetting, originalMasterSetting);
        else QSettings().remove(ui::MixerPreferences::kMasterVisibleSetting);
    });
    preferences.setChannelWidth(ui::MixerPreferences::kDefaultWidth);
    preferences.setMasterVisible(true);
    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false)) return false;
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear();
    project.invalidateTrackIndex();
    for (int i = 0; i < 22; ++i) {
        daw::TrackModel track;
        track.id = "layout-" + std::to_string(i);
        track.name = "Channel " + std::to_string(i + 1);
        track.kind = i % 3 == 0 ? daw::TrackKind::Audio :
            i % 3 == 1 ? daw::TrackKind::Instrument : daw::TrackKind::Bus;
        for (int j = 0; j < (i == 21 ? 10 : i % 4); ++j) {
            daw::InsertModel insert;
            insert.id = track.id + "-fx-" + std::to_string(j);
            insert.name = "Effect " + std::to_string(j + 1);
            track.inserts.push_back(std::move(insert));
        }
        for (int j = 0; j < i % 5; ++j) {
            daw::SendModel send;
            send.id = track.id + "-send-" + std::to_string(j);
            send.destinationTrackId = "layout-2";
            track.sends.push_back(std::move(send));
        }
        project.tracks.push_back(std::move(track));
    }
    MixerWidget mixer(&controller);
    mixer.setAttribute(Qt::WA_DontShowOnScreen, true);
    mixer.resize(800, 900);
    mixer.show();
    int expectedSlots = 11;
    const QString shots = qEnvironmentVariable("DAW_MIXER_SLOTS_SCREENSHOTS");
    const auto settle = [&] {
        for (int i = 0; i < 24; ++i) {
            mixer.syncVisibleStrips();
            QApplication::processEvents();
        }
    };
    const auto aligned = [&](const char* stage) {
        bool ok = mixer.m_strips.size() > 2;
        std::array<std::vector<int>, 2> slotBaselines;
        for (std::size_t i = 0; i < mixer.m_slots.size(); ++i) {
            if (const auto* strip = mixer.m_slots[i])
                ok &= strip->x() == int(i) * channelStride(mixer.channelWidth());
        }
        for (const char* name : {"ChannelProcessingRow", "ChannelSendsRow", "ChannelLevelWell"}) {
            std::optional<int> baseline;
            for (auto* strip : mixer.m_strips) {
                if (strip->isMaster()) continue;
                QWidget* row = strip->findChild<QWidget*>(QString::fromLatin1(name));
                if (!row) { ok = false; continue; }
                if (qstrcmp(name, "ChannelLevelWell") != 0) {
                    row = row->findChild<QWidget*>(QStringLiteral("SlotWell"));
                    if (!row) continue; // The master has no send controls.
                }
                const int top = row->mapTo(&mixer, QPoint{}).y();
                if (baseline && *baseline != top) {
                    std::fprintf(stderr, "FAIL layout: %s at %s: %d != %d\n",
                                 name, stage, top, *baseline);
                    ok = false;
                }
                baseline = top;
            }
        }
        for (auto* strip : mixer.m_strips) {
            for (int section = 0; section < 2; ++section) {
                if (strip->isMaster()) continue;
                auto* rack = strip->findChild<QWidget*>(section == 0 ? "ChannelInsertsRow" : "ChannelSendsRow");
                if (!rack) continue;
                const auto buttons = rack->findChildren<QToolButton*>("SlotButton");
                auto& baselines = slotBaselines[section];
                for (int i = 0; i < buttons.size(); ++i) {
                    const int y = buttons[i]->mapTo(&mixer, QPoint()).y();
                    if (i >= int(baselines.size())) baselines.push_back(y);
                    else ok &= baselines[i] == y;
                }
            }
            ok &= strip->width() == mixer.channelWidth();
            auto* inserts = strip->findChild<QWidget*>(QStringLiteral("ChannelInsertsRow"));
            const auto slotButtons = inserts ? inserts->findChildren<QToolButton*>(QStringLiteral("SlotButton"))
                                       : QList<QToolButton*>{};
            const auto* models = controller.channelInserts(strip->isMaster()
                ? daw::EngineController::kMasterChannelId : strip->trackId().toStdString());
            const int filledCount = models ? int(models->size()) : 0;
            const int stripSlots = strip->isMaster() ? std::max(2, int(project.masterInserts.size()) + 1) : expectedSlots;
            ok &= strip->insertSlotCount() == stripSlots && slotButtons.size() == filledCount;
            for (int i = 0; i < slotButtons.size(); ++i) {
                ok &= slotButtons[i]->property("insertSlotIndex").toInt() == i &&
                    slotButtons[i]->property("insertSlotFilled").toBool();
            }
            const auto addAreas = inserts ? inserts->findChildren<QToolButton*>(QStringLiteral("InsertAddArea"))
                                         : QList<QToolButton*>{};
            ok &= addAreas.size() == 1;
            if (addAreas.size() == 1) {
                auto* add = addAreas.front();
                ok &= add->text().isEmpty() && !add->icon().isNull() && add->menu() && add->isEnabled() &&
                    add->property("insertSlotIndex").toInt() == filledCount &&
                    add->property("insertSlotSpan").toInt() == stripSlots - filledCount;
                for (auto* slot : slotButtons)
                    ok &= slot->mapTo(inserts, QPoint(0, slot->height())).y() <=
                        add->mapTo(inserts, QPoint()).y();
            }
            auto* processing = strip->findChild<QWidget*>(QStringLiteral("ChannelProcessingRow"));
            auto* first = strip->findChild<QWidget*>(QStringLiteral("ChannelInsertsRow"));
            ok &= processing && first && first->mapTo(strip, QPoint{}).y() ==
                processing->mapTo(strip, QPoint{}).y();
            auto* instrumentTier = strip->findChild<QWidget*>(QStringLiteral("ChannelInstrumentTier"));
            ok &= instrumentTier && first && first->mapTo(strip, QPoint{}).y() >=
                instrumentTier->mapTo(strip, QPoint{}).y() + instrumentTier->height();
            if (strip->isMaster()) {
                auto* loudness = strip->findChild<QAbstractButton*>(QStringLiteral("MasterLoudnessDisplay"));
                auto* sends = strip->findChild<QWidget*>(QStringLiteral("ChannelSendsRow"));
                ok &= loudness && loudness->isVisibleTo(strip) && sends && sends->isHidden();
                if (loudness) ok &= loudness->rect().width() >= 56;
            }
            ok &= strip->findChild<QWidget*>(QStringLiteral("ColorSwatch")) &&
                !strip->findChild<QLabel*>(QStringLiteral("NamePlate"));
            auto* well = strip->findChild<QWidget*>(QStringLiteral("ChannelLevelWell"));
            const QList<QWidget*> controls{strip->findChild<ui::FaderWidget*>(),
                strip->findChild<ui::LevelMeter*>(), strip->findChild<ui::PanKnob*>(),
                strip->findChild<QWidget*>(QStringLiteral("ChannelGainReadout")),
                strip->findChild<QWidget*>(QStringLiteral("ChannelPeakReadout"))};
            for (auto* control : controls)
                ok &= well && control && well->rect().contains(
                    QRect(control->mapTo(well, QPoint{}), control->size()));
            for (auto* button : strip->findChildren<QAbstractButton*>()) {
                if (!button->isVisibleTo(strip)) continue;
                ok &= strip->rect().contains(QRect(button->mapTo(strip, QPoint{}), button->size()));
            }
        }
        std::fprintf(stderr, "%s layout: %s\n", ok ? "PASS" : "FAIL", stage);
        return ok;
    };
    settle();
    if (!aligned("audio, instrument, bus and master; different FX/send counts")) return false;
    if (mixer.m_slots.back() || project.tracks.front().inserts.size() != 0 ||
        project.tracks[1].inserts.size() != 1) return false;
    if (!shots.isEmpty() && !mixer.grab().save(shots + "-short-chains.png")) return false;
    ShortcutManager shortcuts;
    SettingsWindow settings(&controller, &shortcuts);
    auto* slider = settings.findChild<QSlider*>(QStringLiteral("MixerChannelWidthSlider"));
    auto* value = settings.findChild<QSpinBox*>(QStringLiteral("MixerChannelWidthValue"));
    auto* reset = settings.findChild<QAbstractButton*>(QStringLiteral("MixerChannelWidthReset"));
    if (!slider || !value || !reset || value->value() != ui::MixerPreferences::kDefaultWidth ||
        slider->value() != ui::MixerPreferences::kDefaultWidth) return false;
    MixerWidget secondMixer(&controller);
    mixer.m_scroll->horizontalScrollBar()->setValue(2 * channelStride(mixer.channelWidth()) + 17);
    settle();
    auto* retainedStrip = mixer.m_slots[3];
    const auto undoDepth = controller.undoDepth();
    for (int width : {83, 99, 100, 140, 180, 75}) {
        const double anchor = double(mixer.m_scroll->horizontalScrollBar()->value()) / channelStride(mixer.channelWidth());
        value->setValue(width);
        settle();
        if (!aligned("live width change, including compact-layout boundaries") ||
            secondMixer.channelWidth() != width || slider->value() != width ||
            QSettings().value(ui::MixerPreferences::kWidthSetting).toInt() != width ||
            mixer.m_slots[3] != retainedStrip || controller.undoDepth() != undoDepth ||
            std::abs(mixer.m_scroll->horizontalScrollBar()->value() - qRound(anchor * channelStride(width))) > 1)
            return false;
    }
    slider->setValue(140);
    if (value->value() != 140) return false;
    reset->click();
    settle();
    if (preferences.channelWidth() != ui::MixerPreferences::kDefaultWidth ||
        value->value() != ui::MixerPreferences::kDefaultWidth ||
        secondMixer.channelWidth() != ui::MixerPreferences::kDefaultWidth || reset->isEnabled()) return false;
    // The narrow send menu invokes the original controls: the live amount
    // knob remains in the row and no action disappears behind its neighbour.
    preferences.setChannelWidth(ui::MixerPreferences::kMinimumWidth);
    settle();
    auto* sendStrip = mixer.m_slots[3];
    auto* sends = sendStrip->findChild<QWidget*>(QStringLiteral("ChannelSendsRow"));
    auto* overflow = sends->findChild<QToolButton*>(QStringLiteral("SlotOverflow"));
    if (!overflow || !overflow->menu()) return false;
    QMetaObject::invokeMethod(overflow->menu(), "aboutToShow", Qt::DirectConnection);
    const auto actions = overflow->menu()->actions();
    if (actions.size() != 3) return false;
    const auto id = sendStrip->trackId().toStdString();
    const bool preFader = project.findTrack(id)->sends.front().preFader;
    actions[1]->trigger();
    if (project.findTrack(id)->sends.front().preFader == preFader) return false;
    controller.undo();
    settle();
    std::fprintf(stderr, "PASS mixer settings: persistence, reset, live peers, scroll anchor, send menu and Undo\n");
    mixer.resize(520, 420);
    settle();
    if (!aligned("short and narrow console")) return false;
    mixer.m_scroll->horizontalScrollBar()->setValue(mixer.m_scroll->horizontalScrollBar()->maximum());
    settle();
    if (!aligned("virtualised channels with longer chains")) return false;
    if (!shots.isEmpty() && !mixer.grab().save(shots + "-long-chain.png")) return false;
    mixer.m_scroll->verticalScrollBar()->setValue(mixer.m_scroll->verticalScrollBar()->maximum());
    settle();
    if (!aligned("track racks scroll while master stays independent")) return false;
    project.tracks.back().inserts.resize(1);
    project.tracks.back().sends.clear();
    expectedSlots = 4;
    mixer.rebuild();
    settle();
    if (!aligned("rack and empty slots shrink after removing the longest chain")) return false;
    project.masterInserts.clear();
    for (int i = 0; i < 12; ++i) {
        daw::InsertModel insert;
        insert.id = "master-fx-" + std::to_string(i);
        insert.name = "Master Effect " + std::to_string(i + 1);
        project.masterInserts.push_back(std::move(insert));
    }
    mixer.rebuild();
    settle();
    if (!aligned("master's longer chain leaves track slots unchanged")) return false;
    project.masterInserts.clear();
    for (auto& track : project.tracks) track.inserts.clear();
    expectedSlots = 2;
    mixer.rebuild();
    settle();
    return aligned("empty project returns to the compact default") &&
           ChannelStrip::checkGroupInputsForTest() && LoudnessDisplay::checkForTest(shots) &&
           checkMasterDockForTest();
}

QRect MixerWidget::stripRectForTrackForTest(const QString& trackId) const {
    for (const ChannelStrip* strip : m_strips) {
        if (!strip->isMaster() && strip->trackId() == trackId)
            return stripRectFor(strip);
    }
    return {};
}

QRect MixerWidget::stripRectFor(const ChannelStrip* strip) const {
    if (!strip || !strip->isVisible()) return {};
    // Strips live inside a scroll area (and the master inside its own column),
    // so their geometry is only meaningful once mapped into this widget.
    const QRect rect(strip->mapTo(const_cast<MixerWidget*>(this), QPoint{}),
                     strip->size());
    // A strip scrolled out of the console must not be reported: its mapped
    // rectangle would still be a valid location on screen.
    {
        const QWidget* viewport = strip->isMaster() ? m_masterScroll->viewport() : m_scroll->viewport();
        if (!viewport) return {};
        const QRect visible(
            viewport->mapTo(const_cast<MixerWidget*>(this), QPoint{}),
            viewport->size());
        QRect clipped = rect.intersected(visible);
        if (strip->isMaster())
            clipped = clipped.intersected(QRect(m_masterDock->mapTo(const_cast<MixerWidget*>(this), QPoint()),
                                                m_masterDock->size()));
        if (clipped.isEmpty()) return {};
        return clipped;
    }
}

const ChannelStrip* MixerWidget::stripAt(const QPoint& position) const {
    for (const ChannelStrip* strip : m_strips) {
        const QRect rect = stripRectFor(strip);
        if (!rect.isNull() && rect.contains(position)) return strip;
    }
    return nullptr;
}

collab::SemanticPoint MixerWidget::collaborationPresenceAt(
    const QPointF& position) const {
    collab::SemanticPoint point;
    point.surface = {collab::SurfaceKind::Mixer, QStringLiteral("main"), {}};
    point.normalized = collab::normalizedSurfacePoint(position, size());
    const ChannelStrip* strip = stripAt(position.toPoint());
    if (!strip) {
        // The header band, the gap between strips, or the separator.
        point.targetId = QStringLiteral("console_chrome");
        return point;
    }
    const QRect rect = stripRectFor(strip);
    // The master strip has no track id, and the server requires trackId to be a
    // UUID, so it is named through targetId instead.
    point.targetId = strip->isMaster() ? QStringLiteral("master_strip")
                                       : QStringLiteral("strip");
    if (!strip->isMaster()) point.trackId = strip->trackId();
    if (rect.height() > 0) {
        point.laneFraction = std::clamp(
            (position.y() - rect.top()) / double(rect.height()), 0.0, 1.0);
    }
    return point;
}

std::optional<QPointF> MixerWidget::collaborationPositionFor(
    const collab::SemanticPoint& point) const {
    if (point.surface.kind != collab::SurfaceKind::Mixer) return std::nullopt;
    const ChannelStrip* strip = nullptr;
    if (point.targetId == QLatin1String("master_strip")) {
        for (const ChannelStrip* candidate : m_strips) {
            if (candidate->isMaster()) { strip = candidate; break; }
        }
    } else if (!point.trackId.isEmpty()) {
        for (const ChannelStrip* candidate : m_strips) {
            if (candidate->trackId() == point.trackId) {
                strip = candidate;
                break;
            }
        }
    } else {
        // Console chrome: the horizontal fraction is meaningful there because
        // the header and separator span the whole widget.
        if (point.normalized.x() < 0.0 || point.normalized.y() < 0.0)
            return std::nullopt;
        return QPointF(point.normalized.x() * width(),
                       point.normalized.y() * height());
    }
    const QRect rect = stripRectFor(strip);
    // Scrolled out of view, or a track this console does not show. Hiding beats
    // falling back to a fraction of the widget, which lands on another channel.
    if (rect.isNull() || rect.height() <= 0) return std::nullopt;
    const double fraction =
        point.laneFraction >= 0.0 ? point.laneFraction : 0.5;
    return QPointF(rect.center().x(), rect.top() + fraction * rect.height());
}

bool MixerWidget::stripIsVisible(const ChannelStrip* strip) const {
    if (!strip || !strip->isVisible()) return false;
    if (strip->isMaster()) return !stripRectFor(strip).isEmpty();
    const QWidget* viewport = m_scroll ? m_scroll->viewport() : nullptr;
    if (!viewport) return false;
    const QRect stripRect(strip->mapTo(const_cast<QWidget*>(viewport), QPoint{}),
                          strip->size());
    return viewport->rect().intersects(stripRect);
}

void MixerWidget::wireStrip(ChannelStrip* strip) {
    strip->setStripWidth(m_channelWidth);
    ui::perf::sample("mixer.strip.created", 1);
        connect(strip, &ChannelStrip::selectRequested, this,
                &MixerWidget::trackSelected);
        connect(strip, &ChannelStrip::edited, this, [this, strip](bool dirty) {
            emit edited(dirty);
            emit channelEdited(strip->trackId(), dirty);
        });
        connect(strip, &ChannelStrip::trackRoutingEdited, this, [this](const QString& source) {
            emit edited();
            emit channelEdited(source, true);
        });
        connect(strip, &ChannelStrip::editorRequested, this,
                &MixerWidget::pluginEditorRequested);
        connect(strip, &ChannelStrip::patternRequested, this,
                &MixerWidget::openPatternRequested);
        connect(strip, &ChannelStrip::removeRequested, this,
                &MixerWidget::trackRemoved);
        connect(strip, &ChannelStrip::automateControlRequested, this,
                &MixerWidget::automateControlRequested);
        connect(strip, &ChannelStrip::automateMuteRequested, this,
                &MixerWidget::automateMuteRequested);
        connect(strip, &ChannelStrip::automateSendRequested, this,
                &MixerWidget::automateSendRequested);
        connect(strip, &ChannelStrip::structureChanged, this, [this] {
            emit structureChanged();
            rebuild();
        }, Qt::QueuedConnection);
        connect(strip, &ChannelStrip::trackCreated, this,
                &MixerWidget::trackCreated, Qt::QueuedConnection);
}

void MixerWidget::rebuild() {
    ui::perf::Scope timing("rebuild.MixerWidget.ms");
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    m_deferredRackChange = false;
    m_rackHeights.fill(0);
    QHash<QString, ChannelStrip*> previous;
    for (auto* strip : m_strips) previous.insert(strip->trackId(), strip);
    m_strips.clear(); m_slots.clear(); m_channels.clear();
    while (auto* item = m_stripsLayout->takeAt(0)) delete item;
    const auto& project = m_controller->project();
    m_insertSlotCount = 2;
    for (const auto& track : project.tracks)
        if (daw::carriesAudio(track))
            m_insertSlotCount = std::max(m_insertSlotCount, int(track.inserts.size()) + 1);
    for (const auto& track : project.tracks) {
        if (!daw::carriesAudio(track)) continue;
        const auto id = QString::fromStdString(track.id);
        m_channels.push_back(id);
        auto* strip = previous.take(id);
        if (strip && (strip->insertSlotCount() != m_insertSlotCount ||
                      strip->property("channelViewState").toByteArray() !=
                         ui::channelViewState(project, track.id))) {
            if (strip->hasActiveGesture()) m_deferredRackChange = true;
            else { strip->hide(); strip->deleteLater(); strip = nullptr; }
        }
        m_slots.push_back(strip);
        // Explicit spacers keep the same pitch for materialised widgets and
        // virtual placeholders (QBoxLayout spacing skips empty spacer items).
        if (m_slots.size() > 1) m_stripsLayout->addSpacing(kStripGap);
        if (strip) m_stripsLayout->addWidget(strip);
        else m_stripsLayout->addSpacerItem(new QSpacerItem(m_channelWidth, 0, QSizePolicy::Fixed));
    }
    m_stripsLayout->addStretch(1);
    m_stripsHost->setMinimumWidth(channelsWidth(int(m_channels.size()), m_channelWidth));
    auto* master = previous.take(QString());
    const auto state = ui::channelViewState(project, {});
    const int masterSlots = std::max(2, int(project.masterInserts.size()) + 1);
    if (master && (master->insertSlotCount() != masterSlots ||
                   master->property("channelViewState").toByteArray() != state)) {
        if (master->hasActiveGesture()) m_deferredRackChange = true;
        else {
            m_masterHost->layout()->removeWidget(master);
            master->hide(); master->deleteLater(); master = nullptr;
        }
    }
    if (!master) {
        master = new ChannelStrip(m_controller, {}, true, m_masterHost, false,
                                   masterSlots);
        master->setProperty("channelViewState", state);
        wireStrip(master);
        m_masterHost->layout()->addWidget(master);
    }
    m_strips.push_back(master);
    for (auto* strip : previous) { strip->hide(); strip->deleteLater(); }
    m_masterHost->setMinimumHeight(master->naturalHeight());
    m_stripsHost->setMinimumHeight(0);
    m_headerCount->setText(tr("%1 channels").arg(m_channels.size() + 1));
    m_syncingStrips = false;
    syncVisibleStrips();
    syncFromModel();
}

void MixerWidget::applyChannelWidth(int width) {
    if (m_channelWidth == width) return;
    // Preserve the channel at the viewport's left edge, including its partial
    // offset. Width changes never rebuild a strip or interrupt an audio edit.
    const double anchor = double(m_scroll->horizontalScrollBar()->value()) / channelStride(m_channelWidth);
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    m_channelWidth = width;
    for (auto* strip : m_strips) strip->setStripWidth(width);
    for (int i = 0; i < int(m_slots.size()); ++i) {
        if (auto* spacer = m_stripsLayout->itemAt(2 * i)->spacerItem())
            spacer->changeSize(width, 0, QSizePolicy::Fixed);
    }
    m_stripsLayout->invalidate();
    int minimumHeight = 0;
    for (const auto* strip : m_strips)
        if (!strip->isMaster()) minimumHeight = std::max(minimumHeight, strip->naturalHeight());
    m_stripsHost->setMinimumHeight(minimumHeight);
    m_stripsHost->setMinimumWidth(channelsWidth(int(m_slots.size()), width));
    m_stripsHost->resize(std::max(m_scroll->viewport()->width(), channelsWidth(int(m_slots.size()), width)),
                        std::max(m_scroll->viewport()->height(), minimumHeight));
    m_stripsLayout->activate();
    m_scroll->horizontalScrollBar()->setValue(qRound(anchor * channelStride(width)));
    m_syncingStrips = false;
    syncVisibleStrips();
    updateMasterGeometry();
}

bool MixerWidget::eventFilter(QObject* object, QEvent* event) {
    if (object == m_masterDock && event->type() == QEvent::Resize) updateMasterGeometry();
    if (m_scroll && object == m_scroll->viewport() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        if (m_masterBottomGap)
            m_masterBottomGap->setFixedHeight(std::max(0, m_scroll->height() - m_scroll->viewport()->height()));
        syncVisibleStrips();
    }
    return QWidget::eventFilter(object, event);
}

void MixerWidget::syncVisibleStrips() {
    ui::perf::Scope timing("syncVisibleStrips.MixerWidget.ms");
    if (m_syncingStrips || !m_scroll) return;
    if (m_deferredRackChange) {
        for (auto* strip : m_strips) {
            const int expectedSlots = strip->isMaster()
                ? std::max(2, int(m_controller->project().masterInserts.size()) + 1) : m_insertSlotCount;
            if (!strip->hasActiveGesture() && (strip->insertSlotCount() != expectedSlots ||
                strip->property("channelViewState").toByteArray() !=
                ui::channelViewState(m_controller->project(), strip->trackId().toStdString()))) {
                rebuild(); return;
            }
        }
    }
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    const int left = m_scroll->horizontalScrollBar()->value();
    const int margin = std::max(2 * channelStride(m_channelWidth), m_scroll->viewport()->width());
    const auto owns = [](QWidget* strip, QWidget* widget) {
        return strip && widget && (strip == widget || strip->isAncestorOf(widget));
    };
    ChannelStrip* master = nullptr;
    for (auto* strip : m_strips) if (strip->isMaster()) master = strip;
    QElapsedTimer budget;
    budget.start();
    bool pending = m_deferredRackChange;
    // Remove distant controls first; a captured gesture keeps its owning strip.
    for (size_t i = 0; i < m_slots.size(); ++i) {
        auto*& strip = m_slots[i];
        const int x = int(i) * channelStride(m_channelWidth);
        const bool pinned = (strip && strip->hasActiveGesture()) ||
            owns(strip, QApplication::focusWidget());
        const bool wanted = (x + m_channelWidth >= left - margin &&
            x <= left + m_scroll->viewport()->width() + margin) || pinned;
        if (strip && !wanted) {
            delete m_stripsLayout->takeAt(2 * int(i));
            strip->hide(); strip->deleteLater(); strip = nullptr;
            m_stripsLayout->insertSpacerItem(2 * int(i), new QSpacerItem(m_channelWidth, 0, QSizePolicy::Fixed));
        }
    }
    // Visible channels precede overscan. A large console never constructs the
    // whole viewport in one event; the frame clock resumes the remaining work.
    bool created = false;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < m_slots.size(); ++i) {
            auto*& strip = m_slots[i];
            if (strip) continue;
            const int x = int(i) * channelStride(m_channelWidth);
            const bool visible = x + m_channelWidth >= left && x <= left + m_scroll->viewport()->width();
            const bool nearby = x + m_channelWidth >= left - margin &&
                x <= left + m_scroll->viewport()->width() + margin;
            if (!nearby || (pass == 0) != visible) continue;
            if (created && budget.nsecsElapsed() >= 5'000'000) { pending = true; continue; }
            strip = new ChannelStrip(m_controller, m_channels[int(i)], false,
                                      m_stripsHost, false, m_insertSlotCount);
            created = true;
            strip->setProperty("channelViewState", ui::channelViewState(
                m_controller->project(), m_channels[int(i)].toStdString()));
            wireStrip(strip);
            delete m_stripsLayout->takeAt(2 * int(i));
            m_stripsLayout->insertWidget(2 * int(i), strip);
            strip->show();
        }
    }
    m_strips.clear();
    const auto measure = [this](const ChannelStrip* strip) {
        if (!strip) return;
        const auto& heights = strip->rackNaturalHeights();
        for (std::size_t i = 0; i < heights.size(); ++i)
            m_rackHeights[i] = std::max(m_rackHeights[i], heights[i]);
    };
    for (const auto* strip : m_slots) measure(strip);
    int consoleHeight = 0;
    if (master) {
        master->setRackHeights(master->rackNaturalHeights());
        m_masterHost->setMinimumHeight(master->naturalHeight());
    }
    for (auto* strip : m_slots) {
        if (!strip) continue;
        strip->setRackHeights(m_rackHeights);
        consoleHeight = std::max(consoleHeight, strip->naturalHeight());
        strip->setSelected(strip->trackId() == m_selectedTrackId);
        m_strips.push_back(strip);
    }
    m_stripsHost->setMinimumHeight(std::max(m_stripsHost->minimumHeight(), consoleHeight));
    if (m_materializeTimer) {
        if (pending) m_materializeTimer->start();
        else m_materializeTimer->stop();
    }
    if (master) m_strips.push_back(master);
}

void MixerWidget::syncMeterTimer() {
    if (m_controller->isPlaying() || m_controller->isRecording()) m_meterTimer->start();
    else { m_meterTimer->stop(); refreshMeters(); }
}

void MixerWidget::refreshMeters() {
    // A detached console follows its own screen, including while the main
    // window is minimised. The low-rate control poll wakes playback changes.
    if (m_controller->isPlaying() || m_controller->isRecording()) m_meterTimer->start();
    else m_meterTimer->stop();
    if (!isVisible()) return;
    for (ChannelStrip* strip : m_strips) {
        if (stripIsVisible(strip)) strip->refreshMeter();
    }
}

void MixerWidget::setSelectedTrack(const QString& trackId) {
    m_selectedTrackId = trackId;
    for (ChannelStrip* strip : m_strips) {
        if (!strip->isMaster()) strip->setSelected(strip->trackId() == trackId);
    }
}
