# Local GPU migration evidence

These are experimental-stage checks, **not acceptance of the complete plan**.
The final controlled comparison reduced GUI + render-thread CPU by **28.7–32.8%**
in this short fixture. The requested minimum 40% is **not met**.

## Setup and comparison

Apple M1, macOS 26.5.1 arm64, Qt 6.11.2, Metal, RelWithDebInfo build. Both
backends use the same compiled application, QWidget host/layout, 1600×907
logical-pixel viewport, DPR 2 and requested 60 FPS. CPU ran first, GPU second,
sequentially with no concurrent build/test process. Each row measures 1.5 seconds
of a time-based triangular horizontal scroll (135 pixels out/back, 720 ms cycle),
eight audio tracks and generated 40-second stereo PCM. Input timer samples can
be coalesced; their target path does not become shorter when the GUI is busy.
This is a single comparison pair, without statistical confidence intervals.

CPU values below are milliseconds of consumed thread CPU within that interval,
not paint duration, wall-clock latency, GPU execution time or CPU percent.
The GPU column adds GUI CPU and actual OS render-thread CPU. WindowServer/driver
process CPU outside the app is not captured. Total application CPU, including
synthetic audio, is reported separately in the raw logs.

| Buffer | Mode | QWidget UI CPU, ms | GPU UI CPU, ms | Reduction |
|---:|:---|---:|---:|---:|
| 32 | Play | 1017.1 | 711.5 | 30.0% |
| 32 | Record | 1043.3 | 740.4 | 29.0% |
| 64 | Play | 1057.0 | 710.1 | 32.8% |
| 64 | Record | 1094.6 | 755.3 | 31.0% |
| 128 | Play | 1011.2 | 712.5 | 29.5% |
| 128 | Record | 1077.1 | 763.2 | 29.1% |
| 512 | Play | 1047.8 | 732.1 | 30.1% |
| 512 | Record | 1101.7 | 786.1 | 28.7% |

CPU rows delivered 89–91 Timeline paint callbacks and GPU rows delivered 91;
the GPU reported 91–92 render frames inside each sample. These are different
pipeline events. The small difference in delivered frame counts is retained
in the comparison data; the table reports measured totals, not FPS-normalized estimates. Frame submission
is not proof that a frame reached the display. Both runs had zero callback budget
overruns, but the harness uses synthetic device delivery through the production
callback code; **these are not physical audio-device xruns**.

Raw [CPU log](cpu-scroll.log), [GPU log](gpu-scroll.log),
[comparison data](comparison.json), [CPU profile](cpu-profile.json),
[GPU profile](gpu-profile.json). Profile JSON includes warmup, scenario transitions
and compatibility-cache checks after the GPU surface is removed. Do not confuse
its whole-run percentiles with the per-scroll-window `AUDIO_SCROLL` values.
Qt `frameSwapped` is recorded as submission interval, not actual presentation;
its whole-run GPU p99 was 49.1 ms, including scenario transitions. No p99 scanout
or audiovisual phase requirement is claimed to pass.

## Functional checks

- [Nine regression tests passed](regressions.log): controller, graph, audio
  presentation snapshots/readers, graphics Auto policy, frame clock, software
  fallback, vector recording and CPU/GPU waveform cache behavior. GPU waveform
  checks cover fractional pan, gain, reversal, recording tails and memory limits.
- [Native Metal smoke test passed](metal-smoke.log): separate render thread,
  geometry/color/rectangular and rounded clipping, exact fractional pointer
  coordinates, timestamps and wheel phases, retained frames, palette changes,
  deferred VSync surface recreation and fallback for a foreign native editor.
  The fallback message in that fixture is expected and asserted.
- [Normal startup](workspace-startup.log) confirmed Metal and a threaded scene
  graph from the saved opt-in, without a GPU environment override or fallback.
- [Local web/video regression passed](web-video-regression.log) after media
  quality/throttling changes. It tests the existing compatibility implementation,
  not a completed WebEngineQuick/VideoOutput migration.
- The final application and relevant targets built successfully; `git diff --check`
  passed. No Windows build or hardware run was performed here.

The generated [workspace screenshot](workspace.png) and
[settings screenshot](graphics-settings.png) were visually inspected. They use
isolated preferences and a generated project, not a user's working project.
Screenshots are test-only readbacks, not the runtime rendering path.

## Remaining acceptance work

See the [implementation status](../README.md). In particular: independent Quick
controllers/panels, retained grid/note geometry, bounded worker preparation,
native media/browser and profile migration, full input/accessibility parity,
separate editor windows, Windows/D3D11 and monitor/device-loss testing, actual
presentation/GPU timing and 30-minute physical-device recording remain unfinished.
The source manifest is [source-sha256.json](source-sha256.json); it includes the
existing workspace changes on top of which this migration was implemented.
