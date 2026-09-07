// Real account service + local HTTP server + isolated in-memory credential vault.
// No requests to the account platform and no access to the user's keychain.
#include "AccountService.hpp"
#include "AiPrefs.hpp"
#include "PlatformDiagnostics.hpp"
#include "SecureStorage.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QLockFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <openssl/evp.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
QByteArray vault;
bool writable = true;
bool readable = true;
int required = 0;
int errors = 0;
int successes = 0;
int refreshes = 0;
int status = 200;
QJsonObject response;
QString receivedToken;
QString receivedRequestID;
void waitFor(const std::function<bool()>& done) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < 4000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    check(done(), "asynchronous account operation completed");
}
void observe(account::Service& service) {
    QObject::connect(&service, &account::Service::authenticationRequired,
                     [](const QString&, bool) { ++required; });
    QObject::connect(&service, &account::Service::errorOccurred,
                     [](const QString&, const QString&) { ++errors; });
    QObject::connect(&service, &account::Service::authenticatedChanged,
                     [](bool ready) { if (ready) ++successes; });
}
QJsonObject session(int generation, qint64 expiry) {
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    check(key, "generate offline signing key");
    unsigned char publicKey[32];
    size_t keySize = sizeof(publicKey);
    check(EVP_PKEY_get_raw_public_key(key, publicKey, &keySize) == 1, "export public key");
    const auto base64 = [](const QByteArray& bytes) {
        return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    };
    const QByteArray payload = QJsonDocument(QJsonObject{
        {"scope", "offline"}, {"exp", expiry}}).toJson(QJsonDocument::Compact);
    const QByteArray body = base64("{\"alg\":\"EdDSA\"}") + '.' + base64(payload);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    unsigned char signature[64];
    size_t size = sizeof(signature);
    check(EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
          EVP_DigestSign(context, signature, &size,
                         reinterpret_cast<const unsigned char*>(body.constData()), body.size()) == 1,
          "sign offline entitlement");
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return {{"access_token", QString("access-%1").arg(generation)},
            {"refresh_token", QString("refresh-%1").arg(generation)},
            {"reporter_token", "reporter"},
            {"offline_entitlement", QString::fromUtf8(body + '.' + base64(
                 QByteArray(reinterpret_cast<const char*>(signature), size)))},
            {"public_key", QString::fromUtf8(QByteArray(
                 reinterpret_cast<const char*>(publicKey), keySize).toBase64())},
            {"server_time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"user", QJsonObject{{"id", "user"}, {"email", "test@vlt.invalid"}}},
            {"device", QJsonObject{{"id", "device"}}}, {"quota", QJsonObject{}}};
}
}
namespace account::securestorage {
bool write(const QByteArray& value, Interaction) {
    if (!writable) return false;
    vault = value;
    return true;
}
QByteArray read() { return readable ? vault : QByteArray{}; }
ReadResult readSession(Interaction interaction) {
    if (interaction == Interaction::Allow) readable = true;
    return {read(), !readable};
}
bool clear(Interaction) { vault.clear(); return true; }
}
QJsonObject PlatformDiagnostics::hardwareSnapshot() { return {}; }
namespace ui::aiprefs {
void setManagedModels(const QList<ModelConnection>&) {}
Provider providerFromId(const QString&) { return Provider::OpenAi; }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir settings;
    check(settings.isValid(), "isolated settings directory");
    QCoreApplication::setOrganizationName("VLTAccountRestoreTest");
    QCoreApplication::setApplicationName("AccountRestoreTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    QTcpServer server;
    check(server.listen(QHostAddress::LocalHost), "start isolated HTTP server");
    qputenv("VLT_API_ORIGIN", QString("http://127.0.0.1:%1/v1").arg(server.serverPort()).toUtf8());
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, buffer] {
                *buffer += socket->readAll();
                const auto end = buffer->indexOf("\r\n\r\n");
                if (end < 0) return;
                int length = 0;
                for (const auto& line : buffer->left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toInt();
                if (buffer->size() < end + 4 + length) return;
                const bool refresh = buffer->startsWith("POST /v1/desktop/auth/refresh ");
                const bool login = buffer->startsWith("POST /v1/desktop/auth/login ");
                if (refresh) {
                    ++refreshes;
                    receivedRequestID.clear();
                    for (const auto& line : buffer->left(end).split('\n'))
                        if (line.toLower().startsWith("idempotency-key:")) receivedRequestID = QString::fromUtf8(line.mid(16).trimmed());
                    check(!QJsonDocument::fromJson(buffer->mid(end + 4, length)).object().contains("request_id"), "legacy API receives a compatible JSON body");
                    receivedToken = QJsonDocument::fromJson(buffer->mid(end + 4, length))
                        .object().value("refresh_token").toString();
                }
                const int code = refresh || login ? status : 200;
                const QByteArray body = QJsonDocument(refresh || login ? response : QJsonObject{})
                    .toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 " + QByteArray::number(code) + " Test\r\n"
                    "Content-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    response = session(1, now + 3600);
    {
        account::Service service; observe(service);
        service.login("test@vlt.invalid", "test-password");
        waitFor([&] { return service.authenticated(); });
        check(!vault.isEmpty() && required == 0 && errors == 0, "login persists the session");
    }
    for (int generation = 2; generation <= 4; ++generation) {
        response = session(generation, now + 3600);
        account::Service service; observe(service);
        service.beginRestore();
        waitFor([&] { return service.authenticated(); });
        check(receivedToken == QString("refresh-%1").arg(generation - 1),
              "each restart uses the latest persisted refresh token");
        check(required == 0 && errors == 0, "restarts never request credentials");
    }
    for (int failure : {408, 429, 500, 502, 503}) {
        status = failure;
        account::Service service; observe(service);
        service.beginRestore();
        waitFor([&] { return service.authenticated(); });
        check(service.snapshot().offline && !vault.isEmpty(), "temporary HTTP errors retain offline access");
        check(required == 0, "temporary failures do not reveal the login form");
    }
    const QByteArray valid = vault;
    vault = QJsonDocument(session(5, now - 60)).toJson(QJsonDocument::Compact);
    const QByteArray expired = vault;
    {
        account::Service service; observe(service);
        service.beginRestore();
        waitFor([&] { return required == 1; });
        check(!service.authenticated() && QJsonDocument::fromJson(vault).object().value("refresh_token") == "refresh-5", "expired offline allowance retains online credentials");
    }
    for (int rejection : {401, 403}) {
        vault = valid; status = rejection;
        response = QJsonObject{{"code", rejection == 401 ? "refresh_token_invalid" : "account_unavailable"}};
        const int before = required;
        account::Service service; observe(service);
        service.beginRestore();
        waitFor([&] { return required > before; });
        check(!service.authenticated() && vault.isEmpty(), "credential rejection clears revoked session");
    }
    status = 200;
    {
        vault = valid;
        writable = false;
        response = session(6, now + 3600);
        account::Service service; observe(service);
        const int beforeErrors = errors;
        service.beginRestore();
        waitFor([&] { return errors > beforeErrors; });
        check(vault == valid && !service.authenticated(), "failed rotation save retains the vault and reports failure");
        const int consumed = refreshes;
        service.beginRestore();
        check(refreshes == consumed, "failed save retry never replays a rotated token");
        writable = true;
        service.beginRestore();
        check(service.authenticated() && refreshes == consumed &&
              QJsonDocument::fromJson(vault).object().value("refresh_token") == "refresh-6",
              "pending rotation is durably saved on retry");
    }
    for (bool denyRead : {false, true}) {
        vault = valid;
        writable = denyRead;
        readable = true;
        response = session(6, now + 3600);
        account::Service service; observe(service);
        const int beforeErrors = errors;
        const int beforeSuccess = successes;
        if (denyRead) readable = false;
        service.login("test@vlt.invalid", "test-password");
        waitFor([&] { return errors > beforeErrors; });
        check(!service.authenticated() && successes == beforeSuccess,
              "failed persistence/readback cannot be hidden by successful authentication");
        writable = readable = true;
        const int beforeRefresh = refreshes;
        service.beginRestore();
        check(service.authenticated() && refreshes == beforeRefresh,
              "retry saves pending credentials without replaying a consumed token");
    }
    {
        vault = valid;
        response = QJsonObject{};
        account::Service service; observe(service);
        service.beginRestore();
        waitFor([&] { return service.authenticated(); });
        check(service.snapshot().offline && QJsonDocument::fromJson(vault).object().value("refresh_token") == QJsonDocument::fromJson(valid).object().value("refresh_token"), "malformed success keeps offline access and saved credentials");
    }

    {
        vault = valid; status = 503;
        QString intent;
        {
            account::Service service; observe(service);
            service.beginRestore();
            waitFor([&] { return service.authenticated(); });
            intent = receivedRequestID;
            check(!intent.isEmpty(), "refresh intent exists before request");
        }
        status = 200; response = session(7, now + 3600);
        account::Service restarted; observe(restarted);
        restarted.beginRestore();
        waitFor([&] { return restarted.authenticated(); });
        check(receivedRequestID == intent, "restart retries the same durable rotation after a lost reply");
        check(!QJsonDocument::fromJson(vault).object().contains("refresh_request_id"), "completed rotation clears its intent");
    }
    {
        auto credentials = QJsonDocument::fromJson(valid).object();
        credentials.remove("refresh_request_id");
        vault = QJsonDocument(credentials).toJson(QJsonDocument::Compact);
        writable = false;
        const int before = refreshes, beforeErrors = errors;
        account::Service service; observe(service);
        service.beginRestore();
        check(refreshes == before && errors > beforeErrors, "unwritable vault cannot consume a refresh token");
        writable = true;
    }
    {
        vault = valid; status = 403; response = QJsonObject{{"code", "proxy_blocked"}};
        const int before = required;
        account::Service service; observe(service);
        service.beginRestore();
        waitFor([&] { return service.authenticated(); });
        check(required == before && !vault.isEmpty(), "proxy 403 is not a server credential revocation");
    }

    {
        vault = valid; readable = false; status = 200; response = session(8, now + 3600);
        const int beforeRequired = required, beforeErrors = errors, beforeRefresh = refreshes;
        account::Service service; observe(service);
        service.beginRestore();
        check(errors > beforeErrors && required == beforeRequired && refreshes == beforeRefresh && vault == valid,
              "locked vault is not mistaken for missing credentials or sent to server");
        service.restoreSavedSession();
        waitFor([&] { return service.authenticated(); });
        check(required == beforeRequired, "explicit vault unlock restores saved sign-in without password login");
    }

    {
        vault = valid; response = session(9, now + 3600); status = 200;
        QLockFile other(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("account-session.lock"));
        check(other.tryLock(0), "simulate another process updating the credential");
        const int before = refreshes;
        account::Service service; observe(service);
        service.beginRestore();
        check(refreshes == before && !service.authenticated(), "concurrent restore waits before reading or sending a credential");
        other.unlock();
        waitFor([&] { return service.authenticated(); });
        check(refreshes == before + 1, "one restore resumes after the vault lock is released");
    }
    {
        vault = valid; response = session(10, now + 3600); status = 200;
        account::Service service; observe(service);
        service.beginRestore();
        service.logout();
        QElapsedTimer timer; timer.start();
        while(timer.elapsed() < 600) { QCoreApplication::processEvents(); QThread::msleep(1); }
        check(!service.authenticated() && vault.isEmpty(), "logout cancels pending restore and cannot sign back in later");
    }
    std::puts("account_restore_test: PASS");
}
