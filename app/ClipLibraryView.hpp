#pragma once
#include <QListWidget>
#include <QSet>
namespace daw { class EngineController; struct ClipLibraryEntry; }
class PreviewLoader;
class QMenu;

/// Project-owned snapshots displayed as musical cards, never filename rows.
class ClipLibraryView : public QListWidget {
    Q_OBJECT
public:
    explicit ClipLibraryView(daw::EngineController* controller, QWidget* parent = nullptr);
    void refresh();
    void setFilter(const QString& query);
    void setZoom(double zoom);
    bool populateActions(QMenu& menu);
    bool showActions();
    void selectEntry(const QString& id);
    void removeSelected();
    const daw::ClipLibraryEntry* entry(const QModelIndex& index) const;
    daw::EngineController* controller() const { return m_controller; }
    double zoom() const { return m_zoom; }
signals:
    void restoreRequested(const QString& id, bool originalPosition);
    void projectEdited();
protected:
    bool event(QEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void showEvent(QShowEvent* event) override;
private:
    void queueWaveforms();
    daw::EngineController* m_controller;
    PreviewLoader* m_loader;
    QString m_filter, m_signature, m_loading;
    QSet<QString> m_requested;
    QPoint m_dragPressOffset{20, 20};
    double m_zoom = 1.0;
};
