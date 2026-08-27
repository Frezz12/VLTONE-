#include "FileBrowserTree.hpp"

#include "ChannelStripPresets.hpp"
#include "FileTypes.hpp"
#include "ProjectTemplates.hpp"

#include <algorithm>
#include "Icons.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
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
        case FileBrowserTree::Kind::PluginGroup: return icons::Glyph::Folder;
        case FileBrowserTree::Kind::Plugin: return icons::Glyph::Plugin;
        case FileBrowserTree::Kind::Other:  break;
    }
    return icons::Glyph::Import;
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
           kind == FileBrowserTree::Kind::PluginGroup;
}

} // namespace

FileBrowserTree::FileBrowserTree(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setRootIsDecorated(true);
    setUniformRowHeights(true);
    setIndentation(12);
    setIconSize(QSize(14, 14));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) {
                // Find the open node for that folder and re-read just it.
                QTreeWidgetItemIterator it(this);
                for (; *it; ++it) {
                    if ((*it)->data(0, kPathRole).toString() != path) continue;
                    if ((*it)->isExpanded()) reloadNode(*it);
                    return;
                }
            });

    connect(this, &QTreeWidget::itemExpanded, this, &FileBrowserTree::expandNode);
    connect(this, &QTreeWidget::itemCollapsed, this, &FileBrowserTree::collapseNode);
    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (!current) return;
                const auto kind = FileBrowserTree::Kind(current->data(0, kKindRole).toInt());
                // Only a real file is auditioned; a plugin row has nothing to
                // play and a folder is not a selection at all.
                if (kind != Kind::Audio && kind != Kind::Midi &&
                    kind != Kind::Other) {
                    return;
                }
                emit fileSelected(current->data(0, kPathRole).toString());
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
    rebuildRoots();
}

QTreeWidgetItem* FileBrowserTree::buildPluginRoot() {
    if (m_plugins.isEmpty()) return nullptr;

    // Grouped by format, in the order the formats first appear, so the folders
    // do not reshuffle when a rescan finds one more plugin.
    QStringList order;
    QHash<QString, QList<const PluginEntry*>> byFormat;
    for (const PluginEntry& entry : m_plugins) {
        if (!byFormat.contains(entry.formatName)) order << entry.formatName;
        byFormat[entry.formatName].push_back(&entry);
    }

    auto* root = new QTreeWidgetItem;
    root->setText(0, tr("Plugins"));
    // A synthetic path, so every "find the row for this path" walk still works
    // and can never collide with a real folder.
    root->setData(0, kPathRole, QStringLiteral("daw://plugins"));
    root->setData(0, kKindRole, int(Kind::PluginGroup));
    root->setIcon(0, icons::icon(icons::Glyph::Plugin, th().textSecondary, 14));
    root->setToolTip(0, tr("Scanned plugins: %1 — drag one onto a track to "
                           "insert it, or onto a clip to apply it to that clip "
                           "alone").arg(m_plugins.size()));
    root->setFlags(Qt::ItemIsEnabled);

    for (const QString& format : order) {
        auto* group = new QTreeWidgetItem;
        group->setText(0, format);
        group->setData(0, kPathRole, QStringLiteral("daw://plugins/") + format);
        group->setData(0, kKindRole, int(Kind::PluginGroup));
        group->setIcon(0, icons::icon(icons::Glyph::Folder, th().textSecondary, 14));
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
            row->setIcon(0, icons::icon(entry->instrument ? icons::Glyph::Synth
                                                          : icons::Glyph::Plugin,
                                        th().textSecondary, 14));
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
    return root;
}

void FileBrowserTree::rebuildRoots() {
    const QStringList open = m_showingResults ? m_parkedExpanded : expandedPaths();
    const QString selected = selectedPath();

    m_showingResults = false;
    for (const QString& path : m_watched) m_watcher->removePath(path);
    m_watched.clear();
    clear();

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

QTreeWidgetItem* FileBrowserTree::makeItem(const QString& path, bool isDirectory) {
    const QFileInfo info(path);
    auto* item = new QTreeWidgetItem;
    item->setText(0, info.fileName());
    item->setData(0, kPathRole, info.absoluteFilePath());

    const Kind kind = kindOf(info);
    item->setData(0, kKindRole, int(kind));
    item->setIcon(0, icons::icon(glyphFor(kind),
                                 draggable(kind) || kind == Kind::Folder
                                     ? th().textSecondary
                                     : th().textSecondary.darker(140),
                                 14));

    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
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
        item->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("…"))));
    }
    return item;
}

void FileBrowserTree::keyPressEvent(QKeyEvent* event) {
    if (event && (event->key() == Qt::Key_Return ||
                  event->key() == Qt::Key_Enter)) {
        QTreeWidgetItem* item = currentItem();
        if (item && Kind(item->data(0, kKindRole).toInt()) ==
                        Kind::ProjectTemplate) {
            emit projectTemplateActivated(
                item->data(0, kPathRole).toString());
            event->accept();
            return;
        }
    }
    QTreeWidget::keyPressEvent(event);
}

void FileBrowserTree::contextMenuEvent(QContextMenuEvent* event) {
    QTreeWidgetItem* item = itemAt(event ? event->pos() : QPoint{});
    if (!item || Kind(item->data(0, kKindRole).toInt()) !=
                     Kind::ProjectTemplate) {
        QTreeWidget::contextMenuEvent(event);
        return;
    }

    const QString path = item->data(0, kPathRole).toString();
    QMenu menu(this);
    QAction* add = menu.addAction(tr("Add Tracks to Current Project"));
    QAction* create = menu.addAction(tr("Create New Project"));
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == add) emit projectTemplateTracksRequested(path);
    else if (chosen == create) emit projectTemplateActivated(path);
}

void FileBrowserTree::populate(QTreeWidgetItem* parent, const QString& path) {
    QDir dir(path);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                          QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    if (entries.isEmpty() && !dir.isReadable()) {
        emit statusMessage(tr("Cannot read %1").arg(path));
        return;
    }
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink() && !entry.exists()) continue;   // a broken alias
        parent->addChild(makeItem(entry.absoluteFilePath(), entry.isDir()));
    }
}

void FileBrowserTree::expandNode(QTreeWidgetItem* item) {
    if (!item || m_showingResults) return;
    const QString path = item->data(0, kPathRole).toString();
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
    if (path.startsWith(QLatin1String("daw://"))) return;
    unwatch(path);
}

void FileBrowserTree::reloadNode(QTreeWidgetItem* item) {
    if (!item) return;
    const QString path = item->data(0, kPathRole).toString();
    const QStringList open = expandedPaths();
    const QString selected = selectedPath();

    while (item->childCount() > 0) delete item->takeChild(0);
    populate(item, path);
    item->setData(0, kUnreadRole, false);
    restoreExpanded(open);

    if (selected.isEmpty()) return;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        if ((*it)->data(0, kPathRole).toString() == selected) {
            setCurrentItem(*it);
            return;
        }
    }
}

void FileBrowserTree::refresh() {
    if (m_showingResults) return;
    setRoots(m_roots);
}

void FileBrowserTree::showResults(const QStringList& paths, bool truncated) {
    if (!m_showingResults) m_parkedExpanded = expandedPaths();
    m_showingResults = true;
    clear();

    for (const QString& path : paths) {
        QTreeWidgetItem* item = makeItem(path, QFileInfo(path).isDir());
        // In a flat list the name alone is ambiguous, so each row says which
        // folder it came from.
        item->setText(0, QFileInfo(path).fileName());
        item->setToolTip(0, path);
        addTopLevelItem(item);
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
    if (paths.isEmpty() && !truncated) {
        auto* none = new QTreeWidgetItem(QStringList(tr("Nothing matches")));
        none->setFlags(Qt::ItemIsEnabled);
        none->setForeground(0, th().textSecondary);
        addTopLevelItem(none);
    }
}

void FileBrowserTree::showTree() {
    if (!m_showingResults) return;
    m_showingResults = false;
    const QStringList open = m_parkedExpanded;
    setRoots(m_roots);
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
    QStringList open;
    QTreeWidgetItemIterator it(const_cast<FileBrowserTree*>(this));
    for (; *it; ++it) {
        if ((*it)->isExpanded()) open << (*it)->data(0, kPathRole).toString();
    }
    return open;
}

void FileBrowserTree::restoreExpanded(const QStringList& paths) {
    if (paths.isEmpty()) return;
    // Expanding fills a node, which can reveal more of the remembered set, so
    // this walks until nothing new opens rather than once over the tree.
    bool opened = true;
    while (opened) {
        opened = false;
        QTreeWidgetItemIterator it(this);
        for (; *it; ++it) {
            QTreeWidgetItem* item = *it;
            if (item->isExpanded()) continue;
            if (Kind(item->data(0, kKindRole).toInt()) != Kind::Folder) continue;
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
