#include "CreatorCanvas.hpp"
#include "CreatorText.hpp"
#include "CreatorStyle.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "Creator/CodeUtilities.hpp"
#include "Theme.hpp"
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMenu>
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
      : model(n), canvas(canvas), description(describeNode(n)),
        desc(description.id.empty() ? nullptr : &description) {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges |
             ItemIsFocusable);
    setCacheMode(DeviceCoordinateCache);
    setData(nodeRole, QString::fromStdString(n.id));
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
    double y = 64;
    for (const auto &port : outputs) {
      auto *item = new Port(QString::fromStdString(n.id), port, true, this);
      item->setPos(width, y);
      labels.push_back(
          {QRectF(20, y - 10, width - 36, 20), creatorText(port.name), true});
      y += 32;
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
        spin->setValue(value * factor);
        spin->setDefaultValue(p.initial * factor);
        spin->setLogarithmic(p.logarithmic);
        spin->setKeyboardTracking(false);
        spin->setFrame(false);
        if (!p.unit.empty())
          spin->setSuffix(" " + QString::fromStdString(p.unit));
        widget = spin;
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, canvas,
                         [canvas, n, p, spin, factor] {
                           emit canvas->parameterEdited(
                               QString::fromStdString(n.id),
                               QString::fromStdString(p.id),
                               spin->value() / factor);
                         });
      }
      styleCreator(widget);
      widget->setFixedSize(128, 28);
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
      proxy->setPos(width - 144, row - 14);
    };
    for (const auto &port : inputs) {
      auto *item = new Port(QString::fromStdString(n.id), port, false, this);
      item->setPos(0, y);
      item->connected = std::any_of(
          graph.connections.begin(), graph.connections.end(),
          [&](const auto &e) { return e.to == n.id && e.toPort == port.id; });
      labels.push_back(
          {QRectF(16, y - 10, port.parameter >= 0 ? 108 : width - 32, 20),
           creatorText(port.name), false});
      if (port.parameter >= 0)
        control(desc->parameters[port.parameter], unsigned(port.parameter), y,
                item->connected);
      y += 32;
    }
    for (unsigned i = 0; i < desc->parameters.size(); ++i)
      if (!desc->parameters[i].modulatable) {
        labels.push_back({QRectF(16, y - 10, 108, 20),
                          creatorText(desc->parameters[i].name), false});
        control(desc->parameters[i], i, y, false);
        y += 32;
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
    p->setPen(QPen(isSelected() ? c.accent : c.border, isSelected() ? 2 : 1));
    p->drawRoundedRect(QRectF(0, 0, width, height), 9, 9);
    p->setPen(QPen(c.border, 1));
    p->drawLine(QPointF(16, 44), QPointF(width - 16, 44));
    icons::paint(*p, creatorCategoryIcon(desc ? desc->category : "Code"),
                 QRectF(15, 13, 18, 18), c.accent);
    p->setPen(c.text);
    QFont title = p->font();
    title.setPixelSize(13);
    title.setWeight(QFont::DemiBold);
    p->setFont(title);
    p->drawText(QRectF(43, 1, width - 59, 42), Qt::AlignVCenter,
                p->fontMetrics().elidedText(creatorText(desc ? desc->name : model.type),
                                            Qt::ElideRight, int(width - 59)));
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
  static constexpr double width = 280;
  double height = 86;
  int codeLabel=-1;
};
class Wire final : public QGraphicsPathItem {
public:
  Wire(Port *from, Port *to, unsigned index) : from(from), to(to) {
    setData(wireRole, index);
    setFlag(ItemIsSelectable);
    setZValue(-1);
    setToolTip(from->title + " → " + to->title);
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
    p->setPen(QPen(!valid?QColor(217,87,91):isSelected() ? creatorColors().text : portColor(from->kind),
                   isSelected() ? 3 : 2, from->kind==PortType::Function || !valid?Qt::DashLine:Qt::SolidLine));
    p->setBrush(Qt::NoBrush);
    p->drawPath(path());
  }
  Port *from, *to;
};
Port *portAt(CreatorCanvas *canvas, QPoint point) {
  for (auto *item : canvas->items(point))
    if (auto *port = dynamic_cast<Port *>(item))
      return port;
  return nullptr;
}
} // namespace
CreatorCanvas::CreatorCanvas(QWidget *parent) : QGraphicsView(parent) {
  setScene(new QGraphicsScene(this));
  setObjectName("CreatorCanvas");
  setAccessibleName(tr("Creator node graph"));
  setAccessibleDescription(
      tr("Tab adds a node. Drag ports to connect. Delete removes selection. "
         "Control plus wheel zooms. Space plus drag pans."));
  setSceneRect(-16000, -16000, 32000, 32000);
  setRenderHint(QPainter::Antialiasing);
  setFrameShape(QFrame::NoFrame);
  setDragMode(RubberBandDrag);
  setRubberBandSelectionMode(Qt::IntersectsItemShape);
  setTransformationAnchor(NoAnchor);
  setViewportUpdateMode(BoundingRectViewportUpdate);
  setMouseTracking(true);
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
                              other->output != port->output &&
                              other->node != port->node;
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
        if (!other->matches(*port) || other->output == port->output ||
            other->node == port->node)
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
    for (const auto &type : nodeRegistry()) types.push_back(&type);
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
    if (!selectedNodes().empty() || !selectedConnections().empty()) {
      menu.addSeparator();
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
