#pragma once
#include "ScriptLibrary.hpp"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

class QMainWindow;
class QWidget;
class QDialog;
class QMenu;
class QToolBar;
class QProcess;
class QAction;
class ShortcutManager;
namespace daw {
class EngineController;
}
namespace ui {
struct ScriptRun;
class ScriptManagerWindow;
class LuaCreatorWindow;
class ScriptService final : public QObject {
    Q_OBJECT
  public:
    using Json = vlt::script::Json;
    struct Project {
        QString id, name;
        daw::EngineController *controller = nullptr;
    };
    struct Bindings {
        std::function<Project()> current;
        std::function<Project(const QString &)> resolve;
        std::function<Json(const QString &)> context;
        std::function<void(const QString &)> edited;
        std::function<bool(const QString &, const QString &)> command;
        std::function<Json(const QString &, const Json &)> workspace;
    };
    ScriptService(QMainWindow *, ShortcutManager *, Bindings);
    ~ScriptService() override;
    void setupMenu();
    void reload();
    QList<ScriptPackage> packages() const { return m_packages; }
    Json settings(const QString &id) const;
    void saveSettings(const QString &id, const Json &value);
    QString run(const QString &command, const Json &context = Json::object(),
                const Json &arguments = Json::object(), const QString &assistantMode = {});
    QString runPackage(const ScriptPackage &, const QString &action,
                       const Json &context = Json::object(), bool preview = false,
                       const QString &assistantMode = {});
    Json describe(const QString &query = {}) const;
    Json invocationContext(daw::EngineController *) const;
    Json status(const QString &runId) const;
    void stop(const QString &runId);
    void stopAll();
    void closeProject(const QString &id);
    void showManager();
    void chooseCommand();
    void runFile();
    void createScript(const ScriptPackage *package = nullptr, const QString &file = {},
                      int line = 0);
    bool openLuaProject(const QString &path);
    void showSdk();
    void showGuide();
    void configureToolbar();
    void rebuildToolbar();
    QToolBar *toolbar() const { return m_toolbar; }
    void appendMenu(QMenu *, daw::EngineController *, Json context);
    static void appendContextMenu(QMenu *, daw::EngineController *, Json context);
    static ScriptService *instance();
    void mcpProjectEdited(const QString &id) { m_bindings.edited(id); }
    static bool runCheck(const QString &screenshots = {});
    static bool runAudioCheck();
    bool approvePermissions(const ScriptPackage &, bool interactive = true);
    static QString permissionLabel(const std::string &);
    QString install(const QString &path, const QString &expectedId = {});
  signals:
    void changed();
    void runChanged(const QString &id);

  private:
    friend class McpService;
    friend class LuaCreatorWindow;
    QProcess *prepareWorker();
    void receive(QProcess *);
    void send(QProcess *, const vlt::script::Message &);
    void dispatch(ScriptRun *, const vlt::script::Message &);
    void reply(ScriptRun *, std::uint64_t, const Json &, const std::vector<char> &binary = {});
    void failReply(ScriptRun *, std::uint64_t, const QString &);
    Json activeCallbacks(const ScriptRun *) const;
    void finish(ScriptRun *, const QString &error = {}, bool recycle = false);
    void tick();
    void event(ScriptRun *, int callback, const Json &, const QString &coalesceKey = {});
    daw::EngineController *controller(ScriptRun *) const;
    void authorize(ScriptRun *, const Json &method);
    Json parameterCall(ScriptRun *, const std::string &method, const Json &);
    Json uiCall(ScriptRun *, const std::string &method, const Json &);
    QWidget *buildScriptContent(ScriptRun *, const Json &, const Json &, QHash<QString, QPointer<QWidget>> &,
                                QList<int> &);
    vlt::script::Message ioCall(ScriptRun *, const std::string &method, const Json &,
                                const std::vector<char> &);
    const QSet<QString> &projectFiles(ScriptRun *) const;
    bool permittedPath(ScriptRun *, const QString &, bool write) const;
    void grantPath(ScriptRun *, const QString &, bool write, bool folder);
    QString persistImport(ScriptRun *, const QString &, QStringList &created);
    void cleanupIo(ScriptRun *);
    void pollIo(ScriptRun *);
    void addLog(ScriptRun *, const QString &);
    void launch(ScriptRun *);
    QMainWindow *m_window;
    ShortcutManager *m_shortcuts;
    Bindings m_bindings;
    QList<ScriptPackage> m_packages;
    QHash<QString, QAction *> m_actions;
    QHash<QProcess *, QByteArray> m_buffers;
    QProcess *m_spare = nullptr;
    QList<std::shared_ptr<ScriptRun>> m_runs;
    QPointer<ScriptManagerWindow> m_manager;
    QPointer<LuaCreatorWindow> m_creator;
    QPointer<QDialog> m_toolbarEditor;
    QPointer<QDialog> m_commandPicker;
    QToolBar *m_toolbar = nullptr;
    QTimer m_tick;
    qint64 m_workerRetryAt = 0;
    bool m_shuttingDown = false;
};
} // namespace ui
