#pragma once

#include <QPixmap>
#include <QRectF>
#include <QRegion>

#include "SelectionModel.hpp"
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ClipKind is an enum, so the ClipHit below needs the real definition rather
// than a forward declaration.
#include "model/Document.hpp"
#include "Host/PluginTypes.hpp"
#include "MidiPreviewIndex.hpp"

namespace daw { class EngineController; }
namespace daw { struct WaveformPeaks; }
namespace daw { struct RecordingSpan; struct RecordingPreview; }
namespace ui { class SelectionModel; }

/// The arrangement timeline: a bar/beat ruler, one lane per track with its clips
/// drawn as rounded rectangles, the grid at the division chosen in the transport
/// bar, and the playhead. With the Select tool clips can be selected, dragged
/// (snapping to the grid, Alt bypasses), trimmed by their edges and faded by
/// their top corners; several clips can be rubber-band-selected and moved or cut
/// together. Overlapping clips crossfade. Audio files can be dropped in from the
/// file manager. The Knife tool splits clips and the Eraser deletes them.
/// Empty space seeks; dragging in the ruler scrubs. Wheel scrolls, Ctrl+wheel
/// zooms, Shift+wheel moves through time, and dragging with the middle mouse
/// button grabs the arrangement in both axes. Rendered with QPainter — the
/// cross-platform replacement for the macOS Metal renderer.
class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    /// Edit tool chosen in the transport bar.
    ///   Select       — move clips, trim by their edges, fade by their top
    ///                  corners, adjust gain by the handle at the bottom.
    ///   Knife        — click a clip to split it (all selected clips, if several).
    ///   Eraser       — click (or drag across) clips to delete them.
    ///   SelectRegion — drag a time region; delete or drag its contents.
    /// Appended, never inserted: the transport's chip menu addresses a tool by its
/// index and stores that index, so renumbering one would silently change
/// everybody's chosen tool.
enum class Tool { Select, Knife, Eraser, SelectRegion, Mute, Draw };

    explicit TimelineWidget(daw::EngineController* controller,
                            QWidget* parent = nullptr);
    ~TimelineWidget() override;

    void setTool(Tool tool);
    /// The tool the modifier key borrows while it is held. Logic calls it the
    /// command-click tool: the pointer stays what it is, and one key turns it
    /// into the knife (or whatever else) for as long as it is down.
    void setSecondaryTool(Tool tool);
    void setSelectedTrack(const QString& id);
    /// Every selected lane, so a multi-track selection made in the headers is
    /// visible here too. The primary stays `setSelectedTrack`'s.
    void setSelectedTracks(const QStringList& ids);
    /// Mirror the clip selection into the shared model the context panel
    /// watches. The timeline keeps owning and drawing its own selection; this
    /// only publishes it.
    void setSelectionModel(ui::SelectionModel* model) { m_selectionModel = model; }
    /// Push the current clip selection into the shared SelectionModel. Cheap
    /// and idempotent — the model drops a push that changes nothing — so it is
    /// called once per interaction rather than at each of the ~20 sites that
    /// touch the selection. Public because selecting a clip also selects its
    /// track, and the shell has to publish the clips *before* it reacts to
    /// that, or the panel would flash a track context on the way through.
    void publishSelection();
    /// Scroll so the playhead stays on screen during playback.
    void ensurePlayheadVisible();
    /// Advance the playback cursor without invalidating the whole arrangement.
    /// A horizontal auto-scroll still requests a full frame because every
    /// timeline x changes in that case.
    void refreshPlaybackFrame();
    /// Repaint only lanes whose in-flight recording preview changed. The
    /// independent playhead clock owns the cursor itself.
    void refreshRecordingFrame();

    /// Grid division in beats (0 = off) and whether dragging snaps to it.
    void setGridBeats(double beats);
    void setSnapEnabled(bool enabled);
    /// The grid spacing in seconds while snapping is on, or 0 when it is off.
    /// Editors outside the arrangement ask for this so that what they snap to
    /// is the grid the user can actually see, at the current tempo.
    double snapSeconds() const;
    void setShowBars(bool showBars);

    void zoomBy(double factor);
    void zoomToFit();

    /// True when at least one clip is selected in the arrangement.
    bool hasClipSelection() const { return !m_selection.isEmpty(); }

    /// Select these clips from outside — the same state a click would leave,
    /// including what the selection model then publishes. Anything not found in
    /// the document is dropped rather than remembered.
    void selectClips(const QVector<ui::ClipSel>& clips);

    /// Horizontal span of the selected clips, in this widget's coordinates.
    ///
    /// False when nothing is selected. The span covers every selected clip, so
    /// a group answers with its full extent and the caller can centre on it.
    /// Clipped to the visible area: a clip scrolled half off-screen should pull
    /// what follows it toward the edge, not off it.
    bool selectionSpanX(int& left, int& right) const;
    /// Start time (seconds) of the primary selected clip, or −1 when none.
    double selectedClipStartSeconds() const;
    /// End time (seconds) of the primary selected clip, or −1 when none.
    double selectedClipEndSeconds() const;
    /// Delete every selected clip. Returns true if anything was selected.
    bool deleteSelectedClips();
    /// Standard arrangement clipboard commands. The clipboard keeps clip
    /// models (not rendered audio), so private FX, notes, fades and takes all
    /// survive a cut/paste.
    bool copySelection();
    bool cutSelection();
    bool pasteClipboard();
    /// Duplicate selected arrangement clips in place after their current span.
    /// Pattern clips bring every linked child MIDI clip with them.
    bool duplicateSelection();
    /// Logic-style Repeat: duplicate the selected clips/region by its own span,
    /// then select/advance to the new result so another press continues it.
    bool repeatSelection();
    /// Toggle the selected clips as a group. Returns false with no selection.
    bool toggleSelectedClipsMuted();
    /// Drop the clip selection without publishing it. Used when the user picks
    /// a track instead — the caller pushes the new track selection immediately
    /// afterwards, and publishing an empty clip list first would make the
    /// context panel close and reopen instead of changing over.
    void clearClipSelection();

    /// Open or close the comp editor on the primary selected clip (the E key).
    /// Returns false when nothing is selected or the clip has no takes.
    bool toggleSelectedClipExpanded();
    /// Open or close a clip's comp editor, animating the lane's growth.
    void toggleClipExpanded(const QString& trackId, const QString& clipId);
    /// Play the opening animation for a clip the *controller* expanded — a
    /// recording that finished with "auto-expand" on. The flag is already set,
    /// so toggling would close it; this only makes the lane grow into it.
    void animateClipOpen(const QString& trackId, const QString& clipId);
    /// Duplicate the take the comp currently plays at the playhead (Ctrl+D
    /// inside an open comp editor). Returns false when no comp editor is open.
    bool duplicateActiveTake();
    /// Hold-A auditioning: while set, clicking a take row plays it solo instead
    /// of comping it in.
    void setAuditionHeld(bool held);

    /// True when a region is committed in the SelectRegion tool (Delete acts on
    /// it instead of the clip selection).
    bool hasRegionSelection() const { return m_regionActive; }
    /// Delete the portions of clips inside the committed region.
    bool deleteRegionSelection();

    /// How far the lanes are scrolled down, in pixels. The arrangement is one
    /// tall stack of lanes inside a fixed-height widget: it scrolls rather than
    /// growing the window, and the header column follows this offset so the two
    /// columns can never drift apart.
    int verticalScroll() const { return m_scrollY; }
    void setVerticalScroll(int y);
    /// Put `y` back inside the legal range after the tracks, the widget or the
    /// mixer's cover changed. Emits only when it actually moves.
    void clampVerticalScroll();
    /// How much of the widget's bottom is hidden under the mixer. Scrolling is
    /// measured against what is left, so a track can always be brought into the
    /// visible strip without collapsing the mixer first.
    void setBottomInset(int px);
    /// Scroll the lane into view if it is off-screen (or under the mixer).
    void ensureLaneVisible(int lane);
    /// Round the arrangement's right-hand corners by this many pixels — how it
    /// meets the AI panel, which is a rounded plate of its own. Zero (the
    /// default) leaves the edge square, which is right when the arrangement
    /// runs to the window edge and there is nothing to meet.
    void setRightCornerRadius(int radius);
    /// Where a lane starts on screen, for the headless check that the header
    /// column is in step with the lanes.
    int laneTopForTest(int lane) const { return laneTop(lane); }
    int bottomInsetForTest() const { return m_bottomInset; }
    std::uint64_t staticFramePaintCountForTest() const {
        return m_staticFramePaintCount;
    }
    /// Horizontal navigation state for the headless middle-drag check.
    double horizontalScrollForTest() const { return m_scrollSeconds; }

    /// Headless check only: the vertical centre of one visible lane, so a test
    /// can click on a lane it has just created.
    int laneCentreForTest(int lane) const;

signals:
    void clipSelected(const QString& trackId, const QString& clipId);
    /// The cycle region was dragged out or moved. The window persists it and
    /// keeps the transport's Cycle button in step.
    void loopRangeChanged();
    /// A plugin was dropped out of the browser onto `target` — a track's name,
    /// or "the clip". The window says so in the status bar; without it the
    /// gesture succeeds invisibly.
    void pluginDropped(const QString& pluginName, const QString& target);
    /// A successfully dropped plugin should present its editor immediately,
    /// just like choosing it from an empty insert slot.
    void pluginEditorRequested(const QString& channelId,
                               const QString& insertId);
    /// A `.vltt` package was dropped into the arrangement. Import is routed
    /// through the window so selection, status and dirty state update together.
    void projectTemplateTracksRequested(const QString& path);
    /// The lanes scrolled vertically; the header column moves with them.
    void verticalScrollChanged(int y);
    void projectEdited();
    /// The arrangement sought the shared transport. Editors repaint their
    /// local playhead immediately even when playback is stopped.
    void playheadMoved();
    /// Emitted when the track set changed (e.g. a dropped file created tracks),
    /// so the header column and other views rebuild — not just repaint.
    void tracksChanged();
    /// A MIDI file was just imported, with what it said about tempo (0 when it
    /// said nothing). The shell decides whether to offer the project that
    /// tempo — the timeline has no business opening a dialog.
    void midiFileImported(const QString& path, double fileTempoBpm,
                          bool hasTempoChanges);
    /// A MIDI clip wants its editor: double-clicked, or freshly created on an
    /// empty MIDI lane. The shell owns the window, so the timeline only asks.
    void openPianoRollRequested(const QString& trackId, const QString& clipId);
    /// A Pattern clip or its parent lane was double-clicked.
    void openPatternRequested(const QString& patternId);
    /// A plain audio clip wants the shared Sample/Clip Editor.
    void openSampleEditorRequested(const QString& trackId, const QString& clipId);
    void openAutomationEditorRequested(const QString& trackId,
                                       const QString& clipId);
    /// Analyze exactly the audible range of an audio clip. The shell owns the
    /// worker/progress/result UI; the arrangement only contributes the target.
    void audioAnalysisRequested(const QString& trackId, const QString& clipId,
                                bool detectTempo, bool detectKey);
    /// Lane heights changed without the track set changing — the comp editor
    /// growing or shrinking. The header column re-reads its row heights so the
    /// two columns stay aligned on every frame of the animation.
    void laneHeightsChanged();

protected:
    bool event(QEvent*) override;   // trackpad pinch (native zoom gesture)
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(class QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void keyPressEvent(class QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(class QDragEnterEvent*) override;
    void dragMoveEvent(class QDragMoveEvent*) override;
    void dragLeaveEvent(class QDragLeaveEvent*) override;
    void dropEvent(class QDropEvent*) override;

private:
    enum class Edge { None, Left, Right };
    enum class Fade { None, In, Out };

    struct ClipHit {
        QString trackId;
        QString clipId;
        double startSeconds = 0.0;
        double durationSeconds = 0.0;
        double offsetSeconds = 0.0;
        double fadeInSeconds = 0.0;
        double fadeOutSeconds = 0.0;
        Edge edge = Edge::None;   // trim edge the pointer is over, if any
        Fade fade = Fade::None;   // fade handle the pointer is over, if any
        Fade fadeCurve = Fade::None; // midpoint curve handle, if any
        bool gainHandle = false;  // bottom-centre volume handle, if any
        daw::ClipKind kind = daw::ClipKind::Audio;
    };

    // A clip identified by both its track and its own id, so the selection
    // survives a clip being moved to another lane.
    struct ClipRef {
        QString trackId;
        QString clipId;
    };

    double xToSeconds(int x) const;
    int secondsToX(double seconds) const;
    double snap(double seconds, bool enabled) const;
    bool hitTestClip(const QPoint& pos, ClipHit& out) const;
    /// Arrangement controls preview continuously. These helpers remember only
    /// a history marker and let the controller record small, gesture-specific
    /// deltas at release. Dense MIDI/sample payloads never enter UI history.
    enum class ProjectGestureKind {
        GroupExisting,
        ClipPosition,
        ClipTrim,
        ClipGain,
        ClipFade,
        ClipFadeCurve,
    };
    void beginProjectGesture(
        const QString& label,
        ProjectGestureKind kind = ProjectGestureKind::GroupExisting);
    void markProjectGestureChanged();
    void finishProjectGesture();
    void beginClipPositionGesture(const QString& label);
    void cancelProjectGesture();

    // ── The cycle region ──
    /// What a press in the cycle strip is doing.
    enum class LoopGrab { None, Create, Move, ResizeStart, ResizeEnd };
    /// Which part of the cycle region `x` is over, given that the pointer is
    /// already known to be in the strip.
    LoopGrab loopGrabAt(int x) const;
    void drawCycleStrip(QPainter& p);
    /// The plugin a drag is carrying, resolved through the plugin manager, or
    /// nothing when the drag is not carrying one.
    std::optional<daw::plugins::PluginDescriptor> pluginFromMime(
        const class QMimeData* mime) const;
    int laneAt(int y) const;                 // lane index under y, or -1
    int laneTop(int lane) const;             // y of a lane's top edge
    int laneHeightAt(int lane) const;        // a lane's on-screen height
    /// The strip of a lane the clip bodies live in: the whole lane normally, and
    /// just the top part when an expanded comp editor has grown the lane.
    int laneBodyHeightAt(int lane) const;
    int lanesBottom() const;                 // y below the last lane
    /// Total height of every visible lane, ignoring the scroll offset.
    int lanesHeight() const;
    /// Lane area actually on screen: the widget minus the ruler and whatever
    /// the mixer covers.
    int visibleLaneHeight() const;
    int maxVerticalScroll() const;
    QString trackIdForLane(int lane) const;  // track id for a lane, or empty
    QRectF clipRect(int lane, const daw::ClipModel& clip) const;
    /// The comp editor's area under an expanded clip — empty when collapsed.
    QRectF compRect(int lane, const daw::ClipModel& clip) const;
    /// Sub-lane of take `index` inside `comp`, clipped to it.
    QRectF takeRowRect(const QRectF& comp, int index) const;
    /// The stacked-rectangles take badge at a clip's top-left corner.
    QRectF badgeRect(const QRectF& body) const;
    /// Run the expand/collapse animation for a track's lane. Drives
    /// `ui::setCompFactor` and asks the header column to follow along; clears the
    /// clip's `expanded` flag once a collapse has finished playing.
    void animateComp(const QString& trackId, const QString& clipId,
                     bool opening);

    /// A take sub-lane under the pointer inside an expanded clip.
    struct TakeHit {
        QString trackId;
        QString clipId;
        QString takeId;
        int takeIndex = -1;
        QRectF row;
    };

    /// The clip behind a (track, clip) id pair, or null when either is gone.
    const daw::ClipModel* findClipModel(const QString& trackId,
                                        const QString& clipId) const;
    /// One frame of the expand/collapse animation.
    void stepCompAnimation();

    bool hitTestTake(const QPoint& pos, TakeHit& out) const;
    /// Everything a take row can do that is not "swipe" or "use all of it":
    /// rename, recolour, audition, reorder, delete.
    void showTakeMenu(const TakeHit& hit, const QPoint& globalPos);
    /// Ask for a new name for a take and apply it.
    void renameTake(const TakeHit& hit);
    /// Paint one take's sub-lane: mini waveform/notes drawn in the clip's own
    /// time, the highlight over the stretches the comp takes from it, and a
    /// floating name badge.
    void drawTakeRow(QPainter& p, const daw::TrackModel& track,
                     const daw::ClipModel& clip, const daw::TakeModel& take,
                     int index, const QRectF& row, double reveal,
                     std::uint64_t midiNotesRevision);
    /// The whole comp editor under an expanded clip: take rows plus the brush
    /// feedback of a swipe in flight.
    void drawCompEditor(QPainter& p, int lane, const daw::TrackModel& track,
                        const daw::ClipModel& clip, const QRectF& comp,
                        std::uint64_t midiNotesRevision);
    /// The stacked-rectangles badge with the take count, top-left of a clip.
    void drawTakeBadge(QPainter& p, const daw::ClipModel& clip,
                       const QRectF& body);
    /// Paint the assembled comp across a clip's body: one waveform piece per
    /// segment, in its take's colour, with the seams marked.
    void drawCompLane(QPainter& p, const daw::TrackModel& track,
                      const daw::ClipModel& clip, const QRectF& area,
                      std::uint64_t midiNotesRevision);
    /// Extend the swipe in flight to the pointer, handing the swept range to the
    /// take being brushed.
    void updateSwipe(const QPoint& pos, bool snapOn);
    bool isClipSelected(const QString& clipId) const;
    /// Whether the pointer is inside the committed region (time + lanes).
    bool regionContains(const QPoint& pos) const;
    /// Reset all SelectRegion state (region gone, nothing moving).
    void clearRegion();
    /// Refresh the region being dragged from the pointer; snaps edges when on.
    void updateRegionFromDrag(const QPoint& pos, bool snapOn);
    /// Split the clips touching the committed region into standalone pieces and
    /// remember them for dragging.
    void collectRegionPieces();
    /// The immutable identity and timeline geometry needed while region edits
    /// split/remove clips. Keeping this snapshot small matters for MIDI clips:
    /// copying a ClipModel here would also copy every note, lane and take merely
    /// to survive vector invalidation.
    struct RegionClipCandidate {
        std::string trackId;
        std::string clipId;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
    };
    /// Snapshot the clips intersecting a region before the first structural
    /// edit invalidates the project's clip vectors.
    std::vector<RegionClipCandidate> regionClipCandidates(double r0,
                                                           double r1) const;
    /// Draw the committed/dragged region: a light box plus a glow over the
    /// clip portions inside it.
    void drawRegion(QPainter& p);
    /// Split a clip so the part inside [r0, r1] becomes its own clip; returns
    /// that piece's id (empty when the clip doesn't touch the region). The
    /// outer fragments keep their original ids.
    std::string regionInnerPiece(const RegionClipCandidate& clip, double r0,
                                 double r1) const;
    void updateCursor(const QPoint& pos);
    void drawRuler(class QPainter& p);
    void drawGrid(QPainter& p);
    void drawLanes(QPainter& p);
    /// Everything except the moving playhead/count-in. Playback restores this
    /// cached layer under the old/new cursor strips instead of traversing the
    /// project at 60 Hz.
    void drawStaticFrame(QPainter& p, const QRegion& paintRegion);
    /// Draw persistent Pattern clips on the parent lane, with linked child MIDI
    /// notes as their compact overview. The parent lane stays visible expanded.
    /// One automation lane's clips: the body, and the curve drawn inside it.
    void drawAutomationClips(QPainter& p, const daw::TrackModel& track,
                             int laneTop, int bodyHeight);
    /// A breakpoint under the pointer, or the segment it is over.
    struct PointHit {
        QString trackId;      ///< the automation lane
        QString clipId;
        int index = -1;       ///< breakpoint under the pointer, or −1
        int segment = -1;     ///< segment the pointer is over, or −1
        double beats = 0.0;   ///< where the pointer is, clip-relative
        double value = 0.0;   ///< and at what height
        QRectF body;          ///< the clip's interior, for value ↔ y
    };
    /// What is under `pos` on an automation lane. False when the pointer is not
    /// over an automation clip at all.
    bool hitTestAutomationPoint(const QPoint& pos, PointHit& out) const;
    /// The index of the breakpoint sitting at `beats`, or −1. Used to re-find a
    /// point after the controller has sorted the vector under us.
    bool isOverAutomation(const QPoint& pos) const;
    void showAutomationMenu(const PointHit& hit, const QPoint& globalPos);
    static int indexOfPointAt(const daw::ClipModel& clip, double beats);
    /// One step of a freehand stroke: put a point where the pointer is and
    /// clear whatever the stroke has just passed over.
    void paintCurveAt(const QPoint& pos, bool snapOn);
    /// Make one automation clip the selection, so the context panel and the
    /// clip commands act on the curve being edited.
    /// The strip along the top of an automation clip that drags the clip rather
    /// than editing its curve — the clip's name lives there.
    static QRectF automationGrip(const QRectF& body);
    void selectAutomationClip(const QString& trackId, const QString& clipId);
    /// Snap a clip-relative beat to the grid.
    double snapBeats(double beats, bool enabled) const;

    /// Where a normalised value sits inside a clip's body, and the inverse.
    /// Everything the curve editor does goes through this pair, so the drawing
    /// and the hit-testing can never disagree about where a point is.
    double automationValueToY(const QRectF& body, double value) const;
    double automationValueAtY(const QRectF& body, double y) const;
    /// The clip-relative beat at a timeline x, for a clip that starts there.
    double automationBeatsAtX(const daw::ClipModel& clip, int x) const;

    void drawPatternClips(QPainter& p, const daw::TrackModel& pattern,
                          int laneTop, int bodyHeight);
    void drawWaveform(QPainter& p, const daw::ClipModel& clip,
                      const QRectF& area);
    /// The envelope of an already-decoded peak set across `area`, where the
    /// source time at the area's left edge is `sourceStartSeconds`. The one
    /// waveform routine: clip bodies, take sub-lanes and the comp lane all go
    /// through it, so the same audio drawn twice lines up pixel for pixel.
    ///
    /// `timeStretch` is how much longer the material is on the timeline than it
    /// is in the file — 2.0 means one second of source covers two seconds of
    /// track. Without it a stretched clip grows but its waveform does not, so
    /// the picture stops describing the audio the moment the Time knob moves.
    void drawPeaks(QPainter& p, const daw::WaveformPeaks* peaks,
                   double sourceStartSeconds, const QRectF& area, float gain,
                   const QColor& color, double timeStretch = 1.0);
    /// One take's material inside `area` (the clip's full width), placed at the
    /// take's own offset into the clip.
    void drawTakeAudio(QPainter& p, const daw::ClipModel& clip,
                       const daw::TakeModel& take, const QRectF& area,
                       const QColor& color,
                       std::uint64_t midiNotesRevision);
    /// A MIDI clip's notes, filling `area` the way a waveform fills an audio
    /// clip: white rounded rects, auto-fitted to the pitches actually present.
    /// `area` is where the notes go (below the caption); `body` is the whole
    /// clip rect, used to clip them to its rounded outline.
    void drawMidiNotes(QPainter& p, const daw::ClipModel& clip,
                       const QRectF& area, const QRectF& body,
                       std::uint64_t midiNotesRevision);
    /// Reuse the clip/take's beat-sorted preview geometry until its owning
    /// track changes. Stable document ids make scroll and partial repaints
    /// allocation-free after the first frame.
    struct MidiPreviewCacheEntry;
    const daw::MidiPreviewIndex& midiPreviewIndex(
        const std::string& clipId, const std::string& ownerId,
        std::span<const daw::NoteModel> notes,
        std::uint64_t midiNotesRevision);
    void trimMidiPreviewCache(const MidiPreviewCacheEntry* keepEntry);
    /// Bounding box of the cursor line and its ruler handle at `x`.
    QRect playheadDirtyRect(int x) const;
    void drawFades(QPainter& p, const daw::ClipModel& clip, const QRectF& r);
    /// The take running on `lane`, drawn as the clip it is about to become:
    /// the track's colour (or the colour of the take it will land as, when a
    /// layer is being punched into an existing clip), the same rounded body and
    /// caption a landed clip has, and a record-red rim that breathes so it is
    /// never mistaken for one. Loop recording draws one body per pass.
    void drawRecordingClip(QPainter& p, const daw::TrackModel& track,
                           int top, int height);
    /// The layer being recorded into an expanded clip, drawn as the row it is
    /// about to become at the bottom of that clip's take stack.
    void drawRecordingTakeRow(QPainter& p, const daw::TrackModel& track,
                              const daw::ClipModel& clip, const QRectF& row);
    /// The input's envelope inside one pass of a running take. Buckets are
    /// placed by the time they were *captured* at, which is what stops the
    /// shape from sliding backwards under the write head as the take grows.
    void drawRecordingEnvelope(QPainter& p, const daw::RecordingPreview& preview,
                               const daw::RecordingSpan& span,
                               const QRectF& area, const QRectF& body,
                               const QColor& color);
    /// The count-in, over the middle of the arrangement: the beats left before
    /// the punch point, big enough to be read without looking away from the
    /// instrument.
    void drawCountIn(QPainter& p);
    /// The small volume-handle circle at the bottom-centre of a clip.
    void drawGainHandle(QPainter& p, const daw::ClipModel& clip,
                        const QRectF& r, bool sel);
    void drawPlayhead(QPainter& p);

    daw::EngineController* m_controller;
    QString m_selectedTrackId;
    QStringList m_selectedTrackIds;
    /// The clip a plugin drag is hovering, so the drop feedback says which of
    /// the clip and the lane will take it.
    ClipRef m_dropClip;

    /// The cycle drag in progress, and what it started from. The anchor is the
    /// edge that stays put while the other one follows the pointer, so dragging
    /// back past the start flips the region rather than collapsing it.
    /// The breakpoint gesture in flight, and the curve as it stood when the
    /// gesture began — the live-edit / commit-on-release split every curve
    /// editor here uses.
    PointHit m_pointDrag;
    bool m_draggingPoint = false;
    bool m_drawingCurve = false;

    bool m_bendingSegment = false;
    double m_bendStartY = 0.0;
    double m_bendStartCurve = 0.0;
    std::vector<daw::AutomationPoint> m_pointsBefore;
    /// Normalized curve at point-pickup time. Downward drags are rebuilt from
    /// it on every move so their guard point is stable and Shift can keep the
    /// exact original value while the handle crosses neighbours.
    std::vector<daw::AutomationPoint> m_dragPoints;

    LoopGrab m_loopGrab = LoopGrab::None;
    double m_loopAnchorSeconds = 0.0;
    double m_loopGrabOffset = 0.0;
    double m_loopGrabLength = 0.0;
    QString m_selectedClipId;          // primary selection (for the inspector)
    QVector<ClipRef> m_selection;      // every selected clip
    ui::SelectionModel* m_selectionModel = nullptr;
    /// Vertical scroll of the lane stack, in pixels; 0 is the first lane at the
    /// top. Horizontal scrolling is in seconds (`m_scrollSeconds`) because time
    /// is the axis there — lanes have no unit but pixels.
    int m_scrollY = 0;
    /// How many pixels at the bottom are covered by the mixer overlay.
    int m_bottomInset = 0;
    /// Radius of the right-hand corners; 0 while the edge is square.
    int m_rightRadius = 0;
    double m_pixelsPerSecond = 80.0;
    double m_scrollSeconds = 0.0;   // time at the left edge
    /// Last cursor x requested or painted. Playback invalidates this narrow
    /// strip and the new one instead of repainting every lane.
    std::optional<int> m_lastPlayheadX;
    QPixmap m_staticFrame;
    bool m_staticFrameValid = false;
    std::uint64_t m_staticFramePaintCount = 0;
    struct MidiPreviewCacheEntry {
        daw::MidiPreviewIndex index;
        std::uint64_t revision = 0;
        std::uint64_t lastUse = 0;
        std::size_t noteCount = 0;
    };
    // The index stores only note order plus one maximum per 128 notes. Keep
    // enough for several dense clips, then evict least-recently painted owners
    // so deleted/hidden clips cannot leave an unbounded GUI cache behind.
    static constexpr std::size_t kMidiPreviewCacheNoteBudget = 256 * 1024;
    static constexpr std::size_t kMidiPreviewCacheOwnerBudget = 256;
    using MidiPreviewOwners =
        std::unordered_map<std::string, MidiPreviewCacheEntry>;
    std::unordered_map<std::string, MidiPreviewOwners> m_midiPreviewCache;
    std::size_t m_midiPreviewCachedNotes = 0;
    std::size_t m_midiPreviewCachedOwners = 0;
    std::uint64_t m_midiPreviewUseCounter = 0;
    /// Exact regions requested by refreshPlaybackFrame but not painted yet.
    /// Any paint containing pixels outside this set is a real content repaint
    /// and refreshes the static cache for those pixels.
    QRegion m_playbackOnlyDirty;
    double m_gridBeats = 0.25;      // 1/16 by default
    bool m_snapEnabled = true;
    bool m_showBars = true;

    Tool m_tool = Tool::Select;
    Tool m_secondaryTool = Tool::Knife;
    /// True while the modifier is down, as read off the last input event. The
    /// tool every gesture actually uses is `tool()`, never `m_tool`.
    bool m_altToolHeld = false;
    /// The tool in force right now.
    Tool tool() const { return m_altToolHeld ? m_secondaryTool : m_tool; }
    /// Re-read the modifier from an event; returns true when it changed, so the
    /// caller can repaint the cursor.
    bool trackAltTool(Qt::KeyboardModifiers modifiers);

    // Drag state.
    // Middle-button pan is deliberately independent of the selected edit tool:
    // it is navigation, so grabbing the canvas must never select, seek or edit
    // a clip underneath it.
    bool m_panning = false;
    QPoint m_panLastPosition;
    bool m_dragging = false;
    bool m_scrubbing = false;
    bool m_projectGestureActive = false;
    ProjectGestureKind m_projectGestureKind = ProjectGestureKind::GroupExisting;
    std::uint64_t m_projectGestureUndoGroupId = 0;
    QString m_projectGestureLabel;
    bool m_projectGestureChanged = false;
    bool m_clipPositionEditOpen = false;
    bool m_clipTrimEditOpen = false;
    QString m_dragTrackId;
    QString m_dragClipId;
    double m_dragGrabOffset = 0.0;  // pointer seconds − clip start
    // Original starts of every selected clip at drag start, so the whole group
    // moves by the same delta. Pairs of (clipId, startSeconds).
    QVector<QPair<QString, double>> m_dragOrigStarts;

    // Trim (edge-drag) state, captured at press so the whole gesture stays
    // relative to the clip's original geometry.
    bool m_trimming = false;
    Edge m_trimEdge = Edge::None;
    QString m_trimTrackId;
    QString m_trimClipId;
    double m_trimOrigStart = 0.0;
    double m_trimOrigOffset = 0.0;
    double m_trimOrigDuration = 0.0;

    // Fade (corner-drag) state.
    bool m_fading = false;
    Fade m_fadeSide = Fade::None;
    QString m_fadeTrackId;
    QString m_fadeClipId;
    double m_fadeStart = 0.0;
    double m_fadeDuration = 0.0;
    double m_fadeOtherSeconds = 0.0;  // the fade on the *other* end, held fixed
    double m_fadeOriginalIn = 0.0;
    double m_fadeOriginalOut = 0.0;
    bool m_fadeCurving = false;
    int m_fadeCurveDragY = 0;
    double m_fadeCurveOriginal = 0.0;

    // Gain (volume) handle drag state. Captured on press so the whole drag
    // stays relative to the original gain; applied to every selected clip.
    bool m_gainDragging = false;
    int m_gainDragY = 0;
    struct GainRef {
        QString trackId;
        QString clipId;
        float origGain = 1.0f;
        float origPan = 0.0f;
    };
    QVector<GainRef> m_gainOrig;

    // Marquee (rubber-band) selection.
    bool m_marqueeActive = false;
    QPoint m_marqueeOrigin;
    QPoint m_marqueeCurrent;

    // SelectRegion (time-selection) tool. A committed region spans a time range
    // across one or more lanes; its contents can be deleted or dragged.
    bool m_regionActive = false;
    bool m_regionPicking = false;   // mouse down, defining a region
    double m_regionStart = 0.0;     // seconds (snapped when enabled)
    double m_regionEnd = 0.0;
    int m_regionLaneA = -1;         // inclusive lane range, -1 = none
    int m_regionLaneB = -1;
    QPoint m_regionOrigin;
    QPoint m_regionCurrent;
    // Moving the committed region: the contents are split out on the first
    // move and the pieces travel with it.
    bool m_regionMovePending = false;
    bool m_regionMoving = false;
    QPoint m_regionPressPos;
    double m_regionMoveGrab = 0.0;          // pointer seconds at move start
    double m_regionMoveOrigStart = 0.0;
    double m_regionMoveOrigEnd = 0.0;
    // The region the pointer last went down in. Qt delivers a double-click as
    // press → release → double-click, and the release has already cleared the
    // region by then — so its geometry is kept here for the double-click that
    // may still be coming. Lane A of −1 means "nothing remembered".
    double m_lastRegionStart = 0.0;
    double m_lastRegionEnd = 0.0;
    int m_lastRegionLaneA = -1;
    int m_lastRegionLaneB = -1;
    struct RegionPiece {
        QString trackId;
        QString clipId;
        double origStart = 0.0;
    };
    std::vector<RegionPiece> m_regionPieces;

    // External file drag hover: the lane the drop would land on (−1 = below all).
    bool m_dropActive = false;
    int m_dropLane = -1;

    // Eraser drag: clips already deleted in this stroke, so we don't re-hit.
    bool m_erasing = false;

    // Comp editor. Expanding grows the lane (see CompLayout.hpp), so the
    // animation is per *track* — one timer walks the factor and both this widget
    // and the header column re-read it each frame.
    class QTimer* m_compTimer = nullptr;
    QString m_compAnimTrackId;
    QString m_compAnimClipId;
    bool m_compAnimOpening = false;
    double m_compAnimFrom = 0.0;
    double m_compAnimElapsedMs = 0.0;

    // Vertical drag on a clip's bottom edge, the third way to open the editor.
    bool m_expandDrag = false;
    QString m_expandTrackId;
    QString m_expandClipId;
    int m_expandPressY = 0;

    // Swipe comping: a stroke across the take rows of an expanded clip. The
    // whole stroke is one undo step, opened by the controller's beginCompEdit.
    bool m_swiping = false;
    QString m_swipeTrackId;
    QString m_swipeClipId;
    QString m_swipeTakeId;
    double m_swipeFromSeconds = 0.0;   // clip-relative, where the stroke began
    double m_swipeToSeconds = 0.0;
    // Hold-A audition: the take being previewed, restored when the key drops.
    bool m_auditionHeld = false;
    QString m_auditionTakeId;

};
