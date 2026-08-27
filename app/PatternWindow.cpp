#include "PatternWindow.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"
#include "FileTypes.hpp"
#include "Icons.hpp"
#include "PluginPickerMenu.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

constexpr int kRowHeight = 54;
constexpr int kMaxSketchNotes = 768;

/// The row background is a selection surface. Child controls keep their own
/// gestures; only presses that land on the exposed grey surface reach this
/// widget, so selecting/reordering cannot steal a fader or open-button drag.
class PatternSourceRow final : public QWidget {
public:
    using Press = std::function<void(const QString&, const QPoint&,
                                     Qt::KeyboardModifiers)>;
    using Move = std::function<void(const QPoint&)>;
    using Release = std::function<void(const QString&, const QPoint&)>;
    using Menu = std::function<void(const QString&, const QPoint&)>;

    explicit PatternSourceRow(QString trackId, QWidget* parent)
        : QWidget(parent), m_trackId(std::move(trackId)) {
        setProperty("trackId", m_trackId);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
    }

    Press onPress;
    Move onMove;
    Release onRelease;
    Menu onMenu;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (onPress) {
            onPress(m_trackId, event->globalPosition().toPoint(),
                    event->modifiers());
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) {
            if (onMove) onMove(event->globalPosition().toPoint());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            if (onRelease)
                onRelease(m_trackId, event->globalPosition().toPoint());
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        if (onMenu) onMenu(m_trackId, event->globalPos());
        event->accept();
    }

private:
    QString m_trackId;
};

QColor rgb(uint32_t value) {
    return QColor(int((value >> 16) & 0xff), int((value >> 8) & 0xff),
                  int(value & 0xff));
}

/// One row's miniature arrangement. It deliberately draws every MIDI clip on
/// the source, rather than mirroring only the first clip, so the Pattern stays
/// useful as a structural overview after it grows beyond a one-bar loop.
class SourceSketch final : public QAbstractButton {
public:
    SourceSketch(daw::EngineController* controller, QString trackId,
                 QWidget* parent)
        : QAbstractButton(parent), m_controller(controller),
          m_trackId(std::move(trackId)) {
        setCursor(Qt::PointingHandCursor);
        setMinimumWidth(180);
        setFixedHeight(34);
        setAccessibleName(QObject::tr("Open this source in the piano roll"));
        setToolTip(QObject::tr("Open piano roll"));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        const auto* track = m_controller
                                ? m_controller->project().findTrack(
                                      m_trackId.toStdString())
                                : nullptr;
        const QColor accent = track ? rgb(track->color) : t.accent;
        QColor fill = mixColors(t.well(), accent, isDown() ? 0.18 : 0.07);
        if (underMouse()) fill = mixColors(fill, accent, 0.08);
        p.setBrush(fill);
        p.setPen(QPen(mixColors(t.separator(), accent, underMouse() ? 0.55 : 0.28),
                      hasFocus() ? 1.8 : 1.0));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
        if (!track) return;

        const QRectF area = QRectF(rect()).adjusted(7, 5, -7, -5);
        const double tempo = m_controller->project().tempo;
        ensureNoteGeometry(*track, area, tempo);
        if (m_cachedNoteCount == 0) {
            p.setPen(t.textSecondary);
            QFont font = p.font();
            font.setPixelSize(9);
            p.setFont(font);
            p.drawText(rect().adjusted(10, 0, -10, 0),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QObject::tr("Click to draw MIDI"));
            return;
        }

        p.setPen(Qt::NoPen);
        p.setBrush(mixColors(accent, t.textPrimary, 0.46));
        if (m_cachedUseLod) p.setRenderHint(QPainter::Antialiasing, false);
        p.drawPath(m_cachedNotes);
    }

private:
    void ensureNoteGeometry(const daw::TrackModel& track, const QRectF& area,
                            double tempo) {
        const std::uint64_t revision =
            m_controller->midiNotesRevision(track.id);
        if (m_cachedRevision == revision && m_cachedSize == size() &&
            m_cachedTempo == tempo) {
            return;
        }

        m_cachedRevision = revision;
        m_cachedSize = size();
        m_cachedTempo = tempo;
        m_cachedNotes = {};
        m_cachedNoteCount = 0;

        double end = 0.0;
        int low = 127;
        int high = 0;
        for (const auto& clip : track.clips) {
            if (clip.kind != daw::ClipKind::Midi) continue;
            end = std::max(end, clip.startSeconds + clip.durationSeconds);
            for (const auto& note : clip.notes) {
                low = std::min(low, note.pitch);
                high = std::max(high, note.pitch);
                ++m_cachedNoteCount;
            }
        }
        if (m_cachedNoteCount == 0) {
            m_cachedUseLod = false;
            return;
        }

        end = std::max(end, daw::beatsToSeconds(4.0, tempo));
        const int span = std::max(12, high - low + 1);
        const int base = low - (span - (high - low + 1)) / 2;
        const double rowH = area.height() / double(span);
        const std::size_t stride = std::max<std::size_t>(
            1, (m_cachedNoteCount + kMaxSketchNotes - 1) / kMaxSketchNotes);
        std::size_t visited = 0;
        m_cachedUseLod = m_cachedNoteCount > kMaxSketchNotes;

        // A miniature is an overview, not an editor. Uniform sampling preserves
        // the phrase envelope while bounding path primitives. Crucially, this
        // full model pass now happens only after a note revision or resize; a
        // hover repaint simply fills the cached path.
        for (const auto& clip : track.clips) {
            if (clip.kind != daw::ClipKind::Midi) continue;
            for (const auto& note : clip.notes) {
                if ((visited++ % stride) != 0) continue;
                const double start = clip.startSeconds +
                    daw::beatsToSeconds(note.startBeats, tempo);
                const double duration = daw::beatsToSeconds(
                    note.lengthBeats, tempo);
                const double x = area.left() + (start / end) * area.width();
                const double w = std::max(2.0, duration / end * area.width());
                const double y = area.bottom() -
                    double(note.pitch - base + 1) * rowH;
                const QRectF noteRect(x, y, w, std::max(2.0, rowH * 0.8));
                if (m_cachedUseLod)
                    m_cachedNotes.addRect(noteRect);
                else
                    m_cachedNotes.addRoundedRect(noteRect, 1.5, 1.5);
            }
        }
    }

    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    QPainterPath m_cachedNotes;
    QSize m_cachedSize;
    std::uint64_t m_cachedRevision =
        std::numeric_limits<std::uint64_t>::max();
    std::size_t m_cachedNoteCount = 0;
    double m_cachedTempo = -1.0;
    bool m_cachedUseLod = false;
};

QToolButton* toolbarButton(icons::Glyph glyph, const QString& text,
                           const QString& tip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("PatternToolbarButton"));
    button->setText(text);
    button->setIcon(icons::icon(glyph, th().textPrimary, 15));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setMinimumHeight(30);
    return button;
}

/// A source name is an opener first and editable metadata second. Delaying the
/// single-click action by the platform double-click interval prevents the first
/// half of a double-click from opening the plugin behind the rename dialog.
class SourceNameButton final : public QAbstractButton {
public:
    using Action = std::function<void()>;

    SourceNameButton(QString text, QString instrumentName, QWidget* parent)
        : QAbstractButton(parent) {
        setFixedWidth(150);
        setFixedHeight(34);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        syncFromModel(std::move(text), std::move(instrumentName));
        m_openTimer.setSingleShot(true);
        connect(&m_openTimer, &QTimer::timeout, this, [this] {
            if (onOpen) onOpen();
        });
    }

    void syncFromModel(QString text, QString instrumentName) {
        const bool changed = !m_modelSynced || this->text() != text ||
                             m_instrumentName != instrumentName;
        if (!changed) return;
        m_modelSynced = true;
        m_instrumentName = std::move(instrumentName);
        setText(std::move(text));
        setAccessibleName(QObject::tr("Open %1").arg(this->text()));
        setToolTip(QObject::tr("Click to open %1. Double-click or right-click "
                               "to rename.")
                       .arg(m_instrumentName.isEmpty()
                                ? QObject::tr("instrument")
                                : m_instrumentName));
        update();
    }

    Action onOpen;
    Action onRename;
    Action onDuplicate;
    Action onRemove;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        QColor fill = isDown() ? mixColors(t.well(), t.accent, 0.16) : t.well();
        if (underMouse()) fill = mixColors(fill, t.accent, 0.08);
        p.setBrush(fill);
        p.setPen(QPen(hasFocus() ? t.accent : t.separator(),
                      hasFocus() ? 1.8 : 1.0));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);

        QFont font = p.font();
        font.setPixelSize(11);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(t.textPrimary);
        const QRect textRect = rect().adjusted(10, 0, -10, 0);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(font).elidedText(text(), Qt::ElideRight,
                                                 textRect.width()));
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        QAbstractButton::mouseReleaseEvent(event);
        if (event->button() != Qt::LeftButton ||
            !rect().contains(event->position().toPoint()))
            return;
        m_openTimer.start(QApplication::doubleClickInterval());
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QAbstractButton::mouseDoubleClickEvent(event);
            return;
        }
        m_openTimer.stop();
        if (onRename) onRename();
        event->accept();
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        m_openTimer.stop();
        QMenu menu(this);
        QAction* open = menu.addAction(
            m_instrumentName.isEmpty()
                ? QObject::tr("Open Instrument")
                : QObject::tr("Open %1").arg(m_instrumentName));
        QAction* rename = menu.addAction(QObject::tr("Rename…"));
        QAction* duplicate = menu.addAction(QObject::tr("Duplicate Source"));
        menu.addSeparator();
        QAction* remove = menu.addAction(QObject::tr("Remove Source"));
        QAction* chosen = menu.exec(event->globalPos());
        if (chosen == open && onOpen) onOpen();
        else if (chosen == rename && onRename) onRename();
        else if (chosen == duplicate && onDuplicate) onDuplicate();
        else if (chosen == remove && onRemove) onRemove();
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_F2) {
            if (onRename) onRename();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Space) {
            if (onOpen) onOpen();
            event->accept();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }

private:
    QString m_instrumentName;
    QTimer m_openTimer;
    bool m_modelSynced = false;
};

} // namespace

PatternWindow::PatternWindow(daw::EngineController* controller, QWidget* parent)
    : QDialog(parent, Qt::Widget), m_controller(controller) {
    setWindowTitle(tr("Pattern"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    resize(900, 360);
    setMinimumSize(640, 240);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* columnHeader = new QWidget(this);
    columnHeader->setObjectName(QStringLiteral("PatternColumnHeader"));
    auto* columns = new QHBoxLayout(columnHeader);
    columns->setContentsMargins(8, 0, 8, 0);
    columns->setSpacing(7);
    auto addColumn = [&](const QString& text, int width) {
        auto* label = new QLabel(text, columnHeader);
        label->setFixedWidth(width);
        columns->addWidget(label);
    };
    addColumn(tr("MIX"), 48);
    addColumn(tr("SOURCE"), 150);
    addColumn(tr("LEVEL"), 112);
    addColumn(tr("PAN"), 30);
    auto* midi = new QLabel(tr("MIDI PATTERN"), columnHeader);
    columns->addWidget(midi, 1);
    addColumn(QString(), 28);
    root->addWidget(columnHeader);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("PatternScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_rowsHost = new QWidget(scroll);
    m_rowsLayout = new QVBoxLayout(m_rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(3);
    m_dropIndicator = new QWidget(m_rowsHost);
    m_dropIndicator->setObjectName(QStringLiteral("PatternDropIndicator"));
    m_dropIndicator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dropIndicator->hide();
    scroll->setWidget(m_rowsHost);
    root->addWidget(scroll, 1);

    auto* deleteSelection = new QAction(tr("Delete selected sources"), this);
    deleteSelection->setObjectName(
        QStringLiteral("pattern.deleteSelectedSources"));
    deleteSelection->setShortcut(QKeySequence::Delete);
    deleteSelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteSelection, &QAction::triggered, this,
            &PatternWindow::deleteSelectedSources);
    addAction(deleteSelection);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        applyTheme();
        if (isVisible()) refresh();
    });
    applyTheme();
}

void PatternWindow::setPattern(const QString& patternId) {
    if (m_patternId == patternId) {
        // A hidden internal editor is refreshed by showEvent.  Rebuilding its
        // complete row tree here as well makes every open pay twice before the
        // first frame is visible.
        if (isVisible()) refresh();
        return;
    }
    m_patternId = patternId;
    if (isVisible()) refresh();
}

void PatternWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refresh();
}

void PatternWindow::keyPressEvent(QKeyEvent* event) {
    const Qt::KeyboardModifiers modifiers =
        event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier |
                              Qt::ShiftModifier | Qt::AltModifier);
    if (event->key() == Qt::Key_Delete ||
        event->key() == Qt::Key_Backspace) {
        deleteSelectedSources();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_A &&
        (modifiers == Qt::ControlModifier ||
         modifiers == Qt::MetaModifier)) {
        const QStringList ids = childTrackIds();
        setSelectedSources(ids, ids.isEmpty() ? QString{} : ids.back());
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && !m_selectedIds.isEmpty()) {
        setSelectedSources({});
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void PatternWindow::closeEvent(QCloseEvent* event) {
    if (m_controller) m_controller->stopPreview();
    QDialog::closeEvent(event);
}

void PatternWindow::refresh() {
    // showEvent is the single catch-up point for a hidden internal editor.
    // Undo/project refreshes while it is closed must not construct or mutate a
    // complete off-screen row tree.
    if (!isVisible()) return;
    const auto* pattern = m_controller
                              ? m_controller->project().findTrack(
                                    m_patternId.toStdString())
                              : nullptr;
    if (!pattern || pattern->kind != daw::TrackKind::Pattern) {
        setWindowTitle(tr("Pattern unavailable"));
        const QStringList ids = childTrackIds();
        if (!rowStructureMatches(ids) || !syncRowsFromModel()) rebuildRows();
        return;
    }
    setWindowTitle(tr("%1 — Pattern")
                       .arg(QString::fromStdString(pattern->name)));
    const QStringList ids = childTrackIds();
    if (!rowStructureMatches(ids) || !syncRowsFromModel()) rebuildRows();
}

QStringList PatternWindow::childTrackIds() const {
    QStringList ids;
    if (!m_controller || m_patternId.isEmpty()) return ids;
    const auto& tracks = m_controller->project().tracks;
    ids.reserve(int(tracks.size()));
    const std::string patternId = m_patternId.toStdString();
    for (const auto& track : tracks) {
        if (track.parentId == patternId)
            ids.push_back(QString::fromStdString(track.id));
    }
    return ids;
}

void PatternWindow::setSelectedSources(const QStringList& ids,
                                       const QString& primary) {
    const QStringList order = childTrackIds();
    QSet<QString> requested;
    requested.reserve(ids.size());
    for (const QString& id : ids) requested.insert(id);
    QStringList selected;
    selected.reserve(std::min(order.size(), ids.size()));
    for (const QString& id : order) {
        if (requested.contains(id)) selected.push_back(id);
    }
    QString lead = primary;
    if (!selected.contains(lead))
        lead = selected.isEmpty() ? QString{} : selected.back();
    if (selected == m_selectedIds && lead == m_primaryId) return;
    m_selectedIds = selected;
    m_primaryId = lead;
    updateSelectionVisuals();
}

void PatternWindow::selectRange(int first, int last) {
    const QStringList ids = childTrackIds();
    if (ids.isEmpty()) {
        setSelectedSources({});
        return;
    }
    first = std::clamp(first, 0, int(ids.size()) - 1);
    last = std::clamp(last, 0, int(ids.size()) - 1);
    const int from = std::min(first, last);
    const int to = std::max(first, last);
    QStringList range;
    range.reserve(to - from + 1);
    for (int i = from; i <= to; ++i) range.push_back(ids[i]);
    const QString lead = ids[last];
    if (range == m_selectedIds && lead == m_primaryId) return;
    m_selectedIds = std::move(range);
    m_primaryId = lead;
    updateSelectionVisuals();
}

void PatternWindow::updateSelectionVisuals() {
    QSet<QString> selectedIds;
    selectedIds.reserve(m_selectedIds.size());
    for (const QString& id : std::as_const(m_selectedIds))
        selectedIds.insert(id);

    for (QWidget* row : std::as_const(m_rowWidgets)) {
        if (!row) continue;
        const QString id = row->property("trackId").toString();
        const bool selected = selectedIds.contains(id);
        const bool primary = selected && id == m_primaryId;
        const QVariant selectedProperty = row->property("selected");
        const QVariant primaryProperty = row->property("primary");
        const bool selectionChanged =
            !selectedProperty.isValid() || selectedProperty.toBool() != selected;
        const bool primaryChanged =
            !primaryProperty.isValid() || primaryProperty.toBool() != primary;
        if (selectionChanged)
            row->setProperty("selected", selected);
        if (primaryChanged)
            row->setProperty("primary", primary);
        if (!selectionChanged && !primaryChanged) continue;

        row->setAccessibleDescription(
            selected ? tr("Selected pattern source. Drag to reorder.")
                     : tr("Not selected. Drag to select a range."));
        row->setCursor(selected ? Qt::OpenHandCursor : Qt::ArrowCursor);
        row->style()->unpolish(row);
        row->style()->polish(row);
        row->update();
    }
}

int PatternWindow::rowIndexAtGlobal(const QPoint& globalPos) const {
    if (!m_rowsHost || m_rowWidgets.isEmpty()) return -1;
    const int y = m_rowsHost->mapFromGlobal(globalPos).y();
    if (y <= m_rowWidgets.front()->geometry().top()) return 0;
    int first = 0;
    int last = m_rowWidgets.size() - 1;
    while (first < last) {
        const int middle = first + (last - first) / 2;
        if (y <= m_rowWidgets[middle]->geometry().bottom())
            last = middle;
        else
            first = middle + 1;
    }
    return first;
}

int PatternWindow::insertionIndexAtGlobal(const QPoint& globalPos) const {
    if (!m_rowsHost || m_rowWidgets.isEmpty()) return 0;
    const int y = m_rowsHost->mapFromGlobal(globalPos).y();
    int first = 0;
    int last = m_rowWidgets.size();
    while (first < last) {
        const int middle = first + (last - first) / 2;
        if (y < m_rowWidgets[middle]->geometry().center().y())
            last = middle;
        else
            first = middle + 1;
    }
    return first;
}

void PatternWindow::beginRowGesture(
    const QString& trackId, const QPoint& globalPos,
    Qt::KeyboardModifiers modifiers) {
    const QStringList ids = childTrackIds();
    const int index = ids.indexOf(trackId);
    if (index < 0) return;

    setFocus(Qt::MouseFocusReason);
    m_rowGestureActive = true;
    m_rangeSelecting = false;
    m_reorderCandidate = false;
    m_reordering = false;
    m_dropIndex = -1;
    m_gesturePressGlobal = globalPos;
    m_gestureAnchorIndex = index;
    m_gestureModifiers = modifiers;

    const bool additive = modifiers & (Qt::ControlModifier | Qt::MetaModifier);
    if (modifiers & Qt::ShiftModifier) {
        const int anchor = ids.indexOf(m_selectionAnchorId);
        m_gestureAnchorIndex = anchor >= 0 ? anchor : index;
        selectRange(m_gestureAnchorIndex, index);
        m_rangeSelecting = true;
        return;
    }
    if (additive) {
        QStringList selection = m_selectedIds;
        if (selection.contains(trackId)) selection.removeAll(trackId);
        else selection.push_back(trackId);
        setSelectedSources(selection, trackId);
        m_selectionAnchorId = trackId;
        return;
    }
    if (m_selectedIds.contains(trackId)) {
        // A selected row is already a movable object. A click without enough
        // motion collapses a multi-selection to that row on release; crossing
        // the platform drag threshold moves the whole selection instead.
        m_reorderCandidate = true;
        return;
    }

    setSelectedSources({trackId}, trackId);
    m_selectionAnchorId = trackId;
    m_rangeSelecting = true;
}

void PatternWindow::updateRowGesture(const QPoint& globalPos) {
    if (!m_rowGestureActive) return;
    if (m_rangeSelecting) {
        const int row = rowIndexAtGlobal(globalPos);
        if (row >= 0) selectRange(m_gestureAnchorIndex, row);
        return;
    }
    if (m_reorderCandidate && !m_reordering &&
        (globalPos - m_gesturePressGlobal).manhattanLength() >=
            QApplication::startDragDistance()) {
        m_reordering = true;
        for (QWidget* row : std::as_const(m_rowWidgets)) {
            if (row && m_selectedIds.contains(
                           row->property("trackId").toString())) {
                row->setCursor(Qt::ClosedHandCursor);
            }
        }
    }
    if (!m_reordering) return;
    updateDropIndicator(insertionIndexAtGlobal(globalPos));
}

void PatternWindow::endRowGesture(const QString& trackId,
                                  const QPoint& globalPos) {
    if (!m_rowGestureActive) return;
    if (m_reordering) {
        updateDropIndicator(insertionIndexAtGlobal(globalPos));
        const int drop = m_dropIndex;
        endRowGestureState();
        reorderSelectedSources(drop);
        return;
    }
    if (m_reorderCandidate &&
        !(m_gestureModifiers & (Qt::ControlModifier | Qt::MetaModifier |
                                Qt::ShiftModifier))) {
        setSelectedSources({trackId}, trackId);
        m_selectionAnchorId = trackId;
    }
    endRowGestureState();
}

void PatternWindow::updateDropIndicator(int insertionIndex) {
    if (!m_dropIndicator || m_rowWidgets.isEmpty()) return;
    m_dropIndex = std::clamp(insertionIndex, 0, int(m_rowWidgets.size()));
    const int y = m_dropIndex >= m_rowWidgets.size()
                      ? m_rowWidgets.back()->geometry().bottom() + 1
                      : m_rowWidgets[m_dropIndex]->geometry().top() - 1;
    m_dropIndicator->setGeometry(4, y - 1,
                                 std::max(1, m_rowsHost->width() - 8), 3);
    m_dropIndicator->show();
    m_dropIndicator->raise();
}

void PatternWindow::endRowGestureState() {
    m_rowGestureActive = false;
    m_rangeSelecting = false;
    m_reorderCandidate = false;
    m_reordering = false;
    m_gestureAnchorIndex = -1;
    m_dropIndex = -1;
    m_gestureModifiers = Qt::NoModifier;
    if (m_dropIndicator) m_dropIndicator->hide();
    updateSelectionVisuals();
}

void PatternWindow::reorderSelectedSources(int dropIndex) {
    const QStringList original = childTrackIds();
    if (!m_controller || original.isEmpty() || m_selectedIds.isEmpty() ||
        dropIndex < 0) {
        return;
    }

    QStringList moving;
    QStringList remaining;
    for (const QString& id : original) {
        (m_selectedIds.contains(id) ? moving : remaining).push_back(id);
    }
    if (moving.isEmpty()) return;

    const int boundedDrop = std::clamp(dropIndex, 0, int(original.size()));
    int insertion = 0;
    for (int i = 0; i < boundedDrop; ++i) {
        if (!m_selectedIds.contains(original[i])) ++insertion;
    }
    insertion = std::clamp(insertion, 0, int(remaining.size()));
    QStringList desired = remaining;
    for (int i = 0; i < moving.size(); ++i)
        desired.insert(insertion + i, moving[i]);
    if (desired == original) return;

    const std::size_t undoStart = m_controller->undoDepth();
    std::size_t cursor =
        m_controller->project().indexOf(m_patternId.toStdString()) + 1;
    for (const QString& id : desired) {
        const std::string sourceId = id.toStdString();
        if (m_controller->project().indexOf(sourceId) != cursor) {
            m_controller->moveTrack(sourceId, cursor,
                                    m_patternId.toStdString());
        }
        const std::size_t now = m_controller->project().indexOf(sourceId);
        cursor = now + 1 +
                 daw::subtreeOf(m_controller->project(), sourceId).size();
    }
    m_controller->collapseUndo(undoStart, "Reorder Pattern Sources");
    refresh();
    emit projectEdited();
}

void PatternWindow::moveSelectedSources(int direction) {
    const QStringList ids = childTrackIds();
    if (ids.isEmpty() || m_selectedIds.isEmpty() || direction == 0) return;
    int first = int(ids.size());
    int last = -1;
    for (const QString& id : m_selectedIds) {
        const int index = ids.indexOf(id);
        if (index < 0) continue;
        first = std::min(first, index);
        last = std::max(last, index);
    }
    if (last < 0) return;
    if (direction < 0) {
        if (first == 0) return;
        reorderSelectedSources(first - 1);
    } else {
        if (last == ids.size() - 1) return;
        reorderSelectedSources(last + 2);
    }
}

void PatternWindow::showSelectionMenu(const QString& trackId,
                                      const QPoint& globalPos) {
    if (!m_selectedIds.contains(trackId)) {
        setSelectedSources({trackId}, trackId);
        m_selectionAnchorId = trackId;
    }
    const int count = m_selectedIds.size();
    QMenu menu(this);
    QAction* open = menu.addAction(tr("Open Piano Roll"));
    menu.addSeparator();
    QAction* transpose = menu.addAction(
        count == 1 ? tr("Transpose Selected Source…")
                   : tr("Transpose %1 Selected Sources…").arg(count));
    QAction* moveUp = menu.addAction(tr("Move Selected Up"));
    QAction* moveDown = menu.addAction(tr("Move Selected Down"));
    menu.addSeparator();
    QAction* selectAll = menu.addAction(tr("Select All Sources"));
    QAction* remove = menu.addAction(
        count == 1 ? tr("Delete Selected Source")
                   : tr("Delete %1 Selected Sources").arg(count));
    remove->setShortcut(QKeySequence::Delete);

    const QStringList ids = childTrackIds();
    int first = int(ids.size());
    int last = -1;
    for (const QString& id : m_selectedIds) {
        const int index = ids.indexOf(id);
        if (index >= 0) {
            first = std::min(first, index);
            last = std::max(last, index);
        }
    }
    moveUp->setEnabled(first > 0 && first < ids.size());
    moveDown->setEnabled(last >= 0 && last < ids.size() - 1);
    selectAll->setEnabled(!ids.isEmpty() && m_selectedIds.size() != ids.size());

    QAction* chosen = menu.exec(globalPos);
    if (chosen == open) openRoll(trackId);
    else if (chosen == transpose) transposeSelectedSources();
    else if (chosen == moveUp) moveSelectedSources(-1);
    else if (chosen == moveDown) moveSelectedSources(1);
    else if (chosen == selectAll)
        setSelectedSources(ids, ids.isEmpty() ? QString{} : ids.back());
    else if (chosen == remove) deleteSelectedSources();
}

void PatternWindow::rebuildRows() {
    if (!isVisible()) return;
    endRowGestureState();
    while (QLayoutItem* item = m_rowsLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    m_rowWidgets.clear();

    const QStringList ids = childTrackIds();
    QStringList surviving;
    for (const QString& id : ids) {
        if (m_selectedIds.contains(id)) surviving.push_back(id);
    }
    m_selectedIds = surviving;
    if (!m_selectedIds.contains(m_primaryId)) {
        m_primaryId = m_selectedIds.isEmpty() ? QString{}
                                               : m_selectedIds.back();
    }
    if (!ids.contains(m_selectionAnchorId))
        m_selectionAnchorId = m_primaryId;

    for (const QString& id : ids) {
        const auto* track =
            m_controller->project().findTrack(id.toStdString());
        if (!track) continue;

        auto* row = new PatternSourceRow(id, m_rowsHost);
        row->setObjectName(QStringLiteral("PatternSourceRow"));
        row->setFixedHeight(kRowHeight);
        const QString accessibleName =
            tr("Pattern source %1").arg(QString::fromStdString(track->name));
        if (row->accessibleName() != accessibleName)
            row->setAccessibleName(accessibleName);
        row->setToolTip(
            tr("Click the grey row to select. Drag an unselected row to select "
               "a range; drag a selected row to reorder the selection."));
        row->onPress = [this](const QString& trackId, const QPoint& global,
                              Qt::KeyboardModifiers modifiers) {
            beginRowGesture(trackId, global, modifiers);
        };
        row->onMove = [this](const QPoint& global) {
            updateRowGesture(global);
        };
        row->onRelease = [this](const QString& trackId,
                                const QPoint& global) {
            endRowGesture(trackId, global);
        };
        row->onMenu = [this](const QString& trackId, const QPoint& global) {
            showSelectionMenu(trackId, global);
        };
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 6, 8, 6);
        layout->setSpacing(7);

        auto* mix = new QWidget(row);
        mix->setFixedWidth(48);
        auto* mixRow = new QHBoxLayout(mix);
        mixRow->setContentsMargins(0, 0, 0, 0);
        mixRow->setSpacing(3);
        auto* mute = new ui::MsrButton("M", Theme::mute(), tr("Mute"), mix);
        auto* solo = new ui::MsrButton("S", Theme::solo(), tr("Solo"), mix);
        mute->setObjectName(QStringLiteral("PatternSourceMute"));
        solo->setObjectName(QStringLiteral("PatternSourceSolo"));
        mute->setChipSize(22, 20);
        solo->setChipSize(22, 20);
        mute->setChecked(track->muted);
        mute->setAutomatable(true);
        connect(mute, &ui::MsrButton::automateRequested, this,
                [this, id] { emit automateMuteRequested(id); });
        solo->setChecked(track->soloed);
        connect(mute, &QAbstractButton::toggled, this,
                [this, id](bool on) {
                    m_controller->setTrackMuted(id.toStdString(), on);
                    emit projectEdited();
                });
        connect(solo, &QAbstractButton::toggled, this,
                [this, id](bool on) {
                    m_controller->setTrackSoloed(id.toStdString(), on);
                    emit projectEdited();
                });
        mixRow->addWidget(mute);
        mixRow->addWidget(solo);
        layout->addWidget(mix);

        auto* name = new SourceNameButton(
            QString::fromStdString(track->name),
            QString::fromStdString(track->instrument.name), row);
        name->setObjectName(QStringLiteral("PatternSourceName"));
        name->onOpen = [this, id] { openInstrument(id); };
        name->onRename = [this, id] { renameSource(id); };
        name->onDuplicate = [this, id] { duplicateSource(id); };
        name->onRemove = [this, id] { removeSource(id); };
        layout->addWidget(name);

        auto* fader = new ui::FaderWidget(Qt::Horizontal, row);
        fader->setObjectName(QStringLiteral("PatternSourceLevel"));
        fader->setAutomatable(true);
        connect(fader, &ui::FaderWidget::automateRequested, this,
                [this, id] { emit automateControlRequested(id, false); });
        fader->setFixedWidth(112);
        fader->setGain(track->volume);
        fader->setToolTip(tr("Level  %1")
                              .arg(ui::formatGainDb(track->volume)));
        auto volumeStart = std::make_shared<std::optional<float>>();
        connect(fader, &ui::FaderWidget::gainChanged, this,
                [this, id, fader, volumeStart](double gain) {
                    const std::string trackId = id.toStdString();
                    if (!*volumeStart) {
                        if (const auto* current =
                                m_controller->project().findTrack(trackId))
                            *volumeStart = current->volume;
                    }
                    m_controller->setTrackVolumeLive(trackId, float(gain));
                    fader->setToolTip(tr("Level  %1")
                                          .arg(ui::formatGainDb(gain)));
                });
        connect(fader, &ui::FaderWidget::editFinished, this,
                [this, id, volumeStart] {
                    if (*volumeStart) {
                        m_controller->commitTrackVolumeEdit(
                            {{id.toStdString(), **volumeStart}});
                        volumeStart->reset();
                    }
                    emit projectEdited();
                });
        layout->addWidget(fader);

        auto* pan = new ui::PanKnob(row);
        pan->setObjectName(QStringLiteral("PatternSourcePan"));
        pan->setAutomatable(true);
        connect(pan, &ui::PanKnob::automateRequested, this,
                [this, id] { emit automateControlRequested(id, true); });
        pan->setFixedSize(30, 30);
        pan->setPan(track->pan);
        pan->setToolTip(tr("Pan"));
        auto panStart = std::make_shared<std::optional<float>>();
        connect(pan, &ui::PanKnob::panChanged, this,
                [this, id, panStart](double value) {
                    const std::string trackId = id.toStdString();
                    if (!*panStart) {
                        if (const auto* current =
                                m_controller->project().findTrack(trackId))
                            *panStart = current->pan;
                    }
                    m_controller->setTrackPanLive(trackId, float(value));
                });
        connect(pan, &ui::PanKnob::editFinished, this,
                [this, id, panStart] {
                    if (*panStart) {
                        m_controller->commitTrackPanEdit(
                            {{id.toStdString(), **panStart}});
                        panStart->reset();
                    }
                    emit projectEdited();
                });
        layout->addWidget(pan);

        auto* sketch = new SourceSketch(m_controller, id, row);
        sketch->setObjectName(QStringLiteral("PatternSourceSketch"));
        connect(sketch, &QAbstractButton::clicked, this,
                [this, id] { openRoll(id); });
        layout->addWidget(sketch, 1);

        auto* remove = new ui::IconButton(icons::Glyph::Trash,
                                          tr("Remove source"), row);
        remove->setButtonSize(28, 28);
        remove->setCursor(Qt::PointingHandCursor);
        connect(remove, &QAbstractButton::clicked, this,
                [this, id] { removeSource(id); });
        layout->addWidget(remove);
        m_rowWidgets.push_back(row);
        m_rowsLayout->addWidget(row);
    }

    auto* add = toolbarButton(
        icons::Glyph::Plus, tr("Add virtual instrument"),
        tr("Add a virtual instrument or Sampler. You can also drop audio "
           "files into this window."),
        m_rowsHost);
    add->setObjectName(QStringLiteral("PatternAddInstrument"));
    add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(add, &QAbstractButton::clicked, this,
            &PatternWindow::showInstrumentMenu);
    m_rowsLayout->addWidget(add);
    m_rowsLayout->addStretch(1);
    updateSelectionVisuals();
    if (m_dropIndicator) {
        m_dropIndicator->hide();
        m_dropIndicator->raise();
    }
}

bool PatternWindow::rowStructureMatches(const QStringList& ids) const {
    // The empty-pattern case still needs its persistent Add row. Without this
    // sentinel an uninitialised tree and a valid zero-source tree both look like
    // the same empty m_rowWidgets list.
    if (!m_rowsHost || !m_rowsHost->findChild<QToolButton*>(
                           QStringLiteral("PatternAddInstrument"))) {
        return false;
    }
    if (m_rowWidgets.size() != ids.size()) return false;
    for (int i = 0; i < ids.size(); ++i) {
        const QWidget* row = m_rowWidgets[i];
        if (!row || row->property("trackId").toString() != ids[i]) return false;
    }
    return true;
}

bool PatternWindow::syncRowsFromModel() {
    if (!m_controller) return m_rowWidgets.isEmpty();
    for (QWidget* row : std::as_const(m_rowWidgets)) {
        if (!row) return false;
        const QString id = row->property("trackId").toString();
        const auto* track =
            m_controller->project().findTrack(id.toStdString());
        if (!track) return false;

        auto* mute = row->findChild<ui::MsrButton*>(
            QStringLiteral("PatternSourceMute"));
        auto* solo = row->findChild<ui::MsrButton*>(
            QStringLiteral("PatternSourceSolo"));
        auto* name = dynamic_cast<SourceNameButton*>(row->findChild<QWidget*>(
            QStringLiteral("PatternSourceName")));
        auto* fader = row->findChild<ui::FaderWidget*>(
            QStringLiteral("PatternSourceLevel"));
        auto* pan = row->findChild<ui::PanKnob*>(
            QStringLiteral("PatternSourcePan"));
        auto* sketch = dynamic_cast<SourceSketch*>(row->findChild<QWidget*>(
            QStringLiteral("PatternSourceSketch")));
        if (!mute || !solo || !name || !fader || !pan || !sketch) return false;

        row->setAccessibleName(
            tr("Pattern source %1").arg(QString::fromStdString(track->name)));
        name->syncFromModel(QString::fromStdString(track->name),
                            QString::fromStdString(track->instrument.name));

        if (mute->isChecked() != track->muted) {
            const QSignalBlocker blocker(mute);
            mute->setChecked(track->muted);
        }
        if (solo->isChecked() != track->soloed) {
            const QSignalBlocker blocker(solo);
            solo->setChecked(track->soloed);
        }
        if (!fader->isEditing()) {
            const bool gainChanged =
                std::abs(fader->gain() - track->volume) >= 1e-6;
            fader->setGain(track->volume);
            if (gainChanged) {
                fader->setToolTip(
                    tr("Level  %1").arg(ui::formatGainDb(track->volume)));
            }
        }
        if (!pan->isEditing()) pan->setPan(track->pan);

        // Notes, timing, colour and tempo are read directly by SourceSketch at
        // paint time. A non-structural MIDI edit therefore schedules only this
        // bounded-LOD repaint and preserves every QObject in the row.
        sketch->update();
    }
    return true;
}

void PatternWindow::showInstrumentMenu() {
    QMenu* plugins = ui::buildPluginMenu(
        this, m_controller, true,
        [this](const daw::plugins::PluginDescriptor& descriptor) {
            const std::string id = m_controller->addPatternInstrument(
                m_patternId.toStdString(), descriptor);
            if (id.empty()) {
                QMessageBox::warning(
                    this, tr("Instrument could not be loaded"),
                    tr("%1 could not be opened. Check the plugin installation "
                       "or rescan it in Plugin Manager.")
                        .arg(QString::fromStdString(descriptor.name)));
                return;
            }
            emit projectEdited();
            refresh();
            const auto* track = m_controller->project().findTrack(id);
            if (track && !track->instrument.id.empty())
                emit openPluginEditorRequested(
                    QString::fromStdString(id),
                    QString::fromStdString(track->instrument.id));
        });
    plugins->setAttribute(Qt::WA_DeleteOnClose);
    plugins->popup(QCursor::pos());
}

void PatternWindow::addSampleFiles(const QStringList& paths,
                                   double startSeconds) {
    bool changed = false;
    QStringList failed;
    for (const QString& path : paths) {
        if (!ui::isAudioFile(path)) continue;
        const bool loaded = !m_controller
                                 ->addPatternSample(m_patternId.toStdString(),
                                                    path.toStdString(),
                                                    startSeconds)
                                 .empty();
        changed |= loaded;
        if (!loaded) failed.push_back(QFileInfo(path).fileName());
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(
            this, tr("Some samples could not be loaded"),
            tr("The following files could not be decoded or loaded into the "
               "Sampler:\n%1")
                .arg(failed.join(QLatin1Char('\n'))));
    }
    if (!changed) return;
    emit projectEdited();
    refresh();
}

void PatternWindow::openInstrument(const QString& trackId) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track || !track->instrument.isLoaded()) return;
    emit openPluginEditorRequested(
        trackId, QString::fromStdString(track->instrument.id));
}

void PatternWindow::renameSource(const QString& trackId) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track) return;
    bool accepted = false;
    const QString current = QString::fromStdString(track->name);
    const QString name = QInputDialog::getText(
        this, tr("Rename Pattern Source"), tr("Source name:"),
        QLineEdit::Normal, current, &accepted);
    if (!accepted || name.trimmed().isEmpty() || name == current) return;
    m_controller->renameTrack(trackId.toStdString(),
                              name.trimmed().toStdString());
    emit projectEdited();
    refresh();
}

void PatternWindow::duplicateSource(const QString& trackId) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track || track->parentId != m_patternId.toStdString()) return;
    const std::string copy =
        m_controller->duplicateTrack(trackId.toStdString(), /*withInserts=*/true);
    if (copy.empty()) return;
    const QString copyId = QString::fromStdString(copy);
    setSelectedSources({copyId}, copyId);
    m_selectionAnchorId = copyId;
    emit projectEdited();
    refresh();
}

void PatternWindow::removeSource(const QString& trackId) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track || track->parentId != m_patternId.toStdString()) return;
    m_controller->removeTrack(trackId.toStdString());
    m_selectedIds.removeAll(trackId);
    if (m_primaryId == trackId) m_primaryId.clear();
    if (m_selectionAnchorId == trackId) m_selectionAnchorId.clear();
    emit projectEdited();
    refresh();
}

void PatternWindow::deleteSelectedSources() {
    if (!m_controller || m_selectedIds.isEmpty()) return;
    const QStringList before = childTrackIds();
    QStringList doomed;
    int firstRemoved = int(before.size());
    for (const QString& id : before) {
        if (!m_selectedIds.contains(id)) continue;
        firstRemoved = std::min(firstRemoved, int(before.indexOf(id)));
        doomed.push_back(id);
    }
    if (doomed.isEmpty()) return;

    const std::size_t undoStart = m_controller->undoDepth();
    for (auto it = doomed.crbegin(); it != doomed.crend(); ++it)
        m_controller->removeTrack(it->toStdString());
    m_controller->collapseUndo(undoStart, "Delete Pattern Sources");

    m_selectedIds.clear();
    m_primaryId.clear();
    m_selectionAnchorId.clear();
    const QStringList after = childTrackIds();
    if (!after.isEmpty()) {
        const QString next =
            after[std::min(firstRemoved, int(after.size()) - 1)];
        m_selectedIds = {next};
        m_primaryId = next;
        m_selectionAnchorId = next;
    }
    refresh();
    emit projectEdited();
}

void PatternWindow::transposeSelectedSources() {
    if (!m_controller || m_selectedIds.isEmpty()) return;
    bool accepted = false;
    const int semitones = QInputDialog::getInt(
        this, tr("Transpose Selected Sources"), tr("Semitones:"),
        1, -48, 48, 1, &accepted);
    if (!accepted || semitones == 0) return;
    transposeSelectedSourcesBy(semitones);
}

void PatternWindow::transposeSelectedSourcesBy(int semitones) {
    if (!m_controller || m_selectedIds.isEmpty() || semitones == 0) return;
    struct Job {
        std::string trackId;
        std::string clipId;
        std::vector<daw::NoteModel> notes;
    };
    std::vector<Job> jobs;
    for (const QString& id : std::as_const(m_selectedIds)) {
        const auto* track =
            m_controller->project().findTrack(id.toStdString());
        if (!track || track->parentId != m_patternId.toStdString()) continue;
        for (const auto& clip : track->clips) {
            if (clip.kind != daw::ClipKind::Midi || clip.notes.empty()) continue;
            Job job{id.toStdString(), clip.id, clip.notes};
            for (auto& note : job.notes) note.pitch += semitones;
            jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty()) return;

    const std::size_t undoStart = m_controller->undoDepth();
    for (Job& job : jobs) {
        m_controller->setClipNotes(job.trackId, job.clipId,
                                   std::move(job.notes),
                                   "Transpose Pattern Source");
    }
    m_controller->collapseUndo(undoStart, "Transpose Pattern Sources");
    refresh();
    emit projectEdited();
}

void PatternWindow::openRoll(const QString& trackId) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track) return;
    for (const auto& clip : track->clips) {
        if (clip.kind != daw::ClipKind::Midi) continue;
        emit openPianoRollRequested(trackId, QString::fromStdString(clip.id));
        return;
    }
    const std::string clip = m_controller->addMidiClip(trackId.toStdString(), 0.0);
    if (clip.empty()) return;
    emit projectEdited();
    emit openPianoRollRequested(trackId, QString::fromStdString(clip));
    refresh();
}

void PatternWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (ui::isAudioFile(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void PatternWindow::dragMoveEvent(QDragMoveEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (ui::isAudioFile(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void PatternWindow::dropEvent(QDropEvent* event) {
    QStringList files;
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (ui::isAudioFile(path)) files.push_back(path);
    }
    if (files.isEmpty()) return;
    event->acceptProposedAction();
    addSampleFiles(files);
}

bool PatternWindow::checkInteractionGesturesForTest() {
    const QStringList ids = childTrackIds();
    if (!isVisible() || ids.size() < 2 || m_rowWidgets.size() != ids.size())
        return false;

    const QList<QWidget*> originalRows = m_rowWidgets;
    refresh();
    const bool stableRefreshIdentity = m_rowWidgets == originalRows;

    // Model changes delivered by Undo/Redo use this same refresh path. Exercise
    // it without touching the undo stack: a live level change must update the
    // existing fader rather than replacing its row or emitting a UI edit back.
    const std::string firstId = ids.front().toStdString();
    const auto* firstTrack = m_controller->project().findTrack(firstId);
    auto* originalFader = m_rowWidgets.front()->findChild<ui::FaderWidget*>(
        QStringLiteral("PatternSourceLevel"));
    if (!firstTrack || !originalFader) return false;
    const float originalVolume = firstTrack->volume;
    const float changedVolume = originalVolume < 1.75f
                                    ? originalVolume + 0.125f
                                    : originalVolume - 0.125f;
    m_controller->setTrackVolumeLive(firstId, changedVolume);
    refresh();
    QWidget* refreshedFirstRow = m_rowWidgets.isEmpty()
                                     ? nullptr
                                     : m_rowWidgets.front();
    auto* refreshedFader = refreshedFirstRow
        ? refreshedFirstRow->findChild<ui::FaderWidget*>(
              QStringLiteral("PatternSourceLevel"))
        : nullptr;
    const bool stateUpdatedInPlace = m_rowWidgets == originalRows &&
        refreshedFader == originalFader &&
        std::abs(refreshedFader->gain() - changedVolume) < 1e-6;
    m_controller->setTrackVolumeLive(firstId, originalVolume);
    refresh();
    const bool stateRestored = m_rowWidgets == originalRows &&
        std::abs(originalFader->gain() - originalVolume) < 1e-6;

    const QStringList saved = m_selectedIds;
    const QString savedPrimary = m_primaryId;
    const QString savedAnchor = m_selectionAnchorId;

    // The same primitive pointer-drag selection uses: it must work in either
    // direction and preserve document order, because transpose/reorder/delete
    // all consume this exact list.
    selectRange(ids.size() - 1, 0);
    const bool fullRange = m_selectedIds == ids &&
                           m_primaryId == ids.front();
    selectRange(1, ids.size() - 1);
    const bool partialRange = m_selectedIds.size() == ids.size() - 1 &&
                              !m_selectedIds.contains(ids.front()) &&
                              m_selectedIds.front() == ids[1];

    m_selectedIds = saved;
    m_primaryId = savedPrimary;
    m_selectionAnchorId = savedAnchor;
    updateSelectionVisuals();
    return stableRefreshIdentity && stateUpdatedInPlace && stateRestored &&
           fullRange && partialRange;
}

void PatternWindow::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
QDialog { background: %BG%; color: %TEXT%; }
#PatternColumnHeader { background: %BG%; }
#PatternColumnHeader QLabel { color: %TEXT2%; font-size: 9px;
                              font-weight: 700; letter-spacing: 0.5px; }
#PatternScroll { background: %BG%; }
#PatternSourceRow { background: %SURFACE%; border: 1px solid %SEP%;
                    border-radius: 8px; }
#PatternSourceRow:hover { background: %HOVER%; border-color: %SECTION%; }
#PatternSourceRow[selected="true"] { background: %SELECTED%;
    border-color: %ACCENT%; }
#PatternSourceRow[primary="true"] { border-width: 2px; }
#PatternDropIndicator { background: %ACCENT%; border-radius: 1px; }
QToolButton#PatternToolbarButton { color: %TEXT%; background: %WELL%;
    border: 1px solid %SEP%; border-radius: 7px; padding: 4px 8px; }
QToolButton#PatternToolbarButton:hover { background: %HOVER%;
    border-color: %ACCENT%; }
QToolButton#PatternToolbarButton:disabled { color: %TEXT2%; background: %SURFACE%; }
QToolButton#PatternAddInstrument { min-height: 32px; text-align: left;
    background: %SURFACE%; border-style: dashed; }
)")
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%WELL%", t.well().name())
        .replace("%SEP%", t.separator().name())
        .replace("%SECTION%", t.sectionDivider().name())
        .replace("%HOVER%", mixColors(t.surface, t.textPrimary, 0.08).name())
        .replace("%SELECTED%", mixColors(t.surface, t.accent, 0.18).name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%TEXT%", t.textPrimary.name()));
}
