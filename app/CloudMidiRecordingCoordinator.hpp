#pragma once
#include "collaboration/ProjectCommand.hpp"
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <map>
namespace collab {
class CollaborationService;
class CollaborationCommandBridge;
class CloudProjectClient;
class RecordingLeaseCoordinator;
class CloudSessionLifecycleController;
class CloudMidiRecordingCoordinator final : public QObject {
  public:
    CloudMidiRecordingCoordinator(CollaborationService *, CollaborationCommandBridge *,
                                  CloudProjectClient *, RecordingLeaseCoordinator *,
                                  CloudSessionLifecycleController *, QString root,
                                  QObject *parent = nullptr);
    bool enqueue(std::vector<daw::collab::PrepareMidiPart> parts,
                 daw::collab::ProjectCommand commit, std::uint64_t through, bool ready);
    bool completeWithAudio(daw::collab::ProjectCommand audioCommit);
    bool contains(const std::string &operationId) const;
    static bool checkPersistenceForTest(QString *error = nullptr);
    std::function<void(const QString &, std::uint64_t)> confirmed;
    std::function<void(const QString &)> failed;

  private:
    struct Pending {
        QString path;
        std::vector<daw::collab::ProjectCommand> parts;
        daw::collab::ProjectCommand commit;
        std::size_t next = 0;
        bool ready = true, inFlight = false, stopped = false, attempted = false, needsProof = false;
        std::uint64_t through = 0;
    };
    CollaborationService *m_service;
    CollaborationCommandBridge *m_bridge;
    QString m_root;
    CloudProjectClient *m_client;
    RecordingLeaseCoordinator *m_leases;
    CloudSessionLifecycleController *m_session;
    quint64 m_lookupRequest = 0;
    std::map<std::string, Pending> m_runs;
    QTimer m_timer;
    bool save(const Pending &);
    void load();
    void pump();
    void acknowledge(const QString &);
};
} // namespace collab
