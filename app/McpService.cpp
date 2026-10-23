#include "McpService.hpp"
#include "MainWindow.hpp"
#include "ProjectWorkspace.hpp"
#include "ScriptService.hpp"
#include "mcp/McpEndpoint.hpp"
#include "ai/ContentCatalog.hpp"
#include "ai/CompositionEngine.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QSettings>
#include <QUuid>

namespace ui {
namespace {
QPointer<McpService> current;
QString token() {
    QByteArray bytes(32, '\0');
    for (auto &byte : bytes) byte = char(QRandomGenerator::system()->generate() & 255);
    return QString::fromLatin1(bytes.toHex());
}
bool equalToken(const std::string &a, const QByteArray &b) {
    unsigned difference = unsigned(a.size() ^ size_t(b.size()));
    for (qsizetype i = 0; i < b.size(); ++i)
        difference |= unsigned(static_cast<unsigned char>(b[i]) ^ (size_t(i) < a.size() ? static_cast<unsigned char>(a[size_t(i)]) : 0));
    return difference == 0;
}
}
McpService *McpService::instance() { return current; }
McpService::McpService(MainWindow &window) : QObject(&window), m_window(window), m_server(new QLocalServer(this)) {
    current = this;
    m_endpoint = vlt::mcp::endpointName();
    QSettings settings;
    m_token = settings.value("mcp/token").toString();
    if (m_token.isEmpty()) { m_token = token(); settings.setValue("mcp/token", m_token); }
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &McpService::accept);
    m_poll.setInterval(50);
    connect(&m_poll, &QTimer::timeout, this, &McpService::poll);
    m_poll.start();
    setEnabled(!QCoreApplication::arguments().contains("--selftest") && settings.value("mcp/enabled", false).toBool());
}
McpService::~McpService() {
    m_shuttingDown = true;
    m_server->close();
    m_lock.reset();
    stopOperations();
    for (auto socket : m_clients.keys()) socket->abort();
    // Snapshot workers have no QObject/controller access and must finish before unload.
    for (const auto &job : m_jobs) {
        if (job->future.valid()) { try { job->future.get(); } catch (...) {} }
        if (job->cleanup) job->cleanup();
        job->cleanup={}; job->publish={}; job->poll={}; job->stop={};
    }
    if (current == this) current = nullptr;
}
bool McpService::enabled() const { return m_enabled; }
bool McpService::listening() const { return m_server->isListening(); }
QString McpService::status() const {
    if (!m_enabled) return tr("Disabled");
    if (!listening()) return tr("Cannot start: %1").arg(m_error);
    int count = 0;
    for (const auto &client : m_clients) if (client->authenticated) ++count;
    return tr("Listening · %1 connected clients").arg(count);
}
QString McpService::configuration(bool codex) const {
    auto command = QDir(QCoreApplication::applicationDirPath()).filePath("vlt_mcp");
#ifdef Q_OS_WIN
    command += ".exe";
#endif
    const auto quoted = [](const QString &s) { return QString::fromStdString(Json(s.toStdString()).dump()); };
    if (codex) return "[mcp_servers.vltone]\ncommand = " + quoted(QDir::toNativeSeparators(command)) +
        "\nargs = []\n\n[mcp_servers.vltone.env]\nVLTONE_MCP_TOKEN = " + quoted(m_token) + "\n";
    return QString::fromStdString(Json{{"mcpServers", {{"vltone", {{"command", QDir::toNativeSeparators(command).toStdString()},
        {"args", Json::array()}, {"env", {{"VLTONE_MCP_TOKEN", m_token.toStdString()}}}}}}}}.dump(2));
}
void McpService::setEnabled(bool enabled) {
    m_enabled = enabled;
    QSettings().setValue("mcp/enabled", enabled);
    if (!enabled) {
        m_server->close();
        m_lock.reset();
        stopOperations();
        for (auto *socket : m_clients.keys()) socket->abort();
    } else if (!m_server->isListening()) {
        m_error.clear();
        const auto directory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/MCP";
        QDir().mkpath(directory);
        m_lock=std::make_unique<QLockFile>(directory+"/"+m_endpoint+".lock");
        if(!m_lock->tryLock(0)) { m_error=tr("Another VLTONE instance owns the MCP endpoint"); m_lock.reset(); emit changed(); return; }
#ifndef Q_OS_WIN
        // Remove only a stale socket. Never steal another running instance's endpoint.
        QLocalSocket probe;
        probe.connectToServer(m_endpoint);
        if (!probe.waitForConnected(100)) QLocalServer::removeServer(m_endpoint);
#endif
        if (!m_server->listen(m_endpoint)) { m_error = m_server->errorString(); m_lock.reset(); }
    }
    emit changed();
}
void McpService::revoke() {
    setEnabled(false);
    m_token = token();
    QSettings().setValue("mcp/token", m_token);
    appendLog(tr("Access revoked; connection token replaced"));
}
void McpService::appendLog(const QString &entry) {
    m_log.append(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + entry);
    while (m_log.size() > 200) m_log.removeFirst();
    emit changed();
}
void McpService::accept() {
    while (auto *socket = m_server->nextPendingConnection()) {
        if (m_clients.size() >= 16) { socket->abort(); socket->deleteLater(); continue; }
        socket->setReadBufferSize(qint64(vlt::mcp::maxMessageBytes) + 1);
        auto client = std::make_shared<Client>();
        client->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        client->catalog = std::make_shared<daw::ai::ContentCatalog>();
        client->candidates = std::make_shared<daw::ai::CompositionCandidateStore>();
        m_clients.insert(socket, client);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { receive(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { disconnect(socket); });
        QTimer::singleShot(3000, socket, [this, socket] {
            auto client = m_clients.value(socket);
            if (client && !client->authenticated) socket->abort();
        });
        if(socket->bytesAvailable()) QTimer::singleShot(0,socket,[this,socket] { receive(socket); });
    }
}
void McpService::write(QLocalSocket *socket, const Json &message) {
    const auto data = QByteArray::fromStdString(message.dump()) + '\n';
    if (data.size() > qint64(vlt::mcp::maxMessageBytes) || socket->bytesToWrite() > qint64(2 * vlt::mcp::maxMessageBytes)) {
        socket->abort(); return;
    }
    socket->write(data);
}
void McpService::receive(QLocalSocket *socket) {
    auto client = m_clients.value(socket);
    if (!client || m_shuttingDown) return;
    if (m_dispatching) { QTimer::singleShot(0, socket, [this,socket] { receive(socket); }); return; }
    const QScopedValueRollback<bool> busy(m_dispatching, true);
    int handled = 0;
    while (m_clients.contains(socket) && socket->canReadLine() && handled++ < 8) {
        const auto line = socket->readLine(qint64(vlt::mcp::maxMessageBytes) + 1);
        if (line.size() > qint64(vlt::mcp::maxMessageBytes)) { socket->abort(); break; }
        Json message;
        try {
            message = Json::parse(line.constData(), line.constData()+line.size(), [](int depth, Json::parse_event_t, Json &) {
                if (depth > 64) throw std::runtime_error("JSON nesting exceeds 64");
                return true;
            });
        } catch (...) {
            if (client->authenticated) write(socket, vlt::mcp::error(nullptr, -32700, "Invalid JSON"));
            else socket->abort();
            continue;
        }
        if (!client->authenticated) {
            if (!message.is_object() || message.value("vltMcp", Json()) != 1 || !message.contains("token") ||
                !message["token"].is_string() || !equalToken(message["token"].get<std::string>(), m_token.toLatin1())) {
                write(socket, {{"authenticated", false}}); socket->disconnectFromServer(); continue;
            }
            client->authenticated = true;
            write(socket, {{"authenticated", true}});
            appendLog(tr("Client connected"));
            continue;
        }
        client->requestId = message.is_object() ? message.value("id", Json()) : Json();
        auto response = client->protocol.receive(message,
            [this, &client](const std::string &method, const Json &params) { return dispatch(*client, method, params); },
            [this, &client](const Json &id) {
                for (const auto &jobId : client->requestJobs.take(QString::fromStdString(id.dump())))
                    if (auto job = m_jobs.value(jobId)) cancelJob(job);
            });
        if (response) write(socket, *response);
        client->requestId = nullptr;
    }
    if (socket->bytesAvailable() >= qint64(vlt::mcp::maxMessageBytes) && !socket->canReadLine()) socket->abort();
    else if (socket->canReadLine()) QTimer::singleShot(0, socket, [this, socket] { receive(socket); });
}
void McpService::disconnect(QLocalSocket *socket) {
    auto client = m_clients.take(socket);
    if (client) {
        for (const auto &job : m_jobs) if (job->owner == client->id && !job->complete) cancelJob(job);
        if (client->authenticated) appendLog(tr("Client disconnected; unfinished operations cancelled"));
    }
    socket->deleteLater();
}
std::shared_ptr<McpService::Job> McpService::newJob(Client &client, ProjectSession *project, const QString &kind) {
    int running = 0;
    for (const auto &job : m_jobs) if (!job->complete) ++running;
    if (running >= 32) throw std::runtime_error("Busy: too many active MCP jobs");
    if (m_jobs.size() >= 128) {
        for (auto it=m_jobs.begin();it!=m_jobs.end();) {
            if (it.value()->complete) it=m_jobs.erase(it); else ++it;
        }
    }
    auto job = std::make_shared<Job>();
    job->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job->owner = client.id; job->kind = kind;
    if (project) { job->project=project->id; job->generation=project->controller.projectGeneration(); }
    m_jobs.insert(job->id, job);
    if (!client.requestId.is_null()) client.requestJobs[QString::fromStdString(client.requestId.dump())].append(job->id);
    return job;
}
McpService::Json McpService::jobStatus(const Job &job) const {
    return {{"jobId", job.id.toStdString()}, {"projectId", job.project.toStdString()}, {"documentId", job.document.toStdString()},
        {"kind", job.kind.toStdString()}, {"state", !job.complete ? (job.cancel ? "cancelling" : "running") :
            (job.cancel ? "cancelled" : (!job.error.isEmpty() ? "failed" : "completed"))},
        {"progress", job.progress.load()}, {"result", job.result}, {"error", job.error.toStdString()}};
}
void McpService::cancelJob(const std::shared_ptr<Job> &job) {
    if (job->complete || job->cancel.exchange(true)) return;
    if (job->stop) job->stop();
    if (!job->scriptRun.isEmpty() && m_window.m_scripts) m_window.m_scripts->stop(job->scriptRun);
}
void McpService::stopOperations() { for (const auto &job : m_jobs) cancelJob(job); }
void McpService::projectClosing(const QString &id) {
    for (const auto &job : m_jobs) if (job->project == id) cancelJob(job);
}
void McpService::poll() {
    if (m_dispatching || m_shuttingDown) return;
    const QScopedValueRollback<bool> dispatching(m_dispatching,true);
    for (const auto &job : m_jobs) {
        if (job->complete) continue;
        try {
            if (!job->project.isEmpty()) {
                auto *project = m_window.m_projects.find(job->project);
                if (!project || project->controller.projectGeneration()!=job->generation) cancelJob(job);
            }
            bool done = false;
            if (job->future.valid() && job->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                job->result = job->future.get();
                if (!job->cancel && job->publish) job->result=job->publish(std::move(job->result));
                done = true;
            } else if (job->poll) done=job->poll();
            else if (!job->scriptRun.isEmpty()) {
                job->result = m_window.m_scripts->status(job->scriptRun);
                done = job->result.value("complete", false);
            } else if (job->cancel && !job->future.valid()) done=true;
            if (!done) continue;
            job->complete=true;
            job->progress=1;
            if (job->result.is_object() && job->result.contains("ok") && !job->result.value("ok",true))
                job->error=QString::fromStdString(job->result.value("error","Operation failed"));
            else if (job->result.is_object() && job->result.contains("error") && job->result["error"].is_string())
                job->error=QString::fromStdString(job->result["error"]);
        } catch (const std::exception &error) { job->complete=true; job->error=QString::fromUtf8(error.what()); }
        if (job->complete) {
            if (job->cleanup) { job->cleanup(); job->cleanup={}; }
            job->publish={}; job->poll={}; job->stop={};
            for (const auto &client : m_clients)
                for (auto it=client->requestJobs.begin();it!=client->requestJobs.end();) {
                    it.value().removeAll(job->id);
                    if (it.value().isEmpty()) it=client->requestJobs.erase(it); else ++it;
                }
            appendLog(job->kind + " · " + (job->cancel ? tr("Cancelled") : job->error.isEmpty() ? tr("Completed") : tr("Failed")));
        }
    }
}
}
