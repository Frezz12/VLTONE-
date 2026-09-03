#pragma once

#include "CollaborationTypes.hpp"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <array>
#include <optional>

namespace collab {

struct PresenceCursorSnapshot {
    ParticipantIdentity participant;
    PresencePacket packet;
    qint64 receivedAtMs = 0;
    qreal opacity = 1.0;
    bool interpolating = false;
};

/// UI-thread model for transient room presence. Participant membership and
/// cursors deliberately have different lifetimes: a quiet musician remains in
/// the participant list while their stale pointer disappears from the canvas.
class PresenceStore final : public QObject {
    Q_OBJECT
public:
    explicit PresenceStore(QObject* parent = nullptr);

    void setLocalParticipantId(const QString& participantId);
    QString localParticipantId() const { return m_localParticipantId; }

    void replaceParticipants(const QVector<ParticipantIdentity>& participants);
    void upsertParticipant(ParticipantIdentity participant);
    /// Announces a fresh connection for one participant. A reconnecting peer
    /// keeps its member id, so the roster entry survives while its ephemeral
    /// sequence counters restart at zero. Without this reset the dedupe below
    /// would silently discard every later packet from that peer.
    void noteParticipantConnected(ParticipantIdentity participant);
    void removeParticipant(const QString& participantId);
    void applyPresence(const PresenceUpdate& update);
    void clear();

    QVector<ParticipantIdentity> participants() const;
    /// O(1) roster lookup for the presence hot path. `participants()` copies
    /// and locale-sorts the whole roster and must not be called per packet.
    std::optional<ParticipantIdentity> participantById(
        const QString& participantId) const;
    int participantCount() const { return m_entries.size(); }
    QVector<PresenceCursorSnapshot> cursorsForSurface(
        const SurfaceAddress& surface, qint64 nowMs = 0) const;
    std::optional<SurfaceKind> recentSurfaceForParticipant(
        const QString& participantId, qint64 nowMs = 0) const;

    static QColor stableParticipantColor(const QString& stableId);

    /// Headless regression for the presence delivery contract: per-channel
    /// dedupe, reconnect recovery and jitter-independent interpolation.
    static bool checkDeliveryForTest(QString* error = nullptr);

    static constexpr qint64 kCursorFullyVisibleMs = 1500;
    static constexpr qint64 kCursorTtlMs = 5000;

signals:
    /// Something changed that every surface must re-read: the roster, the local
    /// identity, or a wholesale clear.
    void presenceChanged();
    /// One surface's cursors changed. Overlays subscribe to their own surface
    /// so a peer moving over the timeline no longer repaints the mixer, the
    /// track list and the transport as well.
    void surfacePresenceChanged(SurfaceKind surface);
    void participantsChanged();

private:
    struct Entry {
        ParticipantIdentity identity;
        PresencePacket packet;
        PresencePacket previousPacket;
        /// One high-water mark per wire kind. The room bus coalesces and drains
        /// the kinds independently, so a shared counter would let a selection or
        /// drag retire an undelivered cursor sample.
        std::array<quint64, kPresenceChannelCount> lastSequence{};
        qint64 receivedAtMs = 0;
        qint64 previousReceivedAtMs = 0;
        bool hasCursor = false;
        bool hasPreviousCursor = false;
    };

    static QString participantKey(const ParticipantIdentity& participant);
    static void resetEphemeralState(Entry& entry);
    void expireStaleCursors();

    QHash<QString, Entry> m_entries;
    QString m_localParticipantId;
    QTimer m_expiryTimer;
};

} // namespace collab

Q_DECLARE_METATYPE(collab::PresenceCursorSnapshot)
