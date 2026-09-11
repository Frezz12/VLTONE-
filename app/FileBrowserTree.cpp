#include "FileBrowserTree.hpp"

#include "ChannelStripPresets.hpp"
#include "FileTypes.hpp"
#include "ProjectTemplates.hpp"

#include <algorithm>
#include <utility>
#include "Icons.hpp"
#include "Theme.hpp"

#include <QApplication>
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
#include <QKeyEvent>
#include <QMenu>
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

QColor tintFor(FileBrowserTree::Kind kind) {
    switch (kind) {
        case FileBrowserTree::Kind::Audio: return Theme::audioAccent();
        case FileBrowserTree::Kind::Midi: return Theme::midiAccent();
        case FileBrowserTree::Kind::ChannelStripPreset:
            return Theme::automationAccent();
        case FileBrowserTree::Kind::Plugin:
        case FileBrowserTree::Kind::ProjectTemplate: return th().accent;
        case FileBrowserTree::Kind::Folder:
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
           kind == FileBrowserTree::Kind::PluginGroup;
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
        group->setIcon(0, browserIcon(icons::Glyph::Folder, th().textSecondary));
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

QTreeWidgetItem* FileBrowserTree::makeItem(const QString& path, bool isDirectory, int cachedKind) {
    const QFileInfo info(path);
    auto* item = new QTreeWidgetItem;
    item->setText(0, info.fileName());
    item->setData(0, kPathRole, info.absoluteFilePath());

    const Kind kind = cachedKind >= 0 ? Kind(cachedKind) : kindOf(info);
    item->setData(0, kKindRole, int(kind));
    item->setIcon(0, browserIcon(kind));

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
    QThreadPool::globalInstance()->start([guard, result, path] {
        QDir dir(path);
        const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                               QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        result->readable = dir.isReadable();
        result->entries.reserve(entries.size());
        for (const auto& entry : entries) {
            if (entry.isSymLink() && !entry.exists()) continue;
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
    for (; result->offset < end; ++result->offset) {
        const auto& [path, kind] = result->entries[result->offset];
        auto* child = makeItem(path, Kind(kind) == Kind::Folder, kind);
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
    // The plugin folders are built whole and are not on disk: nothing to read,
    // and nothing a file-system watcher could usefully watch.
    if (path.startsWith(QLatin1String("daw://"))) return;
    m_restoreExpanded.insert(path);
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
    // A user collapse while enumeration is pending wins over restored state.
    m_restoreExpanded.remove(path);
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
