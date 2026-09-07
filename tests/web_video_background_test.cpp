#include "WebVideoBackground.hpp"
#include "ThemeMediaBackground.hpp"
#include "TimelineBackgroundPrefs.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}
bool waitFor(const std::function<bool()>& condition, int timeout = 10000) {
    if (condition()) return true;
    QEventLoop loop;
    QTimer check, deadline;
    QObject::connect(&check, &QTimer::timeout, &loop, [&] { if (condition()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    check.start(10); deadline.setSingleShot(true); deadline.start(timeout);
    loop.exec();
    return condition();
}
void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
QVariant js(QWebEnginePage* page, const QString& script) {
    struct Result { bool done = false; QVariant value; };
    auto result = std::make_shared<Result>();
    page->runJavaScript(script, QWebEngineScript::ApplicationWorld, [result](const QVariant& value) {
        result->value = value; result->done = true;
    });
    if (!waitFor([&] { return result->done; })) throw std::runtime_error("JavaScript timed out");
    return result->value;
}
QList<ui::WebVideoSource> discover(QWebEnginePage* page) {
    struct Result { bool done = false; QList<ui::WebVideoSource> videos; };
    auto result = std::make_shared<Result>();
    ui::discoverWebVideos(page, page, [result](QList<ui::WebVideoSource> videos) {
        result->videos = std::move(videos); result->done = true;
    });
    if (!waitFor([&] { return result->done; })) throw std::runtime_error("Discovery timed out");
    return result->videos;
}

QWebEnginePage* backgroundPage(const QUrl& url) {
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* view = qobject_cast<QWebEngineView*>(widget);
        if (view && view->page()->property("vltBackgroundPage").toBool() && view->url() == url)
            return view->page();
    }
    return nullptr;
}

class FixtureServer : public QTcpServer {
public:
    QByteArray video;
    quint16 embeddedPort = 0;
    FixtureServer() {
        QFile file(QStringLiteral(DAW_TEST_WEB_VIDEO));
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing WebM fixture");
        video = file.readAll();
        if (!listen(QHostAddress::LocalHost)) throw std::runtime_error("Cannot bind fixture server");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input] {
                    input->append(socket->readAll());
                    if (!input->contains("\r\n\r\n")) return;
                    const QByteArray path = input->split(' ').value(1).split('?').first();
                    QByteArray body, mime = "text/html", status = "200 OK";
                    const QByteArray v = "<video id='movie' muted autoplay src='/clip.webm' style='width:320px;height:180px'></video>";
                    if (path == "/clip.webm") { body = video; mime = "video/webm"; }
                    else if (path == "/single") body = v;
                    else if (path == "/multi") body = "<video id='secondary' muted src='/clip.webm'></video>" + v;
                    else if (path == "/dynamic") body = "<script>setTimeout(()=>{document.body.innerHTML=\"" + v + "\"},500)</script>";
                    else if (path == "/nested") body = "<iframe name='player' src='http://127.0.0.1:" + QByteArray::number(embeddedPort) + "/single'></iframe>";
                    else if (path == "/bad") body = "<video id='movie' src='/absent.webm'></video>";
                    else if (path == "/empty") body = "<p>No video</p>";
                    else { status = "404 Not Found"; body = "Missing"; }
                    if (mime == "text/html") body = "<!doctype html><html><head><title>Video fixture</title></head><body>" + body + "</body></html>";
                    QByteArray rangeHeader;
                    if (mime == "video/webm") {
                        rangeHeader = "Accept-Ranges: bytes\r\n";
                        const auto range = QRegularExpression("Range: bytes=(\\d+)-(\\d*)", QRegularExpression::CaseInsensitiveOption)
                            .match(QString::fromUtf8(*input));
                        if (range.hasMatch()) {
                            const qint64 total = body.size();
                            const qint64 first = range.captured(1).toLongLong();
                            const qint64 last = range.captured(2).isEmpty() ? total - 1 : std::min(total - 1, range.captured(2).toLongLong());
                            if (first <= last && first < total) {
                                status = "206 Partial Content";
                                rangeHeader += "Content-Range: bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/" + QByteArray::number(total) + "\r\n";
                                body = body.mid(first, last - first + 1);
                            }
                        }
                    }
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + mime + "\r\n" + rangeHeader + "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                    socket->disconnect(socket);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl url(const QString& path) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }
};

void load(QWebEngineView& view, const QUrl& url) {
    auto done = std::make_shared<bool>(false);
    const auto connection = QObject::connect(&view, &QWebEngineView::loadFinished, &view, [done](bool) { *done = true; });
    view.load(url);
    const bool loaded = waitFor([&] { return *done; });
    QObject::disconnect(connection);
    if (!loaded) throw std::runtime_error("Page load timed out");
}

void run(QApplication& app, const QString& root) {
    FixtureServer server, embedded;
    server.embeddedPort = embedded.serverPort();
    QWebEngineProfile profile;
    QWebEngineView browser;
    browser.setAttribute(Qt::WA_DontShowOnScreen);
    browser.resize(640, 360);
    auto* page = new QWebEnginePage(&profile, &browser);
    page->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    browser.setPage(page);
    browser.show();
    load(browser, server.url("/empty"));
    require(discover(page).isEmpty(), "empty page has no candidates");
    load(browser, server.url("/multi"));
    require(waitFor([&] { return js(page, "document.getElementById('movie').readyState>=2").toBool(); }), "fixture video is decoded");
    auto videos = discover(page);
    require(videos.size() == 2 && videos[0].elementId == "movie" && videos[0].playing,
            "multiple videos prioritize the playing candidate");
    js(page, "document.getElementById('movie').pause();document.getElementById('movie').currentTime=1.1");
    require(waitFor([&] { return js(page, "document.getElementById('movie').currentTime>=1").toBool(); }), "fixture seek completes");
    videos = discover(page);
    auto source = *std::find_if(videos.begin(), videos.end(), [](const auto& v) { return v.elementId == "movie"; });
    require(source.position >= 1.0, "discovery retains source position");
    auto blobSource = source;
    blobSource.mediaUrl = "blob:http://127.0.0.1/temporary";
    const auto serialized = blobSource.toJson();
    require(serialized.value("mediaUrl").toString().isEmpty() && !serialized.contains("token"), "persistence excludes transient media and document tokens");
    const auto restored = ui::WebVideoSource::fromJson(serialized);
    require(restored.valid() && !restored.frame, "source descriptor round trips without a live frame");
    ui::pauseWebVideo(restored);
    auto invalid = source;
    invalid.pageUrl = QUrl("file:///etc/passwd");
    require(!invalid.valid(), "non-web background navigation is rejected");

    const QString localFile = root + "/local.png";
    QImage still(24, 16, QImage::Format_RGB32); still.fill(Qt::green); still.save(localFile);
    require(ui::timelinebackgroundprefs::setPath(localFile), "local fallback is configured");
    ui::ThemeMediaBackground media;
    media.setTargetSize(QSize(160, 90), 1);
    media.setSource(localFile);
    require(waitFor([&] { return media.hasFrame(); }), "local background decodes");
    require(ui::checkThemeMediaBackgroundForTest(), "existing placement modes pass");

    auto background = std::make_unique<ui::WebVideoBackground>(nullptr, &profile);
    int frames = 0, changes = 0, commits = 0, errors = 0;
    quint64 lastId = 0;
    QImage previous;
    const auto wire = [&] {
        QObject::connect(background.get(), &ui::WebVideoBackground::frameReady, &app, [&](const QImage& image, quint64 id) {
            ++frames; if (!previous.isNull() && previous != image) ++changes;
            previous = image; lastId = id; media.setExternalFrame(image, id);
        });
        QObject::connect(background.get(), &ui::WebVideoBackground::sourceCommitted, &app, [&](const ui::WebVideoSource& original) {
            ++commits; ui::pauseWebVideo(original);
        });
        QObject::connect(background.get(), &ui::WebVideoBackground::cleared, &app, [&] { media.setSource(localFile); });
        QObject::connect(background.get(), &ui::WebVideoBackground::error, &app, [&](const QString& message) {
            ++errors; std::fprintf(stderr, "Expected/diagnostic player error: %s\n", message.toUtf8().constData());
        });
    };
    ui::timelinebackgroundprefs::setPlacement(ui::timelinebackgroundprefs::Placement::Center);
    wire(); background->setPlaying(true); background->request(source);
    require(waitFor([&] { return background->active() && frames >= 3; }), "independent background commits after video frames arrive");
    require(commits == 1 && changes > 0, "hidden renderer produces changing video frames");
    require(ui::timelinebackgroundprefs::placement() == ui::timelinebackgroundprefs::Placement::Fill,
            "browser transfer fills the timeline even after a centred local wallpaper");
    require(background->muted(), "background starts muted");
    auto* player = backgroundPage(source.pageUrl);
    require(player && player->isAudioMuted(), "Chromium audio output starts muted");
    require(js(player, "document.getElementById('movie').currentTime>=1").toBool(), "independent player starts at selected position");
    require(ui::timelinebackgroundprefs::path() == localFile, "web source retains local fallback");
    require(js(page, "document.getElementById('movie').paused").toBool(), "original video is paused on successful commit");
    background->setMuted(false);
    require(!background->muted(), "background sound can be enabled");
    require(waitFor([&] { return !player->isAudioMuted() && !js(player, "document.getElementById('movie').muted").toBool(); }), "sound toggle reaches Chromium and the video element");
    load(browser, server.url("/empty")); browser.hide();
    const int before = frames;
    require(waitFor([&] { return frames > before + 5; }), "closing source content and hiding browser preserve playback");
    background->setPlaying(false); pump(300);
    const int frozen = frames; pump(400);
    require(frames == frozen, "hidden timeline and reduce motion stop frame capture");
    require(player->isAudioMuted() && js(player, "document.getElementById('movie').paused").toBool(), "hidden timeline pauses video and silences audio");
    media.setTargetSize(QSize(320, 120), 2);
    media.setPlacement(ui::timelinebackgroundprefs::Placement::Stretch);
    media.setBlurRadius(0);
    require(waitFor([&] { return media.frame().size() == QSize(640, 240); }), "external video uses the full timeline surface at high DPI");
    const QImage sharp = media.frame().toImage();
    media.setBlurRadius(24);
    require(waitFor([&] { return media.frame().toImage() != sharp; }), "theme blur changes the existing internet background");
    media.setBlurRadius(0);
    media.setPlacement(ui::timelinebackgroundprefs::Placement::Fill);
    background->setPlaying(true);
    require(waitFor([&] { return frames > frozen; }), "background resumes when timeline becomes visible");

    const quint64 firstId = lastId;
    auto bad = source; bad.pageUrl = server.url("/bad"); bad.frameUrl = bad.pageUrl;
    background->request(bad);
    require(waitFor([&] { return errors > 0 && !background->loading(); }), "failed replacement reports a player error");
    require(background->active() && lastId == firstId, "failed replacement keeps previous background");
    require(background->sourceUrl() == bad.pageUrl, "failure recovery opens the failed page rather than the retained wallpaper page");
    auto next = source; next.pageUrl = server.url("/single"); next.frameUrl = next.pageUrl; next.videoIndex = 0;
    background->request(bad); background->request(next);
    require(waitFor([&] { return !background->loading() && lastId != firstId; }), "rapid replacement discards stale pending callbacks");
    const int beforeLoop = frames; pump(3400);
    require(background->active() && frames > beforeLoop + 20 && changes > 20, "finite video loops and remains animated past its duration");
    background->setMuted(false);
    ui::timelinebackgroundprefs::setPlacement(ui::timelinebackgroundprefs::Placement::Stretch);
    background.reset();
    const auto saved = ui::WebVideoSource::fromJson(ui::timelinebackgroundprefs::webSource());
    require(saved.valid() && saved.position > 0, "shutdown persists page and playback position");
    background = std::make_unique<ui::WebVideoBackground>(nullptr, &profile);
    wire(); background->setPlaying(true); background->restore();
    require(waitFor([&] { return background->active(); }), "background restores without a browser tab");
    require(background->muted(), "restored background resets sound to muted");
    require(ui::timelinebackgroundprefs::placement() == ui::timelinebackgroundprefs::Placement::Stretch,
            "restore preserves the subsequently selected background layout");
    background->clear();
    require(!background->active() && ui::timelinebackgroundprefs::webSource().isEmpty(), "remove clears web preference");
    require(waitFor([&] { return media.hasFrame() && media.frame().toImage().pixelColor(20, 20) == QColor(Qt::green); }), "remove restores local frame without stale video composition");

    browser.show(); load(browser, server.url("/nested"));
    auto nested = discover(page);
    require(nested.size() == 1 && nested[0].framePath.size() == 1 && nested[0].frameUrl.port() == embedded.serverPort(), "native discovery crosses iframe origins");
    background->request(nested[0]);
    require(waitFor([&] { return background->active(); }), "cross-origin embedded video renders as background");
    require(previous.width() * 54 == previous.height() * 96, "background captures the isolated video aspect ratio at device pixel ratio");
    const int nestedChanges = changes;
    require(waitFor([&] { return changes > nestedChanges + 3; }), "cross-origin iframe yields moving video pixels");
    require(ui::timelinebackgroundprefs::setPath(localFile), "local selection succeeds while internet background is active");
    background->reloadSettings();
    require(!background->active(), "selecting local media releases internet player");
    load(browser, server.url("/dynamic"));
    pump(650);
    auto dynamic = discover(page);
    require(dynamic.size() == 1, "discovery detects dynamically inserted video");
    background->request(dynamic[0]);
    require(waitFor([&] { return background->active(); }), "background waits for dynamic player creation");
    background->request(next);
    ui::timelinebackgroundprefs::setPath(localFile); background->reloadSettings();
    require(!background->loading() && !background->active(), "local selection cancels in-flight replacement");
    pump(300);
    background.reset();
    QThreadPool::globalInstance()->waitForDone();
    pump(50);
}

// Explicit opt-in smoke check for a real site. Never part of CTest, and always
// uses an ephemeral profile rather than the user's cookies or browser history.
void runExternal(QApplication& app, const QUrl& url) {
    QWebEngineProfile profile;
    QWebEngineView browser;
    browser.setAttribute(Qt::WA_DontShowOnScreen);
    browser.resize(1280, 720);
    auto* page = new QWebEnginePage(&profile, &browser);
    page->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    page->setAudioMuted(true);
    browser.setPage(page); browser.show(); browser.load(url);
    QList<ui::WebVideoSource> sources;
    require(waitFor([&] { sources = discover(page); return !sources.isEmpty(); }, 45000), "external page exposes a video");
    ui::WebVideoBackground background(nullptr, &profile);
    int frames = 0, changes = 0;
    QImage previous;
    QString failure;
    QObject::connect(&background, &ui::WebVideoBackground::error, &app, [&](const QString& error) { failure = error; });
    QObject::connect(&background, &ui::WebVideoBackground::frameReady, &app, [&](const QImage& image, quint64) {
        ++frames; if (!previous.isNull() && previous != image) ++changes; previous = image;
    });
    background.setPlaying(true); background.request(sources.front());
    waitFor([&] { return changes > 10 || !failure.isEmpty(); }, 45000);
    if (!failure.isEmpty()) std::fprintf(stderr, "%s\n", failure.toUtf8().constData());
    require(frames > 10 && changes > 10, "external video renders moving background frames");
    std::printf("External smoke: %s, %d frames, %d changes\n", url.toString().toUtf8().constData(), frames, changes);
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    app.setOrganizationName("VLTTests"); app.setApplicationName("WebVideoBackground");
    app.setProperty("dawHeadlessDataRoot", root.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.path());
    try {
        if (argc == 3 && QByteArray(argv[1]) == "--url") runExternal(app, QUrl(QString::fromUtf8(argv[2])));
        else run(app, root.path());
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
    std::puts("Web video background checks passed");
    return 0;
}
