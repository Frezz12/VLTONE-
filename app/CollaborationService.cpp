#include "CollaborationService.hpp"

#include "AccountService.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/ProjectCommand.hpp"

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
    m_lastTransportSent.start();
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

void CollaborationService::setProjectId(const QString& projectId,
                                        bool requestConnection) {
    const QUuid uuid(projectId);
    const QString safe = uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
    if (safe == m_projectId && requestConnection == m_shouldConnect) return;
    m_projectId = safe;
    m_sessionId.clear();
    m_hashRoundId.clear();
    m_hashRoundSessionId.clear();
    m_hashRoundServerSequence = 0;
    m_hashRoundDeadlineMs = 0;
    m_bootstrapServerSequence = 0;
    m_bootstrapStateHash.clear();
    m_transportConnected = false;
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
    m_hashRoundId.clear();
    m_hashRoundSessionId.clear();
    m_hashRoundServerSequence = 0;
    m_hashRoundDeadlineMs = 0;
    m_bootstrapServerSequence = 0;
    m_bootstrapStateHash.clear();
    m_shouldConnect = false;
    m_transportConnected = false;
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
    m_sessionId.clear();
    m_hashRoundId.clear();
    m_hashRoundSessionId.clear();
    m_hashRoundServerSequence = 0;
    m_hashRoundDeadlineMs = 0;
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
    m_sessionReadOnly = false;
    m_resyncPending = false;
    m_authorizationRequested = false;
    m_sessionId.clear();
    m_hashRoundId.clear();
    m_hashRoundSessionId.clear();
    m_hashRoundServerSequence = 0;
    m_hashRoundDeadlineMs = 0;
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
         int(daw::collab::kProjectCommandSchemaVersion)},
        {QStringLiteral("projectFormatVersion"),
         daw::ProjectSerializer::kFormatVersion},
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
    m_transportSent = false;
    m_presenceStore.clear();
    m_sessionId.clear();
    m_hashRoundId.clear();
    m_hashRoundSessionId.clear();
    m_hashRoundServerSequence = 0;
    m_hashRoundDeadlineMs = 0;
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
    m_transportSent = false;
    m_presenceStore.clear();
    m_sessionId.clear();
    m_hashRoundId.clear();
    m_hashRoundSessionId.clear();
    m_hashRoundServerSequence = 0;
    m_hashRoundDeadlineMs = 0;
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
        m_authorizationRequested = false;
        m_presenceStore.clear();
        setState(CollaborationState::SignedOut, tr("Sign in to collaborate"));
        return;
    }
    if (m_account->snapshot().offline) {
        m_transportConnected = false;
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
           !m_pendingRecoveryBlocked;
}

bool CollaborationService::canSubmitRecoveryOperations() const {
    return m_transportConnected && m_state == CollaborationState::Synced &&
           m_pendingRecoveryBlocked;
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
    return sendEnvelope(WireType::OpSubmit,
                        QJsonObject{{QStringLiteral("command"), command}});
}

bool CollaborationService::submitRecoveryOperation(
    const QJsonObject& command) {
    if (!canSubmitRecoveryOperations() || command.isEmpty()) return false;
    return sendEnvelope(WireType::OpSubmit,
                        QJsonObject{{QStringLiteral("command"), command}});
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
        qint64(*deadline) <= nowMs()) {
        return false;
    }
    m_hashRoundId = roundId;
    m_hashRoundSessionId = sessionId;
    m_hashRoundServerSequence = *serverSequence;
    m_hashRoundDeadlineMs = qint64(*deadline);
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
        m_hashRoundDeadlineMs <= nowMs()) {
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
        m_hashRoundDeadlineMs <= nowMs()) return false;
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
    if (envelope.type == WireType::Unknown) {
        emit protocolWarning(
            tr("Unsupported collaboration message: %1").arg(envelope.typeName));
        return;
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
            bool present = false;
            for (const ParticipantIdentity& participant : participants)
                present = present ||
                          participant.participantId == local->participantId;
            if (!present) participants.push_back(*local);
            m_presenceStore.setLocalParticipantId(local->participantId);
        }
        m_presenceStore.replaceParticipants(participants);
        m_localSessionState.setHostParticipantId(schemaId(
            envelope.payload.value(QStringLiteral("hostParticipantId")).toString(),
            64));
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
        if (writeBlockedReason.isEmpty() ||
            (!readOnly &&
             (writeBlockedReason != QLatin1String("hash_consensus_required") ||
              !hashRound.isObject()))) {
            emit protocolWarning(
                tr("Server omitted the collaboration v2 write gate"));
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
            setState(CollaborationState::ReadOnly,
                     tr("Session is read-only"));
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
        m_hashRoundId.clear();
        m_hashRoundSessionId.clear();
        m_hashRoundServerSequence = 0;
        m_hashRoundDeadlineMs = 0;
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
        m_sessionReadOnly = false;
        m_resyncPending = false;
        m_sessionId.clear();
        m_hashRoundId.clear();
        m_hashRoundSessionId.clear();
        m_hashRoundServerSequence = 0;
        m_hashRoundDeadlineMs = 0;
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
    return true;
}

} // namespace collab
