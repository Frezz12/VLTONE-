# Compact Sampler editor

New Samplers start at unity gain (0 dB); centred voice pan uses the same
unity-preserving stereo balance as audio clips and the mixer. A root-key note
at velocity 127 therefore retains the source sample's level. Piano Roll starts
with velocity 127, then remembers the last edited note's dynamics; Pattern
rhythm fill and computer/Sampler keyboards also start at full velocity. The
channel/FX fader still provides gain above unity. Saved volume values and MIDI
velocities are retained; the Sampler volume parameter range stays unchanged
so existing automation keeps its scale.

The instrument editor defaults to **1040 × 760** logical pixels, excluding the
shared internal title bar. The audio-clip editor uses the same panel at
1040 × 722, without the plugin header. All settings and the expanded keyboard
fit at the default size; smaller windows retain vertical scrolling.

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

## Sample note and tuning

Instrument Playback includes **Note & tuning**. **Detect note** analyzes the
processed source between Start/End, excluding an appended reverb tail, before
Tune, Stretch Pitch and note modulation. It displays the nearest MIDI note,
frequency and signed cents offset using A = 440 Hz. Octave labels match the
existing keyboard (MIDI 69 is A5). Analysis itself does not edit the project.

Root Note changes immediately highlight and reveal
the corresponding key, including notes outside the keyboard's current viewport.
Right-clicking a key updates the Root field immediately with one undo step.

**Set root** assigns that note as Root Note. **Correct tuning** replaces Fine
Tune with the opposite cents offset; coarse Tune/Range and Stretch Pitch retain
their values. Each action is independently undoable and repeated application
is disabled. Fine Tune also accepts keyboard/numeric input in 0.1-cent steps,
spans ±100 cents, works with Range at zero, supports parameter automation, and
is saved with the instrument. It appends `finepitch` after all existing parameter
indices; old states initialize it to zero.

The worker analyzes bounded windows with [YIN](https://www.ee.columbia.edu/~dpwe/papers/deChevK02-yin.pdf)
and refines frequency at the source rate. A stable channel reference avoids
opposite-phase stereo cancellation. Silence, noise, short regions and changing
notes produce an unavailable/unstable result. This tool is intended for one
sustained note; chords and recordings with many notes are outside its scope.
Changing the source or Start/End discards results and cancels pending work;
closing the editor also cancels its worker without blocking the UI.

`sample_pitch_test` checks synthetic detuning, bass/treble, harmonics, stereo/DC,
rejection/cancellation, audible correction and state compatibility. The existing
`--samplercheck` also checks numeric input, background detection, root assignment,
correction, undo/redo and invalidation. Set `DAW_TUNING_CHECK_DIR` to an existing
directory to capture the detected result in light/dark themes during that check.
