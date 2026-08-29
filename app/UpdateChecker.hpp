#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
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
    account::Service* m_account = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    bool m_started = false;
};
