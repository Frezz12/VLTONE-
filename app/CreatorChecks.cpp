#include "Creator/CodeUtilities.hpp"
#include "CollaborationCommandBridge.hpp"
#include "collaboration/CommandGateway.hpp"
#include "CreatorCanvas.hpp"
#include "CreatorCodeEditor.hpp"
#include "CreatorProject.hpp"
#include "CreatorStyle.hpp"
#include "CreatorWindow.hpp"
#include "EngineController.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "MiniModuleLibrary.hpp"
#include "MiniModuleUpdate.hpp"
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMouseEvent>
#include <QMenu>
#include <QToolBar>
#include <QToolButton>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <QWheelEvent>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace ui {
bool CreatorWindow::runCheck(const QString &directory) {
  using namespace daw::plugins::mini;
  unsigned failures = 0;
  QDir().mkpath(directory);
  QFile log(QDir(directory).filePath("checks.log"));
  if (!directory.isEmpty())
    log.open(QIODevice::WriteOnly | QIODevice::Truncate);
  const auto check = [&](bool ok, const char *label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (log.isOpen()) {
      log.write(QByteArray(ok ? "PASS " : "FAIL ") + label + '\n');
      log.flush();
    }
    failures += !ok;
  };
  daw::collab::CommandGateway gateway;
  ::collab::CollaborationCommandBridge bridge(nullptr, &gateway);
  daw::EngineController controller;
  QTemporaryDir temporary;
  check(bool(controller.initialize(48000, 256, false)),
        "Creator controller initialized");
  controller.attachSharedMutationSink(bridge);
  check(!controller.hasCloudProjectBinding(),
        "Creator uses the application's attached bridge in a local project");
  CreatorWindow window(&controller);
  window.m_recoveryDirectory = temporary.path();
  auto project = CreatorProject::create("Creator checks", "Motion Chorus");
  auto &d = project.definition;
  d.nodes.push_back(makeNode("chorus", "chorus"));
  d.nodes.push_back(makeNode("lfo", "lfo"));
  d.nodes.push_back(makeNode("map_range", "map"));
  d.connections = {{"input", "chorus", "out", "in"},
                   {"lfo", "map", "out", "value"},
                   {"map", "chorus", "out", "amount"},
                   {"interface", "lfo", "control_1", "rate"},
                   {"chorus", "output", "out", "in"}};
  project.positions[{}] = {{"input", {30, 45}},   {"chorus", {650, 45}},
                           {"output", {960, 45}}, {"interface", {30, 250}},
                           {"lfo", {330, 240}},   {"map", {650, 390}}};
  check(validate(d).empty(), "audible modulation graph is valid");
  window.setProjectForTest(project);
  window.show();
  QApplication::processEvents();
  bool searchIconCentered = false;
  for (auto *button : window.m_search->findChildren<QToolButton *>())
    if (button->isVisible())
      searchIconCentered = std::abs(button->geometry().center().y() -
                                    window.m_search->rect().center().y()) <= 2 &&
                           window.m_search->rect().contains(button->geometry());
  check(searchIconCentered, "search icon is centered inside its native field");
  window.m_canvas->fitGraph();
  QApplication::processEvents();
  const auto stable = project;
  QString error;
  check(project.save(temporary.filePath("project.vltcreator"), error),
        "Creator project saved");
  CreatorProject reopened;
  check(CreatorProject::open(temporary.filePath("project.vltcreator"), reopened,
                             error) &&
            reopened == project,
        "Creator graph and positions restored exactly");
  project.addMode("Wide");
  check(project.definition.modes.size() == 2 &&
            validate(project.definition).empty(),
        "mode copies the current graph");
  project.removeMode(project.activeMode);
  check(project.definition.modes.empty() &&
            project.definition.nodes == stable.definition.nodes,
        "removing mode restores single graph");
  auto incomplete = stable;
  incomplete.definition.connections.clear();
  check(incomplete.save(temporary.filePath("draft.vltcreator"), error) &&
            CreatorProject::open(temporary.filePath("draft.vltcreator"),
                                 reopened, error) &&
            reopened.definition.connections.empty(),
        "incomplete drafts remain editable");
  window.addNode("constant", {360, 650});
  check(window.project().definition.nodes.size() ==
            stable.definition.nodes.size() + 1,
        "library adds a node");
  window.m_undo->undo();
  check(window.project().definition.nodes.size() ==
            stable.definition.nodes.size(),
        "Creator Undo restores graph");
  window.m_undo->redo();
  window.m_canvas->selectNode(
      QString::fromStdString(window.project().definition.nodes.back().id));
  window.copySelection();
  window.pasteSelection({700, 680});
  check(window.project().definition.nodes.size() ==
            stable.definition.nodes.size() + 2,
        "copy/paste remaps node IDs");
  window.m_undo->undo();
  window.m_undo->undo();
  // Deliver real viewport events: scene transforms, hit testing and connection
  // gestures must work at every tested DPI, rather than only emitting signals.
  auto *canvas = window.m_canvas;
  canvas->fitGraph();
  canvas->setFocus();
  const auto mouse = [&](QEvent::Type type, QPointF point,
                         Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, point,
                      canvas->viewport()->mapToGlobal(point.toPoint()), button,
                      buttons, Qt::NoModifier);
    QApplication::sendEvent(canvas->viewport(), &event);
  };
  const auto positions = canvas->nodePositions();
  const auto head =
      canvas->mapFromScene(positions.value("lfo") + QPointF(80, 14));
  const auto destination = head + QPoint(42, 25);
  const auto undoBeforeDrag = window.m_undo->index();
  mouse(QEvent::MouseButtonPress, head, Qt::LeftButton, Qt::LeftButton);
  mouse(QEvent::MouseMove, destination, Qt::NoButton, Qt::LeftButton);
  mouse(QEvent::MouseButtonRelease, destination, Qt::LeftButton, Qt::NoButton);
  QApplication::processEvents();
  check(canvas->nodePositions().value("lfo") != positions.value("lfo") &&
            window.m_undo->index() == undoBeforeDrag + 1,
        "native node drag is one Undo at the active DPI");
  window.m_undo->undo();
  const auto portPoint = [&](const QString &node, const QString &port, bool output) {
    for (auto *item : canvas->scene()->items())
      if (item->parentItem() && item->parentItem()->data(10).toString() == node &&
          item->data(12).toString() == port && item->data(13).toBool() == output)
        return canvas->mapFromScene(item->scenePos());
    return QPoint(-10000, -10000);
  };
  const auto sourcePort = portPoint("lfo", "out", true);
  const auto inputPort = portPoint("chorus", "rate", false);
  mouse(QEvent::MouseButtonPress, sourcePort, Qt::LeftButton, Qt::LeftButton);
  mouse(QEvent::MouseMove, inputPort, Qt::NoButton, Qt::LeftButton);
  mouse(QEvent::MouseButtonRelease, inputPort, Qt::LeftButton, Qt::NoButton);
  QApplication::processEvents();
  check(window.project().definition.connections.size() ==
            stable.definition.connections.size() + 1,
        "native port drag creates a typed modulation wire");
  window.m_undo->undo();
  const auto anchor = canvas->viewport()->rect().center();
  const auto sceneAnchor = canvas->mapToScene(anchor);
  const double oldZoom = canvas->transform().m11();
  QWheelEvent wheel(anchor, canvas->viewport()->mapToGlobal(anchor), {},
                    {0, 120}, Qt::NoButton, Qt::ControlModifier,
                    Qt::NoScrollPhase, false);
  QApplication::sendEvent(canvas->viewport(), &wheel);
  check(canvas->transform().m11() > oldZoom &&
            QLineF(sceneAnchor, canvas->mapToScene(anchor)).length() < 3,
        "Control wheel zoom preserves the pointer anchor");
  QKeyEvent actualSize(QEvent::KeyPress, Qt::Key_0, Qt::ControlModifier);
  QApplication::sendEvent(canvas, &actualSize);
  check(canvas->transform().m11() == 1,
        "keyboard actual-size command reacts immediately");
  const auto fieldFor = [&](const QString &node) -> CreatorNumberField * {
    for (auto *item : canvas->scene()->items())
      if (auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item))
        if (proxy->parentItem() && proxy->parentItem()->data(10).toString() == node)
          if (auto *field = qobject_cast<CreatorNumberField *>(proxy->widget());
              field && field->isEnabled())
            return field;
    return nullptr;
  };
  auto *field = fieldFor("chorus");
  check(field && !field->accessibleName().isEmpty() &&
            !field->accessibleDescription().isEmpty(),
        "node numeric fields have accessible names and gesture instructions");
  if (field) {
    const double initial = field->value();
    const auto nodePositions = canvas->nodePositions();
    const auto proxyPosition = field->graphicsProxyWidget()->sceneBoundingRect().center();
    canvas->centerOn(proxyPosition);
    QApplication::processEvents();
    const auto point = canvas->mapFromScene(proxyPosition);
    const auto history = window.m_undo->index();
    mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, point + QPoint(0, -40), Qt::NoButton, Qt::LeftButton);
    const double dragged = field->value();
    check(dragged > initial && window.m_undo->index() == history &&
              canvas->nodePositions() == nodePositions,
          "numeric vertical drag changes value immediately without moving/rebuilding nodes");
    mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    check(window.m_undo->index() == history + 1 &&
              fieldFor("chorus") && fieldFor("chorus")->value() == dragged,
          "one numeric gesture commits one project Undo");
    window.m_undo->undo();
    field = fieldFor("chorus");
    check(field && field->value() == initial,
          "Undo restores the number and native node control");
    if (field) {
      // Also deliver events directly to the native field to exercise fine
      // adjustment and cancellation independently of viewport zoom/scroll.
      const auto sendField = [&](QEvent::Type type, QPoint offset,
                                  Qt::MouseButton button, Qt::MouseButtons buttons,
                                  Qt::KeyboardModifiers modifiers) {
        const auto local = field->rect().center() + offset;
        QMouseEvent event(type, local, field->mapToGlobal(local), button, buttons, modifiers);
        QApplication::sendEvent(field, &event);
      };
      sendField(QEvent::MouseButtonPress, {}, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      sendField(QEvent::MouseMove, {0, -40}, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
      check(field->value() > initial && field->value() < dragged,
            "Shift provides finer numeric adjustment");
      QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      QApplication::sendEvent(field, &cancel);
      sendField(QEvent::MouseButtonRelease, {}, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QApplication::processEvents();
      check(fieldFor("chorus")->value() == initial && window.m_undo->index() == history,
            "Escape cancels the gesture without a history entry");
      QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(field, &enter);
      check(!field->isReadOnly(), "Enter opens precise native numeric entry");
      auto *line = field->findChild<QLineEdit *>();
      line->setText(QString::number(initial + field->singleStep()));
      QApplication::sendEvent(field, &enter);
      QApplication::processEvents();
      check(window.m_undo->index() == history + 1 &&
                fieldFor("chorus")->value() > initial,
            "precise entry commits through the same Undo command");
      window.m_undo->undo();
      field = fieldFor("chorus");
      sendField(QEvent::MouseButtonPress, {}, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      sendField(QEvent::MouseMove, {0, -40}, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
      field->hide();
      QApplication::processEvents();
      check(window.m_undo->index() == history + 1 && QWidget::mouseGrabber() == nullptr,
            "hiding a numeric field finishes its gesture and releases the pointer");
      window.m_undo->undo();
    }
  }
  canvas->fitGraph();
  const auto menuPoint = QPoint(12, 12);
  const auto insertionPoint = canvas->mapToScene(menuPoint);
  const auto menuHistory = window.m_undo->index();
  bool categoriesFound = false, addedFromMenu = false;
  QTimer::singleShot(0, &window, [&] {
    auto *menu = window.findChild<QMenu *>("CreatorCanvasMenu");
    if (!menu) return;
    unsigned categories = 0;
    QAction *constant = nullptr;
    for (auto *action : menu->actions())
      if (auto *category = action->menu()) {
        ++categories;
        for (auto *entry : category->actions())
          if (entry->data().toString() == "constant") constant = entry;
      }
    categoriesFound = categories >= 8;
    if (!directory.isEmpty()) menu->grab().save(QDir(directory).filePath("creator-add-menu.png"));
    if (constant) { constant->trigger(); addedFromMenu = true; }
    menu->close();
  });
  QContextMenuEvent context(QContextMenuEvent::Mouse, menuPoint,
                             canvas->viewport()->mapToGlobal(menuPoint));
  QApplication::sendEvent(canvas->viewport(), &context);
  QApplication::processEvents();
  const auto inserted = window.project().definition.nodes.back();
  check(categoriesFound && addedFromMenu && inserted.type == "constant" &&
            window.project().positions.value(window.project().activeMode)
                    .value(QString::fromStdString(inserted.id)) == insertionPoint &&
            window.m_undo->index() == menuHistory + 1,
        "right-click categorized menu inserts the chosen node at the click in one Undo");
  if (window.m_undo->index() > menuHistory) window.m_undo->undo();
  auto *toolbar = window.findChild<QToolBar *>("CreatorToolbar");
  auto *compileButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(window.m_compileAction));
  check(compileButton && compileButton->icon().isNull() &&
            compileButton->toolButtonStyle() == Qt::ToolButtonTextOnly &&
            !compileButton->accessibleName().isEmpty(),
        "Compile is a text-only accessible action");
  const auto originalSize = window.size();
  window.resize(900, 600);
  QApplication::processEvents();
  check(compileButton->isVisible() && toolbar->rect().contains(compileButton->geometry()) &&
            canvas->width() >= 320,
        "toolbar and graph remain usable at the minimum window width");
  canvas->fitGraph();
  if (!directory.isEmpty()) window.grab().save(QDir(directory).filePath("creator-compact.png"));
  window.resize(originalSize);
  canvas->fitGraph();
  const auto count = window.project().definition.connections.size();
  emit window.m_canvas->connectPorts("input", "out", "lfo", "rate");
  QApplication::processEvents();
  check(window.project().definition.connections.size() == count,
        "wrong port types rejected by actual editor");
  window.m_canvas->selectNode("chorus");
  QApplication::processEvents();
  check(!window.m_properties->findChildren<QComboBox *>().isEmpty(),
        "native keyboard connection selectors exist");
  window.m_canvas->selectNode("interface");
  QApplication::processEvents();
  check(window.m_properties->findChild<QWidget *>("MiniModuleCard") != nullptr,
        "Interface uses real 100px module card");
  check(MiniModuleLibrary::write(temporary.filePath("module.vltmini"),
                                 stable.definition, error),
        "compiled graph installs as portable file");
  check(MiniModuleLibrary::read(temporary.filePath("module.vltmini"))
                .definition == stable.definition,
        "portable module reads typed graph unchanged");
  const auto track = controller.addTrack(daw::TrackKind::Audio, "Creator");
  const auto slot = controller.addMiniModule(track, stable.definition);
  const auto master = controller.addMiniModule({}, stable.definition);
  auto updated = stable.definition;
  updated.nodes.push_back(makeNode("gain", "gain"));
  updated.connections.back() = {"chorus", "gain", "out", "in"};
  updated.connections.push_back({"gain", "output", "out", "in"});
  auto update = controller.planMiniModuleUpdate(updated);
  check(update->prepare(),
        "compile prepares all inserted DSP off the live graph");
  std::string updateError;
  check(controller.applyMiniModuleUpdate(update, updateError) &&
            controller.miniModules(track).front().id == slot &&
            controller.miniModules({}).front().id == master &&
            controller.miniModules(track).front().miniModule == updated &&
            controller.miniModules({}).front().miniModule == updated,
        "recompile updates track and Master while retaining slot IDs");
  controller.undo();
  check(controller.miniModules(track).front().miniModule == stable.definition &&
            controller.miniModules({}).front().miniModule == stable.definition,
        "one project Undo restores all compiled instances");
  controller.redo();
  check(controller.miniModules(track).front().miniModule == updated,
        "project Redo reapplies compiled graph");
  auto compiled = stable;
  compiled.definition = updated;
  window.setProjectForTest(compiled);
  window.m_path = temporary.filePath("compile.vltcreator");
  window.m_installDirectory = temporary.filePath("MiniModules");
  unsigned published = 0;
  QObject::connect(&window, &CreatorWindow::modulesCompiled, &window,
                   [&] { ++published; });
  const auto compileAndWait = [&] {
    window.compile();
    QElapsedTimer timeout;
    timeout.start();
    while ((window.m_compileFuture.valid() || window.m_pending) &&
           timeout.elapsed() < 60000) {
      QApplication::processEvents();
      QThread::msleep(1);
    }
    return !window.m_compileFuture.valid() && !window.m_pending;
  };
  const auto installed = window.m_installDirectory + "/" +
                         QString::fromStdString(updated.id) + ".vltmini";
  check(compileAndWait() && published == 1 &&
            MiniModuleLibrary::read(installed).definition == updated &&
            MiniModuleLibrary::entries(window.m_installDirectory).size() == 1,
        "Compile command asynchronously installs a discoverable module");
  for (auto &n : compiled.definition.nodes)
    if (n.type == "chorus")
      for (auto &p : n.parameters)
        if (p.id == "depth")
          p.value = .4;
  window.setProjectForTest(compiled);
  check(compileAndWait() && published == 2 &&
            MiniModuleLibrary::read(installed).definition ==
                compiled.definition &&
            controller.miniModules(track).front().miniModule ==
                compiled.definition &&
            controller.miniModules({}).front().miniModule ==
                compiled.definition,
        "recompile replaces the same installed file and both live instances");
  const auto validCompiled = compiled;
  compiled.definition.connections.clear();
  window.setProjectForTest(compiled);
  check(compileAndWait() && published == 2 &&
            MiniModuleLibrary::read(installed).definition ==
                validCompiled.definition &&
            controller.miniModules(track).front().miniModule ==
                validCompiled.definition,
        "invalid compile preserves installed file and current live algorithm");
  if (!directory.isEmpty()) {
    QDir().mkpath(directory);
    window.setProjectForTest(stable);
    window.m_canvas->scene()->clearSelection();
    window.m_diagnostics->clear();
    window.m_status->setText(tr("Ready"));
    window.m_canvas->fitGraph();
    QApplication::processEvents();
    check(window.grab().save(QDir(directory).filePath("creator.png")),
          "native Creator screenshot saved");
    window.m_canvas->selectNode("interface");
    QApplication::processEvents();
    check(window.grab().save(QDir(directory).filePath("creator-interface.png")),
          "native Interface screenshot saved");
  }
  // Exercise the actual C++ wizard, widgets, Undo stack and asynchronous
  // compiler. These checks also run with RU/EN, light/dark and high DPI.
  window.setProjectForTest(
      CreatorProject::create("C++ checks", "Function module"));
  const auto waitJobs = [&] {
    QElapsedTimer timeout;
    timeout.start();
    while ((window.m_codeFuture.valid() || window.m_compileFuture.valid() ||
            window.m_pending) &&
           timeout.elapsed() < 60000) {
      QApplication::processEvents();
      QThread::msleep(1);
    }
    return !window.m_codeFuture.valid() && !window.m_compileFuture.valid() &&
           !window.m_pending;
  };
  QTimer::singleShot(0, &window, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    if (!directory.isEmpty())
      dialog->grab().save(
          QDir(directory).filePath("creator-function-wizard.png"));
    if (auto *buttons = dialog->findChild<QDialogButtonBox *>())
      buttons->button(QDialogButtonBox::Ok)->click();
  });
  window.addNode("cpp_function", {335, 80});
  check(waitJobs() && window.project().definition.nodes.size() == 4 &&
            window.project().definition.nodes.back().function &&
            !window.project()
                 .definition.nodes.back()
                 .function->analyzedHash.empty(),
        "C++ wizard creates an editable function and checked ports");
  const auto cppId =
      QString::fromStdString(window.project().definition.nodes.back().id);
  window.openCode(cppId);
  auto *text = window.m_code->findChild<CreatorCodeText *>();
  const auto source = text->toPlainText();
  const auto sceneItems = canvas->scene()->items();
  window.m_undo->clear();
  text->moveCursor(QTextCursor::End);
  text->insertPlainText("\n// Work in progress\n");
  check(window.m_undo->count() == 1 && canvas->scene()->items() == sceneItems &&
            window.project().definition.nodes.back().function->source.ends_with(
                "// Work in progress\n"),
        "typing records Undo without recreating the node scene");
  const auto undoKey = QKeySequence(QKeySequence::Undo)[0];
  QKeyEvent undoCode(QEvent::KeyPress, undoKey.key(),
                     undoKey.keyboardModifiers());
  QApplication::sendEvent(text, &undoCode);
  check(text->toPlainText() == source,
        "keyboard source Undo restores text through project history");
  const auto redoKey = QKeySequence(QKeySequence::Redo)[0];
  QKeyEvent redoCode(QEvent::KeyPress, redoKey.key(),
                     redoKey.keyboardModifiers());
  QApplication::sendEvent(text, &redoCode);
  check(text->toPlainText().endsWith("// Work in progress\n"),
        "keyboard source Redo restores the draft");
  window.m_undo->clear();
  text->selectAll();
  QKeyEvent removeCode(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QApplication::sendEvent(text, &removeCode);
  check(text->toPlainText().isEmpty() &&
            window.project().definition.nodes.size() == 4,
        "Delete in the code editor edits text without deleting graph nodes");
  window.m_undo->undo();
  auto cursor = text->textCursor();
  cursor.setPosition(12);
  text->setTextCursor(cursor);
  const auto draft = window.project();
  check(draft.save(temporary.filePath("cpp-draft.vltcreator"), error) &&
            CreatorProject::open(temporary.filePath("cpp-draft.vltcreator"),
                                 reopened, error) &&
            reopened == draft && reopened.codeCursors.value("/" + cppId) == 12,
        "unfinished C++ source and cursor survive saving and reopening");
  window.m_undo->clear();
  const auto goodPorts =
      window.project().definition.nodes.back().function->inputs;
  text->moveCursor(QTextCursor::End);
  text->insertPlainText("\nthis is invalid C++;");
  window.codeOperation("analyze");
  check(
      waitJobs() &&
          window.project().definition.nodes.back().function->inputs ==
              goodPorts &&
          window.m_diagnostics->count() > 0,
      "failed Update keeps the last valid ports and shows source diagnostics");
  window.m_undo->undo();
  auto code = source;
  code.replace("VLT_NODE",
               "float half(float value) { return value * .5f; }\n\nVLT_NODE");
  code.replace("return input;",
               "return {half(input.left), half(input.right)};");
  text->setPlainText(code);
  window.codeOperation("analyze");
  check(waitJobs(), "C++ source checks asynchronously");
  const auto f = *window.project().definition.nodes.back().function;
  emit canvas->connectPorts("input", "out", cppId,
                            QString::fromStdString(f.inputs[0].id));
  QApplication::processEvents();
  emit canvas->connectPorts(cppId, "out", "output", "in");
  QApplication::processEvents();
  check(validate(window.project().definition).empty(),
        "C++ audio ports connect through the native canvas");
  const auto connected = window.project();

  text->setPlainText("#include <vlt/creator.hpp>\nvlt::AudioFrame "
                     "process(float value=.1f){return {value,value};}");
  window.codeOperation("analyze");
  waitJobs();
  check(window.project().definition.connections ==
                connected.definition.connections &&
            !validate(window.project().definition).empty(),
        "removed ports keep their original wires in the editable draft");
  const auto numericPort =
      window.project().definition.nodes.back().function->inputs.front().id;
  emit canvas->connectPorts("interface", "control_1", cppId,
                            QString::fromStdString(numericPort));
  QApplication::processEvents();
  check(window.project().definition.connections.size() ==
            connected.definition.connections.size() + 1,
        "a draft with an orphaned wire accepts a compatible replacement "
        "connection");
  emit canvas->disconnectWire(0);
  QApplication::processEvents();
  check(validate(window.project().definition).empty(),
        "removing the old wire completes the repaired graph");
  window.setProjectForTest(connected);
  window.openCode(cppId);

  QTimer::singleShot(0, &window, [&] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      if (auto *name = dialog->findChild<QLineEdit *>())
        name->setText("shape");
      if (auto *buttons = dialog->findChild<QDialogButtonBox *>())
        buttons->button(QDialogButtonBox::Ok)->click();
    }
  });
  window.codeOperation("bind");
  check(waitJobs() && window.project().definition.nodes.size() == 5 &&
            window.project().definition.connections.back().fromPort ==
                "function",
        "Create function declares an input and connects a new callable node");
  window.m_undo->undo();
  check(window.project().definition == connected.definition,
        "Create function is undone as one graph and source edit");
  const auto depth = window.m_undo->index();
  QTimer::singleShot(0, &window, [&] {
    if (auto *dialog =
            qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
      dialog->setTextValue("half");
      dialog->accept();
    }
  });
  window.codeOperation("extract");
  check(waitJobs() && window.project().definition.nodes.size() == 5 &&
            window.m_undo->index() == depth + 1 &&
            window.project().definition.connections.back().fromPort ==
                "function",
        "Extract function rewrites code and adds a callable wire in one Undo");
  const auto extracted = window.project();
  window.m_undo->undo();
  check(window.project().definition == connected.definition,
        "extraction Undo restores the original function and graph");
  window.m_undo->redo();
  check(window.project().definition == extracted.definition,
        "extraction Redo restores the callable dependency");

  window.m_path = temporary.filePath("cpp-compile.vltcreator");
  const auto cppInstalled =
      window.m_installDirectory + "/" +
      QString::fromStdString(window.project().definition.id) + ".vltmini";
  const auto priorPublished = published;
  check(
      compileAndWait() && published == priorPublished + 1 &&
          !window.project().definition.code.wasm.empty() &&
          MiniModuleLibrary::read(cppInstalled).definition ==
              window.project().definition,
      "Compile installs source plus portable WebAssembly from the C++ editor");
  const auto cppSlot =
      controller.addMiniModule(track, window.project().definition);
  const auto cppMaster =
      controller.addMiniModule({}, window.project().definition);
  check(!cppSlot.empty() && !cppMaster.empty(),
        "compiled function module inserts on Track and Master");
  const auto childId =
      QString::fromStdString(window.project().definition.nodes.back().id);
  window.openCode(childId);
  auto childCode = text->toPlainText();
  childCode.replace(".5f", ".25f");
  text->setPlainText(childCode);
  window.codeOperation("analyze");
  waitJobs();
  check(
      compileAndWait() && published == priorPublished + 2 &&
          controller.miniModules(track).back().id == cppSlot &&
          controller.miniModules(track).back().miniModule ==
              window.project().definition &&
          controller.miniModules({}).back().id == cppMaster &&
          controller.miniModules({}).back().miniModule ==
              window.project().definition,
      "C++ recompile replaces both live algorithms without replacing slot IDs");
  const auto installedCpp = MiniModuleLibrary::read(cppInstalled).definition;
  window.compile();
  text->moveCursor(QTextCursor::End);
  text->insertPlainText("\n// Edited while building");
  check(waitJobs() && published == priorPublished + 2 &&
            MiniModuleLibrary::read(cppInstalled).definition == installedCpp &&
            controller.miniModules(track).back().miniModule == installedCpp,
        "a stale background build never replaces the file or live C++ DSP");
  window.m_undo->undo();
  window.compile();
  window.m_cancel.store(true);
  check(waitJobs() && published == priorPublished + 2 &&
            MiniModuleLibrary::read(cppInstalled).definition == installedCpp,
        "cancelled compilation leaves the installed module intact");
  text->moveCursor(QTextCursor::End);
  text->insertPlainText("\ninvalid C++ source;");
  check(compileAndWait() && published == priorPublished + 2 &&
            MiniModuleLibrary::read(cppInstalled).definition == installedCpp &&
            controller.miniModules({}).back().miniModule == installedCpp,
        "invalid C++ draft preserves the installed file and Master DSP");
  window.m_undo->undo();
  window.m_canvas->selectNode(cppId);
  window.openCode(cppId);
  window.m_diagnostics->clear();
  window.m_status->setText(tr("Ready"));
  window.m_canvas->fitGraph();
  QApplication::processEvents();
  if (!directory.isEmpty())
    check(window.grab().save(QDir(directory).filePath("creator-cpp.png")),
          "native C++ editor and callable graph screenshot saved");
  window.m_undo->setClean();
  window.hide();
  return failures == 0;
}
} // namespace ui
