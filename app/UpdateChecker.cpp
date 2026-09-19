#include "UpdateChecker.hpp"

#include "AccountService.hpp"
#include "LocalizationManager.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <memory>

namespace {
constexpr qint64 maximumInstallerBytes = 2LL * 1024 * 1024 * 1024;
constexpr qint64 maximumMetadataBytes = 1024 * 1024;

bool safeWebUrl(const QUrl& url) {
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
        (url.scheme() == QStringLiteral("https") ||
         (url.scheme() == QStringLiteral("http") &&
          (url.host() == QStringLiteral("localhost") ||
           url.host() == QStringLiteral("127.0.0.1") ||
           url.host() == QStringLiteral("::1"))));
}

bool successfulReply(QNetworkReply* reply) {
    return reply->error() == QNetworkReply::NoError &&
           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
}

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
    // Application metadata includes labels such as "0.2.3-Build-6"; the
    // latest-release API compares the numeric X.Y.Z release, not build labels.
    const QString currentCore = current.section(QLatin1Char(' '), 0, 0)
                                       .section(QLatin1Char('-'), 0, 0);
    return strictVersion(available, &availableVersion) &&
           strictVersion(currentCore, &currentVersion) &&
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, safeOwner, url] {
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
        if (!safeWebUrl(pageUrl) || !UpdateChecker::isNewerVersionForTest(available, current))
            return;

        const QString displayVersion = response.value(QStringLiteral("display_version"))
                                           .toString(available);
        // The latest endpoint supplies a numeric version, but release routes
        // use the full published name (for example, "0.2.0 Build 1").
        const QUrl releaseUrl = url.resolved(QUrl::fromEncoded(
            QUrl::toPercentEncoding(displayVersion)));
        showUpdate(safeOwner, displayVersion, pageUrl, releaseUrl);
    });
}

UpdateChecker::Installer UpdateChecker::installerForRelease(
    const QJsonObject& release, const QUrl& releaseUrl, const QString& platform) {
    const QString kind = platform == QStringLiteral("windows") ? QStringLiteral("windows-exe")
                       : platform == QStringLiteral("macos") ? QStringLiteral("macos-dmg")
                       : QString();
    if (kind.isEmpty()) return {};
    for (const auto& value : release.value(QStringLiteral("artifacts")).toArray()) {
        const auto artifact = value.toObject();
        if (artifact.value(QStringLiteral("kind")).toString() != kind ||
            artifact.value(QStringLiteral("platform")).toString() != platform) continue;
        const QString digest = artifact.value(QStringLiteral("sha256")).toString();
        static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
        const qint64 bytes = artifact.value(QStringLiteral("bytes")).toInteger();
        if (!sha256Pattern.match(digest).hasMatch() || bytes <= 0 || bytes > maximumInstallerBytes)
            continue;

        QUrl downloadUrl(artifact.value(QStringLiteral("download_url")).toString());
        if (downloadUrl.isEmpty()) continue;
        // Public metadata uses /v1/... even when the API is hosted at /api/v1.
        if (downloadUrl.isRelative() && downloadUrl.path().startsWith(QStringLiteral("/v1/"))) {
            const int versionRoot = releaseUrl.path().lastIndexOf(QStringLiteral("/v1/"));
            if (versionRoot < 0) continue;
            downloadUrl.setPath(releaseUrl.path().left(versionRoot) + downloadUrl.path());
        }
        downloadUrl = releaseUrl.resolved(downloadUrl);
        if (!safeWebUrl(downloadUrl) ||
            (releaseUrl.scheme() == QStringLiteral("https") && downloadUrl.scheme() != QStringLiteral("https")))
            continue;
        // Never use a server-provided filename as a local path or command.
        return {downloadUrl, platform == QStringLiteral("windows")
                                ? QStringLiteral("VLTONE-Setup.exe") : QStringLiteral("VLTONE.dmg"),
                QByteArray::fromHex(digest.toLatin1()), bytes};
    }
    return {};
}

QString UpdateChecker::downloadInstaller(QWidget* owner, const QUrl& releaseUrl,
                                         const QString& downloads, QString* error) {
    error->clear();
    if (!safeWebUrl(releaseUrl)) {
        *error = tr("The installer download address is invalid.");
        return {};
    }
    if (downloads.isEmpty() || !QDir().mkpath(downloads)) {
        *error = tr("The Downloads folder is not writable.");
        return {};
    }
    QTemporaryDir directory(downloads + QStringLiteral("/VLTONE-Update-XXXXXX"));
    if (!directory.isValid()) {
        *error = tr("The Downloads folder is not writable.");
        return {};
    }

    QDialog dialog(owner);
    dialog.setWindowTitle(tr("Downloading update"));
    dialog.setMinimumWidth(420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(tr("Preparing download…"), &dialog);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    layout->addWidget(label);
    auto* progress = new QProgressBar(&dialog);
    progress->setAccessibleName(tr("Download progress"));
    progress->setRange(0, 0);
    layout->addWidget(progress);
    auto* buttons = new QDialogButtonBox(&dialog);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    Installer installer;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::unique_ptr<QSaveFile> file;
    QByteArray metadata;
    qint64 received = 0;
    QString result;
    QPointer<QNetworkReply> activeReply;
    const auto request = [&](const QUrl& url, int timeout) {
        QNetworkRequest req(url);
        req.setTransferTimeout(timeout);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        req.setRawHeader("Accept-Encoding", "identity");
        auto* reply = m_network->get(req);
        reply->setReadBufferSize(256 * 1024);
        activeReply = reply;
        return reply;
    };
    const auto fail = [&](const QString& message) {
        *error = message;
        dialog.reject();
    };
    const auto readInstaller = [&](QNetworkReply* reply) {
        while (reply->bytesAvailable() > 0 && error->isEmpty()) {
            const QByteArray chunk = reply->read(64 * 1024);
            if (chunk.isEmpty()) break;
            if (received + chunk.size() > installer.bytes) {
                fail(tr("The downloaded installer does not match the published release. Please try again."));
                return;
            }
            if (file->write(chunk) != chunk.size()) {
                fail(tr("Could not save the installer. Check free disk space and try again."));
                return;
            }
            hash.addData(chunk);
            received += chunk.size();
        }
        progress->setValue(static_cast<int>(received * 100 / installer.bytes));
    };

    auto* details = request(releaseUrl, 10'000);
    connect(details, &QIODevice::readyRead, &dialog, [&, details] {
        metadata += details->readAll();
        if (metadata.size() > maximumMetadataBytes)
            fail(tr("The release information is invalid. Please try again."));
    });
    connect(details, &QNetworkReply::finished, &dialog, [&, details] {
        details->deleteLater();
        if (!error->isEmpty()) return;
        metadata += details->readAll();
        if (!successfulReply(details)) {
            fail(tr("Could not retrieve the installer. Check your connection and try again."));
            return;
        }
        if (metadata.size() > maximumMetadataBytes) {
            fail(tr("The release information is invalid. Please try again."));
            return;
        }
        installer = installerForRelease(QJsonDocument::fromJson(metadata).object(), releaseUrl, updatePlatform());
        if (installer.url.isEmpty()) {
            fail(tr("No compatible installer is available. Open the release page in your browser."));
            return;
        }
        file = std::make_unique<QSaveFile>(directory.filePath(installer.fileName));
        if (!file->open(QIODevice::WriteOnly)) {
            fail(tr("Could not save the installer. Check free disk space and try again."));
            return;
        }
        label->setText(tr("Downloading the installer… VLTONE will close when the download is complete."));
        progress->setRange(0, 100);
        progress->setValue(0);
        auto* download = request(installer.url, 30'000);
        connect(download, &QIODevice::readyRead, &dialog, [&, download] { readInstaller(download); });
        connect(download, &QNetworkReply::finished, &dialog, [&, download] {
            download->deleteLater();
            if (!error->isEmpty()) return;
            readInstaller(download);
            if (!error->isEmpty()) return;
            if (!successfulReply(download)) {
                fail(tr("The download failed. Check your connection and try again."));
                return;
            }
            if (received != installer.bytes || hash.result() != installer.sha256) {
                fail(tr("The downloaded installer does not match the published release. Please try again."));
                return;
            }
            if (!file->commit()) {
                fail(tr("Could not save the installer. Check free disk space and try again."));
                return;
            }
            result = file->fileName();
            dialog.accept();
        });
    });
    const int outcome = dialog.exec();
    // Disconnect before abort: abort() can synchronously emit finished().
    if (activeReply) {
        activeReply->disconnect(&dialog);
        if (!activeReply->isFinished()) activeReply->abort();
        activeReply->deleteLater();
    }
    file.reset();
    if (outcome != QDialog::Accepted) return {};
    directory.setAutoRemove(false); // Keep the verified installer after VLTONE exits.
    return result;
}

bool UpdateChecker::closeAndLaunchInstaller(QWidget* owner, const QString& path, QString* error) {
    error->clear();
    // close() runs the usual save/cancel flow. Keep the event loop alive if the
    // OS refuses the installer (including a declined Windows elevation prompt).
    const bool quitOnClose = QApplication::quitOnLastWindowClosed();
    QApplication::setQuitOnLastWindowClosed(false);
    const bool closed = owner->close();
    const bool launched = closed && QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    if (closed && !launched) {
        owner->show();
        *error = tr("Could not open the installer. You can open it manually:\n%1").arg(QDir::toNativeSeparators(path));
    }
    QApplication::setQuitOnLastWindowClosed(quitOnClose);
    return launched;
}

void UpdateChecker::showUpdate(QWidget* owner, const QString& displayVersion,
                               const QUrl& pageUrl, const QUrl& releaseUrl) {
    QString error;
    QString installerPath;
    while (true) {
        QMessageBox prompt(owner);
        prompt.setTextFormat(Qt::PlainText);
        prompt.setIcon(error.isEmpty() ? QMessageBox::Information : QMessageBox::Warning);
        prompt.setWindowTitle(error.isEmpty() ? tr("Update available") : tr("Update failed"));
        prompt.setText(error.isEmpty() ? tr("VLTONE %1 is available.").arg(displayVersion) : error);
        prompt.setInformativeText(tr("You are using %1. Download the installer and VLTONE will close to let you install the update.")
                                      .arg(QCoreApplication::applicationVersion()));
        QPushButton* download = nullptr;
        if (updatePlatform() == QStringLiteral("windows") || updatePlatform() == QStringLiteral("macos")) {
            download = prompt.addButton(error.isEmpty() ? tr("Download") : tr("Try again"), QMessageBox::AcceptRole);
            prompt.setDefaultButton(download);
        }
        auto* browser = prompt.addButton(tr("Open in browser"), QMessageBox::ActionRole);
        auto* later = prompt.addButton(tr("Later"), QMessageBox::RejectRole);
        prompt.setEscapeButton(later);
        prompt.exec();
        if (prompt.clickedButton() == browser) {
            if (QDesktopServices::openUrl(pageUrl)) return;
            error = tr("Could not open the browser. Please try again.");
            continue;
        }
        if (!download || prompt.clickedButton() != download) return;
        if (installerPath.isEmpty()) {
            installerPath = downloadInstaller(owner, releaseUrl,
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), &error);
        }
        if (installerPath.isEmpty()) {
            if (error.isEmpty()) return; // Download cancelled.
            continue;
        }
        if (closeAndLaunchInstaller(owner, installerPath, &error)) {
            QApplication::quit();
            return;
        }
        if (error.isEmpty()) return; // Closing was cancelled; keep the saved installer.
    }
}
