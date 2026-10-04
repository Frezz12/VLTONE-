#pragma once
#include "CreatorProject.hpp"
#include <QMainWindow>
#include <atomic>
#include <functional>
#include <future>
#include <memory>

class QCloseEvent;
class QShowEvent;
class QLineEdit;
class QListWidget;
class QComboBox;
class QLabel;
class QScrollArea;
class QUndoStack;
class QTimer;
namespace daw {
class EngineController;
}
namespace ui {
class CreatorCanvas;
class CreatorCodeEditor;
class CreatorWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit CreatorWindow(daw::EngineController *, QWidget *parent = nullptr);
  ~CreatorWindow() override;
  const CreatorProject &project() const { return m_project; }
  void setProjectForTest(CreatorProject);
  static bool runCheck(const QString &directory);
signals:
  void modulesCompiled();

protected:
  void closeEvent(QCloseEvent *) override;
  void showEvent(QShowEvent *) override;

private:
  struct CompileResult;
  struct CodeResult;
  void startup();
  void newProject();
  bool openProject(const QString &path = {});
  bool saveProject(bool saveAs = false);
  bool askToSave();
  void edit(const QString &, const std::function<void(CreatorProject &)> &);
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
  void compile();
  void pollCompile();
  void diagnostic(const QString &, const QString &node = {});
  void rememberRecent();
  QString recoveryPath() const;
  daw::EngineController *m_controller;
  CreatorProject m_project;
  QString m_path, m_selected, m_installDirectory, m_recoveryDirectory;
  CreatorCanvas *m_canvas;
  CreatorCodeEditor *m_code;
  QLineEdit *m_search;
  QListWidget *m_library, *m_diagnostics;
  QComboBox *m_modes;
  QLabel *m_status;
  QScrollArea *m_properties;
  QUndoStack *m_undo;
  QTimer *m_compilePoll;
  QAction *m_compileAction;
  QAction *m_cancelAction;
  std::atomic<bool> m_cancel{false};
  std::future<std::shared_ptr<CodeResult>> m_codeFuture;
  QString m_codeId;
  std::future<std::shared_ptr<CompileResult>> m_compileFuture;
  std::shared_ptr<CompileResult> m_pending;
  bool m_started = false, m_refreshing = false;
  int m_interfaceTab = 0;
};
} // namespace ui
