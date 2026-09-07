#include "WebVideoSource.hpp"
#include "WebPrefs.hpp"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <algorithm>
#include <cmath>
#include <memory>

namespace ui {
namespace {
bool webUrl(const QUrl& url) {
    return url.isValid() && !url.host().isEmpty() &&
        (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"));
}
QString literal(const QString& value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}
struct FrameEntry { QWebEngineFrame frame; QList<int> path; };
void collectFrames(QWebEngineFrame frame, QList<int> path, QList<FrameEntry>& entries) {
    if (!frame.isValid() || entries.size() >= 64 || path.size() > 16) return;
    entries.append({frame, path});
    const auto children = frame.children();
    for (int i = 0; i < children.size(); ++i) {
        auto childPath = path;
        childPath.append(i);
        collectFrames(children[i], childPath, entries);
    }
}
} // namespace

bool WebVideoSource::valid() const {
    return webUrl(pageUrl) && videoIndex >= 0 && videoIndex < 64 &&
        framePath.size() <= 16 && std::isfinite(position) && position >= 0 &&
        std::all_of(framePath.begin(), framePath.end(), [](int i) { return i >= 0 && i < 64; });
}

QJsonObject WebVideoSource::toJson() const {
    QJsonArray path;
    for (int index : framePath) path.append(index);
    return {{"pageUrl", pageUrl.toString()}, {"frameUrl", frameUrl.toString()},
            {"framePath", path}, {"elementId", elementId},
            {"mediaUrl", webUrl(QUrl(mediaUrl)) ? mediaUrl : QString()},
            {"title", title.left(256)}, {"videoIndex", videoIndex},
            {"position", std::isfinite(position) ? std::max(0.0, position) : 0.0}};
}

WebVideoSource WebVideoSource::fromJson(const QJsonObject& json) {
    WebVideoSource source;
    source.pageUrl = QUrl(json.value("pageUrl").toString());
    source.frameUrl = QUrl(json.value("frameUrl").toString());
    for (auto index : json.value("framePath").toArray()) source.framePath.append(index.toInt(-1));
    source.elementId = json.value("elementId").toString();
    source.mediaUrl = json.value("mediaUrl").toString();
    if (!webUrl(QUrl(source.mediaUrl))) source.mediaUrl.clear();
    source.title = json.value("title").toString().left(256);
    source.videoIndex = json.value("videoIndex").toInt(-1);
    source.position = json.value("position").toDouble();
    return source;
}

QWebEngineProfile* createWebBrowserProfile(QObject* owner) {
    QDir().mkpath(webprefs::profileStoragePath());
    QDir().mkpath(webprefs::profileCachePath());
    auto* profile = new QWebEngineProfile(QStringLiteral("VLTStudioWeb"), owner);
    profile->setPersistentStoragePath(webprefs::profileStoragePath());
    profile->setCachePath(webprefs::profileCachePath());
    profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
    return profile;
}

void discoverWebVideos(QWebEnginePage* page, QObject* context,
                       std::function<void(QList<WebVideoSource>)> callback) {
    if (!page || !context) return;
    QList<FrameEntry> frames;
    collectFrames(page->mainFrame(), {}, frames);
    if (frames.isEmpty()) { callback({}); return; }
    struct Scan { int remaining; QList<WebVideoSource> videos; };
    auto scan = std::make_shared<Scan>();
    scan->remaining = frames.size();
    const QPointer<QObject> guard(context);
    const QPointer<QWebEnginePage> pageGuard(page);
    const QUrl pageUrl = page->url();
    const QString pageTitle = page->title();
    for (auto entry : frames) {
        entry.frame.runJavaScript(QStringLiteral(R"JS(
(() => Array.from(document.querySelectorAll('video')).slice(0,64).map((v,i) => {
 const r=v.getBoundingClientRect();
 if (!v.__vltVideoToken) v.__vltVideoToken='vlt-'+Math.random().toString(36).slice(2);
 return {index:i, id:v.id, token:v.__vltVideoToken, src:v.currentSrc,
         title:v.getAttribute('aria-label')||v.title||document.title,
         position:Number.isFinite(v.currentTime)?v.currentTime:0,
         playing:!v.paused&&!v.ended, area:Math.max(0,r.width)*Math.max(0,r.height)};
}))()
)JS"), QWebEngineScript::ApplicationWorld,
            [guard, pageGuard, pageUrl, pageTitle, entry, scan, callback](const QVariant& value) {
                // Chromium completes pending JavaScript with an invalid value
                // during page destruction. QPointer is still non-null while
                // the derived destructor runs; even querying page/view URLs
                // from that callback can recreate a page inside its teardown.
                if (!value.isValid() || !guard || !pageGuard) return;
                if (entry.frame.isValid() && pageGuard->url() == pageUrl) {
                    for (const auto& item : value.toList()) {
                        const auto map = item.toMap();
                        WebVideoSource source;
                        source.pageUrl = pageUrl;
                        source.frameUrl = entry.frame.url();
                        source.framePath = entry.path;
                        source.frame = entry.frame;
                        source.elementId = map.value("id").toString();
                        source.mediaUrl = map.value("src").toString();
                        source.token = map.value("token").toString();
                        source.videoIndex = map.value("index").toInt();
                        source.position = map.value("position").toDouble();
                        source.playing = map.value("playing").toBool();
                        source.area = map.value("area").toDouble();
                        source.title = map.value("title", pageTitle).toString().left(256);
                        if (source.valid()) scan->videos.append(source);
                    }
                }
                if (--scan->remaining == 0) {
                    std::stable_sort(scan->videos.begin(), scan->videos.end(), [](const auto& a, const auto& b) {
                        return a.playing != b.playing ? a.playing : a.area > b.area;
                    });
                    callback(scan->videos);
                }
            });
    }
}

QString webVideoLookupScript(const WebVideoSource& source) {
    // A runtime token is deliberately strict: navigating/replacing a video
    // must never pause an unrelated element which happens to have its index.
    return QStringLiteral("Array.from(document.querySelectorAll('video')).find(v=>v.__vltVideoToken===%1)")
        .arg(literal(source.token));
}

void pauseWebVideo(const WebVideoSource& source) {
    if (!source.frame.isValid() || source.token.isEmpty()) return;
    auto frame = source.frame;
    frame.runJavaScript(QStringLiteral("(()=>{const v=%1;if(v)v.pause();})()")
        .arg(webVideoLookupScript(source)), QWebEngineScript::ApplicationWorld);
}
} // namespace ui
