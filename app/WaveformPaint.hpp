#pragma once

#include "WaveformCache.hpp"

#include <QColor>
#include <QRectF>

class QPainter;

namespace ui {

/// Where the envelope sits in time and how loud it is drawn.
struct PeakPaint {
    /// Source time at the left edge of the area — a clip's `offsetSeconds`, or
    /// 0 for a strip that shows a whole file.
    double sourceStartSeconds = 0.0;
    /// How much source time one pixel covers. The caller owns the zoom.
    double secondsPerPixel = 0.0;
    /// Horizontal bounds to stay inside, in the painter's coordinates. The
    /// timeline passes its own width so a clip scrolled off-screen is not drawn
    /// past the viewport; a strip passes its rect.
    double clipLeft = 0.0;
    double clipRight = 0.0;
    /// Read the source envelope from right to left. The source offset remains
    /// measured in processed audio, matching a sample reversed before playback.
    bool reversed = false;
    /// Height multiplier, so a clip's gain visibly swells the wave.
    float gain = 1.0f;
    QColor color = QColor(255, 255, 255);
};

/// Draw a min/max envelope across `area` as one filled shape.
///
/// Lifted out of TimelineWidget so anything with a `WaveformPeaks` can draw one
/// the same way: the clip bodies, the take rows of an open comp editor, the comp
/// lane, and the browser's preview strip. Two zoom regimes — extremes over the
/// buckets a pixel spans when zoomed out, interpolation between neighbouring
/// buckets when zoomed in, so the outline stays a smooth wave instead of
/// stair-stepping.
void paintPeaks(QPainter& painter, const daw::WaveformPeaks* peaks,
                const QRectF& area, const PeakPaint& how);

/// Headless pixel check: an empty waveform still paints its zero axis.
bool checkWaveformBaselineForTest();

} // namespace ui
