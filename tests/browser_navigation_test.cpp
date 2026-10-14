#include "BrowserBackgroundDialog.hpp"
#include "Typography.hpp"
#include "WebBrowserPanel.hpp"
#include "Theme.hpp"
#include "WebPrefs.hpp"
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QWebEngineDownloadRequest>
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
#include <QShortcut>
#include <QKeyEvent>
#include <QTranslator>
#include <QAbstractButton>
#include <QWebEngineDownloadRequest>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <QStackedWidget>
#include <QTabBar>
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
                        QByteArray body = "<title>Ready</title><h1>Ready</h1><p>alpha alpha alpha</p>",
                                   mime = "text/html", status = "200 OK",
                                   headers;
                        if (path.startsWith("http://proxy-check.invalid/")) {
                            proxyReceived = true;
                            body = "<title>Proxy works</title>";
                        }
                        if(path=="/forum.webm") {QFile f(QStringLiteral(DAW_FORUM_TEST_VIDEO));f.open(QIODevice::ReadOnly);body=f.readAll();mime="video/webm";}
                        if(path=="/forum-player")body="<title>Forum player</title><video id='v' muted autoplay loop playsinline src='/forum.webm'></video>";
                        if (path == "/slow") {
                            slowRequested = true;
                            return;
                        }
                        if (path == "/download") {
                            body = QByteArray(4096, 'a');
                            mime = "application/octet-stream";
                            headers = "Content-Disposition: attachment; filename=browser-fixture.txt\r\n";
                        }
                        if (path == "/download-slow") {
                            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                          "Content-Disposition: attachment; filename=slow-fixture.bin\r\n"
                                          "Content-Length: 1048576\r\n\r\n");
                            auto* timer = new QTimer(socket);
                            timer->setInterval(100);
                            QObject::connect(timer, &QTimer::timeout, socket, [socket] { socket->write(QByteArray(4096, 'a')); });
                            timer->start();
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
    bool quick = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--quick") quick = true;
    qputenv("VLT_GPU_WORKSPACE", quick ? "1" : "0");
    if (quick) {
        qputenv("QT_QUICK_BACKEND", "software");
        QtWebEngineQuick::initialize();
    }
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
        WebBrowserPanel panel(nullptr, quick ? nullptr : &profile);
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
        WebBrowserPanel panel(nullptr, quick ? nullptr : &profile);
        panel.resize(900, 700);
        panel.show();
        auto* stack = panel.findChild<QStackedWidget*>("WebViewStack");
        auto* surface = qobject_cast<ui::graphics::BrowserSurface*>(stack->currentWidget());
        const auto shortcut = [&](const QString& sequence) {
            for (auto* action : panel.findChildren<QShortcut*>()) if (action->key() == QKeySequence(sequence)) {
                QMetaObject::invokeMethod(action, "activated", Qt::DirectConnection);
                return true;
            }
            check(false, "browser keyboard command is registered");
            return false;
        };
        check(surface->page()->isQuick() == quick, "requested browser backend is active");
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
        // Both profiles must still deliver ordinary audio, while allowing a
        // trusted VLT download to be routed through the optional installer hook.
        QByteArray wav;QDataStream audioStream(&wav,QIODevice::WriteOnly);audioStream.setByteOrder(QDataStream::LittleEndian);
        audioStream.writeRawData("RIFF",4);audioStream<<quint32(996);audioStream.writeRawData("WAVEfmt ",8);audioStream<<quint32(16)<<quint16(1)<<quint16(1)<<quint32(48000)<<quint32(96000)<<quint16(2)<<quint16(16);audioStream.writeRawData("data",4);audioStream<<quint32(960);for(int i=0;i<480;++i)audioStream<<qint16(0);
        const auto wavPath=settings.path()+"/browser-audio.wav";{QFile f(wavPath);f.open(QIODevice::WriteOnly);f.write(wav);}
        bool audioReady=false;int downloadHooks=0;
        QObject::connect(&panel,&WebBrowserPanel::audioDownloadReady,&panel,[&](const QString&,const audio::platform::AudioFileInfo&){audioReady=true;});
        panel.contentDownloadCompleted=[&](QWebEngineDownloadRequest*,const QString&){++downloadHooks;return false;};
        panel.downloadAudioForTest(wavPath);
        check(waitFor([&]{return audioReady;}),"audio download still reaches DAW after content hook");
        check(downloadHooks==1,"Widgets/Quick use the same download hook");
        audioReady=false;downloadHooks=0;
        panel.contentDownloadCompleted=[&](QWebEngineDownloadRequest*,const QString&){++downloadHooks;return true;};
        panel.downloadAudioForTest(wavPath);check(waitFor([&]{return downloadHooks==1;}),"installer completion hook receives downloaded file");
        check(!audioReady,"handled content does not import audio into the project");
        panel.contentDownloadCompleted={};
        panel.openUrlForTest(server.url("/forum-player").toString());
        check(waitFor([&]{return js("!!document.getElementById('v') && document.getElementById('v').currentTime>0").toBool();}),"forum WebM plays in this browser mode");
        panel.openUrlForTest(ui::webprefs::kStartUrl);
        if (!check(waitFor([&] { return panel.startPageReadyForTest(); }), "new start page loads offline"))
            return 1;
        const auto document = js("({text:'Notebook',cues:[{seconds:12.25,text:'First cue'}],enabled:true})").toMap();
        check(document.value("text").toString() == "Notebook" && document.value("enabled").toBool() &&
                  document.value("cues").toList().value(0).toMap().value("seconds").toDouble() == 12.25,
              "JavaScript returns structured document data to C++");
        auto replies = std::make_shared<QStringList>();
        for (const auto& value : {QStringLiteral("first"), QStringLiteral("second")})
            surface->page()->runJavaScript("'" + value + "'", [replies, value](const QVariant& result) {
                if (result.toString() == value) replies->append(value);
            });
        check(waitFor([&] { return replies->size() == 2; }), "concurrent scripts deliver results to their own callbacks");
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
            // Chromium rounds CSS pixels after Qt's fractional device scaling.
            // Permit the one-pixel rounding difference, not a stale viewport.
            check(waitFor([&] {
                return qAbs(js("innerWidth").toInt() - surface->width()) <= 1;
            }), "browser viewport follows panel resize");
            check(js("document.documentElement.scrollWidth <= innerWidth").toBool(),
                  "start page fits without horizontal scrolling");
            check(js("[...document.querySelectorAll('.shortcut,.search')].every(e => "
                     "e.getBoundingClientRect().right <= innerWidth && e.getBoundingClientRect().left >= 0)").toBool(),
                  "search and pinned sites fit at every panel width");
            check(panel.width() == width && panel.findChild<QLineEdit*>("WebAddress")->width() >= 50,
                  "native browser chrome respects the narrow panel width");
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
        for (auto* rail : panel.findChildren<QPushButton*>())
            if (rail->property("railRole").toString() == "bookmarks") rail->click();
        panel.findChild<QPushButton*>("WebLibraryAction")->click();
        check(waitFor([&] { return shortcutDialog; }), "bookmarks library opens the native editor");
        dialogDriver.stop();
        check(invalidAddressRejected, "shortcut editor rejects executable addresses");
        check(ui::webprefs::isBookmarked(server.url("/ready").toString()), "shortcut persists as a real bookmark");
        auto* libraryList = panel.findChild<QListWidget*>("WebLibraryList");
        auto* librarySearch = panel.findChild<QLineEdit*>("WebLibrarySearch");
        check(libraryList->count() == 1, "new bookmark appears in the library immediately");
        librarySearch->setText("not-a-bookmark");
        check(libraryList->count() == 0 && panel.findChild<QLabel*>("WebLibraryEmpty")->isVisible(), "bookmark search has an explicit empty state");
        librarySearch->setText("LOCAL");
        check(libraryList->count() == 1, "bookmark search matches titles case-insensitively");
        panel.findChild<QAbstractButton*>("WebLibraryClose")->click();
        check(waitFor([&] { return panel.startPageReadyForTest(); }), "start page refreshes after saving shortcut");
        check(js("document.querySelectorAll('.shortcut').length === 4 && "
                 "document.querySelector('.shortcut .name').textContent === 'YouTube'").toBool(),
              "saved bookmarks do not replace the four pinned sites");
        check(js("document.querySelector('.saved-link strong').textContent === 'Local & <sample>'").toBool(),
              "start page displays saved bookmarks with escaped titles");
        const QString darkBackground = th().background.name();
        check(js("getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()").toString() == darkBackground,
              "start page uses the application palette");
        ThemeManager::instance().setThemeId("light", false);
        check(waitFor([&] { return panel.startPageReadyForTest() && js("getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()").toString() == th().background.name(); }),
              "start page updates when the application theme changes");
        check(panel.palette().color(QPalette::Window) == th().background, "native browser chrome follows the light theme");
        ThemeManager::instance().setThemeId("dark", false);
        check(waitFor([&] { return panel.startPageReadyForTest() && js("getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()").toString() == th().background.name(); }), "start page reloads after restoring the dark theme");
        bool searchSubmitted = false;
        const auto navigationPolicy = surface->page()->navigationPolicy;
        surface->page()->navigationPolicy = [&](const QUrl& url, bool) {
            searchSubmitted |= url.host() == "duckduckgo.com" && url.query().contains("q=ambient");
            return false; // Inspect the real form submission without reaching the internet.
        };
        js("document.getElementById('web-query').value='ambient';document.querySelector('.search').requestSubmit()");
        check(waitFor([&] { return searchSubmitted; }), "search button submits the query to the existing search engine");
        surface->page()->navigationPolicy = navigationPolicy;
        panel.openUrlForTest(server.url("/ready").toString());
        auto* view = surface;
        check(waitFor([&] { return view->title() == "Ready"; }),
              "ordinary page loads");
        shortcut("Ctrl+F");
        auto* findText = panel.findChild<QLineEdit*>("WebFindText");
        auto* findResult = panel.findChild<QLabel*>("WebFindResult");
        findText->setText("alpha");
        if (!check(waitFor([&] { return findResult->text() == "1 of 3"; }), "find in page reports actual Chromium match counts"))
            std::fprintf(stderr, "find result: %s\n", findResult->text().toUtf8().constData());
        shortcut("F3");
        check(waitFor([&] { return findResult->text() == "2 of 3"; }), "next match advances the find count");
        shortcut("Shift+F3");
        check(waitFor([&] { return findResult->text() == "1 of 3"; }), "previous match reverses the find count");
        findText->setText("no-such-word");
        check(waitFor([&] { return findResult->text() == "No matches"; }), "find reports a query with no matches");
        shortcut("Escape");
        shortcut("Ctrl+=");
        check(qAbs(view->zoomFactor() - 1.1) < .001 && panel.findChild<QPushButton*>("WebZoom")->isVisible(), "zoom has a visible per-tab reset control");
        panel.findChild<QPushButton*>("WebZoom")->click();
        check(qFuzzyCompare(view->zoomFactor(), 1.0), "zoom resets to actual size");
        shortcut("Ctrl+H");
        librarySearch->setText("127.0.0.1");
        check(libraryList->count() >= 1, "history can be searched by host");
        shortcut("Escape");
        auto* tabBar = panel.findChild<QTabBar*>("WebTabBar");
        QTimer::singleShot(0, &panel, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            auto* mute = menu->findChild<QAction*>("WebMuteTab");
            menu->close();
            if (mute) mute->trigger();
        });
        tabBar->customContextMenuRequested(tabBar->tabRect(tabBar->currentIndex()).center());
        check(js("true").toBool() && (quick ? surface->page()->quickItem()->property("audioMuted").toBool()
                                          : surface->page()->legacy()->isAudioMuted()), "mute tab reaches the active browser backend");
        shortcut("Ctrl+Shift+A");
        librarySearch->setText("Muted");
        check(libraryList->count() == 1, "tab search exposes muted state");
        QMetaObject::invokeMethod(librarySearch, "returnPressed", Qt::DirectConnection);
        check(!panel.findChild<QWidget*>("WebLibrary")->isVisible() && stack->currentWidget() == surface, "tab search activates its result and returns to the page");
        auto* addressEdit = panel.findChild<QLineEdit*>("WebAddress");
        addressEdit->setText(server.url("/ready").toString());
        QKeyEvent newTabKey(QEvent::KeyPress, Qt::Key_Return, Qt::AltModifier);
        QApplication::sendEvent(addressEdit, &newTabKey);
        check(panel.tabCountForTest() == 3, "Alt+Enter opens the typed address in a new tab");
        panel.closeCurrentTabForTest();

        surface->page()->download(server.url("/download"), "browser-fixture.txt");
        check(waitFor([&] { return QFileInfo::exists(QDir(ui::webprefs::downloadDirectory()).filePath("browser-fixture.txt")); }), "genuine browser download writes to isolated Downloads");
        shortcut("Ctrl+J");
        check(waitFor([&] { return libraryList->count() >= 1 && libraryList->item(0)->data(Qt::UserRole + 1).toString().contains("Completed"); }),
              "download library retains completed files after the request is destroyed");
        const int completedDownloads = libraryList->count();
        shortcut("Escape");
        surface->page()->download(server.url("/download-slow"), "slow-fixture.bin");
        shortcut("Ctrl+J");
        check(waitFor([&] { return libraryList->count() == completedDownloads + 1; }), "concurrent download enters the download library");
        const auto downloadAction = [&](const QString& name) {
            QTimer::singleShot(0, &panel, [name] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) return;
                auto* action = menu->findChild<QAction*>(name);
                menu->close();
                if (action) action->trigger();
            });
            libraryList->setCurrentRow(0);
            libraryList->customContextMenuRequested(libraryList->visualItemRect(libraryList->item(0)).center());
        };
        if (libraryList->count() == completedDownloads + 1) {
            downloadAction("WebPauseDownload");
            check(waitFor([&] { return libraryList->item(0)->data(Qt::UserRole + 1).toString().contains("Paused"); }), "download can be paused from its library row");
            downloadAction("WebPauseDownload");
            check(waitFor([&] { return libraryList->item(0)->data(Qt::UserRole + 1).toString().contains("Downloading"); }), "paused download resumes from the same row");
            downloadAction("WebCancelDownload");
            check(waitFor([&] { return libraryList->item(0)->data(Qt::UserRole + 1).toString() == "Cancelled"; }), "cancelled download remains visible with its result");
        }
        shortcut("Escape");
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
                  return panel.tabCountForTest() == 3 &&
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
    // Upgrade an existing session, then reopen the saved session: the built-in
    // site occupies one stable tab without duplicating it or losing selection.
    ui::webprefs::setSessionTabs({server.url("/ready").toString(), ui::webprefs::kStartUrl});
    ui::webprefs::setSessionActiveTab(0);
    for (int launch = 0; launch < 2; ++launch) {
        WebBrowserPanel panel(nullptr, quick ? nullptr : &profile);
        panel.show();
        auto* tabs = panel.findChild<QTabBar*>("WebTabBar");
        auto* stack = panel.findChild<QStackedWidget*>("WebViewStack");
        check(tabs->count() == 3 && tabs->tabText(0) == "VLT Studio" &&
                  tabs->tabToolTip(0) == ui::webprefs::kStudioUrl && !tabs->tabIcon(0).isNull(),
              "VLT Studio opens once as the first tab alongside the saved session");
        check(tabs->currentIndex() == 1, "session restoration preserves the selected ordinary tab");
        check(!tabs->tabButton(0, QTabBar::RightSide), "pinned site has no close button");
        tabs->setCurrentIndex(0);
        panel.closeCurrentTabForTest();
        check(tabs->count() == 3, "close shortcut cannot remove the pinned site");
        tabs->moveTab(0, 2);
        tabs->moveTab(2, 0);
        check(tabs->tabText(0) == "VLT Studio" && tabs->currentIndex() == 0 &&
                  stack->currentWidget() == stack->widget(0),
              "tab reordering keeps the pinned site and selection at the front");
        panel.openUrlForTest(ui::webprefs::kStartUrl);
        check(tabs->count() == 4 && tabs->currentIndex() == 3 && tabs->tabText(0) == "VLT Studio",
              "Home opens an ordinary tab without replacing the pinned site");
        panel.closeCurrentTabForTest();
        tabs->setCurrentIndex(0);
        panel.openUrlForTest(server.url("/ready").toString());
        check(tabs->count() == 4 && panel.findChild<QLineEdit*>("WebAddress")->text() == server.url("/ready").toDisplayString(),
              "address navigation from the pinned site updates the new tab's address immediately");
        panel.closeCurrentTabForTest();
        tabs->setCurrentIndex(1);
        check(waitFor([&] { return ui::webprefs::sessionTabs().size() == 3 &&
                                  ui::webprefs::sessionTabs().first() == ui::webprefs::kStudioUrl &&
                                  ui::webprefs::sessionActiveTab() == 1; }),
              "session saves one pinned site and the selected ordinary tab");
    }
    // Exercise a close immediately after a tab edit, before the save debounce.
    {
        WebBrowserPanel panel(nullptr, quick ? nullptr : &profile);
        panel.openTabForTest(ui::webprefs::kStartUrl);
        check(panel.tabCountForTest() == 4, "new tab added before immediate shutdown");
    }
    check(ui::webprefs::sessionTabs().size() == 4 && ui::webprefs::sessionActiveTab() == 3,
          "shutdown flushes pending session changes");

    if (app.arguments().contains("--capture")) {
        const QString folder = qEnvironmentVariable("VLT_BROWSER_CAPTURE_DIR", QDir::tempPath());
        QTranslator translator;
        const bool russian = translator.load(qEnvironmentVariable("VLT_BROWSER_RU_QM"));
        if (russian) app.installTranslator(&translator);
        ui::webprefs::setSessionTabs({ui::webprefs::kStartUrl});
        ui::webprefs::setSessionActiveTab(0);
        ui::webprefs::addBookmark("Freesound · Field recordings", "https://freesound.org/");
        ui::webprefs::addBookmark("Bandcamp · New releases", "https://bandcamp.com/");
        ui::webprefs::addHistoryEntry("Mixing reference", "https://example.com/reference");
        for (const QString& theme : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("studio-gray")}) {
            ThemeManager::instance().setThemeId(theme, false);
            WebBrowserPanel panel(nullptr, quick ? nullptr : &profile);
            panel.show();
            panel.openUrlForTest(ui::webprefs::kStartUrl);
            check(waitFor([&] { return panel.startPageReadyForTest(); }), "localized themed start page ready");
            for (const int width : {900, 520, 320}) {
                panel.resize(width, 820);
                QElapsedTimer paint;
                paint.start();
                waitFor([&] { return paint.elapsed() > 200; });
                check(panel.width() == width, "localized chrome fits each supported width");
                const QString prefix = QString("%1-%2-%3").arg(theme, russian ? "ru" : "en").arg(width);
                check(panel.grab().save(QDir(folder).filePath(prefix + "-start.png")), "themed start screenshot saved");
                for (auto* rail : panel.findChildren<QPushButton*>())
                    if (rail->property("railRole").toString() == "bookmarks") rail->click();
                QApplication::processEvents();
                check(panel.grab().save(QDir(folder).filePath(prefix + "-bookmarks.png")), "themed library screenshot saved");
                panel.findChild<QAbstractButton*>("WebLibraryClose")->click();
                panel.findChild<QAbstractButton*>("WebTabSearch")->click();
                QApplication::processEvents();
                check(panel.width() == width, "localized tab library does not expand the panel");
                check(panel.grab().save(QDir(folder).filePath(prefix + "-tabs.png")), "themed tabs screenshot saved");
                panel.findChild<QAbstractButton*>("WebLibraryClose")->click();
            }
        }
        if (russian) app.removeTranslator(&translator);
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
