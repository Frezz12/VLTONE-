#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <QUrl>

namespace account { class Service; }

#ifdef DAW_ENABLE_COLLABORATION
QT_BEGIN_NAMESPACE
class QWebSocket;
QT_END_NAMESPACE
#endif

namespace collab {

class CollaborationService;

#ifdef DAW_ENABLE_COLLABORATION
/// Authenticated, bounded WebSocket adapter for CollaborationService.
///
/// The service owns protocol and UI state; this class owns only the socket
/// lifecycle. Credentials are read from AccountService immediately before a
/// handshake and are never placed in a URL, signal, diagnostic or queue.
class CollaborationTransport final : public QObject {
public:
    CollaborationTransport(account::Service* account,
                           CollaborationService* service,
                           QObject* parent = nullptr);
    ~CollaborationTransport() override;

private:
    struct PendingMessage {
        QString text;
        qsizetype utf8Bytes = 0;
    };

    void authorizationRequested(const QString& projectId,
                                const QUrl& endpoint);
    void accountStateChanged();
    void collaborationStateChanged();
    void enqueueTextMessage(const QString& message);
    void startConnection();
    void socketConnected(QWebSocket* socket, quint64 generation);
    void socketTextMessage(QWebSocket* socket, quint64 generation,
                           const QString& message);
    void socketPong(QWebSocket* socket, quint64 generation);
    void socketFailed(QWebSocket* socket, quint64 generation,
                      const QString& safeReason);
    void drainOutbound();
    void healthCheck();
    void scheduleReconnect(bool immediate = false);
    void stopTransport(bool clearTarget);
    void retireSocket();
    void permanentFailure(const QString& safeReason);
    bool isCurrent(const QWebSocket* socket, quint64 generation) const;
    bool accountCanConnect() const;

    account::Service* m_account = nullptr;
    CollaborationService* m_service = nullptr;
    QWebSocket* m_socket = nullptr;
    QTimer m_reconnectTimer;
    QTimer m_handshakeTimer;
    QTimer m_healthTimer;
    QElapsedTimer m_lastReceive;
    QElapsedTimer m_lastPing;
    QElapsedTimer m_lastWriteProgress;
    QElapsedTimer m_connectedFor;
    QQueue<PendingMessage> m_outbound;
    QByteArray m_tokenFingerprint;
    QString m_projectId;
    QUrl m_endpoint;
    qint64 m_outboundBytes = 0;
    quint64 m_generation = 0;
    int m_retryAttempt = 0;
    bool m_desired = false;
    bool m_ready = false;
    bool m_waitingForPong = false;
    bool m_backoffResetForConnection = false;
};
#endif

/// Deterministic, non-network checks for endpoint confinement, retry bounds,
/// queue limits and (when enabled) the Authorization/subprotocol handshake.
bool checkCollaborationTransportForTest(QString* error = nullptr);

} // namespace collab
