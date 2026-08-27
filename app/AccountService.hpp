#pragma once

#include <QJsonObject>
#include <QObject>
#include <QDateTime>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace account {

struct Snapshot {
    QString userId;
    QString email;
    QString nickname;
    QString plan = QStringLiteral("Demo");
    QString deviceId;
    qint64 tokenLimit = 20'000'000;
    qint64 tokensUsed = 0;
    qint64 tokensRemaining = 20'000'000;
    QDateTime cycleEndsAt;
    QDateTime lastSyncAt;
    bool offline = false;
};

class Service final : public QObject {
    Q_OBJECT
public:
    explicit Service(QObject* parent = nullptr);

    static Service* instance();
    const Snapshot& snapshot() const { return m_snapshot; }
    bool authenticated() const { return m_authenticated; }
    QString accessToken() const { return m_accessToken; }
    QString reporterToken() const { return m_reporterToken; }
    QString apiOrigin() const { return m_apiOrigin; }
    QString installationId() const;

    void beginRestore();
    void login(const QString& email, const QString& password);
    void logout();
    void installHeadlessTestSession();
    /// Refresh the public list of administrator-managed AI models. Provider
    /// URLs and keys are issued only for one checked request and are never
    /// persisted in this list.
    void refreshAiModels();
    /// Pull the current server-side AI usage after a managed model request.
    void refreshQuota();

signals:
    void busyChanged(bool busy);
    void authenticatedChanged(bool authenticated);
    void authenticationRequired(const QString& reason, bool offlineExpired);
    void errorOccurred(const QString& code, const QString& message);
    void snapshotChanged();
    void aiModelsChanged();
    void logoutFinished();

private:
    void handleSessionReply(QNetworkReply* reply, bool refresh);
    void acceptSession(const QJsonObject& response);
    bool acceptOffline(const QJsonObject& credentials, QString* reason);
    void persistCredentials(const QJsonObject& response);
    void applyQuota(const QJsonObject& quota);
    void finishLogout();
    void setBusy(bool busy);
    QNetworkReply* postJson(const QString& path, const QJsonObject& body,
                            const QString& bearer = {});

    static Service* s_instance;
    QNetworkAccessManager* m_network = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QString m_apiOrigin;
    QString m_accessToken;
    QString m_refreshToken;
    QString m_reporterToken;
    QString m_offlineEntitlement;
    QString m_publicKey;
    qint64 m_lastServerTime = 0;
    Snapshot m_snapshot;
    bool m_authenticated = false;
    bool m_busy = false;
};

} // namespace account
