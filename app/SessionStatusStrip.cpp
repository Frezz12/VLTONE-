#include "SessionStatusStrip.hpp"

#include "CloudAssetTransferManager.hpp"
#include "CollaborationCommandBridge.hpp"
#include "CollaborationDialogStyle.hpp"
#include "CollaborationService.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "PresenceStore.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

namespace collab {
namespace {

/// One phrase per state. Colour never carries meaning alone here: the strip has
/// a status dot, but the words are what a colour-blind user reads.
QString stateText(CollaborationState state) {
    switch (state) {
        case CollaborationState::LocalOnly:      return SessionActivity::tr("Local project");
        case CollaborationState::Unavailable:
            return SessionActivity::tr("Collaboration unavailable");
        case CollaborationState::SignedOut:      return SessionActivity::tr("Sign in");
        case CollaborationState::NoConnection:   return SessionActivity::tr("No connection");
        case CollaborationState::Uploading:      return SessionActivity::tr("Uploading");
        case CollaborationState::Connecting:     return SessionActivity::tr("Connecting");
        case CollaborationState::Joining:        return SessionActivity::tr("Joining");
        case CollaborationState::Synced:         return SessionActivity::tr("Synced");
        case CollaborationState::Reconnecting:   return SessionActivity::tr("Reconnecting");
        case CollaborationState::ReadOnly:       return SessionActivity::tr("Read-only");
        case CollaborationState::Conflict:       return SessionActivity::tr("Conflict");
        case CollaborationState::Error:          return SessionActivity::tr("Session error");
    }
    return SessionActivity::tr("Session");
}

/// What the synchronizer is doing, when it is doing something worth naming.
/// Idle and Ready are deliberately empty: the state phrase already covers them.
QString syncText(CloudSyncPhase phase) {
    switch (phase) {
        case CloudSyncPhase::FetchingBootstrap:
            return SessionActivity::tr("Fetching project");
        case CloudSyncPhase::DownloadingSnapshot:
            return SessionActivity::tr("Downloading snapshot");
        case CloudSyncPhase::ReplayingOperations:
            return SessionActivity::tr("Replaying edits");
        case CloudSyncPhase::ReconcilingPending:
            return SessionActivity::tr("Reconciling local edits");
        case CloudSyncPhase::CheckingLiveSession:
            return SessionActivity::tr("Checking session");
        case CloudSyncPhase::Failed:
            return SessionActivity::tr("Sync failed");
        case CloudSyncPhase::Idle:
        case CloudSyncPhase::Ready:
            break;
    }
    return {};
}

QString publicationText(CloudPublicationPhase phase) {
    switch (phase) {
        case CloudPublicationPhase::Preflight:  return SessionActivity::tr("Checking project");
        case CloudPublicationPhase::CreatingProject:
            return SessionActivity::tr("Creating cloud project");
        case CloudPublicationPhase::UploadingAssets:
            return SessionActivity::tr("Uploading audio");
        case CloudPublicationPhase::PreparingSnapshot:
            return SessionActivity::tr("Preparing snapshot");
        case CloudPublicationPhase::UploadingSnapshot:
            return SessionActivity::tr("Uploading snapshot");
        case CloudPublicationPhase::Activating:
            return SessionActivity::tr("Activating project");
        case CloudPublicationPhase::Idle:
        case CloudPublicationPhase::Completed:
        case CloudPublicationPhase::Failed:
        case CloudPublicationPhase::Cancelled:
            break;
    }
    return {};
}

QString bytesText(quint64 bytes) {
    constexpr double kUnit = 1024.0;
    if (bytes < 1024) return SessionActivity::tr("%1 B").arg(bytes);
    double value = double(bytes) / kUnit;
    for (const char* suffix : {"KB", "MB", "GB"}) {
        if (value < kUnit || qstrcmp(suffix, "GB") == 0) {
            return QStringLiteral("%1 %2")
                .arg(value, 0, 'f', value < 10.0 ? 1 : 0)
                .arg(QString::fromLatin1(suffix));
        }
        value /= kUnit;
    }
    return SessionActivity::tr("%1 B").arg(bytes);
}

} // namespace

QString SessionActivity::headline() const {
    QStringList parts;
    parts << stateText(state);

    if (participants > 0) {
        parts << (localIsHost
                      ? SessionActivity::tr("%n person, you host", nullptr,
                                            participants)
                      : SessionActivity::tr("%n person", nullptr, participants));
    }

    // At most one work phrase, most specific first. Showing every stage at once
    // turns the strip into a wall the user stops reading.
    if (publishTotal > 0 && publication != CloudPublicationPhase::Idle) {
        parts << SessionActivity::tr("%1 %2 of %3")
                     .arg(publicationText(publication).isEmpty()
                              ? SessionActivity::tr("Publishing")
                              : publicationText(publication))
                     .arg(publishDone)
                     .arg(publishTotal);
    } else if (hydrationTotal > 0 && hydrationDone < hydrationTotal) {
        parts << SessionActivity::tr("Downloading %1 of %2 files")
                     .arg(hydrationDone)
                     .arg(hydrationTotal);
    } else if (transfersActive > 0) {
        const int total = transfersActive + transfersDone;
        parts << SessionActivity::tr("Uploading %1 of %2").arg(transfersDone + 1).arg(total);
    } else if (const QString phase = syncText(sync); !phase.isEmpty()) {
        parts << phase;
    } else if (const QString publishing = publicationText(publication);
               !publishing.isEmpty()) {
        parts << publishing;
    }

    if (pendingOperations > 0)
        parts << SessionActivity::tr("%n edit queued", nullptr, int(pendingOperations));
    if (hashRoundInFlight) parts << SessionActivity::tr("Verifying state");
    if (!warnings.isEmpty())
        parts << SessionActivity::tr("%n warning", nullptr, int(warnings.size()));

    return parts.join(QStringLiteral(" · "));
}

QString SessionActivity::details() const {
    QStringList lines;
    lines << headline();
    if (!stateDetail.isEmpty()) lines << stateDetail;
    if (!notice.isEmpty()) lines << notice;
    if (transferBytesTotal > 0) {
        lines << SessionActivity::tr("Transferred %1 of %2")
                     .arg(bytesText(transferBytesDone),
                          bytesText(transferBytesTotal));
    }
    if (readOnly)
        lines << SessionActivity::tr("This session is read-only for you right now.");
    for (const QString& warning : warnings) lines << warning;
    return lines.join(QStringLiteral("\n"));
}

bool SessionActivity::busy() const noexcept {
    return transfersActive > 0 || hashRoundInFlight ||
           (hydrationTotal > 0 && hydrationDone < hydrationTotal) ||
           (publishTotal > 0 && publishDone < publishTotal) ||
           !syncText(sync).isEmpty() ||
           !publicationText(publication).isEmpty() ||
           state == CollaborationState::Connecting ||
           state == CollaborationState::Joining ||
           state == CollaborationState::Uploading ||
           state == CollaborationState::Reconnecting;
}

int SessionActivity::progressPercent() const noexcept {
    const auto ratio = [](quint64 done, quint64 total) {
        return total == 0 ? -1
                          : int((done * 100) / std::max<quint64>(total, 1));
    };
    if (publishTotal > 0)
        return ratio(quint64(publishDone), quint64(publishTotal));
    if (hydrationTotal > 0)
        return ratio(quint64(hydrationDone), quint64(hydrationTotal));
    if (transferBytesTotal > 0)
        return ratio(transferBytesDone, transferBytesTotal);
    // Busy with nothing measurable: run indeterminate rather than invent a
    // percentage that stalls at some arbitrary number.
    return -1;
}

struct SessionStatusStrip::Impl {
    SessionStatusStrip* q = nullptr;
    SessionActivity activity;

    QLabel* dot = nullptr;
    QLabel* headline = nullptr;
    QProgressBar* progress = nullptr;
    ui::IconButton* warningsButton = nullptr;
    QToolButton* participants = nullptr;
    QMenu* participantsMenu = nullptr;
    ui::IconButton* cloudButton = nullptr;
    ui::IconButton* joinButton = nullptr;

    QPointer<CollaborationService> service;
    QPointer<CloudSessionLifecycleController> lifecycle;
    QPointer<CloudProjectSyncCoordinator> sync;
    QPointer<CloudAssetTransferManager> transfers;
    QPointer<CloudProjectAssetHydrator> hydrator;
    QPointer<CloudProjectPublisher> publisher;
    QPointer<CollaborationCommandBridge> bridge;

    struct TransferRow {
        quint64 done = 0;
        quint64 total = 0;
        bool finished = false;
    };
    QHash<quint64, TransferRow> rows;
    quint64 publishGeneration = 0;

    /// transferProgress arrives at network-packet rate. Repainting per callback
    /// would spend more time in the strip than in the transfer.
    QTimer* repaint = nullptr;
    QTimer* noticeTimer = nullptr;

    void schedule() {
        if (repaint && !repaint->isActive()) repaint->start();
    }

    void recomputeTransfers() {
        int active = 0;
        int done = 0;
        quint64 bytesDone = 0;
        quint64 bytesTotal = 0;
        for (const TransferRow& row : std::as_const(rows)) {
            if (row.finished) {
                ++done;
                bytesDone += row.total;
                bytesTotal += row.total;
                continue;
            }
            ++active;
            bytesDone += row.done;
            bytesTotal += row.total;
        }
        activity.transfersActive = active;
        activity.transfersDone = done;
        activity.transferBytesDone = bytesDone;
        activity.transferBytesTotal = bytesTotal;
        // Once nothing is moving, forget the burst so the next one starts from
        // "1 of 3" rather than continuing an old count.
        if (active == 0) rows.clear();
    }

    void refresh();
    void rebuildParticipants();
};

void SessionStatusStrip::Impl::rebuildParticipants() {
    participantsMenu->clear();
    const auto roster = service ? service->presenceStore()->participants()
                                : QVector<ParticipantIdentity>{};
    if (roster.isEmpty()) {
        QAction* empty = participantsMenu->addAction(SessionActivity::tr("No one else here"));
        empty->setEnabled(false);
        return;
    }
    for (const ParticipantIdentity& participant : roster) {
        QString suffix;
        if (participant.host) suffix += SessionActivity::tr(" — Host");
        if (!participant.role.isEmpty())
            suffix += QStringLiteral(" (%1)").arg(participant.role);
        QAction* action = participantsMenu->addAction(
            safeDisplayName(participant.nickname) + suffix);
        action->setEnabled(false);
        const QColor colour =
            participant.color.isValid()
                ? participant.color
                : PresenceStore::stableParticipantColor(
                      participant.participantId);
        QPixmap swatch(12, 12);
        swatch.fill(colour);
        action->setIcon(QIcon(swatch));
    }
}

void SessionStatusStrip::Impl::refresh() {
    if (service) {
        activity.state = service->state();
        activity.stateDetail = service->stateDetail();
        activity.readOnly = activity.state == CollaborationState::ReadOnly;
        const auto roster = service->presenceStore()->participants();
        activity.participants = int(roster.size());
        const QString host = service->hostParticipantId();
        activity.localIsHost =
            !host.isEmpty() && host == service->localParticipantId();
    }
    recomputeTransfers();

    const QString line = activity.headline();
    headline->setText(line);
    const QString detail = activity.details();
    q->setToolTip(detail);
    headline->setAccessibleName(SessionActivity::tr("Collaboration session status"));
    headline->setAccessibleDescription(detail);

    const bool busy = activity.busy();
    progress->setVisible(busy);
    if (busy) {
        const int percent = activity.progressPercent();
        if (percent < 0) {
            progress->setRange(0, 0);
        } else {
            progress->setRange(0, 100);
            progress->setValue(percent);
        }
    }

    participants->setText(QString::number(activity.participants));
    participants->setVisible(activity.participants > 0);
    warningsButton->setVisible(!activity.warnings.isEmpty());
    warningsButton->setToolTip(activity.warnings.join(QStringLiteral("\n")));
    dot->update();
}

SessionStatusStrip::SessionStatusStrip(QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->q = this;
    setObjectName(QStringLiteral("SessionStatusStrip"));
    setFixedHeight(ui::kBottomBarHeight);
    setAccessibleName(SessionActivity::tr("Collaboration session status"));

    m_impl->dot = new QLabel(this);
    m_impl->dot->setFixedSize(8, 8);

    m_impl->headline = new QLabel(this);
    m_impl->headline->setObjectName(QStringLiteral("CollabSecondary"));
    m_impl->headline->setTextFormat(Qt::PlainText);

    m_impl->progress = new QProgressBar(this);
    m_impl->progress->setFixedSize(120, 6);
    m_impl->progress->setTextVisible(false);
    m_impl->progress->hide();

    m_impl->warningsButton =
        new ui::IconButton(icons::Glyph::Warning, SessionActivity::tr("Session warnings"), this);
    m_impl->warningsButton->setButtonSize(20, 20);
    m_impl->warningsButton->setIdleColor(Theme::cycle());
    m_impl->warningsButton->hide();

    m_impl->participants = new QToolButton(this);
    m_impl->participants->setObjectName(QStringLiteral("SessionParticipants"));
    m_impl->participants->setPopupMode(QToolButton::InstantPopup);
    m_impl->participants->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_impl->participants->setIcon(
        icons::icon(icons::Glyph::Users, th().textSecondary, 14));
    m_impl->participantsMenu = new QMenu(m_impl->participants);
    m_impl->participants->setMenu(m_impl->participantsMenu);
    m_impl->participants->hide();
    connect(m_impl->participantsMenu, &QMenu::aboutToShow, this,
            [this] { m_impl->rebuildParticipants(); });

    m_impl->joinButton =
        new ui::IconButton(icons::Glyph::Link, SessionActivity::tr("Join a session…"), this);
    m_impl->joinButton->setButtonSize(20, 20);
    m_impl->cloudButton =
        new ui::IconButton(icons::Glyph::Cloud, SessionActivity::tr("Cloud projects…"), this);
    m_impl->cloudButton->setButtonSize(20, 20);
    connect(m_impl->joinButton, &QAbstractButton::clicked, this,
            &SessionStatusStrip::joinSessionRequested);
    connect(m_impl->cloudButton, &QAbstractButton::clicked, this,
            &SessionStatusStrip::cloudProjectsRequested);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(6);
    row->addWidget(m_impl->dot);
    row->addWidget(m_impl->headline);
    row->addWidget(m_impl->progress);
    row->addStretch(1);
    row->addWidget(m_impl->warningsButton);
    row->addWidget(m_impl->participants);
    row->addWidget(ui::separatorLine(Qt::Vertical, 14, this));
    row->addWidget(m_impl->joinButton);
    row->addWidget(m_impl->cloudButton);

    m_impl->repaint = new QTimer(this);
    m_impl->repaint->setSingleShot(true);
    m_impl->repaint->setInterval(100);
    connect(m_impl->repaint, &QTimer::timeout, this,
            [this] { m_impl->refresh(); });

    m_impl->noticeTimer = new QTimer(this);
    m_impl->noticeTimer->setSingleShot(true);
    connect(m_impl->noticeTimer, &QTimer::timeout, this, [this] {
        m_impl->activity.notice.clear();
        m_impl->activity.noticeIsError = false;
        m_impl->refresh();
    });

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &SessionStatusStrip::applyTheme);
    applyTheme();
    m_impl->refresh();
}

SessionStatusStrip::~SessionStatusStrip() = default;

SessionActivity SessionStatusStrip::activity() const {
    return m_impl->activity;
}

void SessionStatusStrip::showNotice(const QString& safeMessage, bool error,
                                    int timeoutMs) {
    m_impl->activity.notice =
        collab::dialog::boundedSafeMessage(safeMessage, {});
    m_impl->activity.noticeIsError = error;
    m_impl->refresh();
    if (timeoutMs > 0) m_impl->noticeTimer->start(timeoutMs);
    else m_impl->noticeTimer->stop();
}

void SessionStatusStrip::bindService(CollaborationService* service) {
    if (m_impl->service) m_impl->service->disconnect(this);
    m_impl->service = service;
    if (!service) {
        m_impl->refresh();
        return;
    }
    connect(service, &CollaborationService::stateChanged, this,
            [this] { m_impl->schedule(); });
    connect(service, &CollaborationService::hashRoundRequested, this,
            [this] {
                m_impl->activity.hashRoundInFlight = true;
                m_impl->schedule();
            });
    connect(service, &CollaborationService::roomIdentityChanged, this,
            [this] {
                m_impl->activity.hashRoundInFlight = false;
                m_impl->schedule();
            });
    connect(service, &CollaborationService::liveSessionEnded, this, [this] {
        m_impl->activity = SessionActivity{};
        m_impl->rows.clear();
        m_impl->refresh();
    });
    connect(service->presenceStore(), &PresenceStore::presenceChanged, this,
            [this] { m_impl->schedule(); });
    m_impl->refresh();
}

void SessionStatusStrip::bindLifecycle(
    CloudSessionLifecycleController* lifecycle) {
    if (m_impl->lifecycle) m_impl->lifecycle->disconnect(this);
    m_impl->lifecycle = lifecycle;
    if (!lifecycle) return;
    connect(lifecycle, &CloudSessionLifecycleController::phaseChanged, this,
            [this](CloudSessionLifecyclePhase phase) {
                m_impl->activity.lifecycle = phase;
                m_impl->schedule();
            });
    connect(lifecycle, &CloudSessionLifecycleController::userNotice, this,
            [this](const QString& safeMessage, bool error) {
                showNotice(safeMessage, error, error ? 0 : 6000);
            });
}

void SessionStatusStrip::bindSync(CloudProjectSyncCoordinator* sync) {
    if (m_impl->sync) m_impl->sync->disconnect(this);
    m_impl->sync = sync;
    if (!sync) return;
    connect(sync, &CloudProjectSyncCoordinator::phaseChanged, this,
            [this](CloudSyncPhase phase) {
                m_impl->activity.sync = phase;
                m_impl->schedule();
            });
}

void SessionStatusStrip::bindTransfers(CloudAssetTransferManager* transfers) {
    if (m_impl->transfers) m_impl->transfers->disconnect(this);
    m_impl->transfers = transfers;
    if (!transfers) return;
    connect(transfers, &CloudAssetTransferManager::transferProgress, this,
            [this](quint64 id, CloudTransferKind, quint64 done, quint64 total) {
                Impl::TransferRow& row = m_impl->rows[id];
                row.done = done;
                row.total = std::max(total, done);
                m_impl->schedule();
            });
    connect(transfers, &CloudAssetTransferManager::transferStateChanged, this,
            [this](quint64 id, CloudTransferKind, CloudTransferState state) {
                Impl::TransferRow& row = m_impl->rows[id];
                if (state == CloudTransferState::Ready) {
                    row.finished = true;
                    row.done = row.total;
                } else if (state == CloudTransferState::Failed ||
                           state == CloudTransferState::Cancelled) {
                    // A dead transfer must not sit in the denominator forever.
                    m_impl->rows.remove(id);
                }
                m_impl->schedule();
            });
    connect(transfers, &CloudAssetTransferManager::transferFailed, this,
            [this](quint64 id, CloudTransferKind, const CloudTransferError&) {
                m_impl->rows.remove(id);
                m_impl->schedule();
            });
}

void SessionStatusStrip::bindHydrator(CloudProjectAssetHydrator* hydrator) {
    if (m_impl->hydrator) m_impl->hydrator->disconnect(this);
    m_impl->hydrator = hydrator;
    if (!hydrator) return;
    connect(hydrator, &CloudProjectAssetHydrator::progressChanged, this,
            [this](qsizetype done, qsizetype total) {
                m_impl->activity.hydrationDone = done;
                m_impl->activity.hydrationTotal = total;
                m_impl->schedule();
            });
    connect(hydrator, &CloudProjectAssetHydrator::stateChanged, this,
            [this](CloudHydrationState state) {
                m_impl->activity.hydration = state;
                if (state == CloudHydrationState::Ready ||
                    state == CloudHydrationState::Idle) {
                    m_impl->activity.hydrationDone = 0;
                    m_impl->activity.hydrationTotal = 0;
                }
                m_impl->schedule();
            });
}

void SessionStatusStrip::bindPublisher(CloudProjectPublisher* publisher) {
    if (m_impl->publisher) m_impl->publisher->disconnect(this);
    m_impl->publisher = publisher;
    if (!publisher) return;
    connect(publisher, &CloudProjectPublisher::phaseChanged, this,
            [this](quint64 generation, CloudPublicationPhase phase) {
                // A publication that was superseded keeps emitting until it
                // unwinds; its numbers must not overwrite the current one.
                if (generation < m_impl->publishGeneration) return;
                m_impl->publishGeneration = generation;
                m_impl->activity.publication = phase;
                if (phase == CloudPublicationPhase::Idle ||
                    phase == CloudPublicationPhase::Completed ||
                    phase == CloudPublicationPhase::Failed ||
                    phase == CloudPublicationPhase::Cancelled) {
                    m_impl->activity.publishDone = 0;
                    m_impl->activity.publishTotal = 0;
                }
                m_impl->schedule();
            });
    connect(publisher, &CloudProjectPublisher::progressChanged, this,
            [this](quint64 generation, int done, int total) {
                if (generation < m_impl->publishGeneration) return;
                m_impl->publishGeneration = generation;
                m_impl->activity.publishDone = done;
                m_impl->activity.publishTotal = total;
                m_impl->schedule();
            });
}

void SessionStatusStrip::bindCommandBridge(CollaborationCommandBridge* bridge) {
    if (m_impl->bridge) m_impl->bridge->disconnect(this);
    m_impl->bridge = bridge;
    if (!bridge) return;
    connect(bridge, &CollaborationCommandBridge::pendingOperationCountChanged,
            this, [this](qsizetype depth) {
                m_impl->activity.pendingOperations = depth;
                m_impl->schedule();
            });
    connect(bridge, &CollaborationCommandBridge::mutationBlocked, this,
            [this](const QString& safeMessage) {
                showNotice(safeMessage, true, 8000);
            });
}

void SessionStatusStrip::applyTheme() {
    const Theme& theme = th();
    setStyleSheet(collab::dialog::styleSheet() +
                  QStringLiteral(
                      "QToolButton#SessionParticipants { color: %1; border: 0; "
                      "padding: 1px 4px; } "
                      "QToolButton#SessionParticipants:hover { background: %2; "
                      "border-radius: 4px; }")
                      .arg(theme.textSecondary.name(QColor::HexArgb),
                           theme.selection.name(QColor::HexArgb)));
    if (m_impl->participants) {
        m_impl->participants->setIcon(
            icons::icon(icons::Glyph::Users, theme.textSecondary, 14));
    }
    update();
}

void SessionStatusStrip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const Theme& theme = th();
    painter.fillRect(rect(), theme.surface);
    painter.setPen(QPen(theme.separator(), 1));
    painter.drawLine(0, 0, width(), 0);

    // The dot repeats what the words already say. It is an accent, never the
    // only carrier of the state.
    QColor mark = theme.textSecondary;
    switch (m_impl->activity.state) {
        case CollaborationState::Synced:
            mark = theme.accent;
            break;
        case CollaborationState::Conflict:
        case CollaborationState::Error:
        case CollaborationState::NoConnection:
            mark = Theme::record();
            break;
        case CollaborationState::ReadOnly:
        case CollaborationState::Reconnecting:
        case CollaborationState::Connecting:
        case CollaborationState::Joining:
        case CollaborationState::Uploading:
            mark = Theme::cycle();
            break;
        default:
            break;
    }
    if (m_impl->activity.noticeIsError) mark = Theme::record();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(mark);
    const QRect dot = m_impl->dot->geometry();
    painter.drawEllipse(QRectF(dot.x(), dot.center().y() - 3.5, 7.0, 7.0));
}

bool checkSessionStatusStripForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };

    // Idle: no work phrase, no progress, nothing to say beyond the state.
    SessionActivity idle;
    idle.state = CollaborationState::LocalOnly;
    if (idle.busy() || idle.progressPercent() != -1 ||
        !idle.headline().contains(stateText(CollaborationState::LocalOnly))) {
        return fail(QStringLiteral("an idle session reported work"));
    }

    // Hydration is reported as files, not bytes, and clears itself once done.
    SessionActivity hydrating;
    hydrating.state = CollaborationState::Joining;
    hydrating.hydrationDone = 4;
    hydrating.hydrationTotal = 12;
    if (!hydrating.busy() || hydrating.progressPercent() != 33 ||
        !hydrating.headline().contains(QStringLiteral("4")) ||
        !hydrating.headline().contains(QStringLiteral("12"))) {
        return fail(QStringLiteral("hydration progress was not reported"));
    }
    hydrating.hydrationDone = 12;
    // Asserting on the numbers rather than on the English phrasing: these
    // strings are translated, and a test that only passes in one language is
    // worse than none.
    if (hydrating.headline().contains(QStringLiteral("12"))) {
        return fail(QStringLiteral("finished hydration still reported files"));
    }

    // A publication in flight outranks everything else, and its percentage
    // comes from units rather than bytes.
    SessionActivity publishing;
    publishing.state = CollaborationState::Uploading;
    publishing.publication = CloudPublicationPhase::UploadingAssets;
    publishing.publishDone = 3;
    publishing.publishTotal = 6;
    publishing.hydrationDone = 1;
    publishing.hydrationTotal = 8;
    if (publishing.progressPercent() != 50 ||
        publishing.headline().contains(QStringLiteral("8"))) {
        return fail(QStringLiteral("publication did not outrank hydration"));
    }
    if (!publishing.headline().contains(QStringLiteral("3")) ||
        !publishing.headline().contains(QStringLiteral("6"))) {
        return fail(QStringLiteral("publication counts were not reported"));
    }

    // Byte progress only backs the bar when nothing countable is in flight.
    SessionActivity uploading;
    uploading.state = CollaborationState::Synced;
    uploading.transfersActive = 2;
    uploading.transfersDone = 1;
    uploading.transferBytesDone = 512;
    uploading.transferBytesTotal = 2048;
    if (!uploading.busy() || uploading.progressPercent() != 25) {
        return fail(QStringLiteral("transfer bytes did not drive progress"));
    }
    // Two active plus one finished reads as "2 of 3": the denominator counts
    // the whole burst so the number cannot walk backwards as transfers land.
    if (!uploading.headline().contains(QStringLiteral("2")) ||
        !uploading.headline().contains(QStringLiteral("3"))) {
        return fail(QStringLiteral("transfer counts were wrong: %1")
                        .arg(uploading.headline()));
    }

    // A queue depth is worth saying even when nothing is transferring, and it
    // must not by itself claim the session is busy.
    SessionActivity queued;
    queued.state = CollaborationState::Synced;
    queued.pendingOperations = 2;
    if (queued.busy() || queued.headline().isEmpty() ||
        !queued.headline().contains(QStringLiteral("2"))) {
        return fail(QStringLiteral("queued edits were not reported"));
    }

    // Warnings are counted in the headline and spelled out in the details.
    SessionActivity warned;
    warned.state = CollaborationState::Synced;
    warned.warnings << QStringLiteral("Gravity differs: 1.0 and 1.1");
    if (!warned.headline().contains(QStringLiteral("1")) ||
        !warned.details().contains(QStringLiteral("Gravity differs"))) {
        return fail(QStringLiteral("warnings were not surfaced"));
    }

    // Read-only and the state detail belong in the details text, not the line.
    SessionActivity readOnly;
    readOnly.state = CollaborationState::ReadOnly;
    readOnly.stateDetail = QStringLiteral("Waiting for a verified snapshot");
    readOnly.readOnly = true;
    if (!readOnly.details().contains(readOnly.stateDetail) ||
        readOnly.headline().contains(readOnly.stateDetail)) {
        return fail(QStringLiteral("state detail leaked into the headline"));
    }

    // A session with no measurable extent runs indeterminate rather than
    // pretending to a percentage.
    SessionActivity connecting;
    connecting.state = CollaborationState::Connecting;
    if (!connecting.busy() || connecting.progressPercent() != -1) {
        return fail(QStringLiteral("connecting invented a percentage"));
    }
    return true;
}

} // namespace collab
