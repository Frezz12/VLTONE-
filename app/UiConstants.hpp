#pragma once

#include "Theme.hpp"

#include <QColor>
#include <QRectF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Shared metrics so the track headers and the timeline lanes stay aligned, plus
// the grid vocabulary shared by the transport bar and the timeline.
namespace ui {

inline constexpr int kLaneHeight = 72;   // default lane height
inline constexpr int kMinLaneHeight = 44;
inline constexpr int kMaxLaneHeight = 320;
inline constexpr int kRulerHeight = 34;
/// The cycle strip: the band at the top of a ruler where the loop region is
/// dragged out, above the bar numbers. Shared by the arrangement and the piano
/// roll, which have to feel like one control in two places.
inline constexpr int kLoopStripHeight = 14;
/// How close to a cycle edge counts as grabbing it rather than starting a new
/// region.
inline constexpr int kLoopEdgeGrab = 6;
/// Empty room kept below the last lane, scrolling with the tracks. Without it
/// the bottom track sits flush against the edge — and with the mixer pulled up
/// over the arrangement there would be no way to bring a low track into the
/// strip of arrangement still showing without collapsing the mixer first.
inline constexpr int kLaneTailPadding = 160;
/// Track-header column geometry. The default preserves the established layout,
/// while the floor still leaves room for the track identity, its essential
/// chips and the compact round volume control.
inline constexpr int kTrackHeaderWidth = 300;
inline constexpr int kMinTrackHeaderWidth = 220;
/// The headers may take the rest of the arrangement, but the resize rail and a
/// narrow strip of timeline must remain reachable so the gesture is reversible.
inline constexpr int kMinTimelineWidth = 180;
inline constexpr int kTransportHeight = 66;
inline constexpr int kBottomBarHeight = 28;

/// The on-screen lane height for a track, clamped to the resizable range. A
/// zero/unset stored height falls back to the default. Both the timeline lanes
/// and the header rows go through this, so they always match.
inline int laneHeightFor(double storedHeight) {
    const int v = storedHeight <= 0.0 ? kLaneHeight : int(storedHeight);
    return std::clamp(v, kMinLaneHeight, kMaxLaneHeight);
}

/// 0xRRGGBB → QColor.
inline QColor colorFromRgb(uint32_t rgb) { return ::colorFromRgb(rgb); }

/// Round painted geometry to physical pixels without quantising the model or
/// its hit targets. Fractional logical coordinates are otherwise especially
/// soft on 125/150% displays, where a one-pixel outline straddles device pixels.
inline QRectF pixelAlignedRect(const QRectF& rect, qreal devicePixelRatio) {
    const qreal scale = std::max<qreal>(1.0, devicePixelRatio);
    const auto snap = [scale](qreal value) {
        return std::round(value * scale) / scale;
    };
    const qreal left = snap(rect.left());
    const qreal top = snap(rect.top());
    const qreal right = std::max(left + 1.0 / scale, snap(rect.right()));
    const qreal bottom = std::max(top + 1.0 / scale, snap(rect.bottom()));
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

/// A timeline grid division, measured in beats (one beat = a quarter note).
struct GridDivision {
    QString name;
    double beats;   // 0 → grid/snap off
};

inline const QVector<GridDivision>& gridDivisions() {
    static const QVector<GridDivision> divisions = {
        {QStringLiteral("Off"),  0.0},
        {QStringLiteral("1/1"),  4.0},
        {QStringLiteral("1/2"),  2.0},
        {QStringLiteral("1/4"),  1.0},
        {QStringLiteral("1/8"),  0.5},
        {QStringLiteral("1/16"), 0.25},
        {QStringLiteral("1/32"), 0.125},
        {QStringLiteral("1/4T"), 2.0 / 3.0},
        {QStringLiteral("1/8T"), 1.0 / 3.0},
    };
    return divisions;
}

/// Index of the default division (1/16), matching the transport read-out.
inline constexpr int kDefaultGridIndex = 5;

/// QSettings key for the Space play/pause behaviour, an int matching
/// EngineController::PlaybackMode.
inline constexpr const char* kPlaybackModeSetting = "transport/playMode";
/// Optional audio file used for the metronome. Empty selects the built-in
/// muted knock.
inline constexpr const char* kMetronomeSampleSetting =
    "transport/metronomeSample";
/// The timeline's grid division, as an index into `gridDivisions()`.
inline constexpr const char* kGridIndexSetting = "transport/gridIndex";
/// The selected edit tool, as an index (0 Select, 1 Knife, 2 Eraser, 3 Region).
inline constexpr const char* kEditToolSetting = "transport/editTool";
/// The tool the modifier key borrows while it is held — Logic's command-click
/// tool. Same indices as above.
inline constexpr const char* kAltEditToolSetting = "transport/editToolAlt";
/// The main window's geometry, so it reopens where it was left.
inline constexpr const char* kMainGeometrySetting = "ui/mainGeometry";
/// User-chosen width of the track-header column.
inline constexpr const char* kTrackHeaderWidthSetting = "ui/trackHeaderWidth";

/// How a selected track header and lane are tinted. Two answers, because they
/// serve different eyes: the track's own colour says *which* track at a glance,
/// and a neutral wash never argues with the colour it sits on.
enum class SelectionTint { TrackColour = 0, Neutral = 1 };
inline constexpr const char* kSelectionTintSetting = "ui/selectionTint";

/// The stored choice, defaulting to the track's own colour.
SelectionTint selectionTint();
void setSelectionTint(SelectionTint tint);

/// The colour a selection is washed with on a track of `trackColor` — the one
/// place the two modes are resolved, so the headers and the lanes cannot
/// disagree about what "selected" looks like.
QColor selectionWash(const QColor& trackColor);

} // namespace ui
