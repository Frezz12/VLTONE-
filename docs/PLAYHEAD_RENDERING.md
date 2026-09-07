# Playback cursor rendering

The arrangement and piano roll retain fractional cursor coordinates. Integer
rounding belongs only at the outside edges of the repaint region, never at the
cursor position or the decision to request its next frame. At 4 logical px/s,
rounding that decision limits visible position changes to four per second even
when the display clock runs at 60 Hz.

The arrangement samples presentation time once for its requested frame. The
cursor, halo, trail and remembered painted footprint use that same position.
The old footprint keeps its own trail length so pausing or disabling the trail
clears everything previously painted. Navigation invalidates the prepared
position and redraws the scene with the new viewport.

The trail represents 7/60 seconds of forward playback at the current horizontal
scale, capped at 52 logical pixels. It always extends left from the line. It
does not measure screen displacement, so panning, auto-follow and backward seeks
cannot reverse or stretch it. Stopped/paused transport has no trail.

## Native frame delivery

`FrameClock` consumes the native `UpdateRequest` that it requested for its own
cadence pulse. Previously, after preparing narrow dirty regions it also let the
event reach `QWidgetWindow`, whose default handler calls `repaint()` on the
entire window. This rebuilt the timeline cache and repainted unrelated controls
every frame. See the [Qt QWidgetWindow implementation](https://github.com/qt/qtbase/blob/v6.8.3/src/widgets/kernel/qwidgetwindow.cpp#L354).

Normal `QWidget::update(region)` backing-store events still reach Qt, as do
exposures, resizes and native requests not owned by the clock. No synchronous
paint, extra timer, spring or easing is added to playback or navigation.

Native event-delivery jitter has up to 3 ms of tolerance (never more than half
the requested interval). Deadlines stay on the same absolute time line, so an
early display tick is not discarded and the tolerance cannot increase the
average frame-rate cap. The test models alternating ±1.25 ms delivery jitter.

## Focused design review

The installed Apple HIG, Apple Design and Emil Design Engineering skills are
applied to Qt's existing paint and event architecture.

| Before | After | Why |
| --- | --- | --- |
| Cursor coordinates and repaint decisions used whole pixels | Fractional position reaches antialiased painting | Precise, immediate feedback at every zoom level |
| Screen displacement controlled the trail | Forward project time determines a left-facing trail | Viewport gestures must not imply reversed playback |
| Every native cadence pulse also repainted the window | Only requested dirty regions are painted | Keep frequent interactions within the display's frame budget |
| Old damage used the new trail length | Old painted footprint retains its own length | Pause and zoom must leave no residual pixels |

The position display and audio remain alternative feedback; the optional trail
setting is preserved. Functional cursor movement receives no decorative delay.

## Validation

`ui_frame_clock_test` exercises real Qt event delivery: exact cursor strips,
unaffected sibling widgets, callbacks without paint requests, hidden windows,
multiple windows and cadence changes. It also models jittered 60/120/144 Hz
delivery deterministically.

`VLTONE --uiperfcheck` uses a 500-track / 20,000-clip scene. Its playback test
feeds transport time without opening an audio device and checks fractional
positions, monotonic progress, intermediate paints inside one logical pixel,
zero static-cache rebuilds for cursor strips, left/right pan, a backward seek, rasterized trail
direction, pause and erasure of the full 52-pixel trail. Audio-block display
interpolation remains covered separately by `engine_graph_test`. The application
selftest includes a fractional piano-roll cursor check.

Measured on macOS/Cocoa, 1250×640 logical viewport, DPR 2, 60 Hz display:
before consuming the cadence event, playback paint p95 was 15.85 ms (66 paints).
The final build delivered 73 cursor paints over about 1.22 seconds, with paint
mean 1.16 ms, p95 2.97 ms and max 4.14 ms, and zero static timeline rebuilds.
Paint intervals averaged 16.81 ms (p95 19.97 ms, max 25.04 ms). The cursor
crossed only five whole pixels in that sample; all 73 fractional paint positions
were delivered. Whole-window exposure from the OS is counted separately from
cursor damage in the real-window test. These are synthetic GUI
measurements, not physical display FPS or a heavy-plugin audio benchmark.

```sh
QT_QPA_PLATFORM=cocoa build/bin/ui_frame_clock_test
QT_QPA_PLATFORM=cocoa VLT_UI_PROFILE=/tmp/vltone-playhead.json build/bin/VLTONE --uiperfcheck
QT_QPA_PLATFORM=offscreen build/bin/VLTONE --selftest
build/bin/engine_graph_test
```
