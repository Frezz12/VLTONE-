#include "FileBrowserTree.hpp"

#include "ChannelStripPresets.hpp"
#include "BrowserPrefs.hpp"
#include "FileSearchWorker.hpp"
#include "FileTypes.hpp"
#include "ProjectTemplates.hpp"

#include <algorithm>
#include <utility>
#include "Icons.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QDir>
#include <QDrag>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QContextMenuEvent>
#include <QCursor>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QUrl>

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1;
/// Set on a node that has never been opened; its single child is a placeholder.
constexpr int kUnreadRole = Qt::UserRole + 2;
/// Set on a plugin row: its format (int) and its stable uid, which together
/// are the whole of what a drop target needs to find it again.
constexpr int kPluginFormatRole = Qt::UserRole + 3;
constexpr int kPluginUidRole = Qt::UserRole + 4;
constexpr int kCollectionRole = Qt::UserRole + 5;
constexpr int kCollectionMemberRole = Qt::UserRole + 6;
constexpr int kFolderTintRole = Qt::UserRole + 7;

/// One kqueue file descriptor per watched directory on macOS, so the set is
/// capped. Past this the Refresh button is the way back to the truth.
constexpr int kMaxWatched = 200;

FileBrowserTree::Kind kindOf(const QFileInfo& info) {
    if (ui::projecttemplates::isTemplatePackage(info.filePath()))
        return FileBrowserTree::Kind::ProjectTemplate;
    if (info.isDir()) return FileBrowserTree::Kind::Folder;
    const QString path = info.filePath();
    if (ui::isAudioFile(path)) return FileBrowserTree::Kind::Audio;
    if (ui::isMidiFile(path)) return FileBrowserTree::Kind::Midi;
    if (ui::channelstrippresets::isPresetFile(path))
        return FileBrowserTree::Kind::ChannelStripPreset;
    return FileBrowserTree::Kind::Other;
}

icons::Glyph glyphFor(FileBrowserTree::Kind kind) {
    switch (kind) {
        case FileBrowserTree::Kind::Folder: return icons::Glyph::Folder;
        case FileBrowserTree::Kind::Audio:  return icons::Glyph::Waveform;
        case FileBrowserTree::Kind::Midi:   return icons::Glyph::MidiKeys;
        case FileBrowserTree::Kind::ChannelStripPreset: return icons::Glyph::Save;
        case FileBrowserTree::Kind::ProjectTemplate: return icons::Glyph::Layers;
        case FileBrowserTree::Kind::Collection: return icons::Glyph::Folder;
        case FileBrowserTree::Kind::PluginGroup: return icons::Glyph::Folder;
        case FileBrowserTree::Kind::Plugin: return icons::Glyph::Plugin;
        case FileBrowserTree::Kind::Other:  break;
    }
    return icons::Glyph::Import;
}

QColor tintFor(FileBrowserTree::Kind kind) {
    switch (kind) {
        case FileBrowserTree::Kind::Audio: return Theme::audioAccent();
        case FileBrowserTree::Kind::Midi: return Theme::midiAccent();
        case FileBrowserTree::Kind::ChannelStripPreset:
            return Theme::automationAccent();
        case FileBrowserTree::Kind::Plugin:
        case FileBrowserTree::Kind::ProjectTemplate: return th().accent;
        case FileBrowserTree::Kind::Folder:
        case FileBrowserTree::Kind::Collection:
        case FileBrowserTree::Kind::PluginGroup:
        case FileBrowserTree::Kind::Other: break;
    }
    return th().textSecondary;
}

/// Browser glyphs live in small softly rounded tiles. The silhouette remains
/// the same project icon language, while the tile gives audio, MIDI, folders
/// and plugins a stable shape that is easier to scan in a dense tree.
QIcon browserIcon(icons::Glyph glyph, const QColor& tint) {
    // A library can contain tens of thousands of rows. Their icon palette has
    // only a handful of combinations, so rasterise each one once per theme
    // instead of allocating two pixmaps for every file discovered.
    static QHash<quint64, QIcon> cache;
    const quint64 key = (quint64(quint32(glyph)) << 32) |
                        quint64(tint.rgba()) |
                        (th().dark ? (quint64(1) << 63) : 0);
    const auto cached = cache.constFind(key);
    if (cached != cache.cend()) return cached.value();

    QIcon icon;
    constexpr int logical = 18;
    for (int scale : {1, 2}) {
        QPixmap pixmap(logical * scale, logical * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF tile(0.5, 0.5, logical - 1.0, logical - 1.0);
        QColor fill = tint;
        fill.setAlphaF(th().dark ? 0.16 : 0.11);
        QColor edge = tint;
        edge.setAlphaF(th().dark ? 0.28 : 0.22);
        painter.setPen(QPen(edge, 0.8));
        painter.setBrush(fill);
        painter.drawRoundedRect(tile, 5.5, 5.5);
        icons::paint(painter, glyph, tile.adjusted(3.1, 3.1, -3.1, -3.1),
                     tint);
        icon.addPixmap(pixmap);
    }
    cache.insert(key, icon);
    return icon;
}

QIcon browserIcon(FileBrowserTree::Kind kind) {
    return browserIcon(glyphFor(kind), tintFor(kind));
}

bool draggable(FileBrowserTree::Kind kind) {
    return kind == FileBrowserTree::Kind::Audio ||
           kind == FileBrowserTree::Kind::Midi ||
           kind == FileBrowserTree::Kind::ChannelStripPreset ||
           kind == FileBrowserTree::Kind::ProjectTemplate ||
           kind == FileBrowserTree::Kind::Plugin;
}

/// A row that holds other rows rather than something to drag.
bool isContainer(FileBrowserTree::Kind kind) {
    return kind == FileBrowserTree::Kind::Folder ||
           kind == FileBrowserTree::Kind::Collection ||
           kind == FileBrowserTree::Kind::PluginGroup;
}

const ui::browserprefs::Collection* findCollection(
    const QVector<ui::browserprefs::Collection>& collections,
    const QString& id) {
    const auto it = std::find_if(collections.cbegin(), collections.cend(),
                                 [&id](const auto& collection) {
                                     return collection.id == id;
                                 });
    return it == collections.cend() ? nullptr : &*it;
}

} // namespace

FileBrowserTree::FileBrowserTree(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setRootIsDecorated(true);
    setUniformRowHeights(true);
    setIndentation(12);
    setIconSize(QSize(18, 18));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) {
                m_changedDirectories.insert(path);
                if (m_watchRefreshPending) return;
                m_watchRefreshPending = true;
                QTimer::singleShot(80, this, [this] {
                    m_watchRefreshPending = false;
                    const auto paths = std::exchange(m_changedDirectories, {});
                    QTreeWidgetItemIterator it(this);
                    for (; *it; ++it)
                        if ((*it)->isExpanded() && paths.contains((*it)->data(0, kPathRole).toString()))
                            reloadNode(*it);
                });
            });

    connect(this, &QTreeWidget::itemExpanded, this, &FileBrowserTree::expandNode);
    connect(this, &QTreeWidget::itemCollapsed, this, &FileBrowserTree::collapseNode);
    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (!current) {
                    emit fileSelected({});
                    return;
                }
                const auto kind = FileBrowserTree::Kind(current->data(0, kKindRole).toInt());
                // Only a real file is auditioned; a plugin row has nothing to
                // play and a folder is not a selection at all.
                if (kind != Kind::Audio && kind != Kind::Midi &&
                    kind != Kind::Other) {
                    emit fileSelected({});
                    return;
                }
                emit fileSelected(kind == Kind::Audio || kind == Kind::Midi
                                      ? current->data(0, kPathRole).toString()
                                      : QString{});
            });
    connect(this, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item) return;
                const auto kind = FileBrowserTree::Kind(item->data(0, kKindRole).toInt());
                if (kind == Kind::ChannelStripPreset) {
                    emit channelStripPresetActivated(
                        item->data(0, kPathRole).toString());
                    return;
                }
                if (kind == Kind::ProjectTemplate) {
                    emit projectTemplateActivated(
                        item->data(0, kPathRole).toString());
                    return;
                }
                if (isContainer(kind) || kind == Kind::Plugin) return;
                emit fileActivated(item->data(0, kPathRole).toString());
            });
}

void FileBrowserTree::setRoots(const QStringList& folders) {
    m_roots = folders;
    rebuildRoots();
}

void FileBrowserTree::setPresetRoot(const QString& folder) {
    const QString clean = QDir::cleanPath(folder);
    if (m_presetRoot == clean) return;
    m_presetRoot = clean;
    rebuildRoots();
}

void FileBrowserTree::setPlugins(const QVector<PluginEntry>& plugins) {
    if (plugins == m_plugins) return;
    m_plugins = plugins;
    if (!m_showingResults) rebuildRoots();
    else m_resultQuery.clear(); // Rebuild matching plugin rows on the next result.
}

void FileBrowserTree::setCollectionFilter(const QString& collectionId) {
    if (m_collectionFilter == collectionId) return;
    m_collectionFilter = collectionId;
    rebuildRoots();
}

QTreeWidgetItem* FileBrowserTree::buildPluginRoot(const QString& query) {
    if (m_plugins.isEmpty()) return nullptr;

    // Grouped by format, in the order the formats first appear, so the folders
    // do not reshuffle when a rescan finds one more plugin.
    QStringList order;
    QHash<QString, QList<const PluginEntry*>> byFormat;
    for (const PluginEntry& entry : m_plugins) {
        if (!query.isEmpty() && !FileSearchWorker::matches(entry.name, query) &&
            !entry.vendor.contains(query, Qt::CaseInsensitive)) continue;
        if (!byFormat.contains(entry.formatName)) order << entry.formatName;
        byFormat[entry.formatName].push_back(&entry);
    }

    auto* root = new QTreeWidgetItem;
    root->setText(0, tr("Plugins"));
    // A synthetic path, so every "find the row for this path" walk still works
    // and can never collide with a real folder.
    root->setData(0, kPathRole, QStringLiteral("daw://plugins"));
    root->setData(0, kKindRole, int(Kind::PluginGroup));
    root->setIcon(0, browserIcon(icons::Glyph::Plugin, th().accent));
    root->setToolTip(0, tr("Scanned plugins: %1 — drag one onto a track to "
                           "insert it, or onto a clip to apply it to that clip "
                           "alone").arg(m_plugins.size()));
    root->setFlags(Qt::ItemIsEnabled);

    for (const QString& format : order) {
        auto* group = new QTreeWidgetItem;
        group->setText(0, format);
        group->setData(0, kPathRole, QStringLiteral("daw://plugins/") + format);
        group->setData(0, kKindRole, int(Kind::PluginGroup));
        group->setIcon(0, icons::icon(icons::Glyph::Folder,
                                     th().textSecondary, 16));
        group->setFlags(Qt::ItemIsEnabled);

        QList<const PluginEntry*> entries = byFormat.value(format);
        std::sort(entries.begin(), entries.end(),
                  [](const PluginEntry* a, const PluginEntry* b) {
                      return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
                  });
        for (const PluginEntry* entry : entries) {
            auto* row = new QTreeWidgetItem;
            row->setText(0, entry->name);
            row->setData(0, kPathRole, entry->path);
            row->setData(0, kKindRole, int(Kind::Plugin));
            row->setData(0, kPluginFormatRole, entry->format);
            row->setData(0, kPluginUidRole, entry->uid);
            // Instruments and effects go to different places, and the glyph is
            // the only warning before the drag that one of them will be
            // refused by a track that already has an instrument.
            row->setIcon(0, browserIcon(entry->instrument ? icons::Glyph::Synth
                                                          : icons::Glyph::Plugin,
                                        entry->instrument
                                            ? Theme::midiAccent()
                                            : th().accent));
            row->setToolTip(0, entry->vendor.isEmpty()
                                   ? entry->name
                                   : entry->name + QStringLiteral(" — ") +
                                         entry->vendor);
            row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                          Qt::ItemIsDragEnabled);
            group->addChild(row);
        }
        root->addChild(group);
    }
    if (root->childCount() == 0) { delete root; return nullptr; }
    return root;
}

QTreeWidgetItem* FileBrowserTree::buildCollectionRoot(
    const ui::browserprefs::Collection& collection) {
    auto* root = new QTreeWidgetItem;
    root->setText(0, collection.name);
    root->setData(0, kPathRole,
                  QStringLiteral("daw://collection/") + collection.id);
    root->setData(0, kKindRole, int(Kind::Collection));
    root->setData(0, kCollectionRole, collection.id);
    root->setFlags(Qt::ItemIsEnabled);

    const QColor color(collection.color);
    const QColor tint = color.isValid() ? color : th().textSecondary;
    root->setData(0, kFolderTintRole,
                  color.isValid() ? color.name(QColor::HexArgb) : QString{});
    root->setIcon(0, icons::icon(collection.id == ui::browserprefs::favoritesId()
                                    ? icons::Glyph::Star
                                    : icons::Glyph::Folder,
                                tint, 16));
    if (color.isValid()) root->setForeground(0, color);

    for (const QString& path : collection.paths) {
        const QFileInfo info(path);
        if (!info.exists()) continue;
        auto* child = makeItem(info.absoluteFilePath(), info.isDir(), -1, color);
        child->setData(0, kCollectionMemberRole, collection.id);
        child->setToolTip(0, info.absoluteFilePath());
        root->addChild(child);
    }
    if (root->childCount() == 0) {
        auto* empty = new QTreeWidgetItem(
            QStringList(tr("Empty — right-click a file to add it")));
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(0, th().textSecondary);
        root->addChild(empty);
    }
    return root;
}

void FileBrowserTree::rebuildRoots() {
    const QStringList open = m_showingResults ? m_parkedExpanded : expandedPaths();
    const QString selected = selectedPath();

    m_restoreExpanded = QSet<QString>(open.begin(), open.end());
    m_restoreSelected = selected;
    m_showingResults = false;
    for (const QString& path : m_watched) m_watcher->removePath(path);
    m_watched.clear();
    clear();

    const auto collections = ui::browserprefs::collections();
    if (!m_collectionFilter.isEmpty()) {
        const auto* collection = findCollection(collections, m_collectionFilter);
        if (!collection) {
            m_collectionFilter.clear();
        } else {
            const QColor inherited(collection->color);
            for (const QString& path : collection->paths) {
                const QFileInfo info(path);
                if (!info.exists()) continue;
                auto* item = makeItem(info.absoluteFilePath(), info.isDir(), -1,
                                      inherited);
                item->setData(0, kCollectionMemberRole, collection->id);
                item->setToolTip(0, info.absoluteFilePath());
                addTopLevelItem(item);
            }
            if (topLevelItemCount() == 0) {
                auto* empty = new QTreeWidgetItem(
                    QStringList(tr("This collection is empty")));
                empty->setFlags(Qt::NoItemFlags);
                empty->setForeground(0, th().textSecondary);
                addTopLevelItem(empty);
            }
            return;
        }
    }

    // Virtual collections are the first, stable destinations in the main tab.
    // Favorites is always present; custom collections follow in creation order.
    for (const auto& collection : collections)
        addTopLevelItem(buildCollectionRoot(collection));

    // Plugins and presets are permanent destinations, so both stay above the
    // user's potentially long list of sample folders.
    if (QTreeWidgetItem* plugins = buildPluginRoot()) addTopLevelItem(plugins);
    if (!m_presetRoot.isEmpty()) {
        const QFileInfo info(m_presetRoot);
        if (info.isDir()) {
            QTreeWidgetItem* item = makeItem(info.absoluteFilePath(), true);
            item->setText(0, tr("Presets"));
            item->setToolTip(0, tr("Project templates, Channel Strip templates, "
                                   "and other presets\n%1")
                                    .arg(info.absoluteFilePath()));
            addTopLevelItem(item);
        }
    }

    for (const QString& folder : m_roots) {
        const QFileInfo info(folder);
        if (!info.isDir()) continue;
        QTreeWidgetItem* item = makeItem(info.absoluteFilePath(), true);
        // A root reads better by its own name than by its full path, but the
        // path is what tells two "Samples" folders apart.
        item->setText(0, info.fileName().isEmpty() ? info.absoluteFilePath()
                                                   : info.fileName());
        item->setToolTip(0, info.absoluteFilePath());
        addTopLevelItem(item);
    }
    if (topLevelItemCount() == 0) return;

    restoreExpanded(open);
    if (!selected.isEmpty()) {
        QTreeWidgetItemIterator it(this);
        for (; *it; ++it) {
            if ((*it)->data(0, kPathRole).toString() == selected) {
                setCurrentItem(*it);
                break;
            }
        }
    }
}

QTreeWidgetItem* FileBrowserTree::makeItem(const QString& path, bool isDirectory,
                                           int cachedKind,
                                           const QColor& inheritedFolderColor) {
    const QFileInfo info(path);
    auto* item = new QTreeWidgetItem;
    item->setText(0, info.fileName());
    item->setData(0, kPathRole, info.absoluteFilePath());

    const Kind kind = cachedKind >= 0 ? Kind(cachedKind) : kindOf(info);
    item->setData(0, kKindRole, int(kind));
    QColor folderColor;
    if (kind == Kind::Folder) {
        folderColor = QColor(ui::browserprefs::folderColor(info.absoluteFilePath()));
        if (!folderColor.isValid()) folderColor = inheritedFolderColor;
        item->setData(0, kFolderTintRole,
                      folderColor.isValid()
                          ? folderColor.name(QColor::HexArgb)
                          : QString{});
        item->setIcon(0, icons::icon(icons::Glyph::Folder,
                                    folderColor.isValid()
                                        ? folderColor
                                        : th().textSecondary,
                                    16));
    } else {
        item->setIcon(0, browserIcon(kind));
    }

    Qt::ItemFlags flags = Qt::ItemIsEnabled;
    if (!isContainer(kind)) flags |= Qt::ItemIsSelectable;
    if (draggable(kind)) flags |= Qt::ItemIsDragEnabled;
    item->setFlags(flags);

    if (kind == Kind::Other) {
        // Listed, as asked for, but plainly not something the project can take:
        // dimmed, and with the drag flag off rather than a drag that is refused
        // on arrival.
        QColor dim = th().textSecondary;
        dim.setAlpha(140);
        item->setForeground(0, dim);
    } else if (kind != Kind::Folder) {
        item->setForeground(0, th().textPrimary);
    } else if (folderColor.isValid()) {
        item->setForeground(0, folderColor);
    }

    if (kind == Kind::ChannelStripPreset) {
        item->setToolTip(0, tr("Drag this Channel Strip preset onto a track or "
                               "channel strip, or double-click to apply it to "
                               "the selected channel."));
    }
    if (kind == Kind::ProjectTemplate) {
        item->setToolTip(
            0, tr("Double-click to create a new project, drag into the track "
                  "area to add its tracks, or use the context menu."));
    }

    if (kind == Kind::Folder) {
        item->setData(0, kUnreadRole, true);
        // A placeholder child is what draws the expander arrow before the
        // folder has been read.
        auto* placeholder = new QTreeWidgetItem(QStringList(QStringLiteral("…")));
        placeholder->setFlags(Qt::ItemIsEnabled);
        item->addChild(placeholder);
    }
    return item;
}

void FileBrowserTree::keyPressEvent(QKeyEvent* event) {
    if (event && (event->key() == Qt::Key_Return ||
                  event->key() == Qt::Key_Enter)) {
        QTreeWidgetItem* item = currentItem();
        const Kind kind = item
            ? Kind(item->data(0, kKindRole).toInt())
            : Kind::Other;
        if (kind == Kind::Audio) {
            emit sampleLoadRequested(item->data(0, kPathRole).toString());
            event->accept();
            return;
        }
        if (kind == Kind::ProjectTemplate) {
            emit projectTemplateActivated(
                item->data(0, kPathRole).toString());
            event->accept();
            return;
        }
    }
    QTreeWidget::keyPressEvent(event);
}

bool FileBrowserTree::showSelectedItemActionsMenu() {
    QTreeWidgetItem* item = currentItem();
    if (!item) return false;
    scrollToItem(item);
    const QRect row = visualItemRect(item);
    if (row.isEmpty()) return false;
    QContextMenuEvent event(QContextMenuEvent::Other, row.center(),
                            QCursor::pos());
    contextMenuEvent(&event);
    return true;
}

void FileBrowserTree::contextMenuEvent(QContextMenuEvent* event) {
    QTreeWidgetItem* item = itemAt(event ? event->pos() : QPoint{});
    const bool hasKind = item && item->data(0, kKindRole).isValid();
    const Kind kind = hasKind ? Kind(item->data(0, kKindRole).toInt())
                              : Kind::Other;
    const QString path = item ? item->data(0, kPathRole).toString() : QString{};
    const QString collectionId = item
        ? item->data(0, kCollectionRole).toString()
        : QString{};
    const QString memberOf = item
        ? item->data(0, kCollectionMemberRole).toString()
        : QString{};
    const auto collections = ui::browserprefs::collections();
    const auto* collection = findCollection(collections, collectionId);
    const auto* memberCollection = findCollection(collections, memberOf);

    QMenu menu(this);
    QAction* addTracks = nullptr;
    QAction* createProject = nullptr;
    if (kind == Kind::ProjectTemplate) {
        addTracks = menu.addAction(tr("Add Tracks to Current Project"));
        createProject = menu.addAction(tr("Create New Project"));
        menu.addSeparator();
    }

    QHash<QAction*, QString> addDestinations;
    if (hasKind && !isContainer(kind) && kind != Kind::Plugin) {
        QMenu* addTo = menu.addMenu(tr("Add to Folder"));
        for (const auto& destination : collections) {
            QAction* action = addTo->addAction(
                destination.id == ui::browserprefs::favoritesId()
                    ? icons::icon(icons::Glyph::Star, th().textSecondary, 15)
                    : icons::icon(icons::Glyph::Folder,
                                  QColor(destination.color).isValid()
                                      ? QColor(destination.color)
                                      : th().textSecondary,
                                  15),
                destination.name);
            const bool alreadyThere = std::any_of(
                destination.paths.cbegin(), destination.paths.cend(),
                [&path](const QString& existing) {
                    return QFileInfo(existing).absoluteFilePath().compare(
                        QFileInfo(path).absoluteFilePath(),
#if defined(Q_OS_WIN)
                        Qt::CaseInsensitive
#else
                        Qt::CaseSensitive
#endif
                    ) == 0;
                });
            action->setEnabled(!alreadyThere);
            addDestinations.insert(action, destination.id);
        }
    }

    QAction* removeMember = nullptr;
    if (memberCollection) {
        removeMember = menu.addAction(
            tr("Remove from %1").arg(memberCollection->name));
        menu.addSeparator();
    }

    QAction* chooseColor = nullptr;
    QAction* clearColor = nullptr;
    if (kind == Kind::Folder || kind == Kind::Collection) {
        chooseColor = menu.addAction(tr("Choose Folder Color…"));
        const QString direct = kind == Kind::Collection && collection
            ? collection->color
            : ui::browserprefs::directFolderColor(path);
        if (!direct.isEmpty()) clearColor = menu.addAction(tr("Use Default Color"));
    }

    QAction* pinTab = nullptr;
    QAction* removeTab = nullptr;
    QAction* renameCollection = nullptr;
    QAction* deleteCollection = nullptr;
    if (collection) {
        const auto tabs = ui::browserprefs::tabs();
        const bool pinned = std::any_of(tabs.cbegin(), tabs.cend(),
                                        [&collectionId](const auto& tab) {
                                            return tab.collectionId == collectionId;
                                        });
        menu.addSeparator();
        if (pinned) removeTab = menu.addAction(tr("Remove Icon Tab"));
        else pinTab = menu.addAction(tr("Show as Icon Tab…"));
        if (collectionId != ui::browserprefs::favoritesId()) {
            renameCollection = menu.addAction(tr("Rename Folder…"));
            deleteCollection = menu.addAction(tr("Delete Collection Folder"));
        }
    }

    if (!menu.isEmpty()) menu.addSeparator();
    QAction* newCollection = menu.addAction(tr("New Collection Folder…"));
    QAction* chosen = menu.exec(event ? event->globalPos() : QCursor::pos());
    if (!chosen) return;

    if (chosen == addTracks) {
        emit projectTemplateTracksRequested(path);
        return;
    }
    if (chosen == createProject) {
        emit projectTemplateActivated(path);
        return;
    }
    if (addDestinations.contains(chosen)) {
        const QString destinationId = addDestinations.value(chosen);
        if (ui::browserprefs::addToCollection(destinationId, path)) {
            const auto* destination = findCollection(collections, destinationId);
            emit statusMessage(destination
                ? tr("Added %1 to %2").arg(QFileInfo(path).fileName(),
                                            destination->name)
                : tr("Added to collection"));
            rebuildRoots();
            emit organizationChanged();
        }
        return;
    }
    if (chosen == removeMember) {
        ui::browserprefs::removeFromCollection(memberOf, path);
        rebuildRoots();
        emit organizationChanged();
        return;
    }
    if (chosen == chooseColor) {
        QString current = kind == Kind::Collection && collection
            ? collection->color
            : ui::browserprefs::folderColor(path);
        QColor initial(current);
        if (!initial.isValid()) initial = Theme::midiAccent();
        const QColor color = QColorDialog::getColor(
            initial, this, tr("Folder Color"));
        if (!color.isValid()) return;
        if (kind == Kind::Collection)
            ui::browserprefs::setCollectionColor(collectionId,
                                                  color.name(QColor::HexRgb));
        else
            ui::browserprefs::setFolderColor(path,
                                              color.name(QColor::HexRgb));
        rebuildRoots();
        emit organizationChanged();
        return;
    }
    if (chosen == clearColor) {
        if (kind == Kind::Collection)
            ui::browserprefs::setCollectionColor(collectionId, {});
        else
            ui::browserprefs::setFolderColor(path, {});
        rebuildRoots();
        emit organizationChanged();
        return;
    }
    if (chosen == pinTab) {
        emit tabRequested(collectionId);
        return;
    }
    if (chosen == removeTab) {
        ui::browserprefs::removeTab(collectionId);
        emit organizationChanged();
        return;
    }
    if (chosen == renameCollection && collection) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename Folder"), tr("Name:"), QLineEdit::Normal,
            collection->name, &accepted).trimmed();
        if (accepted && ui::browserprefs::renameCollection(collectionId, name)) {
            rebuildRoots();
            emit organizationChanged();
        }
        return;
    }
    if (chosen == deleteCollection && collection) {
        if (QMessageBox::question(
                this, tr("Delete Collection Folder"),
                tr("Delete “%1”? The original files will not be changed.")
                    .arg(collection->name)) == QMessageBox::Yes) {
            ui::browserprefs::removeCollection(collectionId);
            rebuildRoots();
            emit organizationChanged();
        }
        return;
    }
    if (chosen == newCollection) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("New Collection Folder"),
            tr("Name for the sample shortcuts:"), QLineEdit::Normal, {},
            &accepted).trimmed();
        if (accepted && !ui::browserprefs::createCollection(name).isEmpty()) {
            rebuildRoots();
            emit organizationChanged();
        }
    }
}

void FileBrowserTree::drawBranches(QPainter* painter, const QRect& rect,
                                   const QModelIndex& index) const {
    if (!painter || !index.isValid()) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QColor guide = th().textSecondary;
    guide.setAlpha(th().dark ? 45 : 38);
    painter->setPen(QPen(guide, 1.0));

    const int step = indentation();
    int branchX = rect.right() - step / 2;
    QModelIndex ancestor = index.parent();
    int ancestorX = branchX - step;
    while (ancestor.isValid() && ancestorX >= rect.left()) {
        if (ancestor.row() + 1 < model()->rowCount(ancestor.parent()))
            painter->drawLine(ancestorX, rect.top(), ancestorX, rect.bottom());
        ancestor = ancestor.parent();
        ancestorX -= step;
    }

    if (index.parent().isValid()) {
        const int middle = rect.center().y();
        painter->drawLine(branchX, rect.top(), branchX, middle);
        if (index.row() + 1 < model()->rowCount(index.parent()))
            painter->drawLine(branchX, middle, branchX, rect.bottom());
        painter->drawLine(branchX, middle, rect.right(), middle);
    }

    if (model()->hasChildren(index)) {
        QColor chevron = th().textSecondary;
        chevron.setAlpha(205);
        painter->setPen(QPen(chevron, 1.35, Qt::SolidLine, Qt::RoundCap,
                             Qt::RoundJoin));
        const QPointF center(branchX, rect.center().y());
        QPainterPath path;
        if (isExpanded(index)) {
            path.moveTo(center.x() - 3.0, center.y() - 1.5);
            path.lineTo(center.x(), center.y() + 1.5);
            path.lineTo(center.x() + 3.0, center.y() - 1.5);
        } else {
            path.moveTo(center.x() - 1.5, center.y() - 3.0);
            path.lineTo(center.x() + 1.5, center.y());
            path.lineTo(center.x() - 1.5, center.y() + 3.0);
        }
        painter->drawPath(path);
    }
    painter->restore();
}

struct FileBrowserTree::DirectoryResult {
    QPersistentModelIndex parent;
    quint64 serial = 0;
    QVector<QPair<QString, int>> entries;
    int offset = 0;
    QSet<QString> expanded;
    QString selected;
    bool readable = true;
};

void FileBrowserTree::populate(QTreeWidgetItem* parent, const QString& path) {
    constexpr int generationRole = Qt::UserRole + 8;
    auto result = std::make_shared<DirectoryResult>();
    result->parent = indexFromItem(parent);
    result->serial = ++m_loadSerial;
    result->expanded = m_restoreExpanded;
    result->selected = selectedPath();
    parent->setData(0, generationRole, result->serial);
    const QPointer<FileBrowserTree> guard(this);
    const QStringList ignored = ui::browserprefs::ignoredExtensions();
    QThreadPool::globalInstance()->start([guard, result, path, ignored] {
        QDir dir(path);
        const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                               QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        result->readable = dir.isReadable();
        result->entries.reserve(entries.size());
        for (const auto& entry : entries) {
            if (entry.isSymLink() && !entry.exists()) continue;
            if ((!entry.isDir() || ui::projecttemplates::isTemplatePackage(entry.filePath())) &&
                ui::browserprefs::isIgnoredFile(entry.fileName(), ignored)) continue;
            result->entries.push_back({entry.absoluteFilePath(), int(kindOf(entry))});
        }
        QMetaObject::invokeMethod(qApp, [guard, result] {
            if (guard) guard->applyDirectoryChunk(result);
        }, Qt::QueuedConnection);
    });
}

void FileBrowserTree::applyDirectoryChunk(const std::shared_ptr<DirectoryResult>& result) {
    constexpr int generationRole = Qt::UserRole + 8;
    if (m_showingResults || !result->parent.isValid()) return;
    auto* parent = itemFromIndex(result->parent);
    if (!parent || parent->data(0, generationRole).toULongLong() != result->serial) return;
    if (result->offset == 0) {
        // Remove stale children in bounded turns too. Keep the parent stable so
        // all outstanding persistent indexes invalidate safely when discarded.
        int removed = 0;
        while (parent->childCount() && removed++ < 128) delete parent->takeChild(0);
        if (parent->childCount()) {
            QTimer::singleShot(1, this, [this, result] { applyDirectoryChunk(result); });
            return;
        }
        if (!result->readable) emit statusMessage(tr("Cannot read %1").arg(parent->data(0, kPathRole).toString()));
    }
    const int end = std::min(result->offset + 128, int(result->entries.size()));
    const QColor inherited(parent->data(0, kFolderTintRole).toString());
    for (; result->offset < end; ++result->offset) {
        const auto& [path, kind] = result->entries[result->offset];
        auto* child = makeItem(path, Kind(kind) == Kind::Folder, kind, inherited);
        parent->addChild(child);
        if (result->expanded.contains(path) || m_restoreExpanded.contains(path))
            child->setExpanded(true);
        if (path == result->selected || path == m_restoreSelected) {
            setCurrentItem(child);
            m_restoreSelected.clear();
        }
    }
    if (result->offset < result->entries.size())
        QTimer::singleShot(1, this, [this, result] { applyDirectoryChunk(result); });
}

void FileBrowserTree::expandNode(QTreeWidgetItem* item) {
    if (!item || m_showingResults) return;
    const QString path = item->data(0, kPathRole).toString();
    m_restoreExpanded.insert(path);
    // The plugin folders are built whole and are not on disk: nothing to read,
    // and nothing a file-system watcher could usefully watch.
    if (path.startsWith(QLatin1String("daw://"))) return;
    if (item->data(0, kUnreadRole).toBool()) {
        // Drop the placeholder and read the folder for real.
        while (item->childCount() > 0) delete item->takeChild(0);
        populate(item, path);
        item->setData(0, kUnreadRole, false);
    }
    watch(path);
}

void FileBrowserTree::collapseNode(QTreeWidgetItem* item) {
    if (!item) return;
    const QString path = item->data(0, kPathRole).toString();
    // A user collapse while enumeration is pending wins over restored state.
    m_restoreExpanded.remove(path);
    if (path.startsWith(QLatin1String("daw://"))) return;
    unwatch(path);
}

void FileBrowserTree::reloadNode(QTreeWidgetItem* item) {
    if (!item) return;
    populate(item, item->data(0, kPathRole).toString());
    item->setData(0, kUnreadRole, false);
}

void FileBrowserTree::refresh() {
    if (m_showingResults) return;
    setRoots(m_roots);
}

int FileBrowserTree::showResults(const QStringList& paths, bool truncated,
                                const QString& query, bool searching) {
    const bool append = m_showingResults && m_resultQuery == query &&
        m_resultPaths.size() <= paths.size() &&
        std::equal(m_resultPaths.cbegin(), m_resultPaths.cend(), paths.cbegin());
    if (!m_showingResults) m_parkedExpanded = expandedPaths();
    m_showingResults = true;
    if (!append) {
        clear();
        m_resultQuery = query;
        m_resultPaths.clear();
        m_resultPluginCount = 0;
        if (!query.isEmpty() && m_collectionFilter.isEmpty()) {
            if (auto* plugins = buildPluginRoot(query)) {
                addTopLevelItem(plugins);
                plugins->setExpanded(true);
                for (int i = 0; i < plugins->childCount(); ++i) {
                    auto* group = plugins->child(i);
                    m_resultPluginCount += group->childCount();
                    group->setExpanded(true);
                }
            }
        }
    } else {
        // Keep existing rows, selection, audition and scroll position as the
        // worker adds matches. Only the status footer is replaced.
        while (topLevelItemCount() &&
               !topLevelItem(topLevelItemCount() - 1)->data(0, kKindRole).isValid())
            delete takeTopLevelItem(topLevelItemCount() - 1);
    }
    QColor collectionTint;
    if (!m_collectionFilter.isEmpty()) {
        const auto collections = ui::browserprefs::collections();
        if (const auto* collection = findCollection(collections,
                                                     m_collectionFilter))
            collectionTint = QColor(collection->color);
    }
    for (qsizetype i = m_resultPaths.size(); i < paths.size(); ++i) {
        const QString& path = paths[i];
        QTreeWidgetItem* item = makeItem(path, QFileInfo(path).isDir(), -1,
                                         collectionTint);
        if (!m_collectionFilter.isEmpty())
            item->setData(0, kCollectionMemberRole, m_collectionFilter);
        // In a flat list the name alone is ambiguous, so each row says which
        // folder it came from.
        item->setText(0, QFileInfo(path).fileName());
        item->setToolTip(0, path);
        addTopLevelItem(item);
    }
    m_resultPaths = paths;
    const int count = m_resultPluginCount + int(paths.size());
    if (searching) {
        auto* pending = new QTreeWidgetItem(QStringList(tr("Searching…")));
        pending->setFlags(Qt::NoItemFlags);
        addTopLevelItem(pending);
    }
    if (truncated) {
        auto* note = new QTreeWidgetItem(
            QStringList(tr("… more matches — narrow the search")));
        note->setFlags(Qt::ItemIsEnabled);
        QColor dim = th().textSecondary;
        dim.setAlpha(160);
        note->setForeground(0, dim);
        addTopLevelItem(note);
    }
    if (count == 0 && !truncated && !searching) {
        auto* none = new QTreeWidgetItem(QStringList(tr("Nothing matches")));
        none->setFlags(Qt::ItemIsEnabled);
        none->setForeground(0, th().textSecondary);
        addTopLevelItem(none);
    }
    return count;
}

void FileBrowserTree::showTree() {
    if (!m_showingResults) return;
    m_showingResults = false;
    const QStringList open = m_parkedExpanded;
    rebuildRoots();
    restoreExpanded(open);
}

bool FileBrowserTree::selectFirstPluginForTest() {
    QTreeWidgetItem* first = nullptr;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        if (Kind((*it)->data(0, kKindRole).toInt()) == Kind::PluginGroup) {
            (*it)->setExpanded(true);
        } else if (!first &&
                   Kind((*it)->data(0, kKindRole).toInt()) == Kind::Plugin) {
            first = *it;
        }
    }
    if (!first) return false;
    setCurrentItem(first);
    return true;
}

int FileBrowserTree::pluginRowCountForTest() const {
    int count = 0;
    QTreeWidgetItemIterator it(const_cast<FileBrowserTree*>(this));
    for (; *it; ++it) {
        if (Kind((*it)->data(0, kKindRole).toInt()) == Kind::Plugin) ++count;
    }
    return count;
}

bool FileBrowserTree::containersAreNavigationOnlyForTest() const {
    bool found = false;
    QTreeWidgetItemIterator it(const_cast<FileBrowserTree*>(this));
    for (; *it; ++it) {
        const Kind kind = Kind((*it)->data(0, kKindRole).toInt());
        if (!isContainer(kind)) continue;
        found = true;
        if ((*it)->flags().testFlag(Qt::ItemIsSelectable)) return false;
    }
    return found;
}

bool FileBrowserTree::selectedProjectTemplateForTest() const {
    QTreeWidgetItem* item = currentItem();
    return item && Kind(item->data(0, kKindRole).toInt()) ==
                       Kind::ProjectTemplate;
}

bool FileBrowserTree::activateSelectedProjectTemplateForTest() {
    if (!selectedProjectTemplateForTest()) return false;
    emit projectTemplateActivated(
        currentItem()->data(0, kPathRole).toString());
    return true;
}

QString FileBrowserTree::selectedPath() const {
    QTreeWidgetItem* item = currentItem();
    if (!item) return {};
    const Kind kind = Kind(item->data(0, kKindRole).toInt());
    // A folder is not a file, and a plugin's "path" is its module on disk —
    // nothing the browser's audition or its file drag should ever see.
    if (isContainer(kind) || kind == Kind::Plugin) return {};
    return item->data(0, kPathRole).toString();
}

QStringList FileBrowserTree::expandedPaths() const {
    // Expansion events maintain this set. Walking every loaded file on each
    // folder open or watcher notification makes large libraries quadratic.
    return m_restoreExpanded.values();
}

void FileBrowserTree::restoreExpanded(const QStringList& paths) {
    if (paths.isEmpty()) return;
    for (const auto& path : paths) m_restoreExpanded.insert(path);
    // Expanding fills a node, which can reveal more of the remembered set, so
    // this walks until nothing new opens rather than once over the tree.
    bool opened = true;
    while (opened) {
        opened = false;
        QTreeWidgetItemIterator it(this);
        for (; *it; ++it) {
            QTreeWidgetItem* item = *it;
            if (item->isExpanded()) continue;
            const Kind kind = Kind(item->data(0, kKindRole).toInt());
            if (kind != Kind::Folder && kind != Kind::Collection &&
                kind != Kind::PluginGroup) continue;
            if (!paths.contains(item->data(0, kPathRole).toString())) continue;
            item->setExpanded(true);
            opened = true;
            break;   // the iterator is invalid once children appear
        }
    }
}

void FileBrowserTree::watch(const QString& path) {
    if (path.isEmpty() || m_watched.contains(path)) return;
    if (m_watched.size() >= kMaxWatched) return;   // Refresh is the way back
    if (m_watcher->addPath(path)) m_watched << path;
}

void FileBrowserTree::unwatch(const QString& path) {
    if (path.isEmpty() || !m_watched.contains(path)) return;
    m_watcher->removePath(path);
    m_watched.removeAll(path);
}

QMimeData* FileBrowserTree::dragPayload() const {
    QTreeWidgetItem* item = currentItem();
    if (!item) return nullptr;
    const Kind kind = Kind(item->data(0, kKindRole).toInt());
    if (!draggable(kind)) return nullptr;
    const QString path = item->data(0, kPathRole).toString();

    if (kind == Kind::Plugin) {
        // Not a file: two identifiers the drop site resolves through the
        // plugin manager it already has. A URL here would look importable to
        // every audio target in the application.
        const QString uid = item->data(0, kPluginUidRole).toString();
        if (uid.isEmpty()) return nullptr;
        auto* mime = new QMimeData;
        mime->setData(QLatin1String(ui::kPluginDragMime),
                      ui::encodePluginRef(item->data(0, kPluginFormatRole).toInt(),
                                          uid));
        mime->setText(item->text(0));
        return mime;
    }

    if (path.isEmpty()) return nullptr;

    auto* mime = new QMimeData;
    // Plain file URLs: the sampler, the arrangement and the instrument slot all
    // already take these from the desktop, so the browser needs no format of
    // its own and they need no new code.
    mime->setUrls({QUrl::fromLocalFile(path)});
    // Text as well, so a drop onto a name field or another application gets
    // something sensible rather than nothing.
    mime->setText(path);
    return mime;
}

void FileBrowserTree::startDrag(Qt::DropActions supportedActions) {
    QMimeData* mime = dragPayload();
    if (!mime) return;
    QTreeWidgetItem* item = currentItem();
    const Kind kind = Kind(item->data(0, kKindRole).toInt());
    const QString path = item->data(0, kPathRole).toString();

    // A small themed pill showing what is being dragged: without it the drag is
    // an invisible gesture, and dropping into a plugin slot is guesswork.
    const QString label = kind == Kind::Plugin
                              ? item->text(0)
                              : QFileInfo(path).fileName();
    const QFontMetrics metrics(font());
    const int textWidth = metrics.horizontalAdvance(label);
    const QSize size(std::min(textWidth + 34, 260), metrics.height() + 12);
    QPixmap pixmap(size * devicePixelRatioF());
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF plate(0.5, 0.5, size.width() - 1.0, size.height() - 1.0);
        p.setBrush(th().surfaceElevated);
        p.setPen(QPen(th().accent, 1.0));
        p.drawRoundedRect(plate, 5.0, 5.0);
        icons::paint(p, glyphFor(kind), QRectF(4, 2, 18, plate.height() - 4),
                     th().accent);
        p.setPen(th().textPrimary);
        p.drawText(plate.adjusted(24, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   metrics.elidedText(label, Qt::ElideMiddle, size.width() - 32));
    }

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(12, size.height() / 2));
    drag->exec(supportedActions & ~Qt::MoveAction, Qt::CopyAction);
}
