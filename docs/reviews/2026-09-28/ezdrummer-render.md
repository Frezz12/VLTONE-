# EZdrummer render state — 2026-09-28

Reported error: `cannot restore render plugin state: EZdrummer`.

The installed VST2 (`C:/Program Files/VstPlugins/Toontrack/EZdrummer.dll`, UID
`64666865`, cached vendor version `130`) declares `numPrograms == 1` but
`effGetProgram` returns `1`. The host wrote that index into its VSTL envelope
and then rejected its own snapshot because valid indices are zero based.

`VstInstance::saveState` now stores the absent-program sentinel for invalid
vendor indices. Loading older bank chunks also tolerates that optional
metadata and never calls `effSetProgram` with an invalid index. Parameter-array
validation and envelope/payload validation remain in place. The bank bytes are
unchanged.

Native probe before the fix: save succeeds (2884 bytes), clone restore fails;
changing only the envelope program field to -1 permits restoration. After the
fix: normal restore and an older envelope containing program 1 both succeed;
the clone's freshly saved state matches the source byte for byte.

Regression tests failed before the fix (`load preset` and the legacy bank
program-index check). After the fix all six selected suites pass:
`plugin_vst_test`, `plugin_render_state_test`, `render_test`,
`render_integrity_test`, `render_safety_test`, `track_freeze_test`.
VST2 export coverage reuses the existing track/group/master/clip/dual-mono
checks, including audible settings and pending parameter edits.

The `daw` target built successfully; corrected application:
`D:/Code/DAW/build-windows/bin/VLTONE.exe`. The running installed application
was not replaced or stopped.

## Remaining native-plugin observation

A separate Qt/Windows probe with a real EZdrummer instrument and two MIDI
notes writes a valid two-second stereo Float32 WAV at 48 kHz (96000 frames,
peak approximately 0.41970882), but does not return from destruction of the
render clone. A non-invasive CDB snapshot locates the main thread waiting
inside EZdrummer's `effClose`. This is separate from the now-correct state
restoration. The problem also occurs in a minimal instance-close probe.

Initializing the source editor, pumping Qt events, and running three seconds
of stopped live DSP before export did not eliminate that probe's teardown
wait. Diagnostic experiments retaining the block-size callback, clearing the
transport, closing the editor, declining idle requests, or selecting program
zero did not resolve it; none of those experiments changed production code.

An end-to-end successful return in the user's normal project/UI is not yet
verified. Do not describe the native full-render probe as passed. The user was
asked to try the corrected build with their existing project. Diagnostic
processes were terminated; diagnostic sources and rendered WAVs are under
`.tmp/ezdrummer-*`.
