# VLT Studio Pro

VLT Studio Pro is a cross-platform (Windows + macOS) digital audio workstation.

- **Core** — a portable C++23 audio engine: realtime-safe mixing graph,
  transport, tempo, tracks/clips, recording and offline render. No OS-specific
  code; device I/O runs on **PortAudio** and file decoding on **libsndfile**.
- **Controller** — a framework-agnostic C++ layer (`EngineController`) that owns
  the engine and the document model, with undo/redo and a JSON project format.
- **App** — a **Qt 6 Widgets** front-end: arrangement view (track list +
  timeline with draggable clips), mixer, inspector, transport, and audio-device
  settings.
- **Account platform** — a single Go/PostgreSQL API, a public Next.js account
  site, a separate Next.js admin console, and a network-isolated diagnostics
  reporter process for the DAW.

This is a cross-platform rewrite of a former macOS-only Swift/SwiftUI app; the
portable C++ engine was reused, its Apple-specific I/O replaced, and the UI
rebuilt in Qt. The retired original lives under `DAW/` for reference.

## Build

See **[BUILD.md](BUILD.md)** for macOS and Windows instructions. Quick start on
macOS:

```bash
brew install cmake ninja qt qtwebengine qtserialport portaudio rtmidi libsndfile nlohmann-json
cmake --preset macos
cmake --build build
./build/bin/daw
```

## Status

Working end-to-end: create tracks, import audio and MIDI files, arrange clips
(drag/snap/delete), play with a moving playhead, mix (volume/pan/mute/solo/
meters), edit track properties in the inspector, write parts in the piano roll,
play notes from the computer keyboard or a hot-plugged MIDI keyboard, host VST3,
legacy VST1/VST2, CLAP and
AU plugins and the
built-in sampler, browse and audition sample folders, choose the audio device,
export a mixdown, and save/open portable `.vlt` projects — with undo/redo and
themes.

The integrated Web browser (right edge, `Alt+W`) uses a persistent, single-tab
Qt WebEngine profile. Audio downloaded there can be imported at the playhead on
a new or selected Audio Track. It can stay open beside the independent AI panel.

The AI assistant panel (right edge, off by default, `Alt+A`) works the program
from a description: it creates tracks, loads instruments, writes MIDI parts and
sets levels and effects through the same controller API the UI uses. Before
each managed request the account server checks that the model still exists and
reserves the user's token quota, then issues the current provider URL and key
for a direct request from the desktop. That credential is kept only in memory,
but, like any credential delivered to a client device, it can be extracted by
the device owner. Prompts and answers do not pass through the VLT backend. One
assistant request is one undo entry.

Work survives a crash: the project is journalled to a recovery file every couple
of seconds, and the next launch offers it back. See **Crash recovery** below.

## Headless checks

The app builds and drives itself without a screen, which is how the UI is
verified:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/daw --selftest        # builds every
                                                            # window, plays a
                                                            # typing-keyboard
                                                            # note, drops a MIDI
                                                            # file, auditions a
                                                            # sample, and runs a
                                                            # whole assistant
                                                            # turn against a
                                                            # scripted model (no
                                                            # key, no network);
                                                            # non-zero exit on
                                                            # failure
QT_QPA_PLATFORM=offscreen ./build/bin/daw --screenshot shot.png
```

`--screenshot` grabs the shell; environment variables aim it at a particular
state — `DAW_SHOT_PIANOROLL`, `DAW_SHOT_SAMPLER=<wav>`, `DAW_SHOT_EQUALIZER=1`, `DAW_SHOT_PLUGINS=<tab>`,
`DAW_SHOT_SLOTS`, `DAW_SHOT_RECORD`, `DAW_SHOT_PLUGIN_EDITOR=<name>`, and for
the file browser `DAW_SHOT_BROWSER=<folder>` with the optional
`DAW_SHOT_BROWSER_FILE=<file>` (so the preview strip shows a real waveform) and
`DAW_SHOT_BROWSER_RIGHT=1` (the panel on the other side of the window). For the
assistant, `DAW_SHOT_AI=1` opens it with a finished transcript in it, and
`DAW_SHOT_AI=empty` shows the state a user meets before a key is configured.
`DAW_SHOT_RECOVERY=1` grabs the prompt shown after a crash.

## Channel Strip templates (`.vlts`)

The gear beside a channel's **Audio FX** heading opens **Channel Strip
Presets**. Every saved `.vlts` file appears there, with **Save New Template…**
kept at the bottom of the submenu. A template contains the insert chain (with
each plugin's opaque state), volume, and pan. Sends, routing, mute/solo, mono,
and the instrument slot are deliberately left on the destination channel.

Templates live in the application's local data directory under
`Presets/Channel Strips`. The browser always exposes that managed directory as
the top-level **Presets** folder; double-clicking a `.vlts` file applies it to
the selected mixer channel. A template captured from a normal track can also be
applied to a bus or the master channel, and vice versa.

Crash recovery has two modes of its own, and they are meant to be run as a pair:

```bash
export DAW_RECOVERY_ROOT=/tmp/daw-recovery      # never the real one
QT_QPA_PLATFORM=offscreen ./build/bin/daw --crashtest     # exits 139: SIGSEGV
QT_QPA_PLATFORM=offscreen ./build/bin/daw --recovercheck  # prints the tracks
```

`--crashtest` builds a project, waits for the journal to write, then faults for
real, so nothing about the path is simulated: no destructor runs and the session
directory is left exactly as a crash leaves it. `--recovercheck` then recovers
it through the same code the dialog uses and prints what came back — seven
tracks, against the one a bare startup would have written.

## The AI assistant

The model never touches the document directly. It is given a couple of dozen
tools — `add_track`, `add_midi_clip`, `set_clip_notes`, `add_insert`,
`set_insert_parameter` and so on — each of which is a thin, validated wrapper
over an `EngineController` method, so anything the assistant can do is something
the UI could already do, and everything it does is undoable.

The split matters when changing it:

| Where | What |
|---|---|
| `controller/ai/AiTools.{hpp,cpp}` | The tool registry, the JSON dispatch, the project snapshot, **and the system prompt** — one raw string literal near the bottom of the `.cpp`. Edit that literal to change how the assistant behaves; it is the feature's real behaviour, not the C++ around it. No Qt, no network. |
| `controller/ai/AiSession.{hpp,cpp}` | The agent loop as pure state: the conversation, running the tool calls, the step limit, folding the whole run into one undo entry, and the checkpoints behind "revert this request". |
| `controller/ai/AiWire.{hpp,cpp}` | Both providers' request and response shapes, and the SSE decoder — all pure JSON, so streaming can be tested without a network. |
| `app/LlmClient.{hpp,cpp}` | Per-request managed-model lease, direct provider streaming, quota settlement, and direct locally configured custom providers. Provider Base URLs are resolved to the matching chat route here. |
| `app/AiChatPanel.{hpp,cpp}` | The panel, and the scripted stand-in client the headless check drives. |

A failed tool call is answered to the model as a sentence rather than raised as
an error — that self-correction is most of what makes it work, and it is what
`tests/ai_tools_test.cpp` spends most of its checks on. The same file covers the
two things real models do that the documentation does not mention: sending a
nested array as a *string* of JSON, and writing a tool call into their prose
instead of using the tool channel. Both are accepted.

Because it cannot hear the project, `analyze_track` renders a channel on its own
and reports level, headroom, clipping and a three-band balance, and
`analyze_sample` does the same for a file on disk. Mixing without those is
guesswork, and the prompt says so.

Settings ▸ AI also holds the step limit, how many past requests each new one
carries, and whether the answer streams. Standing instructions for a project
("always sidechain the bass") live in the project itself, behind the panel's
notes button.

At first launch the desktop account gate appears before `MainWindow`, audio,
recovery or project loading. Registration and password reset open the public
site. Refresh credentials and the last verified server time live in Windows
Credential Manager or macOS Keychain; a signed entitlement permits at most 72
hours offline and blocks access after a detected clock rollback. The one-time
settings migration deletes old local provider keys and custom base URLs without
sending them anywhere.

## The context panel

One floating plate under the tool strip showing the tools for whatever is
selected. By default it **rides along the strip to sit above the selected
clip**, or above the middle of a group of them; a track or the recording
options have no horizontal extent, so those return it to the centre. Settings ▸
Context Panel ▸ Position turns the following off and pins it centred.

Two things about how it is wired are worth keeping:

* **The anchor is asked for, not pushed.** `ContextPanel::setAnchorProvider`
  takes a callback the plate calls from inside `targetGeometry()`. Pushing the
  value was tried first and is always one step late: a selection change makes
  the plate recompute its geometry immediately, before the shell has had a
  chance to hand it the new position.
* **Position and content are two animations, deliberately.** The drift
  (`kDriftMs`, no overshoot — it is tracking something the user is looking at)
  and the content swap both drive `geometry`, so the swap stops the drift and
  wins. `m_contentSliding` holds `layoutSelf()` off during the arrival —
  otherwise every frame of the spring re-centres the row and overwrites the
  animation.
* **The swap is a hand-off, not a cross-fade.** The old row leaves the plate
  entirely (a child is clipped to its parent, so past the edge it is simply
  gone — no opacity effect, which was tried and rejected as expensive and unsafe
  during a window flush), the plate is briefly empty, and the new row comes in
  from the right after `kSlideInDelayMs`. Both curves are `OutCubic` for the
  same reason: an ease-*in* on the exit spends its first half barely moving,
  which left the old buttons sitting in full view while the new ones arrived on
  top of them. Two rows on one plate was the bug; the empty beat between them is
  what fixes it.

Both can be photographed frame by frame — `DAW_SHOT_CONTEXT=<clips>` selects,
`DAW_SHOT_CONTEXT_THEN=<clips>` switches after 600 ms, and `DAW_SHOT_DELAY=<ms>`
moves the grab into the middle of the movement.

## Crash recovery

A watchdog process cannot save a crashed program's project — the document lives
in the dead process's memory and is gone the moment it dies. So the work is
preserved by the program itself, ahead of time, and the separate process does
only what needs to be outside.

| Layer | Where | What it actually does |
|---|---|---|
| Journal | `controller/recovery/RecoveryJournal` | The only thing that preserves data. Writes the document to `<app data>/recovery/<pid>-<start>/project.json` on a worker thread, coalescing bursts into one write every couple of seconds. |
| Crash marker | `controller/crash/CrashHandler` | Says *why*. On a fatal signal it `write(2)`s a pre-opened descriptor — signal, backtrace, and the plugin that was on the stack — then re-raises so the OS still gets its crash report. |
| Watchdog | `guard/daw_guard` | Notices freezes, which nothing inside a hung program can. Records the verdict and a health log. Links nlohmann/json and nothing else. |
| Reporter | `reporter/daw_reporter` | Reads the bounded diagnostics outbox, receives a 72-hour telemetry-only token over local IPC, prioritises crash delivery, and retries after the DAW exits. `daw_guard` remains network-free. |

The journal is affordable because it is **not** `ProjectSerializer::save`, which
copies every referenced audio file into the package. It uses `saveDocument` with
`MediaPaths::Absolute`: the document only, referencing the user's own files
where they already are. `load` needs no special case for this — an absolute path
on the right of `fs::path::operator/` replaces the left, so one reader serves
both modes.

**A session directory that still exists is the entire crash signal.** A clean
shutdown deletes it, on `aboutToQuit` rather than `closeEvent` so that Cmd+Q
counts too. No flag to get wrong, and it works even if the watchdog never ran
and no handler ever fired.

What comes back is deliberately narrower than "everything", and the prompt says
so: notes, clips and mix as of the crash; plugin settings as of the last manual
save (capturing state chunks continuously would mean asking every plugin to
serialise itself several times a minute); audio from the original files. The
user's project file is never overwritten automatically — recovered work arrives
unsaved.

The watchdog learns of the program's death without polling: it inherits the read
end of a pipe the DAW holds open forever, so any death at all closes it. One
platform detail worth keeping: macOS does not report `POLLHUP` unless some event
was actually requested, so the guard polls for `POLLIN` — with `events = 0` the
hangup goes unnoticed and the watchdog never learns its parent died.

Settings ▸ Recovery turns either layer off; with the watchdog off the journal
still preserves the work, which is the property that matters.

## Project format

`.vlt` is a portable project package. It contains a clickable `Project.vlt`
manifest, `Content/` with copies of every referenced recording/sample, and
`State/` with plugin state chunks. On macOS the package is presented as one
document with the application logo; on other systems the inner `Project.vlt`
can be opened directly. Either entry point restores the same complete project,
and legacy `.dawp` packages remain readable.

# VLTONE-
