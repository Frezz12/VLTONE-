#pragma once
#include "ScriptLibrary.hpp"
#include "ai/AiSession.hpp"
#include <QMainWindow>
#include <memory>
class QTreeWidget;
class QPlainTextEdit;
class QUndoStack;
class QLineEdit;
class QComboBox;
class QCloseEvent;
class QAction;
namespace ui {
class ScriptService;
class LlmClient;
class LuaAiWorkspace;
class LuaCreatorWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit LuaCreatorWindow(ScriptService *, QWidget *parent = nullptr);
    ~LuaCreatorWindow() override;
    bool openPackage(const ScriptPackage &);
    bool openProject(const QString &path, QString &error);
    void reveal(const QString &file, int line);
    vlt::script::Json document() const {
        return m_document;
    }
    void editDocument(const vlt::script::Json &, const QString &label);
    ScriptPackage package() const;
    bool check(QString &error) const;
    QString execute(bool preview, const QString &action = {});
    bool install(QString &error);
    std::uint64_t revision() const {
        return m_revision;
    }
    ScriptService *service() const {
        return m_service;
    }

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    friend class McpService;
    friend class LuaAiWorkspace;
    void apply(const vlt::script::Json &);
    void applySource(const QString &, const std::string &);
    void refreshCommands();
    void updateRunState();
    void refreshRunLog();
    void selectFile();
    void changedText();
    bool askSave();
    bool save(bool saveAs = false);
    void open();
    void newProject();
    void sendAi();
    void stepAi();
    void stopAi();
    ScriptService *m_service;
    ScriptPackage m_origin;
    vlt::script::Json m_document;
    QString m_path, m_file, m_run;
    QTreeWidget *m_files;
    QPlainTextEdit *m_editor, *m_log, *m_prompt, *m_transcript;
    QComboBox *m_models, *m_commands;
    QAction *m_stop;
    QUndoStack *m_undo;
    std::unique_ptr<LuaAiWorkspace> m_workspace;
    std::unique_ptr<daw::ai::AiSession> m_session;
    std::unique_ptr<LlmClient> m_client;
    std::uint64_t m_revision = 0;
    std::uint64_t m_editGroup = 0;
    bool m_loading = false;
};
class LuaAiWorkspace final : public daw::ai::AiWorkspace {
  public:
    explicit LuaAiWorkspace(LuaCreatorWindow &window) : m_window(window) {}
    std::uint64_t revision() const override;
    std::string systemPrompt(const daw::ai::ToolContext &) const override;
    std::vector<daw::ai::ToolSpec> tools(daw::ai::InteractionMode) const override;
    static std::vector<daw::ai::ToolSpec> toolSpecs(daw::ai::InteractionMode);
    std::optional<daw::ai::ToolResult>
    execute(const daw::ai::ToolCall &, const daw::ai::ToolContext &, std::uint64_t) override;
    void cancel() override;

  private:
    LuaCreatorWindow &m_window;
};
} // namespace ui
