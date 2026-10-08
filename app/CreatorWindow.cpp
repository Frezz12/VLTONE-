#include "CreatorWindow.hpp"
#include "CreatorCommands.hpp"
#include "CreatorAiPanel.hpp"
#include "Creator/CodeUtilities.hpp"
#include "Creator/CreatorCompilerClient.hpp"
#include "CreatorCanvas.hpp"
#include "CreatorCodeEditor.hpp"
#include "CreatorText.hpp"
#include "CreatorStyle.hpp"
#include "EngineController.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "MiniModuleLibrary.hpp"
#include "MiniModuleRack.hpp"
#include "MiniModuleUpdate.hpp"
#include "Theme.hpp"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGroupBox>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>

namespace ui {
using namespace daw::plugins::mini;
using json = nlohmann::json;
namespace {
QString freshId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
class CreatorNodeLibrary final : public QTreeWidget {
public:
  explicit CreatorNodeLibrary(QWidget *parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setIndentation(16);
    setUniformRowHeights(true);
    setExpandsOnDoubleClick(false);
    setDragEnabled(true);
    setDragDropMode(DragOnly);
    setDefaultDropAction(Qt::CopyAction);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTextElideMode(Qt::ElideRight);
  }
protected:
  void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override {
    // The styled native branch otherwise loses its disclosure glyph and paints
    // a separate highlight block beside the selected row.
    const auto colors = creatorColors();
    painter->fillRect(rect, colors.panel);
    if (model()->hasChildren(index)) {
      const QRectF glyph(rect.right() - indentation() + (indentation() - 14) / 2. + 1,
                          rect.center().y() - 7, 14, 14);
      icons::paint(*painter, isExpanded(index) ? icons::Glyph::Chevron : icons::Glyph::ChevronRight,
                   glyph, colors.muted);
    }
  }
  QStringList mimeTypes() const override { return {kCreatorNodeMimeType}; }
  Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }
  QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override {
    if (items.size() != 1) return nullptr;
    const auto type = items.front()->data(0, Qt::UserRole).toString();
    if (type.isEmpty()) return nullptr;
    auto *mime = new QMimeData;
    mime->setData(kCreatorNodeMimeType, type.toUtf8());
    return mime;
  }
};
class ProjectEdit final : public QUndoCommand {
public:
  ProjectEdit(QString name, CreatorProject before, CreatorProject after,
              std::function<void(CreatorProject)> apply, QString mergeKey = {})
      : QUndoCommand(name), before(std::move(before)), after(std::move(after)),
        apply(std::move(apply)), mergeKey(std::move(mergeKey)) {}
  void undo() override { apply(before); }
  void redo() override { apply(after); }
  int id() const override { return mergeKey.isEmpty() ? -1 : 7911; }
  bool mergeWith(const QUndoCommand *command) override {
    const auto *next = dynamic_cast<const ProjectEdit *>(command);
    if (!next || next->mergeKey != mergeKey ||
        (!mergeKey.startsWith("ai/") && next->time - time > std::chrono::milliseconds(800)))
      return false;
    after = next->after;
    time = next->time;
    return true;
  }

private:
  CreatorProject before, after;
  std::function<void(CreatorProject)> apply;
  QString mergeKey;
  std::chrono::steady_clock::time_point time = std::chrono::steady_clock::now();
};
QDoubleSpinBox *number(double minimum, double maximum, double value,
                       QWidget *parent) {
  auto *field = new CreatorNumberField(parent);
  field->setRange(minimum, maximum);
  field->setDecimals(5);
  field->setValue(value);
  field->setDefaultValue(value);
  field->setSingleStep(std::min(1., (maximum - minimum) / 100.));
  field->setKeyboardTracking(false);
  return field;
}
void addStyleChoices(QComboBox *combo) {
  combo->addItem(CreatorWindow::tr("Ring"), "machined");
  combo->addItem(CreatorWindow::tr("Disc"), "rubber");
  combo->addItem(CreatorWindow::tr("Segments"), "glass");
  combo->addItem(CreatorWindow::tr("Fader"), "fader");
}
// A broken draft may contain orphaned wires or unchecked source. Validate the
// new connection on its own so the user can repair it without losing the draft.
} // namespace
struct CreatorWindow::CodeResult {
  QString node, mode, project, operation;
  std::string revision;
  json reply;
};
CreatorWindow::CreatorWindow(daw::EngineController *controller, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_controller(controller),
      m_project(CreatorProject::create(tr("Untitled"), tr("My mini module"))) {
  setObjectName("CreatorWindow");
  m_installDirectory = MiniModuleLibrary::directory();
  m_recoveryDirectory =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
      "/Creator";
  setWindowTitle("Creator[*]");
  resize(1240, 800);
  setMinimumSize(900, 600);
  setAttribute(Qt::WA_DeleteOnClose, false);
  m_undo = new QUndoStack(this);
  m_undo->setUndoLimit(100);
  auto *toolbar = addToolBar(tr("Creator"));
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  toolbar->setObjectName("CreatorToolbar");
  toolbar->setIconSize(QSize(18, 18));
  toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  auto *brand = new QLabel("Creator", toolbar);
  brand->setProperty("creatorRole", "brand");
  toolbar->addWidget(brand);
  auto *newAction = toolbar->addAction(tr("New"));
  creatorIcon(newAction, icons::Glyph::Plus);
  newAction->setShortcut(QKeySequence::New);
  auto *openAction = toolbar->addAction(tr("Open…"));
  creatorIcon(openAction, icons::Glyph::Folder);
  openAction->setShortcut(QKeySequence::Open);
  auto *saveAction = toolbar->addAction(tr("Save"));
  creatorIcon(saveAction, icons::Glyph::Save);
  saveAction->setShortcut(QKeySequence::Save);
  auto *saveAs = new QAction(tr("Save as…"), this);
  saveAs->setShortcut(QKeySequence::SaveAs);
  addAction(saveAs);
  connect(newAction, &QAction::triggered, this, &CreatorWindow::newProject);
  connect(openAction, &QAction::triggered, this, [this] { openProject(); });
  connect(saveAction, &QAction::triggered, this, [this] { saveProject(); });
  connect(saveAs, &QAction::triggered, this, [this] { saveProject(true); });
  toolbar->addSeparator();
  auto *undo = m_undo->createUndoAction(this, tr("Undo"));
  undo->setShortcut(QKeySequence::Undo);
  auto *redo = m_undo->createRedoAction(this, tr("Redo"));
  creatorIcon(undo, icons::Glyph::Undo);
  creatorIcon(redo, icons::Glyph::Redo);
  redo->setShortcut(QKeySequence::Redo);
  toolbar->addAction(undo);
  toolbar->addAction(redo);
  toolbar->addSeparator();
  m_backAction = toolbar->addAction(tr("Back to parent graph"));
  creatorIcon(m_backAction, icons::Glyph::ArrowLeft);
  m_backAction->setEnabled(false);
  auto *createNode = toolbar->addAction(tr("Create node…"), this, &CreatorWindow::groupSelection);
  createNode->setObjectName("CreatorGroupNodes");
  creatorIcon(createNode, icons::Glyph::Layers);
  auto *importNode = toolbar->addAction(tr("Import node…"), this, [this] { importNodeFile(); });
  importNode->setObjectName("CreatorImportNode");
  creatorIcon(importNode, icons::Glyph::Import);
  m_compileAction = new QAction(tr("Compile"), this);
  m_compileAction->setObjectName("CreatorCompile");
  m_compileAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
  m_compileAction->setToolTip(
      tr("Compile and install; update this module in the open project"));
  connect(m_compileAction, &QAction::triggered, this, &CreatorWindow::compile);
  m_cancelAction = new QAction(tr("Cancel build"), this);
  creatorIcon(m_cancelAction, icons::Glyph::Stop);
  m_cancelAction->setEnabled(false);
  m_cancelAction->setVisible(false);
  connect(m_cancelAction, &QAction::changed, this, [this] {
    m_cancelAction->setVisible(m_cancelAction->isEnabled());
  });
  connect(m_cancelAction, &QAction::triggered, this, [this] {
    if (m_ai && m_ai->running()) { m_ai->stop(); return; }
    m_cancel.store(true);
    if (m_build) m_build->cancel();
    m_status->setText(tr("Cancelling…"));
  });
  auto *space = new QWidget(toolbar);
  m_aiAction = toolbar->addAction(tr("Creator assistant"));
  m_aiAction->setObjectName("CreatorAiToggle");
  m_aiAction->setCheckable(true);
  creatorIcon(m_aiAction, icons::Glyph::Assistant);
  connect(m_aiAction, &QAction::triggered, this, [this](bool on) { showAi(on); });
  space->setMinimumWidth(24);
  space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  toolbar->addWidget(space);
  auto *modeLabel = new QLabel(tr("Mode"), toolbar);
  modeLabel->setProperty("creatorRole", "muted");
  toolbar->addWidget(modeLabel);
  m_modes = new QComboBox(toolbar);
  m_modes->setFixedWidth(140);
  m_modes->setFixedHeight(32);
  m_modes->setAccessibleName(tr("Module mode"));
  toolbar->addWidget(m_modes);
  auto *modeMenuButton = new QToolButton(toolbar);
  auto *modeAction = new QAction(tr("Modes…"), modeMenuButton);
  creatorIcon(modeAction, icons::Glyph::Gear);
  modeMenuButton->setDefaultAction(modeAction);
  modeMenuButton->setAccessibleName(tr("Modes…"));
  toolbar->addWidget(modeMenuButton);
  toolbar->addSeparator();
  toolbar->addAction(m_compileAction);
  auto *build = qobject_cast<QToolButton *>(toolbar->widgetForAction(m_compileAction));
  build->setToolButtonStyle(Qt::ToolButtonTextOnly);
  build->setProperty("creatorPrimary", true);
  build->setAccessibleName(tr("Compile"));
  toolbar->addAction(m_cancelAction);
  for (auto *button : toolbar->findChildren<QToolButton *>()) {
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::StrongFocus);
    if (button->defaultAction()) button->setAccessibleName(button->defaultAction()->text());
  }
  connect(
      m_modes, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        m_project.viewports[m_project.layoutKey()] = m_canvas->viewportState();
        m_project.activeMode = m_modes->itemData(index).toString();
        ++m_revision;
        m_project.graphPath.clear();
        m_selected.clear();
        refresh();
        m_canvas->restoreViewport(
            m_project.viewports.value(m_project.layoutKey()));
      });
  connect(modeMenuButton, &QToolButton::clicked, this, [this, modeMenuButton] {
    QMenu menu(this);
    auto *add = menu.addAction(tr("Add mode…"));
    add->setEnabled(m_project.definition.modes.size() < 8);
    auto *rename = menu.addAction(tr("Rename mode…"));
    auto *setDefault = menu.addAction(tr("Set as default"));
    auto *remove = menu.addAction(tr("Remove mode"));
    rename->setEnabled(!m_project.activeMode.isEmpty());
    setDefault->setEnabled(!m_project.activeMode.isEmpty());
    remove->setEnabled(m_project.definition.modes.size() > 1);
    auto *chosen = menu.exec(
        modeMenuButton->mapToGlobal(QPoint(0, modeMenuButton->height())));
    if (chosen == add || chosen == rename) {
      bool ok = false;
      const auto name =
          QInputDialog::getText(
              this, chosen == add ? tr("Add mode") : tr("Rename mode"),
              tr("Name"), QLineEdit::Normal,
              chosen == add ? tr("New mode") : m_modes->currentText(), &ok)
              .trimmed();
      if (!ok || name.isEmpty())
        return;
      edit(tr("Edit modes"), [&](auto &p) {
        if (chosen == add)
          p.addMode(name.left(128));
        else
          for (auto &mode : p.definition.modes)
            if (mode.id == p.activeMode.toStdString())
              mode.name = name.left(128).toStdString();
      });
    } else if (chosen == remove)
      edit(tr("Remove mode"), [](auto &p) { p.removeMode(p.activeMode); });
    else if (chosen == setDefault)
      edit(tr("Set default mode"), [](auto &p) {
        const auto graph = p.graph();
        p.definition.defaultMode = p.activeMode.toStdString();
        p.definition.nodes = graph.nodes;
        p.definition.connections = graph.connections;
        p.definition.code = graph.code;
      });
  });
  auto *split = new QSplitter(this);
  split->setHandleWidth(1);
  setCentralWidget(split);
  auto *libraryPanel = new QWidget(split);
  auto *libraryLayout = new QVBoxLayout(libraryPanel);
  libraryLayout->setContentsMargins(10, 12, 10, 8);
  libraryLayout->setSpacing(8);
  auto *libraryTitle = new QLabel(tr("Nodes"), libraryPanel);
  libraryTitle->setProperty("creatorRole", "heading");
  libraryLayout->addWidget(libraryTitle);
  m_search = new QLineEdit(libraryPanel);
  m_search->setPlaceholderText(tr("Search nodes…"));
  m_search->setAccessibleName(tr("Search node library"));
  m_search->setClearButtonEnabled(true);
  auto *searchIcon = m_search->addAction(QIcon{}, QLineEdit::LeadingPosition);
  creatorIcon(searchIcon, icons::Glyph::Search);
  libraryLayout->addWidget(m_search);
  m_library = new CreatorNodeLibrary(libraryPanel);
  m_library->setObjectName("CreatorNodeLibrary");
  m_library->setAccessibleName(tr("Node library"));
  libraryLayout->addWidget(m_library);
  libraryPanel->setMinimumWidth(190);
  libraryPanel->setMaximumWidth(270);
  const auto collapsed = QSettings().value("creator/collapsedCategories").toStringList();
  m_collapsedCategories = QSet<QString>(collapsed.begin(), collapsed.end());
  const auto fill = [this] {
    QSignalBlocker blocker(m_library);
    const auto selected = m_library->currentItem()
        ? m_library->currentItem()->data(0, Qt::UserRole).toString() : QString{};
    m_library->clear();
    QMap<QString, QTreeWidgetItem *> categories;
    const auto addItem = [&](const QString &name, const QString &type,
                             const QString &category, const QString &label,
                             icons::Glyph glyph) {
      auto *header = categories.value(category);
      if (!header) {
        header = new QTreeWidgetItem(m_library, {label});
        header->setData(0, Qt::UserRole + 1, category);
        header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        header->setIcon(0, icons::icon(glyph, creatorColors().muted, 16));
        header->setForeground(0, creatorColors().muted);
        header->setToolTip(0, label);
        auto font = header->font(0);
        font.setPixelSize(11);
        font.setWeight(QFont::DemiBold);
        header->setFont(0, font);
        header->setSizeHint(0, QSize(0, 28));
        categories.insert(category, header);
      }
      auto *item = new QTreeWidgetItem(header, {name});
      item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
      item->setData(0, Qt::UserRole, type);
      item->setSizeHint(0, QSize(0, 28));
      item->setToolTip(0, name + "\n" + tr("Drag onto the canvas, double-click or press Enter to add"));
      if (type == selected) m_library->setCurrentItem(item);
    };
    std::vector<const NodeDescription *> types;
    for (const auto &n : nodeRegistry())
      if (n.id != "wire" && n.id != "subgraph" && !n.id.starts_with("subgraph_")) types.push_back(&n);
    std::stable_sort(types.begin(), types.end(), [](auto *a, auto *b) {
      return creatorCategoryOrder(a->category) < creatorCategoryOrder(b->category);
    });
    for (const auto *n : types) {
      const auto name = creatorText(n->name), cat = creatorText(n->category);
      if (!m_search->text().isEmpty() &&
          !(name + " " + cat + " " + QString::fromStdString(n->id))
               .contains(m_search->text(), Qt::CaseInsensitive))
        continue;
      addItem(name, QString::fromStdString(n->id), QString::fromStdString(n->category),
              cat, creatorCategoryIcon(n->category));
    }
    for (auto it = m_personalNodes.begin(); it != m_personalNodes.end(); ++it) {
      if ((it.value() + " " + tr("My nodes")).contains(m_search->text(), Qt::CaseInsensitive))
        addItem(it.value(), it.key(), "personal", tr("My nodes"), icons::Glyph::Layers);
    }
    for (auto it = categories.begin(); it != categories.end(); ++it)
      it.value()->setExpanded(!m_search->text().isEmpty() || !m_collapsedCategories.contains(it.key()));
  };
  m_fillLibrary = fill;
  connect(m_library, &QTreeWidget::itemClicked, this, [](auto *item) {
    if (!item->parent()) item->setExpanded(!item->isExpanded());
  });
  const auto rememberCategory = [this](QTreeWidgetItem *item) {
    if (!m_search->text().isEmpty()) return;
    const auto key = item->data(0, Qt::UserRole + 1).toString();
    if (item->isExpanded()) m_collapsedCategories.remove(key);
    else m_collapsedCategories.insert(key);
    QSettings().setValue("creator/collapsedCategories", m_collapsedCategories.values());
  };
  connect(m_library, &QTreeWidget::itemExpanded, this, rememberCategory);
  connect(m_library, &QTreeWidget::itemCollapsed, this, rememberCategory);
  connect(m_search, &QLineEdit::textChanged, this, fill);
  connect(&ThemeManager::instance(), &ThemeManager::changed, this, fill);
  fill();
  auto *center = new QSplitter(Qt::Vertical, split);
  m_split = split;
  center->setMinimumWidth(320);
  center->setHandleWidth(4);
  m_canvas = new CreatorCanvas(center);
  refreshNodeLibrary();
  connect(m_backAction, &QAction::triggered, this, [this] {
    if (m_project.graphPath.empty()) return;
    m_project.viewports[m_project.layoutKey()] = m_canvas->viewportState();
    m_project.graphPath.removeLast(); m_project.codeNode.clear(); m_selected.clear(); refresh();
    ++m_revision;
    m_canvas->restoreViewport(m_project.viewports.value(m_project.layoutKey()));
  });
  m_code = new CreatorCodeEditor(center);
  m_code->hide();
  m_diagnostics = new QListWidget(center);
  m_diagnostics->setObjectName("CreatorDiagnostics");
  m_diagnostics->setAccessibleName(tr("Compile diagnostics"));
  m_diagnostics->setMaximumHeight(170);
  m_diagnostics->hide();
  const auto showDiagnostics = [this] { m_diagnostics->setVisible(m_diagnostics->count() > 0); };
  connect(m_diagnostics->model(), &QAbstractItemModel::rowsInserted, this, showDiagnostics);
  connect(m_diagnostics->model(), &QAbstractItemModel::rowsRemoved, this, showDiagnostics);
  connect(m_diagnostics->model(), &QAbstractItemModel::modelReset, this, showDiagnostics);
  center->setSizes({390, 260, 95});
  auto* right = new QWidget(split);
  right->setObjectName("CreatorRightPanel"); right->setMinimumWidth(250);
  auto* rightLayout = new QVBoxLayout(right); rightLayout->setContentsMargins(0,0,0,0); rightLayout->setSpacing(0);
  auto* rightHeader = new QHBoxLayout; rightHeader->setContentsMargins(8,4,8,4); rightHeader->setSpacing(4);
  auto* parameters = new QToolButton(right); parameters->setText(tr("Parameters")); parameters->setAutoRaise(true);
  auto* aiTab = new QToolButton(right); aiTab->setText("AI"); aiTab->setAutoRaise(true);
  auto* closeChat = new QToolButton(right); closeChat->setObjectName("CreatorAiClose"); closeChat->setAutoRaise(true);
  closeChat->setIcon(icons::icon(icons::Glyph::Close, creatorColors().muted,16)); closeChat->setAccessibleName(tr("Close chat")); closeChat->setToolTip(tr("Close chat"));
  rightHeader->addWidget(parameters); rightHeader->addWidget(aiTab); rightHeader->addStretch(); rightHeader->addWidget(closeChat);
  rightLayout->addLayout(rightHeader);
  m_rightStack = new QStackedWidget(right); rightLayout->addWidget(m_rightStack,1);
  connect(parameters,&QToolButton::clicked,this,[this]{showAi(false);});
  connect(aiTab,&QToolButton::clicked,this,[this]{showAi(true);});
  connect(closeChat,&QToolButton::clicked,this,[this]{showAi(false);});
  connect(m_rightStack,&QStackedWidget::currentChanged,closeChat,[closeChat](int page){closeChat->setVisible(page==1);});
  closeChat->hide();
  m_properties = new QScrollArea(m_rightStack);
  m_properties->setWidgetResizable(true);
  m_properties->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_properties->setMinimumWidth(250);
  m_rightStack->addWidget(m_properties);
  m_properties->setFrameShape(QFrame::NoFrame);
  split->setSizes({215, 745, 280});
  split->setStretchFactor(1, 1);
  split->setChildrenCollapsible(false);
  m_status = new QLabel(tr("Ready"), this);
  m_status->setMinimumWidth(0);
  m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  statusBar()->addWidget(m_status, 1);
  auto *fit = new QPushButton(tr("Fit"), this);
  fit->setIcon(icons::icon(icons::Glyph::ZoomFit, creatorColors().text));
  fit->setToolTip(tr("Fit graph") + " (F)");
  auto *actual = new QPushButton(tr("100%"), this);
  actual->setObjectName("CreatorZoom");
  actual->setMinimumWidth(60);
  actual->setAccessibleName(tr("Actual size"));
  actual->setToolTip(tr("Actual size") + " (Ctrl+0)");
  statusBar()->addPermanentWidget(fit);
  statusBar()->addPermanentWidget(actual);
  connect(fit, &QPushButton::clicked, m_canvas, &CreatorCanvas::fitGraph);
  connect(actual, &QPushButton::clicked, m_canvas, &CreatorCanvas::actualSize);
  connect(m_canvas, &CreatorCanvas::zoomChanged, actual, [actual](double zoom) {
    actual->setText(QString::number(qRound(zoom * 100)) + "%");
  });
  connect(m_library, &QTreeWidget::itemActivated, this, [this](auto *item) {
    const auto type = item->data(0, Qt::UserRole).toString();
    if (!type.isEmpty()) QTimer::singleShot(0, this, [this, type] {
      addNode(type, m_canvas->viewportState().center);
    });
  });
  connect(m_canvas, &CreatorCanvas::status, m_status, &QLabel::setText);
  connect(m_canvas, &CreatorCanvas::codeRequested, this,
          &CreatorWindow::openCode);
  connect(m_code, &CreatorCodeEditor::edited, this, &CreatorWindow::codeEdited);
  connect(m_code, &CreatorCodeEditor::undoRequested, m_undo, &QUndoStack::undo);
  connect(m_code, &CreatorCodeEditor::redoRequested, m_undo, &QUndoStack::redo);
  connect(m_code, &CreatorCodeEditor::cursorMoved, this, [this](int position) {
    if (!m_codeId.isEmpty())
      m_project.codeCursors[m_project.layoutKey() + "/" + m_codeId] = position;
  });
  connect(m_code, &CreatorCodeEditor::updateRequested, this,
          [this] { codeOperation("analyze"); });
  connect(m_code, &CreatorCodeEditor::createRequested, this,
          [this] { codeOperation("bind"); });
  connect(m_code, &CreatorCodeEditor::extractRequested, this,
          [this] { codeOperation("extract"); });
  connect(m_code, &CreatorCodeEditor::closeRequested, this, [this] {
    m_code->hide();
    m_project.codeNode.clear();
    m_codeId.clear();
    m_canvas->setFocus();
  });
  connect(m_canvas, &CreatorCanvas::nodeSelected, this, [this](QString id) {
    m_selected = id;
    properties(id);
    if (m_ai) m_ai->contextChanged();
  });
  connect(m_canvas, &CreatorCanvas::groupRequested, this, &CreatorWindow::groupSelection, Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::enterRequested, this, &CreatorWindow::enterNode, Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::importNodeRequested, this, [this] { importNodeFile(); }, Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::exportNodeRequested, this, &CreatorWindow::exportNodeFile, Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::unpackRequested, this, [this] {
    QString error;
    edit(tr("Expand custom node"), [&](auto &p) { p.unpack(m_selected, error); });
    if (!error.isEmpty()) diagnostic(error);
  }, Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::independentRequested, this, [this] {
    QString error;
    edit(tr("Make independent"), [&](auto &p) { p.makeIndependent(m_selected, error); });
    if (!error.isEmpty()) diagnostic(error);
  }, Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::nodesMoved, this, [this] {
    const auto positions = m_canvas->nodePositions();
    edit(tr("Move nodes"),
         [&](auto &p) { p.positions[p.layoutKey()] = positions; });
  });
  connect(
      m_canvas, &CreatorCanvas::parameterEdited, this,
      [this](QString node, QString parameter, double value) {
        edit(tr("Change node parameter"), [&](auto &p) {
          auto graph = p.graph();
          for (auto &n : graph.nodes)
            if (n.id == node.toStdString()) {
              auto found = std::find_if(
                  n.parameters.begin(), n.parameters.end(),
                  [&](auto &v) { return v.id == parameter.toStdString(); });
              if (found != n.parameters.end())
                found->value = value;
              else
                n.parameters.push_back({parameter.toStdString(), value});
            }
          p.setGraph(graph);
        });
      },
      Qt::QueuedConnection);
  connect(
      m_canvas, &CreatorCanvas::connectPorts, this,
      [this](QString from, QString fromPort, QString to, QString toPort) {
        auto graph = m_project.graph();
        std::erase_if(graph.connections, [&](const auto &e) {
          return e.to == to.toStdString() && e.toPort == toPort.toStdString();
        });
        graph.connections.push_back({from.toStdString(), to.toStdString(),
                                     fromPort.toStdString(),
                                     toPort.toStdString()});
        // Incomplete graphs may be edited; connection edits still reject cycles
        // and type errors.
        const auto error = connectionError(graph, graph.connections.back());
        if (!error.empty()) {
          diagnostic(QString::fromStdString(error), to);
          m_canvas->highlightConnectionPath(from, to, error.find("previous-sample") == std::string::npos);
          return;
        }
        edit(tr("Connect ports"), [&](auto &p) { p.setGraph(graph); });
      },
      Qt::QueuedConnection);
  connect(
      m_canvas, &CreatorCanvas::disconnectWire, this,
      [this](unsigned index) {
        edit(tr("Disconnect ports"), [&](auto &p) {
          auto graph = p.graph();
          if (index < graph.connections.size())
            graph.connections.erase(graph.connections.begin() + index);
          p.setGraph(graph);
        });
      },
      Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::addRequested, this,
          &CreatorWindow::addSearch);
  connect(m_canvas, &CreatorCanvas::addNodeRequested, this,
          &CreatorWindow::addNode, Qt::QueuedConnection);
  connect(m_canvas, &CreatorCanvas::deleteRequested, this,
          &CreatorWindow::removeSelection);
  connect(m_canvas, &CreatorCanvas::copyRequested, this,
          &CreatorWindow::copySelection);
  connect(m_canvas, &CreatorCanvas::pasteRequested, this,
          &CreatorWindow::pasteSelection);
  connect(m_canvas, &CreatorCanvas::duplicateRequested, this, [this] {
    copySelection();
    pasteSelection(m_canvas->viewportState().center + QPointF(32, 32));
  });
  connect(m_diagnostics, &QListWidget::itemActivated, this, [this](auto *item) {
    const auto node = item->data(Qt::UserRole).toString();
    if (!node.isEmpty())
      m_canvas->selectNode(node);
    if (item->data(Qt::UserRole + 1).toUInt()) {
      openCode(node);
      m_code->reveal(item->data(Qt::UserRole + 1).toUInt(),
                     item->data(Qt::UserRole + 2).toUInt());
    }
  });
  connect(m_undo, &QUndoStack::cleanChanged, this,
          [this](bool clean) { setWindowModified(!clean); });
  m_build = new CreatorBuildService(m_controller, this);
  m_ai = new CreatorAiPanel(*this, m_rightStack);
  m_rightStack->addWidget(m_ai);
  connect(m_ai, &CreatorAiPanel::settingsRequested, this, &CreatorWindow::aiSettingsRequested);
  connect(m_ai, &CreatorAiPanel::stateChanged, this, [this](bool working, bool completed) {
    auto pixmap = icons::icon(icons::Glyph::Assistant, creatorColors().text, 18).pixmap(QSize(18,18), devicePixelRatioF());
    const bool unread = !m_ai->isVisible() && (completed || m_aiAction->property("aiUnread").toBool());
    m_aiAction->setProperty("aiUnread", unread && !working);
    if (working || unread) {
      QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(creatorColors().accent); p.drawEllipse(QRectF(12,1,5,5));
    }
    m_aiAction->setIcon(QIcon(pixmap));
    m_aiAction->setToolTip(working ? tr("Assistant is working") : unread ? tr("Assistant finished") : tr("Creator assistant"));
  });
  connect(split,&QSplitter::splitterMoved,this,[this] {
    if (m_rightStack->currentWidget()==m_ai) QSettings().setValue("creator/aiWidth", m_split->sizes().value(2,340));
  });
  m_compilePoll = new QTimer(this);
  m_compilePoll->setInterval(25);
  connect(m_compilePoll, &QTimer::timeout, this, &CreatorWindow::pollCode);
  auto *recovery = new QTimer(this);
  recovery->setInterval(5000);
  connect(recovery, &QTimer::timeout, this, [this] {
    if (!m_started || m_undo->isClean())
      return;
    m_project.viewports[m_project.layoutKey()] = m_canvas->viewportState();
    QDir().mkpath(QFileInfo(recoveryPath()).absolutePath());
    QString error;
    m_project.save(recoveryPath(), error);
  });
  recovery->start();
  styleCreator(this);
  connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this, fit] {
    styleCreator(this);
    fit->setIcon(icons::icon(icons::Glyph::ZoomFit, creatorColors().text));
    properties(m_selected);
  });
  refresh();
  showAi(QSettings().value("creator/aiVisible",false).toBool());
}
CreatorWindow::~CreatorWindow() {
  delete m_ai; m_ai = nullptr;
  m_cancel.store(true);
  if (m_codeFuture.valid())
    m_codeFuture.wait();
  delete m_build; m_build = nullptr;
}
void CreatorWindow::showAi(bool show) {
  if (!m_ai || !m_rightStack) return;
  const auto viewport = m_canvas->viewportState();
  m_rightStack->setCurrentWidget(show ? static_cast<QWidget*>(m_ai) : static_cast<QWidget*>(m_properties));
  m_aiAction->setChecked(show); QSettings().setValue("creator/aiVisible", show);
  auto sizes = m_split->sizes();
  if (sizes.size()==3) {
    const int total = sizes[0]+sizes[1]+sizes[2];
    const int desired = show ? QSettings().value("creator/aiWidth",340).toInt() : 280;
    sizes[2] = std::clamp(desired,250,std::max(250,total-sizes[0]-320));
    sizes[1] = std::max(320,total-sizes[0]-sizes[2]); m_split->setSizes(sizes);
  }
  m_canvas->restoreViewport(viewport);
  if (show) { m_aiAction->setProperty("aiUnread",false); m_ai->reloadSettings(); m_ai->contextChanged(); }
}
void CreatorWindow::showEvent(QShowEvent *e) {
  QMainWindow::showEvent(e);
  if (!m_started) {
    m_started = true;
    QTimer::singleShot(0, this, &CreatorWindow::startup);
  }
}
QString CreatorWindow::recoveryPath() const {
  return m_recoveryDirectory + "/recovery.vltcreator";
}
void CreatorWindow::startup() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Welcome to Creator"));
  dialog.setMinimumWidth(430);
  auto *layout = new QVBoxLayout(&dialog);
  auto *intro = new QLabel(
      tr("Create a mini module or continue an existing project."), &dialog);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto *create = new QPushButton(tr("New project"), &dialog);
  auto *open = new QPushButton(tr("Open project…"), &dialog);
  layout->addWidget(create);
  layout->addWidget(open);
  auto *recent = new QListWidget(&dialog);
  recent->setAccessibleName(tr("Recent Creator projects"));
  for (const auto &path : QSettings().value("creator/recent").toStringList())
    if (QFileInfo::exists(path)) {
      auto *item =
          new QListWidgetItem(QFileInfo(path).completeBaseName(), recent);
      item->setToolTip(path);
      item->setData(Qt::UserRole, path);
    }
  if (recent->count()) {
    layout->addWidget(new QLabel(tr("Recent projects"), &dialog));
    layout->addWidget(recent);
  } else
    recent->hide();
  auto *recover = new QPushButton(tr("Recover unsaved project"), &dialog);
  recover->setVisible(QFileInfo::exists(recoveryPath()));
  layout->addWidget(recover);
  connect(create, &QPushButton::clicked, &dialog, [&] { dialog.done(1); });
  connect(open, &QPushButton::clicked, &dialog, [&] { dialog.done(2); });
  connect(recover, &QPushButton::clicked, &dialog, [&] { dialog.done(3); });
  connect(recent, &QListWidget::itemActivated, &dialog,
          [&] { dialog.done(4); });
  const int action = dialog.exec();
  if (action == 1)
    newProject();
  else if (action == 2)
    openProject();
  else if (action == 3) {
    CreatorProject recovered;
    QString error;
    if (CreatorProject::open(recoveryPath(), recovered, error)) {
      m_path.clear();
      apply(std::move(recovered));
      m_undo->clear();
      m_undo->resetClean();
    } else
      diagnostic(error);
  } else if (action == 4 && recent->currentItem())
    openProject(recent->currentItem()->data(Qt::UserRole).toString());
  m_canvas->restoreViewport(m_project.viewports.value(m_project.layoutKey()));
}
bool CreatorWindow::askToSave() {
  if (m_undo->isClean()) {
    // Cursor and active-editor state are local navigation, not Undo edits.
    // Persist them even when no text or graph change needs a save prompt.
    if (!m_path.isEmpty()) {
      QString error;
      if (!m_project.save(m_path, error))
        diagnostic(error);
    }
    return true;
  }
  const auto choice = QMessageBox::question(
      this, tr("Save Creator project?"),
      tr("Save your changes before continuing?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  return choice == QMessageBox::Discard ||
         (choice == QMessageBox::Save && saveProject());
}
void CreatorWindow::newProject() {
  m_ai->stop();
  if (!askToSave())
    return;
  QDialog dialog(this);
  dialog.setWindowTitle(tr("New Creator project"));
  auto *form = new QFormLayout(&dialog);
  auto *project = new QLineEdit(tr("My project"), &dialog),
       *module = new QLineEdit(tr("My mini module"), &dialog);
  project->setMaxLength(128);
  module->setMaxLength(128);
  form->addRow(tr("Project name"), project);
  form->addRow(tr("Module name"), module);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  const auto valid = [=] {
    buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(!project->text().trimmed().isEmpty() &&
                     !module->text().trimmed().isEmpty());
  };
  connect(project, &QLineEdit::textChanged, &dialog, valid);
  connect(module, &QLineEdit::textChanged, &dialog, valid);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  m_path.clear();
  m_selected.clear();
  m_undo->clear();
  apply(CreatorProject::create(project->text().trimmed(),
                               module->text().trimmed()));
  m_undo->resetClean();
  m_canvas->fitGraph();
}
bool CreatorWindow::openProject(const QString &requested) {
  m_ai->stop();
  if (!askToSave())
    return false;
  const auto path =
      requested.isEmpty()
          ? QFileDialog::getOpenFileName(this, tr("Open Creator project"), {},
                                         tr("Creator projects (*.vltcreator)"))
          : requested;
  if (path.isEmpty())
    return false;
  CreatorProject project;
  QString error;
  if (!CreatorProject::open(path, project, error)) {
    QMessageBox::warning(this, tr("Creator"), error);
    return false;
  }
  m_path = path;
  m_selected.clear();
  m_undo->clear();
  apply(std::move(project));
  m_undo->setClean();
  rememberRecent();
  m_canvas->restoreViewport(m_project.viewports.value(m_project.layoutKey()));
  return true;
}
bool CreatorWindow::saveProject(bool saveAs) {
  auto path = m_path;
  if (path.isEmpty() || saveAs)
    path = QFileDialog::getSaveFileName(
        this, tr("Save Creator project"),
        m_path.isEmpty() ? m_project.name + ".vltcreator" : m_path,
        tr("Creator projects (*.vltcreator)"));
  if (path.isEmpty())
    return false;
  if (!path.endsWith(".vltcreator", Qt::CaseInsensitive))
    path += ".vltcreator";
  m_project.viewports[m_project.layoutKey()] = m_canvas->viewportState();
  QString error;
  if (!m_project.save(path, error)) {
    QMessageBox::warning(this, tr("Creator"), error);
    return false;
  }
  m_path = path;
  m_undo->setClean();
  rememberRecent();
  QFile::remove(recoveryPath());
  m_status->setText(tr("Project saved"));
  return true;
}
void CreatorWindow::rememberRecent() {
  auto recent = QSettings().value("creator/recent").toStringList();
  recent.removeAll(m_path);
  recent.prepend(m_path);
  while (recent.size() > 12)
    recent.removeLast();
  QSettings().setValue("creator/recent", recent);
}
void CreatorWindow::closeEvent(QCloseEvent *event) {
  if (m_ai->running()) { m_closeAfterAi = true; m_ai->stop(); }
  if (m_build->busy() || m_codeFuture.valid()) {
    if (m_closeAfterAi) {
      event->ignore(); QTimer::singleShot(50, this, [this] { if (isVisible()) close(); }); return;
    }
    m_status->setText(
        tr("Compilation is in progress. Close Creator after it finishes."));
    event->ignore();
    return;
  }
  m_closeAfterAi = false;
  if (!askToSave()) {
    event->ignore();
    return;
  }
  QFile::remove(recoveryPath());
  event->accept();
}
void CreatorWindow::setProjectForTest(CreatorProject p) {
  m_started = true;
  m_undo->clear();
  apply(std::move(p));
}
void CreatorWindow::edit(const QString &name,
                         const std::function<void(CreatorProject &)> &change, const QString& mergeKey) {
  auto after = m_project;
  change(after);
  if (after == m_project)
    return;
  m_undo->push(new ProjectEdit(name, m_project, std::move(after),
                               [this](auto p) { apply(std::move(p)); }, mergeKey));
}
void CreatorWindow::apply(CreatorProject p) {
  if (m_ai) m_ai->documentChanging();
  ++m_revision;
  auto previous = m_project.graph(), next = p.graph();
  const auto stripText = [](auto &graph) {
    const auto strip = [](auto &nodes) { for (auto &node : nodes) {
      node.parameters.clear();
      if (node.function) {
        node.function->source.clear();
        node.function->entry.clear();
      }
    } };
    strip(graph.nodes); for (auto &g : graph.subgraphs) strip(g.nodes);
  };
  stripText(previous);
  stripText(next);
  const bool textOnly = m_project.definition.id == p.definition.id &&
                        m_project.activeMode == p.activeMode &&
                        m_project.graphPath == p.graphPath &&
                        m_project.positions == p.positions && previous == next;
  m_project = std::move(p);
  if (m_ai) m_ai->projectChanged();
  if (!textOnly)
    refresh();
  else {
    m_canvas->updateValues(m_project.graph());
    m_canvas->updateCodeStatus(m_project.graph());
    properties(m_selected);
    if (!m_codeId.isEmpty())
      openCode(m_codeId);
  }
}
void CreatorWindow::refresh() {
  m_refreshing = true;
  if (m_backAction) {
    m_backAction->setEnabled(!m_project.graphPath.empty());
    QStringList names{tr("Module")};
    for (const auto &id : m_project.graphPath) for (const auto &g : m_project.definition.subgraphs)
      if (g.id == id.toStdString()) names.push_back(QString::fromStdString(g.name));
    m_backAction->setToolTip(tr("Back to parent graph") + "\n" + names.join(" / "));
  }
  refreshNodeLibrary();
  setWindowTitle(QString::fromStdString(m_project.definition.name) +
                 " — Creator[*]");
  QSignalBlocker blocker(m_modes);
  m_modes->clear();
  if (m_project.definition.modes.empty())
    m_modes->addItem(tr("Main"), QString{});
  else
    for (const auto &mode : m_project.definition.modes)
      m_modes->addItem(
          QString::fromStdString(mode.name) +
              (mode.id == m_project.definition.defaultMode ? " *" : ""),
          QString::fromStdString(mode.id));
  m_modes->setCurrentIndex(m_modes->findData(m_project.activeMode));
  m_canvas->setGraph(m_project.graph(),
                     m_project.positions.value(m_project.layoutKey()));
  properties(m_selected);
  if (!m_project.codeNode.isEmpty())
    openCode(m_project.codeNode);
  else {
    m_codeId.clear();
    m_code->hide();
  }
  m_refreshing = false;
  if (m_ai) m_ai->contextChanged();
}
void CreatorWindow::diagnostic(const QString &message, const QString &node) {
  auto *item = new QListWidgetItem(message, m_diagnostics);
  item->setData(Qt::UserRole, node);
  item->setToolTip(message);
  m_diagnostics->scrollToBottom();
  m_status->setText(message);
}
void CreatorWindow::addNode(const QString &type, QPointF position) {
  if (type.startsWith("library:")) { importNodeFile(type.mid(8), position); return; }
  const auto graph = m_project.graph();
  if (graph.nodes.size() >= kMaxNodes) {
    diagnostic(tr("A graph supports at most 512 expanded nodes."));
    return;
  }
  if ((type == "input" || type == "output" || type == "interface") &&
      std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const auto &n) {
        return n.type == type.toStdString();
      })) {
    for (const auto &n : graph.nodes)
      if (n.type == type.toStdString())
        m_canvas->selectNode(QString::fromStdString(n.id));
    return;
  }
  const auto id = freshId();
  std::optional<FunctionDefinition> function;
  if (type == "cpp_function") {
    function = createFunctionDialog(this);
    if (!function)
      return;
  }
  edit(tr("Add node"), [&](auto &p) {
    auto g = p.graph();
    auto node = makeNode(type.toStdString(), id.toStdString());
    if (type.startsWith("custom:")) { node = makeNode("subgraph", id.toStdString()); node.subgraph = type.mid(7).toStdString(); }
    node.function = function;
    if (type == "map" || type == "reduce") {
      SubgraphDefinition body; body.id = freshId().toStdString(); body.name = type == "map" ? "Map element" : "Reduce elements";
      body.inputs = {{"item", "Item", "number"}, {"index", "Index", "integer"}};
      if (type == "reduce") body.inputs.push_back({"accumulator", "Accumulator", "number"});
      body.outputs = {{"result", "Result", "number"}};
      for (const auto &port : body.inputs) {
        auto boundary = makeNode("subgraph_input", port.id); boundary.port = port.id; boundary.valueType = port.type;
        body.nodes.push_back(boundary);
        p.positions["node/" + QString::fromStdString(body.id)][QString::fromStdString(boundary.id)] = QPointF(20, body.nodes.size() * 110.);
      }
      auto output = makeNode("subgraph_output", "result"); output.port = "result";
      body.nodes.push_back(output); body.connections = {{"item", "result", "out", "in"}};
      p.positions["node/" + QString::fromStdString(body.id)]["result"] = QPointF(600, 100);
      node.subgraph = body.id; g.subgraphs.push_back(body);
    }
    if (type.startsWith("custom:") || (nodeDescription(node.type) && nodeDescription(node.type)->operation >= Operation::Wire)) g.version = 5;
    g.nodes.push_back(std::move(node));
    if (function)
      g.version = std::max(4u, g.version);
    p.setGraph(g);
    p.positions[p.layoutKey()][id] = position;
  });
  m_canvas->selectNode(id);
  if (function) {
    openCode(id);
    codeOperation("analyze");
  }
}
void CreatorWindow::addSearch(QPointF position) {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Add node"));
  dialog.resize(350, 450);
  auto *layout = new QVBoxLayout(&dialog);
  auto *search = new QLineEdit(&dialog);
  search->setPlaceholderText(tr("Search nodes…"));
  auto *list = new QListWidget(&dialog);
  layout->addWidget(search);
  layout->addWidget(list);
  const auto fill = [=] {
    list->clear();
    for (const auto &n : nodeRegistry())
      if (n.id != "wire" && n.id != "subgraph" && !n.id.starts_with("subgraph_") && (creatorText(n.name) + " " + creatorText(n.category) + " " +
           QString::fromStdString(n.id))
              .contains(search->text(), Qt::CaseInsensitive)) {
        auto *item = new QListWidgetItem(
            creatorText(n.name) + "  ·  " + creatorText(n.category), list);
        item->setData(Qt::UserRole, QString::fromStdString(n.id));
      }
    for (auto it = m_personalNodes.begin(); it != m_personalNodes.end(); ++it)
      if (it.value().contains(search->text(), Qt::CaseInsensitive)) { auto *item = new QListWidgetItem(it.value() + " · " + tr("My nodes"), list); item->setData(Qt::UserRole, it.key()); }
    list->setCurrentRow(0);
  };
  connect(search, &QLineEdit::textChanged, &dialog, fill);
  connect(search, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
  connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
  fill();
  search->setFocus();
  if (dialog.exec() == QDialog::Accepted && list->currentItem())
    addNode(list->currentItem()->data(Qt::UserRole).toString(), position);
}
void CreatorWindow::removeSelection() {
  const auto ids = m_canvas->selectedNodes();
  const auto wires = m_canvas->selectedConnections();
  edit(tr("Delete selection"), [&](auto &p) {
    auto g = p.graph();
    std::vector<std::string> removed;
    std::erase_if(g.nodes, [&](const auto &n) {
      if (n.type == "input" || n.type == "output" || n.type == "interface" ||
          !ids.contains(QString::fromStdString(n.id)))
        return false;
      removed.push_back(n.id);
      p.positions[p.layoutKey()].remove(QString::fromStdString(n.id));
      return true;
    });
    unsigned index = 0;
    std::erase_if(g.connections, [&](const auto &e) {
      const bool drop =
          std::find(wires.begin(), wires.end(), index++) != wires.end();
      return drop ||
             std::find(removed.begin(), removed.end(), e.from) !=
                 removed.end() ||
             std::find(removed.begin(), removed.end(), e.to) != removed.end();
    });
    p.setGraph(g);
  });
}
void CreatorWindow::copySelection() {
  auto copy = m_project;
  auto graph = copy.graph();
  const auto ids = m_canvas->selectedNodes();
  std::erase_if(graph.nodes, [&](const auto &n) {
    return !ids.contains(QString::fromStdString(n.id)) || n.type == "input" ||
           n.type == "output" || n.type == "interface";
  });
  std::erase_if(graph.connections, [&](const auto &e) {
    return std::none_of(graph.nodes.begin(), graph.nodes.end(),
                        [&](const auto &n) { return n.id == e.from; }) ||
           std::none_of(graph.nodes.begin(), graph.nodes.end(),
                        [&](const auto &n) { return n.id == e.to; });
  });
  if (graph.nodes.empty())
    return;
  copy.definition = graph;
  copy.activeMode.clear();
  copy.graphPath.clear();
  for (auto it = copy.positions.begin(); it != copy.positions.end();) {
    if (!it.key().startsWith("node/")) it = copy.positions.erase(it); else ++it;
  }
  copy.positions[{}] = m_canvas->nodePositions();
  auto *mime = new QMimeData;
  mime->setData("application/x-vlt-creator-nodes",
                QByteArray::fromStdString(copy.toJson().dump()));
  QApplication::clipboard()->setMimeData(mime);
}
void CreatorWindow::pasteSelection(QPointF position) {
  const auto bytes = QApplication::clipboard()->mimeData()->data(
      "application/x-vlt-creator-nodes");
  if (bytes.isEmpty() || bytes.size() > 4 * 1024 * 1024)
    return;
  CreatorProject clip;
  QString error;
  if (!CreatorProject::fromJson(json::parse(bytes.constData(),
                                            bytes.constData() + bytes.size(),
                                            nullptr, false),
                                clip, error))
    return;
  if (clip.definition.nodes.empty())
    return;
  if (clip.definition.nodes.size() + m_project.graph().nodes.size() >
          kMaxNodes ||
      clip.definition.connections.size() +
              m_project.graph().connections.size() >
          kMaxEdges) {
    diagnostic(tr("Paste would exceed the graph limit."));
    return;
  }
  edit(tr("Paste nodes"), [&](auto &p) {
    auto graph = p.graph();
    std::map<std::string, std::string> definitions;
    for (const auto &g : clip.definition.subgraphs) {
      auto existing = std::find_if(graph.subgraphs.begin(), graph.subgraphs.end(), [&](const auto &current) { return current.id == g.id; });
      definitions[g.id] = existing != graph.subgraphs.end() && *existing != g ? freshId().toStdString() : g.id;
    }
    for (auto g : clip.definition.subgraphs) {
      const auto old = g.id; g.id = definitions.at(old);
      for (auto &n : g.nodes) if (definitions.contains(n.subgraph)) n.subgraph = definitions.at(n.subgraph);
      if (std::none_of(graph.subgraphs.begin(), graph.subgraphs.end(), [&](const auto &current) { return current.id == g.id; })) {
        graph.subgraphs.push_back(g);
        p.positions["node/" + QString::fromStdString(g.id)] = clip.positions.value("node/" + QString::fromStdString(old));
        p.viewports["node/" + QString::fromStdString(g.id)] = clip.viewports.value("node/" + QString::fromStdString(old));
      }
    }
    graph.version = std::max(graph.version, clip.definition.version);
    std::map<std::string, std::string> ids;
    QPointF anchor = clip.positions.value({}).value(
        QString::fromStdString(clip.definition.nodes.front().id));
    for (auto n : clip.definition.nodes) {
      if (n.type == "input" || n.type == "output" || n.type == "interface")
        continue;
      const auto id = freshId();
      ids[n.id] = id.toStdString();
      p.positions[p.layoutKey()][id] =
          position +
          clip.positions.value({}).value(QString::fromStdString(n.id)) - anchor;
      n.id = id.toStdString();
      if (definitions.contains(n.subgraph)) n.subgraph = definitions.at(n.subgraph);
      graph.nodes.push_back(n);
    }
    for (auto e : clip.definition.connections)
      if (ids.contains(e.from) && ids.contains(e.to)) {
        e.from = ids[e.from];
        e.to = ids[e.to];
        graph.connections.push_back(e);
      }
    p.setGraph(graph);
  });
}
void CreatorWindow::properties(const QString &id) {
  auto *body = new QWidget;
  body->setObjectName("CreatorPropertiesBody");
  body->setAutoFillBackground(true);
  auto *column = new QVBoxLayout(body);
  column->setContentsMargins(10, 10, 10, 10);
  column->setSpacing(10);
  const auto graph = m_project.graph();
  const auto node =
      std::find_if(graph.nodes.begin(), graph.nodes.end(),
                   [&](const auto &n) { return n.id == id.toStdString(); });
  if (node == graph.nodes.end()) {
    auto *heading = new QLabel(tr("Build your module"), body);
    heading->setProperty("creatorRole", "heading");
    heading->setWordWrap(true);
    column->addWidget(heading);
    auto *help = new QLabel(
        tr("Right-click the canvas to add a node. Connect matching ports to build the signal path."),
        body);
    help->setWordWrap(true);
    help->setProperty("creatorRole", "muted");
    column->addWidget(help);
    auto *section = new QLabel(tr("PORT TYPES"), body);
    section->setProperty("creatorRole", "section");
    column->addWidget(section);
    const auto c = creatorColors();
    for (const auto &[text, color] : std::initializer_list<std::pair<QString, QColor>>{
        {tr("Audio · sound signal"), c.audio}, {tr("Number · parameter value"), c.number},
        {tr("Gate · trigger or switch"), c.gate}, {tr("Function · callable C++"), c.function},
        {tr("Integer · index or counter"), c.integer}, {tr("Array · fixed numeric collection"), c.array},
        {tr("List · bounded numeric collection"), c.list}, {tr("Buffer · prepared signal history"), c.buffer}}) {
      auto *label = new QLabel(text, body);
      label->setWordWrap(true);
      label->setStyleSheet("color: " + color.name() + "; padding: 4px 0;");
      column->addWidget(label);
    }
    auto *card = new QPushButton(tr("Design module card"), body);
    card->setIcon(icons::icon(icons::Glyph::Layers, c.text));
    column->addSpacing(12);
    column->addWidget(card);
    connect(card, &QPushButton::clicked, this, [this] {
      for (const auto &node : m_project.graph().nodes)
        if (node.type == "interface") {
          m_canvas->selectNode(QString::fromStdString(node.id));
          break;
        }
    });
    auto *hint = new QLabel(tr("Tab  Add node\nF  Fit graph\nCtrl + wheel  Zoom\nSpace + drag  Pan"), body);
    hint->setProperty("creatorRole", "muted");
    hint->setWordWrap(true);
    column->addSpacing(12);
    column->addWidget(hint);
    column->addStretch();
    m_properties->setWidget(body);
    return;
  }
  const auto description = describeNode(*node, &graph);
  const auto *desc = description.id.empty() ? nullptr : &description;
  auto *title = new QLabel(creatorText(desc ? desc->name : node->type), body);
  title->setProperty("creatorRole", "heading");
  title->setWordWrap(true);
  column->addWidget(title);
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapAllRows);
  form->setVerticalSpacing(8);
  column->addLayout(form);
  programmingProperties(body, form, *node);
  // Finish the native input event before replacing its property widgets.
  const auto line = [this, body](QFormLayout *form, const QString &label,
                                 const QString &initial, auto setter) {
    auto *field = new QLineEdit(initial, body);
    field->setMaxLength(128);
    field->setAccessibleName(label);
    form->addRow(label, field);
    connect(field, &QLineEdit::editingFinished, this,
            [this, field, setter, label] {
              const auto value = field->text();
              QTimer::singleShot(0, this, [this, setter, label, value] {
                edit(label, [&](auto &p) { setter(p, value); });
              });
            });
    return field;
  };
  const auto numeric = [this, body](QFormLayout *form, const QString &label,
                                    double lo, double hi, double initial,
                                    auto setter) {
    auto *field = number(lo, hi, initial, body);
    field->setAccessibleName(label);
    form->addRow(label, field);
    connect(field, &QDoubleSpinBox::editingFinished, this,
            [this, field, setter, label] {
              const double value = field->value();
              QTimer::singleShot(0, this, [this, setter, label, value] {
                edit(label, [&](auto &p) { setter(p, value); });
              });
            });
    return field;
  };
  if (node->type == "interface") {
    column->removeItem(form);
    auto *tabs = new QTabWidget(body);
    tabs->setAccessibleName(tr("Module appearance and controls"));
    auto *appearancePage = new QWidget(tabs);
    appearancePage->setLayout(form);
    tabs->addTab(appearancePage, tr("Card"));
    column->addWidget(tabs);
    line(form, tr("Project"), m_project.name, [](auto &p, const auto &v) {
      if (!v.trimmed().isEmpty())
        p.name = v.trimmed();
    });
    line(form, tr("Module"), QString::fromStdString(m_project.definition.name),
         [](auto &p, const auto &v) {
           if (!v.trimmed().isEmpty())
             p.definition.name = v.trimmed().toStdString();
         });
    auto *identity =
        new QLabel(QString::fromStdString(m_project.definition.id), body);
    identity->setWordWrap(true);
    identity->setMinimumWidth(0);
    identity->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identity->setToolTip(
        tr("Stable module ID. Renaming keeps installed instances linked."));
    form->addRow(tr("ID"), identity);
    auto *theme = new QComboBox(body);
    for (const auto *name : {"studio", "graphite", "ivory", "copper"})
      theme->addItem(QString::fromLatin1(name), name);
    theme->setCurrentIndex(theme->findData(
        QString::fromStdString(m_project.definition.appearance.theme)));
    form->addRow(tr("Card theme"), theme);
    connect(theme, qOverload<int>(&QComboBox::activated), this,
            [this, theme](int i) {
              const auto v = theme->itemData(i).toString().toStdString();
              QTimer::singleShot(0, this, [this, v] {
                edit(tr("Card theme"),
                     [&](auto &p) { p.definition.appearance.theme = v; });
              });
            });
    auto *style = new QComboBox(body);
    addStyleChoices(style);
    style->setCurrentIndex(style->findData(
        QString::fromStdString(m_project.definition.appearance.controlStyle)));
    form->addRow(tr("Controls"), style);
    connect(style, qOverload<int>(&QComboBox::activated), this,
            [this, style](int i) {
              const auto v = style->itemData(i).toString().toStdString();
              QTimer::singleShot(0, this, [this, v] {
                edit(tr("Control style"), [&](auto &p) {
                  p.definition.appearance.controlStyle = v;
                });
              });
            });
    auto *color = new QPushButton(tr("Background color…"), body);
    auto *image = new QPushButton(tr("Background image…"), body);
    auto *clear = new QPushButton(tr("Reset background"), body);
    form->addRow(color);
    form->addRow(image);
    form->addRow(clear);
    connect(color, &QPushButton::clicked, this, [this] {
      const auto color = QColorDialog::getColor(
          QColor(QString::fromStdString(
              m_project.definition.appearance.backgroundColor)),
          this, tr("Card background"));
      if (color.isValid())
        edit(tr("Background color"), [&](auto &p) {
          p.definition.appearance.backgroundColor = color.name().toStdString();
        });
    });
    connect(image, &QPushButton::clicked, this, [this] {
      const auto path = QFileDialog::getOpenFileName(
          this, tr("Card background image"), {},
          tr("Images (*.png *.jpg *.jpeg *.webp)"));
      if (path.isEmpty())
        return;
      QImageReader reader(path);
      const auto size = reader.size();
      if (!size.isValid() || size.width() > 32768 || size.height() > 32768) {
        diagnostic(tr("Cannot read this image."));
        return;
      }
      reader.setAutoTransform(true);
      reader.setScaledSize(size.scaled(512, 512, Qt::KeepAspectRatio));
      const auto image = reader.read();
      if (image.isNull()) {
        diagnostic(reader.errorString());
        return;
      }
      QByteArray bytes;
      QBuffer buffer(&bytes);
      buffer.open(QIODevice::WriteOnly);
      image.save(&buffer, "PNG");
      const auto uri = std::string("data:image/png;base64,") +
                       bytes.toBase64().toStdString();
      if (uri.size() > 1024 * 1024) {
        diagnostic(tr("The image is too large after conversion."));
        return;
      }
      edit(tr("Background image"),
           [&](auto &p) { p.definition.appearance.backgroundImage = uri; });
    });
    connect(clear, &QPushButton::clicked, this, [this] {
      edit(tr("Reset background"), [](auto &p) {
        p.definition.appearance.backgroundColor.clear();
        p.definition.appearance.backgroundImage.clear();
      });
    });
    for (unsigned index = 0; index < m_project.definition.controls.size();
         ++index) {
      const auto control = m_project.definition.controls[index];
      auto *group = new QGroupBox(tr("Control %1").arg(index + 1), tabs);
      auto *fields = new QFormLayout(group);
      fields->setRowWrapPolicy(QFormLayout::WrapAllRows);
      tabs->addTab(group, QString::number(index + 1));
      tabs->setTabToolTip(int(index + 1), QString::fromStdString(control.name));
      const auto update = [key = control.id](auto &p, auto mutate) {
        for (auto &c : p.definition.controls)
          if (c.id == key)
            mutate(c);
        p.syncControls();
      };
      line(fields, tr("Name"), QString::fromStdString(control.name),
           [update](auto &p, const auto &v) {
             update(p, [&](auto &c) {
               if (!v.trimmed().isEmpty())
                 c.name = v.trimmed().toStdString();
             });
           });
      line(fields, tr("Unit"), QString::fromStdString(control.unit),
           [update](auto &p, const auto &v) {
             update(p, [&](auto &c) { c.unit = v.left(32).toStdString(); });
           });
      numeric(fields, tr("Minimum"), -1e6, 1e6, control.minimum,
              [update](auto &p, double v) {
                update(p, [&](auto &c) {
                  if (v < c.maximum && (!c.logarithmic || v > 0)) {
                    c.minimum = v;
                    c.initial = std::max(v, c.initial);
                  }
                });
              });
      numeric(fields, tr("Maximum"), -1e6, 1e6, control.maximum,
              [update](auto &p, double v) {
                update(p, [&](auto &c) {
                  if (v > c.minimum) {
                    c.maximum = v;
                    c.initial = std::min(v, c.initial);
                  }
                });
              });
      numeric(fields, tr("Default"), control.minimum, control.maximum,
              control.initial, [update](auto &p, double v) {
                update(p, [&](auto &c) {
                  c.initial = std::clamp(v, c.minimum, c.maximum);
                });
              });
      auto *log = new QCheckBox(tr("Logarithmic"), group);
      log->setChecked(control.logarithmic);
      log->setEnabled(control.minimum > 0);
      fields->addRow(log);
      connect(log, &QCheckBox::clicked, this, [this, update](bool on) {
        QTimer::singleShot(0, this, [this, update, on] {
          edit(tr("Control scale"), [&](auto &p) {
            update(p, [&](auto &c) { c.logarithmic = on && c.minimum > 0; });
          });
        });
      });
      auto *controlStyle = new QComboBox(group);
      controlStyle->addItem(tr("Use card style"), "");
      addStyleChoices(controlStyle);
      controlStyle->setCurrentIndex(
          controlStyle->findData(QString::fromStdString(control.style)));
      fields->addRow(tr("Style"), controlStyle);
      connect(controlStyle, qOverload<int>(&QComboBox::activated), this,
              [this, controlStyle, update](int i) {
                const auto v =
                    controlStyle->itemData(i).toString().toStdString();
                QTimer::singleShot(0, this, [this, v, update] {
                  edit(tr("Control style"), [&](auto &p) {
                    update(p, [&](auto &c) { c.style = v; });
                  });
                });
              });
      auto *remove = new QPushButton(tr("Remove control"), group);
      fields->addRow(remove);
      connect(remove, &QPushButton::clicked, this, [this, key = control.id] {
        edit(tr("Remove control"), [&](auto &p) {
          std::erase_if(p.definition.controls,
                        [&](auto &c) { return c.id == key; });
          p.syncControls();
        });
      });
    }
    auto *add = new QPushButton(tr("Add control"), body);
    add->setEnabled(m_project.definition.controls.size() < 2);
    column->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] {
      edit(tr("Add control"), [](auto &p) {
        if (p.definition.controls.size() < 2)
          p.definition.controls.push_back(
              {freshId().toStdString(), "Control", "", 0, 1, .5});
        p.syncControls();
      });
    });
    tabs->setCurrentIndex(std::min(m_interfaceTab, tabs->count() - 1));
    connect(tabs, &QTabWidget::currentChanged, this,
            [this](int i) { m_interfaceTab = i; });
    column->insertWidget(
        1, new QLabel(tr("Channel Strip preview · 100 px"), body));
    auto preview =
        CreatorProject::create(
            "preview", QString::fromStdString(m_project.definition.name))
            .definition;
    preview.controls = m_project.definition.controls;
    preview.appearance = m_project.definition.appearance;
    for (const auto &mode : m_project.definition.modes)
      preview.modes.push_back({mode.id, mode.name, preview.nodes,
                               preview.connections, preview.controls});
    preview.defaultMode = m_project.definition.defaultMode;
    column->insertWidget(2, MiniModuleRack::createPreview(preview, 100, body),
                         0, Qt::AlignHCenter);
  } else if (desc) {
    auto *hint = new QLabel(tr("Controls are editable on the node. A connected "
                               "input overrides its manual value."),
                            body);
    hint->setWordWrap(true);
    form->addRow(hint);
    // Native combo boxes provide a keyboard alternative to wire dragging.
    for (const auto &input : desc->inputs) {
      auto *source = new QComboBox(body);
      source->setAccessibleName(creatorText(input.name) + " " +
                                tr("connection"));
      source->addItem(input.required ? tr("Not connected") : tr("Manual value"),
                      QStringList{});
      for (const auto &candidate : graph.nodes)
        if (candidate.id != node->id)
          for (const auto &output : outputPorts(candidate, graph))
            if (compatiblePorts(output, input)) {
              const auto type = describeNode(candidate);
              source->addItem(
                  creatorText(type.id.empty() ? candidate.type : type.name) +
                      " / " + creatorText(output.name),
                  QStringList{QString::fromStdString(candidate.id),
                              QString::fromStdString(output.id)});
            }
      for (const auto &edge : graph.connections)
        if (edge.to == node->id && edge.toPort == input.id)
          source->setCurrentIndex(source->findData(
              QStringList{QString::fromStdString(edge.from),
                          QString::fromStdString(edge.fromPort)}));
      form->addRow(creatorText(input.name) + " · " +
                       QString::fromLatin1(portTypeName(input.type)),
                   source);
      connect(
          source, qOverload<int>(&QComboBox::activated), this,
          [this, source, id, port = QString::fromStdString(input.id)](int i) {
            const auto target = source->itemData(i).toStringList();
            if (target.size() == 2)
              emit m_canvas->connectPorts(target[0], target[1], id, port);
            else
              QTimer::singleShot(0, this, [this, id, port] {
                edit(tr("Disconnect port"), [&](auto &p) {
                  auto g = p.graph();
                  std::erase_if(g.connections, [&](auto &e) {
                    return e.to == id.toStdString() &&
                           e.toPort == port.toStdString();
                  });
                  p.setGraph(g);
                });
              });
          });
    }
  }
  column->addStretch();
  m_properties->setWidget(body);
}
void CreatorWindow::openCode(const QString &id) {
  const auto graph = m_project.graph();
  for (const auto &node : graph.nodes)
    if (node.id == id.toStdString() && node.function) {
      m_codeId = id;
      m_project.codeNode = id;
      m_code->setFunction(*node.function, m_project.codeCursors.value(
                                              m_project.layoutKey() + "/" + id));
      m_code->show();
      return;
    }
  m_codeId.clear();
  m_project.codeNode.clear();
  m_code->hide();
}
void CreatorWindow::codeEdited() {
  if (m_codeId.isEmpty())
    return;
  const auto text = m_code->source().toStdString();
  if (text.size() > kMaxFunctionSourceBytes) {
    diagnostic(tr("C++ source exceeds 256 KiB."), m_codeId);
    openCode(m_codeId);
    return;
  }
  auto after = m_project;
  auto graph = after.graph();
  for (auto &node : graph.nodes)
    if (node.id == m_codeId.toStdString() && node.function) {
      node.function->source = text;
      node.function->entry = m_code->entry().toStdString();
    }
  after.setGraph(graph);
  after.codeCursors[after.layoutKey() + "/" + m_codeId] =
      m_code->cursorPosition();
  if (after == m_project)
    return;
  m_undo->push(new ProjectEdit(
      tr("Edit C++ source"), m_project, std::move(after),
      [this](auto p) { apply(std::move(p)); },
      m_project.layoutKey() + "/" + m_codeId));
}
void CreatorWindow::codeOperation(const QString &operation) {
  if (m_codeFuture.valid() || m_build->busy() ||
      m_codeId.isEmpty())
    return;
  const auto graph = m_project.graph();
  const auto node =
      std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto &n) {
        return n.id == m_codeId.toStdString();
      });
  if (node == graph.nodes.end() || !node->function)
    return;
  json request{{"action", operation.toStdString()},
               {"function", functionToJson(*node->function)}};
  if (operation == "bind" || operation == "extract") {
    if (graph.nodes.size() >= kMaxNodes ||
        graph.connections.size() >= kMaxEdges) {
      diagnostic(tr("The graph has reached its node or connection limit."));
      return;
    }
    if (operation == "bind") {
      auto child = createFunctionDialog(this);
      if (!child)
        return;
      request["child"] = functionToJson(*child);
    } else {
      bool ok = false;
      const auto name =
          QInputDialog::getText(this, tr("Extract function"),
                                tr("Name of the local helper function"),
                                QLineEdit::Normal, {}, &ok)
              .trimmed();
      if (!ok || name.isEmpty())
        return;
      request["name"] = name.toStdString();
    }
  }
  auto result = std::make_shared<CodeResult>();
  result->node = m_codeId;
  result->mode = m_project.layoutKey();
  result->project = QString::fromStdString(m_project.definition.id);
  result->operation = operation;
  result->revision = functionSourceHash(*node->function);
  m_cancel.store(false);
  m_cancelAction->setEnabled(true);
  m_code->setBusy(true);
  m_compileAction->setEnabled(false);
  m_status->setText(tr("Checking C++…"));
  m_diagnostics->clear();
  m_codeFuture = std::async(std::launch::async, [this, result, request] {
    result->reply = creatorCompilerRequest(request, &m_cancel);
    return result;
  });
  m_compilePoll->start();
}
void CreatorWindow::pollCode() {
  if (!m_codeFuture.valid() || m_codeFuture.wait_for(std::chrono::milliseconds(
                                   0)) != std::future_status::ready)
    return;
  m_compilePoll->stop();
  m_code->setBusy(false);
  m_compileAction->setEnabled(true);
  m_cancelAction->setEnabled(false);
  std::shared_ptr<CodeResult> result;
  try {
    result = m_codeFuture.get();
  } catch (const std::exception &e) {
    diagnostic(QString::fromUtf8(e.what()));
    return;
  }
  auto graph = m_project.graph();
  auto node =
      std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto &n) {
        return n.id == result->node.toStdString();
      });
  if (m_cancel.load() ||
      result->project != QString::fromStdString(m_project.definition.id) ||
      result->mode != m_project.layoutKey() || node == graph.nodes.end() ||
      !node->function ||
      functionSourceHash(*node->function) != result->revision) {
    diagnostic(tr("The draft changed or the operation was cancelled. Its "
                  "result was discarded."));
    return;
  }
  for (const auto &entry : result->reply.value("diagnostics", json::array())) {
    const unsigned line = entry.value("line", 0u),
                   column = entry.value("column", 0u);
    diagnostic(
        QString("%1:%2 · %3")
            .arg(line)
            .arg(column)
            .arg(QString::fromStdString(entry.value("message", std::string{}))),
        result->node);
    auto *item = m_diagnostics->item(m_diagnostics->count() - 1);
    item->setData(Qt::UserRole + 1, line);
    item->setData(Qt::UserRole + 2, column);
  }
  if (!result->reply.value("ok", false)) {
    diagnostic(QString::fromStdString(result->reply.value(
                   "error", std::string("C++ check failed"))),
               result->node);
    return;
  }
  auto after = m_project;
  QString error;
  if (!creatorApplyFunction(after, result->node, result->operation, result->reply, error)) {
    diagnostic(error, result->node); return;
  }
  edit(result->operation == "analyze" ? tr("Update C++ ports") :
       result->operation == "extract" ? tr("Extract function") : tr("Create function"),
       [&](auto& p) { p = after; });
  openCode(result->node);
  diagnostic(result->operation == "bind"
                 ? tr("Function connected. Add its call at the desired place "
                      "in your algorithm.")
                 : tr("C++ ports updated."),
             result->node);
}
void CreatorWindow::compile() {
  if (m_codeFuture.valid() || m_build->busy()) return;
  startBuild();
}
bool CreatorWindow::startBuild(CreatorBuildService::Done done) {
  if (m_codeFuture.valid() || m_build->busy()) return false;
  const auto revision = codeHash(daw::plugins::mini::toJson(m_project.definition).dump());
  const auto projectId = m_project.definition.id;
  m_diagnostics->clear();
  m_compileAction->setEnabled(false); m_cancelAction->setEnabled(true);
  m_build->progress = [this](const auto& stage) { m_status->setText(stage); };
  const bool chatBuild = bool(done);
  return m_build->start(m_project, m_installDirectory,
    [this, revision, projectId] {
      return m_project.definition.id == projectId && revision == codeHash(daw::plugins::mini::toJson(m_project.definition).dump());
    }, [this, chatBuild, done = std::move(done)](auto result) {
      m_compileAction->setEnabled(true); m_cancelAction->setEnabled(false);
      if (!chatBuild) for (const auto& entry : result->diagnostics) {
        diagnostic(QString("%1:%2 · %3").arg(entry.line).arg(entry.column).arg(QString::fromStdString(entry.message)), QString::fromStdString(entry.node));
        auto* item = m_diagnostics->item(m_diagnostics->count()-1);
        item->setData(Qt::UserRole+1, entry.line); item->setData(Qt::UserRole+2, entry.column);
      }
      if (!result->error.isEmpty()) { if (chatBuild) m_status->setText(result->error); else diagnostic(result->error); }
      else {
        m_project.definition = result->project.definition;
        if (!chatBuild) for (const auto& warning : result->warnings) diagnostic(warning);
        if (!m_path.isEmpty()) { QString error; if (!m_project.save(m_path, error)) diagnostic(error); }
        const auto message = tr("Compiled and installed. Select “%1” in a Channel Strip.").arg(QString::fromStdString(m_project.definition.name));
        if (chatBuild) m_status->setText(message); else diagnostic(message);
        emit modulesCompiled();
      }
      if (done) done(std::move(result));
    });
}
} // namespace ui
