#include "PatternWindow.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"
#include "FileTypes.hpp"
#include "Icons.hpp"
#include "PluginPickerMenu.hpp"
#include "PianoRollWindow.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileDialog>
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
#include <QTemporaryDir>
#include <QFile>
#include <QScrollBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

constexpr int kRowHeight = 60;
constexpr int kKnobSize = 34;
constexpr int kSourceWidth = 150;
constexpr int kMaxSketchNotes = 768;

// Pattern rows are a scrolling list. Turning the wheel over a parameter must
// scroll that list, never make an unnoticed mix edit.
class PatternLevelKnob final : public ui::FaderWidget {
public:
    explicit PatternLevelKnob(QWidget* parent) : FaderWidget(Qt::Horizontal, parent) {
        setCompactKnob(true);
        setFixedSize(kKnobSize, kKnobSize);
        setFocusPolicy(Qt::TabFocus);
        setAccessibleName(QObject::tr("Volume"));
    }
protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
    void paintEvent(QPaintEvent* event) override {
        FaderWidget::paintEvent(event);
        if (!hasFocus()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(th().accent, 1.0, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                switch (key->key()) {
                case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
                case Qt::Key_Home: case Qt::Key_End: event->accept(); return true;
                default: break;
                }
            }
        }
        return FaderWidget::event(event);
    }
    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        auto* entry = menu.addAction(QObject::tr("Enter volume…"));
        auto* reset = menu.addAction(QObject::tr("Reset to 0 dB"));
        menu.addSeparator();
        auto* automate = menu.addAction(QObject::tr("Create Automation Clip"));
        const auto* chosen = menu.exec(event->globalPos());
        if (chosen == automate) { emit automateRequested(); return; }
        double next = 1.0;
        if (chosen == entry) {
            bool accepted = false;
            const double db = QInputDialog::getDouble(this, QObject::tr("Volume"),
                QObject::tr("Level (dB, −96 = silence):"),
                gain() > 0.0 ? 20.0 * std::log10(gain()) : -96.0,
                -96.0, 6.0206, 2, &accepted);
            if (!accepted) return;
            next = db <= -96.0 ? 0.0 : std::pow(10.0, db / 20.0);
        } else if (chosen != reset) return;
        setGain(next);
        emit gainChanged(gain());
        emit editFinished();
    }
    void keyPressEvent(QKeyEvent* event) override {
        double position = ui::faderPositionFromGain(gain());
        const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.005 : 0.02;
        switch (event->key()) {
        case Qt::Key_Up: case Qt::Key_Right: position += step; break;
        case Qt::Key_Down: case Qt::Key_Left: position -= step; break;
        case Qt::Key_Home: position = 0.0; break;
        case Qt::Key_End: position = 1.0; break;
        default: FaderWidget::keyPressEvent(event); return;
        }
        setGain(ui::gainFromFaderPosition(std::clamp(position, 0.0, 1.0)));
        emit gainChanged(gain());
        emit editFinished();
        event->accept();
    }
};

class PatternPanKnob final : public ui::PanKnob {
public:
    explicit PatternPanKnob(QWidget* parent) : PanKnob(parent) {
        setFixedSize(kKnobSize, kKnobSize);
        setAccessibleName(QObject::tr("Pan"));
    }
protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        auto* entry = menu.addAction(QObject::tr("Enter pan…"));
        auto* reset = menu.addAction(QObject::tr("Centre pan"));
        menu.addSeparator();
        auto* automate = menu.addAction(QObject::tr("Create Automation Clip"));
        const auto* chosen = menu.exec(event->globalPos());
        if (chosen == automate) { emit automateRequested(); return; }
        double next = 0.0;
        if (chosen == entry) {
            bool accepted = false;
            next = QInputDialog::getDouble(this, QObject::tr("Pan"),
                QObject::tr("Pan (−100 left, 0 centre, 100 right):"),
                pan() * 100.0, -100.0, 100.0, 1, &accepted) / 100.0;
            if (!accepted) return;
        } else if (chosen != reset) return;
        setPan(next);
        emit panChanged(pan());
        emit editFinished();
    }
};

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
        setMinimumWidth(120);
        setFixedHeight(40);
        setAccessibleName(QObject::tr("Open this source in the piano roll"));
        setToolTip(QObject::tr("Open piano roll"));
    }

    void setTimeRange(double start, double length, double barSeconds) {
        if (m_rangeStart == start && m_rangeLength == length && m_barSeconds == barSeconds) return;
        m_rangeStart = start;
        m_rangeLength = length;
        m_barSeconds = barSeconds;
        m_cachedRevision = std::numeric_limits<std::uint64_t>::max();
        update();
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
        p.save();
        p.setClipRect(area);
        double grid = m_barSeconds / 4.0;
        while (grid / m_rangeLength * area.width() < 16.0) grid *= 2.0;
        p.setPen(QPen(mixColors(t.well(), t.textSecondary, 0.18), 1.0));
        for (double time = 0.0; time < m_rangeLength; time += grid) {
            const double x = area.left() + time / m_rangeLength * area.width();
            p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        }
        ensureNoteGeometry(*track, area, tempo);
        if (m_cachedNoteCount == 0) {
            p.setPen(t.textSecondary);
            QFont font = p.font();
            font.setPixelSize(9);
            p.setFont(font);
            p.drawText(rect().adjusted(10, 0, -10, 0),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(font).elidedText(
                           QObject::tr("Draw MIDI or drop a MIDI file"),
                           Qt::ElideRight, width() - 20));
            p.restore();
            return;
        }

        p.setPen(Qt::NoPen);
        p.setBrush(mixColors(accent, t.textPrimary, 0.46));
        if (m_cachedUseLod) p.setRenderHint(QPainter::Antialiasing, false);
        p.drawPath(m_cachedNotes);
        p.restore();
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

        int low = 127;
        int high = 0;
        for (const auto& clip : track.clips) {
            if (clip.kind != daw::ClipKind::Midi) continue;
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
                const double start = clip.startSeconds - clip.offsetSeconds +
                    daw::beatsToSeconds(note.startBeats, tempo);
                const double duration = daw::beatsToSeconds(
                    note.lengthBeats, tempo);
                const double x = area.left() + ((start - m_rangeStart) / m_rangeLength) * area.width();
                const double w = std::max(2.0, duration / m_rangeLength * area.width());
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
    double m_rangeStart = 0.0;
    double m_rangeLength = 2.0;
    double m_barSeconds = 2.0;
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
        setFixedWidth(kSourceWidth);
        setFixedHeight(40);
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
    std::function<void(QMenu*)> populateMenu;
    void setSourceColor(QColor color) {
        if (m_color == color) return;
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        const QRectF panel = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath shape;
        shape.addRoundedRect(panel, 7, 7);
        p.fillPath(shape, mixColors(t.surfaceElevated, t.accent,
                                    isDown() ? 0.15 : underMouse() ? 0.07 : 0.0));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(hasFocus() ? t.accent : t.separator(), hasFocus() ? 1.5 : 1.0));
        p.drawPath(shape);
        p.setPen(Qt::NoPen);
        p.setBrush(m_color.isValid() ? m_color : t.accent);
        p.drawRoundedRect(QRectF(8, 10, 3, height() - 20), 1.5, 1.5);

        QFont font = p.font();
        font.setPixelSize(12);
        font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(t.textPrimary);
        const QRect nameRect(18, 5, width() - 28, 16);
        p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(font).elidedText(text(), Qt::ElideRight, nameRect.width()));
        font.setPixelSize(10);
        font.setWeight(QFont::Normal);
        p.setFont(font);
        p.setPen(t.textSecondary);
        p.drawText(QRect(18, 22, width() - 28, 13), Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(font).elidedText(m_instrumentName.isEmpty()
                ? QObject::tr("Instrument") : m_instrumentName,
                Qt::ElideRight, width() - 28));
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
        if (populateMenu) populateMenu(&menu);
        menu.exec(event->globalPos());
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
    QColor m_color;
    QString m_instrumentName;
    QTimer m_openTimer;
    bool m_modelSynced = false;
};

} // namespace

PatternWindow::PatternWindow(daw::EngineController* controller, QWidget* parent)
    : QDialog(parent, Qt::Widget), m_controller(controller) {
    setObjectName(QStringLiteral("PatternWindow"));
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
    addColumn(tr("VOL"), kKnobSize);
    addColumn(tr("PAN"), kKnobSize);
    addColumn(tr("MIX"), 48);
    addColumn(tr("SOURCE"), kSourceWidth);
    auto* midi = new QLabel(tr("MIDI PATTERN"), columnHeader);
    columns->addWidget(midi, 1);
    addColumn(QString(), 32);
    addColumn(QString(), 28);
    root->addWidget(columnHeader);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("PatternScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_rowsHost = new QWidget(scroll);
    m_rowsLayout = new QVBoxLayout(m_rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(5);
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

int PatternWindow::replacementRowAtGlobal(const QPoint& globalPos) const {
    if (!m_rowsHost) return -1;
    const QPoint local = m_rowsHost->mapFromGlobal(globalPos);
    for (int i = 0; i < m_rowWidgets.size(); ++i) {
        const QWidget* row = m_rowWidgets[i];
        if (row && row->geometry().adjusted(0, 8, 0, -8).contains(local))
            return i;
    }
    return -1;
}

void PatternWindow::updateExternalDropFeedback(const QPoint& globalPos) {
    const int replacement = replacementRowAtGlobal(globalPos);
    if (replacement != m_externalReplaceIndex) {
        for (int i = 0; i < m_rowWidgets.size(); ++i) {
            QWidget* row = m_rowWidgets[i];
            if (!row) continue;
            const bool target = i == replacement;
            if (row->property("dropTarget").toBool() == target) continue;
            row->setProperty("dropTarget", target);
            row->style()->unpolish(row);
            row->style()->polish(row);
            row->update();
        }
        m_externalReplaceIndex = replacement;
    }
    if (replacement >= 0) {
        if (m_dropIndicator) m_dropIndicator->hide();
        m_dropIndex = -1;
    } else {
        updateDropIndicator(insertionIndexAtGlobal(globalPos));
    }
}

void PatternWindow::clearExternalDropFeedback() {
    m_externalReplaceIndex = -1;
    m_dropIndex = -1;
    if (m_dropIndicator) m_dropIndicator->hide();
    for (QWidget* row : std::as_const(m_rowWidgets)) {
        if (!row || !row->property("dropTarget").toBool()) continue;
        row->setProperty("dropTarget", false);
        row->style()->unpolish(row);
        row->style()->polish(row);
        row->update();
    }
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
                    const auto result =
                        m_controller->setTrackMuted(id.toStdString(), on);
                    syncRowsFromModel();
                    emit projectEdited(
                        daw::collab::marksLocalFileDirty(result));
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
        name->setSourceColor(rgb(track->color));
        name->populateMenu = [this, id](QMenu* menu) { populateSourceMenu(menu, id); };
        layout->addWidget(name);

        auto* fader = new PatternLevelKnob(row);
        fader->setObjectName(QStringLiteral("PatternSourceLevel"));
        fader->setAutomatable(true);
        connect(fader, &ui::FaderWidget::automateRequested, this,
                [this, id] { emit automateControlRequested(id, false); });
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
        layout->insertWidget(0, fader);

        auto* pan = new PatternPanKnob(row);
        pan->setObjectName(QStringLiteral("PatternSourcePan"));
        pan->setAutomatable(true);
        connect(pan, &ui::PanKnob::automateRequested, this,
                [this, id] { emit automateControlRequested(id, true); });
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
        layout->insertWidget(1, pan);

        auto* sketch = new SourceSketch(m_controller, id, row);
        sketch->setObjectName(QStringLiteral("PatternSourceSketch"));
        connect(sketch, &QAbstractButton::clicked, this,
                [this, id] { openRoll(id); });
        layout->addWidget(sketch, 1);

        auto* rhythm = new QToolButton(row);
        rhythm->setObjectName(QStringLiteral("PatternRhythm"));
        rhythm->setFixedSize(32, 32);
        rhythm->setIcon(icons::icon(icons::Glyph::GridDivision, th().textSecondary, 18));
        rhythm->setToolTip(tr("Fill rhythm — replace this sound’s MIDI with evenly spaced notes"));
        rhythm->setAccessibleName(tr("Fill rhythm"));
        rhythm->setCursor(Qt::PointingHandCursor);
        rhythm->setPopupMode(QToolButton::InstantPopup);
        auto* rhythmMenu = new QMenu(rhythm);
        populateRhythmMenu(rhythmMenu, id);
        rhythm->setMenu(rhythmMenu);
        layout->addWidget(rhythm);

        auto* remove = new ui::IconButton(icons::Glyph::Trash,
                                          tr("Remove source"), row);
        remove->setButtonSize(28, 28);
        remove->setCursor(Qt::PointingHandCursor);
        connect(remove, &QAbstractButton::clicked, this,
                [this, id] { removeSource(id); });
        layout->addWidget(remove);
        QWidget::setTabOrder(fader, pan);
        QWidget::setTabOrder(pan, mute);
        QWidget::setTabOrder(mute, solo);
        QWidget::setTabOrder(solo, name);
        QWidget::setTabOrder(name, sketch);
        QWidget::setTabOrder(sketch, rhythm);
        QWidget::setTabOrder(rhythm, remove);
        if (!m_rowWidgets.isEmpty()) {
            // Continue from the previous row's final control, not from its
            // source name (which was constructed before the two left knobs).
            auto* previousLayout = m_rowWidgets.back()->layout();
            auto* previous = previousLayout->itemAt(previousLayout->count() - 1)->widget();
            QWidget::setTabOrder(previous, fader);
        }
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
    auto* footer = new QWidget(m_rowsHost);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(0, 7, 0, 0);
    footerLayout->setSpacing(14);
    add->setText(tr("Add sound"));
    add->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    footerLayout->addWidget(add);
    auto* hint = new QLabel(tr("Drop audio to add a sound. Drop MIDI onto a sound to replace its notes."), footer);
    hint->setObjectName(QStringLiteral("PatternDropHint"));
    hint->setWordWrap(true);
    footerLayout->addWidget(hint, 1);
    m_rowsLayout->addWidget(footer);
    m_rowsLayout->addStretch(1);
    updateSelectionVisuals();
    syncRowsFromModel();
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
    const auto& project = m_controller->project();
    const double barSeconds = daw::beatsToSeconds(
        double(std::max(1, project.timeSigNumerator)) * 4.0 /
            double(std::max(1, project.timeSigDenominator)), project.tempo);
    double rangeStart = std::numeric_limits<double>::max();
    double rangeEnd = 0.0;
    if (const auto* pattern = project.findTrack(m_patternId.toStdString())) {
        for (const auto& clip : pattern->clips) {
            if (clip.kind != daw::ClipKind::Pattern) continue;
            rangeStart = std::min(rangeStart, clip.startSeconds);
            rangeEnd = std::max(rangeEnd, clip.startSeconds + clip.durationSeconds);
        }
    }
    if (rangeStart == std::numeric_limits<double>::max()) rangeStart = 0.0;
    const double rangeLength = std::max(barSeconds, rangeEnd - rangeStart);
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
        name->setSourceColor(rgb(track->color));
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
        sketch->setTimeRange(rangeStart, rangeLength, barSeconds);
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
                                   double startSeconds,
                                   int insertionIndex) {
    bool changed = false;
    QStringList failed;
    QStringList added;
    for (const QString& path : paths) {
        if (!ui::isAudioFile(path)) continue;
        const std::string id = m_controller->addPatternSample(
            m_patternId.toStdString(), path.toStdString(), startSeconds);
        const bool loaded = !id.empty();
        changed |= loaded;
        if (loaded) added.push_back(QString::fromStdString(id));
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
    if (insertionIndex >= 0 && !added.isEmpty()) {
        setSelectedSources(added, added.back());
        m_selectionAnchorId = added.back();
        reorderSelectedSources(insertionIndex);
    }
    emit projectEdited();
    refresh();
}

bool PatternWindow::replaceSample(const QString& trackId,
                                  const QString& path) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track || track->parentId != m_patternId.toStdString()) return false;
    const std::size_t undoStart = m_controller->undoDepth();
    const bool loaded = track->instrument.uid == "daw.sampler" &&
                                !track->instrument.id.empty()
                            ? m_controller->loadSamplerSample(
                                  track->id, track->instrument.id,
                                  path.toStdString())
                            : m_controller->loadInstrumentSampler(
                                  track->id, path.toStdString());
    if (!loaded) return false;
    const QString name = QFileInfo(path).completeBaseName().trimmed();
    if (!name.isEmpty())
        m_controller->renameTrack(trackId.toStdString(), name.toStdString());
    m_controller->collapseUndo(undoStart, "Replace Pattern Sample");
    setSelectedSources({trackId}, trackId);
    m_selectionAnchorId = trackId;
    emit projectEdited();
    refresh();
    return true;
}

void PatternWindow::chooseReplacementSample(const QString& trackId) {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Replace Pattern Sample"), QString(), ui::audioNameFilter());
    if (path.isEmpty()) return;
    if (replaceSample(trackId, path)) return;
    QMessageBox::warning(
        this, tr("Sample could not be replaced"),
        tr("The selected audio file could not be decoded or loaded into the "
           "Sampler."));
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
    const auto result = m_controller->renameTrack(
        trackId.toStdString(), name.trimmed().toStdString());
    emit projectEdited(daw::collab::marksLocalFileDirty(result));
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

void PatternWindow::populateRhythmMenu(QMenu* menu, const QString& trackId) {
    auto* title = menu->addAction(tr("Replace MIDI with a steady rhythm"));
    title->setEnabled(false);
    for (const int divisions : {1, 2, 4, 8, 16, 32}) {
        auto* action = menu->addAction(divisions == 1 ? tr("Every bar")
            : tr("Every 1/%1 bar").arg(divisions));
        action->setObjectName(QStringLiteral("pattern.fill.%1").arg(divisions));
        connect(action, &QAction::triggered, this,
                [this, trackId, divisions] { fillRhythm(trackId, divisions); });
    }
}

void PatternWindow::populateSourceMenu(QMenu* menu, const QString& trackId) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track) return;
    menu->addAction(tr("Open Instrument"), this, [this, trackId] { openInstrument(trackId); });
    menu->addAction(tr("Open piano roll"), this, [this, trackId] { openRoll(trackId); });
    auto* fill = menu->addMenu(tr("Fill rhythm"));
    populateRhythmMenu(fill, trackId);
    const std::string slotId = track->instrument.id;
    if (m_controller->samplerInstance(track->id, slotId)) {
        auto* cut = menu->addAction(tr("Cut Itself"));
        cut->setObjectName(QStringLiteral("pattern.cutItself"));
        cut->setCheckable(true);
        cut->setChecked(m_controller->insertParameter(track->id, slotId, "cutitself") >= 0.5);
        cut->setToolTip(tr("Each new note stops this Sampler’s previous voice."));
        connect(cut, &QAction::triggered, this, [this, trackId, slotId](bool on) {
            const auto id = trackId.toStdString();
            const double before = m_controller->insertParameter(id, slotId, "cutitself");
            m_controller->setInsertParameter(id, slotId, "cutitself", on ? 1.0 : 0.0);
            m_controller->commitInsertParameterEdit(id, slotId, "cutitself", before,
                                                     "Toggle Cut Itself");
            emit projectEdited();
        });
    }
    menu->addSeparator();
    menu->addAction(tr("Rename…"), this, [this, trackId] { renameSource(trackId); });
    menu->addAction(tr("Replace with Sample..."), this, [this, trackId] { chooseReplacementSample(trackId); });
    menu->addAction(tr("Duplicate Source"), this, [this, trackId] { duplicateSource(trackId); });
    menu->addSeparator();
    menu->addAction(tr("Remove Source"), this, [this, trackId] { removeSource(trackId); });
}

namespace {
double patternBarBeats(const daw::ProjectModel& project) {
    return double(std::max(1, project.timeSigNumerator)) * 4.0 /
           double(std::max(1, project.timeSigDenominator));
}

const daw::ClipModel* firstSourceMidi(const daw::TrackModel* track) {
    if (track) for (const auto& clip : track->clips)
        if (clip.kind == daw::ClipKind::Midi) return &clip;
    return nullptr;
}

const daw::ClipModel* sourcePatternClip(const daw::TrackModel* pattern,
                                        const daw::ClipModel* source) {
    if (pattern) for (const auto& clip : pattern->clips)
        if (clip.kind == daw::ClipKind::Pattern &&
            (!source || clip.id == source->patternClipId)) return &clip;
    return nullptr;
}
} // namespace

void PatternWindow::fillRhythm(const QString& trackId, int divisionsPerBar) {
    if (divisionsPerBar < 1 || divisionsPerBar > 32) return;
    const auto& project = m_controller->project();
    const auto* track = project.findTrack(trackId.toStdString());
    if (!track || track->parentId != m_patternId.toStdString()) return;
    const auto* clip = firstSourceMidi(track);
    const auto* owner = sourcePatternClip(project.findTrack(m_patternId.toStdString()), clip);
    const double bar = patternBarBeats(project);
    const double length = clip ? daw::secondsToBeats(clip->durationSeconds, project.tempo)
        : owner ? daw::secondsToBeats(owner->durationSeconds, project.tempo) : bar;
    const double step = bar / divisionsPerBar;
    const double count = std::ceil(length / step - 1e-9);
    if (count <= 0.0 || count > 65536.0) return;
    // A Sampler trigger uses its root key, so rhythm fill does not transpose
    // the sample. Other instruments retain the source's current pitch.
    int pitch = clip && !clip->notes.empty() ? clip->notes.front().pitch : 60;
    if (m_controller->samplerInstance(track->id, track->instrument.id))
        pitch = int(std::lround(m_controller->insertParameter(track->id,
                                             track->instrument.id, "rootnote")));
    std::vector<daw::NoteModel> notes;
    notes.reserve(std::size_t(count));
    for (int index = 0; index < int(count); ++index) {
        daw::NoteModel note;
        note.pitch = pitch;
        note.startBeats = index * step;
        note.lengthBeats = std::min({0.25, step * 0.5, length - note.startBeats});
        note.velocity = 100;
        notes.push_back(std::move(note));
    }
    replaceSourceNotes(trackId, std::move(notes), length, "Fill Pattern Rhythm");
}

bool PatternWindow::replaceSourceNotes(const QString& trackId,
    std::vector<daw::NoteModel> notes, double lengthBeats, const std::string& label) {
    const auto& project = m_controller->project();
    const auto* track = project.findTrack(trackId.toStdString());
    if (!track || track->parentId != m_patternId.toStdString() || notes.empty() ||
        !std::isfinite(lengthBeats) || lengthBeats <= 0.0) return false;
    const auto* source = firstSourceMidi(track);
    const auto* owner = sourcePatternClip(project.findTrack(m_patternId.toStdString()), source);
    const std::string id = track->id;
    const double start = source ? source->startSeconds : owner ? owner->startSeconds : 0.0;
    const double duration = std::max(daw::beatsToSeconds(lengthBeats, project.tempo),
                                     source ? source->durationSeconds : 0.0);
    const std::size_t undoStart = m_controller->undoDepth();
    const std::string clipId = source ? source->id
        : m_controller->addMidiClip(id, start, duration);
    if (clipId.empty()) return false;
    // Re-resolve after addMidiClip: it can allocate a Pattern owner and invalidate
    // model pointers. Extending the owner keeps a long imported phrase audible.
    track = project.findTrack(id);
    source = firstSourceMidi(track);
    owner = sourcePatternClip(project.findTrack(m_patternId.toStdString()), source);
    std::vector<std::pair<std::string, std::string>> trims{{id, clipId}};
    if (owner) trims.emplace_back(m_patternId.toStdString(), owner->id);
    const auto ownerId = owner ? owner->id : std::string{};
    const double ownerStart = owner ? owner->startSeconds : 0.0;
    const double ownerOffset = owner ? owner->offsetSeconds : 0.0;
    const double ownerLength = owner ? std::max(owner->durationSeconds,
                                    start + duration - ownerStart) : 0.0;
    m_controller->beginClipTrimEdit(trims);
    m_controller->setClipTrim(id, clipId, start, 0.0, duration);
    if (!ownerId.empty())
        m_controller->setClipTrim(m_patternId.toStdString(), ownerId,
                                  ownerStart, ownerOffset, ownerLength);
    m_controller->endClipTrimEdit(label);
    m_controller->setClipNotes(id, clipId, std::move(notes), label);
    m_controller->collapseUndo(undoStart, label);
    setSelectedSources({trackId}, trackId);
    refresh();
    emit projectEdited();
    return true;
}

bool PatternWindow::applyMidiFile(const QString& trackId, const QString& path,
                                   QString* error) {
    daw::midifile::File file;
    std::string parseError;
    if (!daw::midifile::parse(path.toStdString(), file, parseError)) {
        if (error) *error = QString::fromStdString(parseError);
        return false;
    }
    if (file.notes.empty()) {
        if (error) *error = tr("This MIDI file contains no notes.");
        return false;
    }
    std::vector<daw::NoteModel> notes;
    notes.reserve(file.notes.size());
    // A sound is the explicit drop target. Merge format-1 note tracks into it;
    // never create extra instrument lanes or adopt the file's tempo.
    for (const auto& input : file.notes) {
        daw::NoteModel note;
        note.pitch = input.pitch;
        note.startBeats = input.startBeats;
        note.lengthBeats = input.lengthBeats;
        note.velocity = input.velocity;
        notes.push_back(std::move(note));
    }
    const double bar = patternBarBeats(m_controller->project());
    return replaceSourceNotes(trackId, std::move(notes),
        std::max(bar, std::ceil(file.lengthBeats / bar - 1e-9) * bar),
        "Apply MIDI to Pattern Source");
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
    const auto urls = event->mimeData()->urls();
    const bool midi = urls.size() == 1 && urls.front().isLocalFile() &&
                       ui::isMidiFile(urls.front().toLocalFile());
    const bool audio = std::any_of(urls.begin(), urls.end(), [](const QUrl& url) {
        return url.isLocalFile() && ui::isAudioFile(url.toLocalFile());
    });
    if (!midi && !audio) return;
    event->setDropAction(Qt::CopyAction);
    event->accept();
    const auto global = mapToGlobal(event->position().toPoint());
    if (!midi || replacementRowAtGlobal(global) >= 0) updateExternalDropFeedback(global);
    else clearExternalDropFeedback();
}

void PatternWindow::dragMoveEvent(QDragMoveEvent* event) {
    const auto urls = event->mimeData()->urls();
    const bool midi = urls.size() == 1 && urls.front().isLocalFile() &&
                       ui::isMidiFile(urls.front().toLocalFile());
    const bool audio = std::any_of(urls.begin(), urls.end(), [](const QUrl& url) {
        return url.isLocalFile() && ui::isAudioFile(url.toLocalFile());
    });
    const auto global = mapToGlobal(event->position().toPoint());
    if ((!midi && !audio) || (midi && replacementRowAtGlobal(global) < 0)) {
        clearExternalDropFeedback();
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    updateExternalDropFeedback(global);
}

void PatternWindow::dragLeaveEvent(QDragLeaveEvent* event) {
    clearExternalDropFeedback();
    event->accept();
}

void PatternWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.front().isLocalFile() &&
        ui::isMidiFile(urls.front().toLocalFile())) {
        const int target = replacementRowAtGlobal(mapToGlobal(event->position().toPoint()));
        const auto ids = childTrackIds();
        clearExternalDropFeedback();
        if (target < 0 || target >= ids.size()) { event->ignore(); return; }
        QString error;
        if (!applyMidiFile(ids[target], urls.front().toLocalFile(), &error)) {
            QMessageBox::warning(this, tr("MIDI could not be applied"), error);
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QStringList files;
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (ui::isAudioFile(path)) files.push_back(path);
    }
    if (files.isEmpty()) return;
    const QPoint globalPos = mapToGlobal(event->position().toPoint());
    const int replacement = replacementRowAtGlobal(globalPos);
    const int insertion = insertionIndexAtGlobal(globalPos);
    const QStringList ids = childTrackIds();
    clearExternalDropFeedback();
    event->acceptProposedAction();
    if (replacement >= 0 && replacement < ids.size()) {
        const QString first = files.takeFirst();
        if (!replaceSample(ids[replacement], first)) {
            QMessageBox::warning(
                this, tr("Sample could not be replaced"),
                tr("The selected audio file could not be decoded or loaded "
                   "into the Sampler."));
        }
        if (!files.isEmpty()) addSampleFiles(files, 0.0, replacement + 1);
        return;
    }
    addSampleFiles(files, 0.0, insertion);
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

bool PatternWindow::checkEditingForTest() {
    daw::EngineController controller;
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto sampler = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
    if (!sampler) return false;
    const std::string pattern = controller.addPattern("Pattern checks");
    const std::string first = controller.addPatternInstrument(pattern, *sampler);
    const std::string second = controller.addPatternInstrument(pattern, *sampler);
    if (first.empty() || second.empty()) return false;
    const auto qFirst = QString::fromStdString(first);
    const auto qSecond = QString::fromStdString(second);
    auto firstClip = [&]() { return firstSourceMidi(controller.project().findTrack(first)); };
    const auto clipId = firstClip()->id;
    const auto slotId = controller.project().findTrack(first)->instrument.id;
    PatternWindow window(&controller);
    window.setPattern(QString::fromStdString(pattern));
    window.resize(640, 340);
    window.show();
    QApplication::processEvents();
    bool ok = true;
    const auto check = [&ok](bool condition, const char* message) {
        if (!condition) std::fprintf(stderr, "pattern editing: %s\n", message);
        ok &= condition;
    };
    check(window.checkInteractionGesturesForTest(), "stable rows and model sync");
    auto* level = window.m_rowWidgets[0]->findChild<ui::FaderWidget*>("PatternSourceLevel");
    auto* pan = window.m_rowWidgets[0]->findChild<ui::PanKnob*>("PatternSourcePan");
    auto* name = window.m_rowWidgets[0]->findChild<QWidget*>("PatternSourceName");
    auto* scroll = window.findChild<QScrollArea*>("PatternScroll");
    check(level && pan && name && level->isCompactKnob() &&
          level->geometry().right() < pan->geometry().left() &&
          pan->geometry().right() < name->geometry().left() &&
          scroll->horizontalScrollBar()->maximum() == 0, "round controls fit at the far left at 640 px");
    const double gain = level->gain();
    const double panValue = pan->pan();
    const auto wheel = [](QWidget* target) {
        const QPointF at(target->rect().center());
        QWheelEvent event(at, target->mapToGlobal(at.toPoint()), {}, QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &event);
    };
    wheel(level); wheel(pan);
    check(level->gain() == gain && pan->pan() == panValue, "scrolling must not edit the mix");

    QMenu sourceMenu;
    window.populateSourceMenu(&sourceMenu, qFirst);
    auto* cut = sourceMenu.findChild<QAction*>("pattern.cutItself");
    check(cut && cut->isCheckable(), "Sampler Cut Itself action exists");
    if (cut) {
        const auto depth = controller.undoDepth();
        cut->trigger();
        check(controller.insertParameter(first, slotId, "cutitself") == 1.0 &&
              controller.undoDepth() == depth + 1, "Cut Itself edits the actual Sampler with one undo");
        controller.undo();
        check(controller.insertParameter(first, slotId, "cutitself") == 0.0, "Cut Itself undo");
    }
    QMenu rhythm;
    window.populateRhythmMenu(&rhythm, qFirst);
    rhythm.findChild<QAction*>("pattern.fill.4")->trigger();
    check(firstClip()->notes.size() == 4 && firstClip()->notes[3].startBeats == 3.0,
          "quarter-bar action makes four on the floor");
    const QString checkShot = qEnvironmentVariable("VLTONE_PATTERN_CHECK_SHOT");
    if (!checkShot.isEmpty()) {
        QApplication::processEvents();
        window.grab().save(checkShot);
    }
    const auto quarters = firstClip()->notes;
    const auto depth = controller.undoDepth();
    rhythm.findChild<QAction*>("pattern.fill.8")->trigger();
    check(firstClip()->notes.size() == 8 && firstClip()->notes[7].startBeats == 3.5 &&
          controller.undoDepth() == depth + 1, "eighth-bar action is a single edit");
    controller.undo();
    check(firstClip()->notes == quarters, "fill undo preserves original notes and IDs");

    QTemporaryDir dir;
    const auto midiPath = dir.filePath("phrase.mid");
    QFile file(midiPath);
    if (!file.open(QIODevice::WriteOnly)) return false;
    // Format 1: two note tracks, ending at beat 9. Both must land on this
    // sound, and the Pattern owner must grow to make the last note audible.
    file.write(QByteArray::fromHex(
        "4d54686400000006000100020060"
        "4d54726b0000000c00903c6460803c0000ff2f00"
        "4d54726b0000000d86009043506080430000ff2f00"));
    file.close();
    const auto tracksBefore = controller.project().tracks.size();
    const auto notesBefore = firstClip()->notes;
    const double lengthBefore = firstClip()->durationSeconds;
    const auto ownerBefore = *sourcePatternClip(controller.project().findTrack(pattern), firstClip());
    const auto secondNotes = firstSourceMidi(controller.project().findTrack(second))->notes;
    const auto importDepth = controller.undoDepth();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(midiPath)});
    const QPoint at = name->mapTo(&window, name->rect().center());
    QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &enter);
    QDropEvent drop(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &drop);
    check(drop.isAccepted() && firstClip()->notes.size() == 2 &&
          firstClip()->notes.back().startBeats == 8.0 &&
          firstClip()->notes.back().pitch == 67 &&
          controller.undoDepth() == importDepth + 1 &&
          daw::secondsToBeats(firstClip()->durationSeconds, controller.tempo()) == 12.0 &&
          sourcePatternClip(controller.project().findTrack(pattern), firstClip())->durationSeconds >=
              firstClip()->durationSeconds, "MIDI drop merges tracks and grows the audible Pattern in one undo");
    check(controller.project().tracks.size() == tracksBefore &&
          controller.project().findTrack(first)->instrument.id == slotId &&
          firstSourceMidi(controller.project().findTrack(second))->notes == secondNotes,
          "MIDI drop preserves instrument, siblings and track count");
    const auto imported = firstClip()->notes;
    controller.undo();
    check(firstClip()->notes == notesBefore && firstClip()->durationSeconds == lengthBefore &&
          sourcePatternClip(controller.project().findTrack(pattern), firstClip())->durationSeconds ==
              ownerBefore.durationSeconds, "MIDI undo restores notes and both clip boundaries");
    controller.redo();
    check(firstClip()->notes == imported, "MIDI redo preserves imported IDs");
    const auto afterImport = controller.undoDepth();
    QDropEvent outside(QPointF(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    window.dropEvent(&outside);
    check(!outside.isAccepted() && controller.undoDepth() == afterImport,
          "MIDI dropped outside a sound is ignored");
    QString error;
    check(!window.applyMidiFile(qFirst, dir.filePath("missing.mid"), &error) &&
          !error.isEmpty() && controller.undoDepth() == afterImport, "invalid MIDI leaves the project intact");

    controller.setTimeSignature(6, 8);
    controller.beginClipTrimEdit(first, clipId);
    controller.setClipTrim(first, clipId, 0.0, 0.0, daw::beatsToSeconds(3.0, controller.tempo()));
    controller.endClipTrimEdit("Test meter");
    window.fillRhythm(qFirst, 8);
    check(firstClip()->notes.size() == 8 && firstClip()->notes.back().startBeats == 2.625,
          "bar fractions respect 6/8 rather than assuming four quarter notes");

    PianoRollWindow roll(&controller);
    roll.setClip(qFirst, QString::fromStdString(clipId));
    roll.show();
    QApplication::processEvents();
    auto* view = roll.findChild<PianoRollView*>();
    check(view && view->ghostTracks() == QSet<QString>{qSecond}, "sibling ghost notes default on");
    auto* ghostMenu = roll.findChild<QMenu*>("PianoRollGhostMenu");
    QMetaObject::invokeMethod(ghostMenu, "aboutToShow", Qt::DirectConnection);
    auto* automatic = ghostMenu->findChild<QAction*>("pianoRoll.autoPatternGhosts");
    check(automatic && automatic->isChecked(), "automatic ghosts are discoverable in the menu");
    if (automatic) {
        automatic->trigger();
        roll.refresh();
        check(view->ghostTracks().isEmpty(), "explicit ghost opt-out survives refresh");
        automatic->trigger();
        check(view->ghostTracks().contains(qSecond), "automatic ghosts can be restored");
    }
    const auto third = controller.addPatternInstrument(pattern, *sampler);
    roll.refresh();
    check(view->ghostTracks().contains(QString::fromStdString(third)), "new Pattern sounds become ghosts automatically");
    roll.setClip(qSecond, QString::fromStdString(firstSourceMidi(controller.project().findTrack(second))->id));
    check(view->ghostTracks().contains(qFirst) && !view->ghostTracks().contains(qSecond),
          "switching sounds swaps the active and ghost lanes");
    const auto otherPattern = controller.addPattern("Other pattern");
    const auto other = controller.addPatternInstrument(otherPattern, *sampler);
    roll.setClip(QString::fromStdString(other), QString::fromStdString(
        firstSourceMidi(controller.project().findTrack(other))->id));
    check(view->ghostTracks().isEmpty(), "automatic ghosts never leak into another Pattern");
    // A source whose MIDI was deleted still gets a correctly owned clip.
    controller.removeClip(first, clipId);
    const auto emptyDepth = controller.undoDepth();
    window.fillRhythm(qFirst, 4);
    check(firstClip() && !firstClip()->patternClipId.empty() &&
          !firstClip()->notes.empty() && controller.undoDepth() == emptyDepth + 1,
          "filling an empty source creates MIDI and its ownership in one undo");
    controller.undo();
    check(!firstClip(), "undo removes a MIDI clip created by fill");
    return ok;
}

void PatternWindow::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#PatternWindow { background: %BG%; color: %TEXT%; }
#PatternColumnHeader { background: %BG%; }
#PatternColumnHeader QLabel { color: %TEXT2%; font-size: 9px;
                              font-weight: 500; letter-spacing: 0.3px; }
#PatternScroll { background: %BG%; }
#PatternSourceRow { background: %SURFACE%; border: 1px solid %SEP%;
                    border-radius: 8px; }
#PatternSourceRow:hover { background: %HOVER%; border-color: %SECTION%; }
#PatternSourceRow[selected="true"] { background: %SELECTED%;
    border-color: %ACCENT%; }
#PatternSourceRow[primary="true"] { border-color: %ACCENT%; }
#PatternSourceRow[dropTarget="true"] { background: %SELECTED%;
    border: 2px solid %ACCENT%; }
#PatternDropIndicator { background: %ACCENT%; border-radius: 1px; }
QToolButton#PatternToolbarButton { color: %TEXT%; background: %WELL%;
    border: 1px solid %SEP%; border-radius: 7px; padding: 4px 8px; }
QToolButton#PatternToolbarButton:hover { background: %HOVER%;
    border-color: %ACCENT%; }
QToolButton#PatternToolbarButton:disabled { color: %TEXT2%; background: %SURFACE%; }
QToolButton#PatternAddInstrument { min-height: 28px; padding: 4px 10px;
    color: %TEXT%; background: %WELL%; border: 1px solid %SEP%; border-radius: 7px; }
QToolButton#PatternAddInstrument:hover { background: %HOVER%; border-color: %ACCENT%; }
#PatternDropHint { color: %TEXT2%; font-size: 10px; }
QToolButton#PatternRhythm { border: 1px solid transparent; border-radius: 6px; }
QToolButton#PatternRhythm:hover, QToolButton#PatternRhythm:focus {
    background: %HOVER%; border-color: %ACCENT%; }
QToolButton#PatternRhythm::menu-indicator { image: none; }
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
