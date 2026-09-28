# Sampler retrigger click — 2026-09-28

Reported symptom: bass samples click between adjacent MIDI notes. The user
confirmed that **Cut Itself** is enabled.

`SamplerInstance::noteOn` called `Voice::kill` on every previous voice before
starting the next note. This replaced the old waveform immediately, including
when the old bass was at its crest and the new sample began at zero. The
default amplitude envelope is disabled, so its Attack/Release settings could
not smooth that transition.

A generated stereo 55 Hz sine reproduces the failure. Retriggering near its
crest at a different MIDI pitch produces a maximum adjacent-sample step of
**0.799971** before the fix and **0.007257** after it. This is measured DSP
output, not a listening test of the user's original sample (not supplied).

The sampler now gives outgoing voices a forced 5 ms fade and incoming
retriggers a matching fade-in. The outgoing amplitude envelope holds its
current value during the forced cut, so even zero Release cannot bypass that
fade. Long user release settings do not prolong the cut, repeated chokes do
not restart it, and the first note retains its original attack. MIDI
`NoteChoke` uses the same short fade.

The existing loop-release ramp also had an independent bug: when its remaining
count reached zero inside a 32-frame modulation block, the following samples
in that block returned to full gain. At 48 kHz the 240-frame fade could
therefore end with 16 full-level samples. Rendering now stops contributing as
soon as the cut reaches zero.

Regression coverage in `tests/sampler_test.cpp` includes nonzero sample starts,
velocity changes, adjacent NoteOff/NoteOn events, disabled/zero/long amplitude
release, 44.1/48/96 kHz, callback-boundary equivalence, bass pitch changes,
first-note attack preservation, loop release and explicit MIDI choke.

The original code fails 11 added continuity checks. After the fix all five
selected suites pass: `sampler_test`, `plugin_midi_test`,
`render_integrity_test`, `audio_realtime_test` and `time_stretch_test`.

```powershell
ctest --test-dir build-windows -R '^(sampler_test|plugin_midi_test|render_integrity_test|audio_realtime_test|time_stretch_test)$' --output-on-failure -j 1
```

The Release `daw` target also builds successfully. Updated application:
`D:/Code/DAW/build-windows/bin/VLTONE.exe`.

Local evidence: `.tmp/sampler-click-before.txt`,
`.tmp/sampler-click-tests.txt`, `.tmp/sampler-click-test-details.txt` and
`.tmp/sampler-click-build.txt`.
