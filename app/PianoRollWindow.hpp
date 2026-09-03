#pragma once

#include "MidiPreviewIndex.hpp"
#include "MidiTools.hpp"
#include "NoteContextPanel.hpp"
#include "CollaborationTypes.hpp"
#include "model/Document.hpp"

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace daw { class EngineController; }
namespace ui { class MsrButton; }

class ArpeggiatorDialog;
class ArticulateDialog;
class ChordDialog;
class GlueDialog;
class QAbstractButton;
class QAction;
class QActionGroup;
class QButtonGroup;
class QComboBox;
class QHideEvent;
class QLabel;
class QMenu;
class QScrollBar;
class QShowEvent;
class QTimer;
class QToolButton;
class QuantizeDialog;
class RandomizeDialog;
class StrumDialog;
class ToolDialog;

/// The note canvas: a piano keyboard down the left edge, a bar/beat grid, the
/// clip's notes, and a velocity lane pinned along the bottom.
///
/// Keyboard, grid and velocity lane are all one widget rather than three. The
/// keys must scroll in exact vertical lockstep with the note rows, and the
/// velocity lane must line up horizontally with them to the pixel — separate
/// widgets would mean a hand-wired shared scrollbar plus a width that differs
/// by whatever the scroll area's scrollbar happens to occupy. The window's two
/// scrollbars sit *outside* this widget, so they take width and height from the
/// whole thing at once and the three parts can never drift apart.
///
/// The clip is addressed by id, never by pointer: the document can be rewritten
/// underneath the window by an undo, a reload, or the track being deleted, and
/// every `ClipModel*`/`NoteModel*` would dangle. `clip()` re-resolves on each
/// use and returns null once the clip is gone, which the paint and input paths
/// all check.
class PianoRollView : public QWidget {
    Q_OBJECT
public:
    /// What the left mouse button does on the grid.
    ///
    /// Erase is deliberately *not* here: like FL, the right button erases in
    /// every mode, so there is never a reason to switch tools to delete a note.
    enum class Tool { Draw, Select, Slice, Mute };

    /// How a note is painted. Purely cosmetic — nothing here changes a note.
    enum class NoteStyle {
        Rounded,   ///< a clean solid body with smooth, antialiased corners
        Flat,      ///< a pixel-aligned rectangle with square corners
    };

    /// What the lane along the bottom is editing.
    enum class LaneParam {
        Velocity,
        Pan,
        Controller,   ///< one of the clip's automation curves, by `laneId()`
    };

    /// Where a note's colour comes from.
    enum class ColorMode {
        Clip,       ///< the clip's own colour — one colour for the whole part
        Velocity,   ///< cool for soft, hot for loud
        Pitch,      ///< around the colour wheel once per octave
        Custom,     ///< the note's own colour, falling back to the clip's
    };

    explicit PianoRollView(daw::EngineController* controller,
                           QWidget* parent = nullptr);
    /// Ends a keyboard audition that outlived the window (closed with a key
    /// held), which would otherwise leave the synth holding the note.
    ~PianoRollView() override;

    void setClip(const QString& trackId, const QString& clipId);
    /// Scroll the notes into view (or middle C on an empty clip) and fit the
    /// clip's length to the width.
    void scrollToContent();

    // ── Grid and snap ──
    /// The division the user picked. `effectiveGridBeats()` is what actually
    /// gets used, which differs when adaptive snap is on.
    void setGridBeats(double beats);
    double gridBeats() const { return m_gridBeats; }
    double effectiveGridBeats() const;
    void setSnapEnabled(bool enabled);
    bool snapEnabled() const { return m_snapEnabled; }
    void setAdaptiveSnap(bool enabled);
    void setSnapToScale(bool enabled);
    bool snapToScale() const { return m_snapToScale; }
    /// 0.5 = straight. Shifts the odd grid slots, and the drawn grid with them,
    /// so notes land where the lines actually are.
    void setSwing(double swing);
    double swing() const { return m_swing; }
    void setScale(int root, daw::miditools::Scale scale);
    int scaleRoot() const { return m_scaleRoot; }
    daw::miditools::Scale scale() const { return m_scale; }

    // ── Tools ──
    void setTool(Tool tool);
    Tool tool() const { return m_tool; }

    // ── View options ──
    void setShowKeyboard(bool show);
    void setShowVelocityLane(bool show);
    void setShowNoteNames(bool show);
    void setNoteBorders(bool show);
    void setNoteStyle(NoteStyle style);
    NoteStyle noteStyle() const { return m_noteStyle; }
    /// Name every key, not only the Cs.
    void setShowAllKeyNames(bool show);
    void setScaleHighlight(bool show);
    void setColorMode(ColorMode mode);
    ColorMode colorMode() const { return m_colorMode; }
    /// 0 = barely-there grid lines, 1 = hard ones.
    void setGridContrast(double contrast);
    /// An invalid colour hands the grid back to the theme.
    void setGridColor(const QColor& color);
    QColor gridColor() const { return m_gridColor; }
    /// Other MIDI tracks to draw underneath, as dimmed reference material.
    void setGhostTracks(const QSet<QString>& trackIds);
    QSet<QString> ghostTracks() const { return m_ghostTracks; }
    void setFollowPlayback(bool follow);
    bool followPlayback() const { return m_followPlayback; }
    /// Repaint only the old/new playhead slivers and keyboard keys whose held
    /// state changed. Called by the window's 30 Hz transport timer.
    void refreshPlayheadFrame();
    /// Pitches currently held by live computer/hardware input for this track.
    void setLivePitches(const std::bitset<128>& pitches);

    // ── The parameter lane along the bottom ──
    /// Point the lane at velocity, pan, or one of the clip's controller curves.
    void setLaneParam(LaneParam param, const QString& laneId = {});
    LaneParam laneParam() const { return m_laneParam; }
    QString laneId() const { return m_laneId; }
    void setLaneHeight(double px);
    double laneHeightPx() const { return m_laneHeight; }

    // ── Zoom and scroll ──
    void zoomHorizontal(double factor);
    /// Set horizontal zoom with bar one fixed at the left edge. Used by the
    /// overview scrollbar's right handle: shortening the overview moves into
    /// the song from its beginning, never from the viewport centre.
    void setHorizontalZoomFromStart(double pixels);
    void zoomVertical(double factor);
    /// Pixels per beat. Zero means "fit the clip to the width", which is what a
    /// roll that has never been zoomed by hand does.
    void setPixelsPerBeat(double px);
    double pixelsPerBeat() const { return m_pxPerBeat; }
    /// Actual rendered width, including the automatic fit used while the
    /// stored value is the zero sentinel.
    double effectivePixelsPerBeat() const;
    /// Height of one semitone row.
    void setRowHeight(double px);
    double rowHeight() const { return m_rowHeight; }
    void zoomToFit();
    void zoomToSelection();
    double scrollX() const { return m_scrollX; }
    double scrollY() const { return m_scrollY; }
    double maxScrollX() const;
    double maxScrollY() const;
    void setScrollX(double x);
    void setScrollY(double y);

    /// Headless regression check for the three pointer invariants most easily
    /// broken by event coalescing: swept erasing, grid-safe brush length and a
    /// stretch handle that only belongs to a multi-note selection.
    bool checkInteractionGesturesForTest();

    /// Privacy-safe collaboration mapping for the application-owned note
    /// canvas. Beat/pitch/value survive different zoom, scroll and lane sizes;
    /// the normalised point remains a safe fallback for chrome regions.
    collab::SemanticPoint collaborationPresenceAt(
        const QPointF& position) const;
    std::optional<QPointF> collaborationPositionFor(
        const collab::SemanticPoint& point) const;
    static bool checkCollaborationPresenceForTest(QString* error = nullptr);

    // ── Selection ──
    void selectAll();
    void selectNone();
    void invertSelection();
    /// Every note sharing a colour with the current selection — the fast way to
    /// grab "all the hats" once parts have been colour-grouped.
    void selectSameColor();
    bool hasSelection() const { return !m_selected.isEmpty(); }
    int selectionCount() const { return int(m_selected.size()); }

    // ── Commands ──
    void deleteSelection();
    void copySelection();
    void cutSelection();
    void paste();
    void duplicateSelection();
    bool canPaste() const;

    /// Run a transform over the selection — or over the whole clip when nothing
    /// is selected — and record it as one undo entry named `label`.
    ///
    /// Notes the transform returns without an id are given one here rather than
    /// in the controller, so the view knows which notes to leave selected.
    void applyTransform(
        const std::function<daw::miditools::Notes(const daw::miditools::Notes&)>&
            transform,
        const QString& label);
    /// Draw what `transform` would do without touching the document. The grid
    /// goes read-only until `clearPreview()`.
    void previewTransform(
        const std::function<daw::miditools::Notes(const daw::miditools::Notes&)>&
            transform);
    bool hasPreview() const { return m_preview.has_value(); }
    /// Commit the already-painted preview as one undoable edit. No transform is
    /// run here: Apply must preserve the exact notes the user is looking at.
    bool commitPreview(const QString& label);
    void clearPreview();

    // ── Edits the context panel drives ──
    //
    // The continuous three update live, then fold the whole drag into one undo
    // entry. Discrete commands already land as one entry through setClipNotes.
    void beginSelectionEdit();
    void endSelectionEdit(const QString& label);
    /// Snapshot the selected notes before the context-panel velocity gesture.
    /// Subsequent values are interpreted as a delta from the displayed group
    /// average, so differently played notes keep their dynamics.
    void beginSelectionVelocityEdit();
    void setSelectionVelocity(int velocity);
    void endSelectionVelocityEdit();
    void setSelectionPan(float pan);
    void setSelectionLength(double beats);
    void setSelectionColor(uint32_t rgb);
    void setSelectionMuted(bool muted);
    void transposeSelection(int semitones);

    /// What the selection looks like as one set of values. For several notes
    /// the continuous fields are averages, which is what a control showing one
    /// number for a group can honestly display.
    struct SelectionSummary {
        int count = 0;
        int velocity = 100;
        float pan = 0.0f;
        double lengthBeats = 1.0;
        int pitch = 60;
        bool muted = false;
        uint32_t color = 0;
    };
    SelectionSummary selectionSummary() const;
    /// Identity of the current selection — same notes, same string.
    QString selectionKey() const;

    /// The notes a command would act on: the selection, or everything.
    daw::miditools::Notes targetNotes() const;
    /// Every note in the clip, selected or not — the reference a command needs
    /// when what it does to one note depends on its neighbours.
    daw::miditools::Notes clipNotes() const;
    /// The clip's length in beats — what "fill the clip" means to a tool.
    double clipBeats() const;
    /// The cycle a rotate turns within: the clip when nothing is selected, and
    /// otherwise the selection's span rounded up to a whole bar, so a pattern
    /// stays on the grid however short its last note is.
    double rotateSpanBeats() const;

signals:
    /// One gesture finished and changed the document. Emitted on release, not
    /// per mouse-move, so the shell repaints and marks dirty once.
    void edited();
    void selectionChanged();
    /// Zoom or scroll moved; the window's scrollbars follow this.
    void viewportChanged();
    /// A short line for the status strip.
    void statusChanged(const QString& text);
    /// The local ruler sought the project transport. The enclosing window
    /// forwards this so the arrangement can repaint even while stopped.
    void playheadMoved();
    /// The cycle region was dragged out here. There is one region for the whole
    /// project, so the arrangement and the transport's lamp have to follow.
    void loopRangeChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    /// Native trackpad gestures (pinch to zoom, smart-zoom to fit) arrive here
    /// rather than as wheel events, so they need their own hook.
    bool event(QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void leaveEvent(QEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    friend class PianoRollWindow;

    /// The clip this window edits, or null when it no longer exists.
    const daw::ClipModel* clip() const;
    const daw::NoteModel* note(const QString& noteId) const;
    /// What to draw and hit-test against: the preview when one is up, otherwise
    /// the clip's own notes.
    const daw::miditools::Notes& visibleNotes() const;

    // ── Geometry ──
    double keyboardWidth() const;
    double laneHeight() const;
    double fieldHeight() const;
    double laneTop() const;
    double contentHeight() const;
    void clampScroll();

    double pxPerBeat() const;
    double xToBeats(double x) const;
    double beatsToX(double beats) const;
    int yToPitch(double y) const;
    double pitchToY(int pitch) const;
    double snapBeats(double beats, bool enabled) const;
    int snapPitch(int pitch) const;
    QRectF noteRect(const daw::NoteModel& n) const;
    QPointF laneHandle(const daw::NoteModel& n) const;
    /// The lane's value for a note, normalised 0 … 1 — velocity or pan, mapped
    /// so the drawing and hit-testing code never has to know which.
    double laneValueOf(const daw::NoteModel& n) const;
    /// Write a normalised value back to whichever parameter the lane is on.
    void setLaneValueOf(const QString& noteId, double value);
    double laneValueAtY(double y) const;
    double laneValueToY(double value) const;
    int velocityAtY(double y) const;
    /// The clip's currently selected controller curve, or null.
    const daw::ControllerLane* controllerLane() const;
    /// Index of the breakpoint under the point, or −1.
    int lanePointAt(const QPointF& pos) const;
    /// Retain only the newest raw pointer sample for a controller-point drag.
    /// The timer writes the reusable gesture vector to the model once per frame;
    /// mouse release calls the same flush synchronously for the exact endpoint.
    void queueControllerLanePoint(const QPointF& pos, bool snapEnabled);
    bool flushControllerLaneWrite();
    void cancelControllerLaneWrite();
    QColor colorFor(const daw::NoteModel& n, const QColor& clipColor) const;

    QString noteAt(const QPointF& pos, bool* onRightEdge,
                   bool* onLeftEdge = nullptr) const;
    QString handleAt(const QPointF& pos) const;
    const daw::MidiPreviewIndex& notePaintIndexFor(
        const daw::miditools::Notes& notes) const;
    void ensureDocumentNoteIdIndex(const daw::ClipModel& clip) const;
    bool freezesDocumentNoteIndex() const noexcept;
    const std::vector<daw::NoteModel>* liveGeometryNotes() const noexcept;
    void updateCursor(const QPointF& pos);
    /// The tool a gesture should use right now — the held-key override if one
    /// is active, otherwise the chosen tool.
    Tool activeTool() const;

    void selectOnly(const QString& noteId);
    void toggleSelected(const QString& noteId);
    void bumpSelectedVelocity(int delta);
    void bumpSelectedPan(int steps);
    /// Keep live wheel feedback, but turn a trackpad/wheel burst into one
    /// history entry after a short idle interval.
    void beginWheelNoteEdit(const std::string& label);
    void finishWheelNoteEdit();
    /// Erase every note crossed by a pointer segment. Mouse-move events can be
    /// much farther apart than a narrow note during a fast sweep, so checking
    /// only the latest point leaves random notes behind.
    bool eraseStroke(const QPointF& from, const QPointF& to);
    bool commitPendingErase();
    void sliceAt(const QPointF& pos, bool acrossAllNotes);
    void muteAt(const QPointF& pos, bool muted);
    /// Arm proportional time scaling from the dedicated group handle. Note
    /// edges never enter this path: they perform an ordinary group trim.
    void beginStretch(const QMouseEvent* ev);
    /// Recompute the phantom from the pointer's current beat.
    void updateStretch(double currentBeats, bool snapOn);
    /// Commit the phantom as one undo entry and clear the gesture state.
    void commitStretch();
    /// Whether a handle stretch snaps to the grid right now. Alt snaps on,
    /// Alt+Shift snaps off, otherwise it follows the global snap setting.
    bool stretchSnapEnabled() const;
    /// The grab handle drawn at the right end of the selection, or a null rect
    /// when nothing is selected. Dragging it stretches the selection.
    QRectF stretchHandleRect() const;
    /// True when the pointer is over the stretch handle.
    bool onStretchHandle(const QPointF& pos) const;
    /// Keep the playhead on screen while it moves.
    void followPlayhead();
    /// Sound `pitch` on the clip's track through its instrument, ending
    /// whatever the keyboard was already sounding. Nothing is written down —
    /// this is the click you hear, not a note.
    void auditionPitch(int pitch);
    /// End the audition, if one is ringing. Called from every way out of the
    /// gesture, including the window closing under it.
    void stopAudition();
    using PitchMask = std::bitset<128>;
    struct SoundingPitchIndex {
        std::array<std::vector<double>, 128> startsByPitch;
        std::array<std::vector<double>, 128> endsByPitch;

        void rebuild(const daw::miditools::Notes& notes);
        PitchMask pitchesAt(double beat,
                            std::size_t* comparisonCount = nullptr) const;
    };
    /// Pitches sounding at the playhead, for the lit keys on the keyboard.
    /// The interval index is rebuilt by edits; transport frames perform only
    /// 256 binary searches over fixed pitch buckets and allocate nothing.
    PitchMask soundingPitches() const;
    PitchMask soundingPitchesAtBeat(double beat) const;
    PitchMask keyboardPitches() const { return soundingPitches() | m_livePitches; }
    void invalidateSoundingPitchIndex() const noexcept;
    void ensureSoundingPitchIndex(const daw::miditools::Notes& notes) const;
    void paintKeyboard(QPainter& p, double fieldBottom);
    void paintLane(QPainter& p);
    void invalidateNotePaintIndex() noexcept;
    /// Forget paint indexes after undo/project reload or switching clips. The
    /// vectors retain capacity during ordinary edits; their revision tokens
    /// make steady repaints allocation-free.
    void invalidateDocumentPaintCaches();
    void seekToLocalBeat(double beats);

    // ── The cycle region ──
    //
    // The same region the arrangement shows: this ruler counts beats from the
    // clip's own start, the controller keeps seconds from the project's, and
    // these two convert between them. A cycle dragged out here is the cycle the
    // whole project plays — there is only one.
    double localBeatToSeconds(double beats) const;
    double secondsToLocalBeat(double seconds) const;
    enum class LoopGrab { None, Create, Move, ResizeStart, ResizeEnd };
    LoopGrab loopGrabAt(double x) const;
    void drawCycleStrip(QPainter& p);
    void paintNoteShape(QPainter& p, const QRectF& r, const QColor& fill,
                        bool selected, bool muted) const;

    void emitStatus();

    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    QString m_clipId;
    /// Valid only while paintEvent is on the stack. Geometry and colour helpers
    /// call clip() many times per frame; resolving the same QString ids through
    /// the track's clip vector for every note made auto-fit painting quadratic in
    /// the number of clips. Painting never mutates the document, so the pointer
    /// is safe for that one stack frame and is cleared before returning.
    const daw::ClipModel* m_paintClip = nullptr;

    /// Selected notes. Several can be selected so a velocity move, a drag or a
    /// whole tool can act on a chord or a phrase at once.
    QSet<QString> m_selected;
    /// The note a drag is anchored to — the one actually grabbed.
    QString m_primary;

    double m_gridBeats = 0.25;
    bool m_snapEnabled = true;
    bool m_adaptiveSnap = false;
    bool m_snapToScale = false;
    double m_swing = 0.5;
    int m_scaleRoot = 0;
    daw::miditools::Scale m_scale = daw::miditools::Scale::Major;

    Tool m_tool = Tool::Draw;
    /// Set while S or T is held: the tool reverts the moment the key comes up,
    /// which is how a single slice happens without leaving Draw.
    std::optional<Tool> m_heldTool;

    bool m_showKeyboard = true;
    bool m_showVelocityLane = true;
    bool m_showNoteNames = false;
    bool m_noteBorders = true;
    NoteStyle m_noteStyle = NoteStyle::Rounded;
    bool m_showAllKeyNames = false;
    bool m_scaleHighlight = false;
    ColorMode m_colorMode = ColorMode::Clip;
    double m_gridContrast = 0.5;
    QColor m_gridColor;              // invalid = follow the theme
    QSet<QString> m_ghostTracks;
    bool m_followPlayback = true;
    int m_lastPlayheadPixel = -1;
    PitchMask m_lastSoundingPitches;
    PitchMask m_livePitches;
    mutable SoundingPitchIndex m_soundingPitchIndex;
    mutable const daw::miditools::Notes* m_soundingPitchSource = nullptr;
    mutable const daw::NoteModel* m_soundingPitchData = nullptr;
    mutable std::size_t m_soundingPitchCount = 0;
    mutable std::size_t m_soundingPitchIndexRebuilds = 0;
    mutable bool m_soundingPitchIndexDirty = true;
    /// Live geometry/mute/context-length gestures keep the previous interval
    /// index for their whole burst, then materialise this dirty bit on commit.
    mutable bool m_soundingPitchInvalidationDeferred = false;

    /// Horizontal zoom. Zero means "fit the clip to the width", which is what a
    /// freshly opened roll does before the user zooms anything.
    double m_pxPerBeat = 0.0;
    double m_rowHeight = 12.0;
    double m_scrollX = 0.0;
    double m_scrollY = 0.0;

    // Drag state. Mirrors the arrangement's naming so the two read alike.
    bool m_moving = false;
    bool m_resizing = false;
    bool m_resizingLeft = false;
    /// Immutable note geometry captured when a note edge is grabbed. Applying
    /// the pointer delta to this snapshot trims every selected note equally
    /// and avoids cumulative drift across mouse-move events.
    std::vector<daw::NoteModel> m_resizeOrig;
    /// Selected notes in their current live-drag state. Mouse moves update this
    /// compact K-note vector instead of rescanning every note in the clip.
    std::vector<daw::NoteModel> m_moveWorking;
    /// Latest visible geometry for a resize/draw. `m_resizeOrig` remains the
    /// immutable delta source while this vector follows the pointer.
    std::vector<daw::NoteModel> m_geometryPaintNotes;
    /// Reused payload for high-frequency multi-note setters.
    std::vector<daw::NoteModel> m_noteUpdateScratch;
    double m_resizeGrabBeats = 0.0;
    bool m_laneDragging = false;
    bool m_marquee = false;
    bool m_erasing = false;
    bool m_eraseChanged = false;
    /// Notes crossed by the current eraser sweep. The document vector stays
    /// stable until release so every mouse sample can reuse the same index.
    QSet<QString> m_pendingErase;
    /// Dragging the ruler moves the project transport in clip-local time.
    bool m_scrubbingPlayhead = false;
    LoopGrab m_loopGrab = LoopGrab::None;
    double m_loopAnchorBeats = 0.0;
    double m_loopGrabOffset = 0.0;
    double m_loopGrabLength = 0.0;
    QPointF m_lastErasePoint;
    /// Everything written between a pointer press and release is collected in
    /// one history entry, even when several selected notes moved together.
    bool m_gestureUndoActive = false;
    bool m_muting = false;
    bool m_mutingTo = true;
    /// A right-press that erased a note must not also open the context menu.
    bool m_suppressContextMenu = false;
    QPointF m_marqueeOrigin;
    QPointF m_marqueeCurrent;
    QPointF m_pointer;
    bool m_pointerInside = false;
    double m_grabBeats = 0.0;
    double m_laneGrab = 0.0;
    /// A freshly painted note keeps its grid-safe default length until the
    /// pointer moves far enough to be an intentional resize.
    bool m_drawing = false;
    QPointF m_drawPress;
    /// Each selected note's lane value at the moment of the press, normalised,
    /// so a group drag applies one delta and keeps its relative shape.
    std::vector<daw::NoteModel> m_laneOrig;
    /// Original velocities for a context-panel gesture. Keeping the immutable
    /// snapshot avoids flattening a chord and avoids cumulative rounding while
    /// the slider produces many live updates.
    QHash<QString, int> m_velocityEditOriginal;
    int m_velocityEditAnchor = 100;
    bool m_velocityEditActive = false;
    bool m_selectionEditUndoActive = false;
    /// Current K-note payload for a context-panel drag. Geometry revisions may
    /// change every slider sample, so retaining it avoids rebuilding the
    /// document id index until the gesture ends.
    std::vector<daw::NoteModel> m_selectionEditWorking;

    // ── Stretch state ──
    //
    // Stretch scales a multi-note selection in time. It arms only from the
    // handle to the right of the group; every note edge remains an ordinary
    // trim. Unlike a move or resize this is *not* a live edit: the
    // originals stay put and a phantom outline shows the result, and the whole
    // gesture becomes one undo entry on release.
    bool m_stretching = false;
    /// The beat that stays fixed while the rest scales around it.
    double m_stretchAnchorBeats = 0.0;
    /// The beat the pointer grabbed at press, the reference for the scale.
    double m_stretchGrabBeats = 0.0;
    /// The selection's original span, used to pick the anchor side.
    double m_stretchOrigSpan = 0.0;
    /// The current scale factor, 1.0 = unchanged, >1 stretch, <1 compress.
    double m_stretchScale = 1.0;
    /// The selected notes as they were at press — the phantom's source.
    std::vector<daw::NoteModel> m_stretchOrig;
    /// The scaled result, drawn as an outline until the gesture is released.
    std::vector<daw::NoteModel> m_stretchPreview;

    LaneParam m_laneParam = LaneParam::Velocity;
    QString m_laneId;
    double m_laneHeight = 84.0;
    /// Set while the divider above the lane is being dragged.
    bool m_laneResizing = false;
    double m_laneResizeGrab = 0.0;
    /// Breakpoint being dragged in a controller curve, or −1.
    int m_lanePointDrag = -1;
    /// The curve as it was when the gesture started, for one undo entry.
    std::vector<daw::AutomationPoint> m_lanePointsBefore;
    /// One mutable copy for the whole gesture. Raw mouse moves update only the
    /// dragged element; a single frame flush copies and normalises it once.
    std::vector<daw::AutomationPoint> m_laneWorkingPoints;
    std::optional<daw::AutomationPoint> m_laneLastWrittenPoint;
    QTimer* m_controllerLaneWriteTimer = nullptr;
    bool m_controllerLaneWritePending = false;
    QString m_laneGestureTrackId;
    QString m_laneGestureClipId;
    QString m_laneGestureLaneId;
    /// Deterministic self-check counter for move-storm coalescing.
    std::size_t m_controllerLaneModelWrites = 0;

    /// The key the mouse is holding down, drawn pressed. −1 = none.
    int m_pressedKey = -1;
    /// The pitch currently sounding from a clicked key, or −1. Separate from
    /// `m_pressedKey` because the note that is *ringing* has to be ended by the
    /// exact pitch that started it — sliding along the keys changes the drawn
    /// key and the sounding one in the same move, and a note-off for the new
    /// pitch would leave the old one held forever.
    int m_auditionPitch = -1;
    double m_lastLength = 1.0;
    int m_wheelAccum = 0;
    QTimer* m_wheelEditTimer = nullptr;
    bool m_wheelEditUndoActive = false;
    std::string m_wheelEditLabel;

    /// What a tool dialog says the result would be. Painted instead of the
    /// clip's notes, and never written anywhere.
    std::optional<daw::miditools::Notes> m_preview;
    /// Beat-range index shared by full and narrow repaints. A monotonic model
    /// revision replaces the former O(N) geometry hash, and block maxima keep
    /// long notes that begin left of the viewport discoverable.
    mutable const daw::miditools::Notes* m_notePaintSource = nullptr;
    mutable std::size_t m_notePaintCount = 0;
    mutable std::uint64_t m_notePaintRevision = 0;
    mutable daw::MidiPreviewIndex m_notePaintIndex;
    std::vector<std::size_t> m_notePaintScratch;
    mutable const daw::miditools::Notes* m_noteIdIndexSource = nullptr;
    mutable std::size_t m_noteIdIndexCount = 0;
    mutable std::uint64_t m_noteIdIndexRevision = 0;
    mutable std::unordered_map<std::string, std::size_t> m_noteById;
    struct GhostPaintIndexEntry {
        daw::MidiPreviewIndex index;
        std::uint64_t revision = 0;
        std::size_t noteCount = 0;
    };
    /// One compact index per ghost clip. Clip offsets and colours are read live,
    /// so moving or recolouring a clip does not require rebuilding note order.
    std::unordered_map<std::string, GhostPaintIndexEntry> m_ghostPaintIndexes;
    std::vector<std::size_t> m_ghostPaintScratch;
    /// IDs of the transformed result inside `m_preview`, prepared while the
    /// preview is built so Apply can preserve selection without regenerating.
    QSet<QString> m_previewSelection;
    bool m_previewWholeClip = true;
};

/// The piano roll editor panel for one MIDI clip.
///
/// Menus, snap and zoom live here; everything that touches notes lives in the
/// view, and everything that computes notes lives in `daw::miditools`. Window
/// chrome lives in InternalEditorFrame so this remains inside the DAW workspace
/// instead of becoming an operating-system window.
class PianoRollWindow : public QWidget {
    Q_OBJECT
public:
    explicit PianoRollWindow(daw::EngineController* controller,
                             QWidget* parent = nullptr);
    ~PianoRollWindow() override;

    /// Point the window at a clip. Safe to call while it is already open.
    void setClip(const QString& trackId, const QString& clipId);
    /// The track being edited — what the typing keyboard plays while this
    /// internal editor is active.
    QString trackId() const { return m_trackId; }
    QString clipId() const { return m_clipId; }
    QWidget* collaborationPresenceSurface() const { return m_view; }
    collab::SemanticPoint collaborationPresenceAt(
        const QPointF& position) const;
    std::optional<QPointF> collaborationPositionFor(
        const collab::SemanticPoint& point) const;
    /// Re-read the document and repaint — call after anything that could have
    /// changed or removed the clip (undo, reload, track deletion).
    void refresh();
    /// Repaint only the shared transport cursor; safe to call for every scrub
    /// event because it does not rebuild menus or editor state.
    void refreshPlayhead();
    /// Replace the live-input pitches painted on the left keyboard.
    void setLivePitches(const std::bitset<128>& pitches);
    bool livePitchHeldForTest(int pitch) const;
    bool checkInteractionGesturesForTest();

    /// The four clipboard chords, performed on the notes.
    ///
    /// Public because the main window's Edit menu has to be able to hand them
    /// over: on macOS the menu bar is the *system* menu bar, and Cocoa fires a
    /// menu item's key equivalent itself — before Qt delivers the key anywhere
    /// and without sending the ShortcutOverride a focused editor would
    /// otherwise claim. See `MainWindow::routeEditChord`.
    void cutNotes();
    void copyNotes();
    void pasteNotes();
    void repeatNotes();
    void deleteNotes();
    /// Build the note-selection island in the application's shared context
    /// strip. The caller owns it through `host`; signal wiring stays here so
    /// note tools still use this editor's existing actions and dialogs.
    NoteContextPanel* createContextPanel(QWidget* host);
    bool hasSelectedNotes() const;
    /// Headless checks only: put every note in the clip into the selection, so
    /// a chord routed here has something to act on.
    void selectAllNotesForTest();

    /// The local header must contain only editor controls and navigation; MIDI
    /// context now belongs to the application's shared strip above it.
    bool checkCompactLayoutForTest();

    /// The cycle strip in this ruler: a drag creates and arms a region, while a
    /// double-click on it removes it. The same region and gesture as the
    /// arrangement's.
    bool checkCycleGestureForTest();

signals:
    void edited();
    /// The cycle region was dragged out in the roll. Forwarded so the
    /// arrangement repaints it and the transport's Cycle lamp follows.
    void loopRangeChanged();
    /// The transport moved from the local ruler. This is not a document edit;
    /// it only asks the arrangement and transport display to repaint now.
    void playheadMoved();
    /// Mute/Solo changed on the clip's track; the arrangement, headers and
    /// mixer need to refresh their copy of those two states.
    void trackStateChanged(bool localFileDirty = true);
    void automateMuteRequested(const QString& trackId);
    void noteSelectionChanged(bool hasSelection);
    /// A modeless MIDI tool wants the same workspace chrome as every other
    /// application editor. MainWindow owns the shared host surface.
    void internalWindowRequested(QWidget* content, const QString& settingsKey);

protected:
    /// Where the settings no menu owns get written back.
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildMenus();
    void buildEditMenu(QMenu* menu);
    void buildViewMenu(QMenu* menu);
    void buildToolsMenu(QMenu* menu);
    void buildSnapMenu(QMenu* menu);
    void buildToolbar();
    void updateTitle();
    void updateScrollBars();
    void updateActionState();
    /// Rebuild the ghost-note list from the project's other MIDI tracks.
    void refreshGhostMenu();
    /// Keep the tool palette and the Tools menu showing the same choice.
    void syncToolActions();
    /// Rebuild the bottom lane's picker from the clip's controller curves.
    void refreshLaneSelector();
    void laneSelectionChanged(int index);
    void addControllerLane();
    void removeControllerLane();
    /// Push the ticked snap division and flavour into the view, and remember
    /// them. Both menus feed this, since the grid is their product.
    void applyGridFromMenus();
    /// Restore the settings that no menu item owns — lane height, zoom, the
    /// chosen tool, the window's own size.
    void loadViewPreferences();
    /// Write those back. Called when the window closes, not per change: a zoom
    /// or a lane drag fires continuously and would hammer the settings file.
    void saveViewPreferences();
    /// Open the dialog behind a tool the context panel asked for.
    void openToolFor(NoteContextPanel::Tool tool);

    /// Open (or raise) a tool dialog. `preview` computes the visual result;
    /// Apply commits that already-computed result under `undoLabel`.
    template <typename Dialog>
    void hostToolDialog(Dialog*& dialog, const QString& undoLabel,
                        const std::function<void(Dialog*)>& preview);
    /// Slider signals can arrive much faster than a frame can be shown. Retain
    /// only the latest pure preview request and run at most one every 16 ms.
    void scheduleToolPreview(ToolDialog* owner, std::function<void()> preview);
    void runPendingToolPreview();
    bool flushToolPreview(ToolDialog* owner);
    void cancelToolPreview(ToolDialog* owner = nullptr);

    daw::EngineController* m_controller = nullptr;
    PianoRollView* m_view = nullptr;
    QScrollBar* m_hScroll = nullptr;
    QScrollBar* m_vScroll = nullptr;
    QString m_trackId;
    QString m_clipId;

    QWidget* m_toolbar = nullptr;
    QMenu* m_editMenu = nullptr;
    QMenu* m_viewMenu = nullptr;
    QMenu* m_toolsMenu = nullptr;
    QMenu* m_snapMenu = nullptr;
    QMenu* m_ghostMenu = nullptr;
    QMenu* m_noteStyleMenu = nullptr;
    QButtonGroup* m_toolButtons = nullptr;
    QComboBox* m_laneSelector = nullptr;
    QToolButton* m_removeLaneButton = nullptr;
    ui::MsrButton* m_trackMuteButton = nullptr;
    QAbstractButton* m_trackSoloButton = nullptr;
    // Kept so the context panel can trigger the very same actions the menu does.
    QAction* m_quantizeAction = nullptr;
    QAction* m_arpAction = nullptr;
    QAction* m_chordAction = nullptr;
    QAction* m_glueAction = nullptr;
    QAction* m_strumAction = nullptr;
    QAction* m_articulateAction = nullptr;
    QAction* m_randomAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_pasteAction = nullptr;
    QAction* m_repeatAction = nullptr;
    QActionGroup* m_toolGroup = nullptr;
    QActionGroup* m_divisionGroup = nullptr;
    QActionGroup* m_flavourGroup = nullptr;
    QHash<QAction*, int> m_toolActions;

    QuantizeDialog* m_quantizeDialog = nullptr;
    ArpeggiatorDialog* m_arpDialog = nullptr;
    ArticulateDialog* m_articulateDialog = nullptr;
    GlueDialog* m_glueDialog = nullptr;
    StrumDialog* m_strumDialog = nullptr;
    RandomizeDialog* m_randomDialog = nullptr;
    /// Several modeless tool windows may stay open; only one owns the preview
    /// currently painted in the grid.
    ToolDialog* m_previewOwner = nullptr;
    QTimer* m_toolPreviewTimer = nullptr;
    QPointer<ToolDialog> m_pendingPreviewOwner;
    std::function<void()> m_pendingToolPreview;
    /// Deterministic self-check counter: several queued parameter signals must
    /// result in exactly one expensive transform.
    std::size_t m_coalescedToolPreviewRuns = 0;
    ChordDialog* m_chordDialog = nullptr;

    /// Undo/project sync may arrive while the editor frame is hidden. Defer
    /// menu reconstruction and repaint until it can produce a visible frame.
    bool m_refreshPending = false;

    /// The last quantize settings, so Quick Quantize can repeat them without
    /// opening anything.
    daw::miditools::QuantizeParams m_lastQuantize;
};
