#include "CollaborationService.hpp"

#include "AccountService.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/ProjectCommand.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace collab {
namespace {

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

constexpr quint64 kLargestExactJsonInteger = 9007199254740991ULL;

std::optional<quint64> exactSequence(const QJsonValue& value) {
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble(-1.0);
    if (!std::isfinite(number) || number < 0.0 ||
        number > double(kLargestExactJsonInteger) ||
        std::floor(number) != number) {
        return std::nullopt;
    }
    return quint64(number);
}

QString schemaId(const QString& value, int maxLength = 96) {
    if (value.isEmpty() || value.size() > maxLength) return {};
    static const QRegularExpression allowed(
        QStringLiteral("^[A-Za-z0-9_.:-]+$"));
    return allowed.match(value).hasMatch() ? value : QString();
}

QString canonicalUuid(const QJsonValue& value) {
    if (!value.isString()) return {};
    const QUuid uuid(value.toString());
    if (uuid.isNull()) return {};
    const QString canonical =
        uuid.toString(QUuid::WithoutBraces).toLower();
    return value.toString() == canonical ? canonical : QString();
}

bool validWelcomeWriteGate(int commandSchemaVersion, bool readOnly,
                           const QString& reason,
                           const QJsonValue& hashRound) {
    if (readOnly) {
        bool allowedReason = reason == QLatin1String("role_read_only");
        if (commandSchemaVersion >= 3)
            allowedReason = allowedReason ||
                reason == QLatin1String("session_starting") ||
                reason == QLatin1String("plugin_not_ready");
        return allowedReason && hashRound.isNull();
    }
    return reason == QLatin1String("hash_consensus_required") &&
           hashRound.isObject();
}

bool hasOnlySnapshotRequestKeys(const QJsonObject& payload) {
    static const QStringList keys{
        QStringLiteral("requestId"),
        QStringLiteral("sessionId"),
        QStringLiteral("hostParticipantId"),
        QStringLiteral("targetServerSeq"),
        QStringLiteral("reason"),
        QStringLiteral("attempt"),
        QStringLiteral("retryAtMs"),
    };
    if (payload.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(),
                       [&payload](const QString& key) {
                           return payload.contains(key);
                       });
}

std::optional<SnapshotRequest> snapshotRequestFromEnvelope(
    const WireEnvelope& envelope, const QString& expectedSessionId,
    const QString& expectedLocalParticipantId,
    const QString& expectedHostParticipantId, QString* error) {
    const auto reject = [error](const QString& message)
        -> std::optional<SnapshotRequest> {
        if (error) *error = message;
        return std::nullopt;
    };
    // Server-control messages have no actor participant. Requiring a canonical
    // server message UUID also prevents a permissive semantic id from being
    // mistaken for a durable dispatch identity.
    if (canonicalUuid(QJsonValue(envelope.messageId)).isEmpty() ||
        !envelope.participantId.isEmpty() || envelope.ephemeralSequence != 0 ||
        envelope.sentAtMs != 0 || envelope.serverTimeMs < 0) {
        return reject(QStringLiteral("Invalid snapshot request envelope"));
    }
    const QJsonObject& payload = envelope.payload;
    if (!hasOnlySnapshotRequestKeys(payload))
        return reject(QStringLiteral("Invalid snapshot request payload"));

    SnapshotRequest request;
    request.requestId = canonicalUuid(payload.value(QStringLiteral("requestId")));
    request.sessionId = canonicalUuid(payload.value(QStringLiteral("sessionId")));
    request.hostParticipantId = canonicalUuid(
        payload.value(QStringLiteral("hostParticipantId")));
    const auto target = exactSequence(
        payload.value(QStringLiteral("targetServerSeq")));
    const auto attempt = exactSequence(payload.value(QStringLiteral("attempt")));
    const auto retryAt = exactSequence(
        payload.value(QStringLiteral("retryAtMs")));
    const QString reason = payload.value(QStringLiteral("reason")).toString();
    if (request.requestId.isEmpty() || request.sessionId.isEmpty() ||
        request.hostParticipantId.isEmpty() || !target || !attempt ||
        *attempt == 0 || *attempt > quint64(std::numeric_limits<int>::max()) ||
        !retryAt || *retryAt > quint64(std::numeric_limits<qint64>::max()) ||
        (reason != QLatin1String("autosave") &&
         reason != QLatin1String("session_end"))) {
        return reject(QStringLiteral("Invalid snapshot request payload"));
    }
    // The room bus targets a participant, but the client still proves the
    // payload belongs to its welcome identity and current authoritative host.
    // A foreign/stale participant id can therefore never trigger local I/O.
    if (expectedSessionId.isEmpty() || expectedLocalParticipantId.isEmpty() ||
        expectedHostParticipantId.isEmpty() ||
        request.sessionId != expectedSessionId ||
        request.hostParticipantId != expectedLocalParticipantId ||
        request.hostParticipantId != expectedHostParticipantId) {
        return reject(QStringLiteral(
            "Snapshot request is not assigned to this session host"));
    }
    request.targetServerSequence = *target;
    request.reason = reason == QLatin1String("session_end")
        ? SnapshotRequestReason::SessionEnd
        : SnapshotRequestReason::Autosave;
    request.attempt = int(*attempt);
    request.retryAtMs = qint64(*retryAt);
    return request;
}

QString surfaceWireName(SurfaceKind kind) {
    switch (kind) {
        case SurfaceKind::Timeline: return QStringLiteral("timeline");
        case SurfaceKind::TrackList: return QStringLiteral("track_list");
        case SurfaceKind::Transport: return QStringLiteral("transport");
        case SurfaceKind::Mixer: return QStringLiteral("mixer");
        case SurfaceKind::PianoRoll: return QStringLiteral("piano_roll");
        case SurfaceKind::AutomationEditor:
            return QStringLiteral("automation_editor");
        case SurfaceKind::SampleEditor: return QStringLiteral("sample_editor");
        case SurfaceKind::BuiltinPlugin: return QStringLiteral("builtin_plugin");
        case SurfaceKind::Browser: return QStringLiteral("file_browser");
        case SurfaceKind::Web: return QStringLiteral("browser");
        case SurfaceKind::Ai: return QStringLiteral("ai");
        case SurfaceKind::Settings: return QStringLiteral("settings");
        case SurfaceKind::Shell: return QStringLiteral("toolbar");
        default: return QStringLiteral("hidden");
    }
}

SurfaceKind surfaceKindFromWire(const QString& name) {
    if (name == QLatin1String("timeline")) return SurfaceKind::Timeline;
    if (name == QLatin1String("track_list")) return SurfaceKind::TrackList;
    if (name == QLatin1String("transport")) return SurfaceKind::Transport;
    if (name == QLatin1String("mixer")) return SurfaceKind::Mixer;
    if (name == QLatin1String("piano_roll")) return SurfaceKind::PianoRoll;
    if (name == QLatin1String("automation_editor"))
        return SurfaceKind::AutomationEditor;
    if (name == QLatin1String("sample_editor")) return SurfaceKind::SampleEditor;
    if (name == QLatin1String("builtin_plugin")) return SurfaceKind::BuiltinPlugin;
    if (name == QLatin1String("file_browser")) return SurfaceKind::Browser;
    if (name == QLatin1String("browser")) return SurfaceKind::Web;
    if (name == QLatin1String("ai")) return SurfaceKind::Ai;
    if (name == QLatin1String("settings")) return SurfaceKind::Settings;
    if (name == QLatin1String("toolbar")) return SurfaceKind::Shell;
    return SurfaceKind::Unknown;
}

PresencePolicy cappedPolicy(PresencePolicy requested, SurfaceKind surface) {
    if (surface == SurfaceKind::ThirdPartyPlugin ||
        surface == SurfaceKind::GenericPlugin || surface == SurfaceKind::Dialog)
        return PresencePolicy::Hidden;
    if ((surface == SurfaceKind::Browser || surface == SurfaceKind::Web ||
         surface == SurfaceKind::Ai || surface == SurfaceKind::Settings) &&
        requested == PresencePolicy::Exact)
        return PresencePolicy::Coarse;
    return requested;
}

QJsonObject safePresencePayload(const PresencePacket& packet) {
    const PresencePolicy policy = cappedPolicy(packet.policy,
                                               packet.point.surface.kind);
    QJsonObject payload{
        {QStringLiteral("surface"), policy == PresencePolicy::Hidden
                                        ? QStringLiteral("hidden")
                                        : surfaceWireName(packet.point.surface.kind)},
        {QStringLiteral("precision"), policy == PresencePolicy::Exact
                                          ? QStringLiteral("exact")
                                          : policy == PresencePolicy::Coarse
                                                ? QStringLiteral("coarse")
                                                : QStringLiteral("hidden")},
    };
    if (policy == PresencePolicy::Hidden || policy == PresencePolicy::Coarse)
        return payload;
    const auto addId = [&payload](const char* key, const QString& value,
                                  int maxLength = 96) {
        const QString safe = schemaId(value, maxLength);
        if (!safe.isEmpty()) payload.insert(QString::fromLatin1(key), safe);
    };
    addId("targetId", packet.point.targetId);
    addId("trackId", packet.point.trackId, 64);
    addId("clipId", packet.point.clipId, 64);
    addId("controlId", packet.point.parameterId);
    if (std::isfinite(packet.point.normalized.x()) &&
        std::isfinite(packet.point.normalized.y()) &&
        packet.point.normalized.x() >= 0.0 &&
        packet.point.normalized.y() >= 0.0) {
        payload.insert(QStringLiteral("u"),
                       std::clamp(packet.point.normalized.x(), 0.0, 1.0));
        payload.insert(QStringLiteral("v"),
                       std::clamp(packet.point.normalized.y(), 0.0, 1.0));
    }
    if (std::isfinite(packet.point.timeSeconds) &&
        packet.point.timeSeconds >= 0.0)
        payload.insert(QStringLiteral("timeSeconds"),
                       packet.point.timeSeconds);
    if (std::isfinite(packet.point.beat) && packet.point.beat >= 0.0)
        payload.insert(QStringLiteral("beat"), packet.point.beat);
    if (packet.point.pitch >= 0 && packet.point.pitch <= 127)
        payload.insert(QStringLiteral("pitch"), packet.point.pitch);
    if (std::isfinite(packet.point.laneFraction) &&
        packet.point.laneFraction >= 0.0)
        payload.insert(QStringLiteral("laneFraction"),
                       std::clamp(packet.point.laneFraction, 0.0, 1.0));
    return payload;
}

std::optional<ParticipantIdentity> participantFromJson(const QJsonObject& json) {
    ParticipantIdentity participant;
    participant.participantId =
        schemaId(json.value(QStringLiteral("participantId")).toString(), 64);
    participant.userId =
        schemaId(json.value(QStringLiteral("userId")).toString(), 64);
    if (participant.participantId.isEmpty() || participant.userId.isEmpty())
        return std::nullopt;
    participant.nickname =
        safeDisplayName(json.value(QStringLiteral("nickname")).toString());
    participant.role = schemaId(json.value(QStringLiteral("role")).toString(), 16);
    participant.color = QColor(json.value(QStringLiteral("color")).toString());
    participant.host = json.value(QStringLiteral("host")).toBool(false);
    return participant;
}

std::optional<PresencePacket> parsePresencePayload(
    const WireEnvelope& envelope, PointerPhase phase, QString* error) {
    const QJsonObject& payload = envelope.payload;
    PresencePacket packet;
    packet.clientSequence = envelope.ephemeralSequence;
    packet.sentAtMs = envelope.serverTimeMs > 0 ? envelope.serverTimeMs
                                                : envelope.sentAtMs;
    packet.phase = phase;
    // Absent on every kind but presence.click, where the server has already
    // required one of primary/secondary/middle.
    packet.button =
        pointerButtonFromName(payload.value(QStringLiteral("button")).toString())
            .value_or(PointerButton::Primary);
    const QString precision = payload.value(QStringLiteral("precision")).toString();
    if (precision == QLatin1String("exact")) packet.policy = PresencePolicy::Exact;
    else if (precision == QLatin1String("coarse"))
        packet.policy = PresencePolicy::Coarse;
    else if (precision == QLatin1String("hidden"))
        packet.policy = PresencePolicy::Hidden;
    else {
        if (error) *error = QStringLiteral("invalid presence precision");
        return std::nullopt;
    }
    packet.point.surface.kind = surfaceKindFromWire(
        payload.value(QStringLiteral("surface")).toString());
    if (packet.policy != PresencePolicy::Hidden &&
        packet.point.surface.kind == SurfaceKind::Unknown) {
        if (error) *error = QStringLiteral("invalid presence surface");
        return std::nullopt;
    }
    const auto readId = [&payload](const char* key, int maxLength = 96) {
        return schemaId(payload.value(QString::fromLatin1(key)).toString(),
                        maxLength);
    };
    packet.point.targetId = readId("targetId");
    packet.point.trackId = readId("trackId", 64);
    packet.point.clipId = readId("clipId", 64);
    packet.point.parameterId = readId("controlId");
    packet.point.normalized = QPointF(
        std::clamp(payload.value(QStringLiteral("u")).toDouble(-1.0), -1.0, 1.0),
        std::clamp(payload.value(QStringLiteral("v")).toDouble(-1.0), -1.0, 1.0));
    packet.point.timeSeconds = std::max(
        -1.0, payload.value(QStringLiteral("timeSeconds")).toDouble(-1.0));
    packet.point.beat =
        std::max(-1.0, payload.value(QStringLiteral("beat")).toDouble(-1.0));
    packet.point.pitch = payload.value(QStringLiteral("pitch")).toInt(-1);
    packet.point.laneFraction = std::clamp(
        payload.value(QStringLiteral("laneFraction")).toDouble(-1.0),
        -1.0, 1.0);
    return packet;
}

} // namespace

CollaborationService::CollaborationService(account::Service* account,
                                           QObject* parent)
    : QObject(parent),
      m_account(account),
      m_presenceStore(this),
      m_localSessionState(this) {
    qRegisterMetaType<PresencePacket>();
    qRegisterMetaType<PresenceUpdate>();
    qRegisterMetaType<TransportFrame>();
    qRegisterMetaType<SnapshotRequest>();
    qRegisterMetaType<WireEnvelope>();
    m_monotonicClock.start();
    m_clockEpochMs = nowMs();
    m_lastTransportSent.start();
    auto* controlTimer = new QTimer(this);
    controlTimer->setInterval(1000);
    connect(controlTimer, &QTimer::timeout, this, [this] {
        if (hashRoundInFlight() && m_hashRoundDeadlineMs <= estimatedServerTimeMs()) {
            clearHashRound();
            trustedResyncRequired(false, false, tr("State verification timed out. Reconnect to retry."));
        }
        if (!isOnline() || m_commandSchemaVersion < 6) return;
        sendPendingControl();
        const qint64 current = m_clockEpochMs + m_monotonicClock.elapsed();
        if (current - m_clockPingAt >= 5000) {
            m_clockPingAt = current;
            sendEnvelope(WireType::ClockPing, {{QStringLiteral("clientSentAtMs"), double(current)}});
        }
    });
    controlTimer->start();
    if (m_account) {
        connect(m_account, &account::Service::authenticatedChanged, this,
                [this](bool) { refreshAccountState(); });
        connect(m_account, &account::Service::snapshotChanged, this,
                &CollaborationService::refreshAccountState);
    }
    refreshAccountState();
}

QString CollaborationService::accountUserId() const {
    if (!m_account || !m_account->authenticated()) return {};
    const QUuid id(m_account->snapshot().userId);
    return id.isNull() ? QString()
                       : id.toString(QUuid::WithoutBraces).toLower();
}

QString CollaborationService::protocolName() const {
    return protocolNameForCommandSchema(m_commandSchemaVersion);
}

bool CollaborationService::setCommandSchemaVersion(int version) {
    if (!daw::collab::supportedProjectCommandSchemaVersion(
            std::uint32_t(version)))
        return false;
    if (version == m_commandSchemaVersion) return true;
    m_commandSchemaVersion = version;
    m_transportConnected = false;
    clearSessionControl();
    m_sessionId.clear();
    clearHashRound();
    emit commandSchemaVersionChanged(version);
    emit roomIdentityChanged({}, {}, {});
    if (m_shouldConnect && !m_projectId.isEmpty()) reconnectNow();
    return true;
}

void CollaborationService::setProjectId(const QString& projectId,
                                        bool requestConnection) {
    const QUuid uuid(projectId);
    const QString safe = uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
    if (safe == m_projectId && requestConnection == m_shouldConnect) return;
    m_projectId = safe;
    m_sessionId.clear();
    clearHashRound();
    m_bootstrapServerSequence = 0;
    m_bootstrapStateHash.clear();
    m_transportConnected = false;
    clearSessionControl();
    m_sessionReadOnly = false;
    m_pendingRecoveryBlocked = false;
    m_resyncPending = false;
    m_authorizationRequested = false;
    m_shouldConnect = requestConnection && !safe.isEmpty();
    m_presenceStore.clear();
    m_localSessionState.setHostParticipantId({});
    emit projectChanged(m_projectId);
    emit roomIdentityChanged({}, {}, {});
    refreshAccountState();
}

void CollaborationService::clearProject() {
    m_projectId.clear();
    m_sessionId.clear();
    clearHashRound();
    m_bootstrapServerSequence = 0;
    m_bootstrapStateHash.clear();
    m_shouldConnect = false;
    m_transportConnected = false;
    clearSessionControl();
    m_sessionReadOnly = false;
    m_pendingRecoveryBlocked = false;
    m_resyncPending = false;
    m_authorizationRequested = false;
    m_presenceStore.clear();
    m_localSessionState.setHostParticipantId({});
    emit projectChanged({});
    emit roomIdentityChanged({}, {}, {});
    setState(CollaborationState::LocalOnly, tr("Local project"));
}

void CollaborationService::reconnectNow() {
    if (m_projectId.isEmpty()) {
        setState(CollaborationState::LocalOnly,
                 tr("Share this project to start a session"));
        return;
    }
    m_shouldConnect = true;
    m_transportConnected = false;
    clearSessionControl();
    m_sessionId.clear();
    clearHashRound();
    m_sessionReadOnly = false;
    m_resyncPending = false;
    m_authorizationRequested = false;
    m_presenceStore.clear();
    m_localSessionState.setHostParticipantId({});
    emit roomIdentityChanged({}, {}, {});
    refreshAccountState();
}

void CollaborationService::disconnectFromProject() {
    m_shouldConnect = false;
    m_transportConnected = false;
    clearSessionControl();
    m_sessionReadOnly = false;
    m_resyncPending = false;
    m_authorizationRequested = false;
    m_sessionId.clear();
    clearHashRound();
    m_presenceStore.clear();
    m_localSessionState.setHostParticipantId({});
    emit roomIdentityChanged({}, {}, {});
    setState(CollaborationState::LocalOnly, tr("Session disconnected"));
}

bool CollaborationService::installVerifiedBootstrapSequence(
    const QString& projectId, quint64 serverSequence) {
    const QUuid uuid(projectId);
    const QString canonical = uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
    if (canonical.isEmpty() || canonical != m_projectId ||
        serverSequence > kLargestExactJsonInteger) {
        return false;
    }
    if (serverSequence != m_bootstrapServerSequence)
        m_bootstrapStateHash.clear();
    m_bootstrapServerSequence = serverSequence;
    return true;
}

bool CollaborationService::installVerifiedBootstrapState(
    const QString& projectId, quint64 serverSequence,
    const QString& stateHash) {
    static const QRegularExpression digestPattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    const QString normalizedHash = stateHash.trimmed().toLower();
    if (!digestPattern.match(normalizedHash).hasMatch() ||
        !installVerifiedBootstrapSequence(projectId, serverSequence)) {
        return false;
    }
    m_bootstrapStateHash = normalizedHash;
    return true;
}

bool CollaborationService::advanceMaterializedSequence(
    const QString& projectId, quint64 serverSequence) {
    const QUuid uuid(projectId);
    const QString canonical = uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
    if (canonical.isEmpty() || canonical != m_projectId ||
        serverSequence > kLargestExactJsonInteger ||
        serverSequence != m_bootstrapServerSequence + 1) {
        return false;
    }
    m_bootstrapServerSequence = serverSequence;
    m_bootstrapStateHash.clear();
    return true;
}

void CollaborationService::trustedTransportConnected() {
    if (!m_shouldConnect || m_projectId.isEmpty() || !m_account ||
        !m_account->authenticated() || m_account->snapshot().offline)
        return;
    m_transportConnected = true;
    m_transportSent = false;
    m_authorizationRequested = false;
    setState(CollaborationState::Joining, tr("Joining session…"));
    QJsonObject hello{
        {QStringLiteral("appVersion"), QCoreApplication::applicationVersion()},
        {QStringLiteral("engineVersion"), QCoreApplication::applicationVersion()},
        {QStringLiteral("commandSchemaVersion"),
         m_commandSchemaVersion},
        {QStringLiteral("projectFormatVersion"),
         daw::collab::kSharedProjectFormatVersion},
        {QStringLiteral("afterServerSeq"),
         double(m_bootstrapServerSequence)},
    };
    if (!m_bootstrapStateHash.isEmpty())
        hello.insert(QStringLiteral("stateHash"), m_bootstrapStateHash);
    sendEnvelope(WireType::Hello, hello);
}

void CollaborationService::trustedTransportDisconnected(
    const QString& safeReason) {
    m_transportConnected = false;
    clearSessionControl();
    m_transportSent = false;
    m_presenceStore.clear();
    m_sessionId.clear();
    clearHashRound();
    m_localSessionState.setHostParticipantId({});
    emit roomIdentityChanged({}, {}, {});
    if (!m_shouldConnect) {
        setState(CollaborationState::LocalOnly, tr("Session disconnected"));
        return;
    }
    const QString detail = safeReason.isEmpty()
        ? tr("Connection closed")
        : safeDisplayName(safeReason);
    setState(CollaborationState::Reconnecting, detail);
    m_authorizationRequested = false;
    requestTrustedTransport();
}

void CollaborationService::trustedTransportUnavailable(
    const QString& safeReason) {
    m_transportConnected = false;
    clearSessionControl();
    m_transportSent = false;
    m_presenceStore.clear();
    m_sessionId.clear();
    clearHashRound();
    m_localSessionState.setHostParticipantId({});
    emit roomIdentityChanged({}, {}, {});
    // Keep authorization latched until reconnectNow() so a permanent endpoint
    // or protocol failure cannot create a tight account-signal retry loop.
    m_authorizationRequested = true;
    const QString detail = safeReason.isEmpty()
        ? tr("Collaboration transport is unavailable")
        : safeDisplayName(safeReason);
    setState(CollaborationState::Unavailable, detail);
}

void CollaborationService::trustedSessionExcluded(const QString& action) {
    if (action != QLatin1String("kick") && action != QLatin1String("ban")) return;
    emit localSessionExcluded(action);
    m_shouldConnect = false;
    trustedTransportUnavailable(action == QLatin1String("ban")
        ? tr("The project owner blocked your access to this project.")
        : tr("The project owner removed you from this session."));
}

void CollaborationService::trustedResyncRequired(
    bool conflict, bool readOnly, const QString& safeReason) {
    if (!m_transportConnected || m_projectId.isEmpty()) return;
    m_resyncPending = true;
    m_sessionReadOnly = m_sessionReadOnly || readOnly;
    if (conflict || readOnly || m_state == CollaborationState::Conflict) {
        setState(CollaborationState::Conflict,
                 safeReason.isEmpty()
                     ? tr("Project conflict requires resync")
                     : safeDisplayName(safeReason));
        return;
    }
    setState(CollaborationState::Reconnecting,
             safeReason.isEmpty()
                 ? tr("Refreshing shared project state")
                 : safeDisplayName(safeReason));
}

bool CollaborationService::trustedResyncCompleted() {
    if (!m_resyncPending || !m_transportConnected || !m_shouldConnect ||
        m_projectId.isEmpty()) {
        return false;
    }
    m_resyncPending = false;
    setState(m_sessionReadOnly ? CollaborationState::ReadOnly
                               : CollaborationState::Joining,
             m_sessionReadOnly ? tr("Session is read-only")
                               : tr("Verifying shared project state"));
    if (!m_sessionReadOnly) requestOrSendRoundHash();
    return true;
}

void CollaborationService::trustedOfflineProjectOpened() {
    if (m_projectId.isEmpty()) return;
    m_shouldConnect = false;
    m_transportConnected = false;
    clearSessionControl();
    m_sessionReadOnly = true;
    m_resyncPending = false;
    m_authorizationRequested = false;
    m_presenceStore.clear();
    setState(CollaborationState::ReadOnly,
             tr("Offline cached project — read-only"));
}

void CollaborationService::receiveTrustedTextMessage(const QString& message) {
    if (!m_transportConnected) return;
    const QByteArray bytes = message.toUtf8();
    if (bytes.size() > m_maxMessageBytes) {
        emit protocolWarning(tr("Oversized collaboration message was ignored"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit protocolWarning(tr("Invalid collaboration JSON was ignored"));
        return;
    }
    QString error;
    const auto envelope = wireEnvelopeFromJson(document.object(), &error);
    if (!envelope) {
        emit protocolWarning(error);
        return;
    }
    handleEnvelope(*envelope);
}

void CollaborationService::refreshAccountState() {
    if (m_projectId.isEmpty() || !m_shouldConnect) {
        setState(CollaborationState::LocalOnly,
                 m_projectId.isEmpty() ? tr("Local project")
                                       : tr("Session disconnected"));
        return;
    }
    if (!m_account || !m_account->authenticated()) {
        m_transportConnected = false;
    clearSessionControl();
        m_authorizationRequested = false;
        m_presenceStore.clear();
        setState(CollaborationState::SignedOut, tr("Sign in to collaborate"));
        return;
    }
    if (m_account->snapshot().offline) {
        m_transportConnected = false;
    clearSessionControl();
        m_authorizationRequested = false;
        m_presenceStore.clear();
        setState(CollaborationState::NoConnection,
                 tr("No collaboration connection"));
        return;
    }
    if (m_transportConnected) return;
    requestTrustedTransport();
}

void CollaborationService::requestTrustedTransport() {
    if (m_state != CollaborationState::Reconnecting) {
        setState(CollaborationState::Unavailable,
                 tr("Scoped session authorization required"));
    }
    if (!m_authorizationRequested && m_shouldConnect && !m_projectId.isEmpty()) {
        m_authorizationRequested = true;
        emit roomAuthorizationRequired(m_projectId, collaborationUrl());
    }
}

void CollaborationService::setState(CollaborationState state,
                                    const QString& detail) {
    if (m_state == state && m_stateDetail == detail) return;
    m_state = state;
    m_stateDetail = detail;
    emit stateChanged(m_state, m_stateDetail);
}

QUrl CollaborationService::collaborationUrl() const {
    if (!m_account) return {};
    QUrl url(m_account->apiOrigin());
    if (url.scheme() == QLatin1String("https"))
        url.setScheme(QStringLiteral("wss"));
    else if (url.scheme() == QLatin1String("http"))
        url.setScheme(QStringLiteral("ws"));
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    path += QStringLiteral("/desktop/projects/%1/live").arg(m_projectId);
    url.setPath(path);
    url.setUserInfo({});
    url.setQuery({});
    url.setFragment({});
    return url;
}

bool CollaborationService::sendEnvelope(WireType type,
                                        const QJsonObject& payload,
                                        bool ephemeral) {
    if (!m_transportConnected) return false;
    WireEnvelope envelope;
    envelope.protocol = protocolName();
    envelope.type = type;
    envelope.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    envelope.sentAtMs = nowMs();
    envelope.payload = payload;
    // Per-kind sequences. The room bus coalesces each kind under its own key
    // and drains them independently, so a shared counter would let a newer
    // selection/drag retire an older cursor sample at the receiver's dedupe.
    if (ephemeral) envelope.ephemeralSequence = ++m_ephemeralSequences[int(type)];
    const QByteArray bytes =
        QJsonDocument(wireEnvelopeToJson(envelope)).toJson(QJsonDocument::Compact);
    if (bytes.size() > m_maxMessageBytes) {
        emit protocolWarning(tr("Collaboration message exceeded the room limit"));
        return false;
    }
    emit outboundTextMessage(QString::fromUtf8(bytes));
    return true;
}

bool CollaborationService::canSubmitOperations() const {
    return m_transportConnected && m_state == CollaborationState::Synced &&
           !m_sessionReadOnly && !m_resyncPending && !m_pendingRecoveryBlocked && (m_commandSchemaVersion < 6 ||
           (m_control.sessionVersion > 0 && (m_localRole == QLatin1String("owner") || m_localRole == QLatin1String("editor")) && (m_control.mode != QLatin1String("follow_host") ||
            m_control.hostMemberId == localParticipantId())));
}

bool CollaborationService::canSubmitRecoveryOperations() const {
    return m_transportConnected && m_state == CollaborationState::Synced &&
           !m_sessionReadOnly && !m_resyncPending && m_pendingRecoveryBlocked && (m_commandSchemaVersion < 6 ||
           (m_control.sessionVersion > 0 && (m_localRole == QLatin1String("owner") || m_localRole == QLatin1String("editor")) && (m_control.mode != QLatin1String("follow_host") ||
            m_control.hostMemberId == localParticipantId())));
}

void CollaborationService::setPendingRecoveryBlocked(bool blocked) {
    if (m_pendingRecoveryBlocked == blocked) return;
    m_pendingRecoveryBlocked = blocked;
    // Writability changes even though the transport state does not. Re-emit
    // so action/controller gates refresh synchronously.
    emit stateChanged(m_state, m_stateDetail);
}

bool CollaborationService::submitOperation(const QJsonObject& command) {
    if (!canSubmitOperations() || command.isEmpty()) return false;
    QJsonObject payload{{QStringLiteral("command"), command}};
    if (m_commandSchemaVersion >= 6) payload.insert(QStringLiteral("sessionVersion"), double(m_control.sessionVersion));
    return sendEnvelope(WireType::OpSubmit, payload);
}

bool CollaborationService::submitRecoveryOperation(
    const QJsonObject& command) {
    if (!canSubmitRecoveryOperations() || command.isEmpty()) return false;
    QJsonObject payload{{QStringLiteral("command"), command}};
    if (m_commandSchemaVersion >= 6) payload.insert(QStringLiteral("sessionVersion"), double(m_control.sessionVersion));
    return sendEnvelope(WireType::OpSubmit, payload);
}

bool CollaborationService::canControlSession() const {
    return m_commandSchemaVersion >= 6 && hasSharedTransport() &&
        isOnline() && !m_sessionReadOnly && !m_resyncPending && !m_pendingRecoveryBlocked &&
        m_control.sessionVersion > 0 && (m_localRole == QLatin1String("owner") || m_localRole == QLatin1String("editor")) &&
        (m_control.mode == QLatin1String("synchronized") ||
         m_control.hostMemberId == localParticipantId()) && m_state == CollaborationState::Synced;
}

bool CollaborationService::canChangeSessionMode() const {
    return m_commandSchemaVersion >= 6 && isOnline() && m_state == CollaborationState::Synced &&
        !m_sessionReadOnly && !m_resyncPending && !m_pendingRecoveryBlocked &&
        m_control.sessionVersion > 0 &&
        (m_localRole == QLatin1String("owner") || m_localRole == QLatin1String("editor")) &&
        (mayModerate() || m_control.hostMemberId == localParticipantId());
}

qint64 CollaborationService::estimatedServerTimeMs() const {
    return m_clockEpochMs + m_monotonicClock.elapsed() + m_serverClockOffsetMs;
}

bool CollaborationService::submitSessionControl(const QString& kind, const QJsonObject& values) {
    if (!canControlSession() || m_pendingControls.size() >= 64) return false;
    const auto number = [&values](const QString& key, double low, double high) {
        const auto value = values.value(key);
        return value.isDouble() && std::isfinite(value.toDouble()) &&
               value.toDouble() >= low && value.toDouble() <= high;
    };
    QStringList allowed{QStringLiteral("positionSeconds"), QStringLiteral("rate")};
    if (kind == QLatin1String("audition")) {
        allowed = {QStringLiteral("trackId"), QStringLiteral("muted"), QStringLiteral("solo")};
        if (canonicalUuid(values.value(QStringLiteral("trackId"))).isEmpty() ||
            (!values.contains(QStringLiteral("muted")) && !values.contains(QStringLiteral("solo")))) return false;
        for (const auto& key : {QStringLiteral("muted"), QStringLiteral("solo")})
            if (values.contains(key) && !values.value(key).isBool()) return false;
    } else {
        if (kind != QLatin1String("play") && kind != QLatin1String("pause") &&
            kind != QLatin1String("stop") && kind != QLatin1String("seek") &&
            kind != QLatin1String("loop")) return false;
        if ((kind == QLatin1String("seek") || values.contains(QStringLiteral("positionSeconds"))) &&
            !number(QStringLiteral("positionSeconds"), 0, 1e9)) return false;
        if (values.contains(QStringLiteral("rate")) && !number(QStringLiteral("rate"), .25, 4)) return false;
        if (kind == QLatin1String("loop")) {
            allowed += {QStringLiteral("loopEnabled"), QStringLiteral("loopStartSeconds"), QStringLiteral("loopEndSeconds")};
            if (!values.value(QStringLiteral("loopEnabled")).isBool() ||
                !number(QStringLiteral("loopStartSeconds"), 0, 1e9) || !number(QStringLiteral("loopEndSeconds"), 0, 1e9) ||
                (values.value(QStringLiteral("loopEnabled")).toBool() &&
                 values.value(QStringLiteral("loopEndSeconds")).toDouble() <= values.value(QStringLiteral("loopStartSeconds")).toDouble())) return false;
        }
    }
    for (auto it = values.begin(); it != values.end(); ++it)
        if (!allowed.contains(it.key())) return false;
    QJsonObject action = values;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    action.insert(QStringLiteral("actionId"), id);
    action.insert(QStringLiteral("kind"), kind);
    action.insert(QStringLiteral("expectedSessionVersion"), double(m_control.sessionVersion));
    m_pendingControls.push_back(action);
    if (m_pendingControls.size() == 1) sendPendingControl();
    return true;
}

void CollaborationService::sendPendingControl() {
    if (canControlSession() && !m_pendingControls.isEmpty())
        sendEnvelope(WireType::SessionControl, m_pendingControls.front());
}

bool CollaborationService::acknowledgeControl(const QString& actionId) {
    if (actionId.isEmpty() || m_pendingControls.isEmpty() ||
        m_pendingControls.front().value(QStringLiteral("actionId")).toString() != actionId) return false;
    m_pendingControls.removeFirst();
    return true;
}

void CollaborationService::clearHashRound() {
    const bool active = hashRoundInFlight();
    m_hashRoundId.clear();
    m_hashRoundSessionId.clear();
    m_hashRoundServerSequence = 0;
    m_hashRoundDeadlineMs = 0;
    if (active) emit hashRoundChanged(false);
}

void CollaborationService::clearSessionControl() {
    m_pendingControls.clear();
    m_readinessRevisions.clear();
    m_requirementsRevision = 0;
    m_control = {};
    m_ownerUserId.clear();
    m_localRole.clear();
    m_clockPingAt = 0;
    m_serverClockOffsetMs = 0;
    m_bestClockRttMs = 60000;
    m_localSessionState.useIndependentTransport();
    emit sessionControlChanged();
}

bool CollaborationService::installSessionControl(const QJsonObject& json) {
    const auto next = sessionControlFromJson(json);
    if (!next || next->sessionVersion < m_control.sessionVersion ||
        (next->sessionVersion == m_control.sessionVersion &&
         (next->transportRevision < m_control.transportRevision || next->auditionRevision < m_control.auditionRevision))) return false;
    if (next->sessionVersion != m_control.sessionVersion) m_pendingControls.clear();
    const bool hostChanged = next->hostMemberId != m_control.hostMemberId;
    m_control = *next;
    m_localSessionState.setHostParticipantId(m_control.hostMemberId);
    if (hostChanged) emit roomIdentityChanged(m_sessionId, localParticipantId(), m_control.hostMemberId);
    emit sessionControlChanged();
    emit stateChanged(m_state, m_stateDetail);
    return true;
}

void CollaborationService::sendPresence(const PresencePacket& packet) {
    if (!isOnline()) return;
    QJsonObject payload = safePresencePayload(packet);
    WireType type = WireType::PresenceCursor;
    if (packet.drag.active) {
        type = WireType::PresenceDrag;
        QString gesture = schemaId(packet.drag.kind, 48).toLower();
        static const QRegularExpression gesturePattern(
            QStringLiteral("^[a-z][a-z0-9_.-]{0,47}$"));
        if (!gesturePattern.match(gesture).hasMatch())
            gesture = QStringLiteral("move");
        payload.insert(QStringLiteral("gesture"), gesture);
        QJsonArray ids;
        for (const QString& id : packet.drag.objectIds) {
            const QString safe = schemaId(id, 64);
            if (!safe.isEmpty() && ids.size() < 64) ids.append(safe);
        }
        if (!ids.isEmpty()) payload.insert(QStringLiteral("entityIds"), ids);
    } else if (packet.selectionChange) {
        type = WireType::PresenceSelection;
        QJsonArray ids;
        for (const QString& id : packet.selectionIds) {
            const QString safe = schemaId(id, 64);
            if (!safe.isEmpty() && ids.size() < 256) ids.append(safe);
        }
        payload.insert(QStringLiteral("entityIds"), ids);
    } else if (packet.phase == PointerPhase::Press ||
               packet.phase == PointerPhase::Release) {
        type = WireType::PresenceClick;
        payload.insert(QStringLiteral("phase"), pointerPhaseName(packet.phase));
        payload.insert(QStringLiteral("button"),
                       pointerButtonName(packet.button));
    }
    sendEnvelope(type, payload, true);
}

void CollaborationService::sendTransport(const TransportFrame& frame) {
    if (!isOnline()) return;
    if (m_commandSchemaVersion >= 6) return;
    const bool stateTransition = !m_transportSent ||
                                 frame.playing != m_lastTransportPlaying;
    if (!stateTransition && m_lastTransportSent.elapsed() < 100) return;
    QJsonObject payload{
        {QStringLiteral("playing"), frame.playing},
        {QStringLiteral("positionSeconds"),
         std::max(0.0, frame.positionSeconds)},
        {QStringLiteral("monotonicAnchorMs"),
         double(std::max<qint64>(0, m_monotonicClock.elapsed()))},
        {QStringLiteral("rate"), 1.0},
    };
    sendEnvelope(WireType::TransportState, payload, true);
    m_transportSent = true;
    m_lastTransportPlaying = frame.playing;
    m_lastTransportSent.restart();
}

bool CollaborationService::acceptHashRound(const QJsonObject& payload) {
    if (payload.size() != 4) return false;
    const QString roundId =
        canonicalUuid(payload.value(QStringLiteral("roundId")));
    const QString sessionId =
        canonicalUuid(payload.value(QStringLiteral("sessionId")));
    const auto serverSequence =
        exactSequence(payload.value(QStringLiteral("serverSeq")));
    const auto deadline =
        exactSequence(payload.value(QStringLiteral("deadlineMs")));
    if (roundId.isEmpty() || sessionId.isEmpty() ||
        sessionId != m_sessionId || !serverSequence || !deadline ||
        *deadline > quint64(std::numeric_limits<qint64>::max()) ||
        qint64(*deadline) <= estimatedServerTimeMs()) {
        return false;
    }
    m_hashRoundId = roundId;
    m_hashRoundSessionId = sessionId;
    m_hashRoundServerSequence = *serverSequence;
    m_hashRoundDeadlineMs = qint64(*deadline);
    emit hashRoundChanged(true);
    if (*serverSequence != m_bootstrapServerSequence) {
        trustedResyncRequired(false, false,
                              tr("Refreshing project for hash verification"));
        emit resyncRequired(QJsonObject{
            {QStringLiteral("reason"), QStringLiteral("hash_round_gap")},
            {QStringLiteral("snapshotSeq"),
             double(m_bootstrapServerSequence)},
            {QStringLiteral("headSeq"), double(*serverSequence)},
            {QStringLiteral("readOnly"), false},
        });
        return true;
    }
    requestOrSendRoundHash();
    return true;
}

void CollaborationService::requestOrSendRoundHash() {
    if (m_hashRoundId.isEmpty() ||
        m_hashRoundSessionId != m_sessionId ||
        m_hashRoundServerSequence != m_bootstrapServerSequence ||
        m_hashRoundDeadlineMs <= estimatedServerTimeMs()) {
        return;
    }
    if (!m_bootstrapStateHash.isEmpty()) {
        sendSnapshotHash();
        return;
    }
    emit hashRoundRequested(m_hashRoundId, m_hashRoundSessionId,
                            m_hashRoundServerSequence,
                            m_hashRoundDeadlineMs);
}

bool CollaborationService::sendSnapshotHash() {
    if (!m_transportConnected || m_bootstrapStateHash.isEmpty() ||
        m_hashRoundId.isEmpty() ||
        m_hashRoundSessionId != m_sessionId ||
        m_hashRoundServerSequence != m_bootstrapServerSequence ||
        m_hashRoundDeadlineMs <= estimatedServerTimeMs()) return false;
    return sendEnvelope(
        WireType::SnapshotHash,
        QJsonObject{
            {QStringLiteral("roundId"), m_hashRoundId},
            {QStringLiteral("serverSeq"),
             double(m_bootstrapServerSequence)},
            {QStringLiteral("sha256"), m_bootstrapStateHash},
        });
}

void CollaborationService::handleEnvelope(const WireEnvelope& envelope) {
    if (envelope.protocol != protocolName()) {
        emit protocolWarning(tr("Unexpected collaboration protocol version"));
        trustedTransportUnavailable(tr("Incompatible collaboration server"));
        return;
    }
    if (envelope.type == WireType::Unknown) {
        emit protocolWarning(
            tr("Unsupported collaboration message: %1").arg(envelope.typeName));
        return;
    }
    if (envelope.type == WireType::ClockPong) {
        if (m_commandSchemaVersion < 6 || envelope.payload.size() != 2 ||
            !envelope.participantId.isEmpty()) return;
        const auto sent = exactSequence(envelope.payload.value(QStringLiteral("clientSentAtMs")));
        const auto server = exactSequence(envelope.payload.value(QStringLiteral("serverTimeMs")));
        const qint64 current = m_clockEpochMs + m_monotonicClock.elapsed();
        if (sent && server && *sent > 0 && qint64(*sent) == m_clockPingAt) {
            const qint64 rtt = current - qint64(*sent);
            if (rtt >= 0 && rtt < 10000 && rtt <= m_bestClockRttMs + 20) {
                m_bestClockRttMs = std::min(m_bestClockRttMs, rtt);
                m_serverClockOffsetMs = qint64(*server) - (qint64(*sent) + rtt / 2);
            }
        }
        return;
    }
    if (envelope.type == WireType::SessionControlChanged || envelope.type == WireType::SessionModeChanged ||
        envelope.type == WireType::SessionParticipantModerated ||
        (envelope.type == WireType::SessionHostChanged && m_commandSchemaVersion >= 6)) {
        if (m_commandSchemaVersion < 6 || !envelope.participantId.isEmpty() || m_sessionId.isEmpty()) return;
        const auto control = envelope.payload.value(QStringLiteral("control")).toObject();
        if (!sessionControlFromJson(control)) {
            emit protocolWarning(tr("Ignored invalid session control"));
            return;
        }
        const bool acknowledged = acknowledgeControl(envelope.payload.value(QStringLiteral("actionId")).toString());
        installSessionControl(control); // Older duplicate receipts still acknowledge their action.
        if (envelope.type == WireType::SessionParticipantModerated) {
            const QString action = envelope.payload.value(QStringLiteral("action")).toString();
            const QString target = canonicalUuid(envelope.payload.value(QStringLiteral("targetUserId")));
            if (!target.isEmpty() && target == accountUserId() &&
                (action == QLatin1String("kick") || action == QLatin1String("ban"))) {
                trustedSessionExcluded(action);
                return;
            }
        }
        if (acknowledged) sendPendingControl();
        return;
    }
    if (envelope.type == WireType::SessionRequirementsChanged) {
        const auto revision = exactSequence(envelope.payload.value(QStringLiteral("pluginRequirementsRevision")));
        if (m_commandSchemaVersion < 6 || !envelope.participantId.isEmpty() || m_sessionId.isEmpty() ||
            !revision || *revision <= m_requirementsRevision || !envelope.payload.value(QStringLiteral("pluginRequirements")).isArray() ||
            !installSessionControl(envelope.payload.value(QStringLiteral("control")).toObject())) return;
        m_requirementsRevision = *revision;
        m_pendingControls.clear();
        m_sessionReadOnly = true;
        setState(CollaborationState::ReadOnly, tr("Checking updated plugin requirements"));
        emit liveSessionRequirementsChanged(m_sessionId, qint64(*revision));
        return;
    }
    if (envelope.type == WireType::SessionCatalogChanged) {
        emit pluginCatalogChanged(envelope.payload);
        return;
    }
    if (envelope.type == WireType::OpRejected) {
        const QString actionId = envelope.payload.value(QStringLiteral("actionId")).toString();
        if (acknowledgeControl(actionId)) {
            emit sessionActionRejected(safeDisplayName(envelope.payload.value(QStringLiteral("message")).toString()));
            emit sessionControlChanged();
            m_pendingControls.clear();
            return;
        }
    }
    if (envelope.type == WireType::Welcome) {
        const auto headSequence = exactSequence(
            envelope.payload.value(QStringLiteral("headSeq")));
        if (!headSequence) {
            emit protocolWarning(tr("Invalid collaboration head sequence"));
            trustedResyncRequired(false, false,
                                  tr("Project bootstrap is invalid"));
            emit resyncRequired(QJsonObject{
                {QStringLiteral("reason"), QStringLiteral("invalid_welcome")},
            });
            return;
        }
        m_sessionId = schemaId(
            envelope.payload.value(QStringLiteral("sessionId")).toString(), 64);
        QVector<ParticipantIdentity> participants;
        const QJsonArray array =
            envelope.payload.value(QStringLiteral("participants")).toArray();
        for (const QJsonValue& value : array) {
            if (const auto participant = participantFromJson(value.toObject()))
                participants.push_back(*participant);
        }
        const auto local = participantFromJson(
            envelope.payload.value(QStringLiteral("participant")).toObject());
        if (local) {
            m_localRole = local->role;
            bool present = false;
            for (const ParticipantIdentity& participant : participants)
                present = present ||
                          participant.participantId == local->participantId;
            if (!present) participants.push_back(*local);
            m_presenceStore.setLocalParticipantId(local->participantId);
        }
        m_presenceStore.replaceParticipants(participants);
        m_ownerUserId = envelope.payload.value(QStringLiteral("ownerUserId")).toString();
        m_requirementsRevision = exactSequence(envelope.payload.value(QStringLiteral("pluginRequirementsRevision"))).value_or(0);
        if (m_commandSchemaVersion >= 6) {
            m_control = {};
            if (!installSessionControl(envelope.payload.value(QStringLiteral("control")).toObject())) {
                trustedTransportUnavailable(tr("Server omitted valid session control state"));
                return;
            }
            // Initial server time handles wall-clock skew before RTT refinement.
            m_serverClockOffsetMs = envelope.serverTimeMs - (m_clockEpochMs + m_monotonicClock.elapsed());
            m_clockPingAt = m_clockEpochMs + m_monotonicClock.elapsed();
            sendEnvelope(WireType::ClockPing, {{QStringLiteral("clientSentAtMs"), double(m_clockPingAt)}});
        }
        if (m_commandSchemaVersion < 6)
            m_localSessionState.setHostParticipantId(schemaId(
                envelope.payload.value(QStringLiteral("hostParticipantId")).toString(), 64));
        emit roomIdentityChanged(
            m_sessionId, m_presenceStore.localParticipantId(),
            m_localSessionState.hostParticipantId());
        const QJsonObject limits =
            envelope.payload.value(QStringLiteral("limits")).toObject();
        m_maxMessageBytes = std::clamp(
            limits.value(QStringLiteral("maxMessageBytes")).toInt(m_maxMessageBytes),
            1024, 8 * 1024 * 1024);
        const bool readOnly =
            envelope.payload.value(QStringLiteral("readOnly")).toBool(false);
        const QString writeBlockedReason = envelope.payload
            .value(QStringLiteral("writeBlockedReason"))
            .toString();
        const QJsonValue hashRound =
            envelope.payload.value(QStringLiteral("hashRound"));
        if (!validWelcomeWriteGate(m_commandSchemaVersion, readOnly,
                                   writeBlockedReason, hashRound)) {
            emit protocolWarning(
                tr("Server omitted the collaboration write gate"));
            trustedResyncRequired(false, true,
                                  tr("Incompatible collaboration server"));
            return;
        }
        m_sessionReadOnly = readOnly;
        if (*headSequence != m_bootstrapServerSequence) {
            m_resyncPending = true;
            setState(CollaborationState::Reconnecting,
                     tr("Downloading project updates"));
            emit resyncRequired(QJsonObject{
                {QStringLiteral("reason"), QStringLiteral("welcome_gap")},
                {QStringLiteral("snapshotSeq"),
                 double(m_bootstrapServerSequence)},
                {QStringLiteral("headSeq"), double(*headSequence)},
                {QStringLiteral("readOnly"), false},
            });
            return;
        }
        m_resyncPending = false;
        if (readOnly) {
            QString detail = tr("Session is read-only");
            if (writeBlockedReason == QLatin1String("session_starting"))
                detail = tr("Waiting for the host to start the session");
            else if (writeBlockedReason == QLatin1String("plugin_not_ready"))
                detail = tr("Plugin compatibility must be resolved");
            setState(CollaborationState::ReadOnly,
                     detail);
            return;
        }
        setState(CollaborationState::Joining,
                 tr("Verifying shared project state"));
        if (!acceptHashRound(hashRound.toObject())) {
            emit protocolWarning(tr("Invalid collaboration hash round"));
            trustedResyncRequired(false, true,
                                  tr("Project hash verification failed"));
        }
        return;
    }
    if (envelope.type == WireType::HashRequested) {
        if (m_sessionReadOnly) return;
        setState(CollaborationState::Joining,
                 tr("Verifying shared project state"));
        if (!acceptHashRound(envelope.payload)) {
            emit protocolWarning(tr("Invalid collaboration hash request"));
            trustedResyncRequired(false, true,
                                  tr("Project hash verification failed"));
        }
        return;
    }
    if (envelope.type == WireType::HashVerified) {
        if (envelope.payload.size() != 2 ||
            canonicalUuid(envelope.payload.value(QStringLiteral("roundId"))) !=
                m_hashRoundId ||
            exactSequence(
                envelope.payload.value(QStringLiteral("serverSeq"))) !=
                std::optional<quint64>(m_hashRoundServerSequence)) {
            emit protocolWarning(tr("Invalid hash verification result"));
            return;
        }
        clearHashRound();
        if (!m_sessionReadOnly && !m_resyncPending) {
            setState(CollaborationState::Synced, tr("Session synced"));
        }
        return;
    }
    if (envelope.type == WireType::PresenceJoined) {
        if (const auto participant = participantFromJson(envelope.payload))
            m_presenceStore.noteParticipantConnected(*participant);
        return;
    }
    if (envelope.type == WireType::PresenceLeft) {
        m_presenceStore.removeParticipant(
            envelope.payload.value(QStringLiteral("participantId")).toString());
        return;
    }
    if (envelope.type == WireType::SessionHostChanged) {
        m_localSessionState.setHostParticipantId(
            envelope.payload.value(QStringLiteral("hostParticipantId")).toString());
        emit roomIdentityChanged(
            m_sessionId, m_presenceStore.localParticipantId(),
            m_localSessionState.hostParticipantId());
        if (!m_sessionReadOnly) {
            setState(CollaborationState::Joining,
                     tr("Verifying host handoff"));
        }
        return;
    }
    if (envelope.type == WireType::SessionReadinessChanged) {
        if (envelope.payload.size() != 4) return;
        const QString participantId = canonicalUuid(
            envelope.payload.value(QStringLiteral("participantId")));
        const QString effectiveRole = envelope.payload
            .value(QStringLiteral("effectiveRole")).toString();
        const QString readinessStatus = envelope.payload
            .value(QStringLiteral("readinessStatus")).toString();
        const auto revision = exactSequence(
            envelope.payload.value(QStringLiteral("readinessRevision")));
        if (participantId.isEmpty() || !revision || *revision == 0 ||
            (effectiveRole != QLatin1String("owner") &&
             effectiveRole != QLatin1String("editor") &&
             effectiveRole != QLatin1String("viewer")) ||
            (readinessStatus != QLatin1String("ready") &&
             readinessStatus != QLatin1String("blocked") &&
             readinessStatus != QLatin1String("viewer"))) {
            emit protocolWarning(tr("Invalid plugin readiness update"));
            return;
        }
        if (*revision < m_requirementsRevision ||
            *revision < m_readinessRevisions.value(participantId, 0)) return;
        m_readinessRevisions.insert(participantId, *revision);
        if (auto participant = m_presenceStore.participantById(participantId)) {
            participant->role = effectiveRole;
            m_presenceStore.upsertParticipant(*participant);
        }
        if (participantId == localParticipantId()) {
            m_localRole = effectiveRole;
            const bool writable = readinessStatus == QLatin1String("ready") &&
                (effectiveRole == QLatin1String("owner") || effectiveRole == QLatin1String("editor"));
            m_sessionReadOnly = !writable;
            if (!writable) {
                m_pendingControls.clear();
                clearHashRound();
                setState(CollaborationState::ReadOnly, tr("Plugin compatibility must be resolved"));
            } else if (m_state == CollaborationState::ReadOnly) {
                setState(CollaborationState::Joining, tr("Verifying shared project state"));
            }
            emit stateChanged(m_state, m_stateDetail);
        }
        emit participantReadinessChanged(
            participantId, effectiveRole, readinessStatus, qint64(*revision));
        return;
    }
    if (envelope.type == WireType::SessionActivated) {
        if (envelope.payload.size() != 2) return;
        const QString sessionId = canonicalUuid(
            envelope.payload.value(QStringLiteral("sessionId")));
        if (sessionId.isEmpty() || sessionId != m_sessionId ||
            envelope.payload.value(QStringLiteral("status")).toString() !=
                QLatin1String("active")) {
            emit protocolWarning(tr("Invalid session activation update"));
            return;
        }
        emit liveSessionActivated(sessionId);
        return;
    }
    if (envelope.type == WireType::SnapshotRequested) {
        QString error;
        const auto request = snapshotRequestFromEnvelope(
            envelope, m_sessionId, m_presenceStore.localParticipantId(),
            m_localSessionState.hostParticipantId(), &error);
        if (!request) {
            emit protocolWarning(error);
            return;
        }
        emit snapshotRequested(*request);
        return;
    }
    if (envelope.type == WireType::SessionEnding) {
        // The room must remain connected while its assigned host uploads the
        // exact final snapshot. Durable edits are blocked, presence/downloads
        // remain available, and session.ended performs the actual teardown.
        m_sessionReadOnly = true;
        emit liveSessionEnding(m_sessionId);
        setState(CollaborationState::ReadOnly,
                 tr("Saving the final project snapshot"));
        return;
    }
    if (envelope.type == WireType::SessionEnded) {
        const QString endedSessionId = m_sessionId;
        m_shouldConnect = false;
        m_transportConnected = false;
    clearSessionControl();
        m_sessionReadOnly = false;
        m_resyncPending = false;
        m_sessionId.clear();
        clearHashRound();
        m_presenceStore.clear();
        m_localSessionState.setHostParticipantId({});
        emit roomIdentityChanged({}, {}, {});
        setState(CollaborationState::LocalOnly, tr("Session ended"));
        if (!endedSessionId.isEmpty()) emit liveSessionEnded(endedSessionId);
        return;
    }
    if (envelope.type == WireType::ResyncRequired) {
        const bool conflict =
            envelope.payload.value(QStringLiteral("reason")).toString() ==
            QLatin1String("conflict");
        const bool readOnly =
            envelope.payload.value(QStringLiteral("readOnly")).toBool(false);
        trustedResyncRequired(conflict, readOnly,
                              conflict || readOnly
                                  ? tr("Project conflict requires resync")
                                  : tr("Refreshing shared project state"));
        emit resyncRequired(envelope.payload);
        return;
    }
    if (envelope.type == WireType::TransportState) {
        // v6 playback has one server-authoritative control revision. A legacy
        // presence frame must never bypass its policy or scheduled clock.
        if (m_commandSchemaVersion >= 6 || !isOnline()) return;
        TransportFrame frame;
        frame.participantId = envelope.participantId;
        frame.positionSeconds = std::max(
            0.0, envelope.payload.value(QStringLiteral("positionSeconds")).toDouble());
        frame.sentAtMs = envelope.serverTimeMs > 0 ? envelope.serverTimeMs
                                                   : envelope.sentAtMs;
        frame.scheduledAtMs = envelope.payload
                                  .value(QStringLiteral("monotonicAnchorMs"))
                                  .toInteger();
        frame.playing = envelope.payload.value(QStringLiteral("playing")).toBool();
        m_localSessionState.offerRemoteTransport(frame);
        return;
    }
    if (envelope.type == WireType::PresenceCursor ||
        envelope.type == WireType::PresenceClick ||
        envelope.type == WireType::PresenceSelection ||
        envelope.type == WireType::PresenceDrag) {
        PointerPhase phase = PointerPhase::Move;
        if (envelope.type == WireType::PresenceClick) {
            phase = pointerPhaseFromName(
                        envelope.payload.value(QStringLiteral("phase")).toString())
                        .value_or(PointerPhase::Press);
        }
        QString error;
        auto packet = parsePresencePayload(envelope, phase, &error);
        if (!packet) {
            emit protocolWarning(error);
            return;
        }
        packet->channel =
            envelope.type == WireType::PresenceClick ? PresenceChannel::Click
            : envelope.type == WireType::PresenceSelection
                ? PresenceChannel::Selection
            : envelope.type == WireType::PresenceDrag ? PresenceChannel::Drag
                                                      : PresenceChannel::Cursor;
        if (envelope.type == WireType::PresenceSelection) {
            packet->selectionChange = true;
            const QJsonArray ids =
                envelope.payload.value(QStringLiteral("entityIds")).toArray();
            for (const QJsonValue& value : ids) {
                const QString id = schemaId(value.toString(), 64);
                if (!id.isEmpty() && packet->selectionIds.size() < 256)
                    packet->selectionIds.push_back(id);
            }
        } else if (envelope.type == WireType::PresenceDrag) {
            packet->drag.active = true;
            packet->drag.kind = schemaId(
                envelope.payload.value(QStringLiteral("gesture")).toString(), 48);
            const QJsonArray ids =
                envelope.payload.value(QStringLiteral("entityIds")).toArray();
            for (const QJsonValue& value : ids) {
                const QString id = schemaId(value.toString(), 64);
                if (!id.isEmpty() && packet->drag.objectIds.size() < 64)
                    packet->drag.objectIds.push_back(id);
            }
            packet->drag.destination = packet->point;
        }
        // A cursor may outrun its presence.join. Adopt the sender from the
        // envelope rather than dropping the packet until the roster catches up;
        // the store fills in colour and nickname when the join lands.
        ParticipantIdentity identity =
            m_presenceStore.participantById(envelope.participantId)
                .value_or(ParticipantIdentity{});
        if (identity.participantId.isEmpty()) {
            identity.participantId = safeSemanticId(envelope.participantId);
            if (identity.participantId.isEmpty()) return;
        }
        m_presenceStore.applyPresence({identity, *packet});
        return;
    }
    if (envelope.type == WireType::OpCommitted ||
        envelope.type == WireType::OpRejected ||
        envelope.type == WireType::LeaseGranted ||
        envelope.type == WireType::LeaseDenied) {
        if (envelope.type == WireType::OpRejected) {
            const QString code =
                envelope.payload.value(QStringLiteral("code")).toString();
            if (code == QLatin1String("sequence_gap")) {
                trustedResyncRequired(
                    false, false,
                    tr("Refreshing shared project after a sequence gap"));
            } else if (code == QLatin1String("conflict")) {
                trustedResyncRequired(
                    true, true, tr("Operation conflicted with the session"));
            }
        }
        emit durableEnvelopeReceived(envelope);
    }
}

bool checkCollaborationPresenceSafetyForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!validWelcomeWriteGate(
            2, false, QStringLiteral("hash_consensus_required"), QJsonObject{}) ||
        !validWelcomeWriteGate(
            2, true, QStringLiteral("role_read_only"), QJsonValue::Null) ||
        validWelcomeWriteGate(
            2, true, QStringLiteral("session_starting"), QJsonValue::Null) ||
        !validWelcomeWriteGate(
            3, true, QStringLiteral("session_starting"), QJsonValue::Null) ||
        !validWelcomeWriteGate(
            3, true, QStringLiteral("plugin_not_ready"), QJsonValue::Null) ||
        validWelcomeWriteGate(
            2, true, QStringLiteral("hash_consensus_required"), QJsonObject{}) ||
        validWelcomeWriteGate(
            3, true, QStringLiteral("unexpected"), QJsonValue::Null) ||
        validWelcomeWriteGate(
            3, false, QStringLiteral("hash_consensus_required"), QJsonValue::Null)) {
        return fail(QStringLiteral("welcome write gate accepted an invalid pair"));
    }
    PresencePacket coarse;
    coarse.policy = PresencePolicy::Exact; // Service must cap this surface.
    coarse.point.surface.kind = SurfaceKind::Browser;
    coarse.point.normalized = QPointF(0.9, 0.1);
    coarse.point.timeSeconds = 123.0;
    coarse.point.targetId = QStringLiteral("private.item");
    const QJsonObject coarseJson = safePresencePayload(coarse);
    if (coarseJson.size() != 2 ||
        coarseJson.value(QStringLiteral("surface")).toString() !=
            QLatin1String("file_browser") ||
        coarseJson.value(QStringLiteral("precision")).toString() !=
            QLatin1String("coarse") ||
        coarseJson.contains(QStringLiteral("u")) ||
        coarseJson.contains(QStringLiteral("targetId")) ||
        coarseJson.contains(QStringLiteral("timeSeconds"))) {
        return fail(QStringLiteral("coarse presence leaked semantic coordinates"));
    }

    PresencePacket nativePlugin = coarse;
    nativePlugin.point.surface.kind = SurfaceKind::ThirdPartyPlugin;
    const QJsonObject hiddenJson = safePresencePayload(nativePlugin);
    if (hiddenJson.size() != 2 ||
        hiddenJson.value(QStringLiteral("surface")).toString() !=
            QLatin1String("hidden") ||
        hiddenJson.value(QStringLiteral("precision")).toString() !=
            QLatin1String("hidden")) {
        return fail(QStringLiteral("third-party plugin presence was not hidden"));
    }

    PresencePacket timeline;
    timeline.policy = PresencePolicy::Exact;
    timeline.point.surface.kind = SurfaceKind::Timeline;
    timeline.point.normalized = QPointF(0.25, 0.75);
    timeline.point.timeSeconds = 10.0;
    timeline.point.trackId = QStringLiteral("track-1");
    timeline.point.laneFraction = 0.4;
    const QJsonObject exactJson = safePresencePayload(timeline);
    if (!exactJson.contains(QStringLiteral("u")) ||
        !exactJson.contains(QStringLiteral("timeSeconds")) ||
        exactJson.value(QStringLiteral("trackId")).toString() !=
            QLatin1String("track-1") ||
        std::abs(exactJson.value(QStringLiteral("laneFraction")).toDouble() -
                 0.4) > 1e-9) {
        return fail(QStringLiteral("exact timeline presence lost semantic fields"));
    }

    LocalSessionState localSession;
    localSession.setHostParticipantId(QStringLiteral("participant-host"));
    localSession.followHost();
    TransportFrame wrong;
    wrong.participantId = QStringLiteral("participant-other");
    wrong.sentAtMs = 10;
    TransportFrame host = wrong;
    host.participantId = QStringLiteral("participant-host");
    if (localSession.transportMode() != TransportMode::FollowHost ||
        localSession.offerRemoteTransport(wrong) ||
        !localSession.offerRemoteTransport(host)) {
        return fail(QStringLiteral("follow-host transport accepted the wrong participant"));
    }
    localSession.noteLocalTransportInteraction();
    if (localSession.transportMode() != TransportMode::Independent)
        return fail(QStringLiteral("local transport did not leave follow mode"));

    QJsonObject control = QJsonDocument::fromJson(R"({
      "mode":"synchronized","sessionVersion":1,
      "hostMemberId":"00000000-0000-4000-8000-000000000001",
      "transport":{"revision":0,"playing":false,"positionSeconds":0,"rate":1,
        "serverTimeMs":1000,"effectiveAtServerMs":1000,"loopEnabled":false,
        "loopStartSeconds":0,"loopEndSeconds":0},
      "audition":{"revision":0,"mutedTrackIds":[],"soloTrackIds":[]}
    })").object();
    if (!sessionControlFromJson(control)) return fail(QStringLiteral("valid v6 control rejected"));
    for (const QString& field : {QStringLiteral("rate"), QStringLiteral("playing"), QStringLiteral("serverTimeMs")}) {
        auto invalid = control;
        auto transport = invalid.value(QStringLiteral("transport")).toObject();
        transport.remove(field);
        invalid.insert(QStringLiteral("transport"), transport);
        if (sessionControlFromJson(invalid)) return fail(QStringLiteral("missing control fields were silently defaulted"));
    }
    auto invalidControl = control;
    auto invalidTransport = control.value(QStringLiteral("transport")).toObject();
    invalidTransport.insert(QStringLiteral("rate"), QStringLiteral("1"));
    invalidControl.insert(QStringLiteral("transport"), invalidTransport);
    if (sessionControlFromJson(invalidControl)) return fail(QStringLiteral("string playback rate accepted"));
    invalidControl = control;
    invalidControl.insert(QStringLiteral("hostMemberId"), QStringLiteral(""));
    if (sessionControlFromJson(invalidControl)) return fail(QStringLiteral("empty host id accepted instead of null"));

    CollaborationService service(nullptr);
    service.m_projectId = QStringLiteral("00000000-0000-4000-8000-000000000010");
    service.m_sessionId = QStringLiteral("00000000-0000-4000-8000-000000000011");
    service.m_presenceStore.setLocalParticipantId(QStringLiteral("00000000-0000-4000-8000-000000000002"));
    service.m_transportConnected = true;
    service.m_state = CollaborationState::Synced;
    service.m_localRole = QStringLiteral("editor");
    if (!service.installSessionControl(control)) return fail(QStringLiteral("initial control not installed"));
    const QString originalHost = service.m_control.hostMemberId;
    service.m_control.hostMemberId = service.localParticipantId();
    service.m_localRole = QStringLiteral("owner");
    if (!service.canChangeSessionMode()) return fail(QStringLiteral("ready conductor cannot change the session mode"));
    service.m_sessionReadOnly = true;
    if (service.canChangeSessionMode()) return fail(QStringLiteral("read-only conductor can change session mode"));
    service.m_sessionReadOnly = false;
    service.m_state = CollaborationState::Joining;
    if (service.canChangeSessionMode()) return fail(QStringLiteral("unready conductor can change session mode"));
    service.m_state = CollaborationState::Synced;
    service.m_localRole.clear();
    if (service.canChangeSessionMode()) return fail(QStringLiteral("unknown role can change session mode"));
    service.m_localRole = QStringLiteral("editor");
    service.m_control.hostMemberId = originalHost;
    int legacyAccepted = 0;
    QObject::connect(&service.m_localSessionState, &LocalSessionState::remoteTransportAccepted,
                     &service, [&](const TransportFrame&) { ++legacyAccepted; });
    service.m_localSessionState.setHostParticipantId(originalHost);
    service.m_localSessionState.followHost();
    WireEnvelope legacyFrame;
    legacyFrame.type = WireType::TransportState;
    legacyFrame.participantId = originalHost;
    legacyFrame.sentAtMs = 1000;
    legacyFrame.payload = {{QStringLiteral("positionSeconds"), 4}, {QStringLiteral("playing"), true}};
    service.handleEnvelope(legacyFrame);
    if (legacyAccepted != 0) return fail(QStringLiteral("legacy frame bypassed v6 transport control"));
    service.m_commandSchemaVersion = 5;
    legacyFrame.protocol = service.protocolName();
    service.handleEnvelope(legacyFrame);
    if (legacyAccepted != 1) return fail(QStringLiteral("legacy transport compatibility was removed"));
    service.m_commandSchemaVersion = 6;
    service.m_localSessionState.useIndependentTransport();
    QList<QJsonObject> sentControls;
    QObject::connect(&service, &CollaborationService::outboundTextMessage, &service, [&](const QString& message) {
        const auto json = QJsonDocument::fromJson(message.toUtf8()).object();
        if (json.value(QStringLiteral("type")).toString() == QLatin1String("session.control"))
            sentControls.push_back(json.value(QStringLiteral("payload")).toObject());
    });
    if (!service.submitSessionControl(QStringLiteral("play")) ||
        !service.submitSessionControl(QStringLiteral("stop")) ||
        !service.submitSessionControl(QStringLiteral("seek"), {{QStringLiteral("positionSeconds"), 12.5}}) ||
        sentControls.size() != 1 || service.m_pendingControls.size() != 3)
        return fail(QStringLiteral("rapid transport gestures were not serialized"));
    service.sendPendingControl();
    if (sentControls.size() != 2 || sentControls[0] != sentControls[1])
        return fail(QStringLiteral("transport retry changed action identity or overtook the queue"));
    const auto acknowledge = [&](int revision) {
        auto transport = control.value(QStringLiteral("transport")).toObject();
        transport.insert(QStringLiteral("revision"), revision);
        control.insert(QStringLiteral("transport"), transport);
        WireEnvelope reply;
        reply.type = WireType::SessionControlChanged;
        reply.payload = {{QStringLiteral("actionId"), service.m_pendingControls.front().value(QStringLiteral("actionId"))},
                         {QStringLiteral("control"), control}};
        service.handleEnvelope(reply);
        return reply;
    };
    auto firstReceipt = acknowledge(1);
    if (sentControls.back().value(QStringLiteral("kind")).toString() != QLatin1String("stop"))
        return fail(QStringLiteral("Stop did not follow acknowledged Play"));
    acknowledge(2);
    if (sentControls.back().value(QStringLiteral("kind")).toString() != QLatin1String("seek"))
        return fail(QStringLiteral("Seek did not follow acknowledged Stop"));
    acknowledge(3);
    service.handleEnvelope(firstReceipt);
    if (!service.m_pendingControls.isEmpty() || service.m_control.transportRevision != 3)
        return fail(QStringLiteral("duplicate receipt rewound playback"));
    if (service.submitSessionControl(QStringLiteral("seek"), {{QStringLiteral("positionSeconds"), -1}}))
        return fail(QStringLiteral("negative seek was submitted"));
    service.submitSessionControl(QStringLiteral("play"));
    control.insert(QStringLiteral("sessionVersion"), 2);
    control.insert(QStringLiteral("mode"), QStringLiteral("follow_host"));
    service.installSessionControl(control);
    if (!service.m_pendingControls.isEmpty() || service.canControlSession() || service.canSubmitOperations())
        return fail(QStringLiteral("host-only policy left follower writes or stale controls enabled"));
    control.insert(QStringLiteral("sessionVersion"), 3);
    control.insert(QStringLiteral("mode"), QStringLiteral("synchronized"));
    service.installSessionControl(control);

    // An echoed ping uses elapsed time anchored once, not a moving wall clock.
    service.m_clockPingAt = service.m_clockEpochMs + service.m_monotonicClock.elapsed() - 40;
    WireEnvelope pong;
    pong.type = WireType::ClockPong;
    pong.payload = {{QStringLiteral("clientSentAtMs"), double(service.m_clockPingAt)},
                    {QStringLiteral("serverTimeMs"), double(service.m_clockPingAt + 2020)}};
    service.handleEnvelope(pong);
    if (std::abs(service.m_serverClockOffsetMs - 2000) > 30 || service.m_bestClockRttMs > 100)
        return fail(QStringLiteral("clock estimate ignored round-trip delay"));
    const qint64 acceptedOffset = service.m_serverClockOffsetMs;
    pong.payload.insert(QStringLiteral("clientSentAtMs"), double(service.m_clockPingAt - 1));
    service.handleEnvelope(pong);
    if (service.m_serverClockOffsetMs != acceptedOffset)
        return fail(QStringLiteral("unsolicited clock echo changed the estimate"));

    WireEnvelope readiness;
    readiness.type = WireType::SessionReadinessChanged;
    readiness.payload = {{QStringLiteral("participantId"), service.localParticipantId()},
        {QStringLiteral("effectiveRole"), QStringLiteral("viewer")},
        {QStringLiteral("readinessStatus"), QStringLiteral("blocked")},
        {QStringLiteral("readinessRevision"), 2}};
    service.handleEnvelope(readiness);
    if (service.m_localRole != QLatin1String("viewer") || service.canSubmitOperations())
        return fail(QStringLiteral("readiness revocation left local writes enabled"));
    readiness.payload.insert(QStringLiteral("readinessRevision"), 1);
    readiness.payload.insert(QStringLiteral("effectiveRole"), QStringLiteral("editor"));
    readiness.payload.insert(QStringLiteral("readinessStatus"), QStringLiteral("ready"));
    service.handleEnvelope(readiness);
    if (service.m_localRole != QLatin1String("viewer"))
        return fail(QStringLiteral("stale readiness promoted a blocked participant"));
    service.m_localRole = QStringLiteral("editor");
    service.m_sessionReadOnly = false;
    service.m_state = CollaborationState::Synced;
    service.submitSessionControl(QStringLiteral("play"));
    service.trustedTransportDisconnected();
    if (!service.m_pendingControls.isEmpty() || service.m_control.sessionVersion != 0 ||
        service.m_serverClockOffsetMs != 0 || !service.m_localRole.isEmpty())
        return fail(QStringLiteral("disconnect retained stale controls or authorization"));
    const qsizetype sentBeforeLateReceipt = sentControls.size();
    service.handleEnvelope(firstReceipt);
    if (service.m_control.sessionVersion != 0 || sentControls.size() != sentBeforeLateReceipt)
        return fail(QStringLiteral("late disconnected receipt revived an old session"));
    return true;
}

} // namespace collab
