# VLT Pitch validation — 2026-09-19

## HTML editor and 0 ms follow-up

The subsequent UI stability pass coalesces drag writes to presentation frames,
holds optimistic edits until host acknowledgement, and updates only changed
DOM values. Readouts keep a fixed decimal format and width, update at 20 Hz
during dragging, and settle immediately on release. Knobs use a 320-pixel
range (tenfold finer with Shift), independent compositor markers and no hover
filter. The Widgets browser backing surface is opaque; the shared Quick scene
retains alpha for its rounded corners.

Regression checks pass on Widgets at 100/150/200%, standalone Quick, and the
native Windows Direct3D11 workspace. During playback a sustained drag outside
the panel stays monotonic, retains visible keyboard pixels, commits the exact
last value without stale readback, and undoes in one step. Offscreen Qt uses a
software scene graph, so the hardware workspace check runs with the Windows
platform plugin. Stage startup and all 86 runtime PE dependencies pass.

The editor now uses embedded HTML/CSS/JavaScript and the existing browser
surface/controller. Fresh instances and dial reset use 20 ms / Humanize 0 /
Vibrato 0. The stored `tune` parameter remains compatible with earlier projects.
The native painted controls were removed only from this plugin. Its shell has
the standard host title bar/top corners, no nested outline, and rounded bottom
corners. Piano keys use layered glass highlights. Material tints and active
controls derive from the application accent, including custom theme colors.

At 0 ms, clean periodic note changes use shorter confirmation and narrower
hysteresis, retaining a quantized plateau during confirmation. The separate
1 ms anti-click ramp and uncertain/consonant protection remain. The clean-glide
regression reaches the destination 44 ms earlier than the 0.5 ms setting in
both modes; mean plateau error is 0.517 / 0.516 cents (RT / HD). The prior
uncertain-excursion and voiced-accent regressions still pass.

The new Hard renders of the complete 131.244-second supplied vocal have no
nonfinite/clipped samples, no detected broadband discontinuity candidates, and
no level-loss regions below -3 dB lasting at least 10 ms. On the 24-second
comparison, Natural produces exactly the same audio samples as the previously
accepted build. These are numerical checks; earlier subjective acceptance
below predates the stronger Hard transitions. New renders are under
`.cache/pitch-validation/snap-ms` and `snap-ms-full`.

Web checks cover real pointer dragging on both Qt browser backends, Shift,
keyboard repeat/undo grouping, numeric milliseconds/reset, presets, automation,
quality guards, note masks, hidden-window timers and rendered pixels.
Accent changes were checked on first load, while open, and after reopening a
hidden editor. Widgets passed at 100/150/200% scale and Quick passed at 100%;
dark, amber and light-theme accent previews retain the dimensional materials.
This sandboxed Windows test runner needs `QTWEBENGINE_DISABLE_SANDBOX=1` and
`QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu` for its offscreen Chromium child.
These are test-launch environment overrides; production browser sandbox
settings are unchanged.

## Implemented behavior

- Native effect `daw.pitch-corrector`, mono and linked stereo; versioned state,
  automation, controller edits/undo, presets, collaboration allowlists and PDC.
- Compact 640×410 Qt editor (minimum 560×360), dedicated dials and dimensional
  piano keys. Actual host editor checked at 100%, 150% and 200% scaling.
- Real-Time reports `ceil(sampleRate × 0.010)` samples; HD reports
  `ceil(sampleRate × 0.080)`. Dry/consonant timing and pitch acquisition are
  distinct. Neither mode is advertised as zero-latency monitoring.
- HD uses the longer YIN analysis and lookahead pitch path, aligned to the
  waveform/LPC synthesis. It replaces the originally planned spectral backend.
  The user confirmed that the metallic timbre disappeared on the supplied vocal.
- Real-Time no longer anchors the read head on an amplitude accent alone.
  Target confirmation rejects short uncertain excursions, and splice positions
  follow actual waveform correlation without repeatedly jumping back and forth.
  The user confirmed the reported 00:04–00:05 click and tuning jump disappeared.
- Reported tail includes 80 ms of filter settling beyond the processing latency.

## Automated checks

All six focused CTest suites pass:

| Suite | Evidence |
| --- | --- |
| `pitch_corrector_test` | Stable pitch within 5 cents, A4 432/440/442, scale/custom notes, sample rates, exact dry delay, variable block partitions, sample-offset automation, linked stereo, finite samples and allocation-free processing/reset. |
| `pitch_corrector_envelope_test` | Short uncertain pitch excursion versus genuine destination, voiced accents without phase resets, breath transitions, held-phase attacks including pre-onset scanning. |
| `pitch_corrector_formant_test` | Six analytic source/filter vowels; calibrated envelope accuracy and improvement over formants disabled. |
| `pitch_corrector_phase_test` | Known harmonic pulse shape with vibrato; relative harmonic phase at 44.1/48/96 kHz. The rejected spectral backend fails this regression. |
| `pitch_corrector_integration_test` | Safe quality deferral, undo/remote state, offline quality, processed monitoring with dry capture, PDC, actual wet last-syllable master/stem export and freeze against an independently flushed reference. |
| `pitch_corrector_ui_test` | Controls, gestures/undo, preset behavior, keyboard/numerical input, accessible piano keys and minimum layouts. |

The release application builds and its offscreen native editor smoke run exits
successfully. The existing whole-application self-test has an unrelated
TrackVolume→TrackPan automation-retarget failure; it is not reported as passing.

## Vocal validation and limits

The provided private vocal was processed locally in full (131.244 seconds),
in both modes with Natural and Hard. Its source was neither modified nor
uploaded. All four complete outputs are finite, below full scale, and preserve
exact silence in the 74.245 seconds of measured silent interiors. Numerical
checks supplement the user's audition; they do not replace listening.
Across 175 active windows, median envelope displacement was 0 ms and the 95th
percentile was at most 3 ms; maxima were 4 ms (Real-Time/HD Natural) and 5 ms
(HD Hard). These are measured vowel-envelope movements, not changes in the
exact dry-path/PDC reference. Very short boundary windows showed level
differences, but none remained below −3 dB for 10 ms; wider 50–250 ms windows
at those outliers differed by less than 0.62 dB. No new broadband discontinuity
candidates were found in the full-file numerical check.
The separate short-breath fixture uses six low-voice durations followed by
short high-frequency consonants and higher notes. All four mode/preset renders
preserve the measured consonant threshold crossing exactly (0-sample deviation)
and its 24 ms energy to numerical precision.

The real-vocal spectral prototype had a large relative harmonic phase error
despite passing steady pitch and formant-envelope checks. The accepted waveform
HD prototype reduced the median measured phase-shape error from approximately
1.54 to 0.016 radians. This metric is diagnostic, not a perceptual quality score.

Benchmarks cover 44.1/48/96 kHz and blocks 32/64/128/512; irregular partitioning
is covered by the deterministic DSP tests. HD's exact YIN sums are distributed
over the existing lookahead budget. Before/after comparisons produce zero
changed samples on the accepted 24-second vocal and synthetic signals at
12/48/96 kHz. In the final isolated 32-sample-block benchmark, worst p99 across
both modes/presets was 155.4 µs at 44.1 kHz, 159.9 µs at 48 kHz and 176.2 µs at
96 kHz (deadlines 725.6, 666.7 and 333.3 µs respectively). The 96 kHz maximum
still reached 628.7 µs in this normal-priority desktop benchmark.
These wall-clock measurements do not distinguish OS preemption from DSP work,
so percentile timings must not be described as a guarantee that every hardware
callback meets its deadline. Physical ASIO
round-trip/recording tests and listening on additional real singers remain
unverified. The numerical recording/PDC tests do not establish device latency.

Local renders and measurement CSV/JSON files are under
`.cache/pitch-validation/`; they are ignored development artifacts and are not
part of the source distribution.
