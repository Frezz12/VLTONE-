#pragma once
#include "CreatorBuildService.hpp"
#include "ai/AiSession.hpp"
#include <QTimer>
namespace ui {
class CreatorWindow;
class CreatorAiWorkspace final : public QObject, public daw::ai::AiWorkspace {
  Q_OBJECT
public:
  explicit CreatorAiWorkspace(CreatorWindow &);
  ~CreatorAiWorkspace() override;
  std::uint64_t revision() const override;
  std::string systemPrompt(const daw::ai::ToolContext &) const override;
  std::vector<daw::ai::ToolSpec> tools(daw::ai::InteractionMode) const override;
  std::optional<daw::ai::ToolResult> execute(const daw::ai::ToolCall &,
                                             const daw::ai::ToolContext &,
                                             std::uint64_t) override;
  void begin(std::uint64_t, const std::string &, std::size_t) override;
  void finish(bool, bool) override;
  void cancel() override;
  std::string completionIssue() const override;
  nlohmann::json context() const;
  void documentChanging();
  void resetProject();
  bool canRestore() const;
  bool restoreResult(std::function<void(QString)>);
  std::function<void(std::uint64_t, std::string, daw::ai::ToolResult)>
      completed;
  std::function<void(QString)> progress;
  nlohmann::json diagnostics = nlohmann::json::array();

private:
  struct Checkpoint {
    CreatorProject before;
    std::uint64_t afterRevision = 0;
    std::string afterHash;
    std::string hostProject;
    QString path, error;
    CreatorFileState beforeFile, afterFile;
    std::shared_ptr<daw::MiniModuleUpdate> installed;
    bool changed = false, conflicted = false;
  };
  void commit(const CreatorProject &, const QString &);
  void pollCode();
  CreatorWindow &m_window;
  QTimer m_timer;
  std::atomic<bool> m_cancel{false};
  std::future<nlohmann::json> m_codeFuture;
  std::uint64_t m_run = 0, m_codeRevision = 0, m_codeRun = 0;
  std::string m_codeCall;
  QString m_node, m_operation, m_layout;
  bool m_applying = false, m_active = false, m_ownsBuild = false;
  bool m_nodesOnly = false;
  bool m_requiresInstall = true;
  std::string m_installedHash;
  QString m_mergeKey;
  std::shared_ptr<Checkpoint> m_checkpoint;
  std::shared_ptr<Checkpoint> m_previousCheckpoint;
};
} // namespace ui
