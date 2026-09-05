#include "CollaborationTransport.hpp"

#include "CollaborationTypes.hpp"

#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>

#ifdef DAW_ENABLE_COLLABORATION
#include "AccountService.hpp"
#include "CollaborationService.hpp"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketHandshakeOptions>
#include <QtWebSockets/QWebSocketProtocol>
#endif

namespace collab {
namespace {

constexpr qint64 kMaxMessageBytes = 1024 * 1024;
constexpr qint64 kMaxQueuedBytes = 4 * 1024 * 1024;
constexpr qsizetype kMaxQueuedMessages = 256;
constexpr qint64 kMaxSocketBufferedBytes = 2 * 1024 * 1024;
constexpr int kHandshakeTimeoutMs = 15'000;
constexpr int kPingIntervalMs = 15'000;
constexpr int kPongTimeoutMs = 10'000;
constexpr int kIdleTimeoutMs = 45'000;
constexpr int kWriteTimeoutMs = 15'000;
constexpr int kStableConnectionMs = 10'000;
constexpr int kReconnectBaseMs = 500;
constexpr int kReconnectCapMs = 30'000;

#ifdef DAW_ENABLE_COLLABORATION
bool isPermanentCloseCode(QWebSocketProtocol::CloseCode code) {
    return code == QWebSocketProtocol::CloseCodeProtocolError ||
           code == QWebSocketProtocol::CloseCodePolicyViolated;
}
#endif

QString canonicalProjectId(const QString& value) {
    static const QRegularExpression uuidPattern(
        QStringLiteral("^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
                       "[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$"));
    if (!uuidPattern.match(value).hasMatch()) return {};
    const QUuid uuid(value);
    if (uuid.isNull()) return {};
    return uuid.toString(QUuid::WithoutBraces).toLower();
}

QUrl endpointForProject(const QString& apiOrigin, const QString& projectId) {
    const QString canonical = canonicalProjectId(projectId);
    if (canonical.isEmpty()) return {};
    QUrl endpoint(apiOrigin);
    if (!endpoint.isValid() || endpoint.host().isEmpty() ||
        !endpoint.userInfo().isEmpty() || endpoint.hasQuery() ||
        endpoint.hasFragment()) {
        return {};
    }
    if (endpoint.scheme() == QLatin1String("https"))
        endpoint.setScheme(QStringLiteral("wss"));
    else if (endpoint.scheme() == QLatin1String("http"))
        endpoint.setScheme(QStringLiteral("ws"));
    else
        return {};
    QString path = endpoint.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    path += QStringLiteral("/desktop/projects/%1/live").arg(canonical);
    endpoint.setPath(path);
    endpoint.setUserInfo({});
    endpoint.setQuery({});
    endpoint.setFragment({});
    return endpoint;
}

bool validateEndpoint(const QString& apiOrigin, const QString& projectId,
                      const QUrl& proposed, QUrl* accepted) {
    const QUrl expected = endpointForProject(apiOrigin, projectId);
    if (expected.isEmpty() || !proposed.isValid() ||
        !proposed.userInfo().isEmpty() || proposed.hasQuery() ||
        proposed.hasFragment()) {
        return false;
    }
    const QString expectedWire =
        expected.toString(QUrl::FullyEncoded | QUrl::NormalizePathSegments);
    const QString proposedWire =
        proposed.toString(QUrl::FullyEncoded | QUrl::NormalizePathSegments);
    if (proposedWire != expectedWire) return false;
    if (accepted) *accepted = expected;
    return true;
}

int reconnectDelayMs(int attempt, int jitterPermille) {
    attempt = std::clamp(attempt, 1, 30);
    jitterPermille = std::clamp(jitterPermille, -200, 200);
    const int shift = std::min(attempt - 1, 20);
    const qint64 exponential = std::min<qint64>(
        kReconnectCapMs, qint64(kReconnectBaseMs) << shift);
    const qint64 jittered = exponential * (1000 + jitterPermille) / 1000;
    return int(std::clamp<qint64>(jittered, 1, kReconnectCapMs));
}

bool canQueueMessage(qsizetype currentMessages, qint64 currentBytes,
                     qint64 messageBytes) {
    return messageBytes > 0 && messageBytes <= kMaxMessageBytes &&
           currentMessages < kMaxQueuedMessages && currentBytes >= 0 &&
           currentBytes <= kMaxQueuedBytes - messageBytes;
}

#ifdef DAW_ENABLE_COLLABORATION
QByteArray tokenFingerprint(const QString& accessToken) {
    if (accessToken.isEmpty()) return {};
    return QCryptographicHash::hash(accessToken.toUtf8(),
                                    QCryptographicHash::Sha256);
}

QNetworkRequest handshakeRequest(const QUrl& endpoint,
                                 const QString& accessToken) {
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(kHandshakeTimeoutMs);
    QByteArray authorization = QByteArrayLiteral("Bearer ");
    authorization += accessToken.toUtf8();
    request.setRawHeader(QByteArrayLiteral("Authorization"), authorization);
    std::fill(authorization.begin(), authorization.end(), '\0');
    return request;
}
#endif

} // namespace

#ifdef DAW_ENABLE_COLLABORATION
CollaborationTransport::CollaborationTransport(account::Service* account,
                                               CollaborationService* service,
                                               QObject* parent)
    : QObject(parent), m_account(account), m_service(service) {
    Q_ASSERT(m_account);
    Q_ASSERT(m_service);
    m_reconnectTimer.setSingleShot(true);
    m_handshakeTimer.setSingleShot(true);
    m_healthTimer.setInterval(1000);

    connect(&m_reconnectTimer, &QTimer::timeout, this,
            &CollaborationTransport::startConnection);
    connect(&m_handshakeTimer, &QTimer::timeout, this, [this] {
        if (m_socket)
            socketFailed(m_socket, m_generation,
                         QStringLiteral("Session handshake timed out"));
    });
    connect(&m_healthTimer, &QTimer::timeout, this,
            &CollaborationTransport::healthCheck);
    connect(m_service, &CollaborationService::roomAuthorizationRequired, this,
            &CollaborationTransport::authorizationRequested);
    connect(m_service, &CollaborationService::outboundTextMessage, this,
            &CollaborationTransport::enqueueTextMessage);
    connect(m_service, &CollaborationService::stateChanged, this,
            [this](CollaborationState, const QString&) {
                collaborationStateChanged();
            });
    connect(m_service, &CollaborationService::projectChanged, this,
            [this](const QString&) { stopTransport(true); });
    connect(m_service, &CollaborationService::commandSchemaVersionChanged,
            this, [this](int) { stopTransport(true); });
    connect(m_account, &account::Service::authenticatedChanged, this,
            [this](bool) { accountStateChanged(); });
    connect(m_account, &account::Service::snapshotChanged, this,
            &CollaborationTransport::accountStateChanged);
    connect(m_account, &account::Service::logoutFinished, this,
            [this] { stopTransport(false); });
    m_tokenFingerprint = tokenFingerprint(m_account->accessToken());
}

CollaborationTransport::~CollaborationTransport() {
    m_reconnectTimer.stop();
    m_handshakeTimer.stop();
    m_healthTimer.stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        delete m_socket;
        m_socket = nullptr;
    }
    std::fill(m_tokenFingerprint.begin(), m_tokenFingerprint.end(), '\0');
}

void CollaborationTransport::authorizationRequested(
    const QString& projectId, const QUrl& endpoint) {
    QUrl accepted;
    const QString canonical = canonicalProjectId(projectId);
    if (canonical.isEmpty() ||
        !validateEndpoint(m_account->apiOrigin(), canonical, endpoint,
                          &accepted)) {
        permanentFailure(QStringLiteral("Invalid collaboration endpoint"));
        return;
    }
    const bool targetChanged = canonical != m_projectId || accepted != m_endpoint;
    if (targetChanged) {
        stopTransport(true);
        m_projectId = canonical;
        m_endpoint = accepted;
        m_retryAttempt = 0;
    }
    m_desired = true;
    if (!accountCanConnect()) {
        permanentFailure(QStringLiteral("Session authorization unavailable"));
        return;
    }
    if (m_socket || m_reconnectTimer.isActive()) return;
    if (m_retryAttempt > 0)
        scheduleReconnect();
    else
        startConnection();
}

void CollaborationTransport::accountStateChanged() {
    const QByteArray fingerprint = tokenFingerprint(m_account->accessToken());
    const bool credentialChanged = fingerprint != m_tokenFingerprint;
    if (credentialChanged) {
        std::fill(m_tokenFingerprint.begin(), m_tokenFingerprint.end(), '\0');
        m_tokenFingerprint = fingerprint;
    }
    if (!accountCanConnect()) {
        stopTransport(false);
        return;
    }
    if (!credentialChanged || !m_desired) return;

    retireSocket();
    m_retryAttempt = 0;
    m_service->trustedTransportDisconnected(
        QStringLiteral("Refreshing session authorization"));
    if (m_desired && !m_socket) scheduleReconnect(true);
}

void CollaborationTransport::collaborationStateChanged() {
    switch (m_service->state()) {
        case CollaborationState::LocalOnly:
        case CollaborationState::SignedOut:
        case CollaborationState::NoConnection:
            stopTransport(false);
            break;
        default:
            break;
    }
}

void CollaborationTransport::enqueueTextMessage(const QString& message) {
    if (!m_ready || !m_socket ||
        m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    const qint64 bytes = message.toUtf8().size();
    if (!canQueueMessage(m_outbound.size(), m_outboundBytes, bytes)) {
        socketFailed(m_socket, m_generation,
                     QStringLiteral("Outbound collaboration queue limit reached"));
        return;
    }
    m_outbound.enqueue({message, qsizetype(bytes)});
    m_outboundBytes += bytes;
    drainOutbound();
}

void CollaborationTransport::startConnection() {
    m_reconnectTimer.stop();
    if (m_socket || !m_desired || m_endpoint.isEmpty() ||
        !accountCanConnect()) {
        return;
    }
    const QString token = m_account->accessToken();
    if (token.isEmpty()) {
        permanentFailure(QStringLiteral("Session authorization unavailable"));
        return;
    }
    const QByteArray fingerprint = tokenFingerprint(token);
    if (fingerprint != m_tokenFingerprint) {
        std::fill(m_tokenFingerprint.begin(), m_tokenFingerprint.end(), '\0');
        m_tokenFingerprint = fingerprint;
    }

    auto* socket = new QWebSocket(QString(),
                                  QWebSocketProtocol::VersionLatest, this);
    socket->setMaxAllowedIncomingFrameSize(kMaxMessageBytes);
    socket->setMaxAllowedIncomingMessageSize(kMaxMessageBytes);
    socket->setReadBufferSize(kMaxMessageBytes * 2);
    m_socket = socket;
    const quint64 generation = ++m_generation;
    m_ready = false;
    m_waitingForPong = false;
    m_backoffResetForConnection = false;
    m_outbound.clear();
    m_outboundBytes = 0;

    connect(socket, &QWebSocket::connected, this,
            [this, socket, generation] {
                socketConnected(socket, generation);
            });
    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, socket, generation](const QString& message) {
                socketTextMessage(socket, generation, message);
            });
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            [this, socket, generation](const QByteArray&) {
                if (isCurrent(socket, generation))
                    permanentFailure(
                        QStringLiteral("Unexpected binary collaboration message"));
            });
    connect(socket, &QWebSocket::pong, this,
            [this, socket, generation](quint64, const QByteArray&) {
                socketPong(socket, generation);
            });
    connect(socket, &QWebSocket::bytesWritten, this,
            [this, socket, generation](qint64) {
                if (!isCurrent(socket, generation)) return;
                m_lastWriteProgress.restart();
                drainOutbound();
            });
    connect(socket, &QWebSocket::disconnected, this,
            [this, socket, generation] {
                if (isCurrent(socket, generation) &&
                    isPermanentCloseCode(socket->closeCode())) {
                    const QString reason = socket->closeReason().trimmed();
                    permanentFailure(
                        reason.isEmpty()
                            ? QStringLiteral("Collaboration connection was rejected")
                            : reason);
                    return;
                }
                socketFailed(socket, generation,
                             QStringLiteral("Session connection closed"));
            });
    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, generation](QAbstractSocket::SocketError) {
                socketFailed(socket, generation,
                             QStringLiteral("Session connection failed"));
            });

    QWebSocketHandshakeOptions options;
    options.setSubprotocols({m_service->protocolName()});
    socket->open(handshakeRequest(m_endpoint, token), options);
    m_handshakeTimer.start(kHandshakeTimeoutMs);
}

void CollaborationTransport::socketConnected(QWebSocket* socket,
                                             quint64 generation) {
    if (!isCurrent(socket, generation)) return;
    m_handshakeTimer.stop();
    if (!accountCanConnect() || !m_desired) {
        stopTransport(false);
        return;
    }
    if (socket->subprotocol() != m_service->protocolName()) {
        permanentFailure(
            QStringLiteral("Collaboration protocol was not accepted"));
        return;
    }
    m_ready = true;
    m_waitingForPong = false;
    m_lastReceive.start();
    m_lastPing.start();
    m_lastWriteProgress.start();
    m_connectedFor.start();
    m_healthTimer.start();
    m_service->trustedTransportConnected();
}

void CollaborationTransport::socketTextMessage(QWebSocket* socket,
                                               quint64 generation,
                                               const QString& message) {
    if (!isCurrent(socket, generation) || !m_ready) return;
    const qint64 bytes = message.toUtf8().size();
    if (bytes <= 0 || bytes > kMaxMessageBytes) {
        permanentFailure(
            QStringLiteral("Invalid collaboration message size"));
        return;
    }
    m_lastReceive.restart();
    m_service->receiveTrustedTextMessage(message);
}

void CollaborationTransport::socketPong(QWebSocket* socket,
                                        quint64 generation) {
    if (!isCurrent(socket, generation) || !m_ready) return;
    m_waitingForPong = false;
    m_lastReceive.restart();
}

void CollaborationTransport::socketFailed(QWebSocket* socket,
                                          quint64 generation,
                                          const QString& safeReason) {
    if (!isCurrent(socket, generation)) return;
    retireSocket();
    if (!m_desired || !accountCanConnect()) return;
    m_retryAttempt = std::min(m_retryAttempt + 1, 30);
    scheduleReconnect();
    m_service->trustedTransportDisconnected(safeReason);
}

void CollaborationTransport::drainOutbound() {
    if (!m_ready || !m_socket ||
        m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    while (!m_outbound.isEmpty()) {
        const PendingMessage& pending = m_outbound.head();
        if (m_socket->bytesToWrite() + pending.utf8Bytes >
            kMaxSocketBufferedBytes) {
            break;
        }
        const PendingMessage sending = m_outbound.dequeue();
        m_outboundBytes -= sending.utf8Bytes;
        if (m_socket->sendTextMessage(sending.text) < 0) {
            socketFailed(m_socket, m_generation,
                         QStringLiteral("Session write failed"));
            return;
        }
    }
}

void CollaborationTransport::healthCheck() {
    if (!m_ready || !m_socket ||
        m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    if (m_lastReceive.hasExpired(kIdleTimeoutMs)) {
        socketFailed(m_socket, m_generation,
                     QStringLiteral("Session connection became idle"));
        return;
    }
    if (m_waitingForPong && m_lastPing.hasExpired(kPongTimeoutMs)) {
        socketFailed(m_socket, m_generation,
                     QStringLiteral("Session heartbeat timed out"));
        return;
    }
    if (!m_waitingForPong && m_lastPing.hasExpired(kPingIntervalMs)) {
        m_waitingForPong = true;
        m_lastPing.restart();
        m_socket->ping(QByteArray::number(m_generation));
    }
    if ((m_socket->bytesToWrite() > 0 || !m_outbound.isEmpty()) &&
        m_lastWriteProgress.hasExpired(kWriteTimeoutMs)) {
        socketFailed(m_socket, m_generation,
                     QStringLiteral("Session write timed out"));
        return;
    }
    if (!m_backoffResetForConnection &&
        m_connectedFor.hasExpired(kStableConnectionMs)) {
        m_retryAttempt = 0;
        m_backoffResetForConnection = true;
    }
    drainOutbound();
}

void CollaborationTransport::scheduleReconnect(bool immediate) {
    if (!m_desired || m_endpoint.isEmpty() || !accountCanConnect() ||
        m_socket) {
        return;
    }
    const int jitter = int(QRandomGenerator::global()->bounded(401)) - 200;
    const int delay = immediate ? 0 : reconnectDelayMs(m_retryAttempt, jitter);
    if (!m_reconnectTimer.isActive() ||
        m_reconnectTimer.remainingTime() > delay) {
        m_reconnectTimer.start(delay);
    }
}

void CollaborationTransport::stopTransport(bool clearTarget) {
    m_desired = false;
    m_reconnectTimer.stop();
    retireSocket();
    m_retryAttempt = 0;
    if (clearTarget) {
        m_projectId.clear();
        m_endpoint.clear();
    }
}

void CollaborationTransport::retireSocket() {
    m_handshakeTimer.stop();
    m_healthTimer.stop();
    m_ready = false;
    m_waitingForPong = false;
    m_outbound.clear();
    m_outboundBytes = 0;
    QWebSocket* socket = m_socket;
    m_socket = nullptr;
    ++m_generation;
    if (!socket) return;
    socket->disconnect(this);
    // Abort before another generation may open. This guarantees there is only
    // one live TCP/WebSocket transport even during credential rotation; the
    // server observes the closed connection and applies its reconnect grace.
    socket->abort();
    socket->deleteLater();
}

void CollaborationTransport::permanentFailure(const QString& safeReason) {
    stopTransport(false);
    m_service->trustedTransportUnavailable(safeReason);
}

bool CollaborationTransport::isCurrent(const QWebSocket* socket,
                                       quint64 generation) const {
    return socket && socket == m_socket && generation == m_generation;
}

bool CollaborationTransport::accountCanConnect() const {
    return m_account && m_account->authenticated() &&
           !m_account->snapshot().offline &&
           !m_account->accessToken().isEmpty();
}
#endif

bool checkCollaborationTransportForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString projectId =
        QStringLiteral("6ba7b810-9dad-41d1-80b4-00c04fd430c8");
    const QUrl endpoint = endpointForProject(
        QStringLiteral("https://api.vlt.invalid/v1"), projectId);
    if (endpoint.scheme() != QLatin1String("wss") ||
        endpoint.path() !=
            QLatin1String("/v1/desktop/projects/"
                          "6ba7b810-9dad-41d1-80b4-00c04fd430c8/live") ||
        endpoint.hasQuery() || !endpoint.userInfo().isEmpty()) {
        return fail(QStringLiteral("collaboration endpoint was not canonical"));
    }
    QUrl accepted;
    if (!validateEndpoint(QStringLiteral("https://api.vlt.invalid/v1"),
                          projectId, endpoint, &accepted) ||
        accepted != endpoint) {
        return fail(QStringLiteral("canonical endpoint was rejected"));
    }
    QUrl exfiltration(endpoint);
    exfiltration.setHost(QStringLiteral("attacker.invalid"));
    if (validateEndpoint(QStringLiteral("https://api.vlt.invalid/v1"),
                         projectId, exfiltration, nullptr)) {
        return fail(QStringLiteral("cross-origin endpoint was accepted"));
    }
    exfiltration = endpoint;
    exfiltration.setQuery(QStringLiteral("access_token=secret"));
    if (validateEndpoint(QStringLiteral("https://api.vlt.invalid/v1"),
                         projectId, exfiltration, nullptr) ||
        !endpointForProject(QStringLiteral("https://api.vlt.invalid/v1"),
                            QStringLiteral("not-a-uuid")).isEmpty()) {
        return fail(QStringLiteral("unsafe endpoint input was accepted"));
    }
    if (reconnectDelayMs(1, -200) != 400 ||
        reconnectDelayMs(1, 200) != 600 ||
        reconnectDelayMs(30, 200) != kReconnectCapMs) {
        return fail(QStringLiteral("reconnect backoff escaped its bounds"));
    }
    if (!canQueueMessage(0, 0, kMaxMessageBytes) ||
        canQueueMessage(kMaxQueuedMessages, 0, 1) ||
        canQueueMessage(0, kMaxQueuedBytes, 1) ||
        canQueueMessage(0, 0, kMaxMessageBytes + 1)) {
        return fail(QStringLiteral("outbound queue limits were not enforced"));
    }
#ifdef DAW_ENABLE_COLLABORATION
    const QNetworkRequest request = handshakeRequest(
        endpoint, QStringLiteral("selftest-token"));
    if (request.url() != endpoint || request.url().hasQuery() ||
        request.rawHeader(QByteArrayLiteral("Authorization")) !=
            QByteArrayLiteral("Bearer selftest-token") ||
        request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() !=
            int(QNetworkRequest::ManualRedirectPolicy)) {
        return fail(QStringLiteral("WebSocket authorization was not header-only"));
    }
    QWebSocketHandshakeOptions options;
    options.setSubprotocols({QString::fromLatin1(kProtocolNameV2),
                             QString::fromLatin1(kProtocolName)});
    if (options.subprotocols() !=
        QStringList{QString::fromLatin1(kProtocolNameV2),
                    QString::fromLatin1(kProtocolName)}) {
        return fail(QStringLiteral("collaboration subprotocol was not requested"));
    }
    if (!isPermanentCloseCode(
            QWebSocketProtocol::CloseCodePolicyViolated) ||
        !isPermanentCloseCode(
            QWebSocketProtocol::CloseCodeProtocolError) ||
        isPermanentCloseCode(QWebSocketProtocol::CloseCodeGoingAway) ||
        isPermanentCloseCode(QWebSocketProtocol::CloseCodeBadOperation)) {
        return fail(QStringLiteral(
            "permanent WebSocket rejection was not classified safely"));
    }
#endif
    return true;
}

} // namespace collab
