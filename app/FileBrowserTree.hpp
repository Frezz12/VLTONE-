#pragma once

class QMenu;

#include <QColor>
#include <QHash>
#include <QSet>
#include <memory>
#include <QStringList>
#include <QTreeWidget>
#include <QVector>

class QFileSystemWatcher;
namespace ui::browserprefs { struct Collection; }

/// The browser's file tree: the folders the user chose, and what is inside them.
///
/// A `QTreeWidget` filled by hand rather than a `QFileSystemModel`, for two
/// reasons: the roots are several unrelated folders (a file-system model has
/// exactly one), and the rows carry meaning of their own — what can be dragged
/// into the project, and what is only listed. Directories are read on expand;
/// enumeration runs off the GUI thread, and children arrive in bounded batches.
/// A folder nobody opened is never touched.
class FileBrowserTree : public QTreeWidget {
    Q_OBJECT
public:
    /// Open the selected item's existing right-click actions.
    bool showSelectedItemActionsMenu();
    bool populateSelectedItemActionsMenu(QMenu& menu);
    /// What a row is, which decides how it is drawn and whether it can be
    /// dragged anywhere.
    ///
    /// `PluginGroup` and `Plugin` are not on disk at all: the browser lists the
    /// scanned plugins beside the sample folders, because "find the thing and
    /// drag it where it goes" is one gesture and it should not depend on
    /// whether the thing is a file.
    enum class Kind {
        Folder,
        Audio,
        Midi,
        ChannelStripPreset,
        ProjectTemplate,
        Other,
        Collection,
        PluginGroup,
        Plugin
    };

    /// One scanned plugin, as much of it as a row needs.
    struct PluginEntry {
        QString name;
        QString vendor;
        QString uid;
        QString path;
        int format = 0;            ///< daw::plugins::Format
        QString formatName;        ///< the folder it is filed under
        bool instrument = false;

        friend bool operator==(const PluginEntry&, const PluginEntry&) = default;
    };

    explicit FileBrowserTree(QWidget* parent = nullptr);

    /// Replace the roots. Expanded folders that still exist stay open.
    void setRoots(const QStringList& folders);
    /// The application-managed preset folder, displayed as the synthetic
    /// top-level "Presets" root before user-added sample folders.
    void setPresetRoot(const QString& folder);
    /// Replace the plugin listing. Grouped into one folder per format under a
    /// single "Plugins" root, which is dropped entirely when the list is empty
    /// — an empty folder that can never fill up is worse than no folder.
    void setPlugins(const QVector<PluginEntry>& plugins);
    /// Empty selects the permanent all-folders view; otherwise show one
    /// virtual collection as an icon-tab destination.
    void setCollectionFilter(const QString& collectionId);
    QString collectionFilter() const { return m_collectionFilter; }
    /// Re-read every open folder from disk.
    void refresh();

    /// Show a flat list of results instead of the tree. The hierarchy and what
    /// was open in it are remembered, so clearing the search puts it back.
    int showResults(const QStringList& paths, bool truncated,
                    const QString& query = {}, bool searching = false);
    /// Return to the folder hierarchy.
    void showTree();
    bool showingResults() const { return m_showingResults; }

    /// Headless check only: open the plugin folders and select the first
    /// plugin row, so a grab shows the listing and a check can read the drag
    /// payload it would carry. Returns false when nothing is listed.
    bool selectFirstPluginForTest();
    /// How many plugin rows are listed, for the same check.
    int pluginRowCountForTest() const;
    /// Folder and plugin-group rows navigate the tree but never become a
    /// highlighted content selection.
    bool containersAreNavigationOnlyForTest() const;
    bool selectedProjectTemplateForTest() const;
    bool activateSelectedProjectTemplateForTest();

    /// The selected file, or empty when a folder (or nothing) is selected.
    QString selectedPath() const;

    /// The mime data a drag from the current selection would carry, or null
    /// when the selection cannot be dragged. The drag itself builds its payload
    /// through this, so a check of what a drop target would receive is a check
    /// of the real thing. Caller owns the result.
    QMimeData* dragPayload() const;

signals:
    /// A file row became the selection — the browser auditions from this.
    void fileSelected(const QString& path);
    /// A file row was double-clicked.
    void fileActivated(const QString& path);
    /// Enter on an audio row applies it to the current Pattern/MIDI target.
    void sampleLoadRequested(const QString& path);
    /// A `.vlts` row was double-clicked. It is applied to the selected channel,
    /// never sent to the audio preview path.
    void channelStripPresetActivated(const QString& path);
    /// A `.vltt` package was activated by double-click or Enter and should
    /// become a new, unsaved project.
    void projectTemplateActivated(const QString& path);
    /// Keyboard/context-menu alternative to dragging a template into the
    /// arrangement: append its tracks to the current project.
    void projectTemplateTracksRequested(const QString& path);
    /// A collection, its contents or a folder colour changed. The panel uses
    /// this to rebuild the icon tabs while the tree refreshes itself.
    void organizationChanged();
    /// A collection row asked to become an icon tab. The panel owns the icon
    /// chooser because the tab strip lives there.
    void tabRequested(const QString& collectionId);
    /// A physical folder should get its own icon tab, backed by a collection.
    void folderTabRequested(const QString& path);
    /// Something worth saying in the status bar (an unreadable folder, a
    /// capped search).
    void statusMessage(const QString& text);

protected:
    /// Drags carry file URLs, which is exactly what the sampler, the timeline
    /// and the instrument slot already accept from the desktop — so the browser
    /// needs no private drag format and those targets need no new code.
    void startDrag(Qt::DropActions supportedActions) override;
    void keyPressEvent(class QKeyEvent* event) override;
    void contextMenuEvent(class QContextMenuEvent* event) override;
    void drawBranches(class QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;

private:
    void populateItemActionsMenu(QMenu& menu, QTreeWidgetItem* item);
    void populate(QTreeWidgetItem* parent, const QString& path);
    /// Fill a node the first time it is opened (it carries a placeholder child
    /// until then, which is what draws the expander arrow).
    void expandNode(QTreeWidgetItem* item);
    void collapseNode(QTreeWidgetItem* item);
    /// Re-read one directory in place, keeping selection and open children.
    void reloadNode(QTreeWidgetItem* item);
    QTreeWidgetItem* makeItem(const QString& path, bool isDirectory,
                              int cachedKind = -1,
                              const QColor& inheritedFolderColor = {});
    QColor folderTint(const QString& directory);
    struct DirectoryResult;
    void applyDirectoryChunk(const std::shared_ptr<DirectoryResult>& result);
    quint64 m_loadSerial = 0;
    QSet<QString> m_restoreExpanded;
    QString m_restoreSelected;
    QSet<QString> m_changedDirectories;
    bool m_watchRefreshPending = false;
    /// Absolute paths of every expanded folder, so a rebuild can restore them.
    QStringList expandedPaths() const;
    void restoreExpanded(const QStringList& paths);
    void watch(const QString& path);
    void unwatch(const QString& path);

    /// Rebuild the whole tree: the plugin root, then the folder roots.
    void rebuildRoots();
    /// The synthetic plugin root, or null when there are no plugins.
    QTreeWidgetItem* buildPluginRoot(const QString& query = {});
    QTreeWidgetItem* buildCollectionRoot(
        const ui::browserprefs::Collection& collection);

    QStringList m_roots;
    QString m_presetRoot;
    QVector<PluginEntry> m_plugins;
    QString m_collectionFilter;
    QFileSystemWatcher* m_watcher = nullptr;
    /// Folders being watched, so the fd cost stays bounded and known.
    QStringList m_watched;
    /// Set while a flat result list is shown; the tree state is parked in
    /// `m_parkedExpanded` until the search is cleared.
    bool m_showingResults = false;
    QStringList m_parkedExpanded;
    QString m_resultQuery;
    QStringList m_resultPaths;
    int m_resultPluginCount = 0;
    QHash<QString, QColor> m_folderTintCache;
};
