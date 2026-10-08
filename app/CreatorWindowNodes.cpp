#include "CreatorWindow.hpp"
#include "CreatorCanvas.hpp"
#include "CreatorStyle.hpp"
#include "CreatorText.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <nlohmann/json.hpp>

namespace ui {
using namespace daw::plugins::mini;
using json = nlohmann::json;
namespace {
QString nodeDirectory() { return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/VLTONE/CreatorNodes"; }
std::string uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
json readNode(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) throw std::runtime_error("Cannot read node file (maximum 32 MiB)");
  auto bytes = file.readAll(); auto data = json::parse(bytes.constData(), bytes.constData() + bytes.size());
  if (data.at("format") != "vltnode" || data.at("version") != 1) throw std::runtime_error("Unsupported custom node file");
  return data;
}
}
void CreatorWindow::refreshNodeLibrary() {
  if (!m_nodeLibraryWatcher) {
    QDir().mkpath(nodeDirectory());
    m_nodeLibraryWatcher = new QFileSystemWatcher(this);
    m_nodeLibraryWatcher->addPath(nodeDirectory());
    auto *refresh = new QTimer(this);
    refresh->setSingleShot(true);
    refresh->setInterval(100);
    connect(m_nodeLibraryWatcher, &QFileSystemWatcher::directoryChanged, refresh, [refresh] { refresh->start(); });
    connect(m_nodeLibraryWatcher, &QFileSystemWatcher::fileChanged, refresh, [refresh] { refresh->start(); });
    connect(refresh, &QTimer::timeout, this, &CreatorWindow::refreshNodeLibrary);
  }
  m_personalNodes.clear();
  for (const auto &g : m_project.definition.subgraphs) m_personalNodes["custom:" + QString::fromStdString(g.id)] = QString::fromStdString(g.name);
  QDir directory(nodeDirectory());
  QStringList files;
  for (const auto &file : directory.entryInfoList({"*.vltnode"}, QDir::Files, QDir::Name)) {
    files.push_back(file.absoluteFilePath());
    try {
      const auto data = readNode(file.absoluteFilePath());
      const auto root = data.at("root").get<std::string>();
      QString name = file.completeBaseName();
      for (const auto &g : data.at("definition").at("subgraphs")) if (g.at("id") == root) name = QString::fromStdString(g.at("name").get<std::string>());
      m_personalNodes["library:" + file.absoluteFilePath()] = name + tr(" · Library");
    } catch (...) { /* An unavailable file stays on disk and can be explicitly opened for diagnostics. */ }
  }
  const auto watched = m_nodeLibraryWatcher->files();
  for (const auto &path : watched) if (!files.contains(path)) m_nodeLibraryWatcher->removePath(path);
  for (const auto &path : files) if (!watched.contains(path)) m_nodeLibraryWatcher->addPath(path);
  if (m_canvas) m_canvas->setLibraryNodes(m_personalNodes);
  if (m_fillLibrary) { auto position = m_library->verticalScrollBar()->value(); m_fillLibrary(); m_library->verticalScrollBar()->setValue(position); }
}
void CreatorWindow::groupSelection() {
  const auto selected = m_canvas->selectedNodes();
  if (selected.empty()) { m_status->setText(tr("Select processing nodes first.")); return; }
  bool ok = false;
  const auto name = QInputDialog::getText(this, tr("Create node from selection"), tr("Node name"), QLineEdit::Normal, tr("My node"), &ok).trimmed();
  if (!ok || name.isEmpty()) return;
  QString id, error;
  edit(tr("Create custom node"), [&](auto &p) { id = p.pack(selected, name.left(128), error); });
  if (!error.isEmpty()) diagnostic(error);
  else m_canvas->selectNode(id);
}
void CreatorWindow::enterNode(const QString &id) {
  const auto graph = m_project.graph();
  auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto &n) { return n.id == id.toStdString(); });
  if (node == graph.nodes.end() || node->subgraph.empty() || m_project.graphPath.size() >= kMaxGraphDepth) return;
  m_project.viewports[m_project.layoutKey()] = m_canvas->viewportState();
  m_project.graphPath.push_back(QString::fromStdString(node->subgraph));
  ++m_revision;
  m_project.codeNode.clear(); m_selected.clear(); refresh();
  m_canvas->restoreViewport(m_project.viewports.value(m_project.layoutKey()));
}
void CreatorWindow::exportNodeFile() {
  const auto graph = m_project.graph();
  auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto &n) { return n.id == m_selected.toStdString() && !n.subgraph.empty(); });
  if (node == graph.nodes.end()) return;
  const auto root = node->subgraph;
  MiniModuleDefinition bundle; bundle.version = 5; bundle.id = "creator.node"; bundle.name = "Custom node";
  std::set<std::string> visited;
  std::function<void(const std::string &)> append = [&](const auto &id) {
    if (!visited.insert(id).second) return;
    auto g = std::find_if(graph.subgraphs.begin(), graph.subgraphs.end(), [&](const auto &g) { return g.id == id; });
    if (g == graph.subgraphs.end()) return;
    bundle.subgraphs.push_back(*g);
    for (const auto &n : g->nodes) if (!n.subgraph.empty()) append(n.subgraph);
  };
  append(root);
  if (bundle.subgraphs.empty()) { diagnostic(tr("Missing node definition")); return; }
  const auto name = QString::fromStdString(bundle.subgraphs.front().name).replace(QRegularExpression("[^\\p{L}\\p{N}_.-]"), "_");
  QDir().mkpath(nodeDirectory());
  auto path = QFileDialog::getSaveFileName(this, tr("Export custom node"), nodeDirectory() + "/" + name + ".vltnode", tr("Creator node (*.vltnode)"));
  if (path.isEmpty()) return;
  if (!path.endsWith(".vltnode", Qt::CaseInsensitive)) path += ".vltnode";
  CreatorProject layouts; layouts.definition = bundle;
  for (const auto &g : bundle.subgraphs) {
    const auto key = "node/" + QString::fromStdString(g.id);
    layouts.positions[key] = m_project.positions.value(key); layouts.viewports[key] = m_project.viewports.value(key);
  }
  auto data = layouts.toJson(); data["format"] = "vltnode"; data["version"] = 1; data["root"] = root;
  auto bytes = data.dump(2); QSaveFile file(path);
  if (bytes.size() > 32 * 1024 * 1024 || !file.open(QIODevice::WriteOnly) || file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit()) { diagnostic(tr("Unable to save node file") + ": " + file.errorString()); return; }
  refreshNodeLibrary(); m_status->setText(tr("Node saved: ") + path);
}
void CreatorWindow::importNodeFile(const QString &given, std::optional<QPointF> position) {
  const auto path = given.isEmpty() ? QFileDialog::getOpenFileName(this, tr("Import custom node"), nodeDirectory(), tr("Creator node (*.vltnode)")) : given;
  if (path.isEmpty()) return;
  try {
    auto data = readNode(path); auto root = data.at("root").get<std::string>();
    data["format"] = "vltcreator"; data["version"] = 3;
    CreatorProject imported; QString why;
    if (!CreatorProject::fromJson(data, imported, why)) throw std::runtime_error(why.toStdString());
    if (std::none_of(imported.definition.subgraphs.begin(), imported.definition.subgraphs.end(), [&](const auto &g) { return g.id == root; })) throw std::runtime_error("Missing root node definition");
    bool conflict = false;
    for (const auto &g : imported.definition.subgraphs) for (const auto &current : m_project.definition.subgraphs) if (g.id == current.id && g != current) conflict = true;
    bool independent = false;
    if (conflict) {
      QMessageBox choice(QMessageBox::Question, tr("Node already exists"), tr("This file contains a different version. Update this project's type or import an independent copy?"), QMessageBox::Cancel, this);
      auto *copy = choice.addButton(tr("Independent copy"), QMessageBox::AcceptRole);
      auto *update = choice.addButton(tr("Update type"), QMessageBox::ActionRole);
      choice.exec(); if (choice.clickedButton() != copy && choice.clickedButton() != update) return;
      independent = choice.clickedButton() == copy;
    }
    if (independent) {
      std::map<std::string, std::string> ids;
      for (const auto &g : imported.definition.subgraphs) ids[g.id] = uuid();
      root = ids.at(root);
      for (auto &g : imported.definition.subgraphs) {
        const auto old = "node/" + QString::fromStdString(g.id); g.id = ids.at(g.id);
        const auto key = "node/" + QString::fromStdString(g.id);
        imported.positions[key] = imported.positions.take(old); imported.viewports[key] = imported.viewports.take(old);
        for (auto &n : g.nodes) if (ids.contains(n.subgraph)) n.subgraph = ids.at(n.subgraph);
      }
    }
    const auto id = uuid();
    edit(tr("Import custom node"), [&](auto &p) {
      auto g = p.graph(); g.version = 5;
      for (const auto &type : imported.definition.subgraphs) {
        auto existing = std::find_if(g.subgraphs.begin(), g.subgraphs.end(), [&](const auto &n) { return n.id == type.id; });
        if (existing == g.subgraphs.end()) g.subgraphs.push_back(type); else *existing = type;
        const auto key = "node/" + QString::fromStdString(type.id);
        if (imported.positions.contains(key)) p.positions[key] = imported.positions[key];
        if (imported.viewports.contains(key)) p.viewports[key] = imported.viewports[key];
      }
      auto node = makeNode("subgraph", id); node.subgraph = root; g.nodes.push_back(node); g.code = {};
      p.positions[p.layoutKey()][QString::fromStdString(id)] = position.value_or(m_canvas->viewportState().center);
      p.setGraph(g);
    });
    m_canvas->selectNode(QString::fromStdString(id));
  } catch (const std::exception &e) { diagnostic(QString::fromUtf8(e.what())); }
}
bool CreatorWindow::programmingProperties(QWidget *body, QFormLayout *form, const NodeDefinition &node) {
  const auto id = node.id;
  const auto change = [this, id](const QString &name, const std::function<void(NodeDefinition &)> &f) {
    edit(name, [&](auto &p) { auto g = p.graph(); for (auto &n : g.nodes) if (n.id == id) f(n); g.code = {}; p.setGraph(g); });
  };
  auto *label = new QLineEdit(QString::fromStdString(node.label), body); label->setMaxLength(128); label->setPlaceholderText(tr("Default name"));
  form->addRow(tr("Label"), label);
  connect(label, &QLineEdit::editingFinished, this, [label, change] { change(tr("Rename node"), [=](auto &n) { n.label = label->text().trimmed().toStdString(); }); }, Qt::QueuedConnection);
  const bool generic = node.type == "history" || node.type == "wire" || node.type == "length" || node.type == "get" || node.type == "set" || node.type == "clear" || node.type == "sum" || node.type == "collection_min" || node.type == "collection_max" || node.type == "map" || node.type == "reduce";
  if (generic) {
    auto *type = new QComboBox(body);
    for (auto t : {PortType::Audio, PortType::Number, PortType::Gate, PortType::Integer, PortType::Array, PortType::List})
      if (node.type == "history" || node.type == "wire" || t == PortType::Array || t == PortType::List) type->addItem(creatorText(portTypeName(t)), portTypeId(t));
    type->setCurrentIndex(std::max(0, type->findData(QString::fromStdString(node.valueType))));
    form->addRow(tr("Value type"), type);
    connect(type, qOverload<int>(&QComboBox::activated), this, [type, change] { const auto v = type->currentData().toString().toStdString(); change(tr("Change value type"), [v](auto &n) { n.valueType = v; }); }, Qt::QueuedConnection);
  }
  if (node.type == "array" || node.type == "list" || (node.type == "history" && (node.valueType == "array" || node.valueType == "list"))) {
    auto *capacity = new CreatorNumberField(body); capacity->setDecimals(0); capacity->setRange(1, kMaxCollection); capacity->setValue(node.capacity); capacity->setDefaultValue(64);
    form->addRow(tr("Capacity"), capacity);
    connect(capacity, &QDoubleSpinBox::editingFinished, this, [capacity, change] { const auto v = unsigned(capacity->value()); change(tr("Change capacity"), [v](auto &n) { n.capacity = v; }); }, Qt::QueuedConnection);
  }
  if (node.type == "array" || node.type == "list" || node.type == "history" || node.type == "curve") {
    auto *values = new QPushButton(node.type == "curve" ? tr("Edit curve points…") : tr("Edit initial values…"), body); form->addRow(values);
    connect(values, &QPushButton::clicked, this, [this, node, change] {
      QDialog dialog(this); dialog.setWindowTitle(node.type == "curve" ? tr("Transfer curve") : tr("Initial values")); dialog.resize(380, 440);
      auto *layout = new QVBoxLayout(&dialog);
      auto *table = new QTableWidget(&dialog); const bool curve = node.type == "curve"; table->setColumnCount(curve ? 2 : 1);
      table->setHorizontalHeaderLabels(curve ? QStringList{"X", "Y"} : QStringList{tr("Value")});
      const unsigned stride = curve ? 2 : 1; table->setRowCount(int(node.values.size() / stride));
      for (unsigned i = 0; i < node.values.size(); ++i) table->setItem(int(i / stride), int(i % stride), new QTableWidgetItem(QString::number(node.values[i], 'g', 12)));
      layout->addWidget(table);
      auto *add = new QPushButton(tr("Add"), &dialog), *remove = new QPushButton(tr("Remove"), &dialog);
      layout->addWidget(add); layout->addWidget(remove);
      connect(add, &QPushButton::clicked, &dialog, [=] { if (table->rowCount() >= int(curve ? kMaxCollection : node.capacity)) return; auto r = table->rowCount(); table->insertRow(r); for (unsigned k = 0; k < stride; ++k) table->setItem(r,int(k),new QTableWidgetItem("0")); });
      connect(remove, &QPushButton::clicked, &dialog, [=] { if (table->currentRow() >= 0) table->removeRow(table->currentRow()); });
      auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
      connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
      std::vector<double> values;
      connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        values.clear(); bool valid = !curve || table->rowCount() >= 2;
        for (int r = 0; r < table->rowCount(); ++r) for (unsigned c = 0; c < stride; ++c) {
          bool ok = false; const auto *item = table->item(r, int(c)); const auto v = item ? item->text().toDouble(&ok) : 0;
          valid &= ok && std::isfinite(v); values.push_back(v);
        }
        if (curve) for (unsigned i = 2; i < values.size(); i += 2) valid &= values[i] > values[i - 2];
        if (!valid) { QMessageBox::information(&dialog, tr("Invalid values"), tr("Enter finite numbers. Curve points need increasing X and at least two points.")); return; }
        dialog.accept();
      });
      if (dialog.exec() == QDialog::Accepted) change(tr("Edit initial data"), [&](auto &n) { n.values = values; });
    });
  }
  if (!node.subgraph.empty()) {
    auto *open = new QPushButton(tr("Open node"), body); form->addRow(open);
    connect(open, &QPushButton::clicked, this, [this,id] { enterNode(QString::fromStdString(id)); });
    auto *ports = new QPushButton(tr("Edit ports…"), body); form->addRow(ports);
    connect(ports, &QPushButton::clicked, this, [this,groupId = node.subgraph] {
      auto g = std::find_if(m_project.definition.subgraphs.begin(), m_project.definition.subgraphs.end(), [&](const auto &g) { return g.id == groupId; });
      if (g == m_project.definition.subgraphs.end()) return;
      auto definition = *g;
      QDialog dialog(this); dialog.setWindowTitle(tr("Custom node ports")); dialog.resize(1040, 600); auto *layout = new QVBoxLayout(&dialog);
      QTableWidget *tables[2]{};
      for (unsigned side = 0; side < 2; ++side) {
        layout->addWidget(new QLabel(side ? tr("Outputs") : tr("Inputs"), &dialog));
        auto *table = tables[side] = new QTableWidget(&dialog); table->setColumnCount(10);
        table->setHorizontalHeaderLabels({tr("Name"),tr("Type"),tr("Unit"),tr("Minimum"),tr("Maximum"),tr("Default"),tr("Logarithmic"),tr("Capacity"),tr("Signature"),tr("ID")});
        table->setColumnHidden(9,true);
        const auto append = [table](const GraphPort &p) {
          int r = table->rowCount(); table->insertRow(r);
          const QStringList values{QString::fromStdString(p.name),QString::fromStdString(p.type),QString::fromStdString(p.unit),QString::number(p.minimum,'g',12),QString::number(p.maximum,'g',12),QString::number(p.initial,'g',12),p.logarithmic ? "1" : "0",QString::number(p.capacity),QString::fromStdString(p.signature),QString::fromStdString(p.id)};
          for (int c = 0; c < values.size(); ++c) table->setItem(r,c,new QTableWidgetItem(values[c]));
        };
        for (const auto &p : side ? definition.outputs : definition.inputs) append(p);
        layout->addWidget(table);
        auto *row = new QHBoxLayout; auto *add = new QPushButton(tr("Add port"), &dialog), *remove = new QPushButton(tr("Remove port"), &dialog); row->addWidget(add); row->addWidget(remove);
        for (int direction : {-1, 1}) {
          auto *move = new QPushButton(direction < 0 ? tr("Up") : tr("Down"), &dialog); row->addWidget(move);
          connect(move, &QPushButton::clicked, &dialog, [table,direction] {
            const auto from = table->currentRow(), to = from + direction;
            if (from < 0 || to < 0 || to >= table->rowCount()) return;
            for (int c = 0; c < table->columnCount(); ++c) { auto *a = table->takeItem(from,c), *b = table->takeItem(to,c); table->setItem(from,c,b); table->setItem(to,c,a); }
            table->setCurrentCell(to,0);
          });
        }
        layout->addLayout(row);
        connect(add,&QPushButton::clicked,&dialog,[table,append] { if (table->rowCount() < 16) { GraphPort p; p.id = uuid(); p.name = "Value"; append(p); } });
        connect(remove,&QPushButton::clicked,&dialog,[table] { table->removeRow(table->currentRow()); });
      }
      auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,&dialog); layout->addWidget(buttons);
      connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
      connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        bool valid = tables[1]->rowCount() > 0;
        for (unsigned side = 0; side < 2; ++side) {
          auto &ports = side ? definition.outputs : definition.inputs; ports.clear(); auto *table = tables[side];
          for (int r = 0; r < table->rowCount(); ++r) {
            const auto text = [&](int c) { return table->item(r,c) ? table->item(r,c)->text() : QString{}; };
            GraphPort p; p.id = text(9).toStdString(); p.name = text(0).toStdString(); p.type = text(1).toStdString(); p.unit = text(2).toStdString();
            bool a,b,c,d; p.minimum=text(3).toDouble(&a);p.maximum=text(4).toDouble(&b);p.initial=text(5).toDouble(&c);p.capacity=text(7).toUInt(&d);
            p.logarithmic = text(6) == "1"; p.signature = text(8).toStdString();
            valid &= a && b && c && d && parsePortType(p.type).has_value() && !p.name.empty() && p.name.size() <= 128 && std::isfinite(p.minimum) && std::isfinite(p.maximum) && std::isfinite(p.initial) && p.minimum <= p.initial && p.initial <= p.maximum && p.capacity >= 1 && p.capacity <= kMaxCollection && (!p.logarithmic || p.minimum > 0);
            ports.push_back(p);
          }
        }
        if (!valid) { QMessageBox::information(&dialog,tr("Invalid ports"),tr("Check port types, finite ranges, defaults and capacity (1–4096).")); return; }
        dialog.accept();
      });
      if (dialog.exec() != QDialog::Accepted) return;
      edit(tr("Edit custom node ports"),[&](auto &p) {
        for (unsigned side=0;side<2;++side) for (const auto &port : side ? definition.outputs : definition.inputs) {
          auto n=std::find_if(definition.nodes.begin(),definition.nodes.end(),[&](const auto &n){ return n.type==(side?"subgraph_output":"subgraph_input") && n.port==port.id; });
          if(n==definition.nodes.end()){auto v=makeNode(side?"subgraph_output":"subgraph_input",uuid());v.port=port.id;definition.nodes.push_back(v);n=std::prev(definition.nodes.end());p.positions["node/"+QString::fromStdString(groupId)][QString::fromStdString(n->id)]=QPointF(side?600:20,definition.nodes.size()*28.);}
          n->label=port.name;n->valueType=port.type;n->capacity=port.capacity;n->signature=port.signature;n->parameters={{"default",port.initial}};
        }
        // Keep removed boundary nodes and their wires visible in the draft.
        // Deleting/reconnecting them is an explicit edit, reversible with Undo.
        for (auto &n : definition.nodes) if (n.type == "subgraph_input" || n.type == "subgraph_output") {
          const auto &ports = n.type == "subgraph_input" ? definition.inputs : definition.outputs;
          if (std::none_of(ports.begin(), ports.end(), [&](const auto &port) { return port.id == n.port; })) n.label = "Missing: " + n.port;
        }
        for(auto &g:p.definition.subgraphs)if(g.id==groupId)g=definition;
        const auto sync = [&](auto &nodes) {
          for (auto &n : nodes) if (n.type == "subgraph" && n.subgraph == groupId) {
            const auto desc = describeNode(n, &p.definition);
            std::erase_if(n.parameters, [&](const auto &value) { return std::none_of(desc.parameters.begin(), desc.parameters.end(), [&](const auto &field) { return field.id == value.id; }); });
            for (auto &value : n.parameters) for (const auto &field : desc.parameters) if (field.id == value.id) value.value = std::clamp(value.value, field.minimum, field.maximum);
          }
        };
        sync(p.definition.nodes); for (auto &mode : p.definition.modes) sync(mode.nodes);
        for (auto &g : p.definition.subgraphs) sync(g.nodes);
        p.definition.code={};for(auto &mode:p.definition.modes)mode.code={};
      });
    });
    if (node.type == "subgraph") {
      auto *sampling = new QComboBox(body); sampling->addItem("1×",1); sampling->addItem("2×",2); sampling->addItem("4×",4);
      for (const auto &g : m_project.definition.subgraphs) if (g.id == node.subgraph) sampling->setCurrentIndex(sampling->findData(g.oversampling));
      form->addRow(tr("Oversampling"), sampling);
      connect(sampling,qOverload<int>(&QComboBox::activated),this,[this,sampling,key=node.subgraph] { const auto factor=sampling->currentData().toUInt(); edit(tr("Change oversampling"),[&](auto &p){for(auto &g:p.definition.subgraphs)if(g.id==key)g.oversampling=factor;p.definition.code={};}); },Qt::QueuedConnection);
    }
  }
  return true;
}
} // namespace ui
