# GPU workspace — implementation and acceptance

Updated 2026-09-10. The integrated Metal/D3D11 workspace is available for testing
behind **Settings → Interface → GPU rendering (experimental)** and a restart.
The compatibility workspace remains available. Hardware acceptance, including
Windows and long recordings, is required before changing the default.

## Scene and editing

`WorkspaceSurface` owns one native `QQuickWindow` for the main workspace. Ordinary
panels, editor canvases, browser pages and media share its scene and stacking order.
macOS selects Metal; Windows selects D3D11. The threaded scene graph owns GPU
resources. There is no QQuickWidget/QRhiWidget heavy canvas or full-canvas screenshot
in the GPU rendering path.

C++ `ScenePaintSource` methods submit vector primitives and immutable numerical
geometry through `SceneRecorder`. `SceneItem` retains QSG nodes and shared geometry
groups. The render thread sees `SceneSnapshot` with presentation and project
revisions, viewport, geometry, immutable image assets and opaque Quick visual IDs;
it never dereferences a QWidget or mutable project/controller object.

The existing QWidget hierarchy still supplies layout, focus, accessibility and
editing handlers. Small platform controls (including native line edits and focus
frames) use cached full-DPI images because macOS style code can require a CGContext.
This is an integration of the existing C++ interface into a shared GPU scene, not
a rewrite of every editor controller into a widget-independent component. Input,
text shaping, path preparation and numerical model work still use CPU.

### Retained content

- Timeline: visible time tiles for grid, clips, MIDI previews and lanes, plus
  numerical waveform LOD and retained waveform geometry. Scroll changes transforms;
  newly visible tiles and unfinished recording tails are rebuilt.
- Piano Roll: tiled grid/notes and velocity, pan and CC values. Hover, selection,
  handles and edit overlays remain current. No QML object per note/sample.
- Automation: retained static scene and current edit overlays.
- Sampler: background peak extraction has two low-priority workers, at most one
  in-flight job per view and only the latest replacement. Generation checks reject
  stale results. Wave geometry is retained while markers/fades are dragged.
- Pattern sketches, Sampler envelope/keyboard, mixer meters, EQ, Gravity, Graphit,
  transport and spectrum canvases submit geometry directly. Hidden views do not
  keep their own presentation loops running. Their changing data still requires
  bounded CPU preparation; numerical particle/curve calculations are not all shaders.

Timeline and Piano Roll retain existing scroll direction, zoom anchors, snapping,
selection, drag/drop and Undo/Redo. No extra input smoothing was added. Pointer
coordinates, wheel phases, timestamps and native gestures are forwarded unchanged.
Interactive WebEngine items own their mouse/keyboard/IME events; C++ overlays take
precedence at hit testing. The container keeps a focus proxy to the active control.

Native child windows were removed from the GPU shell's legacy panel hierarchy.
They caused macOS to copy obscured raster backing stores even after GPU rendering.
Native plugin editors instead open as separate auxiliary windows. Built-in plugin
panels remain in the shared scene. The window container deliberately does not
occlude QWidget damage notifications; changing a normal control must still update
its GPU representation.
The native CPU-load progress bar updates its value immediately but schedules paint
normally; `QProgressBar::setValue()` previously forced a synchronous native flush
inside the GUI timer (observed p95 about 9 ms during 32-frame DSP playback).

Editors can detach and dock without replacing their content/model. A detached
editor gets its own Quick window and render resources. Docked geometry is preserved
separately from desktop coordinates. Notebook auxiliary windows also use Quick.
Persistent Quick pages can transfer before the old scene consumes their removal;
cleanup checks the current item parent so it cannot detach the new owner's page.

## Media and browser

- Photos upload when the image changes. Placement, opacity, tiling and two-pass
  blur use Quick textures and compiled shader resources.
- Local video uses MediaPlayer → bounded `VideoFrameGate` → VideoOutput. Raw video
  frames stay on the native media path; the GPU path does not call `toImage()`.
  Hardware decoding depends on the installed Qt backend and codec. The local macOS
  backend accepts the H.264 test fixture but does not decode its VP8/WebM counterpart;
  Chromium handles the WebM web fixture.
- GIF decoding runs outside the GUI thread, with bounded workers and a latest-frame
  policy. Obsolete source generations are discarded. Quality/FPS caps limit uploads.
- Browser and web backgrounds use native WebEngineView in the scene. GPU web
  backgrounds never use `grab().toImage()`. Tabs keep their real page, history and
  downloads while hidden or remounted. Existing chrome, bookmarks, download/import
  handling, permission policy and settings remain shared C++ code.
- A shared BrowserProfile owns either the Quick or compatibility profile, using the
  previous VLTStudioWeb storage paths. The two implementations never open that
  storage simultaneously. If the main scene fails, open Quick browser pages can be
  remounted in a native fallback window without losing state. A decorative web
  background is suspended, with its descriptor/position preserved.
- Notebook uses the same Quick browser adapter with a separate off-the-record
  profile. A narrow WebChannel exposes only document commands; moving the editor
  between windows preserves the page and its selection/document state.

## Quality, FPS and synchronization

Quality and FPS are independent preferences. New preferences default to Auto and
Display; saved user choices are preserved.

| Quality | Decorative intermediate texture | Animated background cap |
|---|---|---|
| Maximum | 100% of display size × DPI | Source/render rate |
| Medium | 75% per side | 30 FPS |
| Low | 50% per side | 15 FPS |
| Auto | Starts at Maximum | Adapts between the levels above |

Text, notes, waveforms, grid, playhead and controls keep native DPI at every level.
User blur is independent. Maximum never downgrades automatically. Auto lowers after
2 seconds above 80% of the frame budget, or after a new real audio xrun during
active graphics; it raises one level after 10 seconds below 60%. Changes are at
least 2 seconds apart. Multiple active windows use the busiest normalized load;
idle windows cannot reset overload detection. Compatibility mode also reports GUI
paint load. Audio settings are never changed by this policy.

Display follows the window's current screen. Fixed retains the selected limit.
Unlimited removes the software cadence limit and requests swap interval zero;
actual presentation remains controlled by the OS/display. Native media/Chromium
may continue internal decoding/compositor work independently of the background
texture refresh cap. Swapchain recreation waits until mouse/drag/native gestures
finish, preserving the scene and continuing audio.

Quick's `afterAnimating` supplies the common presentation frame, FrameTimer pulse
and scene capture before synchronization. Pending state is coalesced; missed frames
are not replayed. Static scenes do not request continuous rendering. Telemetry also
uses a single latest-frame mailbox.
Coalesced telemetry preserves total render-thread CPU and the longest submission
gap, and reports how many samples were combined. Benchmarks must inspect that count
before treating the delivered samples as a complete interval distribution.

The PortAudio ADC/DAC timestamps and bounded `AudioPresentationSnapshot` history
are connected to transport generations. One output-clock estimate is shared within
a GUI presentation frame, with the last valid tuple used on read contention.
Seek/loop/restart/device changes invalidate incompatible history. Graph latency is
accounted once. Estimated timestamps remain distinguishable from device timestamps.
The audio callback does not invoke Qt/GPU, wait for rendering, or allocate graphics.

## Resource limits and recovery

- Retained geometry: 32 MiB / 64 entries per tile cache; waveform LOD geometry has
  its separate 64 MiB LRU.
- Shared render-thread texture cache: 128 MiB per window across scene segments.
- Visible geometry admission: 64 MiB; path inputs are capped before triangulation.
  Qt's triangulator can still have temporary allocations beyond final geometry size.
- Native control image admission: 512K pixels per control.
- Image/GIF/peak jobs have bounded workers and source-generation checks.
- Resize/DPI/font/palette changes invalidate the relevant recordings and assets.
- Unsupported operations, unavailable hardware or resource failure restore the
  existing compatibility hierarchy and keep the project/controller alive.

## Build and verification

Qt dependencies: Quick, Qml, QuickControls2, Multimedia, ShaderTools, WebEngineQuick,
WebEngineWidgets and GuiPrivate. Limited-compatibility QRhi timing is isolated in
`GpuTiming.cpp`; the Qt private triangulator is isolated in `SceneRecorder.cpp`.
Windows CI targets Qt 6.8.3; this Mac uses Qt 6.11.2. QML imports are passed to the
platform deploy tool, and blur/tile QSB shaders are compiled into resources.
The macOS packaging script prunes unrelated nested modules copied by macdeployqt
using Qt's transitive import scanner, preserves directory-import assets and checks
all required imports and bundled Mach-O dependencies before signing.

```sh
cmake --build build --target VLTONE gpu_scene_test gpu_browser_test gpu_media_test -j 4
ctest --test-dir build -R 'gpu_|graphics_quality|waveform_|ui_frame_clock' --output-on-failure
VLT_GPU_WORKSPACE=1 build/bin/VLTONE
```

Native checks (run on a graphical desktop, sequentially):

```sh
QSG_RHI_BACKEND=metal QSG_RENDER_LOOP=threaded build/bin/gpu_scene_test --hardware
QSG_RHI_BACKEND=metal QSG_RENDER_LOOP=threaded build/bin/gpu_browser_test
QSG_RHI_BACKEND=metal QSG_RENDER_LOOP=threaded build/bin/gpu_media_test --hardware
VLT_GPU_WORKSPACE=1 build/bin/web_video_background_test --gpu
VLT_GPU_WORKSPACE=1 build/bin/VLTONE --editorcheck
```

On Windows use D3D11 and the matching executable paths. Headless tests validate
logic and fallback, not hardware smoothness.

`--project-scroll-check <copied-project.vlt>` drives the real workspace through
native wheel forwarding. `VLT_SCROLL_MS`, `VLT_SCROLL_HIDE_MIXER` and
`VLT_UI_PROFILE` configure the run. `VLT_SCROLL_AUDIO_BLOCK=32|64|128|512` adds
synthetic production DSP callbacks, not a physical device. `VLT_SCROLL_PLUGIN_EDITOR=1`
also opens the first available native insert editor alongside the GPU scene.
Never use the original project package for destructive/test fixture work.

## Acceptance still required

The test build is not a claim of full hardware acceptance. Before enabling GPU by
default, validate Metal and D3D11 on real machines, dense audio/MIDI, video backgrounds,
all four buffers, recording, multiple monitors/DPI/refresh rates, sleep/wake/device
loss, keyboard/IME/accessibility and native plugins. Perform 30-minute physical
recordings and audio/visual loopback measurements.

Diagnostics measure preparation, synchronization, render-thread CPU, completed GPU
work and Qt `frameSwapped` intervals. GPU timestamps are asynchronous (disable with
`VLT_GPU_TIMESTAMPS=0`). **frameSwapped is submission, not physical scanout.** Actual
presentation p95/p99 and audible playhead phase need platform measurement; the code
must not report submission timestamps as those acceptance results. Audio history
covers 512 blocks; extreme output/plugin latency outside that history is a limit.

See the [completion report](../../reviews/2026-09-10/gpu-integration-completion.md)
for measured before/after results, test evidence and the packaged build.
