#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>

namespace {
constexpr int kPollIntervalMs = 2500;
constexpr int kCrashVerdictGraceMs = 1500;
constexpr int kShutdownBudgetMs = 30000;
constexpr int kRequestTimeoutMs = 15000;
constexpr int kRetryIntervalMs = 5000;
}

class Reporter final : public QObject {
public:
    Reporter(QString outbox, QString ipcName, QString origin,
             QString recovery, QString sessionId, QString appVersion,
             QString buildId, QObject* parent = nullptr)
        : QObject(parent), m_outbox(std::move(outbox)), m_origin(std::move(origin)),
          m_recovery(std::move(recovery)), m_sessionId(std::move(sessionId)),
          m_appVersion(std::move(appVersion)), m_buildId(std::move(buildId)) {
        appendReporterLog(QStringLiteral("reporter_started"));
        connect(&m_socket, &QLocalSocket::readyRead, this, [this] {
            m_tokenBuffer += m_socket.readAll();
            for (;;) {
                const int newline = m_tokenBuffer.indexOf('\n');
                if (newline < 0) break;
                const QString next = QString::fromUtf8(
                    m_tokenBuffer.left(newline)).trimmed();
                m_tokenBuffer.remove(0, newline + 1);
                if (!next.isEmpty() && next != m_token) {
                    m_token = next;
                    appendReporterLog(QStringLiteral("reporter_token_updated"));
                }
            }
            if (!m_token.isEmpty()) {
                m_timer.start(kPollIntervalMs);
                drain();
            }
        });
        connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
            if (m_parentGone) return;
            m_parentGone = true;
            m_parentGoneAt = QDateTime::currentDateTimeUtc();
            m_normalEnded = outboxHasNormalEnd();
            appendReporterLog(m_normalEnded
                ? QStringLiteral("parent_disconnected_after_normal_end")
                : QStringLiteral("parent_disconnected_unexpectedly"));
            QTimer::singleShot(kCrashVerdictGraceMs, this, [this] {
                m_crashGraceElapsed = true;
                if (!m_normalEnded && !m_crashStaged) stageCrash();
                drain();
            });
            QTimer::singleShot(kShutdownBudgetMs, qApp, &QCoreApplication::quit);
            drain();
        });
        connect(&m_timer, &QTimer::timeout, this, &Reporter::drain);
        m_socket.connectToServer(ipcName, QIODevice::ReadOnly);
        QTimer::singleShot(5000, this, [this] {
            if (m_token.isEmpty()) QCoreApplication::exit(2);
        });
    }

private:
    void drain() {
        if (m_busy || m_token.isEmpty()) return;
        QDir directory(m_outbox);
        // A crash is actionable and must never wait behind a stale telemetry
        // item. This also drains crash reports left by an offline prior run as
        // soon as a fresh token becomes available.
        const QFileInfoList crashes = directory.entryInfoList(
            {QStringLiteral("*.crash.json")}, QDir::Files, QDir::Name);
        if (!crashes.isEmpty()) {
            uploadCrash(crashes.front().absoluteFilePath());
            return;
        }
        const QFileInfoList telemetry = directory.entryInfoList(
            {QStringLiteral("*.telemetry.json")}, QDir::Files, QDir::Name);
        if (!telemetry.isEmpty()) {
            uploadTelemetry(telemetry.front().absoluteFilePath());
            return;
        }
        if (m_parentGone && m_crashGraceElapsed && !m_normalEnded && !m_crashStaged) {
            stageCrash();
            drain();
            return;
        }
        if (m_parentGone && (m_normalEnded || m_crashGraceElapsed))
            QCoreApplication::quit();
    }

    bool bodyHasNormalEnd(const QJsonObject& body) const {
        for (const QJsonValue& value : body.value(QStringLiteral("events")).toArray()) {
            const QJsonObject event = value.toObject();
            if (event.value(QStringLiteral("session_id")).toString() == m_sessionId &&
                event.value(QStringLiteral("kind")).toString() == QLatin1String("session_ended"))
                return true;
        }
        return false;
    }

    bool outboxHasNormalEnd() const {
        const QFileInfoList telemetry = QDir(m_outbox).entryInfoList(
            {QStringLiteral("*.telemetry.json")}, QDir::Files, QDir::Name);
        for (const QFileInfo& item : telemetry) {
            QFile file(item.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonObject body = QJsonDocument::fromJson(file.readAll())
                                         .object().value(QStringLiteral("body")).toObject();
            if (bodyHasNormalEnd(body)) return true;
        }
        return false;
    }

    void uploadTelemetry(const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { QFile::remove(path); drain(); return; }
        const QJsonDocument envelope = QJsonDocument::fromJson(file.readAll());
        file.close();
        const QJsonObject body = envelope.object().value(QStringLiteral("body")).toObject();
        if (bodyHasNormalEnd(body)) m_normalEnded = true;
        QNetworkRequest request(QUrl(m_origin + QStringLiteral("/desktop/telemetry/batch")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
        request.setTransferTimeout(kRequestTimeoutMs);
        m_busy = true;
        QNetworkReply* reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, path] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool success = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
            const QString error = reply->errorString();
            reply->deleteLater(); m_busy = false;
            if (success) {
                QFile::remove(path);
                drain();
                return;
            }
            appendReporterLog(QStringLiteral("telemetry_upload_failed status=%1 error=%2")
                                  .arg(status).arg(error.left(160)));
            // Invalid local data is a poison item: retrying it forever hides
            // every healthy sample after it. Preserve it for inspection under
            // a non-queue suffix and continue.
            if (status == 400 || status == 413 || status == 422) {
                quarantine(path);
                drain();
                return;
            }
            QTimer::singleShot(kRetryIntervalMs, this, &Reporter::drain);
        });
    }

    QByteArray limitedFile(const QString& path, qint64 maxBytes) const {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        if (file.size() > maxBytes) file.seek(file.size() - maxBytes);
        return file.read(maxBytes);
    }

    QString redactedText(const QByteArray& input) const {
        QString text = QString::fromUtf8(input);
        static const QRegularExpression pathPattern(
            QStringLiteral(R"((?:[A-Za-z]:[\\/]|/)[^\s\]\)\}\"']+)"));
        text.replace(pathPattern, QStringLiteral("<path-redacted>"));
        return text;
    }

    void appendReporterLog(const QString& message) const {
        const QString path = QDir(m_outbox).filePath(QStringLiteral("reporter.log"));
        QFile file(path);
        if (file.exists() && file.size() > 512 * 1024) {
            const QString rotated = path + QStringLiteral(".1");
            QFile::remove(rotated);
            QFile::rename(path, rotated);
        }
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
        const QByteArray line = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8()
            + ' ' + redactedText(message.toUtf8()).toUtf8() + '\n';
        file.write(line);
        file.flush();
    }

    void quarantine(const QString& path) const {
        const QString rejected = path + QStringLiteral(".rejected");
        QFile::remove(rejected);
        if (!QFile::rename(path, rejected)) QFile::remove(path);
        QFileInfoList rejectedFiles = QDir(m_outbox).entryInfoList(
            {QStringLiteral("*.rejected")}, QDir::Files, QDir::Time | QDir::Reversed);
        while (rejectedFiles.size() > 20)
            QFile::remove(rejectedFiles.takeFirst().absoluteFilePath());
    }

    QString markerField(const QByteArray& marker, const QByteArray& name) const {
        const QByteArray prefix = name + '=';
        for (const QByteArray& rawLine : marker.split('\n')) {
            if (rawLine.startsWith(prefix))
                return redactedText(rawLine.mid(prefix.size())).trimmed().left(512);
        }
        return {};
    }

    QJsonArray parsedHealthSamples(const QByteArray& health) const {
        QJsonArray samples;
        const QList<QByteArray> lines = health.split('\n');
        const qsizetype first = std::max<qsizetype>(0, lines.size() - 600);
        for (qsizetype index = first; index < lines.size(); ++index) {
            const QJsonObject raw = QJsonDocument::fromJson(lines.at(index)).object();
            if (raw.isEmpty()) continue;
            const qint64 unixMs = raw.value(QStringLiteral("unixMs")).toInteger();
            QJsonObject sample{
                {QStringLiteral("recorded_at"), unixMs > 0
                    ? QDateTime::fromMSecsSinceEpoch(unixMs, QTimeZone::UTC).toString(Qt::ISODateWithMs)
                    : QString()},
                {QStringLiteral("process_cpu"), raw.value(QStringLiteral("processCpu"))},
                {QStringLiteral("system_cpu"), raw.value(QStringLiteral("systemCpu"))},
                {QStringLiteral("dsp_load"), raw.value(QStringLiteral("dspLoad")).toDouble() * 100.0},
                {QStringLiteral("dsp_peak"), raw.value(QStringLiteral("dspLoadPeak")).toDouble() * 100.0},
                {QStringLiteral("xruns"), raw.value(QStringLiteral("xruns"))},
                {QStringLiteral("resident_bytes"), raw.value(QStringLiteral("residentBytes"))},
                {QStringLiteral("sample_rate"), raw.value(QStringLiteral("sampleRate"))},
                {QStringLiteral("buffer_frames"), raw.value(QStringLiteral("bufferFrames"))},
                {QStringLiteral("track_count"), raw.value(QStringLiteral("trackCount"))},
                {QStringLiteral("clip_count"), raw.value(QStringLiteral("clipCount"))},
                {QStringLiteral("heartbeat"), raw.value(QStringLiteral("heartbeat"))},
                {QStringLiteral("playback_state"), raw.value(QStringLiteral("recording")).toBool()
                    ? QStringLiteral("recording")
                    : raw.value(QStringLiteral("playing")).toBool()
                        ? QStringLiteral("playing") : QStringLiteral("stopped")},
                {QStringLiteral("recording"), raw.value(QStringLiteral("recording"))},
                {QStringLiteral("last_plugin"), redactedText(
                    raw.value(QStringLiteral("lastPlugin")).toString().toUtf8()).left(160)},
            };
            samples.append(sample);
        }
        return samples;
    }

    QJsonArray parsedModules(const QByteArray& marker) const {
        QJsonArray modules;
        QSet<QString> seen;
        static const QRegularExpression frame(
            QStringLiteral(R"(^\s*\d+\s+(\S+)\s+(0x[0-9a-fA-F]+))"));
        for (const QString& line : QString::fromUtf8(marker).split('\n')) {
            const QRegularExpressionMatch match = frame.match(line);
            if (!match.hasMatch()) continue;
            QString name = match.captured(1);
            if (name.contains('/') || name.contains('\\')) name = QFileInfo(name).fileName();
            name = redactedText(name.toUtf8()).left(160);
            if (name.isEmpty() || seen.contains(name)) continue;
            seen.insert(name);
            modules.append(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("version"), QString()},
                {QStringLiteral("base_address"), match.captured(2)},
            });
            if (modules.size() >= 256) break;
        }
        return modules;
    }

    void stageCrash() {
        m_crashStaged = true;
        appendReporterLog(QStringLiteral("crash_staging_started"));
        const QByteArray marker = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("crash.txt")), 256 * 1024);
        const QByteArray health = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("health.jsonl")), 2 * 1024 * 1024);
        const QByteArray session = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("session.json")), 512 * 1024);
        const QByteArray structuredLog = limitedFile(
            QDir(m_recovery).filePath(QStringLiteral("app.log")), 2 * 1024 * 1024);
        const QByteArray reporterLog = limitedFile(
            QDir(m_outbox).filePath(QStringLiteral("reporter.log")), 512 * 1024);
        const QString redactedMarker = redactedText(marker);
        const QJsonObject rawSession = QJsonDocument::fromJson(session).object();
        const QJsonObject rawStats = rawSession.value(QStringLiteral("stats")).toObject();
        QString lastPlugin = markerField(marker, QByteArrayLiteral("plugin"));
        if (lastPlugin.isEmpty()) lastPlugin = markerField(marker, QByteArrayLiteral("last_plugin"));
        if (lastPlugin.isEmpty()) lastPlugin = redactedText(
            rawStats.value(QStringLiteral("lastPlugin")).toString().toUtf8()).left(160);
        const QString signal = markerField(marker, QByteArrayLiteral("signal"));
        const QString exceptionCode = markerField(marker, QByteArrayLiteral("exception_code"));
        const QString exception = markerField(marker, QByteArrayLiteral("exception"));
        QString reason = redactedText(
            rawSession.value(QStringLiteral("crashReason")).toString().toUtf8()).left(512);
        if (reason.isEmpty() && !signal.isEmpty())
            reason = lastPlugin.isEmpty()
                ? QStringLiteral("crashed (%1)").arg(signal.section(' ', 0, 0))
                : QStringLiteral("crashed in %1 (%2)").arg(lastPlugin, signal.section(' ', 0, 0));
        if (reason.isEmpty() && !exception.isEmpty()) reason = exception;
        if (reason.isEmpty() && rawSession.value(QStringLiteral("outcome")).toString() == QLatin1String("hung"))
            reason = QStringLiteral("application_hung");
        if (reason.isEmpty()) reason = QStringLiteral("process_terminated_unexpectedly");
        const QJsonObject safeStats{
            {QStringLiteral("processCpu"), rawStats.value(QStringLiteral("processCpu"))},
            {QStringLiteral("systemCpu"), rawStats.value(QStringLiteral("systemCpu"))},
            {QStringLiteral("dspLoad"), rawStats.value(QStringLiteral("dspLoad"))},
            {QStringLiteral("dspLoadPeak"), rawStats.value(QStringLiteral("dspLoadPeak"))},
            {QStringLiteral("xruns"), rawStats.value(QStringLiteral("xruns"))},
            {QStringLiteral("residentBytes"), rawStats.value(QStringLiteral("residentBytes"))},
            {QStringLiteral("sampleRate"), rawStats.value(QStringLiteral("sampleRate"))},
            {QStringLiteral("bufferFrames"), rawStats.value(QStringLiteral("bufferFrames"))},
            {QStringLiteral("trackCount"), rawStats.value(QStringLiteral("trackCount"))},
            {QStringLiteral("clipCount"), rawStats.value(QStringLiteral("clipCount"))},
            {QStringLiteral("playing"), rawStats.value(QStringLiteral("playing"))},
            {QStringLiteral("recording"), rawStats.value(QStringLiteral("recording"))},
            {QStringLiteral("lastPlugin"), lastPlugin},
        };
        const QJsonObject safeSession{
            {QStringLiteral("format"), rawSession.value(QStringLiteral("format"))},
            {QStringLiteral("version"), rawSession.value(QStringLiteral("version"))},
            {QStringLiteral("app"), rawSession.value(QStringLiteral("app"))},
            {QStringLiteral("startedUnixMs"), rawSession.value(QStringLiteral("startedUnixMs"))},
            {QStringLiteral("heartbeat"), rawSession.value(QStringLiteral("heartbeat"))},
            {QStringLiteral("heartbeatUnixMs"), rawSession.value(QStringLiteral("heartbeatUnixMs"))},
            {QStringLiteral("journalUnixMs"), rawSession.value(QStringLiteral("journalUnixMs"))},
            {QStringLiteral("stats"), safeStats},
            {QStringLiteral("outcome"), rawSession.value(QStringLiteral("outcome"))},
            {QStringLiteral("crashReason"), redactedText(
                rawSession.value(QStringLiteral("crashReason")).toString().toUtf8()).left(512)},
        };
        const QString reportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject metadata{
            {QStringLiteral("report_id"), reportId},
            {QStringLiteral("session_id"), m_sessionId},
            {QStringLiteral("build_id"), m_buildId},
            {QStringLiteral("app_version"), m_appVersion},
#if defined(Q_OS_WIN)
            {QStringLiteral("platform"), QStringLiteral("windows")},
#else
            {QStringLiteral("platform"), QStringLiteral("macos")},
#endif
            {QStringLiteral("reason"), reason.left(512)},
            {QStringLiteral("last_plugin"), lastPlugin},
            {QStringLiteral("occurred_at"), (m_parentGoneAt.isValid()
                ? m_parentGoneAt : QDateTime::currentDateTimeUtc()).toString(Qt::ISODateWithMs)},
            {QStringLiteral("exception_code"), exceptionCode.isEmpty() ? exception : exceptionCode},
            {QStringLiteral("signal"), signal},
            {QStringLiteral("health_samples"), parsedHealthSamples(health)},
            {QStringLiteral("modules"), parsedModules(marker)},
        };
        QByteArray artifact("VLT Studio Pro crash diagnostics\nformat=vlt-crash-log-1\n\n");
        const auto appendSection = [&artifact](const char* name, const QByteArray& body) {
            artifact += '['; artifact += name; artifact += "]\n"; artifact += body;
            if (!artifact.endsWith('\n')) artifact += '\n';
            artifact += '\n';
        };
        appendSection("metadata", QJsonDocument(metadata).toJson(QJsonDocument::Indented));
        appendSection("crash marker", redactedMarker.toUtf8());
        appendSection("session", QJsonDocument(safeSession).toJson(QJsonDocument::Indented));
        appendSection("health", redactedText(health).toUtf8());
        appendSection("application", redactedText(structuredLog).toUtf8());
        appendSection("reporter", redactedText(reporterLog).toUtf8());
        const QString artifactName = QStringLiteral("crash.log");
        const QString artifactType = QStringLiteral("text/plain; charset=utf-8");
        const QString stem = QStringLiteral("%1-%2")
            .arg(QDateTime::currentMSecsSinceEpoch(), 16, 10, QLatin1Char('0'))
            .arg(reportId);
        const QString artifactFile = stem + QStringLiteral(".crash.log");
        QSaveFile storedArtifact(QDir(m_outbox).filePath(artifactFile));
        if (!storedArtifact.open(QIODevice::WriteOnly) ||
            storedArtifact.write(artifact) != artifact.size() || !storedArtifact.commit()) return;
        const QJsonObject envelope{
            {QStringLiteral("metadata"), metadata},
            {QStringLiteral("artifact_file"), artifactFile},
            {QStringLiteral("artifact_name"), artifactName},
            {QStringLiteral("artifact_type"), artifactType},
        };
        QSaveFile storedEnvelope(QDir(m_outbox).filePath(stem + QStringLiteral(".crash.json")));
        if (!storedEnvelope.open(QIODevice::WriteOnly) ||
            storedEnvelope.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact)) < 0 ||
            !storedEnvelope.commit()) {
            QFile::remove(QDir(m_outbox).filePath(artifactFile));
            return;
        }
        trimCrashes();
    }

    void trimCrashes() {
        QDir directory(m_outbox);
        QFileInfoList envelopes = directory.entryInfoList(
            {QStringLiteral("*.crash.json")}, QDir::Files, QDir::Name);
        while (envelopes.size() > 5) {
            QFile file(envelopes.front().absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
                const QString artifact = object.value(QStringLiteral("artifact_file")).toString();
                if (QFileInfo(artifact).fileName() == artifact)
                    QFile::remove(directory.filePath(artifact));
            }
            QFile::remove(envelopes.takeFirst().absoluteFilePath());
        }
    }

    void uploadCrash(const QString& envelopePath) {
        QFile envelopeFile(envelopePath);
        if (!envelopeFile.open(QIODevice::ReadOnly)) { QFile::remove(envelopePath); drain(); return; }
        const QJsonObject envelope = QJsonDocument::fromJson(envelopeFile.readAll()).object();
        const QString artifactNameOnDisk = envelope.value(QStringLiteral("artifact_file")).toString();
        if (QFileInfo(artifactNameOnDisk).fileName() != artifactNameOnDisk) {
            QFile::remove(envelopePath); drain(); return;
        }
        const QString artifactPath = QDir(m_outbox).filePath(artifactNameOnDisk);
        QFile artifactFile(artifactPath);
        if (envelope.isEmpty() || !artifactFile.open(QIODevice::ReadOnly) ||
            artifactFile.size() > 50LL * 1024 * 1024) {
            QFile::remove(envelopePath); QFile::remove(artifactPath); drain(); return;
        }
        auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        QHttpPart metadataPart;
        metadataPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                               QVariant(QStringLiteral("form-data; name=\"metadata\"")));
        metadataPart.setBody(QJsonDocument(envelope.value(QStringLiteral("metadata")).toObject())
                                 .toJson(QJsonDocument::Compact));
        multipart->append(metadataPart);
        QHttpPart artifactPart;
        artifactPart.setHeader(QNetworkRequest::ContentDispositionHeader,
            QVariant(QStringLiteral("form-data; name=\"artifact\"; filename=\"%1\"")
                         .arg(envelope.value(QStringLiteral("artifact_name")).toString())));
        artifactPart.setHeader(QNetworkRequest::ContentTypeHeader,
                               envelope.value(QStringLiteral("artifact_type")).toString());
        artifactPart.setBody(artifactFile.readAll());
        multipart->append(artifactPart);
        QNetworkRequest request(QUrl(m_origin + QStringLiteral("/desktop/crashes")));
        request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
        request.setTransferTimeout(kRequestTimeoutMs);
        m_busy = true;
        QNetworkReply* reply = m_network.post(request, multipart);
        multipart->setParent(reply);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, envelopePath, artifactPath] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool success = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
            const QString error = reply->errorString();
            reply->deleteLater(); m_busy = false;
            if (success) {
                appendReporterLog(QStringLiteral("crash_upload_succeeded"));
                QFile::remove(envelopePath); QFile::remove(artifactPath); drain();
                return;
            }
            appendReporterLog(QStringLiteral("crash_upload_failed status=%1 error=%2")
                                  .arg(status).arg(error.left(160)));
            if (status == 400 || status == 413 || status == 422) {
                quarantine(envelopePath);
                quarantine(artifactPath);
                drain();
                return;
            }
            QTimer::singleShot(kRetryIntervalMs, this, &Reporter::drain);
        });
    }

    QString m_outbox, m_origin, m_recovery, m_sessionId, m_appVersion, m_buildId;
    QLocalSocket m_socket;
    QNetworkAccessManager m_network;
    QTimer m_timer;
    QByteArray m_tokenBuffer;
    QString m_token;
    QDateTime m_parentGoneAt;
    bool m_parentGone = false;
    bool m_normalEnded = false;
    bool m_crashStaged = false;
    bool m_crashGraceElapsed = false;
    bool m_busy = false;
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("daw_reporter"));
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption outbox(QStringLiteral("outbox"), {}, QStringLiteral("path"));
    const QCommandLineOption ipc(QStringLiteral("ipc"), {}, QStringLiteral("name"));
    const QCommandLineOption origin(QStringLiteral("origin"), {}, QStringLiteral("url"));
    const QCommandLineOption recovery(QStringLiteral("recovery"), {}, QStringLiteral("path"));
    const QCommandLineOption session(QStringLiteral("session"), {}, QStringLiteral("uuid"));
    const QCommandLineOption version(QStringLiteral("app-version"), {}, QStringLiteral("version"));
    const QCommandLineOption build(QStringLiteral("build-id"), {}, QStringLiteral("id"));
    parser.addOptions({outbox, ipc, origin, recovery, session, version, build});
    parser.process(app);
    if (parser.value(outbox).isEmpty() || parser.value(ipc).isEmpty() ||
        parser.value(origin).isEmpty() || parser.value(session).isEmpty()) return 2;
    Reporter reporter(parser.value(outbox), parser.value(ipc), parser.value(origin),
                      parser.value(recovery), parser.value(session),
                      parser.value(version), parser.value(build));
    return app.exec();
}
