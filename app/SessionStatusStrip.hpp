#pragma once

#include "CloudProjectAssetHydrator.hpp"
#include "CloudProjectPublisher.hpp"
#include "CloudProjectSyncCoordinator.hpp"
#include "CloudSessionLifecycleController.hpp"
#include "CollaborationTypes.hpp"

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

namespace collab {

class CloudAssetTransferManager;
class CollaborationCommandBridge;
class CollaborationService;

/// Everything the session is currently doing, in one plain struct.
///
/// Deliberately Qt-widget-free so checkSessionStatusStripForTest can drive the
/// whole aggregation and both rendered strings without an event loop. The
/// widget below only paints what this says.
struct SessionActivity {
    // lupdate cannot see through a helper that forwards to
    // QCoreApplication::translate, so every string in this file goes through
    // these declared tr() overloads instead. Without them the whole strip would
    // silently stay English in a translated build.
    Q_DECLARE_TR_FUNCTIONS(SessionActivity)
public:
    CollaborationState state = CollaborationState::LocalOnly;
    /// Already bounded by CollaborationService; never a raw server body.
    QString stateDetail;
    CloudSessionLifecyclePhase lifecycle = CloudSessionLifecyclePhase::Unbound;
    CloudSyncPhase sync = CloudSyncPhase::Idle;
    CloudPublicationPhase publication = CloudPublicationPhase::Idle;
    CloudHydrationState hydration = CloudHydrationState::Idle;

    int participants = 0;
    bool localIsHost = false;
    bool readOnly = false;

    /// Aggregated across CloudAssetTransferManager transfer ids. `transfersTotal`
    /// counts everything in the current burst, finished included, so "4 of 12"
    /// does not walk backwards as transfers complete.
    int transfersActive = 0;
    int transfersDone = 0;
    quint64 transferBytesDone = 0;
    quint64 transferBytesTotal = 0;

    qsizetype hydrationDone = 0;
    qsizetype hydrationTotal = 0;

    int publishDone = 0;
    int publishTotal = 0;

    /// Local edits still waiting on the server.
    qsizetype pendingOperations = 0;
    bool hashRoundInFlight = false;

    /// Last safe notice. Server text arrives pre-bounded; nothing here is ever
    /// a token, a path, or a raw server body.
    QString notice;
    bool noticeIsError = false;
    /// Non-blocking problems, e.g. plugin version drift between participants.
    QStringList warnings;

    /// The single line the strip shows, e.g.
    /// "Synced · 3 people · Uploading 4 of 12 · 2 edits queued".
    QString headline() const;
    /// The longer form for the tooltip and the details popover.
    QString details() const;
    /// True while anything is in flight, which is what reveals the progress bar.
    bool busy() const noexcept;
    /// 0..100, or -1 when the work has no measurable extent (then the bar runs
    /// indeterminate rather than lying about a percentage).
    int progressPercent() const noexcept;
};

/// Persistent bottom strip: session state, who is here, and what is moving.
///
/// Replaces SessionStatusWidget, which lived in the QStatusBar the user can
/// switch off along with the CPU meter. What the session is doing with someone's
/// work is not an optional readout, and the old widget could report state but no
/// progress at all.
class SessionStatusStrip final : public QWidget {
    Q_OBJECT
public:
    explicit SessionStatusStrip(QWidget* parent = nullptr);
    ~SessionStatusStrip() override;

    /// Every bind is optional, re-entrant, and safe to call again after the
    /// project is rebound. Passing nullptr unbinds that source.
    void bindService(CollaborationService* service);
    void bindLifecycle(CloudSessionLifecycleController* lifecycle);
    void bindSync(CloudProjectSyncCoordinator* sync);
    void bindTransfers(CloudAssetTransferManager* transfers);
    void bindHydrator(CloudProjectAssetHydrator* hydrator);
    void bindPublisher(CloudProjectPublisher* publisher);
    void bindCommandBridge(CollaborationCommandBridge* bridge);

    SessionActivity activity() const;

    /// Shows one transient line in the notice area. This is where the formerly
    /// invisible statusBar()->showMessage() calls should go.
    void showNotice(const QString& safeMessage, bool error, int timeoutMs);

signals:
    void cloudProjectsRequested();
    void joinSessionRequested();
    void invitePeopleRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void applyTheme();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Widget-free check of the aggregation rules and the rendered strings.
/// Requires only QCoreApplication.
bool checkSessionStatusStripForTest(QString* error = nullptr);

} // namespace collab
