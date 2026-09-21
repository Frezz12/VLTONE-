#include "CloudMidiRecordingCoordinator.hpp"
#include "CloudProjectClient.hpp"
#include "CloudSessionLifecycleController.hpp"
#include "CollaborationCommandBridge.hpp"
#include "CollaborationService.hpp"
#include "RecordingLeaseCoordinator.hpp"
#include "collaboration/CollaborationState.hpp"
#include "collaboration/CommandJson.hpp"
#include "collaboration/MidiContentJson.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <nlohmann/json.hpp>
namespace collab {
using namespace daw::collab;
using json = nlohmann::json;
CloudMidiRecordingCoordinator::CloudMidiRecordingCoordinator(
    CollaborationService *service, CollaborationCommandBridge *bridge, CloudProjectClient *client,
    RecordingLeaseCoordinator *leases, CloudSessionLifecycleController *session, QString root,
    QObject *parent)
    : QObject(parent), m_service(service), m_bridge(bridge), m_root(std::move(root)),
      m_client(client), m_leases(leases), m_session(session) {
    load();
    if (bridge)
        connect(bridge, &CollaborationCommandBridge::operationDurablyObserved, this,
                [this](const QString &id, quint64, bool) { acknowledge(id); });
    if (bridge)
        connect(bridge, &CollaborationCommandBridge::operationDurabilityFailed, this,
                [this](const QString &id, const QString &, const QString &) {
                    for (auto &[key, p] : m_runs) {
                        const auto &c = p.next < p.parts.size() ? p.parts[p.next] : p.commit;
                        if (QString::fromStdString(c.meta.operationId) == id) {
                            p.inFlight = false;
                            p.stopped = true;
                            if (failed)
                                failed(tr("MIDI сохранён локально; отправка требует устранить "
                                          "конфликт и повторить подключение."));
                        }
                    }
                });
    if (service)
        connect(service, &CollaborationService::stateChanged, this, [this](auto, const auto &) {
            for (auto &[id, p] : m_runs) {
                if (p.attempted)
                    p.needsProof = true;
                p.inFlight = false;
                p.stopped = false;
            }
            pump();
        });
    if (m_client) {
        connect(m_client, &CloudProjectClient::operationLookupReceived, this,
                [this](quint64 request, const CloudOperationLookup &lookup) {
                    if (request != m_lookupRequest)
                        return;
                    m_lookupRequest = 0;
                    if (lookup.found()) {
                        if (m_bridge->confirmedServerSequence() < lookup.headSequence)
                            m_bridge->requireResync(tr("Synchronizing confirmed MIDI recording"));
                        acknowledge(lookup.operationId);
                        return;
                    }
                    for (auto &[id, p] : m_runs) {
                        auto &c = p.next < p.parts.size() ? p.parts[p.next] : p.commit;
                        if (c.meta.operationId != lookup.operationId.toStdString() ||
                            c.meta.projectId != lookup.projectId.toStdString())
                            continue;
                        if (m_bridge->confirmedServerSequence() != lookup.headSequence) {
                            m_bridge->requireResync(tr("Synchronizing MIDI recording recovery"));
                            return;
                        }
                        p.needsProof = false;
                        p.attempted = false;
                        c.meta.baseServerSequence = 0;
                        save(p);
                    }
                    pump();
                });
        connect(m_client, &CloudProjectClient::requestFailed, this,
                [this](quint64 request, CloudRequestKind, const CloudClientError &) {
                    if (request == m_lookupRequest)
                        m_lookupRequest = 0;
                });
    }
    if (m_leases)
        connect(m_leases, &RecordingLeaseCoordinator::leasesAcquired, this,
                [this](const auto &) { pump(); });
    connect(&m_timer, &QTimer::timeout, this, [this] { pump(); });
    m_timer.start(1000);
}
bool CloudMidiRecordingCoordinator::save(const Pending &p) {
    json parts = json::array();
    for (const auto &c : p.parts)
        parts.push_back(projectCommandToJson(c));
    json root{{"version", 1},
              {"next", p.next},
              {"ready", p.ready},
              {"attempted", p.attempted},
              {"parts", std::move(parts)},
              {"commit", projectCommandToJson(p.commit)}};
    const auto bytes = root.dump();
    if (bytes.size() > 128 * 1024 * 1024)
        return false;
    QDir().mkpath(QFileInfo(p.path).absolutePath());
    QSaveFile file(p.path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()) && file.commit();
}
void CloudMidiRecordingCoordinator::load() {
    const auto paths = QDir(m_root).entryInfoList({"*.json"}, QDir::Files);
    for (const auto &path : paths) {
        if (path.size() > 128 * 1024 * 1024 || path.isSymLink())
            continue;
        QFile file(path.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        auto bytes = file.readAll();
        auto j = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
        if (j.is_discarded() || j.value("version", 0) != 1)
            continue;
        try {
            Pending p;
            p.path = path.absoluteFilePath();
            p.next = j.at("next");
            p.ready = j.at("ready");
            p.attempted = j.value("attempted", false);
            p.needsProof = p.attempted;
            auto commit = projectCommandFromJson(j.at("commit"));
            if (!commit || !std::holds_alternative<RecordingCommit>(commit->body))
                continue;
            p.commit = std::move(*commit);
            bool valid = true;
            for (const auto &part : j.at("parts")) {
                auto c = projectCommandFromJson(part);
                if (!c || !std::holds_alternative<PrepareMidiPart>(c->body) ||
                    c->meta.projectId != p.commit.meta.projectId) {
                    valid = false;
                    break;
                }
                p.parts.push_back(std::move(*c));
            }
            if (valid && p.next <= p.parts.size())
                m_runs.emplace(p.commit.meta.operationId, std::move(p));
        } catch (...) {
        }
    }
}
bool CloudMidiRecordingCoordinator::contains(const std::string &id) const {
    return m_runs.contains(id);
}
bool CloudMidiRecordingCoordinator::enqueue(std::vector<PrepareMidiPart> parts,
                                            ProjectCommand commit, std::uint64_t through,
                                            bool ready) {
    if (m_runs.contains(commit.meta.operationId))
        return true;
    Pending p;
    p.commit = std::move(commit);
    p.ready = ready;
    p.through = through;
    p.path = QDir(m_root).filePath(QString::fromStdString(p.commit.meta.operationId) + ".json");
    for (auto &part : parts) {
        ProjectCommand c;
        c.meta = p.commit.meta;
        c.meta.baseServerSequence = 0;
        c.meta.operationId = deterministicMigrationId(
            "midi-recording-part", part.contentId + ":" + std::to_string(part.index));
        c.body = std::move(part);
        if (serializedProjectCommandPayloadSize(c) > kMaxProjectCommandBatchBytes)
            return false;
        p.parts.push_back(std::move(c));
    }
    if (!save(p))
        return false;
    m_runs.emplace(p.commit.meta.operationId, std::move(p));
    pump();
    return true;
}
bool CloudMidiRecordingCoordinator::completeWithAudio(ProjectCommand audio) {
    auto it = m_runs.find(audio.meta.operationId);
    if (it == m_runs.end())
        return false;
    auto &original = it->second;
    if (original.ready)
        return true;
    auto p = original;
    auto &candidate = std::get<RecordingCommit>(p.commit.body);
    candidate.batch = std::make_shared<BatchCommand>(*candidate.batch);
    auto &target = std::get<RecordingCommit>(p.commit.body);
    auto &source = std::get<RecordingCommit>(audio.body);
    target.batch->commands.insert(target.batch->commands.end(), source.batch->commands.begin(),
                                  source.batch->commands.end());
    for (const auto &lease : source.leases)
        if (std::none_of(target.leases.begin(), target.leases.end(),
                         [&](const auto &l) { return l.trackId == lease.trackId; }))
            target.leases.push_back(lease);
    if (!target.leases.empty())
        for (const auto &command : source.batch->commands) {
            if (const auto *add = std::get_if<AddClip>(&command.body))
                if (std::none_of(target.leases.begin(), target.leases.end(),
                                 [&](const auto &l) { return l.trackId == add->trackId; }))
                    target.leases.push_back(
                        {add->trackId,
                         deterministicMigrationId("pending-midi-lease", add->trackId)});
        }
    p.ready = true;
    if (!save(p))
        return false;
    original = std::move(p);
    pump();
    return true;
}
void CloudMidiRecordingCoordinator::acknowledge(const QString &operation) {
    for (auto it = m_runs.begin(); it != m_runs.end(); ++it) {
        auto &p = it->second;
        const auto &c = p.next < p.parts.size() ? p.parts[p.next] : p.commit;
        if (QString::fromStdString(c.meta.operationId) != operation)
            continue;
        p.inFlight = false;
        p.attempted = false;
        p.needsProof = false;
        if (p.next < p.parts.size()) {
            ++p.next;
            if (!save(p)) {
                p.stopped = true;
                if (failed)
                    failed(tr("Не удалось сохранить состояние отправки MIDI."));
            }
        } else {
            const auto project = QString::fromStdString(p.commit.meta.projectId);
            const auto through = p.through;
            if (!QFile::remove(p.path)) {
                p.stopped = true;
                return;
            }
            m_runs.erase(it);
            if (confirmed)
                confirmed(project, through);
        }
        QTimer::singleShot(0, this, [this] { pump(); });
        return;
    }
}
void CloudMidiRecordingCoordinator::pump() {
    if (!m_service || !m_service->canSubmitOperations() || m_service->commandSchemaVersion() < 4)
        return;
    for (auto &[id, p] : m_runs) {
        if (p.commit.meta.projectId != m_service->projectId().toStdString() || p.stopped)
            continue;
        const bool stage = p.next < p.parts.size();
        if (!stage && !p.ready)
            continue;
        auto &c = stage ? p.parts[p.next] : p.commit;
        const auto watch =
            m_bridge->watchDurableOperation(QString::fromStdString(c.meta.operationId));
        if (watch.code == DurableOperationWatchCode::AlreadyObserved) {
            acknowledge(QString::fromStdString(c.meta.operationId));
            return;
        }
        if (p.inFlight || m_bridge->pendingOperationCount() > 0 || !watch.accepted())
            return;
        if (p.needsProof) {
            if (!m_lookupRequest && m_client)
                m_lookupRequest = m_client->lookupOperation(
                    m_service->projectId(), QString::fromStdString(c.meta.operationId));
            return;
        }
        if (!stage) {
            auto &claims = std::get<RecordingCommit>(c.body).leases;
            if (!claims.empty()) {
                if (!m_leases || !m_session)
                    return;
                auto leases = m_leases->leases();
                const bool held =
                    m_leases->state() == RecordingLeaseState::Held &&
                    m_leases->projectId() == m_service->projectId() &&
                    m_leases->sessionId() == m_service->sessionId() &&
                    std::all_of(claims.begin(), claims.end(), [&](const auto &claim) {
                        return std::any_of(leases.begin(), leases.end(), [&](const auto &lease) {
                            return lease.trackId.toStdString() == claim.trackId;
                        });
                    });
                if (!held) {
                    if (m_leases->state() == RecordingLeaseState::Lost)
                        m_leases->resetAfterLoss();
                    if (m_leases->state() == RecordingLeaseState::Idle) {
                        QStringList tracks;
                        for (const auto &claim : claims)
                            tracks.push_back(QString::fromStdString(claim.trackId));
                        m_leases->acquire(m_service->projectId(), m_service->sessionId(),
                                          m_session->role(), CloudSessionStatus::Active, tracks);
                    }
                    return;
                }
                for (auto &claim : claims)
                    for (const auto &lease : leases)
                        if (lease.trackId.toStdString() == claim.trackId)
                            claim.leaseId = lease.id.toStdString();
            }
        }
        // Freeze the exact wire base before first submission, so retries keep
        // the operation identity and payload byte-for-byte.
        if (c.meta.baseServerSequence == 0) {
            c.meta.baseServerSequence = m_bridge->confirmedServerSequence();
            if (!save(p))
                return;
        }
        p.attempted = true;
        if (!save(p))
            return;
        p.inFlight = true;
        const bool sent = stage ? m_bridge->submitLocal(c).submitted()
                                : m_bridge->submitPreparedCommand(c, "Record MIDI") ==
                                      SharedMutationResult::Submitted;
        if (!sent) {
            p.inFlight = false;
            p.stopped = true;
            if (failed)
                failed(tr("MIDI сохранён локально. Отправка продолжится после подключения."));
        }
        return;
    }
}
bool CloudMidiRecordingCoordinator::checkPersistenceForTest(QString *error) {
    using namespace daw;
    const auto require = [&](bool ok, const char *text) {
        if (!ok && error)
            *error = QString::fromUtf8(text);
        return ok;
    };
    QTemporaryDir directory;
    if (!require(directory.isValid(), "MIDI recovery temporary directory"))
        return false;
    ClipModel content;
    content.kind = ClipKind::Midi;
    for (int i = 0; i < 1500; ++i) {
        NoteModel note;
        note.id = newUuid();
        note.startBeats = i * .25;
        content.notes.push_back(note);
    }
    const auto recording = newUuid(), contentId = newUuid(), trackId = newUuid(),
               clipId = newUuid();
    auto parts = prepareMidiContent(recording, contentId, content);
    ProjectCommand commit;
    commit.meta.projectId = newUuid();
    commit.meta.operationId = newUuid();
    commit.meta.transactionId = commit.meta.operationId;
    auto batch = std::make_shared<BatchCommand>();
    ProjectCommand add, apply;
    add.body = AddClip{trackId, clipId, ClipKind::Midi, "Recovered", 0, 200, 0, {}};
    apply.body =
        ApplyMidiContent{trackId, clipId, recording, contentId, std::uint32_t(parts.size())};
    batch->commands = {add, apply};
    commit.body = RecordingCommit{{}, batch};
    {
        CloudMidiRecordingCoordinator coordinator(nullptr, nullptr, nullptr, nullptr, nullptr,
                                                  directory.path());
        if (!require(coordinator.enqueue(parts, commit, 12345, true), "MIDI durable enqueue"))
            return false;
        auto &pending = coordinator.m_runs.at(commit.meta.operationId);
        coordinator.acknowledge(QString::fromStdString(pending.parts[0].meta.operationId));
        pending.attempted = true;
        if (!require(coordinator.save(pending), "MIDI uncertain send persistence"))
            return false;
    }
    {
        CloudMidiRecordingCoordinator restarted(nullptr, nullptr, nullptr, nullptr, nullptr,
                                                directory.path());
        if (!require(restarted.contains(commit.meta.operationId),
                     "MIDI recovery survives process restart"))
            return false;
        auto &pending = restarted.m_runs.at(commit.meta.operationId);
        if (!require(pending.next == 1 && pending.needsProof && pending.through == 0 &&
                         pending.parts.size() == parts.size(),
                     "MIDI restart retains progress and requires operation proof"))
            return false;
        if (!require(restarted.enqueue(parts, commit, 999, true) && restarted.m_runs.size() == 1 &&
                         pending.next == 1,
                     "MIDI recovery does not duplicate an existing run"))
            return false;
        int confirmations = 0;
        restarted.confirmed = [&](const QString &, std::uint64_t cutoff) {
            if (cutoff == 0)
                ++confirmations;
        };
        while (!restarted.m_runs.empty()) {
            const auto &p = restarted.m_runs.begin()->second;
            const auto op = p.next < p.parts.size() ? p.parts[p.next].meta.operationId
                                                    : p.commit.meta.operationId;
            restarted.acknowledge(QString::fromStdString(op));
        }
        if (!require(confirmations == 1 &&
                         QDir(directory.path()).entryList({"*.json"}, QDir::Files).isEmpty(),
                     "MIDI recovery is consumed only after final acknowledgement"))
            return false;
    }
    return true;
}

} // namespace collab
