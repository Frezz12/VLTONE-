#pragma once
#include "CreatorAiWorkspace.hpp"
#include "LlmClient.hpp"
#include <QWidget>
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QToolButton;
namespace ui {
class CreatorAiPanel final : public QWidget {
  Q_OBJECT
public:
  explicit CreatorAiPanel(CreatorWindow &, QWidget *parent = nullptr);
  ~CreatorAiPanel() override;
  void stop();
  void projectChanged();
  void contextChanged();
  void documentChanging() { m_workspace->documentChanging(); }
  bool running() const { return m_session->running(); }
  void reloadSettings();
  void setClientForTest(std::unique_ptr<LlmClient>);
  bool sendForTest(const QString &);
  daw::ai::AiSession &sessionForTest() { return *m_session; }
  CreatorAiWorkspace &workspaceForTest() { return *m_workspace; }
signals:
  void stateChanged(bool working, bool completed);
  void settingsRequested();

protected:
  bool eventFilter(QObject *, QEvent *) override;

private:
  void send();
  void step();
  void next(daw::ai::AiSession::Step);
  void render();
  void updateState();
  void saveHistory();
  QString historyPath() const;
  CreatorWindow &m_window;
  std::unique_ptr<CreatorAiWorkspace> m_workspace;
  std::unique_ptr<daw::ai::AiSession> m_session;
  std::unique_ptr<LlmClient> m_client;
  QComboBox *m_models;
  QPlainTextEdit *m_input;
  QLabel *m_context, *m_stage, *m_partialLabel = nullptr;
  QScrollArea *m_scroll;
  QWidget *m_messages;
  QVBoxLayout *m_transcript;
  QToolButton *m_send, *m_stop;
  QPushButton *m_continue, *m_restore;
  QTimer m_saveTimer, m_streamTimer;
  QString m_id, m_model, m_partial;
  std::uint64_t m_epoch = 0, m_request = 0;
  bool m_scripted = false, m_networkPending = false, m_restoring = false;
  bool m_followTail = true;
};
} // namespace ui
