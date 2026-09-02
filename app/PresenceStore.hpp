#pragma once

#include "CollaborationTypes.hpp"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>

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
    void removeParticipant(const QString& participantId);
    void applyPresence(const PresenceUpdate& update);
    void clear();

    QVector<ParticipantIdentity> participants() const;
    int participantCount() const { return m_entries.size(); }
    QVector<PresenceCursorSnapshot> cursorsForSurface(
        const SurfaceAddress& surface, qint64 nowMs = 0) const;
    std::optional<SurfaceKind> recentSurfaceForParticipant(
        const QString& participantId, qint64 nowMs = 0) const;

    static QColor stableParticipantColor(const QString& stableId);

    static constexpr qint64 kCursorFullyVisibleMs = 1500;
    static constexpr qint64 kCursorTtlMs = 5000;

signals:
    void presenceChanged();
    void participantsChanged();

private:
    struct Entry {
        ParticipantIdentity identity;
        PresencePacket packet;
        PresencePacket previousPacket;
        quint64 lastSequence = 0;
        qint64 receivedAtMs = 0;
        qint64 previousReceivedAtMs = 0;
        bool hasCursor = false;
        bool hasPreviousCursor = false;
    };

    static QString participantKey(const ParticipantIdentity& participant);
    void expireStaleCursors();

    QHash<QString, Entry> m_entries;
    QString m_localParticipantId;
    QTimer m_expiryTimer;
};

} // namespace collab

Q_DECLARE_METATYPE(collab::PresenceCursorSnapshot)
