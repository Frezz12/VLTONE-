# VLTONE Color mini module

Color now lives in the shared mini-module rack. New channels have an empty rack;
adding Color enables it with Drive -20 / Tone 0. Both controls range from -100 to
100. The other built-ins are Doubler (Width 45%, Humanize 35%) and Chorus
(Amount 25%, Rate 0.22 Hz). Reset restores these values.

The rack accepts three cards on every audio channel, summing folder and Master.
Repeated effects and arbitrary order are supported. Cards process from top to
bottom within their selected **Pre FX** or **Post FX** group. Both groups are
before the fader, after the instrument, Sampler FX and merged channel input.
An instrument in the first Audio FX slot still precedes both groups.
Recording captures the physical input before these effects. Empty racks create
no DSP or latency. Host bypass keeps latency and crossfades smoothly.

## Interface

`MiniModuleRack` is shared by mixer and inspector. Matte cards and 36 px digital
knobs use the current theme, with fine outlines, accent rings and clear pointers;
decorative painting is cached by palette colors, size
and DPI. Parameter names and numeric values remain readable at 75/100/150/180 px.
The default 100 px strip places both controls side by side. Only the narrowest
75 px strips use a vertical layout. The shared well grows on empty strips too.
Narrow strips use compact parameter labels with full names in tooltips; custom
long labels and mode names are elided within their own field instead of overlapping.
`ui::Knob` supplies keyboard, wheel, Shift, precise entry, reset, accessibility,
automation and one Undo per gesture. Hiding/collapsing finishes the gesture.

One application-wide switch collapses/expands the entire rack on every channel
and inspector, independently of bypass. Mixer row height includes invisible channels,
so horizontal virtualization does not move the faders. Drag a header to reorder;
its menu also offers Move up/down, Replace, Appearance, Export and Remove.

Appearance offers Studio, Graphite, Ivory and Copper themes, Ring, Disc,
Segments or Fader controls, a background color or an embedded PNG (images are
resized to 512 px). The dialog previews changes and commits one Undo on OK;
Cancel leaves the module unchanged. Appearance does not invalidate Freeze or
reset DSP history. Each external control may also declare its own `style` in
the portable definition, overriding the module-wide control style.
The persisted style IDs remain `machined`, `rubber`, `glass` and `fader` for
compatibility; their current visual variants are Ring, Disc, Segments and Fader.

The optional mode selector sits below the controls: Color has Analog/Tape/Tube,
Doubler has Natural/Tight/Wide and Chorus has Classic/Ensemble/Deep. Modes contain
complete acyclic graphs and their control bindings; IDs, labels and ranges of
the two external controls stay stable. Tape folds Drive to negative magnitude
and uses a gentler tone range; Tube folds it to positive magnitude. Doubler modes
change its humanization range and softness; Chorus modes change depth and softness.

## Portable files

Use the card menu's **Export module** command to save a `.vltmini` file, including
its graph and current static values as defaults. Share that file. The recipient
can use **Install module from file** or place it in:

`Documents/VLTONE/MiniModules`

The **Open mini modules folder** action opens the actual platform Documents
location. Files are rescanned each time the add/replace menu opens; no restart
is needed. Invalid files remain visible with an explanation. Example files are
in `resources/mini-modules`.

The UTF-8 JSON envelope is `{"format":"vltmini","version":3,"definition":...}`;
version 1 and 2 files remain readable.
Definitions contain stable ID/name/version, nodes, ordered connections, and at
most two external controls. A control has ID/name/unit/min/max/default/log and
one or more node-parameter bindings. Definition v2 adds `appearance`, optional
`modes` (up to eight) and `defaultMode`. A mode carries its own nodes, connections
and controls with the same external IDs/ranges; only their bindings differ.
Bindings may use `bipolarMagnitude` to fold a bipolar knob around its center.
The file limit is 2 MiB, including at most 1 MiB of base64 PNG data. Projects embed
the definition, so changing or removing an installed file preserves saved
projects. Creator recompilation explicitly updates matching instances in the
open project. Project format is 14 and strip-preset format is 4. Selected
mode and position are saved per instance, including presets and copies.

## Graph engine

`MiniModuleInstance` implements `PluginInstance`. All three built-ins use the
same declarative format and executor. Version 1 supports acyclic mono/stereo
graphs with Input, Output, Gain, Mix, Color, Doubler and Chorus nodes. A Mix node
has two ordered inputs. The existing `AudioGraph` handles buffers and branch
latency compensation; `GraphProcessor(1)` runs sequentially with no extra worker
threads. Preparation allocates off the audio thread. Timestamped automation is
split at the original frame offsets, including macro fanout.

Doubler fixes Softness at 0.65. Chorus fixes Depth at 0.30 and Softness at 0.65;
Rate spans 0.05-1.5 Hz on a logarithmic knob. Full-size modulation plugins retain
their DSP. Doubler preserves true mono and widens mono material on stereo paths.
Unknown nodes and malformed definitions remain in the document as unavailable
cards; ordinary export reports the error. Local saves, Undo, templates, strip
presets, automation, Freeze and rendering include the rack. Legacy Color keeps
its instance ID, seed, bypass and parameters; virtual Color automation recovers
a card even without an explicit legacy slot. Affected Freeze is invalidated.

The [Creator](CREATOR.md) editor adds typed-port graphs in definition v3.
Code nodes, AI integration and multiplayer support are deferred. The
collaboration protocol remains version 6; mini modules are local-only in this
stage, and publication preflight rejects them rather than dropping the rack.

## Color DSP and calibration

The Jiles-Atherton tape model and Koren triode remain, with 4x oversampling and
48 samples of latency. Negative Drive uses tape; positive Drive adds triode.
Drive zero is exactly the delayed input, including nonzero Tone. Histories are
independent for L/R; parameter smoothing, DC removal and fixed gain compensation
remain. No noise, hum or wow/flutter is added.

For `u=abs(Drive)/100`, the calibrated effective drive is `u` up to 0.2 and
`0.2 + 0.22*(u-0.2)/(0.22+u-0.2)` above it. Tone shelves are limited to +/-3 dB.
The moderate reference is approximately 0.083% THD at 1 kHz/-18 dBFS, Drive -20,
Tone 0. Extreme Drive/Tone combinations at 1 kHz/-6 dBFS measure approximately
2.5-3.1% across 44.1/48/96/192 kHz. Folded-harmonic tests retain the -80 dBc
threshold. These are measurements of this approximation, not of analog hardware.

References: [DAFx tape model](https://www.dafx.de/paper-archive/2019/DAFx2019_paper_3.pdf)
and [Koren triode model](https://www.normankoren.com/Audio/Tubemodspice_article.html).

## Verification and listening files

`mini_module_test` covers definitions, sequential/parallel graphs, latency,
macro fanout, state, frame-offset automation, the sample-rate/block/mono/stereo
matrix, allocation-free process/reset, ordering, limits, migration and unavailable
modules. `channel_color_integration_test` covers actual routing, automation,
recording, dry/source export, Freeze, templates, copies and presets.
`--channel-color-check` exercises native Qt controls and mixer virtualization.

`channel_color_audition` renders production DSP to float WAV with an active
16-second excerpt, one second of pre-roll, latency removal and BS.1770 matching.
Old files are in `artifacts/color/audition`; updated files are in
`artifacts/mini-audition`. Both use 101-117 seconds of the same recording at
-16.0285 LUFS. The original recording is unchanged. These files are prepared for
subjective listening; automated measurements do not replace that review.

`mini_module_test --bench` compares 64 stereo channels with three cards against
the same DSP instances without graph wrappers, including three repeated Color
instances. Timings are machine-dependent; see `artifacts/mini-performance.log`.
