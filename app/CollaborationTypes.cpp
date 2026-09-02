#include "CollaborationTypes.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace collab {
namespace {

QString enumName(const char* value) { return QString::fromLatin1(value); }

void setError(QString* error, const QString& value) {
    if (error) *error = value;
}

double finiteNumber(const QJsonValue& value, double fallback = -1.0) {
    const double number = value.toDouble(fallback);
    return std::isfinite(number) ? number : fallback;
}

qint64 integerValue(const QJsonValue& value, qint64 fallback = 0) {
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::isfinite(number)) return qint64(number);
    }
    bool ok = false;
    const qint64 number = value.toString().toLongLong(&ok);
    return ok ? number : fallback;
}

bool validWireTypeName(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][a-z0-9_.-]{0,63}$"));
    return pattern.match(value).hasMatch();
}

bool strictSnapshotRequestEnvelope(const QJsonObject& json) {
    static const QStringList keys{
        QStringLiteral("protocol"), QStringLiteral("type"),
        QStringLiteral("messageId"), QStringLiteral("serverTimeMs"),
        QStringLiteral("payload"),
    };
    if (json.size() != keys.size() ||
        !std::all_of(keys.begin(), keys.end(), [&json](const QString& key) {
            return json.contains(key);
        }) ||
        !json.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const QString messageId =
        json.value(QStringLiteral("messageId")).toString();
    const QUuid uuid(messageId);
    if (uuid.isNull() ||
        uuid.toString(QUuid::WithoutBraces).toLower() != messageId) {
        return false;
    }
    const QJsonValue timestamp = json.value(QStringLiteral("serverTimeMs"));
    if (!timestamp.isDouble()) return false;
    const double number = timestamp.toDouble(-1.0);
    return std::isfinite(number) && number >= 0.0 &&
           number <= 9007199254740991.0 && std::floor(number) == number;
}

bool isEphemeralType(WireType type) {
    return type == WireType::PresenceCursor || type == WireType::PresenceClick ||
           type == WireType::PresenceSelection || type == WireType::PresenceDrag ||
           type == WireType::TransportState || type == WireType::TransportFollow;
}

QJsonArray safeIdArray(const QStringList& values) {
    QJsonArray result;
    const qsizetype count = std::min<qsizetype>(values.size(), 512);
    for (qsizetype i = 0; i < count; ++i) {
        const QString safe = safeSemanticId(values.at(i));
        if (!safe.isEmpty()) result.append(safe);
    }
    return result;
}

QStringList idArray(const QJsonValue& value) {
    QStringList result;
    const QJsonArray array = value.toArray();
    const qsizetype count = std::min<qsizetype>(array.size(), 512);
    result.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        const QString id = array.at(i).toString();
        if (isWireSafeSemanticId(id)) result.push_back(id);
    }
    return result;
}

} // namespace

QString collaborationStateName(CollaborationState state) {
    switch (state) {
        case CollaborationState::LocalOnly: return enumName("local_only");
        case CollaborationState::Unavailable: return enumName("unavailable");
        case CollaborationState::SignedOut: return enumName("signed_out");
        case CollaborationState::NoConnection: return enumName("no_connection");
        case CollaborationState::Uploading: return enumName("uploading");
        case CollaborationState::Connecting: return enumName("connecting");
        case CollaborationState::Joining: return enumName("joining");
        case CollaborationState::Synced: return enumName("synced");
        case CollaborationState::Reconnecting: return enumName("reconnecting");
        case CollaborationState::ReadOnly: return enumName("read_only");
        case CollaborationState::Conflict: return enumName("conflict");
        case CollaborationState::Error: return enumName("error");
    }
    return enumName("unknown");
}

QString surfaceKindName(SurfaceKind kind) {
    switch (kind) {
        case SurfaceKind::Unknown: return enumName("unknown");
        case SurfaceKind::Shell: return enumName("shell");
        case SurfaceKind::Transport: return enumName("transport");
        case SurfaceKind::Timeline: return enumName("timeline");
        case SurfaceKind::TrackList: return enumName("track_list");
        case SurfaceKind::Mixer: return enumName("mixer");
        case SurfaceKind::PianoRoll: return enumName("piano_roll");
        case SurfaceKind::AutomationEditor: return enumName("automation_editor");
        case SurfaceKind::SampleEditor: return enumName("sample_editor");
        case SurfaceKind::BuiltinPlugin: return enumName("builtin_plugin");
        case SurfaceKind::GenericPlugin: return enumName("generic_plugin");
        case SurfaceKind::ThirdPartyPlugin: return enumName("third_party_plugin");
        case SurfaceKind::Browser: return enumName("browser");
        case SurfaceKind::Web: return enumName("web");
        case SurfaceKind::Ai: return enumName("ai");
        case SurfaceKind::Settings: return enumName("settings");
        case SurfaceKind::Dialog: return enumName("dialog");
    }
    return enumName("unknown");
}

std::optional<SurfaceKind> surfaceKindFromName(const QString& name) {
    for (int value = int(SurfaceKind::Unknown);
         value <= int(SurfaceKind::Dialog); ++value) {
        const auto kind = SurfaceKind(value);
        if (surfaceKindName(kind) == name) return kind;
    }
    return std::nullopt;
}

QString pointerPhaseName(PointerPhase phase) {
    switch (phase) {
        case PointerPhase::Move: return enumName("move");
        case PointerPhase::Press: return enumName("press");
        case PointerPhase::Release: return enumName("release");
        case PointerPhase::Leave: return enumName("leave");
    }
    return enumName("move");
}

std::optional<PointerPhase> pointerPhaseFromName(const QString& name) {
    if (name == QLatin1String("move")) return PointerPhase::Move;
    if (name == QLatin1String("press")) return PointerPhase::Press;
    if (name == QLatin1String("release")) return PointerPhase::Release;
    if (name == QLatin1String("leave")) return PointerPhase::Leave;
    return std::nullopt;
}

QString wireTypeName(WireType type) {
    switch (type) {
        case WireType::Unknown: return enumName("unknown");
        case WireType::Hello: return enumName("hello");
        case WireType::Welcome: return enumName("welcome");
        case WireType::OpSubmit: return enumName("op.submit");
        case WireType::OpCommitted: return enumName("op.committed");
        case WireType::OpRejected: return enumName("op.rejected");
        case WireType::PresenceJoined: return enumName("presence.join");
        case WireType::PresenceLeft: return enumName("presence.leave");
        case WireType::PresenceCursor: return enumName("presence.cursor");
        case WireType::PresenceClick: return enumName("presence.click");
        case WireType::PresenceSelection: return enumName("presence.selection");
        case WireType::PresenceDrag: return enumName("presence.drag");
        case WireType::TransportState: return enumName("transport.state");
        case WireType::TransportFollow: return enumName("transport.follow");
        case WireType::LeaseAcquire: return enumName("lease.acquire");
        case WireType::LeaseGranted: return enumName("lease.granted");
        case WireType::LeaseDenied: return enumName("lease.denied");
        case WireType::LeaseRenew: return enumName("lease.renew");
        case WireType::LeaseRelease: return enumName("lease.release");
        case WireType::SessionHandoff: return enumName("session.handoff");
        case WireType::SessionHostChanged:
            return enumName("session.host_changed");
        case WireType::SessionEnding: return enumName("session.ending");
        case WireType::SnapshotRequested:
            return enumName("snapshot.requested");
        case WireType::SessionEnded: return enumName("session.ended");
        case WireType::ResyncRequired: return enumName("resync.required");
        case WireType::HashRequested: return enumName("hash.requested");
        case WireType::HashVerified: return enumName("hash.verified");
        case WireType::SnapshotHash: return enumName("snapshot.hash");
    }
    return enumName("unknown");
}

std::optional<WireType> wireTypeFromName(const QString& name) {
    for (int value = int(WireType::Hello);
         value <= int(WireType::SnapshotHash); ++value) {
        const auto type = WireType(value);
        if (wireTypeName(type) == name) return type;
    }
    return std::nullopt;
}

bool isWireSafeSemanticId(const QString& value) {
    if (value.isEmpty() || value.size() > 160) return false;
    static const QRegularExpression plain(
        QStringLiteral("^[A-Za-z0-9_.:@-]+$"));
    static const QRegularExpression hashed(
        QStringLiteral("^sha256:[0-9a-f]{32}$"));
    return plain.match(value).hasMatch() || hashed.match(value).hasMatch();
}

QString safeSemanticId(const QString& value) {
    if (value.isEmpty()) return {};
    if (isWireSafeSemanticId(value) && !value.contains(QStringLiteral("://")))
        return value;
    const QByteArray digest =
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(32);
    return QStringLiteral("sha256:") + QString::fromLatin1(digest);
}

QString safeDisplayName(const QString& value) {
    QString result;
    result.reserve(std::min<qsizetype>(48, value.size()));
    for (const QChar character : value) {
        if (result.size() >= 48) break;
        if (character.isPrint() && character != QChar::LineSeparator &&
            character != QChar::ParagraphSeparator) {
            result.append(character);
        }
    }
    result = result.simplified();
    return result.isEmpty() ? QStringLiteral("Participant") : result;
}

QPointF normalizedSurfacePoint(const QPointF& localPosition,
                               const QSize& logicalSize) {
    const qreal width = std::max(1, logicalSize.width());
    const qreal height = std::max(1, logicalSize.height());
    const qreal x = std::isfinite(localPosition.x()) ? localPosition.x() : 0.0;
    const qreal y = std::isfinite(localPosition.y()) ? localPosition.y() : 0.0;
    return QPointF(std::clamp(x / width, qreal(0.0), qreal(1.0)),
                   std::clamp(y / height, qreal(0.0), qreal(1.0)));
}

std::optional<QPointF> surfacePointFromNormalized(
    const SemanticPoint& point, const QSize& logicalSize) {
    if (!std::isfinite(point.normalized.x()) ||
        !std::isfinite(point.normalized.y()) || point.normalized.x() < 0.0 ||
        point.normalized.y() < 0.0) {
        return std::nullopt;
    }
    return QPointF(std::clamp(point.normalized.x(), qreal(0.0), qreal(1.0)) *
                       std::max(1, logicalSize.width()),
                   std::clamp(point.normalized.y(), qreal(0.0), qreal(1.0)) *
                       std::max(1, logicalSize.height()));
}

QJsonObject semanticPointToJson(const SemanticPoint& point) {
    QJsonObject json{
        {QStringLiteral("surface"), surfaceKindName(point.surface.kind)},
    };
    const auto addId = [&json](const char* key, const QString& value) {
        const QString safe = safeSemanticId(value);
        if (!safe.isEmpty()) json.insert(QString::fromLatin1(key), safe);
    };
    addId("instance_id", point.surface.instanceId);
    addId("context_id", point.surface.contextId);
    addId("target_id", point.targetId);
    addId("track_id", point.trackId);
    addId("clip_id", point.clipId);
    addId("parameter_id", point.parameterId);

    if (std::isfinite(point.normalized.x()) &&
        std::isfinite(point.normalized.y()) && point.normalized.x() >= 0.0 &&
        point.normalized.y() >= 0.0) {
        json.insert(QStringLiteral("u"), std::clamp(point.normalized.x(), 0.0, 1.0));
        json.insert(QStringLiteral("v"), std::clamp(point.normalized.y(), 0.0, 1.0));
    }
    if (std::isfinite(point.timeSeconds) && point.timeSeconds >= 0.0)
        json.insert(QStringLiteral("time_seconds"), point.timeSeconds);
    if (std::isfinite(point.beat) && point.beat >= 0.0)
        json.insert(QStringLiteral("beat"), point.beat);
    if (point.pitch >= 0 && point.pitch <= 127)
        json.insert(QStringLiteral("pitch"), point.pitch);
    if (std::isfinite(point.laneFraction) && point.laneFraction >= 0.0)
        json.insert(QStringLiteral("lane_fraction"),
                    std::clamp(point.laneFraction, 0.0, 1.0));
    return json;
}

std::optional<SemanticPoint> semanticPointFromJson(const QJsonObject& json,
                                                   QString* error) {
    const auto kind = surfaceKindFromName(json.value(QStringLiteral("surface")).toString());
    if (!kind || *kind == SurfaceKind::Unknown) {
        setError(error, QStringLiteral("invalid presence surface"));
        return std::nullopt;
    }
    SemanticPoint point;
    point.surface.kind = *kind;
    const auto readId = [&json](const char* key) {
        const QString value = json.value(QString::fromLatin1(key)).toString();
        return isWireSafeSemanticId(value) ? value : QString();
    };
    point.surface.instanceId = readId("instance_id");
    point.surface.contextId = readId("context_id");
    point.targetId = readId("target_id");
    point.trackId = readId("track_id");
    point.clipId = readId("clip_id");
    point.parameterId = readId("parameter_id");
    point.normalized = QPointF(
        std::clamp(finiteNumber(json.value(QStringLiteral("u"))), -1.0, 1.0),
        std::clamp(finiteNumber(json.value(QStringLiteral("v"))), -1.0, 1.0));
    point.timeSeconds =
        std::max(-1.0, finiteNumber(json.value(QStringLiteral("time_seconds"))));
    point.beat = std::max(-1.0, finiteNumber(json.value(QStringLiteral("beat"))));
    point.pitch = json.value(QStringLiteral("pitch")).toInt(-1);
    if (point.pitch < -1 || point.pitch > 127) point.pitch = -1;
    point.laneFraction = std::clamp(
        finiteNumber(json.value(QStringLiteral("lane_fraction"))), -1.0, 1.0);
    return point;
}

QJsonObject presencePacketToJson(const PresencePacket& packet) {
    QJsonObject json{
        {QStringLiteral("client_sequence"), QString::number(packet.clientSequence)},
        {QStringLiteral("sent_at_ms"), QString::number(packet.sentAtMs)},
        {QStringLiteral("phase"), pointerPhaseName(packet.phase)},
        {QStringLiteral("policy"), packet.policy == PresencePolicy::Exact
                                      ? QStringLiteral("exact")
                                      : packet.policy == PresencePolicy::Coarse
                                            ? QStringLiteral("coarse")
                                            : QStringLiteral("hidden")},
        {QStringLiteral("point"), semanticPointToJson(packet.point)},
    };
    const QJsonArray selection = safeIdArray(packet.selectionIds);
    if (packet.selectionChange) {
        json.insert(QStringLiteral("selection_change"), true);
        json.insert(QStringLiteral("selection_ids"), selection);
    }
    if (packet.drag.active) {
        QJsonObject drag{
            {QStringLiteral("active"), true},
            {QStringLiteral("kind"), safeSemanticId(packet.drag.kind)},
            {QStringLiteral("object_ids"), safeIdArray(packet.drag.objectIds)},
            {QStringLiteral("destination"),
             semanticPointToJson(packet.drag.destination)},
        };
        json.insert(QStringLiteral("drag"), drag);
    }
    return json;
}

std::optional<PresencePacket> presencePacketFromJson(const QJsonObject& json,
                                                     QString* error) {
    const auto phase = pointerPhaseFromName(json.value(QStringLiteral("phase")).toString());
    if (!phase) {
        setError(error, QStringLiteral("invalid pointer phase"));
        return std::nullopt;
    }
    const QString policyName = json.value(QStringLiteral("policy")).toString();
    PresencePolicy policy = PresencePolicy::Hidden;
    if (policyName == QLatin1String("exact")) policy = PresencePolicy::Exact;
    else if (policyName == QLatin1String("coarse")) policy = PresencePolicy::Coarse;
    else if (policyName != QLatin1String("hidden")) {
        setError(error, QStringLiteral("invalid presence policy"));
        return std::nullopt;
    }
    const auto point = semanticPointFromJson(
        json.value(QStringLiteral("point")).toObject(), error);
    if (!point) return std::nullopt;

    PresencePacket packet;
    packet.clientSequence = quint64(std::max<qint64>(
        0, integerValue(json.value(QStringLiteral("client_sequence")))));
    packet.sentAtMs = integerValue(json.value(QStringLiteral("sent_at_ms")));
    packet.phase = *phase;
    packet.policy = policy;
    packet.point = *point;
    packet.selectionChange =
        json.value(QStringLiteral("selection_change")).toBool(false);
    packet.selectionIds = idArray(json.value(QStringLiteral("selection_ids")));
    const QJsonObject drag = json.value(QStringLiteral("drag")).toObject();
    packet.drag.active = drag.value(QStringLiteral("active")).toBool(false);
    if (packet.drag.active) {
        const QString kind = drag.value(QStringLiteral("kind")).toString();
        packet.drag.kind = isWireSafeSemanticId(kind) ? kind : QString();
        packet.drag.objectIds = idArray(drag.value(QStringLiteral("object_ids")));
        const auto destination = semanticPointFromJson(
            drag.value(QStringLiteral("destination")).toObject(), error);
        if (!destination) return std::nullopt;
        packet.drag.destination = *destination;
    }
    return packet;
}

QJsonObject transportFrameToJson(const TransportFrame& frame) {
    return {
        {QStringLiteral("participant_id"), safeSemanticId(frame.participantId)},
        {QStringLiteral("position_seconds"), std::max(0.0, frame.positionSeconds)},
        {QStringLiteral("sent_at_ms"), QString::number(frame.sentAtMs)},
        {QStringLiteral("scheduled_at_ms"), QString::number(frame.scheduledAtMs)},
        {QStringLiteral("playing"), frame.playing},
        {QStringLiteral("recording"), frame.recording},
    };
}

std::optional<TransportFrame> transportFrameFromJson(const QJsonObject& json,
                                                     QString* error) {
    TransportFrame frame;
    frame.participantId =
        json.value(QStringLiteral("participant_id")).toString();
    if (!isWireSafeSemanticId(frame.participantId)) {
        setError(error, QStringLiteral("invalid transport participant id"));
        return std::nullopt;
    }
    frame.positionSeconds =
        std::max(0.0, finiteNumber(json.value(QStringLiteral("position_seconds")), 0.0));
    frame.sentAtMs = integerValue(json.value(QStringLiteral("sent_at_ms")));
    frame.scheduledAtMs = integerValue(json.value(QStringLiteral("scheduled_at_ms")));
    frame.playing = json.value(QStringLiteral("playing")).toBool(false);
    frame.recording = json.value(QStringLiteral("recording")).toBool(false);
    return frame;
}

QJsonObject wireEnvelopeToJson(const WireEnvelope& envelope) {
    const QString resolvedType = !envelope.typeName.isEmpty()
        ? envelope.typeName
        : wireTypeName(envelope.type);
    QJsonObject json{
        {QStringLiteral("protocol"), QString::fromLatin1(kProtocolName)},
        {QStringLiteral("type"), resolvedType},
        {QStringLiteral("messageId"), safeSemanticId(envelope.messageId)},
        {QStringLiteral("payload"), envelope.payload},
    };
    if (envelope.sentAtMs > 0)
        json.insert(QStringLiteral("sentAtMs"), double(envelope.sentAtMs));
    if (envelope.serverTimeMs > 0)
        json.insert(QStringLiteral("serverTimeMs"), double(envelope.serverTimeMs));
    const QString participant = safeSemanticId(envelope.participantId);
    if (!participant.isEmpty())
        json.insert(QStringLiteral("participantId"), participant);
    if (isEphemeralType(envelope.type) || envelope.ephemeralSequence > 0)
        json.insert(QStringLiteral("ephemeralSeq"),
                    double(envelope.ephemeralSequence));
    return json;
}

std::optional<WireEnvelope> wireEnvelopeFromJson(const QJsonObject& json,
                                                 QString* error) {
    WireEnvelope envelope;
    envelope.protocol = json.value(QStringLiteral("protocol")).toString();
    if (envelope.protocol != QLatin1String(kProtocolName)) {
        setError(error, QStringLiteral("unsupported collaboration protocol version"));
        return std::nullopt;
    }
    envelope.typeName = json.value(QStringLiteral("type")).toString();
    if (!validWireTypeName(envelope.typeName)) {
        setError(error, QStringLiteral("unknown collaboration envelope type"));
        return std::nullopt;
    }
    envelope.type = wireTypeFromName(envelope.typeName).value_or(WireType::Unknown);
    if (envelope.type == WireType::SnapshotRequested &&
        !strictSnapshotRequestEnvelope(json)) {
        setError(error, QStringLiteral("invalid snapshot request envelope"));
        return std::nullopt;
    }
    envelope.messageId = json.value(QStringLiteral("messageId")).toString();
    envelope.participantId =
        json.value(QStringLiteral("participantId")).toString();
    if ((!envelope.messageId.isEmpty() && !isWireSafeSemanticId(envelope.messageId)) ||
        (!envelope.participantId.isEmpty() &&
         !isWireSafeSemanticId(envelope.participantId))) {
        setError(error, QStringLiteral("invalid collaboration envelope id"));
        return std::nullopt;
    }
    envelope.ephemeralSequence = quint64(std::max<qint64>(
        0, integerValue(json.value(QStringLiteral("ephemeralSeq")))));
    envelope.sentAtMs = integerValue(json.value(QStringLiteral("sentAtMs")));
    envelope.serverTimeMs =
        integerValue(json.value(QStringLiteral("serverTimeMs")));
    envelope.payload = json.value(QStringLiteral("payload")).toObject();
    return envelope;
}

bool checkCollaborationProtocolForTest(QString* error) {
    PresencePacket source;
    source.clientSequence = 42;
    source.sentAtMs = 123456;
    source.phase = PointerPhase::Press;
    source.policy = PresencePolicy::Exact;
    source.point.surface = {SurfaceKind::Timeline, QStringLiteral("arrangement"), {}};
    source.point.timeSeconds = 12.5;
    source.point.trackId = QStringLiteral("track-1");
    source.point.targetId = QStringLiteral("lane");
    source.point.normalized = QPointF(0.25, 0.75);
    source.selectionChange = true;
    source.selectionIds = {QStringLiteral("clip-1")};

    const auto decoded = presencePacketFromJson(presencePacketToJson(source), error);
    if (!decoded || decoded->clientSequence != source.clientSequence ||
        decoded->phase != source.phase || decoded->point.trackId != source.point.trackId ||
        std::abs(decoded->point.timeSeconds - source.point.timeSeconds) > 1e-9) {
        setError(error, QStringLiteral("presence payload did not round-trip"));
        return false;
    }

    const QString secret = QStringLiteral("/Users/person/Private/song.wav");
    const QByteArray encoded = QJsonDocument(semanticPointToJson(SemanticPoint{
        SurfaceAddress{SurfaceKind::Shell, QStringLiteral("shell"), {}},
        secret,
    })).toJson(QJsonDocument::Compact);
    if (encoded.contains(secret.toUtf8()) ||
        !encoded.contains("sha256:")) {
        setError(error, QStringLiteral("unsafe semantic id was not redacted"));
        return false;
    }

    WireEnvelope envelope;
    envelope.type = WireType::PresenceCursor;
    envelope.messageId = QStringLiteral("message-1");
    envelope.sentAtMs = 123456;
    envelope.ephemeralSequence = 42;
    envelope.payload = presencePacketToJson(source);
    const auto wire = wireEnvelopeFromJson(wireEnvelopeToJson(envelope), error);
    if (!wire || wire->type != WireType::PresenceCursor ||
        wire->protocol != QLatin1String(kProtocolName)) {
        setError(error, QStringLiteral("wire envelope did not round-trip"));
        return false;
    }
    return true;
}

bool checkCollaborationPresenceGeometryForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QPointF expected(0.3125, 0.6875);
    for (const QSize size : {QSize(320, 180), QSize(1024, 640),
                             QSize(2560, 1440)}) {
        const QPointF local(expected.x() * size.width(),
                            expected.y() * size.height());
        const QPointF normalized = normalizedSurfacePoint(local, size);
        if (QLineF(normalized, expected).length() > 1e-9)
            return fail(QStringLiteral("normalised presence changed with layout size"));
        SemanticPoint point;
        point.normalized = normalized;
        const auto restored = surfacePointFromNormalized(point, size);
        if (!restored || QLineF(*restored, local).length() > 1e-6)
            return fail(QStringLiteral("normalised presence did not round-trip"));
    }

    // QWidget event positions and sizes are logical pixels. Scaling both as a
    // HiDPI backing store would does not change the wire coordinate.
    const QPointF oneX = normalizedSurfacePoint(QPointF(240, 135), QSize(480, 270));
    const QPointF twoX = normalizedSurfacePoint(QPointF(480, 270), QSize(960, 540));
    if (QLineF(oneX, twoX).length() > 1e-9)
        return fail(QStringLiteral("presence mapping depended on device scale"));

    SemanticPoint hidden;
    hidden.normalized = QPointF(-1.0, -1.0);
    if (surfacePointFromNormalized(hidden, QSize(800, 600)))
        return fail(QStringLiteral("hidden presence produced a drawable point"));
    return true;
}

} // namespace collab
