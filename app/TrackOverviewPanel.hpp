#pragma once

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <array>

class QLabel;
class QLineEdit;
class QMenu;
class QTreeWidget;
class QTreeWidgetItem;
class QResizeEvent;
namespace daw { class EngineController; }

/// An independent, compact navigator for the document's entire track tree.
/// Expansion is local to this view; edits go through the controller/undo stack.
class TrackOverviewPanel final : public QWidget {
    Q_OBJECT
public:
    void setProjectController(daw::EngineController* controller) { m_controller = controller; m_structure.clear(); refresh(); }
    explicit TrackOverviewPanel(daw::EngineController* controller, QWidget* parent = nullptr);
    void refresh();
    void setSelectedTracks(const QStringList& ids, const QString& primary = {});
    QStringList selectedTrackIds() const;
    void populateMenu(QMenu& menu, bool includeDisplayOptions = false);
    bool moveSelection(const QString& parentId, size_t boundary);
    static bool checkForTest(const QString& screenshotDirectory = {});

signals:
    void selectionChanged(const QStringList& ids, const QString& primary);
    void edited(bool structure, bool localFileDirty);
    void revealRequested(const QString& id);
    void hideRequested();
    void moreActionsRequested(const QPoint& position);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyTheme();
    void filterTracks();
    void publishSelection();
    void toggleState(QTreeWidgetItem* item, int column);
    void applyState(QTreeWidgetItem* item, int column, bool on);
    void renameSelected();
    void packSelection(bool summing);
    void moveBy(int direction);
    bool canMove(const QStringList& ids, const QString& parentId) const;
    bool selectionEditable() const;
    size_t afterSubtree(const QString& id) const;
    QStringList actionTargets(QTreeWidgetItem* item) const;
    void updateSummary();
    void updateControlVisibility();
    void setControlVisible(int column, bool visible);
    void showSearch();

    daw::EngineController* m_controller;
    QTreeWidget* m_tree = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_count = nullptr;
    QLabel* m_summary = nullptr;
    std::array<bool, 3> m_visibleControls{true, true, true};
    QHash<QString, QTreeWidgetItem*> m_items;
    QSet<QString> m_collapsed;
    QStringList m_structure;
    bool m_refreshing = false;
    bool m_publishingSelection = false;
};
