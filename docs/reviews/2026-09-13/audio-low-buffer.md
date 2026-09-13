# Audio engine: low-buffer overhead and PCM residency

## Scope

Windows x64, MSVC 14.44, Release, 48 kHz, 24 logical processors. The engine
baseline was built from the unchanged engine files at
`f938ff333a645db9c0435b1cb1e907afd723be2d`. Existing unrelated UI, web and admin
changes were preserved. This is a targeted change to the current engine, not
a replacement scheduler, plugin API or audio backend.

## Changes

1. `PcmReadScope` initializes and releases only the page cursors actually used
   by a node. Previously every node cleared eight records and examined them on
   destruction, including faders, meters and plugins that never read mapped
   audio. Used cursors remain pinned for the scope; replacement is still bounded
   to eight slots. Copying an owning scope is explicitly prohibited.
2. `WorkStealingDeque::pop` returns early for an already-empty owner queue.
   Only its owner can push, and thieves can only advance the top, so this case
   needs no bottom write or full memory fence. The existing fence and last-item
   compare/exchange remain on the nonempty/racing path.
3. `RealtimeSnapshot::read` skips hazard publication for an initially null
   pointer. The nonempty path retains publication/validation barriers and
   control-thread reclamation, without an extra initial atomic load.
4. PCM replacement now uses a fill-order clock separate from the prefetch tick.
   Previously several fills within one tick received the same age. Once the
   cache was full, tie-breaking could repeatedly select the same slot and evict
   a page warmed moments earlier. A new test reproduced the lost residency.
   Successful fills advance the age off the audio thread; realtime readers
   still perform only a relaxed age load. Prefetch cadence is unchanged.

The fourth issue was found during sanitizer testing: the existing concurrent
PCM-reader test failed intermittently. A separate warm-range regression test
then failed before the replacement-age fix and passed afterwards. This is a
residency/correctness improvement, not a measured percentage reduction in DSP.

No audio precision, plugin quality/oversampling, tail handling, MIDI ordering,
latency compensation, bypass semantics or worker priority policy was reduced.

## Measurement method

`audio_engine_bench` now supports `--plugins N --vst3 PATH`, constructing real
hosted VST3 instances behind each track. It also fixes Windows CPU accounting:
MSVC's [`clock()` measures elapsed wall time](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/clock?view=msvc-160),
so the benchmark now uses process user + kernel time from
[`GetProcessTimes`](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes).
The same benchmark changes were built into the preserved baseline executable
**before** production engine edits. CPU time is summed across threads and is
different from the time to finish one audio block.

The principal fixture has 128 tracks, three DAW Test Gain VST3 instances per
track (384 instances), three gain nodes, a meter and the master sum. It has
1,025 nodes / 641 fused tasks. Fusion is the normal engine setting. Each run
also exercises unfused processing, alternating order between rounds.

Example PowerShell invocation from the repository root:

```powershell
.\build-windows\bin\audio_engine_bench.exe --tracks 128 --frames 32 --workers 0 --plugins 3 --vst3 D:\Code\DAW\build-windows\plugins_test\DawTestGainVst3.vst3 --blocks 10000 --rounds 3 --realtime-workers
```

The frozen baseline is `build-windows/bin/audio_engine_bench-before.exe`.
Raw outputs and build/test logs are under `.codex-artifacts/audio-low-buffer`.
Benchmarks were run sequentially; this task did not launch compilation or
tests alongside them. A later validation attempt was contaminated by another
build in the shared workspace, as described below.

### Initial repeated A/B series

Order: baseline, candidate, candidate, baseline; three rounds per invocation,
10,000 measured blocks per mode. Values below are means of six fused-run
results, not pooled quantiles. The candidate was the first three optimizations
before the final snapshot-load refinement and PCM replacement fix.

| Buffer | Block time before | Block time after | Change | Process CPU before/after per block |
| --- | ---: | ---: | ---: | ---: |
| 32 | 49.85 µs | 44.17 µs | −11.4% | 1,036.47 / 957.05 µs |
| 64 | 55.70 µs | 52.55 µs | −5.7% | 1,169.28 / 1,138.82 µs |
| 128 | 74.60 µs | 76.27 µs | +2.2% | 1,572.65 / 1,607.55 µs |

Raw files: `final-{before,deque}-vst3-{32,64,128}.txt`. No fused block in
this series exceeded its nominal buffer budget. The 128-frame case did **not**
demonstrate an improvement. Eight-worker exploratory runs showed a smaller
benefit than the all-core, 32-frame case.

Additional `final-*-heavy.txt` and `final-*-serial.txt` runs had substantial
late-run timing outliers in both variants; they do not establish a dependable
improvement. The `--paced` experiment tests worker parking/waking, but Windows
sleep granularity is not an audio device's callback cadence. Its short CPU
samples are too coarsely quantized for percentage claims.

### Contaminated final-code attempt

`validated-{before,after}-{32,64,128}.txt` preserves an attempted repeat with
the final code. Another Ninja/link process appeared in the shared workspace
during that run. Both variants showed large scheduling stalls and nominal
budget misses. This run is **excluded** from the performance conclusion;
its apparent percentage improvement must not be presented as confirmation.
MMCSS also varies thread priority with consumed CPU quota, so a continuously
busy throughput loop is not interchangeable with device callback timing
([Microsoft MMCSS documentation](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service)).
The exact cause of each stall was not established by an ETW trace.
Subsequent guarded retries (`quiet-*`, `idle-*`) were also interrupted when
new shared-workspace activity was detected. `idle-*` deliberately used normal
thread priority, unlike the application's audio configuration, and showed
substantial drift. These partial diagnostic runs are not headline results.

### Final-code comparison with audio worker priorities

`confirmed-{before,after}-{32,64,128}.txt` completed successfully. Compiler,
linker, Ninja, app and test processes were checked before and after each
invocation; no competing activity of those types was detected. The command
used `--realtime-workers`, two rounds and 5,000 blocks per mode, in
baseline/candidate/candidate/baseline order. Each cell averages four fused
run results (20,000 measured fused blocks per variant and buffer).

| Buffer | Block time before/after | Block-time change | Process CPU before/after per block | CPU change | Mean run p95 before/after |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 | 49.35 / 49.12 µs | −0.5% | 1,025.00 / 967.98 µs | −5.6% | 73.77 / 102.32 µs |
| 64 | 58.38 / 53.35 µs | −8.6% | 1,198.45 / 1,109.35 µs | −7.4% | 90.02 / 77.40 µs |
| 128 | 72.58 / 67.62 µs | −6.8% | 1,532.00 / 1,461.72 µs | −4.6% | 111.28 / 102.28 µs |

The final candidate's CPU cost was lower in all three cases. **There is no
repeatable block-latency improvement established at 32 frames**: its mean was
essentially unchanged in this series and its mean run p95 was worse. The
initial 11.4% result is not a guaranteed 32-frame gain. At 128 frames the two
series also differ; use the final result as an observation, not a universal
promise. These small samples include OS scheduling variation and do not
establish statistical confidence for a user's project.

One baseline 32-frame block exceeded the nominal budget; the candidate had
zero such blocks in this final fused series. This remains a synthetic budget
counter, **not** a hardware xrun measurement.

Frozen final benchmark SHA-256:
`2A673811EB313A6C6351AFF4BB8D365EFC7E8B1BD7A27376571C0357F1013E2E`.
Frozen baseline SHA-256:
`0F92745DB733D148FABF9B6DA7A1C08800B20077408696B95444312A8C836A55`.

## Verification

The current Release application is `build-windows/bin/VLTONE.exe`.
`cmake --build build-windows --target daw -j 2` returned success with no work
remaining after the shared-workspace builds finished (`build-release-verified.log`).
An earlier link attempt encountered a temporarily busy executable during those
concurrent builds. The previously packaged installer/ZIP was not regenerated
as part of this audio optimization pass.

The final Release audio/plugin suite passed **29/29** tests in 67.38 seconds:
engine graph/realtime, meters, automation, VST3/VST/CLAP hosting, MIDI, sampler,
built-in effects, resampling/time stretch, rendering/integrity, recording,
monitoring, track creation and freeze. Log: `ctest-release-final.log`.
This was a focused suite, not a claim that the entire application's tests
pass. The earlier Release packaging run's failures (including `controller_test`
and `m4a_import_test`) were not addressed or re-certified by this patch.

New regression coverage in `audio_realtime_test` checks:

- repeated last-item stealing and empty-queue reuse;
- exactly-once delivery of 65,536 tasks between an owner and three thieves,
  across 512 refill cycles, with a bounded timeout;
- null/non-null snapshot publication, outstanding-reader lifetime and
  concurrent snapshot coherence;
- warming a range in a full PCM cache without self-eviction;
- the initialized PCM cursor prefix, same-page reuse, eight-slot rotation and
  nested realtime/offline scopes.

MSVC AddressSanitizer: `audio_realtime_test` passed five consecutive final
runs, and `engine_graph_test` passed. The latter also checks deterministic
parallel output, serial equivalence, graph handover and latency/transport
behavior. No sanitizer memory error was reported. Logs:
`asan-realtime-fixed-{1..5}.log`, `asan-graph-final.log`.
The ASAN build reused the already-downloaded ONNX SDK; runtime DLL lookup
required the MSVC ASAN and vcpkg binary directories on `PATH`.

## Interpretation and remaining limits

- The VST3 fixture is deliberately cheap and deterministic. It measures host
  and scheduling overhead, not the DSP cost of a commercial synth or reverb.
- The benchmark leaves automation snapshots unset. The controller can publish
  non-null empty automation objects, for which the null-snapshot fast path
  does not apply. Do not extrapolate that component to every real track.
- The throughput benchmark has no audio device, driver callback, GUI load or
  user project. Passing the nominal time budget is not proof of zero ASIO/WASAPI
  xruns. Sustained throughput with MMCSS workers also differs from periodic
  device callbacks.
- A small trial with installed OTT completed its timing output but hung during
  teardown and was interrupted. It is **not** a successful vendor-plugin result
  and was not used for the percentage claims.
- A heavy serial plugin chain can remain the critical path despite idle cores.
  No attempt was made to parallelize dependent inserts or change their audio.
- The graph already avoids callback allocation, uses bounded queues, fuses
  eligible cheap nodes and preserves deterministic summation. Removing their
  synchronization/clears indiscriminately would risk lost tasks or stale audio.

The next meaningful measurement is the user's heavy project on its actual
driver, sample rate and buffer, with block p95/p99/max, callback xruns and node
profiles collected together. The existing controller profiling interface can
separate plugin DSP from host scheduling; neither the current measurements nor
the changes establish a universal 10–20% total-project CPU saving.
