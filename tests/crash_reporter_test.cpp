// Exercise the actual courier, including its fallback when the watchdog has
// not written crashReason yet. No token is sent, so no upload can be attempted.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>
#include <memory>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool runCase(const QByteArray& pluginMarker, const QString& expectedReason,
             const QByteArray& signal = "signal=SIGSEGV (11)\n", const QJsonObject& watchdog = {},
             int uiHeartbeat = 7) {
    QTemporaryDir temp(QDir::tempPath() + "/vlt-report-XXXXXX");
    if (!temp.isValid()) return false;
    const QDir root(temp.path());
    if (!root.mkdir("outbox") || !root.mkdir("recovery")) return false;
    if (!writeFile(root.filePath("recovery/crash.txt"),
                   signal + pluginMarker + "\n") ||
        !writeFile(root.filePath("recovery/session.json"),
                   QJsonDocument(QJsonObject{{"outcome", "crashed"}, {"startedUnixMs", 123},
                       {"uiHeartbeat", uiHeartbeat}, {"stats", QJsonObject{{"lastPlugin", "ValhallaUberMod"}}}})
                       .toJson(QJsonDocument::Compact)))
        return false;
    if (!watchdog.isEmpty() && !writeFile(root.filePath("recovery/watchdog.json"),
            QJsonDocument(watchdog).toJson(QJsonDocument::Compact))) return false;

    QLocalServer server;
    const QString socketName = root.filePath("reporter.sock");
    if (!server.listen(socketName)) {
        std::fprintf(stderr, "IPC listen failed: %s\n", server.errorString().toUtf8().constData());
        return false;
    }
    QProcess reporter;
    reporter.start(QString::fromUtf8(DAW_REPORTER_PATH), {
        "--outbox", root.filePath("outbox"), "--ipc", socketName,
        "--origin", "http://127.0.0.1:1", "--recovery", root.filePath("recovery"),
        "--session", "fixture", "--app-version", "test", "--build-id", "test"});
    if (!reporter.waitForStarted(3000) || !server.waitForNewConnection(3000)) {
        std::fprintf(stderr, "Reporter did not connect: %s\n%s\n",
            reporter.errorString().toUtf8().constData(), reporter.readAllStandardError().constData());
        return false;
    }
    std::unique_ptr<QLocalSocket> parent(server.nextPendingConnection());
    if (!parent) return false;
    // The application dies before it supplies a token. The reporter must still
    // stage an offline crash report, then exit through its no-token deadline.
    parent->disconnectFromServer();
    if (!reporter.waitForFinished(8000)) {
        reporter.kill();
        reporter.waitForFinished();
        return false;
    }
    const QDir outbox(root.filePath("outbox"));
    const QStringList reports = outbox.entryList({"*.crash.json"}, QDir::Files);
    if (reporter.exitStatus() != QProcess::NormalExit || reports.size() != 1) {
        std::fprintf(stderr, "Reporter exit %d, reports %lld: %s\n", reporter.exitCode(),
            static_cast<long long>(reports.size()), reporter.readAllStandardError().constData());
        return false;
    }
    QFile file(outbox.filePath(reports.front()));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto envelope = QJsonDocument::fromJson(file.readAll()).object();
    const auto metadata = envelope["metadata"].toObject();
    bool ok = metadata["reason"].toString() == expectedReason &&
                    metadata["last_plugin"].toString() == "ValhallaUberMod";
    if (!watchdog.isEmpty() && watchdog["startedUnixMs"].toInt() == 123) {
        QFile artifact(outbox.filePath(envelope["artifact_file"].toString()));
        ok = ok && artifact.open(QIODevice::ReadOnly) && artifact.readAll().contains("[watchdog]");
    }
    if (signal.isEmpty()) ok = ok && metadata["exception_code"].toString().isEmpty();
    if (!ok) std::fprintf(stderr, "Unexpected metadata: %s\n",
                          QJsonDocument(metadata).toJson().constData());
    return ok;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (!runCase("last_plugin=ValhallaUberMod", "crashed (SIGSEGV)") ||
        !runCase({}, "crashed (SIGSEGV)") ||
        !runCase("plugin=ValhallaUberMod", "crashed in ValhallaUberMod (SIGSEGV)"))
        return 1;
    const QJsonObject hung{{"startedUnixMs", 123}, {"outcome", "hung"},
        {"uiHeartbeatTracked", true}, {"observedHeartbeat", 7}, {"reason", "application_hung"}};
    if (!runCase({}, "application_hung", {}, hung) ||
        !runCase({}, "process_terminated_unexpectedly", {}, hung, 8) ||
        !runCase({}, "crashed (SIGSEGV)", "signal=SIGSEGV (11)\n", hung) ||
        !runCase({}, "process_terminated_unexpectedly (exit code 0xc0000409)", {},
            {{"startedUnixMs", 123}, {"outcome", "crashed"}, {"processExitCode", double(0xc0000409u)}})) return 1;
    std::puts("PASS crash attribution distinguishes active calls from historical plugin context");
    std::puts("PASS watchdog hang/exit diagnostics survive an absent crash marker");
}
