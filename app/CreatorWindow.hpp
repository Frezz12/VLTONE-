#pragma once
#include "CreatorProject.hpp"
#include "CreatorBuildService.hpp"
#include <QMainWindow>
#include <QSet>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <optional>

class QCloseEvent;
class QShowEvent;
class QLineEdit;
class QListWidget;
class QTreeWidget;
class QComboBox;
class QLabel;
class QScrollArea;
class QUndoStack;
class QTimer;
class QFormLayout;
class QFileSystemWatcher;
class QStackedWidget;
class QSplitter;
namespace daw {
class EngineController;
}
namespace ui {
class CreatorCanvas;
class CreatorCodeEditor;
class CreatorAiPanel;
class CreatorAiWorkspace;
class CreatorWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit CreatorWindow(daw::EngineController *, QWidget *parent = nullptr);
  ~CreatorWindow() override;
  const CreatorProject &project() const { return m_project; }
  void setProjectForTest(CreatorProject);
  static bool runCheck(const QString &directory);
  static bool runAiCheck(const QString &directory);
signals:
  void modulesCompiled();
  void aiSettingsRequested();

protected:
  void closeEvent(QCloseEvent *) override;
  void showEvent(QShowEvent *) override;

private:
  friend class CreatorAiWorkspace;
  friend class CreatorAiPanel;
  struct CodeResult;
  void startup();
  void newProject();
  bool openProject(const QString &path = {});
  bool saveProject(bool saveAs = false);
  bool askToSave();
  void edit(const QString &, const std::function<void(CreatorProject &)> &, const QString& mergeKey = {});
  void apply(CreatorProject);
  void refresh();
  void properties(const QString &node);
  void addNode(const QString &type, QPointF position);
  void openCode(const QString &);
  void codeEdited();
  void codeOperation(const QString &);
  void pollCode();
  void addSearch(QPointF position);
  void removeSelection();
  void copySelection();
  void pasteSelection(QPointF position);
  void groupSelection();
  void enterNode(const QString &);
  void importNodeFile(const QString &path = {}, std::optional<QPointF> position = {});
  void exportNodeFile();
  void refreshNodeLibrary();
  bool programmingProperties(QWidget *, QFormLayout *, const daw::plugins::mini::NodeDefinition &);
  void compile();
  bool startBuild(CreatorBuildService::Done done = {});
  void showAi(bool);
  void diagnostic(const QString &, const QString &node = {});
  void rememberRecent();
  QString recoveryPath() const;
  daw::EngineController *m_controller;
  CreatorProject m_project;
  QString m_path, m_selected, m_installDirectory, m_recoveryDirectory;
  CreatorCanvas *m_canvas;
  CreatorCodeEditor *m_code;
  QLineEdit *m_search;
  QTreeWidget *m_library;
  QListWidget *m_diagnostics;
  QSet<QString> m_collapsedCategories;
  QComboBox *m_modes;
  QLabel *m_status;
  QScrollArea *m_properties;
  QUndoStack *m_undo;
  QTimer *m_compilePoll;
  QAction *m_compileAction;
  QAction *m_cancelAction;
  QAction *m_backAction = nullptr;
  CreatorBuildService* m_build = nullptr;
  CreatorAiPanel* m_ai = nullptr;
  QStackedWidget* m_rightStack = nullptr;
  QSplitter* m_split = nullptr;
  QAction* m_aiAction = nullptr;
  std::uint64_t m_revision = 0;
  std::function<void()> m_fillLibrary;
  QMap<QString, QString> m_personalNodes;
  QFileSystemWatcher *m_nodeLibraryWatcher = nullptr;
  std::atomic<bool> m_cancel{false};
  std::future<std::shared_ptr<CodeResult>> m_codeFuture;
  QString m_codeId;
  bool m_started = false, m_refreshing = false;
  int m_interfaceTab = 0;
  bool m_closeAfterAi = false;
};
} // namespace ui
