# RtMidi 6.0.0 — macOS build

Source: https://github.com/thestk/rtmidi/tree/6.0.0 (`RtMidi.cpp`, `RtMidi.h`, `LICENSE`).
License: MIT-style RtMidi license, reproduced in `LICENSE` and source headers.

Local changes in `RtMidi.cpp`:

- Remove `throw()` from the declarations and definitions of the input/output
  `getCoreMidiClientSingleton` helpers. They report `MIDIClientCreate` failures
  by throwing `RtMidiError`; `throw()` otherwise forces `std::terminate` before
  the application's handler can catch the error.
- Release the temporary client-name CFString before reporting an error, so
  retrying does not leak the string.

`daw_rtmidi` builds only the CoreMIDI backend and is linked statically by the
macOS application. Windows continues to use its configured RtMidi package.
When updating, run `rtmidi_failure_test`: it substitutes MIDIClientCreate and
verifies failure/retry/success without requiring a MIDI device or system server.
The application's asynchronous initial readiness probe remains in place to
avoid blocking startup while MIDIServer is unavailable.
