#include "Creator/CodeUtilities.hpp"
#include "CollaborationCommandBridge.hpp"
#include "collaboration/CommandGateway.hpp"
#include "CreatorCanvas.hpp"
#include "CreatorCodeEditor.hpp"
#include "CreatorProject.hpp"
#include "CreatorStyle.hpp"
#include "Theme.hpp"
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
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMouseEvent>
#include <QMenu>
#include <QMimeData>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <QWheelEvent>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace ui {
bool CreatorWindow::runCheck(const QString &directory) {
  if (qEnvironmentVariableIsSet("DAW_CREATOR_AI_CHECK_ONLY")) return runAiCheck(directory);
  using namespace daw::plugins::mini;
  if (qEnvironmentVariableIsSet("DAW_CREATOR_CHECK_CUSTOM_THEME")) {
    auto theme = th();
    theme.background = QColor("#191c18"); theme.surface = QColor("#272d24");
    theme.surfaceElevated = QColor("#343b30"); theme.accent = QColor("#c5bc7d");
    theme.accentHighlight = QColor("#ded8aa"); theme.textPrimary = QColor("#eceee7");
    theme.textSecondary = QColor("#b7c0b0"); theme.gridLine = QColor("#353f32");
    ThemeManager::instance().applyCustomTheme(theme, false);
  }
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
  daw::EngineController controller{};
  QTemporaryDir temporary;
  check(bool(controller.initialize(48000, 256, false)),
        "Creator controller initialized");
  controller.attachSharedMutationSink(bridge);
  check(!controller.hasCloudProjectBinding(),
        "Creator uses the application's attached bridge in a local project");
  CreatorWindow window(&controller);
  window.m_recoveryDirectory = temporary.path();
  // Explicit website capture: render a real saved graph with current widgets,
  // isolated preferences and no live device. Normal UI checks are unchanged.
  const auto websiteProject = qEnvironmentVariable("DAW_CREATOR_WEBSITE_PROJECT");
  if (!websiteProject.isEmpty()) {
    CreatorProject shot;
    QString error;
    if (directory.isEmpty() || !CreatorProject::open(websiteProject, shot, error)) {
      check(false, "website Creator project opened");
      return false;
    }
    check(validate(shot.definition).empty(), "website graph validates");
    window.resize(1600, 1000);
    window.setProjectForTest(shot);
    window.show();
    QApplication::processEvents();
    window.m_canvas->fitGraph();
    window.m_canvas->selectNode("interface");
    QApplication::processEvents();
    check(window.grab().save(QDir(directory).filePath("creator.png")),
          "fresh website Creator screenshot saved");
    window.m_undo->setClean();
    return failures == 0;
  }
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
  check(creatorColors().panel == th().surface && creatorColors().accent == th().accent &&
        creatorColors().text == th().textPrimary, "Creator follows the complete application palette");
  bool compact = true, portTargets = true;
  for (auto *item : window.m_canvas->scene()->items()) {
    if (item->data(10).isValid()) compact &= item->boundingRect().width() == 242;
    if (item->data(12).isValid()) portTargets &= item->boundingRect().width() >= 24 && item->boundingRect().height() >= 24;
    if (auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item); proxy && proxy->widget()->property("creatorParameter").isValid())
      compact &= proxy->widget()->height() == 24;
  }
  check(compact && portTargets, "compact 240 px nodes keep 24 px controls and port hit targets");
  window.m_canvas->fitGraph();
  QApplication::processEvents();
  const auto stable = project;
  const auto findCategory = [&](const QString &key) -> QTreeWidgetItem * {
    for (int i = 0; i < window.m_library->topLevelItemCount(); ++i) {
      auto *item = window.m_library->topLevelItem(i);
      if (item->data(0, Qt::UserRole + 1).toString() == key) return item;
    }
    return nullptr;
  };
  const auto libraryIndex = [&](const QString &type) {
    auto *model = window.m_library->model();
    for (int i = 0; i < model->rowCount(); ++i) {
      const auto category = model->index(i, 0);
      for (int j = 0; j < model->rowCount(category); ++j) {
        const auto item = model->index(j, 0, category);
        if (item.data(Qt::UserRole).toString() == type) return item;
      }
    }
    return QModelIndex{};
  };
  const auto libraryClick = [&](QPoint point) {
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
      QMouseEvent event(type, point, window.m_library->viewport()->mapToGlobal(point),
                        Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                        Qt::NoModifier);
      QApplication::sendEvent(window.m_library->viewport(), &event);
    }
    QApplication::processEvents();
  };
  const auto collapsedBefore = window.m_collapsedCategories;
  auto *routing = findCategory("Routing");
  check(routing && routing->childCount() > 0 && !(routing->flags() & Qt::ItemIsDragEnabled),
        "library categories have children and cannot be dragged as nodes");
  if (routing) {
    routing->setExpanded(true);
    window.m_library->scrollToItem(routing);
    libraryClick(window.m_library->visualItemRect(routing).center());
    check(!routing->isExpanded(), "single click on category label collapses its nodes");
    const auto rectangle = window.m_library->visualItemRect(routing);
    libraryClick({rectangle.left() - window.m_library->indentation() / 2, rectangle.center().y()});
    check(routing->isExpanded(), "native category disclosure arrow expands without double toggling");
    window.m_library->setCurrentItem(routing);
    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QApplication::sendEvent(window.m_library, &left);
    check(!routing->isExpanded(), "library categories support keyboard collapse");
  }
  if (auto *effects = findCategory("Effects")) effects->setExpanded(false);
  window.m_search->setText("chorus");
  auto *effects = findCategory("Effects");
  check(effects && effects->isExpanded() && libraryIndex("chorus").isValid(),
        "search reveals matching nodes inside collapsed categories");
  window.m_search->clear();
  window.refreshNodeLibrary();
  effects = findCategory("Effects");
  check(effects && !effects->isExpanded(), "clearing search and refreshing the library preserve collapsed categories");
  window.m_collapsedCategories = collapsedBefore;
  window.m_fillLibrary();
  QString groupError;
  auto packed = stable;
  const auto groupId = packed.pack({"chorus", "map"}, "Custom chorus", groupError);
  check(!groupId.isEmpty() && validate(packed.definition).empty(), "UI project packs connected nodes with typed boundaries");
  window.edit(tr("Create custom node"), [&](auto &p) { p = packed; });
  window.enterNode(groupId);
  check(window.project().graphPath.size() == 1 && window.m_backAction->isEnabled(), "custom node opens with hierarchy navigation");
  window.m_backAction->trigger();
  check(window.project().graphPath.empty(), "back returns to outer graph");
  window.m_undo->undo();
  check(window.project().definition == stable.definition, "grouping is one complete Undo");
  window.setProjectForTest(packed);
  window.m_canvas->selectNode(groupId);
  window.copySelection(); window.pasteSelection({880, 500});
  check(window.project().definition.subgraphs == packed.definition.subgraphs &&
        window.project().definition.nodes.size() == packed.definition.nodes.size() + 1,
        "custom node copy reuses the embedded definition");
  window.m_undo->undo();
  check(window.project().definition == packed.definition, "custom node paste is one Undo");
  QFile nodeFile(temporary.filePath("Custom.vltnode"));
  auto nodeBundle = nlohmann::json{{"format","vltnode"},{"version",1},{"name","Portable custom node"},
    {"root",packed.definition.subgraphs.front().id},{"definition",toJson(packed.definition)}}.dump();
  nodeFile.open(QIODevice::WriteOnly);nodeFile.write(nodeBundle.data(),qint64(nodeBundle.size()));nodeFile.close();
  window.setProjectForTest(stable);
  window.importNodeFile(nodeFile.fileName());
  check(window.project().definition.subgraphs == packed.definition.subgraphs && window.project().definition.nodes.size() == stable.definition.nodes.size() + 1,
        "portable vltnode import embeds a reusable definition");
  window.m_undo->undo();
  check(window.project().definition == stable.definition,"vltnode import is one Undo");
  const QPointF importedAt(485, 615);
  window.addNode("library:" + nodeFile.fileName(), importedAt);
  const auto importedId = QString::fromStdString(window.project().definition.nodes.back().id);
  check(window.project().positions.value(window.project().layoutKey()).value(importedId) == importedAt &&
        window.project().definition.subgraphs == packed.definition.subgraphs,
        "library file insertion respects the requested canvas position");
  window.m_undo->undo();
  check(window.project().definition == stable.definition, "positioned library file insertion is one Undo");
  auto editable = packed;
  const auto &ports = editable.definition.subgraphs.front().inputs;
  auto numberPort = std::find_if(ports.begin(),ports.end(),[](const auto &p){return p.type=="number";});
  if (numberPort != ports.end()) {
    const auto removedPort = numberPort->id;
    const auto row = int(numberPort-ports.begin());
    for(auto &n:editable.definition.nodes)if(n.id==groupId.toStdString())n.parameters.push_back({removedPort,.5});
    window.setProjectForTest(editable);window.m_canvas->selectNode(groupId);
    QPushButton *editPorts=nullptr;
    for(auto *button:window.m_properties->findChildren<QPushButton *>())if(button->text()==tr("Edit ports…"))editPorts=button;
    check(editPorts!=nullptr,"custom ports are editable in the inspector");
    if(editPorts) {
      QTimer::singleShot(0,&window,[&] {
        if(auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
          const auto tables=dialog->findChildren<QTableWidget *>();
          if(!tables.empty())tables.front()->removeRow(row);
          if(auto *buttons=dialog->findChild<QDialogButtonBox *>())buttons->button(QDialogButtonBox::Ok)->click();
        }
      });
      editPorts->click();
      const auto &edited=window.project().definition;
      bool removedDefault=false;
      for(const auto &n:edited.nodes)if(n.id==groupId.toStdString())removedDefault=std::none_of(n.parameters.begin(),n.parameters.end(),[&](const auto &p){return p.id==removedPort;});
      check(removedDefault&&!validate(edited).empty()&&edited.connections==editable.definition.connections,
            "port removal clears hidden defaults but preserves broken outer wires");
      window.enterNode(groupId);
      bool visibleBoundary=false;
      for(const auto &n:window.project().graph().nodes)visibleBoundary|=n.port==removedPort&&n.label.starts_with("Missing:");
      check(visibleBoundary,"removed custom port keeps its internal boundary visible");
      window.m_undo->undo();
      check(window.project().definition==editable.definition,"port editing is restored by one Undo");
    }
  }
  window.setProjectForTest(stable);
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
  QPoint wirePoint(-10000, -10000);
  for (auto *item : canvas->scene()->items())
    if (item->data(11).isValid() && item->data(11).toUInt() == 0)
      if (auto *wire = dynamic_cast<QGraphicsPathItem *>(item))
        wirePoint = canvas->mapFromScene(wire->mapToScene(wire->path().pointAtPercent(.5)));
  const auto beforeDisconnect = window.m_undo->index();
  mouse(QEvent::MouseButtonPress, wirePoint, Qt::LeftButton, Qt::LeftButton);
  mouse(QEvent::MouseButtonRelease, wirePoint, Qt::LeftButton, Qt::NoButton);
  QApplication::processEvents();
  check(window.project().definition.connections == stable.definition.connections,
        "single wire click selects without disconnecting");
  mouse(QEvent::MouseButtonDblClick, wirePoint, Qt::LeftButton, Qt::LeftButton);
  mouse(QEvent::MouseButtonRelease, wirePoint, Qt::LeftButton, Qt::NoButton);
  QApplication::processEvents();
  auto disconnected = stable.definition.connections;
  disconnected.erase(disconnected.begin());
  check(window.project().definition.connections == disconnected && window.m_undo->index() == beforeDisconnect + 1,
        "double-clicking a wire removes exactly that connection in one Undo");
  if (window.m_undo->index() > beforeDisconnect) window.m_undo->undo();
  check(window.project().definition.connections == stable.definition.connections,
        "Undo restores a double-clicked wire and its port bindings");
  // Use the library model's real drag payload and deliver Qt drop events to the
  // transformed viewport, exercising both ends of the drag-and-drop contract.
  window.m_search->setText("constant");
  const auto draggedIndex = libraryIndex("constant");
  std::unique_ptr<QMimeData> mime(window.m_library->model()->mimeData({draggedIndex}));
  check(draggedIndex.isValid() && (draggedIndex.flags() & Qt::ItemIsDragEnabled) &&
        window.m_library->dragEnabled() && mime && mime->hasFormat(kCreatorNodeMimeType),
        "library leaf produces a draggable Creator node payload");
  const auto dropAt = canvas->viewport()->rect().center() + QPoint(38, 51);
  const auto dropSceneAt = canvas->mapToScene(dropAt);
  const auto dropHistory = window.m_undo->index();
  if (mime) {
    QDragEnterEvent enter(dropAt, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas->viewport(), &enter);
    QDragMoveEvent move(dropAt, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas->viewport(), &move);
    QDropEvent drop(dropAt, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas->viewport(), &drop);
    QApplication::processEvents();
    const auto &node = window.project().definition.nodes.back();
    check(enter.isAccepted() && move.isAccepted() && drop.isAccepted() && node.type == "constant" &&
          window.project().positions.value(window.project().layoutKey()).value(QString::fromStdString(node.id)) == dropSceneAt &&
          window.m_undo->index() == dropHistory + 1,
          "native library drop inserts at the pointer through canvas zoom and pan in one Undo");
  }
  if (window.m_undo->index() > dropHistory) window.m_undo->undo();
  QMimeData invalidDrag;
  invalidDrag.setData(kCreatorNodeMimeType, "unknown_node");
  QDragEnterEvent invalidEnter(dropAt, Qt::CopyAction, &invalidDrag, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(canvas->viewport(), &invalidEnter);
  check(!invalidEnter.isAccepted() && window.project().definition == stable.definition,
        "unknown drag payload is rejected and Undo restores a dropped node");
  const auto keyboardSource = libraryIndex("constant");
  window.m_library->setCurrentIndex(keyboardSource);
  QKeyEvent addWithEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(window.m_library, &addWithEnter);
  QApplication::processEvents();
  check(window.project().definition.nodes.size() == stable.definition.nodes.size() + 1 &&
        window.project().definition.nodes.back().type == "constant",
        "Enter still adds a library node after enabling drag and drop");
  if (window.m_undo->index() > dropHistory) window.m_undo->undo();
  window.m_search->clear();
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
  bool toolbarFits = window.findChildren<QToolBar *>(QString{}, Qt::FindDirectChildrenOnly).size() == 1;
  QRect previous;
  for (auto *action : toolbar->actions()) {
    auto *widget = toolbar->widgetForAction(action);
    if (!widget || !action->isVisible()) continue;
    toolbarFits &= widget->isVisible() && toolbar->rect().contains(widget->geometry());
    if (!previous.isNull()) toolbarFits &= previous.right() < widget->geometry().left();
    previous = widget->geometry();
    if (auto *button = qobject_cast<QToolButton *>(widget); button && action != window.m_compileAction)
      toolbarFits &= button->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                     !button->icon().isNull() && !button->toolTip().isEmpty() &&
                     button->height() >= 28 && button->width() >= 28;
  }
  check(toolbarFits, "one aligned toolbar keeps icon-only actions visible without overlap at 900 px");
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
    while (window.m_build->busy() &&
           timeout.elapsed() < 60000) {
      QApplication::processEvents();
      QThread::msleep(1);
    }
    return !window.m_build->busy();
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
    while ((window.m_codeFuture.valid() || window.m_build->busy()) &&
           timeout.elapsed() < 60000) {
      QApplication::processEvents();
      QThread::msleep(1);
    }
    return !window.m_codeFuture.valid() && !window.m_build->busy();
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
  window.m_build->cancel();
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
