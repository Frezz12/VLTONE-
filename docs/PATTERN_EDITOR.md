# Pattern editor

The Pattern editor uses the existing Qt Widgets/QPainter controls and the project’s Inter typography. Direct edits stay immediate, with no animation between pointer input and parameter values.

| Before | After | Why |
| --- | --- | --- |
| Long volume fader between the sound name and MIDI | Equal-size circular volume and pan controls at the far left | Keeps mixing controls together and frees horizontal space for notes |
| One-line sound plate with a decorative sheen | Sound name, smaller instrument label and track-color marker | Makes source identity and instrument type readable without opening an editor |
| Each MIDI preview scaled independently | Shared Pattern time range with restrained rhythmic divisions | Notes in different sounds can be compared at the same timeline positions |
| MIDI editing required opening the piano roll | “Fill rhythm” beside every preview and in the sound menu | Creates regular triggers in one action, using clear fractions of a bar |
| Wheel movement changed mix parameters | Wheel scrolls the list; drag vertically, use arrow keys, or enter a value from the control’s context menu | Prevents accidental edits while navigating rows |
| Ghost tracks needed manual selection | Other MIDI/instrument sources in the same Pattern are visible automatically | Keeps harmonic and rhythmic context available while switching sounds |

## Sound actions

Right-click the sound plate for its instrument, piano roll, rhythm fill, rename, sample replacement, duplication and removal. Built-in Samplers also expose a checked **Cut Itself** action bound to the same `cutitself` parameter as the Sampler editor, including undo.

**Fill rhythm** offers every bar, 1/2, 1/4, 1/8, 1/16 and 1/32 of a bar. The project meter defines a bar; for example, 1/8 of a bar in 6/8 is 0.375 quarter-note beats. Fill replaces notes across the source clip’s duration. Samplers use their root note; other instruments use their first existing note’s pitch, or MIDI 60 for an empty source. Notes have velocity 100 and short, nonoverlapping gates.

Like the source’s piano-roll opener, MIDI replacement and fill target its first MIDI clip. If none exists, they create an owned MIDI clip at the first Pattern instance. This editor currently represents a Pattern track rather than an individually selected arrangement instance.

## File drops

- Audio retains the existing behavior: drop onto a source to replace its sample, or between rows to add sounds.
- One local `.mid` or `.midi` file dropped onto a source replaces that source’s notes. All note tracks in a format-1 file merge into that source; pitch, timing, length and velocity are retained. No extra instruments are created and project tempo is preserved.
- A longer MIDI phrase extends the MIDI clip and its Pattern owner to whole bars. A shorter phrase retains the existing clip duration. Notes, offset and both boundaries are restored by one undo operation.
- MIDI has no insertion target between rows. Invalid and note-free files leave the document intact. The existing MIDI parser imports notes; it does not import CC, pitch bend or aftertouch.

## Ghost notes

Automatic ghosts are limited to the nearest Pattern ancestor, including sources nested in ordinary folders. Membership updates on editor refresh rather than on playback frames. Changing the active sound swaps the foreground and ghost sources; switching Pattern contexts removes the previous automatic set.

The Ghost Notes menu provides “Show all pattern sources automatically,” individual visibility choices and “Show none.” Explicit visibility changes survive refreshes within that Pattern. Existing indexed ghost painting is reused.

## Validation

`build/bin/VLTONE --patterncheck` runs the isolated Qt/controller regression check. It also runs in `--selftest`.

Coverage includes 640 px layout bounds; retained row identity; mix wheel protection; Sampler parameter/undo binding; quarter/eighth-bar fill; 6/8 timing; a real two-track MIDI file delivered as a Qt drop; preserved instrument and sibling notes; extension/undo/redo of both clip boundaries; invalid drops; fill into a source without MIDI; automatic ghost membership, opt-out and context switching.

`VLTONE_PATTERN_CHECK_SHOT=/tmp/pattern-compact.png` captures the compact fixture during that check. The existing `DAW_SHOT_PATTERN=1 ... --screenshot /tmp/pattern.png` captures the integrated editor; use `--theme logic-light` for light-mode review.

Design references applied: installed Apple Design HIG guidance on layout, typography, accessibility and drag-and-drop feedback; Apple Design on direct manipulation; Emil Design Engineering on restraint and immediate feedback for repeated editing actions.
