#include "ScrollMotion.hpp"
#include "PianoRollWindow.hpp"
#include "SlideCurveEditor.hpp"
#include "SlideNotes.hpp"
#include "MenuActions.hpp"
#include <QScopedValueRollback>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSpinBox>
#include <QIODevice>
#include "Controls.hpp"
#include "Icons.hpp"
#include "FileTypes.hpp"
#include "KeyboardLayout.hpp"
#include "NoteContextPanel.hpp"
#include "EngineController.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"
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
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QMessageBox>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QHelpEvent>
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
#include <QStyleOptionComboBox>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
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
const daw::TrackModel* containingPattern(const daw::ProjectModel& project,
                                         const daw::TrackModel* track) {
    for (size_t depth = 0; track && !track->parentId.empty() &&
                           depth < project.tracks.size(); ++depth) {
        track = project.findTrack(track->parentId);
        if (track && track->kind == daw::TrackKind::Pattern) return track;
    }
    return nullptr;
}
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

bool hasPrimarySelectionModifier(Qt::KeyboardModifiers modifiers) {
#if defined(Q_OS_MACOS)
    return modifiers.testFlag(Qt::ControlModifier) ||
           modifiers.testFlag(Qt::MetaModifier);
#else
    return modifiers.testFlag(Qt::ControlModifier);
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

constexpr int kPianoZoomSteps = 1000;
double pianoZoomFromSlider(int value) {
    return kMinPxPerBeat * std::pow(kMaxPxPerBeat / kMinPxPerBeat,
                                    double(value) / kPianoZoomSteps);
}
int pianoZoomToSlider(double pixels) {
    return int(std::lround(kPianoZoomSteps *
        std::log(std::clamp(pixels, kMinPxPerBeat, kMaxPxPerBeat) / kMinPxPerBeat) /
        std::log(kMaxPxPerBeat / kMinPxPerBeat)));
}

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
struct NoteClipboard {
    mt::Notes notes;
    double rangeLength = 0.0;
    std::vector<daw::SlideNoteModel> slides;
};

NoteClipboard& clipboard() {
    static NoteClipboard contents;
    return contents;
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

QPainterPath roundedPlayheadTriangle(QPointF a, QPointF b, QPointF c,
                                     qreal radius) {
    const auto step = [](const QPointF& from, const QPointF& to, qreal distance) {
        const QPointF vector = to - from;
        const qreal length = std::sqrt(QPointF::dotProduct(vector, vector));
        return length > 0.0 ? from + vector * (distance / length) : from;
    };
    QPainterPath path;
    path.moveTo(step(a, c, radius));
    path.quadTo(a, step(a, b, radius));
    path.lineTo(step(b, a, radius));
    path.quadTo(b, step(b, c, radius));
    path.lineTo(step(c, b, radius));
    path.quadTo(c, step(c, a, radius));
    path.closeSubpath();
    return path;
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
    : ui::FrameWidget(parent), m_controller(controller) {
    setMouseTracking(true);
    setAcceptDrops(true);
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
        m_controller->endNoteEdit(m_eraseChanged
                                      ? "Erase Notes"
                                      : (m_duplicateDragCreated
                                             ? "Duplicate Notes"
                                             : "Edit Notes"));
    }
    stopAudition();
}

QString PianoRollView::samplerDropPath(const QMimeData* mime) const {
    if (!m_controller || !clip() || !mime || !mime->hasUrls()) return {};
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    if (!track || track->instrument.uid != "daw.sampler" ||
        track->instrument.format != daw::PluginFormat::Internal ||
        track->instrument.id.empty()) return {};
    const auto urls = mime->urls();
    if (urls.size() != 1 || !urls.front().isLocalFile()) return {};
    const QString path = urls.front().toLocalFile();
    return ui::isAudioFile(path) ? path : QString{};
}

bool PianoRollView::inNoteGrid(const QPointF& point) const {
    return point.x() >= keyboardWidth() && point.x() < width() &&
           point.y() >= ui::kRulerHeight && point.y() < laneTop();
}

void PianoRollView::dragEnterEvent(QDragEnterEvent* event) {
    const QString path = samplerDropPath(event->mimeData());
    if (path.isEmpty() || !(event->possibleActions() & Qt::CopyAction)) {
        event->ignore();
        return;
    }
    m_sampleDropName = inNoteGrid(event->position()) ? QFileInfo(path).fileName() : QString{};
    event->setDropAction(Qt::CopyAction);
    event->accept();
    ui::FrameWidget::update();
}

void PianoRollView::dragMoveEvent(QDragMoveEvent* event) {
    const QString path = samplerDropPath(event->mimeData());
    const bool allowed = !path.isEmpty() && inNoteGrid(event->position()) &&
                         (event->possibleActions() & Qt::CopyAction);
    const QString name = allowed ? QFileInfo(path).fileName() : QString{};
    if (m_sampleDropName != name) {
        m_sampleDropName = name;
        ui::FrameWidget::update();
    }
    if (allowed) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else event->ignore();
}

void PianoRollView::dragLeaveEvent(QDragLeaveEvent* event) {
    m_sampleDropName.clear();
    ui::FrameWidget::update();
    event->accept();
}

void PianoRollView::dropEvent(QDropEvent* event) {
    m_sampleDropName.clear();
    ui::FrameWidget::update();
    const QString path = samplerDropPath(event->mimeData());
    if (path.isEmpty() || !inNoteGrid(event->position()) ||
        !(event->possibleActions() & Qt::CopyAction)) {
        event->ignore();
        return;
    }
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    // Resolve the open clip's instrument, never the arrangement's selection.
    const std::string trackId = track->id, slotId = track->instrument.id;
    if (!m_controller->loadSamplerSample(trackId, slotId, path.toStdString())) {
        emit statusChanged(tr("Could not load sample: %1").arg(QFileInfo(path).fileName()));
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    emit edited();
    emit statusChanged(tr("Sample replaced: %1 — sampler settings kept")
                       .arg(QFileInfo(path).fileName()));
}

void PianoRollView::setClip(const QString& trackId, const QString& clipId) {
    m_sampleDropName.clear();
    if (m_trackId != trackId || m_clipId != clipId) { cancelSlideGesture(); if (m_slideEditor) m_slideEditor->close(); m_selectedSlide.clear(); }
    // Whatever the keyboard is sounding belongs to the track being left.
    finishWheelNoteEdit();
    rememberNoteProperties(note(m_primary));
    commitPendingErase();
    if (m_gestureUndoActive || m_selectionEditUndoActive) {
        m_controller->endNoteEdit(m_eraseChanged
                                      ? "Erase Notes"
                                      : (m_duplicateDragCreated
                                             ? "Duplicate Notes"
                                             : "Edit Notes"));
        m_gestureUndoActive = false;
        m_selectionEditUndoActive = false;
    }
    stopAudition();
    cancelControllerLaneWrite();
    m_pressedKey = -1;
    m_paintClip = nullptr;
    if (m_trackId != trackId || m_clipId != clipId) {
        if (!m_clipId.isEmpty()) {
            if (m_timeRange.valid()) m_clipTimeRanges.insert(m_clipId, m_timeRange);
            else m_clipTimeRanges.remove(m_clipId);
        }
        m_timeRange = m_clipTimeRanges.value(clipId);
    }
    m_trackId = trackId;
    m_clipId = clipId;
    m_lastPlayheadX = -1;
    m_lastSoundingPitches.reset();
    invalidateSoundingPitchIndex();
    invalidateDocumentPaintCaches();
    m_selected.clear();
    m_primary.clear();
    m_moving = m_resizing = m_resizingLeft = false;
    m_resizeOrig.clear();
    m_moveWorking.clear();
    m_duplicateDragPending = false;
    m_duplicateDragCreated = false;
    m_geometryPaintNotes.clear();
    m_noteUpdateScratch.clear();
    m_laneOrig.clear();
    m_laneDragging = m_laneRamping = m_marquee = m_erasing = m_muting = false;
    m_scrubbingPlayhead = false;
    m_rangeGrab = RangeGrab::None;
    m_pointerButton = Qt::NoButton;
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
    if (found != m_noteById.end() && found->second < daw::midiNotes(*c).size() &&
        daw::midiNotes(*c)[found->second].id == id)
        return &daw::midiNotes(*c)[found->second];
    return nullptr;
}

const mt::Notes& PianoRollView::visibleNotes() const {
    static const mt::Notes empty;
    if (m_preview) return *m_preview;
    const auto* c = clip();
    return c ? daw::midiNotes(*c) : empty;
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

double PianoRollView::noteStartBeats(double beats, bool enabled) const {
    const double grid = effectiveGridBeats();
    if (!enabled || grid <= 0.0) return std::max(0.0, beats);
    // Placement belongs to the clicked cell, including its right half. Swing
    // shifts odd grid lines, so compare with the actual visible boundary.
    const double slot = std::floor(std::max(0.0, beats) / grid + 1e-9);
    const double boundary = snapBeats(slot * grid, true);
    return boundary > beats + grid * 1e-9
        ? snapBeats((slot - 1.0) * grid, true) : boundary;
}

void PianoRollView::rememberNoteProperties(const daw::NoteModel* source) {
    if (!source) return;
    m_lastLength = std::max(kMinNoteBeats, source->lengthBeats);
    m_lastVelocity = std::clamp(source->velocity, 1, 127);
    m_lastPan = std::clamp(source->pan, -1.0f, 1.0f);
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
    if (param != m_laneParam || laneId != m_laneId)
        finishInterruptedPointerGesture();
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
    for (const auto& lane : daw::midiLanes(*c)) {
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
    m_lastPlayheadX = -1;
    ui::FrameWidget::update();
}

void PianoRollView::setScrollY(double y) {
    m_scrollY = std::clamp(y, 0.0, maxScrollY());
    ui::FrameWidget::update();
}

bool PianoRollView::checkAuditionForTest() {
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Audition");
    const auto other = controller.addTrack(daw::TrackKind::Midi, "Other instrument");
    const auto clipId = controller.addMidiClip(track, 0, 8);
    const auto otherClip = controller.addMidiClip(other, 0, 8);
    PianoRollView view(&controller);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(800, 400);
    view.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    view.show();
    QApplication::processEvents();
    view.m_tool = Tool::Draw;
    view.m_showKeyboard = true;
    view.m_showVelocityLane = false;
    view.m_pxPerBeat = 100;
    view.m_rowHeight = 16;
    view.m_scrollX = 0;
    view.m_scrollY = (kMaxPitch - 66) * view.m_rowHeight - 170;
    view.m_snapEnabled = true;
    view.m_adaptiveSnap = false;
    view.m_gridBeats = 0.25;
    view.m_swing = 0.5;
    view.m_lastLength = 0.5;
    view.m_lastVelocity = 83;
    view.m_snapToScale = false;
    const auto savedClipboard = clipboard();
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s MIDI audition: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    // Drain the real per-track MIDI source, with transport stopped. Checking
    // emitted events also catches accidental sound from snapshot publication.
    const auto events = [&](const std::string& id) {
        daw::engine::MidiBuffer buffer;
        buffer.reserve(daw::engine::kMidiEventsPerBlock);
        daw::engine::ProcessContext context;
        context.frames = 512;
        context.sampleRate = 48000;
        context.transport.tempo = 120;
        context.playing = false;
        context.midiOutput = &buffer;
        const auto graph = controller.routingGraph();
        const auto* ids = controller.trackNodes(id);
        daw::engine::MidiClipPlayerNode* player = nullptr;
        if (graph && ids) {
            for (const auto& entry : graph->nodes)
                if (entry.id == ids->midiClips)
                    player = dynamic_cast<daw::engine::MidiClipPlayerNode*>(entry.node);
        }
        check(player != nullptr, "track MIDI source exists");
        if (player) player->process(context);
        return std::vector<daw::engine::MidiEvent>(buffer.events().begin(),
                                                  buffer.events().end());
    };
    const auto one = [&](const std::string& id, int pitch, bool on, bool audition = true) {
        const auto result = events(id);
        return result.size() == 1 && result.front().data1 == pitch &&
            (audition ? result.front().noteId >= 0 : result.front().noteId < 0) &&
            (on ? result.front().isNoteOn() && result.front().data2 == 83
                : result.front().isNoteOff() && result.front().isNoteChoke == audition);
    };
    const auto point = [&](double beat, int pitch) {
        return QPointF(view.beatsToX(beat),
                       view.pitchToY(pitch) + view.m_rowHeight * 0.5);
    };
    const auto mouse = [&](QEvent::Type type, QPointF pos,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                           Qt::MouseButtons buttons = Qt::LeftButton) {
        QMouseEvent event(type, pos, view.mapToGlobal(pos),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : buttons, modifiers);
        QApplication::sendEvent(&view, &event);
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers);
        QApplication::sendEvent(&view, &event);
    };
    mouse(QEvent::MouseMove, point(0.5, 60), Qt::NoModifier, Qt::NoButton);
    check(events(track).empty(), "hover is silent");
    mouse(QEvent::MouseButtonPress, point(0.5, 60));
    const QString first = view.m_primary;
    check(one(track, 60, true) && events(other).empty() && !view.m_auditionPerformance,
          "placing a note sounds only its clip's instrument at the drawn velocity");
    bool stillHeld = true;
    for (int block = 0; block < 30; ++block) stillHeld &= events(track).empty();
    check(stillHeld && view.m_auditionPitch == 60,
          "holding past the written note length does not schedule a preview note-off");
    mouse(QEvent::MouseButtonRelease, point(0.5, 60));
    check(one(track, 60, false), "release ends the drawn note preview");
    mouse(QEvent::MouseButtonPress, point(2, 62));
    check(one(track, 62, true), "a second placement starts immediately");
    mouse(QEvent::MouseMove, point(2.5, 62));
    check(events(track).empty(), "horizontal placement does not retrigger the same pitch");
    mouse(QEvent::MouseMove, point(3, 65));
    const auto changed = events(track);
    check(changed.size() == 2 && changed[0].isNoteOff() && changed[0].data1 == 62 &&
          changed[1].isNoteOn() && changed[1].data1 == 65,
          "changing pitch during placement releases the old preview");
    mouse(QEvent::MouseButtonRelease, point(3, 65));
    check(one(track, 65, false), "release ends the final placement pitch");
    mouse(QEvent::MouseButtonPress, point(3.5, 67));
    check(one(track, 67, true), "a coalesced-release placement starts its preview");
    mouse(QEvent::MouseButtonRelease, point(3.75, 69));
    check(one(track, 67, false) && view.note(view.m_primary)->pitch == 69,
          "release commits the final pitch without sounding a new preview");

    controller.play();
    mouse(QEvent::MouseButtonPress, point(6, 72));
    const QString duringPlay = view.m_primary;
    check(controller.isPlaying() && events(track).empty() && view.m_auditionPitch < 0,
          "placing a note during Play is silent");
    mouse(QEvent::MouseMove, point(6.5, 73));
    mouse(QEvent::MouseButtonRelease, point(6.75, 74));
    check(events(track).empty() && view.note(duringPlay)->pitch == 74,
          "placement and its final pitch still edit immediately during Play");
    const QPointF playingKey(20, point(0, 60).y());
    mouse(QEvent::MouseButtonPress, playingKey);
    check(one(track, 60, true, false), "piano keys remain a performance input during Play");
    view.refreshPlayheadFrame();
    check(events(track).empty(), "transport updates do not cut a performed piano key");
    mouse(QEvent::MouseButtonRelease, playingKey);
    check(one(track, 60, false, false), "performed key release keeps ordinary MIDI semantics");

    controller.pause();
    mouse(QEvent::MouseButtonPress, point(6, 70));
    check(one(track, 70, true), "a paused transport allows held note preview");
    controller.play();
    view.refreshPlayheadFrame();
    check(one(track, 70, false) && view.m_auditionPitch < 0,
          "starting Play cuts a held placement preview");
    mouse(QEvent::MouseMove, point(6.25, 70));
    mouse(QEvent::MouseButtonRelease, point(6.25, 70));
    check(events(track).empty(), "movement and release cannot restart preview during Play");
    controller.stop();
    mouse(QEvent::MouseButtonPress, point(7, 71));
    check(one(track, 71, true), "a stopped transport allows preview again");
    mouse(QEvent::MouseButtonRelease, point(7, 71));
    check(one(track, 71, false), "the resumed preview still ends on release");

    auto body = view.noteRect(*view.note(first)).center();
    mouse(QEvent::MouseButtonPress, body);
    check(events(track).empty(), "selecting an existing note is silent");
    mouse(QEvent::MouseMove, body + QPointF(50, -16));
    mouse(QEvent::MouseButtonRelease, body + QPointF(50, -16));
    check(view.note(first)->pitch == 61 && events(track).empty(),
          "moving an existing note is silent");
    auto rect = view.noteRect(*view.note(first));
    const QPointF edge(rect.right() - 1, rect.center().y());
    mouse(QEvent::MouseButtonPress, edge);
    mouse(QEvent::MouseMove, edge + QPointF(25, 0));
    mouse(QEvent::MouseButtonRelease, edge + QPointF(25, 0));
    check(view.noteRect(*view.note(first)).width() > rect.width() && events(track).empty(),
          "resizing an existing note is silent");
    body = view.noteRect(*view.note(first)).center();
    const auto beforeDuplicate = daw::midiNotes(*view.clip()).size();
    mouse(QEvent::MouseButtonPress, body, Qt::ShiftModifier);
    mouse(QEvent::MouseMove, body + QPointF(50, -32), Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, body + QPointF(50, -32), Qt::ShiftModifier);
    check(daw::midiNotes(*view.clip()).size() == beforeDuplicate + 1 && events(track).empty(),
          "Shift-drag duplication is silent");
    for (int code : {Qt::Key_C, Qt::Key_V, Qt::Key_B, Qt::Key_X}) {
        const auto before = daw::midiNotes(*view.clip()).size();
        const auto selected = std::size_t(view.m_selected.size());
        key(code);
        const auto after = daw::midiNotes(*view.clip()).size();
        const bool performed = selected > 0 &&
            (code == Qt::Key_C ? clipboard().notes.size() == selected :
             code == Qt::Key_X ? after + selected == before : after == before + selected);
        check(performed && events(track).empty(),
              "copy, paste, duplicate and cut shortcuts are silent");
    }
    controller.undo();
    controller.redo();
    check(events(track).empty(), "undo and redo do not audition notes");

    const QPointF piano(20, point(0, 60).y());
    mouse(QEvent::MouseButtonPress, piano);
    check(one(track, 60, true, false), "the onscreen piano keyboard still sounds");
    mouse(QEvent::MouseMove, piano - QPointF(0, 16));
    const auto keys = events(track);
    check(keys.size() == 2 && keys[0].isNoteOff() && keys[0].data1 == 60 &&
          keys[1].isNoteOn() && keys[1].data1 == 61,
          "dragging piano keys changes the heard pitch");
    mouse(QEvent::MouseButtonRelease, piano - QPointF(0, 16));
    check(one(track, 61, false, false), "piano key release ends its preview");

    for (QEvent::Type type : {QEvent::FocusOut, QEvent::UngrabMouse,
                              QEvent::WindowDeactivate, QEvent::Hide}) {
        mouse(QEvent::MouseButtonPress, point(4, 70));
        check(one(track, 70, true), "interrupted placement starts its preview");
        if (type == QEvent::FocusOut) {
            QFocusEvent interrupted(type, Qt::OtherFocusReason);
            QApplication::sendEvent(&view, &interrupted);
        } else if (type == QEvent::Hide) {
            QHideEvent interrupted;
            QApplication::sendEvent(&view, &interrupted);
        } else {
            QEvent interrupted(type);
            QApplication::sendEvent(&view, &interrupted);
        }
        check(one(track, 70, false) && !view.hasActivePointerGesture(),
              "focus loss, lost grab, deactivation and hiding release the preview");
        controller.undo(); // Keep the next test point empty.
    }
    mouse(QEvent::MouseButtonPress, point(4, 70));
    check(one(track, 70, true), "placement before clip switch sounds");
    view.setClip(QString::fromStdString(other), QString::fromStdString(otherClip));
    check(one(track, 70, false) && events(other).empty(),
          "switching clips releases the previous instrument");
    mouse(QEvent::MouseButtonPress, point(1, 72));
    check(one(other, 72, true) && events(track).empty(),
          "placement after switching uses the new clip's instrument");
    mouse(QEvent::MouseMove, point(1, 72), Qt::NoModifier, Qt::NoButton);
    check(one(other, 72, false), "a missed mouse release cannot leave a note ringing");
    {
        PianoRollView closing(&controller);
        closing.setClip(QString::fromStdString(other), QString::fromStdString(otherClip));
        closing.m_lastVelocity = 83;
        closing.auditionPitch(75);
        check(one(other, 75, true), "preview before destruction sounds");
    }
    check(one(other, 75, false), "destroying the view releases its preview");
    clipboard() = savedClipboard;
    return ok;
}

bool PianoRollView::checkInteractionGesturesForTest() {
    const auto* current = clip();
    if (!current || width() < 200 || height() < 160) return false;

    const mt::Notes originalNotes = daw::midiNotes(*current);
    const QSet<QString> originalSelection = m_selected;
    const QString originalPrimary = m_primary;
    const Tool originalTool = m_tool;
    const double originalGrid = m_gridBeats;
    const bool originalSnap = m_snapEnabled;
    const bool originalAdaptive = m_adaptiveSnap;
    const bool originalLane = m_showVelocityLane;
    const LaneParam originalLaneParam = m_laneParam;
    const QString originalLaneId = m_laneId;
    const double originalPx = m_pxPerBeat;
    const double originalRow = m_rowHeight;
    const double originalScrollX = m_scrollX;
    const double originalScrollY = m_scrollY;
    const double originalLastLength = m_lastLength;
    const int originalLastVelocity = m_lastVelocity;
    const float originalLastPan = m_lastPan;
    const double originalSwing = m_swing;
    const bool originalSnapToScale = m_snapToScale;
    const auto originalPreview = m_preview;
    const QSet<QString> originalPreviewSelection = m_previewSelection;
    const bool originalPreviewWholeClip = m_previewWholeClip;
    const NoteStyle originalNoteStyle = m_noteStyle;
    const bool originalNoteBorders = m_noteBorders;
    const auto originalClipboard = clipboard();
    const TimeRange originalTimeRange = m_timeRange;
    const std::size_t undoStart = m_controller->undoDepth();
    const double originalTransportPosition = m_controller->positionSeconds();
    const double originalLoopStart = m_controller->loopStartSeconds();
    const double originalLoopEnd = m_controller->loopEndSeconds();
    const bool originalLoopEnabled = m_controller->isLoopEnabled();

    const auto lostReleaseMouse = [&](QEvent::Type type, QPointF pos,
                             Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, pos, mapToGlobal(pos), button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(this, &event);
    };
    const QPointF ruler(keyboardWidth() + 24.0, ui::kLoopStripHeight + 4.0);
    lostReleaseMouse(QEvent::MouseButtonPress, ruler, Qt::LeftButton, Qt::LeftButton);
    lostReleaseMouse(QEvent::MouseMove, ruler + QPointF(30, 0), Qt::NoButton,
            Qt::LeftButton);
    const double scrubEndpoint = m_controller->positionSeconds();
    lostReleaseMouse(QEvent::MouseMove, ruler + QPointF(70, 0), Qt::NoButton,
            Qt::NoButton);
    lostReleaseMouse(QEvent::MouseMove, ruler + QPointF(90, 0), Qt::NoButton,
            Qt::NoButton);
    const bool interruptedScrubStops = !m_scrubbingPlayhead &&
        std::abs(m_controller->positionSeconds() - scrubEndpoint) < 1e-9;
    m_marquee = true;
    m_marqueeOrigin = QPointF(90, 65);
    m_marqueeCurrent = QPointF(130, 80);
    m_pointerButton = Qt::LeftButton;
    m_lastPointerPosition = m_marqueeCurrent;
    lostReleaseMouse(QEvent::MouseMove, QPointF(200, 100), Qt::NoButton,
            Qt::NoButton);
    const bool interruptedSelectionStops = !m_marquee &&
        m_marqueeCurrent == QPointF(130, 80);

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

    m_controller->seekSeconds(current->startSeconds +
        daw::beatsToSeconds(0.37123, m_controller->project().tempo));
    refreshPlayheadFrame();
    const double precisePlayheadX = beatsToX(daw::secondsToBeats(
        m_controller->positionSeconds() - current->startSeconds,
        m_controller->project().tempo));
    const bool fractionalPlayhead = std::abs(m_lastPlayheadX - precisePlayheadX) < 1e-8 &&
        std::abs(m_lastPlayheadX - std::round(m_lastPlayheadX)) > 0.01;
    m_controller->seekSeconds(expectedSeek);

    // Ruler positioning follows the same visible grid as note edits. Use an
    // off-grid click so this exercises the pointer route, not just conversion.
    m_gridBeats = 0.25;
    m_snapEnabled = true;
    constexpr double rawRulerBeat = 0.63;
    const QPointF rulerAt(beatsToX(rawRulerBeat),
                          (ui::kLoopStripHeight + ui::kRulerHeight) * 0.5);
    QMouseEvent rulerPress(QEvent::MouseButtonPress, rulerAt,
                           QPointF(mapToGlobal(rulerAt.toPoint())),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &rulerPress);
    QMouseEvent rulerRelease(QEvent::MouseButtonRelease, rulerAt,
                             QPointF(mapToGlobal(rulerAt.toPoint())),
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &rulerRelease);
    const double expectedRulerSeek =
        current->startSeconds +
        daw::beatsToSeconds(snapBeats(rawRulerBeat, true),
                            m_controller->project().tempo);
    const bool rulerSeekSnapped =
        std::abs(m_controller->positionSeconds() - expectedRulerSeek) < 1e-4;

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
    const bool eraseDeferred = clip() && daw::midiNotes(*clip()).size() == 3 &&
                               m_pendingErase.size() == 3;
    QMouseEvent rightRelease(QEvent::MouseButtonRelease, eraseTo,
                             QPointF(mapToGlobal(eraseTo.toPoint())),
                             Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &rightRelease);
    const bool sweptAll = clip() && daw::midiNotes(*clip()).empty();

    replaceNotes(eraseNotes, "Prepare Left Eraser Check");
    setTool(Tool::Erase);
    QMouseEvent ctrlSelectPress(QEvent::MouseButtonPress, blankAt,
                                QPointF(mapToGlobal(blankAt.toPoint())),
                                Qt::LeftButton, Qt::LeftButton,
                                Qt::ControlModifier);
    QApplication::sendEvent(this, &ctrlSelectPress);
    const bool ctrlBorrowsSelection = m_marquee && !m_erasing &&
        clip() && daw::midiNotes(*clip()).size() == 3;
    QMouseEvent ctrlSelectRelease(QEvent::MouseButtonRelease, blankAt,
                                  QPointF(mapToGlobal(blankAt.toPoint())),
                                  Qt::LeftButton, Qt::NoButton,
                                  Qt::ControlModifier);
    QApplication::sendEvent(this, &ctrlSelectRelease);
    QMouseEvent leftErasePress(QEvent::MouseButtonPress, eraseFrom,
                               QPointF(mapToGlobal(eraseFrom.toPoint())),
                               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &leftErasePress);
    QMouseEvent leftEraseMove(QEvent::MouseMove, eraseTo,
                              QPointF(mapToGlobal(eraseTo.toPoint())),
                              Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &leftEraseMove);
    const bool leftEraseDeferred = clip() && daw::midiNotes(*clip()).size() == 3 &&
                                   m_pendingErase.size() == 3;
    QMouseEvent leftEraseRelease(QEvent::MouseButtonRelease, eraseTo,
                                 QPointF(mapToGlobal(eraseTo.toPoint())),
                                 Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(this, &leftEraseRelease);
    const bool leftSweptAll = clip() && daw::midiNotes(*clip()).empty();

    // A click anywhere inside a cell keeps the last length, even below the
    // active grid size. Pointer jitter must not trim that newly placed note.
    m_tool = Tool::Draw;
    m_gridBeats = 0.25;
    m_adaptiveSnap = false;
    m_swing = 0.5;
    m_snapToScale = false;
    m_lastLength = kMinNoteBeats;
    const QPointF drawAt(beatsToX(0.95), pitchToY(60) + m_rowHeight * 0.5);
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
    const bool brushSafe = clip() && daw::midiNotes(*clip()).size() == 1 &&
        std::abs(daw::midiNotes(*clip()).front().startBeats - 0.75) < 1e-9 &&
        std::abs(daw::midiNotes(*clip()).front().lengthBeats - kMinNoteBeats) < 1e-9;

    bool brushProperties = true;
    const auto brushCheck = [&](bool passed, const char* message) {
        if (!passed) std::fprintf(stderr, "piano-roll brush: %s\n", message);
        brushProperties &= passed;
    };
    replaceNotes({}, "Prepare Default Velocity Check");
    {
        PianoRollView fresh(m_controller);
        fresh.resize(640, 400);
        fresh.setClip(m_trackId, m_clipId);
        fresh.setTool(Tool::Draw);
        fresh.m_scrollY = (kMaxPitch - 60) * fresh.m_rowHeight - 100.0;
        const QPointF at(fresh.beatsToX(0.0), fresh.pitchToY(60) + fresh.m_rowHeight * 0.5);
        QMouseEvent press(QEvent::MouseButtonPress, at, fresh.mapToGlobal(at.toPoint()),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&fresh, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, fresh.mapToGlobal(at.toPoint()),
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&fresh, &release);
        brushCheck(daw::midiNotes(*clip()).size() == 1 && daw::midiNotes(*clip()).front().velocity == 127 &&
                   daw::midiNotes(*clip()).front().pan == 0.0f,
                   "a fresh piano roll draws at full velocity and centre pan");
    }
    const auto pointer = [&](QEvent::Type type, const QPointF& at,
                             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, at, QPointF(mapToGlobal(at.toPoint())),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            modifiers);
        QApplication::sendEvent(this, &event);
    };
    const auto gridPoint = [&](double beat, int pitch) {
        return QPointF(beatsToX(beat), pitchToY(pitch) + m_rowHeight * 0.5);
    };
    const auto clickGrid = [&](const QPointF& at) {
        pointer(QEvent::MouseButtonPress, at);
        pointer(QEvent::MouseButtonRelease, at);
    };
    replaceNotes({makeNote("brush-template", 0.25, 0.5)}, "Prepare Brush Check");
    m_selected.clear(); m_primary.clear();
    clickGrid(gridPoint(0.5, 60));
    beginSelectionVelocityEdit();
    setSelectionVelocity(127);
    endSelectionVelocityEdit();
    beginSelectionEdit();
    setSelectionPan(0.375f);
    setSelectionLength(0.125);
    endSelectionEdit(QStringLiteral("Brush Properties"));
    selectNone();
    const auto beforeDraw = m_controller->undoDepth();
    pointer(QEvent::MouseButtonPress, gridPoint(1.23, 62));
    const QString drawnId = m_primary;
    const auto drawnMatches = [&](double start, int pitch) {
        const auto* drawn = note(drawnId);
        return drawn && std::abs(drawn->startBeats - start) < 1e-9 &&
            drawn->pitch == pitch && std::abs(drawn->lengthBeats - 0.125) < 1e-9 &&
            drawn->velocity == 127 && drawn->pan == 0.375f;
    };
    brushCheck(drawnMatches(1.0, 62), "click inherits length, velocity and pan");
    pointer(QEvent::MouseMove, gridPoint(2.46, 64));
    brushCheck(drawnMatches(2.25, 64), "drawing moves the whole note right");
    pointer(QEvent::MouseMove, gridPoint(1.70, 61));
    brushCheck(drawnMatches(1.5, 61), "reversing a draw preserves note properties");
    pointer(QEvent::MouseButtonRelease, gridPoint(1.96, 65));
    brushCheck(drawnMatches(1.75, 65) && m_controller->undoDepth() == beforeDraw + 1,
               "release endpoint and creation commit together");
    m_controller->undo();
    brushCheck(daw::midiNotes(*clip()).size() == 1, "one undo removes the drawn note");
    m_controller->redo();
    brushCheck(daw::midiNotes(*clip()).size() == 2 && drawnMatches(1.75, 65),
               "redo restores the final note and its properties");
    m_laneParam = LaneParam::Velocity;
    bumpSelectedVelocity(-7);
    m_laneParam = LaneParam::Pan;
    bumpSelectedPan(1);
    finishWheelNoteEdit();
    clickGrid(gridPoint(2.73, 67));
    const auto* repeated = note(m_primary);
    brushCheck(repeated && repeated->velocity == 120 && repeated->pan == 0.40625f &&
               std::abs(repeated->lengthBeats - 0.125) < 1e-9,
               "wheel edits become the next note's defaults");
    if (repeated) {
        const QPointF edge(noteRect(*repeated).right() - 1.0, noteRect(*repeated).center().y());
        pointer(QEvent::MouseButtonPress, edge);
        pointer(QEvent::MouseMove, gridPoint(3.25, 67));
        pointer(QEvent::MouseButtonRelease, gridPoint(3.25, 67));
        clickGrid(gridPoint(3.73, 69));
        const auto* resized = note(m_primary);
        brushCheck(resized && std::abs(resized->lengthBeats - 0.75) < 1e-9 &&
                   resized->velocity == 120 && resized->pan == 0.40625f,
                   "explicit edge resize becomes the next note's length");
    }

    // Exercise real pointer input on both halves and exact boundaries of
    // straight, triplet and swung cells at fractional zoom/scroll positions.
    m_pxPerBeat = 221.3;
    m_scrollX = 11.7;
    for (double grid : {0.0625, 0.25, 1.0 / 6.0}) {
        m_gridBeats = grid;
        for (double swing : {0.5, 0.75}) {
            m_swing = swing;
            for (int slot : {2, 3}) {
                const double start = (slot + (slot % 2 ? swing - 0.5 : 0.0)) * grid;
                const double end = (slot + 1 + (slot % 2 ? 0.0 : swing - 0.5)) * grid;
                for (double fraction : {0.0, 0.49, 0.51, 0.99}) {
                    replaceNotes({}, "Prepare Cell Check");
                    m_selected.clear(); m_primary.clear();
                    clickGrid(gridPoint(start + (end - start) * fraction, 60));
                    brushCheck(daw::midiNotes(*clip()).size() == 1 &&
                               std::abs(daw::midiNotes(*clip()).front().startBeats - start) < 1e-9,
                               "placement follows the clicked visible cell");
                }
            }
        }
    }
    replaceNotes({}, "Prepare Free Draw Check");
    m_selected.clear(); m_primary.clear();
    pointer(QEvent::MouseButtonPress, gridPoint(0.63, 60), Qt::AltModifier);
    pointer(QEvent::MouseButtonRelease, gridPoint(0.87, 60), Qt::AltModifier);
    brushCheck(daw::midiNotes(*clip()).size() == 1 &&
               std::abs(daw::midiNotes(*clip()).front().startBeats - 0.87) < 1e-9,
               "Alt retains unsnapped placement and dragging");
    m_gridBeats = 0.25;
    m_swing = 0.5;
    m_pxPerBeat = 220.0;
    m_scrollX = 0.0;

    // Individual modifier-clicks build a scattered selection. Ctrl/Cmd toggles
    // one note; Shift extends the set and keeps an already selected note in it,
    // matching clip selection on the arrangement.
    mt::Notes handleNotes = {makeNote("handle-a", 0.5, 0.5),
                             makeNote("handle-b", 1.5, 0.5)};
    replaceNotes(handleNotes, "Prepare Stretch Handle Check");
    m_selected.clear();
    m_primary.clear();
    clickGrid(noteRect(handleNotes[0]).center());
    const bool singleHidden = stretchHandleRect().isNull();
    pointer(QEvent::MouseButtonPress, noteRect(handleNotes[1]).center(),
            Qt::ShiftModifier);
    pointer(QEvent::MouseButtonRelease, noteRect(handleNotes[1]).center(),
            Qt::ShiftModifier);
    pointer(QEvent::MouseButtonPress, noteRect(handleNotes[0]).center(),
            Qt::ControlModifier);
    pointer(QEvent::MouseButtonRelease, noteRect(handleNotes[0]).center(),
            Qt::ControlModifier);
    const bool ctrlRemovedOne = m_selected == QSet<QString>{
        QStringLiteral("handle-b")};
    pointer(QEvent::MouseButtonPress, noteRect(handleNotes[0]).center(),
            Qt::ControlModifier);
    pointer(QEvent::MouseButtonRelease, noteRect(handleNotes[0]).center(),
            Qt::ControlModifier);
    pointer(QEvent::MouseButtonPress, noteRect(handleNotes[1]).center(),
            Qt::ShiftModifier);
    pointer(QEvent::MouseButtonRelease, noteRect(handleNotes[1]).center(),
            Qt::ShiftModifier);
    const bool discreteSelection = ctrlRemovedOne && m_selected == QSet<QString>{
        QStringLiteral("handle-a"), QStringLiteral("handle-b")};
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
        trimmedClip && daw::midiNotes(*trimmedClip).size() == 2 &&
        std::abs(daw::midiNotes(*trimmedClip)[0].startBeats - 0.5) < 1e-9 &&
        std::abs(daw::midiNotes(*trimmedClip)[1].startBeats - 1.5) < 1e-9 &&
        std::abs(daw::midiNotes(*trimmedClip)[0].lengthBeats - 0.75) < 1e-9 &&
        std::abs(daw::midiNotes(*trimmedClip)[1].lengthBeats - 0.75) < 1e-9;
    const std::string trimUndoLabel = m_controller->undoLabel();
    const bool groupTrimAtomic = trimUndoLabel == "Edit Notes";

    // Shift-drag clones the captured selection and moves the clones together.
    // Original ids and positions remain untouched and one undo removes both.
    mt::Notes duplicateNotes = {makeNote("duplicate-a", 0.5, 0.5),
                                makeNote("duplicate-b", 1.5, 0.5)};
    replaceNotes(duplicateNotes, "Prepare Shift Duplicate Check");
    m_selected = {QStringLiteral("duplicate-a"),
                  QStringLiteral("duplicate-b")};
    m_primary = QStringLiteral("duplicate-a");
    const QPointF duplicateFrom = noteRect(duplicateNotes[0]).center();
    const QPointF duplicateTo =
        duplicateFrom + QPointF(effectivePixelsPerBeat(), 0.0);
    QMouseEvent duplicatePress(
        QEvent::MouseButtonPress, duplicateFrom,
        QPointF(mapToGlobal(duplicateFrom.toPoint())), Qt::LeftButton,
        Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(this, &duplicatePress);
    QMouseEvent duplicateMove(
        QEvent::MouseMove, duplicateTo,
        QPointF(mapToGlobal(duplicateTo.toPoint())), Qt::NoButton,
        Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(this, &duplicateMove);
    QMouseEvent duplicateRelease(
        QEvent::MouseButtonRelease, duplicateTo,
        QPointF(mapToGlobal(duplicateTo.toPoint())), Qt::LeftButton,
        Qt::NoButton, Qt::ShiftModifier);
    QApplication::sendEvent(this, &duplicateRelease);
    const auto* duplicatedClip = clip();
    bool copiesMovedTogether = duplicatedClip && daw::midiNotes(*duplicatedClip).size() == 4 &&
                               m_selected.size() == 2;
    bool copyAtOneAndHalf = false;
    bool copyAtTwoAndHalf = false;
    if (duplicatedClip) {
        for (const auto& copied : daw::midiNotes(*duplicatedClip)) {
            if (!m_selected.contains(QString::fromStdString(copied.id))) continue;
            copyAtOneAndHalf |= std::abs(copied.startBeats - 1.5) < 1e-9;
            copyAtTwoAndHalf |= std::abs(copied.startBeats - 2.5) < 1e-9;
        }
    }
    copiesMovedTogether = copiesMovedTogether && copyAtOneAndHalf &&
                          copyAtTwoAndHalf &&
                          m_controller->undoLabel() == "Duplicate Notes";
    m_controller->undo();
    const bool duplicateUndoAtomic = clip() && daw::midiNotes(*clip()).size() == 2;
    m_primary.clear();

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
        velocityClip && daw::midiNotes(*velocityClip).size() == 2 &&
        daw::midiNotes(*velocityClip)[0].velocity == 52 &&
        daw::midiNotes(*velocityClip)[1].velocity == 102 &&
        daw::midiNotes(*velocityClip)[1].velocity - daw::midiNotes(*velocityClip)[0].velocity == 50;
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
        ceilingClip && daw::midiNotes(*ceilingClip).size() == 2 &&
        daw::midiNotes(*ceilingClip)[0].velocity == 127 &&
        daw::midiNotes(*ceilingClip)[1].velocity == 127;

    // Recorded chord timing/velocities from the velocity-selection regression,
    // shifted into this viewport. Its stalks are less than one pixel apart.
    mt::Notes recordedChord = {
        makeNote("recorded-d", 0.5, 3.3950302),
        makeNote("recorded-fs", 0.5010239372, 3.3235128),
        makeNote("recorded-a", 0.5033452874, 2.9618278),
        makeNote("recorded-b", 0.5041326483, 3.2032736),
        makeNote("separate-note", 2.0, 0.5)};
    const std::array<int, 5> chordPitches{62, 66, 69, 59, 72};
    const std::array<int, 5> chordVelocities{70, 95, 77, 74, 88};
    for (std::size_t i = 0; i < recordedChord.size(); ++i) {
        recordedChord[i].pitch = chordPitches[i];
        recordedChord[i].velocity = chordVelocities[i];
    }
    bool recordedLaneSelection = true;
    m_showVelocityLane = true;
    for (const auto param : {LaneParam::Velocity, LaneParam::Pan}) {
        m_laneParam = param;
        for (const bool group : {false, true}) {
            for (const int direction : {-1, 1}) {
                replaceNotes(recordedChord, "Prepare Recorded Chord Lane Check");
                const QString upper = QStringLiteral("recorded-a");
                m_selected = {upper};
                if (group) m_selected.insert(QStringLiteral("recorded-fs"));
                m_primary = upper;
                const auto selection = m_selected;
                // Aim slightly right of the overlapping stalks, where the
                // unselected B is closer than the selected upper A.
                const QPointF pressAt(laneHandle(recordedChord[3]).x() + 2.0,
                                      laneHandle(recordedChord[2]).y());
                bool correct = handleAt(pressAt) == upper &&
                    handleAt(laneHandle(recordedChord[4])) ==
                        QStringLiteral("separate-note") &&
                    handleAt(QPointF(beatsToX(3.0), pressAt.y())).isEmpty();
                lostReleaseMouse(QEvent::MouseButtonPress, pressAt,
                                 Qt::LeftButton, Qt::LeftButton);
                correct &= m_selected == selection && m_primary == upper;
                const double delta = direction *
                    (param == LaneParam::Velocity ? 12.0 / 127.0 : 0.125);
                const QPointF moveAt(pressAt.x(), laneValueToY(
                    laneValueOf(recordedChord[2]) + delta));
                lostReleaseMouse(QEvent::MouseMove, moveAt,
                                 Qt::NoButton, Qt::LeftButton);
                lostReleaseMouse(QEvent::MouseButtonRelease, moveAt,
                                 Qt::LeftButton, Qt::NoButton);
                mt::Notes expected = recordedChord;
                for (auto& n : expected) {
                    if (!selection.contains(QString::fromStdString(n.id))) continue;
                    if (param == LaneParam::Velocity) n.velocity += direction * 12;
                    else n.pan += float(direction * 0.25);
                }
                correct &= m_selected == selection && clip() &&
                    daw::midiNotes(*clip()) == expected &&
                    m_controller->undoLabel() == "Edit Notes";
                m_controller->undo();
                correct &= clip() && daw::midiNotes(*clip()) == recordedChord;
                m_controller->redo();
                correct &= clip() && daw::midiNotes(*clip()) == expected;
                recordedLaneSelection &= correct;
                if (!correct)
                    std::fprintf(stderr,
                        "recorded chord lane failed: param=%d group=%d direction=%d\n",
                        int(param), int(group), direction);
            }
        }
    }
    // With no selection the nearest column remains directly selectable; notes
    // drawn exactly on the grid still give the selected chord tone priority.
    replaceNotes(recordedChord, "Prepare Lane Hit Check");
    m_selected.clear();
    recordedLaneSelection &= handleAt(laneHandle(recordedChord[3])) ==
        QStringLiteral("recorded-b");
    for (std::size_t i = 0; i < 4; ++i) recordedChord[i].startBeats = 0.5;
    replaceNotes(recordedChord, "Prepare Grid Chord Lane Check");
    m_selected = {QStringLiteral("recorded-a")};
    recordedLaneSelection &= handleAt(laneHandle(recordedChord[3])) ==
        QStringLiteral("recorded-a");
    m_showVelocityLane = false;
    m_laneParam = LaneParam::Velocity;

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
        daw::midiNotes(*clip()).size() == 1 && daw::midiNotes(*clip()).front().id == "history-c";
    m_controller->undo();
    const bool undoneTogether = clip() && daw::midiNotes(*clip()).size() == 3;
    m_controller->redo();
    const bool redoneTogether = clip() && daw::midiNotes(*clip()).size() == 1;

    // Repeat selects its result. Repeating again therefore continues the line
    // instead of duplicating the original phrase on top of the first copy.
    m_timeRange = {};
    mt::Notes repeatNotes = {makeNote("repeat-a", 0.5, 0.5)};
    replaceNotes(repeatNotes, "Prepare Repeat Chain Check");
    m_selected = {QStringLiteral("repeat-a")};
    duplicateSelection();
    duplicateSelection();
    const auto* repeatedClip = clip();
    const bool repeatChains = repeatedClip && daw::midiNotes(*repeatedClip).size() == 3 &&
                              m_selected.size() == 1 &&
                              std::abs(daw::midiNotes(*repeatedClip)[2].startBeats - 1.5) <
                                  1e-9;

    // Repeat copies the local range and advances it, without moving or arming
    // the independent arrangement playback cycle.
    mt::Notes loopNotes = {makeNote("loop-a", 0.25, 0.5),
                           makeNote("loop-outside", 8.0, 0.5)};
    replaceNotes(loopNotes, "Prepare Loop Repeat Check");
    m_selected.clear();
    m_timeRange = {0.0, 1.0};
    duplicateSelection();
    duplicateSelection();
    const auto* loopRepeatedClip = clip();
    const bool loopRepeatChains =
        loopRepeatedClip && daw::midiNotes(*loopRepeatedClip).size() == 4 &&
        m_selected.size() == 1 &&
        std::any_of(daw::midiNotes(*loopRepeatedClip).begin(),
                    daw::midiNotes(*loopRepeatedClip).end(), [](const auto& note) {
                        return std::abs(note.startBeats - 1.25) < 1e-9 &&
                               std::abs(note.lengthBeats - 0.5) < 1e-9;
                    }) &&
        std::any_of(daw::midiNotes(*loopRepeatedClip).begin(),
                    daw::midiNotes(*loopRepeatedClip).end(), [](const auto& note) {
                        return std::abs(note.startBeats - 2.25) < 1e-9 &&
                               std::abs(note.lengthBeats - 0.5) < 1e-9;
                    }) &&
        std::abs(m_timeRange.from - 2.0) < 1e-9 &&
        std::abs(m_timeRange.to - 3.0) < 1e-9 &&
        m_controller->loopStartSeconds() == originalLoopStart &&
        m_controller->loopEndSeconds() == originalLoopEnd &&
        m_controller->isLoopEnabled() == originalLoopEnabled;
    m_timeRange = {};

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
        clipboard().notes.size() == 1 && clip() && daw::midiNotes(*clip()).size() == 1;
    const bool copiedByKey = copyState;
    const Qt::KeyboardModifier alternateCommand =
#if defined(Q_OS_MACOS)
        Qt::MetaModifier;
#else
        Qt::ControlModifier;
#endif
    const bool cutDispatched =
        sendShortcut(alternateCommand, Qt::Key_X);
    const bool cutByKey = clip() && daw::midiNotes(*clip()).empty();
    const bool pasteDispatched =
        sendShortcut(Qt::ControlModifier, Qt::Key_V);
    const bool pastedByKey = clip() && daw::midiNotes(*clip()).size() == 1 &&
                             m_selected.size() == 1;
    const bool repeatDispatched =
        sendShortcut(alternateCommand, Qt::Key_B);
    const bool repeatedByKey = clip() && daw::midiNotes(*clip()).size() == 2 &&
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
        daw::midiNotes(*clip()) == paintedPreview && m_selected.size() == 2;

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
            {{0.5, 0.2}, {0.504, 0.23}, {1.5, 0.8}});
        m_showVelocityLane = true;
        m_laneParam = LaneParam::Controller;
        m_laneId = laneId;
        cancelControllerLaneWrite();

        const bool closeControllerPoints =
            lanePointAt(QPointF(beatsToX(0.5), laneValueToY(0.2))) == 0 &&
            lanePointAt(QPointF(beatsToX(0.504), laneValueToY(0.23))) == 1;
        const std::size_t writesBefore = m_controllerLaneModelWrites;
        const QPointF controllerPressAt(beatsToX(0.504), laneValueToY(0.23));
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
        bool neighbourUnchanged = false;
        if (const auto* lane = controllerLane()) {
            finalPointApplied = std::any_of(
                lane->points.begin(), lane->points.end(),
                [finalBeat, finalValue](const auto& point) {
                    return std::abs(point.beats - finalBeat) < 1e-9 &&
                           std::abs(point.value - finalValue) < 1e-9;
                });
            neighbourUnchanged = lane->points.size() == 3 &&
                lane->points.front().beats == 0.5 &&
                lane->points.front().value == 0.2;
        }
        controllerLaneCoalesced =
            closeControllerPoints && stormStayedQueued &&
            controllerLaneWrites == 1 && finalPointApplied && neighbourUnchanged &&
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
    m_snapEnabled = originalSnap;
    m_adaptiveSnap = originalAdaptive;
    m_showVelocityLane = originalLane;
    m_laneParam = originalLaneParam;
    m_laneId = originalLaneId;
    m_pxPerBeat = originalPx;
    m_rowHeight = originalRow;
    m_scrollX = originalScrollX;
    m_scrollY = originalScrollY;
    m_lastLength = originalLastLength;
    m_lastVelocity = originalLastVelocity;
    m_lastPan = originalLastPan;
    m_swing = originalSwing;
    m_snapToScale = originalSnapToScale;
    m_preview = originalPreview;
    invalidateNotePaintIndex();
    m_previewSelection = originalPreviewSelection;
    m_previewWholeClip = originalPreviewWholeClip;
    m_noteStyle = originalNoteStyle;
    m_noteBorders = originalNoteBorders;
    clipboard() = originalClipboard;
    m_timeRange = originalTimeRange;
    m_controller->seekSeconds(originalTransportPosition);
    clampScroll();
    emit selectionChanged();
    emit viewportChanged();
    update();
    const bool ok = interruptedScrubStops && interruptedSelectionStops &&
                    noteStylesClean && keyboardShapeClean &&
                    localPlayheadMapped && fractionalPlayhead && rulerSeekSnapped &&
                    blankRightClearsSelection &&
                    eraseDeferred && sweptAll && ctrlBorrowsSelection &&
                    leftEraseDeferred && leftSweptAll &&
                    brushSafe && brushProperties && singleHidden &&
                    discreteSelection && groupOffset &&
                    groupTrim && groupTrimAtomic && copiesMovedTogether &&
                    duplicateUndoAtomic && dynamicsPreserved &&
                    velocityAtomic && velocityCeilingIndependent &&
                    recordedLaneSelection &&
                    deletedTogether && undoneTogether && redoneTogether &&
                    repeatChains && loopRepeatChains && shortcutsRouted &&
                    previewCommitExact &&
                    indexedBoundaries && steadyQueriesReuseIndex &&
                    editInvalidatesIndex && logarithmicPlayheadLookup &&
                    controllerLaneCoalesced;
    if (!ok) {
        if (!interruptedScrubStops || !interruptedSelectionStops)
            std::fprintf(stderr, "piano-roll lost-release check: scrub=%d selection=%d\n",
                         int(interruptedScrubStops), int(interruptedSelectionStops));
        if (!fractionalPlayhead) std::fprintf(stderr, "piano-roll fractional playhead check failed\n");
        std::fprintf(stderr,
                     "piano-roll view checks: styles=%d keyboard=%d seek=%d ruler=%d deselect=%d deferErase=%d erase=%d ctrlSelect=%d leftDeferErase=%d leftErase=%d "
                     "brush=%d single=%d "
                     "offset=%d trim=%d trimUndo=%d shiftCopy=%d copyUndo=%d dynamics=%d velocityUndo=%d "
                     "ceiling=%d delete=%d undo=%d redo=%d repeat=%d loop=%d keys=%d "
                     "previewCommit=%d index=%d cache=%d editIndex=%d "
                     "lookup=%d(%zu comparisons) controller=%d(%zu writes)\n",
                     int(noteStylesClean), int(keyboardShapeClean),
                     int(localPlayheadMapped),
                     int(rulerSeekSnapped),
                     int(blankRightClearsSelection),
                     int(eraseDeferred), int(sweptAll), int(ctrlBorrowsSelection),
                     int(leftEraseDeferred),
                     int(leftSweptAll), int(brushSafe), int(singleHidden),
                     int(groupOffset), int(groupTrim), int(groupTrimAtomic),
                     int(copiesMovedTogether), int(duplicateUndoAtomic),
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
        daw::secondsToBeats(c->offsetSeconds + c->durationSeconds, m_controller->project().tempo);
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

void PianoRollView::drawTimeRangeStrip(QPainter& p) {
    const Theme& t = th();
    const double keyWidth = keyboardWidth();
    const QRectF strip(0.0, 0.0, double(width()), double(ui::kLoopStripHeight));
    p.fillRect(strip, mixColors(t.toolbarBackground, t.background, 0.35));
    p.setPen(QPen(mixColors(t.separator(), t.background, 0.35), 1.0));
    p.drawLine(QPointF(0.0, ui::kLoopStripHeight),
               QPointF(double(width()), ui::kLoopStripHeight));
    if (!clip() || !m_controller) return;

    if (!m_timeRange.valid()) return;

    const double rawLeft = beatsToX(m_timeRange.from);
    const double rawRight = beatsToX(m_timeRange.to);
    const double left = std::max(keyWidth, rawLeft);
    const double right = std::min(double(width()), rawRight);
    if (right <= left) return;

    const QColor cycle = Theme::cycle();
    const QRectF bar(left + 0.5, 0.5, right - left,
                     ui::kLoopStripHeight - 1.0);
    p.setRenderHint(QPainter::Antialiasing, false);
    QLinearGradient fill(bar.topLeft(), bar.bottomLeft());
    fill.setColorAt(0.0, cycle.lighter(112));
    fill.setColorAt(1.0, cycle.darker(112));
    p.setBrush(fill);
    p.setPen(QPen(cycle.darker(135), 1.0));
    p.drawRect(bar);

    QColor flag = mixColors(cycle, t.background, 0.34);
    flag.setAlpha(t.dark ? 205 : 220);
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
    QColor wash = cycle;
    wash.setAlpha(t.dark ? 16 : 22);
    p.fillRect(QRectF(left, ui::kRulerHeight, right - left,
                      std::max(0.0, laneTop() - ui::kRulerHeight)), wash);
    QColor edge = cycle;
    edge.setAlpha(t.dark ? 72 : 88);
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

PianoRollView::RangeGrab PianoRollView::rangeGrabAt(double x) const {
    if (!clip() || !m_timeRange.valid()) return RangeGrab::Create;
    const double left = beatsToX(m_timeRange.from);
    const double right = beatsToX(m_timeRange.to);
    if (std::abs(x - left) <= ui::kLoopEdgeGrab) return RangeGrab::ResizeStart;
    if (std::abs(x - right) <= ui::kLoopEdgeGrab) return RangeGrab::ResizeEnd;
    if (x > left && x < right) return RangeGrab::Move;
    return RangeGrab::Create;
}

void PianoRollView::updateTimeRangeDrag(double x, bool snapping) {
    const double at = std::max(0.0, snapBeats(xToBeats(x), snapping));
    if (m_rangeGrab == RangeGrab::Move) {
        const double from = std::max(0.0, at - m_rangeGrabOffset);
        m_timeRange = {from, from + m_rangeGrabLength};
    } else if (m_rangeGrab != RangeGrab::None) {
        m_timeRange = {std::min(m_rangeAnchorBeats, at),
                       std::max(m_rangeAnchorBeats, at)};
    }
}

mt::Notes PianoRollView::notesInTimeRange() const {
    const auto* c = clip();
    if (!c || !m_timeRange.valid()) return {};
    mt::Notes notes;
    for (const auto& source : daw::midiNotes(*c)) {
        const double from = std::max(source.startBeats, m_timeRange.from);
        const double to = std::min(source.startBeats + source.lengthBeats, m_timeRange.to);
        if (to <= from) continue;
        auto note = source;
        note.startBeats = from;
        note.lengthBeats = to - from;
        notes.push_back(std::move(note));
    }
    return notes;
}

void PianoRollView::seekToLocalBeat(double beats, bool snapping) {
    const auto* c = clip();
    if (!c || !m_controller) return;
    const double localBeat =
        std::clamp(snapBeats(beats, snapping),
                   daw::secondsToBeats(c->offsetSeconds, m_controller->project().tempo), clipBeats());
    const double absoluteSeconds =
        c->startSeconds - c->offsetSeconds +
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

void PianoRollView::updateVelocityRamp(const QPointF& pos) {
    if (!m_laneRamping || !clip()) return;
    const QPointF endpoint(std::max(0.0, xToBeats(pos.x())),
                           std::max(1.0 / 127.0, laneValueAtY(pos.y())));
    const double from = std::min(m_laneRampAnchor.x(), endpoint.x());
    const double to = std::max(m_laneRampAnchor.x(), endpoint.x());
    const double previousFrom = std::min(m_laneRampAnchor.x(), m_laneRampEnd.x());
    const double previousTo = std::max(m_laneRampAnchor.x(), m_laneRampEnd.x());
    const double tolerance = 0.5 / std::max(1.0, pxPerBeat());
    const double span = endpoint.x() - m_laneRampAnchor.x();
    m_noteUpdateScratch.clear();
    for (const auto& original : m_laneOrig) {
        const double beat = original.startBeats;
        const bool inside = beat >= from - tolerance && beat <= to + tolerance;
        const bool wasInside = beat >= previousFrom - tolerance &&
                               beat <= previousTo + tolerance;
        if (!inside && !wasInside) continue;
        daw::NoteModel next = original;
        if (inside) {
            const double fraction = std::abs(span) > 1e-9
                ? std::clamp((beat - m_laneRampAnchor.x()) / span, 0.0, 1.0)
                : 1.0;
            const double value = m_laneRampAnchor.y() +
                fraction * (endpoint.y() - m_laneRampAnchor.y());
            next.velocity = std::clamp(int(std::lround(value * 127.0)), 1, 127);
        }
        // Receding or reversing the line restores notes from the press snapshot.
        m_noteUpdateScratch.push_back(std::move(next));
    }
    m_laneRampEnd = endpoint;
    m_controller->setNoteStates(m_trackId.toStdString(), m_clipId.toStdString(),
                                m_noteUpdateScratch);
    update();
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
    int best = -1;
    double bestDistance = kHandleGrabPx + 2.0;
    for (auto point = first; point != lane->points.end(); ++point) {
        if (point->beats > atBeat + marginBeats) break;
        const size_t i = std::size_t(point - lane->points.begin());
        const QPointF at(beatsToX(lane->points[i].beats),
                         laneValueToY(lane->points[i].value));
        const double distance = QLineF(at, pos).length();
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = int(i);
        }
    }
    return best;
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
    if (c && !daw::midiNotes(*c).empty()) {
        lowest = highest = daw::midiNotes(*c).front().pitch;
        for (const auto& n : daw::midiNotes(*c)) {
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
    m_timeRange = {};
    m_selected.clear();
    for (const auto& n : daw::midiNotes(*c)) m_selected.insert(QString::fromStdString(n.id));
    emit selectionChanged();
    emitStatus();
    update();
}

void PianoRollView::selectNone() {
    rememberNoteProperties(note(m_primary));
    m_timeRange = {};
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
    for (const auto& n : daw::midiNotes(*c)) {
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
    for (const auto& n : daw::midiNotes(*c)) {
        if (m_selected.contains(QString::fromStdString(n.id))) wanted.insert(n.color);
    }
    for (const auto& n : daw::midiNotes(*c)) {
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
    rememberNoteProperties(note(m_primary));
    emit edited();
}

// ── Commands ────────────────────────────────────────────────────────────────

mt::Notes PianoRollView::targetNotes() const {
    const auto* c = clip();
    if (!c) return {};
    if (m_selected.isEmpty()) return daw::midiNotes(*c);
    mt::Notes selection;
    for (const auto& n : daw::midiNotes(*c)) {
        if (m_selected.contains(QString::fromStdString(n.id))) selection.push_back(n);
    }
    return selection;
}

mt::Notes PianoRollView::clipNotes() const {
    const auto* c = clip();
    return c ? daw::midiNotes(*c) : mt::Notes{};
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
    for (const auto& n : daw::midiNotes(*c)) {
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
    for (const auto& n : daw::midiNotes(*c)) {
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
            m_selectionEditWorking = daw::midiNotes(*current);
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
    rememberNoteProperties(note(m_primary));
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
    m_lastVelocity = std::clamp(velocity, 1, 127);
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
    m_lastPan = std::clamp(pan, -1.0f, 1.0f);
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
        for (const auto& n : daw::midiNotes(*currentClip)) accumulate(n);
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
    remaining.reserve(daw::midiNotes(*clip()).size());
    for (const auto& note : daw::midiNotes(*clip())) {
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
    mt::Notes selection = m_timeRange.valid() ? notesInTimeRange() : targetNotes();
    if (selection.empty()) {
        if (m_timeRange.valid()) clipboard() = {};
        return;
    }
    // A time selection includes its leading/trailing rests. Ordinary note
    // selections keep the existing behaviour of landing on their first note.
    double start = m_timeRange.from, end = m_timeRange.to;
    if (!m_timeRange.valid()) mt::spanOf(selection, &start, &end);
    for (auto& note : selection) note.startBeats -= start;
    std::vector<daw::SlideNoteModel> gestures;
    if (clip() && m_timeRange.valid()) {
        // A clipboard trim preserves editable note properties, including
        // release velocity, while cropping the linked pitch trajectory.
        gestures = daw::slides::crop(daw::midiNotes(*clip()),
                                     daw::slides::editable(*clip()), selection, start, end);
    } else {
        if(clip())for(auto slide:daw::slides::editable(*clip())) {
            auto belongs=[&](const std::string& id){return std::any_of(selection.begin(),selection.end(),[&](const auto& n){return n.id==id;});};
            if(!belongs(slide.referenceNoteId))continue;
            std::erase_if(slide.targetNoteIds,[&](const auto& id){return !belongs(id);});
            slide.startBeats=std::max(0.,slide.startBeats-start);gestures.push_back(std::move(slide));
        }
    }
    clipboard()={std::move(selection),m_timeRange.valid()?end-start:0.,std::move(gestures)};
    emitStatus();
}

void PianoRollView::cutSelection() {
    copySelection();
    if (!m_timeRange.valid()) {
        deleteSelection();
        return;
    }
    const auto* c = clip();
    if (!c || clipboard().notes.empty()) return;
    // Keep note portions outside the range. A held note spanning both edges
    // becomes two notes; the whole cut is still one undo operation.
    mt::Notes remaining;
    auto gestures=daw::slides::editable(*c);
    const auto curves=daw::slides::compile(daw::midiNotes(*c),gestures);
    for (const auto& source : daw::midiNotes(*c)) {
        const double end = source.startBeats + source.lengthBeats;
        if (end <= m_timeRange.from || source.startBeats >= m_timeRange.to) {
            remaining.push_back(source);
            continue;
        }
        if (source.startBeats < m_timeRange.from) {
            auto left = source;
            left.lengthBeats = m_timeRange.from - source.startBeats;
            remaining.push_back(std::move(left));
        }
        if (end > m_timeRange.to) {
            auto right = source;
            right.id = daw::newUuid();
            right.startBeats = m_timeRange.to;
            right.lengthBeats = end - m_timeRange.to;
            auto fragments=daw::slides::crop({source},gestures,{right},m_timeRange.to,end,&curves);
            for(auto& slide:fragments){slide.startBeats+=m_timeRange.to;gestures.push_back(std::move(slide));}
            remaining.push_back(std::move(right));
        }
    }
    m_controller->setClipMidiObjects(m_trackId.toStdString(),m_clipId.toStdString(),std::move(remaining),std::move(gestures),"Delete Notes");
    invalidateSoundingPitchIndex();
    m_selected.clear();
    m_primary.clear();
    emit selectionChanged();
    emit edited();
    emitStatus();
    update();
}

bool PianoRollView::canPaste() const { return !clipboard().notes.empty(); }

void PianoRollView::paste() {
    const auto* c = clip();
    if (!c || clipboard().notes.empty()) return;
    // Land it where the pointer is, or at the start of the clip when it is not
    // over the grid at all (a paste driven from the menu bar, say).
    const double at =
        m_pointerInside && m_pointer.x() >= keyboardWidth()
            ? snapBeats(xToBeats(m_pointer.x()), m_snapEnabled)
            : 0.0;

    mt::Notes merged = daw::midiNotes(*c);
    QSet<QString> pasted;
    auto copiedNotes=clipboard().notes;auto copiedSlides=clipboard().slides;
    daw::slides::reidentify(copiedNotes,copiedSlides);
    for (const auto& source : copiedNotes) {
        daw::NoteModel n = source;

        n.startBeats = at + source.startBeats;
        pasted.insert(QString::fromStdString(n.id));
        merged.push_back(n);
    }
    auto gestures=daw::slides::editable(*c);for(auto& slide:copiedSlides){slide.startBeats+=at;gestures.push_back(std::move(slide));}
    m_controller->setClipMidiObjects(m_trackId.toStdString(),m_clipId.toStdString(),std::move(merged),std::move(gestures),"Paste Notes");
    invalidateSoundingPitchIndex();
    m_selected = pasted;
    m_primary.clear();
    m_timeRange = clipboard().rangeLength > 0.0
        ? TimeRange{at, at + clipboard().rangeLength} : TimeRange{};
    emit selectionChanged();
    emit edited();
    emitStatus();
    update();
}

void PianoRollView::duplicateSelection() {
    const auto* c=clip();if(!c)return;
    auto selected=m_timeRange.valid()?notesInTimeRange():targetNotes();if(selected.empty())return;
    double from=0,to=0;mt::spanOf(selected,&from,&to);
    const double at=m_timeRange.valid()?m_timeRange.to:from+std::max(to-from,effectiveGridBeats());
    const auto saved=clipboard();copySelection();auto copied=clipboard();clipboard()=saved;
    daw::slides::reidentify(copied.notes,copied.slides);
    auto merged=daw::midiNotes(*c);auto gestures=daw::slides::editable(*c);QSet<QString> copies;
    for(auto& n:copied.notes){n.startBeats+=at;copies.insert(QString::fromStdString(n.id));merged.push_back(n);}
    for(auto& slide:copied.slides){slide.startBeats+=at;gestures.push_back(slide);}
    m_controller->setClipMidiObjects(m_trackId.toStdString(),m_clipId.toStdString(),std::move(merged),std::move(gestures),m_timeRange.valid()?"Repeat Loop Notes":"Duplicate Notes");
    invalidateSoundingPitchIndex();m_selected=copies;m_primary.clear();
    if(m_timeRange.valid())m_timeRange={at,at+m_timeRange.to-m_timeRange.from};
    emit selectionChanged();emit edited();emitStatus();update();
}

// ── Colour ──────────────────────────────────────────────────────────────────

QColor PianoRollView::colorFor(const daw::NoteModel& n,
                               const QColor& clipColor) const {
    QColor base = clipColor;
    switch (m_colorMode) {
        case ColorMode::Clip:
            break;
        case ColorMode::Velocity: {
            // Cool and dim for soft, hot for loud: velocity reads off the grid
            // without opening the lane.
            const double t = std::clamp(double(n.velocity) / 127.0, 0.0, 1.0);
            base = QColor::fromHsvF(0.62 - 0.62 * t, 0.75, 1.0);
            break;
        }
        case ColorMode::Pitch: {
            const double t = double(((n.pitch % 12) + 12) % 12) / 12.0;
            base = QColor::fromHsvF(t, 0.65, 0.95);
            break;
        }
        case ColorMode::Custom:
            base = n.color ? ui::colorFromRgb(n.color) : clipColor;
            break;
    }
    // Preserve hue in every colour mode. Keep the quietest notes visible;
    // velocity changes luminance immediately, including during live gestures.
    const double velocity = std::clamp((double(n.velocity) - 1.0) / 126.0, 0.0, 1.0);
    return QColor::fromHsvF(base.hsvHueF(), base.hsvSaturationF(),
        base.valueF() * (0.45 + 0.55 * std::pow(velocity, 0.75)), base.alphaF());
}

// ── Painting ────────────────────────────────────────────────────────────────

void PianoRollView::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    paintScene(p, event->region());
}

void PianoRollView::paintScene(QPainter& p, const QRegion& region) {
    if (!ui::graphics::isSceneRecording(p) || !clip()) {
        paintGridAndNotes(p, region);
        paintOverlays(p, region);
        return;
    }
    QByteArray signature;
    QDataStream stream(&signature, QIODevice::WriteOnly);
    stream << size() << font() << palette().cacheKey() << p.device()->devicePixelRatioF()
           << pxPerBeat() << m_rowHeight << keyboardWidth() << laneTop() << laneHeight() << int(m_laneParam)
           << quint64(m_controller->projectRevision())
           << quint64(m_controller->midiNotesRevision(m_trackId.toStdString()));
    if (signature != m_gpuTileSignature) {
        m_gpuNoteTiles.clear();
        m_gpuLaneTiles.clear();
        m_gpuTileSignature = signature;
    }
    p.fillRect(rect(), th().background);
    const double key = keyboardWidth(), top = ui::kRulerHeight;
    const QRectF field(key, top, std::max(0., width() - key), std::max(0., laneTop() - top));
    const int tileWidth = std::max(1, std::min(512, int(field.width())));
    const int tileHeight = std::max(1, std::min(256, int(field.height())));
    const double scrollX = m_scrollX, scrollY = m_scrollY;
    for (qint64 y = qint64(std::floor(scrollY / tileHeight)); y * tileHeight < scrollY + field.height(); ++y) {
        for (qint64 x = qint64(std::floor(scrollX / tileWidth)); x * tileWidth < scrollX + field.width(); ++x) {
            const QPointF origin(x * tileWidth - scrollX, y * tileHeight - scrollY);
            p.save();
            p.setClipRect(field.intersected(QRectF(key + origin.x(), top + origin.y(), tileWidth, tileHeight)), Qt::IntersectClip);
            m_gpuNoteTiles.paint(p, (quint64(x) << 16) | quint64(y), size(), origin,
                [&](QPainter& local) {
                    QScopedValueRollback<double> sx(m_scrollX, double(x * tileWidth));
                    QScopedValueRollback<double> sy(m_scrollY, double(y * tileHeight));
                    const QRect dirty(int(key), int(top), tileWidth, tileHeight);
                    local.setClipRect(dirty);
                    paintGridAndNotes(local, QRegion(dirty));
                });
            p.restore();
        }
    }
    // The ruler is anchored vertically, and the keyboard/playhead respond on
    // this frame. They never ride along with a pitch tile.
    p.save();
    const QRect ruler(0, 0, width(), ui::kRulerHeight);
    p.setClipRect(ruler, Qt::IntersectClip);
    paintGridAndNotes(p, QRegion(ruler));
    p.restore();
    paintOverlays(p, region);
}

void PianoRollView::paintGridAndNotes(QPainter& p, const QRegion& region) {
    const Theme& t = th();
    const QRectF dirtyRect(region.boundingRect());
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
    p.setClipRect(QRectF(0, 0, double(width()), fieldBottom), Qt::IntersectClip);
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
    drawTimeRangeStrip(p);
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
    dirtyBeatRanges.reserve(std::size_t(region.rectCount()));
    constexpr double kGridPaintMarginPx = 2.0;
    for (const QRect& updateRect : region) {
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
                    other.startSeconds - other.offsetSeconds - c->startSeconds + c->offsetSeconds, tempo);
                const daw::MidiPreviewIndex* index = nullptr;
                daw::MidiPreviewIndex transient;
                if (other.id.empty()) {
                    transient.rebuild(daw::midiNotes(other));
                    index = &transient;
                } else {
                    // Track ids disambiguate malformed legacy documents that
                    // accidentally reused a clip id on more than one lane.
                    const std::string key = track->id + '\n' + other.id;
                    auto [found, inserted] =
                        m_ghostPaintIndexes.try_emplace(key);
                    GhostPaintIndexEntry& entry = found->second;
                    if (inserted || entry.revision != revision ||
                        entry.noteCount != daw::midiNotes(other).size()) {
                        entry.index.rebuild(daw::midiNotes(other));
                        entry.revision = revision;
                        entry.noteCount = daw::midiNotes(other).size();
                    }
                    index = &entry.index;
                }

                m_ghostPaintScratch.clear();
                const double minimumPaintBeats = 3.0 / std::max(1.0, px);
                for (const auto& range : dirtyBeatRanges) {
                    index->forEachVisible(
                        daw::midiNotes(other),
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
                    const daw::NoteModel& note = daw::midiNotes(other)[noteIndex];
                    QRectF r = noteRect(note);
                    // beatsToX() is affine in beat-space, so translating the
                    // rectangle is equivalent to copying/mutating the note —
                    // without allocating its id string on every ghost paint.
                    r.translate(offset * px, 0.0);
                    if (r.bottom() < gridTop || r.top() > fieldBottom) continue;
                    if (r.right() < keyWidth || r.left() > width()) continue;
                    if (!r.intersects(dirtyRect)) continue;
                    if (!region.intersects(r.toAlignedRect())) continue;
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
    const auto& notesToPaint = m_preview ? *m_preview : daw::midiNotes(*c);
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
        if (!region.intersects(r.toAlignedRect())) return;
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
            const auto linear = [](double c) {
                return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            };
            const double luminance = 0.2126 * linear(fill.redF()) +
                0.7152 * linear(fill.greenF()) + 0.0722 * linear(fill.blueF());
            p.setPen(luminance > 0.179 ? QColor(Qt::black) : QColor(Qt::white));
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

    p.restore();
    m_paintClip = nullptr;
}

void PianoRollView::paintOverlays(QPainter& p, const QRegion& region) {
    paintSlides(p);
    const auto* c = clip();
    if (!c) return;
    m_paintClip = c;
    const Theme& t = th();
    const QRectF dirtyRect(region.boundingRect());
    const double keyWidth = keyboardWidth(), gridTop = ui::kRulerHeight;
    const double fieldBottom = laneTop(), totalBeats = clipBeats();
    p.save();
    p.setClipRect(QRectF(0, 0, double(width()), fieldBottom), Qt::IntersectClip);
    // The editor keeps the complete source phrase editable. Dim the hidden
    // head and mark the audible boundary without deleting its notes.
    if (c->offsetSeconds > 0.0) {
        const double headX = beatsToX(daw::secondsToBeats(c->offsetSeconds,
                                                        m_controller->project().tempo));
        if (headX > keyWidth)
            p.fillRect(QRectF(keyWidth, gridTop, headX - keyWidth, fieldBottom - gridTop),
                       QColor(t.background.red(), t.background.green(), t.background.blue(), 150));
        p.setPen(QPen(t.sectionDivider(), 1.0));
        p.drawLine(QPointF(headX, gridTop), QPointF(headX, fieldBottom));
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
            m_controller->presentationPositionSeconds() - c->startSeconds + c->offsetSeconds,
            m_controller->project().tempo);
        if (beats >= 0.0 && beats <= totalBeats) {
            const double x = beatsToX(beats);
            if (x >= keyWidth && x <= width()) {
                const double playheadWidth = ui::playheadWidth();
                p.save();
                p.setRenderHint(QPainter::Antialiasing, true);
                QColor halo = t.cursor;
                halo.setAlpha(t.dark ? 58 : 44);
                p.setPen(QPen(halo, playheadWidth + 3.0));
                p.drawLine(QPointF(x, 0.0), QPointF(x, fieldBottom));
                p.setPen(QPen(t.cursor, playheadWidth));
                p.drawLine(QPointF(x, 0.0), QPointF(x, fieldBottom));
                const double half =
                    7.0 + (playheadWidth - ui::kPlayheadWidthDefault) * 0.6;
                p.setPen(Qt::NoPen);
                p.setBrush(t.cursor);
                p.drawPath(roundedPlayheadTriangle(
                    QPointF(x, 12.0), QPointF(x - half, 0.0),
                    QPointF(x + half, 0.0), 3.0));
                p.restore();
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
    if (!m_sampleDropName.isEmpty()) {
        const QRectF grid(keyWidth + 2, gridTop + 2,
                          width() - keyWidth - 4, fieldBottom - gridTop - 4);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(t.accent, 2));
        QColor wash = t.accent; wash.setAlpha(24);
        p.setBrush(wash);
        p.drawRoundedRect(grid, 4, 4);
        const QString label = tr("Replace sample · %1").arg(m_sampleDropName);
        const QString elided = p.fontMetrics().elidedText(label, Qt::ElideMiddle,
                                                         int(grid.width()) - 48);
        const QSizeF size(p.fontMetrics().horizontalAdvance(elided) + 24, 32);
        const QRectF badge(grid.center() - QPointF(size.width() / 2, 16), size);
        p.setPen(Qt::NoPen);
        p.setBrush(t.background);
        p.drawRoundedRect(badge, 6, 6);
        p.setPen(t.textPrimary);
        p.drawText(badge, Qt::AlignCenter, elided);
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

// Flat, theme-aware key faces retain the familiar black/white silhouette.
void PianoRollView::paintKeyboard(QPainter& p, double fieldBottom) {
    if (!m_showKeyboard) return;
    const Theme& t = th();
    const double keyWidth = keyboardWidth();
    const double gridTop = ui::kRulerHeight;
    const PitchMask sounding = keyboardPitches();
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const qreal pixel = 1.0 / std::max<qreal>(1.0, dpr);
    const QColor white = mixColors(QColor(232, 235, 241), t.background, 0.12);
    const QColor black = mixColors(QColor(30, 34, 42), t.background, 0.08);

    p.save();
    p.setClipRect(QRectF(0, gridTop, keyWidth, std::max(0.0, fieldBottom - gridTop)));
    p.fillRect(QRectF(0, gridTop, keyWidth, fieldBottom - gridTop), t.background);
    QFont font = p.font();
    font.setPixelSize(9);
    p.setFont(font);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int pass = 0; pass < 2; ++pass) {
        const bool darkKey = pass == 1;
        for (int pitch = kMinPitch; pitch <= kMaxPitch; ++pitch) {
            if (isBlackKey(pitch) != darkKey) continue;
            const double y = pitchToY(pitch);
            if (y + m_rowHeight < gridTop || y > fieldBottom) continue;
            const bool held = pitch == m_pressedKey || sounding.test(size_t(pitch));
            const double w = darkKey ? keyWidth * 0.64 : keyWidth;
            QRectF key = ui::pixelAlignedRect(QRectF(0, y, w, m_rowHeight), dpr);
            key.adjust(0, 0, -pixel, -pixel);
            if (key.height() <= 0) continue;
            const auto path = pianoKeyPath(key, std::min(2.0, key.height() * 0.2));
            const QColor face = darkKey ? black : white;
            p.setPen(Qt::NoPen);
            p.setBrush(held ? mixColors(face, t.accent, 0.65) : face);
            p.drawPath(path);
            if (held) {
                p.fillRect(QRectF(0, key.top(), 3, key.height()), t.accent);
            }
            if ((m_showAllKeyNames || pitch % 12 == 0) && m_rowHeight >= 8.0) {
                p.setPen(darkKey ? QColor(225, 228, 234) : QColor(39, 43, 51));
                p.drawText(key.adjusted(4, 0, -5, 0),
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
    const bool documentNotes = current && &notes == &daw::midiNotes(*current);
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
    if (m_resizing) return &m_geometryPaintNotes;
    if (m_selectionEditUndoActive) return &m_selectionEditWorking;
    return nullptr;
}

void PianoRollView::ensureDocumentNoteIdIndex(
    const daw::ClipModel& current) const {
    const std::uint64_t revision =
        m_controller
            ? m_controller->midiNotesRevision(m_trackId.toStdString())
            : 0;
    if (m_noteIdIndexSource == &daw::midiNotes(current) &&
        m_noteIdIndexCount == daw::midiNotes(current).size() &&
        m_noteIdIndexRevision == revision) {
        return;
    }
    m_noteById.clear();
    m_noteById.reserve(daw::midiNotes(current).size());
    for (std::size_t i = 0; i < daw::midiNotes(current).size(); ++i) {
        if (!daw::midiNotes(current)[i].id.empty())
            m_noteById.try_emplace(daw::midiNotes(current)[i].id, i);
    }
    m_noteIdIndexSource = &daw::midiNotes(current);
    m_noteIdIndexCount = daw::midiNotes(current).size();
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
    ensureSoundingPitchIndex(daw::midiNotes(*c));
    return m_soundingPitchIndex.pitchesAt(beat);
}

PianoRollView::PitchMask PianoRollView::soundingPitches() const {
    const auto* c = clip();
    if (!c || !m_controller || !m_controller->isPlaying()) return {};
    const double at = daw::secondsToBeats(
        m_controller->presentationPositionSeconds() - c->startSeconds + c->offsetSeconds,
        m_controller->project().tempo);
    ensureSoundingPitchIndex(daw::midiNotes(*c));
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

    if (ui::graphics::isSceneRecording(p) && !m_laneDragging && !m_laneResizing &&
        !(m_pointerInside && m_pointer.y() >= fieldBottom - kLaneGripPx)) {
        const int tileWidth = std::max(1, std::min(512, int(parameterField.width())));
        const double scroll = m_scrollX;
        for (qint64 tile = qint64(std::floor(scroll / tileWidth));
             tile * tileWidth < scroll + parameterField.width(); ++tile) {
            const double origin = tile * tileWidth - scroll;
            p.save();
            p.setClipRect(parameterField.intersected(QRectF(keyWidth + origin, fieldBottom,
                                                          tileWidth, laneHeight())), Qt::IntersectClip);
            m_gpuLaneTiles.paint(p, quint64(tile), size(), QPointF(origin, 0), [&](QPainter& local) {
                QScopedValueRollback<double> sx(m_scrollX, double(tile * tileWidth));
                local.setClipRect(QRectF(keyWidth, fieldBottom, tileWidth, laneHeight()));
                paintLaneValues(local);
            });
            p.restore();
        }
    } else paintLaneValues(p);

    if (m_laneRamping) {
        p.save();
        p.setClipRect(parameterField, Qt::IntersectClip);
        const QPointF anchor(beatsToX(m_laneRampAnchor.x()),
                             laneValueToY(m_laneRampAnchor.y()));
        const QPointF endpoint(beatsToX(m_laneRampEnd.x()),
                               laneValueToY(m_laneRampEnd.y()));
        p.setPen(QPen(laneAccent, 1.5));
        p.drawLine(anchor, endpoint);
        p.setBrush(laneAccent);
        p.drawEllipse(anchor, 3.0, 3.0);
        p.drawEllipse(endpoint, 3.0, 3.0);
        p.restore();
    }

    // The value of whatever is being dragged, so a move is readable.
    if (m_laneDragging && !m_laneRamping && !m_primary.isEmpty()) {
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

void PianoRollView::paintLaneValues(QPainter& p) {
    const auto* c = clip();
    if (!c) return;
    const Theme& t = th();
    const double keyWidth = keyboardWidth(), fieldBottom = laneTop();
    const QRectF parameterField(keyWidth, fieldBottom, std::max(0., width() - keyWidth), laneHeight());
    const QRectF visible = p.hasClipping() ? parameterField.intersected(p.clipBoundingRect()) : parameterField;
    const QColor laneAccent = m_laneParam == LaneParam::Controller ? Theme::automationAccent() :
                              m_laneParam == LaneParam::Pan ? Theme::audioAccent() : t.accent;
    const auto* paintedController = m_laneParam == LaneParam::Controller ? controllerLane() : nullptr;
    p.save();
    p.setClipRect(parameterField, Qt::IntersectClip);

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
            return;
        }
        // A curve, not a bar per note: a controller is continuous, and the
        // whole point of the lane is the shape between the breakpoints.
        const double defaultY = laneValueToY(paintedController->defaultValue);
        const double visibleFirstBeat = xToBeats(visible.left() - 2.0);
        const double visibleLastBeat = xToBeats(visible.right() + 2.0);
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
    const auto& laneNotes = m_preview ? *m_preview : daw::midiNotes(*c);
    const daw::MidiPreviewIndex& laneNoteIndex =
        notePaintIndexFor(laneNotes);
    m_notePaintScratch.clear();
    laneNoteIndex.forEachVisible(
        laneNotes, xToBeats(visible.left() - kHandleGrabPx),
        xToBeats(visible.right() + kHandleGrabPx),
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

}

// ── Input ───────────────────────────────────────────────────────────────────

void PianoRollView::mousePressEvent(QMouseEvent* ev) {
    if (hasActivePointerGesture()) finishInterruptedPointerGesture();
    m_pointerButton = ev->button();
    m_lastPointerPosition = ev->position();
    if (!clip()) return;
    finishWheelNoteEdit();
    rememberNoteProperties(note(m_primary));
    setFocus(Qt::MouseFocusReason);
    const QPointF pos = ev->position();
    m_pointer = pos;
    m_pointerInside = true;

    if (slidePress(ev)) return;

    // The upper band selects a local editing range. Only the bar-number ruler
    // below it seeks the project transport; neither changes playback cycle.
    if (pos.y() < ui::kLoopStripHeight && ev->button() == Qt::LeftButton &&
        pos.x() >= keyboardWidth() && clip()) {
        // Clearing even a very narrow range must work between its resize grips.
        if (ev->type() == QEvent::MouseButtonDblClick &&
            m_timeRange.valid() && pos.x() >= beatsToX(m_timeRange.from) &&
            pos.x() <= beatsToX(m_timeRange.to)) {
            m_rangeGrab = RangeGrab::None;
            m_timeRange = {};
            emitStatus();
            update();
            ev->accept();
            return;
        }

        const bool snapOn = m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        m_rangeGrab = rangeGrabAt(pos.x());
        const double at = std::max(0.0, snapBeats(xToBeats(pos.x()), snapOn));
        switch (m_rangeGrab) {
            case RangeGrab::Create:
                m_rangeAnchorBeats = at;
                m_timeRange = {at, at};
                break;
            case RangeGrab::Move:
                m_rangeGrabOffset = at - m_timeRange.from;
                m_rangeGrabLength = m_timeRange.to - m_timeRange.from;
                break;
            case RangeGrab::ResizeStart:
                m_rangeAnchorBeats = m_timeRange.to;
                break;
            case RangeGrab::ResizeEnd:
                m_rangeAnchorBeats = m_timeRange.from;
                break;
            case RangeGrab::None:
                break;
        }
        setCursor(m_rangeGrab == RangeGrab::Move ? Qt::ClosedHandCursor
                                               : Qt::SizeHorCursor);
        update();
        ev->accept();
        return;
    }

    if (pos.y() < ui::kRulerHeight) {
        if (ev->button() == Qt::LeftButton && pos.x() >= keyboardWidth()) {
            m_scrubbingPlayhead = true;
            const bool snapOn =
                m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
            seekToLocalBeat(xToBeats(pos.x()), snapOn);
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
    const bool toggle = !additive &&
        hasPrimarySelectionModifier(ev->modifiers());
    m_duplicateDragPending = false;
    m_duplicateDragCreated = false;

    if (ev->button() == Qt::RightButton && m_showVelocityLane &&
        m_laneParam == LaneParam::Velocity && !m_preview &&
        pos.x() >= keyboardWidth() && pos.y() > laneTop() + kLaneGripPx) {
        double beat = std::max(0.0, xToBeats(pos.x()));
        if (const auto* hit = note(handleAt(pos))) beat = hit->startBeats;
        m_laneRampAnchor = QPointF(beat, std::max(1.0 / 127.0, laneValueAtY(pos.y())));
        m_laneRampEnd = m_laneRampAnchor;
        m_laneOrig = daw::midiNotes(*clip());
        m_noteUpdateScratch.reserve(m_laneOrig.size());
        m_laneDragging = m_laneRamping = true;
        m_suppressContextMenu = true;
        setCursor(Qt::CrossCursor);
        // The anchor is a note onset when a column is hit; keep that exact beat
        // for the press update even if the pointer landed beside its stem.
        updateVelocityRamp(QPointF(beatsToX(beat), pos.y()));
        ev->accept();
        return;
    }

    // The right button erases in every grid mode; the Erase tool uses the same
    // stroke with the left button. Holding and sweeping rubs out a run of notes.
    // Arming it anywhere over the grid, not only on top of a note, is what
    // makes that sweep usable: you start the stroke on empty space and rub
    // across whatever is in the way. The whole travelled segment is tested so
    // coalesced mouse-move events cannot leave notes between samples behind.
    const bool rightErase = ev->button() == Qt::RightButton;
    const bool leftErase = ev->button() == Qt::LeftButton &&
        activeTool() == Tool::Erase &&
        !(ev->modifiers() & Qt::ControlModifier);
    if (rightErase || leftErase) {
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
            m_suppressContextMenu = rightErase;
            updateCursor(pos);
            m_eraseChanged |= eraseStroke(pos, pos);
        }
        if (rightErase || (pos.x() >= keyboardWidth() && pos.y() < laneTop()))
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
                added.id = daw::newUuid();
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
        if (toggle) {
            toggleSelected(hit);
            update();
            return;
        }
        // Grabbing an unselected handle selects that note first, so a drag can
        // still only ever move notes that are selected.
        if (!m_selected.contains(hit)) {
            if (additive) toggleSelected(hit); else selectOnly(hit);
        }
        m_primary = hit;
        rememberNoteProperties(note(hit));
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
        case Tool::Erase:
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

        if (toggle) {
            toggleSelected(hit);
            update();
            return;
        }

        const bool wasSelected = m_selected.contains(hit);
        if (additive && !wasSelected) toggleSelected(hit);
        // Clicking a note that is already part of a multi-selection keeps the
        // selection, so a group can be dragged; clicking elsewhere replaces it.
        if (!m_selected.contains(hit)) selectOnly(hit);
        m_primary = hit;
        rememberNoteProperties(n);
        m_grabBeats = xToBeats(pos.x()) - n->startBeats;
        // The left edge resizes from the head, keeping the note's end put.
        const bool onLeftEdge = pos.x() <= noteRect(*n).left() + kEdgePx;
        // Shift owns duplication even at an edge; copied notes move as a group
        // while every original stays exactly where it was.
        m_resizing = !additive && (onEdge || onLeftEdge);
        m_resizingLeft = m_resizing && onLeftEdge && !onEdge;
        m_moving = !m_resizing;
        m_duplicateDragPending = additive;
        m_movePress = pos;
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

    // Place the last-used note properties immediately, then move the new note
    // as a whole. Only grabbing an existing edge changes its length.
    const double drawLength = std::max(m_lastLength, kMinNoteBeats);
    const std::string noteId = m_controller->addNote(
        m_trackId.toStdString(), m_clipId.toStdString(),
        snapPitch(yToPitch(pos.y())), noteStartBeats(xToBeats(pos.x()), snapOn),
        drawLength, m_lastVelocity);
    if (noteId.empty()) return;
    m_controller->setNotePan(m_trackId.toStdString(), m_clipId.toStdString(),
                             noteId, m_lastPan);
    invalidateSoundingPitchIndex();
    // Note: `clip()` and any NoteModel* taken before this call are now stale —
    // addNote pushes into the vector and can reallocate it.
    selectOnly(QString::fromStdString(noteId));
    m_lastLength = drawLength;
    m_drawing = true;
    m_resizing = false;
    m_resizingLeft = false;
    m_moving = true;
    if (const auto* drawn = note(QString::fromStdString(noteId))) {
        m_moveWorking = {*drawn};
        m_grabBeats = xToBeats(pos.x()) - drawn->startBeats;
        auditionPitch(drawn->pitch);
    }
    emit edited();
    update();
}

void PianoRollView::mouseMoveEvent(QMouseEvent* ev) {
    if (slideMove(ev)) return;
    if (hasActivePointerGesture() && m_pointerButton != Qt::NoButton &&
        !(ev->buttons() & m_pointerButton)) {
        finishInterruptedPointerGesture();
        updateCursor(ev->position());
        return;
    }
    if (m_pointerButton != Qt::NoButton && (ev->buttons() & m_pointerButton))
        m_lastPointerPosition = ev->position();
    const QPointF pos = ev->position();
    m_pointer = pos;
    m_pointerInside = true;
    if (m_rangeGrab != RangeGrab::None) {
        const bool snapping = m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        updateTimeRangeDrag(pos.x(), snapping);
        update();
        return;
    }

    if (m_scrubbingPlayhead) {
        const bool snapOn =
            m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        seekToLocalBeat(xToBeats(pos.x()), snapOn);
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
            const auto& index = notePaintIndexFor(daw::midiNotes(*c));
            const double minimumPaintBeats =
                3.0 / std::max(1.0, pxPerBeat());
            index.forEachVisible(
                daw::midiNotes(*c), xToBeats(box.left()) - minimumPaintBeats,
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

    if (m_laneRamping) {
        updateVelocityRamp(pos);
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

    if (m_moving && m_duplicateDragPending) {
        if ((pos - m_movePress).manhattanLength() <
            QApplication::startDragDistance()) {
            return;
        }
        const auto* current = clip();
        if (!current || m_moveWorking.empty()) return;

        mt::Notes merged = daw::midiNotes(*current);
        auto copies=m_moveWorking;std::vector<daw::SlideNoteModel> copiedSlides;
        for(auto slide:daw::slides::editable(*current)) {
            auto belongs=[&](const std::string& id){return std::any_of(copies.begin(),copies.end(),[&](const auto& n){return n.id==id;});};
            if(!belongs(slide.referenceNoteId))continue;
            std::erase_if(slide.targetNoteIds,[&](const auto& id){return !belongs(id);});copiedSlides.push_back(std::move(slide));
        }
        daw::slides::reidentify(copies,copiedSlides);QSet<QString> copiedIds;QString copiedPrimary;
        for(std::size_t i=0;i<copies.size();++i){if(m_moveWorking[i].id==m_primary.toStdString())copiedPrimary=QString::fromStdString(copies[i].id);copiedIds.insert(QString::fromStdString(copies[i].id));merged.push_back(copies[i]);}
        m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                                   std::move(merged), "Duplicate Notes");
        auto gestures=daw::slides::editable(*clip());gestures.insert(gestures.end(),copiedSlides.begin(),copiedSlides.end());
        m_controller->setClipSlideNotes(m_trackId.toStdString(),m_clipId.toStdString(),std::move(gestures),"Duplicate Notes",false);
        invalidateSoundingPitchIndex();
        m_moveWorking = std::move(copies);
        m_selected = std::move(copiedIds);
        m_primary = copiedPrimary;
        m_duplicateDragPending = false;
        m_duplicateDragCreated = true;
        emit selectionChanged();
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
        if (!m_resizeOrig.empty()) {
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
            }
            m_controller->setNoteStates(m_trackId.toStdString(),
                                        m_clipId.toStdString(),
                                        m_noteUpdateScratch);
            m_geometryPaintNotes = m_noteUpdateScratch;
        }
    } else {
        // The whole selection travels with the grabbed note.
        const double start = m_drawing
            ? noteStartBeats(xToBeats(pos.x()), snapOn)
            : snapBeats(xToBeats(pos.x()) - m_grabBeats, snapOn);
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
        // Only the new note being placed follows the pointer audibly. Moving
        // existing notes, including Shift-drag copies, is a silent edit.
        if (m_drawing && m_pointerButton != Qt::NoButton) auditionPitch(pitch);
    }
    invalidateSoundingPitchIndex();
    update();
}

void PianoRollView::mouseReleaseEvent(QMouseEvent* ev) {
    stopAudition();
    if (slideRelease()) { m_pointerButton = Qt::NoButton; ev->accept(); return; }
    m_pointerButton = Qt::NoButton;
    if (m_rangeGrab != RangeGrab::None) {
        // Qt can coalesce the final move into release. Apply that endpoint
        // before ending the gesture, including when no move event arrived.
        const bool snapping = m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        updateTimeRangeDrag(ev->position().x(), snapping);
        m_rangeGrab = RangeGrab::None;
        if (!m_timeRange.valid()) m_timeRange = {};
        updateCursor(ev->position());
        emitStatus();
        update();
        ev->accept();
        return;
    }
    if (m_scrubbingPlayhead) {
        const bool snapOn =
            m_snapEnabled != bool(ev->modifiers() & Qt::AltModifier);
        seekToLocalBeat(xToBeats(ev->position().x()), snapOn);
        m_scrubbingPlayhead = false;
        updateCursor(ev->position());
        ev->accept();
        return;
    }
    if (m_drawing || (m_duplicateDragPending &&
        (ev->position() - m_movePress).manhattanLength() >=
            QApplication::startDragDistance())) {
        // Some platforms coalesce the final movement into the release event.
        // Feed that endpoint through the normal move path so a quick
        // Shift-drag or drawing gesture cannot lose its final position.
        QMouseEvent finalMove(QEvent::MouseMove, ev->position(),
                              ev->globalPosition(), Qt::NoButton,
                              Qt::LeftButton, ev->modifiers());
        mouseMoveEvent(&finalMove);
    }
    if (m_laneRamping) updateVelocityRamp(ev->position());
    // One signal per gesture, not per move: the moves themselves are live edits.
    const bool changed =
        (m_moving && !m_duplicateDragPending) || m_resizing || m_laneDragging ||
        m_eraseChanged || m_muting || m_stretching;
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
        m_controller->endNoteEdit(m_eraseChanged
                                      ? "Erase Notes"
                                      : (m_duplicateDragCreated
                                             ? "Duplicate Notes"
                                             : "Edit Notes"));
        m_gestureUndoActive = false;
    }
    m_moving = false;
    m_resizing = false;
    m_resizingLeft = false;
    m_resizeOrig.clear();
    m_moveWorking.clear();
    m_duplicateDragPending = false;
    m_duplicateDragCreated = false;
    m_geometryPaintNotes.clear();
    m_laneDragging = m_laneRamping = false;
    m_marquee = false;
    m_erasing = false;
    m_eraseChanged = false;
    m_drawing = false;
    rememberNoteProperties(note(m_primary));
    m_muting = false;
    m_laneResizing = false;
    m_lanePointDrag = -1;
    m_pressedKey = -1;
    stopAudition();
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
    for (const auto& n : daw::midiNotes(*c)) {
        if (!m_selected.contains(QString::fromStdString(n.id)))
            untouched.push_back(n);
    }
    mt::Notes merged = std::move(untouched);
    merged.insert(merged.end(), m_stretchPreview.begin(), m_stretchPreview.end());
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                               merged, "Stretch Notes", true);
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
        ui::ScrollMotion::cancel(this);
        // Pixel deltas are far smaller than a notch, so the exponent keeps a
        // pinch-less trackpad zoom moving at the same rate as a wheel.
        zoomHorizontal(std::pow(1.0015, fine ? dy : dy * 0.9));
        ev->accept();
        return;
    }
    if (modifiers & Qt::AltModifier) {
        ui::ScrollMotion::cancel(this);
        zoomVertical(std::pow(1.0015, fine ? dy : dy * 0.9));
        ev->accept();
        return;
    }
    // Over the parameter lane the wheel edits the selection's value — the one
    // gesture that changes several notes at once.
    if (m_showVelocityLane && ev->position().y() >= laneTop() &&
        !m_selected.isEmpty() && m_laneParam != LaneParam::Controller) {
        ui::ScrollMotion::cancel(this);
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
    const QPointF delta=modifiers&Qt::ShiftModifier ? QPointF(-dy-dx,0) : QPointF(-dx,-dy);
    ui::ScrollMotion::scroll(this,delta,fine,
        [this]{return QPointF(m_scrollX,m_scrollY);},
        [this](QPointF p){setScrollX(p.x());setScrollY(p.y());emit viewportChanged();ui::FrameWidget::update();});
    ev->accept();
}

bool PianoRollView::hasActivePointerGesture() const {
    return m_slideGesture || m_rangeGrab != RangeGrab::None || m_scrubbingPlayhead || m_marquee ||
           m_moving || m_resizing || m_laneDragging || m_erasing || m_muting ||
           m_stretching || m_drawing || m_laneResizing || m_lanePointDrag >= 0 ||
           m_pressedKey >= 0 || m_gestureUndoActive;
}

void PianoRollView::finishInterruptedPointerGesture() {
    if (!hasActivePointerGesture()) return;
    const Qt::MouseButton button = m_pointerButton == Qt::NoButton
        ? Qt::LeftButton : m_pointerButton;
    QMouseEvent release(QEvent::MouseButtonRelease, m_lastPointerPosition,
                        mapToGlobal(m_lastPointerPosition), button,
                        Qt::NoButton, QApplication::keyboardModifiers());
    mouseReleaseEvent(&release);
}

bool PianoRollView::event(QEvent* ev) {
    if (ev->type() == QEvent::MouseButtonDblClick) { auto* e = static_cast<QMouseEvent*>(ev); const auto id = slideAt(e->position()); if (!id.empty() && e->button() == Qt::LeftButton) { slideRelease(); openSlideEditor(id); return true; } }
    if (ev->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(ev);
        if (clip() && help->pos().y() < ui::kLoopStripHeight &&
            help->pos().x() >= keyboardWidth()) {
            QToolTip::showText(help->globalPos(),
                tr("Local range for copying and repeating notes. Double-click to clear; the timeline loop is unchanged."), this);
        } else if (clip() && m_showVelocityLane && m_laneParam == LaneParam::Velocity &&
                   help->pos().x() >= keyboardWidth() &&
                   help->pos().y() > laneTop() + kLaneGripPx) {
            QToolTip::showText(help->globalPos(),
                tr("Right-drag to draw a velocity ramp."), this);
        } else {
            QToolTip::hideText();
            ev->ignore();
        }
        return true;
    }
    if (ev->type() == QEvent::FocusOut ||
        ev->type() == QEvent::UngrabMouse || ev->type() == QEvent::Hide ||
        ev->type() == QEvent::WindowDeactivate)
        finishInterruptedPointerGesture();
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
    if (slideKey(ev)) return;
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
        // A right-button edit already owns this press; do not open a menu over it.
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
    const auto& index = notePaintIndexFor(daw::midiNotes(*c));
    const double beat = xToBeats(pos.x());
    const double minimumPaintBeats = 3.0 / std::max(1.0, pxPerBeat());
    std::size_t bestIndex = 0;
    const daw::NoteModel* best = nullptr;
    // Choose the highest document index, exactly matching the former reverse
    // scan and therefore the visual stacking order for overlapping notes.
    index.forEachVisible(
        daw::midiNotes(*c), beat - minimumPaintBeats,
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
    // The whole column is draggable. Prefer a selected stalk within the grab
    // radius: recorded chords have slightly different onsets, so an invisible
    // subpixel distance must not redirect a drag to an unselected chord tone.
    const auto& index = notePaintIndexFor(daw::midiNotes(*c));
    std::size_t bestIndex = 0;
    bool bestSelected = false;
    QString best;
    double bestDistance = kHandleGrabPx;
    const double beat = xToBeats(pos.x());
    const double beatMargin = kHandleGrabPx / std::max(1.0, pxPerBeat());
    index.forEachVisible(daw::midiNotes(*c), beat - beatMargin,
                         beat + beatMargin +
                             1.0 / std::max(1.0, pxPerBeat()),
                         [&](const daw::NoteModel& n,
                             std::size_t candidateIndex) {
        const double distance = std::abs(beatsToX(n.startBeats) - pos.x());
        if (distance > kHandleGrabPx) return;
        const bool selected = m_selected.contains(QString::fromStdString(n.id));
        if (best.isEmpty() || (selected && !bestSelected) ||
            (selected == bestSelected &&
             (distance < bestDistance ||
              (distance == bestDistance && candidateIndex > bestIndex)))) {
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
    if (m_laneRamping) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (m_erasing) {
        setCursor(toolCursor(icons::Glyph::Eraser));
        return;
    }
    if (pos.y() < ui::kLoopStripHeight && pos.x() >= keyboardWidth()) {
        setCursor(rangeGrabAt(pos.x()) == RangeGrab::Move ? Qt::OpenHandCursor
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
        case Tool::Erase:
            setCursor(toolCursor(icons::Glyph::Eraser));
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
    const auto& index = notePaintIndexFor(daw::midiNotes(*c));
    const double minimumPaintBeats = 3.0 / std::max(1.0, pxPerBeat());
    const double fromBeat = xToBeats(std::min(from.x(), to.x()));
    const double toBeat = xToBeats(std::max(from.x(), to.x()));
    bool newHit = false;
    index.forEachVisible(
        daw::midiNotes(*c), fromBeat - minimumPaintBeats,
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
    for (const auto& n : daw::midiNotes(*c)) {
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
    auto gestures=daw::slides::editable(*c);
    const auto slideCurves=daw::slides::compile(daw::midiNotes(*c),gestures);
    for(const auto& source:daw::midiNotes(*c))if(source.startBeats<at&&source.startBeats+source.lengthBeats>at&&(acrossAllNotes||source.id==hit.toStdString())) {
        auto right=std::find_if(result.begin(),result.end(),[&](const auto& n){return n.startBeats==at&&n.pitch==source.pitch&&n.id!=source.id;});
        if(right!=result.end()){auto slice=daw::slides::crop({source},gestures,{*right},at,source.startBeats+source.lengthBeats,&slideCurves);for(auto& slide:slice){slide.startBeats+=at;gestures.push_back(std::move(slide));}}
    }
    m_controller->setClipMidiObjects(m_trackId.toStdString(),m_clipId.toStdString(),result,std::move(gestures),"Slice Notes");
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

void PianoRollView::refreshPlayheadFrame() {
    if (!m_controller || !isVisible()) return;
    if (m_controller->isPlaying() && !m_auditionPerformance) stopAudition();

    double currentX = -1.0;
    if (const auto* c = clip()) {
        const double beats = daw::secondsToBeats(
            m_controller->presentationPositionSeconds() - c->startSeconds + c->offsetSeconds,
            m_controller->project().tempo);
        if (beats >= 0.0 && beats <= clipBeats()) {
            const double x = beatsToX(beats);
            if (x >= keyboardWidth() && x <= width())
                currentX = x;
        }
    }

    QRegion dirty;
    const int playheadBottom = int(std::ceil(laneTop()));
    const auto addPlayhead = [&](double x) {
        const double reach = 10.0 + ui::playheadWidth();
        if (x >= 0) dirty += QRectF(x - reach, 0.0, reach * 2.0, playheadBottom).toAlignedRect();
    };
    // Advancing within one logical pixel still changes antialiased coverage.
    if (currentX != m_lastPlayheadX) {
        addPlayhead(m_lastPlayheadX);
        addPlayhead(currentX);
        m_lastPlayheadX = currentX;
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
    // Piano keys remain a performance input during playback. Placing notes
    // previews the instrument only while the project transport is stopped.
    if (m_pressedKey < 0 && m_controller->isPlaying()) {
        stopAudition();
        return;
    }
    if (pitch == m_auditionPitch) return;
    stopAudition();
    if (pitch < 0 || pitch > 127 || m_trackId.isEmpty()) return;
    // The velocity a drawn note would get, so the click previews what writing
    // the note there would sound like.
    m_auditionPerformance = m_pressedKey >= 0;
    if (m_controller->liveMidiInput(m_trackId.toStdString(), 0x90, pitch, m_lastVelocity,
            1ULL << 62, m_controller->midiInputStamp(), m_auditionPerformance
                ? daw::LiveMidiOrigin::Performance : daw::LiveMidiOrigin::Audition))
        m_auditionPitch = pitch;
}

void PianoRollView::stopAudition() {
    if (m_auditionPitch < 0) return;
    m_controller->liveMidiInput(m_trackId.toStdString(), 0x80, m_auditionPitch, 0,
        1ULL << 62, m_controller->midiInputStamp(), m_auditionPerformance
            ? daw::LiveMidiOrigin::Cleanup : daw::LiveMidiOrigin::Audition);
    m_auditionPitch = -1;
}

void PianoRollView::emitStatus() {
    QString toolName;
    switch (activeTool()) {
        case Tool::Draw:   toolName = tr("Draw"); break;
        case Tool::Select: toolName = tr("Select"); break;
        case Tool::Slice:  toolName = tr("Slice"); break;
        case Tool::Mute:   toolName = tr("Disable"); break;
        case Tool::Erase:  toolName = tr("Erase"); break;
        case Tool::Slide: toolName = tr("Slide"); break;
    }
    QString text = tr("%1 · %2 notes selected").arg(toolName).arg(m_selected.size());
    if (m_preview) text = tr("%1 · previewing — press Apply to keep it").arg(toolName);
    emit statusChanged(text);
}

// ── PianoRollWindow ─────────────────────────────────────────────────────────

PianoRollWindow::PianoRollWindow(daw::EngineController* controller,
                                 QWidget* parent, QAction* undoAction,
                                 QAction* redoAction)
    : QWidget(parent), m_controller(controller) {
    m_undoAction = undoAction;
    m_redoAction = redoAction;
    m_sharedUndoAction = undoAction != nullptr;
    m_sharedRedoAction = redoAction != nullptr;
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

    m_hScroll = new QScrollBar(Qt::Horizontal, this);
    m_hScroll->setObjectName(QStringLiteral("PianoRollNavigator"));
    m_hScroll->setFixedHeight(12);
    m_hScroll->setAccessibleName(tr("Piano roll horizontal scroll"));
    m_hScroll->setToolTip(tr("Move left or right through the clip"));
    m_vScroll = new QScrollBar(Qt::Vertical, this);
    m_vScroll->setObjectName(QStringLiteral("PianoRollVerticalScroll"));
    m_vScroll->setFixedWidth(ui::kTimelineScrollExtent);
    m_vScroll->setAccessibleName(tr("Piano roll vertical scroll"));
    m_vScroll->setToolTip(tr("Move up or down through the notes"));
    m_hScroll->setFocusPolicy(Qt::NoFocus);
    m_vScroll->setFocusPolicy(Qt::NoFocus);
    connect(m_hScroll, &QScrollBar::valueChanged, this, [this](int value) {
        m_view->setScrollX(double(value));
    });
    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int value) {
        m_view->setScrollY(double(value));
    });

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    buildMenus();
    buildToolbar();

    // A shortcut only fires while its action belongs to the focused editor. A
    // menu hanging off a toolbar button does not provide that association, so
    // every action is registered on this panel by hand. The scoped context is
    // important now that the roll and arrangement share MainWindow: clicking
    // the timeline must hand its shortcuts straight back to the timeline.
    std::function<void(QMenu*)> registerActions = [&](QMenu* menu) {
        for (QAction* action : menu->actions()) {
            // Shared project actions keep their window scope and user binding.
            if ((m_sharedUndoAction && action == m_undoAction) ||
                (m_sharedRedoAction && action == m_redoAction)) continue;
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

    // The same rails and relative scrub controls as the arrangement. Time
    // navigation stays above the ruler; pitch navigation sits on its right.
    auto* navigationHost = new QWidget(this);
    navigationHost->setObjectName(QStringLiteral("PianoRollTopNavigation"));
    navigationHost->setAttribute(Qt::WA_StyledBackground);
    auto* navigation = new QHBoxLayout(navigationHost);
    navigation->setContentsMargins(0, 0, 0, 0);
    navigation->setSpacing(0);
    m_navigationKeyboardSpace = new QWidget(this);
    m_navigationKeyboardSpace->setFixedWidth(int(kKeyboardWidth));
    navigation->addWidget(m_navigationKeyboardSpace);
    navigation->addWidget(m_hScroll, 1);
    navigation->addSpacing(ui::kTimelineScrollExtent);
    column->addWidget(navigationHost);

    auto* grid = new QHBoxLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    grid->addWidget(m_view, 1);
    auto* rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("PianoRollNavigationRail"));
    rail->setFixedWidth(ui::kTimelineScrollExtent);
    auto* railColumn = new QVBoxLayout(rail);
    railColumn->setContentsMargins(0, 0, 0, 0);
    railColumn->setSpacing(0);
    m_navigationRulerSpace = new QWidget(rail);
    m_navigationRulerSpace->setFixedHeight(ui::kRulerHeight);
    railColumn->addWidget(m_navigationRulerSpace);
    auto* controls = new ui::ViewControlStrip(rail);
    controls->setObjectName(QStringLiteral("PianoRollViewControls"));
    auto* controlsColumn = new QVBoxLayout(controls);
    controlsColumn->setContentsMargins(0, 0, 0, 0);
    controlsColumn->setSpacing(0);
    auto* fit = new ui::IconButton(icons::Glyph::ZoomFit, tr("Fit the clip to the window"), controls);
    fit->setObjectName(QStringLiteral("PianoRollZoomFit"));
    fit->setAccessibleName(fit->toolTip());
    fit->setFocusPolicy(Qt::StrongFocus);
    fit->setButtonSize(ui::kViewControlWidth, ui::kViewControlHeight);
    connect(fit, &QAbstractButton::clicked, this, [this] { m_view->zoomToFit(); });
    controlsColumn->addWidget(fit);
    m_timeZoom = new ui::ViewScrubSlider(icons::Glyph::ResizeHorizontal,
        ui::ViewScrubSlider::Axis::Horizontal, pianoZoomToSlider(kMinFitPxPerBeat), controls);
    m_timeZoom->setObjectName(QStringLiteral("PianoRollTimeZoom"));
    m_timeZoom->setRange(0, kPianoZoomSteps);
    m_timeZoom->setSingleStep(12);
    m_timeZoom->setPageStep(60);
    m_timeZoom->setAccessibleName(tr("Note width / horizontal zoom"));
    m_timeZoom->setToolTip(tr("Note width: drag left or right; Shift for precision; double-click resets"));
    connect(m_timeZoom, &QSlider::valueChanged, this, [this](int value) {
        m_view->zoomHorizontal(pianoZoomFromSlider(value) / m_view->effectivePixelsPerBeat());
    });
    controlsColumn->addWidget(m_timeZoom);
    m_noteHeight = new ui::ViewScrubSlider(icons::Glyph::ResizeVertical,
        ui::ViewScrubSlider::Axis::Vertical, 120, controls);
    m_noteHeight->setObjectName(QStringLiteral("NoteHeightScrubber"));
    m_noteHeight->setRange(int(kMinRowHeight * 10), int(kMaxRowHeight * 10));
    m_noteHeight->setSingleStep(5);
    m_noteHeight->setPageStep(20);
    m_noteHeight->setAccessibleName(tr("Note height"));
    m_noteHeight->setToolTip(tr("Note height: drag up or down; Shift for precision; double-click resets"));
    connect(m_noteHeight, &QSlider::valueChanged, this, [this](int value) {
        m_view->zoomVertical((value / 10.0) / m_view->rowHeight());
    });
    controlsColumn->addWidget(m_noteHeight);
    QWidget::setTabOrder(fit, m_timeZoom);
    QWidget::setTabOrder(m_timeZoom, m_noteHeight);
    for (auto* control : {m_timeZoom, m_noteHeight}) {
        connect(control, &QSlider::sliderMoved, this, [this, control](int value) {
            const QString text = control == m_timeZoom
                ? tr("Zoom %1%").arg(int(std::lround(pianoZoomFromSlider(value) / kMinFitPxPerBeat * 100)))
                : tr("Note height %1 px").arg(value / 10.0, 0, 'f', 1);
            ui::ValueBubble::showFor(control, control->rect().center(), text);
        });
        connect(control, &QSlider::sliderReleased, this, [] { ui::ValueBubble::dismiss(); });
    }
    railColumn->addWidget(controls);
    railColumn->addWidget(m_vScroll, 1);
    m_navigationLaneSpace = new QWidget(rail);
    railColumn->addWidget(m_navigationLaneSpace);
    grid->addWidget(rail);
    column->addLayout(grid, 1);

    // Keep the parameter picker directly under the lane it drives.
    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("PianoRollFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* bottom = new QHBoxLayout(footer);
    bottom->setContentsMargins(8, 5, 8, 5);
    bottom->setSpacing(4);
    m_laneSelector = new QComboBox(this);
    m_laneSelector->setToolTip(
        tr("What the lane along the bottom edits: note velocity, note pan, or a "
           "controller curve."));
    m_laneSelector->setMinimumWidth(150);
    m_laneSelector->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_laneSelector, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
        if (!track) return;
        const daw::ControllerLane* lane = nullptr;
        for (const auto& clip : track->clips) if (clip.id == m_clipId.toStdString())
            for (const auto& candidate : daw::midiLanes(clip)) if (candidate.id == m_view->laneId().toStdString()) lane = &candidate;
        if (!lane || lane->parameterId.empty() || !lane->slotId.empty()) return;
        const auto parameter = lane->parameterId;
        QMenu menu(this);
        auto* learn = menu.addAction(tr("Назначить MIDI-ручку"));
        auto* cancel = menu.addAction(tr("Отменить обучение"));
        cancel->setEnabled(m_controller->isMidiLearning());
        auto* remove = menu.addAction(tr("Удалить назначение"));
        const auto* picked = menu.exec(m_laneSelector->mapToGlobal(pos));
        if (picked == learn) m_controller->beginMidiLearn(track->id, parameter);
        if (picked == cancel) m_controller->cancelMidiLearn();
        if (picked == remove) m_controller->removeMidiLearn(track->id, parameter);
    });
    connect(m_laneSelector, &QComboBox::currentIndexChanged, this,
            &PianoRollWindow::laneSelectionChanged);
    bottom->addWidget(m_laneSelector);
    auto* addLane = new QToolButton(this);
    addLane->setIcon(icons::icon(icons::Glyph::Plus, th().textPrimary, 16));
    addLane->setProperty("pianoGlyph", int(icons::Glyph::Plus));
    addLane->setToolTip(tr("Add a controller lane"));
    addLane->setAccessibleName(addLane->toolTip());
    addLane->setFixedSize(28, 28);
    addLane->setAutoRaise(true);
    connect(addLane, &QToolButton::clicked, this, &PianoRollWindow::addControllerLane);
    bottom->addWidget(addLane);
    m_removeLaneButton = new QToolButton(this);
    m_removeLaneButton->setIcon(icons::icon(icons::Glyph::Trash, th().textPrimary, 16));
    m_removeLaneButton->setProperty("pianoGlyph", int(icons::Glyph::Trash));
    m_removeLaneButton->setToolTip(tr("Remove this controller lane"));
    m_removeLaneButton->setAccessibleName(m_removeLaneButton->toolTip());
    m_removeLaneButton->setFixedSize(28, 28);
    m_removeLaneButton->setAutoRaise(true);
    connect(m_removeLaneButton, &QToolButton::clicked, this,
            &PianoRollWindow::removeControllerLane);
    bottom->addWidget(m_removeLaneButton);
    bottom->addStretch(1);
    auto* chords = new QToolButton(footer);
    chords->setObjectName(QStringLiteral("PianoRollBuildChordsButton"));
    chords->setProperty("pianoGlyph", int(icons::Glyph::Chord));
    chords->setIcon(icons::icon(icons::Glyph::Chord, th().textPrimary, 16));
    chords->setText(tr("Chords…"));
    chords->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    chords->setToolTip(tr("Chord Generator (%1)").arg(m_chordAction->shortcut().toString(QKeySequence::NativeText)));
    chords->setAccessibleName(chords->toolTip());
    chords->setAutoRaise(true);
    chords->setFocusPolicy(Qt::NoFocus);
    chords->setFixedHeight(28);
    connect(chords, &QToolButton::clicked, m_chordAction, &QAction::trigger);
    bottom->addWidget(chords);
    column->addWidget(footer);

    refreshLaneSelector();

    loadViewPreferences();
    applyNavigationTheme();
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &PianoRollWindow::applyNavigationTheme);
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
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(28, 28);
    button->setProperty("pianoGlyph", int(glyph));
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
    m_toolbar->setObjectName(QStringLiteral("PianoRollToolbar"));
    m_toolbar->setAttribute(Qt::WA_StyledBackground);
    auto* row = new QHBoxLayout(m_toolbar);
    row->setContentsMargins(6, 4, 6, 4);
    row->setSpacing(3);

    // ── Settings: everything that isn't reached every minute ──
    auto* settings = toolbarButton(m_toolbar, icons::Glyph::Gear,
                                   tr("Piano roll settings"));
    settings->setObjectName(QStringLiteral("PianoRollSettings"));
    auto* settingsMenu = new QMenu(m_toolbar);
    settingsMenu->setObjectName(QStringLiteral("PianoRollSettingsMenu"));
    m_importMidiAction = settingsMenu->addAction(tr("Import MIDI File…"), this,
                                                &PianoRollWindow::importMidiFile);
    m_importMidiAction->setObjectName(QStringLiteral("pianoRoll.importMidi"));
    m_importMidiAction->setToolTip(tr("Replace this clip's notes; the project tempo stays unchanged. Undo restores the original notes."));
    m_exportMidiAction = settingsMenu->addAction(tr("Export MIDI File…"), this,
                                                &PianoRollWindow::exportMidiFile);
    m_exportMidiAction->setObjectName(QStringLiteral("pianoRoll.exportMidi"));
    m_exportMidiAction->setToolTip(tr("Export the audible notes in this clip as a MIDI file."));
    settingsMenu->addSeparator();
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
        line->setObjectName(QStringLiteral("PianoRollToolDivider"));
        line->setFixedHeight(20);
        return line;
    };
    row->addWidget(divider());

    // ── The tool palette ──
    m_toolButtons = new QButtonGroup(this);
    m_toolButtons->setExclusive(true);
    const std::tuple<PianoRollView::Tool, icons::Glyph, QString> palette[] = {
        {PianoRollView::Tool::Draw, icons::Glyph::Brush, tr("Draw (1 / P)")},
        {PianoRollView::Tool::Slice, icons::Glyph::Knife,
         tr("Slice (2) — hold S to borrow it for one cut")},
        {PianoRollView::Tool::Erase, icons::Glyph::Eraser, tr("Erase (3)")},
        {PianoRollView::Tool::Select, icons::Glyph::Pointer, tr("Select (4 / E / Ctrl)")},
        {PianoRollView::Tool::Mute, icons::Glyph::Power,
         tr("Disable notes (5) — hold T to borrow it for one note")},
        {PianoRollView::Tool::Slide, icons::Glyph::Automation, tr("Slide (6)")},
    };
    for (const auto& [tool, glyph, tip] : palette) {
        QToolButton* button = toolbarButton(m_toolbar, glyph, tip, true);
        button->setChecked(tool == PianoRollView::Tool::Draw);
        m_toolButtons->addButton(button, int(tool));
        row->addWidget(button);
    }
    connect(m_toolButtons, &QButtonGroup::idClicked, this, [this](int id) {
        m_view->setTool(PianoRollView::Tool(id));
        syncToolActions();
    });
    row->addWidget(divider());

    auto* slideScope = new QComboBox(m_toolbar);
    slideScope->setObjectName(QStringLiteral("PianoRollSlideScope"));
    slideScope->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    slideScope->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    slideScope->addItems({tr("One note"), tr("Chord")});
    slideScope->setAccessibleName(tr("Slide targets"));
    slideScope->setToolTip(tr("Slide the selected sounding note, or a fixed set of chord notes"));
    connect(slideScope, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->m_slideChord = i == 1; });
    row->addWidget(slideScope);

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

    m_auditionButton = toolbarButton(
        m_toolbar, icons::Glyph::Headphones,
        tr("Hear only this MIDI track"), true);
    m_auditionButton->setObjectName(QStringLiteral("PianoRollAudition"));
    m_auditionButton->setAccessibleName(tr("Hear only this MIDI track"));
    connect(m_auditionButton, &QToolButton::toggled, this, [this](bool on) {
        const QString target = on ? auditionTrackForCurrentClip() : QString{};
        m_controller->setExclusiveAuditionTrack(target.toStdString());
        updateActionState();
    });
    row->addWidget(m_auditionButton);

    m_clipSelector = new QComboBox(m_toolbar);
    m_clipSelector->setObjectName(QStringLiteral("PianoRollClipSelector"));
    m_clipSelector->setAccessibleName(tr("MIDI clip"));
    m_clipSelector->setAccessibleDescription(
        tr("Choose any MIDI clip in the project, including clips inside patterns."));
    m_clipSelector->setPlaceholderText(tr("Choose MIDI clip"));
    m_clipSelector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_clipSelector->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_clipSelector, &QComboBox::activated, this, [this](int index) {
        const QString track = m_clipSelector->itemData(index, Qt::UserRole).toString();
        const QString clip = m_clipSelector->itemData(index, Qt::UserRole + 1).toString();
        if (!track.isEmpty() && !clip.isEmpty() &&
            (track != m_trackId || clip != m_clipId))
            emit clipSwitchRequested(track, clip);
    });
    row->addWidget(m_clipSelector);
    row->addStretch(1);
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


}

void PianoRollWindow::syncToolActions() {
    // The palette and the Tools menu are two faces of one setting, so whichever
    // was used, the other has to follow.
    const int current = int(m_view->tool());
    const bool slidesEnabled=m_controller->canEditSlideNotes();
    if(auto* button=m_toolButtons->button(int(PianoRollView::Tool::Slide))){button->setEnabled(slidesEnabled);button->setToolTip(slidesEnabled?tr("Slide (6)"):tr("Slide editing requires collaboration protocol 5. Reconnect to an updated session."));}
    if (auto* button = m_toolButtons->button(current)) {
        const QSignalBlocker block(m_toolButtons);
        button->setChecked(true);
    }
    if (m_toolGroup) {
        const auto actions = m_toolGroup->actions();
        for (QAction* action : actions) {
            if(action->data().toInt()==int(PianoRollView::Tool::Slide))action->setEnabled(slidesEnabled);
            if (action->data().toInt() == current) action->setChecked(true);
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
        for (const auto& lane : daw::midiLanes(*clip)) {
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
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    const auto parameters = track ? m_controller->insertParameters(track->id, track->instrument.id)
                                 : std::vector<daw::plugins::ParameterInfo>{};
    std::vector<std::string> parameterIds;
    for (const auto& parameter : parameters) if (parameter.isAutomatable) {
        choices << tr("Instrument · %1").arg(QString::fromStdString(parameter.name));
        parameterIds.push_back(parameter.id);
    }

    bool ok = false;
    const QString picked =
        QInputDialog::getItem(this, tr("Add Controller Lane"), tr("Controller:"),
                              choices, 0, false, &ok);
    if (!ok) return;

    std::string name, parameterId;
    int cc = 1;
    const int index = choices.indexOf(picked);
    if (index >= 0 && index < int(std::size(presets))) {
        name = presets[index].name;
        cc = presets[index].cc;
    } else if (index > int(std::size(presets))) {
        parameterId = parameterIds[std::size_t(index - int(std::size(presets)) - 1)];
        name = picked.toStdString(); cc = -1;
    } else {
        cc = QInputDialog::getInt(this, tr("Add Controller Lane"),
                                  tr("CC number:"), 1, 0, 127, 1, &ok);
        if (!ok) return;
        name = tr("CC %1").arg(cc).toStdString();
    }

    const std::string laneId = m_controller->addControllerLane(
        m_trackId.toStdString(), m_clipId.toStdString(), name, cc);
    if (laneId.empty()) return;
    if (!parameterId.empty()) m_controller->setLaneTarget(m_trackId.toStdString(), m_clipId.toStdString(), laneId, {}, parameterId);
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

namespace {
void populateSharedEditorMenu(QMenu& target, QMenu& source) {
    ui::clearMenu(target);
    for (QAction* action : source.actions()) {
        if (auto* sourceMenu = action->menu()) {
            auto* submenu = target.addMenu(sourceMenu->title());
            submenu->setIcon(sourceMenu->icon());
            submenu->setEnabled(action->isEnabled());
            populateSharedEditorMenu(*submenu, *sourceMenu);
            QObject::connect(submenu, &QMenu::aboutToShow, sourceMenu, [submenu, sourceMenu] {
                QMetaObject::invokeMethod(sourceMenu, "aboutToShow", Qt::DirectConnection);
                populateSharedEditorMenu(*submenu, *sourceMenu);
            });
        } else {
            target.addAction(action);
        }
    }
}
} // namespace

void PianoRollWindow::populateActionsMenu(QMenu& menu) {
    menu.addAction(m_importMidiAction);
    menu.addAction(m_exportMidiAction);
    menu.addSeparator();
    for (QMenu* source : {m_editMenu, m_viewMenu, m_toolsMenu, m_snapMenu}) {
        if (!source) continue;
        auto* section = menu.addMenu(source->title());
        populateSharedEditorMenu(*section, *source);
        connect(section, &QMenu::aboutToShow, source, [section, source] {
            QMetaObject::invokeMethod(source, "aboutToShow", Qt::DirectConnection);
            populateSharedEditorMenu(*section, *source);
        });
    }
}

void PianoRollWindow::importMidiFile() {
    const QString targetTrack = m_trackId, targetClip = m_clipId;
    const QString path = QFileDialog::getOpenFileName(this, tr("Import MIDI File…"),
        QSettings().value(QStringLiteral("pianoRoll/midiDirectory")).toString(),
        tr("MIDI files (*.mid *.midi);;All files (*)"));
    if (path.isEmpty()) return;
    if (targetTrack != m_trackId || targetClip != m_clipId) return;
    QString error;
    if (!importMidiFromPath(path, error)) {
        QMessageBox::warning(this, tr("Could not import MIDI"), error);
        return;
    }
    QSettings().setValue(QStringLiteral("pianoRoll/midiDirectory"), QFileInfo(path).absolutePath());
}

bool PianoRollWindow::importMidiFromPath(const QString& path, QString& error) {
    daw::midifile::File file;
    std::string detail;
    if (!daw::midifile::parse(path.toStdString(), file, detail)) {
        error = tr("Could not read the MIDI file: %1").arg(QString::fromStdString(detail));
        return false;
    }
    if (file.notes.empty()) { error = tr("This MIDI file contains no notes."); return false; }
    finishPendingNoteEdit();
    cancelToolPreview();
    if (!m_controller->replaceMidiClipFromFile(m_trackId.toStdString(), m_clipId.toStdString(), file)) {
        error = tr("The MIDI clip is no longer available or cannot be edited.");
        return false;
    }
    // Discard stale selection IDs and cached geometry, then fit the new phrase.
    m_view->setClip(m_trackId, m_clipId);
    m_previewOwner = nullptr;
    m_view->scrollToContent();
    refresh();
    emit noteSelectionChanged(false);
    emit edited();
    return true;
}

void PianoRollWindow::exportMidiFile() {
    finishPendingNoteEdit();
    const auto* clip = m_view->clip();
    if (!clip) return;
    QString name = QString::fromStdString(clip->name);
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    name.replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (name.isEmpty()) name = QStringLiteral("MIDI Clip");
    const QString targetTrack = m_trackId, targetClip = m_clipId;
    const QString directory = QSettings().value(QStringLiteral("pianoRoll/midiDirectory")).toString();
    QFileDialog dialog(this, tr("Export MIDI File…"), directory,
                       tr("MIDI files (*.mid *.midi)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("mid"));
    dialog.selectFile(name + QStringLiteral(".mid"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    if (targetTrack != m_trackId || targetClip != m_clipId) return;
    const QString path = dialog.selectedFiles().first();
    QString error;
    if (!exportMidiToPath(path, error)) {
        QMessageBox::warning(this, tr("Could not export MIDI"), error);
        return;
    }
    QSettings().setValue(QStringLiteral("pianoRoll/midiDirectory"), QFileInfo(path).absolutePath());
    clip=m_view->clip();
    if (clip && (!clip->slideNotes.empty() || std::any_of(clip->takes.begin(),clip->takes.end(),[](const auto& take){return !take.slideNotes.empty();}))) QMessageBox::information(this,tr("MIDI exported"),tr("Ordinary notes were exported. Slide curves are not included in MIDI export; use audio export to preserve them."));
}

bool PianoRollWindow::exportMidiToPath(const QString& path, QString& error) {
    const auto* clip = m_view->clip();
    if (!clip) { error = tr("The MIDI clip is no longer available or cannot be edited."); return false; }
    const auto& project = m_controller->project();
    daw::midifile::File file;
    file.firstTempoBpm = project.tempo;
    file.trackNames.push_back(clip->name);
    file.lengthBeats = daw::secondsToBeats(clip->durationSeconds, project.tempo);
    const double offset = daw::secondsToBeats(clip->offsetSeconds, project.tempo);
    for (const auto& note : daw::midiNotes(*clip)) {
        if (note.muted) continue;
        const double start = std::max(0.0, note.startBeats - offset);
        const double end = std::min(file.lengthBeats, note.startBeats + note.lengthBeats - offset);
        if (end <= start) continue;
        daw::midifile::Note output;
        output.pitch = note.pitch; output.startBeats = start; output.lengthBeats = end - start;
        output.velocity = note.velocity; output.channel = note.channel;
        output.releaseVelocity = note.releaseVelocity;
        file.notes.push_back(output);
    }
    std::vector<std::uint8_t> bytes;
    std::string detail;
    if (!daw::midifile::encode(file, bytes, detail, project.timeSigNumerator, project.timeSigDenominator)) {
        error = tr("Could not create the MIDI file: %1").arg(QString::fromStdString(detail));
        return false;
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) ||
        !output.commit()) {
        error = output.errorString();
        return false;
    }
    return true;
}

void PianoRollWindow::buildEditMenu(QMenu* menu) {
    if (m_undoAction) menu->addAction(m_undoAction);
    else m_undoAction = menu->addAction(tr("Undo"), QKeySequence::Undo, this, [this] {
        finishPendingNoteEdit();
        m_controller->undo();
        refresh();
        emit edited();
    });
    if (m_redoAction) menu->addAction(m_redoAction);
    else m_redoAction = menu->addAction(tr("Redo"), QKeySequence::Redo, this, [this] {
        finishPendingNoteEdit();
        m_controller->redo();
        refresh();
        emit edited();
    });
    menu->addSeparator();

    auto* cut = menu->addAction(tr("Cut"), QKeySequence::Cut, this,
                                [this] { m_view->cutSelection(); });
    cut->setObjectName(QStringLiteral("pianoRoll.edit.cut"));
    auto* copy = menu->addAction(tr("Copy"), QKeySequence::Copy, this,
                                 &PianoRollWindow::copyNotes);
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
    m_ghostMenu->setObjectName(QStringLiteral("PianoRollGhostMenu"));
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
        {tr("Slide (6)"), PianoRollView::Tool::Slide, {}},
        {tr("Draw"), PianoRollView::Tool::Draw, QKeySequence(Qt::Key_P)},
        {tr("Slice"), PianoRollView::Tool::Slice, QKeySequence(Qt::Key_S)},
        {tr("Erase"), PianoRollView::Tool::Erase, {}},
        {tr("Select"), PianoRollView::Tool::Select, QKeySequence(Qt::Key_E)},
        {tr("Disable Notes"), PianoRollView::Tool::Mute,
         QKeySequence(Qt::Key_T)},
    };
    for (const auto& [label, tool, key] : tools) {
        QAction* action = menu->addAction(label);
        action->setData(int(tool));
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
            m_view->previewTransform([d](const mt::Notes& n) { return d->generate(n); });
        }, [this](ChordDialog* d) {
            const auto notes = m_view->targetNotes();
            const double start = m_view->m_timeRange.valid() ? m_view->m_timeRange.from : 0.0;
            const double length = m_view->m_timeRange.valid()
                ? m_view->m_timeRange.to - start : std::min(4.0, m_view->clipBeats());
            d->setContext(notes, start, std::max(kMinNoteBeats, length),
                          m_view->m_lastVelocity, m_view->clipBeats());
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
                                     const std::function<void(Dialog*)>& preview,
                                     const std::function<void(Dialog*)>& prepare) {
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
    if (prepare) prepare(dialog);
    emit internalWindowRequested(
        dialog,
        QStringLiteral("internalEditors/pianoRollTools/") +
            QString::fromLatin1(dialog->metaObject()->className()) +
            (dialog->property("vlt.compactEditor").toBool() ? QStringLiteral("/compact") : QString()));
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
    m_view->setTool(PianoRollView::Tool::Draw);
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
}

void PianoRollWindow::closeEvent(QCloseEvent* event) {
    cancelToolPreview();
    saveViewPreferences();
    QWidget::closeEvent(event);
}

void PianoRollWindow::hideEvent(QHideEvent* event) {
    if (m_auditionButton && m_auditionButton->isChecked()) {
        m_controller->setExclusiveAuditionTrack({});
        const QSignalBlocker block(m_auditionButton);
        m_auditionButton->setChecked(false);
    }
    QWidget::hideEvent(event);
}

void PianoRollWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_view->setTool(PianoRollView::Tool::Draw);
    syncToolActions();
    if (!m_refreshPending) return;
    m_refreshPending = false;
    refresh();
}

void PianoRollWindow::selectAllNotesForTest() {
    if (m_view) m_view->selectAll();
}

bool PianoRollWindow::checkLocalRangeForTest() {
    daw::EngineController fixture;
    if (!fixture.initialize(48000, 512, false).isOk()) return false;
    fixture.setTempo(120.0);
    const auto trackId = fixture.addTrack(daw::TrackKind::Midi, "Local range");
    const auto clipId = fixture.addMidiClip(trackId, 12.0, 16.0);
    const auto otherClip = fixture.addMidiClip(trackId, 40.0, 16.0);
    const auto makeNote = [](int pitch, double at, double length) {
        daw::NoteModel note;
        note.id = daw::newUuid(); note.pitch = pitch;
        note.startBeats = at; note.lengthBeats = length;
        note.velocity = 91; note.releaseVelocity = 28; note.channel = 3;
        return note;
    };
    const mt::Notes originalNotes = {makeNote(60, 0.5, 0.75), makeNote(64, 1.5, 0.5),
        makeNote(67, 2.75, 1.0), makeNote(72, 0.25, 3.25),
        makeNote(55, 0.0, 1.0), makeNote(76, 3.0, 0.5), makeNote(48, 9.0, 0.5)};
    fixture.setClipNotes(trackId, clipId, originalNotes, "Range Fixture");
    fixture.setLoopRangeSeconds(4.0, 20.0); // Eight bars, away from project zero.
    fixture.setLoopEnabled(true);
    fixture.seekSeconds(7.0);
    fixture.play();

    const QScopedValueRollback savedClipboard(clipboard());
    PianoRollWindow editor(&fixture);
    editor.resize(900, 600);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.setClip(QString::fromStdString(trackId), QString::fromStdString(clipId));
    editor.show();
    QApplication::processEvents();
    auto* view = editor.m_view;
    view->m_pxPerBeat = 90.0;
    view->m_scrollX = 0.0;
    view->m_gridBeats = 0.25;
    view->m_snapEnabled = true;
    view->m_adaptiveSnap = false;
    view->scrollToContent();
    int editSignals = 0;
    connect(&editor, &PianoRollWindow::edited, &editor, [&] { ++editSignals; });
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        std::fprintf(stderr, "%s Piano roll range: %s\n", condition ? "PASS" : "FAIL", message);
        ok &= condition;
    };
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    const auto rangeIs = [&](double from, double to) {
        return close(view->m_timeRange.from, from) && close(view->m_timeRange.to, to);
    };
    const auto playbackIntact = [&] {
        return fixture.loopStartSeconds() == 4.0 && fixture.loopEndSeconds() == 20.0 &&
            fixture.isLoopEnabled() && fixture.isPlaying();
    };
    const auto strike = [&](QEvent::Type type, double beat, Qt::MouseButton button,
                            Qt::MouseButtons held, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const QPointF at(view->beatsToX(beat), ui::kLoopStripHeight * 0.5);
        QMouseEvent ev(type, at, view->mapToGlobal(at), button, held, mods);
        QApplication::sendEvent(view, &ev);
    };
    const auto drag = [&](double from, double to, bool releaseOnly = false,
                          Qt::KeyboardModifiers mods = Qt::NoModifier) {
        strike(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
        if (!releaseOnly) strike(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, mods);
        strike(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
    };
    const auto clearRange = [&] {
        const double middle = (view->m_timeRange.from + view->m_timeRange.to) * 0.5;
        drag(middle, middle);
        strike(QEvent::MouseButtonDblClick, middle, Qt::LeftButton, Qt::LeftButton);
        strike(QEvent::MouseButtonRelease, middle, Qt::LeftButton, Qt::NoButton);
    };
    const auto revision = fixture.projectRevision();
    const auto undoDepth = fixture.undoDepth();
    drag(1.0, 3.0);
    check(rangeIs(1.0, 3.0) && playbackIntact(), "create keeps the eight-bar arrangement loop playing");
    const QString shot = qEnvironmentVariable("DAW_PIANO_RANGE_SCREENSHOT");
    if (!shot.isEmpty()) check(editor.grab().save(shot), "local range screenshot");
    drag(2.0, 3.0);
    check(rangeIs(2.0, 4.0) && playbackIntact(), "move preserves length and arrangement loop");
    drag(2.0, 1.0);
    drag(4.0, 3.5, true);
    check(rangeIs(1.0, 3.5) && playbackIntact(), "both resize edges apply the release endpoint");
    clearRange();
    check(!view->m_timeRange.valid() && playbackIntact(), "double-click clears only the local range");
    drag(3.0, 1.0, true);
    check(rangeIs(1.0, 3.0) && playbackIntact(), "reverse drag works without an intermediate move");
    clearRange();
    drag(1.13, 2.87, false, Qt::AltModifier);
    check(rangeIs(1.13, 2.87) && playbackIntact(), "Alt bypasses local snapping");
    clearRange();
    drag(2.0, 2.0);
    check(!view->m_timeRange.valid() && playbackIntact(), "zero-length click leaves no hidden range");
    drag(1.0, 3.0);
    strike(QEvent::MouseButtonPress, 2.0, Qt::LeftButton, Qt::LeftButton);
    strike(QEvent::MouseMove, 3.0, Qt::NoButton, Qt::LeftButton);
    strike(QEvent::MouseMove, 4.0, Qt::NoButton, Qt::NoButton);
    check(rangeIs(2.0, 4.0) && view->m_rangeGrab == PianoRollView::RangeGrab::None &&
        playbackIntact(), "lost release finishes the range without touching transport");
    clearRange();
    drag(1.0, 3.0);
    check(fixture.projectRevision() == revision && fixture.undoDepth() == undoDepth &&
        editSignals == 0 && daw::midiNotes(*view->clip()) == originalNotes,
        "selection gestures do not edit notes, dirty the project or add undo entries");

    const QRect strip(0, 0, view->width(), ui::kLoopStripHeight);
    const QImage enabledStrip = view->grab(strip).toImage();
    fixture.setLoopEnabled(false);
    fixture.setLoopRangeSeconds(6.0, 10.0);
    editor.refresh();
    check(rangeIs(1.0, 3.0) && enabledStrip == view->grab(strip).toImage(),
        "timeline loop changes do not move or dim the local range");
    drag(2.0, 3.0);
    check(rangeIs(2.0, 4.0) && !fixture.isLoopEnabled() &&
        fixture.loopStartSeconds() == 6.0 && fixture.loopEndSeconds() == 10.0,
        "editing a range never arms a disabled arrangement loop");
    clearRange();
    drag(1.0, 3.0);
    fixture.setLoopRangeSeconds(4.0, 20.0);
    fixture.setLoopEnabled(true);

    const auto hasNote = [&](const mt::Notes& notes, int pitch, double at, double length) {
        return std::any_of(notes.begin(), notes.end(), [&](const auto& note) {
            return note.pitch == pitch && close(note.startBeats, at) &&
                close(note.lengthBeats, length) && note.velocity == 91 &&
                note.releaseVelocity == 28 && note.channel == 3;
        });
    };
    // A stale individual note selection must not override the explicit range.
    view->m_selected = {QString::fromStdString(originalNotes.back().id)};
    auto* copyAction = editor.findChild<QAction*>(QStringLiteral("pianoRoll.edit.copy"));
    if (copyAction) copyAction->trigger();
    check(copyAction && editor.m_pasteAction->isEnabled() &&
        clipboard().notes.size() == 4 && close(clipboard().rangeLength, 2.0) &&
        hasNote(clipboard().notes, 60, 0.0, 0.25) && hasNote(clipboard().notes, 64, 0.5, 0.5) &&
        hasNote(clipboard().notes, 67, 1.75, 0.25) && hasNote(clipboard().notes, 72, 0.0, 2.0) &&
        fixture.undoDepth() == undoDepth && playbackIntact(),
        "copy clips intersecting notes to the local range and preserves MIDI properties");
    editor.cutNotes();
    const mt::Notes cut = daw::midiNotes(*view->clip());
    check(cut.size() == 7 && hasNote(cut, 60, 0.5, 0.5) && hasNote(cut, 67, 3.0, 0.75) &&
        hasNote(cut, 72, 0.25, 0.75) && hasNote(cut, 72, 3.0, 0.5) &&
        hasNote(cut, 55, 0.0, 1.0) && hasNote(cut, 76, 3.0, 0.5) &&
        hasNote(cut, 48, 9.0, 0.5) && fixture.undoDepth() == undoDepth + 1 && playbackIntact(),
        "cut preserves all note portions outside the range in one undo operation");
    fixture.undo();
    check(daw::midiNotes(*view->clip()) == originalNotes && rangeIs(1.0, 3.0) && playbackIntact(),
        "undo restores all cut notes without changing either range");
    fixture.redo();
    check(daw::midiNotes(*view->clip()) == cut && playbackIntact(), "redo restores the complete cut");
    fixture.undo();

    const auto switchClip = [&](const std::string& id) {
        editor.setClip(QString::fromStdString(trackId), QString::fromStdString(id));
        QApplication::processEvents();
        view->m_pxPerBeat = 90.0; view->m_scrollX = 0.0;
    };
    switchClip(otherClip);
    check(!view->m_timeRange.valid() && playbackIntact(), "another clip starts without an inherited range");
    view->m_pointerInside = false;
    editor.pasteNotes();
    check(rangeIs(0.0, 2.0) && daw::midiNotes(*view->clip()).size() == 4 && playbackIntact(),
        "paste carries the copied range into another clip");
    editor.repeatNotes();
    editor.repeatNotes();
    check(rangeIs(4.0, 6.0) && daw::midiNotes(*view->clip()).size() == 12 &&
        hasNote(daw::midiNotes(*view->clip()), 64, 4.5, 0.5) && playbackIntact(),
        "repeated range copies advance locally while the arrangement keeps looping");
    switchClip(clipId);
    check(rangeIs(1.0, 3.0) && playbackIntact(), "returning to a clip restores its own range");
    switchClip(otherClip);
    check(rangeIs(4.0, 6.0) && playbackIntact(), "each clip retains its separate editing range");
    editor.hide(); editor.show(); editor.refresh();
    check(rangeIs(4.0, 6.0) && playbackIntact(), "reopening and refreshing preserve the range");

    view->selectNone();
    fixture.setClipNotes(trackId, otherClip, {makeNote(64, 1.5, 0.25)}, "Rest Fixture");
    drag(1.0, 3.0);
    editor.copyNotes();
    view->m_pointer = QPointF(view->beatsToX(4.0), ui::kRulerHeight + 30.0);
    view->m_pointerInside = true;
    editor.pasteNotes();
    editor.repeatNotes();
    check(rangeIs(6.0, 8.0) && daw::midiNotes(*view->clip()).size() == 3 &&
        hasNote(daw::midiNotes(*view->clip()), 64, 4.5, 0.25) &&
        hasNote(daw::midiNotes(*view->clip()), 64, 6.5, 0.25) && playbackIntact(),
        "copy, paste and repeat preserve leading and trailing rests");
    clearRange();
    drag(8.0, 9.0);
    const auto beforeEmpty = fixture.undoDepth();
    const auto notesBeforeEmpty = daw::midiNotes(*view->clip());
    editor.copyNotes(); editor.cutNotes(); editor.repeatNotes();
    check(!view->canPaste() && fixture.undoDepth() == beforeEmpty &&
        daw::midiNotes(*view->clip()) == notesBeforeEmpty && playbackIntact(),
        "an empty range never copies or removes notes elsewhere");
    view->selectAll();
    editor.copyNotes();
    check(!view->m_timeRange.valid() && clipboard().rangeLength == 0.0 &&
        clipboard().notes.size() == 3 && playbackIntact(), "Select All returns to ordinary note copying");
    fixture.stop();
    return ok;
}

bool PianoRollWindow::checkMidiFileActionsForTest() {
    daw::EngineController fixture;
    if (!fixture.initialize(48000, 512, false).isOk()) return false;
    const auto trackId = fixture.addTrack(daw::TrackKind::Midi, "MIDI file check");
    const auto clipId = fixture.addMidiClip(trackId, 3.0, 2.0);
    daw::NoteModel original;
    original.id = daw::newUuid(); original.pitch = 48;
    fixture.setClipNotes(trackId, clipId, {original}, "Fixture");
    PianoRollWindow editor(&fixture);
    editor.resize(900, 600);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.setClip(QString::fromStdString(trackId), QString::fromStdString(clipId));
    editor.show();
    QApplication::processEvents();
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        std::fprintf(stderr, "%s Piano roll MIDI: %s\n", condition ? "PASS" : "FAIL", message);
        ok &= condition;
    };
    auto* settings = editor.findChild<QToolButton*>(QStringLiteral("PianoRollSettings"));
    check(settings && settings->menu()->actions().contains(editor.m_importMidiAction) &&
        settings->menu()->actions().contains(editor.m_exportMidiAction) &&
        editor.m_importMidiAction->isEnabled() && editor.m_exportMidiAction->isEnabled(),
        "file actions are directly accessible in settings");
    daw::midifile::File input;
    input.firstTempoBpm = 93;
    input.lengthBeats = 8;
    input.trackNames = {"Velocity steps"};
    for (int i = 0; i < 3; ++i) {
        daw::midifile::Note n;
        n.pitch = 60; n.startBeats = i * 2; n.lengthBeats = 1.5;
        n.velocity = i == 0 ? 16 : i == 1 ? 72 : 127;
        n.channel = i; n.releaseVelocity = 30 + i;
        input.notes.push_back(n);
    }
    std::vector<std::uint8_t> bytes;
    std::string detail;
    if (!daw::midifile::encode(input, bytes, detail)) return false;
    const QString source = directory.filePath(QString::fromUtf8("фраза.mid"));
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size())) return false;
    file.close();
    const auto undoBefore = fixture.undoDepth();
    const double tempo = fixture.project().tempo;
    QString error;
    check(!editor.importMidiFromPath(directory.filePath("missing.mid"), error) &&
        fixture.undoDepth() == undoBefore && daw::midiNotes(*editor.m_view->clip()) == std::vector{original},
        "invalid imports preserve the notes and history");
    if (!editor.importMidiFromPath(source, error)) return false;
    const auto imported = daw::midiNotes(*editor.m_view->clip());
    check(imported.size() == 3 && fixture.undoDepth() == undoBefore + 1 &&
        fixture.project().tempo == tempo && editor.m_view->clip()->startSeconds == 3.0 &&
        std::abs(daw::secondsToBeats(editor.m_view->clip()->durationSeconds, tempo) - 8) < 1e-6,
        "replacement fits the complete phrase without changing position or tempo");
    fixture.undo();
    check(daw::midiNotes(*editor.m_view->clip()) == std::vector{original} &&
        editor.m_view->clip()->durationSeconds == 2.0, "one undo restores notes and clip length");
    fixture.redo();
    check(daw::midiNotes(*editor.m_view->clip()) == imported, "redo preserves imported note identities");
    const QString destination = directory.filePath(QString::fromUtf8("экспорт.mid"));
    check(editor.exportMidiToPath(destination, error), "atomic MIDI export succeeds");
    daw::midifile::File exported;
    check(daw::midifile::parse(destination.toStdString(), exported, detail) && exported.notes.size() == 3 &&
        std::abs(exported.lengthBeats - 8) < 1e-6, "export can be read back with trailing silence");
    if (exported.notes.size() == 3) for (int i = 0; i < 3; ++i) {
        check(exported.notes[i].velocity == input.notes[i].velocity &&
            exported.notes[i].channel == input.notes[i].channel &&
            exported.notes[i].releaseVelocity == input.notes[i].releaseVelocity &&
            std::abs(exported.notes[i].startBeats - input.notes[i].startBeats) < 1e-6 &&
            std::abs(exported.notes[i].lengthBeats - input.notes[i].lengthBeats) < 1e-6,
            "timing, channel and attack/release velocity survive export");
    }
    editor.m_view->setColorMode(PianoRollView::ColorMode::Clip);
    editor.m_view->setShowNoteNames(false);
    editor.m_view->setNoteStyle(PianoRollView::NoteStyle::Flat);
    editor.m_view->scrollToContent();
    editor.m_view->setPixelsPerBeat(90);
    QApplication::processEvents();
    for (bool selected : {false, true}) {
        if (selected) editor.selectAllNotesForTest();
        const QImage image = editor.m_view->grab().toImage();
        double previous = -1;
        for (const auto& note : imported) {
            const QPointF point = editor.m_view->noteRect(note).center() * image.devicePixelRatio();
            const QColor color = image.pixelColor(point.toPoint());
            check(color.valueF() > previous, "rendered notes brighten with velocity, including selected notes");
            previous = color.valueF();
        }
    }
    const QString shots = qEnvironmentVariable("DAW_PIANO_MIDI_SCREENSHOTS");
    if (!shots.isEmpty()) {
        editor.m_view->m_selected.clear();
        editor.m_view->setShowNoteNames(true);
        editor.grab().save(shots + QStringLiteral(".png"));
        settings->menu()->ensurePolished();
        settings->menu()->adjustSize();
        settings->menu()->grab().save(shots + QStringLiteral(".menu.png"));
    }
    auto trimmed = imported;
    trimmed.front().muted = true;
    fixture.setClipNotes(trackId, clipId, trimmed, "Fixture");
    fixture.setClipTrim(trackId, clipId, 3.0, daw::beatsToSeconds(1, tempo), daw::beatsToSeconds(3, tempo));
    check(std::abs(editor.m_view->clipBeats() - 4) < 1e-9,
          "piano roll retains the source phrase through the trimmed endpoint");
    editor.m_view->seekToLocalBeat(2, false);
    check(std::abs(fixture.positionSeconds() - (3 + daw::beatsToSeconds(1, tempo))) < 1e-9,
          "piano-roll ruler seeking accounts for the hidden source head");
    check(editor.exportMidiToPath(destination, error) &&
        daw::midifile::parse(destination.toStdString(), exported, detail) &&
        exported.notes.size() == 1 && exported.notes.front().channel == 1 &&
        std::abs(exported.notes.front().startBeats - 1) < 1e-6 &&
        std::abs(exported.notes.front().lengthBeats - 1.5) < 1e-6,
        "export respects clip trim and skips muted/outside notes");
    editor.setClip({}, {});
    check(!editor.m_importMidiAction->isEnabled() && !editor.m_exportMidiAction->isEnabled(),
          "file actions disable when the clip disappears");
    return ok;
}

bool PianoRollWindow::checkCompactLayoutForTest() {
    if (!m_view || !m_hScroll || !m_toolbar) return false;
    auto* heightControl =
        findChild<QSlider*>(QStringLiteral("NoteHeightScrubber"));
    if (!heightControl || findChild<NoteContextPanel*>()) return false;

    const int gridTop = m_view->mapTo(this, QPoint(0, 0)).y();
    const int scrollBottom =
        m_hScroll->mapTo(this, QPoint(0, m_hScroll->height())).y();
    const int navigationBottom = scrollBottom;
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
    QWidget* focus = QApplication::focusWidget();
    // An offscreen or briefly deactivated window can have no focus widget
    // even while Qt delivers a key directly to its note canvas.
    const bool inside = (focus && (focus == this || isAncestorOf(focus))) ||
        (!focus && watched == m_view && m_view->isVisible());
    if (!inside || isTextEntry(focus) || qobject_cast<QSlider*>(focus) || QApplication::activePopupWidget())
        return QWidget::eventFilter(watched, event);

    // The main window owns 1–8 for arrangement tools. While the roll has
    // focus, none of those keys may change a tool behind this editor.
    if (!(key->modifiers() & ~Qt::KeypadModifier)) {
        std::optional<PianoRollView::Tool> tool;
        switch (editShortcutKey(key)) {
            case Qt::Key_1: tool = PianoRollView::Tool::Draw; break;
            case Qt::Key_2: tool = PianoRollView::Tool::Slice; break;
            case Qt::Key_3: tool = PianoRollView::Tool::Erase; break;
            case Qt::Key_4: tool = PianoRollView::Tool::Select; break;
            case Qt::Key_5: tool = PianoRollView::Tool::Mute; break;
            case Qt::Key_6: tool = PianoRollView::Tool::Slide; break;
            default: break;
        }
        if (key->key() >= Qt::Key_1 && key->key() <= Qt::Key_8) {
            if (tool && !shortcutOverride && !key->isAutoRepeat()) {
                m_view->finishWheelNoteEdit();
                m_view->setTool(*tool);
                syncToolActions();
            }
            key->accept();
            return true;
        }
    }

    // WidgetWithChildrenShortcut does not outrank a matching WindowShortcut
    // in Qt. Resolve this editor's registered actions before the main menu can
    // make Delete, Ctrl+D, Ctrl+M or zoom commands ambiguous.
    const QKeySequence sequence(QKeyCombination(
        key->modifiers() & ~Qt::KeypadModifier, Qt::Key(editShortcutKey(key))));
    QAction* localAction = nullptr;
    for (QAction* action : actions()) {
        if (action->isEnabled() && action->isVisible() &&
            action->shortcuts().contains(sequence)) {
            localAction = action;
            break;
        }
    }
    if (!localAction && !isPianoRollEditShortcut(key))
        return QWidget::eventFilter(watched, event);

    // Claim the chord before QAction's application/window shortcut resolver
    // can route it back to the arrangement. The following KeyPress performs
    // the edit through the same scoped filter.
    if (shortcutOverride) {
        key->accept();
        return true;
    }

    m_view->finishWheelNoteEdit();
    if (localAction) {
        localAction->trigger();
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

void PianoRollWindow::refreshPatternGhosts() {
    if (!m_view || !m_controller) return;
    const auto& project = m_controller->project();
    const auto patternFor = [&project](const daw::TrackModel* source) {
        const auto* pattern = containingPattern(project, source);
        return pattern ? QString::fromStdString(pattern->id) : QString{};
    };
    const QString pattern = patternFor(project.findTrack(m_trackId.toStdString()));
    if (pattern != m_ghostPatternId) {
        m_ghostPatternId = pattern;
        m_autoPatternGhosts = true;
        m_hiddenPatternGhosts.clear();
    }
    m_patternGhostTracks.clear();
    QSet<QString> available;
    for (const auto& track : project.tracks) {
        if (track.kind != daw::TrackKind::Midi && track.kind != daw::TrackKind::Instrument)
            continue;
        const QString id = QString::fromStdString(track.id);
        available.insert(id);
        if (!pattern.isEmpty() && patternFor(&track) == pattern)
            m_patternGhostTracks.insert(id);
    }
    m_manualGhostTracks.intersect(available);
    m_hiddenPatternGhosts.intersect(m_patternGhostTracks);
    QSet<QString> active = m_manualGhostTracks;
    if (m_autoPatternGhosts)
        active.unite(m_patternGhostTracks - m_hiddenPatternGhosts);
    active.remove(m_trackId);
    m_view->setGhostTracks(active);
}

QString PianoRollWindow::auditionTrackForCurrentClip() const {
    if (!m_controller) return {};
    const auto& project = m_controller->project();
    const auto* track = project.findTrack(m_trackId.toStdString());
    if (!track) return {};
    const bool clipExists = std::any_of(track->clips.begin(), track->clips.end(),
        [this](const daw::ClipModel& clip) {
            return clip.id == m_clipId.toStdString() &&
                   clip.kind == daw::ClipKind::Midi;
        });
    if (!clipExists) return {};
    const auto* pattern = containingPattern(project, track);
    return QString::fromStdString(pattern ? pattern->id : track->id);
}

void PianoRollWindow::refreshClipSelector() {
    if (!m_clipSelector || !m_controller) return;
    struct Entry { QString label, trackId, clipId; };
    std::vector<Entry> entries;
    const auto& project = m_controller->project();
    const double beatsPerBar =
        double(std::max(1, project.timeSigNumerator)) * 4.0 /
        std::max(1, project.timeSigDenominator);
    for (const auto& track : project.tracks) {
        const auto* pattern = containingPattern(project, &track);
        const QString trackName = QString::fromStdString(track.name);
        const QString source = pattern
            ? QStringLiteral("%1 / %2").arg(QString::fromStdString(pattern->name), trackName)
            : trackName;
        for (const auto& clip : track.clips) {
            if (clip.kind != daw::ClipKind::Midi) continue;
            const QString name = clip.name.empty()
                ? tr("Untitled MIDI clip") : QString::fromStdString(clip.name);
            const int bar = 1 + int(std::floor(std::max(0.0,
                daw::secondsToBeats(clip.startSeconds, project.tempo)) / beatsPerBar));
            const QString label = name == trackName
                ? tr("%1 · bar %2").arg(source).arg(bar)
                : tr("%1 — %2 · bar %3").arg(source, name).arg(bar);
            entries.push_back({label,
                               QString::fromStdString(track.id),
                               QString::fromStdString(clip.id)});
        }
    }
    bool changed = m_clipSelector->count() != int(entries.size());
    for (int i = 0; !changed && i < int(entries.size()); ++i) {
        changed = m_clipSelector->itemText(i) != entries[i].label ||
                  m_clipSelector->itemData(i, Qt::UserRole).toString() != entries[i].trackId ||
                  m_clipSelector->itemData(i, Qt::UserRole + 1).toString() != entries[i].clipId;
    }
    const QSignalBlocker block(m_clipSelector);
    if (changed) {
        m_clipSelector->clear();
        for (const Entry& entry : entries) {
            m_clipSelector->addItem(entry.label);
            const int index = m_clipSelector->count() - 1;
            m_clipSelector->setItemData(index, entry.trackId, Qt::UserRole);
            m_clipSelector->setItemData(index, entry.clipId, Qt::UserRole + 1);
            m_clipSelector->setItemData(index, entry.label, Qt::ToolTipRole);
        }
    }
    int selected = -1;
    for (int i = 0; i < int(entries.size()); ++i) {
        if (entries[i].trackId == m_trackId && entries[i].clipId == m_clipId) {
            selected = i;
            break;
        }
    }
    m_clipSelector->setCurrentIndex(selected);
    m_clipSelector->ensurePolished();
    const QFontMetrics metrics(m_clipSelector->font());
    QStyleOptionComboBox option;
    option.initFrom(m_clipSelector);
    const QRect textRect = m_clipSelector->style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, m_clipSelector);
    const int chrome = m_clipSelector->width() - textRect.width();
    m_clipSelector->setFixedWidth(std::clamp(
        metrics.horizontalAdvance(m_clipSelector->currentText()) + chrome + 4, 120, 280));
    m_clipSelector->setEnabled(!entries.empty());
    m_clipSelector->setToolTip(selected >= 0
        ? entries[selected].label
        : tr("Choose any MIDI clip in the project"));
}

void PianoRollWindow::refreshGhostMenu() {
    if (!m_ghostMenu) return;
    refreshPatternGhosts();
    m_ghostMenu->clear();
    if (!m_ghostPatternId.isEmpty()) {
        auto* automatic = m_ghostMenu->addAction(tr("Show all pattern sources automatically"));
        automatic->setObjectName(QStringLiteral("pianoRoll.autoPatternGhosts"));
        automatic->setCheckable(true);
        automatic->setChecked(m_autoPatternGhosts && m_hiddenPatternGhosts.isEmpty());
        connect(automatic, &QAction::triggered, this, [this](bool on) {
            m_autoPatternGhosts = on;
            m_hiddenPatternGhosts.clear();
            refreshPatternGhosts();
        });
        m_ghostMenu->addSeparator();
    }
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
            if (m_autoPatternGhosts && m_patternGhostTracks.contains(trackId)) {
                if (on) m_hiddenPatternGhosts.remove(trackId);
                else m_hiddenPatternGhosts.insert(trackId);
                m_manualGhostTracks.remove(trackId);
            } else {
                if (on) m_manualGhostTracks.insert(trackId);
                else m_manualGhostTracks.remove(trackId);
            }
            refreshPatternGhosts();
        });
    }
    if (!any) {
        QAction* empty = m_ghostMenu->addAction(tr("No other MIDI tracks"));
        empty->setEnabled(false);
    } else {
        m_ghostMenu->addSeparator();
        m_ghostMenu->addAction(tr("Show none"), this,
                               [this] {
                                   m_autoPatternGhosts = false;
                                   m_manualGhostTracks.clear();
                                   refreshPatternGhosts();
                               });
    }
}

void PianoRollWindow::setClip(const QString& trackId, const QString& clipId) {
    cancelToolPreview();
    const bool auditionWasOn = m_auditionButton && m_auditionButton->isChecked();
    m_refreshPending = false;
    m_trackId = trackId;
    m_clipId = clipId;
    m_view->setLivePitches({});
    m_view->setClip(trackId, clipId);
    if (auditionWasOn)
        m_controller->setExclusiveAuditionTrack(
            auditionTrackForCurrentClip().toStdString());
    refreshPatternGhosts();
    m_previewOwner = nullptr;
    updateTitle();
    refreshClipSelector();
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
    refreshPatternGhosts();
    updateTitle();
    refreshClipSelector();
    if (m_auditionButton && m_auditionButton->isChecked() &&
        m_controller->exclusiveAuditionTrackId() !=
            auditionTrackForCurrentClip().toStdString())
        m_controller->setExclusiveAuditionTrack({});
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

void PianoRollWindow::finishPendingNoteEdit() {
    if (m_view) m_view->finishWheelNoteEdit();
}

bool PianoRollWindow::checkHistoryShortcutsForTest() {
    if (!m_view || !m_view->clip()) return false;
    const auto original = daw::midiNotes(*m_view->clip());
    const QPointer<QWidget> originalActive = QApplication::activeWindow();
    const QPointer<QWidget> originalFocus = QApplication::focusWidget();
    const QScopedValueRollback selected(m_view->m_selected);
    const QScopedValueRollback primary(m_view->m_primary);
    const QScopedValueRollback tool(m_view->m_tool);
    const QScopedValueRollback lastLength(m_view->m_lastLength);
    const QScopedValueRollback lastVelocity(m_view->m_lastVelocity);
    const QScopedValueRollback lastPan(m_view->m_lastPan);
    const QScopedValueRollback scrollX(m_view->m_scrollX);
    const QScopedValueRollback scrollY(m_view->m_scrollY);
    const auto sendKey = [](QWidget* target, const QKeySequence& sequence,
                            bool cyrillic = false) {
        // Earlier full-suite probes activate standalone editors. Offscreen Qt
        // needs its active QWidget restored as well as the canvas focus.
        QApplication::setActiveWindow(target->window());
        target->setFocus(Qt::OtherFocusReason);
        const auto chord = sequence[0];
        int key = chord.key();
        quint32 scan = 0, native = 0;
        if (cyrillic) {
            if (key == Qt::Key_Z) { key = 0x042f; scan = 0x2c; native = 0x06; }
            if (key == Qt::Key_Y) { key = 0x041d; scan = 0x15; native = 0x10; }
#if defined(Q_OS_LINUX)
            if (QApplication::platformName() == QStringLiteral("xcb")) scan += 8;
#endif
        }
        QKeyEvent press(QEvent::KeyPress, key, chord.keyboardModifiers(), scan, native, 0);
        QApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, key, chord.keyboardModifiers(), scan, native, 0);
        QApplication::sendEvent(target, &release);
        QApplication::processEvents();
    };
    const auto notes = [this] { return daw::midiNotes(*m_view->clip()); };
    const auto roundTrip = [&](const char* label, const std::function<void()>& edit,
                               bool cyrillic = false) {
        m_view->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        const auto before = notes();
        edit();
        const auto after = notes();
        sendKey(m_view, m_undoAction->shortcut(), cyrillic);
        const bool undone = notes() == before;
        sendKey(m_view, m_redoAction->shortcut(), cyrillic);
        const bool redone = notes() == after;
        if (before == after || !undone || !redone) {
            std::fprintf(stderr, "history shortcut %s: changed=%d undo=%d redo=%d focus=%d\n",
                         label, int(before != after), int(undone), int(redone),
                         int(QApplication::focusWidget() == m_view));
            return false;
        }
        return true;
    };
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                              {}, "Prepare History Shortcut Check");
    refresh();
    m_view->setTool(PianoRollView::Tool::Draw);
    m_view->scrollToContent();
    const auto point = [&](double beat, int pitch) {
        return QPointF(m_view->beatsToX(beat),
                       m_view->pitchToY(pitch) + m_view->m_rowHeight * 0.5);
    };
    const auto pointer = [&](QEvent::Type type, const QPointF& at) {
        QMouseEvent event(type, at, m_view->mapToGlobal(at.toPoint()),
                         type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                         type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(m_view, &event);
    };
    bool ok = roundTrip("draw", [&] {
        pointer(QEvent::MouseButtonPress, point(0.5, 60));
        pointer(QEvent::MouseButtonRelease, point(0.5, 60));
    });
    if (ok) ok = roundTrip("move", [&] {
        const auto note = notes().front();
        const auto from = point(note.startBeats + note.lengthBeats * 0.5, note.pitch);
        const auto to = point(note.startBeats + note.lengthBeats * 0.5 + 1.0, note.pitch + 2);
        pointer(QEvent::MouseButtonPress, from);
        pointer(QEvent::MouseMove, to);
        pointer(QEvent::MouseButtonRelease, to);
    });
    if (ok) ok = roundTrip("resize", [&] {
        m_view->selectAll();
        m_view->beginSelectionEdit();
        m_view->setSelectionLength(notes().front().lengthBeats + 0.5);
        m_view->endSelectionEdit(QStringLiteral("Resize Notes"));
    });
    if (ok) ok = roundTrip("velocity wheel before idle", [&] {
        m_view->selectAll();
        m_view->bumpSelectedVelocity(-7);
    });
    if (ok) ok = roundTrip("repeat", [&] {
        m_view->selectAll();
        sendKey(m_view, QKeySequence(Qt::CTRL | Qt::Key_B));
    });
    if (ok) ok = roundTrip("duplicate Ctrl+D", [&] {
        m_view->selectAll();
        sendKey(m_view, QKeySequence(Qt::CTRL | Qt::Key_D));
    });
    if (ok) ok = roundTrip("mute Ctrl+M", [&] {
        m_view->selectAll();
        sendKey(m_view, QKeySequence(Qt::CTRL | Qt::Key_M));
    });
    if (ok) ok = roundTrip("Cyrillic history keys", [&] {
        m_view->selectAll();
        sendKey(m_view, QKeySequence(Qt::SHIFT | Qt::Key_Up));
    }, true);
    const auto undoKeys = m_undoAction->shortcuts();
    const auto redoKeys = m_redoAction->shortcuts();
    m_undoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Z));
    m_redoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Y));
    if (ok) ok = roundTrip("rebound history keys", [&] {
        m_view->selectAll();
        sendKey(m_view, QKeySequence(Qt::SHIFT | Qt::Key_Down));
    });
    m_undoAction->setShortcuts(undoKeys);
    m_redoAction->setShortcuts(redoKeys);
    if (ok) {
        QLineEdit field(this);
        field.setText(QStringLiteral("before"));
        field.show();
        field.setFocus();
        QApplication::processEvents();
        field.insert(QStringLiteral(" after"));
        const auto before = notes();
        sendKey(&field, QKeySequence::Undo);
        ok = field.text() == QStringLiteral("before") && notes() == before;
        sendKey(&field, QKeySequence::Redo);
        ok = ok && field.text() == QStringLiteral("before after") && notes() == before;
        if (!ok) std::fprintf(stderr, "history keys did not stay in the text field\n");
    }
    if (ok) ok = roundTrip("delete selection", [&] {
        m_view->selectAll();
        sendKey(m_view, QKeySequence(Qt::Key_Delete));
    });
    m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(),
                              original, "Restore History Shortcut Check");
    refresh();
    emit edited();
    QApplication::setActiveWindow(originalActive);
    if (originalFocus) originalFocus->setFocus(Qt::OtherFocusReason);
    if (ok) std::fprintf(stderr, "PASS piano-roll history keyboard round trips\n");
    return ok;
}

bool PianoRollWindow::checkInteractionGesturesForTest() {
    if (!m_view || !m_hScroll) return false;
    auto* heightControl =
        findChild<QSlider*>(QStringLiteral("NoteHeightScrubber"));
    if (!heightControl || !m_timeZoom) return false;

    const auto originalTool = m_view->tool();
    m_view->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    bool numericToolShortcuts = true;
    for (const auto& [key, tool] :
         {std::pair{Qt::Key_1, PianoRollView::Tool::Draw},
          std::pair{Qt::Key_2, PianoRollView::Tool::Slice},
          std::pair{Qt::Key_3, PianoRollView::Tool::Erase},
          std::pair{Qt::Key_4, PianoRollView::Tool::Select},
          std::pair{Qt::Key_5, PianoRollView::Tool::Mute},
          std::pair{Qt::Key_6, PianoRollView::Tool::Slide}}) {
        QKeyEvent shortcut(QEvent::ShortcutOverride, key, Qt::NoModifier);
        shortcut.ignore();
        QApplication::sendEvent(m_view, &shortcut);
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(m_view, &press);
        numericToolShortcuts = numericToolShortcuts && shortcut.isAccepted() &&
            m_view->tool() == tool && m_toolButtons->checkedId() == int(tool);
    }
    for (int key = Qt::Key_7; key <= Qt::Key_8; ++key) {
        QKeyEvent shortcut(QEvent::ShortcutOverride, key, Qt::NoModifier);
        shortcut.ignore();
        QApplication::sendEvent(m_view, &shortcut);
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(m_view, &press);
        numericToolShortcuts = numericToolShortcuts && shortcut.isAccepted() &&
            m_view->tool() == PianoRollView::Tool::Slide;
    }
    m_view->setTool(originalTool);
    syncToolActions();
    if (!numericToolShortcuts)
        std::fprintf(stderr, "Piano Roll numeric tool shortcuts failed\n");

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

    // Build every MIDI Tool dialog and verify that numeric input uniformly uses
    // the Sampler's rotary controls, with one exact live read-out per knob. This
    // guards both top-level parameters and the arpeggiator's per-step values.
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
            if (!dialog) return false;
            if (qobject_cast<ChordDialog*>(dialog))
                return dialog->findChild<QComboBox*>(QStringLiteral("ChordSource")) &&
                       dialog->findChild<QSpinBox*>(QStringLiteral("ChordInversion"));
            if (!dialog->findChildren<QAbstractSpinBox*>().isEmpty())
                return false;
            if (!dialog->findChild<QWidget*>(QStringLiteral("MidiToolFormScroller")))
                return false;
            const auto knobs = dialog->findChildren<ui::Knob*>();
            const auto readouts = dialog->findChildren<QLabel*>();
            const int numericReadouts =
                int(std::count_if(readouts.begin(), readouts.end(), [](QLabel* label) {
                    return label->property("midiNumericReadout").toBool();
                }));
            return !knobs.isEmpty() &&
                   std::all_of(knobs.begin(), knobs.end(), [](ui::Knob* knob) {
                       QWidget* row = knob->parentWidget();
                       return knob->property("midiNumericKnob").toBool() &&
                              knob->size() == QSize(44, 44) &&
                              row && row->property("midiNumericRow").toBool() &&
                              row->height() >= knob->height() &&
                              row->rect().contains(knob->geometry());
                   }) &&
                   numericReadouts == knobs.size();
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

    // The shared right-side controls preserve the centre beat, support
    // immediate reversal and reset without changing note data.
    const double originalPx = m_view->pixelsPerBeat();
    const double originalScrollX = m_view->scrollX();
    m_view->setPixelsPerBeat(240.0);
    m_view->setScrollX(std::min(120.0, m_view->maxScrollX()));
    updateScrollBars();
    const double centreX = m_view->keyboardWidth() + m_view->width() * 0.5;
    m_view->m_pointerInside = false; // The pointer is over the navigation control.
    const double beforeBeat = m_view->xToBeats(centreX);
    const QPointF zoomStart(m_timeZoom->rect().center());
    const QPointF zoomGlobal = m_timeZoom->mapToGlobal(zoomStart.toPoint());
    const auto zoomMouse = [&](QEvent::Type type, QPointF delta, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, zoomStart + delta, zoomGlobal + delta, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(m_timeZoom, &event);
    };
    zoomMouse(QEvent::MouseButtonPress, {}, Qt::LeftButton, Qt::LeftButton);
    zoomMouse(QEvent::MouseMove, {24, 0}, Qt::NoButton, Qt::LeftButton);
    const double zoomed = m_view->effectivePixelsPerBeat();
    const bool navigatorZoom = zoomed > 240.0;
    const bool navigatorAnchored = std::abs(m_view->xToBeats(centreX) - beforeBeat) < 1e-6;
    zoomMouse(QEvent::MouseMove, {-12, 0}, Qt::NoButton, Qt::LeftButton);
    const bool reverses = m_view->effectivePixelsPerBeat() < zoomed;
    zoomMouse(QEvent::MouseButtonRelease, {}, Qt::LeftButton, Qt::NoButton);
    zoomMouse(QEvent::MouseButtonDblClick, {}, Qt::LeftButton, Qt::LeftButton);
    const bool resets = std::abs(m_view->effectivePixelsPerBeat() - kMinFitPxPerBeat) < 1.0;
    m_view->setPixelsPerBeat(originalPx);
    m_view->setScrollX(originalScrollX);

    const bool scrollAboveGrid = m_hScroll->geometry().bottom() <=
                                 m_view->geometry().top();
    return editShortcuts && toolShortcuts && numericToolShortcuts &&
           pinnedControls && heightDrag &&
           numericToolControls && coalescedToolPreview &&
           navigatorZoom && navigatorAnchored && reverses && resets &&
           scrollAboveGrid &&
           m_view->checkInteractionGesturesForTest() &&
           PianoRollView::checkVelocityRampForTest() && checkMidiFileActionsForTest();
}

void PianoRollWindow::applyNavigationTheme() {
    const auto& t = th();
    m_hScroll->setStyleSheet(ui::navigationScrollBarStyle());
    m_vScroll->setStyleSheet(ui::navigationScrollBarStyle());
    setStyleSheet(QStringLiteral(R"(
#PianoRollToolbar, #PianoRollFooter { background: %1; border: none; border-radius: 0; }
#PianoRollFooter { border-top: 1px solid %2; }
#PianoRollTopNavigation { background: %1; }
#PianoRollNavigationRail { background: %1; }
#PianoRollToolDivider { color: %2; }
#PianoRollToolbar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 0; }
#PianoRollToolbar QToolButton:hover { background: %3; border-color: %2; }
#PianoRollToolbar QToolButton:pressed { background: %4; }
#PianoRollToolbar QToolButton:checked { background: %5; border-color: %6; }
#PianoRollToolbar QToolButton::menu-indicator { width: 5px; height: 5px; subcontrol-position: bottom right; right: 1px; bottom: 1px; }
#PianoRollToolbar QComboBox, #PianoRollFooter QComboBox { min-height: 20px; border-radius: 4px; }
)").arg(t.headerBackground.name(), t.separator().name(), t.surfaceElevated.name(),
          t.well().name(), mixColors(t.headerBackground, t.accent, 0.22).name(), t.accent.name()));
    m_hScroll->setStyleSheet(QStringLiteral(R"(
QScrollBar:horizontal { background: %1; height: 12px; border: none; margin: 0; }
QScrollBar::handle:horizontal { background: %2; border: none; border-radius: 3px; min-width: 32px; margin: 3px 0; }
QScrollBar::handle:horizontal:hover { background: %3; }
QScrollBar::handle:horizontal:pressed { background: %4; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; height: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
)").arg(t.headerBackground.name(), mixColors(t.headerBackground, t.textPrimary, 0.30).name(),
          mixColors(t.headerBackground, t.textPrimary, 0.46).name(), t.accent.name()));
    for (auto* button : findChildren<QToolButton*>())
        if (button->property("pianoGlyph").isValid())
            button->setIcon(icons::icon(icons::Glyph(button->property("pianoGlyph").toInt()), t.textPrimary, 16));
    refreshClipSelector();
    updateScrollBars();
}

void PianoRollWindow::updateScrollBars() {
    if (!m_view || !m_hScroll || !m_vScroll) return;
    // Blocked, or setting the range would fire valueChanged straight back into
    // the view and fight whatever the user is doing.
    const QSignalBlocker blockH(m_hScroll);
    const QSignalBlocker blockV(m_vScroll);
    m_hScroll->setRange(0, int(std::ceil(m_view->maxScrollX())));
    m_hScroll->setPageStep(std::max(1, int(m_view->width() - m_view->keyboardWidth())));
    m_hScroll->setSingleStep(std::max(1, int(m_view->effectivePixelsPerBeat() / 4)));
    m_hScroll->setValue(int(std::lround(m_view->scrollX())));
    m_vScroll->setRange(0, int(std::ceil(m_view->maxScrollY())));
    m_vScroll->setPageStep(std::max(1, int(m_view->fieldHeight())));
    m_vScroll->setSingleStep(std::max(1, int(m_view->rowHeight())));
    m_vScroll->setValue(int(std::lround(m_view->scrollY())));
    if (m_timeZoom) {
        const QSignalBlocker block(m_timeZoom);
        m_timeZoom->setValue(pianoZoomToSlider(m_view->effectivePixelsPerBeat()));
    }
    if (m_noteHeight) {
        const QSignalBlocker block(m_noteHeight);
        m_noteHeight->setValue(int(std::lround(m_view->rowHeight() * 10)));
    }
    if (m_navigationKeyboardSpace) m_navigationKeyboardSpace->setFixedWidth(int(m_view->keyboardWidth()));
    if (m_navigationLaneSpace) m_navigationLaneSpace->setFixedHeight(std::max(0, int(m_view->height() - m_view->laneTop())));
}

void PianoRollWindow::updateActionState() {
    const bool midiClip = m_view && m_view->clip();
    if (m_importMidiAction) m_importMidiAction->setEnabled(midiClip);
    if (m_exportMidiAction) m_exportMidiAction->setEnabled(midiClip);
    if (m_undoAction && !m_sharedUndoAction) {
        m_undoAction->setEnabled(m_controller->canUndo());
        const std::string label = m_controller->undoLabel();
        m_undoAction->setText(label.empty()
                                  ? tr("Undo")
                                  : tr("Undo %1").arg(ui::translatedUndoLabel(label)));
    }
    if (m_redoAction && !m_sharedRedoAction) {
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
    if (m_auditionButton) {
        const QString target = auditionTrackForCurrentClip();
        const auto* targetTrack = m_controller->project().findTrack(target.toStdString());
        const bool pattern = targetTrack && targetTrack->kind == daw::TrackKind::Pattern;
        const QString label = pattern ? tr("Hear only this pattern")
                                      : tr("Hear only this MIDI track");
        const QSignalBlocker block(m_auditionButton);
        m_auditionButton->setEnabled(!target.isEmpty());
        m_auditionButton->setChecked(!target.isEmpty() &&
            m_controller->exclusiveAuditionTrackId() == target.toStdString());
        m_auditionButton->setToolTip(label);
        m_auditionButton->setAccessibleName(label);
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
        const auto links = m_controller->linkedClips({track->id, c.id}).size();
        if (links > 1) label += tr(" · Linked: %1").arg(links);
        setWindowTitle(tr("Piano Roll — %1").arg(label));
        return;
    }
    setWindowTitle(tr("Piano Roll"));
}
