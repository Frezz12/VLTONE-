# Audio engine: heavy-session performance

The implementation keeps the existing graph, buffer arena, deterministic input order and PDC. It targets callback tail latency and bounded resource use, rather than replacing the engine.

## Implemented

- `shared/RealtimeMetrics.hpp`: fixed SPSC queues for individual callback/graph timings; lifetime block, deadline-overrun, maximum and dropped-event counters. `TimingAccumulator` calculates nearest-rank p95/p99/p99.9 outside DSP. Device input/output underflow and overflow are counted separately; RenderGate silence has its own counter. Optional per-worker node timings and active-pass wait times use separate preallocated queues. Wait rows describe aggregate dependency/work-stealing waits, not a direct measurement of OS wake-up latency.
- `AudioWorkerRegistration`: helper-thread registration follows device start, stop and replacement. macOS uses time-constraint scheduling and joins the output device's Audio Workgroup when available; Windows uses MMCSS `Pro Audio`. Registration/teardown happens on each helper between passes, not inside a DSP job. Failure retains the ordinary scheduling path. The default pool still uses all available logical cores (up to the existing 128-thread cap); diagnostic counts exclude the PortAudio callback itself.
- `BackgroundExecutor`: a shared two-worker pool replaces one sampler preparation thread per instrument. Admission is one task while any transport plays and two while stopped. A second task already running when playback starts is allowed to finish. Requests coalesce, obsolete generations are cancelled, and instrument destruction cancels queued work and waits for its running task. Offline sampler flushes still wait for the latest generation.
- `ClipPlayerNode`: snapshots contain a balanced interval index built on the control thread. Seek and ordinary playback visit intersecting intervals in stable start/order sequence. There is no 2048-clip active-set limit or fallback scan through past material. Traversal uses a logarithmic call stack and allocates no active collection in DSP.
- `ScopedNoDenormals`: scoped FTZ/DAZ on x86 and FTZ on Apple ARM64, including callback, graph workers and offline DSP. The prior FP mode is restored. Silence detection tests float bits so subnormal values and NaNs still wake sleeping plugins; both signs of exact zero remain silent.
- `PcmReadCache`: mapped PCM is read by a background reader into a fixed 256 MiB, eight-way page cache (4096 floats/page). Realtime reads pin cache pages without mutexes, allocation, or synchronous backing-file reads; a miss supplies silence and increments a counter. Requests include a seek epoch and source identity. Source destruction waits only on the control thread. Preparing play/seek warms bounded source windows; playback hints include upcoming clips, loop starts and stretch-window history. Offline rendering reads immutable backing storage directly. The cache attempts `mlock`/`VirtualLock`; diagnostics expose locked bytes. A refused lock is a fallback, **not a guarantee against swap faults under RAM pressure**. Cache PCM capacity is fixed; registry/queue/metadata have additional bounded-per-source overhead.
- Track context menu: **Freeze Track… / Unfreeze Track**, cancellable and undoable. Freeze renders an independent source, instrument and inserts to float WAV before the fader, preserving the original document and plugin instances. Fader, pan, mono and outgoing sends remain live. Isolating the render from downstream buses avoids baking their latency or truncating a tail because a downstream fader is quiet. Publication checks the source revision and source contents. Editing source DSP/material invalidates the frozen cache. State and WAV survive save/load/recovery; missing media falls back to the original chain. Cloud publication uses the original editable sources. Original instances remain resident, so this version saves DSP, not their memory.
- Graph task fusion: opt-in cheap built-in nodes in one-input, one-successor chains share a scheduler task. Node DSP calls, buffers and mixing order remain unchanged. Branches, sidechain edges, MIDI nodes and latency boundaries are excluded. `GraphProcessor::setTaskFusion` supports controlled A/B measurements. Full multi-block anticipative rendering is not implemented.

Freeze initially supports independent audio/MIDI/instrument tracks. Armed/monitored tracks, incoming sends/routing, external sidechains, linked Pattern clips, playback injections and external plugin-parameter automation are rejected with a menu explanation. Effect tails use the existing silence detector with a 30-second ceiling. Original media and generated freeze files must be retained for undo/redo; unfreezing does not delete a file still referenced by history.

## Reproducible checks

```sh
cmake --build build --target audio_realtime_test track_freeze_test engine_graph_test sampler_test meter_node_test platform_test time_stretch_test controller_test
ctest --test-dir build --output-on-failure -R '^(audio_realtime_test|track_freeze_test|engine_graph_test|sampler_test|meter_node_test|platform_test|time_stretch_test|controller_test)$' -j1
```

`audio_realtime_test` covers telemetry overflow/quantiles, xrun flags, denormal silence semantics, shared preparation admission/cancellation, cache misses/eviction/concurrent readers, mapped PCM seek versus offline output, 100k past clips and 3000 overlapping clips, RenderGate accounting, worker-registration teardown, and fused/unfused/serial output at 48/96 kHz and buffers 32/256/512. `track_freeze_test` covers actual plugin inserts, latency in the source/downstream bus, muted source material, pre-fader equivalence, removed DSP nodes, portable save/load, undo/redo, cancellation, revision rejection and source-edit thawing. Existing graph/plugin tests cover PDC, MIDI, sidechain and effect tails.

Local validation: the macOS application and benchmark targets built successfully. All 13 selected CTest suites passed (25.73 s): the eight listed above plus recovery, collaboration protocol, CLAP insert, VST3 and MIDI plugin tests. No Windows build was run in this environment.

## Benchmarks

Headless A/B (no audio-device claims):

```sh
cmake --build build --target audio_engine_bench load_bench
build/bin/audio_engine_bench --tracks 1000 --frames 256 --rate 48000 --blocks 400
build/bin/audio_engine_bench --tracks 1000 --frames 256 --rate 48000 --blocks 400 --heavy
```

Change `--frames` to 512/32, `--rate` to 96000 and `--workers` to compare pool sizes. Run benchmarks alone, without a concurrent build/test pass. The tool alternates fused/unfused order across three rounds. `--heavy` adds four biquad sections per source; these are deterministic synthetic effects, not third-party plugins.

Local observations, 2026-09-07, macOS 26.5.1, native build with `-O2`, eight default workers, 48 kHz/256:

| Synthetic session | Scheduler tasks before → after | Median of round means, unfused → fused |
| --- | ---: | ---: |
| 1000 sources, three gains and meter | 5001 → 2001 | 0.5570 → 0.4639 ms (about 17% less) |
| Same, plus four biquad sections | 6001 → 3001 | 3.1156 → 3.0233 ms (about 3% less) |

The light case used 200 measured blocks per round/mode; the heavy case used 400. In the isolated heavy run, fused maxima were 5.1420/4.5035/4.8212 ms, versus 4.6031/6.0195/5.6423 ms unfused. This is insufficient evidence for a universal p99.9 or sustainable-load claim. One earlier heavy run overlapped tests and is excluded.

Live-device acceptance (output is intentionally muted):

```sh
build/bin/load_bench --tracks 64 --plugins 2 --seconds 600 --rate 48000 --blocks 256,512
build/bin/load_bench --tracks 64 --plugins 2 --seconds 600 --rate 96000 --blocks 256,512
build/bin/load_bench --tracks 64 --plugins 0 --seconds 30 --rate 48000 --blocks 32,64,128
# Optional diagnostics, whose overhead must be reported separately:
build/bin/load_bench --tracks 64 --plugins 2 --seconds 30 --blocks 256 --profile /tmp/audio-nodes.csv
```

`--profile` also writes a `.nodes.csv` sidecar mapping graph generations/node IDs to names. Effects come from the local catalog; `--name` filters it. Missing plugin instances or dropped timing telemetry make a run incomplete. Each callback carries its own frame budget. Acceptance requires at least 600 seconds, no observed device xruns, no callback overruns, no RenderGate/PCM gaps and callback p99.9 below 80% of budget. Increase track/effect counts to find the maximum sustainable load, rather than comparing only average DSP percentages. A short run is labelled `SHORT_PASS`, never `PASS_10MIN`.

This execution environment exposes **no PortAudio output device**: the attempted 64-track 48 kHz/256+512 smoke test returned `no audio output device available`. The ten-minute hardware criterion and Windows runtime/MMCSS behavior have not been certified here. Test the reference machines with long master chains, sidechains, dense automation, time-stretch and many samplers; include seek, loops, routing changes, save/recovery and preparation under constrained RAM. Record PCM locked bytes and misses as well as callback quantiles. Native ARM64 Windows FP-mode support is outside the current x64 Windows target.

Platform references: [Apple Audio Workgroups](https://developer.apple.com/documentation/audiotoolbox/workgroup-management), [Microsoft MMCSS](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service).
