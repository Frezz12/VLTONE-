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
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QMap>
#include <QMessageBox>
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

bool syncBusy(CloudSyncPhase phase) {
    return phase != CloudSyncPhase::Idle && phase != CloudSyncPhase::Ready &&
           phase != CloudSyncPhase::Failed;
}

QString transferName(CloudTransferKind kind) {
    switch (kind) {
        case CloudTransferKind::AssetUpload: return SessionActivity::tr("Audio upload");
        case CloudTransferKind::SnapshotUpload: return SessionActivity::tr("Project save");
        case CloudTransferKind::AssetDownload: return SessionActivity::tr("Audio download");
        case CloudTransferKind::SnapshotDownload: return SessionActivity::tr("Project download");
        case CloudTransferKind::AbortUpload: return SessionActivity::tr("Cancelling upload");
    }
    return {};
}

bool downloading(CloudTransferKind kind) {
    return kind == CloudTransferKind::AssetDownload ||
           kind == CloudTransferKind::SnapshotDownload;
}

QString modeText(const QString& mode) {
    if (mode == QLatin1String("independent")) return SessionActivity::tr("Independent listening");
    if (mode == QLatin1String("follow_host")) return SessionActivity::tr("Follow conductor");
    if (mode == QLatin1String("synchronized")) return SessionActivity::tr("Shared transport");
    return {};
}

} // namespace

QString SessionActivity::headline() const {
    QStringList parts;
    parts << stateText(state);
    if (!mode.isEmpty()) parts << modeText(mode);
    if (!conductor.isEmpty()) parts << SessionActivity::tr("Conductor: %1").arg(conductor);

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
    } else if (hydrationTotal > 0 && hydrationDone + hydrationFailed < hydrationTotal) {
        parts << SessionActivity::tr("Downloading %1 of %2 files")
                     .arg(hydrationDone)
                     .arg(hydrationTotal);
    } else if (transfersActive > 0) {
        const int total = transfersActive + transfersDone;
        parts << (transfersDownloading == transfersActive
                      ? SessionActivity::tr("Downloading %1 of %2")
                      : transfersDownloading == 0
                          ? SessionActivity::tr("Uploading %1 of %2")
                          : SessionActivity::tr("Transferring %1 of %2"))
                     .arg(transfersDone + 1).arg(total);
    } else if (const QString phase = syncText(sync); !phase.isEmpty()) {
        parts << phase;
    } else if (const QString publishing = publicationText(publication);
               !publishing.isEmpty()) {
        parts << publishing;
    }

    if (pendingOperations > 0)
        parts << SessionActivity::tr("%n edit queued", nullptr, int(pendingOperations));
    if (transfersFailed + hydrationFailed > 0)
        parts << SessionActivity::tr("%n file needs attention", nullptr,
                                     int(transfersFailed + hydrationFailed));
    if (localResultsPending > 0)
        parts << SessionActivity::tr("%n local result retained for recovery", nullptr, int(localResultsPending));
    if (persistentProblems > 0)
        parts << SessionActivity::tr("%n problem", nullptr, persistentProblems);
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
           (hydrationTotal > 0 && hydrationDone + hydrationFailed < hydrationTotal) ||
           (publishTotal > 0 && publishDone < publishTotal &&
            publication != CloudPublicationPhase::Failed &&
            publication != CloudPublicationPhase::Cancelled &&
            publication != CloudPublicationPhase::Completed) ||
           syncBusy(sync) ||
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
    QToolButton* detailsButton = nullptr;
    QFrame* popup = nullptr;
    QLabel* popupSummary = nullptr;
    QComboBox* mode = nullptr;
    QCheckBox* cursors = nullptr;
    QTreeWidget* roster = nullptr;
    QTreeWidget* tasks = nullptr;
    QPushButton* retryTask = nullptr;
    QPushButton* retryAll = nullptr;
    QPushButton* cancelTask = nullptr;
    QPushButton* taskDetails = nullptr;
    QToolButton* participantActions = nullptr;
    QTimer* hoverTimer = nullptr;
    QTimer* closeTimer = nullptr;
    bool pinned = false;
    bool mayModerate = false;
    QString sessionMode = QStringLiteral("independent");
    bool mayChangeMode = false;
    struct ParticipantStatus { QString userId, nickname, status; };
    QHash<QString, ParticipantStatus> participantStatus;
    struct AssetFailure { QString message; bool retryable = false; };
    QHash<QString, AssetFailure> assetFailures;
    QHash<QString, QString> pluginProbes;
    QHash<QString, QString> localResults;
    QMap<quint64, QString> notices;
    quint64 nextNoticeId = 0;
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
        CloudTransferKind kind = CloudTransferKind::AssetUpload;
        CloudTransferState state = CloudTransferState::Preparing;
        QString error;
        bool retryable = false;
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
        int failed = 0;
        int downloads = 0;
        quint64 bytesDone = 0;
        quint64 bytesTotal = 0;
        for (const TransferRow& row : std::as_const(rows)) {
            if (row.state == CloudTransferState::Failed) {
                ++failed;
                continue;
            }
            if (row.state == CloudTransferState::Cancelled) continue;
            if (row.finished) {
                ++done;
                bytesDone += row.total;
                bytesTotal += row.total;
                continue;
            }
            ++active;
            if (downloading(row.kind)) ++downloads;
            bytesDone += row.done;
            bytesTotal += row.total;
        }
        activity.transfersActive = active;
        activity.transfersDone = done;
        activity.transfersFailed = failed;
        activity.transfersDownloading = downloads;
        activity.transferBytesDone = bytesDone;
        activity.transferBytesTotal = bytesTotal;
        // Once nothing is moving, forget the burst so the next one starts from
        // "1 of 3" rather than continuing an old count.
        if (active == 0) {
            for (auto it = rows.begin(); it != rows.end();) {
                if (it->finished || it->state == CloudTransferState::Cancelled)
                    it = rows.erase(it);
                else ++it;
            }
        }
    }

    void refresh();
    void rebuildParticipants();
    void ensurePopup();
    void showPopup(bool pin);
    void refreshPopup();
    void updateTaskActions();
    void taskAction(bool retry);
    void retryAllTasks();
    void retryDownloads();
    void showTaskDetails();
    void populateParticipantActions(QMenu* menu);
};

void SessionStatusStrip::Impl::ensurePopup() {
    if (popup) return;
    popup = new QFrame(q, Qt::Tool | Qt::FramelessWindowHint);
    popup->setObjectName(QStringLiteral("SessionDetails"));
    popup->setWindowTitle(SessionActivity::tr("Session details"));
    popup->setAccessibleName(popup->windowTitle());
    popup->setAttribute(Qt::WA_ShowWithoutActivating);
    popup->resize(580, 460);
    popup->installEventFilter(q);
    auto* column = new QVBoxLayout(popup);
    column->setContentsMargins(16, 14, 16, 14);
    column->setSpacing(10);
    auto* heading = new QHBoxLayout;
    heading->addWidget(dialog::titleLabel(SessionActivity::tr("Session"), popup));
    heading->addStretch();
    auto* close = new QPushButton(SessionActivity::tr("Close"), popup);
    heading->addWidget(close);
    column->addLayout(heading);
    QObject::connect(close, &QPushButton::clicked, q, [this] {
        popup->hide(); pinned = false; detailsButton->setFocus();
    });
    popupSummary = new QLabel(popup);
    popupSummary->setTextFormat(Qt::PlainText);
    popupSummary->setWordWrap(true);
    column->addWidget(popupSummary);
    auto* controls = new QHBoxLayout;
    mode = new QComboBox(popup);
    mode->setAccessibleName(SessionActivity::tr("Synchronization mode"));
    mode->addItem(SessionActivity::tr("Independent listening"), QStringLiteral("independent"));
    mode->addItem(SessionActivity::tr("Follow conductor"), QStringLiteral("follow_host"));
    mode->addItem(SessionActivity::tr("Shared transport"), QStringLiteral("synchronized"));
    controls->addWidget(mode, 1);
    cursors = new QCheckBox(SessionActivity::tr("Show other cursors"), popup);
    cursors->setChecked(service ? service->presenceStore()->remoteCursorsVisible()
        : QSettings().value(QStringLiteral("collaboration/showRemoteCursors"), true).toBool());
    controls->addWidget(cursors);
    column->addLayout(controls);
    QObject::connect(mode, &QComboBox::activated, q, [this](int index) {
        const QString requested = mode->itemData(index).toString();
        const QSignalBlocker blocker(mode);
        mode->setCurrentIndex(mode->findData(sessionMode));
        if (mayChangeMode) emit q->sessionModeRequested(requested);
    });
    QObject::connect(cursors, &QCheckBox::toggled, q, [this](bool visible) {
        QSettings().setValue(QStringLiteral("collaboration/showRemoteCursors"), visible);
        if (service) service->presenceStore()->setRemoteCursorsVisible(visible);
    });
    roster = new QTreeWidget(popup);
    roster->setHeaderLabels({SessionActivity::tr("Participant"), SessionActivity::tr("Status")});
    roster->setAccessibleName(SessionActivity::tr("Participants and readiness"));
    roster->setRootIsDecorated(false);
    roster->setMinimumHeight(100);
    roster->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    roster->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    column->addWidget(roster, 1);
    auto* rosterActions = new QHBoxLayout;
    participantActions = new QToolButton(popup);
    participantActions->setText(SessionActivity::tr("Participant actions"));
    participantActions->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(participantActions);
    participantActions->setMenu(menu);
    QObject::connect(menu, &QMenu::aboutToShow, q, [this, menu] {
        populateParticipantActions(menu);
    });
    rosterActions->addWidget(participantActions);
    rosterActions->addStretch();
    auto* settings = new QPushButton(SessionActivity::tr("Session settings…"), popup);
    rosterActions->addWidget(settings);
    QObject::connect(settings, &QPushButton::clicked, q, [this] {
        popup->hide(); pinned = false; emit q->sessionSettingsRequested();
    });
    column->addLayout(rosterActions);
    tasks = new QTreeWidget(popup);
    tasks->setHeaderLabels({SessionActivity::tr("Activity"), SessionActivity::tr("Status")});
    tasks->setAccessibleName(SessionActivity::tr("Transfers and problems"));
    tasks->setRootIsDecorated(false);
    tasks->setMinimumHeight(100);
    tasks->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tasks->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    column->addWidget(tasks, 1);
    auto* actions = new QHBoxLayout;
    retryTask = new QPushButton(SessionActivity::tr("Retry"), popup);
    retryAll = new QPushButton(SessionActivity::tr("Retry all"), popup);
    cancelTask = new QPushButton(SessionActivity::tr("Cancel"), popup);
    taskDetails = new QPushButton(SessionActivity::tr("Details…"), popup);
    actions->addWidget(retryTask);
    actions->addWidget(retryAll);
    actions->addWidget(cancelTask);
    actions->addStretch();
    actions->addWidget(taskDetails);
    column->addLayout(actions);
    QObject::connect(tasks, &QTreeWidget::itemSelectionChanged, q,
                     [this] { updateTaskActions(); });
    QObject::connect(retryTask, &QPushButton::clicked, q, [this] { taskAction(true); });
    QObject::connect(retryAll, &QPushButton::clicked, q, [this] { retryAllTasks(); });
    QObject::connect(cancelTask, &QPushButton::clicked, q, [this] { taskAction(false); });
    QObject::connect(taskDetails, &QPushButton::clicked, q, [this] { showTaskDetails(); });
    QObject::connect(tasks, &QTreeWidget::itemDoubleClicked, q, [this] { showTaskDetails(); });
    popup->setStyleSheet(dialog::styleSheet());
}

void SessionStatusStrip::Impl::showPopup(bool pin) {
    ensurePopup();
    closeTimer->stop();
    pinned = pinned || pin;
    refreshPopup();
    const QRect available = q->screen()->availableGeometry();
    popup->resize(std::min(580, available.width()), std::min(460, available.height()));
    const QPoint anchor = q->mapToGlobal(QPoint(0, 0));
    popup->move(std::clamp(anchor.x(), available.left(),
                          std::max(available.left(), available.right() - popup->width() + 1)),
                std::clamp(anchor.y() - popup->height() - 4, available.top(),
                          std::max(available.top(), available.bottom() - popup->height() + 1)));
    popup->show();
    popup->raise();
    if (pin) { popup->activateWindow(); roster->setFocus(); }
}

void SessionStatusStrip::Impl::populateParticipantActions(QMenu* menu) {
    menu->clear();
    const auto* item = roster->currentItem();
    const QString participantId = item ? item->data(0, Qt::UserRole).toString() : QString();
    const QString userId = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
    const bool other = service && !participantId.isEmpty() &&
        participantId != service->localParticipantId();
    auto* lead = menu->addAction(SessionActivity::tr("Make conductor"));
    lead->setEnabled(other && mayModerate);
    QObject::connect(lead, &QAction::triggered, q, [this, participantId] {
        emit q->participantLeadRequested(participantId);
    });
    const std::pair<const char*, QString> actions[] = {
        {"readmit", SessionActivity::tr("Readmit to session")},
        {"kick", SessionActivity::tr("Remove from session")},
        {"ban", SessionActivity::tr("Block from project")},
        {"unban", SessionActivity::tr("Unblock")}};
    for (const auto& [code, title] : actions) {
        auto* action = menu->addAction(title);
        action->setEnabled(other && mayModerate && !userId.isEmpty());
        QObject::connect(action, &QAction::triggered, q,
                         [this, participantId, userId, code] {
            emit q->participantModerationRequested(participantId, userId,
                                                    QString::fromLatin1(code));
        });
    }
}

void SessionStatusStrip::Impl::refreshPopup() {
    if (!popup) return;
    popupSummary->setText(activity.details());
    mode->setCurrentIndex(mode->findData(sessionMode));
    mode->setEnabled(mayChangeMode);
    const QString selected = roster->currentItem()
        ? roster->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const int rosterScroll = roster->verticalScrollBar()->value();
    roster->clear();
    QSet<QString> included;
    const auto addPerson = [&](const QString& id, const QString& user,
                               const QString& name, const QString& status) {
        auto* item = new QTreeWidgetItem(roster, {name, status});
        item->setData(0, Qt::UserRole, id);
        item->setData(0, Qt::UserRole + 1, user);
        if (id == selected) roster->setCurrentItem(item);
        included.insert(id);
    };
    if (service) {
        for (const auto& person : service->presenceStore()->participants()) {
            QStringList status;
            status << (person.online ? SessionActivity::tr("Online") : SessionActivity::tr("Offline"));
            if (person.host) status << SessionActivity::tr("Host");
            if (!person.role.isEmpty()) status << person.role;
            if (participantStatus.contains(person.participantId))
                status << participantStatus.value(person.participantId).status;
            addPerson(person.participantId, person.userId,
                      safeDisplayName(person.nickname), status.join(QStringLiteral(" · ")));
        }
    }
    for (auto it = participantStatus.cbegin(); it != participantStatus.cend(); ++it) {
        if (!included.contains(it.key()))
            addPerson(it.key(), it->userId, safeDisplayName(it->nickname), it->status);
    }
    roster->verticalScrollBar()->setValue(rosterScroll);
    const QString selectedTask = tasks->currentItem()
        ? tasks->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const int taskScroll = tasks->verticalScrollBar()->value();
    const QSignalBlocker blocker(tasks);
    tasks->clear();
    const auto addTask = [&](const QString& id, const QString& name,
                            const QString& status, bool retry, bool cancel, int kind) {
        auto* item = new QTreeWidgetItem(tasks, {name, status.left(180)});
        item->setToolTip(1, status);
        item->setData(0, Qt::UserRole, id);
        item->setData(0, Qt::UserRole + 1, retry);
        item->setData(0, Qt::UserRole + 2, cancel);
        item->setData(0, Qt::UserRole + 3, kind);
        if (id == selectedTask) tasks->setCurrentItem(item);
    };
    for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
        if (it->finished) continue;
        QString status = it->state == CloudTransferState::Failed
            ? it->error : it->total > 0
                ? SessionActivity::tr("%1 of %2").arg(bytesText(it->done), bytesText(it->total))
                : SessionActivity::tr("In progress");
        if (it->state != CloudTransferState::Failed && !it->error.isEmpty())
            status += QStringLiteral("\n") + it->error;
        addTask(QString::number(it.key()), transferName(it->kind), status,
                it->state == CloudTransferState::Failed && it->retryable,
                !downloading(it->kind), int(it->kind));
    }
    for (auto it = assetFailures.cbegin(); it != assetFailures.cend(); ++it)
        addTask(QStringLiteral("asset:") + it.key(), SessionActivity::tr("Audio download"),
                it->message, it->retryable, false, int(CloudTransferKind::AssetDownload));
    for (auto it = pluginProbes.cbegin(); it != pluginProbes.cend(); ++it) {
        const bool checking = it.value() == QLatin1String("checking");
        const QString detail = checking ? SessionActivity::tr("Checking plugin state compatibility…")
            : it.value() == QLatin1String("missing")
                ? SessionActivity::tr("Plugin or required asset is unavailable. Install or rescan the matching plugin and retry.")
                : SessionActivity::tr("Plugin state validation failed. Rescan the plugin and retry.");
        addTask(QStringLiteral("plugin:") + it.key(), SessionActivity::tr("External plugin"),
                detail, !checking, false, -1);
    }
    for (auto it = localResults.cbegin(); it != localResults.cend(); ++it)
        addTask(QStringLiteral("recovery:") + it.key(), it.value(),
                SessionActivity::tr("Local result retained for recovery"), true, false, -1);
    if (activity.sync == CloudSyncPhase::Failed)
        addTask(QStringLiteral("sync"), SessionActivity::tr("Open project"),
                activity.notice.isEmpty() ? activity.stateDetail : activity.notice, true, false, -1);
    if (activity.publication == CloudPublicationPhase::Failed)
        addTask(QStringLiteral("publication"), SessionActivity::tr("Create cloud project"),
                activity.notice, true, false, -1);
    for (auto it = notices.cbegin(); it != notices.cend(); ++it)
        addTask(QStringLiteral("notice:") + QString::number(it.key()), SessionActivity::tr("Session error"),
                it.value(), false, true, -1);
    if (!tasks->currentItem() && tasks->topLevelItemCount())
        tasks->setCurrentItem(tasks->topLevelItem(0));
    tasks->verticalScrollBar()->setValue(taskScroll);
    updateTaskActions();
}

void SessionStatusStrip::Impl::updateTaskActions() {
    const auto* item = tasks->currentItem();
    const bool recovery = item && item->data(0, Qt::UserRole).toString().startsWith(QLatin1String("recovery:"));
    const bool notice = item && item->data(0, Qt::UserRole).toString().startsWith(QLatin1String("notice:"));
    retryTask->setText(recovery ? SessionActivity::tr("Recover…") : SessionActivity::tr("Retry"));
    retryTask->setEnabled(item && item->data(0, Qt::UserRole + 1).toBool());
    cancelTask->setText(notice ? SessionActivity::tr("Dismiss") : SessionActivity::tr("Cancel all uploads"));
    cancelTask->setEnabled(item && item->data(0, Qt::UserRole + 2).toBool());
    taskDetails->setEnabled(item != nullptr);
    bool retryable = false;
    for (int index = 0; index < tasks->topLevelItemCount(); ++index) {
        const auto* task = tasks->topLevelItem(index);
        if (task->data(0, Qt::UserRole + 1).toBool() &&
            !task->data(0, Qt::UserRole).toString().startsWith(QLatin1String("recovery:"))) retryable = true;
    }
    retryAll->setEnabled(retryable);
}

void SessionStatusStrip::Impl::taskAction(bool retry) {
    const auto* item = tasks->currentItem();
    if (!item || !item->data(0, Qt::UserRole + (retry ? 1 : 2)).toBool()) return;
    const QString id = item->data(0, Qt::UserRole).toString();
    const auto kind = CloudTransferKind(item->data(0, Qt::UserRole + 3).toInt());
    if (!retry && id.startsWith(QLatin1String("notice:"))) {
        const QString dismissed = notices.take(id.mid(7).toULongLong());
        if (activity.notice == dismissed) { activity.notice.clear(); activity.noticeIsError = false; }
        refresh();
        refreshPopup();
        return;
    }
    if (!retry) { emit q->cancelUploadsRequested(); return; }
    if (id == QLatin1String("publication")) emit q->retryPublicationRequested();
    else if (id.startsWith(QLatin1String("recovery:"))) emit q->localResultRecoveryRequested(id.mid(9));
    else if (id.startsWith(QLatin1String("plugin:"))) emit q->retryPluginProbesRequested();
    else if (id == QLatin1String("sync") || kind == CloudTransferKind::SnapshotDownload)
        emit q->reconnectRequested();
    else if (kind == CloudTransferKind::AssetDownload) retryDownloads();
    else emit q->retryUploadsRequested();
}

void SessionStatusStrip::Impl::retryDownloads() {
    if (!hydrator) return;
    hydrator->retryFailed();
    activity.hydrationFailed = hydrator->failedCount();
    schedule();
}

void SessionStatusStrip::Impl::retryAllTasks() {
    // Coordinators own complete transactions. Retry each owner once, even when
    // several files in that transaction failed. Recovery needs a user choice.
    bool uploads = false, downloads = false, plugins = false;
    bool publication = false, reconnect = false;
    for (int index = 0; index < tasks->topLevelItemCount(); ++index) {
        const auto* item = tasks->topLevelItem(index);
        if (!item->data(0, Qt::UserRole + 1).toBool()) continue;
        const QString id = item->data(0, Qt::UserRole).toString();
        const auto kind = CloudTransferKind(item->data(0, Qt::UserRole + 3).toInt());
        if (id.startsWith(QLatin1String("recovery:"))) continue;
        if (id == QLatin1String("publication")) publication = true;
        else if (id.startsWith(QLatin1String("plugin:"))) plugins = true;
        else if (id == QLatin1String("sync") || kind == CloudTransferKind::SnapshotDownload) reconnect = true;
        else if (kind == CloudTransferKind::AssetDownload) downloads = true;
        else uploads = true;
    }
    if (reconnect) emit q->reconnectRequested();
    if (publication) emit q->retryPublicationRequested();
    if (uploads) emit q->retryUploadsRequested();
    if (downloads) retryDownloads();
    if (plugins) emit q->retryPluginProbesRequested();
}

void SessionStatusStrip::Impl::showTaskDetails() {
    const auto* item = tasks->currentItem();
    if (!item) return;
    auto* detail = new QMessageBox(QMessageBox::Information, SessionActivity::tr("Session details"),
        item->text(0), QMessageBox::Close, popup);
    detail->setAttribute(Qt::WA_DeleteOnClose);
    detail->setTextFormat(Qt::PlainText);
    detail->setInformativeText(item->toolTip(1));
    detail->open();
}

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
        activity.hashRoundInFlight = service->hashRoundInFlight();
        const auto roster = service->presenceStore()->participants();
        activity.participants = int(roster.size());
        const QString host = service->hostParticipantId();
        activity.localIsHost =
            !host.isEmpty() && host == service->localParticipantId();
        activity.mode = service->sessionId().isEmpty() ? QString() : sessionMode;
        const auto conductor = service->presenceStore()->participantById(host);
        activity.conductor = conductor ? safeDisplayName(conductor->nickname) : QString();
    }
    recomputeTransfers();
    activity.localResultsPending = localResults.size();
    activity.persistentProblems = int(notices.size());
    for (auto it = pluginProbes.cbegin(); it != pluginProbes.cend(); ++it)
        if (it.value() != QLatin1String("checking")) ++activity.persistentProblems;

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
    warningsButton->setVisible(!activity.warnings.isEmpty() || activity.persistentProblems > 0 ||
        activity.transfersFailed > 0 || activity.hydrationFailed > 0);
    warningsButton->setToolTip(detail);
    q->update();
    if (popup && popup->isVisible()) refreshPopup();
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
    m_impl->headline->setMinimumWidth(0);
    m_impl->headline->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

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
    m_impl->detailsButton = new QToolButton(this);
    m_impl->detailsButton->setText(SessionActivity::tr("Session details"));
    m_impl->detailsButton->setAccessibleDescription(
        SessionActivity::tr("Preview participants and transfers. Click to keep open; Escape closes."));
    m_impl->detailsButton->setMinimumHeight(24);
    row->addWidget(m_impl->detailsButton);
    connect(m_impl->detailsButton, &QToolButton::clicked, this, [this] {
        m_impl->showPopup(true);
    });
    connect(m_impl->warningsButton, &QAbstractButton::clicked, this, [this] {
        m_impl->showPopup(true);
    });
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
    m_impl->hoverTimer = new QTimer(this);
    m_impl->hoverTimer->setSingleShot(true);
    m_impl->hoverTimer->setInterval(300);
    connect(m_impl->hoverTimer, &QTimer::timeout, this, [this] {
        if (isVisible()) m_impl->showPopup(false);
    });
    m_impl->closeTimer = new QTimer(this);
    m_impl->closeTimer->setSingleShot(true);
    m_impl->closeTimer->setInterval(180);
    connect(m_impl->closeTimer, &QTimer::timeout, this, [this] {
        if (m_impl->popup && !m_impl->pinned && !underMouse() &&
            !m_impl->popup->underMouse()) m_impl->popup->hide();
    });
    installEventFilter(this);
    m_impl->detailsButton->installEventFilter(this);
    qApp->installEventFilter(this);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &SessionStatusStrip::applyTheme);
    applyTheme();
    m_impl->refresh();
}

SessionStatusStrip::~SessionStatusStrip() {
    if (qApp) qApp->removeEventFilter(this);
}

bool SessionStatusStrip::eventFilter(QObject* watched, QEvent* event) {
    const bool trigger = watched == this || watched == m_impl->detailsButton;
    if (trigger && (event->type() == QEvent::Enter || event->type() == QEvent::FocusIn))
        m_impl->hoverTimer->start();
    if ((trigger || watched == m_impl->popup) && event->type() == QEvent::Leave) {
        m_impl->hoverTimer->stop();
        m_impl->closeTimer->start();
    }
    if (watched == m_impl->popup && event->type() == QEvent::Enter)
        m_impl->closeTimer->stop();
    if (watched == this && event->type() == QEvent::Hide) {
        m_impl->hoverTimer->stop();
        if (m_impl->popup) m_impl->popup->hide();
        m_impl->pinned = false;
    }
    if (m_impl->popup && m_impl->popup->isVisible()) {
        auto* widget = qobject_cast<QWidget*>(watched);
        const bool inside = widget && (widget == m_impl->popup ||
                                        m_impl->popup->isAncestorOf(widget));
        if (event->type() == QEvent::KeyPress && inside &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            m_impl->popup->hide(); m_impl->pinned = false;
            m_impl->detailsButton->setFocus();
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress && widget) {
            if (inside) m_impl->pinned = true;
            else if (widget != this && !isAncestorOf(widget) &&
                     !qobject_cast<QMenu*>(widget)) {
                m_impl->popup->hide(); m_impl->pinned = false;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SessionStatusStrip::setSessionMode(const QString& mode, bool mayChange) {
    if (mode != QLatin1String("independent") && mode != QLatin1String("follow_host") &&
        mode != QLatin1String("synchronized")) return;
    m_impl->sessionMode = mode;
    m_impl->mayChangeMode = mayChange;
    m_impl->schedule();
}

void SessionStatusStrip::setMayModerate(bool allowed) {
    m_impl->mayModerate = allowed;
}

void SessionStatusStrip::setParticipantStatus(const QString& participantId,
    const QString& userId, const QString& nickname, const QString& status) {
    if (participantId.isEmpty()) return;
    if (status.isEmpty()) {
        m_impl->participantStatus.remove(safeSemanticId(participantId));
        m_impl->schedule();
        return;
    }
    m_impl->participantStatus.insert(safeSemanticId(participantId),
        {safeSemanticId(userId), safeDisplayName(nickname), status.left(160)});
    m_impl->schedule();
}

void SessionStatusStrip::refreshDetails() { m_impl->refresh(); }

void SessionStatusStrip::setPluginProbeStatus(const QString& insertId, const QString& status) {
    if (status.isEmpty() || status == QLatin1String("ready")) m_impl->pluginProbes.remove(insertId);
    else m_impl->pluginProbes.insert(insertId, status);
    m_impl->schedule();
}

void SessionStatusStrip::setLocalResultRecovery(const QString& path, const QString& label) {
    if (path.isEmpty()) return;
    if (label.isEmpty()) m_impl->localResults.remove(path);
    else m_impl->localResults.insert(path, dialog::boundedSafeMessage(label, SessionActivity::tr("Local result")));
    m_impl->activity.localResultsPending = m_impl->localResults.size();
    m_impl->schedule();
}

SessionActivity SessionStatusStrip::activity() const {
    return m_impl->activity;
}

void SessionStatusStrip::showNotice(const QString& safeMessage, bool error,
                                    int timeoutMs) {
    m_impl->activity.notice =
        collab::dialog::boundedSafeMessage(safeMessage, {});
    m_impl->activity.noticeIsError = error;
    if (error && !m_impl->activity.notice.isEmpty() &&
        !m_impl->notices.values().contains(m_impl->activity.notice))
        m_impl->notices.insert(++m_impl->nextNoticeId, m_impl->activity.notice);
    m_impl->refresh();
    if (timeoutMs > 0) m_impl->noticeTimer->start(timeoutMs);
    else m_impl->noticeTimer->stop();
}

void SessionStatusStrip::bindService(CollaborationService* service) {
    if (m_impl->service) {
        m_impl->service->presenceStore()->disconnect(this);
        m_impl->service->disconnect(this);
    }
    m_impl->service = service;
    if (!service) {
        m_impl->refresh();
        return;
    }
    connect(service, &CollaborationService::stateChanged, this,
            [this] { m_impl->schedule(); });
    connect(service, &CollaborationService::hashRoundChanged, this,
            [this](bool active) {
                m_impl->activity.hashRoundInFlight = active;
                m_impl->schedule();
            });
    connect(service, &CollaborationService::projectChanged, this, [this] {
        m_impl->rows.clear();
        m_impl->assetFailures.clear();
        m_impl->pluginProbes.clear();
        m_impl->participantStatus.clear();
        m_impl->notices.clear();
        m_impl->activity = SessionActivity{};
        m_impl->refresh();
    });
    connect(service, &CollaborationService::participantReadinessChanged, this,
            [this](const QString& id, const QString& role, const QString& readiness, qint64) {
                auto& status = m_impl->participantStatus[id];
                if (m_impl->service) {
                    if (auto person = m_impl->service->presenceStore()->participantById(id)) {
                        status.userId = person->userId;
                        status.nickname = person->nickname;
                    }
                }
                status.status = role + QStringLiteral(" · ") + readiness;
                m_impl->schedule();
            });
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
    connect(service->presenceStore(), &PresenceStore::remoteCursorsVisibleChanged,
            this, [this](bool visible) {
                if (m_impl->cursors) {
                    const QSignalBlocker blocker(m_impl->cursors);
                    m_impl->cursors->setChecked(visible);
                }
            });
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
                if (phase == CloudSyncPhase::Ready) {
                    for (auto it = m_impl->rows.begin(); it != m_impl->rows.end();) {
                        if (it->kind == CloudTransferKind::SnapshotDownload &&
                            it->state == CloudTransferState::Failed)
                            it = m_impl->rows.erase(it);
                        else ++it;
                    }
                }
                m_impl->schedule();
            });
    connect(sync, &CloudProjectSyncCoordinator::synchronizationFailed, this,
            [this](quint64, const CloudSyncError& error) {
                showNotice(error.safeMessage, true, 0);
            });
}

void SessionStatusStrip::bindTransfers(CloudAssetTransferManager* transfers) {
    if (m_impl->transfers) m_impl->transfers->disconnect(this);
    m_impl->transfers = transfers;
    if (!transfers) return;
    connect(transfers, &CloudAssetTransferManager::transferProgress, this,
            [this](quint64 id, CloudTransferKind kind, quint64 done, quint64 total) {
                Impl::TransferRow& row = m_impl->rows[id];
                row.kind = kind;
                row.done = done;
                row.total = std::max(total, done);
                m_impl->schedule();
            });
    connect(transfers, &CloudAssetTransferManager::transferStateChanged, this,
            [this](quint64 id, CloudTransferKind kind, CloudTransferState state) {
                Impl::TransferRow& row = m_impl->rows[id];
                row.kind = kind;
                row.state = state;
                row.finished = state == CloudTransferState::Ready;
                if (state == CloudTransferState::Ready) {
                    row.done = row.total;
                    row.error.clear(); row.retryable = false;
                } else if (state == CloudTransferState::Cancelled ||
                           (kind == CloudTransferKind::AssetDownload &&
                            state == CloudTransferState::Failed)) {
                    // The hydrator owns automatic retries and reports one
                    // terminal failure per logical asset below.
                    m_impl->rows.remove(id);
                } else if (state != CloudTransferState::Failed) {
                    row.retryable = false;
                }
                m_impl->schedule();
            });
    connect(transfers, &CloudAssetTransferManager::transferFailed, this,
            [this](quint64 id, CloudTransferKind kind, const CloudTransferError& error) {
                if (error.code == CloudTransferErrorCode::Cancelled) {
                    m_impl->rows.remove(id);
                    m_impl->schedule();
                    return;
                }
                if (kind == CloudTransferKind::AssetDownload) return;
                auto& row = m_impl->rows[id];
                row.kind = kind;
                row.state = CloudTransferState::Failed;
                row.finished = false;
                row.error = dialog::boundedSafeMessage(error.safeMessage,
                    SessionActivity::tr("Transfer failed"));
                if (!error.apiCode.isEmpty())
                    row.error += QStringLiteral(" (%1)").arg(error.apiCode.left(64));
                row.retryable = error.retryable;
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
                m_impl->activity.hydrationFailed = m_impl->hydrator
                    ? m_impl->hydrator->failedCount() : 0;
                if (state == CloudHydrationState::Ready ||
                    state == CloudHydrationState::Idle) {
                    m_impl->activity.hydrationDone = 0;
                    m_impl->activity.hydrationTotal = 0;
                    m_impl->assetFailures.clear();
                }
                m_impl->schedule();
            });
    connect(hydrator, &CloudProjectAssetHydrator::assetUnavailable, this,
            [this](const QString& id, const QString& message, bool retryable) {
                m_impl->assetFailures.insert(id, {
                    dialog::boundedSafeMessage(message, SessionActivity::tr("Audio unavailable")), retryable});
                m_impl->activity.hydrationFailed = m_impl->hydrator->failedCount();
                m_impl->schedule();
            });
    connect(hydrator, &CloudProjectAssetHydrator::hydrationSettled, this,
            [this](const QString&, qsizetype failures) {
                m_impl->activity.hydrationFailed = failures;
                if (failures == 0) m_impl->assetFailures.clear();
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
    connect(publisher, &CloudProjectPublisher::publicationFailed, this,
            [this](quint64, CloudPublicationPhase, const QString&,
                   const QString& message, bool) { showNotice(message, true, 0); });
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
    if (m_impl->popup)
        m_impl->popup->setStyleSheet(collab::dialog::styleSheet());
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
    QLinearGradient surface(0, 0, 0, height());
    surface.setColorAt(0, theme.panelTop());
    surface.setColorAt(1, theme.panelBottom());
    painter.fillRect(rect(), surface);
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
    if (m_impl->activity.noticeIsError || m_impl->activity.persistentProblems > 0 ||
        m_impl->activity.transfersFailed > 0 || m_impl->activity.hydrationFailed > 0) mark = Theme::record();
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
    SessionActivity failed;
    failed.state = CollaborationState::Synced;
    failed.sync = CloudSyncPhase::Failed;
    failed.transfersFailed = 1;
    failed.hydrationDone = 1;
    failed.hydrationFailed = 2;
    failed.hydrationTotal = 3;
    if (failed.busy() || !failed.headline().contains(QStringLiteral("3")))
        return fail(QStringLiteral("settled failures looked like ongoing transfers"));
    SessionActivity download;
    download.state = CollaborationState::Synced;
    download.transfersActive = download.transfersDownloading = 1;
    if (!download.headline().contains(SessionActivity::tr("Downloading %1 of %2").arg(1).arg(1)))
        return fail(QStringLiteral("downloads were labelled as uploads"));
    SessionActivity recovery;
    recovery.localResultsPending = 2;
    recovery.publication = CloudPublicationPhase::Failed;
    recovery.publishTotal = 3;
    recovery.publishDone = 1;
    if (recovery.busy() || !recovery.headline().contains(QStringLiteral("2")))
        return fail(QStringLiteral("retained local results were hidden or counted as transfers"));
    SessionActivity shared;
    shared.mode = QStringLiteral("synchronized");
    shared.conductor = QStringLiteral("Alice");
    shared.persistentProblems = 2;
    if (!shared.headline().contains(modeText(shared.mode)) ||
        !shared.headline().contains(shared.conductor) || shared.busy())
        return fail(QStringLiteral("the compact status lost the mode, conductor or settled problems"));

    if (qobject_cast<QApplication*>(QCoreApplication::instance())) {
        SessionStatusStrip strip;
        const QString message = QStringLiteral("Retained transfer error: ") + QString(190, QLatin1Char('x'));
        strip.showNotice(message, true, 1);
        strip.showNotice(message, true, 1);
        strip.showNotice(QStringLiteral("Connected again"), false, 1);
        QMetaObject::invokeMethod(strip.m_impl->noticeTimer, "timeout", Qt::DirectConnection);
        strip.m_impl->ensurePopup();
        strip.m_impl->refreshPopup();
        if (strip.activity().persistentProblems != 1 || strip.m_impl->tasks->topLevelItemCount() != 1 ||
            strip.m_impl->tasks->topLevelItem(0)->toolTip(1) != message ||
            !strip.m_impl->taskDetails->isEnabled() || strip.m_impl->retryAll->isEnabled())
            return fail(QStringLiteral("an error vanished, duplicated or lost its full details after a normal status"));
        strip.m_impl->cancelTask->click();
        if (strip.activity().persistentProblems != 0 || strip.m_impl->tasks->topLevelItemCount() != 0)
            return fail(QStringLiteral("explicit dismissal did not remove the retained error"));

        auto& upload = strip.m_impl->rows[1];
        upload.state = CloudTransferState::Failed; upload.retryable = true;
        upload.error = QStringLiteral("Upload unavailable");
        strip.m_impl->rows[2] = upload;
        strip.m_impl->pluginProbes.insert(QStringLiteral("plugin1"), QStringLiteral("failed"));
        strip.m_impl->pluginProbes.insert(QStringLiteral("plugin2"), QStringLiteral("checking"));
        strip.m_impl->activity.publication = CloudPublicationPhase::Failed;
        strip.m_impl->activity.sync = CloudSyncPhase::Failed;
        strip.m_impl->localResults.insert(QStringLiteral("retained-result"), QStringLiteral("Recording"));
        strip.m_impl->refreshPopup();
        int uploads = 0, plugins = 0, publications = 0, reconnects = 0, recoveries = 0;
        QObject::connect(&strip, &SessionStatusStrip::retryUploadsRequested, &strip, [&] { ++uploads; });
        QObject::connect(&strip, &SessionStatusStrip::retryPluginProbesRequested, &strip, [&] { ++plugins; });
        QObject::connect(&strip, &SessionStatusStrip::retryPublicationRequested, &strip, [&] { ++publications; });
        QObject::connect(&strip, &SessionStatusStrip::reconnectRequested, &strip, [&] { ++reconnects; });
        QObject::connect(&strip, &SessionStatusStrip::localResultRecoveryRequested, &strip, [&] { ++recoveries; });
        strip.m_impl->retryAll->click();
        if (uploads != 1 || plugins != 1 || publications != 1 || reconnects != 1 || recoveries != 0)
            return fail(QStringLiteral("Retry all repeated a transaction or implicitly chose a recovery action"));
        const QString screenshot = qEnvironmentVariable("DAW_COLLAB_STATUS_SHOT");
        if (!screenshot.isEmpty()) {
            strip.setSessionMode(QStringLiteral("follow_host"), true);
            strip.setMayModerate(true);
            strip.setParticipantStatus(QStringLiteral("alice"), QStringLiteral("alice"), QStringLiteral("Alice"),
                SessionActivity::tr("Online") + QStringLiteral(" · ") + SessionActivity::tr("Host"));
            strip.setParticipantStatus(QStringLiteral("bob"), QStringLiteral("bob"), QStringLiteral("Bob"),
                SessionActivity::tr("Checking plugin state compatibility…"));
            strip.setParticipantStatus(QStringLiteral("eve"), QStringLiteral("eve"), QStringLiteral("Eve"),
                SessionActivity::tr("Read-only"));
            strip.m_impl->activity.state = CollaborationState::Synced;
            strip.m_impl->activity.mode = QStringLiteral("follow_host");
            strip.m_impl->activity.conductor = QStringLiteral("Alice");
            strip.m_impl->activity.participants = 3;
            strip.m_impl->activity.sync = CloudSyncPhase::Ready;
            strip.m_impl->activity.publication = CloudPublicationPhase::Idle;
            strip.m_impl->refresh();
            strip.m_impl->showPopup(true);
            QApplication::processEvents();
            if (!strip.m_impl->popup->grab().save(screenshot))
                return fail(QStringLiteral("session panel screenshot could not be saved"));
        }
    }
    return true;
}

} // namespace collab
