# Warp Audio

Right-click an ordinary local audio clip and choose **Warp Audio**. The selected
clip opens in the **Warp** tab beside **Mixer**. The shared bottom panel retains
the mixer's resize handle, covered timeline inset, and floating-window behavior.
Selection changes do not retarget the editor. Closing the floating window docks
both tabs; closing the panel keeps its state.

## Editing

- First open creates an identity map from the current trim/stretch and starts
  cancellable background transient analysis. Suggestions never move audio.
- Double-click adds a marker; click a transient suggestion to promote it.
  Ctrl-click selects multiple markers. Drag moves the selection; Alt bypasses
  Snap. Arrow keys move by the grid, Shift+arrow by 0.01 quarter-note beats.
- Delete removes unlocked markers. The context menu locks, unlocks, resets or
  quantizes the selection. The two clip boundaries are fixed.
- Quantize uses the project grid by default, with adjustable strength. Reset
  removes interior markers. Warp Off restores the original linear playback and
  keeps the edited map. Options exposes pitch preservation, algorithm and onset
  sensitivity.
- Ctrl+wheel zooms around the pointer; wheel pans independently of arrangement.
  Space uses the project transport. One completed drag makes one history entry;
  Escape cancels it, including the live audio change.

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
multichannel audio and tape fades report their incompatibility. Segment ratios
use the existing DSP limit of 1000:1. Groove, group/semantic Warp and automatic
track alignment are outside this version.

Automated coverage: `warp_audio_test` (attack timing across segments, stereo,
live transitions, seek and allocation checks), `warp_test` (maps, controller
history, persistence, copies, trim/split, BPM, export and audio versions),
`warp_ui_test` (real Qt marker input) and `warp_panel_test` (context menu,
pinning, tabs, detach/dock and timeline inset). Regression targets include
`controller_test`, `time_stretch_test`, `processing_render_test`,
`track_mixer_sync_ui_test` and `mixer_fader_input_test`.
