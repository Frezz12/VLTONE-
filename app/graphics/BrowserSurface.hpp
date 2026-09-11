#pragma once
#include "QuickVisual.hpp"
#include "ScenePaintSource.hpp"
#include <QWidget>
#include <QWebEnginePage>
#include <QWebChannel>
#include <QWebEngineSettings>
#include <QWebEngineFrame>
#include <QWebEngineLoadingInfo>
#include <QWebEngineNewWindowRequest>
#include <QWebEngineHistory>
#include <QIcon>
#include <optional>
#include <functional>

class QWebEngineProfile;
class QQuickWebEngineProfile;
class QWebEngineDownloadRequest;
class QWebEngineView;
class QWebChannel;

namespace ui::graphics {
class BrowserProfile : public QObject {
    Q_OBJECT
public:
    explicit BrowserProfile(QObject* owner, QWebEngineProfile* supplied = nullptr, bool persistent = true);
    ~BrowserProfile() override;
    QWebEngineProfile* legacy = nullptr;
    QQuickWebEngineProfile* quick = nullptr;
    QQmlEngine* engine();
signals:
    void downloadRequested(QWebEngineDownloadRequest*);
private:
    QQmlEngine* m_engine = nullptr;
};

// Shared navigation/download/editing contracts for the native Quick page and
// the compatibility WebEngine widget. Browser chrome and session logic use the
// same C++ implementation on both backends.
class BrowserPage : public QuickVisual {
    Q_OBJECT
public:
    BrowserPage(BrowserProfile*, QObject* owner);
    ~BrowserPage() override;
    QQuickItem* createItem(QQmlEngine*, QQuickItem*) override;
    void releaseItem(QQuickItem*) override;
    bool interactive() const override { return true; }
    QWebEnginePage* legacy() const { return m_legacy; }
    bool isQuick() const { return m_profile->quick; }
    QQuickItem* quickItem();
    QWebEngineHistory* history() const;
    QUrl url() const;
    QString title() const;
    qreal zoomFactor() const;
    void setZoomFactor(qreal);
    void load(const QUrl&);
    void setHtml(const QString&, const QUrl&);
    void back(); void forward(); void reload(); void stop();
    void setBackgroundColor(const QColor&);
    void setAudioMuted(bool);
    void setWebAttribute(QWebEngineSettings::WebAttribute, bool);
    void triggerAction(QWebEnginePage::WebAction);
    void findText(const QString&, QWebEnginePage::FindFlags = {});
    void download(const QUrl&, const QString&);
    void openRequest(QWebEngineNewWindowRequest&);
    std::optional<QWebEngineFrame> mainFrame() const;
    void runJavaScript(const QString&, std::function<void(const QVariant&)> callback = {});
    void setWebChannelObject(const QString& name, QObject* object);
    // Local app documents supply their own stricter main-frame policy.
    std::function<bool(const QUrl&, bool)> navigationPolicy;
    std::function<void(const QUrl&)> navigationRejected;
    std::function<bool(const QUrl&)> internalNavigationAllowed;

    Q_INVOKABLE bool allowNavigation(const QUrl& url, bool mainFrame);
    Q_INVOKABLE void notifyLoading(const QWebEngineLoadingInfo&);
    Q_INVOKABLE void notifyUrl();
    Q_INVOKABLE void notifyTitle();
    Q_INVOKABLE void notifyProgress();
    Q_INVOKABLE void notifyIcon();
    Q_INVOKABLE void notifyNewWindow(QWebEngineNewWindowRequest* request);
    Q_INVOKABLE void notifyTerminated(int status, int code);
    Q_INVOKABLE void receiveFrame(const QWebEngineFrame& frame) { m_frame = frame; }
    Q_INVOKABLE void attachWebChannel(QWebChannel*);
signals:
    void loadStarted();
    void loadFinished(bool);
    void loadProgress(int);
    void titleChanged(const QString&);
    void urlChanged(const QUrl&);
    void iconChanged(const QIcon&);
    void loadingChanged(const QWebEngineLoadingInfo&);
    void renderProcessTerminated(QWebEnginePage::RenderProcessTerminationStatus, int);
    void newWindowRequested(QWebEngineNewWindowRequest&);
private:
    void call(const char* method, QVariant a = {}, QVariant b = {});
    BrowserProfile* m_profile;
    QPointer<QWebChannel> m_channel;
    QHash<QString, QPointer<QObject>> m_channelObjects;
    QWebEnginePage* m_legacy = nullptr;
    QPointer<QQuickItem> m_quick;
    std::optional<QWebEngineFrame> m_frame;
};

class BrowserSurface : public QWidget, public ScenePaintSource {
    Q_OBJECT
public:
    BrowserSurface(BrowserProfile*, QWidget* parent = nullptr);
    ~BrowserSurface() override;
    BrowserPage* page() const { return m_page; }
    QWebEngineHistory* history() const { return m_page->history(); }
    QUrl url() const { return m_page->url(); }
    QString title() const { return m_page->title(); }
    qreal zoomFactor() const { return m_page->zoomFactor(); }
    void setZoomFactor(qreal v) { m_page->setZoomFactor(v); }
    void setUrl(const QUrl& v) { m_page->load(v); }
    void load(const QUrl& v) { m_page->load(v); }
    void setHtml(const QString& html, const QUrl& base) { m_page->setHtml(html, base); }
    void back() { m_page->back(); } void forward() { m_page->forward(); }
    void reload() { m_page->reload(); } void stop() { m_page->stop(); }
    bool ownsQuickFocus() const;
    void paintScene(QPainter&, const QRegion&) override;
signals:
    void loadStarted(); void loadFinished(bool); void loadProgress(int);
    void titleChanged(const QString&); void urlChanged(const QUrl&); void iconChanged(const QIcon&);
protected:
    void paintEvent(QPaintEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void ensureFallback();
    BrowserPage* m_page;
    QWebEngineView* m_legacyView = nullptr;
    QPointer<QWidget> m_fallback;
    bool m_fallbackPending = false;
};
}
