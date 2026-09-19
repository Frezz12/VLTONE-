# Render review — 2026-09-19

Scope: offline export, plugin state transfer, output validation and heavy audio-only projects. No installer was built or installed. Unrelated working-tree changes are out of scope.

## Findings addressed

1. **P1 — Native VST3 edits could disappear during export.** The render clone only copied pending events from `PluginNode`; native-editor edits live in `Vst3Instance::m_editorEdits`, and an unprocessed preset change may live in its parameter-sync queue. Component state alone does not include those unprocessed changes. The new regression failed before the fix (`DAW Test Gain VST3 / track: export retains audible settings`). The format now exposes a non-consuming pending-event snapshot; clone restoration applies component state, then actual pending edits, in processing order. It does not replay the entire potentially stale controller mirror. Track, group, master, clip and dual-mono scopes are covered, including preservation of the live queue.

2. **P1 — Failed state capture could silently reuse recovery data.** Export called `captureRecoverySnapshot`, whose recovery cache intentionally retains a previous chunk when `saveState` fails. That is useful for crash recovery but not a faithful export of the current sound. Export now captures fresh native state and reports failure instead of publishing an outdated mix. CLAP without the optional state extension retains parameter fallback. Bypassed effects and unrelated freeze-track effects are not captured. Regression: prime the recovery cache, force VST3 save failure, assert no published file; dry export must still succeed.

3. **P1 — Invalid DSP samples could be published as successful audio.** Float output accepted NaN/infinity. Integer output called `llround` before clipping, so huge finite samples exceeded the integer conversion range. The writer now rejects non-finite samples and clips finite values before conversion. Tests cover Float32/Int24, positive and negative extreme values, and a CLAP fault injecting NaN during export. The existing output transaction removes failed partial renders and preserves previous exports.

4. **P2 — Small realtime buffers imposed unnecessary offline scheduling overhead.** Audio-only export inherited the device buffer, even though it has no device deadline. Automatic export now uses at least 1024 frames. MIDI/pattern/automation arrangements retain device granularity because enlarging their blocks would change event/automation resolution. `Spec::blockSize` permits an explicit compatibility/benchmark size up to 8192. A block-size probe tests all three paths. The clone also starts in offline mode and reuses the live plugin catalog instead of loading that catalog from disk again.

5. **P2 — Invalid render requests were insufficiently checked.** NaN/infinite rates and ranges, negative tail settings, sample-position overflow and stem-only exports with no surviving channels now fail explicitly. Regression tests verify that no output files are published.

Relevant implementation: `controller/EngineControllerRender.cpp`, `plugins/Host/PluginNode.cpp`, `plugins/Vst3/Vst3Instance.cpp`, `core/platform/AudioFileDecoder.cpp`.

## Measurement method

`render_bench` is opt-in (`EXCLUDE_FROM_ALL`), not a timing-gated CTest. It builds a 48 kHz stereo audio arrangement with generated source audio and CLAP test-gain inserts, then measures the complete `renderProject` call (snapshot, clone, DSP and file writing). Fixture/project construction is excluded. Each configuration runs three times; the median is reported. Every generated file is decoded and compared sample-by-sample across repetitions/device-buffer settings.

64 tracks / 256 inserts / 30 seconds, before and after this review's implementation:

| Device block | Before (device-sized render) | After (automatic 1024) | Ratio |
| --- | --- | --- | --- |
| 128 | 7.78918 s | 1.74187 s | 4.47x |
| 512 | 2.53521 s | 1.76802 s | 1.43x |
| 1024 | 1.72700 s | 1.68831 s | Within normal timing variation |

Maximum sample difference across repetitions/device settings was zero in both benchmark runs. Before/after runs were separate invocations, not an archived cross-binary audio comparison. Sample-exact render/reference comparisons are additionally covered by the integrity suite. Post-change measurements were taken after builds and tests finished. These are synthetic scheduling-heavy measurements, not a promised speedup for commercial convolution/oversampling plugins.

Post-change individual times (seconds): 128: 1.74187, 1.85661, 1.70633; 512: 1.63008, 1.76802, 1.90308; 1024: 1.54464, 1.68831, 1.88264.

Reproduce from an MSVC developer prompt:

```powershell
cmake --build build-windows --target render_bench -j 6
build-windows/bin/render_bench.exe 64 4 30
# Optional explicit block size, independent of device size:
build-windows/bin/render_bench.exe 64 4 30 128
```

## Verification

The final affected targets compiled successfully with MSVC 14.44 in the existing Release build. All 12 selected CTest suites passed (9.76 seconds): `plugin_render_state_test`, `render_test`, `render_safety_test`, `render_integrity_test`, `processing_render_test`, `plugin_vst3_test`, `track_freeze_test`, `platform_test`, `recovery_test`, `plugin_host_test`, `plugin_midi_test`, `plugin_batch_test`. This was not a full application/repository test run; Audio Units are not exercised on Windows.

```powershell
ctest --test-dir build-windows -R "^(plugin_render_state_test|render_test|render_safety_test|render_integrity_test|processing_render_test|plugin_vst3_test|track_freeze_test|platform_test|recovery_test|plugin_host_test|plugin_midi_test|plugin_batch_test)$" --output-on-failure -j 2
```

Implementation follows the existing clone, render-gate and output-transaction design. No new dependency, scheduler or plugin worker architecture was introduced.

## Limits

The original full user project with commercial plugins has not completed a verified end-to-end render in this investigation. The earlier long-running headless attempt was cancelled. These fixes address demonstrated failure mechanisms; they do not establish that every part of the reported late/one-sided instrumental is resolved. Verify that project in the application before releasing a setup build. Audio-only block enlargement can change behavior of block-sensitive third-party processors; explicit block override is retained for comparison. MIDI/automation-heavy projects intentionally do not receive that optimization.
