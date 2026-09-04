#pragma once

#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>
#include "CollaborationTypes.hpp"
#include "Host/PluginTypes.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <utility>
#include <vector>

namespace daw { class EngineController; struct TrackModel; }
class QVBoxLayout;
class QLabel;
class QAbstractButton;
namespace ui { class LevelMeter; class FaderWidget; class PanKnob;
               class MsrButton; class IconButton; }

/// The track headers column: one row per visible track.
///
/// A row is a single line — colour rail, a large kind icon, the name over a
/// strip of small M/S/monitor/record chips, then the fader and the pan knob —
/// and it compacts level and pan into a pair of round controls when there is no
/// width for the full throw. Folder tracks add a disclosure chevron and
/// indent their children; a folder that does not sum has no channel at all, so
/// it shows nothing but its name and mute/solo.
///
/// Selection is a *set*: click to replace it, shift-click to extend from the
/// anchor, ctrl/cmd-click to toggle one row. Row height matches the timeline
/// lane height so headers line up with their lanes.
class TrackListWidget : public QWidget {
    Q_OBJECT
public:
    explicit TrackListWidget(daw::EngineController* controller,
                             QWidget* parent = nullptr);

    void rebuild();
    void refreshMeters();
    /// Re-read "is anything muted / soloed" and light the two chips over the
    /// column accordingly. Cheap; called whenever any row's flags change.
    void refreshGlobalChips();
    /// Re-read level, pan and the flags of every row from the document, without
    /// rebuilding a single widget. What a change made somewhere else — the
    /// mixer, the context panel — travels back through.
    void syncTrackValues();
    /// Mirror track volume/pan automation at the playhead without echoing the
    /// displayed values back into the document.
    void refreshAutomationValues();
    /// Re-read each row's height from its track (cheap — no widget rebuild), so
    /// the headers follow a lane being resized in the timeline.
    void syncRowHeights();
    /// Move the rows by the arrangement's vertical scroll offset. The timeline
    /// owns that offset — this column is a second view of the same lanes.
    void setVerticalScroll(int y);
    /// A row's rectangle in this column's coordinates, for the headless check
    /// that the headers and the lanes agree. Null when there is no such row.
    QRect rowRectForTest(int index) const;
    /// Which of a row's optional controls are on screen right now, for the
    /// headless check that a squeezed row sheds them in the right order.
    /// Returns {fader, pan}; both false when there is no such row.
    std::pair<bool, bool> rowControlsForTest(const QString& trackId) const;
    /// One of a row's chips, by its letter, and its fader — for the headless
    /// check that a control touched on one selected row drives every selected
    /// row. Null when there is no such row or no such control.
    ui::MsrButton* rowChipForTest(const QString& trackId,
                                  const QString& letter) const;
    ui::FaderWidget* rowFaderForTest(const QString& trackId) const;

    /// Which track's row covers `position`, and where that track's row sits
    /// right now. Presence uses the pair to describe a collaborator's pointer
    /// as "this track, this far down its row" instead of a fraction of the
    /// column, which lands on the wrong track whenever two participants are
    /// scrolled differently or have different row heights.
    QString trackIdAt(const QPoint& position) const;
    QRect rowRectForTrack(const QString& trackId) const;

    /// Presence encode/decode, mirroring TimelineWidget's pair. A point inside
    /// a row is described by its track and how far down that row it sits; the
    /// ruler above the rows is reported as chrome with no track.
    collab::SemanticPoint collaborationPresenceAt(const QPointF& position) const;
    std::optional<QPointF> collaborationPositionFor(
        const collab::SemanticPoint& point) const;
    /// Headless regression: a pointer keeps naming the same track when the
    /// column is scrolled or resized, and a track this column does not show is
    /// hidden rather than mapped onto a neighbour.
    static bool checkCollaborationPresenceForTest(QString* error = nullptr);
    /// Headless regression for drag-painting M/S across several row chips.
    static bool checkButtonPaintForTest(QString* error = nullptr);

    /// The row that owns the single-track contexts — the last one clicked, or
    /// empty when nothing is selected.
    QString selectedTrackId() const { return m_selectedId; }
    /// Everything selected, in display order.
    QStringList selectedTrackIds() const { return m_selectedIds; }
    void setSelectedTrack(const QString& id);
    void setSelectedTracks(const QStringList& ids, const QString& primary = {});

    /// Whether the transport is armed or rolling into a recording, and which
    /// tracks the recording would land on. The record chip only exists while
    /// there is a recording to be had, and lights on the tracks that will take
    /// it — targets follow the selection, so this is the window's answer, not
    /// something a row can work out for itself.
    void setRecordState(bool engaged, const QStringList& targets);

signals:
    void selectionChanged(const QString& trackId);
    /// The whole selection, whenever it changes. `selectionChanged` carries the
    /// primary row for the single-track views; this carries the set.
    void selectionSetChanged(const QStringList& trackIds);
    /// Something about the tracks changed (name, flags, order, folders) — the
    /// rest of the UI needs to re-read the document.
    void tracksChanged(bool localFileDirty = true);
    /// Order/parenting changed; views that cache lane order must rebuild.
    void orderChanged();
    /// A plugin dropped onto a row loaded successfully and wants its editor.
    void pluginEditorRequested(const QString& channelId,
                               const QString& insertId);
    /// A `.vltt` package was dropped into the track column. The window owns
    /// the import because it also owns dirty state, selection and user errors.
    void projectTemplateTracksRequested(const QString& path);
    /// A row's height changed (drag on its bottom edge); the timeline lanes and
    /// the dirty flag need to follow.
    void trackHeightChanged();
    /// The wheel was turned over the headers: scroll the arrangement by this
    /// many pixels. The column does not scroll itself — the lanes own the
    /// offset and hand it back through `setVerticalScroll`.
    void verticalScrollRequested(int deltaPixels);
    /// The R chip on a row was clicked. Which tracks a recording lands on is
    /// the window's business — it owns the pins and the fallback to the
    /// selection — so the row only reports the click.
    void recordPinToggled(const QString& trackId, bool pinned);
    /// The user asked to pack the current selection into a folder, and chose
    /// which kind. The window owns folder creation because it owns what is
    /// selected everywhere else, not just here.
    void packRequested(bool summing);
    /// The P chip on a Pattern row opens its compact source editor.
    void openPatternRequested(const QString& patternId);
    /// A fader or pan control requested automation. `pan`
    /// says which of the two — the window builds the target, since it is the
    /// one place in the program that knows what automating anything means.
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automateSendRequested(const QString& trackId, const QString& sendId);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    /// A plugin dragged out of the browser lands on the row it is dropped on.
    /// The column is where a track *is*, so it is where somebody aims.
    void dragEnterEvent(class QDragEnterEvent* ev) override;
    void dragMoveEvent(class QDragMoveEvent* ev) override;
    void dragLeaveEvent(class QDragLeaveEvent* ev) override;
    void dropEvent(class QDropEvent* ev) override;
    void resizeEvent(class QResizeEvent* ev) override;
    void wheelEvent(class QWheelEvent* ev) override;
    /// Right-click on the empty space below the rows: the same track-creation
    /// menu the arrangement offers, so an empty project is never a dead end.
    void contextMenuEvent(class QContextMenuEvent* ev) override;

private:
    struct Row {
        std::string id;
        bool isFolder = false;
        /// A fader, a pan and a meter exist on this row — false for a folder
        /// that does not sum, which is a container and carries no signal.
        bool hasChannel = false;
        int depth = 0;
        QWidget* container = nullptr;
        QWidget* colorRail = nullptr;
        QWidget* icon = nullptr;
        ui::FaderWidget* fader = nullptr;
        ui::PanKnob* pan = nullptr;
        ui::LevelMeter* meter = nullptr;
        ui::MsrButton* mute = nullptr;
        ui::MsrButton* solo = nullptr;
        ui::MsrButton* monitor = nullptr;
        ui::MsrButton* record = nullptr;
        ui::MsrButton* pattern = nullptr;
        QWidget* nameEdit = nullptr;
    };

    QWidget* buildRow(const daw::TrackModel& track, int number, int depth);
    void applyHighlight();
    void applyTheme();
    void showTrackContextMenu(const QString& id, const QPoint& globalPos);
    /// Adapt a row to the resizable column. Level and pan become a compact
    /// round pair before either control has to disappear.
    void applyRowAdaptivity(const Row& row);
    /// The row a plugin drag is over, and the drop itself. Split out because
    /// the highlight and the landing must agree on which row that is.
    int pluginDropRowAt(const QPoint& posInList, bool instrument) const;
    /// The plugin a drag is carrying, resolved through the plugin manager.
    std::optional<daw::plugins::PluginDescriptor> pluginFromMime(
        const class QMimeData* mime) const;
    /// Recompute the row under a browser drag from the event's live position.
    /// Returns true when the payload is one this column understands.
    bool updateBrowserDropTarget(const QPoint& posInList,
                                 const class QMimeData* mime);

    /// Put the record chips in step with the current record state.
    void applyRecordChips();
    /// The row whose bottom edge is within a few px of a list-local point (for
    /// the resize handle), or -1.
    int rowResizeEdgeAt(const QPoint& posInList) const;

    // ── Acting on a whole selection ──
    /// The tracks a control on `id`'s row should drive. Touching a control on a
    /// row that is part of the selection drives every selected row — that is
    /// what selecting several of them is *for*; touching one outside the
    /// selection drives only itself.
    std::vector<std::string> actionTargets(const QString& id) const;

    /// One continuous fader or pan drag over a selection.
    ///
    /// Relative, not absolute: the tracks keep their balance, and the amount
    /// they move by is measured against where they stood when the drag started
    /// rather than accumulated per event. Accumulating loses the ratio the
    /// moment one track hits an end stop, and dragging back would not restore
    /// the mix.
    struct GroupGesture {
        QString driverId;
        float driverStart = 0.0f;
        std::vector<std::pair<std::string, float>> start;

        bool coversDrag(const QString& id) const {
            return !start.empty() && driverId == id;
        }
        void clear() { start.clear(); driverId.clear(); }
    };
    /// Begin (or continue) a group drag driven by `id`, and hand back its
    /// targets with the values they started from.
    const GroupGesture& beginGroupGesture(GroupGesture& gesture,
                                          const QString& id, float driverStart,
                                          float (*read)(const daw::TrackModel&));
    /// Run `act` on every track a control on `id`'s row drives, then put the
    /// column, the chips and the rest of the window back in step. Re-entrant
    /// calls are dropped: driving a chip re-emits its own `toggled`.
    void applyToGroup(const QString& id,
                      const std::function<void(const std::string&)>& act);
    void applyMuteToGroup(const QString& id, bool muted);
    /// Paint one M/S state over the row buttons crossed by a vertical drag.
    /// A click keeps the existing selected-track group behaviour; only a real
    /// drag switches to per-row painting.
    void applyTrackButtonPaint(QAbstractButton* button);
    void applyTrackButtonPaintAlong(const QPoint& fromGlobal,
                                    const QPoint& toGlobal);
    void finishTrackButtonPaint();
    /// Move every selected track's level with the one being dragged, keeping
    /// the ratios they started the drag with.
    void applyGroupGain(const QString& id, float gain);
    /// The same for pan, by offset rather than ratio — a pan has a centre, and
    /// scaling towards it is not what turning a knob means.
    void applyGroupPan(const QString& id, float pan);

    // ── Selection ──
    /// Apply a click with its modifiers: plain replaces, shift extends from the
    /// anchor, ctrl/cmd toggles.
    void clickSelect(const QString& id, Qt::KeyboardModifiers modifiers);
    /// Publish the current set, and the primary row with it.
    void emitSelection();
    int rowIndexOf(const QString& id) const;

    // ── Drag to reorder ──
    int rowAtPosition(const QPoint& posInList) const;
    void updateDropTarget(const QPoint& posInList);
    void showDropFeedback();
    void finishDrag();
    void cancelDrag();

    daw::EngineController* m_controller;
    /// A row's rect in this widget's coordinates (its own geometry is relative
    /// to the scrolling host).
    QRect rowGeometry(size_t index) const;
    /// Re-read every row height, resize the host and adapt its controls.
    void layoutRows();
    /// Scrolling changes only the host origin; its size and child layout stay
    /// valid until a rebuild, resize or lane-height change.
    void positionRowsHost();

    QWidget* m_viewport = nullptr;    // clips the rows; fixed height
    QWidget* m_rowsHost = nullptr;    // holds every row; moved to scroll
    int m_scrollY = 0;
    QVBoxLayout* m_rowsLayout = nullptr;
    /// Over the column, where the "TRACKS" caption used to be: clear-all-mutes
    /// and clear-all-solos, lit while there is anything to clear.
    ui::MsrButton* m_clearMutes = nullptr;
    ui::MsrButton* m_clearSolos = nullptr;
    QWidget* m_indicator = nullptr;   // DropIndicator, raised above the rows
    std::vector<Row> m_rows;

    QStringList m_selectedIds;
    QString m_selectedId;       // the primary — the last row clicked
    QString m_anchorId;         // where a shift-click measures its range from

    bool m_recordEngaged = false;
    QStringList m_recordTargets;

    GroupGesture m_gainGesture;
    GroupGesture m_panGesture;
    /// Guards the group handlers against the echo their own setters cause: a
    /// chip driven for every selected row re-emits `toggled` on each of them,
    /// and each of those would drive the group again.
    bool m_applyingGroup = false;

    bool m_trackButtonPaintPending = false;
    bool m_trackButtonPainting = false;
    bool m_trackButtonPaintTarget = false;
    bool m_trackButtonPaintLocalFileDirty = false;
    QString m_trackButtonPaintRole;
    QPoint m_trackButtonPaintPressGlobal;
    QPoint m_trackButtonPaintLastGlobal;
    QSet<QString> m_trackButtonPainted;

    bool m_pressing = false;
    bool m_dragging = false;
    QPoint m_pressPos;
    int m_dragRow = -1;

    // Row height resize (dragging a row's bottom edge).
    bool m_resizingHeight = false;
    QString m_resizeId;
    int m_resizeStartY = 0;
    int m_resizeStartH = 0;
    /// Every track the drag resizes, and the height it started from.
    std::vector<std::pair<std::string, double>> m_resizeStart;
    /// Raw document values for undo (a default/zero height is visually mapped
    /// to the standard lane height, but must still restore as zero).
    std::vector<std::pair<std::string, double>> m_resizeUndoStart;
    int m_dropRow = -1;        // insertion slot: 0 … rowCount
    /// The row a plugin is being dragged over, or -1.
    int m_pluginDropRow = -1;
    QString m_projectTemplateDropPath;
    bool m_dropIntoFolder = false;
    int m_dropFolderRow = -1;
};
