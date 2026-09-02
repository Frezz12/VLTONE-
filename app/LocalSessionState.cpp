#include "LocalSessionState.hpp"

namespace collab {

LocalSessionState::LocalSessionState(QObject* parent) : QObject(parent) {}

void LocalSessionState::setHostParticipantId(const QString& participantId) {
    const QString safe = safeSemanticId(participantId);
    if (m_hostParticipantId == safe) return;
    m_hostParticipantId = safe;
    if (m_mode == TransportMode::FollowHost) {
        m_followedParticipantId = m_hostParticipantId;
        emit transportModeChanged(m_mode, m_followedParticipantId);
    }
}

void LocalSessionState::setTransportMode(TransportMode mode,
                                         const QString& participantId) {
    QString target;
    if (mode == TransportMode::FollowHost) target = m_hostParticipantId;
    else if (mode == TransportMode::FollowParticipant)
        target = safeSemanticId(participantId);

    // Following an unknown participant cannot have observable behaviour. Keep
    // the previous mode instead of displaying a checked action that does none.
    if (mode != TransportMode::Independent && target.isEmpty()) return;
    if (m_mode == mode && m_followedParticipantId == target) return;
    m_mode = mode;
    m_followedParticipantId = target;
    m_lastAcceptedSequence = 0;
    m_lastAcceptedAtMs = 0;
    emit transportModeChanged(m_mode, m_followedParticipantId);
}

void LocalSessionState::followHost() {
    setTransportMode(TransportMode::FollowHost);
}

void LocalSessionState::followParticipant(const QString& participantId) {
    setTransportMode(TransportMode::FollowParticipant, participantId);
}

void LocalSessionState::useIndependentTransport() {
    setTransportMode(TransportMode::Independent);
}

void LocalSessionState::noteLocalTransportInteraction() {
    if (m_mode != TransportMode::Independent) useIndependentTransport();
}

bool LocalSessionState::offerRemoteTransport(const TransportFrame& frame) {
    if (m_mode == TransportMode::Independent || frame.participantId.isEmpty())
        return false;
    const QString sender = safeSemanticId(frame.participantId);
    if (sender != m_followedParticipantId) return false;
    // sentAtMs is the ordering key until the wire format gains a dedicated
    // transport sequence. Equal timestamps are accepted once so play/pause and
    // position from one scheduled frame stay atomic.
    if (frame.sentAtMs > 0 && frame.sentAtMs < m_lastAcceptedAtMs) return false;
    m_lastAcceptedAtMs = std::max(m_lastAcceptedAtMs, frame.sentAtMs);
    emit remoteTransportAccepted(frame);
    return true;
}

} // namespace collab
