#include "CreatorCanvas.hpp"
#include "CreatorText.hpp"
#include "CreatorStyle.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "Creator/CodeUtilities.hpp"
#include "Theme.hpp"
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <QPushButton>
#include <algorithm>
#include <cmath>

namespace ui {
using namespace daw::plugins::mini;
namespace {
constexpr int nodeRole = 10, wireRole = 11;
QColor portColor(PortType type) {
  const auto c = creatorColors();
  if (type == PortType::Integer) return c.integer;
  if (type == PortType::Array) return c.array;
  if (type == PortType::List) return c.list;
  if (type == PortType::Buffer) return c.buffer;
  return type == PortType::Audio ? c.audio : type == PortType::Function ? c.function
         : type == PortType::Gate ? c.gate : c.number;
}
QPainterPath cable(QPointF a, QPointF b) {
  QPainterPath path(a);
  const double reach = std::clamp(std::abs(b.x() - a.x()) * .45, 55., 240.);
  path.cubicTo(a + QPointF(reach, 0), b - QPointF(reach, 0), b);
  return path;
}
class Port final : public QGraphicsItem {
public:
  Port(QString node, const PortDescription &port, bool output,
       QGraphicsItem *parent)
      : QGraphicsItem(parent), node(std::move(node)),
        id(QString::fromStdString(port.id)), title(creatorText(port.name)),
        kind(port.type), signature(port.signature), output(output), missing(port.name.starts_with("Missing:")) {
    setAcceptHoverEvents(true);
    setCursor(Qt::CrossCursor);
    setZValue(5);
    setData(12, id);
    setData(13, output);
    setToolTip(title + " · " + QString::fromLatin1(portTypeName(kind)) +
               (output ? " · Output" : " · Input"));
  }
  QRectF boundingRect() const override { return {-12, -12, 24, 24}; }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *,
             QWidget *) override {
    p->setRenderHint(QPainter::Antialiasing);
    QColor color = missing ? QColor(217,87,91) : portColor(kind);
    if (dimmed)
      color.setAlpha(70);
    if (hot || compatible) {
      p->setPen(QPen(color, 1));
      p->setBrush(Qt::NoBrush);
      p->drawEllipse(QRectF(-9, -9, 18, 18));
    }
    p->setPen(QPen(color, 1.5));
    p->setBrush(connected || output ? color : creatorColors().card);
    if (kind == PortType::Audio)
      p->drawEllipse(QRectF(-4.5, -4.5, 9, 9));
    else if (kind == PortType::Gate)
      p->drawRoundedRect(QRectF(-4.5, -4.5, 9, 9), 1, 1);
    else if(kind==PortType::Function) {
      p->drawPolygon(QPolygonF{{-5,-3},{0,-6},{5,-3},{5,3},{0,6},{-5,3}});
    } else if (kind == PortType::Integer) {
      p->drawPolygon(QPolygonF{{0,-6},{6,5},{-6,5}});
    } else if (kind == PortType::Array) {
      p->drawRect(QRectF(-5,-5,10,10)); p->drawLine(-1,-5,-1,5);
    } else if (kind == PortType::List) {
      p->setBrush(Qt::NoBrush);
      for (int y : {-4, 0, 4}) p->drawLine(-5,y,5,y);
    } else if (kind == PortType::Buffer) {
      p->drawRoundedRect(QRectF(-6,-4,12,8),4,4); p->drawLine(1,-4,1,4);
    } else {
      QPolygonF diamond{{0, -5}, {5, 0}, {0, 5}, {-5, 0}};
      p->drawPolygon(diamond);
    }
  }
  void hoverEnterEvent(QGraphicsSceneHoverEvent *) override {
    hot = true;
    update();
  }
  void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override {
    hot = false;
    update();
  }
  QString node, id, title;
  PortType kind;
  std::string signature;
  bool missing=false;
  bool matches(const Port &other) const {return !missing && !other.missing && kind==other.kind && (kind!=PortType::Function || signature==other.signature);}
  bool output, hot = false, connected = false, compatible = false,
               dimmed = false;
};
class NodeItem final : public QGraphicsObject {
public:
  NodeItem(const NodeDefinition &n, const MiniModuleDefinition &graph,
           CreatorCanvas *canvas)
      : model(n), canvas(canvas), description(describeNode(n, &graph)),
        desc(description.id.empty() ? nullptr : &description) {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges |
             ItemIsFocusable);
    setCacheMode(DeviceCoordinateCache);
    setData(nodeRole, QString::fromStdString(n.id));
    setData(20, n.label.starts_with("Missing:"));
    setToolTip(desc ? creatorText(desc->name) + " · " +
                          creatorText(desc->category)
                    : QString::fromStdString(n.type));
    if (!desc) {
      height = 90;
      return;
    }
    auto outputs = outputPorts(n, graph);
    auto inputs = desc->inputs;
    // Keep disconnected/renamed draft endpoints visible and removable.
    for (const auto &e : graph.connections) {
      auto &ports = e.from == n.id ? outputs : inputs;
      const auto &id = e.from == n.id ? e.fromPort : e.toPort;
      if ((e.from == n.id || e.to == n.id) &&
          std::none_of(ports.begin(), ports.end(), [&](const auto &p) { return p.id == id; }))
        ports.push_back({id, "Missing: " + id, PortType::Number});
    }
    double y = 52;
    for (const auto &port : outputs) {
      auto *item = new Port(QString::fromStdString(n.id), port, true, this);
      item->setPos(width, y);
      labels.push_back(
          {QRectF(20, y - 10, width - 36, 20), creatorText(port.name), true});
      y += 28;
    }
    const auto control = [&](const NodeParameterDescription &p, unsigned index,
                             double row, bool connected) {
      double value = p.initial;
      for (const auto &parameter : model.parameters)
        if (parameter.id == p.id)
          value = parameter.value;
      QWidget *widget = nullptr;
      if (!p.choices.empty()) {
        auto *combo = new QComboBox;
        for (const auto &choice : p.choices)
          combo->addItem(creatorText(choice));
        combo->setCurrentIndex(int(value - p.minimum));
        widget = combo;
        QObject::connect(combo, qOverload<int>(&QComboBox::activated), canvas,
                         [canvas, n, p](int v) {
                           emit canvas->parameterEdited(
                               QString::fromStdString(n.id),
                               QString::fromStdString(p.id), v + p.minimum);
                         });
      } else {
        auto *spin = new CreatorNumberField;
        const double factor =
            p.unit == "%" && p.minimum >= 0 && p.maximum <= 1 ? 100 : 1;
        spin->setRange(p.minimum * factor, p.maximum * factor);
        spin->setDecimals(factor == 100 || p.maximum > 100000 ? 2 : 3);
        spin->setSingleStep((p.logarithmic ? std::max(.001, p.initial * .01)
                                           : (p.maximum - p.minimum) / 100) *
                            factor);
        const bool discrete = (n.type == "history" && (n.valueType == "integer" || n.valueType == "gate")) ||
          std::any_of(desc->inputs.begin(), desc->inputs.end(), [&](const auto &port) { return port.parameter == int(index) && (port.type == PortType::Integer || port.type == PortType::Gate); });
        if (discrete) { spin->setDecimals(0); spin->setSingleStep(1); }
        spin->setValue(value * factor);
        spin->setDefaultValue(p.initial * factor);
        spin->setLogarithmic(p.logarithmic);
        spin->setKeyboardTracking(false);
        spin->setFrame(false);
        if (!p.unit.empty())
          spin->setSuffix(" " + QString::fromStdString(p.unit));
        widget = spin;
        spin->setProperty("creatorFactor", factor);
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, canvas,
                         [canvas, n, p, spin, factor] {
                           emit canvas->parameterEdited(
                               QString::fromStdString(n.id),
                               QString::fromStdString(p.id),
                               spin->value() / factor);
                         });
      }
      styleCreator(widget);
      widget->setFixedSize(108, 24);
      widget->setAccessibleName(creatorText(desc->name) + " " +
                                creatorText(p.name));
      widget->setToolTip(
          connected
              ? CreatorCanvas::tr("Controlled by a connection; disconnect to "
                                  "restore the manual value")
              : creatorText(p.name) + "\n" + CreatorCanvas::tr(
                    "Drag vertically · Shift: fine · Double-click: type · Ctrl-click: reset"));
      widget->setEnabled(!connected);
      widget->setProperty("creatorParameter", int(index));
      auto *proxy = new QGraphicsProxyWidget(this);
      proxy->setWidget(widget);
      proxy->setPos(width - 120, row - 12);
    };
    for (const auto &port : inputs) {
      auto *item = new Port(QString::fromStdString(n.id), port, false, this);
      item->setPos(0, y);
      item->connected = std::any_of(
          graph.connections.begin(), graph.connections.end(),
          [&](const auto &e) { return e.to == n.id && e.toPort == port.id; });
      labels.push_back(
          {QRectF(12, y - 10, port.parameter >= 0 ? 100 : width - 24, 20),
           creatorText(port.name), false});
      if (port.parameter >= 0)
        control(desc->parameters[port.parameter], unsigned(port.parameter), y,
                item->connected);
      y += 28;
    }
    for (unsigned i = 0; i < desc->parameters.size(); ++i)
      if (!desc->parameters[i].modulatable) {
        labels.push_back({QRectF(12, y - 10, 100, 20),
                          creatorText(desc->parameters[i].name), false});
        control(desc->parameters[i], i, y, false);
        y += 28;
      }
    if (n.type == "interface") {
      labels.push_back({QRectF(16, y, width - 32, 24),
                        CreatorCanvas::tr("Select to edit the module card"),
                        false});
      y += 34;
    }
    if (n.function) {
      const bool current = n.function->analyzedHash == functionSourceHash(*n.function);
      codeLabel=int(labels.size());
      labels.push_back({QRectF(16, y, width - 32, 24),
                        current ? CreatorCanvas::tr("Ports up to date") : CreatorCanvas::tr("Update ports after editing"), false});
      y += 28;
      auto *button = new QPushButton(CreatorCanvas::tr("Edit C++"));
      styleCreator(button);
      button->setIcon(icons::icon(icons::Glyph::Edit, creatorColors().text));
      button->setFixedSize(int(width - 32), 32);
      button->setAccessibleName(CreatorCanvas::tr("Edit C++ function") + " " + QString::fromStdString(n.function->entry));
      auto *proxy = new QGraphicsProxyWidget(this);
      proxy->setWidget(button); proxy->setPos(16, y);
      QObject::connect(button, &QPushButton::clicked, canvas, [canvas, id = n.id] {
        emit canvas->codeRequested(QString::fromStdString(id));
      });
      y += 40;
    }
    height = std::max(96., y + 10);
  }
  QRectF boundingRect() const override {
    return {-1, -1, width + 2, height + 2};
  }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *,
             QWidget *) override {
    p->setRenderHint(QPainter::Antialiasing);
    const auto c = creatorColors();
    p->setBrush(c.card);
    p->setPen(QPen(data(20).toBool() ? QColor(217,87,91) : isSelected() ? c.accent : c.border,
                   data(20).toBool() || isSelected() ? 2 : 1));
    p->drawRoundedRect(QRectF(0, 0, width, height), 9, 9);
    p->setPen(QPen(c.border, 1));
    p->drawLine(QPointF(12, 34), QPointF(width - 12, 34));
    icons::paint(*p, creatorCategoryIcon(desc ? desc->category : "Code"),
                 QRectF(12, 9, 16, 16), c.accent);
    p->setPen(c.text);
    QFont title = p->font();
    title.setPixelSize(12);
    title.setWeight(QFont::DemiBold);
    p->setFont(title);
    p->drawText(QRectF(36, 1, width - 48, 32), Qt::AlignVCenter,
                p->fontMetrics().elidedText(creatorText(desc ? desc->name : model.type),
                                            Qt::ElideRight, int(width - 48)));
    p->setPen(c.muted);
    QFont body = p->font();
    body.setPixelSize(12);
    body.setWeight(QFont::Normal);
    p->setFont(body);
    for (const auto &label : labels)
      p->drawText(label.rect,
                  Qt::AlignVCenter |
                      (label.right ? Qt::AlignRight : Qt::AlignLeft),
                  p->fontMetrics().elidedText(label.text, Qt::ElideRight,
                                              int(label.rect.width())));
  }
  QVariant itemChange(GraphicsItemChange change,
                      const QVariant &value) override {
    if (change == ItemPositionHasChanged && scene())
      canvas->updateWires();
    return QGraphicsObject::itemChange(change, value);
  }
  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
    if (!model.subgraph.empty()) emit canvas->enterRequested(QString::fromStdString(model.id));
    else QGraphicsObject::mouseDoubleClickEvent(event);
  }
  NodeDefinition model;
  CreatorCanvas *canvas;
  NodeDescription description;
  const NodeDescription *desc;
  struct Label {
    QRectF rect;
    QString text;
    bool right;
  };
  std::vector<Label> labels;
  static constexpr double width = 240;
  double height = 86;
  int codeLabel=-1;
};
class Wire final : public QGraphicsPathItem {
public:
  Wire(Port *from, Port *to, unsigned index) : from(from), to(to) {
    setData(wireRole, index);
    setFlag(ItemIsSelectable);
    setZValue(-1);
    setToolTip(from->title + " → " + to->title + "\n" +
               CreatorCanvas::tr("Double-click to disconnect"));
    // Keep the scene's broad-phase bounds as wide as the clickable stroke.
    setPen(QPen(Qt::transparent, 12));
    updatePath();
  }
  void updatePath() { setPath(cable(from->scenePos(), to->scenePos())); }
  QPainterPath shape() const override {
    QPainterPathStroker s;
    s.setWidth(12);
    return s.createStroke(path());
  }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *,
             QWidget *) override {
    p->setRenderHint(QPainter::Antialiasing);
    const bool valid=from->matches(*to);
    p->setPen(QPen(!valid || data(20).toBool() ? QColor(217,87,91):isSelected() ? creatorColors().text : portColor(from->kind),
                   isSelected() ? 3 : 2, from->kind==PortType::Function || !valid?Qt::DashLine:Qt::SolidLine));
    p->setBrush(Qt::NoBrush);
    p->drawPath(path());
  }
  Port *from, *to;
};
Port *portAt(CreatorCanvas *canvas, QPoint point) {
  // Hit slop is measured in viewport logical pixels, independently of zoom.
  Port *nearest = nullptr;
  double distance = 12.01;
  for (auto *item : canvas->items(QRect(point - QPoint(12, 12), QSize(25, 25))))
    if (auto *port = dynamic_cast<Port *>(item)) {
      const double candidate = QLineF(point, canvas->mapFromScene(port->scenePos())).length();
      if (candidate < distance) { nearest = port; distance = candidate; }
    }
  return nearest;
}
} // namespace
CreatorCanvas::CreatorCanvas(QWidget *parent) : QGraphicsView(parent) {
  setScene(new QGraphicsScene(this));
  setObjectName("CreatorCanvas");
  setAccessibleName(tr("Creator node graph"));
  setAccessibleDescription(
      tr("Drag nodes from the library. Tab adds a node. Drag ports to connect. "
         "Double-click a wire to disconnect. Delete removes selection. "
         "Control plus wheel zooms. Space plus drag pans."));
  setSceneRect(-16000, -16000, 32000, 32000);
  setRenderHint(QPainter::Antialiasing);
  setFrameShape(QFrame::NoFrame);
  setDragMode(RubberBandDrag);
  setRubberBandSelectionMode(Qt::IntersectsItemShape);
  setTransformationAnchor(NoAnchor);
  setViewportUpdateMode(BoundingRectViewportUpdate);
  setMouseTracking(true);
  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);
  connect(scene(), &QGraphicsScene::selectionChanged, this, [this] {
    const auto ids = selectedNodes();
    emit nodeSelected(ids.size() == 1 ? ids.front() : QString{});
  });
  connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
    for (auto *item : scene()->items()) {
      if (auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(item)) {
        styleCreator(proxy->widget());
        if (auto *button = qobject_cast<QPushButton *>(proxy->widget()))
          button->setIcon(icons::icon(icons::Glyph::Edit, creatorColors().text));
      }
      item->update(); // invalidate each cached node body as well as the view
    }
    scene()->invalidate();
    viewport()->update();
  });
}
CreatorCanvas::~CreatorCanvas() {
  // Removing selected items emits selectionChanged. Tear down the scene while
  // the derived view is alive, without calling an already destroyed inspector.
  scene()->blockSignals(true);
  cancelConnection();
  scene()->clear();
}
void CreatorCanvas::updateValues(const MiniModuleDefinition &graph) {
  m_graph = graph;
  for (auto *item : scene()->items()) if (auto *node = dynamic_cast<NodeItem *>(item)) {
    auto model = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto &n) { return n.id == node->model.id; });
    if (model == graph.nodes.end()) continue;
    node->model.parameters = model->parameters;
    for (auto *child : node->childItems()) if (auto *proxy = dynamic_cast<QGraphicsProxyWidget *>(child)) {
      auto *widget = proxy->widget();
      if (!widget || !widget->property("creatorParameter").isValid()) continue;
      const auto index = widget->property("creatorParameter").toUInt();
      if (index >= node->description.parameters.size()) continue;
      const auto &parameter = node->description.parameters[index];
      auto value = parameter.initial;
      for (const auto &p : model->parameters) if (p.id == parameter.id) value = p.value;
      QSignalBlocker block(widget);
      if (auto *spin = qobject_cast<CreatorNumberField *>(widget)) spin->setValue(value * spin->property("creatorFactor").toDouble());
      else if (auto *combo = qobject_cast<QComboBox *>(widget)) combo->setCurrentIndex(int(value - parameter.minimum));
    }
  }
}
void CreatorCanvas::setGraph(const MiniModuleDefinition &graph,
                             const QMap<QString, QPointF> &positions) {
  const auto selected = selectedNodes();
  cancelConnection();
  QSignalBlocker blocker(scene());
  scene()->clear();
  m_graph = graph;
  QMap<QString, Port *> ports;
  unsigned index = 0;
  for (const auto &node : graph.nodes) {
    auto *item = new NodeItem(node, graph, this);
    scene()->addItem(item);
    const auto id = QString::fromStdString(node.id);
    item->setPos(positions.value(
        id, {60. + (index % 3) * 300, 60. + (index / 3) * 240}));
    item->setSelected(selected.contains(id));
    ++index;
    for (auto *child : item->childItems())
      if (auto *port = dynamic_cast<Port *>(child))
        ports[id + (port->output ? "/out/" : "/in/") + port->id] = port;
  }
  for (unsigned i = 0; i < graph.connections.size(); ++i) {
    const auto &e = graph.connections[i];
    auto *a =
        ports.value(QString::fromStdString(e.from + "/out/" + e.fromPort));
    auto *b = ports.value(QString::fromStdString(e.to + "/in/" + e.toPort));
    if (a && b) {
      a->connected = b->connected = true;
      scene()->addItem(new Wire(a, b, i));
    }
  }
}
void CreatorCanvas::updateCodeStatus(const MiniModuleDefinition &graph) {
  for(auto *item:scene()->items())if(auto *node=dynamic_cast<NodeItem *>(item);node && node->codeLabel>=0)
    for(const auto &n:graph.nodes)if(n.id==node->model.id && n.function) {
      node->model=n;
      node->labels[node->codeLabel].text=n.function->analyzedHash==functionSourceHash(*n.function)?tr("Ports up to date"):tr("Update ports after editing");node->update();
    }
}
QMap<QString, QPointF> CreatorCanvas::nodePositions() const {
  QMap<QString, QPointF> result;
  for (auto *item : scene()->items())
    if (item->data(nodeRole).isValid())
      result[item->data(nodeRole).toString()] = item->pos();
  return result;
}
QStringList CreatorCanvas::selectedNodes() const {
  QStringList ids;
  for (auto *item : scene()->selectedItems())
    if (item->data(nodeRole).isValid())
      ids << item->data(nodeRole).toString();
  return ids;
}
std::vector<unsigned> CreatorCanvas::selectedConnections() const {
  std::vector<unsigned> indices;
  for (auto *item : scene()->selectedItems())
    if (item->data(wireRole).isValid())
      indices.push_back(item->data(wireRole).toUInt());
  return indices;
}
void CreatorCanvas::selectNode(const QString &id) {
  scene()->clearSelection();
  for (auto *item : scene()->items())
    if (item->data(nodeRole).toString() == id &&
        item->data(nodeRole).isValid()) {
      item->setSelected(true);
      ensureVisible(item, 40, 40);
      break;
    }
}
void CreatorCanvas::highlightConnectionPath(const QString &from, const QString &to, bool includeMemory) {
  QMap<QString, QString> parent;
  QStringList pending{to}; parent[to] = {};
  for (qsizetype i = 0; i < pending.size() && !parent.contains(from); ++i)
    for (const auto &edge : m_graph.connections) if (QString::fromStdString(edge.from) == pending[i]) {
      const auto target = QString::fromStdString(edge.to);
      const auto node = std::find_if(m_graph.nodes.begin(), m_graph.nodes.end(), [&](const auto &n) { return n.id == edge.to; });
      if (!includeMemory && node != m_graph.nodes.end() && isMemoryWrite(*node, edge.toPort)) continue;
      if (!parent.contains(target)) { parent[target] = pending[i]; pending.push_back(target); }
    }
  QStringList path{from, to};
  if (parent.contains(from)) for (auto at = from; at != to && !at.isEmpty(); at = parent.value(at)) path.push_back(at);
  for (auto *item : scene()->items()) {
    bool highlighted = item->data(nodeRole).isValid() && path.contains(item->data(nodeRole).toString());
    if (auto *wire = dynamic_cast<Wire *>(item)) highlighted = path.contains(wire->from->node) && path.contains(wire->to->node);
    item->setData(20, highlighted); item->update();
  }
}
CreatorViewport CreatorCanvas::viewportState() const {
  return {mapToScene(viewport()->rect().center()), transform().m11()};
}
void CreatorCanvas::restoreViewport(const CreatorViewport &view) {
  resetTransform();
  scale(view.zoom, view.zoom);
  centerOn(view.center);
  emit zoomChanged(transform().m11());
}
void CreatorCanvas::fitGraph() {
  fitInView(scene()->itemsBoundingRect().adjusted(-50, -50, 50, 50),
            Qt::KeepAspectRatio);
  const double zoom = transform().m11();
  if (zoom > 1)
    scale(1 / zoom, 1 / zoom);
  emit zoomChanged(transform().m11());
}
void CreatorCanvas::actualSize() {
  const auto center = viewportState().center;
  resetTransform();
  centerOn(center);
  emit zoomChanged(1);
}
void CreatorCanvas::updateWires() {
  for (auto *item : scene()->items())
    if (auto *wire = dynamic_cast<Wire *>(item))
      wire->updatePath();
}
void CreatorCanvas::drawBackground(QPainter *p, const QRectF &r) {
  const auto c = creatorColors();
  p->fillRect(r, c.canvas);
  if (transform().m11() < .4)
    return;
  p->setPen(QPen(c.grid, 1));
  const double step = 24;
  for (double y = std::floor(r.top() / step) * step; y < r.bottom(); y += step)
    for (double x = std::floor(r.left() / step) * step; x < r.right();
         x += step)
      p->drawPoint(QPointF(x, y));
}
void CreatorCanvas::cancelConnection() {
  delete m_wire;
  m_wire = nullptr;
  m_port = nullptr;
  for (auto *item : scene()->items())
    if (auto *port = dynamic_cast<Port *>(item)) {
      port->compatible = port->dimmed = false;
      port->update();
    }
}
void CreatorCanvas::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::MiddleButton ||
      (m_space && event->button() == Qt::LeftButton)) {
    m_panning = true;
    m_panAt = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton)
    if (auto *port = portAt(this, event->pos())) {
      cancelConnection();
      m_port = port;
      m_wire = scene()->addPath(cable(port->scenePos(), port->scenePos()),
                                QPen(portColor(port->kind), 2));
      m_wire->setZValue(20);
      for (auto *item : scene()->items())
        if (auto *other = dynamic_cast<Port *>(item)) {
          other->compatible = other->matches(*port) &&
                              other->output != port->output;
          other->dimmed = !other->compatible && other != port;
          other->update();
        }
      event->accept();
      return;
    }
  m_beforeMove = nodePositions();
  QGraphicsView::mousePressEvent(event);
}
void CreatorCanvas::mouseMoveEvent(QMouseEvent *event) {
  if (m_panning) {
    const QPoint delta = event->pos() - m_panAt;
    m_panAt = event->pos();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
    return;
  }
  if (auto *port = static_cast<Port *>(m_port)) {
    m_wire->setPath(port->output
                        ? cable(port->scenePos(), mapToScene(event->pos()))
                        : cable(mapToScene(event->pos()), port->scenePos()));
    return;
  }
  QGraphicsView::mouseMoveEvent(event);
}
void CreatorCanvas::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && !m_space && !m_panning &&
      !portAt(this, event->pos())) {
    // Only the top item may react: a wire behind a node must stay untouched.
    if (auto *wire = dynamic_cast<Wire *>(itemAt(event->pos()))) {
      const auto index = wire->data(wireRole).toUInt();
      cancelConnection();
      emit disconnectWire(index);
      event->accept();
      return;
    }
  }
  QGraphicsView::mouseDoubleClickEvent(event);
}
QString CreatorCanvas::draggedNode(const QMimeData *mime) const {
  if (!mime || !mime->hasFormat(kCreatorNodeMimeType)) return {};
  const auto type = QString::fromUtf8(mime->data(kCreatorNodeMimeType));
  if (m_libraryNodes.contains(type)) return type;
  for (const auto &node : nodeRegistry())
    if (type == QString::fromStdString(node.id) && node.id != "wire" &&
        node.id != "subgraph" && !node.id.starts_with("subgraph_")) return type;
  return {};
}
void CreatorCanvas::dragEnterEvent(QDragEnterEvent *event) {
  dragMoveEvent(event);
}
void CreatorCanvas::dragMoveEvent(QDragMoveEvent *event) {
  if (!draggedNode(event->mimeData()).isEmpty() &&
      event->possibleActions().testFlag(Qt::CopyAction)) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
  } else event->ignore();
}
void CreatorCanvas::dropEvent(QDropEvent *event) {
  const auto type = draggedNode(event->mimeData());
  if (type.isEmpty() || !event->possibleActions().testFlag(Qt::CopyAction)) {
    event->ignore();
    return;
  }
  cancelConnection();
  event->setDropAction(Qt::CopyAction);
  event->accept();
  setFocus(Qt::MouseFocusReason);
  emit addNodeRequested(type, mapToScene(event->position().toPoint()));
}
void CreatorCanvas::mouseReleaseEvent(QMouseEvent *event) {
  if (m_panning) {
    m_panning = false;
    unsetCursor();
    event->accept();
    return;
  }
  if (auto *from = static_cast<Port *>(m_port)) {
    auto *to = portAt(this, event->pos());
    const bool valid = to && to->compatible;
    QString source, sourcePort, target, targetPort;
    if (valid) {
      if (!from->output)
        std::swap(from, to);
      source = from->node;
      sourcePort = from->id;
      target = to->node;
      targetPort = to->id;
    }
    cancelConnection();
    if (valid)
      emit connectPorts(source, sourcePort, target, targetPort);
    else
      emit status(tr("Connect an output to an input of the same type."));
    event->accept();
    return;
  }
  QGraphicsView::mouseReleaseEvent(event);
  if (m_beforeMove != nodePositions())
    emit nodesMoved();
}
void CreatorCanvas::wheelEvent(QWheelEvent *event) {
  if (!(event->modifiers() & Qt::ControlModifier)) {
    QGraphicsView::wheelEvent(event);
    return;
  }
  const auto anchor = mapToScene(event->position().toPoint());
  const double old = transform().m11(),
               zoom = std::clamp(
                   old * std::pow(1.0015, event->angleDelta().y()), .25, 2.5);
  scale(zoom / old, zoom / old);
  const auto after = mapToScene(event->position().toPoint());
  centerOn(viewportState().center + anchor - after);
  emit zoomChanged(zoom);
  event->accept();
}
void CreatorCanvas::keyPressEvent(QKeyEvent *event) {
  // Native editors own their shortcuts, including Backspace/Delete and text
  // selection.
  if (dynamic_cast<QGraphicsProxyWidget *>(scene()->focusItem())) {
    QGraphicsView::keyPressEvent(event);
    return;
  }
  if (event->key() == Qt::Key_Space) {
    m_space = true;
    event->accept();
  } else if (event->key() == Qt::Key_Tab) {
    emit addRequested(viewportState().center);
    event->accept();
  } else if (event->key() == Qt::Key_Delete ||
             event->key() == Qt::Key_Backspace)
    emit deleteRequested();
  else if (event->key() == Qt::Key_Escape) {
    cancelConnection();
    scene()->clearSelection();
  } else if (event->matches(QKeySequence::Copy))
    emit copyRequested();
  else if (event->matches(QKeySequence::Paste))
    emit pasteRequested(viewportState().center);
  else if (event->key() == Qt::Key_D &&
           event->modifiers() & Qt::ControlModifier)
    emit duplicateRequested();
  else if (event->key() == Qt::Key_F)
    fitGraph();
  else if (event->key() == Qt::Key_0 &&
           event->modifiers() & Qt::ControlModifier)
    actualSize();
  else if (event->matches(QKeySequence::SelectAll)) {
    for (auto *item : scene()->items())
      if (item->data(nodeRole).isValid())
        item->setSelected(true);
  } else
    QGraphicsView::keyPressEvent(event);
}
bool CreatorCanvas::focusNextPrevChild(bool next) {
  // QWidget normally consumes Tab before keyPressEvent. Reserve it for the
  // canvas command, while native parameter editors retain normal traversal.
  if (next && !dynamic_cast<QGraphicsProxyWidget *>(scene()->focusItem()))
    return false;
  return QGraphicsView::focusNextPrevChild(next);
}
void CreatorCanvas::keyReleaseEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Space)
    m_space = false;
  QGraphicsView::keyReleaseEvent(event);
}
void CreatorCanvas::contextMenuEvent(QContextMenuEvent *event) {
  QMenu menu(this);
  menu.setObjectName("CreatorCanvasMenu");
  if (auto *port = portAt(this, event->pos())) {
    menu.addSection(port->title + " · " +
                    QString::fromLatin1(portTypeName(port->kind)));
    for (auto *item : scene()->items())
      if (auto *other = dynamic_cast<Port *>(item)) {
        if (!other->matches(*port) || other->output == port->output)
          continue;
        auto *action = menu.addAction(other->parentItem()->toolTip() + " / " +
                                      other->title);
        connect(action, &QAction::triggered, this, [this, port, other] {
          auto *a = port->output ? port : other,
               *b = port->output ? other : port;
          emit connectPorts(a->node, a->id, b->node, b->id);
        });
      }
    if (port->connected) {
      menu.addSeparator();
      auto *disconnect = menu.addAction(tr("Disconnect"));
      connect(disconnect, &QAction::triggered, this, [this, port] {
        for (unsigned i = 0; i < m_graph.connections.size(); ++i) {
          const auto &e = m_graph.connections[i];
          if ((!port->output && e.to == port->node.toStdString() &&
               e.toPort == port->id.toStdString()) ||
              (port->output && e.from == port->node.toStdString() &&
               e.fromPort == port->id.toStdString())) {
            emit disconnectWire(i);
            break;
          }
        }
      });
    }
  } else {
    const auto position = mapToScene(event->pos());
    auto *add = menu.addAction(icons::icon(icons::Glyph::Search, creatorColors().text), tr("Search nodes…"));
    add->setShortcut(QKeySequence(Qt::Key_Tab));
    connect(
        add, &QAction::triggered, this,
        [this, position] { emit addRequested(position); }, Qt::QueuedConnection);
    menu.addSeparator();
    std::vector<const NodeDescription *> types;
    for (const auto &type : nodeRegistry()) if (type.id != "wire" && type.id != "subgraph" && !type.id.starts_with("subgraph_")) types.push_back(&type);
    std::stable_sort(types.begin(), types.end(), [](auto *a, auto *b) {
      return creatorCategoryOrder(a->category) < creatorCategoryOrder(b->category);
    });
    QMenu *category = nullptr;
    std::string last;
    for (const auto *type : types) {
      if (!category || last != type->category) {
        last = type->category;
        category = menu.addMenu(icons::icon(creatorCategoryIcon(last), creatorColors().accent), creatorText(last));
      }
      auto *action = category->addAction(creatorText(type->name));
      action->setData(QString::fromStdString(type->id));
      connect(action, &QAction::triggered, this, [this, id = type->id, position] {
        emit addNodeRequested(QString::fromStdString(id), position);
      }, Qt::QueuedConnection);
    }
    auto *personal = menu.addMenu(tr("My nodes"));
    for (auto it = m_libraryNodes.begin(); it != m_libraryNodes.end(); ++it)
      connect(personal->addAction(it.value()), &QAction::triggered, this, [this, type = it.key(), position] { emit addNodeRequested(type, position); }, Qt::QueuedConnection);
    personal->addSeparator();
    personal->addAction(tr("Import node…"), this, &CreatorCanvas::importNodeRequested);
    if (!selectedNodes().empty() || !selectedConnections().empty()) {
      menu.addSeparator();
      menu.addAction(tr("Create node from selection…"), this, &CreatorCanvas::groupRequested);
      if (selectedNodes().size() == 1) {
        const auto id = selectedNodes().front().toStdString();
        auto selected = std::find_if(m_graph.nodes.begin(), m_graph.nodes.end(), [&](const auto &n) { return n.id == id; });
        if (selected != m_graph.nodes.end() && selected->type == "subgraph") {
          menu.addAction(tr("Open node"), this, [this, id] { emit enterRequested(QString::fromStdString(id)); });
          menu.addAction(tr("Expand into graph"), this, &CreatorCanvas::unpackRequested);
          menu.addAction(tr("Make independent"), this, &CreatorCanvas::independentRequested);
          menu.addAction(tr("Export node…"), this, &CreatorCanvas::exportNodeRequested);
        }
      }
      menu.addAction(tr("Duplicate"), this, &CreatorCanvas::duplicateRequested);
      menu.addAction(tr("Delete"), this, &CreatorCanvas::deleteRequested);
    }
    menu.addSeparator();
    menu.addAction(tr("Fit graph"), this, &CreatorCanvas::fitGraph);
    menu.addAction(tr("100%"), this, &CreatorCanvas::actualSize);
  }
  menu.exec(event->globalPos());
}
} // namespace ui
