#pragma once

#include "WaveformCache.hpp"

#include <QColor>
#include <QRectF>
#include <span>

class QPainter;

namespace ui {

enum class WaveformStyle { Crisp = 0, Smooth = 1 };
inline constexpr const char* kWaveformStyleSetting = "ui/waveformStyle";
WaveformStyle waveformStyle(); // Cached preference; defaults to Crisp.
void setWaveformStyle(WaveformStyle style);

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
    /// Optional decoded source for a staircase trace at individual-sample zoom.
    const daw::engine::SampleBuffer* samples = nullptr;
    WaveformStyle style = waveformStyle();
};

/// Draw a min/max envelope using bounded, source-anchored raster tiles.
///
/// Lifted out of TimelineWidget so anything with a `WaveformPeaks` can draw one
/// the same way: the clip bodies, the take rows of an open comp editor, the comp
/// lane, and the browser's preview strip. Crisp holds each source column;
/// Smooth connects the envelope edges with antialiased slopes. At sample zoom,
/// decoded samples form a staircase or a connected line respectively.
void paintPeaks(QPainter& painter, const daw::WaveformPeaks* peaks,
                const QRectF& area, const PeakPaint& how);

/// Append-only capture envelope. Only sealed prefixes enter the raster cache;
/// the current peak bucket is redrawn until it closes. Change id on compaction.
void paintRecordingPeaks(QPainter& painter, std::span<const float> envelope,
                         double bucketSeconds, std::uint64_t id,
                         const QRectF& area, const PeakPaint& how);

struct WaveformPaintStats {
    std::uint64_t tileBuilds = 0;
    std::uint64_t tileHits = 0;
    std::size_t bytes = 0;
};
WaveformPaintStats waveformPaintStatsForTest();
WaveformPaintStats waveformGeometryStatsForTest();
void resetWaveformPaintCacheForTest();

/// Headless pixel check: an empty waveform still paints its zero axis.
bool checkWaveformBaselineForTest();

} // namespace ui
