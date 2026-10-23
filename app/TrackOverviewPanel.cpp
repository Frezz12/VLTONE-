#include "TrackOverviewPanel.hpp"
#include "EngineController.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "TrackIcons.hpp"
#include "UiConstants.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDrag>
#include <QMimeData>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QResizeEvent>
#include <QShortcut>
#include <QSettings>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace {
constexpr int IdRole = Qt::UserRole;
constexpr int ColorRole = Qt::UserRole + 1;
constexpr int AvailableRole = Qt::UserRole + 2;
constexpr int FolderRole = Qt::UserRole + 3;
constexpr std::array<const char*, 3> ControlSettings{
    "ui/trackOverview/showMute", "ui/trackOverview/showSolo", "ui/trackOverview/showLock"};

QColor selectionColor() { return mixColors(th().surface, th().accent, th().dark ? .14 : .10); }
QColor hoverColor() { return mixColors(th().surface, th().textPrimary, th().dark ? .04 : .025); }

std::vector<std::string> idsForController(const QStringList& ids) {
    std::vector<std::string> result;
    for (const auto& id : ids) result.push_back(id.toStdString());
    return result;
}

class TrackDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return {QStyledItemDelegate::sizeHint(option, index).width(),
                std::max(24, option.fontMetrics.height() + 8)};
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto opt = option;
        if (index.column() == 0) {
            QStyledItemDelegate::paint(p, opt, index);
            p->fillRect(QRect(opt.rect.left(), opt.rect.top() + 7, 2, opt.rect.height() - 14),
                        index.data(ColorRole).value<QColor>());
            return;
        }
        const bool on = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const bool available = index.data(AvailableRole).toBool();
        const bool selected = opt.state & QStyle::State_Selected;
        p->save();
        if (selected) p->fillRect(opt.rect, selectionColor());
        else if (opt.state & QStyle::State_MouseOver) p->fillRect(opt.rect, hoverColor());
        // Keep a 24 px hit area while drawing a compact, square control.
        QRect chip(0, 0, 16, 16);
        chip.moveCenter(opt.rect.center());
        const QColor active = index.column() == 1 ? Theme::mute()
                           : index.column() == 2 ? Theme::solo() : th().accent;
        const QColor ink = on ? contrastingTextColor(active)
                              : selected ? th().textPrimary : th().textSecondary;
        p->setPen(Qt::NoPen);
        p->setBrush(on ? active : Qt::transparent);
        p->drawRoundedRect(chip, 3, 3);
        p->setOpacity(available || on ? 1.0 : .35);
        if (index.column() == 3) {
            icons::paint(*p, on ? icons::Glyph::Lock : icons::Glyph::Unlock,
                         QRectF(chip.center().x() - 5, chip.center().y() - 6, 11, 12), ink);
        } else {
            auto font = opt.font; font.setBold(true); p->setFont(font); p->setPen(ink);
            p->drawText(chip, Qt::AlignCenter, index.column() == 1 ? "M" : "S");
        }
        p->restore();
    }
    // Mouse/keyboard activation is handled by the tree as one group command.
    bool editorEvent(QEvent*, QAbstractItemModel*, const QStyleOptionViewItem&, const QModelIndex&) override {
        return false;
    }
};

class OverviewTree final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    static constexpr int OnTrack = OnItem;
    static constexpr int AboveTrack = AboveItem;
    std::function<void(QTreeWidgetItem*, int)> toggle;
    std::function<void()> find;
    std::function<void(int, Qt::KeyboardModifiers)> key;
    std::function<bool(QTreeWidgetItem*, int, bool)> drop;
protected:
    void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->matches(QKeySequence::Find)) {
                event->accept(); return true;
            }
            if (keyEvent->modifiers() == Qt::NoModifier &&
                (keyEvent->key() == Qt::Key_M || keyEvent->key() == Qt::Key_S || keyEvent->key() == Qt::Key_L)) {
                event->accept(); return true;
            }
        }
        return QTreeWidget::event(event);
    }
    void mousePressEvent(QMouseEvent* event) override {
        m_dragConsumed = false;
        const auto index = indexAt(event->position().toPoint());
        m_folderPressed = {};
        if (event->button() == Qt::LeftButton && index.isValid() && index.column() > 0) {
            m_pressed = QPersistentModelIndex(index);
            setFocus(Qt::MouseFocusReason);
            event->accept();
            return;
        }
        m_pressed = {};
        if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier &&
            index.column() == 0 && index.data(FolderRole).toBool()) {
            m_folderPressed = index;
            m_pressPosition = event->position().toPoint();
        }
        QTreeWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        // A native drag owns the rest of this press. Rebuilding the tree on
        // drop invalidates Qt's pressed index; a trailing move would otherwise
        // start a new range selection from the old screen position.
        if (m_pressed.isValid() || (m_dragConsumed && event->buttons() != Qt::NoButton)) {
            event->accept(); return;
        }
        if ((event->position().toPoint() - m_pressPosition).manhattanLength() >= QApplication::startDragDistance())
            m_folderPressed = {};
        QTreeWidget::mouseMoveEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        const auto index = indexAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && index.column() == 0 && index.data(FolderRole).toBool()) {
            m_folderPressed = {}; event->accept(); return;
        }
        QTreeWidget::mouseDoubleClickEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (m_dragConsumed) { event->accept(); return; }
        const auto index = indexAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && m_pressed.isValid()) {
            const bool hit = m_pressed == index;
            m_pressed = {};
            if (hit && toggle) toggle(itemFromIndex(index), index.column());
            event->accept();
            return;
        }
        const auto folder = m_folderPressed;
        m_folderPressed = {};
        QTreeWidget::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && folder.isValid() && folder == index &&
            event->modifiers() == Qt::NoModifier) {
            if (auto* item = itemFromIndex(folder)) item->setExpanded(!item->isExpanded());
        }
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->matches(QKeySequence::Find)) {
            if (find) find();
            event->accept(); return;
        }
        if (event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_M || event->key() == Qt::Key_S || event->key() == Qt::Key_L)) {
            if (toggle) toggle(currentItem(), event->key() == Qt::Key_M ? 1 : event->key() == Qt::Key_S ? 2 : 3);
            event->accept(); return;
        }
        if (event->key() == Qt::Key_Space && currentColumn() > 0) {
            if (toggle) toggle(currentItem(), currentColumn());
            event->accept(); return;
        }
        if (event->key() == Qt::Key_F2 || event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter ||
            ((event->modifiers() & Qt::AltModifier) &&
             (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down))) {
            if (key) key(event->key(), event->modifiers());
            event->accept(); return;
        }
        QTreeWidget::keyPressEvent(event);
    }
    void startDrag(Qt::DropActions) override {
        m_folderPressed = {};
        if (selectedItems().isEmpty()) return;
        m_dragConsumed = true;
        // QAbstractItemView's default source removes rows after a successful
        // move. Our controller has already moved/rebuilt them at that point.
        QDrag drag(this);
        drag.setMimeData(model()->mimeData(selectedIndexes()));
        QPixmap preview(196, 30); preview.fill(Qt::transparent);
        {
            QPainter painter(&preview);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(th().accent); painter.setBrush(th().surfaceElevated);
            painter.drawRoundedRect(preview.rect().adjusted(0, 0, -1, -1), 5, 5);
            painter.setPen(th().textPrimary);
            const QString title = selectedItems().size() == 1 ? selectedItems().first()->text(0)
                : TrackOverviewPanel::tr("Selected: %1").arg(selectedItems().size());
            painter.drawText(preview.rect().adjusted(10, 0, -10, 0), Qt::AlignVCenter,
                             fontMetrics().elidedText(title, Qt::ElideRight, 176));
        }
        drag.setPixmap(preview); drag.setHotSpot(QPoint(18, 15));
        drag.exec(Qt::MoveAction, Qt::MoveAction);
        // Our drop bypasses the base implementation's row mutation. Still
        // clear its hover/auto-scroll state for both successful and cancelled drags.
        QDragLeaveEvent leave;
        QTreeWidget::dragLeaveEvent(&leave);
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        QTreeWidget::dragMoveEvent(event);
        if (event->source() != this || !drop ||
            !drop(itemAt(event->position().toPoint()), int(dropIndicatorPosition()), false))
            event->ignore();
    }
    void dropEvent(QDropEvent* event) override {
        // The controller owns the hierarchy; never let QTreeWidget move a
        // second copy of it or implicitly make an audio track into a folder.
        if (event->source() == this && drop &&
            drop(itemAt(event->position().toPoint()), int(dropIndicatorPosition()), true)) {
            event->setDropAction(Qt::MoveAction); event->accept();
        } else event->ignore();
    }
private:
    QPersistentModelIndex m_pressed;
    QPersistentModelIndex m_folderPressed;
    QPoint m_pressPosition;
    bool m_dragConsumed = false;
};
}

TrackOverviewPanel::TrackOverviewPanel(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("TrackOverviewPanel");
    setMinimumWidth(180);
    for (int i = 0; i < 3; ++i) m_visibleControls[i] = QSettings().value(ControlSettings[i], true).toBool();
    setAccessibleName(tr("Tracks"));
    setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 0, 2, 0); layout->setSpacing(0);
    auto* header = new QWidget(this);
    header->setObjectName("TrackOverviewHeader"); header->setFixedHeight(ui::kRulerHeight);
    auto* top = new QHBoxLayout(header);
    top->setContentsMargins(6, 0, 3, 0); top->setSpacing(3);
    auto* title = new QLabel(tr("Tracks"), header);
    auto titleFont = title->font(); titleFont.setBold(true); title->setFont(titleFont);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    top->addWidget(title, 1);
    m_count = new QLabel(header); m_count->setObjectName("TrackOverviewCount");
    top->addWidget(m_count);
    auto* actions = new QToolButton(header);
    actions->setObjectName("TrackOverviewActions"); actions->setText(QStringLiteral("…"));
    actions->setToolTip(tr("Track actions")); actions->setAccessibleName(tr("Track actions"));
    actions->setFixedSize(24, 24); top->addWidget(actions);
    auto* close = new ui::IconButton(icons::Glyph::Close, tr("Hide tracks"), header);
    close->setButtonSize(24, 24); top->addWidget(close);
    connect(close, &QAbstractButton::clicked, this, &TrackOverviewPanel::hideRequested);
    connect(actions, &QAbstractButton::clicked, this, [this, actions] {
        QMenu menu(this); populateMenu(menu, true); menu.exec(actions->mapToGlobal(QPoint(0, actions->height())));
    });
    layout->addWidget(header);
    auto* searchRow = new QWidget(this);
    auto* searchLayout = new QHBoxLayout(searchRow); searchLayout->setContentsMargins(6, 6, 6, 6);
    m_search = new QLineEdit(searchRow); m_search->setObjectName("TrackOverviewSearch");
    m_search->setPlaceholderText(tr("Find tracks…")); m_search->setAccessibleName(tr("Find tracks"));
    m_search->setClearButtonEnabled(true); m_search->setMinimumHeight(24);
    m_search->setMinimumWidth(0); m_search->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    searchLayout->addWidget(m_search); layout->addWidget(searchRow);
    auto* tree = new OverviewTree(this); m_tree = tree;
    tree->setObjectName("TrackOverviewTree"); tree->setAccessibleName(tr("Track hierarchy"));
    tree->setColumnCount(4); tree->setHeaderLabels({tr("Name"), "M", "S", tr("Lock")});
    tree->setHeaderHidden(true); tree->setRootIsDecorated(false); tree->setIndentation(6);
    tree->setExpandsOnDoubleClick(false);
    tree->setUniformRowHeights(true); tree->setAnimated(false); tree->setMouseTracking(true);
    tree->setIconSize(QSize(14, 14)); tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tree->setDragDropMode(QAbstractItemView::InternalMove); tree->setDefaultDropAction(Qt::MoveAction);
    tree->setDropIndicatorShown(true); tree->setAutoExpandDelay(600);
    tree->setItemDelegate(new TrackDelegate(tree));
    tree->header()->setStretchLastSection(false);
    tree->header()->setMinimumSectionSize(24);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column) {
        tree->header()->setSectionResizeMode(column, QHeaderView::Fixed); tree->setColumnWidth(column, 24);
    }
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (auto* item = m_tree->itemAt(pos); item && !item->isSelected()) {
            const QSignalBlocker blocker(m_tree);
            m_tree->clearSelection(); item->setSelected(true); m_tree->setCurrentItem(item);
        }
        publishSelection();
        QMenu menu(this); populateMenu(menu); menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });
    connect(tree, &QTreeWidget::itemSelectionChanged, this, &TrackOverviewPanel::publishSelection);
    connect(tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current) {
        if (current && current->isSelected()) publishSelection();
    });
    connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        // Native accessibility actions change CheckStateRole directly.
        if (!m_refreshing && column > 0 && column < 4)
            applyState(item, column, item->checkState(column) == Qt::Checked);
    });
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column == 0 && !item->data(0, FolderRole).toBool()) emit revealRequested(item->data(0, IdRole).toString());
    });
    connect(tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (!m_refreshing && m_search->text().isEmpty()) m_collapsed.insert(item->data(0, IdRole).toString());
    });
    connect(tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!m_refreshing && m_search->text().isEmpty()) m_collapsed.remove(item->data(0, IdRole).toString());
    });
    tree->toggle = [this](auto* item, int column) { toggleState(item, column); };
    tree->find = [this] { showSearch(); };
    tree->key = [this](int key, Qt::KeyboardModifiers) {
        if (key == Qt::Key_F2) renameSelected();
        else if (key == Qt::Key_Up || key == Qt::Key_Down) moveBy(key == Qt::Key_Up ? -1 : 1);
        else if (m_tree->currentItem()) emit revealRequested(m_tree->currentItem()->data(0, IdRole).toString());
    };
    tree->drop = [this](QTreeWidgetItem* item, int position, bool commit) {
        if (!m_search->text().isEmpty()) return false;
        QString parent;
        size_t boundary = m_controller->project().tracks.size();
        if (item) {
            const QString id = item->data(0, IdRole).toString();
            const auto* track = m_controller->project().findTrack(id.toStdString());
            if (!track) return false;
            if (position == OverviewTree::OnTrack) {
                if (track->kind != daw::TrackKind::Folder) return false;
                parent = id; boundary = afterSubtree(id);
            } else {
                parent = QString::fromStdString(track->parentId);
                boundary = position == OverviewTree::AboveTrack
                    ? m_controller->project().indexOf(track->id) : afterSubtree(id);
            }
        }
        return commit ? moveSelection(parent, boundary) : canMove(selectedTrackIds(), parent);
    };
    layout->addWidget(tree, 1);
    m_summary = new QLabel(this); m_summary->setObjectName("TrackOverviewSummary");
    m_summary->setMinimumWidth(0); m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_summary->setContentsMargins(8, 5, 4, 5); layout->addWidget(m_summary);
    connect(m_search, &QLineEdit::textChanged, this, &TrackOverviewPanel::filterTracks);
    auto* dismissSearch = new QShortcut(QKeySequence(Qt::Key_Escape), m_search);
    dismissSearch->setContext(Qt::WidgetShortcut);
    connect(dismissSearch, &QShortcut::activated, this, [this] {
        m_search->clear(); m_tree->setFocus();
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { applyTheme(); refresh(); });
    connect(&ThemeManager::instance(), &ThemeManager::trackIconsChanged, this, &TrackOverviewPanel::refresh);
    applyTheme(); refresh(); updateControlVisibility();
}

void TrackOverviewPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_summary) updateSummary();
}

void TrackOverviewPanel::updateControlVisibility() {
    for (int column = 1; column < 4; ++column)
        m_tree->setColumnHidden(column, !m_visibleControls[column - 1]);
    m_tree->viewport()->update();
}

void TrackOverviewPanel::setControlVisible(int column, bool visible) {
    m_visibleControls[column - 1] = visible;
    QSettings().setValue(ControlSettings[column - 1], visible);
    updateControlVisibility();
}

void TrackOverviewPanel::showSearch() {
    m_search->setFocus(); m_search->selectAll();
}

void TrackOverviewPanel::applyTheme() {
    setStyleSheet(QStringLiteral(
        "#TrackOverviewPanel { background:%1; border-right:1px solid %2; }"
        "#TrackOverviewHeader { background:%3; border-bottom:1px solid %2; }"
        "#TrackOverviewActions { color:%4; background:transparent; border:0; border-radius:4px; font-weight:bold; }"
        "#TrackOverviewActions:hover { color:%6; background:%5; }"
        "#TrackOverviewCount, #TrackOverviewSummary { color:%4; }"
        "#TrackOverviewSearch { background:%5; color:%6; border:1px solid %2; border-radius:5px; padding:2px 5px; }"
        "#TrackOverviewTree { background:%1; color:%6; border:0; outline:0;"
        " selection-background-color:%7; selection-color:%6; }"
        "#TrackOverviewTree::item { padding:0 2px; }"
        "#TrackOverviewTree::item:selected { background:%7; color:%6; }"
        "#TrackOverviewTree::item:hover:!selected { background:%8; }")
        .arg(th().surface.name(), th().separator().name(), th().panelTop().name(),
             th().textSecondary.name(), th().surfaceElevated.name(), th().textPrimary.name(),
             selectionColor().name(), hoverColor().name()));
    auto palette = m_tree->palette();
    palette.setColor(QPalette::Highlight, selectionColor());
    palette.setColor(QPalette::HighlightedText, th().textPrimary);
    m_tree->setPalette(palette);
}

QStringList TrackOverviewPanel::selectedTrackIds() const {
    QStringList result;
    // Document order is stable even if Ctrl-clicks arrived in another order.
    // Read Qt's selection ranges instead of converting every track ID on
    // every current-item/selection signal in a large project.
    const auto& project = m_controller->project();
    for (const auto& index : m_tree->selectionModel()->selectedRows(0)) {
        const auto id = index.data(IdRole).toString();
        if (project.findTrack(id.toStdString())) result.push_back(id);
    }
    std::sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
        return project.indexOf(a.toStdString()) < project.indexOf(b.toStdString());
    });
    return result;
}

void TrackOverviewPanel::setSelectedTracks(const QStringList& ids, const QString& primary) {
    // The workspace echoes selection synchronously. Keep the native range
    // anchor while this view is the source of that selection.
    if (m_publishingSelection) return;
    const QString lead = primary.isEmpty() ? ids.value(0) : primary;
    const QString current = m_tree->currentItem() ? m_tree->currentItem()->data(0, IdRole).toString() : QString{};
    if (selectedTrackIds() == ids && current == lead) { updateSummary(); return; }
    const QSignalBlocker blocker(m_tree);
    m_tree->clearSelection();
    auto* currentItem = m_items.value(lead);
    m_tree->setCurrentItem(currentItem, 0, QItemSelectionModel::NoUpdate);
    for (const auto& id : ids) if (auto* item = m_items.value(id)) item->setSelected(true);
    updateSummary();
}

void TrackOverviewPanel::publishSelection() {
    if (m_refreshing) return;
    const QStringList ids = selectedTrackIds();
    QString primary = m_tree->currentItem() ? m_tree->currentItem()->data(0, IdRole).toString() : QString{};
    if (!ids.contains(primary)) primary = ids.value(0);
    updateSummary();
    const QScopedValueRollback<bool> publishing(m_publishingSelection, true);
    emit selectionChanged(ids, primary);
}

void TrackOverviewPanel::refresh() {
    m_refreshing = true;
    const QSignalBlocker blocker(m_tree);
    const auto selection = selectedTrackIds();
    const QString primary = m_tree->currentItem() ? m_tree->currentItem()->data(0, IdRole).toString() : QString{};
    const int scroll = m_tree->verticalScrollBar()->value();
    QStringList structure;
    for (const auto& track : m_controller->project().tracks)
        structure << QString::fromStdString(track.id) << QString::fromStdString(track.parentId);
    if (structure != m_structure) {
        m_tree->clear(); m_items.clear(); m_structure = structure;
        for (const auto& track : m_controller->project().tracks)
            m_items.insert(QString::fromStdString(track.id), new QTreeWidgetItem);
        for (const auto& track : m_controller->project().tracks) {
            auto* item = m_items.value(QString::fromStdString(track.id));
            auto* parent = m_items.value(QString::fromStdString(track.parentId));
            if (parent && parent != item) parent->addChild(item); else m_tree->addTopLevelItem(item);
        }
        m_collapsed.intersect(QSet<QString>(m_items.keyBegin(), m_items.keyEnd()));
    }
    for (const auto& track : m_controller->project().tracks) {
        const QString id = QString::fromStdString(track.id);
        auto* item = m_items.value(id);
        const bool folder = daw::isFolder(track);
        const bool locked = m_controller->isTrackEditLocked(track.id);
        item->setData(0, IdRole, id); item->setData(0, ColorRole, colorFromRgb(track.color));
        item->setData(0, FolderRole, folder);
        item->setText(0, QString::fromStdString(track.name));
        auto font = m_tree->font(); font.setBold(folder); item->setFont(0, font);
        auto icon = ui::trackicons::icon(QString::fromStdString(track.iconId), colorFromRgb(track.color));
        if (icon.isNull()) icon = icons::icon(folder ? (track.summing || track.kind == daw::TrackKind::Pattern
            ? icons::Glyph::FolderSum : icons::Glyph::Folder) : track.kind == daw::TrackKind::Automation
            ? icons::Glyph::Automation : track.kind == daw::TrackKind::Audio
            ? icons::Glyph::Waveform : track.kind == daw::TrackKind::Master
            ? icons::Glyph::Mixer : icons::Glyph::MidiKeys, colorFromRgb(track.color));
        item->setIcon(0, icon);
        auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
        if (!locked && track.kind != daw::TrackKind::Master && !daw::isAutomationLane(track)) flags |= Qt::ItemIsDragEnabled;
        if (!locked && track.kind == daw::TrackKind::Folder) flags |= Qt::ItemIsDropEnabled;
        item->setFlags(flags);
        item->setCheckState(1, track.muted ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(2, track.soloed ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(3, locked ? Qt::Checked : Qt::Unchecked);
        const bool mixable = !locked && (daw::carriesAudio(track) || folder);
        item->setData(1, AvailableRole, mixable); item->setData(2, AvailableRole, mixable);
        const bool lockable = track.kind != daw::TrackKind::Master && !m_controller->hasCloudProjectBinding() &&
            !m_controller->isRecording() && !m_controller->offlineRenderInProgress() && !m_controller->isTrackEditLocked(track.parentId);
        item->setData(3, AvailableRole, lockable);
        item->setToolTip(0, QString::fromStdString(track.name) +
            (folder ? tr("\nClick to expand or collapse") : QString{}) +
            (locked ? tr("\nEditing locked") : QString{}));
        item->setToolTip(1, tr("Mute — applies to selected tracks"));
        item->setToolTip(2, tr("Solo — applies to selected tracks"));
        item->setToolTip(3, m_controller->hasCloudProjectBinding() ? tr("Editing locks are available in local projects") :
            m_controller->isTrackEditLocked(track.parentId) ? tr("Unlock the parent folder first") :
            tr("Lock track editing, including volume, pan, plugins, Mute and Solo."));
        for (int column = 1; column < 4; ++column)
            item->setData(column, Qt::AccessibleTextRole,
                (column == 1 ? tr("Mute") : column == 2 ? tr("Solo") : tr("Editing lock")) + ": " + item->text(0));
    }
    m_count->setText(QString::number(m_items.size()));
    setSelectedTracks(selection, primary); filterTracks();
    m_tree->verticalScrollBar()->setValue(scroll);
    m_refreshing = false;
}

void TrackOverviewPanel::filterTracks() {
    const QSignalBlocker blocker(m_tree);
    const QString query = m_search->text().trimmed();
    const auto visit = [&](auto&& self, QTreeWidgetItem* item, bool parentMatch) -> bool {
        const bool match = query.isEmpty() || parentMatch || item->text(0).contains(query, Qt::CaseInsensitive);
        bool childMatch = false;
        for (int i = 0; i < item->childCount(); ++i) childMatch |= self(self, item->child(i), match);
        item->setHidden(!match && !childMatch);
        item->setExpanded(query.isEmpty() ? !m_collapsed.contains(item->data(0, IdRole).toString()) : childMatch);
        return match || childMatch;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) visit(visit, m_tree->topLevelItem(i), false);
    // A filtered-out row must not be silently included in a subsequent command.
    bool changed = false;
    if (!m_refreshing) for (auto* item : m_items) {
        if (item->isSelected() && item->isHidden()) { item->setSelected(false); changed = true; }
    }
    updateSummary();
    if (changed) publishSelection();
}

void TrackOverviewPanel::updateSummary() {
    const int selected = m_tree->selectionModel()->selectedRows(0).size();
    int visible = 0;
    if (!selected) for (auto* item : m_items) if (!item->isHidden()) ++visible;
    const QString summary = selected ? tr("Selected: %1").arg(selected)
        : visible == 0 ? (m_items.isEmpty() ? tr("Add tracks to get started") : tr("No matching tracks"))
        : tr("%1 tracks").arg(visible);
    m_summary->setText(m_summary->fontMetrics().elidedText(summary, Qt::ElideRight, std::max(0, width() - 12)));
    m_summary->setToolTip(summary);
}

QStringList TrackOverviewPanel::actionTargets(QTreeWidgetItem* item) const {
    if (!item) return {};
    return item->isSelected() ? selectedTrackIds() : QStringList{item->data(0, IdRole).toString()};
}

void TrackOverviewPanel::toggleState(QTreeWidgetItem* item, int column) {
    if (item) applyState(item, column, item->checkState(column) != Qt::Checked);
}

void TrackOverviewPanel::applyState(QTreeWidgetItem* item, int column, bool on) {
    if (!item || !item->data(column, AvailableRole).toBool() || !m_controller->sharedEditingAllowed()) {
        refresh(); return;
    }
    const auto ids = idsForController(actionTargets(item));
    bool dirty = !m_controller->hasCloudProjectBinding();
    if (column == 1) dirty = daw::collab::marksLocalFileDirty(m_controller->setTracksMuted(ids, on));
    else if (column == 2) {
        for (const auto& id : ids) m_controller->setTrackSoloed(id, on);
    } else if (column == 3) {
        if (!m_controller->setTracksEditLocked(ids, on)) { refresh(); return; }
    } else return;
    refresh(); emit edited(false, dirty);
}

size_t TrackOverviewPanel::afterSubtree(const QString& id) const {
    const auto& project = m_controller->project();
    size_t boundary = project.indexOf(id.toStdString());
    if (boundary == std::string::npos) return project.tracks.size();
    ++boundary;
    for (const auto& child : daw::subtreeOf(project, id.toStdString()))
        boundary = std::max(boundary, project.indexOf(child) + 1);
    return boundary;
}

bool TrackOverviewPanel::selectionEditable() const {
    if (selectedTrackIds().isEmpty() || !m_controller->sharedEditingAllowed() || m_controller->isRecording()) return false;
    const auto ids = selectedTrackIds();
    const auto& project = m_controller->project();
    for (const auto& id : ids) {
        const auto* track = project.findTrack(id.toStdString());
        if (!track) return false;
        const bool carriedByParent = std::any_of(ids.begin(), ids.end(), [&](const QString& other) {
            return other != id && daw::isDescendantOf(project, track->id, other.toStdString());
        });
        if (carriedByParent) continue;
        const auto* owner = project.findTrack(track->parentId);
        if (track->kind == daw::TrackKind::Master || daw::isAutomationLane(*track) ||
            (owner && owner->kind == daw::TrackKind::Pattern) ||
            m_controller->isTrackTreeEditLocked(track->id)) return false;
    }
    return true;
}

bool TrackOverviewPanel::canMove(const QStringList& ids, const QString& parentId) const {
    if (!selectionEditable() || ids.isEmpty()) return false;
    const auto& project = m_controller->project();
    const auto parent = parentId.toStdString();
    if (!parent.empty()) {
        const auto* track = project.findTrack(parent);
        if (!track || track->kind != daw::TrackKind::Folder || m_controller->isTrackEditLocked(parent)) return false;
    }
    for (const auto& id : ids)
        if (id == parentId || daw::isDescendantOf(project, parent, id.toStdString())) return false;
    return true;
}

bool TrackOverviewPanel::moveSelection(const QString& parentId, size_t boundary) {
    const auto ids = selectedTrackIds();
    if (!canMove(ids, parentId)) return false;
    if (!m_controller->moveTracks(idsForController(ids), boundary, parentId.toStdString())) return false;
    m_collapsed.remove(parentId); refresh();
    emit edited(true, !m_controller->hasCloudProjectBinding());
    return true;
}

void TrackOverviewPanel::renameSelected() {
    const auto ids = selectedTrackIds();
    if (ids.size() != 1 || !m_controller->sharedEditingAllowed()) return;
    const auto* track = m_controller->project().findTrack(ids.front().toStdString());
    if (!track || m_controller->isTrackEditLocked(track->id)) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("Rename track"), tr("Name"),
        QLineEdit::Normal, QString::fromStdString(track->name), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    const auto result = m_controller->renameTrack(ids.front().toStdString(), name.toStdString());
    refresh(); emit edited(false, daw::collab::marksLocalFileDirty(result));
}

void TrackOverviewPanel::packSelection(bool summing) {
    if (!selectionEditable()) return;
    const auto id = m_controller->packIntoFolder(idsForController(selectedTrackIds()),
        (summing ? tr("Summing folder") : tr("Folder")).toStdString(), summing);
    if (id.empty()) return;
    refresh(); emit edited(true, !m_controller->hasCloudProjectBinding());
    setSelectedTracks({QString::fromStdString(id)}); publishSelection();
}

void TrackOverviewPanel::moveBy(int direction) {
    const auto ids = selectedTrackIds();
    if (ids.isEmpty()) return;
    const auto& project = m_controller->project();
    const auto* first = project.findTrack(ids.first().toStdString());
    if (!first) return;
    QStringList siblings;
    for (const auto& track : project.tracks) if (track.parentId == first->parentId)
        siblings.push_back(QString::fromStdString(track.id));
    // Ignore descendants already carried by a selected root.
    QStringList roots;
    for (const auto& id : ids) {
        const auto* track = project.findTrack(id.toStdString());
        bool covered = false;
        for (const auto& other : ids) covered |= other != id && daw::isDescendantOf(project, id.toStdString(), other.toStdString());
        if (covered) continue;
        if (!track || track->parentId != first->parentId) return;
        roots.push_back(id);
    }
    if (roots.isEmpty()) return;
    const int neighbor = siblings.indexOf(direction < 0 ? roots.first() : roots.last()) + direction;
    if (neighbor < 0 || neighbor >= siblings.size()) return;
    moveSelection(QString::fromStdString(first->parentId), direction < 0
        ? project.indexOf(siblings[neighbor].toStdString()) : afterSubtree(siblings[neighbor]));
}

void TrackOverviewPanel::populateMenu(QMenu& menu, bool includeDisplayOptions) {
    menu.setToolTipsVisible(true);
    if (includeDisplayOptions) {
        for (int column = 1; column <= 3; ++column) {
            auto* action = menu.addAction(column == 1 ? tr("Show Mute") : column == 2
                ? tr("Show Solo") : tr("Show editing lock"));
            action->setObjectName(QString("overview.showControl%1").arg(column));
            action->setCheckable(true); action->setChecked(m_visibleControls[column - 1]);
            connect(action, &QAction::toggled, this, [this, column](bool visible) {
                setControlVisible(column, visible);
            });
        }
        menu.addSeparator();
    }
    const auto ids = selectedTrackIds();
    const bool editable = selectionEditable();
    const auto add = [&](const QString& text, const char* name, bool enabled, auto callback) {
        auto* action = menu.addAction(text); action->setObjectName(name); action->setEnabled(enabled);
        connect(action, &QAction::triggered, this, callback); return action;
    };
    add(tr("Show on timeline"), "overview.reveal", !ids.isEmpty(), [this, ids] { emit revealRequested(ids.first()); });
    add(tr("Rename…"), "overview.rename", ids.size() == 1 && editable, [this] { renameSelected(); });
    menu.addSeparator();
    for (int column = 1; column <= 3; ++column) {
        const bool allOn = !ids.isEmpty() && std::all_of(ids.begin(), ids.end(), [this, column](const QString& id) {
            return m_items.value(id)->checkState(column) == Qt::Checked;
        });
        const QString text = column == 1 ? (allOn ? tr("Unmute selected") : tr("Mute selected"))
            : column == 2 ? (allOn ? tr("Unsolo selected") : tr("Solo selected"))
            : allOn ? tr("Unlock editing") : tr("Lock editing");
        auto* action = add(text, column == 1 ? "overview.mute" : column == 2 ? "overview.solo" : "overview.lock",
            !ids.isEmpty() && m_controller->sharedEditingAllowed() &&
                (column != 3 || (!m_controller->hasCloudProjectBinding() && !m_controller->isRecording() && !m_controller->offlineRenderInProgress())),
            [this, ids, column, allOn] {
                // Drive from a row with the intended state, without changing selection.
                for (const auto& id : ids) if (auto* item = m_items.value(id);
                    item && (item->checkState(column) == Qt::Checked) == allOn && item->data(column, AvailableRole).toBool()) {
                    toggleState(item, column); break;
                }
            });
        if (column == 3 && m_controller->hasCloudProjectBinding()) action->setToolTip(tr("Editing locks are available in local projects"));
    }
    menu.addSeparator();
    auto* move = menu.addMenu(tr("Move to folder")); move->setObjectName("overview.moveTo"); move->setEnabled(editable);
    auto* root = move->addAction(tr("Top level")); root->setObjectName("overview.moveRoot");
    root->setEnabled(canMove(ids, {}));
    connect(root, &QAction::triggered, this, [this] { moveSelection({}, m_controller->project().tracks.size()); });
    for (const auto& track : m_controller->project().tracks) {
        if (track.kind != daw::TrackKind::Folder) continue;
        const auto id = QString::fromStdString(track.id);
        QStringList path{QString::fromStdString(track.name)};
        auto parent = track.parentId;
        for (size_t guard = 0; !parent.empty() && guard < m_controller->project().tracks.size(); ++guard) {
            const auto* ancestor = m_controller->project().findTrack(parent); if (!ancestor) break;
            path.prepend(QString::fromStdString(ancestor->name)); parent = ancestor->parentId;
        }
        auto* action = move->addAction(icons::icon(track.summing ? icons::Glyph::FolderSum : icons::Glyph::Folder,
            colorFromRgb(track.color)), path.join(" / "));
        action->setData(id); action->setEnabled(canMove(ids, id));
        connect(action, &QAction::triggered, this, [this, id] { moveSelection(id, afterSubtree(id)); });
    }
    add(tr("New folder from selection"), "overview.pack", editable, [this] { packSelection(false); });
    add(tr("New summing folder from selection"), "overview.packSumming", editable, [this] { packSelection(true); });
    if (ids.size() == 1) if (const auto* track = m_controller->project().findTrack(ids.first().toStdString());
        track && track->kind == daw::TrackKind::Folder) {
        const bool summing = track->summing;
        add(summing ? tr("Convert to regular folder") : tr("Convert to summing folder"), "overview.summing", editable,
            [this, ids, summing] {
                m_controller->setFolderSumming(ids.first().toStdString(), !summing);
                refresh(); emit edited(true, !m_controller->hasCloudProjectBinding());
            });
    }
    add(tr("Move up"), "overview.up", editable && m_search->text().isEmpty(), [this] { moveBy(-1); });
    add(tr("Move down"), "overview.down", editable && m_search->text().isEmpty(), [this] { moveBy(1); });
    menu.addSeparator();
    add(tr("Expand all"), "overview.expand", !m_items.isEmpty(), [this] { m_collapsed.clear(); filterTracks(); });
    add(tr("Collapse all"), "overview.collapse", !m_items.isEmpty(), [this] {
        for (auto* item : m_items) if (item->childCount()) m_collapsed.insert(item->data(0, IdRole).toString());
        filterTracks();
    });
    menu.addSeparator();
    auto* create = menu.addMenu(tr("Add track"));
    for (const auto& [kind, name] : std::initializer_list<std::pair<daw::TrackKind, QString>>{
             {daw::TrackKind::Audio, tr("Audio track")}, {daw::TrackKind::Instrument, tr("Instrument track")},
             {daw::TrackKind::Folder, tr("Folder")}}) {
        auto* action = create->addAction(name); action->setEnabled(m_controller->sharedEditingAllowed());
        connect(action, &QAction::triggered, this, [this, kind, name] {
            const auto id = kind == daw::TrackKind::Folder ? m_controller->addFolder(false, name.toStdString())
                : m_controller->addTrack(kind, name.toStdString());
            if (id.empty()) return;
            refresh(); emit edited(true, !m_controller->hasCloudProjectBinding());
            setSelectedTracks({QString::fromStdString(id)}); publishSelection();
        });
    }
    add(tr("More track actions…"), "overview.more", !ids.isEmpty(), [this] { emit moreActionsRequested(QCursor::pos()); });
    menu.addSeparator();
    add(tr("Find tracks…"), "overview.find", true, [this] { showSearch(); });
    add(tr("Hide tracks"), "overview.hide", true, [this] { emit hideRequested(); });
}
