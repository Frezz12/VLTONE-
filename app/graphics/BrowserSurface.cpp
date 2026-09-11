#include "BrowserSurface.hpp"
#include "GraphicsPreferences.hpp"
#include "WebPrefs.hpp"
#include "Typography.hpp"
#include "WebPermissionPolicy.hpp"
#include <QQuickWebEngineProfile>
#include <QQuickWebEngineDownloadRequest>
#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QVBoxLayout>
#include <QFocusEvent>
#include <QQmlComponent>
#include <QDir>
#include <QWebEngineScript>
#include <QApplication>
#include <QMetaEnum>
#include <QQuickWindow>
#include <QTimer>
#include <QResizeEvent>
#include <QQuickImageProvider>
#include "SceneRecordingTag.hpp"

namespace ui::graphics {
BrowserProfile::BrowserProfile(QObject* owner, QWebEngineProfile* supplied, bool persistent) : QObject(owner) {
    // An explicitly supplied profile is a compatibility/test profile. Otherwise
    // exactly one backend opens the existing storage directories.
    if (!supplied && gpuWorkspaceEnabled()) {
        quick = persistent ? new QQuickWebEngineProfile(QStringLiteral("VLTStudioWeb"), this) : new QQuickWebEngineProfile(this);
        if (persistent) {
            quick->setPersistentStoragePath(webprefs::profileStoragePath());
            quick->setCachePath(webprefs::profileCachePath());
            quick->setPersistentCookiesPolicy(QQuickWebEngineProfile::ForcePersistentCookies);
            quick->setHttpCacheType(QQuickWebEngineProfile::DiskHttpCache);
        }
        installFontUrlHandler(quick);
        connect(quick, &QQuickWebEngineProfile::downloadRequested, this, [this](QQuickWebEngineDownloadRequest* request) {
            emit downloadRequested(request);
        });
    } else {
        legacy = supplied ? supplied : persistent ? new QWebEngineProfile(QStringLiteral("VLTStudioWeb"), this) : new QWebEngineProfile(this);
        if (!supplied && persistent) {
            legacy->setPersistentStoragePath(webprefs::profileStoragePath());
            legacy->setCachePath(webprefs::profileCachePath());
            legacy->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
            legacy->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
        }
        installFontUrlHandler(legacy);
        connect(legacy, &QWebEngineProfile::downloadRequested, this, &BrowserProfile::downloadRequested);
    }
}
BrowserProfile::~BrowserProfile() { delete m_engine; m_engine = nullptr; }
QQmlEngine* BrowserProfile::engine() {
    if (!m_engine) m_engine = new QQmlEngine(this);
    return m_engine;
}
namespace {
class CompatiblePage : public QWebEnginePage {
public:
    BrowserPage* bridge;
    CompatiblePage(QWebEngineProfile* profile, BrowserPage* owner) : QWebEnginePage(profile, owner), bridge(owner) {}
    bool acceptNavigationRequest(const QUrl& url, NavigationType, bool main) override {
        return bridge->allowNavigation(url, main);
    }
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString& message,
                                  int line, const QString& source) override {
        if (url().scheme() == "http" || url().scheme() == "https") return;
        QWebEnginePage::javaScriptConsoleMessage(level, message, line, source);
    }
};
}
BrowserPage::BrowserPage(BrowserProfile* profile, QObject* owner) : QuickVisual(owner), m_profile(profile) {
    if (profile->legacy) {
        m_legacy = new CompatiblePage(profile->legacy, this);
        denyWebPagePermissions(m_legacy, this);
        connect(m_legacy, &QWebEnginePage::loadStarted, this, &BrowserPage::loadStarted);
        connect(m_legacy, &QWebEnginePage::loadFinished, this, &BrowserPage::loadFinished);
        connect(m_legacy, &QWebEnginePage::loadProgress, this, &BrowserPage::loadProgress);
        connect(m_legacy, &QWebEnginePage::titleChanged, this, &BrowserPage::titleChanged);
        connect(m_legacy, &QWebEnginePage::urlChanged, this, &BrowserPage::urlChanged);
        connect(m_legacy, &QWebEnginePage::iconChanged, this, &BrowserPage::iconChanged);
        connect(m_legacy, &QWebEnginePage::loadingChanged, this, &BrowserPage::loadingChanged);
        connect(m_legacy, &QWebEnginePage::renderProcessTerminated, this, &BrowserPage::renderProcessTerminated);
        connect(m_legacy, &QWebEnginePage::newWindowRequested, this, &BrowserPage::newWindowRequested);
    }
}
BrowserPage::~BrowserPage() {
    navigationRejected = {}; internalNavigationAllowed = {};
    delete m_quick.data();
    delete m_legacy; m_legacy = nullptr;
}
QQuickItem* BrowserPage::quickItem() {
    if (m_legacy || m_quick) return m_quick;
    QQmlComponent component(m_profile->engine(), QUrl(QStringLiteral("qrc:/vlt/browser/BrowserView.qml")));
    m_quick = qobject_cast<QQuickItem*>(component.createWithInitialProperties({
        {QStringLiteral("bridge"), QVariant::fromValue(static_cast<QObject*>(this))},
        {QStringLiteral("profile"), QVariant::fromValue(m_profile->quick)}}));
    if (!m_quick) { qWarning() << component.errors(); return nullptr; }
    QQmlEngine::setObjectOwnership(m_quick, QQmlEngine::CppOwnership);
    m_quick->setParent(this);
    return m_quick;
}
QQuickItem* BrowserPage::createItem(QQmlEngine*, QQuickItem* parent) {
    auto* item = quickItem();
    if (item) { item->setParentItem(parent); item->setVisible(true); }
    return item;
}
void BrowserPage::releaseItem(QQuickItem* item) {
    if (item != m_quick) return;
    item->setVisible(false);
    item->setParentItem(nullptr);
    // Keep the page, cookies, history and active downloads alive across tabs.
}
void BrowserPage::call(const char* method, QVariant a, QVariant b) {
    auto* item = quickItem();
    if (!item) return;
    if (b.isValid()) QMetaObject::invokeMethod(item, method, Q_ARG(QVariant, a), Q_ARG(QVariant, b));
    else if (a.isValid()) QMetaObject::invokeMethod(item, method, Q_ARG(QVariant, a));
    else QMetaObject::invokeMethod(item, method);
}
QUrl BrowserPage::url() const { return m_legacy ? m_legacy->url() : m_quick ? m_quick->property("url").toUrl() : QUrl(); }
QString BrowserPage::title() const { return m_legacy ? m_legacy->title() : m_quick ? m_quick->property("title").toString() : QString(); }
QWebEngineHistory* BrowserPage::history() const {
    return m_legacy ? m_legacy->history() : m_quick ? qvariant_cast<QWebEngineHistory*>(m_quick->property("history")) : nullptr;
}
qreal BrowserPage::zoomFactor() const { return m_legacy ? m_legacy->zoomFactor() : m_quick ? m_quick->property("zoomFactor").toDouble() : 1; }
void BrowserPage::setZoomFactor(qreal v) { if (m_legacy) m_legacy->setZoomFactor(v); else if (auto* q = quickItem()) q->setProperty("zoomFactor", v); }
void BrowserPage::load(const QUrl& v) { if (m_legacy) m_legacy->load(v); else if (auto* q = quickItem()) q->setProperty("url", v); }
void BrowserPage::setHtml(const QString& html, const QUrl& base) { if (m_legacy) m_legacy->setHtml(html, base); else call("setDocument", html, base); }
void BrowserPage::back() { if (m_legacy) m_legacy->triggerAction(QWebEnginePage::Back); else call("back"); }
void BrowserPage::forward() { if (m_legacy) m_legacy->triggerAction(QWebEnginePage::Forward); else call("forward"); }
void BrowserPage::reload() { if (m_legacy) m_legacy->triggerAction(QWebEnginePage::Reload); else call("refresh"); }
void BrowserPage::stop() { if (m_legacy) m_legacy->triggerAction(QWebEnginePage::Stop); else call("halt"); }
void BrowserPage::setBackgroundColor(const QColor& color) { if (m_legacy) m_legacy->setBackgroundColor(color); else if (auto* q = quickItem()) q->setProperty("backgroundColor", color); }
void BrowserPage::setAudioMuted(bool muted) { if (m_legacy) m_legacy->setAudioMuted(muted); else if (auto* q = quickItem()) q->setProperty("audioMuted", muted); }
void BrowserPage::setWebAttribute(QWebEngineSettings::WebAttribute attribute, bool enabled) {
    if (m_legacy) { m_legacy->settings()->setAttribute(attribute, enabled); return; }
    const char* name = nullptr;
    switch (attribute) {
    case QWebEngineSettings::JavascriptCanOpenWindows: name = "javascriptCanOpenWindows"; break;
    case QWebEngineSettings::LocalContentCanAccessFileUrls: name = "localContentCanAccessFileUrls"; break;
    case QWebEngineSettings::LocalContentCanAccessRemoteUrls: name = "localContentCanAccessRemoteUrls"; break;
    case QWebEngineSettings::PlaybackRequiresUserGesture: name = "playbackRequiresUserGesture"; break;
    case QWebEngineSettings::FullScreenSupportEnabled: name = "fullScreenSupportEnabled"; break;
    default: return;
    }
    call("configure", QString::fromLatin1(name), enabled);
}
void BrowserPage::triggerAction(QWebEnginePage::WebAction action) {
    if (m_legacy) { m_legacy->triggerAction(action); return; }
    // Resolve QML enums by name; their integer values are not a cross-version contract.
    const auto meta = QMetaEnum::fromType<QWebEnginePage::WebAction>();
    call("edit", QString::fromLatin1(meta.valueToKey(action)));
}
void BrowserPage::findText(const QString& text, QWebEnginePage::FindFlags flags) {
    if (m_legacy) m_legacy->findText(text, flags);
    else call("find", text, flags.testFlag(QWebEnginePage::FindBackward));
}
void BrowserPage::download(const QUrl& url, const QString& name) {
    if (m_legacy) { m_legacy->download(url, name); return; }
    // The element click uses Chromium's normal downloadRequested path, including
    // cookies and redirect handling; it does not fetch files in the host process.
    call("downloadFile", url, name);
}
void BrowserPage::openRequest(QWebEngineNewWindowRequest& request) {
    if (m_legacy) request.openIn(m_legacy);
    else call("acceptWindow", QVariant::fromValue(&request));
}
std::optional<QWebEngineFrame> BrowserPage::mainFrame() const { return m_legacy ? std::optional(m_legacy->mainFrame()) : m_frame; }
void BrowserPage::runJavaScript(const QString& script, std::function<void(const QVariant&)> callback) {
    if (auto frame = mainFrame(); frame && frame->isValid()) {
        frame->runJavaScript(script, std::move(callback));
    } else if (callback) QTimer::singleShot(0, this, [callback = std::move(callback)] { callback({}); });
}
void BrowserPage::attachWebChannel(QWebChannel* channel) {
    m_channel = channel;
    if (channel) for (auto it = m_channelObjects.cbegin(); it != m_channelObjects.cend(); ++it)
        if (it.value()) channel->registerObject(it.key(), it.value());
}
void BrowserPage::setWebChannelObject(const QString& name, QObject* object) {
    m_channelObjects.insert(name, object);
    if (m_legacy && !m_channel) { m_channel = new QWebChannel(this); m_legacy->setWebChannel(m_channel); }
    if (m_channel) m_channel->registerObject(name, object);
}
bool BrowserPage::allowNavigation(const QUrl& url, bool main) {
    if (navigationPolicy) return navigationPolicy(url, main);
    const bool ordinary = url.scheme() == "https" || url.scheme() == "http" || url == QUrl("about:blank");
    if (ordinary || !main || (internalNavigationAllowed && internalNavigationAllowed(url))) return true;
    if (navigationRejected) navigationRejected(url);
    return false;
}
void BrowserPage::notifyLoading(const QWebEngineLoadingInfo& info) {
    emit loadingChanged(info);
    if (info.status() == QWebEngineLoadingInfo::LoadStartedStatus) emit loadStarted();
    if (info.status() == QWebEngineLoadingInfo::LoadSucceededStatus || info.status() == QWebEngineLoadingInfo::LoadFailedStatus)
        emit loadFinished(info.status() == QWebEngineLoadingInfo::LoadSucceededStatus);
}
void BrowserPage::notifyUrl() { emit urlChanged(url()); }
void BrowserPage::notifyTitle() { emit titleChanged(title()); }
void BrowserPage::notifyProgress() { if (m_quick) emit loadProgress(m_quick->property("loadProgress").toInt()); }
void BrowserPage::notifyIcon() {
    if (!m_quick) return;
    const auto iconUrl = m_quick->property("icon").toUrl();
    auto* provider = m_profile->engine()->imageProvider(iconUrl.host());
    auto* asynchronous = dynamic_cast<QQuickAsyncImageProvider*>(provider);
    if (!asynchronous || iconUrl.scheme() != "image") { emit iconChanged(QIcon()); return; }
    auto* response = asynchronous->requestImageResponse(iconUrl.path().mid(1), QSize(32, 32));
    if (!response) return;
    const QPointer<BrowserPage> guard(this);
    connect(response, &QQuickImageResponse::finished, response, [response, guard, iconUrl] {
        if (guard && guard->m_quick && guard->m_quick->property("icon").toUrl() == iconUrl) {
            std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
            emit guard->iconChanged(texture ? QIcon(QPixmap::fromImage(texture->image())) : QIcon());
        }
        response->deleteLater();
    }, Qt::QueuedConnection);
}
void BrowserPage::notifyNewWindow(QWebEngineNewWindowRequest* request) { if (request) emit newWindowRequested(*request); }
void BrowserPage::notifyTerminated(int status, int code) { emit renderProcessTerminated(static_cast<QWebEnginePage::RenderProcessTerminationStatus>(status), code); }

BrowserSurface::BrowserSurface(BrowserProfile* profile, QWidget* parent) : QWidget(parent), m_page(new BrowserPage(profile, this)) {
    setFocusPolicy(Qt::StrongFocus);
    if (profile->legacy) {
        m_legacyView = new QWebEngineView(this);
        m_legacyView->setPage(m_page->legacy());
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_legacyView);
        setFocusProxy(m_legacyView);
    }
    connect(m_page, &BrowserPage::loadStarted, this, &BrowserSurface::loadStarted);
    connect(m_page, &BrowserPage::loadFinished, this, &BrowserSurface::loadFinished);
    connect(m_page, &BrowserPage::loadProgress, this, &BrowserSurface::loadProgress);
    connect(m_page, &BrowserPage::titleChanged, this, &BrowserSurface::titleChanged);
    connect(m_page, &BrowserPage::urlChanged, this, &BrowserSurface::urlChanged);
    connect(m_page, &BrowserPage::iconChanged, this, &BrowserSurface::iconChanged);
}
BrowserSurface::~BrowserSurface() {
    if (m_fallback && m_page->isQuick()) m_page->releaseItem(m_page->quickItem());
    delete m_fallback.data();
    delete m_legacyView; delete m_page;
}
bool BrowserSurface::ownsQuickFocus() const {
    return QApplication::focusWidget() == this && m_page->isQuick() &&
           m_page->quickItem() && m_page->quickItem()->hasActiveFocus();
}
void BrowserSurface::paintScene(QPainter& painter, const QRegion&) {
    if (!m_page->isQuick()) return;
    if (isSceneRecording(painter)) m_page->paintVisual(painter, rect());
    else if (!m_fallback && !m_fallbackPending) {
        m_fallbackPending = true;
        QTimer::singleShot(0, this, [this] { m_fallbackPending = false; ensureFallback(); });
    }
}
void BrowserSurface::ensureFallback() {
    if (m_fallback || !isVisible()) return;
    for (QWidget* parent = this; parent; parent = parent->parentWidget())
        if (parent->property("vlt.gpuSurfaceActive").toBool()) return;
    // A failed workspace must not destroy tabs or reopen their profile with a
    // second backend. Keep the same Chromium page in a native Quick window.
    auto* window = new QQuickWindow;
    window->setColor(Qt::white);
    m_fallback = QWidget::createWindowContainer(window, this);
    m_fallback->setGeometry(rect());
    auto* item = m_page->createItem(nullptr, window->contentItem());
    if (item) {
        item->setSize(size());
        connect(window, &QWindow::widthChanged, item, [item](int width) { item->setWidth(width); });
        connect(window, &QWindow::heightChanged, item, [item](int height) { item->setHeight(height); });
    }
    setFocusProxy(m_fallback);
    m_fallback->show();
}
void BrowserSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_fallback) m_fallback->setGeometry(rect());
}
void BrowserSurface::paintEvent(QPaintEvent*) { QPainter painter(this); paintScene(painter, QRegion(rect())); }
void BrowserSurface::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    if (m_page->isQuick()) if (auto* item = m_page->quickItem()) {
        if (item->window()) item->window()->requestActivate();
        item->forceActiveFocus(event->reason());
    }
}
}
