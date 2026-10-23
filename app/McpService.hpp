#pragma once
#include "ai/AiTools.hpp"
#include "mcp/McpProtocol.hpp"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <future>

class MainWindow;
class QLocalServer;
class QLocalSocket;
class QLockFile;
struct ProjectSession;
namespace ui {
class CreatorAiWorkspace;
class CreatorWindow;
class LuaCreatorWindow;
struct ScriptRun;
class McpService final : public QObject {
    Q_OBJECT
public:
    using Json = nlohmann::json;
    explicit McpService(MainWindow &);
    ~McpService() override;
    static McpService *instance();
    bool enabled() const;
    bool listening() const;
    QString status() const;
    QStringList log() const { return m_log; }
    QString configuration(bool codex) const;
    void setEnabled(bool);
    void revoke();
    void stopOperations();
    void projectClosing(const QString &);
    static bool runCheck(MainWindow &, const QString &directory);
signals:
    void changed();
private:
    struct Client {
        QString id;
        vlt::mcp::Protocol protocol;
        bool authenticated = false;
        std::shared_ptr<daw::ai::ContentCatalog> catalog;
        std::shared_ptr<daw::ai::CompositionCandidateStore> candidates;
        Json requestId;
        QHash<QString, QStringList> requestJobs;
    };
    struct Job {
        QString id, owner, project, document, kind, scriptRun;
        std::uint64_t generation = 0;
        std::atomic<bool> cancel{false};
        std::atomic<double> progress{0};
        std::future<Json> future;
        std::function<void()> stop;
        std::function<Json(Json)> publish;
        std::function<void()> cleanup;
        std::function<bool()> poll;
        Json result;
        QString error;
        bool complete = false;
    };
    void accept();
    void receive(QLocalSocket *);
    void disconnect(QLocalSocket *);
    void poll();
    void appendLog(const QString &);
    void write(QLocalSocket *, const Json &);
    Json dispatch(Client &, const std::string &, const Json &);
    Json call(Client &, const std::string &, const Json &);
    std::vector<daw::ai::ToolSpec> specs() const;
    Json resources() const;
    Json readResource(const std::string &) const;
    daw::ai::ToolContext context(Client &, ProjectSession &);
    ProjectSession &resolve(const Json &, bool edit);
    Json stamp(ProjectSession &) const;
    void edited(ProjectSession &);
    void activateAudio(ProjectSession &, bool enabled = true);
    Json projects(Client &, const Json &);
    Json creator(Client &, ProjectSession &, const std::string &, const Json &);
    Json lua(Client &, ProjectSession &, const std::string &, const Json &);
    Json plugins(Client &, const Json &);
    Json render(Client &, ProjectSession &, const Json &);
    Json analyze(Client &, ProjectSession &, const std::string &, const Json &);
    ScriptRun *ownedScript(Client &, ProjectSession &, const QString &);
    Json scriptUi(Client &, ProjectSession &, const Json &);
    Json capture(Client &, ProjectSession &, const Json &);
    std::shared_ptr<Job> newJob(Client &, ProjectSession *, const QString &kind);
    Json jobStatus(const Job &) const;
    void cancelJob(const std::shared_ptr<Job> &);
    CreatorWindow *creatorWindow(ProjectSession &, bool create);
    LuaCreatorWindow *luaWindow(ProjectSession &, bool create);
    MainWindow &m_window;
    QLocalServer *m_server;
    std::unique_ptr<QLockFile> m_lock;
    QHash<QLocalSocket *, std::shared_ptr<Client>> m_clients;
    QHash<QString, std::shared_ptr<Job>> m_jobs;
    QTimer m_poll;
    QString m_token, m_endpoint, m_error;
    QStringList m_log;
    bool m_enabled = false, m_dispatching = false, m_shuttingDown = false;
    QHash<QString, QPointer<CreatorWindow>> m_creators;
    QHash<QString, QPointer<LuaCreatorWindow>> m_lua;
};
}
