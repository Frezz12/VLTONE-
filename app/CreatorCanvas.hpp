#pragma once
#include "CreatorProject.hpp"
#include <QGraphicsView>
#include <QStringList>
#include <functional>

class QGraphicsPathItem;
namespace ui {
class CreatorCanvas final : public QGraphicsView {
  Q_OBJECT
public:
  explicit CreatorCanvas(QWidget *parent = nullptr);
  ~CreatorCanvas() override;
  void setGraph(const daw::plugins::mini::MiniModuleDefinition &,
                const QMap<QString, QPointF> &positions);
  QMap<QString, QPointF> nodePositions() const;
  QStringList selectedNodes() const;
  std::vector<unsigned> selectedConnections() const;
  void selectNode(const QString &);
  CreatorViewport viewportState() const;
  void restoreViewport(const CreatorViewport &);
  void fitGraph();
  void actualSize();
  void updateWires();
  void updateCodeStatus(const daw::plugins::mini::MiniModuleDefinition &);
signals:
  void connectPorts(QString source, QString sourcePort, QString destination,
                    QString destinationPort);
  void disconnectWire(unsigned index);
  void nodesMoved();
  void parameterEdited(QString node, QString parameter, double value);
  void nodeSelected(QString node);
  void codeRequested(QString node);
  void addRequested(QPointF position);
  void addNodeRequested(QString type, QPointF position);
  void deleteRequested();
  void duplicateRequested();
  void copyRequested();
  void pasteRequested(QPointF position);
  void status(QString text);
  void zoomChanged(double zoom);

protected:
  bool focusNextPrevChild(bool next) override;
  void drawBackground(QPainter *, const QRectF &) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void keyReleaseEvent(QKeyEvent *) override;
  void contextMenuEvent(QContextMenuEvent *) override;

private:
  void cancelConnection();
  void *m_port = nullptr;
  QGraphicsPathItem *m_wire = nullptr;
  bool m_space = false, m_panning = false;
  QPoint m_panAt;
  QMap<QString, QPointF> m_beforeMove;
  daw::plugins::mini::MiniModuleDefinition m_graph;
};
} // namespace ui
