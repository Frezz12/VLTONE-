#include "TelemetryClient.hpp"

#include "AccountService.hpp"
#include "MainWindow.hpp"
#include "PlatformDiagnostics.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace {
QString reporterPath() {
    QDir directory(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("daw_reporter.exe");
#else
    const QString name = QStringLiteral("daw_reporter");
#endif
    const QString path = directory.filePath(name);
    return QFile::exists(path) ? path : QString();
}
}

TelemetryClient::TelemetryClient(MainWindow* window, QObject* parent)
    : QObject(parent), m_window(window), m_ipc(new QLocalServer(this)),
      m_sampleTimer(new QTimer(this)), m_reporterRetryTimer(new QTimer(this)),
      m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    m_outbox = QDir(base).filePath(QStringLiteral("diagnostics-outbox"));
    QDir().mkpath(m_outbox);

    m_reporterRetryTimer->setSingleShot(true);
    connect(m_reporterRetryTimer, &QTimer::timeout, this, &TelemetryClient::startReporter);
    connect(m_ipc, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = m_ipc->nextPendingConnection()) {
            ++m_reporterConnections;
            m_reporterStarting = false;
            m_reporterRetryTimer->stop();
            logReporterStatus(QStringLiteral("connected"));
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                m_reporterConnections = std::max(0, m_reporterConnections - 1);
                socket->deleteLater();
                if (!m_shuttingDown) {
                    logReporterStatus(QStringLiteral("disconnected"));
                    scheduleReporterRetry(1000);
                }
            });
            sendReporterToken();
        }
    });
    if (auto* service = account::Service::instance()) {
        connect(service, &account::Service::authenticatedChanged, this,
                [this](bool authenticated) {
            if (!authenticated) return;
            sendReporterToken();
            startReporter();
        });
    }
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        m_shuttingDown = true;
        m_reporterRetryTimer->stop();
        if (m_sampleCount) flushSample();
        enqueue(QStringLiteral("session_ended"), {{QStringLiteral("reason"), QStringLiteral("normal")}});
        appendDiagnosticLog(QStringLiteral("session_ended"));
    });

    startReporter();
    enqueue(QStringLiteral("session_started"), {
        {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("build_id"), QCoreApplication::applicationVersion()},
        {QStringLiteral("hardware"), PlatformDiagnostics::hardwareSnapshot()},
    });
    appendDiagnosticLog(QStringLiteral("session_started"), {
        {QStringLiteral("session_id"), m_sessionId},
        {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
    });
    m_sampleTimer->setInterval(1000);
    connect(m_sampleTimer, &QTimer::timeout, this, &TelemetryClient::sample);
    m_sampleWindow.start();
    sample();
    flushSample();
    m_sampleTimer->start();
}

void TelemetryClient::startReporter() {
    if (m_shuttingDown || m_reporterConnections > 0 || m_reporterStarting) return;
    auto* service = account::Service::instance();
    const QString executable = reporterPath();
    if (!service || service->reporterToken().isEmpty()) {
        logReporterStatus(QStringLiteral("waiting_for_token"));
        scheduleReporterRetry();
        return;
    }
    if (executable.isEmpty()) {
        logReporterStatus(QStringLiteral("executable_missing"));
        scheduleReporterRetry(30000);
        return;
    }
    if (!m_ipc->isListening()) {
        m_ipcName = QStringLiteral("vlt-telemetry-%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!m_ipc->listen(m_ipcName)) {
            logReporterStatus(QStringLiteral("ipc_listen_failed"),
                              {{QStringLiteral("error"), m_ipc->errorString()}});
            scheduleReporterRetry();
            return;
        }
    }
    const QStringList arguments{
        QStringLiteral("--outbox"), m_outbox,
        QStringLiteral("--ipc"), m_ipcName,
        QStringLiteral("--origin"), service->apiOrigin(),
        QStringLiteral("--recovery"), m_window->recoverySessionDir(),
        QStringLiteral("--session"), m_sessionId,
        QStringLiteral("--app-version"), QCoreApplication::applicationVersion(),
        QStringLiteral("--build-id"), QCoreApplication::applicationVersion(),
    };
    qint64 reporterPid = 0;
    if (!QProcess::startDetached(executable, arguments, QString(), &reporterPid)) {
        logReporterStatus(QStringLiteral("launch_failed"));
        scheduleReporterRetry();
        return;
    }
    m_reporterStarting = true;
    logReporterStatus(QStringLiteral("launched"),
                      {{QStringLiteral("pid"), reporterPid}});
    QTimer::singleShot(5000, this, [this] {
        if (m_reporterConnections > 0 || m_shuttingDown) return;
        m_reporterStarting = false;
        logReporterStatus(QStringLiteral("connection_timeout"));
        scheduleReporterRetry();
    });
}

void TelemetryClient::scheduleReporterRetry(int delayMs) {
    if (!m_shuttingDown && !m_reporterRetryTimer->isActive())
        m_reporterRetryTimer->start(delayMs);
}

void TelemetryClient::sendReporterToken() {
    auto* service = account::Service::instance();
    if (!service || service->reporterToken().isEmpty()) return;
    const QByteArray line = service->reporterToken().toUtf8() + '\n';
    for (QLocalSocket* socket : m_ipc->findChildren<QLocalSocket*>()) {
        if (socket->state() == QLocalSocket::ConnectedState) {
            socket->write(line);
            socket->flush();
        }
    }
}

void TelemetryClient::sample() {
    m_latest = m_window->telemetrySnapshot();
    m_processSum += m_latest.value(QStringLiteral("process_cpu")).toDouble();
    m_systemSum += m_latest.value(QStringLiteral("system_cpu")).toDouble();
    m_dspSum += m_latest.value(QStringLiteral("dsp_load")).toDouble();
    m_dspPeak = std::max(m_dspPeak, m_latest.value(QStringLiteral("dsp_peak")).toDouble());
    ++m_sampleCount;
    if (m_sampleWindow.isValid() && m_sampleWindow.elapsed() >= 5 * 60 * 1000)
        flushSample();
}

void TelemetryClient::flushSample() {
    if (!m_sampleCount) return;
    m_latest.insert(QStringLiteral("process_cpu"), m_processSum / m_sampleCount);
    m_latest.insert(QStringLiteral("system_cpu"), m_systemSum / m_sampleCount);
    m_latest.insert(QStringLiteral("dsp_load"), m_dspSum / m_sampleCount);
    m_latest.insert(QStringLiteral("dsp_peak"), m_dspPeak);
    enqueue(QStringLiteral("sample"), m_latest);
    appendDiagnosticLog(QStringLiteral("telemetry_sample"), m_latest);
    m_processSum = m_systemSum = m_dspSum = m_dspPeak = 0.0;
    m_sampleCount = 0;
    m_sampleWindow.restart();
}

void TelemetryClient::enqueue(const QString& kind, const QJsonObject& payload) {
    const QJsonObject event{
        {QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("session_id"), m_sessionId},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("occurred_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("payload"), payload},
    };
    const QJsonObject envelope{{QStringLiteral("body"),
        QJsonObject{{QStringLiteral("events"), QJsonArray{event}}}}};
    const QString name = QStringLiteral("%1-%2-%3.telemetry.json")
        .arg(QDateTime::currentMSecsSinceEpoch(), 16, 10, QLatin1Char('0'))
        .arg(++m_sequence, 6, 10, QLatin1Char('0'))
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSaveFile file(QDir(m_outbox).filePath(name));
    if (file.open(QIODevice::WriteOnly)) {
        const QByteArray body = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        if (file.write(body) != body.size() || !file.commit())
            appendDiagnosticLog(QStringLiteral("telemetry_enqueue_failed"),
                                {{QStringLiteral("kind"), kind}});
    } else {
        appendDiagnosticLog(QStringLiteral("telemetry_enqueue_failed"),
                            {{QStringLiteral("kind"), kind},
                             {QStringLiteral("error"), file.errorString()}});
    }
    trimOutbox();
}

void TelemetryClient::trimOutbox() {
    constexpr qint64 kLimit = 250LL * 1024 * 1024;
    QDir directory(m_outbox);
    QFileInfoList samples = directory.entryInfoList(
        {QStringLiteral("*.telemetry.json")}, QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo& item : directory.entryInfoList(QDir::Files)) total += item.size();
    for (const QFileInfo& sample : samples) {
        if (total <= kLimit) break;
        total -= sample.size();
        QFile::remove(sample.absoluteFilePath());
    }
}

void TelemetryClient::appendDiagnosticLog(const QString& event,
                                          const QJsonObject& details) {
    const QString path = QDir(m_window->recoverySessionDir())
                             .filePath(QStringLiteral("app.log"));
    QFile file(path);
    if (file.exists() && file.size() > 2 * 1024 * 1024) {
        const QString rotated = path + QStringLiteral(".1");
        QFile::remove(rotated);
        QFile::rename(path, rotated);
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QJsonObject line = details;
    line.insert(QStringLiteral("event"), event);
    line.insert(QStringLiteral("recorded_at"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    file.write(QJsonDocument(line).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.flush();
}

void TelemetryClient::logReporterStatus(const QString& status,
                                        const QJsonObject& details) {
    if (status == m_lastReporterStatus) return;
    m_lastReporterStatus = status;
    QJsonObject fields = details;
    fields.insert(QStringLiteral("status"), status);
    appendDiagnosticLog(QStringLiteral("reporter_status"), fields);
}
