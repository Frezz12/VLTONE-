#pragma once

#include "CloudProjectAssetHydrator.hpp"
#include "CloudProjectClient.hpp"
#include "CloudProjectSyncCoordinator.hpp"

#include <QCoreApplication>
#include <QDialog>
#include <QString>

#include <memory>

class QCloseEvent;
namespace daw { class EngineController; }

namespace collab {

class CollaborationService;

/// The stages a joiner passes through, in order. Naming them is the point: the
/// old flow was three disconnected menu actions and a paste box, and a user who
/// waited had no idea whether anything was happening.
enum class JoinStep : quint8 {
    ResolveCode,       ///< Redeem the invitation code; grants membership.
    VerifyMembership,  ///< Read the project back: role, status, live session.
    Compatibility,     ///< App, engine and schema versions against the project.
    DownloadSnapshot,  ///< Bootstrap, snapshot, operation replay.
    HydrateAssets,     ///< Audio and plugin state referenced by the document.
    NegotiatePlugins,  ///< Match the exact v3 manifest and derive effective role.
    Connect,           ///< REST join, then the WebSocket.
};
inline constexpr int kJoinStepCount = 7;

enum class JoinStepState : quint8 { Pending, Running, Done, Failed, Skipped };

/// Widget-free flow machine.
///
/// Ports are std::function so checkJoinSessionDialogForTest can drive the whole
/// sequence synchronously, with no event loop and no widgets — the same shape
/// InviteRequestState uses in CloudProjectInviteDialog.cpp.
class JoinFlowState final {
    // The flow's own strings are produced by free helper functions and by this
    // class, neither of which is a QObject. Declaring tr() here pins the
    // translation context to a name lupdate and Qt both spell the same way —
    // calling JoinSessionDialog::tr from outside the class does not, because
    // lupdate records the unqualified name while Qt looks up the namespaced one
    // from the meta-object, and the lookup silently misses.
    Q_DECLARE_TR_FUNCTIONS(JoinFlowState)
public:
    struct Ports {
        std::function<quint64(const QString& code)> acceptCode;
        std::function<quint64(const QString& projectId)> fetchProject;
        /// Starts snapshot download and replay. Returns false when the
        /// synchronizer will not take the project.
        std::function<bool(const QString& projectId)> beginSync;
        std::function<quint64(const QString& projectId,
                              const QString& sessionId,
                              const QString& password,
                              const daw::collab::PluginReadinessReport&,
                              int commandSchemaVersion)>
            join;
        std::function<daw::collab::PluginReadinessReport(
            const std::vector<daw::collab::PluginRequirement>&, qint64)>
            inspectPlugins;
        std::function<bool(int commandSchemaVersion)> selectProtocol;
        std::function<bool(quint64)> cancel;
    };

    explicit JoinFlowState(Ports ports);
    ~JoinFlowState();

    /// Digits only. Accepts anything a person might paste: spaces, dashes,
    /// brackets, a stray newline.
    static QString normalizeCode(const QString& typed);
    /// Loosely bounded rather than pinned to a width: the server owns the code
    /// length and may change it, and a client that hard-coded one would start
    /// refusing valid codes before it was updated.
    static bool validCode(const QString& normalized);
    /// Empty, or 6..128 characters — mirrors auth.ValidateSessionSecret.
    static bool validPassword(const QString& value);

    JoinStepState stepState(JoinStep step) const noexcept;
    /// Short per-step detail, e.g. "4 of 12 files". Empty when there is none.
    QString stepDetail(JoinStep step) const;
    JoinStep currentStep() const noexcept;
    bool running() const noexcept;
    bool finished() const noexcept;
    bool succeeded() const noexcept;
    QString projectId() const;
    QString sessionId() const;
    bool passwordRequired() const noexcept;
    const QString& safeMessage() const noexcept;
    bool messageIsError() const noexcept;

    /// Starts, or resumes after requirePassword(). A code is required only on
    /// the first attempt; a resume reuses the membership already granted.
    bool begin(const QString& code, const QString& password);

    void onCodeAccepted(quint64 requestId, const CloudProjectView& project);
    void onProjectReceived(quint64 requestId, const CloudProjectView& project);
    void onSyncPhase(CloudSyncPhase phase);
    void onHydrationProgress(qsizetype done, qsizetype total);
    void onHydrationSettled(bool degraded);
    void onActiveSession(const CloudSessionState& state);
    void onSessionState(quint64 requestId, const CloudSessionState& state);
    void onFailed(quint64 requestId, const CloudClientError& error);
    /// The server says this session is protected. The dialog reveals the
    /// password field and the flow parks until begin() is called again.
    void requirePassword();
    /// Cancels anything in flight and wipes the password.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Modal that narrates a join instead of leaving the user staring at nothing.
class JoinSessionDialog final : public QDialog {
    Q_OBJECT
public:
    JoinSessionDialog(CloudProjectClient* projects,
                      CloudProjectSyncCoordinator* sync,
                      CloudProjectAssetHydrator* hydrator,
                      CollaborationService* service,
                      daw::EngineController* controller,
                      QWidget* parent = nullptr);
    ~JoinSessionDialog() override;

    /// Pre-fills the code from a vlt://join/<code> link.
    ///
    /// It never submits. A page in a browser must not be able to walk a user
    /// into someone else's room without them pressing anything.
    void presetCode(const QString& code);

    QString joinedProjectId() const;

public slots:
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void applyTheme();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Deterministic fake-port checks: step ordering, password resume, stale
/// responses, failure isolation and secret lifetime. Constructs no widgets and
/// therefore needs only QCoreApplication.
bool checkJoinSessionDialogForTest(QString* error = nullptr);

} // namespace collab
