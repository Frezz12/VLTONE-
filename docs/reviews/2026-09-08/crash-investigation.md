# Two crash reports — 2026-09-08

## 11:26:14 UTC — CoreMIDI / RtMidi (confirmed)

Report `a29beefd`: `SIGABRT`, through `__cxa_call_unexpected` and
`MidiInCore::getCoreMidiClientSingleton`, from the MIDI scan timer. The session
had one empty track and no plugins. Audio was using Volt 1, 48 kHz, 8 frames.

RtMidi 6.0.0 declares its CoreMIDI client helper `throw()` but throws
`RtMidiError` when `MIDIClientCreate` fails. This terminates the process before
`MidiInputManager` can catch the exception. The existing asynchronous readiness
probe cannot guarantee that this later, separate client creation succeeds.

The macOS build now statically links a vendored RtMidi 6.0.0 with the incorrect
exception specifications removed. A temporary CFString is also released before
the error is thrown, to avoid leaking it on retries. Windows keeps its package.

`rtmidi_failure_test` substitutes only `MIDIClientCreate`, injects repeated input
and output failures, then permits success. It requires no MIDI device/server.
The same test compiled with the unmodified upstream source exits with SIGABRT;
the patched source catches all six failures and recovers successfully.

## 11:25:50 UTC — Cocoa object destruction (exact cause unconfirmed)

Report `72e2e1c2`: `SIGSEGV` in `__RELEASE_OBJECTS_IN_THE_ARRAY__`,
`-[__NSArrayM dealloc]`, and the main AppKit autorelease-pool drain. No plugin
frame appears in the fault stack. The raw marker contains only
`last_plugin=ValhallaUberMod`, not an active `plugin=` call.

The reporter incorrectly used this historical name to produce “crashed in
ValhallaUberMod”. It now keeps the name as context and attributes a signal to
a plugin only when the marker identifies an active call. The integration test
`crash_reporter_test` covers historical marker, historical session fallback,
and active-call cases through the actual reporter process. It supplies no
token and cannot upload anything.

The AU host also had a reproducible lifetime defect relevant to this stack:
temporary Cocoa collections created while opening an editor could retain its
view in the caller's pool. Closing the editor used a *new* pool, which cannot
drain older references. Disposing the AU in the same GUI turn therefore left
the view to deallocate later, after its unit had gone away.

`AuInstance::openEditor` now drains setup temporaries in its own pool while both
the view and its AU are alive. `au_editor_lifetime_test` uses a small native
fixture whose temporary collection retains the view. Before the fix all three
views deallocated after AU disposal; after it, explicit close and implicit
destruction finish before unit disposal without relying on the outer pool.

An additional assertion exposed a second lifetime violation in `closeEditor`:
ARC released its unused factory local before the view was destroyed, despite
the source's apparent assignment order. The factory now has
`objc_precise_lifetime` and outlives a nested pool that detaches and drains the
view. The fixture also checks factory/view destruction order; its three
failures disappear with this change.

This confirms a host defect, **not that it caused this particular Valhalla
crash**. The report does not identify the plugin format or the operation before
the failure. State restoration, editor teardown, other loaded plugins, and UI
objects remain possible sources of the corrupted collection. Initial isolated
ValhallaUberMod AU and VST3 state/processing/editor probes did not reproduce the
reported crash. The manual `plugin_lifetime_probe` supports both teardown before
and after its creation pool drains, at 48 kHz / 8 frames.

An exact reproduction still needs the affected project and the action before
11:25:50 UTC. The last telemetry shows 14 tracks, one clip, stopped transport,
DSP load 106.7%, and process CPU 424.6%; it does not show an active recording.
Those readings indicate little realtime headroom, but neither crash stack
establishes DSP overload as the crash cause.

## Final validation

- `rtmidi_failure_test`, `au_editor_lifetime_test`, `plugin_host_test`,
  `plugin_vst3_test`, `plugin_au_test`, and `crash_reporter_test`: passed.
- Installed ValhallaUberMod AU, VST3 and VST: 12 complete cycles per format
  passed with `NSZombieEnabled=YES`. Each cycle restores state, reads parameter
  text, opens/closes the editor and processes 8-frame blocks at 48 kHz. The
  probe services the native run loop; alternate cycles destroy the instance
  before the outer creation pool drains. No zombie diagnostics were emitted.
- `build/bin/VLTONE` and `daw_reporter` built successfully. `otool -L` confirms
  that VLTONE no longer loads `librtmidi.7.dylib`; its corrected CoreMIDI helpers
  are defined in the application binary.
- `git diff --check`: passed.

These checks cover the fixes and isolated plugin lifecycles. They do not
reproduce the original 14-track project or establish its crackle-free realtime
performance with an 8-frame device buffer.
