#include "AccountService.hpp"

#include "AiPrefs.hpp"
#include "PlatformDiagnostics.hpp"
#include "SecureStorage.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QLockFile>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSysInfo>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>

#include <openssl/evp.h>

#include <algorithm>
#include <utility>

namespace account {

Service* Service::s_instance = nullptr;

namespace {
#ifndef VLT_DEFAULT_API_ORIGIN
#define VLT_DEFAULT_API_ORIGIN "http://localhost:8080/v1"
#endif

QByteArray decodeBase64Url(QByteArray value) {
    while (value.size() % 4) value.append('=');
    return QByteArray::fromBase64(value, QByteArray::Base64UrlEncoding);
}

bool verifyEntitlement(const QString& token, const QString& publicKey,
                       QJsonObject* claims) {
    const QList<QByteArray> parts = token.toUtf8().split('.');
    if (parts.size() != 3) return false;
    const QByteArray key = QByteArray::fromBase64(publicKey.toUtf8());
    const QByteArray signature = decodeBase64Url(parts.at(2));
    if (key.size() != 32 || signature.size() != 64) return false;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(key.constData()), size_t(key.size()));
    if (!pkey) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const QByteArray signedBody = parts.at(0) + '.' + parts.at(1);
    const bool valid = context && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestVerify(context,
            reinterpret_cast<const unsigned char*>(signature.constData()), size_t(signature.size()),
            reinterpret_cast<const unsigned char*>(signedBody.constData()), size_t(signedBody.size())) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(pkey);
    if (!valid) return false;
    const QJsonDocument payload = QJsonDocument::fromJson(decodeBase64Url(parts.at(1)));
    if (!payload.isObject()) return false;
    *claims = payload.object();
    return claims->value(QStringLiteral("scope")).toString() == QLatin1String("offline");
}

QString platformName() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("unsupported");
#endif
}

QString normalizedApiOrigin(QString origin) {
    while (origin.endsWith('/')) origin.chop(1);
    // Older builds and developer environments used the host root as the
    // origin and added /v1 at every call site. Accept that spelling too, but
    // keep one canonical, versioned base URL everywhere inside the app.
    if (!origin.endsWith(QStringLiteral("/v1")))
        origin += QStringLiteral("/v1");
    return origin;
}
}

Service::Service(QObject* parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)),
      m_refreshTimer(new QTimer(this)) {
    Q_ASSERT(!s_instance);
    s_instance = this;
    m_apiOrigin = normalizedApiOrigin(qEnvironmentVariable(
        "VLT_API_ORIGIN", QString::fromUtf8(VLT_DEFAULT_API_ORIGIN)));

    // One-way privacy migration from pre-account builds. Values are removed
    // locally and are never read into memory or transmitted to VLT servers.
    QSettings settings;
    for (const QString& key : {
             QStringLiteral("ai/apiKey.openai"), QStringLiteral("ai/apiKey.anthropic"),
             QStringLiteral("ai/baseUrl.openai"), QStringLiteral("ai/baseUrl.anthropic"),
             QStringLiteral("ai/music.apiKey")}) {
        settings.remove(key);
    }
    settings.setValue(QStringLiteral("account/legacyAiSecretsRemoved"), true);
    m_refreshTimer->setInterval(12 * 60 * 1000);
    connect(m_refreshTimer, &QTimer::timeout, this, &Service::beginRestore);
    m_restoreRetryTimer = new QTimer(this);
    m_restoreRetryTimer->setSingleShot(true);
    m_restoreRetryTimer->setInterval(500);
    connect(m_restoreRetryTimer, &QTimer::timeout, this, &Service::beginRestore);
}

Service::~Service() {
    for (auto* reply : m_network->findChildren<QNetworkReply*>()) {
        reply->disconnect(this);
        reply->abort();
    }
    s_instance = nullptr;
}

Service* Service::instance() { return s_instance; }

QString Service::installationId() const {
    QSettings settings;
    QString value = settings.value(QStringLiteral("account/installationId")).toString();
    if (QUuid(value).isNull()) {
        value = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("account/installationId"), value);
    }
    return value;
}

bool Service::lockCredentials(bool retry) {
    if (m_credentialLock && m_credentialLock->isLocked()) return true;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!directory.isEmpty() && QDir().mkpath(directory)) {
        m_credentialLock = std::make_unique<QLockFile>(QDir(directory).filePath("account-session.lock"));
        m_credentialLock->setStaleLockTime(0);
        if (m_credentialLock->tryLock(0)) return true;
        if (retry && m_credentialLock->error() == QLockFile::LockFailedError) {
            if (!m_restoreRetryTimer->isActive()) m_restoreRetryTimer->start();
            return false;
        }
    }
    emit errorOccurred(QStringLiteral("session_storage_busy"),
                       tr("Saved sign-in is temporarily unavailable. Close other copies of VLTONE and try again."));
    return false;
}

void Service::restoreSavedSession() {
    if (m_busy) return;
    m_allowVaultAccess = true;
    beginRestore();
}

void Service::beginRestore() {
    if (m_busy) return;
    if (!lockCredentials(true)) return;
    const auto interaction = std::exchange(m_allowVaultAccess, false)
        ? securestorage::Interaction::Allow : securestorage::Interaction::Disallow;
    if (!m_pendingSession.isEmpty()) {
        // A refresh already consumed the old token. Retry its local save before
        // sending another request. The durable request ID also protects this
        // rotation if the process exits before the pending save succeeds.
        acceptSession(m_pendingSession, interaction == securestorage::Interaction::Allow);
        return;
    }
    const auto saved = securestorage::readSession(interaction);
    if (saved.unavailable) {
        m_credentialLock.reset();
        emit errorOccurred(QStringLiteral("secure_storage_locked"),
            tr("Saved sign-in is locked by the operating system. Choose Restore saved sign-in to allow access without entering your account password again."));
        return;
    }
    const QJsonDocument stored = QJsonDocument::fromJson(saved.value);
    if (!stored.isObject()) {
        m_credentialLock.reset();
        emit authenticationRequired(tr("Sign in to continue."), false);
        return;
    }
    QJsonObject credentials = stored.object();
    m_refreshToken = credentials.value(QStringLiteral("refresh_token")).toString();
    if (m_refreshToken.isEmpty()) {
        m_credentialLock.reset();
        emit authenticationRequired(tr("Sign in to continue."), false);
        return;
    }
    // Persist the operation BEFORE the server consumes the token. A crash,
    // lost response or failed write can then retry exactly the same rotation.
    QString requestID = credentials.value(QStringLiteral("refresh_request_id")).toString();
    if (QUuid(requestID).isNull()) {
        requestID = QUuid::createUuid().toString(QUuid::WithoutBraces);
        credentials.insert(QStringLiteral("refresh_request_id"), requestID);
        const auto envelope = QJsonDocument(credentials).toJson(QJsonDocument::Compact);
        if (!securestorage::write(envelope, interaction) || securestorage::read() != envelope) {
            m_credentialLock.reset();
            emit errorOccurred(QStringLiteral("secure_storage_failed"),
                               tr("The operating-system credential vault could not save this session."));
            return;
        }
    }
    setBusy(true);
    QNetworkReply* reply = postJson(QStringLiteral("/desktop/auth/refresh"), {
        {QStringLiteral("refresh_token"), m_refreshToken},
        {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
    }, {}, requestID);
    connect(reply, &QNetworkReply::finished, this, [this, reply, credentials] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto session = QJsonDocument::fromJson(reply->peek(reply->bytesAvailable())).object();
        const bool validSession = !session.value("access_token").toString().isEmpty() &&
                                  !session.value("refresh_token").toString().isEmpty();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 && validSession) {
            handleSessionReply(reply, true);
            if (m_pendingSession.isEmpty()) m_credentialLock.reset();
            setBusy(false);
            return;
        }
        const auto error = QJsonDocument::fromJson(reply->readAll()).object();
        const QString code = error.value(QStringLiteral("code")).toString();
        const bool rejected = (status == 401 &&
            (code == "refresh_token_invalid" || code == "refresh_token_reused")) ||
            (status == 403 && code == "account_unavailable");
        const bool temporaryFailure = status == 0 || status == 408 || status == 429 ||
                                      status >= 500 || (status >= 200 && status < 300) ||
                                      ((status == 401 || status == 403) && !rejected);
        reply->deleteLater();
        QString reason;
        if (temporaryFailure && acceptOffline(credentials, &reason)) {
            m_credentialLock.reset();
            setBusy(false);
            return;
        }
        // Only an explicit credential rejection invalidates the saved login.
        // Maintenance, proxy errors and rate limiting must not sign users out.
        if (rejected) {
            securestorage::clear();
            m_authenticated = false;
            m_accessToken.clear();
            m_refreshTimer->stop();
        }
        m_credentialLock.reset();
        setBusy(false);
        emit authenticationRequired(reason.isEmpty() ? tr("Online sign-in is required.") : reason,
                                    temporaryFailure);
    });
}

void Service::login(const QString& email, const QString& password) {
    if (m_busy) return;
    if (!lockCredentials(false)) return;
    setBusy(true);
    const QJsonObject hardware = PlatformDiagnostics::hardwareSnapshot();
    QNetworkReply* reply = postJson(QStringLiteral("/desktop/auth/login"), {
        {QStringLiteral("email"), email.trimmed()},
        {QStringLiteral("password"), password},
        {QStringLiteral("installation_id"), installationId()},
        {QStringLiteral("device_name"), platformName() == QLatin1String("windows")
             ? tr("Windows device") : tr("Mac device")},
        {QStringLiteral("platform"), platformName()},
        {QStringLiteral("os_version"), QSysInfo::prettyProductName()},
        {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("hardware"), hardware},
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleSessionReply(reply, false);
        if (m_pendingSession.isEmpty()) m_credentialLock.reset();
        setBusy(false);
    });
}

void Service::handleSessionReply(QNetworkReply* reply, bool refresh) {
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkDetail = reply->errorString();
    const QJsonDocument document = QJsonDocument::fromJson(body);
    reply->deleteLater();

    if (status == 0 || (networkError != QNetworkReply::NoError && body.isEmpty())) {
        const QString message = tr("Could not connect to the account server (%1). %2")
                                    .arg(m_apiOrigin, networkDetail);
        if (refresh) emit authenticationRequired(message, true);
        else emit errorOccurred(QStringLiteral("network_unavailable"), message);
        return;
    }
    if (status < 200 || status >= 300 || !document.isObject()) {
        if (!document.isObject()) {
            const QString message =
                tr("The account server returned an invalid response (HTTP %1).")
                    .arg(status);
            if (refresh) emit authenticationRequired(message, false);
            else emit errorOccurred(QStringLiteral("invalid_server_response"),
                                    message);
            return;
        }
        const QJsonObject error = document.object();
        const QString code = error.value(QStringLiteral("code")).toString(
            refresh ? QStringLiteral("refresh_failed") : QStringLiteral("login_failed"));
        const QString message = error.value(QStringLiteral("message")).toString(
            tr("The server could not complete sign-in."));
        if (refresh) emit authenticationRequired(message, false);
        else emit errorOccurred(code, message);
        return;
    }
    acceptSession(document.object(), !refresh);
}

void Service::acceptSession(const QJsonObject& response, bool explicitSignIn) {
    // Never close the login gate before the rotated credential is durable and
    // readable without an OS dialog. Otherwise a successful login immediately
    // hides the storage error and the next launch asks for a password again.
    if (response.value(QStringLiteral("access_token")).toString().isEmpty() ||
        response.value(QStringLiteral("refresh_token")).toString().isEmpty()) {
        m_credentialLock.reset();
        emit errorOccurred(QStringLiteral("invalid_server_response"),
                           tr("The server could not complete sign-in."));
        return;
    }
    if (!persistCredentials(response, explicitSignIn)) {
        m_pendingSession = response;
        return;
    }
    // response may refer to m_pendingSession; clear it only after consuming it.
    m_accessToken = response.value(QStringLiteral("access_token")).toString();
    m_refreshToken = response.value(QStringLiteral("refresh_token")).toString();
    m_reporterToken = response.value(QStringLiteral("reporter_token")).toString();
    m_offlineEntitlement = response.value(QStringLiteral("offline_entitlement")).toString();
    m_publicKey = response.value(QStringLiteral("public_key")).toString();
    m_lastServerTime = QDateTime::fromString(response.value(QStringLiteral("server_time")).toString(), Qt::ISODate).toSecsSinceEpoch();
    const QJsonObject user = response.value(QStringLiteral("user")).toObject();
    const QJsonObject device = response.value(QStringLiteral("device")).toObject();
    const QJsonObject quota = response.value(QStringLiteral("quota")).toObject();
    m_snapshot.userId = user.value(QStringLiteral("id")).toString();
    m_snapshot.email = user.value(QStringLiteral("email")).toString();
    m_snapshot.nickname = user.value(QStringLiteral("nickname")).toString();
    m_snapshot.deviceId = device.value(QStringLiteral("id")).toString();
    applyQuota(quota);
    m_snapshot.lastSyncAt = QDateTime::fromSecsSinceEpoch(m_lastServerTime, QTimeZone::UTC);
    m_snapshot.offline = false;
    m_pendingSession = {};
    m_credentialLock.reset();
    m_authenticated = true;
    m_refreshTimer->start();
    emit snapshotChanged();
    emit authenticatedChanged(true);
    refreshAiModels();
}

bool Service::persistCredentials(const QJsonObject& response, bool explicitSignIn) {
    QJsonObject stored;
    for (const QString& key : {QStringLiteral("refresh_token"), QStringLiteral("reporter_token"),
                               QStringLiteral("offline_entitlement"), QStringLiteral("public_key"),
                               QStringLiteral("server_time"), QStringLiteral("offline_expires_at")}) {
        stored.insert(key, response.value(key));
    }
    const QJsonObject user = response.value(QStringLiteral("user")).toObject();
    stored.insert(QStringLiteral("user"), QJsonObject{
        {QStringLiteral("id"), user.value(QStringLiteral("id"))},
        {QStringLiteral("email"), user.value(QStringLiteral("email"))},
        {QStringLiteral("nickname"), user.value(QStringLiteral("nickname"))},
    });
    const QJsonObject device = response.value(QStringLiteral("device")).toObject();
    stored.insert(QStringLiteral("device"), QJsonObject{
        {QStringLiteral("id"), device.value(QStringLiteral("id"))},
    });
    const QJsonObject quota = response.value(QStringLiteral("quota")).toObject();
    QJsonObject storedQuota;
    for (const QString& key : {QStringLiteral("base_limit"), QStringLiteral("adjustment"),
                               QStringLiteral("used_tokens"), QStringLiteral("remaining_tokens"),
                               QStringLiteral("ends_at")}) {
        storedQuota.insert(key, quota.value(key));
    }
    stored.insert(QStringLiteral("quota"), storedQuota);
    stored.insert(QStringLiteral("last_observed_time"),
                  response.value(QStringLiteral("server_time")));
    const auto interaction = explicitSignIn ? securestorage::Interaction::Allow
                                            : securestorage::Interaction::Disallow;
    const QByteArray envelope = QJsonDocument(stored).toJson(QJsonDocument::Compact);
    if (!securestorage::write(envelope, interaction) || securestorage::read() != envelope) {
        emit errorOccurred(QStringLiteral("secure_storage_failed"),
                           tr("The operating-system credential vault could not save this session."));
        return false;
    }
    return true;
}

bool Service::acceptOffline(const QJsonObject& credentials, QString* reason) {
    const QString entitlement = credentials.value(QStringLiteral("offline_entitlement")).toString();
    const QString publicKey = credentials.value(QStringLiteral("public_key")).toString();
    QJsonObject claims;
    if (!verifyEntitlement(entitlement, publicKey, &claims)) {
        *reason = tr("The stored offline entitlement is invalid.");
        return false;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 lastServer = QDateTime::fromString(credentials.value(QStringLiteral("server_time")).toString(), Qt::ISODate).toSecsSinceEpoch();
    const qint64 lastObserved = QDateTime::fromString(
        credentials.value(QStringLiteral("last_observed_time")).toString(),
        Qt::ISODate).toSecsSinceEpoch();
    if (now + 300 < std::max(lastServer, lastObserved)) {
        *reason = tr("The system clock was moved backwards. Connect to the server to continue.");
        return false;
    }
    if (claims.value(QStringLiteral("exp")).toInteger() <= now) {
        *reason = tr("The 72-hour offline allowance has expired.");
        return false;
    }
    const QJsonObject user = credentials.value(QStringLiteral("user")).toObject();
    const QJsonObject device = credentials.value(QStringLiteral("device")).toObject();
    const QJsonObject quota = credentials.value(QStringLiteral("quota")).toObject();
    m_refreshToken = credentials.value(QStringLiteral("refresh_token")).toString();
    m_reporterToken = credentials.value(QStringLiteral("reporter_token")).toString();
    m_offlineEntitlement = entitlement;
    m_publicKey = publicKey;
    m_lastServerTime = lastServer;
    m_snapshot.userId = user.value(QStringLiteral("id")).toString();
    m_snapshot.email = user.value(QStringLiteral("email")).toString();
    m_snapshot.nickname = user.value(QStringLiteral("nickname")).toString();
    m_snapshot.deviceId = device.value(QStringLiteral("id")).toString();
    applyQuota(quota);
    m_snapshot.lastSyncAt = QDateTime::fromSecsSinceEpoch(lastServer, QTimeZone::UTC);
    m_snapshot.offline = true;
    m_authenticated = true;
    m_refreshTimer->start();
    QJsonObject updated = credentials;
    updated.insert(QStringLiteral("last_observed_time"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!securestorage::write(QJsonDocument(updated).toJson(QJsonDocument::Compact))) {
        *reason = tr("The operating-system credential vault could not update the offline clock guard.");
        m_authenticated = false;
        m_refreshTimer->stop();
        return false;
    }
    emit snapshotChanged();
    emit authenticatedChanged(true);
    return true;
}

void Service::logout() {
    if (!lockCredentials(false)) return;
    m_refreshTimer->stop();
    m_restoreRetryTimer->stop();
    for (auto* pending : m_network->findChildren<QNetworkReply*>()) {
        pending->disconnect(this);
        pending->abort();
        pending->deleteLater();
    }
    m_pendingSession = {};
    m_allowVaultAccess = false;
    setBusy(true);
    if (!m_accessToken.isEmpty()) {
        QNetworkReply* reply = postJson(QStringLiteral("/desktop/auth/logout"), {}, m_accessToken);
        QTimer::singleShot(5000, reply, [reply] {
            if (reply->isRunning()) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            finishLogout();
        });
        return;
    }
    finishLogout();
}

void Service::finishLogout() {
    securestorage::clear(securestorage::Interaction::Allow);
    m_refreshTimer->stop();
    m_accessToken.clear(); m_refreshToken.clear(); m_reporterToken.clear();
    m_offlineEntitlement.clear(); m_publicKey.clear(); m_snapshot = {};
    m_pendingSession = {};
    m_credentialLock.reset();
    m_authenticated = false;
    setBusy(false);
    ui::aiprefs::setManagedModels({});
    emit snapshotChanged();
    emit aiModelsChanged();
    emit authenticatedChanged(false);
    emit logoutFinished();
}

void Service::installHeadlessTestSession() {
    m_snapshot.email = QStringLiteral("selftest@vlt.invalid");
    m_snapshot.nickname = QStringLiteral("Self-test");
    m_snapshot.deviceId = QStringLiteral("00000000-0000-0000-0000-000000000001");
    m_snapshot.lastSyncAt = QDateTime::currentDateTimeUtc();
    m_authenticated = true;
}

void Service::refreshAiModels() {
    if (!m_authenticated || m_accessToken.isEmpty()) return;
    QNetworkRequest request(QUrl(m_apiOrigin + QStringLiteral("/desktop/ai/models")));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    request.setTransferTimeout(15'000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(body);
        reply->deleteLater();
        if (status < 200 || status >= 300 || !document.isObject()) return;
        QList<ui::aiprefs::ModelConnection> models;
        for (const QJsonValue& value :
             document.object().value(QStringLiteral("models")).toArray()) {
            if (!value.isObject()) continue;
            const QJsonObject object = value.toObject();
            ui::aiprefs::ModelConnection model;
            model.id = object.value(QStringLiteral("id")).toString();
            model.displayName =
                object.value(QStringLiteral("display_name")).toString();
            model.provider = ui::aiprefs::providerFromId(
                object.value(QStringLiteral("provider")).toString());
            model.model = object.value(QStringLiteral("model")).toString();
            model.source = ui::aiprefs::ModelSource::Managed;
            models.append(std::move(model));
        }
        ui::aiprefs::setManagedModels(models);
        emit aiModelsChanged();
    });
}

void Service::refreshQuota() {
    if (!m_authenticated || m_accessToken.isEmpty() || m_snapshot.offline) return;
    QNetworkRequest request(QUrl(m_apiOrigin + QStringLiteral("/desktop/me")));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
    request.setTransferTimeout(15'000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(body);
        reply->deleteLater();
        if (status < 200 || status >= 300 || !document.isObject()) return;
        const QJsonObject response = document.object();
        applyQuota(response.value(QStringLiteral("quota")).toObject());
        m_snapshot.lastSyncAt = QDateTime::fromString(
            response.value(QStringLiteral("server_time")).toString(), Qt::ISODate);
        emit snapshotChanged();
    });
}

void Service::applyQuota(const QJsonObject& quota) {
    m_snapshot.tokenLimit =
        qint64(quota.value(QStringLiteral("base_limit")).toDouble()) +
        qint64(quota.value(QStringLiteral("adjustment")).toDouble());
    m_snapshot.tokensUsed =
        qint64(quota.value(QStringLiteral("used_tokens")).toDouble());
    m_snapshot.tokensRemaining =
        qint64(quota.value(QStringLiteral("remaining_tokens")).toDouble());
    m_snapshot.cycleEndsAt = QDateTime::fromString(
        quota.value(QStringLiteral("ends_at")).toString(), Qt::ISODate);
}

void Service::setBusy(bool busy) {
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged(busy);
}

QNetworkReply* Service::postJson(const QString& path, const QJsonObject& body,
                                 const QString& bearer, const QString& requestID) {
    QNetworkRequest request(QUrl(m_apiOrigin + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(15'000);
    if (!bearer.isEmpty()) request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    if (!requestID.isEmpty()) request.setRawHeader("Idempotency-Key", requestID.toUtf8());
    return m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

} // namespace account
