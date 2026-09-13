# Windows workspace motion

## Cause and fix

The GPU workspace already uses Qt Quick's threaded Direct3D 11 renderer on
Windows. Moving a QWidget editor or resizing the mixer nevertheless exposed
backing-store regions, which were mistaken for changes to the contents of the
retained layers. Large projects amplified that unnecessary GUI-thread work.

WorkspaceSurface now distinguishes explicit updates from geometry exposure for
all child windows, including the native plugin host. The exposure flag survives
a Quick frame until Qt consumes the corresponding backing-store update. Clearing
it at scene capture was incorrect: Quick can run before the queued QWidget update.
Existing Qt repaint processing is preserved; no private Qt state is modified.
The distinction uses Qt's existing dirty-widget list, as implemented in
[Qt 6.8.3's repaint manager](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/widgets/kernel/qwidgetrepaintmanager.cpp).

Changing the mixer's bottom inset no longer clears the timeline's lane index and
GPU tiles. Tiles retain the full viewport height; the current mixer position clips
their output. A genuine scroll clamp still invalidates them. No drag animation,
new dependency, raster screenshot cache, or additional rendering backend was added.

## Measurement

Release build, Windows, Qt 6.8.3, D3D11, threaded scene graph. The new
`--workspace-motion-check` constructs 500 MIDI tracks with approximately 20,000
clips, opens the real built-in Sampler, and drives title-bar / mixer-handle input.
Each target warms up before a 4.8-second measured gesture. Native mode uses the
same HWND wrapper as vendor editors, with deterministic Sampler content.

| Target | Reported frame preparation p95, before → after | Submission interval p95, before → after |
| --- | --- | --- |
| Sampler window | 16.39 → 0.85 ms | 20.01 → 8.89 ms |
| Native editor wrapper | 7.42 → 0.79 ms | 15.38 → 9.01 ms |
| Mixer resize | 40.26 → 2.37 ms | 52.83 → 12.51 ms |

Direct instrumentation of **every** final scene capture, including captures not
delivered in frame telemetry, gives p95 of 0.93 / 0.94 / 2.69 ms respectively.
Sampler and native-window drags caused **zero** static timeline paints during the
final measured interval, versus 243 and 207 before the fix.

Raw JSON, logs and screenshots are under `.codex-artifacts/motion-perf/`:
`before-{sampler,native,mixer}.json` and `final-{sampler,native,mixer}.json`.
The pre-fix benchmark executable was retained locally as
`build-windows/bin/VLTONE-motion-before.exe` for repeat comparisons.

These are synthetic heavy-UI measurements, **not physical display FPS** or a
guarantee for arbitrary third-party DSP load. No audio device was opened, no
user project was loaded/saved, and preferences were isolated. Frame telemetry
can coalesce; direct capture timing therefore remains separately available.
The user's real heavy project and loaded vendor plugins still merit a manual run.

Run from a deployed build directory with Qt runtime paths configured:

```powershell
$env:DAW_PREF_DIR = 'D:\Code\DAW\.tmp\motion-review'
$env:VLT_GPU_WORKSPACE = '1'
$env:QT_QPA_PLATFORM = 'windows'
$env:QSG_RHI_BACKEND = 'd3d11'
$env:QSG_RENDER_LOOP = 'threaded'
$env:VLT_MOTION_TARGET = 'mixer' # sampler, native, mixer
$env:VLT_UI_PROFILE = 'D:\Code\DAW\.codex-artifacts\motion-perf\review.json'
cmd.exe /d /c .\VLTONE.exe --workspace-motion-check --language en
```

GPU rendering remains the existing opt-in setting; compatibility mode and the
user's saved preference were not changed.

## Verification

- Release app and GPU test built successfully.
- Hardware `gpu_scene_test --hardware`: passed on Windows/D3D11 with a separate
  render thread. Covers moved layer pixels, unchanged sibling/child paint counts,
  resizing, explicit updates during movement, and a Quick frame delivered before
  the queued QWidget exposure update. Existing context-menu tests cover both
  Windows release and macOS press policies.
- Mixer benchmark: revealed timeline pixels equal a forced complete repaint.
- All three final motion checks: exit 0.
- CTest: `gpu_scene_test`, `gpu_fallback_test`, `ui_frame_clock_test`,
  `mixer_fader_input_test` — 4/4 passed.
- `--selftest`, `--uiperfcheck`, `--plugin-interaction-check` — exit 0.
- `git diff --check` for app and tests: no whitespace errors.
