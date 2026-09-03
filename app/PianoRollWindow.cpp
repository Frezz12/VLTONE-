#include "PianoRollWindow.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "KeyboardLayout.hpp"
#include "NoteContextPanel.hpp"
#include "EngineController.hpp"
#include "PianoRollTools.hpp"
#include "Theme.hpp"
#include "UndoTranslations.hpp"
#include "UiConstants.hpp"

#include <QActionGroup>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCursor>
#include <QColorDialog>
#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QLinearGradient>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QRegion>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QToolButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <utility>

namespace mt = daw::miditools;

namespace {

constexpr int kMinPitch = 0;
constexpr int kMaxPitch = 127;
constexpr int kPitchCount = kMaxPitch - kMinPitch + 1;
constexpr double kKeyboardWidth = 56.0;
/// Grab zone for the edge resizes, matching the arrangement's feel.
constexpr double kEdgePx = 6.0;
constexpr double kMinNoteBeats = 1.0 / 32.0;

constexpr double kMinRowHeight = 5.0;
constexpr double kMaxRowHeight = 40.0;
constexpr double kMinPxPerBeat = 4.0;
constexpr double kMaxPxPerBeat = 800.0;
/// How narrow a beat is allowed to get when the roll sizes itself on open. A
/// long clip fitted to the window puts sixteenths a few pixels apart, which is
/// neither readable nor clickable, so it opens scrolled at this width instead.
/// Only the automatic fit is floored — zooming out by hand still goes to
/// `kMinPxPerBeat`.
constexpr double kMinFitPxPerBeat = 160.0;

/// The velocity lane along the bottom: a stalk per note with a grab circle at
/// its top, the height standing for the velocity.
constexpr double kLaneHeight = 84.0;
constexpr double kMinLaneHeight = 44.0;
constexpr double kMaxLaneHeight = 360.0;
/// Grab strip along the lane's top edge that drags it taller or shorter.
constexpr double kLaneGripPx = 5.0;
constexpr double kLanePadding = 12.0;
constexpr double kHandleRadius = 4.5;
constexpr double kHandleGrabPx = 7.0;
/// Wheel units per velocity step. One notch is 120, so a notch moves 3.
constexpr int kWheelPerStep = 40;
/// Consecutive wheel/trackpad value changes remain one undo gesture until the
/// hand has been idle for a fraction of a second.
constexpr int kWheelEditCommitMs = 200;
/// Tool transforms can be much heavier than painting a frame. Slider events are
/// coalesced to the display cadence so dragging a control cannot queue stale
/// whole-clip transforms faster than the result can be shown.
constexpr int kToolPreviewFrameMs = 16;
/// Controller-lane setters normalise the whole curve and rebuild the track's
/// automation snapshot. Pointer samples are cheaper than that work, so retain
/// only the newest sample until the next display frame.
constexpr int kControllerLaneFrameMs = 16;
/// Ignore the few pixels a stationary mouse reports between press and release;
/// without this, a click intended to place a default-length note immediately
/// turns into a resize down to the hard minimum.
constexpr double kDrawResizeThresholdPx = 5.0;
int editShortcutKey(const QKeyEvent* event) {
    const int physical = ui::physicalUsKey(event);
    return physical ? physical : event->key();
}

bool isPrimaryEditChord(const QKeyEvent* event) {
    const Qt::KeyboardModifiers modifiers =
        event->modifiers() & ~Qt::KeypadModifier;
#if defined(Q_OS_MACOS)
    return modifiers == Qt::ControlModifier || modifiers == Qt::MetaModifier;
#else
    return modifiers == Qt::ControlModifier;
#endif
}

bool isPianoRollEditShortcut(const QKeyEvent* event) {
    if (!isPrimaryEditChord(event)) return false;
    switch (editShortcutKey(event)) {
        case Qt::Key_X:
        case Qt::Key_C:
        case Qt::Key_V:
        case Qt::Key_B:
            return true;
        default:
            return false;
    }
}

bool isTextEntry(const QWidget* widget) {
    return qobject_cast<const QLineEdit*>(widget) ||
           qobject_cast<const QTextEdit*>(widget) ||
           qobject_cast<const QPlainTextEdit*>(widget) ||
           qobject_cast<const QAbstractSpinBox*>(widget) ||
           (qobject_cast<const QComboBox*>(widget) &&
            qobject_cast<const QComboBox*>(widget)->isEditable());
}

bool segmentCrossesRect(const QPointF& from, const QPointF& to,
                        const QRectF& rect) {
    const QRectF target = rect.adjusted(-1.5, -1.5, 1.5, 1.5);
    if (target.contains(from) || target.contains(to)) return true;
    const QLineF stroke(from, to);
    if (stroke.length() < 0.01) return false;
    const QLineF edges[] = {
        QLineF(target.topLeft(), target.topRight()),
        QLineF(target.topRight(), target.bottomRight()),
        QLineF(target.bottomRight(), target.bottomLeft()),
        QLineF(target.bottomLeft(), target.topLeft()),
    };
    for (const QLineF& edge : edges) {
        if (stroke.intersects(edge, nullptr) == QLineF::BoundedIntersection)
            return true;
    }
    return false;
}

/// Compact vertical scrubber used beside the upper timeline scrollbar. The
/// control tracks the pointer directly: upward makes rows taller, downward
/// makes them thinner, while the wheel/menus remain keyboard alternatives.
class RowHeightScrubber final : public QToolButton {
public:
    using ReadValue = std::function<double()>;
    using WriteValue = std::function<void(double)>;

    explicit RowHeightScrubber(QWidget* parent = nullptr) : QToolButton(parent) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
        setFixedSize(28, 20);
    }

    void bind(ReadValue read, WriteValue write) {
        m_read = std::move(read);
        m_write = std::move(write);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !m_read) {
            QToolButton::mousePressEvent(event);
            return;
        }
        m_dragging = true;
        m_startY = event->globalPosition().y();
        m_startValue = m_read();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_dragging || !(event->buttons() & Qt::LeftButton) || !m_write) {
            QToolButton::mouseMoveEvent(event);
            return;
        }
        const double upward = m_startY - event->globalPosition().y();
        m_write(m_startValue + upward * 0.18);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!m_dragging || event->button() != Qt::LeftButton) {
            QToolButton::mouseReleaseEvent(event);
            return;
        }
        m_dragging = false;
        setCursor(Qt::SizeVerCursor);
        event->accept();
    }

private:
    ReadValue m_read;
    WriteValue m_write;
    qreal m_startY = 0.0;
    double m_startValue = 12.0;
    bool m_dragging = false;
};

/// Horizontal overview/navigation strip. Its ordinary thumb still pans like a
/// native scrollbar; only two explicit edges add gestures: the lower edge
/// changes strip thickness and the thumb's right edge changes time zoom.
class PianoRollNavigator final : public QScrollBar {
public:
    using ReadZoom = std::function<double()>;
    using WriteZoom = std::function<void(double)>;

    explicit PianoRollNavigator(QWidget* parent = nullptr)
        : QScrollBar(Qt::Horizontal, parent) {
        setObjectName(QStringLiteral("PianoRollNavigator"));
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
        setNavigatorHeight(18);
        setToolTip(QObject::tr(
            "Drag to move · drag the right edge to zoom · drag the lower edge "
            "to change thickness"));
        setAccessibleName(QObject::tr("Piano Roll navigation overview"));
        setAccessibleDescription(QObject::tr(
            "Drag the thumb to scroll, its right edge to zoom, or the lower "
            "edge to change the control thickness."));
    }

    void bindZoom(ReadZoom read, WriteZoom write) {
        m_readZoom = std::move(read);
        m_writeZoom = std::move(write);
    }

    void setNavigatorHeight(int height) {
        setFixedHeight(std::clamp(height, kMinHeight, kMaxHeight));
    }

    QRect thumbRectForTest() const { return thumbRect(); }

protected:
    void paintEvent(QPaintEvent* event) override {
        QScrollBar::paintEvent(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        const QRect thumb = thumbRect();

        // A bright bracket marks the only thumb edge that changes zoom.
        if (!thumb.isEmpty()) {
            const QColor zoomInk =
                m_mode == Mode::Zoom || m_hoverZoom ? t.accent : t.textSecondary;
            const double x = thumb.right() - 1.0;
            const double top = thumb.top() + 3.0;
            const double bottom = thumb.bottom() - 3.0;
            p.setPen(QPen(zoomInk, m_mode == Mode::Zoom ? 2.2 : 1.4,
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x, top), QPointF(x, bottom));
            p.drawLine(QPointF(x - 3.0, top), QPointF(x, top));
            p.drawLine(QPointF(x - 3.0, bottom), QPointF(x, bottom));
        }

        // The lower-edge grip is short enough not to look like another value
        // track, but visible enough to advertise vertical resizing.
        const QColor heightInk =
            m_mode == Mode::Height || m_hoverBottom ? t.accent : t.textSecondary;
        p.setPen(QPen(heightInk, m_mode == Mode::Height ? 2.2 : 1.4,
                      Qt::SolidLine, Qt::RoundCap));
        const double cx = width() / 2.0;
        const double edgeY = height() - 2.5;
        p.drawLine(QPointF(cx - 16.0, edgeY), QPointF(cx + 16.0, edgeY));
        p.drawLine(QPointF(cx - 3.5, edgeY - 3.0), QPointF(cx, edgeY));
        p.drawLine(QPointF(cx + 3.5, edgeY - 3.0), QPointF(cx, edgeY));
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton &&
            onBottomEdge(event->position())) {
            m_mode = Mode::Height;
            m_pressGlobal = event->globalPosition();
            m_startHeight = height();
            setCursor(Qt::SizeVerCursor);
            event->accept();
            update();
            return;
        }
        if (event->button() == Qt::LeftButton &&
            onZoomEdge(event->position()) && m_readZoom && m_writeZoom) {
            m_mode = Mode::Zoom;
            m_pressGlobal = event->globalPosition();
            m_startZoom = m_readZoom();
            setCursor(Qt::SizeHorCursor);
            event->accept();
            update();
            return;
        }
        m_mode = Mode::Scroll;
        QScrollBar::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_mode == Mode::Height) {
            const int delta = int(std::lround(event->globalPosition().y() -
                                              m_pressGlobal.y()));
            setNavigatorHeight(m_startHeight + delta);
            event->accept();
            update();
            return;
        }
        if (m_mode == Mode::Zoom && m_writeZoom) {
            const double delta = event->globalPosition().x() - m_pressGlobal.x();
            // Left shortens the represented viewport and therefore zooms in;
            // right lengthens it and zooms out. Exponential mapping gives the
            // same perceived sensitivity at both wide and narrow scales.
            m_writeZoom(m_startZoom * std::exp(-delta / 160.0));
            event->accept();
            update();
            return;
        }
        if (m_mode == Mode::Scroll) {
            QScrollBar::mouseMoveEvent(event);
            return;
        }
        updateHover(event->position());
        QScrollBar::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (m_mode == Mode::Height || m_mode == Mode::Zoom) {
            m_mode = Mode::None;
            updateHover(event->position());
            event->accept();
            update();
            return;
        }
        QScrollBar::mouseReleaseEvent(event);
        m_mode = Mode::None;
        updateHover(event->position());
    }

    void leaveEvent(QEvent* event) override {
        if (m_mode == Mode::None) {
            m_hoverBottom = false;
            m_hoverZoom = false;
            unsetCursor();
            update();
        }
        QScrollBar::leaveEvent(event);
    }

private:
    enum class Mode { None, Scroll, Height, Zoom };
    static constexpr int kMinHeight = 14;
    static constexpr int kMaxHeight = 56;
    static constexpr double kEdgeGrab = 6.0;

    QRect thumbRect() const {
        QStyleOptionSlider option;
        initStyleOption(&option);
        return style()->subControlRect(QStyle::CC_ScrollBar, &option,
                                       QStyle::SC_ScrollBarSlider, this);
    }

    bool onBottomEdge(const QPointF& point) const {
        return point.y() >= height() - kEdgeGrab;
    }

    bool onZoomEdge(const QPointF& point) const {
        const QRect thumb = thumbRect();
        return !thumb.isEmpty() &&
               point.y() >= thumb.top() && point.y() <= thumb.bottom() &&
               std::abs(point.x() - thumb.right()) <= kEdgeGrab;
    }

    void updateHover(const QPointF& point) {
        m_hoverBottom = onBottomEdge(point);
        m_hoverZoom = !m_hoverBottom && onZoomEdge(point);
        setCursor(m_hoverBottom ? Qt::SizeVerCursor
                                : m_hoverZoom ? Qt::SizeHorCursor
                                              : Qt::ArrowCursor);
        update();
    }

    ReadZoom m_readZoom;
    WriteZoom m_writeZoom;
    QPointF m_pressGlobal;
    int m_startHeight = 18;
    double m_startZoom = 1.0;
    Mode m_mode = Mode::None;
    bool m_hoverBottom = false;
    bool m_hoverZoom = false;
};

/// Tool cursors drawn from the icon set, so the pointer says which tool is
/// live. Built once and cached: a QCursor is rasterised from a pixmap, and
/// doing that per mouse-move would repaint an icon several hundred times a
/// second for nothing.
const QCursor& toolCursor(icons::Glyph glyph) {
    static QHash<int, QCursor> cache;
    auto it = cache.find(int(glyph));
    if (it != cache.end()) return it.value();
    const qreal dpr = qApp->devicePixelRatio();
    const int size = 24;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Drawn twice: a dark halo under a light glyph, so the cursor stays visible
    // over both a pale note and the near-black grid.
    icons::paint(p, glyph, QRectF(0, 0, size, size), QColor(0, 0, 0, 150));
    icons::paint(p, glyph, QRectF(0.6, 0.6, size - 1.2, size - 1.2),
                 QColor(0xFA, 0xFA, 0xFA));
    p.end();
    // Hot spot at the working end of each tool, not at the icon's centre: the
    // brush paints from its bristles, the blade cuts at its edge, and the
    // arrow points from its own tip.
    const QPoint hot = glyph == icons::Glyph::Brush   ? QPoint(6, 18)
                     : glyph == icons::Glyph::Knife   ? QPoint(12, 18)
                     : glyph == icons::Glyph::Pointer ? QPoint(6, 4)
                                                      : QPoint(12, 12);
    return *cache.insert(int(glyph), QCursor(pm, hot.x(), hot.y()));
}

/// Note copy/paste survives changing clip and closing the window, which is the
/// whole point of it — a phrase gets copied from one part into another.
std::vector<daw::NoteModel>& clipboard() {
    static std::vector<daw::NoteModel> notes;
    return notes;
}

bool isBlackKey(int pitch) {
    switch (((pitch % 12) + 12) % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

QString noteName(int pitch) {
    return QString::fromStdString(mt::pitchName(pitch));
}

/// Piano keys meet the window with a square edge and round only at the playing
/// end. A fully rounded rectangle starts outside the widget at x=0, so Qt clips
/// its antialiased corners and makes the left edge look bitten away.
QPainterPath pianoKeyPath(const QRectF& rect, qreal radius) {
    const qreal r = std::clamp(radius, 0.0,
                               std::min(rect.width(), rect.height()) * 0.5);
    QPainterPath path;
    path.moveTo(rect.left(), rect.top());
    path.lineTo(rect.right() - r, rect.top());
    path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + r);
    path.lineTo(rect.right(), rect.bottom() - r);
    path.quadTo(rect.right(), rect.bottom(), rect.right() - r, rect.bottom());
    path.lineTo(rect.left(), rect.bottom());
    path.closeSubpath();
    return path;
}

/// Every choice the roll offers lives under "pianoRoll/" in the user's
/// settings, so the window comes back the way it was left rather than at the
/// factory defaults.
QVariant pianoRollPref(const QString& key, const QVariant& fallback) {
    return QSettings().value(QStringLiteral("pianoRoll/") + key, fallback);
}

void setPianoRollPref(const QString& key, const QVariant& value) {
    QSettings().setValue(QStringLiteral("pianoRoll/") + key, value);
}

/// The snap ladder, coarse to fine. Adaptive snap walks it looking for the
/// finest division that is still wide enough to aim at.
const QVector<double>& snapLadder() {
    static const QVector<double> beats = {4.0,  2.0,   1.0,    0.5,
                                          0.25, 0.125, 0.0625, 0.03125};
    return beats;
}

} // namespace

// ── PianoRollView ───────────────────────────────────────────────────────────

PianoRollView::PianoRollView(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setMouseTracking(true);
    // Delete has to reach us, and a click must be able to take focus away from
    // a menu or a tool dialog.
    setFocusPolicy(Qt::StrongFocus);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
    m_controllerLaneWriteTimer = new QTimer(this);
    m_controllerLaneWriteTimer->setSingleShot(true);
    m_controllerLaneWriteTimer->setInterval(kControllerLaneFrameMs);
    m_controllerLaneWriteTimer->setTimerType(Qt::PreciseTimer);
    connect(m_controllerLaneWriteTimer, &QTimer::timeout, this,
            [this] { flushControllerLaneWrite(); });
    m_wheelEditTimer = new QTimer(this);
    m_wheelEditTimer->setSingleShot(true);
    m_wheelEditTimer->setInterval(kWheelEditCommitMs);
    connect(m_wheelEditTimer, &QTimer::timeout, this,
            [this] { finishWheelNoteEdit(); });
}

PianoRollView::~PianoRollView() {
    finishWheelNoteEdit();
    cancelControllerLaneWrite();
    commitPendingErase();
    if (m_gestureUndoActive || m_selectionEditUndoActive) {
        m_controller->endNoteEdit(m_eraseChanged ? "Erase Notes"
                                                  : "Edit Notes");
    }
    stopAudition();
}

void PianoRollView::setClip(const QString& trackId, const QString& clipId) {
    // Whatever the keyboard is sounding belongs to the track being left.
    finishWheelNoteEdit();
    commitPendingErase();
    if (m_gestureUndoActive || m_selectionEditUndoActive) {
        m_controller->endNoteEdit(m_eraseChanged ? "Erase Notes"
                                                  : "Edit Notes");
        m_gestureUndoActive = false;
        m_selectionEditUndoActive = false;
    }
    stopAudition();
    cancelControllerLaneWrite();
    m_pressedKey = -1;
    m_paintClip = nullptr;
    m_trackId = trackId;
    m_clipId = clipId;
    m_lastPlayheadPixel = -1;
    m_lastSoundingPitches.reset();
    invalidateSoundingPitchIndex();
    invalidateDocumentPaintCaches();
    m_selected.clear();
    m_primary.clear();
    m_moving = m_resizing = m_resizingLeft = false;
    m_resizeOrig.clear();
    m_moveWorking.clear();
    m_geometryPaintNotes.clear();
    m_noteUpdateScratch.clear();
    m_laneOrig.clear();
    m_laneDragging = m_marquee = m_erasing = m_muting = false;
    m_eraseChanged = false;
    m_pendingErase.clear();
    m_drawing = false;
    m_velocityEditOriginal.clear();
    m_velocityEditActive = false;
    m_selectionEditWorking.clear();
    m_laneResizing = false;
    m_lanePointDrag = -1;
    m_lanePointsBefore.clear();
    m_laneWorkingPoints.clear();
    m_laneLastWrittenPoint.reset();
    // Only a *controller* lane is tied to the clip that was open — its id means
    // nothing in the new one. Velocity and pan are properties of any note, so
    // pointing the lane back at velocity every time a clip opens would throw
    // away a choice the user made on purpose.
    if (m_laneParam == LaneParam::Controller) {
        m_laneParam = LaneParam::Velocity;
        m_laneId.clear();
    }
    m_preview.reset();
    invalidateNotePaintIndex();
    m_previewSelection.clear();
    m_previewWholeClip = true;
    m_pxPerBeat = 0.0;   // let the new clip pick its own width
    m_scrollX = 0.0;
    emit selectionChanged();
    emit viewportChanged();
    emitStatus();
    update();
}

const daw::ClipModel* PianoRollView::clip() const {
    if (m_paintClip) return m_paintClip;
    if (!m_controller || m_trackId.isEmpty() || m_clipId.isEmpty()) return nullptr;
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    if (!track) return nullptr;
    const std::string id = m_clipId.toStdString();
    for (const auto& c : track->clips) {
        if (c.id == id && c.kind == daw::ClipKind::Midi) return &c;
    }
    return nullptr;
}

const daw::NoteModel* PianoRollView::note(const QString& noteId) const {
    const auto* c = clip();
    if (!c || noteId.isEmpty()) return nullptr;
    ensureDocumentNoteIdIndex(*c);
    const std::string id = noteId.toStdString();
    const auto found = m_noteById.find(id);
    if (found != m_noteById.end() && found->second < c->notes.size() &&
        c->notes[found->second].id == id)
        return &c->notes[found->second];
    return nullptr;
}

const mt::Notes& PianoRollView::visibleNotes() const {
    static const mt::Notes empty;
    if (m_preview) return *m_preview;
    const auto* c = clip();
    return c ? c->notes : empty;
}

// ── Grid, snap, scale ───────────────────────────────────────────────────────

void PianoRollView::setGridBeats(double beats) {
    m_gridBeats = beats;
    update();
}

double PianoRollView::effectiveGridBeats() const {
    if (!m_adaptiveSnap) return m_gridBeats;
    // The finest division still worth aiming at: anything under ~10 px apart is
    // a line you cannot hit on purpose, so the grid coarsens as you zoom out.
    const double px = pxPerBeat();
    for (double beats : snapLadder()) {
        if (beats * px >= 10.0) return beats;
    }
    return snapLadder().back();
}

void PianoRollView::setSnapEnabled(bool enabled) {
    m_snapEnabled = enabled;
    update();
}

void PianoRollView::setAdaptiveSnap(bool enabled) {
    m_adaptiveSnap = enabled;
    update();
}

void PianoRollView::setSnapToScale(bool enabled) {
    m_snapToScale = enabled;
    update();
}

void PianoRollView::setSwing(double swing) {
    m_swing = std::clamp(swing, 0.5, 0.9);
    update();
}

void PianoRollView::setScale(int root, mt::Scale scale) {
    m_scaleRoot = ((root % 12) + 12) % 12;
    m_scale = scale;
    update();
}

double PianoRollView::snapBeats(double beats, bool enabled) const {
    const double grid = effectiveGridBeats();
    if (!enabled || grid <= 0.0) return std::max(0.0, beats);
    const double slot = std::round(beats / grid);
    double snapped = slot * grid;
    // Swing moves the odd slots, and the drawn grid moves with them, so a note
    // still lands exactly on the line you can see.
    if (std::abs(m_swing - 0.5) > 1e-9 && std::llround(slot) % 2 != 0) {
        snapped += (m_swing - 0.5) * grid;
    }
    return std::max(0.0, snapped);
}

int PianoRollView::snapPitch(int pitch) const {
    if (!m_snapToScale) return pitch;
    return mt::snapPitchToScale(pitch, m_scaleRoot, m_scale);
}

// ── Tools ───────────────────────────────────────────────────────────────────

void PianoRollView::setTool(Tool tool) {
    m_tool = tool;
    m_heldTool.reset();
    updateCursor(m_pointer);
    emitStatus();
    update();
}

PianoRollView::Tool PianoRollView::activeTool() const {
    return m_heldTool.value_or(m_tool);
}

// ── View options ────────────────────────────────────────────────────────────

void PianoRollView::setShowKeyboard(bool show) {
    m_showKeyboard = show;
    update();
}

void PianoRollView::setShowVelocityLane(bool show) {
    m_showVelocityLane = show;
    clampScroll();
    update();
}

void PianoRollView::setShowNoteNames(bool show) {
    m_showNoteNames = show;
    update();
}

void PianoRollView::setNoteBorders(bool show) {
    m_noteBorders = show;
    update();
}

void PianoRollView::setScaleHighlight(bool show) {
    m_scaleHighlight = show;
    update();
}

void PianoRollView::setColorMode(ColorMode mode) {
    m_colorMode = mode;
    update();
}

void PianoRollView::setGridContrast(double contrast) {
    m_gridContrast = std::clamp(contrast, 0.0, 1.0);
    update();
}

void PianoRollView::setGridColor(const QColor& color) {
    m_gridColor = color;
    update();
}

void PianoRollView::setGhostTracks(const QSet<QString>& trackIds) {
    if (m_ghostTracks == trackIds) return;
    m_ghostTracks = trackIds;
    m_ghostPaintIndexes.clear();
    m_ghostPaintScratch.clear();
    update();
}

void PianoRollView::setFollowPlayback(bool follow) {
    m_followPlayback = follow;
    update();
}

// ── Geometry ────────────────────────────────────────────────────────────────

double PianoRollView::keyboardWidth() const {
    return m_showKeyboard ? kKeyboardWidth : 0.0;
}

double PianoRollView::laneHeight() const {
    return m_showVelocityLane ? m_laneHeight : 0.0;
}

void PianoRollView::setLaneHeight(double px) {
    m_laneHeight = std::clamp(px, kMinLaneHeight, kMaxLaneHeight);
    clampScroll();
    emit viewportChanged();
    update();
}

void PianoRollView::setLaneParam(LaneParam param, const QString& laneId) {
    m_laneParam = param;
    m_laneId = laneId;
    emitStatus();
    update();
}

void PianoRollView::setNoteStyle(NoteStyle style) {
    m_noteStyle = style;
    update();
}

void PianoRollView::setShowAllKeyNames(bool show) {
    m_showAllKeyNames = show;
    update();
}

const daw::ControllerLane* PianoRollView::controllerLane() const {
    const auto* c = clip();
    if (!c || m_laneParam != LaneParam::Controller || m_laneId.isEmpty()) {
        return nullptr;
    }
    const std::string id = m_laneId.toStdString();
    for (const auto& lane : c->lanes) {
        if (lane.id == id) return &lane;
    }
    return nullptr;
}

double PianoRollView::fieldHeight() const {
    return std::max(m_rowHeight,
                    double(height()) - laneHeight() - ui::kRulerHeight);
}

double PianoRollView::laneTop() const { return double(height()) - laneHeight(); }

double PianoRollView::contentHeight() const { return kPitchCount * m_rowHeight; }

double PianoRollView::maxScrollY() const {
    return std::max(0.0, contentHeight() - fieldHeight());
}

double PianoRollView::maxScrollX() const {
    const double usable = double(width()) - keyboardWidth();
    return std::max(0.0, clipBeats() * pxPerBeat() - usable);
}

void PianoRollView::clampScroll() {
    m_scrollY = std::clamp(m_scrollY, 0.0, maxScrollY());
    m_scrollX = std::clamp(m_scrollX, 0.0, maxScrollX());
}

void PianoRollView::setScrollX(double x) {
    const double clamped = std::clamp(x, 0.0, maxScrollX());
    if (std::abs(clamped - m_scrollX) < 1.0e-6) return;
    m_scrollX = clamped;
    m_lastPlayheadPixel = -1;
    update();
}

void PianoRollView::setScrollY(double y) {
    m_scrollY = std::clamp(y, 0.0, maxScrollY());
    update();
}

bool PianoRollView::checkInteractionGesturesForTest() {
    const auto* current = clip();
    if (!current || width() < 200 || height() < 160) return false;

    const mt::Notes originalNotes = current->notes;
    const QSet<QString> originalSelection = m_selected;
    const QString originalPrimary = m_primary;
    const Tool originalTool = m_tool;
    const double originalGrid = m_gridBeats;
    const bool originalAdaptive = m_adaptiveSnap;
    const bool originalLane = m_showVelocityLane;
    const LaneParam originalLaneParam = m_laneParam;
    const QString originalLaneId = m_laneId;
    const double originalPx = m_pxPerBeat;
    const double originalRow = m_rowHeight;
    const double originalScrollX = m_scrollX;
    const double originalScrollY = m_scrollY;
    const double originalLastLength = m_lastLength;
    const auto originalPreview = m_preview;
    const QSet<QString> originalPreviewSelection = m_previewSelection;
    const bool originalPreviewWholeClip = m_previewWholeClip;
    const NoteStyle originalNoteStyle = m_noteStyle;
    const bool originalNoteBorders = m_noteBorders;
    const mt::Notes originalClipboard = clipboard();
    const std::size_t undoStart = m_controller->undoDepth();
    const double originalTransportPosition = m_controller->positionSeconds();
    const double originalLoopStart = m_controller->loopStartSeconds();
    const double originalLoopEnd = m_controller->loopEndSeconds();
    const bool originalLoopEnabled = m_controller->isLoopEnabled();

    // Both public note styles must keep their ends intact while remaining
    // visibly distinct. This tiny raster check catches a centred outline being
    // clipped at either edge and Flat accidentally becoming rounded again.
    const auto renderNoteStyle = [this](NoteStyle style) {
        QImage image(32, 16, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        m_noteStyle = style;
        m_noteBorders = false;
        QPainter painter(&image);
        paintNoteShape(painter, QRectF(2.0, 2.0, 28.0, 12.0),
                       QColor(90, 160, 225), false, false);
        painter.end();
        return image;
    };
    const QImage roundedNote = renderNoteStyle(NoteStyle::Rounded);
    const QImage flatNote = renderNoteStyle(NoteStyle::Flat);
    const bool noteStylesClean =
        qAlpha(roundedNote.pixel(2, 8)) > 240 &&
        qAlpha(roundedNote.pixel(29, 8)) > 240 &&
        qAlpha(roundedNote.pixel(2, 2)) < qAlpha(flatNote.pixel(2, 2)) &&
        qAlpha(flatNote.pixel(2, 2)) > 240 &&
        qAlpha(flatNote.pixel(1, 8)) == 0;
    const QPainterPath testKey = pianoKeyPath(QRectF(0, 0, 20, 10), 2.0);
    const bool keyboardShapeClean =
        testKey.contains(QPointF(0.25, 0.25)) &&
        !testKey.contains(QPointF(19.75, 0.25)) &&
        testKey.contains(QPointF(19.75, 5.0));
    m_noteStyle = originalNoteStyle;
    m_noteBorders = originalNoteBorders;

    const auto makeNote = [](const char* id, double start, double length) {
        daw::NoteModel note;
        note.id = id;
        note.pitch = 60;
        note.startBeats = start;
        note.lengthBeats = length;
        return note;
    };
    const auto replaceNotes = [this](mt::Notes notes, const char* label) {
        m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                                   std::move(notes), label);
        invalidateSoundingPitchIndex();
    };

    m_preview.reset();
    invalidateNotePaintIndex();
    m_previewSelection.clear();
    m_previewWholeClip = true;
    m_showVelocityLane = false;
    m_pxPerBeat = 220.0;
    m_rowHeight = 14.0;
    m_scrollX = 0.0;
    m_scrollY = std::clamp((kMaxPitch - 60) * m_rowHeight - height() * 0.45,
                           0.0, maxScrollY());
    m_selected.clear();
    m_primary.clear();

    // A local beat is an offset from this clip, never an arrangement bar
    // number. Seeking one local bar must therefore land one bar after the
    // clip's absolute start.
    const double localSeekBeat = std::min(
        clipBeats(),
        double(std::max(1, m_controller->project().timeSigNumerator)));
    const double expectedSeek =
        current->startSeconds +
        daw::beatsToSeconds(localSeekBeat, m_controller->project().tempo);
    seekToLocalBeat(localSeekBeat);
    const bool localPlayheadMapped =
        std::abs(m_controller->positionSeconds() - expectedSeek) < 1e-4;

    // Three narrow notes with gaps between them. A single coalesced move from
    // the first to the third must erase the middle one too.
    mt::Notes eraseNotes = {makeNote("erase-a", 0.5, 0.18),
                            makeNote("erase-b", 1.0, 0.18),
                            makeNote("erase-c", 1.5, 0.18)};
    replaceNotes(eraseNotes, "Prepare Eraser Check");
    m_selected = {QStringLiteral("erase-a")};
    m_primary = QStringLiteral("erase-a");
    const QPointF blankAt(beatsToX(2.5),
                          pitchToY(60) + m_rowHeight * 0.5);
    QMouseEvent blankPress(QEvent::MouseButtonPress, blankAt,
                           QPointF(mapToGlobal(blankAt.toPoint())),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(this, &blankPress);
    QMouseEvent blankRelease(QEvent::MouseButtonRelease, blankAt,
                             QPointF(mapToGlobal(blankAt.toPoint())),
                             Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &blankRelease);
    const bool blankRightClearsSelection = m_selected.isEmpty();

    const QPointF eraseFrom = noteRect(eraseNotes.front()).center();
    const QPointF eraseTo = noteRect(eraseNotes.back()).center();
    QMouseEvent rightPress(QEvent::MouseButtonPress, eraseFrom,
                           QPointF(mapToGlobal(eraseFrom.toPoint())),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(this, &rightPress);
    QMouseEvent rightMove(QEvent::MouseMove, eraseTo,
                          QPointF(mapToGlobal(eraseTo.toPoint())), Qt::NoButton,
                          Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(this, &rightMove);
    const bool eraseDeferred = clip() && clip()->notes.size() == 3 &&
                               m_pendingErase.size() == 3;
    QMouseEvent rightRelease(QEvent::MouseButtonRelease, eraseTo,
                             QPointF(mapToGlobal(eraseTo.toPoint())),
                             Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &rightRelease);
    const bool sweptAll = clip() && clip()->notes.empty();

    // A click with sub-threshold pointer jitter keeps at least the active grid
    // length even if the previously resized note was at the hard minimum.
    m_tool = Tool::Draw;
    m_gridBeats = 0.25;
    m_adaptiveSnap = false;
    m_lastLength = kMinNoteBeats;
    const QPointF drawAt(beatsToX(0.75), pitchToY(60) + m_rowHeight * 0.5);
    QMouseEvent drawPress(QEvent::MouseButtonPress, drawAt,
                          QPointF(mapToGlobal(drawAt.toPoint())), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &drawPress);
    const QPointF tinyJitter = drawAt + QPointF(2.0, 1.0);
    QMouseEvent drawMove(QEvent::MouseMove, tinyJitter,
                         QPointF(mapToGlobal(tinyJitter.toPoint())), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &drawMove);
    QMouseEvent drawRelease(QEvent::MouseButtonRelease, tinyJitter,
                            QPointF(mapToGlobal(tinyJitter.toPoint())),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &drawRelease);
    const bool brushSafe = clip() && clip()->notes.size() == 1 &&
                           clip()->notes.front().lengthBeats >= 0.25 - 1e-9;

    // The group handle is absent for one note and separated to the right when
    // the second note joins the selection.
    mt::Notes handleNotes = {makeNote("handle-a", 0.5, 0.5),
                             makeNote("handle-b", 1.5, 0.5)};
    replaceNotes(handleNotes, "Prepare Stretch Handle Check");
    m_selected = {QStringLiteral("handle-a")};
    const bool singleHidden = stretchHandleRect().isNull();
    m_selected.insert(QStringLiteral("handle-b"));
    const QRectF groupHandle = stretchHandleRect();
    const double notesRight = std::max(noteRect(handleNotes[0]).right(),
                                       noteRect(handleNotes[1]).right());
    const bool groupOffset = !groupHandle.isNull() &&
                             groupHandle.left() > notesRight;

    // Dragging a selected note edge is a group trim, not proportional stretch:
    // both lengths gain the same amount and neither start is displaced.
    const QPointF trimFrom(noteRect(handleNotes[0]).right() - 1.0,
                           noteRect(handleNotes[0]).center().y());
    const QPointF trimTo(beatsToX(1.25), trimFrom.y());
    QMouseEvent trimPress(QEvent::MouseButtonPress, trimFrom,
                          QPointF(mapToGlobal(trimFrom.toPoint())),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &trimPress);
    QMouseEvent trimMove(QEvent::MouseMove, trimTo,
                         QPointF(mapToGlobal(trimTo.toPoint())), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &trimMove);
    QMouseEvent trimRelease(QEvent::MouseButtonRelease, trimTo,
                            QPointF(mapToGlobal(trimTo.toPoint())),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &trimRelease);
    const auto* trimmedClip = clip();
    const bool groupTrim =
        trimmedClip && trimmedClip->notes.size() == 2 &&
        std::abs(trimmedClip->notes[0].startBeats - 0.5) < 1e-9 &&
        std::abs(trimmedClip->notes[1].startBeats - 1.5) < 1e-9 &&
        std::abs(trimmedClip->notes[0].lengthBeats - 0.75) < 1e-9 &&
        std::abs(trimmedClip->notes[1].lengthBeats - 0.75) < 1e-9;
    const std::string trimUndoLabel = m_controller->undoLabel();
    const bool groupTrimAtomic = trimUndoLabel == "Edit Notes";

    // The context-panel velocity is a group offset, not an absolute value.
    // A soft/loud pair must keep the same interval after the slider moves.
    mt::Notes velocityNotes = {makeNote("velocity-a", 0.5, 0.5),
                               makeNote("velocity-b", 1.5, 0.5)};
    velocityNotes[0].velocity = 40;
    velocityNotes[1].velocity = 90;
    replaceNotes(velocityNotes, "Prepare Relative Velocity Check");
    m_selected = {QStringLiteral("velocity-a"),
                  QStringLiteral("velocity-b")};
    beginSelectionVelocityEdit();
    setSelectionVelocity(77);  // average 65 + 12
    endSelectionVelocityEdit();
    const auto* velocityClip = clip();
    const bool dynamicsPreserved =
        velocityClip && velocityClip->notes.size() == 2 &&
        velocityClip->notes[0].velocity == 52 &&
        velocityClip->notes[1].velocity == 102 &&
        velocityClip->notes[1].velocity - velocityClip->notes[0].velocity == 50;
    const std::string velocityUndoLabel = m_controller->undoLabel();
    const bool velocityAtomic = velocityUndoLabel == "Change Note Velocity";

    // A note already near the ceiling must not consume the group's remaining
    // upward range. Driving the average control to 127 eventually puts both
    // the loud and quiet notes at 127.
    mt::Notes ceilingNotes = {makeNote("ceiling-a", 0.5, 0.5),
                              makeNote("ceiling-b", 1.5, 0.5)};
    ceilingNotes[0].velocity = 100;
    ceilingNotes[1].velocity = 120;
    replaceNotes(ceilingNotes, "Prepare Velocity Ceiling Check");
    m_selected = {QStringLiteral("ceiling-a"), QStringLiteral("ceiling-b")};
    beginSelectionVelocityEdit();
    setSelectionVelocity(127);
    endSelectionVelocityEdit();
    const auto* ceilingClip = clip();
    const bool velocityCeilingIndependent =
        ceilingClip && ceilingClip->notes.size() == 2 &&
        ceilingClip->notes[0].velocity == 127 &&
        ceilingClip->notes[1].velocity == 127;

    // A multi-note delete must be a single history entry. Undo/redo should
    // move the whole chord together, matching the one gesture that removed it.
    mt::Notes historyNotes = {makeNote("history-a", 0.5, 0.5),
                              makeNote("history-b", 1.0, 0.5),
                              makeNote("history-c", 1.5, 0.5)};
    replaceNotes(historyNotes, "Prepare Atomic Delete Check");
    m_selected = {QStringLiteral("history-a"), QStringLiteral("history-b")};
    deleteSelection();
    const bool deletedTogether =
        m_controller->undoLabel() == "Delete Notes" && clip() &&
        clip()->notes.size() == 1 && clip()->notes.front().id == "history-c";
    m_controller->undo();
    const bool undoneTogether = clip() && clip()->notes.size() == 3;
    m_controller->redo();
    const bool redoneTogether = clip() && clip()->notes.size() == 1;

    // Repeat selects its result. Repeating again therefore continues the line
    // instead of duplicating the original phrase on top of the first copy.
    m_controller->setLoopRangeSeconds(0.0, 0.0);
    m_controller->setLoopEnabled(false);
    mt::Notes repeatNotes = {makeNote("repeat-a", 0.5, 0.5)};
    replaceNotes(repeatNotes, "Prepare Repeat Chain Check");
    m_selected = {QStringLiteral("repeat-a")};
    duplicateSelection();
    duplicateSelection();
    const auto* repeatedClip = clip();
    const bool repeatChains = repeatedClip && repeatedClip->notes.size() == 3 &&
                              m_selected.size() == 1 &&
                              std::abs(repeatedClip->notes[2].startBeats - 1.5) <
                                  1e-9;

    // An active cycle is a time selection. Repeat copies every note portion
    // inside it and moves the cycle to the copy, so the same shortcut chains.
    mt::Notes loopNotes = {makeNote("loop-a", 0.25, 0.5),
                           makeNote("loop-outside", 8.0, 0.5)};
    replaceNotes(loopNotes, "Prepare Loop Repeat Check");
    m_selected.clear();
    m_controller->setLoopRangeSeconds(localBeatToSeconds(0.0),
                                      localBeatToSeconds(1.0));
    m_controller->setLoopEnabled(true);
    duplicateSelection();
    duplicateSelection();
    const auto* loopRepeatedClip = clip();
    const bool loopRepeatChains =
        loopRepeatedClip && loopRepeatedClip->notes.size() == 4 &&
        m_selected.size() == 1 &&
        std::any_of(loopRepeatedClip->notes.begin(),
                    loopRepeatedClip->notes.end(), [](const auto& note) {
                        return std::abs(note.startBeats - 1.25) < 1e-9 &&
                               std::abs(note.lengthBeats - 0.5) < 1e-9;
                    }) &&
        std::any_of(loopRepeatedClip->notes.begin(),
                    loopRepeatedClip->notes.end(), [](const auto& note) {
                        return std::abs(note.startBeats - 2.25) < 1e-9 &&
                               std::abs(note.lengthBeats - 0.5) < 1e-9;
                    }) &&
        std::abs(secondsToLocalBeat(m_controller->loopStartSeconds()) - 2.0) <
            1e-9 &&
        std::abs(secondsToLocalBeat(m_controller->loopEndSeconds()) - 3.0) <
            1e-9;
    m_controller->setLoopRangeSeconds(0.0, 0.0);
    m_controller->setLoopEnabled(false);

    // Exercise the actual key-event route, not merely QAction metadata. Both
    // modifier spellings are intentional: Qt/native/remote keyboards can
    // report the platform command key through either path, and both must edit
    // the focused roll. C copies, X cuts, V pastes, then B repeats the pasted
    // selection and leaves the new copy selected.
    mt::Notes shortcutNotes = {makeNote("shortcut-a", 0.5, 0.5)};
    replaceNotes(shortcutNotes, "Prepare Shortcut Routing Check");
    m_selected = {QStringLiteral("shortcut-a")};
    setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    const auto sendShortcut = [this](Qt::KeyboardModifier modifier, int key) {
        quint32 nativeScan = 0;
        quint32 nativeVirtual = 0;
#if defined(Q_OS_MACOS)
        switch (key) {
            case Qt::Key_X: nativeVirtual = 0x07; break;
            case Qt::Key_C: nativeVirtual = 0x08; break;
            case Qt::Key_V: nativeVirtual = 0x09; break;
            case Qt::Key_B: nativeVirtual = 0x0b; break;
            default: break;
        }
#elif defined(Q_OS_WIN) || defined(Q_OS_LINUX)
        switch (key) {
            case Qt::Key_X: nativeScan = 0x2d; break;
            case Qt::Key_C: nativeScan = 0x2e; break;
            case Qt::Key_V: nativeScan = 0x2f; break;
            case Qt::Key_B: nativeScan = 0x30; break;
            default: break;
        }
#if defined(Q_OS_LINUX)
        if (QApplication::platformName() == QStringLiteral("xcb"))
            nativeScan += 8;
#endif
#endif
        QKeyEvent overrideEvent(QEvent::ShortcutOverride, key, modifier,
                                nativeScan, nativeVirtual, 0, QString());
        overrideEvent.ignore();
        QApplication::sendEvent(this, &overrideEvent);
        const bool claimed = overrideEvent.isAccepted();
        QKeyEvent pressEvent(QEvent::KeyPress, key, modifier,
                             nativeScan, nativeVirtual, 0, QString());
        pressEvent.ignore();
        QApplication::sendEvent(this, &pressEvent);
        return claimed && pressEvent.isAccepted();
    };
    const bool copyDispatched =
        sendShortcut(Qt::ControlModifier, Qt::Key_C);
    const bool copyState =
        clipboard().size() == 1 && clip() && clip()->notes.size() == 1;
    const bool copiedByKey = copyState;
    const Qt::KeyboardModifier alternateCommand =
#if defined(Q_OS_MACOS)
        Qt::MetaModifier;
#else
        Qt::ControlModifier;
#endif
    const bool cutDispatched =
        sendShortcut(alternateCommand, Qt::Key_X);
    const bool cutByKey = clip() && clip()->notes.empty();
    const bool pasteDispatched =
        sendShortcut(Qt::ControlModifier, Qt::Key_V);
    const bool pastedByKey = clip() && clip()->notes.size() == 1 &&
                             m_selected.size() == 1;
    const bool repeatDispatched =
        sendShortcut(alternateCommand, Qt::Key_B);
    const bool repeatedByKey = clip() && clip()->notes.size() == 2 &&
                               m_selected.size() == 1;
    const bool shortcutsRouted = copiedByKey && cutByKey && pastedByKey &&
                                 repeatedByKey;

    // Apply must commit the exact preview. A transform with an observable run
    // count catches both regressions at once: recalculating on Apply and then
    // drawing a second "next Apply" preview.
    replaceNotes({makeNote("preview-source", 0.0, 1.0)},
                 "Prepare Preview Commit Check");
    m_selected = {QStringLiteral("preview-source")};
    m_primary = QStringLiteral("preview-source");
    int previewRuns = 0;
    previewTransform([&previewRuns](const mt::Notes& source) {
        ++previewRuns;
        mt::Notes result = source;
        if (!result.empty()) {
            result.front().startBeats = 0.375 * previewRuns;
            daw::NoteModel generated = result.front();
            generated.id.clear();
            generated.pitch += 7;
            generated.startBeats += 0.25;
            result.push_back(std::move(generated));
        }
        return result;
    });
    const mt::Notes paintedPreview = visibleNotes();
    const bool previewCommitted = commitPreview(QStringLiteral("Commit Preview Check"));
    const bool previewCommitExact =
        previewCommitted && previewRuns == 1 && !m_preview && clip() &&
        clip()->notes == paintedPreview && m_selected.size() == 2;

    // The 16 ms playhead clock must not walk the clip. Exercise exact start/end
    // boundaries, a forward seek, a backwards loop jump and an in-place edit
    // through the same cache used by refreshPlayheadFrame().
    mt::Notes intervalNotes = {makeNote("index-a", 1.0, 2.0),
                               makeNote("index-b", 2.0, 2.0),
                               makeNote("index-c", 2.0, 0.5),
                               makeNote("index-muted", 1.0, 8.0)};
    intervalNotes[1].pitch = 60;  // overlaps index-a on the same key
    intervalNotes[2].pitch = 64;
    intervalNotes[3].pitch = 67;
    intervalNotes[3].muted = true;
    replaceNotes(intervalNotes, "Prepare Playhead Index Check");

    PitchMask expected60;
    expected60.set(60);
    PitchMask expected60And64 = expected60;
    expected60And64.set(64);
    const std::size_t indexBuildsBefore = m_soundingPitchIndexRebuilds;
    const bool indexedBoundaries =
        soundingPitchesAtBeat(0.999).none() &&
        soundingPitchesAtBeat(1.0) == expected60 &&
        soundingPitchesAtBeat(2.0) == expected60And64 &&
        soundingPitchesAtBeat(2.5) == expected60 &&
        soundingPitchesAtBeat(4.0).none() &&
        // A loop/seek can move backwards by any distance; the result must not
        // depend on the direction of the previous transport frame.
        soundingPitchesAtBeat(1.5) == expected60;
    PitchMask queryChecksum;
    for (int i = 0; i < 1024; ++i)
        queryChecksum ^= soundingPitchesAtBeat(double(i % 32) * 0.125);
    const bool steadyQueriesReuseIndex =
        m_soundingPitchIndexRebuilds == indexBuildsBefore + 1;

    // Same-size setNoteStates edits keep the note vector's address and size,
    // so pointer-based cache validation alone cannot catch them. The real
    // selection-length path must explicitly invalidate and rebuild once.
    m_selected = {QStringLiteral("index-a")};
    setSelectionLength(0.25);
    const std::size_t buildsAfterEdit = m_soundingPitchIndexRebuilds;
    const bool editInvalidatesIndex =
        soundingPitchesAtBeat(1.5).none() &&
        m_soundingPitchIndexRebuilds == buildsAfterEdit + 1;

    // Deterministic complexity check instead of a timing threshold: even with
    // 65k intervals in one pitch bucket, a query may perform only logarithmic
    // binary-search comparisons. The old implementation visited all 65k and
    // allocated a QSet on every frame.
    mt::Notes perfNotes;
    perfNotes.reserve(65536 + intervalNotes.size());
    for (int i = 0; i < 65536; ++i) {
        daw::NoteModel note;
        note.pitch = 12;
        note.startBeats = 16.0 + double(i) * 0.125;
        note.lengthBeats = 0.0625;
        perfNotes.push_back(std::move(note));
    }
    perfNotes.insert(perfNotes.end(), intervalNotes.begin(), intervalNotes.end());
    SoundingPitchIndex perfIndex;
    perfIndex.rebuild(perfNotes);
    std::size_t maximumComparisons = 0;
    PitchMask perfChecksum;
    for (int i = 0; i < 1024; ++i) {
        std::size_t comparisons = 0;
        perfChecksum ^= perfIndex.pitchesAt(double(i % 64) * 0.125,
                                            &comparisons);
        maximumComparisons = std::max(maximumComparisons, comparisons);
    }
    const bool logarithmicPlayheadLookup = maximumComparisons <= 64;
    (void)queryChecksum;
    (void)perfChecksum;

    // A controller-point drag can produce hundreds of pointer samples between
    // two display frames. Feed a burst synchronously (so the event loop cannot
    // fire the timer between samples), then release at a distinct final point.
    // There must be one model write, and that write must contain the release
    // coordinates rather than the last move coordinates.
    bool controllerLaneCoalesced = false;
    std::size_t controllerLaneWrites = 0;
    const std::string coalescingLaneId = m_controller->addControllerLane(
        m_trackId.toStdString(), m_clipId.toStdString(), "Coalescing Check", 119);
    if (!coalescingLaneId.empty()) {
        const QString laneId = QString::fromStdString(coalescingLaneId);
        m_controller->setLanePoints(
            m_trackId.toStdString(), m_clipId.toStdString(), coalescingLaneId,
            {{0.5, 0.2}, {1.5, 0.8}});
        m_showVelocityLane = true;
        m_laneParam = LaneParam::Controller;
        m_laneId = laneId;
        cancelControllerLaneWrite();

        const std::size_t writesBefore = m_controllerLaneModelWrites;
        const QPointF controllerPressAt(beatsToX(0.5), laneValueToY(0.2));
        QMouseEvent controllerPress(
            QEvent::MouseButtonPress, controllerPressAt,
            QPointF(mapToGlobal(controllerPressAt.toPoint())), Qt::LeftButton,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(this, &controllerPress);

        for (int i = 0; i < 32; ++i) {
            const double beat = 0.55 + double(i) * 0.015;
            const double value = 0.25 + double(i) * 0.01;
            const QPointF at(beatsToX(beat), laneValueToY(value));
            QMouseEvent move(QEvent::MouseMove, at,
                             QPointF(mapToGlobal(at.toPoint())), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(this, &move);
        }
        const bool stormStayedQueued =
            m_controllerLaneModelWrites == writesBefore &&
            m_controllerLaneWriteTimer && m_controllerLaneWriteTimer->isActive();

        // Even grid slot: swing cannot shift it, so the assertion is independent
        // of the user's saved swing preference.
        constexpr double finalBeat = 1.0;
        constexpr double finalValue = 0.73;
        const QPointF controllerReleaseAt(beatsToX(finalBeat),
                                          laneValueToY(finalValue));
        QMouseEvent controllerRelease(
            QEvent::MouseButtonRelease, controllerReleaseAt,
            QPointF(mapToGlobal(controllerReleaseAt.toPoint())), Qt::LeftButton,
            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(this, &controllerRelease);
        controllerLaneWrites = m_controllerLaneModelWrites - writesBefore;

        bool finalPointApplied = false;
        if (const auto* lane = controllerLane()) {
            finalPointApplied = std::any_of(
                lane->points.begin(), lane->points.end(),
                [finalBeat, finalValue](const auto& point) {
                    return std::abs(point.beats - finalBeat) < 1e-9 &&
                           std::abs(point.value - finalValue) < 1e-9;
                });
        }
        controllerLaneCoalesced =
            stormStayedQueued && controllerLaneWrites == 1 && finalPointApplied &&
            m_controllerLaneWriteTimer && !m_controllerLaneWriteTimer->isActive() &&
            !m_controllerLaneWritePending && m_laneWorkingPoints.empty();
        m_controller->removeControllerLane(m_trackId.toStdString(),
                                           m_clipId.toStdString(),
                                           coalescingLaneId);
    }

    replaceNotes(originalNotes, "Restore Piano Roll Gesture Check");
    m_controller->collapseUndo(undoStart, "Piano Roll Gesture Check");
    m_selected = originalSelection;
    m_primary = originalPrimary;
    m_tool = originalTool;
    m_gridBeats = originalGrid;
    m_adaptiveSnap = originalAdaptive;
    m_showVelocityLane = originalLane;
    m_laneParam = originalLaneParam;
    m_laneId = originalLaneId;
    m_pxPerBeat = originalPx;
    m_rowHeight = originalRow;
    m_scrollX = originalScrollX;
    m_scrollY = originalScrollY;
    m_lastLength = originalLastLength;
    m_preview = originalPreview;
    invalidateNotePaintIndex();
    m_previewSelection = originalPreviewSelection;
    m_previewWholeClip = originalPreviewWholeClip;
    m_noteStyle = originalNoteStyle;
    m_noteBorders = originalNoteBorders;
    clipboard() = originalClipboard;
    m_controller->setLoopRangeSeconds(originalLoopStart, originalLoopEnd);
    m_controller->setLoopEnabled(originalLoopEnabled);
    m_controller->seekSeconds(originalTransportPosition);
    clampScroll();
    emit selectionChanged();
    emit viewportChanged();
    update();
    const bool ok = noteStylesClean && keyboardShapeClean &&
                    localPlayheadMapped &&
                    blankRightClearsSelection &&
                    eraseDeferred && sweptAll && brushSafe && singleHidden && groupOffset &&
                    groupTrim && groupTrimAtomic && dynamicsPreserved &&
                    velocityAtomic && velocityCeilingIndependent &&
                    deletedTogether && undoneTogether && redoneTogether &&
                    repeatChains && loopRepeatChains && shortcutsRouted &&
                    previewCommitExact &&
                    indexedBoundaries && steadyQueriesReuseIndex &&
                    editInvalidatesIndex && logarithmicPlayheadLookup &&
                    controllerLaneCoalesced;
    if (!ok) {
        std::fprintf(stderr,
                     "piano-roll view checks: styles=%d keyboard=%d seek=%d deselect=%d deferErase=%d erase=%d "
                     "brush=%d single=%d "
                     "offset=%d trim=%d trimUndo=%d dynamics=%d velocityUndo=%d "
                     "ceiling=%d delete=%d undo=%d redo=%d repeat=%d loop=%d keys=%d "
                     "previewCommit=%d index=%d cache=%d editIndex=%d "
                     "lookup=%d(%zu comparisons) controller=%d(%zu writes)\n",
                     int(noteStylesClean), int(keyboardShapeClean),
                     int(localPlayheadMapped),
                     int(blankRightClearsSelection),
                     int(eraseDeferred), int(sweptAll), int(brushSafe), int(singleHidden),
                     int(groupOffset), int(groupTrim), int(groupTrimAtomic),
                     int(dynamicsPreserved), int(velocityAtomic),
                     int(velocityCeilingIndependent),
                     int(deletedTogether), int(undoneTogether),
                     int(redoneTogether), int(repeatChains),
                     int(loopRepeatChains), int(shortcutsRouted),
                     int(previewCommitExact),
                     int(indexedBoundaries), int(steadyQueriesReuseIndex),
                     int(editInvalidatesIndex), int(logarithmicPlayheadLookup),
                     maximumComparisons, int(controllerLaneCoalesced),
                     controllerLaneWrites);
        std::fprintf(stderr, "undo labels: trim='%s' velocity='%s'\n",
                     trimUndoLabel.c_str(), velocityUndoLabel.c_str());
        if (!shortcutsRouted) {
            std::fprintf(stderr,
                         "piano-roll shortcut checks: copy=%d cut=%d paste=%d "
                         "repeat=%d (dispatch copy=%d cut=%d paste=%d repeat=%d)\n",
                         int(copiedByKey), int(cutByKey), int(pastedByKey),
                         int(repeatedByKey), int(copyDispatched),
                         int(cutDispatched), int(pasteDispatched),
                         int(repeatDispatched));
        }
    }
    return ok;
}

double PianoRollView::clipBeats() const {
    const auto* c = clip();
    if (!c) return 4.0;
    const double beats =
        daw::secondsToBeats(c->durationSeconds, m_controller->project().tempo);
    return beats > 0.0 ? beats : 4.0;
}

double PianoRollView::pxPerBeat() const {
    if (m_pxPerBeat > 0.0) return m_pxPerBeat;
    const double usable = double(width()) - keyboardWidth();
    if (usable <= 0.0) return kMinFitPxPerBeat;
    // Fit the clip, but never tighter than `kMinFitPxPerBeat` — past that the
    // notes are too thin to grab and the roll scrolls instead.
    return std::max(usable / clipBeats(), kMinFitPxPerBeat);
}

double PianoRollView::xToBeats(double x) const {
    return (x - keyboardWidth() + m_scrollX) / pxPerBeat();
}

double PianoRollView::beatsToX(double beats) const {
    return keyboardWidth() + beats * pxPerBeat() - m_scrollX;
}

int PianoRollView::yToPitch(double y) const {
    const int row = int(std::floor(
        (y - ui::kRulerHeight + m_scrollY) / m_rowHeight));
    return std::clamp(kMaxPitch - row, kMinPitch, kMaxPitch);
}

double PianoRollView::pitchToY(int pitch) const {
    return ui::kRulerHeight +
           double(kMaxPitch - pitch) * m_rowHeight - m_scrollY;
}

collab::SemanticPoint PianoRollView::collaborationPresenceAt(
    const QPointF& position) const {
    collab::SemanticPoint point;
    point.surface = {
        collab::SurfaceKind::PianoRoll, QStringLiteral("notes"),
        collab::safeSemanticId(m_trackId + QLatin1Char(':') + m_clipId)};
    point.normalized = collab::normalizedSurfacePoint(position, size());
    point.trackId = m_trackId;
    point.clipId = m_clipId;

    const double keys = keyboardWidth();
    if (position.y() < ui::kRulerHeight) {
        point.targetId = QStringLiteral("ruler");
        if (position.x() >= keys)
            point.beat = std::max(0.0, xToBeats(position.x()));
        return point;
    }
    if (m_showVelocityLane && position.y() >= laneTop()) {
        point.targetId = QStringLiteral("parameter_lane");
        if (position.x() >= keys)
            point.beat = std::max(0.0, xToBeats(position.x()));
        point.laneFraction = laneValueAtY(position.y());
        if (m_laneParam == LaneParam::Velocity)
            point.parameterId = QStringLiteral("note.velocity");
        else if (m_laneParam == LaneParam::Pan)
            point.parameterId = QStringLiteral("note.pan");
        else
            point.parameterId = collab::safeSemanticId(m_laneId);
        return point;
    }

    point.pitch = yToPitch(position.y());
    if (position.x() < keys) {
        point.targetId = QStringLiteral("keyboard");
    } else {
        point.targetId = QStringLiteral("note_grid");
        point.beat = std::max(0.0, xToBeats(position.x()));
    }
    return point;
}

std::optional<QPointF> PianoRollView::collaborationPositionFor(
    const collab::SemanticPoint& point) const {
    const QString track = collab::safeSemanticId(m_trackId);
    const QString clipId = collab::safeSemanticId(m_clipId);
    if (point.trackId != track || point.clipId != clipId) return std::nullopt;

    const auto fallback = collab::surfacePointFromNormalized(point, size());
    qreal x = fallback ? fallback->x() : -1.0;
    qreal y = fallback ? fallback->y() : -1.0;
    if (std::isfinite(point.beat) && point.beat >= 0.0)
        x = beatsToX(point.beat);
    if (point.pitch >= kMinPitch && point.pitch <= kMaxPitch)
        y = pitchToY(point.pitch) + m_rowHeight * 0.5;
    else if (point.targetId == QLatin1String("parameter_lane") &&
             std::isfinite(point.laneFraction) &&
             point.laneFraction >= 0.0)
        y = laneValueToY(point.laneFraction);
    if (x < 0.0 || y < 0.0) return std::nullopt;
    return QPointF(x, y);
}

bool PianoRollView::checkCollaborationPresenceForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    PianoRollView view(nullptr);
    view.m_trackId = QStringLiteral("track-1");
    view.m_clipId = QStringLiteral("clip-1");
    view.resize(720, 420);
    view.m_pxPerBeat = 84.0;
    view.m_rowHeight = 12.0;
    view.m_scrollX = 36.0;
    view.m_scrollY = 500.0;

    const QPointF source(view.beatsToX(3.25),
                         view.pitchToY(67) + view.m_rowHeight * 0.5);
    const collab::SemanticPoint semantic =
        view.collaborationPresenceAt(source);
    if (std::abs(semantic.beat - 3.25) > 1e-9 || semantic.pitch != 67 ||
        semantic.trackId != QLatin1String("track-1") ||
        semantic.clipId != QLatin1String("clip-1")) {
        return fail(QStringLiteral("piano-roll presence lost beat/pitch context"));
    }

    view.resize(1280, 700);
    view.m_pxPerBeat = 132.0;
    view.m_rowHeight = 18.0;
    view.m_scrollX = 105.0;
    view.m_scrollY = 700.0;
    const auto remapped = view.collaborationPositionFor(semantic);
    const QPointF expected(view.beatsToX(3.25),
                           view.pitchToY(67) + view.m_rowHeight * 0.5);
    if (!remapped || QLineF(*remapped, expected).length() > 1e-6)
        return fail(QStringLiteral("piano-roll presence did not follow layout"));

    collab::SemanticPoint otherClip = semantic;
    otherClip.clipId = QStringLiteral("clip-2");
    if (view.collaborationPositionFor(otherClip))
        return fail(QStringLiteral("piano-roll presence crossed clip context"));
    return true;
}

double PianoRollView::localBeatToSeconds(double beats) const {
    const auto* c = clip();
    if (!c || !m_controller) return 0.0;
    return std::max(0.0, c->startSeconds +
                             daw::beatsToSeconds(beats, m_controller->project().tempo));
}

double PianoRollView::secondsToLocalBeat(double seconds) const {
    const auto* c = clip();
    if (!c || !m_controller) return 0.0;
    return daw::secondsToBeats(seconds - c->startSeconds,
                               m_controller->project().tempo);
}

void PianoRollView::drawCycleStrip(QPainter& p) {
    const Theme& t = th();
    const double keyWidth = keyboardWidth();
    const QRectF strip(0.0, 0.0, double(width()), double(ui::kLoopStripHeight));
    p.fillRect(strip, mixColors(t.toolbarBackground, t.background, 0.35));
    p.setPen(QPen(mixColors(t.separator(), t.background, 0.35), 1.0));
    p.drawLine(QPointF(0.0, ui::kLoopStripHeight),
               QPointF(double(width()), ui::kLoopStripHeight));
    if (!clip() || !m_controller) return;

    // The controller keeps the cycle in project seconds; this ruler counts
    // beats from the clip's own start. The region is the same region — a cycle
    // set here is the cycle the arrangement plays.
    const double from = secondsToLocalBeat(m_controller->loopStartSeconds());
    const double to = secondsToLocalBeat(m_controller->loopEndSeconds());
    if (!(to > from)) return;

    const double rawLeft = beatsToX(from);
    const double rawRight = beatsToX(to);
    const double left = std::max(keyWidth, rawLeft);
    const double right = std::min(double(width()), rawRight);
    if (right <= left) return;
    const bool on = m_controller->isLoopEnabled();

    const QColor cycle = Theme::cycle();
    const QRectF bar(left + 0.5, 0.5, right - left,
                     ui::kLoopStripHeight - 1.0);
    p.setRenderHint(QPainter::Antialiasing, false);
    if (on) {
        QLinearGradient fill(bar.topLeft(), bar.bottomLeft());
        fill.setColorAt(0.0, cycle.lighter(112));
        fill.setColorAt(1.0, cycle.darker(112));
        p.setBrush(fill);
        p.setPen(QPen(cycle.darker(135), 1.0));
    } else {
        QColor idle = cycle;
        idle.setAlpha(t.dark ? 46 : 60);
        p.setBrush(idle);
        p.setPen(QPen(mixColors(cycle, t.background, 0.45), 1.0));
    }
    p.drawRect(bar);

    QColor flag = mixColors(cycle, t.background, on ? 0.34 : 0.18);
    flag.setAlpha(on ? (t.dark ? 205 : 220) : (t.dark ? 92 : 110));
    p.setPen(Qt::NoPen);
    p.setBrush(flag);
    if (rawLeft >= keyWidth && rawLeft <= width()) {
        p.drawPolygon(QPolygonF{QPointF(rawLeft + 1.0, 1.0),
                                QPointF(rawLeft + 8.0, 1.0),
                                QPointF(rawLeft + 1.0, 8.0)});
    }
    if (rawRight >= keyWidth && rawRight <= width()) {
        p.drawPolygon(QPolygonF{QPointF(rawRight - 1.0, 1.0),
                                QPointF(rawRight - 8.0, 1.0),
                                QPointF(rawRight - 1.0, 8.0)});
    }
    if (on) {
        QColor wash = cycle;
        wash.setAlpha(t.dark ? 16 : 22);
        p.fillRect(QRectF(left, ui::kRulerHeight, right - left,
                          std::max(0.0, laneTop() - ui::kRulerHeight)),
                   wash);
    }
    QColor edge = cycle;
    edge.setAlpha(on ? (t.dark ? 72 : 88) : (t.dark ? 34 : 46));
    p.setPen(QPen(edge, 1.0, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    if (rawLeft >= keyWidth && rawLeft <= width())
        p.drawLine(QPointF(rawLeft, ui::kLoopStripHeight),
                   QPointF(rawLeft, laneTop()));
    if (rawRight >= keyWidth && rawRight <= width())
        p.drawLine(QPointF(rawRight, ui::kLoopStripHeight),
                   QPointF(rawRight, laneTop()));
    p.setRenderHint(QPainter::Antialiasing, false);
}

PianoRollView::LoopGrab PianoRollView::loopGrabAt(double x) const {
    if (!m_controller || !clip()) return LoopGrab::Create;
    const double from = m_controller->loopStartSeconds();
    const double to = m_controller->loopEndSeconds();
    if (!(to > from)) return LoopGrab::Create;
    const double left = beatsToX(secondsToLocalBeat(from));
    const double right = beatsToX(secondsToLocalBeat(to));
    if (std::abs(x - left) <= ui::kLoopEdgeGrab) return LoopGrab::ResizeStart;
    if (std::abs(x - right) <= ui::kLoopEdgeGrab) return LoopGrab::ResizeEnd;
    if (x > left && x < right) return LoopGrab::Move;
    return LoopGrab::Create;
}

void PianoRollView::seekToLocalBeat(double beats) {
    const auto* c = clip();
    if (!c || !m_controller) return;
    const double localBeat = std::clamp(beats, 0.0, clipBeats());
    const double absoluteSeconds =
        c->startSeconds +
        daw::beatsToSeconds(localBeat, m_controller->project().tempo);
    m_controller->seekSeconds(std::max(0.0, absoluteSeconds));
    emit playheadMoved();
    update();
}

QRectF PianoRollView::noteRect(const daw::NoteModel& n) const {
    const double x = beatsToX(n.startBeats);
    const double w = std::max(3.0, n.lengthBeats * pxPerBeat());
    return QRectF(x, pitchToY(n.pitch) + 1.0, w, m_rowHeight - 2.0);
}

// The lane draws and edits one number per note, and which number that is comes
// from `m_laneParam`. Everything below works in a normalised 0 … 1 so the
// painting and hit-testing never branch on the parameter — only these four
// functions do.

double PianoRollView::laneValueOf(const daw::NoteModel& n) const {
    switch (m_laneParam) {
        case LaneParam::Velocity: return double(n.velocity) / 127.0;
        case LaneParam::Pan:      return (double(n.pan) + 1.0) / 2.0;
        case LaneParam::Controller: break;
    }
    return 0.0;
}

void PianoRollView::setLaneValueOf(const QString& noteId, double value) {
    value = std::clamp(value, 0.0, 1.0);
    switch (m_laneParam) {
        case LaneParam::Velocity:
            m_controller->setNoteVelocity(
                m_trackId.toStdString(), m_clipId.toStdString(),
                noteId.toStdString(), int(std::lround(value * 127.0)));
            return;
        case LaneParam::Pan:
            m_controller->setNotePan(m_trackId.toStdString(),
                                     m_clipId.toStdString(),
                                     noteId.toStdString(),
                                     float(value * 2.0 - 1.0));
            return;
        case LaneParam::Controller:
            return;
    }
}

double PianoRollView::laneValueToY(double value) const {
    // Keep both extremes fully inside the lane. Previously value zero landed
    // exactly on the widget edge, which cut the lower half off every circular
    // handle and made the lane look unfinished.
    const double top = laneTop() + kLanePadding;
    const double bottom = double(height()) - kHandleRadius - 2.0;
    const double travel = std::max(1.0, bottom - top);
    return bottom - std::clamp(value, 0.0, 1.0) * travel;
}

double PianoRollView::laneValueAtY(double y) const {
    const double top = laneTop() + kLanePadding;
    const double bottom = double(height()) - kHandleRadius - 2.0;
    const double travel = std::max(1.0, bottom - top);
    return std::clamp((bottom - y) / travel, 0.0, 1.0);
}

QPointF PianoRollView::laneHandle(const daw::NoteModel& n) const {
    return QPointF(beatsToX(n.startBeats), laneValueToY(laneValueOf(n)));
}

int PianoRollView::velocityAtY(double y) const {
    return std::clamp(int(std::lround(laneValueAtY(y) * 127.0)), 1, 127);
}

int PianoRollView::lanePointAt(const QPointF& pos) const {
    const auto* lane = controllerLane();
    if (!lane) return -1;
    const double marginBeats =
        (kHandleGrabPx + 2.0) / std::max(1.0, pxPerBeat());
    const double atBeat = xToBeats(pos.x());
    const auto first = std::lower_bound(
        lane->points.begin(), lane->points.end(), atBeat - marginBeats,
        [](const daw::AutomationPoint& point, double beat) {
            return point.beats < beat;
        });
    for (auto point = first; point != lane->points.end(); ++point) {
        if (point->beats > atBeat + marginBeats) break;
        const size_t i = std::size_t(point - lane->points.begin());
        const QPointF at(beatsToX(lane->points[i].beats),
                         laneValueToY(lane->points[i].value));
        if (QLineF(at, pos).length() <= kHandleGrabPx + 2.0) return int(i);
    }
    return -1;
}

void PianoRollView::queueControllerLanePoint(const QPointF& pos,
                                             bool snapEnabled) {
    if (m_lanePointDrag < 0 ||
        m_lanePointDrag >= int(m_laneWorkingPoints.size())) {
        return;
    }
    auto& point = m_laneWorkingPoints[std::size_t(m_lanePointDrag)];
    point.beats = std::max(0.0, snapBeats(xToBeats(pos.x()), snapEnabled));
    point.value = laneValueAtY(pos.y());

    // Moving back to the last model state before the timer fires cancels the
    // pending write completely. This also suppresses repeated samples that snap
    // to the same beat/value while the pointer is still moving.
    m_controllerLaneWritePending =
        !m_laneLastWrittenPoint || point != *m_laneLastWrittenPoint;
    if (!m_controllerLaneWritePending) {
        if (m_controllerLaneWriteTimer) m_controllerLaneWriteTimer->stop();
        return;
    }
    if (m_controllerLaneWriteTimer && !m_controllerLaneWriteTimer->isActive())
        m_controllerLaneWriteTimer->start();
}

bool PianoRollView::flushControllerLaneWrite() {
    if (m_controllerLaneWriteTimer) m_controllerLaneWriteTimer->stop();
    if (!m_controllerLaneWritePending || !m_controller ||
        m_lanePointDrag < 0 ||
        m_lanePointDrag >= int(m_laneWorkingPoints.size()) ||
        m_laneGestureTrackId.isEmpty() || m_laneGestureClipId.isEmpty() ||
        m_laneGestureLaneId.isEmpty()) {
        m_controllerLaneWritePending = false;
        return false;
    }

    m_controllerLaneWritePending = false;
    // Exactly one vector copy and one normalisation per display frame. The
    // working vector stays in gesture order so the dragged point keeps a stable
    // identity even after the model sorts points by beat.
    std::vector<daw::AutomationPoint> points = m_laneWorkingPoints;
    m_controller->setLanePoints(m_laneGestureTrackId.toStdString(),
                                m_laneGestureClipId.toStdString(),
                                m_laneGestureLaneId.toStdString(),
                                std::move(points));
    m_laneLastWrittenPoint =
        m_laneWorkingPoints[std::size_t(m_lanePointDrag)];
    ++m_controllerLaneModelWrites;
    update();
    return true;
}

void PianoRollView::cancelControllerLaneWrite() {
    if (m_controllerLaneWriteTimer) m_controllerLaneWriteTimer->stop();
    m_controllerLaneWritePending = false;
    m_laneWorkingPoints.clear();
    m_laneLastWrittenPoint.reset();
    m_laneGestureTrackId.clear();
    m_laneGestureClipId.clear();
    m_laneGestureLaneId.clear();
}

// ── Zoom ────────────────────────────────────────────────────────────────────

void PianoRollView::zoomHorizontal(double factor) {
    // Zoom about the pointer when it is over the grid, so the note under the
    // cursor stays put — the only zoom that doesn't lose your place.
    const double anchorX =
        m_pointerInside ? m_pointer.x() : keyboardWidth() + width() * 0.5;
    const double anchorBeats = xToBeats(anchorX);
    m_pxPerBeat = std::clamp(pxPerBeat() * factor, kMinPxPerBeat, kMaxPxPerBeat);
    m_scrollX = anchorBeats * pxPerBeat() - (anchorX - keyboardWidth());
    clampScroll();
    emit viewportChanged();
    update();
}

void PianoRollView::setHorizontalZoomFromStart(double pixels) {
    m_pxPerBeat = std::clamp(pixels, kMinPxPerBeat, kMaxPxPerBeat);
    // The right overview bracket represents the visible range's end; its left
    // edge is implicitly bar one. Every drag therefore returns the viewport to
    // that fixed origin instead of preserving a centre beat.
    m_scrollX = 0.0;
    clampScroll();
    emit viewportChanged();
    update();
}

double PianoRollView::effectivePixelsPerBeat() const { return pxPerBeat(); }

void PianoRollView::zoomVertical(double factor) {
    const bool pointerInField =
        m_pointerInside && m_pointer.y() >= ui::kRulerHeight &&
        m_pointer.y() < laneTop();
    const double anchorY = pointerInField
                               ? m_pointer.y()
                               : ui::kRulerHeight + fieldHeight() * 0.5;
    const double fieldY = anchorY - ui::kRulerHeight;
    const double anchorRow = (fieldY + m_scrollY) / m_rowHeight;
    m_rowHeight = std::clamp(m_rowHeight * factor, kMinRowHeight, kMaxRowHeight);
    m_scrollY = anchorRow * m_rowHeight - fieldY;
    clampScroll();
    emit viewportChanged();
    update();
}

void PianoRollView::setPixelsPerBeat(double px) {
    // Zero is meaningful: it hands the width back to "fit the clip".
    m_pxPerBeat = px <= 0.0 ? 0.0 : std::clamp(px, kMinPxPerBeat, kMaxPxPerBeat);
    clampScroll();
    emit viewportChanged();
    update();
}

void PianoRollView::setRowHeight(double px) {
    m_rowHeight = std::clamp(px, kMinRowHeight, kMaxRowHeight);
    clampScroll();
    emit viewportChanged();
    update();
}

void PianoRollView::zoomToFit() {
    // Asked for by name, so it really fits: the whole clip however long it is.
    // The floor in `pxPerBeat()` is for the width the roll picks on its own,
    // and applying it here would mean "fit" showed only part of the clip.
    const double usable = double(width()) - keyboardWidth();
    m_pxPerBeat =
        usable > 0.0
            ? std::clamp(usable / clipBeats(), kMinPxPerBeat, kMaxPxPerBeat)
            : 0.0;
    m_scrollX = 0.0;
    scrollToContent();
    emit viewportChanged();
}

void PianoRollView::zoomToSelection() {
    mt::Notes selection = targetNotes();
    if (selection.empty()) return;
    double start = 0.0, end = 0.0;
    mt::spanOf(selection, &start, &end);
    const double span = std::max(end - start, kMinNoteBeats);
    const double usable = std::max(1.0, double(width()) - keyboardWidth());
    m_pxPerBeat = std::clamp(usable / span * 0.9, kMinPxPerBeat, kMaxPxPerBeat);
    m_scrollX = std::max(0.0, start * pxPerBeat() - usable * 0.05);

    int low = selection.front().pitch, high = selection.front().pitch;
    for (const auto& n : selection) {
        low = std::min(low, n.pitch);
        high = std::max(high, n.pitch);
    }
    const double centre =
        (double(kMaxPitch - low) + double(kMaxPitch - high)) / 2.0 * m_rowHeight;
    m_scrollY = centre - fieldHeight() / 2.0;
    clampScroll();
    emit viewportChanged();
    update();
}

void PianoRollView::scrollToContent() {
    const auto* c = clip();
    int lowest = 60;   // middle C when there is nothing to look at yet
    int highest = 60;
    if (c && !c->notes.empty()) {
        lowest = highest = c->notes.front().pitch;
        for (const auto& n : c->notes) {
            lowest = std::min(lowest, n.pitch);
            highest = std::max(highest, n.pitch);
        }
    }
    const double centre =
        (double(kMaxPitch - lowest) + double(kMaxPitch - highest)) / 2.0 *
        m_rowHeight;
    m_scrollY = centre - fieldHeight() / 2.0;
    clampScroll();
    emit viewportChanged();
    update();
}

void PianoRollView::resizeEvent(QResizeEvent* ev) {
    clampScroll();
    emit viewportChanged();
    QWidget::resizeEvent(ev);
}

void PianoRollView::hideEvent(QHideEvent* ev) {
    // A close normally hides this modeless editor rather than destroying it.
    // Do not leave a wheel burst's controller transaction open while hidden.
    finishWheelNoteEdit();
    QWidget::hideEvent(ev);
}

// ── Selection ───────────────────────────────────────────────────────────────

void PianoRollView::selectOnly(const QString& noteId) {
    m_selected.clear();
    if (!noteId.isEmpty()) m_selected.insert(noteId);
    m_primary = noteId;
    emit selectionChanged();
}

void PianoRollView::toggleSelected(const QString& noteId) {
    if (noteId.isEmpty()) return;
    if (m_selected.contains(noteId)) {
        m_selected.remove(noteId);
        if (m_primary == noteId) m_primary.clear();
    } else {
        m_selected.insert(noteId);
        m_primary = noteId;
    }
    emit selectionChanged();
}

void PianoRollView::selectAll() {
    const auto* c = clip();
    if (!c) return;
    m_selected.clear();
    for (const auto& n : c->notes) m_selected.insert(QString::fromStdString(n.id));
    emit selectionChanged();
    emitStatus();
    update();
}

void PianoRollView::selectNone() {
    m_selected.clear();
    m_primary.clear();
    emit selectionChanged();
    emitStatus();
    update();
}

void PianoRollView::invertSelection() {
    const auto* c = clip();
    if (!c) return;
    QSet<QString> inverted;
    for (const auto& n : c->notes) {
        const QString id = QString::fromStdString(n.id);
        if (!m_selected.contains(id)) inverted.insert(id);
    }
    m_selected = inverted;
    m_primary.clear();
    emit selectionChanged();
    emitStatus();
    update();
}

void PianoRollView::selectSameColor() {
    const auto* c = clip();
    if (!c || m_selected.isEmpty()) return;
    QSet<uint32_t> wanted;
    for (const auto& n : c->notes) {
        if (m_selected.contains(QString::fromStdString(n.id))) wanted.insert(n.color);
    }
    for (const auto& n : c->notes) {
        if (wanted.contains(n.color)) m_selected.insert(QString::fromStdString(n.id));
    }
    emit selectionChanged();
    emitStatus();
    update();
}

void PianoRollView::bumpSelectedVelocity(int delta) {
    if (delta == 0 || m_selected.isEmpty() || !clip()) return;
    m_noteUpdateScratch.clear();
    m_noteUpdateScratch.reserve(std::size_t(m_selected.size()));
    for (const QString& id : m_selected) {
        const auto* current = note(id);
        if (!current) continue;
        daw::NoteModel next = *current;
        next.velocity = std::clamp(next.velocity + delta, 1, 127);
        m_noteUpdateScratch.push_back(std::move(next));
    }
    if (m_noteUpdateScratch.empty()) return;
    beginWheelNoteEdit("Change Note Velocity");
    m_controller->setNoteStates(m_trackId.toStdString(),
                                m_clipId.toStdString(), m_noteUpdateScratch);
    update();
}

void PianoRollView::bumpSelectedPan(int steps) {
    if (steps == 0 || m_selected.isEmpty() || !clip()) return;
    m_noteUpdateScratch.clear();
    m_noteUpdateScratch.reserve(std::size_t(m_selected.size()));
    for (const QString& id : m_selected) {
        const auto* current = note(id);
        if (!current) continue;
        daw::NoteModel next = *current;
        const double value = std::clamp(
            laneValueOf(*current) + steps / 64.0, 0.0, 1.0);
        next.pan = float(value * 2.0 - 1.0);
        m_noteUpdateScratch.push_back(std::move(next));
    }
    if (m_noteUpdateScratch.empty()) return;
    beginWheelNoteEdit("Change Note Pan");
    m_controller->setNoteStates(m_trackId.toStdString(),
                                m_clipId.toStdString(), m_noteUpdateScratch);
    update();
}

void PianoRollView::beginWheelNoteEdit(const std::string& label) {
    if (m_wheelEditUndoActive && m_wheelEditLabel != label)
        finishWheelNoteEdit();
    if (!m_wheelEditUndoActive) {
        m_controller->beginNoteEdit(m_trackId.toStdString(),
                                    m_clipId.toStdString());
        m_wheelEditUndoActive = true;
        m_wheelEditLabel = label;
    }
    if (m_wheelEditTimer) m_wheelEditTimer->start();
}

void PianoRollView::finishWheelNoteEdit() {
    if (m_wheelEditTimer) m_wheelEditTimer->stop();
    if (!m_wheelEditUndoActive) return;
    const std::string label = std::move(m_wheelEditLabel);
    m_wheelEditLabel.clear();
    m_wheelEditUndoActive = false;
    m_controller->endNoteEdit(label);
    emit edited();
}

// ── Commands ────────────────────────────────────────────────────────────────

mt::Notes PianoRollView::targetNotes() const {
    const auto* c = clip();
    if (!c) return {};
    if (m_selected.isEmpty()) return c->notes;
    mt::Notes selection;
    for (const auto& n : c->notes) {
        if (m_selected.contains(QString::fromStdString(n.id))) selection.push_back(n);
    }
    return selection;
}

mt::Notes PianoRollView::clipNotes() const {
    const auto* c = clip();
    return c ? c->notes : mt::Notes{};
}

double PianoRollView::rotateSpanBeats() const {    // With nothing selected the command runs on the whole clip, so the clip is
    // the cycle and there is nothing to guess.
    if (m_selected.isEmpty()) return clipBeats();

    const mt::Notes notes = targetNotes();
    if (notes.empty()) return clipBeats();
    double start = 0.0, end = 0.0;
    mt::spanOf(notes, &start, &end);

    // Round the selection's span up to a whole bar. A one-bar drum pattern whose
    // last note is a sixteenth spans 3.75 beats, and rotating inside 3.75 would
    // walk the whole pattern off the grid on the first press — the bar is what
    // the user means by "the phrase", not wherever the last note happens to end.
    const double bar = std::max(1, m_controller->project().timeSigNumerator);
    const double bars = std::max(1.0, std::ceil((end - start) / bar - 1e-9));
    return bars * bar;
}

void PianoRollView::applyTransform(
    const std::function<mt::Notes(const mt::Notes&)>& transform,
    const QString& label) {
    const auto* c = clip();
    if (!c || !transform) return;
    clearPreview();

    // Everything the command does *not* act on is carried through untouched.
    mt::Notes untouched;
    mt::Notes target;
    const bool wholeClip = m_selected.isEmpty();
    for (const auto& n : c->notes) {
        if (wholeClip || m_selected.contains(QString::fromStdString(n.id))) {
            target.push_back(n);
        } else {
            untouched.push_back(n);
        }
    }
    if (target.empty()) return;

    mt::Notes result = transform(target);
    // Mint the uuids here rather than leaving it to `setClipNotes`: the ids are
    // what the selection is made of, and the view has to keep hold of them.
    QSet<QString> keep;
    for (auto& n : result) {
        if (n.id.empty()) n.id = daw::newUuid();
        keep.insert(QString::fromStdString(n.id));
    }

    mt::Notes merged = std::move(untouched);
    merged.insert(merged.end(), result.begin(), result.end());
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               merged, label.toStdString());
    invalidateSoundingPitchIndex();

    if (!wholeClip) {
        m_selected = keep;
        if (!m_selected.contains(m_primary)) m_primary.clear();
    }
    emit selectionChanged();
    emit edited();
    emitStatus();
    update();
}

void PianoRollView::previewTransform(
    const std::function<mt::Notes(const mt::Notes&)>& transform) {
    const auto* c = clip();
    if (!c || !transform) return;
    mt::Notes untouched;
    mt::Notes target;
    const bool wholeClip = m_selected.isEmpty();
    for (const auto& n : c->notes) {
        if (wholeClip || m_selected.contains(QString::fromStdString(n.id))) {
            target.push_back(n);
        } else {
            untouched.push_back(n);
        }
    }
    mt::Notes result = transform(target);
    QSet<QString> resultIds;
    for (auto& note : result) {
        if (note.id.empty()) note.id = daw::newUuid();
        resultIds.insert(QString::fromStdString(note.id));
    }
    untouched.insert(untouched.end(), result.begin(), result.end());
    m_preview = std::move(untouched);
    invalidateNotePaintIndex();
    m_previewSelection = std::move(resultIds);
    m_previewWholeClip = wholeClip;
    emitStatus();
    update();
}

bool PianoRollView::commitPreview(const QString& label) {
    if (!m_preview || !clip()) return false;

    // Move the exact painted notes into the document before the event loop can
    // paint again. There is no second transform (and therefore no second RNG
    // pass) between what the user saw and what lands in the clip.
    mt::Notes committed = std::move(*m_preview);
    m_preview.reset();
    invalidateNotePaintIndex();
    const QSet<QString> committedSelection = std::move(m_previewSelection);
    m_previewSelection.clear();
    const bool wholeClip = m_previewWholeClip;
    m_previewWholeClip = true;

    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               std::move(committed), label.toStdString());
    invalidateSoundingPitchIndex();
    if (!wholeClip) {
        m_selected = committedSelection;
        if (!m_selected.contains(m_primary)) m_primary.clear();
    }
    emit selectionChanged();
    emit edited();
    emitStatus();
    update();
    return true;
}

void PianoRollView::clearPreview() {
    if (!m_preview) return;
    m_preview.reset();
    invalidateNotePaintIndex();
    m_previewSelection.clear();
    m_previewWholeClip = true;
    emitStatus();
    update();
}

// ── Edits the context panel drives ─────────────────────────────────────────

void PianoRollView::beginSelectionEdit() {
    if (m_selectionEditUndoActive) return;
    finishWheelNoteEdit();
    m_controller->beginNoteEdit(m_trackId.toStdString(), m_clipId.toStdString());
    m_selectionEditUndoActive = true;
    m_selectionEditWorking.clear();
    if (const auto* current = clip()) {
        if (m_selected.isEmpty()) {
            m_selectionEditWorking = current->notes;
        } else {
            m_selectionEditWorking.reserve(std::size_t(m_selected.size()));
            for (const QString& id : m_selected) {
                if (const auto* selected = note(id))
                    m_selectionEditWorking.push_back(*selected);
            }
        }
    }
}

void PianoRollView::endSelectionEdit(const QString& label) {
    if (!m_selectionEditUndoActive) return;
    m_controller->endNoteEdit(label.toStdString());
    m_selectionEditUndoActive = false;
    m_selectionEditWorking.clear();
    if (m_soundingPitchInvalidationDeferred)
        invalidateSoundingPitchIndex();
}

void PianoRollView::beginSelectionVelocityEdit() {
    beginSelectionEdit();
    m_velocityEditOriginal.clear();
    if (m_selectionEditWorking.empty()) {
        m_velocityEditActive = false;
        endSelectionEdit(QStringLiteral("Change Note Velocity"));
        return;
    }
    double total = 0.0;
    for (const auto& n : m_selectionEditWorking) {
        m_velocityEditOriginal.insert(QString::fromStdString(n.id), n.velocity);
        total += n.velocity;
    }
    m_velocityEditAnchor =
        int(std::lround(total / double(m_velocityEditOriginal.size())));
    m_velocityEditActive = true;
}

void PianoRollView::setSelectionVelocity(int velocity) {
    if (m_velocityEditActive && !m_velocityEditOriginal.isEmpty()) {
        // The control asks for the group's *average*, not a raw offset. Find
        // the offset whose clamped notes best reach that average. Once a loud
        // note hits 127 it stays there while quieter notes keep rising; at 127
        // every selected note is guaranteed to be at the ceiling.
        const int target = std::clamp(velocity, 1, 127);
        const bool raising = target >= m_velocityEditAnchor;
        int bestDelta = 0;
        double bestError = std::numeric_limits<double>::max();
        const int firstDelta = raising ? 0 : -126;
        const int lastDelta = raising ? 126 : 0;
        std::array<int, 128> velocityCounts{};
        for (auto it = m_velocityEditOriginal.constBegin();
             it != m_velocityEditOriginal.constEnd(); ++it) {
            ++velocityCounts[std::size_t(std::clamp(it.value(), 1, 127))];
        }
        for (int candidate = firstDelta; candidate <= lastDelta; ++candidate) {
            double sum = 0.0;
            // There are only 127 possible source velocities. A histogram keeps
            // the exact clamping behaviour while avoiding 127 full passes over a
            // large selection for every slider tick.
            for (int source = 1; source <= 127; ++source) {
                sum += double(velocityCounts[std::size_t(source)]) *
                       std::clamp(source + candidate, 1, 127);
            }
            const double average =
                sum / double(m_velocityEditOriginal.size());
            const double error = std::abs(average - target);
            const bool fartherInDirection =
                raising ? candidate > bestDelta : candidate < bestDelta;
            if (error < bestError - 1e-9 ||
                (std::abs(error - bestError) <= 1e-9 && fartherInDirection)) {
                bestError = error;
                bestDelta = candidate;
            }
        }
        for (auto& current : m_selectionEditWorking) {
            const auto original = m_velocityEditOriginal.constFind(
                QString::fromStdString(current.id));
            if (original != m_velocityEditOriginal.constEnd())
                current.velocity = original.value() + bestDelta;
        }
        m_controller->setNoteStates(m_trackId.toStdString(),
                                    m_clipId.toStdString(),
                                    m_selectionEditWorking);
        update();
        return;
    }
    // Retain an absolute setter for direct/programmatic callers. Interactive
    // context-panel changes always bracket this with begin/end above.
    mt::Notes updates = targetNotes();
    for (auto& note : updates) note.velocity = velocity;
    m_controller->setNoteStates(m_trackId.toStdString(),
                                m_clipId.toStdString(), updates);
    update();
}

void PianoRollView::endSelectionVelocityEdit() {
    m_velocityEditOriginal.clear();
    m_velocityEditActive = false;
    endSelectionEdit(QStringLiteral("Change Note Velocity"));
}

void PianoRollView::setSelectionPan(float pan) {
    if (m_selectionEditUndoActive) {
        for (auto& note : m_selectionEditWorking) note.pan = pan;
        m_controller->setNoteStates(m_trackId.toStdString(),
                                    m_clipId.toStdString(),
                                    m_selectionEditWorking);
        update();
        return;
    }
    mt::Notes updates = targetNotes();
    for (auto& note : updates) note.pan = pan;
    m_controller->setNoteStates(m_trackId.toStdString(),
                                m_clipId.toStdString(), updates);
    update();
}

void PianoRollView::setSelectionLength(double beats) {
    if (m_selectionEditUndoActive) {
        for (auto& note : m_selectionEditWorking)
            note.lengthBeats = beats;
        m_controller->setNoteStates(m_trackId.toStdString(),
                                    m_clipId.toStdString(),
                                    m_selectionEditWorking);
        invalidateSoundingPitchIndex();
        m_lastLength = beats;
        update();
        return;
    }
    mt::Notes updates = targetNotes();
    for (auto& note : updates) note.lengthBeats = beats;
    m_controller->setNoteStates(m_trackId.toStdString(),
                                m_clipId.toStdString(), updates);
    invalidateSoundingPitchIndex();
    m_lastLength = beats;
    update();
}

void PianoRollView::setSelectionColor(uint32_t rgb) {
    applyTransform([rgb](const mt::Notes& n) { return mt::setColor(n, rgb); },
                   tr("Note Colour"));
}

void PianoRollView::setSelectionMuted(bool muted) {
    applyTransform([muted](const mt::Notes& n) { return mt::setMuted(n, muted); },
                   muted ? tr("Mute Notes") : tr("Unmute Notes"));
}

void PianoRollView::transposeSelection(int semitones) {
    if (semitones == 0) return;
    applyTransform(
        [semitones](const mt::Notes& n) { return mt::transpose(n, semitones); },
        tr("Transpose"));
}

PianoRollView::SelectionSummary PianoRollView::selectionSummary() const {
    SelectionSummary summary;
    const auto* currentClip = clip();
    if (!currentClip) return summary;

    double velocity = 0.0, pan = 0.0, length = 0.0, pitch = 0.0;
    bool allMuted = true;
    const bool wholeClip = m_selected.isEmpty();
    const auto accumulate = [&](const daw::NoteModel& n) {
        if (summary.count == 0) summary.color = n.color;
        velocity += n.velocity;
        pan += n.pan;
        length += n.lengthBeats;
        pitch += n.pitch;
        allMuted = allMuted && n.muted;
        // A mixed selection has no one colour, so it reports "inherit".
        if (n.color != summary.color) summary.color = 0;
        ++summary.count;
    };
    if (wholeClip) {
        for (const auto& n : currentClip->notes) accumulate(n);
    } else {
        for (const QString& id : m_selected) {
            if (const auto* selected = note(id)) accumulate(*selected);
        }
    }
    if (summary.count == 0) return SelectionSummary{};
    const double count = double(summary.count);
    summary.velocity = int(std::lround(velocity / count));
    summary.pan = float(pan / count);
    summary.lengthBeats = length / count;
    summary.pitch = int(std::lround(pitch / count));
    summary.muted = allMuted;
    return summary;
}

QString PianoRollView::selectionKey() const {
    if (m_selected.isEmpty()) return QStringLiteral("all:") + m_clipId;
    // Sorted, so the same set of notes always produces the same string however
    // the hash happened to order it.
    QList<QString> ids = m_selected.values();
    std::sort(ids.begin(), ids.end());
    return ids.join('/');
}

void PianoRollView::deleteSelection() {
    if (m_selected.isEmpty() || !clip()) return;
    // One gesture is one history entry. Removing each note separately made a
    // chord come back one key at a time on Undo and disappear one key at a time
    // on Redo. Replacing the vector records the whole selection atomically.
    const QSet<QString> doomed = m_selected;
    mt::Notes remaining;
    remaining.reserve(clip()->notes.size());
    for (const auto& note : clip()->notes) {
        if (!doomed.contains(QString::fromStdString(note.id))) {
            remaining.push_back(note);
        }
    }
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               std::move(remaining), "Delete Notes");
    invalidateSoundingPitchIndex();
    m_selected.clear();
    m_primary.clear();
    emit selectionChanged();
    emit edited();
    emitStatus();
    update();
}

void PianoRollView::copySelection() {
    mt::Notes selection = targetNotes();
    if (selection.empty()) return;
    // Stored relative to the earliest note, so a paste can land anywhere.
    double start = 0.0, end = 0.0;
    mt::spanOf(selection, &start, &end);
    for (auto& n : selection) n.startBeats -= start;
    clipboard() = std::move(selection);
    emitStatus();
}

void PianoRollView::cutSelection() {
    copySelection();
    deleteSelection();
}

bool PianoRollView::canPaste() const { return !clipboard().empty(); }

void PianoRollView::paste() {
    const auto* c = clip();
    if (!c || clipboard().empty()) return;
    // Land it where the pointer is, or at the start of the clip when it is not
    // over the grid at all (a paste driven from the menu bar, say).
    const double at =
        m_pointerInside && m_pointer.x() >= keyboardWidth()
            ? snapBeats(xToBeats(m_pointer.x()), m_snapEnabled)
            : 0.0;

    mt::Notes merged = c->notes;
    QSet<QString> pasted;
    for (const auto& source : clipboard()) {
        daw::NoteModel n = source;
        n.id = daw::newUuid();
        n.startBeats = at + source.startBeats;
        pasted.insert(QString::fromStdString(n.id));
        merged.push_back(n);
    }
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               merged, "Paste Notes");
    invalidateSoundingPitchIndex();
    m_selected = pasted;
    m_primary.clear();
    emit selectionChanged();
    emit edited();
    emitStatus();
    update();
}

void PianoRollView::duplicateSelection() {
    const auto* c = clip();
    if (!c) return;
    const double loopFromSeconds = m_controller->loopStartSeconds();
    const double loopToSeconds = m_controller->loopEndSeconds();
    if (m_controller->isLoopEnabled() &&
        loopToSeconds > loopFromSeconds) {
        const double loopFrom = secondsToLocalBeat(loopFromSeconds);
        const double loopTo = secondsToLocalBeat(loopToSeconds);
        const double length = loopTo - loopFrom;
        if (length <= 0.0) return;

        mt::Notes merged = c->notes;
        QSet<QString> copies;
        for (const auto& source : c->notes) {
            const double sourceEnd = source.startBeats + source.lengthBeats;
            const double insideFrom = std::max(source.startBeats, loopFrom);
            const double insideTo = std::min(sourceEnd, loopTo);
            if (insideTo <= insideFrom) continue;
            daw::NoteModel note = source;
            note.id = daw::newUuid();
            note.startBeats = insideFrom + length;
            note.lengthBeats = insideTo - insideFrom;
            copies.insert(QString::fromStdString(note.id));
            merged.push_back(std::move(note));
        }
        if (copies.isEmpty()) return;

        m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                                   merged, "Repeat Loop Notes");
        invalidateSoundingPitchIndex();
        m_selected = copies;
        m_primary.clear();
        m_controller->setLoopRangeSeconds(loopToSeconds,
                                          loopToSeconds +
                                              (loopToSeconds - loopFromSeconds));
        m_controller->setLoopEnabled(true);
        emit selectionChanged();
        emit loopRangeChanged();
        emit edited();
        update();
        return;
    }

    mt::Notes selection = targetNotes();
    if (selection.empty()) return;
    double start = 0.0, end = 0.0;
    mt::spanOf(selection, &start, &end);
    // A duplicate lands immediately after the phrase it came from, which is how
    // a two-bar idea becomes four bars in one keystroke.
    const double offset = std::max(end - start, effectiveGridBeats());

    mt::Notes merged = c->notes;
    QSet<QString> copies;
    for (const auto& source : selection) {
        daw::NoteModel n = source;
        n.id = daw::newUuid();
        n.startBeats = source.startBeats + offset;
        copies.insert(QString::fromStdString(n.id));
        merged.push_back(n);
    }
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               merged, "Duplicate Notes");
    invalidateSoundingPitchIndex();
    m_selected = copies;
    emit selectionChanged();
    emit edited();
    update();
}

// ── Colour ──────────────────────────────────────────────────────────────────

QColor PianoRollView::colorFor(const daw::NoteModel& n,
                               const QColor& clipColor) const {
    switch (m_colorMode) {
        case ColorMode::Clip:
            return clipColor;
        case ColorMode::Velocity: {
            // Cool and dim for soft, hot for loud: velocity reads off the grid
            // without opening the lane.
            const double t = std::clamp(double(n.velocity) / 127.0, 0.0, 1.0);
            return QColor::fromHsvF(0.62 - 0.62 * t, 0.75, 0.55 + 0.45 * t);
        }
        case ColorMode::Pitch: {
            const double t = double(((n.pitch % 12) + 12) % 12) / 12.0;
            return QColor::fromHsvF(t, 0.65, 0.95);
        }
        case ColorMode::Custom:
            return n.color ? ui::colorFromRgb(n.color) : clipColor;
    }
    return clipColor;
}

// ── Painting ────────────────────────────────────────────────────────────────

void PianoRollView::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    const Theme& t = th();
    const QRectF dirtyRect(event->region().boundingRect());
    p.fillRect(rect(), t.background);

    const auto* c = clip();
    if (!c) {
        // The clip was deleted (or its track was) while the window was open.
        // Showing an empty state rather than closing means an undo that brings
        // it back makes the window live again, with no extra bookkeeping.
        p.setPen(t.textSecondary);
        p.drawText(rect(), Qt::AlignCenter,
                   tr("The clip this editor was opened on no longer exists."));
        return;
    }
    // The document cannot change while this synchronous paint is on the stack.
    // Reuse the one resolved pointer in every geometry/helper call, then drop it
    // before returning so undo or a later edit can never leave a cached pointer.
    m_paintClip = c;

    const double keyWidth = keyboardWidth();
    const double gridTop = ui::kRulerHeight;
    const double fieldBottom = laneTop();
    const QRectF field(keyWidth, gridTop, double(width()) - keyWidth,
                       std::max(0.0, fieldBottom - gridTop));
    const QColor gridBase = m_gridColor.isValid() ? m_gridColor : t.gridLine;
    const QColor gridStrong =
        m_gridColor.isValid() ? m_gridColor.lighter(140) : t.gridLineStrong;
    const QColor clipColor = ui::colorFromRgb(c->color);

    p.save();
    p.setClipRect(QRectF(0, 0, double(width()), fieldBottom));
    p.fillRect(field, t.well());

    // The ruler is clip-local: bar 1 is always the clip's own beginning, even
    // when that beginning is bar 37 on the arrangement. Seeking converts the
    // local beat back to project seconds in `seekToLocalBeat()`.
    QLinearGradient rulerFill(0.0, 0.0, 0.0, gridTop);
    rulerFill.setColorAt(
        0.0, mixColors(t.surfaceElevated, t.toolbarBackground, 0.30));
    rulerFill.setColorAt(
        1.0, mixColors(t.surface, t.toolbarBackground, 0.45));
    p.fillRect(QRectF(0.0, 0.0, double(width()), gridTop), rulerFill);
    drawCycleStrip(p);
    p.setPen(QPen(t.sectionDivider(), 1.0));
    p.drawLine(QPointF(0.0, gridTop - 1.0),
               QPointF(double(width()), gridTop - 1.0));

    // Row banding, so a pitch can be read off the grid at a glance. The *white*
    // key rows are lightened rather than the black ones darkened: on a dark
    // theme the field is already near-black and there is no darker left to go.
    const QColor whiteRow = mixColors(t.well(), t.textPrimary, 0.09);
    const QColor scaleRow = mixColors(t.well(), t.accent, 0.16);
    for (int pitch = kMinPitch; pitch <= kMaxPitch; ++pitch) {
        const double y = pitchToY(pitch);
        if (y + m_rowHeight < gridTop || y > fieldBottom) continue;
        // Scale highlighting wins over the black/white banding: when it is on,
        // what matters is which notes are in the key, not which are black.
        const bool degree =
            m_scaleHighlight && mt::inScale(pitch, m_scaleRoot, m_scale);
        if (m_scaleHighlight) {
            if (!degree) continue;
            p.fillRect(QRectF(keyWidth, y, field.width(), m_rowHeight), scaleRow);
        } else if (!isBlackKey(pitch)) {
            p.fillRect(QRectF(keyWidth, y, field.width(), m_rowHeight), whiteRow);
        }
    }
    // A stronger line under every B → C boundary, so octaves are countable.
    p.setPen(QPen(gridBase, 1.0));
    for (int pitch = kMinPitch; pitch <= kMaxPitch; pitch += 12) {
        const double y = pitchToY(pitch) + m_rowHeight;
        if (y < gridTop || y > fieldBottom) continue;
        p.drawLine(QPointF(keyWidth, y), QPointF(field.right(), y));
    }

    // Vertical grid: subdivisions, then beats and bars on top. Contrast is a
    // user setting because a busy part wants a fainter grid than a sparse one.
    const int beatsPerBar = std::max(1, m_controller->project().timeSigNumerator);
    const double totalBeats = clipBeats();
    const double px = pxPerBeat();
    const double grid = effectiveGridBeats();
    const double faint = 0.25 + 0.5 * m_gridContrast;

    // Work from the actual disjoint update region rather than its bounding box.
    // A normal playback tick produces two tiny strips (old and new playhead); a
    // seek can put them far apart. Starting at beat zero -- or treating those two
    // strips as one wide rectangle -- made a long clip expensive at 60 Hz even
    // though Qt was going to accept only a handful of painted pixels.
    std::vector<std::pair<double, double>> dirtyBeatRanges;
    dirtyBeatRanges.reserve(std::size_t(event->region().rectCount()));
    constexpr double kGridPaintMarginPx = 2.0;
    for (const QRect& updateRect : event->region()) {
        if (updateRect.top() > fieldBottom) continue;
        const double leftPx = std::max(
            keyWidth, double(updateRect.left()) - kGridPaintMarginPx);
        const double rightPx = std::min(
            double(width()), double(updateRect.right()) + kGridPaintMarginPx);
        if (rightPx < leftPx) continue;
        const double rawFirst = xToBeats(leftPx);
        const double rawLast = xToBeats(rightPx);
        if (rawLast < 0.0) continue;
        // Keep the whole visible time interval here, including the blank area
        // beyond this clip. Notes and ghosts may have a tail there; the grid
        // loops below clamp their own indices to the clip duration.
        dirtyBeatRanges.emplace_back(std::max(0.0, rawFirst), rawLast);
    }
    std::sort(dirtyBeatRanges.begin(), dirtyBeatRanges.end());
    std::size_t mergedRangeCount = 0;
    for (const auto& range : dirtyBeatRanges) {
        if (mergedRangeCount > 0 &&
            range.first <= dirtyBeatRanges[mergedRangeCount - 1].second + 1e-9) {
            dirtyBeatRanges[mergedRangeCount - 1].second = std::max(
                dirtyBeatRanges[mergedRangeCount - 1].second, range.second);
        } else {
            dirtyBeatRanges[mergedRangeCount++] = range;
        }
    }
    dirtyBeatRanges.resize(mergedRangeCount);

    if (grid > 0.0 && grid * px >= 4.0) {
        p.setPen(QPen(mixColors(gridBase, t.background, 1.0 - faint), 1.0));
        const std::int64_t finalSlot = std::int64_t(
            std::floor((totalBeats + 1e-9) / grid));
        for (const auto& range : dirtyBeatRanges) {
            // One slot of padding catches an odd swung line whose unswung beat
            // lies just outside the dirty interval.
            const std::int64_t firstSlot = std::max<std::int64_t>(
                0, std::int64_t(std::floor(range.first / grid)) - 1);
            const std::int64_t lastSlot = std::min<std::int64_t>(
                finalSlot, std::int64_t(std::ceil(range.second / grid)) + 1);
            for (std::int64_t slot = firstSlot; slot <= lastSlot; ++slot) {
                double at = double(slot) * grid;
                if (std::abs(m_swing - 0.5) > 1e-9 && slot % 2 != 0)
                    at += (m_swing - 0.5) * grid;
                if (at < range.first - 1e-9 || at > range.second + 1e-9)
                    continue;
                const double x = beatsToX(at);
                if (x < keyWidth || x > width()) continue;
                p.drawLine(QPointF(x, gridTop), QPointF(x, fieldBottom));
            }
        }
    }
    const int finalBeat = int(std::ceil(totalBeats));
    for (const auto& range : dirtyBeatRanges) {
        const int firstBeat = std::max(0, int(std::floor(range.first)));
        const int lastBeat = std::min(finalBeat, int(std::ceil(range.second)));
        for (int beat = firstBeat; beat <= lastBeat; ++beat) {
            const bool bar = beat % beatsPerBar == 0;
            const double x = beatsToX(double(beat));
            if (x < keyWidth || x > width()) continue;
            p.setPen(QPen(bar ? gridStrong : gridBase,
                          bar ? 1.0 + m_gridContrast : 1.0));
            p.drawLine(QPointF(x, gridTop), QPointF(x, fieldBottom));
        }
    }

    // Local bar numbers and beat ticks. Labels thin out only when the current
    // zoom would make them collide; their numbering never changes with scroll
    // or with the clip's absolute project position.
    QFont rulerFont = p.font();
    rulerFont.setPixelSize(10);
    p.setFont(rulerFont);
    const double barWidth = beatsPerBar * px;
    const int labelStride =
        std::max(1, int(std::ceil(48.0 / std::max(1.0, barWidth))));
    const int rulerBeatCount = int(std::ceil(totalBeats - 1e-9));
    // A playhead strip can cross the right half of a bar number while the
    // number's anchor beat lies just outside that strip. Include the maximum
    // label overhang so clearing the old playhead never erases part of a label.
    const double rulerLabelOverhang = std::max(
        48.0,
        double(p.fontMetrics().horizontalAdvance(QString::number(
                   std::max(1, rulerBeatCount / beatsPerBar + 1))) +
               6));
    for (const auto& range : dirtyBeatRanges) {
        const int firstBeat = std::max(
            0, int(std::floor(range.first - rulerLabelOverhang / px)));
        const int lastBeat = std::min(
            rulerBeatCount - 1, int(std::ceil(range.second)));
        for (int beat = firstBeat; beat <= lastBeat; ++beat) {
            const double x = beatsToX(double(beat));
            if (x < keyWidth || x > width()) continue;
            const bool bar = beat % beatsPerBar == 0;
            p.setPen(QPen(bar ? gridStrong : gridBase, 1.0));
            p.drawLine(QPointF(x, gridTop - (bar ? 8.0 : 4.0)),
                       QPointF(x, gridTop));
            if (bar && (beat / beatsPerBar) % labelStride == 0) {
                p.setPen(t.textSecondary);
                p.drawText(QPointF(x + 4.0, gridTop - 10.0),
                           QString::number(beat / beatsPerBar + 1));
            }
        }
    }

    // ── Ghost notes ──
    //
    // Other parts, drawn faint and never hit-tested: they are there so a line
    // can be written against the bass or the chords, not to be edited here.
    if (!m_ghostTracks.isEmpty() && !dirtyBeatRanges.empty() &&
        dirtyRect.intersects(field)) {
        const double tempo = m_controller->project().tempo;
        // Usually only one or two tracks are ghosted. Resolve those directly
        // instead of walking every track in the project on each playhead frame.
        for (const QString& trackId : m_ghostTracks) {
            if (trackId == m_trackId) continue;
            const auto* track =
                m_controller->project().findTrack(trackId.toStdString());
            if (!track) continue;
            const std::uint64_t revision =
                m_controller->midiNotesRevision(track->id);
            const QColor ghost = mixColors(ui::colorFromRgb(track->color),
                                           t.background, 0.55);
            for (const auto& other : track->clips) {
                if (other.kind != daw::ClipKind::Midi) continue;
                // Ghost clips are placed against *this* clip's start, so the
                // beat grid on screen is the shared timeline.
                const double offset = daw::secondsToBeats(
                    other.startSeconds - c->startSeconds, tempo);
                const daw::MidiPreviewIndex* index = nullptr;
                daw::MidiPreviewIndex transient;
                if (other.id.empty()) {
                    transient.rebuild(other.notes);
                    index = &transient;
                } else {
                    // Track ids disambiguate malformed legacy documents that
                    // accidentally reused a clip id on more than one lane.
                    const std::string key = track->id + '\n' + other.id;
                    auto [found, inserted] =
                        m_ghostPaintIndexes.try_emplace(key);
                    GhostPaintIndexEntry& entry = found->second;
                    if (inserted || entry.revision != revision ||
                        entry.noteCount != other.notes.size()) {
                        entry.index.rebuild(other.notes);
                        entry.revision = revision;
                        entry.noteCount = other.notes.size();
                    }
                    index = &entry.index;
                }

                m_ghostPaintScratch.clear();
                const double minimumPaintBeats = 3.0 / std::max(1.0, px);
                for (const auto& range : dirtyBeatRanges) {
                    index->forEachVisible(
                        other.notes,
                        range.first - offset - minimumPaintBeats,
                        range.second - offset + 1.0 / std::max(1.0, px),
                        [this](const daw::NoteModel&, std::size_t noteIndex) {
                            m_ghostPaintScratch.push_back(noteIndex);
                        });
                }
                // One long note can cross both old and new playhead strips.
                // Paint it once through Qt's disjoint clip region.
                std::sort(m_ghostPaintScratch.begin(),
                          m_ghostPaintScratch.end());
                m_ghostPaintScratch.erase(
                    std::unique(m_ghostPaintScratch.begin(),
                                m_ghostPaintScratch.end()),
                    m_ghostPaintScratch.end());
                p.setBrush(ghost);
                p.setPen(Qt::NoPen);
                const bool roundedGhosts =
                    m_noteStyle == NoteStyle::Rounded && px >= 64.0 &&
                    m_rowHeight >= 8.0;
                p.setRenderHint(QPainter::Antialiasing, roundedGhosts);
                for (std::size_t noteIndex : m_ghostPaintScratch) {
                    const daw::NoteModel& note = other.notes[noteIndex];
                    QRectF r = noteRect(note);
                    // beatsToX() is affine in beat-space, so translating the
                    // rectangle is equivalent to copying/mutating the note —
                    // without allocating its id string on every ghost paint.
                    r.translate(offset * px, 0.0);
                    if (r.bottom() < gridTop || r.top() > fieldBottom) continue;
                    if (r.right() < keyWidth || r.left() > width()) continue;
                    if (!r.intersects(dirtyRect)) continue;
                    if (!event->region().intersects(r.toAlignedRect())) continue;
                    r = ui::pixelAlignedRect(r, devicePixelRatioF());
                    if (!roundedGhosts || r.width() < 12.0)
                        p.drawRect(r);
                    else
                        p.drawRoundedRect(r, 3, 3);
                }
                p.setRenderHint(QPainter::Antialiasing, false);
            }
        }
    }

    // ── Notes ──
    QFont noteFont = p.font();
    noteFont.setPixelSize(9);
    const auto& notesToPaint = m_preview ? *m_preview : c->notes;
    const std::uint64_t currentNoteRevision =
        m_preview ? 0
                  : m_controller->midiNotesRevision(m_trackId.toStdString());
    const daw::MidiPreviewIndex& notePaintIndex =
        notePaintIndexFor(notesToPaint);
    const bool frozenGeometry =
        !m_preview && freezesDocumentNoteIndex() &&
        m_notePaintSource == &notesToPaint &&
        (m_notePaintRevision != currentNoteRevision ||
         m_notePaintCount != notesToPaint.size());
    const auto gestureOwnsNote = [&](const daw::NoteModel& note) {
        if (!frozenGeometry) return false;
        if (m_selectionEditUndoActive && m_selected.isEmpty()) return true;
        return m_selected.contains(QString::fromStdString(note.id));
    };

    const auto paintNote = [&](const daw::NoteModel& n) {
        const QRectF r = noteRect(n);
        if (r.bottom() < gridTop || r.top() > fieldBottom) return;
        if (r.right() < keyWidth || r.left() > width()) return;
        if (!r.intersects(dirtyRect)) return;
        if (!event->region().intersects(r.toAlignedRect())) return;
        const bool selected = m_selected.contains(QString::fromStdString(n.id));

        QColor fill = colorFor(n, clipColor);
        if (n.muted) {
            // A muted note stays exactly where it is and reads as switched off.
            fill = mixColors(fill, t.background, 0.7);
        }
        if (selected) fill = mixColors(fill, Qt::white, 0.35);
        if (m_preview) fill = mixColors(fill, t.accent, 0.35);

        paintNoteShape(p, r, fill, selected, n.muted);

        if (m_showNoteNames && m_rowHeight >= 11.0 && r.width() > 26.0) {
            p.setFont(noteFont);
            p.setPen(fill.lightnessF() > 0.6 ? QColor(0x22, 0x22, 0x22)
                                             : QColor(0xF0, 0xF0, 0xF0));
            p.drawText(r.adjusted(4, 0, -3, 0),
                       Qt::AlignLeft | Qt::AlignVCenter, noteName(n.pitch));
        }
    };

    if (!dirtyBeatRanges.empty() && dirtyRect.intersects(field)) {
        m_notePaintScratch.clear();
        const double minimumPaintBeats = 3.0 / std::max(1.0, px);
        for (const auto& range : dirtyBeatRanges) {
            notePaintIndex.forEachVisible(
                notesToPaint, range.first - minimumPaintBeats,
                range.second + 1.0 / std::max(1.0, px),
                [this](const daw::NoteModel&, std::size_t noteIndex) {
                    m_notePaintScratch.push_back(noteIndex);
                });
        }
        // Preserve document draw order for overlapping notes. The range index
        // is only a query accelerator, and one long note may cross two dirty
        // playhead strips, hence the de-duplication.
        std::sort(m_notePaintScratch.begin(), m_notePaintScratch.end());
        m_notePaintScratch.erase(
            std::unique(m_notePaintScratch.begin(), m_notePaintScratch.end()),
            m_notePaintScratch.end());
        for (std::size_t index : m_notePaintScratch) {
            if (!m_pendingErase.contains(
                    QString::fromStdString(notesToPaint[index].id)) &&
                !gestureOwnsNote(notesToPaint[index]))
                paintNote(notesToPaint[index]);
        }
        if (frozenGeometry) {
            if (const auto* live = liveGeometryNotes()) {
                for (const auto& note : *live) paintNote(note);
            }
        }
    }

    // ── Stretch phantom ──
    //
    // While a stretch is armed the originals stay put and a hollow outline
    // shows where every note would land, so the gesture can be aimed before it
    // is committed. The scale factor rides along near the pointer.
    if (m_stretching) {
        // Wash the originals back so the phantom reads as the live result.
        for (const auto& n : m_stretchOrig) {
            const QRectF r = noteRect(n);
            if (r.bottom() < gridTop || r.top() > fieldBottom) continue;
            if (r.right() < keyWidth || r.left() > width()) continue;
            p.fillRect(r, QColor(t.background.red(), t.background.green(),
                                 t.background.blue(), 110));
        }
        for (const auto& n : m_stretchPreview) {
            const QRectF r = ui::pixelAlignedRect(
                noteRect(n), devicePixelRatioF());
            if (r.bottom() < gridTop || r.top() > fieldBottom) continue;
            if (r.right() < keyWidth || r.left() > width()) continue;
            p.setPen(QPen(t.accent, 1.5, Qt::DashLine));
            p.setBrush(QColor(t.accent.red(), t.accent.green(), t.accent.blue(),
                              45));
            const bool rounded = m_noteStyle == NoteStyle::Rounded &&
                                 r.width() >= 8.0 && r.height() >= 6.0;
            p.setRenderHint(QPainter::Antialiasing, rounded);
            if (rounded) p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75),
                                           3.0, 3.0);
            else p.drawRect(r.adjusted(0.75, 0.75, -0.75, -0.75));
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        // The scale factor, as a percentage, next to the pointer.
        if (m_pointerInside) {
            const int percent = int(std::lround(m_stretchScale * 100.0));
            const QString label = tr("%1%").arg(percent);
            QFont f = p.font();
            f.setPixelSize(11);
            f.setBold(true);
            p.setFont(f);
            const QRectF tr = p.fontMetrics().boundingRect(label);
            const QPointF tip(m_pointer.x() + 14, m_pointer.y() - 14);
            const QRectF bg(tip.x() - 4, tip.y() - tr.height() - 4,
                            tr.width() + 8, tr.height() + 8);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 190));
            p.drawRoundedRect(bg, 3, 3);
            p.setPen(t.textPrimary);
            p.drawText(bg, Qt::AlignCenter, label);
        }
    }

    // ── Stretch handle ──
    //
    // A plain double-headed arrow sits *after* a multi-note selection, centred
    // on the group. No glass plate: the icon itself is the affordance, and the
    // larger invisible rect below keeps it easy to grab.
    if (!m_stretching) {
        const QRectF hr = stretchHandleRect();
        if (!hr.isNull() && hr.right() >= keyWidth && hr.left() <= width()) {
            const bool hover = onStretchHandle(m_pointer);
            const QColor ink = hover ? t.accent : t.textSecondary;
            icons::paint(p, icons::Glyph::ResizeHorizontal,
                         hr.adjusted(2.0, 2.0, -2.0, -2.0), ink);
        }
    }

    if (m_marquee) {
        const QRectF box = QRectF(m_marqueeOrigin, m_marqueeCurrent).normalized();
        p.setPen(QPen(t.accent, 1.0, Qt::DashLine));
        p.setBrush(QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 40));
        p.drawRect(box);
    }

    // The blade's line, so a slice is aimed rather than guessed at.
    if (activeTool() == Tool::Slice && m_pointerInside &&
        m_pointer.x() >= keyWidth && m_pointer.y() >= gridTop &&
        m_pointer.y() < fieldBottom) {
        const double x = beatsToX(snapBeats(xToBeats(m_pointer.x()), m_snapEnabled));
        p.setPen(QPen(t.accent, 1.0, Qt::DashLine));
        p.drawLine(QPointF(x, gridTop), QPointF(x, fieldBottom));
    }

    // ── Playhead ──
    if (m_controller) {
        const double beats = daw::secondsToBeats(
            m_controller->presentationPositionSeconds() - c->startSeconds,
            m_controller->project().tempo);
        if (beats >= 0.0 && beats <= totalBeats) {
            const double x = beatsToX(beats);
            if (x >= keyWidth && x <= width()) {
                p.setPen(QPen(t.accent, 1.5));
                p.drawLine(QPointF(x, gridTop - 1.0),
                           QPointF(x, fieldBottom));
                QPainterPath marker;
                marker.moveTo(x - 5.0, 0.0);
                marker.lineTo(x + 5.0, 0.0);
                marker.lineTo(x, 7.0);
                marker.closeSubpath();
                p.fillPath(marker, t.accent);
            }
        }
    }

    // The keyboard goes on last so nothing can scroll over it. A moving
    // playhead normally dirties only two narrow grid strips, so don't traverse
    // all 128 keys unless that fixed column is actually in the update region.
    if (dirtyRect.intersects(QRectF(0.0, gridTop, keyWidth + 2.0,
                                    fieldBottom - gridTop))) {
        paintKeyboard(p, fieldBottom);
    }
    p.restore();

    // ── The parameter lane ──
    if (dirtyRect.bottom() > fieldBottom) paintLane(p);
    m_paintClip = nullptr;
}

/// One note's body, in whichever style the roll is set to. Both styles paint
/// inside the pixel-aligned bounds so neither end is clipped by its own stroke.
void PianoRollView::paintNoteShape(QPainter& p, const QRectF& r,
                                   const QColor& fill, bool selected,
                                   bool muted) const {
    const Theme& t = th();
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const qreal pixel = 1.0 / std::max<qreal>(1.0, dpr);
    const QRectF shape = ui::pixelAlignedRect(r, dpr);
    const qreal borderWidth = selected ? 2.0 * pixel
                              : m_noteBorders ? pixel
                                              : 0.0;
    const QColor border = selected
                              ? t.textPrimary
                              : mixColors(fill, Qt::black, muted ? 0.35 : 0.48);

    p.save();
    if (m_noteStyle == NoteStyle::Flat) {
        // The flat style is deliberately raster-sharp: border and fill are
        // nested rectangles rather than a centred pen that loses half a pixel
        // at the note's beginning and end.
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        if (borderWidth > 0.0) {
            p.fillRect(shape, border);
            const QRectF inner = shape.adjusted(borderWidth, borderWidth,
                                                -borderWidth, -borderWidth);
            if (inner.width() > 0.0 && inner.height() > 0.0)
                p.fillRect(inner, fill);
        } else {
            p.fillRect(shape, fill);
        }
        p.restore();
        return;
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF body = shape.adjusted(borderWidth * 0.5,
                                       borderWidth * 0.5,
                                      -borderWidth * 0.5,
                                      -borderWidth * 0.5);
    if (body.width() <= 0.0 || body.height() <= 0.0) {
        p.restore();
        return;
    }
    const qreal radius = std::min({4.0, body.height() * 0.32,
                                  body.width() * 0.25});
    QColor top = mixColors(fill, Qt::white, muted ? 0.05 : 0.12);
    QColor bottom = mixColors(fill, Qt::black, muted ? 0.04 : 0.10);
    QLinearGradient face(body.topLeft(), body.bottomLeft());
    face.setColorAt(0.0, top);
    face.setColorAt(1.0, bottom);
    p.setBrush(face);
    if (borderWidth > 0.0) p.setPen(QPen(border, borderWidth));
    else p.setPen(Qt::NoPen);
    p.drawRoundedRect(body, radius, radius);
    p.restore();
}

/// A piano that reads as a piano: keys lit from the left, a shadow under each
/// one, black keys sitting proud of the whites, and any key currently held —
/// by the mouse or by the playhead — pressed in.
void PianoRollView::paintKeyboard(QPainter& p, double fieldBottom) {
    if (!m_showKeyboard) return;
    const Theme& t = th();
    const double keyWidth = keyboardWidth();
    const double gridTop = ui::kRulerHeight;
    const PitchMask sounding = keyboardPitches();
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const qreal pixel = 1.0 / std::max<qreal>(1.0, dpr);

    p.save();
    p.setClipRect(QRectF(0, gridTop, keyWidth,
                         std::max(0.0, fieldBottom - gridTop)));
    p.fillRect(QRectF(0, gridTop, keyWidth,
                      std::max(0.0, fieldBottom - gridTop)),
               QColor(0x14, 0x14, 0x16));

    QFont font = p.font();
    font.setPixelSize(9);
    p.setFont(font);

    // Two passes: every white key first, then the black ones on top. A black
    // key overlaps its neighbours, which is the whole reason a piano looks like
    // a piano and cannot be drawn in one pass down the rows.
    for (int pass = 0; pass < 2; ++pass) {
        const bool black = pass == 1;
        for (int pitch = kMinPitch; pitch <= kMaxPitch; ++pitch) {
            if (isBlackKey(pitch) != black) continue;
            const double y = pitchToY(pitch);
            if (y + m_rowHeight < gridTop || y > fieldBottom) continue;

            const bool held = pitch == m_pressedKey || sounding.test(size_t(pitch));
            const double w = black ? keyWidth * 0.62 : keyWidth;
            QRectF key = ui::pixelAlignedRect(QRectF(0, y, w, m_rowHeight), dpr);
            key.adjust(0.0, 0.0, 0.0, -pixel);
            if (key.height() <= 0.0) continue;
            const qreal radius = std::min(2.0, key.height() * 0.24);
            const QPainterPath keyPath = pianoKeyPath(key, radius);

            QLinearGradient face(key.topLeft(), key.bottomLeft());
            if (black) {
                face.setColorAt(0.0, held ? QColor(0x5A, 0x5A, 0x62)
                                          : QColor(0x3A, 0x3A, 0x40));
                face.setColorAt(0.55, held ? QColor(0x33, 0x33, 0x39)
                                           : QColor(0x1C, 0x1C, 0x20));
                face.setColorAt(1.0, QColor(0x0E, 0x0E, 0x11));
            } else {
                face.setColorAt(0.0, held ? QColor(0xD6, 0xE4, 0xF6)
                                          : QColor(0xFA, 0xFA, 0xFA));
                face.setColorAt(0.6, held ? QColor(0xC2, 0xD6, 0xEE)
                                          : QColor(0xEC, 0xEC, 0xEC));
                face.setColorAt(1.0, held ? QColor(0xA8, 0xC2, 0xE2)
                                          : QColor(0xCF, 0xCF, 0xD2));
            }
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(face);
            p.drawPath(keyPath);

            // The lit edge along the top and the shadow along the bottom are
            // what give the key its thickness; a held key loses the highlight
            // and gains the shadow, so it reads as pushed in.
            if (m_rowHeight >= 7.0) {
                p.setPen(QPen(QColor(255, 255, 255, held ? 30 : 90), pixel));
                p.drawLine(key.topLeft() + QPointF(pixel, pixel * 0.5),
                           key.topRight() + QPointF(-radius, pixel * 0.5));
                p.setPen(QPen(QColor(0, 0, 0, black ? 150 : 70), pixel));
                p.drawLine(key.bottomLeft() + QPointF(pixel, -pixel * 0.5),
                           key.bottomRight() + QPointF(-radius, -pixel * 0.5));
            }
            if (black) {
                p.setPen(QPen(QColor(0, 0, 0, 150), pixel));
                p.drawLine(QPointF(key.right() - pixel * 0.5,
                                   key.top() + radius),
                           QPointF(key.right() - pixel * 0.5,
                                   key.bottom() - radius));
            }
            if (held) {
                // A wash of the accent so a sounding note is obvious at a
                // glance, not just a slightly different grey.
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(t.accent.red(), t.accent.green(),
                                  t.accent.blue(), black ? 110 : 70));
                p.drawPath(keyPath);
            }

            const bool nameIt = m_showAllKeyNames || pitch % 12 == 0;
            if (nameIt && m_rowHeight >= 8.0) {
                p.setPen(black ? QColor(0xD8, 0xD8, 0xDC) : QColor(0x44, 0x44, 0x48));
                p.drawText(key.adjusted(4, 0, -4, 0),
                           Qt::AlignRight | Qt::AlignVCenter, noteName(pitch));
            }
        }
    }
    p.restore();
}

void PianoRollView::SoundingPitchIndex::rebuild(const mt::Notes& notes) {
    for (auto& starts : startsByPitch) starts.clear();
    for (auto& ends : endsByPitch) ends.clear();

    for (const auto& note : notes) {
        if (note.muted || note.pitch < kMinPitch || note.pitch > kMaxPitch)
            continue;
        const std::size_t pitch = std::size_t(note.pitch - kMinPitch);
        startsByPitch[pitch].push_back(note.startBeats);
        endsByPitch[pitch].push_back(note.startBeats + note.lengthBeats);
    }
    for (std::size_t pitch = 0; pitch < startsByPitch.size(); ++pitch) {
        std::sort(startsByPitch[pitch].begin(), startsByPitch[pitch].end());
        std::sort(endsByPitch[pitch].begin(), endsByPitch[pitch].end());
    }
}

PianoRollView::PitchMask
PianoRollView::SoundingPitchIndex::pitchesAt(
    double beat, std::size_t* comparisonCount) const {
    PitchMask pitches;
    const auto upperBound = [beat, comparisonCount](
                                const std::vector<double>& values) {
        std::size_t first = 0;
        std::size_t count = values.size();
        while (count > 0) {
            const std::size_t step = count / 2;
            const std::size_t middle = first + step;
            if (comparisonCount) ++*comparisonCount;
            if (values[middle] <= beat) {
                first = middle + 1;
                count -= step + 1;
            } else {
                count = step;
            }
        }
        return first;
    };
    for (std::size_t pitch = 0; pitch < startsByPitch.size(); ++pitch) {
        const auto& starts = startsByPitch[pitch];
        const auto& ends = endsByPitch[pitch];
        // Starts are inclusive and ends exclusive. upper_bound therefore
        // counts both sets at the exact boundary without visiting intervals.
        if (upperBound(starts) > upperBound(ends)) pitches.set(pitch);
    }
    return pitches;
}

void PianoRollView::invalidateSoundingPitchIndex() const noexcept {
    if (m_moving || m_resizing || m_muting || m_selectionEditUndoActive) {
        m_soundingPitchInvalidationDeferred = true;
        return;
    }
    m_soundingPitchInvalidationDeferred = false;
    m_soundingPitchIndexDirty = true;
}

const daw::MidiPreviewIndex& PianoRollView::notePaintIndexFor(
    const mt::Notes& notes) const {
    const auto* current = clip();
    const bool documentNotes = current && &notes == &current->notes;
    const std::uint64_t revision =
        documentNotes && m_controller
            ? m_controller->midiNotesRevision(m_trackId.toStdString())
            : 0;
    // During a geometry gesture the selected notes are painted from a compact
    // live vector below. The index keeps snapshot start/end values for every
    // unchanged note, so rebuilding/sorting 100k notes on every mouse sample
    // would only make the pointer lag without improving the visible result.
    if (documentNotes && freezesDocumentNoteIndex() &&
        m_notePaintSource == &notes) {
        return m_notePaintIndex;
    }
    if (m_notePaintSource != &notes || m_notePaintCount != notes.size() ||
        m_notePaintRevision != revision) {
        m_notePaintSource = &notes;
        m_notePaintCount = notes.size();
        m_notePaintRevision = revision;
        m_notePaintIndex.rebuild(notes);
        if (documentNotes) invalidateSoundingPitchIndex();
    }
    return m_notePaintIndex;
}

bool PianoRollView::freezesDocumentNoteIndex() const noexcept {
    return m_moving || m_resizing || m_drawing ||
           m_selectionEditUndoActive;
}

const std::vector<daw::NoteModel>*
PianoRollView::liveGeometryNotes() const noexcept {
    if (m_moving) return &m_moveWorking;
    if (m_resizing || m_drawing) return &m_geometryPaintNotes;
    if (m_selectionEditUndoActive) return &m_selectionEditWorking;
    return nullptr;
}

void PianoRollView::ensureDocumentNoteIdIndex(
    const daw::ClipModel& current) const {
    const std::uint64_t revision =
        m_controller
            ? m_controller->midiNotesRevision(m_trackId.toStdString())
            : 0;
    if (m_noteIdIndexSource == &current.notes &&
        m_noteIdIndexCount == current.notes.size() &&
        m_noteIdIndexRevision == revision) {
        return;
    }
    m_noteById.clear();
    m_noteById.reserve(current.notes.size());
    for (std::size_t i = 0; i < current.notes.size(); ++i) {
        if (!current.notes[i].id.empty())
            m_noteById.try_emplace(current.notes[i].id, i);
    }
    m_noteIdIndexSource = &current.notes;
    m_noteIdIndexCount = current.notes.size();
    m_noteIdIndexRevision = revision;
}

void PianoRollView::invalidateNotePaintIndex() noexcept {
    m_notePaintSource = nullptr;
    m_notePaintCount = 0;
    m_notePaintRevision = 0;
    m_notePaintScratch.clear();
}

void PianoRollView::invalidateDocumentPaintCaches() {
    invalidateNotePaintIndex();
    m_noteIdIndexSource = nullptr;
    m_noteIdIndexCount = 0;
    m_noteIdIndexRevision = 0;
    m_noteById.clear();
    m_ghostPaintIndexes.clear();
    m_ghostPaintScratch.clear();
    // This is the hard reset used by setClip/project replacement. It must win
    // even if the old clip was switched away mid-gesture.
    m_soundingPitchInvalidationDeferred = false;
    m_soundingPitchIndexDirty = true;
}

void PianoRollView::ensureSoundingPitchIndex(const mt::Notes& notes) const {
    if (!m_soundingPitchIndexDirty && m_soundingPitchSource == &notes &&
        m_soundingPitchData == notes.data() &&
        m_soundingPitchCount == notes.size()) {
        return;
    }
    m_soundingPitchIndex.rebuild(notes);
    m_soundingPitchSource = &notes;
    m_soundingPitchData = notes.data();
    m_soundingPitchCount = notes.size();
    m_soundingPitchIndexDirty = false;
    ++m_soundingPitchIndexRebuilds;
}

PianoRollView::PitchMask
PianoRollView::soundingPitchesAtBeat(double beat) const {
    const auto* c = clip();
    if (!c) return {};
    ensureSoundingPitchIndex(c->notes);
    return m_soundingPitchIndex.pitchesAt(beat);
}

PianoRollView::PitchMask PianoRollView::soundingPitches() const {
    const auto* c = clip();
    if (!c || !m_controller || !m_controller->isPlaying()) return {};
    const double at = daw::secondsToBeats(
        m_controller->presentationPositionSeconds() - c->startSeconds,
        m_controller->project().tempo);
    ensureSoundingPitchIndex(c->notes);
    return m_soundingPitchIndex.pitchesAt(at);
}

void PianoRollView::paintLane(QPainter& p) {
    if (!m_showVelocityLane) return;
    const auto* c = clip();
    if (!c) return;
    const Theme& t = th();
    const double keyWidth = keyboardWidth();
    const double fieldBottom = laneTop();
    const QRectF lane(0, fieldBottom, double(width()), laneHeight());
    const QRectF parameterField(keyWidth, fieldBottom,
                                std::max(0.0, double(width()) - keyWidth),
                                laneHeight());
    const QColor laneAccent =
        m_laneParam == LaneParam::Controller
            ? Theme::automationAccent()
            : m_laneParam == LaneParam::Pan ? Theme::audioAccent() : t.accent;
    const daw::ControllerLane* paintedController =
        m_laneParam == LaneParam::Controller ? controllerLane() : nullptr;

    // Recessed, but not flat: a restrained vertical tone separates the lane
    // from the piano grid without adding a decorative glass layer.
    QLinearGradient background(0.0, fieldBottom, 0.0, lane.bottom());
    background.setColorAt(0.0, mixColors(t.surface, t.background, 0.22));
    background.setColorAt(1.0, mixColors(t.background, t.surface, 0.18));
    p.fillRect(lane, background);
    p.fillRect(QRectF(0.0, fieldBottom, keyWidth, laneHeight()),
               mixColors(t.surface, t.background, 0.36));
    p.setPen(QPen(t.separator(), 1.0));
    p.drawLine(QPointF(keyWidth - 0.5, fieldBottom),
               QPointF(keyWidth - 0.5, lane.bottom()));

    // The divider doubles as the resize target. It lights up under the pointer
    // and has a visible two-way arrow, so the gesture is discoverable without
    // requiring the user to hit an unexplained one-pixel line.
    const bool overGrip =
        m_pointerInside && std::abs(m_pointer.y() - fieldBottom) <= kLaneGripPx;
    p.setPen(QPen(overGrip || m_laneResizing ? t.accent : t.separator(),
                  overGrip || m_laneResizing ? 2.0 : 1.0));
    p.drawLine(lane.topLeft(), lane.topRight());

    const auto paintResizeGrip = [&] {
        const QRectF grip(std::max(keyWidth + 8.0,
                                   parameterField.center().x() - 15.0),
                          fieldBottom - 8.0, 30.0, 16.0);
        p.setPen(QPen(overGrip || m_laneResizing ? laneAccent : t.separator(),
                      1.0));
        p.setBrush(mixColors(t.surfaceElevated, t.background, 0.18));
        p.drawRoundedRect(grip, 8.0, 8.0);
        icons::paint(p, icons::Glyph::ResizeVertical,
                     grip.adjusted(7.0, 0.0, -7.0, 0.0),
                     overGrip || m_laneResizing
                         ? laneAccent
                         : mixColors(t.textPrimary, laneAccent, 0.16));
    };

    QString label = tr("VEL");
    QString range = tr("1–127");
    if (m_laneParam == LaneParam::Pan) label = tr("PAN");
    if (m_laneParam == LaneParam::Pan) range = tr("L — R");
    if (paintedController) {
        label = QString::fromStdString(paintedController->name).toUpper();
        range = tr("0–100%");
    }
    QFont laneFont = p.font();
    laneFont.setPixelSize(9);
    laneFont.setWeight(QFont::DemiBold);
    laneFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    p.setFont(laneFont);
    p.setPen(laneAccent);
    p.drawText(QRectF(7.0, fieldBottom + 8.0, keyWidth - 12.0, 14.0),
               Qt::AlignLeft | Qt::AlignVCenter, label);
    laneFont.setWeight(QFont::Normal);
    laneFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    p.setFont(laneFont);
    p.setPen(t.textSecondary);
    p.drawText(QRectF(7.0, fieldBottom + 23.0, keyWidth - 12.0, 13.0),
               Qt::AlignLeft | Qt::AlignVCenter, range);

    p.save();
    p.setClipRect(parameterField);

    // Quiet value guides make height and centre readable at a glance, but stay
    // behind the actual data. The middle guide is slightly stronger.
    for (double guide : {0.25, 0.5, 0.75}) {
        QColor guideColor = t.separator();
        guideColor.setAlphaF(guide == 0.5 ? 0.72 : 0.42);
        p.setPen(QPen(guideColor, 1.0,
                      guide == 0.5 ? Qt::SolidLine : Qt::DashLine));
        const double y = laneValueToY(guide);
        p.drawLine(QPointF(keyWidth, y), QPointF(width(), y));
    }

    if (m_laneParam == LaneParam::Controller) {
        if (!paintedController) {
            p.restore();
            paintResizeGrip();
            return;
        }
        // A curve, not a bar per note: a controller is continuous, and the
        // whole point of the lane is the shape between the breakpoints.
        const double defaultY = laneValueToY(paintedController->defaultValue);
        const double visibleFirstBeat = xToBeats(keyWidth - 2.0);
        const double visibleLastBeat = xToBeats(double(width()) + 2.0);
        const auto firstVisible = std::lower_bound(
            paintedController->points.begin(), paintedController->points.end(),
            visibleFirstBeat,
            [](const daw::AutomationPoint& point, double beat) {
                return point.beats < beat;
            });
        auto firstPoint = firstVisible;
        if (firstPoint != paintedController->points.begin()) --firstPoint;
        QPolygonF line;
        // Keep one vertex on either side of the viewport so clipping preserves
        // the exact incoming/outgoing segment without allocating points for
        // the rest of a long controller recording.
        if (firstPoint == paintedController->points.begin() &&
            (paintedController->points.empty() ||
             firstPoint->beats >= visibleFirstBeat)) {
            line << QPointF(beatsToX(0.0), defaultY);
        }
        for (auto point = firstPoint; point != paintedController->points.end();
             ++point) {
            line << QPointF(beatsToX(point->beats),
                            laneValueToY(point->value));
            if (point->beats > visibleLastBeat) break;
        }
        if (line.isEmpty()) line << QPointF(beatsToX(0.0), defaultY);
        if (paintedController->points.empty() ||
            paintedController->points.back().beats <= visibleLastBeat) {
            line << QPointF(beatsToX(clipBeats()), line.back().y());
        }

        QPolygonF filled = line;
        const double floorY = laneValueToY(0.0);
        filled << QPointF(line.back().x(), floorY)
               << QPointF(line.front().x(), floorY);
        p.setPen(Qt::NoPen);
        QColor curveFill = laneAccent;
        curveFill.setAlpha(38);
        p.setBrush(curveFill);
        p.drawPolygon(filled);
        p.setPen(QPen(mixColors(t.background, t.surface, 0.4), 4.0,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
        p.setPen(QPen(laneAccent, 1.8, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line);

        const auto firstHandle = std::lower_bound(
            paintedController->points.begin(), paintedController->points.end(),
            visibleFirstBeat,
            [](const daw::AutomationPoint& point, double beat) {
                return point.beats < beat;
            });
        for (auto point = firstHandle; point != paintedController->points.end();
             ++point) {
            if (point->beats > visibleLastBeat) break;
            const QPointF handle(beatsToX(point->beats),
                                 laneValueToY(point->value));
            p.setBrush(t.surfaceElevated);
            p.setPen(QPen(laneAccent, 1.8));
            p.drawEllipse(handle, kHandleRadius + 0.5, kHandleRadius + 0.5);
            p.setPen(Qt::NoPen);
            p.setBrush(laneAccent);
            p.drawEllipse(handle, 1.7, 1.7);
        }
        p.restore();
        paintResizeGrip();
        return;
    }

    // Pan hangs off its centre; velocity rises from a floor that leaves enough
    // room for the full circular handle.
    const bool signedParam = m_laneParam == LaneParam::Pan;
    const double baseY = signedParam ? laneValueToY(0.5) : laneValueToY(0.0);
    if (signedParam) {
        p.setPen(QPen(mixColors(t.textSecondary, t.background, 0.42), 1.2));
        p.drawLine(QPointF(keyWidth, baseY), QPointF(width(), baseY));
    }

    // `handleAt` is O(note count). Resolve the hovered column once, not once for
    // every stalk: the old loop turned a dense lane repaint into O(N^2).
    const QString hoveredId =
        m_pointerInside && m_pointer.y() >= fieldBottom ? handleAt(m_pointer)
                                                        : QString{};
    const auto& laneNotes = m_preview ? *m_preview : c->notes;
    const daw::MidiPreviewIndex& laneNoteIndex =
        notePaintIndexFor(laneNotes);
    m_notePaintScratch.clear();
    laneNoteIndex.forEachVisible(
        laneNotes, xToBeats(keyWidth - kHandleGrabPx),
        xToBeats(double(width()) + kHandleGrabPx),
        [this](const daw::NoteModel&, std::size_t noteIndex) {
            m_notePaintScratch.push_back(noteIndex);
        });
    std::sort(m_notePaintScratch.begin(), m_notePaintScratch.end());
    for (std::size_t noteIndex : m_notePaintScratch) {
        const auto& n = laneNotes[noteIndex];
        if (m_pendingErase.contains(QString::fromStdString(n.id))) continue;
        const QPointF handle = laneHandle(n);
        if (handle.x() < keyWidth - kHandleGrabPx ||
            handle.x() > width() + kHandleGrabPx) {
            continue;
        }
        const QString id = QString::fromStdString(n.id);
        const bool selected = m_selected.contains(id);
        const bool hovered = !hoveredId.isEmpty() && hoveredId == id;
        const QColor colour =
            selected ? laneAccent
                     : mixColors(t.textSecondary, t.background, 0.28);
        p.setPen(QPen(mixColors(t.background, t.surfaceElevated, 0.3),
                      selected || hovered ? 4.0 : 3.0, Qt::SolidLine,
                      Qt::RoundCap));
        p.drawLine(QPointF(handle.x(), baseY), handle);
        p.setPen(QPen(colour, hovered ? 2.4 : selected ? 2.0 : 1.2,
                      Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(handle.x(), baseY), handle);
        const double radius = hovered ? kHandleRadius + 1.2
                                      : selected ? kHandleRadius + 0.5
                                                 : kHandleRadius;
        p.setBrush(t.surfaceElevated);
        p.setPen(QPen(colour, hovered ? 2.2 : selected ? 1.8 : 1.2));
        p.drawEllipse(handle, radius, radius);
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawEllipse(handle, hovered ? 2.1 : 1.6,
                      hovered ? 2.1 : 1.6);
    }
    p.restore();

    // The value of whatever is being dragged, so a move is readable.
    if (m_laneDragging && !m_primary.isEmpty()) {
        if (const auto* n = note(m_primary)) {
            QString readout = tr("velocity %1").arg(n->velocity);
            if (m_laneParam == LaneParam::Pan) {
                readout = std::abs(n->pan) < 0.005
                              ? tr("pan centre")
                              : tr("pan %1%2")
                                    .arg(n->pan < 0 ? tr("L") : tr("R"))
                                    .arg(int(std::lround(std::abs(n->pan) * 100)));
            }
            QFont readoutFont = p.font();
            readoutFont.setPixelSize(10);
            readoutFont.setWeight(QFont::DemiBold);
            p.setFont(readoutFont);
            const QRectF bubble(width() - 116.0, fieldBottom + 7.0, 106.0, 22.0);
            p.setPen(QPen(mixColors(t.separator(), laneAccent, 0.35), 1.0));
            p.setBrush(mixColors(t.surfaceElevated, t.background, 0.08));
            p.drawRoundedRect(bubble, 7.0, 7.0);
            p.setPen(t.textPrimary);
            p.drawText(bubble.adjusted(8.0, 0.0, -8.0, 0.0),
                       Qt::AlignRight | Qt::AlignVCenter, readout);
        }
    }
    paintResizeGrip();
}

// ── Input ───────────────────────────────────────────────────────────────────

void PianoRollView::mousePressEvent(QMouseEvent* ev) {
    if (!clip()) return;
    finishWheelNoteEdit();
    setFocus(Qt::MouseFocusReason);
    const QPointF pos = ev->position();
    m_pointer = pos;
    m_pointerInside = true;

    // The top strip belongs to the transport, not note editing. Its coordinate
    // system starts at the clip's first beat and is converted to absolute
    // project time only when seeking.
    // The cycle strip: the band above the bar numbers, the same one the
    // arrangement has, driving the same region.
    if (pos.y() < ui::kLoopStripHeight && ev->button() == Qt::LeftButton &&
        pos.x() >= keyboardWidth() && clip()) {
        // A double-click removes the cycle completely. A single drag is enough
        // to create and arm it, matching the arrangement ruler.
        if (ev->type() == QEvent::MouseButtonDblClick &&
            loopGrabAt(pos.x()) == LoopGrab::Move) {
            m_loopGrab = LoopGrab::None;
            m_controller->setLoopRangeSeconds(0.0, 0.0);
            m_controller->setLoopEnabled(false);
            emit loopRangeChanged();
            update();
            ev->accept();
            return;
        }

        const bool snapOn = m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        m_loopGrab = loopGrabAt(pos.x());
        const double at = std::max(0.0, snapBeats(xToBeats(pos.x()), snapOn));
        switch (m_loopGrab) {
            case LoopGrab::Create:
                m_loopAnchorBeats = at;
                m_controller->setLoopRangeSeconds(localBeatToSeconds(at),
                                                  localBeatToSeconds(at));
                break;
            case LoopGrab::Move:
                m_loopGrabOffset =
                    at - secondsToLocalBeat(m_controller->loopStartSeconds());
                m_loopGrabLength =
                    secondsToLocalBeat(m_controller->loopEndSeconds()) -
                    secondsToLocalBeat(m_controller->loopStartSeconds());
                break;
            case LoopGrab::ResizeStart:
                m_loopAnchorBeats =
                    secondsToLocalBeat(m_controller->loopEndSeconds());
                break;
            case LoopGrab::ResizeEnd:
                m_loopAnchorBeats =
                    secondsToLocalBeat(m_controller->loopStartSeconds());
                break;
            case LoopGrab::None:
                break;
        }
        setCursor(m_loopGrab == LoopGrab::Move ? Qt::ClosedHandCursor
                                               : Qt::SizeHorCursor);
        update();
        ev->accept();
        return;
    }

    if (pos.y() < ui::kRulerHeight) {
        if (ev->button() == Qt::LeftButton && pos.x() >= keyboardWidth()) {
            m_scrubbingPlayhead = true;
            seekToLocalBeat(xToBeats(pos.x()));
            setCursor(Qt::SizeHorCursor);
            ev->accept();
        }
        return;
    }

    if (!m_gestureUndoActive) {
        m_controller->beginNoteEdit(m_trackId.toStdString(),
                                    m_clipId.toStdString());
        m_gestureUndoActive = true;
    }
    const bool snapOn = m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
    const bool additive = ev->modifiers() & Qt::ShiftModifier;

    // The right button erases in every mode — the reason there is no eraser
    // tool to switch to. Holding it and sweeping rubs out a run of notes.
    // Arming it anywhere over the grid, not only on top of a note, is what
    // makes that sweep usable: you start the stroke on empty space and rub
    // across whatever is in the way. The whole travelled segment is tested so
    // coalesced mouse-move events cannot leave notes between samples behind.
    if (ev->button() == Qt::RightButton) {
        if (pos.x() >= keyboardWidth() && pos.y() < laneTop()) {
            bool onEdge = false;
            if (noteAt(pos, &onEdge).isEmpty() && !m_selected.isEmpty()) {
                m_selected.clear();
                m_primary.clear();
                emit selectionChanged();
            }
            m_erasing = true;
            m_eraseChanged = false;
            m_pendingErase.clear();
            m_lastErasePoint = pos;
            m_suppressContextMenu = true;
            updateCursor(pos);
            m_eraseChanged |= eraseStroke(pos, pos);
        }
        return;
    }
    if (ev->button() != Qt::LeftButton) return;
    // A preview belongs to a tool dialog; editing under it would be edited away
    // the moment the dialog recomputed.
    if (m_preview) return;

    // ── The grip that resizes the lane ──
    if (m_showVelocityLane && std::abs(pos.y() - laneTop()) <= kLaneGripPx) {
        m_laneResizing = true;
        m_laneResizeGrab = pos.y() + m_laneHeight;
        return;
    }

    // ── The parameter lane ──
    if (m_showVelocityLane && pos.y() >= laneTop()) {
        if (pos.x() < keyboardWidth()) return;

        if (m_laneParam == LaneParam::Controller) {
            const auto* lane = controllerLane();
            if (!lane) return;
            cancelControllerLaneWrite();
            m_lanePointsBefore = lane->points;
            m_laneWorkingPoints = lane->points;
            m_laneGestureTrackId = m_trackId;
            m_laneGestureClipId = m_clipId;
            m_laneGestureLaneId = m_laneId;
            const int hit = lanePointAt(pos);
            if (hit >= 0) {
                m_lanePointDrag = hit;
                m_laneLastWrittenPoint =
                    m_laneWorkingPoints[std::size_t(m_lanePointDrag)];
            } else {
                // Clicking empty lane adds a breakpoint and grabs it, so one
                // gesture both creates and places the point.
                daw::AutomationPoint added;
                added.beats = snapBeats(xToBeats(pos.x()), m_snapEnabled);
                added.value = laneValueAtY(pos.y());
                m_laneWorkingPoints.push_back(added);
                m_lanePointDrag = int(m_laneWorkingPoints.size()) - 1;
                m_laneLastWrittenPoint.reset();
                queueControllerLanePoint(pos, m_snapEnabled);
            }
            update();
            return;
        }

        const QString hit = handleAt(pos);
        if (hit.isEmpty()) return;
        // Grabbing an unselected handle selects that note first, so a drag can
        // still only ever move notes that are selected.
        if (!m_selected.contains(hit)) {
            if (additive) toggleSelected(hit); else selectOnly(hit);
        }
        m_primary = hit;
        m_laneDragging = true;
        m_laneGrab = laneValueAtY(pos.y());
        m_laneOrig.clear();
        m_laneOrig.reserve(std::size_t(m_selected.size()));
        // The revision-keyed id index makes capture proportional to the K
        // selected notes after its first build, not to every note in the clip.
        for (const QString& id : m_selected) {
            if (const auto* selected = note(id))
                m_laneOrig.push_back(*selected);
        }
        update();
        return;
    }

    // ── The keyboard ──
    if (pos.x() < keyboardWidth()) {
        // Press the key: lit, and sounded on the track's instrument for as long
        // as the button is held.
        m_pressedKey = yToPitch(pos.y());
        auditionPitch(m_pressedKey);
        update();
        return;
    }

    bool onEdge = false;
    const QString hit = noteAt(pos, &onEdge);

    switch (activeTool()) {
        case Tool::Slice:
            sliceAt(pos, additive);
            return;
        case Tool::Mute:
            if (!hit.isEmpty()) {
                const auto* n = note(hit);
                m_muting = true;
                m_mutingTo = n ? !n->muted : true;
                muteAt(pos, m_mutingTo);
            }
            return;
        case Tool::Draw:
        case Tool::Select:
            break;
    }

    // The stretch handle at the selection's right end: grabbing it stretches
    // the whole selection, snapping to the grid by default.
    if (onStretchHandle(pos)) {
        const auto* c = clip();
        if (c && !m_selected.isEmpty()) {
            m_primary = *m_selected.constBegin();
            beginStretch(ev);
            return;
        }
    }

    if (!hit.isEmpty()) {
        const auto* n = note(hit);
        if (!n) return;

        if (additive) {
            toggleSelected(hit);
            update();
            return;                 // shift-click selects, it does not drag
        }
        // Clicking a note that is already part of a multi-selection keeps the
        // selection, so a group can be dragged; clicking elsewhere replaces it.
        if (!m_selected.contains(hit)) selectOnly(hit);
        m_primary = hit;
        m_grabBeats = xToBeats(pos.x()) - n->startBeats;
        // The left edge resizes from the head, keeping the note's end put.
        const bool onLeftEdge = pos.x() <= noteRect(*n).left() + kEdgePx;
        m_resizing = onEdge || onLeftEdge;
        m_resizingLeft = onLeftEdge && !onEdge;
        m_moving = !m_resizing;
        m_resizeOrig.clear();
        m_moveWorking.clear();
        if (m_resizing || m_moving) {
            auto& snapshot = m_resizing ? m_resizeOrig : m_moveWorking;
            snapshot.reserve(std::size_t(m_selected.size()));
            for (const QString& id : m_selected) {
                if (const auto* selected = note(id))
                    snapshot.push_back(*selected);
            }
        }
        if (m_resizing) {
            m_geometryPaintNotes = m_resizeOrig;
            m_resizeGrabBeats =
                m_resizingLeft ? n->startBeats
                               : n->startBeats + n->lengthBeats;
        }
        update();
        return;
    }

    // Empty grid. In Select mode a drag rubber-bands; in Draw mode it draws,
    // and Ctrl/Cmd rubber-bands instead.
    const bool marquee = activeTool() == Tool::Select ||
                         (ev->modifiers() & Qt::ControlModifier);
    if (marquee) {
        m_marquee = true;
        m_marqueeOrigin = pos;
        m_marqueeCurrent = pos;
        m_selected.clear();
        m_primary.clear();
        emit selectionChanged();
        update();
        return;
    }

    // Draw: drop a note and go straight into resizing it, so a bare click
    // places one of the last-used length and a click-drag draws it to size.
    const double drawLength =
        std::max({m_lastLength, effectiveGridBeats(), kMinNoteBeats});
    const std::string noteId = m_controller->addNote(
        m_trackId.toStdString(), m_clipId.toStdString(),
        snapPitch(yToPitch(pos.y())), snapBeats(xToBeats(pos.x()), snapOn),
        drawLength);
    if (noteId.empty()) return;
    invalidateSoundingPitchIndex();
    // Note: `clip()` and any NoteModel* taken before this call are now stale —
    // addNote pushes into the vector and can reallocate it.
    selectOnly(QString::fromStdString(noteId));
    m_lastLength = drawLength;
    m_drawing = true;
    m_drawPress = pos;
    m_resizing = true;
    m_resizingLeft = false;
    m_moving = false;
    if (const auto* drawn = note(QString::fromStdString(noteId))) {
        m_resizeOrig = {*drawn};
        m_geometryPaintNotes = m_resizeOrig;
        m_resizeGrabBeats = drawn->startBeats + drawn->lengthBeats;
    }
    emit edited();
    update();
}

void PianoRollView::mouseMoveEvent(QMouseEvent* ev) {
    const QPointF pos = ev->position();
    m_pointer = pos;
    m_pointerInside = true;
    if (m_loopGrab != LoopGrab::None) {
        const bool snapping = m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        const double at = std::max(0.0, snapBeats(xToBeats(pos.x()), snapping));
        if (m_loopGrab == LoopGrab::Move) {
            const double from = std::max(0.0, at - m_loopGrabOffset);
            m_controller->setLoopRangeSeconds(
                localBeatToSeconds(from),
                localBeatToSeconds(from + m_loopGrabLength));
        } else {
            m_controller->setLoopRangeSeconds(
                localBeatToSeconds(std::min(m_loopAnchorBeats, at)),
                localBeatToSeconds(std::max(m_loopAnchorBeats, at)));
        }
        if (m_controller->loopEndSeconds() >
            m_controller->loopStartSeconds()) {
            m_controller->setLoopEnabled(true);
        }
        update();
        return;
    }

    if (m_scrubbingPlayhead) {
        seekToLocalBeat(xToBeats(pos.x()));
        return;
    }
    const bool snapOn = m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
    if (m_erasing) {
        m_eraseChanged |= eraseStroke(m_lastErasePoint, pos);
        m_lastErasePoint = pos;
        return;
    }
    if (m_muting) {
        muteAt(pos, m_mutingTo);
        return;
    }

    if (m_laneResizing) {
        setLaneHeight(m_laneResizeGrab - pos.y());
        return;
    }
    if (m_pressedKey >= 0 && pos.x() < keyboardWidth()) {
        // Sliding down the keys plays them in turn, as on a real one.
        const int pitch = yToPitch(pos.y());
        if (pitch != m_pressedKey) {
            m_pressedKey = pitch;
            auditionPitch(pitch);
            update();
        }
        return;
    }
    if (m_lanePointDrag >= 0) {
        queueControllerLanePoint(pos, snapOn);
        return;
    }

    if (m_marquee) {
        m_marqueeCurrent = pos;
        const QRectF box = QRectF(m_marqueeOrigin, m_marqueeCurrent).normalized();
        m_selected.clear();
        if (const auto* c = clip()) {
            const auto& index = notePaintIndexFor(c->notes);
            const double minimumPaintBeats =
                3.0 / std::max(1.0, pxPerBeat());
            index.forEachVisible(
                c->notes, xToBeats(box.left()) - minimumPaintBeats,
                xToBeats(box.right()) +
                    1.0 / std::max(1.0, pxPerBeat()),
                [&](const daw::NoteModel& n, std::size_t) {
                    if (noteRect(n).intersects(box))
                        m_selected.insert(QString::fromStdString(n.id));
                });
        }
        emit selectionChanged();
        update();
        return;
    }

    if (m_laneDragging) {
        // Delta, not absolute: a group keeps its relative dynamics — or its
        // relative stereo spread, when the lane is on pan.
        const double delta = laneValueAtY(pos.y()) - m_laneGrab;
        m_noteUpdateScratch.clear();
        m_noteUpdateScratch.reserve(m_laneOrig.size());
        for (const auto& original : m_laneOrig) {
            daw::NoteModel next = original;
            const double value = std::clamp(
                laneValueOf(original) + delta, 0.0, 1.0);
            if (m_laneParam == LaneParam::Velocity) {
                next.velocity = int(std::lround(value * 127.0));
            } else if (m_laneParam == LaneParam::Pan) {
                next.pan = float(value * 2.0 - 1.0);
            }
            m_noteUpdateScratch.push_back(std::move(next));
        }
        m_controller->setNoteStates(m_trackId.toStdString(),
                                    m_clipId.toStdString(), m_noteUpdateScratch);
        update();
        return;
    }

    if (m_stretching) {
        updateStretch(xToBeats(pos.x()), stretchSnapEnabled());
        return;
    }

    if (!m_moving && !m_resizing) {
        updateCursor(pos);
        if (activeTool() == Tool::Slice) update();   // the blade line follows
        return;
    }
    const daw::NoteModel* n = nullptr;
    if (m_moving || m_resizing) {
        const auto& working = m_moving ? m_moveWorking : m_resizeOrig;
        const std::string primaryId = m_primary.toStdString();
        const auto found = std::find_if(
            working.begin(), working.end(),
            [&](const daw::NoteModel& candidate) {
                return candidate.id == primaryId;
            });
        if (found != working.end()) n = &*found;
    } else {
        n = note(m_primary);
    }
    if (!n) return;

    if (m_resizing) {
        if (m_drawing &&
            std::abs(pos.x() - m_drawPress.x()) < kDrawResizeThresholdPx) {
            return;
        }
        const bool drawingResize = m_drawing;
        m_drawing = false;
        if (!drawingResize && !m_resizeOrig.empty()) {
            // A note edge is trim, even for a multi-selection. Every selected
            // note receives the same edge delta; starts and spacing stay put
            // for a right-edge trim, while a left-edge trim keeps each tail.
            const double target = snapBeats(xToBeats(pos.x()), snapOn);
            const double delta = target - m_resizeGrabBeats;
            m_noteUpdateScratch.clear();
            m_noteUpdateScratch.reserve(m_resizeOrig.size());
            for (const auto& original : m_resizeOrig) {
                daw::NoteModel next = original;
                double start = next.startBeats;
                double length = next.lengthBeats;
                if (m_resizingLeft) {
                    const double end = original.startBeats + original.lengthBeats;
                    start = std::clamp(original.startBeats + delta, 0.0,
                                       std::max(0.0, end - kMinNoteBeats));
                    length = std::max(kMinNoteBeats, end - start);
                } else {
                    length = std::max(kMinNoteBeats,
                                      original.lengthBeats + delta);
                }
                next.startBeats = start;
                next.lengthBeats = length;
                m_noteUpdateScratch.push_back(std::move(next));
                if (original.id == m_primary.toStdString() &&
                    !m_resizingLeft) {
                    m_lastLength = length;
                }
            }
            m_controller->setNoteStates(m_trackId.toStdString(),
                                        m_clipId.toStdString(),
                                        m_noteUpdateScratch);
            m_geometryPaintNotes = m_noteUpdateScratch;
        } else if (m_resizingLeft) {
            // Dragging the head: the tail stays exactly where it is.
            const double end = n->startBeats + n->lengthBeats;
            const double start = std::min(snapBeats(xToBeats(pos.x()), snapOn),
                                          end - kMinNoteBeats);
            m_controller->setNote(m_trackId.toStdString(), m_clipId.toStdString(),
                                  m_primary.toStdString(), n->pitch, start,
                                  end - start);
            m_geometryPaintNotes = {*n};
            m_geometryPaintNotes.front().startBeats = start;
            m_geometryPaintNotes.front().lengthBeats = end - start;
        } else {
            const double end = snapBeats(xToBeats(pos.x()), snapOn);
            const double minimum = drawingResize
                                       ? std::max(kMinNoteBeats,
                                                  effectiveGridBeats())
                                       : kMinNoteBeats;
            const double length = std::max(minimum, end - n->startBeats);
            m_controller->setNote(m_trackId.toStdString(), m_clipId.toStdString(),
                                  m_primary.toStdString(), n->pitch, n->startBeats,
                                  length);
            m_geometryPaintNotes = {*n};
            m_geometryPaintNotes.front().lengthBeats = length;
            m_lastLength = length;
        }
    } else {
        // The whole selection travels with the grabbed note.
        const double start = snapBeats(xToBeats(pos.x()) - m_grabBeats, snapOn);
        const int pitch = snapPitch(yToPitch(pos.y()));
        const double beatDelta = start - n->startBeats;
        const int pitchDelta = pitch - n->pitch;
        for (auto& next : m_moveWorking) {
            next.pitch = std::clamp(next.pitch + pitchDelta,
                                    kMinPitch, kMaxPitch);
            next.startBeats = std::max(0.0, next.startBeats + beatDelta);
        }
        m_controller->setNoteStates(m_trackId.toStdString(),
                                    m_clipId.toStdString(), m_moveWorking);
    }
    invalidateSoundingPitchIndex();
    update();
}

void PianoRollView::mouseReleaseEvent(QMouseEvent* ev) {
    if (m_loopGrab != LoopGrab::None) {
        m_loopGrab = LoopGrab::None;
        if (m_controller->loopEndSeconds() <= m_controller->loopStartSeconds()) {
            m_controller->setLoopRangeSeconds(0.0, 0.0);
            m_controller->setLoopEnabled(false);
        } else {
            m_controller->setLoopEnabled(true);
        }
        // A valid range is already armed by the drag; a click without travel
        // leaves no invisible cycle behind.
        updateCursor(ev->position());
        emit loopRangeChanged();
        update();
        ev->accept();
        return;
    }
    if (m_scrubbingPlayhead) {
        seekToLocalBeat(xToBeats(ev->position().x()));
        m_scrubbingPlayhead = false;
        updateCursor(ev->position());
        ev->accept();
        return;
    }
    // One signal per gesture, not per move: the moves themselves are live edits.
    const bool changed =
        m_moving || m_resizing || m_laneDragging || m_eraseChanged || m_muting ||
        m_stretching;
    if (m_lanePointDrag >= 0 || !m_lanePointsBefore.empty()) {
        // Mouse systems may deliver the release at a position for which no final
        // move event was sent. Fold that exact endpoint into the working vector,
        // then synchronously flush it before history snapshots the result.
        const bool snapOn =
            m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        queueControllerLanePoint(ev->position(), snapOn);
        flushControllerLaneWrite();
        // The curve was edited live; this is where the whole gesture becomes
        // one undo entry.
        m_controller->commitLaneEdit(m_laneGestureTrackId.toStdString(),
                                     m_laneGestureClipId.toStdString(),
                                     m_laneGestureLaneId.toStdString(),
                                     m_lanePointsBefore, "Edit Controller Lane");
        m_lanePointsBefore.clear();
        cancelControllerLaneWrite();
        emit edited();
    }
    if (m_stretching) {
        commitStretch();
        m_stretching = false;
        m_stretchOrig.clear();
        m_stretchPreview.clear();
    }
    if (m_erasing) commitPendingErase();
    if (m_gestureUndoActive) {
        m_controller->endNoteEdit(m_eraseChanged ? "Erase Notes"
                                                 : "Edit Notes");
        m_gestureUndoActive = false;
    }
    m_moving = false;
    m_resizing = false;
    m_resizingLeft = false;
    m_resizeOrig.clear();
    m_moveWorking.clear();
    m_geometryPaintNotes.clear();
    m_laneDragging = false;
    m_marquee = false;
    m_erasing = false;
    m_eraseChanged = false;
    m_drawing = false;
    m_muting = false;
    m_laneResizing = false;
    m_lanePointDrag = -1;
    if (m_pressedKey >= 0) {
        m_pressedKey = -1;
        stopAudition();
        update();
    }
    m_laneOrig.clear();
    if (m_soundingPitchInvalidationDeferred)
        invalidateSoundingPitchIndex();
    if (changed) emit edited();
    emitStatus();
    update();
    updateCursor(ev->position());
}

// ── Stretch ─────────────────────────────────────────────────────────────────
//
// Stretching scales the whole selection in time from its left boundary. It is
// deliberately reachable only through the double-headed handle to the right
// of a multi-selection; note edges always trim lengths instead.
//
// Snap follows the modifiers: Alt snaps to the grid, Alt+Shift is free, and a
// plain handle drag follows the snap setting.
//
// Unlike a move or resize this is not a live edit. The originals stay where
// they are and a phantom outline shows the result, so the gesture reads as one
// deliberate operation and commits as a single undo entry on release.

void PianoRollView::beginStretch(const QMouseEvent* ev) {
    const auto* c = clip();
    if (!c || m_selected.size() < 2) return;
    m_stretchOrig.clear();
    m_stretchOrig.reserve(std::size_t(m_selected.size()));
    for (const QString& id : m_selected) {
        if (const auto* selected = note(id))
            m_stretchOrig.push_back(*selected);
    }
    if (m_stretchOrig.empty()) return;

    // The selection's span, from the leftmost start to the rightmost end.
    double selStart = std::numeric_limits<double>::infinity();
    double selEnd = -std::numeric_limits<double>::infinity();
    for (const auto& n : m_stretchOrig) {
        selStart = std::min(selStart, n.startBeats);
        selEnd = std::max(selEnd, n.startBeats + n.lengthBeats);
    }
    m_stretchOrigSpan = std::max(1e-9, selEnd - selStart);

    m_stretching = true;
    m_stretchGrabBeats = xToBeats(ev->position().x());
    m_stretchScale = 1.0;
    m_stretchAnchorBeats = selStart;

    updateStretch(m_stretchGrabBeats, stretchSnapEnabled());
    emitStatus();
}

void PianoRollView::updateStretch(double currentBeats, bool snapOn) {
    if (m_stretchOrig.empty()) return;
    const double anchor = m_stretchAnchorBeats;
    const double grab = m_stretchGrabBeats;
    double scale = 1.0;
    if (std::abs(grab - anchor) > 1e-9) {
        scale = (currentBeats - anchor) / (grab - anchor);
    }
    // Keep the result sane: a scale of exactly zero would collapse every note
    // onto the anchor, and a runaway one would fling them off the clip.
    scale = std::clamp(scale, 0.01, 100.0);
    m_stretchScale = scale;

    m_stretchPreview.clear();
    m_stretchPreview.reserve(m_stretchOrig.size());
    for (const auto& n : m_stretchOrig) {
        daw::NoteModel out = n;
        double newStart = anchor + (n.startBeats - anchor) * scale;
        double newEnd =
            anchor + (n.startBeats + n.lengthBeats - anchor) * scale;
        if (snapOn) {
            newStart = snapBeats(newStart, true);
            newEnd = snapBeats(newEnd, true);
        }
        out.startBeats = newStart;
        out.lengthBeats = std::max(kMinNoteBeats, newEnd - newStart);
        m_stretchPreview.push_back(out);
    }
    update();
}

void PianoRollView::commitStretch() {
    if (m_stretchPreview.empty()) return;
    const auto* c = clip();
    if (!c) return;
    // Everything the stretch does not touch is carried through untouched, and
    // the scaled notes keep their ids so the selection survives the commit.
    mt::Notes untouched;
    for (const auto& n : c->notes) {
        if (!m_selected.contains(QString::fromStdString(n.id)))
            untouched.push_back(n);
    }
    mt::Notes merged = std::move(untouched);
    merged.insert(merged.end(), m_stretchPreview.begin(), m_stretchPreview.end());
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               merged, "Stretch Notes");
    invalidateSoundingPitchIndex();
    emit edited();
    emitStatus();
    update();
}

bool PianoRollView::stretchSnapEnabled() const {
    const auto mods = QApplication::keyboardModifiers();
    const bool alt = mods & Qt::AltModifier;
    const bool shift = mods & Qt::ShiftModifier;
    if (alt && shift) return false;   // Alt+Shift = free stretch, no grid
    if (alt) return true;             // Alt = snap to the current grid
    return m_snapEnabled;             // plain handle = follow the setting
}

QRectF PianoRollView::stretchHandleRect() const {
    if (m_selected.size() < 2) return QRectF();
    if (!clip()) return QRectF();
    // The selection's span: leftmost start to rightmost end, and the vertical
    // middle of the highest and lowest selected notes.
    double selStart = std::numeric_limits<double>::infinity();
    double selEnd = -std::numeric_limits<double>::infinity();
    int topPitch = -1;
    int bottomPitch = 128;
    for (const QString& id : m_selected) {
        const auto* selected = note(id);
        if (!selected) continue;
        selStart = std::min(selStart, selected->startBeats);
        selEnd = std::max(selEnd,
                          selected->startBeats + selected->lengthBeats);
        topPitch = std::max(topPitch, selected->pitch);
        bottomPitch = std::min(bottomPitch, selected->pitch);
    }
    if (selEnd < selStart) return QRectF();
    const double selectionRight = beatsToX(selEnd);
    const double yTop = pitchToY(topPitch);
    const double yBottom = pitchToY(bottomPitch) + m_rowHeight;
    const double cy = (yTop + yBottom) / 2.0;
    const double w = 24.0;
    const double h = 24.0;
    // A visible gap separates the group operation from the ordinary resize
    // edge of the rightmost note, so the two gestures cannot be confused.
    return QRectF(selectionRight + 8.0, cy - h / 2.0, w, h);
}

bool PianoRollView::onStretchHandle(const QPointF& pos) const {
    const QRectF r = stretchHandleRect();
    return !r.isNull() && r.contains(pos);
}

void PianoRollView::wheelEvent(QWheelEvent* ev) {
    const auto modifiers = ev->modifiers();
    // A trackpad reports the actual distance the fingers travelled; a mouse
    // wheel only reports notches. Preferring pixels is what makes two-finger
    // scrolling smooth and, crucially, two-dimensional — the previous code read
    // `angleDelta().y()` only, so a trackpad could not scroll sideways at all.
    const QPoint pixels = ev->pixelDelta();
    const QPoint notches = ev->angleDelta();
    const bool fine = !pixels.isNull();
    const double dx = fine ? pixels.x() : notches.x();
    const double dy = fine ? pixels.y() : notches.y();

    // Ctrl/Cmd zooms horizontally, Alt vertically — the two axes are
    // independent, because a dense chord voicing and a long phrase need
    // different things from the same clip.
    if (modifiers & Qt::ControlModifier) {
        // Pixel deltas are far smaller than a notch, so the exponent keeps a
        // pinch-less trackpad zoom moving at the same rate as a wheel.
        zoomHorizontal(std::pow(1.0015, fine ? dy : dy * 0.9));
        ev->accept();
        return;
    }
    if (modifiers & Qt::AltModifier) {
        zoomVertical(std::pow(1.0015, fine ? dy : dy * 0.9));
        ev->accept();
        return;
    }
    // Over the parameter lane the wheel edits the selection's value — the one
    // gesture that changes several notes at once.
    if (m_showVelocityLane && ev->position().y() >= laneTop() &&
        !m_selected.isEmpty() && m_laneParam != LaneParam::Controller) {
        m_wheelAccum += int(std::lround(fine ? dy * 4.0 : dy));
        const int steps = m_wheelAccum / kWheelPerStep;
        if (steps != 0) {
            m_wheelAccum -= steps * kWheelPerStep;
            if (m_laneParam == LaneParam::Velocity) {
                bumpSelectedVelocity(steps);
            } else {
                bumpSelectedPan(steps);
            }
        }
        ev->accept();
        return;
    }

    // Shift is the mouse-wheel way of asking for horizontal scroll; a trackpad
    // just reports the sideways component and needs no modifier.
    if (modifiers & Qt::ShiftModifier) {
        setScrollX(m_scrollX - dy - dx);
    } else {
        if (dx != 0.0) setScrollX(m_scrollX - dx);
        m_scrollY -= dy;
        clampScroll();
    }
    emit viewportChanged();
    update();
    ev->accept();
}

bool PianoRollView::event(QEvent* ev) {
    if (ev->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(ev);
        if (isPianoRollEditShortcut(key)) {
            // The main arrangement owns the same chords. Claim them at the
            // focused note canvas so Qt never resolves Cmd/Ctrl against the
            // wrong window or drops the press as an ambiguous shortcut.
            key->accept();
            return true;
        }
    }

    // Pinch and smart-zoom arrive as native gestures, not as wheel events with
    // a modifier, so they have to be picked up here or the trackpad has no way
    // to zoom at all.
    if (ev->type() == QEvent::NativeGesture) {
        auto* gesture = static_cast<QNativeGestureEvent*>(ev);
        switch (gesture->gestureType()) {
            case Qt::ZoomNativeGesture: {
                // `value` is the fractional change since the last event.
                const double factor = 1.0 + gesture->value();
                m_pointer = gesture->position();
                m_pointerInside = true;
                // Pinching zooms time; holding Shift pinches the pitch axis,
                // since the two are independent here.
                if (gesture->modifiers() & Qt::ShiftModifier) zoomVertical(factor);
                else zoomHorizontal(factor);
                ev->accept();
                return true;
            }
            case Qt::SmartZoomNativeGesture:
                // Two-finger double tap: the "show me everything" gesture.
                zoomToFit();
                ev->accept();
                return true;
            default:
                break;
        }
    }
    return QWidget::event(ev);
}

void PianoRollView::keyPressEvent(QKeyEvent* ev) {
    finishWheelNoteEdit();
    if (isPianoRollEditShortcut(ev)) {
        switch (editShortcutKey(ev)) {
            case Qt::Key_X: cutSelection(); break;
            case Qt::Key_C: copySelection(); break;
            case Qt::Key_V: paste(); break;
            case Qt::Key_B: duplicateSelection(); break;
            default: break;
        }
        ev->accept();
        return;
    }
    // Holding S or T borrows the tool for as long as the key is down, which is
    // how one note gets sliced or muted without ever leaving Draw.
    if (!ev->isAutoRepeat() && !(ev->modifiers() & ~Qt::KeypadModifier)) {
        if (ev->key() == Qt::Key_S && m_tool != Tool::Slice) {
            m_heldTool = Tool::Slice;
            updateCursor(m_pointer);
            emitStatus();
            update();
            return;
        }
        if (ev->key() == Qt::Key_T && m_tool != Tool::Mute) {
            m_heldTool = Tool::Mute;
            updateCursor(m_pointer);
            emitStatus();
            update();
            return;
        }
    }
    if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
        deleteSelection();
        return;
    }
    QWidget::keyPressEvent(ev);
}

void PianoRollView::keyReleaseEvent(QKeyEvent* ev) {
    if (!ev->isAutoRepeat() && m_heldTool &&
        (ev->key() == Qt::Key_S || ev->key() == Qt::Key_T)) {
        m_heldTool.reset();
        updateCursor(m_pointer);
        emitStatus();
        update();
        return;
    }
    QWidget::keyReleaseEvent(ev);
}

void PianoRollView::contextMenuEvent(QContextMenuEvent* ev) {
    if (m_suppressContextMenu) {
        // The same right-press armed the eraser; a menu on top of that would be
        // the second thing one button did, and it would land under the pointer
        // exactly where the sweep is about to go.
        m_suppressContextMenu = false;
        ev->accept();
        return;
    }
    ev->ignore();
}

void PianoRollView::leaveEvent(QEvent*) {
    m_pointerInside = false;
    unsetCursor();
    update();
}

// ── Hit testing ─────────────────────────────────────────────────────────────

QString PianoRollView::noteAt(const QPointF& pos, bool* onRightEdge,
                              bool* onLeftEdge) const {
    if (onRightEdge) *onRightEdge = false;
    if (onLeftEdge) *onLeftEdge = false;
    const auto* c = clip();
    if (!c) return {};
    const auto& index = notePaintIndexFor(c->notes);
    const double beat = xToBeats(pos.x());
    const double minimumPaintBeats = 3.0 / std::max(1.0, pxPerBeat());
    std::size_t bestIndex = 0;
    const daw::NoteModel* best = nullptr;
    // Choose the highest document index, exactly matching the former reverse
    // scan and therefore the visual stacking order for overlapping notes.
    index.forEachVisible(
        c->notes, beat - minimumPaintBeats,
        beat + 1.0 / std::max(1.0, pxPerBeat()),
        [&](const daw::NoteModel& candidate, std::size_t candidateIndex) {
            if (best && candidateIndex <= bestIndex) return;
            if (!noteRect(candidate).contains(pos)) return;
            best = &candidate;
            bestIndex = candidateIndex;
        });
    if (!best) return {};
    const QRectF r = noteRect(*best);
    if (onRightEdge) *onRightEdge = pos.x() >= r.right() - kEdgePx;
    if (onLeftEdge) *onLeftEdge = pos.x() <= r.left() + kEdgePx;
    return QString::fromStdString(best->id);
}

QString PianoRollView::handleAt(const QPointF& pos) const {
    const auto* c = clip();
    if (!c) return {};
    // Nearest stalk horizontally: the lane is a column per note, so anywhere in
    // the column grabs it rather than only the circle itself.
    const auto& index = notePaintIndexFor(c->notes);
    std::size_t bestIndex = 0;
    bool bestSelected = false;
    QString best;
    double bestDistance = kHandleGrabPx;
    const double beat = xToBeats(pos.x());
    const double beatMargin = kHandleGrabPx / std::max(1.0, pxPerBeat());
    index.forEachVisible(c->notes, beat - beatMargin,
                         beat + beatMargin +
                             1.0 / std::max(1.0, pxPerBeat()),
                         [&](const daw::NoteModel& n,
                             std::size_t candidateIndex) {
        const double distance = std::abs(beatsToX(n.startBeats) - pos.x());
        if (distance > bestDistance) return;
        // On a tie prefer a selected note, so a stack of unisons stays editable.
        const bool selected = m_selected.contains(QString::fromStdString(n.id));
        const bool closer = distance < bestDistance;
        const bool tiedSelected =
            distance == bestDistance && selected &&
            (!bestSelected || candidateIndex > bestIndex);
        if (closer || tiedSelected) {
            bestDistance = distance;
            best = QString::fromStdString(n.id);
            bestIndex = candidateIndex;
            bestSelected = selected;
        }
    });
    return best;
}

void PianoRollView::updateCursor(const QPointF& pos) {
    if (!clip()) {
        unsetCursor();
        return;
    }
    // While the right button is down the pointer *is* an eraser, whatever tool
    // is selected — that is the one gesture that ignores the tool entirely.
    if (m_erasing) {
        setCursor(toolCursor(icons::Glyph::Eraser));
        return;
    }
    if (pos.y() < ui::kLoopStripHeight && pos.x() >= keyboardWidth()) {
        setCursor(loopGrabAt(pos.x()) == LoopGrab::Move ? Qt::OpenHandCursor
                                                        : Qt::SizeHorCursor);
        return;
    }
    if (pos.y() < ui::kRulerHeight) {
        setCursor(pos.x() >= keyboardWidth() ? Qt::SizeHorCursor
                                             : Qt::ArrowCursor);
        return;
    }
    if (m_showVelocityLane && std::abs(pos.y() - laneTop()) <= kLaneGripPx) {
        setCursor(Qt::SizeVerCursor);
        return;
    }
    if (pos.x() < keyboardWidth()) {
        setCursor(m_showKeyboard ? Qt::PointingHandCursor : Qt::ArrowCursor);
        return;
    }
    if (m_showVelocityLane && pos.y() >= laneTop()) {
        if (m_laneParam == LaneParam::Controller) {
            setCursor(lanePointAt(pos) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
            return;
        }
        setCursor(handleAt(pos).isEmpty() ? Qt::ArrowCursor : Qt::SizeVerCursor);
        return;
    }
    const Tool currentTool = activeTool();
    switch (currentTool) {
        case Tool::Slice:
            setCursor(toolCursor(icons::Glyph::Knife));
            return;
        case Tool::Mute:
            setCursor(toolCursor(icons::Glyph::NoteMute));
            return;
        case Tool::Select:
        case Tool::Draw:
            break;
    }

    // The dedicated group-stretch arrow sits in empty grid space, so it must be
    // tested before note hit-testing. Its cursor now matches the only gesture
    // that can actually start proportional scaling.
    if (onStretchHandle(pos)) {
        setCursor(Qt::SizeHorCursor);
        return;
    }

    bool onEdge = false;
    bool onLeftEdge = false;
    const QString hit = noteAt(pos, &onEdge, &onLeftEdge);
    if (hit.isEmpty()) {
        setCursor(toolCursor(currentTool == Tool::Draw ? icons::Glyph::Brush
                                                       : icons::Glyph::Pointer));
        return;
    }
    // A note edge always means trim. Proportional scaling has its own visible
    // handle above, so the two gestures never share the same hit target.
    if (onEdge || onLeftEdge) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    setCursor(currentTool == Tool::Draw ? Qt::OpenHandCursor
                                        : toolCursor(icons::Glyph::Pointer));
}

// ── Tool gestures ───────────────────────────────────────────────────────────

bool PianoRollView::eraseStroke(const QPointF& from, const QPointF& to) {
    const auto* c = clip();
    if (!c) return false;

    // Keep the document vector untouched throughout the sweep. The paint path
    // hides pending ids immediately, while every mouse sample reuses this same
    // stable index; release performs one structural mutation and one undo step.
    const auto& index = notePaintIndexFor(c->notes);
    const double minimumPaintBeats = 3.0 / std::max(1.0, pxPerBeat());
    const double fromBeat = xToBeats(std::min(from.x(), to.x()));
    const double toBeat = xToBeats(std::max(from.x(), to.x()));
    bool newHit = false;
    index.forEachVisible(
        c->notes, fromBeat - minimumPaintBeats,
        toBeat + 1.0 / std::max(1.0, pxPerBeat()),
        [&](const daw::NoteModel& candidate, std::size_t) {
            if (!segmentCrossesRect(from, to, noteRect(candidate))) return;
            const QString id = QString::fromStdString(candidate.id);
            if (m_pendingErase.contains(id)) return;
            m_pendingErase.insert(id);
            m_selected.remove(id);
            if (m_primary == id) m_primary.clear();
            newHit = true;
        });
    if (!newHit) return false;
    emit selectionChanged();
    update();
    return true;
}

bool PianoRollView::commitPendingErase() {
    if (m_pendingErase.isEmpty() || !m_controller) return false;
    std::vector<std::string> ids;
    ids.reserve(std::size_t(m_pendingErase.size()));
    for (const QString& id : m_pendingErase) ids.push_back(id.toStdString());
    m_controller->removeNotes(m_trackId.toStdString(),
                              m_clipId.toStdString(), ids);
    m_pendingErase.clear();
    invalidateSoundingPitchIndex();
    return true;
}

void PianoRollView::sliceAt(const QPointF& pos, bool acrossAllNotes) {
    const auto* c = clip();
    if (!c) return;
    const double at = snapBeats(xToBeats(pos.x()), m_snapEnabled);

    bool onEdge = false;
    const QString hit = noteAt(pos, &onEdge);
    if (hit.isEmpty() && !acrossAllNotes) return;

    mt::Notes result;
    for (const auto& n : c->notes) {
        const bool cutThis =
            acrossAllNotes || QString::fromStdString(n.id) == hit;
        if (!cutThis) {
            result.push_back(n);
            continue;
        }
        for (auto& piece : mt::splitAt({n}, at)) {
            if (piece.id.empty()) piece.id = daw::newUuid();
            result.push_back(piece);
        }
    }
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               result, "Slice Notes");
    invalidateSoundingPitchIndex();
    emit edited();
    update();
}

void PianoRollView::muteAt(const QPointF& pos, bool muted) {
    bool onEdge = false;
    const QString hit = noteAt(pos, &onEdge);
    if (hit.isEmpty()) return;
    const auto* n = note(hit);
    if (!n || n->muted == muted) return;
    m_controller->setNoteMuted(m_trackId.toStdString(), m_clipId.toStdString(),
                               hit.toStdString(), muted);
    invalidateSoundingPitchIndex();
    update();
}

void PianoRollView::followPlayhead() {
    const auto* c = clip();
    if (!c || !m_followPlayback || !m_controller->isPlaying()) return;
    const double beats = daw::secondsToBeats(
        m_controller->presentationPositionSeconds() - c->startSeconds,
        m_controller->project().tempo);
    const double x = beatsToX(beats);
    const double usable = double(width()) - keyboardWidth();
    if (x < keyboardWidth() || x > width() - usable * 0.15) {
        // Jump so the playhead sits a fifth of the way in, leaving most of the
        // view showing what is about to happen rather than what just did.
        setScrollX(beats * pxPerBeat() - usable * 0.2);
        emit viewportChanged();
    }
}

void PianoRollView::refreshPlayheadFrame() {
    if (!m_controller || !isVisible()) return;
    followPlayhead();

    int currentPixel = -1;
    if (const auto* c = clip()) {
        const double beats = daw::secondsToBeats(
            m_controller->presentationPositionSeconds() - c->startSeconds,
            m_controller->project().tempo);
        if (beats >= 0.0 && beats <= clipBeats()) {
            const double x = beatsToX(beats);
            if (x >= keyboardWidth() && x <= width())
                currentPixel = int(std::lround(x));
        }
    }

    QRegion dirty;
    const int playheadBottom = int(std::ceil(laneTop()));
    const auto addPlayhead = [&](int x) {
        if (x >= 0) dirty += QRect(x - 7, 0, 15, playheadBottom);
    };
    if (currentPixel != m_lastPlayheadPixel) {
        addPlayhead(m_lastPlayheadPixel);
        addPlayhead(currentPixel);
        m_lastPlayheadPixel = currentPixel;
    }

    const PitchMask sounding = keyboardPitches();
    if (sounding != m_lastSoundingPitches) {
        m_lastSoundingPitches = sounding;
        dirty += QRect(0, int(ui::kRulerHeight),
                       int(std::ceil(keyboardWidth())) + 2,
                       std::max(0, playheadBottom - int(ui::kRulerHeight)));
    }
    if (!dirty.isEmpty()) update(dirty);
}

void PianoRollView::setLivePitches(const PitchMask& pitches) {
    if (m_livePitches == pitches) return;
    m_livePitches = pitches;
    m_lastSoundingPitches = keyboardPitches();
    update(QRect(0, int(ui::kRulerHeight),
                 int(std::ceil(keyboardWidth())) + 2,
                 std::max(0, int(std::ceil(laneTop())) -
                                 int(ui::kRulerHeight))));
}

void PianoRollView::auditionPitch(int pitch) {
    if (pitch == m_auditionPitch) return;
    stopAudition();
    if (pitch < 0 || pitch > 127 || m_trackId.isEmpty()) return;
    // The velocity a drawn note would get, so the click previews what writing
    // the note there would sound like.
    if (m_controller->liveNoteOn(m_trackId.toStdString(), pitch, 100))
        m_auditionPitch = pitch;
}

void PianoRollView::stopAudition() {
    if (m_auditionPitch < 0) return;
    m_controller->liveNoteOff(m_trackId.toStdString(), m_auditionPitch);
    m_auditionPitch = -1;
}

void PianoRollView::emitStatus() {
    QString toolName;
    switch (activeTool()) {
        case Tool::Draw:   toolName = tr("Draw"); break;
        case Tool::Select: toolName = tr("Select"); break;
        case Tool::Slice:  toolName = tr("Slice"); break;
        case Tool::Mute:   toolName = tr("Disable"); break;
    }
    QString text = tr("%1 · %2 notes selected").arg(toolName).arg(m_selected.size());
    if (m_preview) text = tr("%1 · previewing — press Apply to keep it").arg(toolName);
    emit statusChanged(text);
}

// ── PianoRollWindow ─────────────────────────────────────────────────────────

PianoRollWindow::PianoRollWindow(daw::EngineController* controller,
                                 QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setWindowTitle(tr("Piano Roll"));
    resize(1100, 640);

    m_toolPreviewTimer = new QTimer(this);
    m_toolPreviewTimer->setSingleShot(true);
    m_toolPreviewTimer->setInterval(kToolPreviewFrameMs);
    connect(m_toolPreviewTimer, &QTimer::timeout, this,
            &PianoRollWindow::runPendingToolPreview);

    m_view = new PianoRollView(m_controller, this);
    m_view->setGridBeats(ui::gridDivisions()[ui::kDefaultGridIndex].beats);
    const QString storedGrid = pianoRollPref("view.gridColor", QString()).toString();
    if (!storedGrid.isEmpty()) m_view->setGridColor(QColor(storedGrid));
    connect(m_view, &PianoRollView::edited, this, [this] {
        updateActionState();
        emit edited();
    });
    connect(m_view, &PianoRollView::selectionChanged, this, [this] {
        updateActionState();
        emit noteSelectionChanged(hasSelectedNotes());
    });
    connect(m_view, &PianoRollView::viewportChanged, this,
            &PianoRollWindow::updateScrollBars);
    connect(m_view, &PianoRollView::playheadMoved, this,
            &PianoRollWindow::playheadMoved);
    connect(m_view, &PianoRollView::loopRangeChanged, this,
            &PianoRollWindow::loopRangeChanged);

    auto* navigator = new PianoRollNavigator(this);
    navigator->bindZoom(
        [this] { return m_view->effectivePixelsPerBeat(); },
        [this](double pixels) { m_view->setHorizontalZoomFromStart(pixels); });
    m_hScroll = navigator;
    m_vScroll = new QScrollBar(Qt::Vertical, this);
    connect(m_hScroll, &QScrollBar::valueChanged, this, [this](int value) {
        m_view->setScrollX(double(value));
    });
    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int value) {
        m_view->setScrollY(double(value));
    });

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    buildMenus();
    buildToolbar();

    // A shortcut only fires while its action belongs to the focused editor. A
    // menu hanging off a toolbar button does not provide that association, so
    // every action is registered on this panel by hand. The scoped context is
    // important now that the roll and arrangement share MainWindow: clicking
    // the timeline must hand its shortcuts straight back to the timeline.
    std::function<void(QMenu*)> registerActions = [&](QMenu* menu) {
        for (QAction* action : menu->actions()) {
            if (QMenu* submenu = action->menu()) {
                registerActions(submenu);
            } else if (!action->shortcut().isEmpty()) {
                action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
                addAction(action);
            }
        }
    };
    for (QMenu* menu : {m_editMenu, m_viewMenu, m_toolsMenu, m_snapMenu}) {
        registerActions(menu);
    }

    column->addWidget(m_toolbar);

    // Navigation stays directly under the tools: the full clip can be panned
    // without travelling to the controller lane at the bottom. The spacer
    // matches the piano keyboard, so the scrollbar track starts exactly where
    // note time starts.
    auto* navigation = new QHBoxLayout;
    navigation->setContentsMargins(0, 0, 0, 0);
    navigation->setSpacing(6);
    navigation->addSpacing(int(kKeyboardWidth));
    navigation->addWidget(m_hScroll, 1);
    auto* rowHeight = new RowHeightScrubber(this);
    rowHeight->setObjectName(QStringLiteral("NoteHeightScrubber"));
    rowHeight->setIcon(
        icons::icon(icons::Glyph::ResizeVertical, th().textSecondary, 16));
    rowHeight->setToolTip(
        tr("Note height — drag up to make rows taller, down to make them thinner"));
    rowHeight->setAccessibleName(tr("Note height"));
    rowHeight->setAccessibleDescription(
        tr("Drag vertically to change the height of piano-roll note rows."));
    rowHeight->bind([this] { return m_view->rowHeight(); },
                    [this](double value) {
                        const double current = m_view->rowHeight();
                        if (current > 0.0) m_view->zoomVertical(value / current);
                    });
    navigation->addWidget(rowHeight);
    // Reserve the vertical scrollbar's column so both navigation axes line up
    // with the same note field rather than drifting by one control width.
    navigation->addSpacing(m_vScroll->sizeHint().width());
    column->addLayout(navigation);

    auto* grid = new QHBoxLayout;
    grid->setSpacing(0);
    grid->addWidget(m_view, 1);
    grid->addWidget(m_vScroll);
    column->addLayout(grid, 1);

    // The lane's parameter picker sits directly under the lane it drives,
    // beside the horizontal scrollbar, rather than up in the toolbar with the
    // things that act on notes.
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(6);
    m_laneSelector = new QComboBox(this);
    m_laneSelector->setToolTip(
        tr("What the lane along the bottom edits: note velocity, note pan, or a "
           "controller curve."));
    m_laneSelector->setMinimumWidth(150);
    connect(m_laneSelector, &QComboBox::currentIndexChanged, this,
            &PianoRollWindow::laneSelectionChanged);
    bottom->addWidget(m_laneSelector);
    auto* addLane = new QToolButton(this);
    addLane->setIcon(icons::icon(icons::Glyph::Plus, th().textPrimary, 16));
    addLane->setToolTip(tr("Add a controller lane"));
    addLane->setAutoRaise(true);
    connect(addLane, &QToolButton::clicked, this, &PianoRollWindow::addControllerLane);
    bottom->addWidget(addLane);
    m_removeLaneButton = new QToolButton(this);
    m_removeLaneButton->setIcon(icons::icon(icons::Glyph::Trash, th().textPrimary, 16));
    m_removeLaneButton->setToolTip(tr("Remove this controller lane"));
    m_removeLaneButton->setAutoRaise(true);
    connect(m_removeLaneButton, &QToolButton::clicked, this,
            &PianoRollWindow::removeControllerLane);
    bottom->addWidget(m_removeLaneButton);
    bottom->addStretch(1);
    column->addLayout(bottom);

    refreshLaneSelector();

    loadViewPreferences();
    updateActionState();
    // QAction shortcut resolution happens before a focused widget's
    // keyPressEvent and the arrangement owns the same chords. This scoped
    // application filter gives the active editor first refusal while leaving
    // every other window, popup and text field alone.
    if (qApp) qApp->installEventFilter(this);
}

PianoRollWindow::~PianoRollWindow() {
    cancelToolPreview();
    if (qApp) qApp->removeEventFilter(this);
}

NoteContextPanel* PianoRollWindow::createContextPanel(QWidget* host) {
    if (!host || !m_view) return nullptr;
    auto* panel = new NoteContextPanel(m_view, host);
    connect(panel, &NoteContextPanel::projectEdited, this, [this] {
        updateActionState();
        emit edited();
    });
    connect(panel, &NoteContextPanel::toolRequested, this,
            &PianoRollWindow::openToolFor);
    connect(m_view, &PianoRollView::selectionChanged, panel,
            &NoteContextPanel::refresh);
    connect(m_view, &PianoRollView::edited, panel,
            &NoteContextPanel::refresh);
    return panel;
}

bool PianoRollWindow::hasSelectedNotes() const {
    return m_view && m_view->selectionCount() > 0;
}

void PianoRollWindow::buildMenus() {
    // The four menus are built as free-standing QMenus and then hung off the
    // toolbar buttons: the settings pop-up gets Edit/View/Tools as submenus,
    // while Snap and the ghost list each get a button of their own, because
    // those two are reached constantly while writing a part and a submenu would
    // put two extra clicks on the most-used controls in the window.
    m_editMenu = new QMenu(tr("Edit"), this);
    buildEditMenu(m_editMenu);
    m_viewMenu = new QMenu(tr("View"), this);
    buildViewMenu(m_viewMenu);
    m_toolsMenu = new QMenu(tr("Tools"), this);
    buildToolsMenu(m_toolsMenu);
    m_snapMenu = new QMenu(tr("Snap"), this);
    buildSnapMenu(m_snapMenu);
}

namespace {

/// A flat icon button for the roll's toolbar.
QToolButton* toolbarButton(QWidget* parent, icons::Glyph glyph,
                           const QString& tip, bool checkable = false) {
    auto* button = new QToolButton(parent);
    button->setIcon(icons::icon(glyph, th().textPrimary, 18));
    button->setIconSize(QSize(18, 18));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setAutoRaise(true);
    button->setCheckable(checkable);
    button->setFocusPolicy(Qt::NoFocus);   // the grid keeps the keyboard focus
    return button;
}

} // namespace

void PianoRollWindow::buildToolbar() {
    m_toolbar = new QWidget(this);
    auto* row = new QHBoxLayout(m_toolbar);
    row->setContentsMargins(2, 2, 2, 2);
    row->setSpacing(4);

    // ── Settings: everything that isn't reached every minute ──
    auto* settings = toolbarButton(m_toolbar, icons::Glyph::Gear,
                                   tr("Edit, View and Tools"));
    auto* settingsMenu = new QMenu(m_toolbar);
    settingsMenu->addMenu(m_editMenu);
    settingsMenu->addMenu(m_viewMenu);
    settingsMenu->addMenu(m_toolsMenu);
    settings->setMenu(settingsMenu);
    settings->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(settings);

    // A hairline between groups of buttons. A lambda rather than one widget,
    // since each separator has to be its own instance in the layout.
    auto divider = [this] {
        auto* line = new QFrame(m_toolbar);
        line->setFrameShape(QFrame::VLine);
        line->setStyleSheet(QString("color: %1;").arg(th().separator().name()));
        return line;
    };
    row->addWidget(divider());

    // ── The tool palette ──
    m_toolButtons = new QButtonGroup(this);
    m_toolButtons->setExclusive(true);
    const std::tuple<PianoRollView::Tool, icons::Glyph, QString> palette[] = {
        {PianoRollView::Tool::Draw, icons::Glyph::Brush, tr("Draw (P)")},
        {PianoRollView::Tool::Select, icons::Glyph::Pointer, tr("Select (E)")},
        {PianoRollView::Tool::Slice, icons::Glyph::Knife,
         tr("Slice — hold S to borrow it for one cut")},
        {PianoRollView::Tool::Mute, icons::Glyph::Power,
         tr("Disable notes — hold T to borrow it for one note")},
    };
    for (const auto& [tool, glyph, tip] : palette) {
        QToolButton* button = toolbarButton(m_toolbar, glyph, tip, true);
        button->setChecked(tool == PianoRollView::Tool::Draw);
        m_toolButtons->addButton(button, int(tool));
        row->addWidget(button);
    }
    connect(m_toolButtons, &QButtonGroup::idClicked, this, [this](int id) {
        m_view->setTool(PianoRollView::Tool(id));
        setPianoRollPref("tool", id);
        syncToolActions();
    });
    row->addWidget(divider());

    // Chord building is a permanent creative command, not a property of the
    // current selection, so it stays pinned here instead of consuming space in
    // the floating context panel.
    auto* chords = toolbarButton(
        m_toolbar, icons::Glyph::Chord,
        tr("Build chords (%1)")
            .arg(m_chordAction->shortcut().toString(QKeySequence::NativeText)));
    chords->setObjectName(QStringLiteral("PianoRollBuildChordsButton"));
    connect(chords, &QToolButton::clicked, m_chordAction, &QAction::trigger);
    row->addWidget(chords);

    // These letter chips match the track headers and channel strips. The note
    // disable tool deliberately uses Power, so track mute and note disable are
    // visually distinct.
    m_trackMuteButton = new ui::MsrButton(
        tr("M"), Theme::mute(), tr("Mute this MIDI track"), m_toolbar);
    m_trackMuteButton->setObjectName(QStringLiteral("PianoRollTrackMute"));
    m_trackMuteButton->setAccessibleName(tr("Mute this MIDI track"));
    m_trackMuteButton->setAutomatable(true);
    connect(m_trackMuteButton, &ui::MsrButton::automateRequested, this, [this] {
        if (!m_trackId.isEmpty()) emit automateMuteRequested(m_trackId);
    });
    connect(m_trackMuteButton, &QAbstractButton::clicked, this, [this](bool on) {
        if (m_trackId.isEmpty()) return;
        const auto result =
            m_controller->setTrackMuted(m_trackId.toStdString(), on);
        updateActionState();
        emit trackStateChanged(daw::collab::marksLocalFileDirty(result));
    });
    row->addWidget(m_trackMuteButton);

    m_trackSoloButton = new ui::MsrButton(
        tr("S"), Theme::solo(), tr("Solo this MIDI track"), m_toolbar);
    m_trackSoloButton->setObjectName(QStringLiteral("PianoRollTrackSolo"));
    m_trackSoloButton->setAccessibleName(tr("Solo this MIDI track"));
    connect(m_trackSoloButton, &QAbstractButton::clicked, this, [this](bool on) {
        if (m_trackId.isEmpty()) return;
        m_controller->setTrackSoloed(m_trackId.toStdString(), on);
        updateActionState();
        emit trackStateChanged();
    });
    row->addWidget(m_trackSoloButton);
    row->addWidget(divider());

    // ── Snap, ghosts, note style ──
    auto* snap = toolbarButton(m_toolbar, icons::Glyph::Magnet, tr("Snap and grid"));
    snap->setMenu(m_snapMenu);
    snap->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(snap);

    auto* ghosts = toolbarButton(m_toolbar, icons::Glyph::Ghost,
                                 tr("Ghost notes from other MIDI tracks"));
    ghosts->setMenu(m_ghostMenu);
    ghosts->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(ghosts);

    auto* style = toolbarButton(m_toolbar, icons::Glyph::NoteStyle,
                                tr("How notes are drawn"));
    style->setMenu(m_noteStyleMenu);
    style->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(style);
    row->addWidget(divider());

    // ── Zoom ──
    auto* zoomOut = toolbarButton(m_toolbar, icons::Glyph::ZoomOut, tr("Zoom out"));
    connect(zoomOut, &QToolButton::clicked, this,
            [this] { m_view->zoomHorizontal(1.0 / 1.3); });
    row->addWidget(zoomOut);
    auto* zoomIn = toolbarButton(m_toolbar, icons::Glyph::ZoomIn, tr("Zoom in"));
    connect(zoomIn, &QToolButton::clicked, this,
            [this] { m_view->zoomHorizontal(1.3); });
    row->addWidget(zoomIn);
    auto* zoomFit = toolbarButton(m_toolbar, icons::Glyph::ZoomFit,
                                  tr("Fit the clip to the window"));
    connect(zoomFit, &QToolButton::clicked, this, [this] { m_view->zoomToFit(); });
    row->addWidget(zoomFit);

    row->addStretch(1);
}

void PianoRollWindow::syncToolActions() {
    // The palette and the Tools menu are two faces of one setting, so whichever
    // was used, the other has to follow.
    const int current = int(m_view->tool());
    if (auto* button = m_toolButtons->button(current)) {
        const QSignalBlocker block(m_toolButtons);
        button->setChecked(true);
    }
    if (m_toolGroup) {
        const auto actions = m_toolGroup->actions();
        if (current >= 0 && current < actions.size()) {
            actions[current]->setChecked(true);
        }
    }
}

void PianoRollWindow::refreshLaneSelector() {
    if (!m_laneSelector) return;
    const QSignalBlocker block(m_laneSelector);
    m_laneSelector->clear();
    // Velocity and pan live on the notes themselves; everything after the
    // separator is a curve stored on the clip.
    m_laneSelector->addItem(tr("Velocity"), QString());
    m_laneSelector->addItem(tr("Pan"), QString());

    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    const daw::ClipModel* clip = nullptr;
    if (track) {
        for (const auto& c : track->clips) {
            if (c.id == m_clipId.toStdString()) clip = &c;
        }
    }
    if (clip) {
        for (const auto& lane : clip->lanes) {
            const QString label =
                lane.cc >= 0
                    ? tr("%1 (CC %2)")
                          .arg(QString::fromStdString(lane.name)).arg(lane.cc)
                    : QString::fromStdString(lane.name);
            m_laneSelector->addItem(label, QString::fromStdString(lane.id));
        }
    }

    // Restore what the view is on, so a rebuild after adding a lane does not
    // silently jump the user back to velocity.
    int index = 0;
    if (m_view->laneParam() == PianoRollView::LaneParam::Pan) {
        index = 1;
    } else if (m_view->laneParam() == PianoRollView::LaneParam::Controller) {
        const int found = m_laneSelector->findData(m_view->laneId());
        index = found >= 0 ? found : 0;
    }
    m_laneSelector->setCurrentIndex(index);
    if (m_removeLaneButton) m_removeLaneButton->setEnabled(index >= 2);
}

void PianoRollWindow::laneSelectionChanged(int index) {
    if (index == 0) {
        m_view->setLaneParam(PianoRollView::LaneParam::Velocity);
    } else if (index == 1) {
        m_view->setLaneParam(PianoRollView::LaneParam::Pan);
    } else {
        m_view->setLaneParam(PianoRollView::LaneParam::Controller,
                             m_laneSelector->itemData(index).toString());
    }
    // Only the two note parameters are remembered: a controller lane belongs to
    // one clip, and pointing a fresh clip at a lane id it does not have would
    // reopen the roll on an empty curve.
    if (index <= 1) setPianoRollPref("lane.param", index);
    if (m_removeLaneButton) m_removeLaneButton->setEnabled(index >= 2);
}

void PianoRollWindow::addControllerLane() {
    // The common controllers by name, plus a free choice — nobody remembers
    // that expression is 11, and nobody should have to.
    struct Preset { const char* name; int cc; };
    static const Preset presets[] = {
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Mod Wheel"), 1},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Breath"), 2},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Expression"), 11},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Volume"), 7},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Pan"), 10},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Sustain"), 64},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Cutoff"), 74},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Resonance"), 71},
        {QT_TRANSLATE_NOOP("PianoRollWindow", "Pitch Bend"), -2},
    };
    QStringList choices;
    for (const auto& preset : presets) {
        const QString name = QCoreApplication::translate(
            "PianoRollWindow", preset.name);
        choices << (preset.cc >= 0
                        ? tr("%1 (CC %2)").arg(name).arg(preset.cc)
                        : name);
    }
    choices << tr("Other CC…");

    bool ok = false;
    const QString picked =
        QInputDialog::getItem(this, tr("Add Controller Lane"), tr("Controller:"),
                              choices, 0, false, &ok);
    if (!ok) return;

    std::string name;
    int cc = 1;
    const int index = choices.indexOf(picked);
    if (index >= 0 && index < int(std::size(presets))) {
        name = presets[index].name;
        cc = presets[index].cc;
    } else {
        cc = QInputDialog::getInt(this, tr("Add Controller Lane"),
                                  tr("CC number:"), 1, 0, 127, 1, &ok);
        if (!ok) return;
        name = tr("CC %1").arg(cc).toStdString();
    }

    const std::string laneId = m_controller->addControllerLane(
        m_trackId.toStdString(), m_clipId.toStdString(), name, cc);
    if (laneId.empty()) return;
    m_view->setLaneParam(PianoRollView::LaneParam::Controller,
                         QString::fromStdString(laneId));
    refreshLaneSelector();
    updateActionState();
    emit edited();
}

void PianoRollWindow::removeControllerLane() {
    if (m_view->laneParam() != PianoRollView::LaneParam::Controller) return;
    m_controller->removeControllerLane(m_trackId.toStdString(),
                                       m_clipId.toStdString(),
                                       m_view->laneId().toStdString());
    m_view->setLaneParam(PianoRollView::LaneParam::Velocity);
    refreshLaneSelector();
    updateActionState();
    emit edited();
}

namespace {

// ── Remembered settings ──
//
// Every choice the roll offers is stored under "pianoRoll/" and read back on
// the next launch. The helpers below are the whole mechanism: a menu item
// declares its key and its factory default, and gets loading, applying and
// saving for free. Doing it any other way means one setting is always the one
// that got forgotten.

/// A checkable item backed by a settings key: it opens showing what was stored,
/// pushes that into the view straight away, and writes back on every change.
QAction* addToggle(QMenu* menu, const QString& text, const QString& key,
                   bool defaultOn, const std::function<void(bool)>& apply,
                   const QKeySequence& shortcut = {}) {
    const bool stored = pianoRollPref(key, defaultOn).toBool();
    QAction* action = menu->addAction(text);
    action->setCheckable(true);
    action->setChecked(stored);
    if (!shortcut.isEmpty()) action->setShortcut(shortcut);
    QObject::connect(action, &QAction::toggled, menu, [key, apply](bool on) {
        setPianoRollPref(key, on);
        apply(on);
    });
    // The stored state has to reach the view now, not on the first click.
    apply(stored);
    return action;
}

/// A group of mutually exclusive items backed by one key holding the index.
/// `apply` is called with the stored choice at build time, exactly as
/// `addToggle` does — so what is ticked and what the view is doing agree.
template <typename T>
void addChoice(QMenu* menu, const QString& key, int defaultIndex,
               const std::vector<std::pair<QString, T>>& items,
               const std::function<void(T)>& apply) {
    int stored = pianoRollPref(key, defaultIndex).toInt();
    if (stored < 0 || stored >= int(items.size())) stored = defaultIndex;

    auto* group = new QActionGroup(menu);
    for (int i = 0; i < int(items.size()); ++i) {
        QAction* action = menu->addAction(items[size_t(i)].first);
        action->setCheckable(true);
        action->setChecked(i == stored);
        group->addAction(action);
        const T value = items[size_t(i)].second;
        QObject::connect(action, &QAction::triggered, menu, [key, i, value, apply] {
            setPianoRollPref(key, i);
            apply(value);
        });
    }
    apply(items[size_t(stored)].second);
}

} // namespace

void PianoRollWindow::buildEditMenu(QMenu* menu) {
    m_undoAction = menu->addAction(tr("Undo"), QKeySequence::Undo, this, [this] {
        m_controller->undo();
        refresh();
        emit edited();
    });
    m_redoAction = menu->addAction(tr("Redo"), QKeySequence::Redo, this, [this] {
        m_controller->redo();
        refresh();
        emit edited();
    });
    menu->addSeparator();

    auto* cut = menu->addAction(tr("Cut"), QKeySequence::Cut, this,
                                [this] { m_view->cutSelection(); });
    cut->setObjectName(QStringLiteral("pianoRoll.edit.cut"));
    auto* copy = menu->addAction(tr("Copy"), QKeySequence::Copy, this,
                                 [this] { m_view->copySelection(); });
    copy->setObjectName(QStringLiteral("pianoRoll.edit.copy"));
    m_pasteAction = menu->addAction(tr("Paste"), QKeySequence::Paste, this,
                                    [this] { m_view->paste(); });
    m_pasteAction->setObjectName(QStringLiteral("pianoRoll.edit.paste"));
    m_repeatAction = menu->addAction(tr("Repeat Notes"), this,
                                     [this] { m_view->duplicateSelection(); });
    // Qt maps CTRL to Command on macOS and Control on Windows/Linux. Building
    // the sequence from modifiers also lets the physical-key filter replace a
    // Cyrillic letter without reparsing translated shortcut text.
    m_repeatAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_B),
                                  QKeySequence(Qt::CTRL | Qt::Key_D)});
    m_repeatAction->setObjectName(QStringLiteral("pianoRoll.edit.repeat"));
    auto* remove = menu->addAction(tr("Delete"), QKeySequence::Delete, this,
                                   [this] { m_view->deleteSelection(); });
    remove->setObjectName(QStringLiteral("pianoRoll.edit.delete"));
    menu->addSeparator();

    menu->addAction(tr("Select All"), QKeySequence::SelectAll, this,
                    [this] { m_view->selectAll(); });
    menu->addAction(tr("Select None"), QKeySequence(tr("Ctrl+Shift+A")), this,
                    [this] { m_view->selectNone(); });
    menu->addAction(tr("Invert Selection"), QKeySequence(tr("Ctrl+I")), this,
                    [this] { m_view->invertSelection(); });
    menu->addAction(tr("Select Same Colour"), this,
                    [this] { m_view->selectSameColor(); });
    menu->addSeparator();

    menu->addAction(tr("Transpose Up a Semitone"),
                    QKeySequence(Qt::SHIFT | Qt::Key_Up), this, [this] {
                        m_view->applyTransform(
                            [](const mt::Notes& n) { return mt::transpose(n, 1); },
                            tr("Transpose"));
                    });
    menu->addAction(tr("Transpose Down a Semitone"),
                    QKeySequence(Qt::SHIFT | Qt::Key_Down), this, [this] {
                        m_view->applyTransform(
                            [](const mt::Notes& n) { return mt::transpose(n, -1); },
                            tr("Transpose"));
                    });
    menu->addAction(tr("Transpose Up an Octave"),
                    QKeySequence(Qt::CTRL | Qt::Key_Up), this, [this] {
                        m_view->applyTransform(
                            [](const mt::Notes& n) { return mt::transpose(n, 12); },
                            tr("Transpose Octave"));
                    });
    menu->addAction(tr("Transpose Down an Octave"),
                    QKeySequence(Qt::CTRL | Qt::Key_Down), this, [this] {
                        m_view->applyTransform(
                            [](const mt::Notes& n) { return mt::transpose(n, -12); },
                            tr("Transpose Octave"));
                    });
    menu->addAction(tr("Transpose…"), this, [this] {
        bool ok = false;
        const int semitones = QInputDialog::getInt(
            this, tr("Transpose"), tr("Semitones:"), 0, -48, 48, 1, &ok);
        if (!ok || semitones == 0) return;
        m_view->applyTransform(
            [semitones](const mt::Notes& n) { return mt::transpose(n, semitones); },
            tr("Transpose"));
    });
    menu->addSeparator();

    menu->addAction(tr("Nudge Left"), QKeySequence(Qt::SHIFT | Qt::Key_Left), this,
                    [this] {
                        const double step = m_view->effectiveGridBeats();
                        m_view->applyTransform(
                            [step](const mt::Notes& n) { return mt::nudge(n, -step); },
                            tr("Nudge"));
                    });
    menu->addAction(tr("Nudge Right"), QKeySequence(Qt::SHIFT | Qt::Key_Right),
                    this, [this] {
                        const double step = m_view->effectiveGridBeats();
                        m_view->applyTransform(
                            [step](const mt::Notes& n) { return mt::nudge(n, step); },
                            tr("Nudge"));
                    });
    // Rotate is the cyclic twin of nudge: the same shortcut with Cmd added,
    // because it is the same gesture with the phrase's two ends joined up.
    // `rotateSpanBeats` decides what "the phrase" is.
    menu->addAction(tr("Rotate Left"),
                    QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_Left), this,
                    [this] {
                        const double step = m_view->effectiveGridBeats();
                        const double span = m_view->rotateSpanBeats();
                        m_view->applyTransform(
                            [step, span](const mt::Notes& n) {
                                return mt::rotate(n, -step, span);
                            },
                            tr("Rotate"));
                    });
    menu->addAction(tr("Rotate Right"),
                    QKeySequence(Qt::SHIFT | Qt::CTRL | Qt::Key_Right), this,
                    [this] {
                        const double step = m_view->effectiveGridBeats();
                        const double span = m_view->rotateSpanBeats();
                        m_view->applyTransform(
                            [step, span](const mt::Notes& n) {
                                return mt::rotate(n, step, span);
                            },
                            tr("Rotate"));
                    });
    menu->addSeparator();

    auto* velocity = menu->addMenu(tr("Velocity"));
    velocity->addAction(tr("Set…"), this, [this] {
        bool ok = false;
        const int value = QInputDialog::getInt(this, tr("Set Velocity"),
                                               tr("Velocity (1–127):"), 100, 1,
                                               127, 1, &ok);
        if (!ok) return;
        m_view->applyTransform(
            [value](const mt::Notes& n) { return mt::setVelocity(n, value); },
            tr("Set Velocity"));
    });
    velocity->addAction(tr("Scale…"), this, [this] {
        bool ok = false;
        const int percent = QInputDialog::getInt(this, tr("Scale Velocity"),
                                                 tr("Percent:"), 100, 1, 400, 5,
                                                 &ok);
        if (!ok) return;
        const double factor = percent / 100.0;
        m_view->applyTransform(
            [factor](const mt::Notes& n) { return mt::scaleVelocity(n, factor); },
            tr("Scale Velocity"));
    });
    velocity->addAction(tr("Ramp…"), this, [this] {
        bool ok = false;
        const int from = QInputDialog::getInt(this, tr("Velocity Ramp"),
                                              tr("From:"), 30, 1, 127, 1, &ok);
        if (!ok) return;
        const int to = QInputDialog::getInt(this, tr("Velocity Ramp"), tr("To:"),
                                            110, 1, 127, 1, &ok);
        if (!ok) return;
        m_view->applyTransform(
            [from, to](const mt::Notes& n) { return mt::rampVelocity(n, from, to); },
            tr("Velocity Ramp"));
    });

    auto* duration = menu->addMenu(tr("Duration"));
    for (auto [label, beats] :
         {std::pair<const char*, double>{"1/1", 4.0}, {"1/2", 2.0}, {"1/4", 1.0},
          {"1/8", 0.5}, {"1/16", 0.25}, {"1/32", 0.125},
          {"1/4 dotted", 1.5}, {"1/8 dotted", 0.75},
          {"1/4 triplet", 2.0 / 3.0}, {"1/8 triplet", 1.0 / 3.0}}) {
        duration->addAction(QString::fromUtf8(label), this, [this, beats] {
            m_view->applyTransform(
                [beats](const mt::Notes& n) { return mt::setLength(n, beats); },
                tr("Set Note Length"));
        });
    }
    duration->addSeparator();
    duration->addAction(tr("Scale…"), this, [this] {
        bool ok = false;
        const int percent = QInputDialog::getInt(this, tr("Scale Duration"),
                                                 tr("Percent:"), 100, 1, 800, 5,
                                                 &ok);
        if (!ok) return;
        const double factor = percent / 100.0;
        m_view->applyTransform(
            [factor](const mt::Notes& n) { return mt::scaleLength(n, factor); },
            tr("Scale Duration"));
    });
    menu->addSeparator();

    menu->addAction(tr("Mute Notes"), QKeySequence(tr("Ctrl+M")), this, [this] {
        m_view->applyTransform(
            [](const mt::Notes& n) { return mt::toggleMuted(n); }, tr("Mute Notes"));
    });
    auto* colour = menu->addMenu(tr("Note Colour"));
    colour->addAction(tr("Set…"), this, [this] {
        const QColor picked = QColorDialog::getColor(Qt::white, this,
                                                     tr("Note Colour"));
        if (!picked.isValid()) return;
        const uint32_t rgb = uint32_t(picked.rgb() & 0xFFFFFFu);
        m_view->applyTransform(
            [rgb](const mt::Notes& n) { return mt::setColor(n, rgb); },
            tr("Note Colour"));
        m_view->setColorMode(PianoRollView::ColorMode::Custom);
    });
    colour->addAction(tr("Clear"), this, [this] {
        m_view->applyTransform([](const mt::Notes& n) { return mt::setColor(n, 0); },
                               tr("Clear Note Colour"));
    });
}

void PianoRollWindow::buildViewMenu(QMenu* menu) {
    addToggle(menu, tr("Piano Keyboard"), "view.keyboard", true,
              [this](bool on) { m_view->setShowKeyboard(on); },
              QKeySequence(tr("Ctrl+K")));
    // Alt+L, not Ctrl+L: Ctrl+L is Quick Legato, and Ctrl+Shift+L is the main
    // window's layer-invert hold, which would win from its global menu bar.
    addToggle(menu, tr("Parameter Lane"), "view.lane", true,
              [this](bool on) { m_view->setShowVelocityLane(on); },
              QKeySequence(tr("Alt+L")));
    addToggle(menu, tr("Follow Playback"), "view.followPlayback", true,
              [this](bool on) { m_view->setFollowPlayback(on); });
    menu->addSeparator();

    // ── Colouring ──
    auto* colours = menu->addMenu(tr("Colour Notes By"));
    addChoice<PianoRollView::ColorMode>(
        colours, "view.colorMode", 0,
        {{tr("Clip"), PianoRollView::ColorMode::Clip},
         {tr("Velocity"), PianoRollView::ColorMode::Velocity},
         {tr("Pitch"), PianoRollView::ColorMode::Pitch},
         {tr("Their own colour"), PianoRollView::ColorMode::Custom}},
        [this](PianoRollView::ColorMode mode) { m_view->setColorMode(mode); });

    // ── Grid ──
    auto* grid = menu->addMenu(tr("Grid"));
    grid->addAction(tr("Colour…"), this, [this] {
        const QColor picked = QColorDialog::getColor(
            m_view->gridColor().isValid() ? m_view->gridColor() : th().gridLine,
            this, tr("Grid Colour"));
        if (!picked.isValid()) return;
        m_view->setGridColor(picked);
        setPianoRollPref("view.gridColor", picked.name());
    });
    grid->addAction(tr("Use the theme's colour"), this, [this] {
        m_view->setGridColor(QColor());
        // Empty, not a colour: the theme's grid line changes with the theme, so
        // storing today's value would freeze it to this one.
        setPianoRollPref("view.gridColor", QString());
    });
    grid->addSeparator();
    addChoice<double>(grid, "view.gridContrast", 1,
                      {{tr("Low contrast"), 0.15},
                       {tr("Medium contrast"), 0.5},
                       {tr("High contrast"), 1.0}},
                      [this](double value) { m_view->setGridContrast(value); });

    // ── Scale ──
    menu->addSeparator();
    addToggle(menu, tr("Scale Highlight"), "view.scaleHighlight", false,
              [this](bool on) { m_view->setScaleHighlight(on); });

    auto* rootMenu = menu->addMenu(tr("Scale Root"));
    static const char* pitchClassNames[12] = {"C", "C#", "D",  "D#", "E",  "F",
                                              "F#", "G", "G#", "A",  "A#", "B"};
    std::vector<std::pair<QString, int>> roots;
    for (int i = 0; i < 12; ++i) {
        roots.emplace_back(QString::fromUtf8(pitchClassNames[i]), i);
    }
    addChoice<int>(rootMenu, "scale.root", 0, roots,
                   [this](int root) { m_view->setScale(root, m_view->scale()); });

    auto* scaleMenu = menu->addMenu(tr("Scale"));
    std::vector<std::pair<QString, mt::Scale>> scales;
    for (auto scale : mt::allScales()) {
        scales.emplace_back(QString::fromStdString(mt::scaleName(scale)), scale);
    }
    addChoice<mt::Scale>(scaleMenu, "scale.kind", 1, scales,
                         [this](mt::Scale scale) {
                             m_view->setScale(m_view->scaleRoot(), scale);
                         });

    // ── Ghost notes and note appearance ──
    //
    // Both hang off their own toolbar buttons rather than appearing here too:
    // one menu shown from two places is one menu the user has to learn twice.
    m_ghostMenu = new QMenu(tr("Ghost Notes"), this);
    connect(m_ghostMenu, &QMenu::aboutToShow, this,
            &PianoRollWindow::refreshGhostMenu);

    m_noteStyleMenu = new QMenu(tr("Note Style"), this);
    addChoice<PianoRollView::NoteStyle>(
        m_noteStyleMenu, "view.noteStyle", 0,
        {{tr("Rounded"), PianoRollView::NoteStyle::Rounded},
         {tr("Flat"), PianoRollView::NoteStyle::Flat}},
        [this](PianoRollView::NoteStyle style) { m_view->setNoteStyle(style); });
    m_noteStyleMenu->addSeparator();
    addToggle(m_noteStyleMenu, tr("Note Names on Notes"), "view.noteNames", false,
              [this](bool on) { m_view->setShowNoteNames(on); });
    addToggle(m_noteStyleMenu, tr("Name Every Key"), "view.keyNames", false,
              [this](bool on) { m_view->setShowAllKeyNames(on); });
    addToggle(m_noteStyleMenu, tr("Note Borders"), "view.noteBorders", true,
              [this](bool on) { m_view->setNoteBorders(on); });

    // ── Zoom ──
    menu->addSeparator();
    menu->addAction(tr("Zoom In"), QKeySequence::ZoomIn, this,
                    [this] { m_view->zoomHorizontal(1.25); });
    menu->addAction(tr("Zoom Out"), QKeySequence::ZoomOut, this,
                    [this] { m_view->zoomHorizontal(1.0 / 1.25); });
    menu->addAction(tr("Taller Rows"), QKeySequence(tr("Ctrl+Shift+=")), this,
                    [this] { m_view->zoomVertical(1.25); });
    menu->addAction(tr("Shorter Rows"), QKeySequence(tr("Ctrl+Shift+-")), this,
                    [this] { m_view->zoomVertical(1.0 / 1.25); });
    menu->addAction(tr("Zoom to Fit"), QKeySequence(tr("Ctrl+0")), this,
                    [this] { m_view->zoomToFit(); });
    menu->addAction(tr("Zoom to Selection"), QKeySequence(tr("Ctrl+Shift+0")), this,
                    [this] { m_view->zoomToSelection(); });
}

void PianoRollWindow::buildToolsMenu(QMenu* menu) {
    // ── Tool modes ──
    m_toolGroup = new QActionGroup(this);
    const std::tuple<QString, PianoRollView::Tool, QKeySequence> tools[] = {
        {tr("Draw"), PianoRollView::Tool::Draw, QKeySequence(Qt::Key_P)},
        {tr("Select"), PianoRollView::Tool::Select, QKeySequence(Qt::Key_E)},
        {tr("Slice"), PianoRollView::Tool::Slice, QKeySequence(Qt::Key_S)},
        {tr("Disable Notes"), PianoRollView::Tool::Mute,
         QKeySequence(Qt::Key_T)},
    };
    for (const auto& [label, tool, key] : tools) {
        QAction* action = menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(tool == PianoRollView::Tool::Draw);
        // No shortcut on the action itself: S and T also *hold* to borrow the
        // tool, and that lives in the view's key handler. A menu shortcut would
        // swallow the press before the view ever saw it.
        if (tool == PianoRollView::Tool::Draw ||
            tool == PianoRollView::Tool::Select) {
            action->setShortcut(key);
        } else {
            action->setToolTip(tr("Hold the key to borrow the tool for one edit"));
        }
        m_toolGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool] {
            m_view->setTool(tool);
            setPianoRollPref("tool", int(tool));
            syncToolActions();
        });
    }
    menu->addSeparator();

    // ── Dialog-driven tools ──
    m_quantizeAction = menu->addAction(tr("Quantize…"),
                                       QKeySequence(Qt::ALT | Qt::Key_Q), this, [this] {
        hostToolDialog<QuantizeDialog>(m_quantizeDialog, tr("Quantize"),
                                       [this](QuantizeDialog* d) {
            const auto params = d->params();
            m_lastQuantize = params;
            auto run = [params](const mt::Notes& n) { return mt::quantize(n, params); };
            m_view->previewTransform(run);
        });
        m_quantizeDialog->setGridBeats(m_view->effectiveGridBeats());
    });
    menu->addAction(tr("Quick Quantize"), QKeySequence(Qt::Key_Q), this, [this] {
        // Repeat the last settings with no dialog at all — the shortcut you
        // reach for a hundred times an hour.
        mt::QuantizeParams params = m_lastQuantize;
        if (params.gridBeats <= 0.0) params.gridBeats = m_view->effectiveGridBeats();
        m_view->applyTransform(
            [params](const mt::Notes& n) { return mt::quantize(n, params); },
            tr("Quantize"));
    });
    m_arpAction = menu->addAction(tr("Arpeggiator…"), this, [this] {
        hostToolDialog<ArpeggiatorDialog>(
            m_arpDialog, tr("Arpeggiate"), [this](ArpeggiatorDialog* d) {
                const auto params = d->params();
                const double end = m_view->clipBeats();
                auto run = [params, end](const mt::Notes& n) {
                    return mt::arpeggiate(n, params, end);
                };
                m_view->previewTransform(run);
            });
        m_arpDialog->setRateBeats(m_view->effectiveGridBeats());
    });
    m_arpAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_A));
    m_chordAction = menu->addAction(tr("Chord Generator…"), this, [this] {
        hostToolDialog<ChordDialog>(m_chordDialog, tr("Build Chords"),
                                    [this](ChordDialog* d) {
            const auto params = d->params();
            auto run = [params](const mt::Notes& n) {
                return mt::buildChords(n, params);
            };
            m_view->previewTransform(run);
        });
    });
    m_chordAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_B));
    m_glueAction = menu->addAction(tr("Glue…"), this, [this] {
        hostToolDialog<GlueDialog>(m_glueDialog, tr("Glue"), [this](GlueDialog* d) {
            const auto params = d->params();
            auto run = [params](const mt::Notes& n) { return mt::glue(n, params); };
            m_view->previewTransform(run);
        });
    });
    m_glueAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_G));
    m_strumAction = menu->addAction(tr("Strum…"), this, [this] {
        hostToolDialog<StrumDialog>(m_strumDialog, tr("Strum"),
                                    [this](StrumDialog* d) {
            const auto params = d->params();
            auto run = [params](const mt::Notes& n) { return mt::strum(n, params); };
            m_view->previewTransform(run);
        });
    });
    m_strumAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_S));
    m_articulateAction = menu->addAction(tr("Articulate…"), this, [this] {
        hostToolDialog<ArticulateDialog>(
            m_articulateDialog, tr("Articulate"), [this](ArticulateDialog* d) {
                const auto params = d->params();
                auto run = [params](const mt::Notes& n) {
                    return mt::articulate(n, params);
                };
                m_view->previewTransform(run);
            });
    });
    // Alt+A is the requested Arpeggiator binding; T is the first distinctive
    // letter left in Articulate and avoids an ambiguous shortcut.
    m_articulateAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_T));
    m_randomAction = menu->addAction(tr("Randomize…"), this, [this] {
        hostToolDialog<RandomizeDialog>(
            m_randomDialog, tr("Randomize"), [this](RandomizeDialog* d) {
                const auto params = d->params();
                auto run = [params](const mt::Notes& n) {
                    return mt::randomize(n, params);
                };
                m_view->previewTransform(run);
            });
        m_randomDialog->setRegionEndBeats(m_view->clipBeats());
        m_randomDialog->setGridBeats(m_view->effectiveGridBeats());
    });
    m_randomAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_R));
    menu->addSeparator();

    // ── One-shot tools ──
    menu->addAction(tr("Glue Overlapping"), this, [this] {
        mt::GlueParams params;
        m_view->applyTransform(
            [params](const mt::Notes& n) { return mt::glue(n, params); }, tr("Glue"));
    });
    menu->addAction(tr("Quick Legato"), QKeySequence(tr("Ctrl+L")), this, [this] {
        // The clip is the reference, not the selection: "the next note" means the
        // next one in the part, so a stretch stops at an unselected note in its
        // way and one selected note still has somewhere to reach.
        const mt::Notes context = m_view->clipNotes();
        m_view->applyTransform(
            [context](const mt::Notes& n) { return mt::legato(n, context); },
            tr("Legato"));
    });
    menu->addAction(tr("Discard Lengths"), QKeySequence(tr("Shift+D")), this,
                    [this] {
                        // The current grid step, not the last length drawn. The
                        // grid is a number the user picked and can see on
                        // screen, so the result is predictable and the command
                        // means something even with one note selected — the
                        // last-drawn length is usually that same note's, which
                        // made this look like it did nothing.
                        const double length = m_view->effectiveGridBeats();
                        m_view->applyTransform(
                            [length](const mt::Notes& n) {
                                return mt::setLength(n, length);
                            },
                            tr("Discard Lengths"));
                    });
    menu->addAction(tr("Humanize"), this, [this] {
        // A fresh seed each time, so pressing it twice does not apply the same
        // "random" offsets twice over.
        const uint32_t seed = QRandomGenerator::global()->bounded(1, 999999);
        m_view->applyTransform(
            [seed](const mt::Notes& n) { return mt::humanize(n, 0.02, 12, seed); },
            tr("Humanize"));
    });
    menu->addAction(tr("Invert Pitches"), this, [this] {
        m_view->applyTransform([](const mt::Notes& n) { return mt::invertPitch(n); },
                               tr("Invert"));
    });
    menu->addAction(tr("Reverse in Time"), this, [this] {
        m_view->applyTransform([](const mt::Notes& n) { return mt::reverseTime(n); },
                               tr("Reverse"));
    });
    menu->addAction(tr("Snap Pitches to Scale"), this, [this] {
        const int root = m_view->scaleRoot();
        const auto scale = m_view->scale();
        m_view->applyTransform(
            [root, scale](const mt::Notes& n) {
                return mt::snapToScale(n, root, scale);
            },
            tr("Snap to Scale"));
    });
    menu->addAction(tr("Split at Grid"), this, [this] {
        const double grid = m_view->effectiveGridBeats();
        m_view->applyTransform(
            [grid](const mt::Notes& n) { return mt::splitAtGrid(n, grid); },
            tr("Split at Grid"));
    });
    menu->addAction(tr("Limit Pitch Range…"), this, [this] {
        bool ok = false;
        const int low = QInputDialog::getInt(this, tr("Limit"), tr("Lowest note:"),
                                             48, 0, 127, 1, &ok);
        if (!ok) return;
        const int high = QInputDialog::getInt(this, tr("Limit"), tr("Highest note:"),
                                              84, 0, 127, 1, &ok);
        if (!ok) return;
        m_view->applyTransform(
            [low, high](const mt::Notes& n) { return mt::limitPitch(n, low, high); },
            tr("Limit"));
    });
}

void PianoRollWindow::buildSnapMenu(QMenu* menu) {
    addToggle(menu, tr("Snap to Grid"), "snap.enabled", true,
              [this](bool on) { m_view->setSnapEnabled(on); });
    addToggle(menu, tr("Adaptive"), "snap.adaptive", false,
              [this](bool on) { m_view->setAdaptiveSnap(on); })
        ->setToolTip(tr("Follow the zoom: the finer you zoom in, the finer the "
                        "grid you snap to."));
    addToggle(menu, tr("Snap to Scale"), "snap.toScale", false,
              [this](bool on) { m_view->setSnapToScale(on); })
        ->setToolTip(tr("Drawn and dragged notes land only on degrees of the "
                        "scale set in the View menu."));
    menu->addSeparator();

    // The division and the flavour multiply into one grid value, so each one
    // re-reads the other rather than caching a beat count that could go stale.
    const int storedDenominator = pianoRollPref("snap.denominator", 16).toInt();
    const int storedFlavour = pianoRollPref("snap.flavour", 0).toInt();

    m_divisionGroup = new QActionGroup(this);
    for (int denominator : {1, 2, 4, 8, 16, 32, 64, 128}) {
        QAction* action = menu->addAction(QString("1/%1").arg(denominator));
        action->setCheckable(true);
        action->setChecked(denominator == storedDenominator);
        action->setData(denominator);
        m_divisionGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                &PianoRollWindow::applyGridFromMenus);
    }
    menu->addSeparator();

    m_flavourGroup = new QActionGroup(this);
    const std::pair<QString, mt::GridFlavour> flavours[] = {
        {tr("Straight"), mt::GridFlavour::Straight},
        {tr("Triplet"), mt::GridFlavour::Triplet},
        {tr("Dotted"), mt::GridFlavour::Dotted},
    };
    for (const auto& [label, flavour] : flavours) {
        QAction* action = menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(int(flavour) == storedFlavour);
        action->setData(int(flavour));
        m_flavourGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                &PianoRollWindow::applyGridFromMenus);
    }
    // Nothing was clicked, so push the stored pair into the view by hand.
    applyGridFromMenus();
    menu->addSeparator();

    m_view->setSwing(pianoRollPref("snap.swing", 0.5).toDouble());
    menu->addAction(tr("Swing…"), this, [this] {
        bool ok = false;
        const int percent = QInputDialog::getInt(
            this, tr("Swing"),
            tr("Swing (50% is straight; higher pushes the off-beats late):"),
            int(std::lround(m_view->swing() * 100.0)), 50, 90, 1, &ok);
        if (!ok) return;
        m_view->setSwing(percent / 100.0);
        setPianoRollPref("snap.swing", m_view->swing());
    });
}

void PianoRollWindow::scheduleToolPreview(ToolDialog* owner,
                                          std::function<void()> preview) {
    if (!owner || !preview || !m_toolPreviewTimer) return;
    if (m_pendingPreviewOwner && m_pendingPreviewOwner.data() != owner)
        cancelToolPreview();
    m_pendingPreviewOwner = owner;
    m_pendingToolPreview = std::move(preview);
    // Do not restart an active timer: later signals replace the callback's data,
    // while the first signal still guarantees one result on the next frame.
    if (!m_toolPreviewTimer->isActive()) m_toolPreviewTimer->start();
}

void PianoRollWindow::runPendingToolPreview() {
    QPointer<ToolDialog> owner = m_pendingPreviewOwner;
    std::function<void()> preview = std::move(m_pendingToolPreview);
    m_pendingPreviewOwner.clear();
    m_pendingToolPreview = {};
    if (!owner || !preview) return;
    ++m_coalescedToolPreviewRuns;
    preview();
}

bool PianoRollWindow::flushToolPreview(ToolDialog* owner) {
    if (!owner || m_pendingPreviewOwner.data() != owner ||
        !m_pendingToolPreview) {
        return false;
    }
    if (m_toolPreviewTimer) m_toolPreviewTimer->stop();
    runPendingToolPreview();
    return true;
}

void PianoRollWindow::cancelToolPreview(ToolDialog* owner) {
    if (owner && m_pendingPreviewOwner.data() != owner) return;
    if (m_toolPreviewTimer) m_toolPreviewTimer->stop();
    m_pendingPreviewOwner.clear();
    m_pendingToolPreview = {};
}

template <typename Dialog>
void PianoRollWindow::hostToolDialog(Dialog*& dialog,
                                     const QString& undoLabel,
                                     const std::function<void(Dialog*)>& preview) {
    if (!dialog) {
        dialog = new Dialog(this);
        const QPointer<Dialog> guarded(dialog);
        connect(dialog, &ToolDialog::paramsChanged, this,
                [this, guarded, preview] {
            if (!guarded) return;
            Dialog* current = guarded.data();
            if (current->previewEnabled()) {
                if (m_previewOwner && m_previewOwner != current) {
                    m_view->clearPreview();
                    m_previewOwner = nullptr;
                }
                scheduleToolPreview(current, [this, guarded, preview] {
                    if (!guarded || !guarded->previewEnabled()) return;
                    preview(guarded.data());
                    m_previewOwner = guarded.data();
                });
            } else {
                cancelToolPreview(current);
                if (m_previewOwner != current) return;
                m_view->clearPreview();
                m_previewOwner = nullptr;
            }
        });
        connect(dialog, &ToolDialog::applyRequested, this,
                [this, guarded, preview, undoLabel] {
                    if (!guarded) return;
                    Dialog* current = guarded.data();
                    // With Preview off there is no painted result yet. Build it
                    // and commit it in the same event-loop turn; with Preview on
                    // flushes the latest coalesced parameters first, preserving
                    // the exact result rather than committing the previous frame.
                    const bool flushed = flushToolPreview(current);
                    if (!flushed) {
                        // A different modeless dialog may have a pending result;
                        // it must not overwrite this Apply one frame later.
                        cancelToolPreview();
                        if (m_previewOwner != current || !m_view->hasPreview())
                            preview(current);
                    }
                    m_view->commitPreview(undoLabel);
                    m_previewOwner = nullptr;
                });
        connect(dialog, &ToolDialog::rejected, this,
                [this, guarded] {
                    if (!guarded) return;
                    Dialog* current = guarded.data();
                    cancelToolPreview(current);
                    if (m_previewOwner != current) return;
                    m_view->clearPreview();
                    m_previewOwner = nullptr;
                });
    }
    if (m_pendingPreviewOwner && m_pendingPreviewOwner.data() != dialog)
        cancelToolPreview();
    if (m_previewOwner && m_previewOwner != dialog) {
        m_view->clearPreview();
        m_previewOwner = nullptr;
    }
    emit internalWindowRequested(
        dialog,
        QStringLiteral("internalEditors/pianoRollTools/") +
            QString::fromLatin1(dialog->metaObject()->className()));
    dialog->show();
    if (dialog->previewEnabled()) {
        if (!flushToolPreview(dialog)) {
            preview(dialog);
            m_previewOwner = dialog;
        }
    }
}

void PianoRollWindow::loadViewPreferences() {
    // The continuous state: what no menu item owns, and what the user sets by
    // dragging rather than by picking.
    m_view->setLaneHeight(pianoRollPref("lane.height", 84.0).toDouble());
    const int laneParam = pianoRollPref("lane.param", 0).toInt();
    m_view->setLaneParam(laneParam == 1 ? PianoRollView::LaneParam::Pan
                                        : PianoRollView::LaneParam::Velocity);
    m_view->setRowHeight(pianoRollPref("zoom.rowHeight", 12.0).toDouble());
    // Zero means "let the roll pick its own width", which is also the default,
    // so a roll that was never zoomed by hand still opens sized to the clip —
    // subject to the readable minimum in `pxPerBeat()`.
    m_view->setPixelsPerBeat(pianoRollPref("zoom.pxPerBeat", 0.0).toDouble());
    if (auto* navigator = dynamic_cast<PianoRollNavigator*>(m_hScroll)) {
        navigator->setNavigatorHeight(
            pianoRollPref("navigation.height", 18).toInt());
    }
    m_view->setTool(PianoRollView::Tool(pianoRollPref("tool", 0).toInt()));
    syncToolActions();
    refreshLaneSelector();

}

void PianoRollWindow::saveViewPreferences() {
    setPianoRollPref("lane.height", m_view->laneHeightPx());
    // A controller lane belongs to one clip, so only the two note parameters
    // are worth remembering; anything else reopens on velocity.
    setPianoRollPref("lane.param",
                     m_view->laneParam() == PianoRollView::LaneParam::Pan ? 1 : 0);
    setPianoRollPref("zoom.rowHeight", m_view->rowHeight());
    setPianoRollPref("zoom.pxPerBeat", m_view->pixelsPerBeat());
    setPianoRollPref("navigation.height", m_hScroll ? m_hScroll->height() : 18);
    setPianoRollPref("tool", int(m_view->tool()));
}

void PianoRollWindow::closeEvent(QCloseEvent* event) {
    cancelToolPreview();
    saveViewPreferences();
    QWidget::closeEvent(event);
}

void PianoRollWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!m_refreshPending) return;
    m_refreshPending = false;
    refresh();
}

void PianoRollWindow::selectAllNotesForTest() {
    if (m_view) m_view->selectAll();
}

bool PianoRollWindow::checkCycleGestureForTest() {
    if (!m_view || !m_controller) return false;
    m_controller->setLoopEnabled(false);
    m_controller->setLoopRangeSeconds(0.0, 0.0);
    QApplication::processEvents();

    const auto strike = [&](QEvent::Type type, const QPoint& at,
                            Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent ev(type, QPointF(at), QPointF(m_view->mapToGlobal(at)), button,
                       held, Qt::NoModifier);
        QApplication::sendEvent(m_view, &ev);
    };
    const int y = ui::kLoopStripHeight / 2;
    const QPoint from(m_view->width() / 3, y);
    const QPoint to(m_view->width() * 2 / 3, y);
    strike(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    strike(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    strike(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();

    const double start = m_controller->loopStartSeconds();
    const double end = m_controller->loopEndSeconds();
    if (!(end > start)) {
        std::fprintf(stderr, "dragging the roll's cycle strip made no region\n");
        return false;
    }
    if (!m_controller->isLoopEnabled()) {
        std::fprintf(stderr, "the roll did not arm the dragged cycle\n");
        return false;
    }

    const QPoint on((from.x() + to.x()) / 2, y);
    const auto doubleClick = [&] {
        strike(QEvent::MouseButtonPress, on, Qt::LeftButton, Qt::LeftButton);
        strike(QEvent::MouseButtonRelease, on, Qt::LeftButton, Qt::NoButton);
        strike(QEvent::MouseButtonDblClick, on, Qt::LeftButton, Qt::LeftButton);
        strike(QEvent::MouseButtonRelease, on, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
    };
    doubleClick();
    if (m_controller->isLoopEnabled() ||
        m_controller->loopStartSeconds() != 0.0 ||
        m_controller->loopEndSeconds() != 0.0) {
        std::fprintf(stderr, "double-clicking the roll's region did not remove it\n");
        return false;
    }

    m_controller->setLoopRangeSeconds(0.0, 0.0);
    m_controller->setLoopEnabled(false);
    QApplication::processEvents();
    return true;
}

bool PianoRollWindow::checkCompactLayoutForTest() {
    if (!m_view || !m_hScroll || !m_toolbar) return false;
    auto* heightControl =
        findChild<QToolButton*>(QStringLiteral("NoteHeightScrubber"));
    if (!heightControl || findChild<NoteContextPanel*>()) return false;

    const int gridTop = m_view->mapTo(this, QPoint(0, 0)).y();
    const int scrollBottom =
        m_hScroll->mapTo(this, QPoint(0, m_hScroll->height())).y();
    const int heightControlBottom =
        heightControl->mapTo(this, QPoint(0, heightControl->height())).y();
    const int navigationBottom = std::max(scrollBottom, heightControlBottom);
    const bool compact = gridTop - navigationBottom <= 2;
    const bool edgeToEdge = m_toolbar->x() == 0 &&
                            m_toolbar->width() == width() &&
                            m_view->mapTo(this, QPoint(0, 0)).x() == 0 &&
                            m_vScroll->mapTo(this, QPoint(m_vScroll->width(), 0)).x() ==
                                width();
    if (!compact || !edgeToEdge) {
        std::fprintf(stderr,
                     "the piano-roll header is not compact or edge-to-edge "
                     "(gap=%d, toolbar=%d..%d, width=%d)\n",
                     gridTop - navigationBottom, m_toolbar->x(),
                     m_toolbar->x() + m_toolbar->width(), width());
    }
    return compact && edgeToEdge;
}

void PianoRollWindow::cutNotes() {
    if (!m_view) return;
    m_view->cutSelection();
    updateActionState();
}

void PianoRollWindow::copyNotes() {
    if (!m_view) return;
    m_view->copySelection();
    updateActionState();
}

void PianoRollWindow::pasteNotes() {
    if (!m_view) return;
    m_view->paste();
    updateActionState();
}

void PianoRollWindow::repeatNotes() {
    if (!m_view) return;
    m_view->duplicateSelection();
    updateActionState();
}

void PianoRollWindow::deleteNotes() {
    if (!m_view) return;
    m_view->deleteSelection();
    updateActionState();
}

bool PianoRollWindow::eventFilter(QObject* watched, QEvent* event) {
    const bool shortcutOverride = event->type() == QEvent::ShortcutOverride;
    if ((!shortcutOverride && event->type() != QEvent::KeyPress) || !m_view)
        return QWidget::eventFilter(watched, event);

    auto* key = static_cast<QKeyEvent*>(event);
    if (!isPianoRollEditShortcut(key))
        return QWidget::eventFilter(watched, event);

    QWidget* focus = QApplication::focusWidget();
    const bool inside = focus && (focus == this || isAncestorOf(focus));
    if (!inside || isTextEntry(focus) || QApplication::activePopupWidget())
        return QWidget::eventFilter(watched, event);

    // Claim the chord before QAction's application/window shortcut resolver
    // can route it back to the arrangement. The following KeyPress performs
    // the edit through the same scoped filter.
    if (shortcutOverride) {
        key->accept();
        return true;
    }

    switch (editShortcutKey(key)) {
        case Qt::Key_X: m_view->cutSelection(); break;
        case Qt::Key_C: m_view->copySelection(); break;
        case Qt::Key_V: m_view->paste(); break;
        case Qt::Key_B: m_view->duplicateSelection(); break;
        default: return QWidget::eventFilter(watched, event);
    }
    key->accept();
    updateActionState();
    return true;
}

void PianoRollWindow::applyGridFromMenus() {
    if (!m_divisionGroup || !m_flavourGroup) return;
    QAction* division = m_divisionGroup->checkedAction();
    QAction* flavour = m_flavourGroup->checkedAction();
    if (!division || !flavour) return;
    const int denominator = division->data().toInt();
    const int kind = flavour->data().toInt();
    m_view->setGridBeats(mt::gridBeatsFor(denominator, mt::GridFlavour(kind)));
    // Only a real change is written: this also runs at build time to apply what
    // was stored, and a launch that re-saves what it just read can only ever
    // overwrite a good setting with a worse one.
    if (pianoRollPref("snap.denominator", 16).toInt() != denominator) {
        setPianoRollPref("snap.denominator", denominator);
    }
    if (pianoRollPref("snap.flavour", 0).toInt() != kind) {
        setPianoRollPref("snap.flavour", kind);
    }
}

void PianoRollWindow::openToolFor(NoteContextPanel::Tool tool) {
    // The panel asks, the window opens — so a tool reached from the plate and
    // the same tool reached from the menu are one dialog with one set of
    // settings, not two that disagree.
    switch (tool) {
        case NoteContextPanel::Tool::Quantize:    m_quantizeAction->trigger(); return;
        case NoteContextPanel::Tool::Arpeggiator: m_arpAction->trigger(); return;
        case NoteContextPanel::Tool::Chord:       m_chordAction->trigger(); return;
        case NoteContextPanel::Tool::Strum:       m_strumAction->trigger(); return;
        case NoteContextPanel::Tool::Glue:        m_glueAction->trigger(); return;
        case NoteContextPanel::Tool::Articulate:  m_articulateAction->trigger(); return;
        case NoteContextPanel::Tool::Randomize:   m_randomAction->trigger(); return;
    }
}

void PianoRollWindow::refreshGhostMenu() {
    if (!m_ghostMenu) return;
    m_ghostMenu->clear();
    const QSet<QString> active = m_view->ghostTracks();
    bool any = false;
    for (const auto& track : m_controller->project().tracks) {
        if (track.kind != daw::TrackKind::Midi &&
            track.kind != daw::TrackKind::Instrument) {
            continue;
        }
        const QString trackId = QString::fromStdString(track.id);
        if (trackId == m_trackId) continue;
        any = true;
        QAction* action =
            m_ghostMenu->addAction(QString::fromStdString(track.name));
        action->setCheckable(true);
        action->setChecked(active.contains(trackId));
        connect(action, &QAction::toggled, this, [this, trackId](bool on) {
            QSet<QString> tracks = m_view->ghostTracks();
            if (on) tracks.insert(trackId); else tracks.remove(trackId);
            m_view->setGhostTracks(tracks);
        });
    }
    if (!any) {
        QAction* empty = m_ghostMenu->addAction(tr("No other MIDI tracks"));
        empty->setEnabled(false);
    } else {
        m_ghostMenu->addSeparator();
        m_ghostMenu->addAction(tr("Show none"), this,
                               [this] { m_view->setGhostTracks({}); });
    }
}

void PianoRollWindow::setClip(const QString& trackId, const QString& clipId) {
    cancelToolPreview();
    m_refreshPending = false;
    m_trackId = trackId;
    m_clipId = clipId;
    m_view->setLivePitches({});
    m_view->setClip(trackId, clipId);
    m_previewOwner = nullptr;
    updateTitle();
    refreshLaneSelector();
    updateActionState();
    // Deferred: the view has no useful height until the window is laid out, and
    // setClip is normally called just before show().
    QMetaObject::invokeMethod(this, [this] {
        m_view->scrollToContent();
        updateScrollBars();
        m_view->setFocus(Qt::OtherFocusReason);
    }, Qt::QueuedConnection);
}

collab::SemanticPoint PianoRollWindow::collaborationPresenceAt(
    const QPointF& position) const {
    return m_view ? m_view->collaborationPresenceAt(position)
                  : collab::SemanticPoint{};
}

std::optional<QPointF> PianoRollWindow::collaborationPositionFor(
    const collab::SemanticPoint& point) const {
    return m_view ? m_view->collaborationPositionFor(point) : std::nullopt;
}

void PianoRollWindow::refresh() {
    if (!isVisible()) {
        m_refreshPending = true;
        return;
    }
    m_refreshPending = false;
    // Geometry/id/ghost indexes validate themselves by source, count and the
    // controller's monotonic MIDI revision. Generic project refreshes include
    // mixer, loop and unrelated-track changes, so clearing those large caches
    // here would force needless full-note rebuilds. Only the playhead keyboard
    // state needs a cheap dirty bit for external mute/undo changes.
    if (m_view) m_view->invalidateSoundingPitchIndex();
    updateTitle();
    // An undo can put a controller lane back or take one away, so the picker is
    // rebuilt rather than trusted.
    refreshLaneSelector();
    updateActionState();
    if (m_view) {
        m_view->update();
    }
}

void PianoRollWindow::refreshPlayhead() {
    if (m_view) m_view->refreshPlayheadFrame();
}

void PianoRollWindow::setLivePitches(const std::bitset<128>& pitches) {
    if (m_view) m_view->setLivePitches(pitches);
}

bool PianoRollWindow::livePitchHeldForTest(int pitch) const {
    return m_view && pitch >= 0 && pitch < 128 &&
           m_view->m_livePitches.test(std::size_t(pitch));
}

bool PianoRollWindow::checkInteractionGesturesForTest() {
    if (!m_view || !m_hScroll) return false;
    auto* heightControl =
        findChild<QToolButton*>(QStringLiteral("NoteHeightScrubber"));
    auto* navigator = dynamic_cast<PianoRollNavigator*>(m_hScroll);
    if (!heightControl || !navigator) return false;

    const auto hasShortcut = [](const QAction* action,
                                const QKeySequence& wanted) {
        return action && action->shortcuts().contains(wanted);
    };
    const bool editShortcuts =
        findChild<QAction*>(QStringLiteral("pianoRoll.edit.cut")) &&
        findChild<QAction*>(QStringLiteral("pianoRoll.edit.copy")) &&
        findChild<QAction*>(QStringLiteral("pianoRoll.edit.paste")) &&
        findChild<QAction*>(QStringLiteral("pianoRoll.edit.delete")) &&
        hasShortcut(m_repeatAction, QKeySequence(Qt::CTRL | Qt::Key_B));
    const bool toolShortcuts =
        hasShortcut(m_quantizeAction, QKeySequence(Qt::ALT | Qt::Key_Q)) &&
        hasShortcut(m_arpAction, QKeySequence(Qt::ALT | Qt::Key_A)) &&
        hasShortcut(m_chordAction, QKeySequence(Qt::ALT | Qt::Key_B)) &&
        hasShortcut(m_glueAction, QKeySequence(Qt::ALT | Qt::Key_G)) &&
        hasShortcut(m_strumAction, QKeySequence(Qt::ALT | Qt::Key_S)) &&
        hasShortcut(m_articulateAction, QKeySequence(Qt::ALT | Qt::Key_T)) &&
        hasShortcut(m_randomAction, QKeySequence(Qt::ALT | Qt::Key_R));
    const bool pinnedControls =
        findChild<QToolButton*>(QStringLiteral("PianoRollBuildChordsButton")) &&
        m_trackMuteButton && m_trackSoloButton;

    // Build every MIDI Tool dialog and verify that numeric input is uniformly
    // slider-based, with one exact live read-out per slider. This guards both
    // top-level parameters and the arpeggiator's per-step values.
    for (QAction* action : {m_quantizeAction, m_arpAction, m_chordAction,
                            m_glueAction, m_strumAction, m_articulateAction,
                            m_randomAction}) {
        if (action) action->trigger();
    }
    const QList<ToolDialog*> toolDialogs = {
        m_quantizeDialog, m_arpDialog,       m_chordDialog, m_glueDialog,
        m_strumDialog,    m_articulateDialog, m_randomDialog};
    const bool numericToolControls =
        std::all_of(toolDialogs.begin(), toolDialogs.end(), [](ToolDialog* dialog) {
            if (!dialog || !dialog->findChildren<QAbstractSpinBox*>().isEmpty())
                return false;
            const auto sliders = dialog->findChildren<QSlider*>();
            const auto readouts = dialog->findChildren<QLabel*>();
            const int numericReadouts =
                int(std::count_if(readouts.begin(), readouts.end(), [](QLabel* label) {
                    return label->property("midiNumericReadout").toBool();
                }));
            return !sliders.isEmpty() &&
                   std::all_of(sliders.begin(), sliders.end(), [](QSlider* slider) {
                       return slider->property("midiNumericSlider").toBool();
                   }) &&
                   numericReadouts == sliders.size();
        });

    // Three parameter notifications in one event-loop turn retain only the last
    // request and execute one transform. flushToolPreview is the same path Apply
    // uses, so this also verifies that an immediate click cannot commit the
    // previous frame's parameters.
    cancelToolPreview();
    const std::size_t previewRunsBefore = m_coalescedToolPreviewRuns;
    bool previewSignalsInvoked = m_quantizeDialog != nullptr;
    for (int i = 0; i < 3 && previewSignalsInvoked; ++i) {
        previewSignalsInvoked = QMetaObject::invokeMethod(
            m_quantizeDialog, "paramsChanged", Qt::DirectConnection);
    }
    const bool previewWasQueued =
        previewSignalsInvoked && m_toolPreviewTimer &&
        m_toolPreviewTimer->isActive() &&
        m_pendingPreviewOwner.data() == m_quantizeDialog;
    const bool previewFlushed = flushToolPreview(m_quantizeDialog);
    const bool coalescedToolPreview =
        previewWasQueued && previewFlushed &&
        m_coalescedToolPreviewRuns == previewRunsBefore + 1 &&
        m_previewOwner == m_quantizeDialog && m_view->hasPreview();
    for (ToolDialog* dialog : toolDialogs) {
        if (dialog) dialog->close();
    }
    m_view->clearPreview();
    m_previewOwner = nullptr;

    const double originalHeight = m_view->rowHeight();
    m_view->setRowHeight(12.0);
    const QPoint start = heightControl->rect().center();
    const QPoint finish = start - QPoint(0, 24);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                      QPointF(heightControl->mapToGlobal(start)), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(heightControl, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish),
                     QPointF(heightControl->mapToGlobal(finish)), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(heightControl, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(finish),
                        QPointF(heightControl->mapToGlobal(finish)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(heightControl, &release);
    const bool heightDrag = m_view->rowHeight() > 12.0;
    m_view->setRowHeight(originalHeight);

    // The lower edge changes only the navigator's thickness.
    const int originalNavigatorHeight = navigator->height();
    const QPointF navHeightStart(navigator->width() * 0.5,
                                 navigator->height() - 1.0);
    const QPointF navHeightGlobal =
        navigator->mapToGlobal(navHeightStart.toPoint());
    const QPointF navHeightFinishGlobal = navHeightGlobal + QPointF(0.0, 12.0);
    QMouseEvent navHeightPress(
        QEvent::MouseButtonPress, navHeightStart, navHeightGlobal,
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(navigator, &navHeightPress);
    QMouseEvent navHeightMove(
        QEvent::MouseMove,
        QPointF(navigator->mapFromGlobal(navHeightFinishGlobal.toPoint())),
        navHeightFinishGlobal, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(navigator, &navHeightMove);
    const bool navigatorHeightDrag =
        navigator->height() > originalNavigatorHeight;
    QMouseEvent navHeightRelease(
        QEvent::MouseButtonRelease,
        QPointF(navigator->mapFromGlobal(navHeightFinishGlobal.toPoint())),
        navHeightFinishGlobal, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(navigator, &navHeightRelease);
    navigator->setNavigatorHeight(originalNavigatorHeight);

    // Pulling the thumb's right bracket left zooms in while the view centre is
    // preserved by PianoRollView.
    const double originalPx = m_view->pixelsPerBeat();
    const double originalScrollX = m_view->scrollX();
    m_view->setPixelsPerBeat(240.0);
    m_view->setScrollX(std::min(120.0, m_view->maxScrollX()));
    updateScrollBars();
    QApplication::processEvents();
    const QRect thumb = navigator->thumbRectForTest();
    const QPointF zoomStart(thumb.right() - 1.0, thumb.center().y());
    const QPointF zoomGlobal = navigator->mapToGlobal(zoomStart.toPoint());
    const QPointF zoomFinishGlobal = zoomGlobal - QPointF(48.0, 0.0);
    QMouseEvent zoomPress(QEvent::MouseButtonPress, zoomStart, zoomGlobal,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(navigator, &zoomPress);
    QMouseEvent zoomMove(
        QEvent::MouseMove,
        QPointF(navigator->mapFromGlobal(zoomFinishGlobal.toPoint())),
        zoomFinishGlobal, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(navigator, &zoomMove);
    QMouseEvent zoomRelease(
        QEvent::MouseButtonRelease,
        QPointF(navigator->mapFromGlobal(zoomFinishGlobal.toPoint())),
        zoomFinishGlobal, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(navigator, &zoomRelease);
    const bool navigatorZoom = m_view->effectivePixelsPerBeat() > 240.0;
    const bool navigatorAnchoredAtStart = std::abs(m_view->scrollX()) < 1e-9;
    m_view->setPixelsPerBeat(originalPx);
    m_view->setScrollX(originalScrollX);

    const bool scrollAboveGrid = m_hScroll->geometry().bottom() <=
                                 m_view->geometry().top();
    return editShortcuts && toolShortcuts && pinnedControls && heightDrag &&
           numericToolControls && coalescedToolPreview &&
           navigatorHeightDrag && navigatorZoom &&
           navigatorAnchoredAtStart &&
           scrollAboveGrid &&
           m_view->checkInteractionGesturesForTest();
}

void PianoRollWindow::updateScrollBars() {
    if (!m_view || !m_hScroll || !m_vScroll) return;
    // Blocked, or setting the range would fire valueChanged straight back into
    // the view and fight whatever the user is doing.
    const QSignalBlocker blockH(m_hScroll);
    const QSignalBlocker blockV(m_vScroll);
    m_hScroll->setRange(0, int(std::ceil(m_view->maxScrollX())));
    m_hScroll->setPageStep(std::max(1, m_view->width()));
    m_hScroll->setValue(int(std::lround(m_view->scrollX())));
    m_vScroll->setRange(0, int(std::ceil(m_view->maxScrollY())));
    m_vScroll->setPageStep(std::max(1, m_view->height()));
    m_vScroll->setValue(int(std::lround(m_view->scrollY())));
}

void PianoRollWindow::updateActionState() {
    if (m_undoAction) {
        m_undoAction->setEnabled(m_controller->canUndo());
        const std::string label = m_controller->undoLabel();
        m_undoAction->setText(label.empty()
                                  ? tr("Undo")
                                  : tr("Undo %1").arg(ui::translatedUndoLabel(label)));
    }
    if (m_redoAction) {
        m_redoAction->setEnabled(m_controller->canRedo());
        const std::string label = m_controller->redoLabel();
        m_redoAction->setText(label.empty()
                                  ? tr("Redo")
                                  : tr("Redo %1").arg(ui::translatedUndoLabel(label)));
    }
    if (m_pasteAction) m_pasteAction->setEnabled(m_view->canPaste());
    const auto* track = m_controller
                            ? m_controller->project().findTrack(
                                  m_trackId.toStdString())
                            : nullptr;
    if (m_trackMuteButton) {
        const QSignalBlocker block(m_trackMuteButton);
        m_trackMuteButton->setEnabled(track != nullptr);
        m_trackMuteButton->setChecked(track && track->muted);
    }
    if (m_trackSoloButton) {
        const QSignalBlocker block(m_trackSoloButton);
        m_trackSoloButton->setEnabled(track != nullptr);
        m_trackSoloButton->setChecked(track && track->soloed);
    }
    updateScrollBars();
}

void PianoRollWindow::updateTitle() {
    // In the title bar rather than on a line of its own inside the window: a
    // window saying what it is editing is what a title bar is for, and the line
    // it used to occupy is now the context panel's.
    const auto* track =
        m_controller ? m_controller->project().findTrack(m_trackId.toStdString())
                     : nullptr;
    if (!track) {
        setWindowTitle(tr("Piano Roll"));
        return;
    }
    const std::string clipId = m_clipId.toStdString();
    for (const auto& c : track->clips) {
        if (c.id != clipId) continue;
        QString label = QString("%1 — %2")
                            .arg(QString::fromStdString(track->name))
                            .arg(QString::fromStdString(c.name));
        // The instrument is what sounds these notes, so the editor names it
        // rather than leaving the question open.
        label += track->instrument.name.empty()
                     ? tr("  ·  no instrument")
                     : QString("  ·  %1").arg(
                           QString::fromStdString(track->instrument.name));
        setWindowTitle(tr("Piano Roll — %1").arg(label));
        return;
    }
    setWindowTitle(tr("Piano Roll"));
}
