#pragma once
#include "CreatorProject.hpp"
#include <QGraphicsView>
#include <QStringList>
#include <functional>

class QGraphicsPathItem;
class QMimeData;
namespace ui {
inline constexpr auto kCreatorNodeMimeType = "application/x-vltone-creator-node";
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
  void updateValues(const daw::plugins::mini::MiniModuleDefinition &);
  void highlightConnectionPath(const QString &from, const QString &to, bool includeMemory);
  void setLibraryNodes(const QMap<QString, QString> &nodes) { m_libraryNodes = nodes; }
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
  void groupRequested();
  void enterRequested(QString node);
  void unpackRequested();
  void independentRequested();
  void exportNodeRequested();
  void importNodeRequested();

protected:
  bool focusNextPrevChild(bool next) override;
  void drawBackground(QPainter *, const QRectF &) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void keyReleaseEvent(QKeyEvent *) override;
  void contextMenuEvent(QContextMenuEvent *) override;
  void dragEnterEvent(QDragEnterEvent *) override;
  void dragMoveEvent(QDragMoveEvent *) override;
  void dropEvent(QDropEvent *) override;

private:
  void cancelConnection();
  QString draggedNode(const QMimeData *) const;
  void *m_port = nullptr;
  QGraphicsPathItem *m_wire = nullptr;
  bool m_space = false, m_panning = false;
  QPoint m_panAt;
  QMap<QString, QPointF> m_beforeMove;
  daw::plugins::mini::MiniModuleDefinition m_graph;
  QMap<QString, QString> m_libraryNodes;
};
} // namespace ui
