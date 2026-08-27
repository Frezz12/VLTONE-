#include "MusicClient.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

using json = nlohmann::json;
namespace ai = daw::ai;

namespace ui {

namespace {

/// A stem no request can be blamed for: the slug of what was asked, plus the
/// time, so two takes of the same idea never overwrite each other.
QString stampedName(const QString& stem, const QString& format) {
    const QString when =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    const QString base = stem.isEmpty() ? QStringLiteral("generated") : stem;
    return base + "-" + when + "." + (format.isEmpty() ? QStringLiteral("mp3")
                                                       : format);
}

} // namespace

MusicClient::MusicClient(QObject* parent) : QObject(parent) {}

MusicClient::~MusicClient() { cancel(); }

bool MusicClient::busy() const { return !m_inFlight.isNull(); }

void MusicClient::answer(Outcome outcome) {
    Done callback = std::move(m_onDone);
    m_onDone = nullptr;
    if (callback) callback(std::move(outcome));
}

QString MusicClient::saveAudio(const QByteArray& bytes, const QString& stem,
                               const QString& format, QString& error) const {
    if (bytes.isEmpty()) {
        error = tr("the server sent an empty audio file.");
        return {};
    }
    const QString folder = m_config.folder;
    if (!QDir().mkpath(folder)) {
        error = tr("could not create the folder for generated audio (%1).")
                    .arg(folder);
        return {};
    }
    const QString path = QDir(folder).filePath(stampedName(stem, format));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = tr("could not write %1: %2").arg(path, file.errorString());
        return {};
    }
    if (file.write(bytes) != bytes.size()) {
        error = tr("could not write the whole file to %1.").arg(path);
        file.close();
        file.remove();
        return {};
    }
    file.close();
    return path;
}

void MusicClient::generate(const ai::MusicBrief& brief, Done onDone) {
    cancel();
    m_onDone = std::move(onDone);

    if (m_config.url.trimmed().isEmpty()) {
        answer({{}, 0.0, tr("no music server is configured. Set its URL in "
                            "Settings ▸ AI.")});
        return;
    }
    const QUrl url(m_config.url.trimmed());
    if (!url.isValid() || url.scheme().isEmpty()) {
        answer({{}, 0.0, tr("the music server URL is not a valid address: %1")
                             .arg(m_config.url)});
        return;
    }

    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // Optional by design: a model running on the user's own machine may want no
    // authorization at all, and an empty Bearer header is worse than none.
    if (!m_config.apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_config.apiKey.toUtf8());
    request.setTransferTimeout(m_config.timeoutSeconds * 1000);

    const json body =
        ai::requestJson(brief, m_config.model.toStdString(),
                        m_config.format.toStdString(), m_config.sampleRate,
                        m_config.bitrate);
    const QString stem = QString::fromStdString(ai::slug(brief.prompt));

    QNetworkReply* http =
        m_net.post(request, QByteArray::fromStdString(body.dump()));
    m_inFlight = http;

    connect(http, &QNetworkReply::finished, this, [this, http, stem] {
        http->deleteLater();
        if (m_inFlight != http) return;   // cancelled; the callback is gone
        m_inFlight = nullptr;

        const QByteArray payload = http->readAll();
        const int status =
            http->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        json parsed = json::parse(payload.constData(),
                                  payload.constData() + payload.size(), nullptr,
                                  /*allow_exceptions=*/false);

        if (http->error() != QNetworkReply::NoError || status >= 400) {
            // The server's own words beat Qt's whenever the body carried any.
            QString detail;
            if (!parsed.is_discarded()) {
                const ai::MusicResult result = ai::parseReply(parsed);
                detail = QString::fromStdString(result.error);
            }
            if (detail.isEmpty()) detail = http->errorString();
            if (detail.isEmpty()) detail = tr("the request failed");
            answer({{}, 0.0,
                    status ? tr("%1 (HTTP %2)").arg(detail).arg(status) : detail});
            return;
        }
        if (parsed.is_discarded()) {
            answer({{}, 0.0, tr("the music server sent something that is not "
                                "JSON.")});
            return;
        }

        const ai::MusicResult result = ai::parseReply(parsed);
        if (!result.error.empty()) {
            answer({{}, 0.0, QString::fromStdString(result.error)});
            return;
        }
        const QString format = result.format.empty()
                                   ? m_config.format
                                   : QString::fromStdString(result.format);
        if (!result.audioBytes.empty()) {
            QString error;
            const QString path = saveAudio(
                QByteArray(result.audioBytes.data(),
                           qsizetype(result.audioBytes.size())),
                stem, format, error);
            answer({path, result.seconds, error});
            return;
        }
        fetchAudio(QString::fromStdString(result.audioUrl), stem, result.seconds);
    });
}

void MusicClient::fetchAudio(const QString& url, const QString& stem,
                             double seconds) {
    const QUrl target(url);
    if (!target.isValid()) {
        answer({{}, 0.0, tr("the music server returned an audio link that is "
                            "not a valid address.")});
        return;
    }
    QNetworkRequest request{target};
    request.setTransferTimeout(m_config.timeoutSeconds * 1000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    // The extension the link itself carries is better evidence than the format
    // that was asked for: a server may hand back what it happens to have.
    QString format = QFileInfo(target.path()).suffix().toLower();
    if (format.isEmpty()) format = m_config.format;

    QNetworkReply* http = m_net.get(request);
    m_inFlight = http;
    connect(http, &QNetworkReply::finished, this, [this, http, stem, format,
                                                   seconds] {
        http->deleteLater();
        if (m_inFlight != http) return;
        m_inFlight = nullptr;

        const int status =
            http->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http->error() != QNetworkReply::NoError || status >= 400) {
            QString detail = http->errorString();
            if (detail.isEmpty()) detail = tr("the download failed");
            answer({{}, 0.0,
                    tr("could not download the generated audio: %1").arg(detail)});
            return;
        }
        QString error;
        const QString path = saveAudio(http->readAll(), stem, format, error);
        answer({path, seconds, error});
    });
}

void MusicClient::cancel() {
    m_onDone = nullptr;
    if (QNetworkReply* http = m_inFlight) {
        m_inFlight = nullptr;
        http->abort();
    }
}

} // namespace ui
