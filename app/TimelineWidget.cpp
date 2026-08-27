#include "TimelineWidget.hpp"
#include "AutomationTools.hpp"
#include "FileTypes.hpp"
#include "ProjectTemplates.hpp"
#include "WaveformPaint.hpp"
#include "CompLayout.hpp"
#include "Controls.hpp"
#include "KeyboardLayout.hpp"
#include "SelectionModel.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include "EngineController.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QHash>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QRegion>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_set>
#include <vector>

namespace {
/// A grid line is only worth drawing once it is this far apart on screen.
constexpr double kMinGridSpacingPx = 6.0;
/// Width of the trim-handle zone at each clip edge.
constexpr int kEdgePx = 6;
/// The top strip of a clip where the fade corner handles live.
constexpr int kFadeZonePx = 16;
/// How close the pointer must be to a fade handle to grab it.
constexpr double kFadeGrabPx = 8.0;
/// Radius of the gain-handle circle at a clip's bottom centre.
constexpr double kGainHandleR = 3.5;
/// How close the pointer must be to the gain handle to grab it.
constexpr double kGainGrabPx = 9.0;
/// Vertical drag distance that doubles (or halves) a clip's gain.
constexpr double kGainPixelsPerDoubling = 100.0;
constexpr float kMaxTimelineClipGain = 15.8489319f; // +24 dB
/// A clip nearly fills its lane; this hairline of breathing room keeps adjacent
/// clips from visually fusing with the track dividers.
constexpr int kClipVerticalInset = 2;

/// Arrangement tools use the same unmistakable cursor vocabulary as the piano
/// roll. A dark halo under the light vector icon keeps it legible over both a
/// bright clip and the near-black empty grid.
const QCursor& arrangementToolCursor(icons::Glyph glyph) {
    static QHash<int, QCursor> cache;
    auto it = cache.find(int(glyph));
    if (it != cache.end()) return it.value();

    const qreal dpr = qApp->devicePixelRatio();
    constexpr int size = 24;
    QPixmap pixmap(int(size * dpr), int(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    icons::paint(painter, glyph, QRectF(0, 0, size, size),
                 QColor(0, 0, 0, 165));
    icons::paint(painter, glyph, QRectF(0.7, 0.7, size - 1.4, size - 1.4),
                 QColor(0xFA, 0xFA, 0xFA));
    painter.end();

    const QPoint hot = glyph == icons::Glyph::Brush   ? QPoint(6, 18)
                     : glyph == icons::Glyph::Knife   ? QPoint(20, 6)
                     : glyph == icons::Glyph::Pointer ? QPoint(6, 4)
                                                      : QPoint(12, 12);
    return *cache.insert(int(glyph),
                         QCursor(pixmap, hot.x(), hot.y()));
}

double fadeGainAt(double normalised, double curve) {
    const double t = std::clamp(normalised, 0.0, 1.0);
    const double exponent = std::pow(4.0, -std::clamp(curve, -1.0, 1.0));
    return std::pow(t, exponent);
}

struct ArrangementClipboardClip {
    QString trackId;
    daw::ClipModel clip;
    double relativeStart = 0.0;
    daw::EngineController::PatternClipMembers patternMembers;
};

std::vector<ArrangementClipboardClip>& arrangementClipboard() {
    static std::vector<ArrangementClipboardClip> clips;
    return clips;
}

int editShortcutKey(const QKeyEvent* event) {
    const int physical = ui::physicalUsKey(event);
    return physical ? physical : event->key();
}

bool isPrimaryEditChord(const QKeyEvent* event) {
    const Qt::KeyboardModifiers modifiers =
        event->modifiers() & ~Qt::KeypadModifier;
    // Qt maps Command to ControlModifier on macOS. Some remote keyboards report
    // it as Meta there, so accept both; on Windows/Linux Meta is the system key
    // and must remain available to the operating system.
#if defined(Q_OS_MACOS)
    return modifiers == Qt::ControlModifier || modifiers == Qt::MetaModifier;
#else
    return modifiers == Qt::ControlModifier;
#endif
}

bool isArrangementEditShortcut(const QKeyEvent* event) {
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

/// Triangle path with rounded corners; each corner is a quadratic arc of
/// `radius` inset along its two edges.
QPainterPath roundedTriangle(QPointF a, QPointF b, QPointF c, qreal radius) {
    auto step = [](const QPointF& from, const QPointF& to, qreal d) {
        const QPointF v = to - from;
        const qreal len = std::sqrt(QPointF::dotProduct(v, v));
        return len > 0.0 ? from + v * (d / len) : from;
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
} // namespace

TimelineWidget::TimelineWidget(daw::EngineController* controller,
                               QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setMinimumWidth(ui::kMinTimelineWidth);
    setMouseTracking(true);
    setAcceptDrops(true);
    // Clipboard commands belong to the arrangement after a clip click. The
    // default NoFocus policy left them attached to whichever control had focus
    // previously, so the same visible selection sometimes ignored Cmd/Ctrl.
    setFocusPolicy(Qt::StrongFocus);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

TimelineWidget::~TimelineWidget() {
    // MainWindow deliberately destroys controller-facing children before its
    // EngineController member. Keep a final backstop for deletion paths that do
    // not deliver Hide/UngrabMouse while a clip drag owns the transaction.
    if (m_projectGestureActive || m_clipPositionEditOpen || m_clipTrimEditOpen)
        finishProjectGesture();
}

void TimelineWidget::beginProjectGesture(const QString& label,
                                         ProjectGestureKind kind) {
    if (!m_controller) return;
    // A new press after a lost release must land the previous endpoint before
    // replacing its history marker. No project snapshot is captured here: a
    // note-heavy clip must cost exactly the same to grab as an empty one.
    if (m_projectGestureActive || m_clipPositionEditOpen || m_clipTrimEditOpen)
        cancelProjectGesture();
    m_projectGestureActive = true;
    m_projectGestureKind = kind;
    m_projectGestureUndoGroupId = m_controller->beginUndoGroup().id;
    m_projectGestureLabel = label;
    m_projectGestureChanged = false;
}

void TimelineWidget::markProjectGestureChanged() {
    if (m_projectGestureActive) m_projectGestureChanged = true;
}

void TimelineWidget::finishProjectGesture() {
    const bool active = m_projectGestureActive;
    const bool changed = active && m_projectGestureChanged;
    const std::string label = m_projectGestureLabel.toStdString();

    if (m_controller && active) {
        // Each continuous control contributes only its scalar/placement delta.
        // The controller APIs also publish any realtime state deferred during
        // the drag, so this remains the shared release/cancellation exit.
        switch (m_projectGestureKind) {
        case ProjectGestureKind::ClipPosition:
            if (m_clipPositionEditOpen) {
                m_controller->endClipPositionEdit(changed ? label : std::string{});
                m_clipPositionEditOpen = false;
            }
            break;
        case ProjectGestureKind::ClipTrim:
            if (m_clipTrimEditOpen) {
                m_controller->endClipTrimEdit(changed ? label : std::string{});
                m_clipTrimEditOpen = false;
            }
            break;
        case ProjectGestureKind::ClipGain:
            if (changed) {
                for (const auto& gain : m_gainOrig) {
                    m_controller->commitClipFxLevelEdit(
                        gain.trackId.toStdString(), gain.clipId.toStdString(),
                        gain.origGain, gain.origPan, label);
                }
            }
            break;
        case ProjectGestureKind::ClipFade:
            if (changed) {
                m_controller->commitClipFadeEdit(
                    m_fadeTrackId.toStdString(), m_fadeClipId.toStdString(),
                    m_fadeOriginalIn, m_fadeOriginalOut, label);
            }
            break;
        case ProjectGestureKind::ClipFadeCurve:
            if (changed) {
                m_controller->commitClipFadeCurveEdit(
                    m_fadeTrackId.toStdString(), m_fadeClipId.toStdString(),
                    m_fadeSide == Fade::In, m_fadeCurveOriginal, label);
            }
            break;
        case ProjectGestureKind::GroupExisting:
            break;
        }

        // Region moves also create split commands before their position delta;
        // multi-clip gain and eraser strokes create one small command per clip.
        // Present all of those as the single gesture the user performed. The
        // group is closed even for a no-op so eviction protection cannot leak.
        m_controller->collapseUndo(
            daw::EngineController::UndoGroup{m_projectGestureUndoGroupId},
            label);
    }

    // Defensive publication backstop for an interrupted gesture whose UI kind
    // was lost or superseded. These no-label closes intentionally add no undo.
    if (m_clipPositionEditOpen) {
        if (m_controller) m_controller->endClipPositionEdit();
        m_clipPositionEditOpen = false;
    }
    if (m_clipTrimEditOpen) {
        if (m_controller) m_controller->endClipTrimEdit(std::string{});
        m_clipTrimEditOpen = false;
    }
    m_projectGestureActive = false;
    m_projectGestureKind = ProjectGestureKind::GroupExisting;
    m_projectGestureUndoGroupId = 0;
    m_projectGestureLabel.clear();
    m_projectGestureChanged = false;
}

void TimelineWidget::beginClipPositionGesture(const QString& label) {
    beginProjectGesture(label, ProjectGestureKind::ClipPosition);
    if (!m_controller) return;
    m_controller->beginClipPositionEdit();
    m_clipPositionEditOpen = true;
}

void TimelineWidget::cancelProjectGesture() {
    if (!m_projectGestureActive && !m_clipPositionEditOpen &&
        !m_clipTrimEditOpen) {
        return;
    }
    const bool changed = m_projectGestureChanged;
    finishProjectGesture();
    m_dragging = false;
    m_regionMoving = false;
    m_regionActive = m_regionEnd > m_regionStart && m_regionLaneA >= 0;
    m_trimming = false;
    m_trimEdge = Edge::None;
    m_fading = false;
    m_fadeCurving = false;
    m_fadeSide = Fade::None;
    m_gainDragging = false;
    m_erasing = false;
    m_dragOrigStarts.clear();
    m_regionPieces.clear();
    m_gainOrig.clear();
    if (changed) emit projectEdited();
}

void TimelineWidget::publishSelection() {
    if (!m_selectionModel) return;
    QVector<ui::ClipSel> out;
    out.reserve(m_selection.size());
    for (const auto& ref : m_selection) out.push_back({ref.trackId, ref.clipId});
    // The primary clip leads, so a single-clip context shows the one the user
    // acted on last rather than whichever happened to be added first.
    for (int i = 1; i < out.size(); ++i) {
        if (out[i].clipId == m_selectedClipId) {
            out.swapItemsAt(0, i);
            break;
        }
    }
    m_selectionModel->setClips(out);
}

void TimelineWidget::selectClips(const QVector<ui::ClipSel>& clips) {
    m_selection.clear();
    m_selectedClipId.clear();
    const auto& project = m_controller->project();
    for (const ui::ClipSel& want : clips) {
        for (const daw::TrackModel& track : project.tracks) {
            if (QString::fromStdString(track.id) != want.trackId) continue;
            for (const daw::ClipModel& clip : track.clips) {
                if (QString::fromStdString(clip.id) != want.clipId) continue;
                m_selection.append(ClipRef{want.trackId, want.clipId});
                m_selectedClipId = want.clipId;
            }
            break;
        }
    }
    publishSelection();
    update();
}

void TimelineWidget::clearClipSelection() {
    if (m_selection.isEmpty() && m_selectedClipId.isEmpty()) return;
    m_selection.clear();
    m_selectedClipId.clear();
    update();
}

bool TimelineWidget::deleteSelectedClips() {
    if (m_selection.isEmpty()) return false;
    const auto undoGroup = m_controller->beginUndoGroup();
    for (const auto& ref : m_selection) {
        m_controller->removeClip(ref.trackId.toStdString(),
                                 ref.clipId.toStdString());
    }
    m_controller->collapseUndo(undoGroup, "Delete Clips");
    m_selection.clear();
    m_selectedClipId.clear();
    publishSelection();
    emit projectEdited();
    update();
    return true;
}

bool TimelineWidget::copySelection() {
    if (m_selection.isEmpty()) return false;
    const auto& project = m_controller->project();
    std::unordered_set<std::string> selectedPatternClips;
    for (const auto& ref : m_selection) {
        const auto* track = project.findTrack(ref.trackId.toStdString());
        if (!track) continue;
        for (const auto& clip : track->clips) {
            if (clip.id == ref.clipId.toStdString() &&
                clip.kind == daw::ClipKind::Pattern) {
                selectedPatternClips.insert(clip.id);
            }
        }
    }

    double origin = std::numeric_limits<double>::max();
    for (const auto& ref : m_selection) {
        if (const auto* track = project.findTrack(ref.trackId.toStdString())) {
            for (const auto& clip : track->clips) {
                if (clip.id == ref.clipId.toStdString() &&
                    !selectedPatternClips.contains(clip.patternClipId)) {
                    origin = std::min(origin, clip.startSeconds);
                }
            }
        }
    }
    if (!std::isfinite(origin)) return false;

    auto& clipboard = arrangementClipboard();
    clipboard.clear();
    for (const auto& ref : m_selection) {
        if (const auto* track = project.findTrack(ref.trackId.toStdString())) {
            for (const auto& clip : track->clips) {
                if (clip.id != ref.clipId.toStdString()) continue;
                // Selecting a Pattern parent already captures its members. Do
                // not copy a visible expanded child a second time when a marquee
                // happened to include both lanes.
                if (selectedPatternClips.contains(clip.patternClipId)) break;

                ArrangementClipboardClip item;
                item.trackId = ref.trackId;
                item.clip = clip;
                item.relativeStart = clip.startSeconds - origin;
                if (clip.kind == daw::ClipKind::Pattern) {
                    for (const auto& memberTrack : project.tracks) {
                        for (const auto& member : memberTrack.clips) {
                            if (member.patternClipId == clip.id) {
                                item.patternMembers.emplace_back(memberTrack.id,
                                                                 member);
                            }
                        }
                    }
                }
                clipboard.push_back(std::move(item));
                break;
            }
        }
    }
    return !clipboard.empty();
}

bool TimelineWidget::cutSelection() {
    // A region cut first isolates the intersecting material. Splitting does not
    // change what is heard; the subsequent removals and splits are collapsed
    // into one undo entry.
    if (m_regionActive) {
        const auto undoGroup = m_controller->beginUndoGroup();
        auto& clipboard = arrangementClipboard();
        clipboard.clear();
        const auto candidates =
            regionClipCandidates(m_regionStart, m_regionEnd);
        for (const auto& candidate : candidates) {
            const std::string inner = regionInnerPiece(
                candidate, m_regionStart, m_regionEnd);
            if (inner.empty()) continue;
            const auto* track = m_controller->project().findTrack(candidate.trackId);
            if (!track) continue;
            auto found = std::find_if(track->clips.begin(), track->clips.end(),
                                      [&](const daw::ClipModel& c) {
                                          return c.id == inner;
                                      });
            if (found == track->clips.end()) continue;
            ArrangementClipboardClip item;
            item.trackId = QString::fromStdString(candidate.trackId);
            item.clip = *found;
            item.relativeStart = found->startSeconds - m_regionStart;
            if (found->kind == daw::ClipKind::Pattern) {
                for (const auto& memberTrack :
                     m_controller->project().tracks) {
                    for (const auto& member : memberTrack.clips) {
                        if (member.patternClipId == found->id) {
                            item.patternMembers.emplace_back(memberTrack.id,
                                                             member);
                        }
                    }
                }
            }
            clipboard.push_back(std::move(item));
            m_controller->removeClip(candidate.trackId, inner);
        }
        if (clipboard.empty()) {
            m_controller->releaseUndoGroup(undoGroup);
            return false;
        }
        m_controller->collapseUndo(undoGroup, "Cut Region");
        clearRegion();
        m_selection.clear();
        m_selectedClipId.clear();
        publishSelection();
        emit projectEdited();
        update();
        return true;
    }

    if (!copySelection()) return false;
    const auto undoGroup = m_controller->beginUndoGroup();
    const bool deleted = deleteSelectedClips();
    if (deleted) m_controller->collapseUndo(undoGroup, "Cut Clips");
    else m_controller->releaseUndoGroup(undoGroup);
    return deleted;
}

bool TimelineWidget::pasteClipboard() {
    const auto clipboard = arrangementClipboard();
    if (clipboard.empty()) return false;
    const double at = std::max(0.0, m_controller->positionSeconds());
    const auto undoGroup = m_controller->beginUndoGroup();
    QVector<ClipRef> pasted;
    for (const auto& item : clipboard) {
        const std::string id = item.clip.kind == daw::ClipKind::Pattern
            ? m_controller->insertPatternClipCopy(
                  item.trackId.toStdString(), item.clip, item.patternMembers,
                  at + item.relativeStart)
            : m_controller->insertClipCopy(
                  item.trackId.toStdString(), item.clip,
                  at + item.relativeStart);
        if (!id.empty()) pasted.push_back({item.trackId, QString::fromStdString(id)});
    }
    if (pasted.isEmpty()) {
        m_controller->releaseUndoGroup(undoGroup);
        return false;
    }
    m_controller->collapseUndo(undoGroup, "Paste Clips");
    clearRegion();
    m_selection = pasted;
    m_selectedClipId = pasted.back().clipId;
    publishSelection();
    emit clipSelected(pasted.front().trackId, pasted.front().clipId);
    emit projectEdited();
    update();
    return true;
}

bool TimelineWidget::duplicateSelection() {
    if (m_regionActive || m_selection.isEmpty()) return false;
    return repeatSelection();
}

bool TimelineWidget::repeatSelection() {
    if (m_regionActive) {
        const double length = m_regionEnd - m_regionStart;
        if (length <= 0.0) return false;
        const auto candidates =
            regionClipCandidates(m_regionStart, m_regionEnd);

        const auto undoGroup = m_controller->beginUndoGroup();
        bool any = false;
        for (const auto& candidate : candidates) {
            const std::string inner = regionInnerPiece(
                candidate, m_regionStart, m_regionEnd);
            if (inner.empty()) continue;
            const auto* track = m_controller->project().findTrack(candidate.trackId);
            if (!track) continue;
            auto found = std::find_if(track->clips.begin(), track->clips.end(),
                                      [&](const daw::ClipModel& c) {
                                          return c.id == inner;
                                      });
            if (found == track->clips.end()) continue;
            any |= !m_controller->duplicateClipAt(
                       candidate.trackId, inner, found->startSeconds + length)
                       .empty();
        }
        if (!any) {
            m_controller->releaseUndoGroup(undoGroup);
            return false;
        }
        m_controller->collapseUndo(undoGroup, "Repeat Region");
        m_regionStart += length;
        m_regionEnd += length;
        m_selection.clear();
        m_selectedClipId.clear();
        publishSelection();
        emit projectEdited();
        update();
        return true;
    }

    if (m_selection.isEmpty()) return false;
    struct Source {
        QString trackId;
        QString clipId;
        double start = 0.0;
        double end = 0.0;
    };
    std::vector<Source> sources;
    double first = std::numeric_limits<double>::max();
    double last = 0.0;
    const auto& project = m_controller->project();
    std::unordered_set<std::string> selectedPatternClips;
    for (const auto& ref : m_selection) {
        const auto* track = project.findTrack(ref.trackId.toStdString());
        if (!track) continue;
        for (const auto& clip : track->clips) {
            if (clip.id == ref.clipId.toStdString() &&
                clip.kind == daw::ClipKind::Pattern) {
                selectedPatternClips.insert(clip.id);
            }
        }
    }
    for (const auto& ref : m_selection) {
        const auto* track = project.findTrack(ref.trackId.toStdString());
        if (!track) continue;
        for (const auto& clip : track->clips) {
            if (clip.id != ref.clipId.toStdString()) continue;
            if (selectedPatternClips.contains(clip.patternClipId)) break;
            const double end = clip.startSeconds + clip.durationSeconds;
            sources.push_back({ref.trackId, ref.clipId, clip.startSeconds, end});
            first = std::min(first, clip.startSeconds);
            last = std::max(last, end);
            break;
        }
    }
    if (sources.empty()) return false;
    const double offset = std::max(last - first,
                                   std::max(0.001, snapSeconds()));
    const auto undoGroup = m_controller->beginUndoGroup();
    QVector<ClipRef> copies;
    for (const auto& source : sources) {
        const std::string id = m_controller->duplicateClipAt(
            source.trackId.toStdString(), source.clipId.toStdString(),
            source.start + offset);
        if (!id.empty()) copies.push_back({source.trackId, QString::fromStdString(id)});
    }
    if (copies.isEmpty()) {
        m_controller->releaseUndoGroup(undoGroup);
        return false;
    }
    m_controller->collapseUndo(undoGroup, "Repeat Clips");
    m_selection = copies;
    m_selectedClipId = copies.back().clipId;
    publishSelection();
    emit clipSelected(copies.front().trackId, copies.front().clipId);
    emit projectEdited();
    update();
    return true;
}

bool TimelineWidget::toggleSelectedClipsMuted() {
    if (m_selection.isEmpty()) return false;
    const auto& project = m_controller->project();
    bool allMuted = true;
    for (const auto& ref : m_selection) {
        const auto* track = project.findTrack(ref.trackId.toStdString());
        if (!track) continue;
        for (const auto& clip : track->clips) {
            if (clip.id == ref.clipId.toStdString()) allMuted &= clip.muted;
        }
    }
    const auto undoGroup = m_controller->beginUndoGroup();
    for (const auto& ref : m_selection) {
        m_controller->setClipMuted(ref.trackId.toStdString(),
                                   ref.clipId.toStdString(), !allMuted);
    }
    m_controller->collapseUndo(undoGroup,
                               allMuted ? "Unmute Clips" : "Mute Clips");
    emit projectEdited();
    update();
    return true;
}

double TimelineWidget::selectedClipStartSeconds() const {
    if (m_selectedClipId.isEmpty()) return -1.0;
    const auto& project = m_controller->project();
    for (const auto& track : project.tracks) {
        for (const auto& clip : track.clips) {
            if (QString::fromStdString(clip.id) == m_selectedClipId) {
                return clip.startSeconds;   // already the trimmed start
            }
        }
    }
    return -1.0;
}

double TimelineWidget::selectedClipEndSeconds() const {
    if (m_selectedClipId.isEmpty()) return -1.0;
    const auto& project = m_controller->project();
    for (const auto& track : project.tracks) {
        for (const auto& clip : track.clips) {
            if (QString::fromStdString(clip.id) == m_selectedClipId) {
                return clip.startSeconds + clip.durationSeconds;
            }
        }
    }
    return -1.0;
}

bool TimelineWidget::deleteRegionSelection() {
    if (!m_regionActive) return false;
    const auto candidates = regionClipCandidates(m_regionStart, m_regionEnd);
    bool any = false;
    for (const auto& candidate : candidates) {
        const std::string inner =
            regionInnerPiece(candidate, m_regionStart, m_regionEnd);
        if (inner.empty()) continue;
        m_controller->removeClip(candidate.trackId, inner);
        any = true;
    }
    if (any) {
        m_selection.clear();
        m_selectedClipId.clear();
        m_regionActive = false;
        publishSelection();
        emit projectEdited();
        update();
    }
    return any;
}

std::vector<TimelineWidget::RegionClipCandidate>
TimelineWidget::regionClipCandidates(double r0, double r1) const {
    std::vector<RegionClipCandidate> candidates;
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    for (int lane = std::max(0, m_regionLaneA);
         lane <= m_regionLaneB && lane < int(rows.size()); ++lane) {
        const auto& track = project.tracks[rows[size_t(lane)].index];
        for (const auto& clip : track.clips) {
            const double end = clip.startSeconds + clip.durationSeconds;
            if (end <= r0 || clip.startSeconds >= r1) continue;
            candidates.push_back(
                {track.id, clip.id, clip.startSeconds, end});
        }
    }
    return candidates;
}

std::string TimelineWidget::regionInnerPiece(const RegionClipCandidate& clip,
                                             double r0, double r1) const {
    const double cs = clip.startSeconds;
    const double ce = clip.endSeconds;
    if (ce <= r0 || cs >= r1) return {};
    std::string id = clip.clipId;
    if (cs < r0) {
        id = m_controller->splitClip(clip.trackId, id, r0);
        if (id.empty()) return {};
    }
    if (ce > r1) (void)m_controller->splitClip(clip.trackId, id, r1);
    return id;
}

bool TimelineWidget::regionContains(const QPoint& pos) const {
    if (!m_regionActive) return false;
    const int x = pos.x();
    if (x < secondsToX(m_regionStart) || x > secondsToX(m_regionEnd)) return false;
    const int lane = laneAt(pos.y());
    return lane >= m_regionLaneA && lane <= m_regionLaneB && lane >= 0;
}

void TimelineWidget::clearRegion() {
    if (m_regionMoving) finishProjectGesture();
    m_regionActive = false;
    m_regionPicking = false;
    m_regionMovePending = false;
    m_regionMoving = false;
    m_regionPieces.clear();
    m_regionLaneA = -1;
    m_regionLaneB = -1;
}

void TimelineWidget::updateRegionFromDrag(const QPoint& pos, bool snapOn) {
    const auto& rows = daw::visibleTracks(m_controller->project());
    if (rows.empty()) { clearRegion(); return; }
    const double a = xToSeconds(m_regionOrigin.x());
    const double b = xToSeconds(pos.x());
    m_regionStart = snap(std::min(a, b), snapOn);
    m_regionEnd = snap(std::max(a, b), snapOn);
    const int last = int(rows.size()) - 1;
    const int la = std::clamp(laneAt(m_regionOrigin.y()), 0, last);
    const int lb = std::clamp(laneAt(pos.y()), 0, last);
    m_regionLaneA = std::min(la, lb);
    m_regionLaneB = std::max(la, lb);
    m_regionCurrent = pos;
}

void TimelineWidget::collectRegionPieces() {
    m_regionPieces.clear();
    const double r0 = m_regionStart;
    const double r1 = m_regionEnd;
    const auto candidates = regionClipCandidates(r0, r1);
    for (const auto& candidate : candidates) {
        const std::string id = regionInnerPiece(candidate, r0, r1);
        if (id.empty()) continue;
        double start = std::max(candidate.startSeconds, r0);
        if (const auto* track =
                m_controller->project().findTrack(candidate.trackId)) {
            for (const auto& clip : track->clips) {
                if (clip.id == id) {
                    start = clip.startSeconds;
                    break;
                }
            }
        }
        m_regionPieces.push_back({QString::fromStdString(candidate.trackId),
                                  QString::fromStdString(id), start});
    }
}

void TimelineWidget::drawRegion(QPainter& p) {
    if (!m_regionActive && !m_regionPicking) return;
    if (m_regionLaneA < 0 || m_regionLaneB < m_regionLaneA) return;
    const int x0 = secondsToX(m_regionStart);
    const int x1 = secondsToX(m_regionEnd);
    if (x1 - x0 < 1) return;
    const int top = laneTop(m_regionLaneA);
    const int bottom = laneTop(m_regionLaneB + 1);

    // The light selection box. The fill and dashed border are the same whether
    // or not clips sit inside the region, so its look never changes.
    const Theme& t = th();
    p.fillRect(QRect(x0, top, x1 - x0, bottom - top), t.ink(24));
    p.setPen(QPen(t.ink(150), 1.0, Qt::DashLine));
    p.drawRect(QRect(x0, top, x1 - x0, bottom - top));
}

void TimelineWidget::setTool(Tool tool) {
    m_tool = tool;
    // A committed time selection belongs to the arrangement, not to the tool
    // that created it. Switching back to the pointer, knife, eraser, mute or
    // draw tool must leave the region available to grab and move. Only abandon
    // an unfinished rubber-band if the user changes tools mid-pick.
    if (tool != Tool::SelectRegion && m_regionPicking) clearRegion();
    unsetCursor();
    update();
}

void TimelineWidget::setSecondaryTool(Tool tool) {
    m_secondaryTool = tool;
    if (m_altToolHeld) {
        unsetCursor();
        update();
    }
}

bool TimelineWidget::trackAltTool(Qt::KeyboardModifiers modifiers) {
    // Qt::ControlModifier is ⌘ on macOS and Ctrl elsewhere — the same key Logic
    // borrows a tool with on each platform. The physical Control key is not
    // usable for this on macOS: the system turns Control-click into a
    // right-click before it ever reaches us.
    const bool held = modifiers.testFlag(Qt::ControlModifier);
    if (held == m_altToolHeld) return false;
    m_altToolHeld = held;
    unsetCursor();
    update();
    return true;
}

void TimelineWidget::setSelectedTracks(const QStringList& ids) {
    if (m_selectedTrackIds == ids) return;
    m_selectedTrackIds = ids;
    update();
}

void TimelineWidget::setSelectedTrack(const QString& id) {
    m_selectedTrackId = id;
    update();
}

void TimelineWidget::setGridBeats(double beats) {
    m_gridBeats = beats;
    update();
}

void TimelineWidget::setSnapEnabled(bool enabled) {
    m_snapEnabled = enabled;
}

void TimelineWidget::setShowBars(bool showBars) {
    m_showBars = showBars;
    update();
}

void TimelineWidget::zoomBy(double factor) {
    m_pixelsPerSecond = std::clamp(m_pixelsPerSecond * factor, 4.0, 1200.0);
    update();
}

void TimelineWidget::zoomToFit() {
    const double duration = std::max(4.0, m_controller->durationSeconds());
    m_scrollSeconds = 0.0;
    m_pixelsPerSecond = std::clamp((width() - 40) / duration, 4.0, 1200.0);
    update();
}

double TimelineWidget::xToSeconds(int x) const {
    return m_scrollSeconds + x / m_pixelsPerSecond;
}
int TimelineWidget::secondsToX(double seconds) const {
    return int((seconds - m_scrollSeconds) * m_pixelsPerSecond);
}

double TimelineWidget::snapSeconds() const {
    if (!m_snapEnabled || m_gridBeats <= 0.0) return 0.0;
    const double tempo = std::max(1.0, m_controller->project().tempo);
    return m_gridBeats * 60.0 / tempo;
}

double TimelineWidget::snap(double seconds, bool enabled) const {
    if (!enabled || m_gridBeats <= 0.0) return std::max(0.0, seconds);
    const double tempo = std::max(1.0, m_controller->project().tempo);
    const double gridSeconds = m_gridBeats * 60.0 / tempo;
    return std::max(0.0, std::round(seconds / gridSeconds) * gridSeconds);
}

int TimelineWidget::laneAt(int yy) const {
    if (yy < ui::kRulerHeight) return -1;
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    int y = ui::kRulerHeight - m_scrollY;
    for (int i = 0; i < int(rows.size()); ++i) {
        const int h = ui::laneHeightForTrack(project.tracks[rows[size_t(i)].index]);
        if (yy < y + h) return i;
        y += h;
    }
    return -1;
}

int TimelineWidget::laneTop(int lane) const {
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    int y = ui::kRulerHeight - m_scrollY;
    for (int i = 0; i < lane && i < int(rows.size()); ++i)
        y += ui::laneHeightForTrack(project.tracks[rows[size_t(i)].index]);
    return y;
}

int TimelineWidget::laneHeightAt(int lane) const {
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    if (lane < 0 || lane >= int(rows.size())) return ui::kLaneHeight;
    return ui::laneHeightForTrack(project.tracks[rows[size_t(lane)].index]);
}

/// The part of a lane a clip's body occupies — the whole lane normally, and just
/// the top strip when the comp editor has grown the lane beneath it.
int TimelineWidget::laneBodyHeightAt(int lane) const {
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    if (lane < 0 || lane >= int(rows.size())) return ui::kLaneHeight;
    return ui::laneHeightFor(project.tracks[rows[size_t(lane)].index].height);
}

int TimelineWidget::lanesBottom() const {
    const auto& rows = daw::visibleTracks(m_controller->project());
    return laneTop(int(rows.size()));
}

int TimelineWidget::lanesHeight() const {
    const auto& project = m_controller->project();
    int total = 0;
    for (const auto& row : daw::visibleTracks(project))
        total += ui::laneHeightForTrack(project.tracks[row.index]);
    return total;
}

int TimelineWidget::visibleLaneHeight() const {
    // What is left of the arrangement once the ruler and whatever the mixer is
    // covering are taken off it. Scrolling is measured against *this*, which is
    // what lets a track be brought into view without collapsing the mixer.
    return std::max(0, height() - ui::kRulerHeight - m_bottomInset);
}

int TimelineWidget::maxVerticalScroll() const {
    return std::max(0, lanesHeight() + ui::kLaneTailPadding - visibleLaneHeight());
}

void TimelineWidget::setVerticalScroll(int y) {
    const int clamped = std::clamp(y, 0, maxVerticalScroll());
    if (clamped == m_scrollY) return;
    m_scrollY = clamped;
    update();
    // The header column is a second view of the same lanes; it follows this one
    // rather than keeping a scroll position of its own.
    emit verticalScrollChanged(m_scrollY);
}

void TimelineWidget::clampVerticalScroll() { setVerticalScroll(m_scrollY); }

void TimelineWidget::setBottomInset(int px) {
    const int value = std::max(0, px);
    if (value == m_bottomInset) return;
    m_bottomInset = value;
    clampVerticalScroll();
    update();
}

void TimelineWidget::ensureLaneVisible(int lane) {
    const auto& rows = daw::visibleTracks(m_controller->project());
    if (lane < 0 || lane >= int(rows.size())) return;
    // laneTop is in screen coordinates, so the arithmetic is done in content
    // coordinates: where the lane sits inside the stack of lanes.
    const int top = laneTop(lane) - ui::kRulerHeight + m_scrollY;
    const int bottom = top + laneHeightAt(lane);
    if (top < m_scrollY) setVerticalScroll(top);
    else if (bottom > m_scrollY + visibleLaneHeight())
        setVerticalScroll(bottom - visibleLaneHeight());
}

bool TimelineWidget::selectionSpanX(int& left, int& right) const {
    if (m_selection.isEmpty() || !m_controller) return false;

    int lo = std::numeric_limits<int>::max();
    int hi = std::numeric_limits<int>::min();
    const auto& project = m_controller->project();
    for (const ClipRef& ref : m_selection) {
        for (const daw::TrackModel& track : project.tracks) {
            if (QString::fromStdString(track.id) != ref.trackId) continue;
            for (const daw::ClipModel& clip : track.clips) {
                if (QString::fromStdString(clip.id) != ref.clipId) continue;
                lo = std::min(lo, secondsToX(clip.startSeconds));
                hi = std::max(hi, secondsToX(clip.startSeconds + clip.durationSeconds));
            }
            break;
        }
    }
    if (lo > hi) return false;

    // Clamped to the visible strip so a clip that is mostly off-screen still
    // gives an answer inside the window rather than one the caller has to
    // discard.
    left = std::clamp(lo, 0, width());
    right = std::clamp(hi, 0, width());
    return true;
}

QString TimelineWidget::trackIdForLane(int lane) const {
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    if (lane < 0 || lane >= int(rows.size())) return {};
    return QString::fromStdString(project.tracks[rows[size_t(lane)].index].id);
}

QRectF TimelineWidget::clipRect(int lane, const daw::ClipModel& clip) const {
    const int y = laneTop(lane);
    const int h = laneBodyHeightAt(lane);
    const int x = secondsToX(clip.startSeconds);
    const int w = std::max(2, int(clip.durationSeconds * m_pixelsPerSecond));
    return QRectF(x, y + kClipVerticalInset, w,
                  std::max(2, h - 2 * kClipVerticalInset));
}

QRectF TimelineWidget::compRect(int lane, const daw::ClipModel& clip) const {
    if (!clip.expanded || clip.takes.empty()) return {};
    const int top = laneTop(lane) + laneBodyHeightAt(lane);
    const int bottom = laneTop(lane) + laneHeightAt(lane);
    if (bottom <= top) return {};
    const int x = secondsToX(clip.startSeconds);
    const int w = std::max(2, int(clip.durationSeconds * m_pixelsPerSecond));
    return QRectF(x, top, w, bottom - top);
}

/// The sub-lane of take `index` inside an expanded clip's editor. Rows are laid
/// out from the top of the comp area downwards, so the newest take (last in the
/// list, and the one just recorded) sits at the bottom nearest the next track.
QRectF TimelineWidget::takeRowRect(const QRectF& comp, int index) const {
    if (comp.isEmpty()) return {};
    const double top = comp.top() + double(index * ui::kTakeRowHeight);
    if (top >= comp.bottom()) return {};
    const double h = std::min(double(ui::kTakeRowHeight), comp.bottom() - top);
    return QRectF(comp.left(), top, comp.width(), h);
}

/// Under the caption strip at the clip's top-left. A clip too small to hold the
/// badge without covering its own waveform simply doesn't show one.
QRectF TimelineWidget::badgeRect(const QRectF& body) const {
    constexpr double kW = 34.0;
    constexpr double kH = 13.0;
    if (body.width() < kW + 10.0 || body.height() < kH + 20.0) return {};
    return QRectF(body.left() + 4.0, body.top() + 16.0, kW, kH);
}

const daw::ClipModel* TimelineWidget::findClipModel(const QString& trackId,
                                                    const QString& clipId) const {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track) return nullptr;
    for (const auto& clip : track->clips) {
        if (QString::fromStdString(clip.id) == clipId) return &clip;
    }
    return nullptr;
}

void TimelineWidget::animateComp(const QString& trackId, const QString& clipId,
                                 bool opening) {
    const std::string tid = trackId.toStdString();
    // One animation at a time — a second would fight the first over the same
    // factor. A collapse that gets interrupted still drops its clip's flag, or
    // the clip would stay open with no editor to show for it.
    if (m_compTimer && m_compTimer->isActive()) {
        m_compTimer->stop();
        if (!m_compAnimOpening && !m_compAnimClipId.isEmpty()) {
            m_controller->setClipExpanded(m_compAnimTrackId.toStdString(),
                                          m_compAnimClipId.toStdString(), false);
            ui::clearCompFactor(m_compAnimTrackId.toStdString());
        }
    }
    const auto* track = m_controller->project().findTrack(tid);
    m_compAnimTrackId = trackId;
    m_compAnimClipId = clipId;
    m_compAnimOpening = opening;
    m_compAnimFrom = track ? ui::compFactor(*track) : (opening ? 0.0 : 1.0);
    m_compAnimElapsedMs = 0.0;
    ui::setCompFactor(tid, m_compAnimFrom);
    if (!m_compTimer) {
        m_compTimer = new QTimer(this);
        m_compTimer->setInterval(16);
        connect(m_compTimer, &QTimer::timeout, this,
                &TimelineWidget::stepCompAnimation);
    }
    m_compTimer->start();
}

void TimelineWidget::stepCompAnimation() {
    m_compAnimElapsedMs += double(m_compTimer->interval());
    const double t =
        std::clamp(m_compAnimElapsedMs / double(ui::kCompAnimMs), 0.0, 1.0);
    const double target = m_compAnimOpening ? 1.0 : 0.0;
    const double eased =
        QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t);
    const std::string tid = m_compAnimTrackId.toStdString();
    ui::setCompFactor(tid, m_compAnimFrom + (target - m_compAnimFrom) * eased);
    if (t >= 1.0) {
        m_compTimer->stop();
        if (m_compAnimOpening) {
            ui::setCompFactor(tid, 1.0);
        } else {
            // The document flag drops only now: the rows have to stay drawable
            // while the lane shrinks, or the editor would blink out of existence
            // before it had finished closing.
            m_controller->setClipExpanded(tid, m_compAnimClipId.toStdString(),
                                          false);
            ui::clearCompFactor(tid);
        }
        m_compAnimClipId.clear();
    }
    updateGeometry();
    update();
    emit laneHeightsChanged();
}

void TimelineWidget::animateClipOpen(const QString& trackId,
                                     const QString& clipId) {
    const daw::ClipModel* clip = findClipModel(trackId, clipId);
    if (!clip || !clip->expanded || clip->takes.empty()) return;
    // An expanded clip nobody has animated reads as fully open, so the factor is
    // forced back to zero first — otherwise there is nothing left to animate.
    ui::setCompFactor(trackId.toStdString(), 0.0);
    animateComp(trackId, clipId, true);
}

void TimelineWidget::toggleClipExpanded(const QString& trackId,
                                        const QString& clipId) {
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    const daw::ClipModel* clip = findClipModel(trackId, clipId);
    if (!track || !clip || clip->takes.empty()) return;   // nothing stacked
    if (!clip->expanded) {
        m_controller->setClipExpanded(trackId.toStdString(),
                                      clipId.toStdString(), true);
        animateComp(trackId, clipId, true);
        return;
    }
    // Another expanded clip on this lane is still holding the editor open, so
    // this one closes on its own and the lane stays where it is.
    bool othersOpen = false;
    for (const auto& c : track->clips) {
        if (QString::fromStdString(c.id) == clipId) continue;
        if (c.expanded && !c.takes.empty()) othersOpen = true;
    }
    if (othersOpen) {
        m_controller->setClipExpanded(trackId.toStdString(),
                                      clipId.toStdString(), false);
        update();
        return;
    }
    animateComp(trackId, clipId, false);
}

bool TimelineWidget::toggleSelectedClipExpanded() {
    if (m_selectedClipId.isEmpty()) return false;
    for (const auto& ref : m_selection) {
        if (ref.clipId != m_selectedClipId) continue;
        const daw::ClipModel* clip = findClipModel(ref.trackId, ref.clipId);
        if (!clip || clip->takes.empty()) return false;
        toggleClipExpanded(ref.trackId, ref.clipId);
        return true;
    }
    return false;
}

/// Which take sub-lane the pointer is over, if any. Only expanded clips have
/// rows, and the rows live strictly below the clip bodies, so a hit here and a
/// clip hit are mutually exclusive.
bool TimelineWidget::hitTestTake(const QPoint& pos, TakeHit& out) const {
    const int lane = laneAt(pos.y());
    if (lane < 0) return false;
    if (pos.y() < laneTop(lane) + laneBodyHeightAt(lane)) return false;
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    const auto& track = project.tracks[rows[size_t(lane)].index];
    for (const auto& clip : track.clips) {
        const QRectF comp = compRect(lane, clip);
        if (comp.isEmpty()) continue;
        if (pos.x() < comp.left() || pos.x() > comp.right()) continue;
        for (int i = 0; i < int(clip.takes.size()); ++i) {
            const QRectF row = takeRowRect(comp, i);
            if (row.isEmpty()) break;
            if (pos.y() < row.top() || pos.y() > row.bottom()) continue;
            out.trackId = QString::fromStdString(track.id);
            out.clipId = QString::fromStdString(clip.id);
            out.takeId = QString::fromStdString(clip.takes[size_t(i)].id);
            out.takeIndex = i;
            out.row = row;
            return true;
        }
    }
    return false;
}

void TimelineWidget::renameTake(const TakeHit& hit) {
    const daw::ClipModel* clip = findClipModel(hit.trackId, hit.clipId);
    const daw::TakeModel* take =
        clip ? daw::findTake(*clip, hit.takeId.toStdString()) : nullptr;
    if (!take) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename Take"), tr("Take name"), QLineEdit::Normal,
        QString::fromStdString(take->name), &ok);
    if (!ok) return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return;
    m_controller->setTakeName(hit.trackId.toStdString(),
                              hit.clipId.toStdString(),
                              hit.takeId.toStdString(), trimmed.toStdString());
    emit projectEdited();
    update();
}

/// Everything a take can have done to it that does not deserve a click target of
/// its own. Deleting the audio file lives here rather than on the row's × chip:
/// it is the one action undo cannot walk back, so it asks first.
void TimelineWidget::showTakeMenu(const TakeHit& hit, const QPoint& globalPos) {
    const daw::ClipModel* clip = findClipModel(hit.trackId, hit.clipId);
    const daw::TakeModel* take =
        clip ? daw::findTake(*clip, hit.takeId.toStdString()) : nullptr;
    if (!take) return;
    const std::string trackId = hit.trackId.toStdString();
    const std::string clipId = hit.clipId.toStdString();
    const std::string takeId = hit.takeId.toStdString();
    const int index = hit.takeIndex;
    const int count = int(clip->takes.size());

    QMenu menu(this);
    QAction* pick = menu.addAction(tr("Use for Whole Clip"));
    // Soloing a take used to be a chip on the row. The chips are gone — the row
    // is audio now, and the pointer over it is always the comp brush — so the
    // one action that had nowhere else to live moved here.
    const bool soloed = m_controller->soloTake() == takeId;
    QAction* solo = menu.addAction(soloed ? tr("Stop Auditioning")
                                          : tr("Audition Take Alone"));
    QAction* rename = menu.addAction(tr("Rename…"));

    // The palette is fixed swatches rather than a colour dialog: a take's colour
    // is a label, and eight distinguishable ones are more useful than 16 million.
    QMenu* colors = menu.addMenu(tr("Colour"));
    static constexpr uint32_t kSwatches[] = {0x4A90D9, 0x59B36B, 0xD9A34A,
                                             0xD96A5A, 0xA97AD9, 0x4FB8C4,
                                             0xC45A96, 0x8A8F98};
    QVector<QPair<QAction*, uint32_t>> colorActions;
    for (uint32_t rgb : kSwatches) {
        QPixmap pm(12, 12);
        pm.fill(colorFromRgb(rgb));
        QAction* a = colors->addAction(QIcon(pm), QString());
        a->setText(tr("Colour %1").arg(colorActions.size() + 1));
        colorActions.push_back({a, rgb});
    }

    QAction* dup = menu.addAction(tr("Duplicate Take"));
    QAction* mute = menu.addAction(take->muted ? tr("Unmute Take")
                                               : tr("Mute Take"));
    menu.addSeparator();
    QAction* up = menu.addAction(tr("Move Up"));
    up->setEnabled(index > 0);
    QAction* down = menu.addAction(tr("Move Down"));
    down->setEnabled(index >= 0 && index < count - 1);
    menu.addSeparator();
    QAction* del = menu.addAction(tr("Delete Take"));
    QAction* delFile = menu.addAction(tr("Delete Take and Audio File…"));
    delFile->setEnabled(clip->kind == daw::ClipKind::Audio &&
                        !take->filePath.empty());

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;

    for (const auto& [action, rgb] : colorActions) {
        if (chosen != action) continue;
        m_controller->setTakeColor(trackId, clipId, takeId, rgb);
        emit projectEdited();
        update();
        return;
    }

    if (chosen == pick) {
        m_controller->selectTake(trackId, clipId, takeId);
    } else if (chosen == solo) {
        if (soloed) m_controller->setSoloTake({}, {}, {});
        else m_controller->setSoloTake(trackId, clipId, takeId);
        update();
        return;   // auditioning is not an edit to the document
    } else if (chosen == rename) {
        renameTake(hit);
        return;
    } else if (chosen == colors->menuAction()) {
        return;
    } else if (chosen == dup) {
        m_controller->duplicateTake(trackId, clipId, takeId);
        emit laneHeightsChanged();
    } else if (chosen == mute) {
        m_controller->setTakeMuted(trackId, clipId, takeId, !take->muted);
    } else if (chosen == up || chosen == down) {
        m_controller->moveTake(trackId, clipId, takeId,
                               size_t(index + (chosen == up ? -1 : 1)));
    } else if (chosen == del) {
        m_controller->removeTake(trackId, clipId, takeId, false);
        emit laneHeightsChanged();
    } else if (chosen == delFile) {
        // The file may still be referenced by another take (a duplicate shares
        // it) — the controller checks and keeps it if so. Say what will happen
        // either way, because undo cannot bring a file back.
        const auto answer = QMessageBox::question(
            this, tr("Delete audio file"),
            tr("Delete \"%1\" and erase its audio file from disk?\n\n"
               "Undo will restore the take but not the file.")
                .arg(QString::fromStdString(take->name)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        m_controller->removeTake(trackId, clipId, takeId, true);
        emit laneHeightsChanged();
    } else {
        return;
    }
    emit projectEdited();
    update();
}

/// Extend the stroke to the pointer and hand the whole swept range to the take
/// being brushed. The range is re-applied from the stroke's origin every move
/// rather than incrementally, so dragging back over material already swept
/// shrinks the selection instead of leaving a trail behind.
///
/// Both ends snap to the grid when snapping is on (Alt bypasses it, as
/// everywhere else here), which is what makes a comp seam land on the beat
/// instead of a few milliseconds off it.
void TimelineWidget::updateSwipe(const QPoint& pos, bool snapOn) {
    if (!m_swiping) return;
    const daw::ClipModel* clip = findClipModel(m_swipeTrackId, m_swipeClipId);
    if (!clip) return;
    const double rel =
        std::clamp(snap(xToSeconds(pos.x()), snapOn) - clip->startSeconds, 0.0,
                   clip->durationSeconds);
    m_swipeToSeconds = rel;
    const double a = std::min(m_swipeFromSeconds, m_swipeToSeconds);
    const double b = std::max(m_swipeFromSeconds, m_swipeToSeconds);
    m_controller->setCompSegment(m_swipeTrackId.toStdString(),
                                 m_swipeClipId.toStdString(),
                                 m_swipeTakeId.toStdString(), a, b);
    update();
}

void TimelineWidget::setAuditionHeld(bool held) {
    if (m_auditionHeld == held) return;
    m_auditionHeld = held;
    // Letting go of A drops the preview: auditioning is a look, not an edit, so
    // nothing about it should outlive the key.
    if (!held && !m_auditionTakeId.isEmpty()) {
        m_controller->setSoloTake({}, {}, {});
        m_auditionTakeId.clear();
        update();
    }
    updateCursor(mapFromGlobal(QCursor::pos()));
}

/// Ctrl+D inside an open editor. The take it copies is the one the comp plays
/// at the playhead — the same take the user is listening to, which is the only
/// unambiguous reading of "duplicate this layer" without a row selection.
bool TimelineWidget::duplicateActiveTake() {
    const auto& project = m_controller->project();
    for (const auto& track : project.tracks) {
        for (const auto& clip : track.clips) {
            if (!clip.expanded || clip.takes.empty()) continue;
            const double rel = m_controller->positionSeconds() - clip.startSeconds;
            std::string takeId =
                daw::activeTakeAt(clip, std::clamp(rel, 0.0,
                                                   clip.durationSeconds));
            if (takeId.empty()) takeId = clip.takes.back().id;
            if (m_controller->duplicateTake(track.id, clip.id, takeId).empty())
                return false;
            emit projectEdited();
            emit laneHeightsChanged();
            update();
            return true;
        }
    }
    return false;
}

bool TimelineWidget::isClipSelected(const QString& clipId) const {
    for (const auto& ref : m_selection)
        if (ref.clipId == clipId) return true;
    return false;
}

bool TimelineWidget::hitTestClip(const QPoint& pos, ClipHit& out) const {
    const int lane = laneAt(pos.y());
    if (lane < 0) return false;
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    const auto& track = project.tracks[rows[size_t(lane)].index];
    // Clip bodies live in the top strip of the lane; the rest belongs to the
    // comp editor, which does its own hit testing.
    if (pos.y() > laneTop(lane) + laneBodyHeightAt(lane)) return false;
    for (const auto& clip : track.clips) {
        const int x = secondsToX(clip.startSeconds);
        const int w =
            std::max(2, int(clip.durationSeconds * m_pixelsPerSecond));
        if (pos.x() >= x && pos.x() <= x + w) {
            out.trackId = QString::fromStdString(track.id);
            out.clipId = QString::fromStdString(clip.id);
            out.startSeconds = clip.startSeconds;
            out.durationSeconds = clip.durationSeconds;
            out.offsetSeconds = clip.offsetSeconds;
            out.fadeInSeconds = clip.fadeInSeconds;
            out.fadeOutSeconds = clip.fadeOutSeconds;
            out.edge = Edge::None;
            out.fade = Fade::None;
            out.fadeCurve = Fade::None;
            out.kind = clip.kind;

            // A MIDI clip has no fades and no gain, and its left edge can't be
            // trimmed: the left trim moves `offsetSeconds`, which for notes
            // means nothing and would silently drag the whole phrase along. It
            // also must not report a gain handle, or the double-click that
            // opens the piano roll would lose to the gain-reset branch.
            if (clip.kind == daw::ClipKind::Midi ||
                clip.kind == daw::ClipKind::Pattern) {
                if (w > 2 * kEdgePx && pos.x() >= x + w - kEdgePx) {
                    out.edge = Edge::Right;
                }
                return true;
            }
            if (clip.kind == daw::ClipKind::Automation) {
                // Both edges trim a curve — pulling the head is how a curve is
                // moved off the start of the song without redrawing it. No
                // fades and no gain handle: the clip's whole interior belongs
                // to the breakpoints.
                if (w > 2 * kEdgePx) {
                    if (pos.x() <= x + kEdgePx) out.edge = Edge::Left;
                    else if (pos.x() >= x + w - kEdgePx) out.edge = Edge::Right;
                }
                return true;
            }

            const int clipTop = laneTop(lane) + kClipVerticalInset;
            // Fade handles live in the top strip of the clip, at the current
            // fade-end positions (at the corners when the fades are zero).
            if (w > 2 * kEdgePx && pos.y() <= clipTop + kFadeZonePx) {
                const double inX = x + clip.fadeInSeconds * m_pixelsPerSecond;
                const double outX = (x + w) - clip.fadeOutSeconds * m_pixelsPerSecond;
                const double dIn = std::abs(pos.x() - inX);
                const double dOut = std::abs(pos.x() - outX);
                const bool inHit = dIn <= kFadeGrabPx;
                const bool outHit = dOut <= kFadeGrabPx;
                if (inHit || outHit) {
                    // When both handles land on the same spot (a fade dragged
                    // all the way across), grab the longer one so it can be
                    // pulled back out again.
                    if (inHit && outHit) {
                        if (clip.fadeOutSeconds > clip.fadeInSeconds)
                            out.fade = Fade::Out;
                        else if (clip.fadeInSeconds > clip.fadeOutSeconds)
                            out.fade = Fade::In;
                        else
                            out.fade = (dOut <= dIn) ? Fade::Out : Fade::In;
                    } else {
                        out.fade = inHit ? Fade::In : Fade::Out;
                    }
                    return true;
                }
            }
            // Each non-zero fade has a midpoint handle for its curve. It lives
            // on the actual envelope, so the target stays visually attached
            // while the user bends it.
            const int clipBottom =
                laneTop(lane) + laneBodyHeightAt(lane) - kClipVerticalInset;
            const double clipHeight = std::max(1, clipBottom - clipTop);
            auto curveHit = [&](Fade side, double seconds, double curve) {
                const double widthPx = seconds * m_pixelsPerSecond;
                if (widthPx < 10.0) return false;
                const double gain = fadeGainAt(0.5, curve);
                const double cx = side == Fade::In
                                      ? x + widthPx * 0.5
                                      : x + w - widthPx * 0.5;
                const double cy = clipBottom - gain * clipHeight;
                return std::hypot(pos.x() - cx, pos.y() - cy) <= kFadeGrabPx;
            };
            if (curveHit(Fade::In, clip.fadeInSeconds, clip.fadeInCurve)) {
                out.fadeCurve = Fade::In;
                return true;
            }
            if (curveHit(Fade::Out, clip.fadeOutSeconds, clip.fadeOutCurve)) {
                out.fadeCurve = Fade::Out;
                return true;
            }
            if (w > 2 * kEdgePx) {
                if (pos.x() <= x + kEdgePx) out.edge = Edge::Left;
                else if (pos.x() >= x + w - kEdgePx) out.edge = Edge::Right;
            }
            // Volume handle: the small circle at the bottom centre of the clip.
            if (std::hypot(pos.x() - (x + w / 2.0), pos.y() - clipBottom) <=
                kGainGrabPx) {
                out.gainHandle = true;
            }
            return true;
        }
    }
    return false;
}

void TimelineWidget::ensurePlayheadVisible() {
    const double pos = m_controller->presentationPositionSeconds();
    const double leftSec = m_scrollSeconds;
    const double rightSec = xToSeconds(width());
    if (pos > rightSec - 0.5) {
        m_scrollSeconds = pos - (rightSec - leftSec) * 0.25;
    } else if (pos < leftSec) {
        m_scrollSeconds = std::max(0.0, pos);
    }
}

QRect TimelineWidget::playheadDirtyRect(int x) const {
    // The ruler handle reaches seven pixels either side of the line. Two extra
    // pixels cover its antialiased edge and the 1.6 px cursor pen.
    return QRect(x - 9, 0, 19, height()).intersected(rect());
}

void TimelineWidget::refreshPlaybackFrame() {
    const double scrollBefore = m_scrollSeconds;
    ensurePlayheadVisible();
    const int currentX =
        secondsToX(m_controller->presentationPositionSeconds());

    if (m_scrollSeconds != scrollBefore) {
        // Auto-follow moved every item horizontally, so a dirty cursor strip is
        // not sufficient for this frame.
        m_lastPlayheadX = currentX;
        m_staticFrameValid = false;
        m_playbackOnlyDirty = {};
        update();
        return;
    }

    if (m_lastPlayheadX && *m_lastPlayheadX == currentX) return;

    QRegion dirty;
    if (m_lastPlayheadX)
        dirty = dirty.united(playheadDirtyRect(*m_lastPlayheadX));
    dirty = dirty.united(playheadDirtyRect(currentX));
    m_lastPlayheadX = currentX;
    if (!dirty.isEmpty()) {
        m_playbackOnlyDirty = m_playbackOnlyDirty.united(dirty);
        update(dirty);
    }
}

void TimelineWidget::refreshRecordingFrame() {
    const double scrollBefore = m_scrollSeconds;
    ensurePlayheadVisible();
    if (m_scrollSeconds != scrollBefore) {
        // Auto-follow changed every x coordinate, so no lane-sized repaint can
        // represent this frame faithfully.
        m_lastPlayheadX =
            secondsToX(m_controller->presentationPositionSeconds());
        m_staticFrameValid = false;
        m_playbackOnlyDirty = {};
        update();
        return;
    }

    const auto& targets = m_controller->recordingTracks();
    if (targets.empty()) return;
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    QRegion dirty;
    int laneY = ui::kRulerHeight - m_scrollY;
    for (const auto& row : rows) {
        const daw::TrackModel& track = project.tracks[row.index];
        const int laneH = ui::laneHeightForTrack(track);
        if (std::find(targets.begin(), targets.end(), track.id) !=
            targets.end()) {
            dirty = dirty.united(
                QRect(0, laneY - 2, width(), laneH + 4));
        }
        laneY += laneH;
    }
    dirty = dirty.intersected(
        QRect(0, ui::kRulerHeight, width(),
              std::max(0, height() - ui::kRulerHeight - m_bottomInset)));
    if (!dirty.isEmpty()) update(dirty);
}

// ── Painting ───────────────────────────────────────────────────────────────

void TimelineWidget::drawGrid(QPainter& p) {
    const auto& project = m_controller->project();
    const double tempo = std::max(1.0, project.tempo);
    const double secondsPerBeat = 60.0 / tempo;
    const int beatsPerBar = std::max(1, project.timeSigNumerator);
    // A playback frame invalidates two 19 px cursor strips. Iterating grid
    // divisions for the complete viewport on every one of those frames made a
    // visually tiny repaint retain almost all of a full paint's fixed cost.
    // Expand by two pixels for the antialiased one-pixel lines and iterate only
    // the actual horizontal clip.
    const QRectF dirty = p.clipBoundingRect().intersected(QRectF(rect()));
    if (dirty.isEmpty()) return;
    const int dirtyLeft = std::max(0, int(std::floor(dirty.left())) - 2);
    const int dirtyRight = std::min(width(), int(std::ceil(dirty.right())) + 2);
    const double leftSec = std::max(0.0, xToSeconds(dirtyLeft));
    const double rightSec = xToSeconds(dirtyRight);
    const Theme& t = th();

    // Subdivision lines, if the chosen division is legible at this zoom.
    if (m_gridBeats > 0.0) {
        const double stepSeconds = m_gridBeats * secondsPerBeat;
        if (stepSeconds * m_pixelsPerSecond >= kMinGridSpacingPx) {
            p.setPen(QPen(mixColors(t.gridLine, t.background, 0.45), 1));
            long first = long(std::floor(leftSec / stepSeconds));
            if (first < 0) first = 0;
            for (long i = first;; ++i) {
                const double time = i * stepSeconds;
                if (time > rightSec) break;
                const int x = secondsToX(time);
                p.drawLine(x, ui::kRulerHeight, x, height());
            }
        }
    }

    // Beat and bar lines on top of the subdivisions.
    long firstBeat = long(std::floor(leftSec / secondsPerBeat));
    if (firstBeat < 0) firstBeat = 0;
    const bool beatsLegible = secondsPerBeat * m_pixelsPerSecond >= kMinGridSpacingPx;
    for (long b = firstBeat;; ++b) {
        const double time = b * secondsPerBeat;
        if (time > rightSec) break;
        const bool isBar = (b % beatsPerBar) == 0;
        if (!isBar && !beatsLegible) continue;
        const int x = secondsToX(time);
        p.setPen(QPen(isBar ? t.gridLineStrong : t.gridLine, 1));
        p.drawLine(x, ui::kRulerHeight, x, height());
    }
}

void TimelineWidget::drawCycleStrip(QPainter& p) {
    const Theme& t = th();
    const QRect strip(0, 0, width(), ui::kLoopStripHeight);

    // The empty strip still reads as a place where something goes: a shade
    // darker than the ruler under it, with a hairline where the two meet.
    p.fillRect(strip, mixColors(t.toolbarBackground, t.background, 0.35));
    p.setPen(QPen(mixColors(t.separator(), t.background, 0.35), 1));
    p.drawLine(0, ui::kLoopStripHeight, width(), ui::kLoopStripHeight);

    const double from = m_controller->loopStartSeconds();
    const double to = m_controller->loopEndSeconds();
    if (!(to > from)) return;

    const int left = secondsToX(from);
    const int right = secondsToX(to);
    if (right < 0 || left > width()) return;
    const bool on = m_controller->isLoopEnabled();

    // Lit when the cycle is armed, an outline when it is only defined. The
    // difference has to be obvious at a glance: "there is a region" and "the
    // playhead is going round it" are not the same state, and pressing C is
    // what moves between them.
    const QColor cycle = Theme::cycle();
    // Square ends make the loop read as an exact time range rather than a pill.
    // Half-pixel alignment keeps the one-pixel border crisp on both themes.
    const QRectF bar(left + 0.5, 0.5, std::max(2, right - left),
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

    // Tiny inward flags name the two ends without adding text or large handles.
    // They remain deliberately dim when a range is defined but not armed.
    QColor flag = mixColors(cycle, t.background, on ? 0.34 : 0.18);
    flag.setAlpha(on ? (t.dark ? 205 : 220) : (t.dark ? 92 : 110));
    p.setPen(Qt::NoPen);
    p.setBrush(flag);
    if (left >= 0 && left <= width()) {
        p.drawPolygon(QPolygonF{QPointF(left + 1.0, 1.0),
                                QPointF(left + 8.0, 1.0),
                                QPointF(left + 1.0, 8.0)});
    }
    if (right >= 0 && right <= width()) {
        p.drawPolygon(QPolygonF{QPointF(right - 1.0, 1.0),
                                QPointF(right - 8.0, 1.0),
                                QPointF(right - 1.0, 8.0)});
    }

    // The lanes get a wash of the same colour while it is armed, so the region
    // is visible where the eye actually is — down on the clips, not up on a
    // 12-pixel strip nobody is looking at.
    if (on) {
        QColor wash = cycle;
        wash.setAlpha(t.dark ? 16 : 22);
        p.fillRect(QRect(left, ui::kRulerHeight, std::max(1, right - left),
                         height() - ui::kRulerHeight),
                   wash);
    }

    // Faint dashed boundaries carry the range through the ruler and every lane.
    // They are visible in the defined-but-off state too, but never as strongly
    // as a grid bar or the playhead.
    QColor edge = cycle;
    edge.setAlpha(on ? (t.dark ? 72 : 88) : (t.dark ? 34 : 46));
    p.setPen(QPen(edge, 1.0, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawLine(left, ui::kLoopStripHeight, left, height());
    p.drawLine(right, ui::kLoopStripHeight, right, height());
    p.setRenderHint(QPainter::Antialiasing, false);
}

TimelineWidget::LoopGrab TimelineWidget::loopGrabAt(int x) const {
    const double from = m_controller->loopStartSeconds();
    const double to = m_controller->loopEndSeconds();
    if (!(to > from)) return LoopGrab::Create;
    const int left = secondsToX(from);
    const int right = secondsToX(to);
    if (std::abs(x - left) <= ui::kLoopEdgeGrab) return LoopGrab::ResizeStart;
    if (std::abs(x - right) <= ui::kLoopEdgeGrab) return LoopGrab::ResizeEnd;
    // Inside means move; anywhere else in the strip starts a new region, which
    // is how a cycle is *re*-defined without first clearing the old one.
    if (x > left && x < right) return LoopGrab::Move;
    return LoopGrab::Create;
}

void TimelineWidget::drawRuler(QPainter& p) {
    const auto& project = m_controller->project();
    const double tempo = std::max(1.0, project.tempo);
    const double secondsPerBeat = 60.0 / tempo;
    const int beatsPerBar = std::max(1, project.timeSigNumerator);
    const QRectF dirty = p.clipBoundingRect().intersected(QRectF(rect()));
    if (dirty.isEmpty()) return;
    // Labels extend to the right of their tick. Include a small look-behind so
    // a tick just outside a cursor strip can still repaint the text crossing
    // into it, without enumerating the rest of the ruler.
    const int dirtyLeft = std::max(0, int(std::floor(dirty.left())) - 96);
    const int dirtyRight = std::min(width(), int(std::ceil(dirty.right())) + 2);
    const double leftSec = std::max(0.0, xToSeconds(dirtyLeft));
    const double rightSec = xToSeconds(dirtyRight);
    const Theme& t = th();

    QLinearGradient rulerFill(0, 0, 0, ui::kRulerHeight);
    rulerFill.setColorAt(0.0, mixColors(t.surfaceElevated, t.toolbarBackground, 0.30));
    rulerFill.setColorAt(1.0, mixColors(t.surface, t.toolbarBackground, 0.45));
    p.fillRect(QRect(0, 0, width(), ui::kRulerHeight), rulerFill);
    drawCycleStrip(p);
    p.setPen(QPen(t.sectionDivider(), 1));
    // The neighbouring QSS borders occupy their last in-bounds pixel. Drawing
    // at y == height was half clipped and made the join look one pixel lower.
    p.drawLine(0, ui::kRulerHeight - 1, width(), ui::kRulerHeight - 1);

    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);

    if (m_showBars) {
        const double barSeconds = secondsPerBeat * beatsPerBar;
        // Thin out bar numbers when bars get close together.
        const int stride = std::max(
            1, int(std::ceil(60.0 / std::max(1.0, barSeconds * m_pixelsPerSecond))));
        long firstBar = long(std::floor(leftSec / barSeconds));
        if (firstBar < 0) firstBar = 0;
        for (long bar = firstBar;; ++bar) {
            const double time = bar * barSeconds;
            if (time > rightSec) break;
            const int x = secondsToX(time);
            p.setPen(QPen(t.gridLineStrong, 1));
            p.drawLine(x, ui::kRulerHeight - 8, x, ui::kRulerHeight);
            if (bar % stride == 0) {
                p.setPen(t.textSecondary);
                p.drawText(x + 4, ui::kRulerHeight - 10,
                           QString::number(bar + 1));
            }
        }
    } else {
        // Seconds ruler: pick a step that keeps labels ~70 px apart.
        const double raw = 70.0 / m_pixelsPerSecond;
        const double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120};
        double step = steps[std::size(steps) - 1];
        for (double s : steps) {
            if (s >= raw) { step = s; break; }
        }
        long first = long(std::floor(leftSec / step));
        if (first < 0) first = 0;
        for (long i = first;; ++i) {
            const double time = i * step;
            if (time > rightSec) break;
            const int x = secondsToX(time);
            p.setPen(QPen(t.gridLineStrong, 1));
            p.drawLine(x, ui::kRulerHeight - 8, x, ui::kRulerHeight);
            p.setPen(t.textSecondary);
            const int mins = int(time) / 60;
            p.drawText(x + 4, ui::kRulerHeight - 10,
                       QString::asprintf("%d:%04.1f", mins, time - mins * 60));
        }
    }
}

void TimelineWidget::drawLanes(QPainter& p) {
    const Theme& t = th();
    const auto& project = m_controller->project();
    const auto& rows = daw::visibleTracks(project);
    const QRegion paintRegion = p.clipRegion();
    const auto rowIntersectsPaint = [&](int y, int height) {
        return paintRegion.intersects(QRect(0, y, width(), height + 1));
    };

    int laneIndex = 0;
    // The same origin `laneTop` uses: the lane stack scrolls under a fixed
    // ruler, and a second accumulator that forgot the offset would leave the
    // lane fills standing still while their clips moved.
    int laneY = ui::kRulerHeight - m_scrollY;
    for (const auto& row : rows) {
        const auto& track = project.tracks[row.index];
        const int laneH = ui::laneHeightForTrack(track);
        if (rowIntersectsPaint(laneY, laneH)) {
            const QString trackId = QString::fromStdString(track.id);
            const bool selected = trackId == m_selectedTrackId ||
                                  m_selectedTrackIds.contains(trackId);

            QColor lane = laneIndex % 2
                              ? mixColors(t.background, t.surface, 0.34)
                              : t.background;
            if (track.kind == daw::TrackKind::Folder ||
                track.kind == daw::TrackKind::Pattern) {
                // Folder lanes are inert: darker, and they hold no clips.
                lane = mixColors(lane, t.background, 0.5);
            }
            // The wash is only needed for a selected, visible row. Keeping this
            // inside the cull also keeps preference and colour work out of the
            // overwhelmingly common off-screen rows.
            if (selected) {
                const QColor wash =
                    ui::selectionWash(colorFromRgb(track.color));
                lane = mixColors(lane, wash, t.dark ? 0.14 : 0.09);
                QColor selectedEdge =
                    mixColors(t.sectionDivider(), wash, 0.60);
                selectedEdge.setAlpha(t.dark ? 150 : 125);
                p.fillRect(0, laneY, width(), laneH, lane);
                p.setPen(QPen(selectedEdge, 1));
                p.drawLine(0, laneY, width(), laneY);
            } else {
                p.fillRect(0, laneY, width(), laneH, lane);
            }
            p.setPen(QPen(mixColors(t.gridLineStrong, t.background, 0.18), 1));
            p.drawLine(0, laneY + laneH, width(), laneY + laneH);
        }
        laneY += laneH;
        ++laneIndex;
    }

    // Grid lines are drawn under the clips but over the lane fills.
    drawGrid(p);

    laneIndex = 0;
    laneY = ui::kRulerHeight - m_scrollY;
    struct PaintedClip {
        const daw::ClipModel* model = nullptr;
        QRectF body;
        QRectF comp;
    };
    // Retain scratch capacity between 16 ms playhead paints. Local vectors
    // previously allocated and released their buffers every frame for every
    // visible arrangement, even though this method is GUI-thread-only and
    // never recursive.
    static thread_local std::vector<PaintedClip> visibleClips;
    static thread_local std::vector<const PaintedClip*> audioByStart;
    visibleClips.clear();
    audioByStart.clear();
    for (const auto& row : rows) {
        const auto& track = project.tracks[row.index];
        const int laneH = ui::laneHeightForTrack(track);
        if (!rowIntersectsPaint(laneY, laneH)) {
            laneY += laneH;
            ++laneIndex;
            continue;
        }
        const QColor trackColor = colorFromRgb(track.color);

        if (track.kind == daw::TrackKind::Pattern) {
            drawPatternClips(p, track, laneY,
                             ui::laneHeightFor(track.height));
            laneY += laneH;
            ++laneIndex;
            continue;
        }
        if (daw::isAutomationLane(track)) {
            drawAutomationClips(p, track, laneY,
                                ui::laneHeightFor(track.height));
            laneY += laneH;
            ++laneIndex;
            continue;
        }
        const std::uint64_t midiNotesRevision =
            m_controller->midiNotesRevision(track.id);

        // Two passes so overlapping clips don't hide each other's audio: first
        // every clip body, then every waveform and its labels. A later clip's
        // body still sits over an earlier one, but both waveforms are painted
        // afterwards, so the overlap shows both waves.
        auto inPaintRegion = [&](const QRectF& r) {
            return paintRegion.intersects(
                r.toAlignedRect().adjusted(-2, -2, 2, 2));
        };

        // laneTop()/laneBodyHeightAt() both resolve visibleTracks(). Calling
        // them from clipRect for every clip made a narrow playhead repaint hash
        // the complete project hierarchy O(clips) times. This lane already has
        // all of that geometry; derive each rectangle once and reuse it below.
        const int bodyH = ui::laneHeightFor(track.height);
        const auto bodyRect = [&](const daw::ClipModel& clip) {
            const int x = secondsToX(clip.startSeconds);
            const int w = std::max(
                2, int(clip.durationSeconds * m_pixelsPerSecond));
            return QRectF(x, laneY + kClipVerticalInset, w,
                          std::max(2, bodyH - 2 * kClipVerticalInset));
        };
        const auto expandedRect = [&](const daw::ClipModel& clip) {
            if (!clip.expanded || clip.takes.empty()) return QRectF{};
            const int top = laneY + bodyH;
            const int bottom = laneY + laneH;
            if (bottom <= top) return QRectF{};
            const int x = secondsToX(clip.startSeconds);
            const int w = std::max(
                2, int(clip.durationSeconds * m_pixelsPerSecond));
            return QRectF(x, top, w, bottom - top);
        };

        // Compute viewport membership once and reuse it for bodies, content,
        // comp rows and crossfades. Previously each pass walked the complete
        // track independently and rebuilt the same rectangle four times.
        visibleClips.clear();
        visibleClips.reserve(track.clips.size());
        for (const auto& clip : track.clips) {
            const QRectF bodyBounds = bodyRect(clip);
            const QRectF compBounds = expandedRect(clip);
            QRectF paintedBounds = bodyBounds;
            if (!compBounds.isEmpty())
                paintedBounds = paintedBounds.united(compBounds);
            if (inPaintRegion(paintedBounds))
                visibleClips.push_back({&clip, bodyBounds, compBounds});
        }

        // Pass 1 — bodies + caption strips.
        for (const PaintedClip& painted : visibleClips) {
            const auto& clip = *painted.model;
            const QRectF& r = painted.body;
            const bool sel = isClipSelected(QString::fromStdString(clip.id));
            const QColor baseColor = clip.muted
                                         ? QColor(101, 105, 113)
                                         : trackColor;
            const QColor clipColor =
                sel ? mixColors(baseColor, Qt::white, 0.22) : baseColor;
            p.setBrush(clipColor);
            p.setPen(QPen(sel ? t.textPrimary
                              : mixColors(clipColor, Qt::black, 0.45),
                          sel ? 1.8 : 1.0));
            p.drawRoundedRect(r, 5, 5);

            const QRectF caption(r.left(), r.top(), r.width(), 14);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 60));
            QPainterPath capPath;
            capPath.addRoundedRect(caption, 5, 5);
            p.drawPath(capPath);
        }

        // Pass 2 — waveforms, fades, names, mono/stereo dots.
        for (const PaintedClip& painted : visibleClips) {
            const auto& clip = *painted.model;
            const QRectF& r = painted.body;
            const QRectF caption(r.left(), r.top(), r.width(), 14);

            const QRectF content(r.left(), caption.bottom(), r.width(),
                                 r.bottom() - caption.bottom());
            if (daw::isLayered(clip)) {
                // A layered clip shows its comp — the assembled result — where
                // a plain clip shows its own waveform. Same picture whether or
                // not the editor is open, so opening it changes nothing above.
                p.save();
                QPainterPath body;
                body.addRoundedRect(r, 5, 5);
                p.setClipPath(body, Qt::IntersectClip);
                drawCompLane(p, track, clip, content, midiNotesRevision);
                p.restore();
                if (clip.kind == daw::ClipKind::Audio) {
                    drawFades(p, clip, r);
                    drawGainHandle(
                        p, clip, r,
                        isClipSelected(QString::fromStdString(clip.id)));
                }
            } else if (clip.kind == daw::ClipKind::Midi) {
                // Notes stand in for the waveform. Fades and the gain handle
                // are audio-only controls and have nothing to act on here.
                drawMidiNotes(p, clip, content, r, midiNotesRevision);
            } else {
                drawWaveform(p, clip, content);
                drawFades(p, clip, r);
                drawGainHandle(p, clip, r,
                               isClipSelected(QString::fromStdString(clip.id)));
            }
            if (clip.muted) {
                // Grey the content, not merely the border: mute remains clear
                // even on tracks whose own colour is already subdued.
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(55, 58, 64, 118));
                p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 4, 4);
                if (clip.kind == daw::ClipKind::Audio) {
                    // Editing affordances stay bright even while the content
                    // itself is visibly switched off.
                    drawFades(p, clip, r);
                    drawGainHandle(
                        p, clip, r,
                        isClipSelected(QString::fromStdString(clip.id)));
                }
            }
            drawTakeBadge(p, clip, r);

            QFont f = p.font();
            f.setPixelSize(10);
            p.setFont(f);
            p.setPen(clip.muted ? QColor(220, 222, 226, 175)
                                : QColor(255, 255, 255, 220));
            const QRectF nameRect = caption.adjusted(clip.muted ? 20 : 6, 0, -4, 0);
            p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                       QString::fromStdString(clip.name));
            if (clip.muted) {
                const QPointF centre(r.left() + 10.0, caption.center().y());
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(225, 227, 232, 190), 1.2));
                p.drawEllipse(centre, 4.0, 4.0);
                p.drawLine(centre + QPointF(-3.0, 3.0),
                           centre + QPointF(3.0, -3.0));
            }

            // Mono/stereo dots just after the name: one circle mono, two stereo.
            // A MIDI clip has no source channels, so it gets none — and is kept
            // out of the cache lookup, which would hash an empty path.
            int channels = clip.kind == daw::ClipKind::Midi ? 0 : clip.channels;
            if (channels <= 0 && clip.kind == daw::ClipKind::Audio) {
                if (const auto* pk =
                        m_controller->waveforms().cached(clip.filePath)) {
                    channels = pk->channels;
                }
            }
            if (channels > 0) {
                const QFontMetrics fm(f);
                const double nameW =
                    fm.horizontalAdvance(QString::fromStdString(clip.name));
                double dotX = nameRect.left() + nameW + 8.0;
                const double dotY = caption.center().y();
                const int dots = channels >= 2 ? 2 : 1;
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 200));
                for (int d = 0; d < dots; ++d) {
                    if (dotX + 3.0 > r.right() - 3.0) break;
                    p.drawEllipse(QPointF(dotX, dotY), 2.0, 2.0);
                    dotX += 6.0;
                }
            }
        }

        // The take being recorded right now, drawn as the clip it is about to
        // become rather than as a red smear: same shape, same caption, and the
        // colour the material will actually have once it lands.
        if (m_controller->isRecording()) {
            drawRecordingClip(p, track, laneY + kClipVerticalInset,
                              bodyH - 2 * kClipVerticalInset);
        }

        // The comp editors of any expanded clips, filling the extra lane height
        // they asked for. Drawn after the bodies so a row never lands under a
        // neighbouring clip that happens to overlap it.
        for (const PaintedClip& painted : visibleClips)
            drawCompEditor(p, laneIndex, track, *painted.model, painted.comp,
                           midiNotesRevision);

        // Crossfade seams: where two clips on this lane overlap they crossfade,
        // drawn on top as two crossing curves over the shared region.
        // Only audio clips crossfade; overlapping MIDI clips simply both play.
        audioByStart.clear();
        audioByStart.reserve(visibleClips.size());
        for (const PaintedClip& painted : visibleClips) {
            if (painted.model->kind == daw::ClipKind::Audio)
                audioByStart.push_back(&painted);
        }
        std::sort(audioByStart.begin(), audioByStart.end(),
                  [](const PaintedClip* a, const PaintedClip* b) {
                      return a->model->startSeconds < b->model->startSeconds;
                  });
        for (size_t i = 0; i + 1 < audioByStart.size(); ++i) {
            const daw::ClipModel& a = *audioByStart[i]->model;
            const daw::ClipModel& b = *audioByStart[i + 1]->model;
            const double aEnd =
                a.startSeconds + a.durationSeconds;
            const double bStart = b.startSeconds;
            if (aEnd <= bStart) continue;
            const QRectF& ar = audioByStart[i]->body;
            const double left = secondsToX(bStart);
            const double right = secondsToX(aEnd);
            const QRectF crossfade(left, ar.top(), right - left, ar.height());
            if (!inPaintRegion(crossfade)) continue;
            p.setPen(QPen(QColor(255, 255, 255, 160), 1.2));
            p.drawLine(QPointF(left, ar.bottom()), QPointF(right, ar.top()));
            p.drawLine(QPointF(left, ar.top()), QPointF(right, ar.bottom()));
        }
        laneY += laneH;
        ++laneIndex;
    }
}

namespace {
/// Room kept above and below a curve inside a clip so a point sitting at 0 or 1
/// is still a circle rather than half of one.
constexpr double kAutomationPad = 7.0;
/// A breakpoint's radius, and how close the pointer must come to grab it.
constexpr double kPointRadius = 4.0;
constexpr double kPointGrab = 8.0;
/// The strip along the top of an automation clip that is *not* the curve.
///
/// Everywhere else on the clip the pointer is editing the curve, which leaves
/// nowhere to take hold of the clip itself. This strip is that grip: it carries
/// the name, and it drags, selects and opens like any other clip's body.
constexpr double kAutomationGrip = 14.0;
} // namespace

QRectF TimelineWidget::automationGrip(const QRectF& body) {
    return QRectF(body.left(), body.top(),
                  body.width(), std::min(kAutomationGrip, body.height() * 0.4));
}

void TimelineWidget::paintCurveAt(const QPoint& pos, bool snapOn) {
    const daw::ClipModel* clip =
        findClipModel(m_pointDrag.trackId, m_pointDrag.clipId);
    if (!clip) return;

    const double beats =
        std::max(0.0, snapBeats(automationBeatsAtX(*clip, pos.x()), snapOn));
    const double value = automationValueAtY(m_pointDrag.body, pos.y());

    std::vector<daw::AutomationPoint> points = clip->automation.points;
    // A stroke overwrites what it passes over rather than piling points on top
    // of it: drawing over a curve replaces that stretch, which is what a pencil
    // does everywhere else.
    const double reach = std::max(m_gridBeats * 0.5,
                                  daw::secondsToBeats(2.0 / m_pixelsPerSecond,
                                                      m_controller->project().tempo));
    std::erase_if(points, [&](const daw::AutomationPoint& point) {
        return std::abs(point.beats - beats) < reach;
    });

    daw::AutomationPoint added;
    added.beats = beats;
    added.value = value;
    points.push_back(added);
    m_controller->setAutomationPoints(m_pointDrag.trackId.toStdString(),
                                      m_pointDrag.clipId.toStdString(), points);
    update();
}

void TimelineWidget::showAutomationMenu(const PointHit& hit,
                                       const QPoint& globalPos) {
    const daw::ClipModel* clip = findClipModel(hit.trackId, hit.clipId);
    if (!clip) return;
    const std::vector<daw::AutomationPoint> before = clip->automation.points;
    std::vector<daw::AutomationPoint> points = before;

    QMenu menu(this);
    QAction* remove = nullptr;
    if (hit.index >= 0 && std::size_t(hit.index) < points.size()) {
        remove = menu.addAction(tr("Delete Point"));
        menu.addSeparator();
    }

    // The shape of the run under the pointer. Named after what it does rather
    // than after its maths — "hold" is what a switch needs, and nobody looks
    // for it under "step interpolation".
    const int segment = hit.index >= 0 ? hit.index : hit.segment;
    QAction* linear = nullptr;
    QAction* hold = nullptr;
    QAction* curve = nullptr;
    QAction* straighten = nullptr;
    if (segment >= 0 && std::size_t(segment) < points.size()) {
        const daw::AutomationPoint& at = points[std::size_t(segment)];
        auto* shapes = menu.addMenu(tr("Segment"));
        const auto add = [&](const QString& text, daw::AutomationSegment kind) {
            QAction* action = shapes->addAction(text);
            action->setCheckable(true);
            action->setChecked(at.shape == kind);
            return action;
        };
        linear = add(tr("Straight"), daw::AutomationSegment::Linear);
        hold = add(tr("Hold"), daw::AutomationSegment::Hold);
        curve = add(tr("S-Curve"), daw::AutomationSegment::SCurve);
        if (std::abs(at.curve) > 1e-9) {
            shapes->addSeparator();
            straighten = shapes->addAction(tr("Remove Bend"));
        }
    }

    menu.addSeparator();
    QAction* smooth = menu.addAction(tr("Smooth Curve"));
    smooth->setToolTip(tr("Ease every segment of this curve into the next"));
    QAction* clear = menu.addAction(tr("Clear Curve"));
    menu.addSeparator();
    QAction* edit = menu.addAction(tr("Open Curve Editor…"));
    edit->setToolTip(tr("Generators, and the fields that point this curve at "
                        "something else"));

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;

    if (chosen == edit) {
        emit openAutomationEditorRequested(hit.trackId, hit.clipId);
        return;
    }
    if (chosen == remove) {
        points.erase(points.begin() + hit.index);
    } else if (chosen == clear) {
        points.clear();
    } else if (chosen == smooth) {
        for (daw::AutomationPoint& point : points) {
            point.shape = daw::AutomationSegment::SCurve;
            point.curve = 0.0;
        }
    } else if (segment >= 0 && std::size_t(segment) < points.size()) {
        daw::AutomationPoint& at = points[std::size_t(segment)];
        if (chosen == linear) at.shape = daw::AutomationSegment::Linear;
        else if (chosen == hold) at.shape = daw::AutomationSegment::Hold;
        else if (chosen == curve) at.shape = daw::AutomationSegment::SCurve;
        else if (chosen == straighten) at.curve = 0.0;
        else return;
    } else {
        return;
    }

    m_controller->setAutomationPoints(hit.trackId.toStdString(),
                                      hit.clipId.toStdString(), points);
    m_controller->commitAutomationEdit(hit.trackId.toStdString(),
                                       hit.clipId.toStdString(), before,
                                       "Edit Automation");
    emit projectEdited();
    update();
}

int TimelineWidget::laneCentreForTest(int lane) const {
    return laneTop(lane) + laneHeightAt(lane) / 2;
}

bool TimelineWidget::isOverAutomation(const QPoint& pos) const {
    PointHit hit;
    return hitTestAutomationPoint(pos, hit);
}

int TimelineWidget::indexOfPointAt(const daw::ClipModel& clip, double beats) {
    const auto& points = clip.automation.points;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (std::abs(points[i].beats - beats) < 1e-6) return int(i);
    }
    return -1;
}

double TimelineWidget::snapBeats(double beats, bool enabled) const {
    if (!enabled || m_gridBeats <= 0.0) return beats;
    return std::round(beats / m_gridBeats) * m_gridBeats;
}

void TimelineWidget::selectAutomationClip(const QString& trackId,
                                          const QString& clipId) {
    if (m_selection.size() == 1 && m_selection.front().clipId == clipId) return;
    m_selection = {ClipRef{trackId, clipId}};
    m_selectedClipId = clipId;
    m_selectedTrackId = trackId;
    publishSelection();
    emit clipSelected(trackId, clipId);
}

bool TimelineWidget::hitTestAutomationPoint(const QPoint& pos,
                                           PointHit& out) const {
    // Only the pointer and the brush draw curves. Under the knife, the eraser,
    // the mute tool or a region select, an automation clip is a *clip*: it
    // cuts, deletes, mutes and selects like every other one, and a stray click
    // never leaves a breakpoint behind.
    if (tool() != Tool::Select && tool() != Tool::Draw) return false;

    const int lane = laneAt(pos.y());
    if (lane < 0) return false;
    const QString trackId = trackIdForLane(lane);
    const auto* track = m_controller->project().findTrack(trackId.toStdString());
    if (!track || !daw::isAutomationLane(*track)) return false;

    const double tempo = m_controller->project().tempo;
    // Last first: a later clip is drawn over an earlier one, so it is the one
    // the pointer is on.
    for (auto it = track->clips.rbegin(); it != track->clips.rend(); ++it) {
        const daw::ClipModel& clip = *it;
        if (clip.kind != daw::ClipKind::Automation) continue;
        const QRectF body = clipRect(lane, clip);
        if (!body.contains(QPointF(pos))) continue;
        // The grip belongs to the clip, not to the curve.
        if (automationGrip(body).contains(QPointF(pos))) return false;

        out.trackId = trackId;
        out.clipId = QString::fromStdString(clip.id);
        out.body = body;
        out.beats = automationBeatsAtX(clip, pos.x());
        out.value = automationValueAtY(body, pos.y());
        out.index = -1;
        out.segment = -1;

        const auto& points = clip.automation.points;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const double x = secondsToX(clip.startSeconds +
                                        daw::beatsToSeconds(points[i].beats, tempo));
            const double y = automationValueToY(body, points[i].value);
            if (QLineF(QPointF(x, y), QPointF(pos)).length() <= kPointGrab) {
                out.index = int(i);
                break;
            }
        }
        // Which run the pointer is over, so a bend gesture knows what it bends.
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            if (out.beats >= points[i].beats && out.beats <= points[i + 1].beats) {
                out.segment = int(i);
                break;
            }
        }
        return true;
    }
    return false;
}

double TimelineWidget::automationValueToY(const QRectF& body, double value) const {
    // Less room above than below: the grip already separates the curve from
    // whatever is over it, so spending a second pad there only squeezes a lane
    // that has little height to spare.
    const double top = automationGrip(body).bottom() + 4.0;
    const double bottom = body.bottom() - kAutomationPad;
    return bottom - (bottom - top) * std::clamp(value, 0.0, 1.0);
}

double TimelineWidget::automationValueAtY(const QRectF& body, double y) const {
    const double top = automationGrip(body).bottom() + 4.0;
    const double bottom = body.bottom() - kAutomationPad;
    if (bottom - top < 1.0) return 0.0;
    return std::clamp((bottom - y) / (bottom - top), 0.0, 1.0);
}

double TimelineWidget::automationBeatsAtX(const daw::ClipModel& clip, int x) const {
    return daw::secondsToBeats(xToSeconds(x) - clip.startSeconds,
                               m_controller->project().tempo);
}

void TimelineWidget::drawAutomationClips(QPainter& p,
                                         const daw::TrackModel& track,
                                         int laneY, int bodyH) {
    const Theme& t = th();
    const double tempo = m_controller->project().tempo;
    const QRegion paintRegion = p.clipRegion();

    for (const daw::ClipModel& clip : track.clips) {
        if (clip.kind != daw::ClipKind::Automation) continue;
        const int x = secondsToX(clip.startSeconds);
        const int w = std::max(
            2, int(clip.durationSeconds * m_pixelsPerSecond));
        const QRectF body(x, laneY + kClipVerticalInset, w,
                          std::max(2, bodyH - 2 * kClipVerticalInset));
        if (!paintRegion.intersects(
                body.toAlignedRect().adjusted(-2, -2, 2, 2)))
            continue;

        const bool selected = isClipSelected(QString::fromStdString(clip.id));
        const QColor accent = clip.muted ? QColor(101, 105, 113)
                                         : colorFromRgb(clip.color);

        // The body is a well the curve is drawn *in*, not a block the curve is
        // drawn *on*: a solid clip the colour of the track would fight the line
        // it is there to show.
        QColor fill = mixColors(t.well(), accent, t.dark ? 0.16 : 0.10);
        if (selected) fill = mixColors(fill, t.textPrimary, 0.10);
        p.setBrush(fill);
        p.setPen(QPen(selected ? t.textPrimary
                               : mixColors(accent, t.background, 0.45),
                      selected ? 1.8 : 1.0));
        p.drawRoundedRect(body, 5, 5);

        p.save();
        p.setClipRect(body.adjusted(1, 1, -1, -1), Qt::IntersectClip);

        // Guides at a quarter, a half and three quarters, so a value can be
        // read off the lane without dragging anything to find out.
        p.setPen(QPen(mixColors(t.gridLine, t.background, 0.35), 1.0));
        for (const double at : {0.25, 0.5, 0.75}) {
            const double y = automationValueToY(body, at);
            p.drawLine(QPointF(body.left(), y), QPointF(body.right(), y));
        }

        // The curve itself, sampled per pixel so a bend or an S reads as the
        // shape it is rather than as the straight line between its ends.
        const auto& points = clip.automation.points;
        const double fallback = clip.automation.defaultValue;
        const double lengthBeats = daw::secondsToBeats(clip.durationSeconds, tempo);
        const QRectF painted = body.intersected(p.clipBoundingRect());
        const int from = painted.isEmpty()
                             ? 0
                             : int(std::floor(painted.left())) - 1;
        const int to = painted.isEmpty()
                           ? -1
                           : int(std::ceil(painted.right())) + 1;

        static thread_local QPolygonF line;
        static thread_local QPolygonF under;
        line.clear();
        line.reserve(std::max(0, to - from + 1));
        for (int x = from; x <= to; ++x) {
            const double beats =
                std::clamp(automationBeatsAtX(clip, x), 0.0, lengthBeats);
            line << QPointF(x, automationValueToY(
                                   body, daw::automationValueAt(points, beats,
                                                                fallback)));
        }
        if (line.size() >= 2) {
            under = line;
            under << QPointF(line.back().x(), body.bottom());
            under << QPointF(line.front().x(), body.bottom());
            QColor wash = accent;
            wash.setAlpha(t.dark ? 44 : 54);
            p.setPen(Qt::NoPen);
            p.setBrush(wash);
            p.drawPolygon(under);

            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(Qt::NoBrush);
            // A dark halo under the line, so it stays readable where it crosses
            // its own fill.
            p.setPen(QPen(mixColors(t.background, accent, 0.25), 3.0));
            p.drawPolyline(line);
            p.setPen(QPen(mixColors(accent, t.textPrimary, 0.25), 1.8));
            p.drawPolyline(line);
        }

        // The breakpoints. Only the ones inside the clip: a point past the end
        // is kept in the document — trimming a clip shorter and longer again
        // must not destroy the curve — but it does not sound and is not shown.
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF pointDirty =
            p.clipBoundingRect().adjusted(-kPointRadius - 2.0, 0.0,
                                           kPointRadius + 2.0, 0.0);
        const double firstVisibleBeat = std::clamp(
            automationBeatsAtX(clip, int(std::floor(pointDirty.left()))),
            0.0, lengthBeats);
        const double lastVisibleBeat = std::clamp(
            automationBeatsAtX(clip, int(std::ceil(pointDirty.right()))),
            0.0, lengthBeats);
        auto point = std::lower_bound(
            points.begin(), points.end(), firstVisibleBeat,
            [](const daw::AutomationPoint& candidate, double beats) {
                return candidate.beats < beats;
            });
        for (; point != points.end() &&
               point->beats <= lastVisibleBeat + 1e-9;
             ++point) {
            if (point->beats < 0.0 || point->beats > lengthBeats + 1e-9)
                continue;
            const double x = secondsToX(clip.startSeconds +
                                        daw::beatsToSeconds(point->beats, tempo));
            if (x < body.left() - 2 || x > body.right() + 2) continue;
            const QPointF at(x, automationValueToY(body, point->value));
            p.setBrush(t.surfaceElevated);
            p.setPen(QPen(mixColors(accent, t.textPrimary, 0.30), 1.8));
            p.drawEllipse(at, kPointRadius, kPointRadius);
            p.setPen(Qt::NoPen);
            p.setBrush(mixColors(accent, t.textPrimary, 0.30));
            p.drawEllipse(at, 1.5, 1.5);
        }
        p.setRenderHint(QPainter::Antialiasing, false);

        // The grip goes on last, over everything: it is the one part of the
        // clip that is not the curve — where the pointer takes hold of the clip
        // itself — so it has to stay legible even where a curve pressed against
        // the ceiling would otherwise run through its name.
        const QRectF grip = automationGrip(body);
        p.setPen(Qt::NoPen);
        p.setBrush(mixColors(fill, accent, t.dark ? 0.34 : 0.24));
        p.drawRect(grip);
        p.setPen(QPen(mixColors(accent, t.background, 0.4), 1.0));
        p.drawLine(grip.bottomLeft(), grip.bottomRight());
        if (grip.width() > 34) {
            QFont label = p.font();
            label.setPixelSize(9);
            p.setFont(label);
            p.setPen(mixColors(t.textPrimary, accent, 0.25));
            p.drawText(grip.adjusted(6, 0, -5, 0),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(label).elidedText(
                           QString::fromStdString(clip.name), Qt::ElideRight,
                           int(grip.width()) - 11));
        }
        p.restore();
    }
}

void TimelineWidget::drawPatternClips(
    QPainter& p, const daw::TrackModel& pattern, int laneY, int bodyH) {
    const Theme& t = th();
    const auto& project = m_controller->project();
    const QRegion paintRegion = p.clipRegion();
    std::optional<std::vector<std::string>> children;

    for (const daw::ClipModel& container : pattern.clips) {
        if (container.kind != daw::ClipKind::Pattern) continue;
        const int x = secondsToX(container.startSeconds);
        const int w = std::max(
            2, int(container.durationSeconds * m_pixelsPerSecond));
        const QRectF body(x, laneY + kClipVerticalInset, w,
                          std::max(2, bodyH - 2 * kClipVerticalInset));
        if (!paintRegion.intersects(
                body.toAlignedRect().adjusted(-2, -2, 2, 2)))
            continue;

        if (!children) children = daw::subtreeOf(project, pattern.id);

        int low = 127;
        int high = 0;
        int notes = 0;
        int sources = 0;
        for (const std::string& id : *children) {
            const auto* child = project.findTrack(id);
            if (!child) continue;
            bool used = false;
            for (const auto& childClip : child->clips) {
                if (childClip.kind != daw::ClipKind::Midi ||
                    childClip.patternClipId != container.id) {
                    continue;
                }
                used = true;
                for (const auto& note : childClip.notes) {
                    low = std::min(low, note.pitch);
                    high = std::max(high, note.pitch);
                    ++notes;
                }
            }
            if (used) ++sources;
        }

        const QColor base = container.muted
                                ? QColor(101, 105, 113)
                                : colorFromRgb(pattern.color);
        const bool selected =
            isClipSelected(QString::fromStdString(container.id));
        const QColor fill = selected ? mixColors(base, t.textPrimary, 0.20)
                                     : base;
        p.setBrush(fill);
        p.setPen(QPen(selected ? t.textPrimary
                               : mixColors(fill, t.background, 0.52),
                      selected ? 1.8 : 1.0));
        p.drawRoundedRect(body, 6, 6);

        const QRectF caption(body.left(), body.top(), body.width(), 15);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 70));
        QPainterPath cap;
        cap.addRoundedRect(caption, 6, 6);
        p.drawPath(cap);

        const QRectF content(body.left() + 4, caption.bottom() + 2,
                             body.width() - 8,
                             body.bottom() - caption.bottom() - 5);
        if (notes > 0 && content.height() >= 5.0) {
            const int used = high - low + 1;
            const int span = std::max(12, used);
            const int basePitch = low - (span - used) / 2;
            const double rowH = content.height() / double(span);
            QPainterPath clipPath;
            clipPath.addRoundedRect(body, 6, 6);
            p.save();
            p.setClipPath(clipPath, Qt::IntersectClip);
            for (const std::string& id : *children) {
                const auto* child = project.findTrack(id);
                if (!child) continue;
                const QColor noteColor = mixColors(
                    colorFromRgb(child->color), t.textPrimary,
                    container.muted ? 0.25 : 0.48);
                p.setBrush(noteColor);
                p.setPen(Qt::NoPen);
                for (const auto& childClip : child->clips) {
                    if (childClip.kind != daw::ClipKind::Midi ||
                        childClip.patternClipId != container.id) {
                        continue;
                    }
                    for (const auto& note : childClip.notes) {
                        const double noteStart = childClip.startSeconds +
                            daw::beatsToSeconds(note.startBeats,
                                               project.tempo);
                        const double noteLength = daw::beatsToSeconds(
                            note.lengthBeats, project.tempo);
                        const double nx = secondsToX(noteStart);
                        const double nw = std::max(
                            2.0, noteLength * m_pixelsPerSecond);
                        const double ny = content.bottom() -
                            double(note.pitch - basePitch + 1) * rowH;
                        const QRectF noteRect(
                            nx, ny, nw, std::max(2.0, rowH * 0.78));
                        if (paintRegion.intersects(
                                noteRect.toAlignedRect().adjusted(-1, -1, 1, 1)))
                            p.drawRoundedRect(noteRect, 1.5, 1.5);
                    }
                }
            }
            p.restore();
        }

        QFont font = p.font();
        font.setPixelSize(10);
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(255, 255, 255,
                        container.muted ? 170 : 225));
        const QString label = sources > 0
            ? tr("%1 · %2 sources")
                  .arg(QString::fromStdString(
                      container.name.empty() ? pattern.name : container.name))
                  .arg(sources)
            : tr("%1 · add a sound")
                  .arg(QString::fromStdString(
                      container.name.empty() ? pattern.name : container.name));
        p.drawText(caption.adjusted(7, 0, -5, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, label);
    }
}

void TimelineWidget::drawTakeBadge(QPainter& p, const daw::ClipModel& clip,
                                   const QRectF& body) {
    if (clip.takes.empty()) return;
    const QRectF r = badgeRect(body);
    if (r.isEmpty()) return;
    const int count = int(clip.takes.size());
    // A single take is barely worth mentioning: drawn dim, so the clip reads as
    // a container without shouting about it.
    const int alpha = count > 1 ? 150 : 60;

    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, alpha));
    p.drawRoundedRect(r, 3, 3);

    // Stacked sheets: three bars, each a pixel further right than the one below
    // it, so the glyph reads as a pile of takes.
    const double sx = r.left() + 3.5;
    double sy = r.top() + 3.0;
    p.setBrush(QColor(255, 255, 255, count > 1 ? 210 : 120));
    for (int i = 0; i < 3; ++i) {
        p.drawRect(QRectF(sx + double(i), sy, 7.0, 1.6));
        sy += 3.0;
    }

    QFont f = p.font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, count > 1 ? 235 : 140));
    p.drawText(QRectF(r.left() + 14.0, r.top(), 12.0, r.height()),
               Qt::AlignCenter, QString::number(count));

    // The chevron is the affordance that opens the editor: it points down while
    // the clip is closed and up once the rows are showing.
    const QPointF c(r.right() - 6.0, r.center().y());
    constexpr double d = 3.0;
    QPolygonF tri;
    if (clip.expanded) {
        tri << QPointF(c.x() - d, c.y() + d * 0.6)
            << QPointF(c.x() + d, c.y() + d * 0.6)
            << QPointF(c.x(), c.y() - d * 0.8);
    } else {
        tri << QPointF(c.x() - d, c.y() - d * 0.6)
            << QPointF(c.x() + d, c.y() - d * 0.6)
            << QPointF(c.x(), c.y() + d * 0.8);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 205));
    p.drawPolygon(tri);
    p.restore();
}

/// The assembled comp across a clip's body: each segment's audio drawn in its
/// own take's colour, with a hairline at every seam. This is what a layered clip
/// shows instead of a single waveform — collapsed or expanded, it is the same
/// picture, so closing the editor doesn't change what the clip looks like.
void TimelineWidget::drawCompLane(QPainter& p, const daw::TrackModel& track,
                                  const daw::ClipModel& clip,
                                  const QRectF& area,
                                  std::uint64_t midiNotesRevision) {
    (void)track;
    if (area.height() < 4.0) return;
    for (const auto& seg : clip.comp) {
        const daw::TakeModel* take = daw::findTake(clip, seg.takeId);
        if (!take || take->muted) continue;
        const double x0 = area.left() + seg.startSeconds * m_pixelsPerSecond;
        const double x1 = area.left() + seg.endSeconds * m_pixelsPerSecond;
        const QRectF slice(x0, area.top(), std::max(1.0, x1 - x0),
                           area.height());
        if (!p.clipRegion().intersects(slice.toAlignedRect())) continue;
        p.save();
        p.setClipRect(slice.intersected(area), Qt::IntersectClip);
        drawTakeAudio(p, clip, *take, area,
                      mixColors(colorFromRgb(take->color), Qt::white, 0.65),
                      midiNotesRevision);
        p.restore();
    }
    // Seams: the joins the comp was assembled at, marked so the user can see
    // where one attempt hands over to the next.
    p.setPen(QPen(QColor(0, 0, 0, 120), 1.0));
    for (size_t i = 1; i < clip.comp.size(); ++i) {
        const double x =
            area.left() + clip.comp[i].startSeconds * m_pixelsPerSecond;
        if (x < area.left() || x > area.right() ||
            !p.clipRegion().intersects(
                QRect(int(std::floor(x)) - 1, int(std::floor(area.top())), 3,
                      int(std::ceil(area.height())))))
            continue;
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
}

namespace {
/// The take's badge: number and name, floating over the left edge of its own
/// row. It used to be a 116 px label column with buttons in it, which pushed
/// the row's audio that far right of the clip it belongs to — so the waveform
/// under a clip was never actually under it, and a swipe landed a comp range
/// 116 px from the pointer. The row is now pure audio in the clip's own time,
/// and the badge is a label drawn on top of it, not a column carved out of it.
QRectF takeBadgeRect(const QRectF& row, const QFontMetrics& fm,
                     const QString& text) {
    // Never more than a third of the row: the badge is a label on the audio,
    // and audio it hides is audio the user cannot aim a swipe at.
    const double w = std::min({row.width() - 8.0, row.width() * 0.45,
                               double(fm.horizontalAdvance(text)) + 14.0});
    if (w <= 12.0) return {};
    return QRectF(row.left() + 4.0, row.top() + 4.0, w, 14.0);
}
} // namespace

void TimelineWidget::drawTakeRow(QPainter& p, const daw::TrackModel& track,
                                 const daw::ClipModel& clip,
                                 const daw::TakeModel& take, int index,
                                 const QRectF& row, double reveal,
                                 std::uint64_t midiNotesRevision) {
    (void)track;
    const Theme& t = th();
    const QColor takeColor = colorFromRgb(take.color);
    const bool solo = m_controller->soloTake() == take.id;

    p.save();
    // The cascade: rows slide up into place and fade in, so a stack of five
    // takes reads as a stack building rather than a block appearing.
    p.setOpacity(std::clamp(reveal, 0.0, 1.0));
    p.translate(0.0, (1.0 - reveal) * 10.0);

    p.setPen(Qt::NoPen);
    p.setBrush(mixColors(t.background, t.surface, index % 2 ? 0.35 : 0.20));
    p.drawRect(row);

    // The audio, in the clip's own time: row.left() *is* the clip's start, so a
    // peak in a take sits directly under the same peak in the comp above it and
    // the pointer means the same instant in both.
    const QRectF wave(row.left(), row.top() + 3.0, row.width(),
                      row.height() - 6.0);
    p.save();
    p.setClipRect(wave, Qt::IntersectClip);
    // The wave is pushed away from the row underneath it, not always towards
    // white: on a light palette a lightened take colour washes out into the
    // row and the layer stops being readable at all.
    drawTakeAudio(p, clip, take, wave,
                  take.muted ? QColor(120, 120, 120, 110)
                             : mixColors(takeColor,
                                         t.dark ? QColor(255, 255, 255)
                                                : QColor(0, 0, 0),
                                         0.35),
                  midiNotesRevision);
    p.restore();

    // Where the comp takes this take, lit up over the dim wave.
    for (const auto& seg : clip.comp) {
        if (seg.takeId != take.id) continue;
        const double x0 = row.left() + seg.startSeconds * m_pixelsPerSecond;
        const double x1 = row.left() + seg.endSeconds * m_pixelsPerSecond;
        const QRectF lit(x0, row.top(), std::max(1.0, x1 - x0), row.height());
        const QRectF clipped = lit.intersected(row);
        if (clipped.isEmpty()) continue;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(takeColor.red(), takeColor.green(), takeColor.blue(),
                          70));
        p.drawRect(clipped);
        p.setPen(QPen(mixColors(takeColor, Qt::white, 0.4), 1.0));
        p.drawLine(clipped.topLeft(), clipped.bottomLeft());
        p.drawLine(clipped.topRight(), clipped.bottomRight());
    }

    // The badge. Muted and auditioned are drawn into it rather than given chips
    // of their own — both are states you set from the row's menu and then want
    // out of the way. The take the comp mostly plays wears a ring, so
    // double-clicking a row to promote it has something visible to change.
    double owned = 0.0;
    for (const auto& seg : clip.comp) {
        if (seg.takeId == take.id) owned += seg.endSeconds - seg.startSeconds;
    }
    const bool leading = owned > clip.durationSeconds * 0.5;
    QFont f = p.font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    QString text = QString::number(index + 1) + QStringLiteral("  ") +
                   QString::fromStdString(take.name);
    if (take.muted) text += tr("  (muted)");
    if (solo) text += tr("  (solo)");
    const QRectF badge = takeBadgeRect(row, fm, text);
    if (!badge.isEmpty()) {
        p.setPen(leading ? QPen(t.ink(190), 1.0) : QPen(Qt::NoPen));
        p.setBrush(take.muted ? QColor(90, 90, 90, 180)
                              : QColor(takeColor.red(), takeColor.green(),
                                       takeColor.blue(), solo ? 225 : 175));
        p.drawRoundedRect(badge, 7, 7);
        p.setPen(takeColor.lightness() > 140 || take.muted
                     ? QColor(20, 20, 20)
                     : QColor(245, 245, 245));
        p.drawText(badge.adjusted(6, 0, -4, 0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(text, Qt::ElideRight,
                                 int(badge.width() - 10.0)));
    }

    p.setPen(QPen(t.separator(), 1.0));
    p.drawLine(QPointF(row.left(), row.bottom()),
               QPointF(row.right(), row.bottom()));
    p.restore();
}

void TimelineWidget::drawCompEditor(QPainter& p, int lane,
                                    const daw::TrackModel& track,
                                    const daw::ClipModel& clip,
                                    const QRectF& comp,
                                    std::uint64_t midiNotesRevision) {
    (void)lane;
    if (comp.isEmpty()) return;
    if (comp.right() < 0 || comp.left() > width()) return;

    const Theme& t = th();
    p.setPen(Qt::NoPen);
    p.setBrush(mixColors(t.background, Qt::black, 0.25));
    p.drawRect(comp);

    const int count = int(clip.takes.size());
    for (int i = 0; i < count; ++i) {
        const QRectF row = takeRowRect(comp, i);
        if (row.isEmpty()) break;   // the lane has not grown this far yet
        drawTakeRow(p, track, clip, clip.takes[size_t(i)], i, row,
                    ui::takeRowReveal(track, i, count), midiNotesRevision);
    }

    // The layer being recorded into this clip, as the row it is about to be.
    // The stack grows by it while it is being played, so a punch-in reads as
    // "this is becoming take 4" rather than as a red bar over the clip.
    if (ui::pendingTakeClip(track) == clip.id) {
        const QRectF row = takeRowRect(comp, count);
        if (!row.isEmpty()) drawRecordingTakeRow(p, track, clip, row);
    }

    // The stroke in flight, drawn as a bright band over the take being brushed
    // so the user sees the range before letting go. The band carries its own
    // length: comping is judged in bars and seconds, and the alternative is
    // letting go to find out.
    if (m_swiping && QString::fromStdString(clip.id) == m_swipeClipId) {
        const double a = std::min(m_swipeFromSeconds, m_swipeToSeconds);
        const double b = std::max(m_swipeFromSeconds, m_swipeToSeconds);
        const QRectF band(comp.left() + a * m_pixelsPerSecond, comp.top(),
                          std::max(1.0, (b - a) * m_pixelsPerSecond),
                          comp.height());
        p.setPen(QPen(t.ink(150), 1.0));
        p.setBrush(t.ink(30));
        const QRectF drawn = band.intersected(comp);
        p.drawRect(drawn);

        const double tempo = std::max(1.0, m_controller->project().tempo);
        const QString text = QString("%1 s · %2 beats")
                                 .arg(b - a, 0, 'f', 2)
                                 .arg((b - a) * tempo / 60.0, 0, 'f', 2);
        const QFontMetrics fm(p.font());
        QRectF label(0, 0, fm.horizontalAdvance(text) + 12.0, fm.height() + 4.0);
        label.moveCenter(QPointF(drawn.center().x(), comp.top() - label.height()));
        if (label.left() < comp.left()) label.moveLeft(comp.left());
        if (label.right() > comp.right()) label.moveRight(comp.right());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(label, 4.0, 4.0);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(label, Qt::AlignCenter, text);
    }
}

void TimelineWidget::drawFades(QPainter& p, const daw::ClipModel& clip,
                               const QRectF& r) {
    const double inW = clip.fadeInSeconds * m_pixelsPerSecond;
    const double outW = clip.fadeOutSeconds * m_pixelsPerSecond;
    const bool sel = isClipSelected(QString::fromStdString(clip.id));
    const QColor wedge(0, 0, 0, 90);
    const QColor line(255, 255, 255, 170);

    p.setRenderHint(QPainter::Antialiasing, true);
    auto fadePath = [&](bool fadeIn, double width, double curve) {
        QPainterPath path;
        constexpr int kSteps = 24;
        for (int i = 0; i <= kSteps; ++i) {
            const double x = double(i) / kSteps;
            const double gain = fadeGainAt(fadeIn ? x : 1.0 - x, curve);
            const QPointF point(fadeIn ? r.left() + x * width
                                       : r.right() - width + x * width,
                                r.bottom() - gain * r.height());
            if (i == 0) path.moveTo(point);
            else path.lineTo(point);
        }
        return path;
    };

    if (inW > 1.0) {
        const QPainterPath curve = fadePath(true, inW, clip.fadeInCurve);
        QPainterPath fill;
        fill.moveTo(r.left(), r.top());
        fill.lineTo(r.left() + inW, r.top());
        for (int i = 24; i >= 0; --i) {
            const double x = double(i) / 24.0;
            const double gain = fadeGainAt(x, clip.fadeInCurve);
            fill.lineTo(r.left() + x * inW,
                        r.bottom() - gain * r.height());
        }
        fill.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(wedge);
        p.drawPath(fill);
        p.setPen(QPen(line, 1.4));
        p.drawPath(curve);
    }
    if (outW > 1.0) {
        const QPainterPath curve = fadePath(false, outW, clip.fadeOutCurve);
        QPainterPath fill;
        fill.moveTo(r.right() - outW, r.top());
        fill.lineTo(r.right(), r.top());
        for (int i = 24; i >= 0; --i) {
            const double x = double(i) / 24.0;
            const double gain = fadeGainAt(1.0 - x, clip.fadeOutCurve);
            fill.lineTo(r.right() - outW + x * outW,
                        r.bottom() - gain * r.height());
        }
        fill.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(wedge);
        p.drawPath(fill);
        p.setPen(QPen(line, 1.4));
        p.drawPath(curve);
    }

    // Grab knobs at the top corners, shown on the selected clip so the fade is
    // clearly draggable (also visible when a fade already exists).
    if (sel || inW > 1.0 || outW > 1.0) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 230));
        p.drawEllipse(QPointF(r.left() + inW, r.top()), 3.0, 3.0);
        p.drawEllipse(QPointF(r.right() - outW, r.top()), 3.0, 3.0);
        if (inW >= 10.0) {
            const double gain = fadeGainAt(0.5, clip.fadeInCurve);
            p.drawEllipse(QPointF(r.left() + inW * 0.5,
                                  r.bottom() - gain * r.height()), 3.0, 3.0);
        }
        if (outW >= 10.0) {
            const double gain = fadeGainAt(0.5, clip.fadeOutCurve);
            p.drawEllipse(QPointF(r.right() - outW * 0.5,
                                  r.bottom() - gain * r.height()), 3.0, 3.0);
        }
    }
}

namespace {
/// How far through the recording pulse we are, 0…1 and back. Driven by the wall
/// clock rather than a frame counter so the beat of it stays the same whether
/// the arrangement is repainting at 30 fps or struggling.
double recordPulse() {
    const double ms = double(QDateTime::currentMSecsSinceEpoch() % 1200);
    return 0.5 -
           0.5 * std::cos(ms / 1200.0 * 2.0 * std::numbers::pi_v<double>);
}
}  // namespace

void TimelineWidget::drawRecordingTakeRow(QPainter& p,
                                          const daw::TrackModel& track,
                                          const daw::ClipModel& clip,
                                          const QRectF& row) {
    const auto preview = m_controller->recordingPreview(track.id);
    if (!preview.active || preview.targetClipId != clip.id) return;

    const Theme& t = th();
    const QColor color = colorFromRgb(preview.color);
    const QColor rec = Theme::record();

    p.save();
    p.setPen(Qt::NoPen);
    // The same alternating ground the landed rows sit on, so the new row is
    // part of the stack rather than something floating over it.
    p.setBrush(mixColors(t.background, t.surface,
                         int(clip.takes.size()) % 2 ? 0.35 : 0.20));
    p.drawRect(row);

    for (const auto& span : preview.spans) {
        const double left = (span.startSeconds - m_scrollSeconds) * m_pixelsPerSecond;
        const double right = (span.endSeconds - m_scrollSeconds) * m_pixelsPerSecond;
        const QRectF piece(left, row.top(), std::max(1.0, right - left),
                           row.height());
        const QRectF drawn = piece.intersected(row);
        if (drawn.isEmpty()) continue;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(color.red(), color.green(), color.blue(), 70));
        p.drawRect(drawn);
        // Pushed away from the row under it exactly as a landed take's wave
        // is, so the new row belongs to the same stack.
        drawRecordingEnvelope(p, preview, span, drawn, row,
                              mixColors(color,
                                        t.dark ? QColor(255, 255, 255)
                                               : QColor(0, 0, 0),
                                        0.35));
    }

    // The badge the landed rows carry, with a record dot in place of the
    // number it does not have yet.
    QFont f = p.font();
    f.setPixelSize(10);
    p.setFont(f);
    const QFontMetrics fm(f);
    const QString label = QString::fromStdString(preview.name);
    const double w = std::min({row.width() - 8.0, row.width() * 0.45,
                               double(fm.horizontalAdvance(label)) + 26.0});
    if (w > 12.0) {
        const QRectF badge(row.left() + 4.0, row.top() + 4.0, w, 14.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(badge, 7, 7);
        p.setBrush(QColor(rec.red(), rec.green(), rec.blue(),
                          int(140 + 115 * recordPulse())));
        p.drawEllipse(QPointF(badge.left() + 9.0, badge.center().y()), 3.0, 3.0);
        p.setPen(QColor(255, 255, 255, 225));
        p.drawText(badge.adjusted(16, 0, -4, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, label);
    }

    // The write head, so the row reads as still being written.
    if (!preview.spans.empty()) {
        const double head =
            (preview.spans.back().endSeconds - m_scrollSeconds) * m_pixelsPerSecond;
        if (head >= row.left() && head <= row.right()) {
            p.setPen(QPen(QColor(rec.red(), rec.green(), rec.blue(), 220), 1.5));
            p.drawLine(QPointF(head, row.top() + 1), QPointF(head, row.bottom() - 1));
        }
    }
    p.restore();
}

void TimelineWidget::drawRecordingClip(QPainter& p,
                                       const daw::TrackModel& track,
                                       int top, int height) {
    const auto preview = m_controller->recordingPreview(track.id);
    if (!preview.active || preview.spans.empty()) return;

    const QColor body = colorFromRgb(preview.color);
    const QColor rec = Theme::record();
    const double pulse = recordPulse();

    if (height < 6) return;

    for (size_t i = 0; i < preview.spans.size(); ++i) {
        const auto& span = preview.spans[i];
        // The newest pass is the one being written; the ones before it are
        // finished loop passes and are drawn as settled material.
        const bool live = i + 1 == preview.spans.size();

        const double left = (span.startSeconds - m_scrollSeconds) * m_pixelsPerSecond;
        const double right = (span.endSeconds - m_scrollSeconds) * m_pixelsPerSecond;
        const QRectF r(left, top, std::max(2.0, right - left), height);
        if (r.right() < 0 || r.left() > width()) continue;

        // The clip body: exactly what a landed clip looks like, so the take
        // stops being a red stripe that turns into something else on stop.
        p.setBrush(body);
        p.setPen(QPen(mixColors(body, Qt::black, 0.45), 1.0));
        p.drawRoundedRect(r, 5, 5);

        const QRectF caption(r.left(), r.top(), r.width(), 14);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 60));
        QPainterPath capPath;
        capPath.addRoundedRect(caption, 5, 5);
        p.drawPath(capPath);

        const QRectF content(r.left(), caption.bottom(), r.width(),
                             r.bottom() - caption.bottom());
        drawRecordingEnvelope(p, preview, span, content, r,
                              QColor(255, 255, 255, 210));

        // Name, then the record dot in front of it — the one thing that says
        // "this is not finished yet" besides the pulsing rim below.
        QFont f = p.font();
        f.setPixelSize(10);
        p.setFont(f);
        const QRectF nameRect = caption.adjusted(6, 0, -4, 0);
        if (nameRect.width() > 24.0) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(rec.red(), rec.green(), rec.blue(),
                              live ? int(140 + 115 * pulse) : 200));
            p.drawEllipse(QPointF(nameRect.left() + 3.0, caption.center().y()),
                          3.0, 3.0);
            p.setPen(QColor(255, 255, 255, 220));
            p.drawText(nameRect.adjusted(10, 0, 0, 0),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QString::fromStdString(preview.name));
        }

        // The rim: a record-red outline that breathes, so a take in progress is
        // never mistaken for a clip that is already there — including when it is
        // being layered straight on top of one.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(rec.red(), rec.green(), rec.blue(),
                             live ? int(120 + 100 * pulse) : 110),
                      live ? 1.6 : 1.0));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);

        // The write head, where the take is being written right now.
        if (live && r.width() > 2.0) {
            p.setPen(QPen(QColor(rec.red(), rec.green(), rec.blue(), 235), 1.5));
            p.drawLine(QPointF(r.right(), r.top() + 2),
                       QPointF(r.right(), r.bottom() - 2));
        }
    }
}

void TimelineWidget::drawRecordingEnvelope(QPainter& p,
                                           const daw::RecordingPreview& preview,
                                           const daw::RecordingSpan& span,
                                           const QRectF& area,
                                           const QRectF& body,
                                           const QColor& color) {
    // Peaks are only computed once a file exists, and while the take is running
    // there is no file — so the shape comes from the input meter, bucketed by
    // the recorder's own clock. It is coarser than the waveform that replaces
    // it when the take lands, but it is the same signal, in the same place.
    const double step = preview.envelopeStepSeconds;
    if (preview.envelope.size() < 2 || step <= 0.0 || area.height() < 4.0) return;
    if (area.width() < 2.0) return;

    // Only the buckets this pass covers, and only the part of it on screen.
    const double spanLength = std::max(0.0, span.endSeconds - span.startSeconds);
    const double fromCapture =
        std::max(span.captureOffsetSeconds,
                 span.captureOffsetSeconds + (m_scrollSeconds - span.startSeconds));
    const double toCapture =
        std::min(span.captureOffsetSeconds + spanLength,
                 span.captureOffsetSeconds + (xToSeconds(width()) - span.startSeconds));
    if (toCapture <= fromCapture) return;

    const size_t last = preview.envelope.size() - 1;
    const size_t first = std::min(last, size_t(std::max(0.0, fromCapture / step)));
    const size_t stop = std::min(last, size_t(std::max(0.0, toCapture / step)));
    if (stop <= first) return;

    // One point per bucket while a bucket is at least a pixel wide, and the
    // loudest of the buckets a pixel covers once it is not — the same two
    // regimes a finished waveform is drawn in.
    const double pixelsPerBucket = step * m_pixelsPerSecond;
    const size_t stride =
        pixelsPerBucket >= 1.0 ? 1 : size_t(std::ceil(1.0 / std::max(0.001, pixelsPerBucket)));

    const double mid = area.center().y();
    const double half = std::max(2.0, area.height() / 2.0 - 1.0);
    QVector<QPointF> upper;
    QVector<QPointF> lower;
    upper.reserve(int((stop - first) / stride) + 2);
    lower.reserve(upper.capacity());
    for (size_t i = first; i <= stop; i += stride) {
        float peak = 0.0f;
        for (size_t k = i; k < std::min(stop + 1, i + stride); ++k)
            peak = std::max(peak, preview.envelope[k]);
        // Bucket i covers [i·step, (i+1)·step) of recorded time — an absolute
        // instant, not "the nth frame drawn". That is what keeps an already
        // drawn peak from creeping backwards as the take grows.
        const double captureTime = double(i) * step;
        const double timelineTime =
            span.startSeconds + (captureTime - span.captureOffsetSeconds);
        const double x = (timelineTime - m_scrollSeconds) * m_pixelsPerSecond;
        upper.append(QPointF(x, mid - double(peak) * half));
        lower.append(QPointF(x, mid + double(peak) * half));
    }
    if (upper.size() < 2) return;

    QPainterPath shape;
    shape.moveTo(upper.front());
    for (const QPointF& point : upper) shape.lineTo(point);
    for (auto it = lower.rbegin(); it != lower.rend(); ++it) shape.lineTo(*it);
    shape.closeSubpath();

    p.save();
    // Clipped to the clip's rounded outline, so the shape never spills out of
    // the take-to-be at either end.
    QPainterPath clip;
    clip.addRoundedRect(body, 5, 5);
    p.setClipPath(clip, Qt::IntersectClip);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(shape);
    p.restore();
}

void TimelineWidget::drawCountIn(QPainter& p) {
    const int beats = m_controller->countInBeatsRemaining();
    if (beats <= 0) return;

    // One pulse per beat: the number is drawn at full size as the beat lands
    // and settles over its length, so the count is felt as well as read.
    const double beatSeconds =
        60.0 / std::max(1.0, m_controller->tempo());
    const double intoBeat =
        1.0 - std::fmod(m_controller->countInRemainingSeconds(), beatSeconds) /
                  std::max(0.001, beatSeconds);
    const double swell = 1.0 - 0.18 * std::min(1.0, intoBeat * 2.0);

    const QRectF area(0, ui::kRulerHeight, width(),
                      height() - ui::kRulerHeight);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont f = font();
    f.setPixelSize(int(96 * swell));
    f.setBold(true);
    p.setFont(f);
    const QString text = QString::number(beats);
    const QColor rec = Theme::record();

    // A dark disc behind it, so the number is legible over clips as well as
    // over empty lanes.
    const double radius = 62.0 * swell;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 120));
    p.drawEllipse(area.center(), radius, radius);
    p.setPen(QPen(QColor(rec.red(), rec.green(), rec.blue(),
                         int(120 + 110 * (1.0 - intoBeat))),
                  2.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(area.center(), radius, radius);

    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(area, Qt::AlignCenter, text);
    p.restore();
}

void TimelineWidget::drawGainHandle(QPainter& p, const daw::ClipModel& clip,
                                    const QRectF& r, bool sel) {
    (void)clip;
    if (r.width() < 2 * kGainHandleR + 2) return;
    const QPointF c(r.center().x(), r.bottom());
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(255, 255, 255, sel ? 255 : 190), 1.0));
    p.setBrush(sel ? QColor(30, 30, 30, 210) : QColor(0, 0, 0, 60));
    p.drawEllipse(c, kGainHandleR, kGainHandleR);
}

void TimelineWidget::drawWaveform(QPainter& p, const daw::ClipModel& clip,
                                  const QRectF& area) {
    drawPeaks(p, m_controller->waveforms().cached(clip.filePath),
              clip.offsetSeconds, area, clip.gain, QColor(255, 255, 255),
              clip.sampleEdit.stretchTime);
}

/// The waveform envelope of `peaks` across `area`, where the source time at the
/// area's left edge is `sourceStartSeconds`. Shared by the clip bodies, the take
/// rows of an open comp editor and the comp lane, so a take drawn in a sub-lane
/// lines up sample-for-sample with the same audio drawn in the clip above it.
///
/// The drawing itself is `ui::paintPeaks`; this only supplies the zoom and the
/// viewport bound, which are the two things it needs from the timeline.
void TimelineWidget::drawPeaks(QPainter& p, const daw::WaveformPeaks* peaks,
                               double sourceStartSeconds, const QRectF& area,
                               float gain, const QColor& color,
                               double timeStretch) {
    // Source seconds per screen pixel. A stretched clip covers more timeline
    // per second of file, so each pixel steps through less of the source.
    const double stretch = timeStretch > 0.0 ? timeStretch : 1.0;
    ui::PeakPaint how;
    how.sourceStartSeconds = sourceStartSeconds;
    how.secondsPerPixel =
        m_pixelsPerSecond > 0.0 ? 1.0 / (m_pixelsPerSecond * stretch) : 0.0;
    const QRectF painted = area.intersected(p.clipBoundingRect());
    if (painted.isEmpty()) return;
    how.clipLeft = painted.left();
    how.clipRight = painted.right();
    how.gain = gain;
    how.color = color;
    ui::paintPeaks(p, peaks, area, how);
}

/// One take's audio inside `area`, which spans the clip's whole width. A take
/// covers only part of that width (`clipOffsetSeconds` in, `lengthSeconds`
/// long), so the material is drawn into that sub-rectangle and the rest of the
/// row is left as empty backing — the gap is information: it shows where this
/// take has nothing to offer.
const daw::MidiPreviewIndex& TimelineWidget::midiPreviewIndex(
    const std::string& clipId, const std::string& ownerId,
    std::span<const daw::NoteModel> notes,
    std::uint64_t midiNotesRevision) {
    // Clips and takes receive UUIDs from the same generator, so their ids form
    // stable keys. A malformed legacy owner without an id is still painted
    // correctly, but deliberately not retained because it has no stable key.
    if (clipId.empty() || ownerId.empty()) {
        static thread_local daw::MidiPreviewIndex transient;
        transient.rebuild(notes);
        return transient;
    }

    auto [clipCache, clipInserted] =
        m_midiPreviewCache.try_emplace(clipId);
    (void)clipInserted;
    auto [found, inserted] = clipCache->second.try_emplace(ownerId);
    MidiPreviewCacheEntry& entry = found->second;
    if (inserted) ++m_midiPreviewCachedOwners;
    ++m_midiPreviewUseCounter;
    if (m_midiPreviewUseCounter == 0) {
        // An overflow needs centuries of continuous 60 Hz painting, but keep
        // the LRU ordering defined even then.
        for (auto& [cachedClipId, owners] : m_midiPreviewCache) {
            (void)cachedClipId;
            for (auto& [cachedOwnerId, cached] : owners) {
                (void)cachedOwnerId;
                cached.lastUse = 0;
            }
        }
        m_midiPreviewUseCounter = 1;
    }
    entry.lastUse = m_midiPreviewUseCounter;

    if (inserted || entry.revision != midiNotesRevision ||
        entry.noteCount != notes.size()) {
        m_midiPreviewCachedNotes -= entry.noteCount;
        entry.index.rebuild(notes);
        entry.revision = midiNotesRevision;
        entry.noteCount = notes.size();
        m_midiPreviewCachedNotes += entry.noteCount;
        trimMidiPreviewCache(&entry);
    }
    return entry.index;
}

void TimelineWidget::trimMidiPreviewCache(
    const MidiPreviewCacheEntry* keepEntry) {
    while ((m_midiPreviewCachedNotes > kMidiPreviewCacheNoteBudget ||
            m_midiPreviewCachedOwners > kMidiPreviewCacheOwnerBudget) &&
           m_midiPreviewCachedOwners > 1) {
        auto oldestClip = m_midiPreviewCache.end();
        MidiPreviewOwners::iterator oldestOwner;
        for (auto clip = m_midiPreviewCache.begin();
             clip != m_midiPreviewCache.end(); ++clip) {
            for (auto owner = clip->second.begin();
                 owner != clip->second.end(); ++owner) {
                if (&owner->second == keepEntry) continue;
                if (oldestClip == m_midiPreviewCache.end() ||
                    owner->second.lastUse < oldestOwner->second.lastUse) {
                    oldestClip = clip;
                    oldestOwner = owner;
                }
            }
        }
        if (oldestClip == m_midiPreviewCache.end()) break;
        m_midiPreviewCachedNotes -= oldestOwner->second.noteCount;
        oldestClip->second.erase(oldestOwner);
        --m_midiPreviewCachedOwners;
        if (oldestClip->second.empty()) m_midiPreviewCache.erase(oldestClip);
    }
}

void TimelineWidget::drawTakeAudio(QPainter& p, const daw::ClipModel& clip,
                                   const daw::TakeModel& take,
                                   const QRectF& area, const QColor& color,
                                   std::uint64_t midiNotesRevision) {
    const double x0 = area.left() + take.clipOffsetSeconds * m_pixelsPerSecond;
    const double w = take.lengthSeconds * m_pixelsPerSecond;
    if (w < 1.0) return;
    const QRectF span(x0, area.top(), w, area.height());
    const QRectF vis = span.intersected(area);
    if (vis.isEmpty()) return;

    if (clip.kind == daw::ClipKind::Midi) {
        // No synth yet, so a MIDI take has notes but never a file: draw the
        // pitch rectangles the same way the clip body does.
        if (take.notes.empty()) return;
        const daw::MidiPreviewIndex& index =
            midiPreviewIndex(clip.id, take.id, take.notes, midiNotesRevision);
        const double tempo = std::max(1.0, m_controller->project().tempo);
        const double pxPerBeat = (60.0 / tempo) * m_pixelsPerSecond;
        if (!(pxPerBeat > 0.0)) return;

        // clipOffsetSeconds is already represented by span.left(). Translate
        // the painter's horizontal dirty window back into take-relative beats.
        const QRectF painted = vis.intersected(p.clipBoundingRect());
        if (painted.isEmpty()) return;
        const double minNoteWidth = std::max(1.5, 0.02 * m_pixelsPerSecond);
        const double fromBeat =
            (painted.left() - span.left() - minNoteWidth) / pxPerBeat;
        const double toBeat =
            (painted.right() - span.left() + 1.0) / pxPerBeat;

        const double lowest = double(index.lowestPitch());
        const double highest = double(index.highestPitch());
        const double range = std::max(4.0, highest - lowest);
        const QRegion paintRegion = p.clipRegion();
        QPainterPath path;
        bool any = false;
        index.forEachVisible(
            take.notes, fromBeat, toBeat,
            [&](const daw::NoteModel& note, std::size_t) {
                const double x = span.left() + note.startBeats * pxPerBeat;
                const double noteWidth =
                    std::max(minNoteWidth, note.lengthBeats * pxPerBeat);
                const double y =
                    vis.bottom() -
                    ((double(note.pitch) - lowest) / range) *
                        (vis.height() - 3.0) -
                    3.0;
                const QRectF noteRect(x, y, noteWidth, 2.0);
                if (!paintRegion.intersects(
                        noteRect.toAlignedRect().adjusted(-1, -1, 1, 1))) {
                    return;
                }
                path.addRect(noteRect);
                any = true;
            });
        if (!any) return;

        p.save();
        p.setClipRect(vis, Qt::IntersectClip);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(path);
        p.restore();
        return;
    }

    p.save();
    p.setClipRect(vis, Qt::IntersectClip);
    drawPeaks(p, m_controller->waveforms().cached(take.filePath),
              take.offsetSeconds, span, take.gain, color);
    p.restore();
}

void TimelineWidget::drawMidiNotes(QPainter& p, const daw::ClipModel& clip,
                                   const QRectF& area, const QRectF& body,
                                   std::uint64_t midiNotesRevision) {
    if (area.height() < 6.0 || area.width() < 2.0) return;
    if (clip.notes.empty()) return;   // an empty clip is just its coloured box

    const daw::MidiPreviewIndex& index =
        midiPreviewIndex(clip.id, clip.id, clip.notes, midiNotesRevision);

    // Auto-fit the pitch axis to what the clip actually contains. A fixed
    // C1–C7 would squash a bass line into a two-pixel smear at the bottom of
    // the lane; fitting is what makes a note block readable at lane height.
    // kMinSpan keeps a one- or two-note clip from filling the lane edge to edge.
    constexpr int kMinSpan = 12;             // one octave
    const int lowest = index.lowestPitch();
    const int highest = index.highestPitch();
    const int used = highest - lowest + 1;
    const int span = std::max(used, kMinSpan);
    const int base = lowest - (span - used) / 2;   // centre the used range

    const double rowHeight = area.height() / double(span);
    // A gap between rows so neighbouring semitones read apart — but a
    // proportional one: subtracting a flat pixel from an already-thin row is
    // most of its height, and the notes thin out to invisible hairlines.
    const double gap = std::min(1.0, rowHeight * 0.2);
    const double noteHeight = std::max(2.0, rowHeight - gap);

    // Hoisted: the tempo lookup and the beat scale are the same for every note.
    const double tempo = std::max(1.0, m_controller->project().tempo);
    const double pxPerBeat =
        daw::beatsToSeconds(1.0, tempo) * m_pixelsPerSecond;
    if (pxPerBeat <= 0.0) return;

    // Convert the painter's dirty x range into clip-relative beats. Expanding
    // the left edge by the minimum on-screen width keeps a very short note
    // visible when its musical end lies just outside the repaint strip.
    const QRectF painted =
        area.intersected(body).intersected(p.clipBoundingRect());
    if (painted.isEmpty()) return;
    constexpr double kMinNoteWidth = 2.0;
    const double fromBeat =
        (painted.left() - area.left() - kMinNoteWidth) / pxPerBeat;
    const double toBeat =
        (painted.right() - area.left() + 1.0) / pxPerBeat;

    QPainterPath path;
    bool any = false;
    const QRegion paintRegion = p.clipRegion();
    index.forEachVisible(
        clip.notes, fromBeat, toBeat,
        [&](const daw::NoteModel& note, std::size_t) {
            const double x = area.left() + note.startBeats * pxPerBeat;
            const double noteWidth =
                std::max(kMinNoteWidth, note.lengthBeats * pxPerBeat);
            const double y =
                area.bottom() - double(note.pitch - base + 1) * rowHeight;
            const double radius =
                std::min({2.0, noteHeight * 0.4, noteWidth * 0.4});
            const QRectF noteRect(x, y, noteWidth, noteHeight);
            if (!paintRegion.intersects(
                    noteRect.toAlignedRect().adjusted(-1, -1, 1, 1))) {
                return;
            }
            path.addRoundedRect(noteRect, radius, radius);
            any = true;
        });
    if (!any) return;

    // Clipped to the clip's own rounded body: a tempo change moves the notes
    // but not the clip's length, so without this they would spill past its
    // right edge — and a note on the lowest row would poke out of the rounded
    // bottom corners.
    QPainterPath bodyClip;
    bodyClip.addRoundedRect(body, 5, 5);
    p.save();
    p.setClipPath(bodyClip, Qt::IntersectClip);
    p.setClipRect(area, Qt::IntersectClip);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255));
    p.drawPath(path);          // one fill for the whole clip, not one per note
    p.restore();
}

void TimelineWidget::drawPlayhead(QPainter& p) {
    const Theme& t = th();
    const int x = secondsToX(m_controller->presentationPositionSeconds());
    if (x < -6 || x > width() + 6) return;

    p.setPen(QPen(t.cursor, 1.6));
    p.drawLine(x, 0, x, height());

    // Ruler handle so the playhead is grabbable and visible at a glance.
    p.setPen(Qt::NoPen);
    p.setBrush(t.cursor);
    p.drawPath(roundedTriangle(QPointF(x, 12.0), QPointF(x - 7.0, 0.0),
                               QPointF(x + 7.0, 0.0), 3.0));
}

void TimelineWidget::setRightCornerRadius(int radius) {
    const int value = std::max(0, radius);
    if (value == m_rightRadius) return;
    m_rightRadius = value;
    m_staticFrameValid = false;
    update();
}

namespace {
/// The widget's rectangle with only its right-hand corners rounded — the shape
/// the arrangement takes when a panel sits beside it. Built by hand rather than
/// with `addRoundedRect`, which rounds all four.
QPainterPath rightRoundedShape(const QRectF& r, double radius) {
    QPainterPath path;
    path.moveTo(r.left(), r.top());
    path.lineTo(r.right() - radius, r.top());
    path.quadTo(r.right(), r.top(), r.right(), r.top() + radius);
    path.lineTo(r.right(), r.bottom() - radius);
    path.quadTo(r.right(), r.bottom(), r.right() - radius, r.bottom());
    path.lineTo(r.left(), r.bottom());
    path.closeSubpath();
    return path;
}
}  // namespace

void TimelineWidget::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    m_staticFrameValid = false;
    m_playbackOnlyDirty = {};
    // A taller arrangement can show more lanes, which may make the current
    // scroll position illegal.
    clampVerticalScroll();
}

void TimelineWidget::drawStaticFrame(QPainter& p,
                                     const QRegion& paintRegion) {
    ++m_staticFramePaintCount;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setClipRegion(paintRegion, Qt::IntersectClip);

    // Everything is drawn inside the arrangement's own shape, so a clip, a lane
    // fill or the playhead cannot spill past a rounded corner.
    if (m_rightRadius > 0) {
        const QPainterPath shape =
            rightRoundedShape(QRectF(rect()), std::min<double>(
                                                  m_rightRadius, height() / 2.0));
        p.setClipPath(shape, Qt::IntersectClip);
        p.fillPath(shape, th().background);
    } else {
        p.fillRect(rect(), th().background);
    }

    // The mixer is an opaque overlay over the lower part of this widget. Do
    // not traverse or render lanes that cannot be seen beneath it.
    const int laneViewportBottom = std::max(0, height() - m_bottomInset);
    const QRegion laneRegion =
        paintRegion.intersected(QRect(0, 0, width(), laneViewportBottom));
    if (!laneRegion.isEmpty()) {
        p.save();
        p.setClipRegion(laneRegion, Qt::IntersectClip);
        drawLanes(p);
        p.restore();
    }

    drawRegion(p);

    // Drop target highlight for an external file drag.
    if (m_dropActive) {
        const Theme& t = th();
        if (m_dropLane >= 0) {
            const int y = laneTop(m_dropLane);
            const int h = laneHeightAt(m_dropLane);
            p.fillRect(0, y, width(), h,
                       mixColors(t.accent, t.background, 0.75));
            p.setPen(QPen(t.accent, 1.4));
            p.drawRect(0, y, width() - 1, h - 1);
        } else {
            // Below every lane: a new track would be created here.
            const int y = lanesBottom();
            p.setPen(QPen(t.accent, 2, Qt::DashLine));
            p.drawLine(0, y, width(), y);
        }
    }

    // Rubber-band selection rectangle.
    if (m_marqueeActive) {
        const Theme& t = th();
        const QRect box = QRect(m_marqueeOrigin, m_marqueeCurrent).normalized();
        p.setPen(QPen(t.accent, 1.0, Qt::DashLine));
        p.setBrush(QColor(t.accent.red(), t.accent.green(), t.accent.blue(), 40));
        p.drawRect(box);
    }

    drawRuler(p);
    p.restore();
}

void TimelineWidget::paintEvent(QPaintEvent* event) {
    const QRegion paintRegion = event->region();
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize(std::max(1, int(std::ceil(width() * dpr))),
                          std::max(1, int(std::ceil(height() * dpr))));
    const bool cacheSizeChanged =
        m_staticFrame.size() != pixelSize ||
        std::abs(m_staticFrame.devicePixelRatioF() - dpr) > 1e-6;
    if (cacheSizeChanged) {
        m_staticFrame = QPixmap(pixelSize);
        m_staticFrame.setDevicePixelRatio(dpr);
        m_staticFrame.fill(Qt::transparent);
        m_staticFrameValid = false;
    }

    // `refreshPlaybackFrame` is the sole narrow-update caller in this widget.
    // When the event contains only those requested strips, the static layer is
    // already exact: copying it erases the old cursor in constant time. A full
    // update (edit, scroll, resize, theme, recording preview...) repaints the
    // cache first and therefore cannot leave stale content behind.
    const bool playbackOnly =
        m_staticFrameValid && !m_playbackOnlyDirty.isEmpty() &&
        paintRegion.subtracted(m_playbackOnlyDirty).isEmpty();
    if (!playbackOnly) {
        const QRegion staticDirty =
            m_staticFrameValid ? paintRegion : QRegion(rect());
        QPainter cachePainter(&m_staticFrame);
        cachePainter.setClipRegion(staticDirty);
        cachePainter.setCompositionMode(QPainter::CompositionMode_Source);
        cachePainter.fillRect(rect(), Qt::transparent);
        cachePainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        drawStaticFrame(cachePainter, staticDirty);
        m_staticFrameValid = true;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setClipRegion(paintRegion, Qt::IntersectClip);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawPixmap(QPoint(0, 0), m_staticFrame);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // The cached plate already has the rounded edge, and the dynamic overlays
    // must obey the same boundary.
    if (m_rightRadius > 0) {
        const QPainterPath shape = rightRoundedShape(
            QRectF(rect()),
            std::min<double>(m_rightRadius, height() / 2.0));
        p.setClipPath(shape, Qt::IntersectClip);
    }

    drawPlayhead(p);
    const int currentPlayheadX =
        secondsToX(m_controller->presentationPositionSeconds());
    if (paintRegion.intersects(playheadDirtyRect(currentPlayheadX)))
        m_lastPlayheadX = currentPlayheadX;
    // Last, over everything: during a count-in the number is the only thing
    // that matters on this screen.
    drawCountIn(p);
    m_playbackOnlyDirty = m_playbackOnlyDirty.subtracted(paintRegion);
}

// ── Interaction ────────────────────────────────────────────────────────────

void TimelineWidget::updateCursor(const QPoint& pos) {
    if (m_dragging) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_trimming || m_fading) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    if (m_fadeCurving) {
        setCursor(Qt::SizeVerCursor);
        return;
    }
    if (pos.y() < ui::kLoopStripHeight) {
        setCursor(loopGrabAt(pos.x()) == LoopGrab::Move ? Qt::OpenHandCursor
                                                        : Qt::SizeHorCursor);
        return;
    }
    if (pos.y() < ui::kRulerHeight) {
        setCursor(Qt::SizeHorCursor);   // scrub
        return;
    }
    if (m_regionMovePending || m_regionMoving) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (regionContains(pos)) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (m_swiping) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    if (m_expandDrag) {
        setCursor(Qt::SplitVCursor);
        return;
    }
    // Inside an open comp editor the brush is always the tool, so the cursor
    // says so before the user presses anything.
    TakeHit take;
    if (hitTestTake(pos, take)) {
        setCursor(m_auditionHeld ? Qt::WhatsThisCursor : Qt::PointingHandCursor);
        return;
    }
    ClipHit hit;
    const bool over = hitTestClip(pos, hit);
    switch (tool()) {
        case Tool::Knife:
            setCursor(arrangementToolCursor(icons::Glyph::Knife));
            return;
        case Tool::Eraser:
            setCursor(arrangementToolCursor(icons::Glyph::Eraser));
            return;
        case Tool::Mute:
            setCursor(arrangementToolCursor(icons::Glyph::Power));
            return;
        case Tool::Draw:
            setCursor(arrangementToolCursor(icons::Glyph::Brush));
            return;
        case Tool::Select:
        default:
            if (over && !hit.gainHandle) {
                // The bottom edge of a layered clip is the comp editor's pull
                // handle, so it gets its own cursor before anything else claims
                // the corner it shares with the trim edges.
                if (const daw::ClipModel* clip =
                        findClipModel(hit.trackId, hit.clipId)) {
                    const QRectF body = clipRect(laneAt(pos.y()), *clip);
                    if (daw::isLayered(*clip) && !body.isEmpty() &&
                        pos.y() >= body.bottom() - 4.0) {
                        setCursor(Qt::SplitVCursor);
                        return;
                    }
                }
            }
            if (over && hit.fadeCurve != Fade::None) setCursor(Qt::SizeVerCursor);
            else if (over && hit.fade != Fade::None) setCursor(Qt::SizeHorCursor);
            else if (over && hit.gainHandle) setCursor(Qt::SizeVerCursor);
            else if (over && hit.edge != Edge::None) setCursor(Qt::SizeHorCursor);
            else setCursor(over ? Qt::OpenHandCursor
                                : arrangementToolCursor(icons::Glyph::Pointer));
            return;
        case Tool::SelectRegion:
            setCursor(arrangementToolCursor(icons::Glyph::Crosshair));
            return;   // the strip is handled above, before any tool sees it
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* ev) {
    setFocus(Qt::MouseFocusReason);
    if (ev->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panLastPosition = ev->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }
    if (ev->button() != Qt::LeftButton) return;
    // Read the modifier before anything decides what this click means: the
    // gesture that follows belongs to whichever tool is in force now.
    trackAltTool(ev->modifiers());
    const QPoint pos = ev->position().toPoint();

    // The cycle strip sits above the bar numbers and owns its own band of the
    // ruler: dragging there defines the region the playhead goes round, and
    // the rest of the ruler still scrubs.
    if (pos.y() < ui::kLoopStripHeight) {
        // A double-click on the region switches the cycle on and off — the
        // mouse's way of doing what C does, without leaving the ruler after
        // marking a region out. The same gesture the piano roll's ruler has,
        // because it is the same region.
        if (ev->type() == QEvent::MouseButtonDblClick &&
            loopGrabAt(pos.x()) == LoopGrab::Move) {
            m_loopGrab = LoopGrab::None;
            m_controller->setLoopEnabled(!m_controller->isLoopEnabled());
            emit loopRangeChanged();
            update();
            ev->accept();
            return;
        }

        const bool snapOn = m_snapEnabled && !(ev->modifiers() & Qt::AltModifier);
        m_loopGrab = loopGrabAt(pos.x());
        const double at = std::max(0.0, snap(xToSeconds(pos.x()), snapOn));
        switch (m_loopGrab) {
            case LoopGrab::Create:
                // The press is one edge; the drag decides which one it was.
                m_loopAnchorSeconds = at;
                m_controller->setLoopRangeSeconds(at, at);
                break;
            case LoopGrab::Move:
                m_loopGrabOffset = at - m_controller->loopStartSeconds();
                m_loopGrabLength = m_controller->loopEndSeconds() -
                                   m_controller->loopStartSeconds();
                break;
            case LoopGrab::ResizeStart:
                m_loopAnchorSeconds = m_controller->loopEndSeconds();
                break;
            case LoopGrab::ResizeEnd:
                m_loopAnchorSeconds = m_controller->loopStartSeconds();
                break;
            case LoopGrab::None:
                break;
        }
        setCursor(m_loopGrab == LoopGrab::Move ? Qt::ClosedHandCursor
                                               : Qt::SizeHorCursor);
        update();
        return;
    }

    if (pos.y() < ui::kRulerHeight) {
        m_scrubbing = true;
        m_controller->seekSeconds(std::max(0.0, xToSeconds(pos.x())));
        emit playheadMoved();
        setCursor(Qt::SizeHorCursor);
        update();
        return;
    }

    // Once committed, a region stays a first-class selection even after the
    // user picks another tool. Grabbing inside it always moves its contents;
    // the Region tool is only needed to draw the rectangle in the first place.
    if (m_regionActive && regionContains(pos)) {
        m_lastRegionStart = m_regionStart;
        m_lastRegionEnd = m_regionEnd;
        m_lastRegionLaneA = m_regionLaneA;
        m_lastRegionLaneB = m_regionLaneB;
        m_regionActive = false;
        m_regionMovePending = true;
        m_regionMoving = false;
        m_regionPressPos = pos;
        m_regionMoveGrab = xToSeconds(pos.x());
        m_regionMoveOrigStart = m_regionStart;
        m_regionMoveOrigEnd = m_regionEnd;
        m_regionPieces.clear();
        m_selection.clear();
        m_selectedClipId.clear();
        setCursor(Qt::ClosedHandCursor);
        update();
        return;
    }

    ClipHit hit;
    const bool over = hitTestClip(pos, hit);
    const bool snapOn = m_snapEnabled && !(ev->modifiers() & Qt::AltModifier);

    // Double-click opens a curve's editor — but only where the pointer is not
    // already editing the curve: on the grip strip, or under a tool that treats
    // the clip as a clip. Anywhere else a double-click is two clicks on the
    // curve, and two clicks on a curve place points.
    //
    // Ahead of every clip-kind branch further down, so a curve never opens the
    // sample editor.
    if (ev->type() == QEvent::MouseButtonDblClick && over &&
        hit.kind == daw::ClipKind::Automation) {
        const daw::ClipModel* clip = findClipModel(hit.trackId, hit.clipId);
        const bool onGrip =
            clip && automationGrip(clipRect(laneAt(pos.y()), *clip))
                        .contains(QPointF(pos));
        const bool editingCurve = tool() == Tool::Select || tool() == Tool::Draw;
        if (onGrip || !editingCurve) {
            selectAutomationClip(hit.trackId, hit.clipId);
            emit openAutomationEditorRequested(hit.trackId, hit.clipId);
            update();
            ev->accept();
            return;
        }
    }

    // On a breakpoint, double-click has the same meaning as on the control it
    // drives: return to that target's neutral/factory position. It is handled
    // before the ordinary point press so the gesture cannot accidentally move
    // or add a handle on its second click.
    if (ev->type() == QEvent::MouseButtonDblClick && hit.edge == Edge::None) {
        PointHit point;
        if (hitTestAutomationPoint(pos, point) && point.index >= 0) {
            const daw::ClipModel* clip =
                findClipModel(point.trackId, point.clipId);
            if (clip && std::size_t(point.index) < clip->automation.points.size()) {
                std::vector<daw::AutomationPoint> points = clip->automation.points;
                const std::vector<daw::AutomationPoint> before = points;
                points[std::size_t(point.index)].value =
                    m_controller->automationResetValue(clip->automation.target);
                m_controller->setAutomationPoints(point.trackId.toStdString(),
                                                  point.clipId.toStdString(), points);
                m_controller->commitAutomationEdit(
                    point.trackId.toStdString(), point.clipId.toStdString(), before,
                    "Reset Automation Point");
                selectAutomationClip(point.trackId, point.clipId);
                emit projectEdited();
                update();
                ev->accept();
                return;
            }
        }
    }

    // ── Curve editing ──
    //
    // Ahead of every tool branch, because an automation lane holds nothing but
    // curves: on one, the pointer is always editing a curve and never cutting,
    // erasing or muting a clip. The clip itself is still grabbed by its edges
    // and its name strip, which `hitTestClip` reports as before.
    if (PointHit point; hit.edge == Edge::None &&
                        hitTestAutomationPoint(pos, point)) {
        const daw::ClipModel* clip =
            findClipModel(point.trackId, point.clipId);
        if (clip) {
            m_pointsBefore = clip->automation.points;
            m_pointDrag = point;
            selectAutomationClip(point.trackId, point.clipId);

            // Alt over a run bends it. The same modifier that turns snapping
            // off elsewhere, and for the same reason: it is the "shape this
            // freely" key.
            if ((ev->modifiers() & Qt::AltModifier) && point.index < 0 &&
                point.segment >= 0) {
                m_bendingSegment = true;
                m_bendStartY = pos.y();
                m_bendStartCurve =
                    clip->automation.points[std::size_t(point.segment)].curve;
                setCursor(Qt::SizeVerCursor);
                ev->accept();
                return;
            }

            std::vector<daw::AutomationPoint> points = clip->automation.points;

            // The Draw tool paints a curve freehand: the stroke lays points
            // down as the hand moves, replacing whatever it crosses. Point by
            // point is the Select tool's job.
            if (tool() == Tool::Draw) {
                m_drawingCurve = true;
                paintCurveAt(pos, snapOn);
                setCursor(Qt::CrossCursor);
                ev->accept();
                return;
            }

            if (point.index < 0) {
                // Empty space: one gesture both creates the breakpoint and
                // places it, the way the piano roll's controller lane does.
                daw::AutomationPoint added;
                added.beats = std::max(0.0, snapBeats(point.beats, snapOn));
                added.value = point.value;
                // A new point inherits the shape of the run it lands in, so
                // splitting a curved segment does not straighten it.
                if (point.segment >= 0)
                    added.shape = points[std::size_t(point.segment)].shape;
                points.push_back(added);
                daw::normalizeAutomation(points);
                m_controller->setAutomationPoints(point.trackId.toStdString(),
                                                  point.clipId.toStdString(),
                                                  points);
                const daw::ClipModel* after =
                    findClipModel(point.trackId, point.clipId);
                m_pointDrag.index = after ? indexOfPointAt(*after, added.beats) : -1;
            }
            m_dragPoints = points;
            m_draggingPoint = m_pointDrag.index >= 0;
            setCursor((ev->modifiers() & Qt::ShiftModifier)
                          ? Qt::SizeHorCursor
                          : Qt::SizeAllCursor);
            update();
            ev->accept();
            return;
        }
    }

    // The take badge on a layered clip: its arrow opens and closes the editor.
    // Checked before every tool branch so the Knife and Eraser can't split or
    // delete a clip the user was only trying to open.
    if (over && !hit.clipId.isEmpty()) {
        if (const daw::ClipModel* clip = findClipModel(hit.trackId, hit.clipId)) {
            if (clip->takes.size() > 1 &&
                badgeRect(clipRect(laneAt(pos.y()), *clip)).contains(QPointF(pos))) {
                toggleClipExpanded(hit.trackId, hit.clipId);
                return;
            }
        }
    }

    // The third way to open the editor: pull the clip's bottom edge down. Armed
    // here and acted on in the move handler, because a press on the edge that
    // never moves should still just select the clip.
    if (over && !hit.gainHandle && ev->type() != QEvent::MouseButtonDblClick) {
        if (const daw::ClipModel* clip = findClipModel(hit.trackId, hit.clipId)) {
            const QRectF body = clipRect(laneAt(pos.y()), *clip);
            if (daw::isLayered(*clip) && !body.isEmpty() &&
                pos.y() >= body.bottom() - 4.0) {
                m_expandDrag = true;
                m_expandTrackId = hit.trackId;
                m_expandClipId = hit.clipId;
                m_expandPressY = pos.y();
                setCursor(Qt::SplitVCursor);
                return;
            }
        }
    }

    // The comp editor's take rows. They sit below the clip bodies, so a hit here
    // means the pointer is in the editor and none of the clip-editing branches
    // below should see this press at all.
    TakeHit take;
    if (hitTestTake(pos, take)) {
        // Hold-A: click a row to hear that layer alone, no edit.
        if (m_auditionHeld) {
            m_auditionTakeId = take.takeId;
            m_controller->setSoloTake(take.trackId.toStdString(),
                                      take.clipId.toStdString(),
                                      take.takeId.toStdString());
            update();
            return;
        }

        // Double-click promotes the whole layer: "this attempt, all of it",
        // without swiping end to end. It is the coarse gesture the fine one
        // (the swipe below) is a refinement of, so both live on the row itself
        // and there is nothing else in the editor to aim at.
        if (ev->type() == QEvent::MouseButtonDblClick) {
            m_controller->selectTake(take.trackId.toStdString(),
                                     take.clipId.toStdString(),
                                     take.takeId.toStdString());
            emit projectEdited();
            update();
            return;
        }

        // Over the audio, this is a swipe: the comp tool is always live inside
        // the editor, so a press starts painting immediately. The whole stroke
        // is one undo step.
        const daw::ClipModel* clip = findClipModel(take.trackId, take.clipId);
        if (!clip) return;
        const bool snapOn = m_snapEnabled && !(ev->modifiers() & Qt::AltModifier);
        m_swiping = true;
        m_swipeTrackId = take.trackId;
        m_swipeClipId = take.clipId;
        m_swipeTakeId = take.takeId;
        m_swipeFromSeconds =
            std::clamp(snap(xToSeconds(pos.x()), snapOn) - clip->startSeconds,
                       0.0, clip->durationSeconds);
        m_swipeToSeconds = m_swipeFromSeconds;
        m_controller->beginCompEdit(take.trackId.toStdString(),
                                    take.clipId.toStdString());
        updateSwipe(pos, snapOn);
        setCursor(Qt::PointingHandCursor);
        return;
    }

    // Double-clicking the gain handle resets the clip(s) back to unity gain —
    // the neutral, "no change" value (1.0, i.e. zero in dB). Like the drag, it
    // acts on every selected clip.
    if (ev->type() == QEvent::MouseButtonDblClick && tool() == Tool::Select &&
        over && hit.gainHandle) {
        if (!isClipSelected(hit.clipId)) {
            m_selection = {ClipRef{hit.trackId, hit.clipId}};
        }
        m_selectedClipId = hit.clipId;
        beginProjectGesture(tr("Reset Clip Gain"),
                            ProjectGestureKind::ClipGain);
        m_gainOrig.clear();
        for (const auto& ref : m_selection) {
            if (const auto* clip = m_controller->audioClip(
                    ref.trackId.toStdString(), ref.clipId.toStdString());
                clip) {
                m_gainOrig.push_back(
                    {ref.trackId, ref.clipId, clip->gain, clip->pan});
                if (clip->gain != 1.0f) markProjectGestureChanged();
            }
            m_controller->setClipGain(ref.trackId.toStdString(),
                                      ref.clipId.toStdString(), 1.0f);
        }
        finishProjectGesture();
        m_gainOrig.clear();
        emit projectEdited();
        emit clipSelected(hit.trackId, hit.clipId);
        update();
        return;
    }

    // Double-click opens the editor that matches the clip kind. Audio and the
    // instrument Sampler share the Sample/Clip editor surface; MIDI keeps the
    // piano roll. Empty MIDI lane space still creates a one-bar clip.
    //
    // This sits after the gain-handle branch above (which is audio-only anyway,
    // since hitTestClip reports no gain handle for a MIDI clip) and before the
    // tool branches, so Knife and Eraser keep behaving exactly as they do now.
    if (ev->type() == QEvent::MouseButtonDblClick && tool() == Tool::Select) {
        if (over && hit.kind == daw::ClipKind::Midi) {
            m_selection = {ClipRef{hit.trackId, hit.clipId}};
            m_selectedClipId = hit.clipId;
            emit clipSelected(hit.trackId, hit.clipId);
            emit openPianoRollRequested(hit.trackId, hit.clipId);
            update();
            return;
        }
        if (over && hit.kind == daw::ClipKind::Audio) {
            m_selection = {ClipRef{hit.trackId, hit.clipId}};
            m_selectedClipId = hit.clipId;
            emit clipSelected(hit.trackId, hit.clipId);
            emit openSampleEditorRequested(hit.trackId, hit.clipId);
            update();
            return;
        }
        if (over && hit.kind == daw::ClipKind::Pattern) {
            m_selection = {ClipRef{hit.trackId, hit.clipId}};
            m_selectedClipId = hit.clipId;
            emit clipSelected(hit.trackId, hit.clipId);
            emit openPatternRequested(hit.trackId);
            update();
            return;
        }
        if (!over) {
            const int lane = laneAt(pos.y());
            const QString trackId = lane >= 0 ? trackIdForLane(lane) : QString();
            const auto* track =
                trackId.isEmpty() ? nullptr
                                  : m_controller->project().findTrack(
                                        trackId.toStdString());
            if (track && track->kind == daw::TrackKind::Pattern) {
                emit openPatternRequested(trackId);
                return;
            }
            if (track && daw::trackAccepts(track->kind, daw::ClipKind::Midi)) {
                const std::string clipId = m_controller->addMidiClip(
                    trackId.toStdString(),
                    std::max(0.0, snap(xToSeconds(pos.x()), snapOn)));
                if (!clipId.empty()) {
                    const QString newClip = QString::fromStdString(clipId);
                    m_selection = {ClipRef{trackId, newClip}};
                    m_selectedClipId = newClip;
                    m_selectedTrackId = trackId;
                    emit projectEdited();
                    emit clipSelected(trackId, newClip);
                    emit openPianoRollRequested(trackId, newClip);
                    update();
                    return;
                }
            }
        }
    }

    // Double-click inside a region just picked with the Region tool: fill it
    // with a MIDI clip of exactly that length, on every MIDI lane it covers.
    //
    // The region itself is gone by now — the release that preceded this event
    // cleared it — so this works off the geometry remembered when the pointer
    // went down inside it.
    if (ev->type() == QEvent::MouseButtonDblClick &&
        m_lastRegionLaneA >= 0 &&
        m_lastRegionEnd - m_lastRegionStart > 0.0) {
        const int lane = laneAt(pos.y());
        const bool inside = pos.x() >= secondsToX(m_lastRegionStart) &&
                            pos.x() <= secondsToX(m_lastRegionEnd) &&
                            lane >= m_lastRegionLaneA && lane <= m_lastRegionLaneB;
        // One shot: consumed whether or not it lands, so a later stray
        // double-click can't resurrect a region the user has moved on from.
        const double start = m_lastRegionStart;
        const double length = m_lastRegionEnd - m_lastRegionStart;
        const int laneA = m_lastRegionLaneA;
        const int laneB = m_lastRegionLaneB;
        m_lastRegionLaneA = -1;

        if (inside) {
            QString firstTrack;
            QString firstClip;
            for (int l = laneA; l <= laneB; ++l) {
                const QString trackId = trackIdForLane(l);
                if (trackId.isEmpty()) continue;
                const auto* track =
                    m_controller->project().findTrack(trackId.toStdString());
                if (!track || !daw::trackAccepts(track->kind, daw::ClipKind::Midi))
                    continue;
                const std::string clipId = m_controller->addMidiClip(
                    trackId.toStdString(), start, length);
                if (clipId.empty()) continue;
                if (firstClip.isEmpty()) {
                    firstTrack = trackId;
                    firstClip = QString::fromStdString(clipId);
                }
            }
            if (!firstClip.isEmpty()) {
                m_selection = {ClipRef{firstTrack, firstClip}};
                m_selectedClipId = firstClip;
                m_selectedTrackId = firstTrack;
                clearRegion();
                emit projectEdited();
                emit clipSelected(firstTrack, firstClip);
                emit openPianoRollRequested(firstTrack, firstClip);
                update();
                return;
            }
        }
    }

    // SelectRegion draws a new time selection. Presses inside an existing one
    // were already handled above, before any selected tool could steal it.
    if (tool() == Tool::SelectRegion) {
        // New region: drop any old one and start picking. Forgetting the
        // remembered geometry here is what stops a double-click on empty space
        // from filling a region the user abandoned earlier.
        m_lastRegionLaneA = -1;
        clearRegion();
        m_regionPicking = true;
        m_regionOrigin = pos;
        m_regionCurrent = pos;
        m_selection.clear();
        m_selectedClipId.clear();
        update();
        return;
    }

    // Knife: cut the clip under the pointer. With several clips selected, the
    // knife makes one vertical cut — every selected clip is split at the same
    // timeline position.
    if (tool() == Tool::Knife) {
        if (over) {
            const double at = snap(xToSeconds(pos.x()), snapOn);
            const auto undoGroup = m_controller->beginUndoGroup();
            bool cut = false;
            if (m_selection.size() > 1 && isClipSelected(hit.clipId)) {
                for (const auto& ref : m_selection) {
                    if (!m_controller
                             ->splitClip(ref.trackId.toStdString(),
                                         ref.clipId.toStdString(), at)
                             .empty()) {
                        cut = true;
                    }
                }
            } else {
                cut = !m_controller
                           ->splitClip(hit.trackId.toStdString(),
                                       hit.clipId.toStdString(), at)
                           .empty();
            }
            if (cut) {
                m_controller->collapseUndo(undoGroup, "Split Clips");
                emit projectEdited();
                update();
            } else m_controller->releaseUndoGroup(undoGroup);
        }
        return;
    }

    // Eraser: delete the clip under the pointer (drag to delete more).
    if (tool() == Tool::Eraser) {
        beginProjectGesture(tr("Erase Clips"));
        m_erasing = true;
        if (over) {
            m_controller->removeClip(hit.trackId.toStdString(),
                                     hit.clipId.toStdString());
            markProjectGestureChanged();
            m_selection.removeIf(
                [&](const ClipRef& r) { return r.clipId == hit.clipId; });
            if (m_selectedClipId == hit.clipId) m_selectedClipId.clear();
            emit projectEdited();
            update();
        }
        return;
    }

    // Mute tool: a single click switches the clip without changing its place.
    // Clicking one member of a selection applies the same state to the group.
    if (tool() == Tool::Mute) {
        if (!over) return;
        if (!isClipSelected(hit.clipId)) {
            m_selection = {ClipRef{hit.trackId, hit.clipId}};
            m_selectedClipId = hit.clipId;
            publishSelection();
        }
        toggleSelectedClipsMuted();
        emit clipSelected(hit.trackId, hit.clipId);
        return;
    }

    // Select tool. Fade curve → fade corner → trim edge → body move.
    if (over && hit.fadeCurve != Fade::None) {
        m_selection = {ClipRef{hit.trackId, hit.clipId}};
        m_selectedClipId = hit.clipId;
        beginProjectGesture(tr("Shape Clip Fade"),
                            ProjectGestureKind::ClipFadeCurve);
        m_fadeCurving = true;
        m_fadeSide = hit.fadeCurve;
        m_fadeTrackId = hit.trackId;
        m_fadeClipId = hit.clipId;
        m_fadeCurveDragY = pos.y();
        m_fadeCurveOriginal = 0.0;
        if (const auto* clip = findClipModel(hit.trackId, hit.clipId)) {
            m_fadeCurveOriginal = hit.fadeCurve == Fade::In
                                      ? clip->fadeInCurve
                                      : clip->fadeOutCurve;
        }
        setCursor(Qt::SizeVerCursor);
        publishSelection();
        emit clipSelected(hit.trackId, hit.clipId);
        update();
        return;
    }
    if (over && hit.fade != Fade::None) {
        m_selection = {ClipRef{hit.trackId, hit.clipId}};
        m_selectedClipId = hit.clipId;
        beginProjectGesture(tr("Set Clip Fade"),
                            ProjectGestureKind::ClipFade);
        m_fading = true;
        m_fadeSide = hit.fade;
        m_fadeTrackId = hit.trackId;
        m_fadeClipId = hit.clipId;
        m_fadeStart = hit.startSeconds;
        m_fadeDuration = hit.durationSeconds;
        m_fadeOtherSeconds =
            hit.fade == Fade::In ? hit.fadeOutSeconds : hit.fadeInSeconds;
        m_fadeOriginalIn = hit.fadeInSeconds;
        m_fadeOriginalOut = hit.fadeOutSeconds;
        setCursor(Qt::SizeHorCursor);
        emit clipSelected(hit.trackId, hit.clipId);
        update();
        return;
    }
    // Gain handle: drag up to make the clip louder, down to make it quieter.
    // With several clips selected the whole group gets the same change.
    if (over && hit.gainHandle) {
        if (!isClipSelected(hit.clipId)) {
            m_selection = {ClipRef{hit.trackId, hit.clipId}};
        }
        m_selectedClipId = hit.clipId;
        beginProjectGesture(tr("Set Clip Gain"),
                            ProjectGestureKind::ClipGain);
        m_gainDragging = true;
        m_gainDragY = pos.y();
        m_gainOrig.clear();
        const auto& project = m_controller->project();
        for (const auto& ref : m_selection) {
            if (const auto* tr = project.findTrack(ref.trackId.toStdString())) {
                for (const auto& c : tr->clips) {
                    if (QString::fromStdString(c.id) == ref.clipId) {
                        m_gainOrig.push_back(
                            {ref.trackId, ref.clipId, c.gain, c.pan});
                    }
                }
            }
        }
        setCursor(Qt::SizeVerCursor);
        emit clipSelected(hit.trackId, hit.clipId);
        update();
        return;
    }
    if (over && hit.edge != Edge::None && hit.durationSeconds > 0.0) {
        m_selection = {ClipRef{hit.trackId, hit.clipId}};
        m_selectedClipId = hit.clipId;
        beginProjectGesture(tr("Trim Clip"),
                            ProjectGestureKind::ClipTrim);
        m_trimming = true;
        m_trimEdge = hit.edge;
        m_trimTrackId = hit.trackId;
        m_trimClipId = hit.clipId;
        m_trimOrigStart = hit.startSeconds;
        m_trimOrigOffset = hit.offsetSeconds;
        m_trimOrigDuration = hit.durationSeconds;
        m_controller->beginClipTrimEdit(m_trimTrackId.toStdString(),
                                        m_trimClipId.toStdString());
        m_clipTrimEditOpen = true;
        setCursor(Qt::SizeHorCursor);
        emit clipSelected(hit.trackId, hit.clipId);
        update();
        return;
    }
    if (over) {
        // Clicking a clip that is not already part of the selection selects
        // just it; clicking one that is keeps the group so it can be dragged.
        if (!isClipSelected(hit.clipId)) {
            m_selection = {ClipRef{hit.trackId, hit.clipId}};
        }
        m_selectedClipId = hit.clipId;
        beginClipPositionGesture(tr("Move Clips"));
        m_dragging = true;
        m_dragTrackId = hit.trackId;
        m_dragClipId = hit.clipId;
        m_dragGrabOffset = xToSeconds(pos.x()) - hit.startSeconds;
        // Remember every selected clip's start so the whole group moves as one.
        m_dragOrigStarts.clear();
        const auto& project = m_controller->project();
        for (const auto& ref : m_selection) {
            if (const auto* tr = project.findTrack(ref.trackId.toStdString())) {
                for (const auto& c : tr->clips) {
                    if (QString::fromStdString(c.id) == ref.clipId) {
                        m_dragOrigStarts.push_back({ref.clipId, c.startSeconds});
                    }
                }
            }
        }
        setCursor(Qt::ClosedHandCursor);
        emit clipSelected(hit.trackId, hit.clipId);
        update();
        return;
    }

    // Empty space: start a rubber-band selection, deselect, and seek.
    m_selection.clear();
    m_selectedClipId.clear();
    m_marqueeActive = true;
    m_marqueeOrigin = pos;
    m_marqueeCurrent = pos;
    m_controller->seekSeconds(std::max(0.0, xToSeconds(pos.x())));
    emit playheadMoved();
    update();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* ev) {
    // A cancelled implicit grab may arrive without MouseButtonRelease. Land the
    // current endpoint as soon as Qt reports that the left button is no longer
    // down, otherwise the controller would remain in deferred-publish mode.
    if (m_projectGestureActive && !(ev->buttons() & Qt::LeftButton)) {
        cancelProjectGesture();
        updateCursor(ev->position().toPoint());
        update();
        return;
    }
    // If the platform cancelled its implicit mouse grab, do not leave the
    // timeline in a mode where ordinary hover movement keeps panning forever.
    if (m_panning && !(ev->buttons() & Qt::MiddleButton)) {
        m_panning = false;
        updateCursor(ev->position().toPoint());
    }
    if (m_panning) {
        // Grab-and-drag navigation: the content follows the hand, hence the
        // inverse sign on both scroll offsets. Use incremental deltas so the
        // gesture remains stable when either axis reaches its clamp.
        const QPoint position = ev->position().toPoint();
        const QPoint delta = position - m_panLastPosition;
        m_panLastPosition = position;
        m_scrollSeconds = std::max(
            0.0, m_scrollSeconds - double(delta.x()) / m_pixelsPerSecond);
        setVerticalScroll(m_scrollY - delta.y());
        update();
        ev->accept();
        return;
    }
    // Not during a drag: a gesture keeps the tool it started with, however the
    // hand moves on the keyboard halfway through it.
    if (!(ev->buttons() & Qt::LeftButton)) trackAltTool(ev->modifiers());
    const QPoint pos = ev->position().toPoint();
    const bool snapOn = m_snapEnabled && !(ev->modifiers() & Qt::AltModifier);

    if (m_drawingCurve) {
        paintCurveAt(pos, snapOn);
        return;
    }

    if (m_bendingSegment || m_draggingPoint) {
        const daw::ClipModel* clip =
            findClipModel(m_pointDrag.trackId, m_pointDrag.clipId);
        if (!clip) return;
        std::vector<daw::AutomationPoint> points = clip->automation.points;

        if (m_bendingSegment) {
            if (m_pointDrag.segment < 0 ||
                std::size_t(m_pointDrag.segment) >= points.size()) {
                return;
            }
            // Sixty pixels for the whole bend, the same feel the clip fades'
            // curve handles have.
            const double dy = (m_bendStartY - pos.y()) / 60.0;
            points[std::size_t(m_pointDrag.segment)].curve =
                std::clamp(m_bendStartCurve + dy, -1.0, 1.0);
        } else {
            if (m_pointDrag.index < 0 ||
                std::size_t(m_pointDrag.index) >= m_dragPoints.size()) {
                return;
            }
            const double moved = std::max(
                0.0, snapBeats(automationBeatsAtX(*clip, pos.x()), snapOn));
            // The height is never snapped: a grid of values is not a thing, and
            // a curve is drawn by eye. Shift is the exception: it locks the
            // starting value and turns the gesture into horizontal-only timing.
            const double value = (ev->modifiers() & Qt::ShiftModifier)
                                     ? m_dragPoints[std::size_t(m_pointDrag.index)].value
                                     : automationValueAtY(m_pointDrag.body, pos.y());
            const double guard = snapOn && m_gridBeats > 0.0
                                     ? m_gridBeats
                                     : daw::secondsToBeats(
                                           8.0 / m_pixelsPerSecond,
                                           m_controller->project().tempo);
            points = daw::autotools::dragPoint(
                m_dragPoints, std::size_t(m_pointDrag.index), moved, value, guard);
            m_controller->setAutomationPoints(m_pointDrag.trackId.toStdString(),
                                              m_pointDrag.clipId.toStdString(),
                                              points);
            setCursor((ev->modifiers() & Qt::ShiftModifier)
                          ? Qt::SizeHorCursor
                          : Qt::SizeAllCursor);
            update();
            return;
        }
        m_controller->setAutomationPoints(m_pointDrag.trackId.toStdString(),
                                          m_pointDrag.clipId.toStdString(),
                                          points);
        update();
        return;
    }

    if (m_loopGrab != LoopGrab::None) {
        const double at = std::max(0.0, snap(xToSeconds(pos.x()), snapOn));
        switch (m_loopGrab) {
            case LoopGrab::Move: {
                const double from = std::max(0.0, at - m_loopGrabOffset);
                m_controller->setLoopRangeSeconds(from, from + m_loopGrabLength);
                break;
            }
            case LoopGrab::Create:
            case LoopGrab::ResizeStart:
            case LoopGrab::ResizeEnd:
                // Sorted, not clamped: dragging back past where the press
                // landed turns the region round instead of pinning it to zero
                // width, which is what makes a right-to-left drag work.
                m_controller->setLoopRangeSeconds(
                    std::min(m_loopAnchorSeconds, at),
                    std::max(m_loopAnchorSeconds, at));
                break;
            case LoopGrab::None:
                break;
        }
        update();
        return;
    }

    if (m_scrubbing) {
        m_controller->seekSeconds(std::max(0.0, xToSeconds(pos.x())));
        emit playheadMoved();
        update();
        return;
    }

    // A swipe in flight owns the pointer until it is let go — the stroke may
    // wander outside the row it started on, and it should keep painting that
    // take rather than jumping to whichever row it happens to pass over.
    if (m_swiping) {
        updateSwipe(pos, m_snapEnabled && !(ev->modifiers() & Qt::AltModifier));
        return;
    }

    // The bottom-edge pull. One threshold in each direction, then the animation
    // takes over — this is a gesture to open a drawer, not a resize handle.
    if (m_expandDrag) {
        const int dy = pos.y() - m_expandPressY;
        if (std::abs(dy) < 8) return;
        const daw::ClipModel* clip = findClipModel(m_expandTrackId, m_expandClipId);
        if (clip && clip->expanded == (dy > 0)) {
            // Already the way the pull is asking for; nothing to do but stop.
            m_expandDrag = false;
            return;
        }
        m_expandDrag = false;
        toggleClipExpanded(m_expandTrackId, m_expandClipId);
        updateCursor(pos);
        return;
    }

    // Region gestures continue under whichever tool was selected after the
    // rectangle was committed. Only `m_regionPicking` requires the Region
    // tool; pending/moving are properties of the selection itself.
    if (m_regionPicking || m_regionMovePending || m_regionMoving) {
        const bool snapOn =
            m_snapEnabled && !(ev->modifiers() & Qt::ShiftModifier);
        if (m_regionPicking) {
            updateRegionFromDrag(pos, snapOn);
            update();
            return;
        }
        if (m_regionMovePending) {
            // The contents tear out of the region only once the drag starts.
            if ((pos - m_regionPressPos).manhattanLength() > 4) {
                beginClipPositionGesture(tr("Move Region"));
                m_regionMovePending = false;
                m_regionMoving = true;
                m_regionActive = true;
                m_regionMoveGrab = xToSeconds(pos.x());
                collectRegionPieces();
                if (!m_regionPieces.empty()) markProjectGestureChanged();
            }
            update();
            return;
        }
        if (m_regionMoving) {
            const double newStart = snap(m_regionMoveOrigStart +
                                             (xToSeconds(pos.x()) - m_regionMoveGrab),
                                         snapOn);
            const double delta = newStart - m_regionMoveOrigStart;
            std::vector<daw::EngineController::ClipStartChange> changes;
            changes.reserve(m_regionPieces.size());
            for (const auto& piece : m_regionPieces) {
                changes.push_back({piece.trackId.toStdString(),
                                   piece.clipId.toStdString(),
                                   std::max(0.0, piece.origStart + delta)});
            }
            m_controller->setClipStartsSeconds(changes);
            m_regionStart = m_regionMoveOrigStart + delta;
            m_regionEnd = m_regionMoveOrigEnd + delta;
            update();
            return;
        }
    }

    if (m_marqueeActive) {
        m_marqueeCurrent = pos;
        update();
        return;
    }

    // Gain: the change is exponential so equal drags give equal perceived
    // loudness steps; up (negative dy) doubles, down halves.
    if (m_gainDragging) {
        const double factor =
            std::pow(2.0, -(pos.y() - m_gainDragY) / kGainPixelsPerDoubling);
        for (const auto& g : m_gainOrig) {
            markProjectGestureChanged();
            m_controller->setClipGain(
                g.trackId.toStdString(), g.clipId.toStdString(),
                std::clamp(float(g.origGain * factor), 0.0f,
                           kMaxTimelineClipGain));
        }
        update();
        return;
    }

    // Fade curvature: up bends toward full level sooner, down keeps the ramp
    // near silence longer. The full useful range is one compact 60 px drag.
    if (m_fadeCurving) {
        const double curve = std::clamp(
            m_fadeCurveOriginal - (pos.y() - m_fadeCurveDragY) / 60.0,
            -1.0, 1.0);
        markProjectGestureChanged();
        m_controller->setClipFadeCurve(
            m_fadeTrackId.toStdString(), m_fadeClipId.toStdString(),
            m_fadeSide == Fade::In, curve);
        update();
        return;
    }

    // Fade: drag a top corner in/out to set the head or tail fade length.
    // Fades are continuous — they don't snap to the grid, so the ramp glides.
    if (m_fading) {
        const double pointer = xToSeconds(pos.x());
        const double maxFade = std::max(0.0, m_fadeDuration - m_fadeOtherSeconds);
        double fadeIn = m_fadeSide == Fade::In ? 0.0 : m_fadeOtherSeconds;
        double fadeOut = m_fadeSide == Fade::Out ? 0.0 : m_fadeOtherSeconds;
        if (m_fadeSide == Fade::In) {
            fadeIn = std::clamp(pointer - m_fadeStart, 0.0, maxFade);
        } else {
            fadeOut = std::clamp((m_fadeStart + m_fadeDuration) - pointer, 0.0,
                                 maxFade);
        }
        markProjectGestureChanged();
        m_controller->setClipFade(m_fadeTrackId.toStdString(),
                                  m_fadeClipId.toStdString(), fadeIn, fadeOut);
        update();
        return;
    }

    // Eraser stroke: delete any clip dragged over.
    if (m_erasing) {
        ClipHit hit;
        if (hitTestClip(pos, hit)) {
            m_controller->removeClip(hit.trackId.toStdString(),
                                     hit.clipId.toStdString());
            markProjectGestureChanged();
            if (m_selectedClipId == hit.clipId) m_selectedClipId.clear();
            emit projectEdited();
            update();
        }
        return;
    }

    // Trim: resize by the dragged edge, kept relative to the original geometry.
    if (m_trimming) {
        constexpr double kMinTrim = 0.02;
        const double pointer = snap(xToSeconds(pos.x()), snapOn);
        if (m_trimEdge == Edge::Right) {
            const double newDuration = std::max(kMinTrim, pointer - m_trimOrigStart);
            markProjectGestureChanged();
            m_controller->setClipTrim(m_trimTrackId.toStdString(),
                                      m_trimClipId.toStdString(), m_trimOrigStart,
                                      m_trimOrigOffset, newDuration);
        } else {  // Left edge
            double delta = pointer - m_trimOrigStart;
            const double stretch = std::max(
                0.01, m_controller->clipSampleParameter(
                          m_trimTrackId.toStdString(), m_trimClipId.toStdString(),
                          "stretch.time"));
            // Can't expose audio before the source start, shrink below the
            // minimum, or push the clip start before zero.
            const double minDelta =
                std::max(-m_trimOrigOffset * stretch, -m_trimOrigStart);
            const double maxDelta = m_trimOrigDuration - kMinTrim;
            delta = std::clamp(delta, minDelta, maxDelta);
            markProjectGestureChanged();
            m_controller->setClipTrim(
                m_trimTrackId.toStdString(), m_trimClipId.toStdString(),
                m_trimOrigStart + delta, m_trimOrigOffset + delta / stretch,
                m_trimOrigDuration - delta);
        }
        update();
        return;
    }

    if (!m_dragging) {
        updateCursor(pos);
        return;
    }

    markProjectGestureChanged();

    // Alt is the usual "ignore the grid for a moment" modifier.
    const double pointerStart = snap(xToSeconds(pos.x()) - m_dragGrabOffset, snapOn);

    // A single clip can be dragged onto another lane that takes its kind —
    // audio onto audio, MIDI onto MIDI/instrument.
    if (m_selection.size() == 1) {
        const int lane = laneAt(pos.y());
        const QString target = trackIdForLane(lane);
        if (!target.isEmpty() && target != m_dragTrackId) {
            const auto* tr =
                m_controller->project().findTrack(target.toStdString());
            // The clip's kind has to come from the document; the drag state
            // only carries ids.
            daw::ClipKind dragKind = daw::ClipKind::Audio;
            if (const auto* src = m_controller->project().findTrack(
                    m_dragTrackId.toStdString())) {
                for (const auto& c : src->clips) {
                    if (c.id == m_dragClipId.toStdString()) {
                        dragKind = c.kind;
                        break;
                    }
                }
            }
            if (tr && daw::trackAccepts(tr->kind, dragKind)) {
                m_controller->moveClipToTrack(m_dragTrackId.toStdString(),
                                              m_dragClipId.toStdString(),
                                              target.toStdString());
                m_dragTrackId = target;
                if (!m_selection.isEmpty()) m_selection[0].trackId = target;
            }
        }
    }

    // Original start of the grabbed clip; the group moves by the same delta.
    double grabbedOrig = pointerStart;
    for (const auto& kv : m_dragOrigStarts) {
        if (kv.first == m_dragClipId) { grabbedOrig = kv.second; break; }
    }
    const double delta = pointerStart - grabbedOrig;
    std::vector<daw::EngineController::ClipStartChange> changes;
    changes.reserve(std::size_t(m_selection.size()));
    for (const auto& ref : m_selection) {
        double orig = grabbedOrig;
        for (const auto& kv : m_dragOrigStarts) {
            if (kv.first == ref.clipId) { orig = kv.second; break; }
        }
        changes.push_back({ref.trackId.toStdString(), ref.clipId.toStdString(),
                           std::max(0.0, orig + delta)});
    }
    m_controller->setClipStartsSeconds(changes);
    update();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* ev) {
    if (m_draggingPoint || m_bendingSegment || m_drawingCurve) {
        m_draggingPoint = false;
        m_bendingSegment = false;
        m_drawingCurve = false;
        // One undo entry for the whole gesture, from the curve as it stood when
        // the hand went down — the split every curve editor here uses.
        m_controller->commitAutomationEdit(m_pointDrag.trackId.toStdString(),
                                           m_pointDrag.clipId.toStdString(),
                                           m_pointsBefore, "Edit Automation");
        m_pointsBefore.clear();
        m_dragPoints.clear();
        updateCursor(ev->position().toPoint());
        emit projectEdited();
        update();
        ev->accept();
        return;
    }
    if (ev->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        updateCursor(ev->position().toPoint());
        ev->accept();
        return;
    }
    if (m_loopGrab != LoopGrab::None) {
        const LoopGrab was = m_loopGrab;
        m_loopGrab = LoopGrab::None;
        // A press that never became a drag has left a zero-length region
        // behind; clear it rather than leaving an invisible one to trap the
        // playhead the next time Cycle is switched on.
        if (m_controller->loopEndSeconds() <= m_controller->loopStartSeconds()) {
            m_controller->setLoopRangeSeconds(0.0, 0.0);
            if (was == LoopGrab::Create && m_controller->isLoopEnabled()) {
                m_controller->setLoopEnabled(false);
            }
        }
        // Deliberately *not* armed by the drag. Marking out a region and
        // switching the cycle on are two decisions — the region stays as a dim
        // outline until C (or the transport's Cycle button) lights it, which is
        // also what makes the lit state mean something.
        updateCursor(ev->position().toPoint());
        emit loopRangeChanged();
        update();
        ev->accept();
        return;
    }
    if (m_expandDrag) {
        // Released without travelling far enough: treat it as a plain click on
        // the clip so the edge is not a dead strip.
        m_expandDrag = false;
        m_selection = {ClipRef{m_expandTrackId, m_expandClipId}};
        m_selectedClipId = m_expandClipId;
        publishSelection();
        emit clipSelected(m_expandTrackId, m_expandClipId);
        updateCursor(ev->position().toPoint());
        update();
        return;
    }

    if (m_swiping) {
        m_swiping = false;
        m_controller->endCompEdit();
        m_swipeTakeId.clear();
        emit projectEdited();
        updateCursor(ev->position().toPoint());
        update();
        return;
    }
    if (m_regionPicking) {
        m_regionPicking = false;
        // A bare click leaves no region behind.
        if (m_regionEnd - m_regionStart > 0.0 && m_regionLaneA >= 0) {
            m_regionActive = true;
        } else {
            clearRegion();
        }
        update();
    }
    if (m_regionMovePending) {
        m_regionMovePending = false;   // clicked inside the region, didn't drag
        clearRegion();
        update();
    }
    if (m_regionMoving) {
        m_regionMoving = false;
        m_regionActive = m_regionEnd > m_regionStart && m_regionLaneA >= 0;
        finishProjectGesture();
        m_regionPieces.clear();
        emit projectEdited();
    }
    if (m_fadeCurving) {
        m_fadeCurving = false;
        finishProjectGesture();
        m_fadeSide = Fade::None;
        emit projectEdited();
    }
    if (m_dragging) {
        m_dragging = false;
        finishProjectGesture();
        m_dragOrigStarts.clear();
        emit projectEdited();
    }
    if (m_trimming) {
        m_trimming = false;
        finishProjectGesture();
        m_trimEdge = Edge::None;
        emit projectEdited();
    }
    if (m_fading) {
        m_fading = false;
        finishProjectGesture();
        m_fadeSide = Fade::None;
        emit projectEdited();
    }
    if (m_gainDragging) {
        m_gainDragging = false;
        finishProjectGesture();
        m_gainOrig.clear();
        emit projectEdited();
    }
    if (m_marqueeActive) {
        m_marqueeActive = false;
        const QRect box = QRect(m_marqueeOrigin, m_marqueeCurrent).normalized();
        // A bare click (no drag) leaves the selection empty — it was just a seek.
        if (box.width() > 3 || box.height() > 3) {
            m_selection.clear();
            const auto& project = m_controller->project();
            const auto& rows = daw::visibleTracks(project);
            for (int lane = 0; lane < int(rows.size()); ++lane) {
                const auto& track = project.tracks[rows[size_t(lane)].index];
                for (const auto& clip : track.clips) {
                    if (clipRect(lane, clip).intersects(box)) {
                        m_selection.push_back(
                            {QString::fromStdString(track.id),
                             QString::fromStdString(clip.id)});
                    }
                }
            }
            if (!m_selection.isEmpty()) {
                m_selectedClipId = m_selection.front().clipId;
                emit clipSelected(m_selection.front().trackId, m_selectedClipId);
            }
        }
        update();
    }
    if (m_erasing) finishProjectGesture();
    m_erasing = false;
    m_scrubbing = false;
    updateCursor(ev->position().toPoint());
}

void TimelineWidget::leaveEvent(QEvent*) {
    // An implicit mouse grab keeps a middle-button drag alive outside the
    // widget. Keep its hand cursor until the matching release comes back.
    if (!m_panning) unsetCursor();
}

bool TimelineWidget::event(QEvent* e) {
    if ((m_projectGestureActive || m_clipPositionEditOpen ||
         m_clipTrimEditOpen) &&
        (e->type() == QEvent::UngrabMouse || e->type() == QEvent::Hide)) {
        cancelProjectGesture();
    }
    if (e->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(e);
        if (isArrangementEditShortcut(key)) {
            // Claim the chord before duplicate menu actions can make Qt mark
            // it ambiguous. The matching KeyPress is handled below.
            key->accept();
            return true;
        }
    }

    // Trackpad pinch on macOS arrives as a native zoom gesture. Zoom around the
    // fingers using the same anchor trick as Ctrl+wheel so the spot under the
    // cursor stays put.
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            const double cursorX = g->position().x();
            const double anchor = xToSeconds(int(cursorX));
            zoomBy(1.0 + g->value());
            m_scrollSeconds =
                std::max(0.0, anchor - cursorX / m_pixelsPerSecond);
            update();
            return true;
        }
    }

    const bool handled = QWidget::event(e);
    // One publish point per interaction, after the handler has settled, rather
    // than at every site that touches m_selection. The model ignores a push
    // that changes nothing, so the over-calling costs a short vector compare.
    switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::ContextMenu:
        case QEvent::Drop:
            publishSelection();
            break;
        default:
            break;
    }
    return handled;
}

void TimelineWidget::keyPressEvent(QKeyEvent* event) {
    if (isArrangementEditShortcut(event)) {
        switch (editShortcutKey(event)) {
            case Qt::Key_X: cutSelection(); break;
            case Qt::Key_C: copySelection(); break;
            case Qt::Key_V: pasteClipboard(); break;
            case Qt::Key_B: repeatSelection(); break;
            default: break;
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineWidget::wheelEvent(QWheelEvent* ev) {
    if (ev->modifiers() & Qt::ControlModifier) {
        // Zoom around the pointer so the spot under the cursor stays put.
        const double anchor = xToSeconds(int(ev->position().x()));
        zoomBy(ev->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15);
        m_scrollSeconds =
            std::max(0.0, anchor - ev->position().x() / m_pixelsPerSecond);
    } else if (ev->modifiers() & Qt::ShiftModifier ||
               std::abs(ev->angleDelta().x()) > std::abs(ev->angleDelta().y())) {
        const double delta = -(ev->angleDelta().x() != 0 ? ev->angleDelta().x()
                                                         : ev->angleDelta().y());
        m_scrollSeconds = std::max(0.0, m_scrollSeconds + delta / 4.0 / m_pixelsPerSecond);
    } else {
        // Straight up and down moves through the tracks — the arrangement is a
        // tall stack of lanes in a fixed window, so this is the axis the wheel
        // is for. Time is on Shift (and on a trackpad's own horizontal swipe).
        setVerticalScroll(m_scrollY - ev->angleDelta().y() / 2);
    }
    update();
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* ev) {
    // Take rows first: they are inside a lane but below the clip bodies, so a
    // right-click there is about the take, not the clip above it.
    TakeHit takeHit;
    if (hitTestTake(ev->pos(), takeHit)) {
        showTakeMenu(takeHit, ev->globalPos());
        return;
    }

    // A curve, before the clip that carries it: on an automation lane the
    // interior belongs to the breakpoints, and a right-click there is about the
    // point or the run under it.
    if (PointHit point; hitTestAutomationPoint(ev->pos(), point)) {
        showAutomationMenu(point, ev->globalPos());
        return;
    }

    ClipHit hit;
    if (hitTestClip(ev->pos(), hit)) {
        QMenu menu(this);
        QAction* openRoll = nullptr;
        QAction* openPattern = nullptr;
        QAction* detectBpm = nullptr;
        QAction* detectKey = nullptr;
        QAction* detectBoth = nullptr;
        if (hit.kind == daw::ClipKind::Midi) {
            openRoll = menu.addAction(tr("Open Piano Roll"));
            menu.addSeparator();
        } else if (hit.kind == daw::ClipKind::Pattern) {
            openPattern = menu.addAction(tr("Open Pattern Editor"));
            menu.addSeparator();
        } else if (hit.kind == daw::ClipKind::Audio) {
            detectBpm = menu.addAction(tr("Detect BPM…"));
            detectKey = menu.addAction(tr("Detect Key…"));
            detectBoth = menu.addAction(tr("Detect BPM & Key…"));
            menu.addSeparator();
        }

        const daw::ClipModel* clip = findClipModel(hit.trackId, hit.clipId);
        // Right-click anywhere inside an existing fade to switch its processing
        // without having to hit the tiny corner knob precisely.
        Fade menuFade = Fade::None;
        QAction* gainFade = nullptr;
        QAction* tapeFade = nullptr;
        if (clip && clip->kind == daw::ClipKind::Audio) {
            const double at = xToSeconds(ev->pos().x());
            const double end = clip->startSeconds + clip->durationSeconds;
            if (clip->fadeInSeconds > 0.0 &&
                at >= clip->startSeconds &&
                at <= clip->startSeconds + clip->fadeInSeconds) {
                menuFade = Fade::In;
            } else if (clip->fadeOutSeconds > 0.0 &&
                       at >= end - clip->fadeOutSeconds && at <= end) {
                menuFade = Fade::Out;
            }
            if (menuFade != Fade::None) {
                QMenu* fadeMenu = menu.addMenu(
                    menuFade == Fade::In ? tr("Fade In") : tr("Fade Out"));
                gainFade = fadeMenu->addAction(tr("Volume Fade"));
                tapeFade = fadeMenu->addAction(
                    menuFade == Fade::In ? tr("Speed Up (Tape Start)")
                                         : tr("Speed Down (Tape Stop)"));
                gainFade->setCheckable(true);
                tapeFade->setCheckable(true);
                const daw::ClipFadeMode current = menuFade == Fade::In
                                                      ? clip->fadeInMode
                                                      : clip->fadeOutMode;
                gainFade->setChecked(current == daw::ClipFadeMode::Gain);
                tapeFade->setChecked(current == daw::ClipFadeMode::Tape);
                menu.addSeparator();
            }
        }

        // Comp actions, on a layered clip only.
        QAction* expand = nullptr;
        QAction* flatten = nullptr;
        QAction* commit = nullptr;
        QAction* crop = nullptr;
        QAction* unused = nullptr;
        if (clip && daw::isLayered(*clip)) {
            expand = menu.addAction(clip->expanded ? tr("Close Comp Editor")
                                                   : tr("Open Comp Editor"));
            flatten = menu.addAction(tr("Flatten Comp to New Take"));
            commit = menu.addAction(tr("Commit Comp…"));
            crop = menu.addAction(tr("Crop Takes to Comp…"));
            unused = menu.addAction(tr("Delete Unused Takes"));
            menu.addSeparator();
        }

        QAction* repeat = menu.addAction(tr("Repeat Clip"));
        QAction* muteClip = menu.addAction(
            clip && clip->muted ? tr("Unmute Clip") : tr("Mute Clip"));
        QAction* del = menu.addAction(tr("Delete Clip"));
        // With several clips selected, offer to delete them all.
        QAction* delAll = nullptr;
        if (m_selection.size() > 1 && isClipSelected(hit.clipId)) {
            delAll = menu.addAction(
                tr("Delete %1 Selected Clips").arg(m_selection.size()));
        }
        QAction* chosen = menu.exec(ev->globalPos());
        const std::string trackId = hit.trackId.toStdString();
        const std::string clipId = hit.clipId.toStdString();
        if (chosen == gainFade && gainFade) {
            m_controller->setClipFadeMode(trackId, clipId,
                                          menuFade == Fade::In,
                                          daw::ClipFadeMode::Gain);
            emit projectEdited();
            update();
        } else if (chosen == tapeFade && tapeFade) {
            m_controller->setClipFadeMode(trackId, clipId,
                                          menuFade == Fade::In,
                                          daw::ClipFadeMode::Tape);
            emit projectEdited();
            update();
        } else if (chosen == openRoll && openRoll) {
            emit openPianoRollRequested(hit.trackId, hit.clipId);
        } else if (chosen == openPattern && openPattern) {
            emit openPatternRequested(hit.trackId);
        } else if (chosen == detectBpm && detectBpm) {
            emit audioAnalysisRequested(hit.trackId, hit.clipId, true, false);
        } else if (chosen == detectKey && detectKey) {
            emit audioAnalysisRequested(hit.trackId, hit.clipId, false, true);
        } else if (chosen == detectBoth && detectBoth) {
            emit audioAnalysisRequested(hit.trackId, hit.clipId, true, true);
        } else if (expand && chosen == expand) {
            toggleClipExpanded(hit.trackId, hit.clipId);
        } else if (flatten && chosen == flatten) {
            // Non-destructive: the bake lands as one more take beside the ones
            // it was made from, so the comp can still be taken apart afterwards.
            if (m_controller->flattenComp(trackId, clipId).empty()) return;
            emit projectEdited();
            emit laneHeightsChanged();
            update();
        } else if (commit && chosen == commit) {
            if (QMessageBox::question(
                    this, tr("Commit comp"),
                    tr("Turn this clip into a plain clip playing the comp?\n\n"
                       "Its %1 takes are removed from the project and the clip "
                       "can no longer be expanded. Their audio files are left "
                       "on disk.")
                        .arg(clip->takes.size()),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) {
                return;
            }
            m_controller->commitComp(trackId, clipId);
            emit projectEdited();
            emit laneHeightsChanged();
            update();
        } else if (crop && chosen == crop) {
            if (QMessageBox::question(
                    this, tr("Crop takes"),
                    tr("Trim the audio files of this clip's takes to what the "
                       "comp plays?\n\nThe files are rewritten on disk and the "
                       "trimmed audio cannot be recovered."),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) {
                return;
            }
            const size_t n = m_controller->cropToComp(trackId, clipId);
            if (n == 0) return;
            emit projectEdited();
            update();
        } else if (unused && chosen == unused) {
            // Document-only: the files stay, so this needs no confirmation. The
            // takes it drops are the ones no comp anywhere references.
            if (m_controller->deleteUnusedTakes(false) == 0) return;
            emit projectEdited();
            emit laneHeightsChanged();
            update();
        } else if (chosen == repeat) {
            if (!isClipSelected(hit.clipId)) {
                m_selection = {ClipRef{hit.trackId, hit.clipId}};
                m_selectedClipId = hit.clipId;
            }
            repeatSelection();
        } else if (chosen == muteClip) {
            if (!isClipSelected(hit.clipId)) {
                m_selection = {ClipRef{hit.trackId, hit.clipId}};
                m_selectedClipId = hit.clipId;
            }
            toggleSelectedClipsMuted();
        } else if (chosen == delAll && delAll) {
            deleteSelectedClips();
        } else if (chosen == del) {
            m_controller->removeClip(hit.trackId.toStdString(),
                                     hit.clipId.toStdString());
            m_selection.removeIf(
                [&](const ClipRef& r) { return r.clipId == hit.clipId; });
            if (m_selectedClipId == hit.clipId) m_selectedClipId.clear();
            emit projectEdited();
            update();
        }
        return;
    }

    // Empty lane area, or below the last lane. On a MIDI lane, offer to put a
    // clip there first; the rest of the menu creates tracks.
    QMenu menu(this);
    QAction* addMidiClip = nullptr;
    QAction* addPatternClip = nullptr;
    const int lane = laneAt(ev->pos().y());
    const QString laneTrackId = lane >= 0 ? trackIdForLane(lane) : QString();
    if (const auto* laneTrack =
            laneTrackId.isEmpty()
                ? nullptr
                : m_controller->project().findTrack(laneTrackId.toStdString());
        laneTrack) {
        if (daw::trackAccepts(laneTrack->kind, daw::ClipKind::Midi)) {
            addMidiClip = menu.addAction(tr("Add MIDI Clip"));
            menu.addSeparator();
        } else if (laneTrack->kind == daw::TrackKind::Pattern) {
            addPatternClip = menu.addAction(tr("Add Pattern Clip"));
            menu.addSeparator();
        }
    }

    const auto trackKinds = ui::addTrackKindItems(menu);

    QAction* chosen = menu.exec(ev->globalPos());
    if (!chosen) return;
    if (chosen == addMidiClip) {
        const std::string clipId = m_controller->addMidiClip(
            laneTrackId.toStdString(),
            std::max(0.0, snap(xToSeconds(ev->pos().x()), m_snapEnabled)));
        if (!clipId.empty()) {
            emit projectEdited();
            update();
        }
        return;
    }
    if (chosen == addPatternClip) {
        const std::string clipId = m_controller->addPatternClip(
            laneTrackId.toStdString(),
            std::max(0.0, snap(xToSeconds(ev->pos().x()), m_snapEnabled)));
        if (!clipId.empty()) {
            emit projectEdited();
            update();
        }
        return;
    }
    if (const auto spec = trackKinds.constFind(chosen);
        spec != trackKinds.constEnd()) {
        spec->create(*m_controller);
        emit projectEdited();
        emit tracksChanged();
        update();
    }
}

// ── External file drag-and-drop ──────────────────────────────────────────────

namespace {
/// Does this drag carry anything the arrangement can take in — audio, or a
/// Standard MIDI File? Both the desktop and the browser panel hand over plain
/// file URLs, so one predicate covers them.
bool mimeHasImportable(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) return false;
    for (const QUrl& u : mime->urls()) {
        if (ui::isImportableFile(u.toLocalFile())) return true;
    }
    return false;
}

/// A plugin dragged out of the browser. It carries no file, so it is a separate
/// question from `mimeHasImportable` and lands somewhere entirely different.
bool mimeHasPlugin(const QMimeData* mime) {
    int format = 0;
    QString uid;
    return ui::decodePluginRef(mime, format, uid);
}

QString projectTemplateFromMime(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) return {};
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (ui::projecttemplates::isTemplatePackage(path)) return path;
    }
    return {};
}
} // namespace

std::optional<daw::plugins::PluginDescriptor> TimelineWidget::pluginFromMime(
    const QMimeData* mime) const {
    int format = 0;
    QString uid;
    if (!ui::decodePluginRef(mime, format, uid)) return std::nullopt;
    // Resolved here rather than carried in the drag: two identifiers cannot go
    // stale, a copied descriptor can.
    return m_controller->pluginManager().find(daw::plugins::Format(format),
                                              uid.toStdString());
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* ev) {
    if (!mimeHasImportable(ev->mimeData()) && !mimeHasPlugin(ev->mimeData()) &&
        projectTemplateFromMime(ev->mimeData()).isEmpty())
        return;
    m_dropActive = true;
    m_dropLane = projectTemplateFromMime(ev->mimeData()).isEmpty()
                     ? laneAt(ev->position().toPoint().y())
                     : -1;
    ev->acceptProposedAction();
    update();
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* ev) {
    if (!projectTemplateFromMime(ev->mimeData()).isEmpty()) {
        m_dropClip = {};
        m_dropLane = -1;
        ev->acceptProposedAction();
        update();
        return;
    }
    if (mimeHasPlugin(ev->mimeData())) {
        // A plugin lands on one thing, not at a time: highlight the clip under
        // the pointer when there is one, and the lane otherwise, so the drop is
        // not a guess about which of the two it will hit.
        ClipHit hit;
        m_dropClip = hitTestClip(ev->position().toPoint(), hit)
                         ? ClipRef{hit.trackId, hit.clipId}
                         : ClipRef{};
        m_dropLane = m_dropClip.clipId.isEmpty()
                         ? laneAt(ev->position().toPoint().y())
                         : -1;
        ev->acceptProposedAction();
        update();
        return;
    }
    if (!mimeHasImportable(ev->mimeData())) return;
    m_dropLane = laneAt(ev->position().toPoint().y());
    ev->acceptProposedAction();
    update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {
    m_dropActive = false;
    m_dropClip = {};
    update();
}

void TimelineWidget::dropEvent(QDropEvent* ev) {
    m_dropActive = false;
    const ClipRef overClip = m_dropClip;
    m_dropClip = {};

    if (const QString templ = projectTemplateFromMime(ev->mimeData());
        !templ.isEmpty()) {
        ev->acceptProposedAction();
        emit projectTemplateTracksRequested(templ);
        update();
        return;
    }

    // ── A plugin out of the browser ──
    if (const auto plugin = pluginFromMime(ev->mimeData())) {
        ClipHit hit;
        const bool onClip = hitTestClip(ev->position().toPoint(), hit);
        (void)overClip;
        QString landedOn;
        QString editorChannel;
        QString editorSlot;
        if (onClip && hit.kind == daw::ClipKind::Audio) {
            // Onto a clip: the plugin runs on that clip alone, before it joins
            // the track — which is what "apply it to this clip" has to mean.
            const std::string id = m_controller->addClipFxInsert(
                hit.trackId.toStdString(), hit.clipId.toStdString(), *plugin);
            if (!id.empty()) {
                landedOn = tr("the clip");
                editorChannel = hit.trackId;
                editorSlot = QString::fromStdString(id);
            }
        } else {
            const QString trackId = trackIdForLane(laneAt(ev->position().toPoint().y()));
            const auto* track =
                trackId.isEmpty()
                    ? nullptr
                    : m_controller->project().findTrack(trackId.toStdString());
            if (track && daw::carriesAudio(*track)) {
                const QString targetName = QString::fromStdString(track->name);
                if (plugin->isInstrument) {
                    // An instrument is not an insert; it goes at the head of
                    // the chain, where the notes are played.
                    const bool pattern =
                        track->kind == daw::TrackKind::Pattern;
                    bool loaded = false;
                    if (pattern) {
                        const std::string child =
                            m_controller->addPatternInstrument(
                                trackId.toStdString(), *plugin,
                                std::max(0.0, xToSeconds(
                                    ev->position().toPoint().x())));
                        loaded = !child.empty();
                        const auto* childTrack = loaded
                            ? m_controller->project().findTrack(child)
                            : nullptr;
                        if (childTrack && !childTrack->instrument.id.empty()) {
                            editorChannel = QString::fromStdString(child);
                            editorSlot = QString::fromStdString(
                                childTrack->instrument.id);
                        }
                    } else {
                        loaded = m_controller->setTrackInstrumentPlugin(
                            trackId.toStdString(), *plugin);
                        const auto* loadedTrack = loaded
                            ? m_controller->project().findTrack(
                                  trackId.toStdString())
                            : nullptr;
                        if (loadedTrack && !loadedTrack->instrument.id.empty()) {
                            editorChannel = trackId;
                            editorSlot = QString::fromStdString(
                                loadedTrack->instrument.id);
                        }
                    }
                    if (loaded) {
                        landedOn = targetName;
                    }
                } else {
                    const std::string id =
                        m_controller->addInsert(trackId.toStdString(), *plugin);
                    if (!id.empty()) {
                        landedOn = targetName;
                        editorChannel = trackId;
                        editorSlot = QString::fromStdString(id);
                    }
                }
            }
        }
        ev->acceptProposedAction();
        if (!landedOn.isEmpty()) {
            emit projectEdited();
            emit tracksChanged();
            emit pluginDropped(QString::fromStdString(plugin->name), landedOn);
            if (!editorChannel.isEmpty() && !editorSlot.isEmpty())
                emit pluginEditorRequested(editorChannel, editorSlot);
        }
        update();
        return;
    }

    if (!ev->mimeData()->hasUrls()) { update(); return; }

    QStringList files;
    for (const QUrl& u : ev->mimeData()->urls()) {
        const QString p = u.toLocalFile();
        if (!p.isEmpty() && ui::isImportableFile(p)) files << p;
    }
    if (files.isEmpty()) { update(); return; }

    const QPoint pos = ev->position().toPoint();
    const bool snapOn = m_snapEnabled && !(ev->modifiers() & Qt::AltModifier);
    const double start = snap(std::max(0.0, xToSeconds(pos.x())), snapOn);

    // The first file drops onto the lane under the cursor when that lane can
    // hold it; every other file (and a drop onto empty space or the wrong kind
    // of lane) makes a new track, stacked top-to-bottom.
    const QString laneTrack = trackIdForLane(laneAt(pos.y()));
    const daw::TrackModel* laneModel =
        laneTrack.isEmpty() ? nullptr
                            : m_controller->project().findTrack(laneTrack.toStdString());
    const daw::TrackKind laneKind = laneModel ? laneModel->kind
                                               : daw::TrackKind::Master;

    bool imported = false;
    bool firstUsedLane = false;
    for (const QString& file : files) {
        const bool isMidi = ui::isMidiFile(file);
        if (!isMidi && laneModel && laneKind == daw::TrackKind::Pattern) {
            imported |= !m_controller
                              ->addPatternSample(laneTrack.toStdString(),
                                                 file.toStdString(), start)
                              .empty();
            continue;
        }
        // MIDI wants a lane that takes notes; audio wants an audio lane.
        const bool laneFits =
            laneModel && (isMidi ? daw::trackAccepts(laneKind, daw::ClipKind::Midi)
                                 : laneKind == daw::TrackKind::Audio);

        QString trackId;
        if (!firstUsedLane && laneFits) {
            trackId = laneTrack;
            firstUsedLane = true;
        } else {
            const QString name = QFileInfo(file).completeBaseName();
            // A MIDI file lands on an *instrument* track: a bare MIDI lane
            // would arrive silent, with nothing for the notes to play through.
            trackId = QString::fromStdString(m_controller->addTrack(
                isMidi ? daw::TrackKind::Instrument : daw::TrackKind::Audio,
                name.toStdString()));
        }

        if (isMidi) {
            daw::midifile::File info;
            const std::vector<std::string> clips = m_controller->importMidiFile(
                file.toStdString(), trackId.toStdString(), start, &info);
            if (!clips.empty()) {
                imported = true;
                emit midiFileImported(file, info.firstTempoBpm, info.hasTempoChanges);
            }
        } else if (!m_controller
                        ->importAudio(file.toStdString(), trackId.toStdString(),
                                      start)
                        .empty()) {
            imported = true;
        }
    }

    ev->acceptProposedAction();
    if (imported) {
        emit projectEdited();
        emit tracksChanged();
    }
    update();
}
