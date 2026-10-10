#include "Creator/CodeUtilities.hpp"
#include "CreatorAiPanel.hpp"
#include "CreatorCanvas.hpp"
#include "CreatorCodeEditor.hpp"
#include "CreatorCommands.hpp"
#include "CreatorStyle.hpp"
#include "CreatorWindow.hpp"
#include "EngineController.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "MiniModuleLibrary.hpp"
#include "Theme.hpp"
#include "serialization/InsertJson.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QUndoStack>
#include <cstdio>
namespace ui {
namespace ai = daw::ai;
using json = nlohmann::json;
using namespace daw::plugins::mini;
namespace {
class CreatorScript final : public LlmClient {
public:
  using Script =
      std::function<ai::ModelReply(int, const std::vector<ai::Message> &)>;
  explicit CreatorScript(Script script, int delay = 1)
      : LlmClient(Provider::OpenAi), script(std::move(script)), delay(delay) {}
  void send(const QString &, const std::vector<ai::Message> &messages,
            Reply reply) override {
    busyFlag = true;
    const auto serial = ++generation;
    auto response = script(calls++, messages);
    QTimer::singleShot(delay, this,
                       [this, serial, reply = std::move(reply),
                        response = std::move(response)]() mutable {
                         if (serial != generation)
                           return;
                         busyFlag = false;
                         reply(std::move(response));
                       });
  }
  void cancel() override {
    ++generation;
    busyFlag = false;
  }
  bool busy() const override { return busyFlag; }
  Script script;
  int calls = 0, delay;
  std::uint64_t generation = 0;
  bool busyFlag = false;
};
bool waitUntil(const std::function<bool()> &done, int limit = 90000) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < limit) {
    QApplication::processEvents();
    QThread::msleep(2);
  }
  QApplication::processEvents();
  return done();
}
} // namespace
bool CreatorWindow::runAiCheck(const QString &directory) {
  unsigned failures = 0;
  QDir().mkpath(directory);
  QFile log(directory + "/checks.log");
  log.open(QIODevice::WriteOnly | QIODevice::Truncate);
  const auto check = [&](bool ok, const char *label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    log.write(QByteArray(ok ? "PASS " : "FAIL ") + label + '\n');
    log.flush();
    failures += !ok;
  };
  QTemporaryDir temporary;
  const auto oldHistory = qgetenv("DAW_CREATOR_AI_HISTORY_DIR");
  qputenv("DAW_CREATOR_AI_HISTORY_DIR", temporary.filePath("history").toUtf8());
  struct RestoreEnv {
    QByteArray value;
    ~RestoreEnv() {
      if (value.isNull())
        qunsetenv("DAW_CREATOR_AI_HISTORY_DIR");
      else
        qputenv("DAW_CREATOR_AI_HISTORY_DIR", value);
    }
  } restore{oldHistory};
  if (qEnvironmentVariableIsSet("DAW_CREATOR_CHECK_CUSTOM_THEME")) {
    auto theme = th();
    theme.background = QColor("#191c18");
    theme.surface = QColor("#272d24");
    theme.surfaceElevated = QColor("#343b30");
    theme.accent = QColor("#c5bc7d");
    ThemeManager::instance().applyCustomTheme(theme, false);
  }
  daw::EngineController controller{};
  check(bool(controller.initialize(48000, 256, false)),
        "AI check controller initializes");
  CreatorWindow window(&controller);
  window.m_recoveryDirectory = temporary.path();
  window.m_installDirectory = temporary.filePath("modules");
  auto initial = CreatorProject::create("AI checks", "AI Gain");
  window.setProjectForTest(initial);
  window.show();
  window.showAi(true);
  QApplication::processEvents();
  auto *panel = window.m_ai;
  auto &workspace = panel->workspaceForTest();
  const auto master = controller.addMiniModule({}, initial.definition);
  auto different = initial.definition;
  different.nodes.push_back(makeNode("gain", "old-gain"));
  different.connections = {{"input", "old-gain", "out", "in"},
                           {"old-gain", "output", "out", "in"}};
  const auto track = controller.addTrack(daw::TrackKind::Audio, "AI target");
  const auto slot = controller.addMiniModule(track, different);
  controller.setInsertParameter(track, slot, "control_1", .37);
  controller.setInsertBypassed(track, slot, true);
  const auto beforeMaster =
      daw::serialization::insertsToJson(controller.miniModules({}));
  auto beforeTrack =
      daw::serialization::insertsToJson(controller.miniModules(track));
  const std::string source = R"(#include <vlt/creator.hpp>
vlt::AudioFrame shade(vlt::AudioFrame input, float gain) { return input * gain; }
VLT_NODE vlt::AudioFrame process(VLT_PORT("input") vlt::AudioFrame input, VLT_PORT("gain") float gain = 0.5f) {
 return shade(input, gain);
}
)";
  const auto tool = [&](const std::string &name, json args) {
    return ai::ModelReply{"",
                          {{"call-" + std::to_string(workspace.revision()),
                            name, std::move(args)}},
                          ""};
  };
  bool compilerErrorSeen = false, installedSeen = false;
  auto script = std::make_unique<CreatorScript>(
      [&](int step, const auto &messages) -> ai::ModelReply {
        const auto revision = workspace.revision();
        switch (step) {
        case 0:
          return tool("creator_context", json::object());
        case 1:
          return tool("creator_sdk",
                      {{"section", "AI_GUIDE_RU.md"}, {"length", 24}});
        case 2: {
          FunctionDefinition f;
          f.source = source + "\nthis is not valid C++;";
          return tool("creator_edit", {{"revision", revision},
                                       {"operations",
                                        {{{"op", "add"},
                                          {"id", "cpp"},
                                          {"type", "cpp_function"},
                                          {"function", functionToJson(f)}}}}});
        }
        case 3:
          return tool(
              "creator_cpp",
              {{"revision", revision}, {"node", "cpp"}, {"action", "analyze"}});
        case 4:
          compilerErrorSeen =
              !messages.back().outcomes.empty() &&
              !messages.back().outcomes.front().ok &&
              messages.back().outcomes.front().result.contains("diagnostics");
          return tool(
              "creator_edit",
              {{"revision", revision},
               {"operations",
                {{{"op", "source"}, {"id", "cpp"}, {"source", source}}}}});
        case 5:
          return tool(
              "creator_cpp",
              {{"revision", revision}, {"node", "cpp"}, {"action", "analyze"}});
        case 6: {
          const auto graph = window.project().graph();
          const auto found =
              std::find_if(graph.nodes.begin(), graph.nodes.end(),
                           [](const auto &n) { return n.id == "cpp"; });
          const auto out = found != graph.nodes.end() && found->function &&
                                   !found->function->outputs.empty()
                               ? found->function->outputs[0].id
                               : "missing";
          return tool("creator_edit", {{"revision", revision},
                                       {"operations",
                                        {{{"op", "disconnect"},
                                          {"from", "input"},
                                          {"from_port", "out"},
                                          {"to", "output"},
                                          {"to_port", "in"}},
                                         {{"op", "connect"},
                                          {"from", "input"},
                                          {"from_port", "out"},
                                          {"to", "cpp"},
                                          {"to_port", "input"}},
                                         {{"op", "connect"},
                                          {"from", "cpp"},
                                          {"from_port", out},
                                          {"to", "output"},
                                          {"to_port", "in"}}}}});
        }
        case 7:
          return tool("creator_cpp", {{"revision", revision},
                                      {"node", "cpp"},
                                      {"action", "extract"},
                                      {"name", "shade"}});
        case 8:
          return tool("creator_build", {{"revision", revision}});
        default:
          installedSeen = !messages.back().outcomes.empty() &&
                          messages.back().outcomes.front().ok;
          return {"Ready — compiled and installed.", {}, ""};
        }
      });
  auto *driver = script.get();
  panel->setClientForTest(std::move(script));
  bool movedDuringBuild = false;
  const auto moveDuringBuild = QObject::connect(
      panel, &CreatorAiPanel::stateChanged, &window, [&](bool working, bool) {
        if (!movedDuringBuild && working && driver->calls == 9 &&
            panel->sessionForTest().waitingForTool()) {
          movedDuringBuild = true;
          controller.setInsertParameter(track, slot, "control_1", .42);
          beforeTrack = daw::serialization::insertsToJson(controller.miniModules(track));
        }
      });
  check(panel->sendForTest("Create a custom C++ gain effect and install it"),
        "Creator assistant starts a scripted implementation request");
  check(waitUntil([&] { return !panel->running(); }),
        "assistant completes its asynchronous tool loop");
  QObject::disconnect(moveDuringBuild);
  auto* live = controller.insertInstance(track, slot);
  const auto controlIndex = live ? live->parameterIndexForId("control_1") : -1;
  check(movedDuringBuild && live && controlIndex >= 0 &&
            std::abs(live->parameterValue(unsigned(controlIndex)) - .42) < 1e-5,
        "manual knob edit during preparation reaches the newly installed DSP");
  check(compilerErrorSeen, "model receives compiler error with structured "
                           "diagnostics and fixes source");
  check(installedSeen && driver->calls == 10,
        "assistant waits for compilation and installs before reporting "
        "completion");
  const auto installedPath = window.m_installDirectory + "/" +
                             QString::fromStdString(initial.definition.id) +
                             ".vltmini";
  const auto installed = MiniModuleLibrary::read(installedPath);
  check(installed.error.isEmpty() && !installed.definition.code.wasm.empty(),
        "unsaved Creator project installs portable C++ module without a Save "
        "dialog");
  check(window.m_path.isEmpty(),
        "AI compilation does not choose or save a Creator project path");
  check(window.m_undo->count() == 1,
        "all uninterrupted AI edits form one Creator Undo");
  check(window.project().definition.nodes.size() == 5,
        "extract helper creates a separate callable function node");
  check(controller.miniModules({}).front().id == master &&
            controller.miniModules(track).front().id == slot &&
            controller.miniModules(track).front().bypassed,
        "Track and Master update without replacing IDs or bypass");
  check(controller.miniModules({}).front().miniModule == installed.definition &&
            controller.miniModules(track).front().miniModule ==
                installed.definition,
        "all affected instances receive the compiled definition");
  check(window.project().positions.value({}).value("input") ==
            initial.positions.value({}).value("input"),
        "AI preserves existing node layout");
  window.resize(900, 650);
  window.showAi(true);
  QApplication::processEvents();
  check(window.m_canvas->width() >= 320 && panel->width() >= 250,
        "900-pixel Creator leaves at least 320 logical pixels for the graph");
  auto *input = panel->findChild<QPlainTextEdit *>("CreatorAiInput");
  input->setPlainText("draft message");
  auto cursor = input->textCursor();
  cursor.setPosition(5);
  input->setTextCursor(cursor);
  const auto viewport = window.m_canvas->viewportState();
  window.showAi(false);
  window.showAi(true);
  QApplication::processEvents();
  check(input->toPlainText() == "draft message" &&
            input->textCursor().position() == 5 &&
            std::abs(window.m_canvas->viewportState().zoom - viewport.zoom) <
                1e-9,
        "panel toggles preserve message draft, cursor and canvas scale");
  window.m_canvas->selectNode("cpp");
  check(window.m_rightStack->currentWidget() == panel,
        "node selection leaves AI panel open");
  const auto beforeDelete = window.project().definition.nodes.size();
  input->selectAll();
  QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QApplication::sendEvent(input, &remove);
  check(input->toPlainText().isEmpty() &&
            window.project().definition.nodes.size() == beforeDelete,
        "Delete in chat edits text and does not delete graph nodes");
  check(window.grab().save(directory + "/creator-ai.png"),
        "real Qt Creator AI screenshot saved");
  QString rollbackError;
  bool rolled = false;
  check(workspace.restoreResult([&](QString error) {
    rollbackError = error;
    rolled = true;
  }),
        "full AI rollback starts");
  check(waitUntil([&] { return rolled; }) && rollbackError.isEmpty(),
        "previous mono/stereo DSP prepares before full rollback publication");
  check(window.project().definition == initial.definition &&
            !QFile::exists(installedPath),
        "first-install rollback restores draft and removes its owned file");
  check(daw::serialization::insertsToJson(controller.miniModules({})) ==
                beforeMaster &&
            daw::serialization::insertsToJson(controller.miniModules(track)) ==
                beforeTrack,
        "rollback restores different original versions per Track/Master "
        "instance");

  // Explain-only requests cannot mutate even if a provider invents an edit
  // call.
  const auto old = window.project().definition;
  panel->setClientForTest(std::make_unique<CreatorScript>(
      [&](int step, const auto &) -> ai::ModelReply {
        if (!step)
          return tool(
              "creator_edit",
              {{"revision", workspace.revision()},
               {"operations",
                {{{"op", "add"}, {"id", "unauthorized"}, {"type", "gain"}}}}});
        return {"This graph passes the input to the output.", {}, ""};
      }));
  panel->sendForTest("Explain how this graph works, do not change it");
  waitUntil([&] { return !panel->running(); });
  check(window.project().definition == old,
        "explanation mode exposes only read-only Creator tools");

  panel->setClientForTest(std::make_unique<CreatorScript>(
      [&](int, const auto &) {
        return tool("creator_edit",
                    {{"revision", workspace.revision()},
                     {"operations",
                      {{{"op", "add"}, {"id", "late"}, {"type", "gain"}}}}});
      },
      120));
  panel->sendForTest("Add a gain node");
  panel->stop();
  waitUntil([&] { return !panel->running(); });
  check(window.project().definition == old,
        "Stop during network request prevents late edits");

  panel->setClientForTest(std::make_unique<CreatorScript>(
      [](int, const auto &) -> ai::ModelReply {
        return {"The input connects to the output.", {}, ""};
      },
      50));
  panel->sendForTest("Explain this graph");
  window.showAi(false);
  check(waitUntil([&] { return !panel->running(); }) &&
            window.m_aiAction->property("aiUnread").toBool(),
        "hidden chat continues and marks completion on the Assistant button");
  window.showAi(true);

  // One successful edit followed by a deliberate compiler wait, then Stop.
  panel->setClientForTest(std::make_unique<CreatorScript>(
      [&](int step, const auto &) -> ai::ModelReply {
        if (!step)
          return tool("creator_build", {{"revision", workspace.revision()}});
        return {"Done", {}, ""};
      }));
  const auto cancelBuild = QObject::connect(
      panel, &CreatorAiPanel::stateChanged, &window, [&](bool working, bool) {
        if (working && panel->sessionForTest().waitingForTool())
          panel->stop();
      });
  panel->sendForTest("Compile this module");
  waitUntil([&] { return !panel->running(); }, 1000);
  QObject::disconnect(cancelBuild);
  check(waitUntil([&] { return !window.m_build->busy(); }) &&
            !QFile::exists(installedPath),
        "Stop cancels owned build and prevents publication");

  // Atomic batch leaves the graph intact if any operation is invalid.
  auto draft = window.project();
  QString error;
  check(!creatorApplyBatch(
            draft,
            json::array({{{"op", "add"}, {"id", "atomic"}, {"type", "gain"}},
                         {{"op", "connect"},
                          {"from", "atomic"},
                          {"from_port", "missing"},
                          {"to", "output"},
                          {"to_port", "in"}}}),
            error) &&
            draft == window.project(),
        "invalid operation rolls back the entire graph batch");

  // Existing file updates restore exact bytes, without reverting unrelated DAW
  // edits.
  check(MiniModuleLibrary::write(installedPath, initial.definition, error),
        "existing module fixture saved");
  CreatorFileState originalFile;
  CreatorFileState::read(installedPath, originalFile, error);
  const auto installUtility = [&] {
    panel->setClientForTest(std::make_unique<CreatorScript>(
        [&](int step, const auto &) -> ai::ModelReply {
          if (!step)
            return tool(
                "creator_edit",
                {{"revision", workspace.revision()},
                 {"operations",
                  {{{"op", "add"}, {"id", "utility"}, {"type", "gain"}}}}});
          if (step == 1)
            return tool("creator_build", {{"revision", workspace.revision()}});
          return {"Installed", {}, ""};
        }));
    panel->sendForTest("Add a utility gain node and install the module");
    return waitUntil([&] { return !panel->running(); }) &&
           panel->sessionForTest().lastError().empty();
  };
  check(installUtility(), "existing module update completes");
  const auto unrelated =
      controller.addTrack(daw::TrackKind::Audio, "Unrelated manual edit");
  rolled = false;
  rollbackError.clear();
  workspace.restoreResult([&](QString e) {
    rollbackError = e;
    rolled = true;
  });
  waitUntil([&] { return rolled; });
  CreatorFileState restoredFile;
  CreatorFileState::read(installedPath, restoredFile, error);
  check(rollbackError.isEmpty() && restoredFile == originalFile,
        "update rollback restores the previous installed file exactly");
  check(std::any_of(controller.project().tracks.begin(),
                    controller.project().tracks.end(),
                    [&](const auto &t) { return t.id == unrelated; }),
        "full AI rollback preserves unrelated DAW changes");
  check(installUtility(), "second update prepares conflict checks");
  const auto afterUpdate = window.project();
  CreatorFileState expectedFile;
  CreatorFileState::read(installedPath, expectedFile, error);
  QFile changed(installedPath);
  changed.open(QIODevice::Append);
  changed.write("\n ");
  changed.close();
  rolled = false;
  rollbackError.clear();
  workspace.restoreResult([&](QString e) {
    rollbackError = e;
    rolled = true;
  });
  waitUntil([&] { return rolled; });
  check(!rollbackError.isEmpty() && window.project() == afterUpdate,
        "changed installed file refuses the whole rollback without changing "
        "Creator");
  check(MiniModuleLibrary::write(installedPath, afterUpdate.definition, error),
        "restore the expected file for the instance conflict check");
  controller.setInsertParameter(track, slot, "control_1", .91);
  rolled = false;
  rollbackError.clear();
  workspace.restoreResult([&](QString e) {
    rollbackError = e;
    rolled = true;
  });
  waitUntil([&] { return rolled; });
  CreatorFileState afterConflict;
  CreatorFileState::read(installedPath, afterConflict, error);
  check(!rollbackError.isEmpty() && window.project() == afterUpdate &&
            afterConflict == expectedFile,
        "changed live instance refuses rollback before publishing any file or "
        "DSP");
  controller.setInsertParameter(track, slot, "control_1", .37);
  window.edit("Manual change after AI",
              [&](auto &p) { p.definition.name = "Manual module name"; });
  rolled = false;
  rollbackError.clear();
  workspace.restoreResult([&](QString e) {
    rollbackError = e;
    rolled = true;
  });
  check(rolled && !rollbackError.isEmpty() &&
            window.project().definition.name == "Manual module name",
        "manual Creator edits after AI are never overwritten by full rollback");

  // A response based on an earlier revision receives a conflict instead of
  // applying.
  panel->setClientForTest(std::make_unique<CreatorScript>(
      [&](int step, const auto &) -> ai::ModelReply {
        if (!step)
          return tool(
              "creator_edit",
              {{"revision", workspace.revision()},
               {"operations",
                {{{"op", "add"}, {"id", "stale-node"}, {"type", "gain"}}}}});
        return {"The document changed; no edit was applied.", {}, ""};
      },
      35));
  panel->sendForTest("Add a node");
  window.edit("Manual edit during response",
              [&](auto &p) { p.definition.name = "Edited while thinking"; });
  waitUntil([&] { return !panel->running(); });
  check(std::none_of(window.project().definition.nodes.begin(),
                     window.project().definition.nodes.end(),
                     [](const auto &n) { return n.id == "stale-node"; }),
        "manual edits during model response reject stale graph changes");

  // Chat data stays local and follows the module ID, including a Save As copy.
  input->setPlainText("saved local draft");
  auto savedCursor = input->textCursor();
  savedCursor.setPosition(4);
  input->setTextCursor(savedCursor);
  auto saved = window.project();
  const auto savedMessages = panel->sessionForTest().messages().size();
  window.setProjectForTest(
      CreatorProject::create("Another project", "Another module"));
  check(panel->sessionForTest().messages().empty(),
        "new Creator project has a separate conversation");
  window.setProjectForTest(saved);
  check(panel->sessionForTest().messages().size() == savedMessages &&
            input->toPlainText() == "saved local draft" &&
            input->textCursor().position() == 4,
        "local history, draft and cursor reload by stable module ID");
  check(!saved.toJson().contains("messages") &&
            !toJson(saved.definition).contains("messages"),
        "portable projects and modules never embed the AI conversation");
  window.m_undo->setClean();
  window.hide();
  return failures == 0;
}
} // namespace ui
