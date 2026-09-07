# Compact Sampler editor

The instrument editor defaults to **960 × 600** logical pixels (previously
1080 × 688), excluding the shared internal title bar. The audio-clip editor uses
the same panel at 960 × 562, without the plugin header. The instrument window's
area is about 22% smaller.

| Before | After | Why |
| --- | --- | --- |
| Envelope, tools and keyboard precede the waveform | Waveform and file actions stay above the settings tabs | Start with the sound being edited; retain visual context while adjusting it |
| Many small controls in one long processing row | Playback, Envelope and Processing tabs with named groups | Expose related controls together without increasing the window |
| 34 px dials, 8 px labels replaced by values on hover | 40 px dials, 10 px persistent captions and 11 px values | Read the parameter name and its value simultaneously |
| Dense teeth and glow around small dials | Clear continuous arc, contrasting pointer and keyboard focus outline | Make the value and editing target distinguishable at desktop scale |
| Wheel scrolling changes parameters | Wheel events over parameter knobs and mode selectors are ignored by the control | Navigate the page without changing the sound |
| Large disabled MIDI envelope in the audio-clip editor | Only relevant tabs and parameters appear for audio clips | Avoid spending space on unavailable controls |
| Separate hardcoded monospace readout fonts | Readouts inherit the application's Inter typography | Keep typography consistent with the rest of VLTONE |

The waveform markers, envelope handles, keyboard audition/root assignment,
sample loading/drop, sampler FX rack, parameter automation and undo bindings
remain on the existing controller paths. Parameter dragging is immediate; no
position/value animation is added. The main body never scrolls horizontally;
vertical scrolling provides access to expanded keyboard content in small windows.

Design references: installed Apple HIG layout, typography, accessibility and
entering-data guidance; Apple Design direct manipulation; Emil Design Engineering
stable geometry, persistent context and restrained feedback. These principles
are implemented with Qt Widgets/QPainter, using the existing theme palette.

Validation: `--samplercheck` exercises every tab at default/minimum widths in
instrument and missing-target Clip contexts, control containment, keyboard
expansion, wheel protection and a real sampler parameter drag with one-step undo.
`--selftest` includes this check and the existing automation/editor tests.
Screenshot fixtures support `DAW_SHOT_SAMPLER`, `DAW_SHOT_SAMPLER_MIN`,
`DAW_SHOT_SAMPLER_PROCESSING`, `DAW_SHOT_SAMPLER_ENVELOPE` and
`DAW_SHOT_CLIP_EDITOR`; pass a real audio path to `DAW_SHOT_SAMPLER` to see a waveform.
