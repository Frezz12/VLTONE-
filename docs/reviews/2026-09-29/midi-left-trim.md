# MIDI clip head trimming

The MIDI and Pattern hit-test branch explicitly disabled the left edge. Removing that restriction alone would have moved the phrase: MIDI playback and timeline previews previously ignored `offsetSeconds`. In addition, the trim gesture queried an audio-only stretch parameter for MIDI, receiving zero and then using a 0.01 stretch ratio.

Both edges now use the existing trim gesture. MIDI uses a 1:1 source-time ratio, while audio continues to use its stretch factor. The source notes, slides and controller lanes remain unchanged while dragging; start, source offset and duration are the undo endpoint. Recovering the head by extending it to the left restores the hidden notes.

`midiPlaybackClips` materializes the audible MIDI source window on publication, including held notes, controller state and comp segments. Splitting, recording into a trimmed clip, tempo changes and comp normalization account for the source window. Timeline note/take previews retain their absolute placement. Piano Roll retains the full source phrase, dims the hidden head, and maps ruler seeking and the playhead to the source origin. AI music context excludes notes outside the audible range.

Regression checks exercise pointer press/move/release through both QWidget and the exposed GPU workspace, for Audio, MIDI, Pattern and Automation clips. They check both edges, mid-drag reversal, recovery in a later gesture, one undo endpoint, redo, immutable source storage, note playback positions and preview placement. Separate checks cover controller chasing, comp timing, tempo changes, MIDI export and Piano Roll seeking.

Validation logs and repeatable runners are in `artifacts/midi-trim/`. Release builds succeeded. Final `controller_test`, `midi_recording_test` and `project_music_context_test` passed. CPU GUI checks passed, including Piano Roll source bounds, seeking and MIDI export. The packaged stage passed native GPU checks at 100% and 150% scale with a clean Windows PATH; all four clip kinds passed both edge gestures, reversal and history, with no unsupported-painter or workspace fallback errors.

The broader `slide_test` has a separate failure in its clip/take slide serialization fixture; the trim changes do not enter that path for its zero-offset fixture. This failure is retained in `core-tests.log`, rather than reported as a passing full test suite.

Updated stage: `build-windows/stage/bin/VLTONE.exe`. SHA-256: `8EC1C08D55A1D757094519BE048B37EEAB041DB1A72EF0830C5B6763E6F825CF`, matching the frozen verified executable in `artifacts/midi-trim/VLTONE-verified.exe`.
