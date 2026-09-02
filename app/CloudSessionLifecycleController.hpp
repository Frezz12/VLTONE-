#pragma once

#include "CloudProjectClient.hpp"

#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>

namespace collab {

class CloudProjectSyncCoordinator;
class CollaborationService;

enum class CloudSessionLifecyclePhase : quint8 {
    Unbound,
    WaitingForVerifiedProject,
    Ready,
    Starting,
    Connecting,
    Active,
    Ending,
    Leaving,
    Left,
};

/// Conservative UI/application coordinator for the REST portion of a live
/// session lifecycle.  It does not own credentials, sockets, project data, or
/// final-snapshot work.  The backend remains authoritative; this class merely
/// prevents obviously stale/duplicate requests and connects accepted REST
/// transitions to the already verified CollaborationService bootstrap.
class CloudSessionLifecycleController final : public QObject {
    Q_OBJECT
public:
    CloudSessionLifecycleController(CloudProjectClient* projects,
                                    CloudProjectSyncCoordinator* synchronizer,
                                    CollaborationService* service,
                                    QObject* parent = nullptr);
    ~CloudSessionLifecycleController() override;

    void bindProject(const QString& projectId);
    void clearBinding();

    QString projectId() const;
    QString sessionId() const;
    CloudProjectRole role() const noexcept;
    CloudProjectStatus projectStatus() const noexcept;
    CloudSessionLifecyclePhase phase() const noexcept;

    bool canStartSession() const;
    bool canEndSession() const;
    bool canLeaveSession() const;

public slots:
    bool startSession();
    bool endSession();
    bool leaveSession();

signals:
    void phaseChanged(collab::CloudSessionLifecyclePhase phase);
    void capabilitiesChanged(bool canStart, bool canEnd, bool canLeave);
    /// Fixed, translated-safe UI copy only. No server body, token, filename,
    /// path, or project payload is ever forwarded through this signal.
    void userNotice(const QString& safeMessage, bool error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend bool checkCloudSessionLifecycleControllerForTest(QString* error);
};

bool checkCloudSessionLifecycleControllerForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudSessionLifecyclePhase)
