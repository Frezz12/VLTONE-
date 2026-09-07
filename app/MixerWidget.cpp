#include "UiPerformance.hpp"
#include "UiFrameClock.hpp"
#include <QElapsedTimer>
#include "MixerWidget.hpp"
#include "ChannelViewState.hpp"
#include <QApplication>
#include <QEvent>
#include <QScopedValueRollback>

#include <algorithm>
#include "ChannelStrip.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include "EngineController.hpp"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

MixerWidget::MixerWidget(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("MixerPanel");
    setAttribute(Qt::WA_StyledBackground, true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Flat console command bar: it meets the timeline and both side edges
    // exactly, while the small accent rail gives the title a clear origin.
    m_header = new QWidget(this);
    m_header->setObjectName("MixerHeader");
    m_header->setFixedHeight(32);
    auto* head = new QHBoxLayout(m_header);
    head->setContentsMargins(10, 0, 6, 0);
    head->setSpacing(7);

    m_headerAccent = new QWidget(m_header);
    m_headerAccent->setObjectName(QStringLiteral("MixerHeaderAccent"));
    m_headerAccent->setFixedSize(2, 16);
    m_headerGlyph = new QLabel(m_header);
    m_headerGlyph->setFixedSize(16, 16);
    auto* title = new QLabel(tr("MIXER"), m_header);
    title->setObjectName("MixerTitle");
    m_headerCount = new QLabel(m_header);
    m_headerCount->setObjectName(QStringLiteral("MixerHeaderCount"));
    auto* settings = new ui::IconButton(icons::Glyph::Gear,
                                        tr("Mixer settings"), m_header);
    settings->setObjectName(QStringLiteral("MixerSettingsButton"));
    settings->setButtonSize(28, 24);
    settings->setFocusPolicy(Qt::StrongFocus);
    settings->setAccessibleName(tr("Mixer settings"));
    connect(settings, &QAbstractButton::clicked, this,
            &MixerWidget::settingsRequested);

    head->addWidget(m_headerAccent);
    head->addWidget(m_headerGlyph);
    head->addWidget(title);
    head->addWidget(ui::separatorLine(Qt::Vertical, 14, m_header));
    head->addWidget(m_headerCount);
    head->addStretch(1);
    head->addWidget(settings);
    outer->addWidget(m_header);

    // ── Strips: scrolling channels on the left, master pinned right ──
    auto* body = new QWidget(this);
    auto* bodyRow = new QHBoxLayout(body);
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->setSpacing(0);

    m_stripsHost = new QWidget(body);
    m_stripsLayout = new QHBoxLayout(m_stripsHost);
    m_stripsLayout->setContentsMargins(8, 8, 8, 8);
    m_stripsLayout->setSpacing(5);
    m_stripsLayout->addStretch(1);

    m_scroll = new QScrollArea(body);
    m_scroll->setWidget(m_stripsHost);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_masterHost = new QWidget(body);
    auto* masterRow = new QHBoxLayout(m_masterHost);
    masterRow->setContentsMargins(6, 8, 8, 8);
    masterRow->setSpacing(0);

    // The master rides in its own scroll view with hidden bars, slaved to the
    // channel area, so both stay vertically in step when the pane is short.
    auto* masterScroll = new QScrollArea(body);
    masterScroll->setWidget(m_masterHost);
    masterScroll->setWidgetResizable(true);
    masterScroll->setFrameShape(QFrame::NoFrame);
    masterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    masterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    masterScroll->setFixedWidth(140);
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged,
            masterScroll->verticalScrollBar(), &QScrollBar::setValue);

    m_scroll->viewport()->installEventFilter(this);
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this] { syncVisibleStrips(); });
    bodyRow->addWidget(m_scroll, 1);
    bodyRow->addWidget(ui::separatorLine(Qt::Vertical, 0, body));
    bodyRow->addWidget(masterScroll, 0);
    outer->addWidget(body, 1);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &MixerWidget::applyTheme);
    m_meterTimer = new ui::FrameTimer(this);
    connect(m_meterTimer, &ui::FrameTimer::timeout, this, &MixerWidget::refreshMeters);
    m_materializeTimer = new ui::FrameTimer(this);
    connect(m_materializeTimer, &ui::FrameTimer::timeout, this, &MixerWidget::syncVisibleStrips);
    applyTheme();
    rebuild();
}

void MixerWidget::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#MixerPanel { background: %BG%; border: none; }
#MixerHeader { background: %HEADER%; border: none;
               border-top: 1px solid %SECTION%;
               border-bottom: 1px solid %SECTION%; }
#MixerTitle { color: %TEXT%; font-size: 10px; font-weight: 700;
              letter-spacing: 0.6px; }
#MixerHeaderCount { color: %TEXT2%; font-size: 10px; }
)")
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%HEADER%", mixColors(t.surface, t.surfaceElevated, 0.28).name())
        .replace("%SEP%", t.separator().name())
        .replace("%SECTION%", t.sectionDivider().name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%TEXT%", t.textPrimary.name()));
    if (m_headerAccent)
        m_headerAccent->setStyleSheet(
            QStringLiteral("background: %1;").arg(t.accent.name()));
    if (m_headerGlyph)
        m_headerGlyph->setPixmap(
            icons::icon(icons::Glyph::Mixer, t.accent, 15).pixmap(15, 15));
    update();
}

double MixerWidget::faderGainForTest(const QString& trackId) const {
    for (ChannelStrip* strip : m_strips) {
        if (strip->trackId() == trackId) return strip->faderGainForTest();
    }
    return -1.0;
}

void MixerWidget::syncFromModel(const QStringList& trackIds) {
    for (ChannelStrip* strip : m_strips)
        if (trackIds.isEmpty() || trackIds.contains(strip->trackId())) strip->syncFromModel();
}

void MixerWidget::refreshAutomationValues() {
    if (!isVisible()) return;
    for (ChannelStrip* strip : m_strips) {
        if (stripIsVisible(strip)) strip->refreshAutomationValues();
    }
}

bool MixerWidget::checkCollaborationPresenceForTest(QString* error) {
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
    if (!strip->isMaster()) {
        const QWidget* viewport = m_scroll ? m_scroll->viewport() : nullptr;
        if (!viewport) return {};
        const QRect visible(
            viewport->mapTo(const_cast<MixerWidget*>(this), QPoint{}),
            viewport->size());
        const QRect clipped = rect.intersected(visible);
        if (clipped.isEmpty()) return {};
        return clipped;
    }
    return rect;
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
    if (strip->isMaster()) return true;
    const QWidget* viewport = m_scroll ? m_scroll->viewport() : nullptr;
    if (!viewport) return false;
    const QRect stripRect(strip->mapTo(const_cast<QWidget*>(viewport), QPoint{}),
                          strip->size());
    return viewport->rect().intersects(stripRect);
}

void MixerWidget::wireStrip(ChannelStrip* strip) {
    ui::perf::sample("mixer.strip.created", 1);
        connect(strip, &ChannelStrip::selectRequested, this,
                &MixerWidget::trackSelected);
        connect(strip, &ChannelStrip::edited, this, [this, strip](bool dirty) {
            emit edited(dirty);
            emit channelEdited(strip->trackId(), dirty);
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
}

void MixerWidget::rebuild() {
    ui::perf::Scope timing("rebuild.MixerWidget.ms");
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    m_deferredRackChange = false;
    QHash<QString, ChannelStrip*> previous;
    for (auto* strip : m_strips) previous.insert(strip->trackId(), strip);
    m_strips.clear(); m_slots.clear(); m_channels.clear();
    while (auto* item = m_stripsLayout->takeAt(0)) delete item;
    const auto& project = m_controller->project();
    for (const auto& track : project.tracks) {
        if (!daw::carriesAudio(track)) continue;
        const auto id = QString::fromStdString(track.id);
        m_channels.push_back(id);
        auto* strip = previous.take(id);
        if (strip && strip->property("channelViewState").toByteArray() !=
                         ui::channelViewState(project, track.id)) {
            if (strip->hasActiveGesture()) m_deferredRackChange = true;
            else { strip->hide(); strip->deleteLater(); strip = nullptr; }
        }
        m_slots.push_back(strip);
        if (strip) m_stripsLayout->addWidget(strip);
        else m_stripsLayout->addSpacerItem(new QSpacerItem(112, 0, QSizePolicy::Fixed));
    }
    m_stripsLayout->addStretch(1);
    auto* master = previous.take(QString());
    const auto state = ui::channelViewState(project, {});
    if (master && master->property("channelViewState").toByteArray() != state) {
        if (master->hasActiveGesture()) m_deferredRackChange = true;
        else {
            m_masterHost->layout()->removeWidget(master);
            master->hide(); master->deleteLater(); master = nullptr;
        }
    }
    if (!master) {
        master = new ChannelStrip(m_controller, {}, true, m_masterHost);
        master->setProperty("channelViewState", state);
        wireStrip(master);
        m_masterHost->layout()->addWidget(master);
    }
    m_strips.push_back(master);
    for (auto* strip : previous) { strip->hide(); strip->deleteLater(); }
    // A fully virtualised console still needs its ordinary vertical extent.
    m_stripsHost->setMinimumHeight(master->naturalHeight() + 16);
    m_headerCount->setText(tr("%1 channels").arg(m_channels.size() + 1));
    m_syncingStrips = false;
    syncVisibleStrips();
    syncFromModel();
}

bool MixerWidget::eventFilter(QObject* object, QEvent* event) {
    if (m_scroll && object == m_scroll->viewport() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        syncVisibleStrips();
    return QWidget::eventFilter(object, event);
}

void MixerWidget::syncVisibleStrips() {
    ui::perf::Scope timing("syncVisibleStrips.MixerWidget.ms");
    if (m_syncingStrips || !m_scroll) return;
    if (m_deferredRackChange) {
        for (auto* strip : m_strips) {
            if (!strip->hasActiveGesture() && strip->property("channelViewState").toByteArray() !=
                ui::channelViewState(m_controller->project(), strip->trackId().toStdString())) {
                rebuild(); return;
            }
        }
    }
    const QScopedValueRollback<bool> guard(m_syncingStrips, true);
    const int left = m_scroll->horizontalScrollBar()->value();
    const int margin = std::max(234, m_scroll->viewport()->width());
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
        const int x = 8 + int(i) * 117;
        const bool pinned = (strip && strip->hasActiveGesture()) ||
            owns(strip, QApplication::focusWidget());
        const bool wanted = (x + 112 >= left - margin &&
            x <= left + m_scroll->viewport()->width() + margin) || pinned;
        if (strip && !wanted) {
            delete m_stripsLayout->takeAt(int(i));
            strip->hide(); strip->deleteLater(); strip = nullptr;
            m_stripsLayout->insertSpacerItem(int(i), new QSpacerItem(112, 0, QSizePolicy::Fixed));
        }
    }
    // Visible channels precede overscan. A large console never constructs the
    // whole viewport in one event; the frame clock resumes the remaining work.
    bool created = false;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < m_slots.size(); ++i) {
            auto*& strip = m_slots[i];
            if (strip) continue;
            const int x = 8 + int(i) * 117;
            const bool visible = x + 112 >= left && x <= left + m_scroll->viewport()->width();
            const bool nearby = x + 112 >= left - margin &&
                x <= left + m_scroll->viewport()->width() + margin;
            if (!nearby || (pass == 0) != visible) continue;
            if (created && budget.nsecsElapsed() >= 5'000'000) { pending = true; continue; }
            strip = new ChannelStrip(m_controller, m_channels[int(i)], false, m_stripsHost);
            created = true;
            strip->setProperty("channelViewState", ui::channelViewState(
                m_controller->project(), m_channels[int(i)].toStdString()));
            wireStrip(strip);
            delete m_stripsLayout->takeAt(int(i));
            m_stripsLayout->insertWidget(int(i), strip);
            strip->show();
        }
    }
    m_strips.clear();
    for (auto* strip : m_slots) {
        if (!strip) continue;
        m_stripsHost->setMinimumHeight(std::max(m_stripsHost->minimumHeight(), strip->naturalHeight() + 16));
        strip->setSelected(strip->trackId() == m_selectedTrackId);
        m_strips.push_back(strip);
    }
    if (m_materializeTimer) {
        if (pending) m_materializeTimer->start();
        else m_materializeTimer->stop();
    }
    if (master) m_strips.push_back(master);
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
