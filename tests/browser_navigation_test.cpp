#include "BrowserBackgroundDialog.hpp"
#include "Typography.hpp"
#include "WebBrowserPanel.hpp"
#include "WebPrefs.hpp"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <cstdio>
#include <functional>
#include <memory>

namespace {
bool waitFor(const std::function<bool()>& predicate, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate())
            return true;
        QThread::msleep(5);
    } while (timer.elapsed() < timeout);
    return false;
}
class Server : public QTcpServer {
  public:
    QByteArray png;
    bool slowRequested = false, postReceived = false, badImage = false,
         proxyReceived = false;
    int imageRequests = 0;
    Server() {
        QImage image(320, 180, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QBuffer output(&png);
        output.open(QIODevice::WriteOnly);
        image.save(&output, "PNG");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket,
                        &QObject::deleteLater);
                connect(
                    socket, &QTcpSocket::readyRead, socket,
                    [this, socket, input] {
                        input->append(socket->readAll());
                        if (!input->contains("\r\n\r\n"))
                            return;
                        const auto path = input->split(' ').value(1);
                        QByteArray body = "<title>Ready</title><h1>Ready</h1>",
                                   mime = "text/html", status = "200 OK",
                                   headers;
                        if (path.startsWith("http://proxy-check.invalid/")) {
                            proxyReceived = true;
                            body = "<title>Proxy works</title>";
                        }
                        if (path == "/slow") {
                            slowRequested = true;
                            return;
                        }
                        if (path == "/redirect") {
                            status = "302 Found";
                            headers = "Location: /ready\r\n";
                        }
                        if (path == "/popup")
                            body =
                                "<title>Popup host</title><script>function "
                                "blank(){const "
                                "w=window.open('');w.document.write('<title>"
                                "Blank "
                                "popup</title>');w.document.close();}function "
                                "post(){document.forms[0].submit();}</"
                                "script><form "
                                "action='/posted' method='post' "
                                "target='_blank'><input "
                                "name='token' value='keep-me'></form>";
                        if (path == "/posted") {
                            if (!input->contains("token=keep-me"))
                                return;
                            postReceived = input->startsWith("POST ");
                            body = "<title>POST preserved</title>";
                        }
                        if (path == "/v1/browser-backgrounds") {
                            const QString hash = QString::fromLatin1(
                                QCryptographicHash::hash(
                                    png, QCryptographicHash::Sha256)
                                    .toHex());
                            body = QJsonDocument(
                                       QJsonObject{
                                           {"backgrounds",
                                            QJsonArray{QJsonObject{
                                                {"id", "00000000-0000-4000-"
                                                       "8000-000000000123"},
                                                {"title", "Test mountains"},
                                                {"mime_type", "image/png"},
                                                {"sha256", hash}}}}})
                                       .toJson();
                            mime = "application/json";
                        } else if (path.endsWith("/thumbnail")) {
                            body = png;
                            mime = "image/png";
                        } else if (path.endsWith("/image")) {
                            ++imageRequests;
                            body = badImage ? QByteArray("corrupt image") : png;
                            mime = "image/png";
                        }
                        socket->write("HTTP/1.1 " + status +
                                      "\r\nContent-Type: " + mime +
                                      "\r\nContent-Length: " +
                                      QByteArray::number(body.size()) +
                                      "\r\nConnection: close\r\n" + headers +
                                      "\r\n" + body);
                        socket->disconnectFromHost();
                    });
            }
        });
    }
    QUrl url(const QString& path) const {
        return QUrl(
            QString("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }
};
QPushButton* button(QWidget& owner, const QString& text) {
    for (auto* b : owner.findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}
} // namespace
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS"))
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                "--disable-gpu --disable-features=WebGPU");
    ui::registerFontUrlScheme();
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VLTBrowserRegression");
    QCoreApplication::setApplicationName("BrowserRegression-" +
                                         settings.path().section('/', -1));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings.path());
    QStandardPaths::setTestModeEnabled(true);
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok)
            ++failures;
        return ok;
    };
    Server server;
    if (!check(server.listen(QHostAddress::LocalHost), "local fixture server"))
        return 1;
    if (app.arguments().contains("--proxy")) {
        // A local forward proxy is sufficient to resolve this reserved domain;
        // without the proxy Chromium cannot reach it. This changes only this
        // process.
        QNetworkProxy::setApplicationProxy(QNetworkProxy(
            QNetworkProxy::HttpProxy, "127.0.0.1", server.serverPort()));
        QWebEngineProfile profile;
        WebBrowserPanel panel(nullptr, &profile);
        panel.show();
        panel.openUrlForTest("http://proxy-check.invalid/ready");
        const bool ok =
            check(waitFor([&] {
                      return server.proxyReceived &&
                             panel.tabTitlesForTest().contains("Proxy works");
                  }),
                  "Chromium uses the configured local forward proxy");
        return ok ? 0 : 1;
    }
    QWebEngineProfile profile;
    {
        WebBrowserPanel panel(nullptr, &profile);
        panel.resize(900, 700);
        panel.show();
        panel.openUrlForTest(server.url("/ready").toString());
        auto* view = panel.findChild<QWebEngineView*>();
        check(waitFor([&] { return view->title() == "Ready"; }),
              "ordinary page loads");
        panel.openUrlForTest(server.url("/slow").toString());
        check(waitFor([&] { return server.slowRequested; }),
              "slow navigation begins");
        panel.openUrlForTest(server.url("/redirect").toString());
        check(waitFor([&] {
                  return view->url() == server.url("/ready") &&
                         view->title() == "Ready";
              }),
              "cancelled navigation cannot replace successor or redirect");
        panel.openUrlForTest(server.url("/popup").toString());
        check(waitFor([&] { return view->title() == "Popup host"; }),
              "popup host loads");
        view->page()->runJavaScript("blank()");
        check(waitFor([&] {
                  return panel.tabCountForTest() == 2 &&
                         panel.tabTitlesForTest().contains("Blank popup");
              }),
              "window.open empty URL preserves writable window context");
        panel.closeCurrentTabForTest();
        view->page()->runJavaScript("post()");
        check(waitFor([&] {
                  return server.postReceived &&
                         panel.tabTitlesForTest().contains("POST preserved");
              }),
              "target blank preserves POST body");
        panel.closeCurrentTabForTest();
        QTcpServer unused;
        unused.listen(QHostAddress::LocalHost);
        const auto failedPort = unused.serverPort();
        unused.close();
        bool failed = false;
        QObject::connect(&panel, &WebBrowserPanel::statusMessage, &panel,
                         [&](const QString& text) {
                             if (text.contains("Could not load"))
                                 failed = true;
                         });
        panel.openUrlForTest(
            QString("http://127.0.0.1:%1/unavailable").arg(failedPort));
        check(
            waitFor([&] { return failed && view->url().port() == failedPort; }),
            "real network failure keeps failed URL");
        panel.openUrlForTest(server.url("/ready").toString());
        check(waitFor([&] { return view->title() == "Ready"; }),
              "navigation recovers after genuine failure");
        for (int i = 0; i < 8; ++i) {
            panel.openTabForTest(server.url("/slow").toString());
            panel.closeCurrentTabForTest();
        }
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    {
        BrowserBackgroundDialog dialog(server.url("/v1"));
        dialog.show();
        auto* list = dialog.findChild<QListWidget*>();
        check(waitFor([&] {
                  return list->count() == 1 && !list->item(0)->icon().isNull();
              }),
              "catalog and thumbnail load");
        if (app.arguments().contains("--capture"))
            dialog.grab().save(
                QDir::temp().filePath("vlt-browser-background-dialog.png"));
        list->setCurrentRow(0);
        server.badImage = true;
        button(dialog, "Use background")->click();
        check(waitFor([&] {
                  return server.imageRequests == 1 && list->isEnabled();
              }),
              "invalid download returns control");
        check(dialog.selectedPath().isEmpty() &&
                  dialog.result() != QDialog::Accepted,
              "corrupt background never replaces selection");
        server.badImage = false;
        button(dialog, "Use background")->click();
        check(waitFor([&] { return dialog.result() == QDialog::Accepted; }),
              "valid full background downloads and applies");
        check(QFile::exists(dialog.selectedPath()),
              "selected background cached locally");
        const auto origin = server.url("/v1");
        server.close();
        BrowserBackgroundDialog offline(origin);
        offline.show();
        auto* cached = offline.findChild<QListWidget*>();
        check(cached->count() == 1, "offline catalog restored");
        cached->setCurrentRow(0);
        button(offline, "Use background")->click();
        check(offline.result() == QDialog::Accepted &&
                  offline.selectedPath() == dialog.selectedPath(),
              "cached selection works offline without downloading");
    }
    QDir(ui::webprefs::profileStoragePath()).removeRecursively();
    std::fprintf(stderr, "browser regression failures: %d\n", failures);
    return failures ? 1 : 0;
}
