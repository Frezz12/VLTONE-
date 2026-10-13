#pragma once
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QByteArray>
#include <QList>
#include <atomic>
#include <memory>
class WebBrowserPanel;
class QTemporaryDir;
class QJsonObject;
class QWebEngineCookieStore;
class QNetworkAccessManager;
class QWebEngineDownloadRequest;
namespace account { class Service; }
namespace ui {
class ForumBrowserBridge final : public QObject {
    Q_OBJECT
public:
    ForumBrowserBridge(WebBrowserPanel*, account::Service*);
    ~ForumBrowserBridge() override;
private:
    void exportPackage();
    void syncAccount(bool explicitLogin = false);
    void clearCookie();
    void installDownloaded(QWebEngineDownloadRequest*, const QString& path);
    void runInstaller(const QString&, const QJsonObject&, std::shared_ptr<QTemporaryDir>, bool replace = false);
    WebBrowserPanel* m_panel;
    QPointer<account::Service> m_account;
    QNetworkAccessManager* m_network;
    QWebEngineCookieStore* m_cookies;
    QUrl m_origin;
    QByteArray m_cookie;
    QString m_accountKey;
    quint64 m_generation = 0;
    bool m_ready = false;
    QList<std::shared_ptr<std::atomic_bool>> m_cancellations;
};
}
