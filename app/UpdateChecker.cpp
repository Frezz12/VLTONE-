#include "UpdateChecker.hpp"

#include "AccountService.hpp"
#include "LocalizationManager.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QUrlQuery>
#include <QVersionNumber>

namespace {
QString updatePlatform() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

bool strictVersion(const QString& text, QVersionNumber* out) {
    int suffix = 0;
    const QVersionNumber parsed = QVersionNumber::fromString(text, &suffix);
    if (suffix != text.size() || parsed.segmentCount() != 3 ||
        parsed.majorVersion() < 0 || parsed.minorVersion() < 0 ||
        parsed.microVersion() < 0) {
        return false;
    }
    *out = parsed;
    return true;
}
}

UpdateChecker::UpdateChecker(account::Service* account, QObject* parent)
    : QObject(parent), m_account(account),
      m_network(new QNetworkAccessManager(this)) {}

bool UpdateChecker::isNewerVersionForTest(const QString& available,
                                          const QString& current) {
    QVersionNumber availableVersion;
    QVersionNumber currentVersion;
    return strictVersion(available, &availableVersion) &&
           strictVersion(current, &currentVersion) &&
           QVersionNumber::compare(availableVersion, currentVersion) > 0;
}

QUrl UpdateChecker::latestReleaseUrlForTest(const QString& apiOrigin,
                                            const QString& platform,
                                            const QString& locale) {
    QUrl url(apiOrigin + QStringLiteral("/releases/latest"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("platform"), platform);
    query.addQueryItem(QStringLiteral("locale"), locale);
    url.setQuery(query);
    return url;
}

void UpdateChecker::start(QWidget* owner) {
    if (m_started || !m_account || !owner) return;
    m_started = true;

    const QUrl url = latestReleaseUrlForTest(
        m_account->apiOrigin(), updatePlatform(),
        ui::LocalizationManager::instance().websiteLocale());
    QNetworkRequest request(url);
    request.setTransferTimeout(10'000);
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = m_network->get(request);
    const QPointer<QWidget> safeOwner(owner);
    connect(reply, &QNetworkReply::finished, this, [reply, safeOwner] {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const bool success = reply->error() == QNetworkReply::NoError &&
                             status >= 200 && status < 300 && status != 204;
        reply->deleteLater();
        if (!success || !safeOwner) return;

        const QJsonObject response = QJsonDocument::fromJson(body).object();
        const QString available = response.value(QStringLiteral("version")).toString();
        const QUrl pageUrl(response.value(QStringLiteral("page_url")).toString());
        const QString current = QCoreApplication::applicationVersion();
        if (!pageUrl.isValid() || !UpdateChecker::isNewerVersionForTest(available, current))
            return;

        QMessageBox prompt(safeOwner);
        prompt.setIcon(QMessageBox::Information);
        prompt.setWindowTitle(UpdateChecker::tr("Update available"));
        prompt.setText(UpdateChecker::tr("VLT Studio Pro %1 is available.").arg(available));
        prompt.setInformativeText(
            UpdateChecker::tr("You are using %1. The download button opens the release page in your browser.").arg(current));
        QPushButton* download = prompt.addButton(UpdateChecker::tr("Download"), QMessageBox::AcceptRole);
        prompt.addButton(UpdateChecker::tr("Later"), QMessageBox::RejectRole);
        prompt.exec();
        if (prompt.clickedButton() == download) QDesktopServices::openUrl(pageUrl);
    });
}
