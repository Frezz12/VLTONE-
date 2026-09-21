# MIDI recording and retrospective capture

## User flow

Arm Record, select a MIDI/instrument track (or arm several tracks), then start
transport. Hardware MIDI, the typing keyboard and the Piano Roll's screen keys
use the same capture path. MIDI-only recording needs no audio input and writes
no WAV. Count-in and mixed audio/MIDI recording use the existing transport.
Targets and recording preferences are frozen for the take. Capture is not
quantized. A growing preview is separate from the document; stopping lands one
undoable edit.

Overwrite replaces the recorded range, Layers retains takes and comp regions,
and MIDI overdub merges notes. Overdub replaces automation only for controller
or parameter targets touched in the recorded interval. Held notes split at loop
boundaries, with chased controller state; an empty final pass does not erase the
previous pass. MIDI takes can be selected, edited, split and copied.

The Piano Roll lane selector offers instrument parameters and channel MIDI
controllers. Its parameter menu and the host instrument controls expose
«Назначить MIDI-ручку», cancel learning and remove mapping. The next absolute
CC maps to the parameter's range and step behavior. Mappings are local QSettings
data scoped to the user, project, track and instrument UID. A mapped gesture
records the instrument parameter lane and does not also send its raw CC.
Manual instrument gestures during recording create lanes automatically and
suppress previous playback automation for that parameter until the next loop
pass. Automation Write retains its behavior outside MIDI recording.

Right-click Record and choose «Восстановить сыгранное». The same action is
registered as `transport.restoreMidi`, without a default shortcut. It places
new clips on the original tracks from the current cursor, preserving pauses and
relative timing. The buffer covers at most the last ten minutes since consumption
and also works with transport stopped. Leading/trailing silence is removed.
Recording/count-in and an empty buffer disable the action. Missing source tracks
leave the buffer intact and report an error. A successful local take/recovery
consumes only its timestamp interval; a shared take waits for durable server
confirmation. Newer playing remains available. Changing projects clears input
history.

With the typing keyboard enabled, bare letters and digits are reserved even
when they do not play notes (including A). Space, Escape, function keys,
modifier combinations, text entry and open menus remain available. User
shortcut assignments are retained and restored when musical input is disabled.

## Capture and playback

`MidiInputManager` stamps hardware messages before Qt delivery. The transport
publishes a bounded atomic musical clock, accumulated across loop wraps and
tempo changes. `EngineController::liveMidiInput` identifies source and origin;
clip playback and editing audition do not enter capture. The hardware dispatch
queue is bounded; overflow releases held input rather than leaving stuck notes.
Focus/device cleanup closes held notes and resets pedal/controller routes.

`MidiRecording` builds notes and typed controller lanes without publishing the
audio graph per event. Notes preserve MIDI channel, attack/release velocity and
event order. Controller lanes represent CC, 14-bit pitch bend, channel/poly
pressure and program change, or an instrument parameter. `TakeModel` contains
its own lanes. The shared MIDI player materializes comp regions and applies
clip and Pattern boundaries for both playback and offline audio rendering.

Local project format is version 9, with compatible defaults for old projects.
The shared command schema and protocol are version 4; versions 2 and 3 remain
readable, but cannot interpret the new MIDI-content commands.

## Shared recording and recovery

`recording.prepareMidi` stages typed, immutable parts identified by persistent
recording/content IDs. Parts contain notes, lanes, take metadata and comp data;
they stay hidden from the timeline and below the existing 1 MiB message limit.
The reducer and Go validator check MIDI ranges and assembled identities.

The final `recording.commit` applies the prepared content together with clip
geometry, ownership and audio children in one atomic transaction. Repeating an
operation ID is idempotent. One user Undo restores the previous material, guarded
by descendant revision preconditions so later remote edits are not overwritten.
New clips use the existing path without track reservations. Existing material
uses the project's permission, reservation and conflict checks.

`CloudMidiRecordingCoordinator` persists pending parts, progress, final commit
and uncertain-send state with QSaveFile under the local recovery root's
`MidiPending` directory. Reconnect/restart resumes pending work. An uncertain
operation must be resolved by authoritative operation lookup before it is
changed or retried. MIDI content is retained until the final durable ACK.
Monotonic input timestamps are deliberately not restored across application
restarts, so an old ACK cannot clear new input from a different process.

### Release order

1. Deploy the server with migration `000025_midi_recording_v4` and schema-4
   validation/session support.
2. Verify schema-2/3 compatibility and schema-4 staging/commit on the deployed
   environment.
3. Release the client that negotiates protocol 4 and uses MIDI recording.

A project containing schema-4 operations refuses an older session version.
This repository change does not deploy the server or run a production migration.

## Verification

Build `daw`, `midi_recording_test`, `midi_recording_protocol_test`,
`controller_test`, `collaboration_protocol_test`, `recording_preview_test`,
`recording_commit_planner_test`, `audio_realtime_test`, and `plugin_midi_test`.
Run the corresponding CTest targets and `midi_input_ui_test` (offscreen Qt).
Run `go test ./internal/collab ./internal/api ./migrations` in `backend`.

The new deterministic tests cover source/channel overlap, retriggers, release
velocity, all channel MIDI controller types, pedal cleanup, loop slicing,
controller-only/empty capture, frozen targets, count-in, audition exclusion,
no audio-input/WAV requirement, no graph rebuild per input event, modes,
MIDI Learn, manual parameter recording, tempo retiming, persistence and Undo.
Retrospective tests cover consecutive recoveries, ten-minute held notes,
trailing unmatched releases, delayed Qt delivery and preserving new input after an older ACK.

Protocol tests exercise more than 1 MiB of content, hidden preparation,
snapshot reconstruction, a shared C++/Go wire fixture, incomplete/duplicate commit, reducer convergence and
conflicting Undo. The UI selftest also reloads a pending MIDI transfer from disk,
retains its acknowledged-part progress and resolves it once. These are local
protocol/recovery tests, not a live two-client network deployment test.

Before release, also exercise physical MIDI devices and native third-party
plugin windows on supported operating systems, and the complete two-client
server/database flow with network interruption between parts, restart, mixed
audio/MIDI upload and conflicting edits. The development environment has no
attached MIDI keyboard or deployed collaboration test session.

## Scope

Channel MIDI 1.0 only. SysEx, MIDI 2.0 and relative-encoder learning are excluded.
Typing velocity remains fixed. No new runtime dependency is required.
