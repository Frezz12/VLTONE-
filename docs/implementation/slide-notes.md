# Slide notes

Slide notes are silent control objects in MIDI clips and takes. They never enter
ordinary note generators or MIDI note export. Project format **11** and command
protocol **5** preserve them separately from sounding notes.

## Editing

Choose **Slide** in the piano-roll toolbar or press **6**, then drag to set the
transition length. **One note / Chord** chooses a single voice or a saved set of
voices. Selected notes sounding at the start are preferred; otherwise the closest
pitch wins, then the later start. The preview outlines the linked notes.

Double-click a slide to open its nonmodal curve editor. Drag/add points, use the
pencil for several rises and falls, or choose Linear, Smooth, Wave or Return.
Alt-drag bends a segment; right-click deletes an interior point. Time and pitch
snap, numeric semitones/cents, shape copy/paste, rebind and phrase audition are
available in the editor. Shape paste adapts normalized time to the destination
length. The first height follows the sounding voice. Escape cancels the gesture;
one completed gesture creates one undo entry.

The grid shows the resulting pitch path, including the held result after a
slide. Later overlapping slides replace earlier movement continuously. Chord
slides add the same interval to the saved targets. Inactive slides show a dashed
outline and warning; their inactive tail is hatched. Base notes are extended only
by **Extend base note**, as an explicit undoable action. Deleting a base keeps
its slide available for undo or rebinding. Copy, transpose, time stretch, note and
clip cuts carry/remap linked data; cut fragments store their reached initial pitch.

The pencil simplifies against the full original stroke with less than one cent
of vertical error. Published curves contain at most 256 points. Drawings that
cannot meet both limits keep the last accepted shape and show a point-limit hint.

## Sampler and playback

The instrument sampler has a **Slide / Legato** page. Legato defaults off, its
transition defaults to 100 ms and an S curve, with optional beat divisions.
Switch smoothing defaults to 2 ms (0–20 ms); input MIDI Bend Range defaults to
±2 semitones. New parameters follow every existing SMP and INS index.

A slide changes the existing voice's pitch without restarting its source,
envelopes, filter or stretch processor. Resample evaluates pitch per sample;
Stretch retains its processor state. Release holds the reached pitch. A one-shot
still ends at the end of its sample; indefinite sustain requires Loop. Legato
returns to the previous held key, and an explicit curve takes priority over its
automatic transition. Legato duration never scales a drawn slide.

`SlideNotes::compile` resolves links and preceding/overlapping slides on the
control thread. A note and its compiled pitch segments travel in one immutable
`MidiClipPlayerNode` schedule. Stable note IDs survive routing, edits and latency
compensation. Schedule reconciliation preserves unchanged active voices. Pitch
ramps retain original segment phase across block boundaries and tempo changes;
live-edit smoothing is separate from the uninterrupted drawn curve.

Realtime queues are reserved during preparation for maximum buffer size and
128-voice pitch traffic, including segment boundaries and external 1 kHz events.
Delay queues also account for their compensation horizon. The pitch scheduling,
voice adapter and sampler do not allocate or lock in their audio callbacks.
Offline rendering and freeze use this same playback path. Editing slides thaws
an existing freeze. MIDI-file export warns that it exports ordinary notes only;
it never turns slide controls into Note On events.

## Instrument delivery

The instrument editor's **Slide** control exposes Auto, Per note, MPE, Pitch Bend
and Off, the bend range, unknown-release reserve and resolved capability status.
The built-in sampler uses independent per-voice ramps. For plugins, Auto selects
only advertised support: native per-note expression, then declared MPE, then
MIDI Pitch Bend. Manual MPE is available for MIDI-capable instruments. Unsupported
modes are displayed rather than inferred from the plugin format.

CLAP uses advertised [note-port dialects](https://raw.githubusercontent.com/free-audio/clap/main/include/clap/ext/note-ports.h)
and note-expression tuning events. VST3 queries the tuning expression and uses
[NoteExpressionValueEvent](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/structSteinberg_1_1Vst_1_1NoteExpressionValueEvent.html)
with the voice's note ID. MIDI compatibility uses channel pitch bend;
[MPE](https://midi.org/mpe-midi-polyphonic-expression) assigns channel 1 as common
and channels 2–16 to voices, with a default ±48-semitone member range. The DAW
range must match the instrument's range.

MPE keeps a member channel through release until Note End or a declared tail;
unknown tails use the configurable two-second reserve. When all members are
occupied the new voice is skipped and overload is shown. Pitch Bend explicitly
shows **Affects the entire channel**; the later slide wins a channel conflict.
Channel automation and slides are composed once in semitones. Release bends are
retained for independent MPE voices. Out-of-range regions are marked in the curve
editor, and the instrument control reports clipping.

## Collaboration and release order

`slide.set` upserts one complete slide or deletes it with a null value. It names
track, clip and optional take IDs. C++ and Go validate UUIDs, finite MIDI heights,
ordered normalized points, supported shapes and bounds. Undo is conditional on
field writers. Clip/take recording-content parts include separate slide arrays.
Older projects load with empty arrays. Sessions negotiating protocols 2–4 cannot
edit slides; the toolbar/editor explains the required update.

1. Deploy the backend and migration **000027_slide_notes_v5**, which permits
   sessions using command schema 5 while retaining 2–4.
2. Verify older-session compatibility and v5 create/edit/delete/reconnect on the
   deployed server. A project with v5 operations must reject older session schemas.
3. Release the protocol-5 client. Do not release it ahead of the server migration.

The repository change prepares both sides; it does **not** deploy a production
server or apply a production database migration. Downgrading the database is
blocked while v5 sessions remain; data is never silently rewritten as v4.

The new data/operation schemas are in `protocol/schema/slide-note.schema.json`
and `protocol/schema/slide-command-v5.schema.json`; cross-field rules are enforced
by the C++ and Go validators.

## Verification

Build/run `slide_test`, `sampler_test`, `controller_test`, `plugin_midi_test`,
`plugin_vst3_test`, `collaboration_protocol_test`, `midi_recording_test`,
`midi_recording_protocol_test`, `miditools_test`, `midifile_test` and
`track_freeze_test`. Run `go test ./internal/collab ./internal/api ./migrations`
from `backend`.

`VLTONE --slidecheck --theme dark --language en` runs deterministic Qt input
checks. Set `DAW_SLIDE_CHECK_DIR` to save piano-roll/editor images. Also check
Russian, light theme and `QT_SCALE_FACTOR=1.5` / `2`. `--samplercheck` exercises
the sampler interface. These checks use the offscreen Qt platform, synthetic
samples and the CLAP/VST3 fixture plugins; they do not certify every third-party
instrument or replace a live two-client deployment and listening test.

Verified locally on Windows/MSVC: all 11 listed CTest targets passed, as did the
three Go packages. The piano-roll input checks passed at 100%, 150% and 200%; the
sampler layout checks passed in English and Russian across all built-in themes.
The isolated executable is `build-slide/bin/VLTONE.exe`, with Qt/runtime DLLs
beside it. A clean-PATH launch passed the slide and sampler checks. The runtime
dependency scanner passed before the offscreen test-platform plugin was added;
the final offscreen launch also verified that plugin can load.
