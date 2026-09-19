# VLT Pitch

VLT Pitch is the built-in `daw.pitch-corrector` audio effect for a single vocal
melody. Insert it in a vocal track's Audio FX chain. It supports mono and linked
stereo. Chords, a choir and an entire mix are outside its intended input.

## Recording and quality

Enable the track's input monitor to hear correction while recording. The take
remains dry, so the performance and its tuning settings remain editable.

- **Real-Time** uses a causal pitch-synchronous shifter and reports a 10 ms
  processing reference to the host. Note acquisition needs additional history;
  10 ms does not describe the time to identify a newly sung note, the audio
  interface round trip, or the phase displacement of individual shifted periods.
- **HD** uses a longer pitch-analysis history and 20 ms of lookahead to choose
  a stable pitch path. It aligns those decisions with the source audio and
  uses the same pitch-synchronous waveform/source-filter synthesis as Real-Time,
  with a fixed 80 ms processing reference (`ceil(sampleRate × 0.080)`).
  The planned Signalsmith backend was replaced after real-vocal audition
  exposed metallic timbre even at a constant 25-cent shift with formants off.
  Relative-harmonic-phase measurements also exposed dispersion that steady
  pitch and magnitude-spectrum tests had missed.
- The display reports the current algorithmic latency. Device buffering and
  slower effects elsewhere in the compensated graph can add monitoring delay.
- Stop playback/recording and disable monitoring before changing quality.
  Quality changes received through undo or a shared project remain visibly
  pending until the engine is idle. Other musical controls remain editable.
- Freeze/export use the selected saved quality; they never silently upgrade
  Real-Time to HD.

## Controls

The compact graphite editor has one large Retune Speed dial, two smaller
Humanize/Vibrato dials, and a piano keyboard below them. Key, scale, A4 reference,
and quality remain available across the top. Voice range and output are in
Settings. The editor is local HTML/CSS/JavaScript in the app's existing Qt
WebEngine surface, with a bounded WebChannel bridge to the controller. All
resources are compiled into the executable; no web server, internet connection
or additional UI framework is needed. It opens at 640 × 410
and can shrink to 560 × 360, excluding the host's editor header.
Its HTML content has no nested outer frame. VLT Pitch keeps the host's standard
dark title bar and 8 px top corners, with a graphite body and softer 24 px
bottom corners. Other editor windows retain their existing appearance.
The application theme's accent tints the glass keys and metal controls and
colors active notes, knob markers, status lights and keyboard focus. Theme
changes reach the open editor through its existing bridge without reloading it;
the host title bar retains its standard color.
Drag updates are coalesced to browser presentation frames and retain local
ownership until the host acknowledges the final edit. Knob markers track the
pointer directly; fixed-width numerical readouts refresh at 20 Hz during a
drag and immediately show the exact value on release. A full drag spans 320
pixels, with tenfold finer Shift control. Telemetry updates only changed DOM
values and does not rebuild the controls.

| Control | Function |
| --- | --- |
| Retune Speed | 0–200 ms. Clockwise/upward dragging makes correction faster; **0 ms** is the most aggressive setting. Click the number for direct entry. |
| Humanize | Relaxes sustained notes after 120 ms, preserving natural movement. |
| Vibrato | Retains periodic movement within a stable note. |
| A4 Hz | Reference pitch, 400–480 Hz in 0.1 Hz steps; default 440 Hz. |
| Key / Scale | Root and scale; Custom selects allowed absolute pitch classes. |
| Piano keyboard | Enable or exclude pitch classes. An edit selects Custom and preserves the other allowed notes; the final allowed note cannot be removed. The current correction target is highlighted and depressed. |
| Voice | Auto 60–1200 Hz, Low 60–350, Mid 100–800, High 180–1200. |
| Formants | Preserves the vocal spectral envelope; enabled by default. |
| Settings | Voice range, correction amount, output level, and save/delete user presets. |
| Send to all (branching icon beside Settings) | Copy all 12 parameters from this editor to every other VLT Pitch in the project, including master, Clip FX, sampler FX and both dual-mono processors. Bypass, routing and automation remain individual. One Undo restores every destination. |
| Preset menu | Choose Natural / Pop / Tight / Hard, or recall local user presets. |
| Output | −24 to +12 dB. |
| On / Bypass | Enable or bypass the effect through the host. |
| Pitch readout | Input note → target note, followed by signed correction in cents: “Shift +35 ct” raises pitch and “Shift −35 ct” lowers it. |

Faster retuning progressively reduces Humanize and Vibrato's influence; at
0 ms the result is intentionally quantized. Very confident voiced transitions
use a 4 ms destination confirmation and 2-cent boundary hysteresis, holding a
plateau during confirmation instead of passing the sung glide. Less confident
segments retain the longer confirmation and consonant protection. A separate
1 ms coefficient ramp remains to prevent
zipper noise. Silence, consonants and uncertain pitch are protected from forced
note correction.

Retune time is distinct from the processing latency shown at the bottom right.
The stored `tune` ID and normalized 0–100 automation values remain unchanged:
the UI converts with `ms = 200 × (1 − tune/100)²`. Existing projects and user
presets need no migration. Natural / Pop / Tight / Hard display 72 / 18 / 2 / 0 ms.

New instances start at **20 ms Retune Speed, Humanize 0, Vibrato 0**, displayed
as Default. Double-click reset returns each dial to these values. Factory
styles remain separate choices in the preset menu.

After analyzing a beat, **Apply key to VLT Pitch** sends its root and scale to
the project's existing VLT Pitch instances. Other vocal settings stay intact.
For downloaded audio, the key is applied only after the audio imports successfully.
Enable **Settings → Quick Import → Set detected key in VLT Pitch** to apply
the analyzed key automatically when the selected template opens. Key detection
must be enabled; unavailable results leave the template's tuning unchanged.
The analysis dialog shows when the automatic transfer has already happened.
Undoing Quick Import restores the clean template and its original pitch settings.
Sending a full configuration with a different quality requires playback and input
monitoring to stop; a refused send changes no destination settings.

Natural / Pop / Tight / Hard change only Tune, Humanize and Vibrato. They retain
key, scale, reference frequency, voice range, quality and output level. Full user
presets store all parameters. The three dials support vertical dragging, Shift
fine control, keyboard editing, double-click reset and automation. Numerical
readouts accept direct entry. Quality and the custom note mask are not automation
lanes. Piano note/scale edits undo together, and all twelve keys support keyboard
focus and activation.

## Reproducible validation

Build `pitch_corrector_test`, `pitch_corrector_envelope_test`, `pitch_corrector_formant_test`,
`pitch_corrector_phase_test`,
`pitch_corrector_integration_test` and `pitch_corrector_ui_test`, then run their
CTest entries. The tests cover sample
rates, variable block partitions, tuning accuracy, allocation-free processing,
attack/PDC timing, state, quality deferral, dry capture, export and UI editing.
The envelope regression includes short breath consonants and low-to-high note
attacks after 5, 15 and 30 ms gaps with different source phases. It checks actual
output timing and level independently of the pitch detector's confidence.
The formant regression generates six analytic source/filter vowels, checks its
own spectral calibration, and compares the preserved envelope against the
known filter and against processing with formants disabled.
The phase regression corrects synthetic vocal pulses with vibrato and checks
their known relative harmonic phases at 44.1, 48 and 96 kHz. It rejects the
previous spectral backend while allowing arbitrary common phase or delay.

The synthesis phase stays continuous through short uncertain frames. Protection
returns the pitch ratio toward unity instead of repeatedly mixing opposing wet
and dry phases. In Real-Time, a separate read-head anchor uses the existing
10 ms history to preserve consonants and attacks. The LPC filter keeps source
timestamps and state for both splice heads; formant bypass keeps that state warm
for a smooth return.

The opt-in `pitch_corrector_bench` target takes:

```text
pitch_corrector_bench INPUT_WAV OUTPUT_DIR [seconds=24] [block=64] [scale=0] [key=0]
```

It writes aligned dry/Natural/Hard WAVs for both modes, timing/level measurements
and pitch trajectories. A zero duration audits the complete source; a positive
duration begins just before its first audible phrase. It does not modify or
upload the source. Real vocal audition remains part of evaluating a preset;
synthetic pitch accuracy alone does not establish vocal quality.

DSP references: [YIN](https://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf) and
[pitch-synchronous delay splicing](https://www.katjaas.nl/pitchshiftlowlatency/pitchshiftlowlatency.html).
Both vocal synthesis modes are independent implementations. The project's
existing Signalsmith time-stretch implementation remains separate.
