#include "CreatorAiPanel.hpp"
#include "AccountService.hpp"
#include "AiChatShared.hpp"
#include "Creator/CodeUtilities.hpp"
#include "CreatorCanvas.hpp"
#include "CreatorCodeEditor.hpp"
#include "CreatorStyle.hpp"
#include "CreatorWindow.hpp"
#include "Theme.hpp"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTextCursor>
#include <QVBoxLayout>
namespace ui {
namespace ai = daw::ai;
using json = nlohmann::json;
CreatorAiPanel::CreatorAiPanel(CreatorWindow &window, QWidget *parent)
    : QWidget(parent), m_window(window),
      m_workspace(std::make_unique<CreatorAiWorkspace>(window)),
      m_session(std::make_unique<ai::AiSession>(*m_workspace)) {
  setObjectName("CreatorAiPanel");
  setMinimumWidth(250);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 8, 10, 10);
  layout->setSpacing(8);
  auto *header = new QHBoxLayout;
  header->setSpacing(4);
  m_models = new QComboBox(this);
  m_models->setObjectName("CreatorAiModel");
  m_models->setMinimumWidth(0);
  m_models->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_models->setMinimumContentsLength(8);
  m_models->setAccessibleName(tr("AI model"));
  header->addWidget(m_models, 1);
  auto *history =
      aiMessageAction(this, icons::Glyph::Clock, tr("Conversation history"));
  header->addWidget(history);
  auto *settings = aiMessageAction(this, icons::Glyph::Gear, tr("AI settings"));
  header->addWidget(settings);
  connect(settings, &QToolButton::clicked, this,
          &CreatorAiPanel::settingsRequested);
  connect(history, &QToolButton::clicked, this, [this, history] {
    QMenu menu(this);
    for (std::size_t i = 0; i < m_session->messages().size(); ++i) {
      const auto &msg = m_session->messages()[i];
      if (msg.role != ai::Role::User)
        continue;
      auto *a = menu.addAction(
          QString::fromStdString(msg.text).simplified().left(60));
      connect(a, &QAction::triggered, this, [this, i] {
        if (auto *card = m_messages->findChild<QWidget *>("CreatorAiTurn" +
                                                          QString::number(i)))
          m_scroll->ensureWidgetVisible(card);
      });
    }
    if (menu.actions().empty())
      menu.addAction(tr("No messages yet"))->setEnabled(false);
    menu.exec(history->mapToGlobal(QPoint(0, history->height())));
  });
  layout->addLayout(header);
  m_context = aiCardText(this, {}, "CreatorAiContext", true);
  m_context->setProperty("creatorRole", "muted");
  layout->addWidget(m_context);
  m_scroll = new QScrollArea(this);
  m_scroll->setWidgetResizable(true);
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_scroll->setAccessibleName(tr("Creator conversation"));
  m_scroll->viewport()->installEventFilter(this);
  m_scroll->verticalScrollBar()->installEventFilter(this);
  connect(m_scroll->verticalScrollBar(), &QScrollBar::sliderMoved, this,
          [this](int value) {
            m_followTail =
                value >= m_scroll->verticalScrollBar()->maximum() - 20;
          });
  m_messages = new QWidget(m_scroll);
  m_transcript = new QVBoxLayout(m_messages);
  m_transcript->setContentsMargins(0, 0, 0, 0);
  m_transcript->setSpacing(10);
  m_scroll->setWidget(m_messages);
  layout->addWidget(m_scroll, 1);
  m_stage = aiCardText(this, {}, "CreatorAiStage", true);
  m_stage->setProperty("creatorRole", "muted");
  layout->addWidget(m_stage);
  m_input = new QPlainTextEdit(this);
  m_input->setObjectName("CreatorAiInput");
  m_input->setPlaceholderText(
      tr("Describe an effect or ask about this graph…"));
  m_input->setAccessibleName(tr("Message to Creator assistant"));
  m_input->setMinimumHeight(80);
  m_input->setMaximumHeight(140);
  m_input->setTabChangesFocus(false);
  m_input->installEventFilter(this);
  layout->addWidget(m_input);
  auto *footer = new QHBoxLayout;
  footer->setSpacing(4);
  m_continue = new QPushButton(tr("Continue"), this);
  m_continue->setObjectName("CreatorAiContinue");
  footer->addWidget(m_continue);
  footer->addStretch();
  m_send = aiMessageAction(this, icons::Glyph::ArrowUp, tr("Send message"));
  m_send->setObjectName("CreatorAiSend");
  m_stop = aiMessageAction(this, icons::Glyph::Stop, tr("Stop"));
  m_stop->setObjectName("CreatorAiStop");
  footer->addWidget(m_send);
  footer->addWidget(m_stop);
  layout->addLayout(footer);
  m_restore = new QPushButton(tr("Undo AI result"), this);
  m_restore->setObjectName("CreatorAiRestore");
  layout->addWidget(m_restore);
  connect(m_send, &QToolButton::clicked, this, &CreatorAiPanel::send);
  connect(m_stop, &QToolButton::clicked, this, &CreatorAiPanel::stop);
  connect(m_continue, &QPushButton::clicked, this, [this] {
    if (running() || m_restoring || m_window.m_build->busy())
      return;
    reloadSettings();
    if (!m_client) {
      m_stage->setText(tr("Open AI settings to connect a model."));
      return;
    }
    if (!m_session->resume())
      return;
    ++m_epoch;
    m_partial.clear();
    render();
    updateState();
    saveHistory();
    step();
  });
  connect(m_restore, &QPushButton::clicked, this, [this] {
    m_restoring = true;
    updateState();
    if (!m_workspace->restoreResult([this](const QString &error) {
          m_restoring = false;
          m_stage->setText(error.isEmpty() ? tr("State before AI restored.")
                                           : error);
          updateState();
          saveHistory();
        })) {
      m_restoring = false;
      updateState();
    }
  });
  connect(m_models, qOverload<int>(&QComboBox::activated), this, [this](int i) {
    m_model = m_models->itemData(i).toString();
    reloadSettings();
    m_saveTimer.start();
  });
  m_saveTimer.setInterval(500);
  m_saveTimer.setSingleShot(true);
  connect(&m_saveTimer, &QTimer::timeout, this, &CreatorAiPanel::saveHistory);
  connect(m_input, &QPlainTextEdit::textChanged, this, [this] {
    m_saveTimer.start();
    updateState();
  });
  connect(m_input, &QPlainTextEdit::cursorPositionChanged, this,
          [this] { m_saveTimer.start(); });
  m_streamTimer.setInterval(16);
  m_streamTimer.setSingleShot(true);
  connect(&m_streamTimer, &QTimer::timeout, this, [this] {
    if (m_partialLabel)
      m_partialLabel->setText(m_partial);
    if (m_followTail)
      m_scroll->verticalScrollBar()->setValue(
          m_scroll->verticalScrollBar()->maximum());
  });
  m_workspace->completed = [this](std::uint64_t run, std::string call,
                                  ai::ToolResult result) {
    if (!running() || run != m_session->runId() || !m_session->waitingForTool())
      return;
    next(m_session->completeTool(run, call, result));
  };
  m_workspace->progress = [this](const QString &stage) {
    m_stage->setText(stage);
  };
  connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
    for (auto *b : findChildren<QToolButton *>())
      if (b->property("aiGlyph").isValid())
        b->setIcon(icons::icon(icons::Glyph(b->property("aiGlyph").toInt()),
                               creatorColors().muted, 16));
  });
  if (auto *account = account::Service::instance()) {
    connect(account, &account::Service::aiModelsChanged, this,
            &CreatorAiPanel::reloadSettings);
    connect(account, &account::Service::authenticatedChanged, this,
            [this] { reloadSettings(); });
  }
  projectChanged();
}
CreatorAiPanel::~CreatorAiPanel() {
  stop();
  saveHistory();
}
bool CreatorAiPanel::eventFilter(QObject *object, QEvent *event) {
  if ((object == m_scroll->viewport() ||
       object == m_scroll->verticalScrollBar()) &&
      (event->type() == QEvent::Wheel || event->type() == QEvent::KeyPress)) {
    m_followTail = false;
    QTimer::singleShot(0, this, [this] {
      m_followTail = m_scroll->verticalScrollBar()->value() >=
                     m_scroll->verticalScrollBar()->maximum() - 20;
    });
  }
  if (object == m_input && event->type() == QEvent::ShortcutOverride) {
    event->accept();
    return true;
  }
  if (object == m_input && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
        !(key->modifiers() & Qt::ShiftModifier)) {
      send();
      return true;
    }
  }
  return QWidget::eventFilter(object, event);
}
void CreatorAiPanel::contextChanged() {
  const auto &p = m_window.m_project;
  QStringList parts{QString::fromStdString(p.definition.name)};
  if (!p.activeMode.isEmpty())
    for (const auto &m : p.definition.modes)
      if (m.id == p.activeMode.toStdString())
        parts << QString::fromStdString(m.name);
  if (!p.graphPath.empty())
    parts << p.graphPath.join(" / ");
  auto selected = m_window.m_canvas->selectedNodes();
  if (!selected.empty())
    parts << tr("Selected: %1").arg(selected.join(", "));
  const auto context = parts.join(" · ");
  m_context->setToolTip(context);
  m_context->setText(fontMetrics().elidedText(context, Qt::ElideMiddle,
                                              std::max(180, width() - 24)));
}
void CreatorAiPanel::reloadSettings() {
  if (running())
    return;
  QSignalBlocker blocked(m_models);
  m_models->clear();
  for (const auto &c : aiprefs::availableModels())
    m_models->addItem(c.displayName, c.id);
  if (m_model.isEmpty())
    m_model = aiprefs::activeModelId();
  int index = m_models->findData(m_model);
  if (index < 0 && m_models->count())
    index = 0;
  m_models->setCurrentIndex(index);
  if (index >= 0)
    m_model = m_models->itemData(index).toString();
  if (!m_scripted) {
    aiprefs::ModelConnection c;
    if (aiprefs::modelById(m_model, &c)) {
      auto provider = c.provider == aiprefs::Provider::OpenAi
                          ? LlmClient::Provider::OpenAi
                          : LlmClient::Provider::Anthropic;
      if (!m_client || m_client->provider() != provider)
        m_client = std::make_unique<LlmClient>(provider);
      m_client->setConfig(aiModelConfig(c));
    } else
      m_client.reset();
  }
  m_session->setMaxIterations(aiprefs::maxIterations());
  m_session->setHistoryLimit(aiprefs::historyLimit());
  updateState();
}
void CreatorAiPanel::setClientForTest(std::unique_ptr<LlmClient> client) {
  stop();
  m_scripted = true;
  m_client = std::move(client);
  updateState();
}
bool CreatorAiPanel::sendForTest(const QString &text) {
  m_input->setPlainText(text);
  send();
  return running();
}
void CreatorAiPanel::send() {
  if (running() || m_restoring || m_window.m_build->busy())
    return;
  const auto prompt = m_input->toPlainText().trimmed();
  if (prompt.isEmpty())
    return;
  reloadSettings();
  if (!m_client) {
    m_stage->setText(tr("Open AI settings to connect a model."));
    return;
  }
  if (!m_session->begin(prompt.toStdString()))
    return;
  ++m_epoch;
  m_partial.clear();
  m_followTail = true;
  m_input->clear();
  render();
  updateState();
  saveHistory();
  step();
}
void CreatorAiPanel::step() {
  if (!running() || m_session->waitingForTool() || m_networkPending ||
      !m_client)
    return;
  m_networkPending = true;
  const auto epoch = m_epoch, request = ++m_request;
  m_client->setAvailableTools(m_session->availableTools());
  m_client->setPartialSink([this, epoch, request](const auto &text) {
    if (epoch != m_epoch || request != m_request || !m_networkPending)
      return;
    m_partial += text;
    m_streamTimer.start();
  });
  m_client->setUsageSink([this, epoch](auto usage) {
    if (epoch == m_epoch)
      m_session->addUsage(usage);
  });
  m_client->setStatusSink([this, epoch](const auto &status) {
    if (epoch == m_epoch)
      m_stage->setText(status);
  });
  m_stage->setText(tr("Thinking…"));
  m_client->send(QString::fromStdString(m_session->systemPrompt()),
                 m_session->wireMessages(), [this, epoch, request](auto reply) {
                   if (epoch != m_epoch || request != m_request ||
                       !m_networkPending || !running())
                     return;
                   m_networkPending = false;
                   if (!reply.error.empty() && reply.text.empty())
                     reply.text = m_partial.toStdString();
                   m_partial.clear();
                   m_streamTimer.stop();
                   next(m_session->applyReply(reply));
                 });
}
void CreatorAiPanel::next(ai::AiSession::Step value) {
  render();
  updateState();
  saveHistory();
  contextChanged();
  if (value == ai::AiSession::Step::NeedsRequest)
    QTimer::singleShot(0, this, [this] { step(); });
  else if (value != ai::AiSession::Step::WaitingForTool) {
    m_stage->setText(m_session->lastError().empty()
                         ? tr("Completed")
                         : QString::fromStdString(m_session->lastError()));
    emit stateChanged(false, true);
  }
}
void CreatorAiPanel::stop() {
  if (!m_session || !running())
    return;
  ++m_epoch;
  m_networkPending = false;
  m_streamTimer.stop();
  if (m_client)
    m_client->cancel();
  m_session->cancel();
  if (running())
    m_session->applyReply({m_partial.toStdString(),
                           {},
                           "Stopped. You can continue this request."});
  m_partial.clear();
  render();
  updateState();
  saveHistory();
  m_stage->setText(tr("Stopped. You can continue this request."));
}
void CreatorAiPanel::updateState() {
  const bool busy = running() || m_restoring;
  m_models->setEnabled(!busy);
  m_send->setVisible(!busy);
  m_send->setEnabled(m_client && !m_input->toPlainText().trimmed().isEmpty());
  m_stop->setVisible(running());
  m_continue->setVisible(!busy && !m_session->lastError().empty());
  m_restore->setVisible(!busy && m_workspace->canRestore());
  emit stateChanged(busy, false);
}
void CreatorAiPanel::render() {
  const int scroll = m_scroll->verticalScrollBar()->value();
  const bool atEnd = m_followTail;
  while (auto *item = m_transcript->takeAt(0)) {
    if (item->widget())
      delete item->widget();
    delete item;
  }
  m_partialLabel = nullptr;
  const auto &messages = m_session->messages();
  if (messages.empty())
    m_transcript->addWidget(aiCardText(
        m_messages,
        tr("Describe the mini-module you want. The assistant can connect "
           "nodes, write C++, fix compiler errors and install the result."),
        "AiEmptyText"));
  for (std::size_t i = 0; i < messages.size(); ++i) {
    const auto &msg = messages[i];
    if (msg.role == ai::Role::Tool) {
      for (const auto &outcome : msg.outcomes) {
        auto *details = new QToolButton(m_messages);
        details->setAutoRaise(true);
        details->setToolButtonStyle(Qt::ToolButtonTextOnly);
        const auto stage =
            outcome.name == "creator_build"  ? tr("Compile and install")
            : outcome.name == "creator_cpp"  ? tr("Check C++ ports")
            : outcome.name == "creator_edit" ? tr("Update graph")
                                             : tr("Read context");
        details->setText(
            (outcome.ok ? QStringLiteral("✓ ") : QStringLiteral("! ")) + stage);
        details->setAccessibleName(stage);
        m_transcript->addWidget(details);
        auto *body = new QWidget(m_messages);
        auto *lines = new QVBoxLayout(body);
        lines->setContentsMargins(6, 2, 6, 4);
        const auto error = QString::fromStdString(
            outcome.result.value("error", std::string{}));
        if (!error.isEmpty())
          lines->addWidget(aiCardText(body, error, "AiError"));
        if (outcome.result.contains("diagnostics"))
          for (const auto &d : outcome.result["diagnostics"]) {
            const auto node =
                QString::fromStdString(d.value("node", std::string{}));
            const unsigned line = d.value("line", 0u),
                           column = d.value("column", 0u);
            auto *link =
                new QPushButton(QString("%1:%2 · %3")
                                    .arg(node)
                                    .arg(line)
                                    .arg(QString::fromStdString(
                                        d.value("message", std::string{}))),
                                body);
            link->setToolTip(link->text());
            lines->addWidget(link);
            connect(link, &QPushButton::clicked, this,
                    [this, node, line, column] {
                      m_window.m_canvas->selectNode(node);
                      if (line) {
                        m_window.openCode(node);
                        m_window.m_code->reveal(line, column);
                      }
                    });
          }
        if (lines->isEmpty())
          lines->addWidget(
              aiCardText(body, outcome.ok ? tr("Done") : tr("Operation failed"),
                         "AiToolDetail"));
        body->hide();
        m_transcript->addWidget(body);
        connect(details, &QToolButton::clicked, body,
                [body] { body->setVisible(!body->isVisible()); });
      }
    } else if (!msg.text.empty()) {
      auto *card = new QWidget(m_messages);
      card->setObjectName("CreatorAiTurn" + QString::number(i));
      auto *l = new QVBoxLayout(card);
      l->setContentsMargins(8, 6, 8, 6);
      l->setSpacing(4);
      auto *role = aiCardText(
          card, msg.role == ai::Role::User ? tr("You") : tr("Assistant"),
          "AiRole", true);
      role->setProperty("creatorRole", "muted");
      l->addWidget(role);
      l->addWidget(
          aiCardText(card, QString::fromStdString(msg.text), "AiMessageText"));
      m_transcript->addWidget(card);
    }
  }
  if (running()) {
    m_partialLabel = aiCardText(m_messages, m_partial, "AiPartialText");
    m_transcript->addWidget(m_partialLabel);
  }
  m_transcript->addStretch();
  QTimer::singleShot(20, this, [this, scroll, atEnd] {
    m_scroll->verticalScrollBar()->setValue(
        atEnd && m_followTail ? m_scroll->verticalScrollBar()->maximum()
                              : scroll);
  });
}
QString CreatorAiPanel::historyPath() const {
  if (m_id.isEmpty())
    return {};
  auto directory = qEnvironmentVariable("DAW_CREATOR_AI_HISTORY_DIR");
  if (directory.isEmpty())
    directory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
        "/Creator/ai";
  return directory + "/" +
         QString::fromStdString(
             daw::plugins::mini::codeHash(m_id.toStdString())) +
         ".json";
}
void CreatorAiPanel::saveHistory() {
  const auto path = historyPath();
  if (path.isEmpty())
    return;
  json messages = json::array();
  for (const auto &msg : m_session->messages()) {
    json j{{"role", int(msg.role)},
           {"text", msg.text},
           {"calls", json::array()},
           {"outcomes", json::array()}};
    for (const auto &c : msg.calls)
      j["calls"].push_back({{"id", c.id},
                            {"name", c.name},
                            {"args", c.args},
                            {"from_text", c.fromText}});
    for (const auto &o : msg.outcomes)
      j["outcomes"].push_back({{"id", o.callId},
                               {"name", o.name},
                               {"ok", o.ok},
                               {"result", o.result},
                               {"from_text", o.fromText}});
    messages.push_back(std::move(j));
  }
  const json doc{
      {"version", 1},
      {"module", m_id.toStdString()},
      {"model", m_model.toStdString()},
      {"draft", m_input->toPlainText().toStdString()},
      {"cursor", m_input->textCursor().position()},
      {"messages", messages},
      {"mode", int(m_session->context().mode)},
      {"error",
       running()
           ? "Previous request was interrupted; inspect state and continue."
           : m_session->lastError()},
      {"usage",
       {{"input", m_session->usage().inputTokens},
        {"output", m_session->usage().outputTokens},
        {"cached", m_session->usage().cachedTokens},
        {"cache_creation", m_session->usage().cacheCreationTokens}}}};
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  const auto bytes = QByteArray::fromStdString(doc.dump());
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit())
    m_stage->setText(tr("Could not save the local conversation."));
}
void CreatorAiPanel::projectChanged() {
  const auto id = QString::fromStdString(m_window.m_project.definition.id);
  if (id == m_id) {
    contextChanged();
    return;
  }
  stop();
  saveHistory();
  m_workspace->resetProject();
  m_session->clear();
  m_id = id;
  m_model.clear();
  m_input->clear();
  m_stage->clear();
  QFile file(historyPath());
  if (file.open(QIODevice::ReadOnly) && file.size() <= 32 * 1024 * 1024) {
    try {
      const auto j = json::parse(file.readAll().toStdString());
      if (j.value("version", 0) != 1 ||
          j.value("module", std::string{}) != id.toStdString())
        throw std::runtime_error("Unknown history");
      m_model = QString::fromStdString(j.value("model", std::string{}));
      m_input->setPlainText(
          QString::fromStdString(j.value("draft", std::string{})));
      auto cursor = m_input->textCursor();
      cursor.setPosition(std::clamp(j.value("cursor", 0), 0,
                                    m_input->document()->characterCount() - 1));
      m_input->setTextCursor(cursor);
      std::vector<ai::Message> messages;
      for (const auto &entry : j.at("messages")) {
        const auto role = entry.at("role").get<int>();
        if (role < 0 || role > 2)
          throw std::runtime_error("Invalid role");
        ai::Message m;
        m.role = ai::Role(role);
        m.text = entry.value("text", std::string{});
        for (const auto &c : entry.at("calls"))
          m.calls.push_back({c.at("id"), c.at("name"), c.at("args"),
                             c.value("from_text", false)});
        for (const auto &o : entry.at("outcomes"))
          m.outcomes.push_back({o.at("id"), o.at("name"), o.at("ok"),
                                o.at("result"), o.value("from_text", false)});
        messages.push_back(std::move(m));
      }
      m_session->restoreMessages(
          std::move(messages), j.value("error", std::string{}),
          ai::InteractionMode(std::clamp(j.value("mode", 2), 0, 3)));
      if (j.contains("usage")) {
        const auto &u = j.at("usage");
        m_session->addUsage({u.value("input", std::uint64_t{}),
                             u.value("output", std::uint64_t{}),
                             u.value("cached", std::uint64_t{}),
                             u.value("cache_creation", std::uint64_t{})});
      }
    } catch (const std::exception &) {
      m_stage->setText(tr("Local conversation could not be loaded."));
    }
  }
  reloadSettings();
  contextChanged();
  render();
  updateState();
}
} // namespace ui
