#pragma once

#include <QColor>
#include <QJsonObject>
#include <QMetaType>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace collab {

inline constexpr int kProtocolVersion = 1;
inline constexpr auto kProtocolName = "vlt-collab-v1";

enum class CollaborationState {
    LocalOnly,
    Unavailable,
    SignedOut,
    NoConnection,
    Uploading,
    Connecting,
    Joining,
    Synced,
    Reconnecting,
    ReadOnly,
    Conflict,
    Error,
};

enum class PresencePolicy { Hidden, Coarse, Exact };

enum class SurfaceKind {
    Unknown,
    Shell,
    Transport,
    Timeline,
    TrackList,
    Mixer,
    PianoRoll,
    AutomationEditor,
    SampleEditor,
    BuiltinPlugin,
    GenericPlugin,
    ThirdPartyPlugin,
    Browser,
    Web,
    Ai,
    Settings,
    Dialog,
};

enum class PointerPhase { Move, Press, Release, Leave };
enum class TransportMode { Independent, FollowHost, FollowParticipant };

/// A surface is identified semantically, never by QWidget address, native
/// handle, screen position, window title, URL or filesystem path.
struct SurfaceAddress {
    SurfaceKind kind = SurfaceKind::Unknown;
    QString instanceId;
    QString contextId;

    bool operator==(const SurfaceAddress&) const = default;
};

/// The common coordinate vocabulary. A surface fills only the fields it owns:
/// Timeline uses time/track/lane, Piano Roll uses beat/pitch, and ordinary safe
/// Qt controls use the normalised point plus an explicit semantic target id.
struct SemanticPoint {
    SurfaceAddress surface;
    QString targetId;
    QPointF normalized{-1.0, -1.0};
    double timeSeconds = -1.0;
    double beat = -1.0;
    int pitch = -1;
    double laneFraction = -1.0;
    QString trackId;
    QString clipId;
    QString parameterId;
};

struct DragPreview {
    bool active = false;
    QString kind;
    QStringList objectIds;
    SemanticPoint destination;
};

struct PresencePacket {
    quint64 clientSequence = 0;
    qint64 sentAtMs = 0;
    PointerPhase phase = PointerPhase::Move;
    PresencePolicy policy = PresencePolicy::Hidden;
    SemanticPoint point;
    bool selectionChange = false;
    QStringList selectionIds;
    DragPreview drag;
};

struct ParticipantIdentity {
    QString participantId;
    QString userId;
    QString nickname;
    QColor color;
    QString role;
    bool host = false;
    bool online = true;
};

struct PresenceUpdate {
    ParticipantIdentity participant;
    PresencePacket packet;
};

struct TransportFrame {
    QString participantId;
    double positionSeconds = 0.0;
    qint64 sentAtMs = 0;
    qint64 scheduledAtMs = 0;
    bool playing = false;
    bool recording = false;
};

enum class SnapshotRequestReason : quint8 { Autosave, SessionEnd };

/// Server-assigned request to persist one exact confirmed project generation.
/// It deliberately contains no document bytes, paths, provider identifiers or
/// user-controlled participant identity.
struct SnapshotRequest {
    QString requestId;
    QString sessionId;
    QString hostParticipantId;
    quint64 targetServerSequence = 0;
    SnapshotRequestReason reason = SnapshotRequestReason::Autosave;
    int attempt = 0;
    qint64 retryAtMs = 0;
};

enum class WireType {
    Unknown,
    Hello,
    Welcome,
    OpSubmit,
    OpCommitted,
    OpRejected,
    PresenceJoined,
    PresenceLeft,
    PresenceCursor,
    PresenceClick,
    PresenceSelection,
    PresenceDrag,
    TransportState,
    TransportFollow,
    LeaseAcquire,
    LeaseGranted,
    LeaseDenied,
    LeaseRenew,
    LeaseRelease,
    SessionHandoff,
    SessionHostChanged,
    SessionEnding,
    SnapshotRequested,
    SessionEnded,
    ResyncRequired,
    SnapshotHash,
};

/// Every WebSocket message uses this versioned outer envelope. Payload schemas
/// are type-specific; presence payloads are additionally whitelist-serialised
/// below so arbitrary widget data can never hitch a ride.
struct WireEnvelope {
    QString protocol = QString::fromLatin1(kProtocolName);
    WireType type = WireType::Unknown;
    /// Preserved when a newer server sends a syntactically valid type this
    /// build does not know yet. Known outbound messages should use `type`.
    QString typeName;
    QString messageId;
    QString participantId;
    quint64 ephemeralSequence = 0;
    qint64 sentAtMs = 0;
    qint64 serverTimeMs = 0;
    QJsonObject payload;
};

QString collaborationStateName(CollaborationState state);
QString surfaceKindName(SurfaceKind kind);
std::optional<SurfaceKind> surfaceKindFromName(const QString& name);
QString pointerPhaseName(PointerPhase phase);
std::optional<PointerPhase> pointerPhaseFromName(const QString& name);
QString wireTypeName(WireType type);
std::optional<WireType> wireTypeFromName(const QString& name);

/// Stable ids are allowed through unchanged only when they cannot resemble a
/// path or URL. Everything else becomes a deterministic hash, retaining
/// cross-client identity without exposing its source string.
QString safeSemanticId(const QString& value);
bool isWireSafeSemanticId(const QString& value);
QString safeDisplayName(const QString& value);

/// Convert between widget-local logical coordinates and the normalised wire
/// coordinates used by safe surfaces. No screen/global position or device
/// pixel ratio enters this mapping, so it stays stable across window sizes and
/// HiDPI scale factors.
QPointF normalizedSurfacePoint(const QPointF& localPosition,
                               const QSize& logicalSize);
std::optional<QPointF> surfacePointFromNormalized(
    const SemanticPoint& point, const QSize& logicalSize);

QJsonObject semanticPointToJson(const SemanticPoint& point);
std::optional<SemanticPoint> semanticPointFromJson(const QJsonObject& json,
                                                   QString* error = nullptr);
QJsonObject presencePacketToJson(const PresencePacket& packet);
std::optional<PresencePacket> presencePacketFromJson(const QJsonObject& json,
                                                     QString* error = nullptr);
QJsonObject transportFrameToJson(const TransportFrame& frame);
std::optional<TransportFrame> transportFrameFromJson(const QJsonObject& json,
                                                     QString* error = nullptr);
QJsonObject wireEnvelopeToJson(const WireEnvelope& envelope);
std::optional<WireEnvelope> wireEnvelopeFromJson(const QJsonObject& json,
                                                 QString* error = nullptr);

bool checkCollaborationProtocolForTest(QString* error = nullptr);
bool checkCollaborationPresenceGeometryForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CollaborationState)
Q_DECLARE_METATYPE(collab::PresencePacket)
Q_DECLARE_METATYPE(collab::PresenceUpdate)
Q_DECLARE_METATYPE(collab::TransportFrame)
Q_DECLARE_METATYPE(collab::TransportMode)
Q_DECLARE_METATYPE(collab::SnapshotRequest)
Q_DECLARE_METATYPE(collab::WireEnvelope)
