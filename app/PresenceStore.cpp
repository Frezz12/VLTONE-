#include "PresenceStore.hpp"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace collab {
namespace {

qint64 wallClockMs() { return QDateTime::currentMSecsSinceEpoch(); }

bool sameSurface(const SurfaceAddress& requested,
                 const SurfaceAddress& actual) {
    if (requested.kind != actual.kind) return false;
    if (!requested.instanceId.isEmpty() && !actual.instanceId.isEmpty() &&
        requested.instanceId != actual.instanceId) {
        return false;
    }
    if (!requested.contextId.isEmpty() && !actual.contextId.isEmpty() &&
        requested.contextId != actual.contextId) {
        return false;
    }
    return true;
}

bool interpolationContextMatches(const SemanticPoint& from,
                                 const SemanticPoint& to) {
    return from.surface == to.surface && from.trackId == to.trackId &&
           from.clipId == to.clipId;
}

bool hasPosition(const SemanticPoint& point) {
    return (std::isfinite(point.normalized.x()) && point.normalized.x() >= 0.0 &&
            std::isfinite(point.normalized.y()) && point.normalized.y() >= 0.0) ||
           (std::isfinite(point.timeSeconds) && point.timeSeconds >= 0.0);
}

void interpolateScalar(double& output, double from, double to, qreal progress) {
    if (std::isfinite(from) && std::isfinite(to) && from >= 0.0 && to >= 0.0)
        output = from + (to - from) * progress;
}

} // namespace

PresenceStore::PresenceStore(QObject* parent) : QObject(parent) {
    m_expiryTimer.setInterval(250);
    m_expiryTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_expiryTimer, &QTimer::timeout, this,
            &PresenceStore::expireStaleCursors);
    m_expiryTimer.start();
}

QString PresenceStore::participantKey(const ParticipantIdentity& participant) {
    const QString participantId = safeSemanticId(participant.participantId);
    return !participantId.isEmpty() ? participantId
                                    : safeSemanticId(participant.userId);
}

void PresenceStore::setLocalParticipantId(const QString& participantId) {
    m_localParticipantId = safeSemanticId(participantId);
}

void PresenceStore::replaceParticipants(
    const QVector<ParticipantIdentity>& participants) {
    QHash<QString, Entry> replacement;
    for (ParticipantIdentity participant : participants) {
        const QString key = participantKey(participant);
        if (key.isEmpty()) continue;
        participant.participantId = safeSemanticId(participant.participantId);
        participant.userId = safeSemanticId(participant.userId);
        participant.nickname = safeDisplayName(participant.nickname);
        if (!participant.color.isValid())
            participant.color = stableParticipantColor(key);
        auto previous = m_entries.constFind(key);
        Entry entry;
        if (previous != m_entries.cend()) entry = previous.value();
        entry.identity = participant;
        replacement.insert(key, entry);
    }
    m_entries = std::move(replacement);
    emit participantsChanged();
    emit presenceChanged();
}

void PresenceStore::upsertParticipant(ParticipantIdentity participant) {
    const QString key = participantKey(participant);
    if (key.isEmpty()) return;
    participant.participantId = safeSemanticId(participant.participantId);
    participant.userId = safeSemanticId(participant.userId);
    participant.nickname = safeDisplayName(participant.nickname);
    if (!participant.color.isValid())
        participant.color = stableParticipantColor(key);
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->identity.participantId == participant.participantId &&
        it->identity.nickname == participant.nickname &&
        it->identity.color == participant.color &&
        it->identity.role == participant.role &&
        it->identity.host == participant.host &&
        it->identity.online == participant.online) {
        return;
    }
    if (it == m_entries.end()) {
        Entry entry;
        entry.identity = participant;
        m_entries.insert(key, entry);
    } else {
        it->identity = participant;
    }
    emit participantsChanged();
    emit presenceChanged();
}

void PresenceStore::removeParticipant(const QString& participantId) {
    const QString key = safeSemanticId(participantId);
    if (key.isEmpty() || m_entries.remove(key) == 0) return;
    emit participantsChanged();
    emit presenceChanged();
}

void PresenceStore::applyPresence(const PresenceUpdate& update) {
    const QString key = participantKey(update.participant);
    if (key.isEmpty() || key == m_localParticipantId) return;

    upsertParticipant(update.participant);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    if (update.packet.clientSequence > 0 && it->lastSequence > 0 &&
        update.packet.clientSequence <= it->lastSequence) {
        return;
    }
    if (update.packet.clientSequence > 0)
        it->lastSequence = update.packet.clientSequence;
    const qint64 receivedNow = wallClockMs();
    const bool cursorUpdate = !update.packet.selectionChange &&
        update.packet.phase != PointerPhase::Leave &&
        update.packet.policy != PresencePolicy::Hidden &&
        hasPosition(update.packet.point);
    if (cursorUpdate) {
        if (it->hasCursor && interpolationContextMatches(
                                 it->packet.point, update.packet.point)) {
            it->previousPacket = it->packet;
            it->previousReceivedAtMs = it->receivedAtMs;
            it->hasPreviousCursor = true;
        } else {
            it->hasPreviousCursor = false;
            it->previousReceivedAtMs = 0;
        }
    }

    PresencePacket merged = update.packet;
    if (merged.selectionChange && it->hasCursor) {
        merged.point = it->packet.point;
        merged.phase = it->packet.phase;
        merged.drag = it->packet.drag;
    } else if (!merged.selectionChange && it->packet.selectionChange) {
        merged.selectionChange = true;
        merged.selectionIds = it->packet.selectionIds;
    }
    it->packet = std::move(merged);
    if (cursorUpdate) {
        it->receivedAtMs = receivedNow;
        it->hasCursor = true;
    } else if (!update.packet.selectionChange) {
        it->receivedAtMs = receivedNow;
        it->hasCursor = false;
        it->hasPreviousCursor = false;
    }
    emit presenceChanged();
}

void PresenceStore::clear() {
    if (m_entries.isEmpty() && m_localParticipantId.isEmpty()) return;
    m_entries.clear();
    m_localParticipantId.clear();
    emit participantsChanged();
    emit presenceChanged();
}

QVector<ParticipantIdentity> PresenceStore::participants() const {
    QVector<ParticipantIdentity> result;
    result.reserve(m_entries.size());
    for (const Entry& entry : m_entries) result.push_back(entry.identity);
    std::sort(result.begin(), result.end(),
              [](const ParticipantIdentity& a, const ParticipantIdentity& b) {
                  if (a.host != b.host) return a.host;
                  return a.nickname.localeAwareCompare(b.nickname) < 0;
              });
    return result;
}

QVector<PresenceCursorSnapshot> PresenceStore::cursorsForSurface(
    const SurfaceAddress& surface, qint64 nowMs) const {
    if (nowMs <= 0) nowMs = wallClockMs();
    QVector<PresenceCursorSnapshot> result;
    for (const Entry& entry : m_entries) {
        if (!entry.hasCursor || !sameSurface(surface, entry.packet.point.surface))
            continue;
        const qint64 age = std::max<qint64>(0, nowMs - entry.receivedAtMs);
        if (age >= kCursorTtlMs) continue;
        qreal opacity = 1.0;
        if (age > kCursorFullyVisibleMs) {
            opacity = qreal(kCursorTtlMs - age) /
                      qreal(kCursorTtlMs - kCursorFullyVisibleMs);
        }
        PresenceCursorSnapshot snapshot{
            entry.identity, entry.packet, entry.receivedAtMs, opacity, false};
        if (entry.hasPreviousCursor &&
            entry.receivedAtMs > entry.previousReceivedAtMs &&
            interpolationContextMatches(entry.previousPacket.point,
                                        entry.packet.point)) {
            // Render just behind the newest packet. This turns 20 Hz network
            // samples into smooth local motion without guessing beyond data.
            constexpr qint64 kRenderDelayMs = 65;
            const qint64 targetMs = nowMs - kRenderDelayMs;
            const qreal progress = std::clamp(
                qreal(targetMs - entry.previousReceivedAtMs) /
                    qreal(entry.receivedAtMs - entry.previousReceivedAtMs),
                qreal(0.0), qreal(1.0));
            SemanticPoint& point = snapshot.packet.point;
            const SemanticPoint& previous = entry.previousPacket.point;
            if (std::isfinite(previous.normalized.x()) &&
                std::isfinite(previous.normalized.y()) &&
                previous.normalized.x() >= 0.0 &&
                previous.normalized.y() >= 0.0 &&
                point.normalized.x() >= 0.0 && point.normalized.y() >= 0.0) {
                point.normalized = previous.normalized +
                    (point.normalized - previous.normalized) * progress;
            }
            interpolateScalar(point.timeSeconds, previous.timeSeconds,
                              point.timeSeconds, progress);
            interpolateScalar(point.beat, previous.beat, point.beat, progress);
            interpolateScalar(point.laneFraction, previous.laneFraction,
                              point.laneFraction, progress);
            snapshot.interpolating = progress < 1.0;
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}

std::optional<SurfaceKind> PresenceStore::recentSurfaceForParticipant(
    const QString& participantId, qint64 nowMs) const {
    const QString key = safeSemanticId(participantId);
    const auto it = m_entries.constFind(key);
    if (it == m_entries.cend() || it->packet.policy == PresencePolicy::Hidden ||
        it->packet.point.surface.kind == SurfaceKind::Unknown)
        return std::nullopt;
    if (nowMs <= 0) nowMs = wallClockMs();
    if (nowMs - it->receivedAtMs >= kCursorTtlMs) return std::nullopt;
    return it->packet.point.surface.kind;
}

QColor PresenceStore::stableParticipantColor(const QString& stableId) {
    // Fixed, colour-blind-friendlier hues. Nicknames are always drawn as well,
    // so colour is never the sole identity channel.
    static const QColor palette[] = {
        QColor(QStringLiteral("#2F80ED")), QColor(QStringLiteral("#9B51E0")),
        QColor(QStringLiteral("#00A896")), QColor(QStringLiteral("#F2994A")),
        QColor(QStringLiteral("#EB5757")), QColor(QStringLiteral("#56CCF2")),
        QColor(QStringLiteral("#B7791F")), QColor(QStringLiteral("#6B7FD7")),
    };
    uint hash = 2166136261u;
    for (const QChar ch : stableId) {
        hash ^= ch.unicode();
        hash *= 16777619u;
    }
    return palette[hash % std::size(palette)];
}

void PresenceStore::expireStaleCursors() {
    const qint64 now = wallClockMs();
    bool changed = false;
    for (Entry& entry : m_entries) {
        if (entry.hasCursor && now - entry.receivedAtMs >= kCursorTtlMs) {
            entry.hasCursor = false;
            changed = true;
        }
    }
    // Keep repainting during the fade, but sleep when there is no cursor.
    bool fading = false;
    for (const Entry& entry : m_entries) {
        if (entry.hasCursor && now - entry.receivedAtMs >= kCursorFullyVisibleMs) {
            fading = true;
            break;
        }
    }
    if (changed || fading) emit presenceChanged();
}

} // namespace collab
