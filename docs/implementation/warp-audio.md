# Warp Audio

Right-click an ordinary local audio clip and choose **Warp Audio**. The selected
clip opens in its own **Warp** bottom panel. **W** shows Warp and **X** switches
to Mixer; each keeps its own height. The panel expands upward over the arrangement
with the correct covered timeline inset. Selection changes do not retarget the
editor. Closing the panel keeps its clip, map and viewport; opening another project
clears the binding. The product name remains **Warp**.

## Editing

- First open creates an identity map from the current trim/stretch and starts
  cancellable background transient analysis. Suggestions never move audio.
- Double-click adds a marker; click a transient suggestion to promote it.
  Ctrl-click or a marquee selects multiple markers. Drag moves the selection; Alt bypasses
  Snap. Arrow keys move by the grid, Shift+arrow by 0.01 quarter-note beats.
- Delete removes unlocked markers. The context menu locks, unlocks, resets or
  quantizes the selection. The two clip boundaries are fixed.
- Quantize uses the project grid by default, with adjustable strength. Reset
  removes interior markers. Warp Off restores the original linear playback and
  keeps the edited map. The inspector exposes pitch preservation, algorithm, onset
  sensitivity and precise marker position. It overlays the waveform below 900
  logical pixels; the header button opens/closes it without animation.
- Ctrl+wheel zooms around the pointer; wheel pans independently of arrangement.
  Space uses the project transport. One completed drag makes one history entry;
  Escape cancels it, including the live audio change.
- The overview pans the viewport and its edges change zoom. The ruler selects
  an assisted range; numeric range fields and Whole clip provide alternatives.
- A floating drag readout shows source, target, delta in milliseconds and the
  stretch on either side. Shapes distinguish suggested, manual and locked anchors.

## Assisted timing, Swing and Groove

**Suggest alignment** analyses the selected ruler range (the whole clip otherwise)
and previews a replacement map. **Before / After** switches the shared transport
between confirmed and proposed timing. **Apply** creates one undo entry; **Cancel**,
Escape and closing the panel restore confirmed playback without history.

The assistant inserts confident attacks, keeps every manual marker and locked
anchor, and never changes clip boundaries or project BPM. Uncertain attacks need
explicit inclusion. Tolerance preserves small timing offsets. Strength always
recomputes from the confirmed map; late analysis does not silently replace an
auditioned proposal. Range boundary anchors preserve timing outside the range.

The inspector reuses the MIDI groove presets and target-grid arithmetic, with
eighth/sixteenth Swing, timing strength and a Preview groove action. Save groove
extracts timing from 1/2/4 bars of the current audio or a chosen project MIDI clip,
starting at the selected range's beginning. User presets are stored in local
preferences (up to 64); committed marker coordinates travel with the project and
do not depend on a preset. Audio groove extraction does not affect gain/velocity.

The tempo readout uses the existing DSP tempo detector, reports its confidence,
ambiguity and alternatives, and never applies a detected BPM automatically.

## Implementation

`ClipWarpModel` stores stable marker IDs, source seconds, clip-relative musical
positions and the pre-Warp duration. `WarpMap.hpp` supplies the shared forward,
inverse and segment-speed functions. The editor, arrangement waveform, clip
placement and renderer all use these functions. Copying copies the map by value;
trim/split insert new boundary anchors through the inverse mapping. Tempo edits
retain musical positions. The optional project field leaves old projects off.

`WarpController.cpp` validates edits and owns transactions. Live drag mutations
are coalesced through the controller's existing deferred snapshot publication.
FFT onset analysis runs outside the audio callback and rejects stale generations.
`WarpTools` generates repeatable proposals. `miditools::gridTarget` is shared with
MIDI without changing its arithmetic. Three-band spectral flux uses a local
adaptive floor and energy refinement; confidence is an evidence score, not a
calibrated probability. Cached results are keyed by immutable processed source,
source range and algorithm version; sensitivity only filters the candidates.

Assistant sessions belong to `EngineController`, separately from `ClipModel`.
Only live placement publication opts into the preview. Render-clips-offline,
mixdown and save/recovery snapshots use the confirmed map. Source fingerprint,
map, placement, tempo and meter changes invalidate sessions, and Undo/Redo cancel
them. Live map replacement uses the existing immutable snapshots and DSP crossfade.

The editor uses shared theme/fonts/icons and `FrameClock`. Its static waveform,
grid and markers are cached at device resolution. Playhead ticks damage only the
old/new cursor strips; hidden editors stop their frame timer. Analysis polling
runs only while background jobs are pending.

`WarpPlayback` prepares two stretch processors outside the callback, alternating
at rate boundaries so each segment compensates its own DSP look-ahead. A short
overlap smooths boundaries. Replaced maps retain the previous stream and its
output cache for a 5 ms transition, including On/Off. Fixed 128-frame internal
quanta keep output independent of host block size. Seek resets the stream;
rendering, boundary changes and seeking perform no heap allocation.

Render dependencies include the active map and tempo. Offline processing prints
Warp once, clears it on the baked source, and retains it in original-audio
history. Cloud publication rejects maps in both clips and audio history.

## Supported range and checks

Local mono/stereo audio clips are supported. Comp recordings, sample loops,
multichannel audio, frozen tracks and tape fades report their incompatibility. Segment ratios
use the existing DSP limit of 1000:1. Group/semantic Warp and automatic reference
track alignment remain later roadmap items; see [Warp roadmap](../roadmap/warp.md).

Automated coverage: `warp_audio_test` (attack timing across segments, stereo,
live transitions, seek and allocation checks), `warp_test` (maps, controller
history, persistence, copies, trim/split, BPM, export and audio versions),
`warp_ui_test` (real Qt marker input, proposals, precise fields and compact layout)
and `warp_panel_test` (context menu, pinning, independent panel heights and inset).
`warp_test` also checks preview isolation, save/export, range anchors and groove
arithmetic. Regression targets include `miditools_test`,
`controller_test`, `time_stretch_test`, `processing_render_test`,
`track_mixer_sync_ui_test` and `mixer_fader_input_test`.
