#include "MiniModuleRack.hpp"
#include "UiPerformance.hpp"
#include "UiFrameClock.hpp"
#include <QElapsedTimer>
#include "MixerWidget.hpp"
#include "RackWidget.hpp"
#include <QStackedWidget>
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
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QScopeGuard>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QTimer>
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

} // namespace

MixerWidget::MixerWidget(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    const auto& preferences = ui::MixerPreferences::instance();
    m_view = preferences.view();
    m_channelWidth = m_view == ui::MixerPreferences::View::Compact
        ? preferences.compactWidth() : preferences.channelWidth();
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
    m_headerTitle = title;
    m_viewToggle = new QToolButton(m_header);
    m_viewToggle->setObjectName("MixerRackToggle");
    m_viewToggle->setFixedSize(24, 24);
    m_viewToggle->setFocusPolicy(Qt::StrongFocus);
    connect(m_viewToggle, &QToolButton::clicked, this, [this] { setRackMode(!m_rackMode); });
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
    m_headerGlyph->hide();
    head->addWidget(m_viewToggle);
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
    // The host reserves the project's width; only resident strips have
    // geometry. Thousands of QLayout spacers otherwise make every insertion
    // and width change traverse the whole project despite virtualization.
    m_stripsHost->installEventFilter(this);

    m_scroll = new QScrollArea(body);
    m_scroll->setObjectName(QStringLiteral("MixerChannelsScroll"));
    m_scroll->viewport()->setObjectName(QStringLiteral("MixerChannelsViewport"));
    // This plain viewport has no offset-dependent painting; its child page
    // carries all channel content. Item-view viewports must still repaint.
    m_scroll->viewport()->setProperty("vlt.staticScrollViewport", true);
    m_scroll->horizontalScrollBar()->setObjectName(QStringLiteral("MixerNavigationBar"));
    m_scroll->verticalScrollBar()->setObjectName(QStringLiteral("MixerNavigationBar"));
    m_scroll->setWidget(m_stripsHost);
    m_stripsHost->setProperty("vlt.retainOffscreen", true);
    m_stripsHost->setProperty("vlt.scrollLayerGroup", true);
    // The viewport supplies the background. Keeping the page transparent also
    // avoids raster backing-store blits under the retained GPU scene.
    m_stripsHost->setAutoFillBackground(false);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_scroll->viewport()->installEventFilter(this);
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this] { syncVisibleStrips(); });
    m_splitter = new QSplitter(Qt::Horizontal, body);
    m_splitter->setObjectName(QStringLiteral("MixerDetailsSplitter"));
    m_splitter->setHandleWidth(6);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_scroll);
    m_detailsScroll = new QScrollArea(m_splitter);
    m_detailsScroll->setObjectName(QStringLiteral("MixerDetailsScroll"));
    m_detailsScroll->setFrameShape(QFrame::NoFrame);
    m_detailsScroll->setWidgetResizable(true);
    m_detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_detailsScroll->setMinimumWidth(ui::MixerPreferences::kMinimumDetailsWidth);
    m_detailsScroll->setMaximumWidth(ui::MixerPreferences::kMaximumDetailsWidth);
    m_detailsScroll->verticalScrollBar()->setObjectName(QStringLiteral("MixerNavigationBar"));
    m_splitter->addWidget(m_detailsScroll);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_detailsScroll->setVisible(m_view == ui::MixerPreferences::View::Compact);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        if (!m_settingDetailsWidth)
            ui::MixerPreferences::instance().setDetailsWidth(m_detailsScroll->width());
    });
    bodyRow->addWidget(m_splitter, 1);
    m_views = new QStackedWidget(this);
    m_views->addWidget(body);
    m_rack = new RackWidget(controller, m_views);
    m_views->addWidget(m_rack);
    outer->addWidget(m_views, 1);
    connect(m_rack, &RackWidget::editorRequested, this, &MixerWidget::pluginEditorRequested);
    connect(m_rack, &RackWidget::automationRequested, this, &MixerWidget::automatePluginRequested);
    connect(m_rack, &RackWidget::sendAutomationRequested, this, &MixerWidget::automateSendRequested);
    connect(m_rack, &RackWidget::channelRequested, this, &MixerWidget::trackSelected);
    connect(m_rack, &RackWidget::trackCreated, this, &MixerWidget::trackCreated);
    connect(m_rack, &RackWidget::edited, this, [this](bool structural) {
        emit channelEdited(m_rack->channelId(), true);
        if (structural) {
            QTimer::singleShot(0, this, &MixerWidget::rebuild);
            emit structureChanged();
        }
    });

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &MixerWidget::applyTheme);
    connect(&ui::MixerPreferences::instance(), &ui::MixerPreferences::channelWidthChanged,
            this, [this](int width) {
                if (m_view == ui::MixerPreferences::View::Classic) applyChannelWidth(width);
            });
    connect(&ui::MixerPreferences::instance(), &ui::MixerPreferences::compactWidthChanged,
            this, [this](int width) {
                if (m_view == ui::MixerPreferences::View::Compact) applyChannelWidth(width);
            });
    connect(&ui::MixerPreferences::instance(), &ui::MixerPreferences::viewChanged,
            this, &MixerWidget::applyView);
    connect(&ui::MixerPreferences::instance(), &ui::MixerPreferences::detailsWidthChanged,
            this, &MixerWidget::applyDetailsWidth);
    connect(&ui::MixerPreferences::instance(), &ui::MixerPreferences::masterVisibleChanged,
            this, &MixerWidget::setMasterVisible);
    m_meterTimer = new ui::FrameTimer(this);
    connect(m_meterTimer, &ui::FrameTimer::timeout, this, &MixerWidget::refreshMeters);
    m_materializeTimer = new ui::FrameTimer(this);
    connect(m_materializeTimer, &ui::FrameTimer::timeout, this, &MixerWidget::syncVisibleStrips);
    applyTheme();
    rebuild();
    applyDetailsWidth(preferences.detailsWidth());
    QTimer::singleShot(0, this, [this] {
        applyDetailsWidth(ui::MixerPreferences::instance().detailsWidth());
    });
    setRackMode(QSettings().value("ui/rackMode", false).toBool());
}

bool MixerWidget::rackCommand(const QString& command) {
    return m_rackMode && m_rack && m_rack->command(command);
}
void MixerWidget::setRackMode(bool enabled) {
    const bool changed = m_rackMode != enabled;
    if (m_rack) m_rack->finishEdits();
    if (changed) finishChannelEdits();
    m_rackMode = enabled;
    m_views->setCurrentIndex(enabled ? 1 : 0);
    m_viewToggle->setIcon(icons::icon(enabled ? icons::Glyph::Layers : icons::Glyph::Mixer, th().accent, 16));
    m_viewToggle->setToolTip(enabled ? tr("Show mixer") : tr("Show rack"));
    m_viewToggle->setAccessibleName(m_viewToggle->toolTip());
    m_headerTitle->setText(enabled ? tr("RACK") : tr("MIXER"));
    m_headerCount->setText(enabled ? m_rack->channelName() : tr("%1 channels").arg(m_channels.size()));
    m_masterToggle->setVisible(!enabled);
    if (enabled) { m_rack->setChannel(m_selectedTrackId); m_headerCount->setText(m_rack->channelName()); m_rack->setFocus(); }
    else syncVisibleStrips();
    QSettings().setValue("ui/rackMode", enabled);
    if (changed) emit rackModeChanged(enabled);
    syncMeterTimer();
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
#MixerChannelsScroll, #MixerMasterScroll, #MixerChannelsViewport, #MixerDetailsScroll,
#MixerStripHost, #MixerMasterHost { background: transparent; border: none; }
#MixerDetailsSplitter::handle { background: %SECTION%; border-left: 2px solid %WELL%; border-right: 2px solid %WELL%; }
#MixerDetailsSplitter::handle:hover { background: %ACCENT%; }
#MasterVisibilityButton { color: %TEXT2%; background: %WELL%; border: 1px solid %SEP%;
                         border-radius: 6px; padding: 2px 8px; font-size: 11px; }
#MasterVisibilityButton:checked { color: %TEXT%; background: %SELECT%; border-color: %SELECT_EDGE%; }
#MasterVisibilityButton:hover { background: %HOVER%; color: %TEXT%; }
#MasterVisibilityButton:pressed { background: %WELL%; }
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

void MixerWidget::updateMasterToggle() {
    const auto action = m_masterVisible ? tr("Hide master channel") : tr("Show master channel");
    const QSignalBlocker block(m_masterToggle);
    m_masterToggle->setChecked(m_masterVisible);
    m_masterToggle->setToolTip(action); m_masterToggle->setAccessibleName(action);
    m_masterToggle->setIcon(icons::icon(m_masterVisible ? icons::Glyph::ArrowRight : icons::Glyph::ArrowLeft,
        m_masterVisible ? th().accent : th().textSecondary, 12));

}

void MixerWidget::setMasterVisible(bool visible) {
    m_masterVisible = visible;
    updateMasterToggle();
}

double MixerWidget::faderGainForTest(const QString& trackId) const {
    for (ChannelStrip* strip : m_strips) {
        if (strip->trackId() == trackId) return strip->faderGainForTest();
    }
    return -1.0;
}

void MixerWidget::syncFromModel(const QStringList& trackIds) {
    if (m_rack) { m_rack->sync(); if (m_rackMode) m_headerCount->setText(m_rack->channelName()); }
    QSet<QString> affected(trackIds.begin(), trackIds.end());
    for (const auto& id : trackIds) {
        if (const auto* track = m_controller->project().findTrack(id.toStdString());
            track && !track->outputBusId.empty())
            affected.insert(QString::fromStdString(track->outputBusId));
    }
    for (ChannelStrip* strip : m_strips)
        if (trackIds.isEmpty() || affected.contains(strip->trackId())) strip->syncFromModel();
    syncDetails();
    // An inspector can edit a strip that has not been materialized by the
    // mixer yet. Reserve its new rack height before it scrolls into view.
    if(trackIds.isEmpty() || std::any_of(trackIds.begin(),trackIds.end(),[&](const auto& id) {
        return std::none_of(m_strips.begin(),m_strips.end(),[&](const auto* strip){return strip->trackId()==id;});
    })) syncVisibleStrips();
}

void MixerWidget::refreshAutomationValues() {
    if (!isVisible() || m_rackMode) return;
    for (ChannelStrip* strip : m_strips) {
        if (stripIsVisible(strip)) strip->refreshAutomationValues();
    }
    if (m_details && ui::isVisibleInViewport(m_details)) m_details->refreshAutomationValues();
}

bool MixerWidget::checkCollaborationPresenceForTest(QString* error) {
    const QVariant previousRack = QSettings().value("ui/rackMode");
    const auto restoreRack = qScopeGuard([&] {
        if (previousRack.isValid()) QSettings().setValue("ui/rackMode", previousRack);
        else QSettings().remove("ui/rackMode");
    });
    auto& preferences = ui::MixerPreferences::instance();
    const bool wasVisible = preferences.masterVisible();
    const auto restoreMaster = qScopeGuard([&] { preferences.setMasterVisible(wasVisible); });
    preferences.setMasterVisible(true);
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    daw::EngineController controller{};
    controller.initialize(48000.0, 512, false);
    const QString trackId =
        QString::fromStdString(controller.addTrack(daw::TrackKind::Audio, "A"));
    (void)controller.addTrack(daw::TrackKind::Audio, "B");
    if (trackId.isEmpty())
        return fail(QStringLiteral("mixer presence fixture has no tracks"));

    MixerWidget mixer(&controller);
    mixer.setRackMode(false);
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
    if (mixer.collaborationPositionFor(master))
        return fail(QStringLiteral("mixer must not expose the removed master strip"));
    return true;
}

bool MixerWidget::checkLayoutForTest() {
    const QVariant previousRack = QSettings().value("ui/rackMode");
    const auto restoreRack = qScopeGuard([&] {
        if (previousRack.isValid()) QSettings().setValue("ui/rackMode", previousRack);
        else QSettings().remove("ui/rackMode");
    });
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
    daw::EngineController controller{};
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
        // The menu test compiles this routing. Its destination bus must not
        // send back into itself when the send tap is changed transactionally.
        for (int j = 0; j < (i == 2 ? 0 : i % 5); ++j) {
            daw::SendModel send;
            send.id = track.id + "-send-" + std::to_string(j);
            send.destinationTrackId = "layout-2";
            track.sends.push_back(std::move(send));
        }
        project.tracks.push_back(std::move(track));
    }
    MixerWidget mixer(&controller);
    mixer.setRackMode(false);
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
        std::vector<int> residentSlots;
        for (int i = 0; i < int(mixer.m_slots.size()); ++i)
            if (mixer.m_slots[i]) residentSlots.push_back(i);
        ok &= residentSlots == mixer.m_residentSlots;
        ok &= mixer.m_strips.size() == mixer.m_residentSlots.size();
        std::array<std::vector<int>, 2> slotBaselines;
        for (std::size_t i = 0; i < mixer.m_slots.size(); ++i) {
            if (const auto* strip = mixer.m_slots[i])
                ok &= strip->x() == int(i) * channelStride(mixer.channelWidth()) &&
                      strip->trackId() == mixer.m_channels[int(i)];
        }
        for (const char* name : {"ChannelProcessingRow", "ChannelSendsRow", "ChannelLevelWell"}) {
            std::optional<int> baseline;
            for (auto* strip : mixer.m_strips) {
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
            if (strip->insertSlotCount() != stripSlots || slotButtons.size() != filledCount)
                std::fprintf(stderr, "FAIL slots %s: slots %d/%d buttons %lld/%d\n", strip->trackId().toUtf8().constData(),
                    strip->insertSlotCount(), stripSlots, qlonglong(slotButtons.size()), filledCount);
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
                ok &= loudness && loudness->isVisibleTo(strip) && sends &&
                    sends->height() == mixer.m_rackHeights[4] &&
                    !sends->findChild<QToolButton*>(QStringLiteral("SendAddArea"));
                if (loudness) ok &= loudness->rect().width() >= 56;
            }
            ok &= strip->findChild<QWidget*>(QStringLiteral("ColorSwatch")) &&
                !strip->findChild<QLabel*>(QStringLiteral("NamePlate"));
            auto* well = strip->findChild<QWidget*>(QStringLiteral("ChannelLevelWell"));
            const QList<QWidget*> controls{strip->findChild<ui::FaderWidget*>(),
                strip->findChild<ui::LevelMeter*>(), strip->findChild<ui::PanKnob*>(),
                strip->findChild<QWidget*>(QStringLiteral("ChannelGainReadout")),
                strip->findChild<QWidget*>(QStringLiteral("ChannelPeakReadout"))};
            for (auto* control : controls) {
                const bool contained = well && control && well->rect().contains(
                    QRect(control->mapTo(well, QPoint{}), control->size()));
                if (!contained && control && well) {
                    const QRect rect(control->mapTo(well, QPoint{}), control->size());
                    std::fprintf(stderr, "FAIL control %s: %s %d,%d %dx%d in %dx%d\n", strip->trackId().toUtf8().constData(),
                        control->metaObject()->className(), rect.x(), rect.y(), rect.width(), rect.height(), well->width(), well->height());
                }
                ok &= contained;
            }
            for (auto* button : strip->findChildren<QAbstractButton*>()) {
                if (!button->isVisibleTo(strip)) continue;
                const QRect rect(button->mapTo(strip, QPoint{}), button->size());
                const bool contained = strip->rect().contains(rect);
                if (!contained) std::fprintf(stderr, "FAIL button %s %s: %d,%d %dx%d in %dx%d\n",
                    strip->trackId().toUtf8().constData(), button->objectName().toUtf8().constData(),
                    rect.x(), rect.y(), rect.width(), rect.height(), strip->width(), strip->height());
                ok &= contained;
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
    // Settings pages are lazy: exercise the same entry point as opening the
    // mixer settings, independently of the user's last selected page.
    settings.showTab(SettingsWindow::kMixerTab);
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
            std::abs(mixer.m_scroll->horizontalScrollBar()->value() - qRound(anchor * channelStride(width))) > 1) {
            std::fprintf(stderr, "FAIL mixer width %d: peer=%d slider=%d saved=%d stable=%d undo=%zu/%zu scroll=%d expected=%d max=%d\n",
                width, secondMixer.channelWidth(), slider->value(), QSettings().value(ui::MixerPreferences::kWidthSetting).toInt(),
                mixer.m_slots[3] == retainedStrip, controller.undoDepth(), undoDepth,
                mixer.m_scroll->horizontalScrollBar()->value(), qRound(anchor * channelStride(width)),
                mixer.m_scroll->horizontalScrollBar()->maximum());
            return false;
        }
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
    mixer.m_scroll->horizontalScrollBar()->setValue(0);
    settle();
    const QPointer<ChannelStrip> revisited = mixer.m_slots.front();
    mixer.m_scroll->horizontalScrollBar()->setValue(mixer.m_scroll->horizontalScrollBar()->maximum());
    settle();
    if (!aligned("virtualised channels with longer chains")) return false;
    mixer.m_scroll->horizontalScrollBar()->setValue(0);
    settle();
    if (!revisited || mixer.m_slots.front() != revisited) {
        std::fprintf(stderr, "FAIL mixer: reversing scroll recreated a recently visited channel\n");
        return false;
    }
    mixer.m_scroll->horizontalScrollBar()->setValue(mixer.m_scroll->horizontalScrollBar()->maximum());
    settle();
    if (!shots.isEmpty() && !mixer.grab().save(shots + "-long-chain.png")) return false;
    mixer.m_scroll->verticalScrollBar()->setValue(mixer.m_scroll->verticalScrollBar()->maximum());
    settle();
    if (!aligned("track racks and pinned master scroll as one row")) return false;
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
    if (!aligned("master's longer chain keeps shared baselines and unchanged track slots")) return false;
    project.masterInserts.clear();
    for (auto& track : project.tracks) track.inserts.clear();
    expectedSlots = 2;
    mixer.rebuild();
    settle();
    if (!aligned("empty project returns to the compact default")) return false;
    // Exceed the residency budget and reverse across a cold viewport. Every
    // live slot must survive rebuild/reordering in the resident index, while
    // far-away widgets are actually evicted rather than accumulating forever.
    for (int i = 22; i < 96; ++i) {
        daw::TrackModel track;
        track.id = "layout-" + std::to_string(i);
        track.name = "Channel " + std::to_string(i + 1);
        track.kind = daw::TrackKind::Audio;
        project.tracks.push_back(std::move(track));
    }
    project.invalidateTrackIndex();
    mixer.rebuild();
    for (const bool right : {false, true, false, true}) {
        mixer.m_scroll->horizontalScrollBar()->setValue(right
            ? mixer.m_scroll->horizontalScrollBar()->maximum() : 0);
        settle();
        const int stride = channelStride(mixer.channelWidth());
        const int viewport = mixer.m_scroll->viewport()->width();
        const int capacity = std::max(24, 5 * ((viewport + stride - 1) / stride) + 2);
        if (!aligned("bounded resident channels after cold scroll") ||
            int(mixer.m_residentSlots.size()) > capacity) return false;
    }
    std::reverse(project.tracks.begin(), project.tracks.end());
    project.invalidateTrackIndex();
    mixer.rebuild();
    settle();
    return aligned("resident channels after track reorder") &&
           ChannelStrip::checkGroupInputsForTest() && ChannelStrip::checkRebindForTest() &&
           LoudnessDisplay::checkForTest(shots) &&
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
        const QWidget* viewport = m_scroll->viewport();
        if (!viewport) return {};
        const QRect visible(
            viewport->mapTo(const_cast<MixerWidget*>(this), QPoint{}),
            viewport->size());
        QRect clipped = rect.intersected(visible);
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
    return ui::isVisibleInViewport(strip);
}

void MixerWidget::wireStrip(ChannelStrip* strip) {
    connect(strip, &ChannelStrip::rackSelectionRequested, this, [this](const QString& channel, const QStringList& ids) {
        setSelectedTrack(channel); m_rack->selectDevices(ids); emit trackSelected(channel);
    });
    strip->setStripWidth(m_channelWidth);
    ui::perf::sample("mixer.strip.created", 1);
        connect(strip, &ChannelStrip::selectRequested, this,
                [this](const QString& id) { setSelectedTrack(id); emit trackSelected(id); });
        connect(strip, &ChannelStrip::timelineRequested, this,
                &MixerWidget::timelineRequested);
        connect(strip, &ChannelStrip::createTracksRequested, this,
                &MixerWidget::createTracksRequested);
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
        connect(strip, &ChannelStrip::automatePluginRequested, this, &MixerWidget::automatePluginRequested);
        connect(strip, &ChannelStrip::rackLayoutChanged, this, [this, strip] {
            if (strip->presentation() == ChannelStrip::Presentation::Details) return;
            m_stripLayoutDirty = true; m_rackHeights[1] = 0; syncVisibleStrips();
        });
        connect(strip, &ChannelStrip::structureChanged, this, [this] {
            emit structureChanged();
            rebuild();
        }, Qt::QueuedConnection);
        connect(strip, &ChannelStrip::trackCreated, this,
                &MixerWidget::trackCreated, Qt::QueuedConnection);
}

void MixerWidget::contextMenuEvent(QContextMenuEvent* event) {
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("MixerContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    connect(menu->addAction(tr("Show Master on Timeline")), &QAction::triggered, this,
            [this] { emit timelineRequested(QString::fromLatin1(daw::EngineController::kMasterChannelId)); });
    menu->addSeparator();
    const auto kinds = ui::addTrackKindItems(*menu);
    for (auto it = kinds.cbegin(); it != kinds.cend(); ++it)
        connect(it.key(), &QAction::triggered, this, [this, spec = it.value()] {
            if (!spec.create(*m_controller).empty()) emit trackCreated();
        });
    menu->addSeparator();
    connect(menu->addAction(tr("Create Tracks…")), &QAction::triggered,
            this, &MixerWidget::createTracksRequested);
    connect(menu->addAction(tr("Mixer settings")), &QAction::triggered,
            this, &MixerWidget::settingsRequested);
    menu->popup(event->globalPos());
    event->accept();
}

void MixerWidget::rebuild() {
    ui::perf::Scope timing("rebuild.MixerWidget.ms");
    m_stripLayoutDirty = true;
    m_colorHeightRevision = ~quint64(0);
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    for (auto* strip : m_recycledStrips) strip->deleteLater();
    m_recycledStrips.clear();
    m_deferredRackChange = false;
    m_rackHeights.fill(0);
    QHash<QString, ChannelStrip*> previous;
    for (auto* strip : m_strips) previous.insert(strip->trackId(), strip);
    m_strips.clear(); m_slots.clear(); m_channels.clear(); m_residentSlots.clear();
    const auto& project = m_controller->project();
    m_insertSlotCount = 2;
    if (m_view == ui::MixerPreferences::View::Classic)
        for (const auto& track : project.tracks)
            if (daw::carriesAudio(track))
                m_insertSlotCount = std::max(m_insertSlotCount, ui::channelInsertRows(project, track.id));
    for (const auto& track : project.tracks) {
        if (!daw::carriesAudio(track)) continue;
        const auto id = QString::fromStdString(track.id);
        m_channels.push_back(id);
        auto* strip = previous.take(id);
        if (strip && m_view == ui::MixerPreferences::View::Classic &&
                     (strip->insertSlotCount() != m_insertSlotCount ||
                      strip->property("channelViewState").toByteArray() !=
                         ui::channelViewState(project, track.id))) {
            if (strip->hasActiveGesture()) m_deferredRackChange = true;
            else { strip->hide(); strip->deleteLater(); strip = nullptr; }
        }
        m_slots.push_back(strip);
        if (strip) m_residentSlots.push_back(int(m_slots.size()) - 1);
    }
    m_stripsHost->setMinimumWidth(channelsWidth(int(m_channels.size()), m_channelWidth));
    for (auto* strip : previous) { strip->finishEdits(); strip->hide(); strip->deleteLater(); }
    m_stripsHost->setMinimumHeight(0);
    m_headerCount->setText(m_rackMode ? m_rack->channelName() : tr("%1 channels").arg(m_channels.size()));
    m_syncingStrips = false;
    syncVisibleStrips();
    syncFromModel();
}

void MixerWidget::applyChannelWidth(int width) {
    if (m_channelWidth == width) return;
    m_stripLayoutDirty = true;
    // Preserve the channel at the viewport's left edge, including its partial
    // offset. Width changes never rebuild a strip or interrupt an audio edit.
    const double anchor = double(m_scroll->horizontalScrollBar()->value()) / channelStride(m_channelWidth);
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    m_channelWidth = width;
    for (auto* strip : m_strips) strip->setStripWidth(width);
    int minimumHeight = 0;
    for (const auto* strip : m_strips)
        if (!strip->isMaster()) minimumHeight = std::max(minimumHeight, strip->naturalHeight());
    m_stripsHost->setMinimumHeight(minimumHeight);
    m_stripsHost->setMinimumWidth(channelsWidth(int(m_slots.size()), width));
    m_stripsHost->resize(std::max(m_scroll->viewport()->width(), channelsWidth(int(m_slots.size()), width)),
                        std::max(m_scroll->viewport()->height(), minimumHeight));
    positionStrips();
    m_scroll->horizontalScrollBar()->setValue(qRound(anchor * channelStride(width)));
    m_syncingStrips = false;
    syncVisibleStrips();
}

void MixerWidget::finishChannelEdits() {
    for (auto* strip : m_strips) strip->finishEdits();
    if (m_details) m_details->finishEdits();
}

void MixerWidget::applyView(ui::MixerPreferences::View view) {
    if (view == m_view) return;
    finishChannelEdits();
    const double anchor = double(m_scroll->horizontalScrollBar()->value()) / channelStride(m_channelWidth);
    m_verticalPositions[m_view == ui::MixerPreferences::View::Compact ? 1 : 0] =
        m_scroll->verticalScrollBar()->value();
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    for (auto* strip : m_strips) delete strip;
    for (auto* strip : m_recycledStrips) delete strip;
    m_strips.clear(); m_recycledStrips.clear(); m_residentSlots.clear();
    std::fill(m_slots.begin(), m_slots.end(), nullptr);
    m_view = view;
    m_detailsScroll->setVisible(view == ui::MixerPreferences::View::Compact);
    if (view == ui::MixerPreferences::View::Classic && m_details) {
        delete m_detailsScroll->takeWidget();
        m_details = nullptr;
    }
    const auto& preferences = ui::MixerPreferences::instance();
    m_channelWidth = view == ui::MixerPreferences::View::Compact ? preferences.compactWidth() : preferences.channelWidth();
    m_stripsHost->setMinimumSize(channelsWidth(int(m_slots.size()), m_channelWidth), 0);
    if (m_splitter->parentWidget()->layout()) m_splitter->parentWidget()->layout()->activate();
    applyDetailsWidth(preferences.detailsWidth());
    m_syncingStrips = false;
    rebuild();
    m_scroll->horizontalScrollBar()->setValue(qRound(anchor * channelStride(m_channelWidth)));
    m_scroll->verticalScrollBar()->setValue(m_verticalPositions[view == ui::MixerPreferences::View::Compact ? 1 : 0]);
    syncVisibleStrips();
}

void MixerWidget::applyDetailsWidth(int width) {
    if (m_view != ui::MixerPreferences::View::Compact) return;
    const QScopedValueRollback<bool> guard(m_settingDetailsWidth, true);
    width = std::clamp(width, ui::MixerPreferences::kMinimumDetailsWidth, ui::MixerPreferences::kMaximumDetailsWidth);
    m_splitter->setSizes({std::max(1, m_splitter->width() - m_splitter->handleWidth() - width), width});
}

void MixerWidget::syncDetails() {
    if (m_view != ui::MixerPreferences::View::Compact || m_syncingDetails) return;
    const QScopedValueRollback<bool> guard(m_syncingDetails, true);
    const auto* track = m_controller->project().findTrack(m_selectedTrackId.toStdString());
    const QString id = track && daw::carriesAudio(*track) ? m_selectedTrackId : QString{};
    const QByteArray state = id.isEmpty() ? QByteArray{} : ui::channelViewState(m_controller->project(), id.toStdString());
    if (!m_details) {
        m_details = new ChannelStrip(m_controller, id, false, nullptr, false, 0, ChannelStrip::Presentation::Details);
        wireStrip(m_details);
        m_detailsScroll->setWidget(m_details);
        m_details->setProperty("channelViewState", state);
    } else if (m_details->trackId() != id ||
               (m_details->property("channelViewState").toByteArray() != state && !m_details->hasActiveGesture())) {
        const bool sameChannel = m_details->trackId() == id;
        const int scroll = sameChannel ? m_detailsScroll->verticalScrollBar()->value() : 0;
        m_details->setDetailsChannel(id);
        m_details->setProperty("channelViewState", state);
        m_detailsScroll->verticalScrollBar()->setValue(scroll);
    } else {
        m_details->syncFromModel();
    }
}

bool MixerWidget::eventFilter(QObject* object, QEvent* event) {
    if (object == m_stripsHost && event->type() == QEvent::Resize) positionStrips();
    if (m_scroll && object == m_scroll->viewport() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        syncVisibleStrips();
    }
    return QWidget::eventFilter(object, event);
}

void MixerWidget::positionStrips() {
    if (!m_stripsHost) return;
    for (const int i : m_residentSlots)
        m_slots[i]->setGeometry(i * channelStride(m_channelWidth), 0,
                               m_channelWidth, m_stripsHost->height());
}

void MixerWidget::syncVisibleStrips() {
    ui::perf::Scope timing("syncVisibleStrips.MixerWidget.ms");
    if (m_syncingStrips || !m_scroll) return;
    if (m_deferredRackChange) {
        for (auto* strip : m_strips) {
            const int expectedSlots = strip->isMaster()
                ? ui::channelInsertRows(m_controller->project(), {}) : m_insertSlotCount;
            if (!strip->hasActiveGesture() && (strip->insertSlotCount() != expectedSlots ||
                strip->property("channelViewState").toByteArray() !=
                ui::channelViewState(m_controller->project(), strip->trackId().toStdString()))) {
                rebuild(); return;
            }
        }
    }
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    const int left = m_scroll->horizontalScrollBar()->value();
    const int stride = channelStride(m_channelWidth);
    const int viewportWidth = m_scroll->viewport()->width();
    const int margin = std::max(2 * stride, viewportWidth);
    const int first = std::max(0, (left - margin - m_channelWidth + stride - 1) / stride);
    const int last = std::min(int(m_slots.size()) - 1, (left + viewportWidth + margin) / stride);
    const auto owns = [](QWidget* strip, QWidget* widget) {
        return strip && widget && (strip == widget || strip->isAncestorOf(widget));
    };
    QElapsedTimer budget;
    budget.start();
    const double period = ui::FrameClock::instance().periodSeconds(this);
    const qint64 creationBudget = qint64(std::clamp(
        (period > 0 ? period : 1. / 240.) * 1e9 * .25, 250'000., 2'000'000.));
    bool pending = m_deferredRackChange;
    bool changed = false;
    // Retain recently visited channels within a viewport-sized budget. A
    // direction change must not recreate hundreds of controls just discarded
    // behind the pointer. New projects still build only the visible overscan.
    const int capacity = std::max(24, 5 * ((viewportWidth + stride - 1) / stride) + 2);
    int missing = 0;
    for (int i = first; i <= last; ++i) missing += !m_slots[i];
    int evict = int(m_residentSlots.size()) + missing - capacity;
    if (evict > 0) {
        std::vector<int> candidates;
        for (const int i : m_residentSlots)
            if (i < first || i > last) candidates.push_back(i);
        const int center = left + viewportWidth / 2;
        std::sort(candidates.begin(), candidates.end(), [=](int a, int b) {
            return std::abs(a * stride - center) > std::abs(b * stride - center);
        });
        for (const int i : candidates) {
            auto*& strip = m_slots[i];
            if (strip->hasActiveGesture() || owns(strip, QApplication::focusWidget())) continue;
            strip->hide();
            if (m_recycledStrips.size() < 8) m_recycledStrips.push_back(strip);
            else strip->deleteLater();
            strip = nullptr;
            changed = true;
            if (--evict == 0) break;
        }
        std::erase_if(m_residentSlots, [this](int i) { return !m_slots[i]; });
    }
    // Visible channels precede overscan. A large console never constructs the
    // whole viewport in one event; the frame clock resumes the remaining work.
    bool created = false;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = first; i <= last; ++i) {
            auto*& strip = m_slots[i];
            if (strip) continue;
            const int x = int(i) * channelStride(m_channelWidth);
            const bool visible = x + m_channelWidth >= left && x <= left + m_scroll->viewport()->width();
            const bool nearby = x + m_channelWidth >= left - margin &&
                x <= left + m_scroll->viewport()->width() + margin;
            if (!nearby || (pass == 0) != visible) continue;
            if (created && budget.nsecsElapsed() >= creationBudget) { pending = true; continue; }
            ui::perf::Scope creation("createStrip.MixerWidget.ms");
            for (auto it = m_recycledStrips.begin(); it != m_recycledStrips.end(); ++it) {
                if ((*it)->rebindChannel(m_channels[i], m_insertSlotCount)) {
                    strip = *it;
                    m_recycledStrips.erase(it);
                    ui::perf::sample("mixer.strip.reused", 1);
                    break;
                }
            }
            if (!strip) {
                strip = new ChannelStrip(m_controller, m_channels[i], false,
                                          m_stripsHost, false, m_insertSlotCount,
                                          m_view == ui::MixerPreferences::View::Compact
                                              ? ChannelStrip::Presentation::Compact : ChannelStrip::Presentation::Classic);
                wireStrip(strip);
            }
            m_residentSlots.push_back(i);
            created = true;
            changed = true;
            strip->setProperty("channelViewState", ui::channelViewState(
                m_controller->project(), m_channels[int(i)].toStdString()));
            strip->setChannelNumber(i + 1);
            strip->setGeometry(i * stride, 0, m_channelWidth, m_stripsHost->height());
            strip->show();
        }
    }
    if (m_materializeTimer) {
        if (pending) m_materializeTimer->start();
        else m_materializeTimer->stop();
    }
    const bool classic = m_view == ui::MixerPreferences::View::Classic;
    const bool colorHeightDirty = classic && ui::MixerPreferences::instance().colorVisible() &&
        (m_colorHeightRevision != m_controller->projectRevision() ||
         m_colorHeightGeneration != m_controller->projectGeneration() || m_colorHeightWidth != m_channelWidth);
    if (!changed && !m_stripLayoutDirty && !colorHeightDirty) return;
    ui::perf::Scope layoutTiming("layoutStrips.MixerWidget.ms");
    if (changed) std::sort(m_residentSlots.begin(), m_residentSlots.end());
    m_stripLayoutDirty = false;
    m_strips.clear();
    const auto measure = [this](const ChannelStrip* strip) {
        if (!strip) return;
        const auto& heights = strip->rackNaturalHeights();
        for (std::size_t i = 0; i < heights.size(); ++i)
            m_rackHeights[i] = std::max(m_rackHeights[i], heights[i]);
    };
    m_rackHeights[1]=0;
    if(classic && ui::MixerPreferences::instance().colorVisible()) {
        if (m_colorHeightRevision != m_controller->projectRevision() ||
            m_colorHeightGeneration != m_controller->projectGeneration() || m_colorHeightWidth != m_channelWidth) {
            m_colorHeight = 0;
            for (const auto& track : m_controller->project().tracks) if (daw::carriesAudio(track))
                m_colorHeight = std::max(m_colorHeight, ui::MiniModuleRack::naturalHeight(m_controller->project(), track.miniModules, m_channelWidth));
            m_colorHeightRevision = m_controller->projectRevision();
            m_colorHeightGeneration = m_controller->projectGeneration(); m_colorHeightWidth = m_channelWidth;
        }
        m_rackHeights[1] = m_colorHeight;
    }
    if (classic) for (const int i : m_residentSlots) measure(m_slots[i]);
    int consoleHeight = 0;
    for (const int i : m_residentSlots) {
        auto* strip = m_slots[i];
        if (classic) strip->setRackHeights(m_rackHeights);
        strip->setChannelNumber(i + 1);
        consoleHeight = std::max(consoleHeight, strip->naturalHeight());
        strip->setSelected(strip->trackId() == m_selectedTrackId);
        m_strips.push_back(strip);
    }
    consoleHeight = std::max(m_stripsHost->minimumHeight(), consoleHeight);
    m_stripsHost->setMinimumHeight(consoleHeight);
    positionStrips();
}

void MixerWidget::syncMeterTimer() {
    if (ui::isVisibleInViewport(this) && !m_rackMode &&
        (m_controller->isPlaying() || m_controller->isRecording())) m_meterTimer->start();
    else { m_meterTimer->stop(); refreshMeters(); }
}

void MixerWidget::refreshMeters() {
    ui::perf::Scope timing("refreshMeters.MixerWidget.ms");
    // A detached console follows its own screen, including while the main
    // window is minimised. The low-rate control poll wakes playback changes.
    if (!ui::isVisibleInViewport(this) || m_rackMode) return;
    for (ChannelStrip* strip : m_strips) {
        if (stripIsVisible(strip)) strip->refreshMeter();
    }
}

void MixerWidget::setSelectedTrack(const QString& trackId) {
    if (m_selectedTrackId != trackId) finishChannelEdits();
    m_selectedTrackId = trackId;
    if (m_rack) { m_rack->setChannel(trackId); if (m_rackMode) m_headerCount->setText(m_rack->channelName()); }
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    for (ChannelStrip* strip : m_strips) {
        strip->setSelected(strip->isMaster()
            ? (trackId == daw::EngineController::kMasterChannelId || (track && track->kind == daw::TrackKind::Master)) : strip->trackId() == trackId);
    }
    syncDetails();
}
