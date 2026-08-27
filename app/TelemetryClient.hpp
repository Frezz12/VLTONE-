#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>

class MainWindow;
class QLocalServer;
class QTimer;

/// Writes a bounded, privacy-filtered local outbox. Network I/O belongs to
/// daw_reporter so a crash cannot strand the final diagnostics in this process.
class TelemetryClient final : public QObject {
    Q_OBJECT
public:
    explicit TelemetryClient(MainWindow* window, QObject* parent = nullptr);

private:
    void startReporter();
    void sample();
    void flushSample();
    void enqueue(const QString& kind, const QJsonObject& payload);
    void trimOutbox();
    void scheduleReporterRetry(int delayMs = 5000);
    void sendReporterToken();
    void appendDiagnosticLog(const QString& event, const QJsonObject& details = {});
    void logReporterStatus(const QString& status, const QJsonObject& details = {});

    MainWindow* m_window = nullptr;
    QLocalServer* m_ipc = nullptr;
    QTimer* m_sampleTimer = nullptr;
    QTimer* m_reporterRetryTimer = nullptr;
    QString m_outbox;
    QString m_ipcName;
    QString m_sessionId;
    QString m_lastReporterStatus;
    QJsonObject m_latest;
    QElapsedTimer m_sampleWindow;
    double m_processSum = 0.0;
    double m_systemSum = 0.0;
    double m_dspSum = 0.0;
    double m_dspPeak = 0.0;
    int m_sampleCount = 0;
    int m_reporterConnections = 0;
    qint64 m_sequence = 0;
    bool m_reporterStarting = false;
    bool m_shuttingDown = false;
};
