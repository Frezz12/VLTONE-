# Integration guide

## Build options

| CMake option | Standalone repository | Added as a subdirectory |
| --- | --- | --- |
| `VLT_SPECTRE_BUILD_WIDGETS` | OFF | OFF |
| `VLT_SPECTRE_BUILD_EXAMPLES` | ON | OFF |
| `VLT_SPECTRE_BUILD_TESTS` | ON | OFF |

`VltSpectre::Core` requires C++17 and lock-free 32-bit atomics. `VltSpectre::Widgets` links Core and Qt 6 Widgets. The core has no Qt include or runtime dependency. You can also compile `src/Analyzer.cpp` and `src/DisplaySmoothing.cpp` directly and add `include` to your search paths.

To install a package:

```sh
cmake --install build --config Release --prefix /your/install/prefix
```

In the consuming project, set `CMAKE_PREFIX_PATH` to that prefix:

```cmake
find_package(VltSpectre CONFIG REQUIRED COMPONENTS Core)
target_link_libraries(app PRIVATE VltSpectre::Core)
# Request Widgets only when needed:
find_package(VltSpectre CONFIG REQUIRED COMPONENTS Widgets)
target_link_libraries(app PRIVATE VltSpectre::Widgets)
```

Even an installation that includes Widgets can be consumed as Core without discovering Qt. `tests/package-consumer` is a minimal executable that checks the installed package.

## Thread ownership

Use exactly one producer and one consumer per analyzer.

| Operation | Owning thread | Allocation |
| --- | --- | --- |
| Constructor, `prepare()`, destructor | Setup/teardown; producer and consumer stopped | Allowed |
| `pushSample`, `pushStereo`, `pushInterleaved`, `reset` | Audio producer | None after preparation |
| `read`, `frame`, `setBallistics`, core `setFrozen` | One consumer | None after preparation |
| `smoothDisplaySpectrum` | UI or worker | Allocates |
| All `SpectrumWidget` operations | Qt UI thread | Allocates/copies as needed |

`prepare()` validates settings before replacing a working configuration. Invalid settings return false and optionally describe the error; allocation failure can throw `std::bad_alloc`. Stop both sides before changing sample rate, FFT size or frequency projection.

Capture never waits for a consumer and never performs an FFT. Every hop it copies the latest full stereo window into a private buffer, then exchanges ownership through a triple-buffer mailbox. The cost is bounded, but not free: the copy is O(FFT size) each hop. A larger hop lowers producer cost. Profile your actual callback sizes and CPU before choosing very large FFTs and very small hops.

`read()` takes the newest complete window and may skip intermediate windows. If no new window exists, it returns false and does not advance envelopes. This is appropriate for a live visual analyzer, not lossless offline frame collection. When an audio source stops calling `push*`, its picture stays at the last received state; feed silent samples while the transport is stopped if you want it to decay.

The returned frame is owned by the consumer. Do not concurrently access it from a different thread. If analysis runs on a worker, copy or transfer frames to the UI through your host's synchronized handoff. To serve several views, read once and share the result on the consumer side.

`reset()` belongs to the producer and may run concurrently with `read()`. It publishes an empty generation and waits for a new full window, preventing samples from an old stream from leaking into the next one. Freeze on the core stops consumption while capture continues; unfreeze returns to the latest data. Widget freeze is independent: it holds the displayed curve while your analyzers may keep running.

## Capture and analysis

```cpp
using namespace vlt::spectre;
AnalyzerSettings settings;
settings.fftSize = 8192;
settings.hopSize = 512;
settings.pointCount = 768;
settings.minimumFrequency = 20;
settings.maximumFrequency = 20000;

Analyzer before, after;
std::string error;
// Setup only: handle false/error before enabling the audio callback.
const bool prepared = before.prepare(sampleRate, settings, &error)
                   && after.prepare(sampleRate, settings, &error);
```

In the host's audio callback, capture input before processing and output afterwards:

```cpp
before.pushStereo(inputLeft, inputRight, frames);
processYourEffect();
after.pushStereo(outputLeft, outputRight, frames);
```

`pushInterleaved(data, frames, channels)` is available for interleaved floats. Mono is duplicated; inputs with more channels use the first two. Nonfinite samples are replaced by zero. No input buffer is retained. For separate L/R, mid/side or other custom combinations, prepare the desired signals in your host and use additional analyzers.

For a processor with latency, the input and output captures represent different source times unless the host aligns them. Delay the analysis-only input tap by the processor latency if you need a time-aligned comparison. Spectre does not own or alter the host's audio routing.

## Qt view and layers

Create a `SpectrumWidget` like an ordinary child widget. Call `setTrace()` from the UI thread after reading its analyzer. Stable string IDs update existing traces. `removeTrace()` and `clearTraces()` remove them. Malformed frames are rejected; `setTrace()` also returns false while frozen. Empty/unready frames are supported.

```cpp
SpectrumView view;
view.minimumDb = -90;
view.maximumDb = 0;
view.tiltDbPerOctave = 0;
view.highFrequencyLiftDb = 12;
view.highFrequencyStartHz = 2000;
view.highFrequencyEndHz = 16000;
view.smoothingOctaves = 1.0 / 6.0;
view.transitionMs = 180;
widget.setView(view);
widget.setTheme(SpectrumTheme::dark());
```

The high-frequency shelf is exactly zero at/below its start, rises smoothly on a log frequency axis, and reaches its configured lift at the end. Use `highFrequencyLiftDb = 0` and `tiltDbPerOctave = 0` for an unweighted scale. Set `smoothingOctaves = 0` for an unsmoothed contour and `transitionMs = 0` to disable presentation motion, including for reduced-motion preferences.

`TraceStyle` provides a label, line/fill colors, line width, pen style, visibility, `zOrder` and `fillReference`. Invalid/default colors select a theme-aware palette. Lower `zOrder` is behind higher values; equal orders retain insertion order. Supply an alpha channel in the fill color for transparency.

With `fillReference = "before"`, a trace fills only the area between itself and the trace named `before`, including cuts as well as boosts. A missing reference suppresses the fill while keeping the line. For a meaningful comparison, use matching sample rates, FFT parameters, frequency grids and capture timing. The library intentionally does not compute an output spectrum from an EQ curve: send actual output audio.

`setTraceStyle()` works while frozen. Legend labels, grid, theme and time constants remain controlled by the host. Disable `legend`, `frequencyLabels`, `decibelLabels` and `interactive` for a miniature spectrum. The widget never changes application-wide styles or creates audio devices or workers.

For a custom QPainter composition use `spectrumPath(frame, plotRect, view)`. Clip the returned path to your plot. It applies visual smoothing and weighting but has no internal animation state. For a non-Qt renderer use the core arrays and optional `smoothDisplaySpectrum()`; the Qt widget is not required.

## Reading values correctly

The cursor reports the analyzer's time-smoothed `levelsDb` without visual smoothing, tilt or shelf. The strongest-peak label in the demo uses the instantaneous FFT peak estimate. Those can differ briefly during a transient. The marker follows the rounded display contour; its y position is not itself a calibrated dBFS reading when visual processing is enabled.

Peak-amplitude calibration, stereo power averaging, window length and limitations are described in [architecture.md](architecture.md). A plotted layer difference is a visualization of two envelopes, not a transfer-function measurement.
