#include "PresenceStore.hpp"

#include <QDateTime>
#include <QSet>

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

bool hasNormalized(const SemanticPoint& point) {
    return std::isfinite(point.normalized.x()) &&
           std::isfinite(point.normalized.y()) &&
           point.normalized.x() >= 0.0 && point.normalized.y() >= 0.0;
}

bool hasPosition(const SemanticPoint& point) {
    return hasNormalized(point) ||
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

void PresenceStore::resetEphemeralState(Entry& entry) {
    entry.lastSequence.fill(0);
    entry.packet = PresencePacket();
    entry.previousPacket = PresencePacket();
    entry.receivedAtMs = 0;
    entry.previousReceivedAtMs = 0;
    entry.hasCursor = false;
    entry.hasPreviousCursor = false;
}

void PresenceStore::noteParticipantConnected(ParticipantIdentity participant) {
    const QString key = participantKey(participant);
    if (key.isEmpty()) return;
    upsertParticipant(std::move(participant));
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    resetEphemeralState(*it);
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
    const auto channel = std::size_t(update.packet.channel);
    if (channel >= it->lastSequence.size()) return;
    quint64& lastSequence = it->lastSequence[channel];
    if (update.packet.clientSequence > 0 && lastSequence > 0 &&
        update.packet.clientSequence <= lastSequence) {
        // A peer that reconnects keeps its member id but restarts its counters.
        // The bus holds at most one coalesced packet per key, so a sequence this
        // far behind cannot be reordering — it is a fresh connection. Recovering
        // here keeps presence alive even when presence.join was missed.
        constexpr quint64 kSequenceRestartMargin = 64;
        if (lastSequence - update.packet.clientSequence < kSequenceRestartMargin)
            return;
        resetEphemeralState(*it);
    }
    if (update.packet.clientSequence > 0)
        lastSequence = update.packet.clientSequence;
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
    const SurfaceKind previousSurface = it->packet.point.surface.kind;
    it->packet = std::move(merged);
    if (cursorUpdate) {
        it->receivedAtMs = receivedNow;
        it->hasCursor = true;
    } else if (!update.packet.selectionChange) {
        it->receivedAtMs = receivedNow;
        it->hasCursor = false;
        it->hasPreviousCursor = false;
    }
    const SurfaceKind currentSurface = it->packet.point.surface.kind;
    emit surfacePresenceChanged(currentSurface);
    // A pointer that crossed between surfaces must also erase itself from the
    // one it left.
    if (previousSurface != currentSurface)
        emit surfacePresenceChanged(previousSurface);
}

void PresenceStore::clear() {
    if (m_entries.isEmpty() && m_localParticipantId.isEmpty()) return;
    m_entries.clear();
    m_localParticipantId.clear();
    emit participantsChanged();
    emit presenceChanged();
}

std::optional<ParticipantIdentity> PresenceStore::participantById(
    const QString& participantId) const {
    const auto it = m_entries.constFind(safeSemanticId(participantId));
    if (it == m_entries.cend()) return std::nullopt;
    return it->identity;
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
            interpolationContextMatches(entry.previousPacket.point,
                                        entry.packet.point)) {
            // Walk from the previous sample to the newest one over the span the
            // sender actually used, so the render stays exactly one packet
            // behind and network jitter never becomes visible speed. The span
            // is measured on the server clock stamped into both packets;
            // arrival times are only the local start of the walk, because two
            // coalesced packets can share one arrival instant.
            constexpr qint64 kMinimumSpanMs = 8;
            constexpr qint64 kMaximumSpanMs = 250;
            const qint64 sentSpan =
                entry.packet.sentAtMs > 0 && entry.previousPacket.sentAtMs > 0
                    ? entry.packet.sentAtMs - entry.previousPacket.sentAtMs
                    : 0;
            const qint64 arrivalSpan =
                entry.receivedAtMs - entry.previousReceivedAtMs;
            const qint64 spanMs = std::clamp(
                sentSpan > 0 ? sentSpan : arrivalSpan, kMinimumSpanMs,
                kMaximumSpanMs);
            const qint64 elapsed =
                std::max<qint64>(0, nowMs - entry.receivedAtMs);
            const qreal progress =
                std::clamp(qreal(elapsed) / qreal(spanMs), qreal(0.0),
                           qreal(1.0));
            SemanticPoint& point = snapshot.packet.point;
            const SemanticPoint& previous = entry.previousPacket.point;
            if (hasNormalized(previous) && hasNormalized(point)) {
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

bool PresenceStore::checkDeliveryForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString peerId = QStringLiteral("peer-1");
    ParticipantIdentity peer;
    peer.participantId = peerId;
    peer.nickname = QStringLiteral("Peer");

    SurfaceAddress surface{SurfaceKind::Timeline, QStringLiteral("arrangement"),
                           {}};
    const auto cursorAt = [&](PresenceChannel channel, quint64 sequence,
                              qint64 sentAtMs, qreal x) {
        PresenceUpdate update;
        update.participant = peer;
        update.packet.channel = channel;
        update.packet.clientSequence = sequence;
        update.packet.sentAtMs = sentAtMs;
        update.packet.policy = PresencePolicy::Exact;
        update.packet.point.surface = surface;
        update.packet.point.normalized = QPointF(x, 0.5);
        return update;
    };

    // Read the cursor once its interpolation has settled on the newest sample.
    // Sampling at "now" would return a point part-way through the walk from the
    // previous packet, which says nothing about which packet actually won.
    const auto settledX = [&](PresenceStore& target) {
        const auto cursors = target.cursorsForSurface(
            surface, QDateTime::currentMSecsSinceEpoch() + 300);
        return cursors.isEmpty()
                   ? qreal(-1.0)
                   : cursors.front().packet.point.normalized.x();
    };

    PresenceStore store;
    store.setLocalParticipantId(QStringLiteral("me"));
    store.applyPresence(cursorAt(PresenceChannel::Cursor, 5, 1000, 0.5));
    if (store.cursorsForSurface(surface).size() != 1)
        return fail(QStringLiteral("first cursor packet was not stored"));

    // A stale cursor sample must lose to the newer one on its own channel.
    store.applyPresence(cursorAt(PresenceChannel::Cursor, 4, 900, 0.9));
    if (!qFuzzyCompare(settledX(store), qreal(0.5)))
        return fail(QStringLiteral("reordered cursor packet was applied"));

    // A different channel carries its own counter. Before the split, a drag or
    // selection numbered below the cursor high-water mark was silently dropped.
    PresenceUpdate drag = cursorAt(PresenceChannel::Drag, 2, 1100, 0.7);
    drag.packet.drag.active = true;
    drag.packet.drag.kind = QStringLiteral("clip");
    store.applyPresence(drag);
    if (!store.cursorsForSurface(surface).front().packet.drag.active)
        return fail(QStringLiteral("drag lost to the cursor sequence"));

    // Raise the high-water mark, then prove both halves of the restart rule.
    // A small backwards step is ordinary reordering and must stay rejected --
    // that guard is what keeps a stale sample from overwriting a fresh one.
    store.applyPresence(cursorAt(PresenceChannel::Cursor, 200, 2000, 0.4));
    store.applyPresence(cursorAt(PresenceChannel::Cursor, 190, 1900, 0.95));
    if (!qFuzzyCompare(settledX(store), qreal(0.4)))
        return fail(QStringLiteral("a reordered sample beat the newest one"));
    // A peer that restarts keeps its member id but restarts its counters, so a
    // sequence this far behind can only be a fresh connection.
    store.applyPresence(cursorAt(PresenceChannel::Cursor, 1, 5000, 0.25));
    if (!qFuzzyCompare(settledX(store), qreal(0.25)))
        return fail(QStringLiteral("reconnected peer stayed deduped forever"));

    // An explicit join is the deterministic path for the same recovery.
    PresenceStore rejoined;
    rejoined.setLocalParticipantId(QStringLiteral("me"));
    rejoined.applyPresence(cursorAt(PresenceChannel::Cursor, 900, 1000, 0.5));
    rejoined.noteParticipantConnected(peer);
    if (!rejoined.cursorsForSurface(surface).isEmpty())
        return fail(QStringLiteral("join did not clear the stale cursor"));
    rejoined.applyPresence(cursorAt(PresenceChannel::Cursor, 1, 2000, 0.75));
    if (rejoined.cursorsForSurface(surface).size() != 1)
        return fail(QStringLiteral("join did not reset the sequence gate"));

    // Two samples that share one arrival instant still interpolate across the
    // span their sender used, instead of snapping to the newest point.
    PresenceStore paced;
    paced.setLocalParticipantId(QStringLiteral("me"));
    paced.applyPresence(cursorAt(PresenceChannel::Cursor, 1, 10'000, 0.0));
    paced.applyPresence(cursorAt(PresenceChannel::Cursor, 2, 10'100, 1.0));
    const auto snapshots = paced.cursorsForSurface(surface);
    if (snapshots.size() != 1 || !snapshots.front().interpolating)
        return fail(QStringLiteral("coalesced arrivals disabled interpolation"));
    if (snapshots.front().packet.point.normalized.x() > 0.5) {
        return fail(QStringLiteral(
            "interpolation snapped past the sender's own span"));
    }
    return true;
}

void PresenceStore::expireStaleCursors() {
    const qint64 now = wallClockMs();
    // Keep repainting during the fade, but sleep when there is no cursor — and
    // wake only the surfaces that actually have an expiring or fading pointer.
    QSet<SurfaceKind> affected;
    for (Entry& entry : m_entries) {
        if (!entry.hasCursor) continue;
        const qint64 age = now - entry.receivedAtMs;
        if (age >= kCursorTtlMs) {
            entry.hasCursor = false;
            affected.insert(entry.packet.point.surface.kind);
        } else if (age >= kCursorFullyVisibleMs) {
            affected.insert(entry.packet.point.surface.kind);
        }
    }
    for (const SurfaceKind surface : affected)
        emit surfacePresenceChanged(surface);
}

} // namespace collab
