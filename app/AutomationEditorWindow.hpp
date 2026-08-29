#pragma once

#include "AutomationTools.hpp"
#include "model/Document.hpp"

#include <QString>
#include <QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QToolButton;

namespace daw { class EngineController; }

/// The curve canvas: one automation clip, drawn big enough to shape by hand.
///
/// The same gestures the arrangement lane offers, because a curve should not
/// behave differently depending on how much of it you can see — click empty
/// space to add a point and drag it straight away, drag a point to move it,
/// Shift-drag a point in time, Alt-drag a segment to bend it, right-click for
/// its shape. What the lane cannot give is room: a Shift rubber band from empty
/// space, a value axis in the parameter's own units, and the generators that act
/// on a selection.
///
/// It owns no data. Every edit goes to `EngineController::setAutomationPoints`
/// while the gesture runs and to `commitAutomationEdit` when it is let go, so
/// the whole drag is one undo entry and the engine hears it live.
class AutomationCurveView : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Draw };

    explicit AutomationCurveView(daw::EngineController* controller,
                                 QWidget* parent = nullptr);

    void setClip(const QString& trackId, const QString& clipId);
    void setTool(Tool tool);
    Tool tool() const noexcept { return m_tool; }
    /// Grid division in beats; 0 switches snapping off.
    void setSnapBeats(double beats);
    /// The shape a freshly placed point gives the segment that starts at it.
    void setNewPointShape(daw::AutomationSegment shape);

    /// The selected stretch, or the whole curve when nothing is selected — the
    /// range every generator and transform acts on.
    daw::autotools::Range range() const;
    bool hasSelection() const noexcept { return m_hasSelection; }
    void selectAll();
    void clearSelection();

    /// Show `points` without writing them: the live preview a generator dialog
    /// paints while its controls are being dragged. The engine hears it too —
    /// the points are pushed through the non-undoable path — so a preview is
    /// audible, which is the only way to judge an LFO rate.
    void showPreview(const daw::autotools::Points& points);
    /// Put back whatever the clip held before the preview started.
    void cancelPreview();
    /// Keep the preview, as one undo entry.
    void commitPreview(const QString& label);
    /// The curve as it stands, preview included.
    daw::autotools::Points points() const;
    /// Replace the curve as one undo entry — how the toolbar's transforms land.
    void applyPoints(const daw::autotools::Points& points, const QString& label);
    /// Screen position of a breakpoint for the headless gesture check.
    QPoint pointPositionForTest(int index) const;

signals:
    void edited();       ///< A finished change: mark the project dirty.
    void liveEdited();   ///< Mid-gesture: repaint, touch nothing else.
    void selectionChanged();
    void readoutChanged(const QString& text);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    const daw::ClipModel* clip() const;
    /// The clip's curve — the preview while one is running.
    const daw::autotools::Points& curve() const;
    double lengthBeats() const;

    QRectF plot() const;
    double beatsToX(double beats) const;
    double xToBeats(double x) const;
    double valueToY(double value) const;
    double yToValue(double y) const;
    double snap(double beats) const;

    /// Index of the point within grab distance of `pos`, or −1.
    int pointAt(const QPointF& pos) const;
    /// Index of the point that starts the segment under `pos`, or −1.
    int segmentAt(const QPointF& pos) const;

    void pushLive(daw::autotools::Points points);
    void commit(const QString& label);
    void beginGesture();
    QString readoutFor(double beats, double value) const;

    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    QString m_clipId;

    Tool m_tool = Tool::Select;
    double m_snapBeats = 0.0;
    daw::AutomationSegment m_newShape = daw::AutomationSegment::Linear;

    /// The curve as it was when the gesture started — the "before" half of the
    /// undo entry, and what a cancelled preview goes back to.
    daw::autotools::Points m_before;
    bool m_beforeActive = false;
    bool m_gesture = false;

    daw::autotools::Points m_preview;
    bool m_previewing = false;

    int m_dragPoint = -1;
    /// Stable source for a point drag, so every mouse move is computed from
    /// the exact pickup state even when the handle crosses its neighbours.
    daw::autotools::Points m_dragPoints;
    int m_bendSegment = -1;
    double m_bendStartY = 0.0;
    double m_bendStartCurve = 0.0;
    bool m_drawing = false;

    bool m_hasSelection = false;
    double m_selectFrom = 0.0;
    double m_selectTo = 0.0;
    bool m_banding = false;
    double m_bandAnchor = 0.0;

    int m_hoverPoint = -1;
    QPointF m_cursor;
    bool m_hasCursor = false;
};

/// Top-level editor for one automation clip.
///
/// Opened by double-clicking a curve in the arrangement. Three fields across
/// the top say what the curve drives — channel, then what on it, then which
/// parameter — so a curve that took a while to shape can be pointed at another
/// plugin instead of being drawn again. Under them, the generators: an LFO with
/// a live preview, ramps, and the transforms that act on a selection.
class AutomationEditorWindow : public QWidget {
    Q_OBJECT
public:
    AutomationEditorWindow(daw::EngineController* controller, QString trackId,
                           QString clipId, QWidget* parent = nullptr);

    const QString& trackId() const { return m_trackId; }
    const QString& clipId() const { return m_clipId; }

    /// The document moved under the window — an undo, a rename, a plugin
    /// swapped in the slot this curve drives. Re-read everything.
    void refresh();

signals:
    void closing(const QString& trackId, const QString& clipId);
    void projectEdited();
    void liveEdited();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildTargetRow(class QVBoxLayout* outer);
    void buildToolbar(class QVBoxLayout* outer);
    void reloadTargetFields();
    void applyTargetFromFields();
    void showLfoDialog();
    void updateTitle();

    /// The target the three combos currently spell out.
    daw::AutomationTarget targetFromFields() const;
    const daw::ClipModel* clip() const;

    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    QString m_clipId;

    QComboBox* m_channel = nullptr;
    QComboBox* m_what = nullptr;
    QComboBox* m_parameter = nullptr;
    QLabel* m_readout = nullptr;
    QLabel* m_hint = nullptr;
    QComboBox* m_snap = nullptr;
    QComboBox* m_shape = nullptr;
    QToolButton* m_select = nullptr;
    QToolButton* m_draw = nullptr;
    AutomationCurveView* m_view = nullptr;

    /// Set while the combos are being repopulated, so filling them does not
    /// read back as the user choosing a new target.
    bool m_reloading = false;
};
