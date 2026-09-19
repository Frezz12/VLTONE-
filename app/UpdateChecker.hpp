#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QJsonObject;
class QWidget;

namespace account { class Service; }

class UpdateChecker final : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(account::Service* account, QObject* parent = nullptr);

    void start(QWidget* owner);
    static bool isNewerVersionForTest(const QString& available,
                                      const QString& current);
    static QUrl latestReleaseUrlForTest(const QString& apiOrigin,
                                        const QString& platform,
                                        const QString& locale);

private:
    friend class UpdateCheckerTest;
    struct Installer {
        QUrl url;
        QString fileName;
        QByteArray sha256;
        qint64 bytes = 0;
    };
    static Installer installerForRelease(const QJsonObject& release,
                                         const QUrl& releaseUrl,
                                         const QString& platform);
    QString downloadInstaller(QWidget* owner, const QUrl& releaseUrl,
                              const QString& downloads, QString* error);
    static bool closeAndLaunchInstaller(QWidget* owner, const QString& path, QString* error);
    void showUpdate(QWidget* owner, const QString& displayVersion,
                    const QUrl& pageUrl, const QUrl& releaseUrl);

    account::Service* m_account = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    bool m_started = false;
};
