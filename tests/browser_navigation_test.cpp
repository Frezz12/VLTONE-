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
#include <QLineEdit>
#include <QMenu>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
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
    ui::initializeApplicationFonts();
    QTemporaryDir settings;
    app.setProperty("dawHeadlessDataRoot", settings.path());
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
        auto* surface = panel.findChild<ui::graphics::BrowserSurface*>();
        auto js = [&](const QString& source) {
            auto result = std::make_shared<QVariant>();
            auto done = std::make_shared<bool>(false);
            surface->page()->runJavaScript(source, [result, done](const QVariant& value) {
                *result = value;
                *done = true;
            });
            check(waitFor([&] { return *done; }), "start page JavaScript callback");
            return *result;
        };
        panel.openUrlForTest(ui::webprefs::kStartUrl);
        if (!check(waitFor([&] { return panel.startPageReadyForTest(); }), "new start page loads offline"))
            return 1;
        check(js("[...document.querySelectorAll('.shortcut .name')].map(e => e.textContent).join(',')")
                  .toString() == "YouTube,SoundCloud,Splice,Spotify",
              "start page contains exactly the four requested pinned sites");
        check(js("document.querySelectorAll('.discover,.categories,.story').length").toInt() == 0,
              "discovery block and sample recommendations removed");
        check(js("document.querySelector('img.brand').naturalWidth === 112").toBool(),
              "bundled VLTONE logo loads offline");
        for (auto* rail : panel.findChildren<QPushButton*>())
            check(rail->property("railRole").toString() != "samples", "sample sidebar button removed");
        for (const int width : {900, 520, 320}) {
            panel.resize(width, 760);
            check(waitFor([&] {
                return js("innerWidth").toInt() == surface->width();
            }), "browser viewport follows panel resize");
            check(js("document.documentElement.scrollWidth <= innerWidth").toBool(),
                  "start page fits without horizontal scrolling");
            check(js("[...document.querySelectorAll('.shortcut,.search')].every(e => "
                     "e.getBoundingClientRect().right <= innerWidth && e.getBoundingClientRect().left >= 0)").toBool(),
                  "search and pinned sites fit at every panel width");
            if (app.arguments().contains("--capture")) {
                const auto folder = qEnvironmentVariable("VLT_BROWSER_CAPTURE_DIR", QDir::tempPath());
                QDir().mkpath(folder);
                QElapsedTimer paint;
                paint.start();
                waitFor([&] { return paint.elapsed() >= 250; });
                check(panel.grab().save(QDir(folder).filePath(QString("browser-%1.png").arg(width))),
                      "browser screenshot saved");
            }
        }
        panel.resize(900, 700);
        bool settingsRequested = false;
        QObject::connect(&panel, &WebBrowserPanel::settingsRequested, &panel,
                         [&] { settingsRequested = true; });
        for (auto* rail : panel.findChildren<QPushButton*>())
            if (rail->property("railRole").toString() == "settings") rail->click();
        check(settingsRequested, "sidebar settings button invokes the existing settings flow");

        bool shortcutDialog = false;
        bool invalidAddressRejected = false;
        QTimer dialogDriver;
        QObject::connect(&dialogDriver, &QTimer::timeout, &panel, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || !dialog->findChild<QLineEdit*>("WebShortcutAddress")) return;
            dialogDriver.stop();
            shortcutDialog = true;
            auto* address = dialog->findChild<QLineEdit*>("WebShortcutAddress");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            address->setText("javascript:alert(1)");
            buttons->button(QDialogButtonBox::Save)->click();
            invalidAddressRejected = dialog->isVisible() && ui::webprefs::bookmarks().isEmpty();
            dialog->findChild<QLineEdit*>("WebShortcutTitle")->setText("Local & <sample>");
            address->setText(server.url("/ready").toString());
            buttons->button(QDialogButtonBox::Save)->click();
        });
        dialogDriver.start(25);
        QTimer::singleShot(0, &panel, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            auto* add = menu->findChild<QAction*>("WebAddBookmark");
            menu->close();
            if (add) add->trigger();
        });
        for (auto* rail : panel.findChildren<QPushButton*>())
            if (rail->property("railRole").toString() == "bookmarks") rail->click();
        check(waitFor([&] { return shortcutDialog; }), "bookmarks menu opens the native editor");
        dialogDriver.stop();
        check(invalidAddressRejected, "shortcut editor rejects executable addresses");
        check(ui::webprefs::isBookmarked(server.url("/ready").toString()), "shortcut persists as a real bookmark");
        check(waitFor([&] { return panel.startPageReadyForTest(); }), "start page refreshes after saving shortcut");
        check(js("document.querySelectorAll('.shortcut').length === 4 && "
                 "document.querySelector('.shortcut .name').textContent === 'YouTube'").toBool(),
              "saved bookmarks do not replace the four pinned sites");
        bool searchSubmitted = false;
        const auto navigationPolicy = surface->page()->navigationPolicy;
        surface->page()->navigationPolicy = [&](const QUrl& url, bool) {
            searchSubmitted = url.host() == "duckduckgo.com" && url.query().contains("q=ambient");
            return false; // Inspect the real form submission without reaching the internet.
        };
        js("document.getElementById('web-query').value='ambient';document.querySelector('.search').requestSubmit()");
        check(waitFor([&] { return searchSubmitted; }), "search button submits the query to the existing search engine");
        surface->page()->navigationPolicy = navigationPolicy;
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
        check(waitFor([&] {
                  return js("document.readyState === 'complete' && document.forms.length === 1").toBool();
              }), "popup fixture form is ready before submitting");
        view->page()->runJavaScript("blank()");
        check(waitFor([&] {
                  return panel.tabCountForTest() == 2 &&
                         panel.tabTitlesForTest().contains("Blank popup");
              }),
              "window.open empty URL preserves writable window context");
        panel.closeCurrentTabForTest();
        view->page()->runJavaScript("post()");
        const bool posted = check(waitFor([&] {
                  return server.postReceived &&
                         panel.tabTitlesForTest().contains("POST preserved");
              }),
              "target blank preserves POST body");
        if (!posted)
            std::fprintf(stderr, "POST received=%d; tabs=%s\n", server.postReceived,
                         panel.tabTitlesForTest().join(" | ").toUtf8().constData());
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
