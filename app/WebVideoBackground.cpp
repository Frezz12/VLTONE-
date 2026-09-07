#include "WebVideoBackground.hpp"
#include "TimelineBackgroundPrefs.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <algorithm>
#include <cmath>
#include <utility>

namespace ui {
namespace {
class BackgroundPage final : public QWebEnginePage {
public:
    BackgroundPage(QWebEngineProfile* profile, QObject* parent) : QWebEnginePage(profile, parent) {}
protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType, bool) override {
        return url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http") ||
               url == QUrl(QStringLiteral("about:blank"));
    }
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString&, int, const QString&) override {}
};

QString isolateScript(const QString& element) {
    // Keep the original player and its ancestors alive so MSE and site event
    // handlers keep working. Only this independent page's presentation changes.
    return QStringLiteral(R"JS(
(()=>{const el=%1;if(!el)return false;
 let style=document.getElementById('__vlt_background_style');
 if(!style){style=document.createElement('style');style.id='__vlt_background_style';
 style.textContent='html,body{margin:0!important;overflow:hidden!important;background:black!important}body *{visibility:hidden!important}';
 (document.head||document.documentElement).appendChild(style);}
 for(let p=el.parentElement;p;p=p.parentElement){
  for(const [k,v] of Object.entries({transform:'none',filter:'none',perspective:'none',contain:'none',overflow:'visible',opacity:'1',clip:'auto','clip-path':'none'}))p.style.setProperty(k,v,'important');
 }
 for(const [k,v] of Object.entries({visibility:'visible',position:'fixed',left:'0',top:'0',width:'100vw',height:'100vh',margin:'0',padding:'0',border:'0','max-width':'none','max-height':'none','min-width':'0','min-height':'0','object-fit':'contain','z-index':'2147483647',opacity:'1'}))el.style.setProperty(k,v,'important');
 return true;})()
)JS").arg(element);
}

void pauseOtherFrames(QWebEngineFrame frame, const QList<int>& path,
                      const QList<int>& selectedPath, int& remaining) {
    if (!frame.isValid() || remaining-- <= 0) return;
    if (path != selectedPath)
        frame.runJavaScript(QStringLiteral("document.querySelectorAll('video,audio').forEach(v=>{v.muted=true;v.pause()})"),
                            QWebEngineScript::ApplicationWorld);
    const auto children = frame.children();
    for (int i = 0; i < children.size(); ++i) {
        auto childPath = path; childPath.append(i);
        pauseOtherFrames(children[i], childPath, selectedPath, remaining);
    }
}
}

struct WebVideoBackground::Session : QObject {
    explicit Session(QObject* parent) : QObject(parent) { age.start(); }
    ~Session() override { delete view; }
    QWebEngineView* view = nullptr;
    WebVideoSource requested;
    WebVideoSource selected;
    QElapsedTimer age;
    QElapsedTimer pollAge;
    quint64 id = 0;
    bool scanning = false;
    bool installed = false;
    bool polling = false;
    bool ready = false;
    double position = 0;
    qint64 preparedAt = 0;
    qint64 lastReadyAt = 0;
    qint64 lastSilencedAt = -1000;
    int isolationPending = 0;
};

WebVideoBackground::WebVideoBackground(QObject* parent, QWebEngineProfile* profile)
    : QObject(parent), m_profile(profile) {
    m_timer.setInterval(34); // at most 30 frames/s, with one status query in flight
    connect(&m_timer, &QTimer::timeout, this, &WebVideoBackground::tick);
    m_saveTimer.setInterval(5000);
    connect(&m_saveTimer, &QTimer::timeout, this, &WebVideoBackground::savePosition);
}

WebVideoBackground::~WebVideoBackground() {
    savePosition();
    delete std::exchange(m_pending, nullptr);
    delete std::exchange(m_active, nullptr);
    // Failed sessions can still be waiting for deferred deletion.
    for (auto* child : children())
        if (auto* session = dynamic_cast<Session*>(child)) delete session;
    if (m_ownsProfile) delete m_profile;
}

QWebEngineProfile* WebVideoBackground::browserProfile() {
    if (!m_profile) { m_profile = createWebBrowserProfile(this); m_ownsProfile = true; }
    return m_profile;
}

bool WebVideoBackground::owns(Session* session) const {
    return session && (session == m_pending || session == m_active);
}

QUrl WebVideoBackground::sourceUrl() const {
    // After a failed replacement, Open must lead to the page which needs
    // attention, while the retained previous wallpaper continues to play.
    return m_pending ? m_pending->requested.pageUrl : m_lastRequestedUrl;
}

void WebVideoBackground::request(const WebVideoSource& source) {
    if (!source.valid()) { emit error(tr("This video cannot be used as a timeline background.")); return; }
    delete std::exchange(m_pending, nullptr);
    auto* session = new Session(this);
    m_pending = session;
    session->id = ++m_nextId;
    session->requested = source;
    session->position = source.position;
    m_lastRequestedUrl = source.pageUrl;
    m_sourceRevision = timelinebackgroundprefs::sourceRevision();
    session->view = new QWebEngineView;
    session->view->setAttribute(Qt::WA_DontShowOnScreen);
    session->view->setAttribute(Qt::WA_ShowWithoutActivating);
    session->view->setFocusPolicy(Qt::NoFocus);
    session->view->resize(960, 540);
    auto* page = new BackgroundPage(browserProfile(), session->view);
    page->setProperty("vltBackgroundPage", true);
    session->view->setPage(page);
    page->setAudioMuted(true);
    page->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    connect(page, &QWebEnginePage::permissionRequested, session,
            [](QWebEnginePermission permission) { permission.deny(); });
    connect(page, &QWebEnginePage::renderProcessTerminated, session,
        [this, session](QWebEnginePage::RenderProcessTerminationStatus, int) {
            fail(session, tr("The background video player stopped. Open the page and try again."));
        });
    connect(session->view, &QWebEngineView::loadFinished, session, [this, session](bool ok) {
        if (!ok && owns(session)) fail(session, tr("Could not load the video page. Check your connection or open the page to sign in."));
    });
    session->view->load(source.pageUrl);
    session->view->show();
    m_timer.start();
    emit stateChanged();
}

void WebVideoBackground::restore() {
    const auto stored = timelinebackgroundprefs::webSource();
    if (stored.isEmpty() || m_active || m_pending) return;
    const auto source = WebVideoSource::fromJson(stored);
    if (source.valid()) request(source);
}

void WebVideoBackground::clear() {
    delete std::exchange(m_pending, nullptr);
    delete std::exchange(m_active, nullptr);
    m_muted = true;
    m_lastRequestedUrl = QUrl();
    m_timer.stop(); m_saveTimer.stop();
    timelinebackgroundprefs::clearWebSource();
    emit cleared();
    emit stateChanged();
}

void WebVideoBackground::reloadSettings() {
    if ((m_active || m_pending) && m_sourceRevision != timelinebackgroundprefs::sourceRevision()) clear();
}

void WebVideoBackground::setPlaying(bool playing) {
    if (m_playing == playing) return;
    m_playing = playing;
    if (m_active) {
        m_active->view->page()->setAudioMuted(m_muted || !m_playing);
        m_active->pollAge.invalidate();
    }
}

void WebVideoBackground::setMuted(bool muted) {
    m_muted = muted;
    if (m_active) {
        m_active->view->page()->setAudioMuted(m_muted || !m_playing);
        m_active->pollAge.invalidate();
    }
    emit stateChanged();
}

void WebVideoBackground::savePosition() {
    if (!m_active || timelinebackgroundprefs::webSource().isEmpty()) return;
    auto source = m_active->requested;
    source.position = m_active->position;
    timelinebackgroundprefs::setWebSource(source.toJson());
}

void WebVideoBackground::fail(Session* session, const QString& reason) {
    if (!owns(session)) return;
    const bool wasActive = session == m_active;
    if (wasActive) { savePosition(); m_active = nullptr; m_saveTimer.stop(); }
    else m_pending = nullptr;
    session->view->page()->setAudioMuted(true);
    session->deleteLater(); // may be executing a WebEngine callback
    if (wasActive) emit cleared();
    if (!m_active && !m_pending) m_timer.stop();
    emit stateChanged();
    emit error(reason);
}

void WebVideoBackground::inspect(Session* session) {
    if (session->scanning) return;
    session->scanning = true;
    const QPointer<Session> guard(session);
    discoverWebVideos(session->view->page(), session, [this, guard](QList<WebVideoSource> candidates) {
        if (!guard || !owns(guard)) return;
        guard->scanning = false;
        const auto& wanted = guard->requested;
        auto best = candidates.end();
        int bestScore = -1;
        for (auto it = candidates.begin(); it != candidates.end(); ++it) {
            const bool main = wanted.framePath.isEmpty();
            if (main != it->framePath.isEmpty()) continue;
            if (!main && it->frameUrl != wanted.frameUrl && it->framePath != wanted.framePath) continue;
            int score = it->framePath == wanted.framePath ? 4 : 0;
            if (it->frameUrl == wanted.frameUrl) score += 8;
            if (!wanted.elementId.isEmpty() && it->elementId == wanted.elementId) score += 32;
            if (!wanted.mediaUrl.isEmpty() && !wanted.mediaUrl.startsWith("blob:") && it->mediaUrl == wanted.mediaUrl) score += 64;
            if (it->videoIndex == wanted.videoIndex) score += 16;
            if (score > bestScore) { bestScore = score; best = it; }
        }
        if (best != candidates.end()) prepare(guard, *best);
    });
}

void WebVideoBackground::prepare(Session* session, const WebVideoSource& candidate) {
    if (!candidate.frame || !candidate.frame->isValid()) return;
    session->selected = candidate;
    session->installed = true;
    session->ready = false;
    session->preparedAt = session->age.elapsed();
    session->isolationPending = candidate.framePath.size() + 1;
    const QPointer<Session> guard(session);
    const auto isolated = [this, guard](const QVariant& result) {
        if (!guard || !owns(guard)) return;
        if (!result.toBool()) {
            fail(guard, tr("This player could not start the background video. Open the page and try another video."));
            return;
        }
        --guard->isolationPending;
    };
    // Expand each containing iframe in its own origin via native frame APIs.
    auto parent = session->view->page()->mainFrame();
    for (int index : candidate.framePath) {
        const auto children = parent.children();
        if (index < 0 || index >= children.size()) { session->installed = false; return; }
        const auto child = children[index];
        const auto hint = QString::fromUtf8(QJsonDocument(QJsonArray{child.htmlName(), child.url().toString()})
            .toJson(QJsonDocument::Compact));
        const QString element = QStringLiteral("(()=>{const h=%1,a=Array.from(document.querySelectorAll('iframe,frame')),at=a[%2];return (at&&((h[0]&&at.name===h[0])||at.src===h[1])&&at)||(h[0]&&a.find(f=>f.name===h[0]))||a.find(f=>f.src===h[1])||at})()")
            .arg(hint).arg(index);
        parent.runJavaScript(isolateScript(element), QWebEngineScript::ApplicationWorld, isolated);
        parent = child;
    }
    session->selected.frame->runJavaScript(isolateScript(webVideoLookupScript(candidate)), QWebEngineScript::ApplicationWorld, isolated);
    poll(session);
}

void WebVideoBackground::poll(Session* session) {
    if (session->polling || !session->selected.frame || !session->selected.frame->isValid()) return;
    session->polling = true;
    session->pollAge.restart();
    const bool playing = session == m_pending || m_playing;
    const bool muted = session == m_pending || m_muted || !m_playing;
    const QString script = QStringLiteral(R"JS(
(()=>{const v=%1;if(!v)return {missing:true};
 if(v.mediaKeys)return {encrypted:true};
 if(document.querySelector('.html5-video-player.ad-showing'))return {waiting:true};
 if(v.error)return {failed:true};
 const seek=%2;
 if(!v.__vltSeekApplied && v.readyState>=1){
   if(Number.isFinite(v.duration)&&v.duration>0){v.currentTime=Math.min(seek,Math.max(0,v.duration-0.05));v.__vltSeekApplied=true;}
   else if(v.duration===Infinity)v.__vltSeekApplied=true;
 }
 v.controls=false;v.loop=Number.isFinite(v.duration)&&v.duration>0;
 v.muted=%3;v.volume=1;
 for(const other of document.querySelectorAll('video,audio'))if(other!==v)other.pause();
 if(%4){if(v.paused){const promise=v.play();if(promise)promise.catch(()=>{v.__vltPlayFailed=true});}}
 else v.pause();
 return {ready:v.readyState>=2&&!v.seeking&&v.videoWidth>0&&(!%4||!v.paused), width:v.videoWidth,height:v.videoHeight,
         position:Number.isFinite(v.currentTime)?v.currentTime:0, blocked:!!v.__vltPlayFailed, paused:v.paused};
})()
)JS").arg(webVideoLookupScript(session->selected))
        .arg(session->requested.position, 0, 'g', 16)
        .arg(muted ? "true" : "false").arg(playing ? "true" : "false");
    const QPointer<Session> guard(session);
    session->selected.frame->runJavaScript(script, QWebEngineScript::ApplicationWorld,
        [this, guard, playing](const QVariant& result) {
            if (!guard || !owns(guard)) return;
            guard->polling = false;
            const auto state = result.toMap();
            if (state.value("encrypted").toBool() || state.value("failed").toBool() || state.value("blocked").toBool()) {
                fail(guard, tr("This player could not start the background video. Open the page and try another video.")); return;
            }
            if (state.isEmpty() || state.value("missing").toBool()) {
                guard->installed = false; guard->ready = false;
                guard->preparedAt = guard->age.elapsed();
                return;
            }
            guard->ready = state.value("ready").toBool();
            if (!guard->ready) return;
            guard->lastReadyAt = guard->age.elapsed();
            guard->position = state.value("position").toDouble();
            const QSize native(state.value("width").toInt(), state.value("height").toInt());
            if (!native.isEmpty()) {
                const QSize pixels = native.width() > 1280 || native.height() > 720
                    ? native.scaled(QSize(1280, 720), Qt::KeepAspectRatio) : native;
                const qreal dpr = std::max<qreal>(1, guard->view->devicePixelRatioF());
                const QSize size(std::max(1, int(std::lround(pixels.width() / dpr))),
                                 std::max(1, int(std::lround(pixels.height() / dpr))));
                if (guard->view->size() != size) {
                    guard->view->resize(size);
                    guard->preparedAt = guard->age.elapsed();
                }
            }
        });
}

void WebVideoBackground::tick() {
    for (Session* session : {m_pending, m_active}) {
        if (!owns(session)) continue;
        if (session == m_pending && session->age.elapsed() > 30000) {
            fail(session, tr("No playable video was found. Open the page to sign in or select the video again.")); continue;
        }
        if (session == m_active && session->age.elapsed() - session->lastReadyAt > 30000) {
            fail(session, tr("The background video player stopped. Open the page and try again.")); continue;
        }
        if (!session->installed || !session->selected.frame || !session->selected.frame->isValid()) {
            session->installed = false;
            session->ready = false;
            if (!session->pollAge.isValid() || session->pollAge.elapsed() > 400) {
                session->pollAge.restart(); inspect(session);
            }
            continue;
        }
        if (!session->pollAge.isValid() || session->pollAge.elapsed() >= 200) poll(session);
        if (session == m_active && !m_muted && session->age.elapsed() - session->lastSilencedAt >= 1000) {
            session->lastSilencedAt = session->age.elapsed();
            int remaining = 64;
            pauseOtherFrames(session->view->page()->mainFrame(), {}, session->selected.framePath, remaining);
        }
        if (!session->ready || session->isolationPending > 0 || session->age.elapsed() - session->preparedAt < 150) continue;
        if (session == m_active && !m_playing) continue;
        const QImage frame = session->view->grab().toImage();
        if (frame.isNull()) continue;
        if (session == m_pending) {
            auto* previous = m_active;
            m_active = session;
            m_pending = nullptr;
            delete previous;
            m_muted = true;
            timelinebackgroundprefs::setWebSource(session->requested.toJson());
            timelinebackgroundprefs::setEnabled(true);
            // A browser transfer means wallpaper across the arrangement, even
            // if the last local image used original-size centring or tiling.
            // A restored source has no document token: retain the user's
            // subsequently chosen layout in that case.
            if (!session->requested.token.isEmpty())
                timelinebackgroundprefs::setPlacement(timelinebackgroundprefs::Placement::Fill);
            session->view->page()->setAudioMuted(true);
            session->pollAge.invalidate();
            m_saveTimer.start();
            emit sourceCommitted(session->requested);
            emit stateChanged();
        }
        emit frameReady(frame, session->id);
    }
}
} // namespace ui
