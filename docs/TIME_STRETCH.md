# Audio clip stretching

`engine/DSP/TimeStretch.cpp` adapts the vendored MIT Signalsmith Stretch library
for both arrangement clips and Sampler voices. The original pair of unaligned
Hann-windowed grains has been removed from both playback paths.

| Mode | Analysis window at 48 kHz | Overlap | Intended material |
| --- | --- | --- | --- |
| Stretch (legacy numeric ID 1, `Drums`) | 96 ms | 8 | General audio and rhythmic recordings |
| Loop | 80 ms | 8 | Repeated phrases |
| Vocal | 100 ms | 6 | Voice; compensates formants during pitch shifts |
| Complex | 120 ms | 6 | Polyphonic recordings and complete mixes |

Window/hop lengths scale with sample rate. Stereo channels are analysed
together. Ratios beyond 2x are decomposed into multiple stages of at most 2x,
avoiding extreme-ratio phase randomisation and amplitude cancellation. Pitch
and formant changes happen only in the final stage. Spectral computation is
spread across output samples instead of occurring in a single large burst.

Processing uses fixed 128-frame internal blocks and fractional input accounting,
so host buffer sizes do not change the result. Preparation allocates FFTs and
scratch buffers on the control thread. A decoded-source reader supplies
look-ahead and zero padding; clips and notes do not acquire graph latency.
An unchanged source at unity speed/pitch passes through sample-exactly.

Sampler mode automation prepares all required profile banks before playback
or export. Ordinary manual mode selection retains only one profile bank.

## Tempo following

Every non-Resample audio mode retains its start and duration in beats. On a
tempo change from A to B, output duration, fades, and `stretchTime` are multiplied
by A/B. Source offsets and source spans remain unchanged. Comp boundaries and
take placement offsets follow the tempo; take source lengths do not.

The same model helper serves local edits and the collaboration reducer. Project
loading allows automatic ratios outside the manual Time knob's 0.25–4 range.
Undo, redo, and save/reopen preserve the ratio and clip geometry together.

The accompanying backend changes must be deployed with cloud clients:
`clip.setSampleEdit` now advances the existing `project:tempoCascade` conflict
guard, and validation accepts the expanded automatic ratio range.

## Verification

`time_stretch_test` checks unity transparency, pitch, sustained level, stereo
phase/balance, polyphonic partials, transient placement, source looping,
seeking, host block independence, moving Time, exact clip bounds, and absence
of audio-thread allocation. Controller and sampler tests cover tempo following,
comp playback, project persistence, undo/redo, and mode automation.

These synthetic regression checks do not promise inaudible processing on every
recording. Extreme ratios and simultaneous large pitch shifts remain more
demanding than moderate tempo adjustments.

See `third_party/signalsmith/README.md` for pinned upstream commits and licenses.
