#pragma once

#include "CollaborationTypes.hpp"

#include <QObject>
#include <QString>

namespace collab {

/// Per-device collaboration choices. None of this belongs in ProjectModel: two
/// musicians may play, loop, solo and record independently while editing the
/// same durable document.
class LocalSessionState final : public QObject {
    Q_OBJECT
public:
    explicit LocalSessionState(QObject* parent = nullptr);

    TransportMode transportMode() const { return m_mode; }
    QString hostParticipantId() const { return m_hostParticipantId; }
    QString followedParticipantId() const { return m_followedParticipantId; }

    void setHostParticipantId(const QString& participantId);
    void setTransportMode(TransportMode mode,
                          const QString& participantId = {});
    void followHost();
    void followParticipant(const QString& participantId);
    void useIndependentTransport();

    /// A local play/seek/stop action always hands transport control back to this
    /// device. It never changes another participant's mode.
    void noteLocalTransportInteraction();

    /// Accept only the transient frame matching the local follow choice. Record
    /// state remains informative: callers must never arm local inputs from it.
    bool offerRemoteTransport(const TransportFrame& frame);

signals:
    void transportModeChanged(collab::TransportMode mode,
                              const QString& participantId);
    void remoteTransportAccepted(const collab::TransportFrame& frame);

private:
    TransportMode m_mode = TransportMode::Independent;
    QString m_hostParticipantId;
    QString m_followedParticipantId;
    quint64 m_lastAcceptedSequence = 0;
    qint64 m_lastAcceptedAtMs = 0;
};

} // namespace collab
