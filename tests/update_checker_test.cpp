#include "UpdateChecker.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
const QByteArray installerBody(300'000, 'V'); // Spans several network/file chunks.

QJsonObject artifact(const QString& platform) {
    const QString kind = platform == QStringLiteral("windows")
        ? QStringLiteral("windows-exe") : QStringLiteral("macos-dmg");
    return {{"kind", kind}, {"platform", platform}, {"bytes", installerBody.size()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(installerBody, QCryptographicHash::Sha256).toHex())},
            {"file_name", "../../untrusted.exe"},
            {"download_url", "/v1/releases/0.2.0%20Build%201/download/" + kind}};
}

QString nativePlatform() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#else
    return QStringLiteral("macos");
#endif
}

class ClosingWindow : public QWidget {
public:
    bool allowClose = false;
    int closeRequests = 0;
    void closeEvent(QCloseEvent* event) override {
        ++closeRequests;
        event->setAccepted(allowClose);
    }
};

// Real local HTTP exercises reply streaming and cancellation without fetching
// or executing an actual installer, touching Downloads, or changing settings.
class ReleaseServer : public QTcpServer {
public:
    QJsonObject release{{"artifacts", QJsonArray{artifact(nativePlatform())}}};
    QByteArray body = installerBody;
    int status = 200;
    bool cancelMetadata = false;
    bool cancelDownload = false;
    bool truncated = false;
    bool redirect = false;
    QList<QByteArray> requests;

    ReleaseServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    const QByteArray path = request.split(' ').value(1);
                    requests.append(path);
                    const bool download = path.contains("/download/");
                    if ((download && cancelDownload) || (!download && cancelMetadata)) {
                        // Leave a request in flight while the user cancels it.
                        QTimer::singleShot(0, socket, [] {
                            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                                dialog->reject();
                        });
                        return;
                    }
                    if (download && redirect && !path.endsWith("/redirected")) {
                        socket->write("HTTP/1.1 302 Found\r\nLocation: /api/v1/download/redirected\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    const QByteArray payload = download ? body : QJsonDocument(release).toJson(QJsonDocument::Compact);
                    const auto size = payload.size() + (download && truncated ? 10 : 0);
                    socket->write("HTTP/1.1 " + QByteArray::number(download ? status : 200) +
                                  " Response\r\nContent-Length: " + QByteArray::number(size) +
                                  "\r\nConnection: close\r\n\r\n" + payload);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QUrl releaseUrl() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/releases/0.2.0%20Build%201").arg(serverPort()));
    }
};
}

class UpdateCheckerTest : public QObject {
    Q_OBJECT
    QUrl openedUrl;
    QWidget* observedWindow = nullptr;
    bool wasClosedBeforeLaunch = false;

private slots:
    void captureUrl(const QUrl& url) {
        openedUrl = url;
        wasClosedBeforeLaunch = observedWindow && !observedWindow->isVisible();
    }

    void initTestCase() { QApplication::setQuitOnLastWindowClosed(false); }

    void cleanup() {
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        openedUrl = QUrl();
        observedWindow = nullptr;
    }

    void platformAndUrlSelection() {
        const QUrl releaseUrl(QStringLiteral("https://example.invalid/api/v1/releases/0.2.0%20Build%201"));
        QJsonObject release{{"artifacts", QJsonArray{artifact("macos"), artifact("windows")}}};
        for (const auto& platform : {QStringLiteral("windows"), QStringLiteral("macos")}) {
            const auto selected = UpdateChecker::installerForRelease(release, releaseUrl, platform);
            QVERIFY(!selected.url.isEmpty());
            QCOMPARE(selected.url.toEncoded(), QByteArray("https://example.invalid/api/v1/releases/0.2.0%20Build%201/download/") +
                (platform == "windows" ? "windows-exe" : "macos-dmg"));
            QCOMPARE(selected.fileName, platform == "windows" ? "VLTONE-Setup.exe" : "VLTONE.dmg");
            QCOMPARE(selected.sha256.size(), 32);
        }
        QVERIFY(UpdateChecker::installerForRelease(release, releaseUrl, "linux").url.isEmpty());
        release["artifacts"] = QJsonArray{artifact("macos")};
        QVERIFY(UpdateChecker::installerForRelease(release, releaseUrl, "windows").url.isEmpty());
        QVERIFY(UpdateChecker::isNewerVersionForTest("0.10.0", "0.9.9"));
        QVERIFY(!UpdateChecker::isNewerVersionForTest("0.2-beta", "0.1.0"));
        QVERIFY(!UpdateChecker::isNewerVersionForTest("0.1.0", "0.1.0"));
        QVERIFY(UpdateChecker::isNewerVersionForTest("0.2.4", "0.2.3-Build-6"));
        QVERIFY(UpdateChecker::isNewerVersionForTest("0.2.4", "0.2.3 Build 6"));
        QVERIFY(!UpdateChecker::isNewerVersionForTest("0.2.3", "0.2.3-Build-6"));
    }

    void rejectUnsafeMetadata() {
        const QUrl base(QStringLiteral("https://example.invalid/v1/releases/0.2.0"));
        for (const QString& unsafe : {QStringLiteral("file:///tmp/setup.exe"), QStringLiteral("http://example.invalid/setup.exe"),
                                     QStringLiteral("https://user:pass@example.invalid/setup.exe"), QString()}) {
            auto item = artifact("windows");
            item["download_url"] = unsafe;
            QVERIFY(UpdateChecker::installerForRelease({{"artifacts", QJsonArray{item}}}, base, "windows").url.isEmpty());
        }
        for (const auto& field : {QStringLiteral("sha256"), QStringLiteral("bytes"), QStringLiteral("platform")}) {
            auto item = artifact("windows");
            item.remove(field);
            QVERIFY(UpdateChecker::installerForRelease({{"artifacts", QJsonArray{item}}}, base, "windows").url.isEmpty());
        }
        auto item = artifact("windows");
        item["bytes"] = 3LL * 1024 * 1024 * 1024;
        QVERIFY(UpdateChecker::installerForRelease({{"artifacts", QJsonArray{item}}}, base, "windows").url.isEmpty());
    }

    void download_data() {
        QTest::addColumn<QString>("scenario");
        for (const char* scenario : {"success", "redirect", "bad-hash", "short", "oversized", "http-error", "truncated",
                                     "cancel-metadata", "cancel-download", "missing-installer", "disk-error"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void download() {
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QSKIP("Installer downloads are supported on Windows and macOS.");
#endif
        QFETCH(QString, scenario);
        ReleaseServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir downloads;
        QVERIFY(downloads.isValid());
        QString destination = downloads.path();
        if (scenario == "bad-hash") server.body[0] = 'X';
        if (scenario == "short") server.body.chop(1);
        if (scenario == "oversized") server.body.append('X');
        if (scenario == "http-error") server.status = 503;
        server.truncated = scenario == "truncated";
        server.redirect = scenario == "redirect";
        server.cancelMetadata = scenario == "cancel-metadata";
        server.cancelDownload = scenario == "cancel-download";
        if (scenario == "missing-installer") server.release = {};
        if (scenario == "disk-error") {
            destination = downloads.filePath("regular-file");
            QFile blocked(destination);
            QVERIFY(blocked.open(QIODevice::WriteOnly));
        }
        QWidget owner;
        owner.show();
        UpdateChecker checker(nullptr);
        QString error;
        const QString path = checker.downloadInstaller(&owner, server.releaseUrl(), destination, &error);
        QVERIFY(owner.isVisible());
        if (scenario == "success" || scenario == "redirect") {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY(path.startsWith(downloads.path() + '/'));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), installerBody);
            QCOMPARE(server.requests.first(), QByteArray("/api/v1/releases/0.2.0%20Build%201"));
            QVERIFY(server.requests.at(1).startsWith("/api/v1/releases/0.2.0%20Build%201/download/"));
        } else {
            QVERIFY(path.isEmpty());
            QCOMPARE(error.isEmpty(), scenario.startsWith("cancel-"));
            // Aborted downloads leave neither an executable nor a partial file.
            QCOMPARE(QDir(downloads.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        }
    }

    void closingCanBeCancelled() {
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "captureUrl");
        ClosingWindow owner;
        observedWindow = &owner;
        owner.show();
        QString error;
        QVERIFY(!UpdateChecker::closeAndLaunchInstaller(&owner, "unused.exe", &error));
        QVERIFY(error.isEmpty());
        QVERIFY(owner.isVisible());
        QVERIFY(openedUrl.isEmpty());
        QCOMPARE(owner.closeRequests, 1);

        owner.allowClose = true;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("setup with spaces.exe"));
        QVERIFY(UpdateChecker::closeAndLaunchInstaller(&owner, path, &error));
        QCOMPARE(openedUrl, QUrl::fromLocalFile(path));
        QVERIFY(wasClosedBeforeLaunch);
        QVERIFY(!QApplication::quitOnLastWindowClosed());
    }

    void browserAndLater() {
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "captureUrl");
        QWidget owner;
        owner.show();
        UpdateChecker checker(nullptr);
        const QUrl page(QStringLiteral("https://example.invalid/ru/releases/0.2.0"));
        for (const QString& button : {QStringLiteral("Later"), QStringLiteral("Open in browser")}) {
            QTimer::singleShot(0, this, [button] {
                auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                QVERIFY(prompt);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
                QCOMPARE(prompt->buttons().size(), 3);
#else
                QCOMPARE(prompt->buttons().size(), 2);
#endif
                for (auto* candidate : prompt->buttons())
                    if (candidate->text() == button) candidate->click();
            });
            checker.showUpdate(&owner, "0.2.0 Build 1", page, {});
            QVERIFY(owner.isVisible());
            QCOMPARE(openedUrl, button == "Later" ? QUrl() : page);
        }
    }
};

QTEST_MAIN(UpdateCheckerTest)
#include "update_checker_test.moc"
