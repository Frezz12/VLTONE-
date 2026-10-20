# Vocal alignment

Open **Vocal Alignment** from the context panel. With no selection the same
panel offers audio import and track creation. The first audio clip selected
chronologically is the guide; clicking another selected clip does not change
that order. A marquee uses lane order, then clip start time.

The window is nonmodal. Drop a timeline clip on the guide to replace it, drop
clips on the add button to append targets, or drop one clip on a target row to
replace it. These transfers copy references and restore the timeline gesture.
**Take selection** explicitly replaces the window contents. **Set as guide**
swaps the selected target with the guide. Keyboard users can populate the
window with the selection buttons and navigate the controls with Tab.

The left column holds the guide and targets. Select a target to compare its
waveform against the guide, with **Before** and **After** lanes on the same
project-time axis. After a successful run, the original waveform stays visible;
the third lane follows the applied Warp map. Connecting lines show movement of
the same source positions, not inferred word boundaries. The detail line reports
maximum movement, matches and skipped regions for the selected target.

Wheel zoom is anchored at the pointer. Drag or use Left/Right to pan, use +/−
to zoom, and Home or the fit icon to show all clips. The marker icon toggles
connecting lines. All compact actions have tooltips and accessible names.
Delete removes a target from the window, and double-click opens its Warp editor.
The gear reveals advanced settings; they scroll within small windows. The
numeric density field and slider stay in sync. Editing settings leaves the
last comparison visible until Align is pressed again. External clip edits or
Undo invalidate that result and require taking the selection again.

The preview uses the existing source-envelope renderer and exact Warp timing,
including trim, existing markers and printed offline sources. It is a timing
visualization, not a second audio renderer or an audio audition toggle. Peak
preparation for snapshots runs on the shared background executor and is cancelled
when the comparison changes or its window is destroyed.

**Align** processes all targets in one cancellable background task. Results
are editable Warp maps, using the existing Vocal stretching profile, or a
rigid clip translation for Shift only. The guide is unchanged. A batch commits
only after all source snapshots have been checked. One Undo restores the
entire run, including any promotion of processed audio into version history.
Changing settings in the same window recalculates from its saved baseline;
0% restores that baseline. After manual audio edits or Undo, take the selection
again to deliberately establish a new baseline.

Hybrid vocal combines log-band spectral shape, energy and onset strength,
estimates a coarse offset, and runs a coarse/fine constrained dynamic time
warp within the maximum project-time shift. Attacks matches onset peaks near
that offset. Shift only uses one translation; waveform refinement is used
only for near-identical audio, since phase cycles of independent performances
are not reliable syllable boundaries. Stereo powers are combined for analysis
without cancelling opposite-polarity channels.

Defaults: hybrid, 100% density, 5 ms tolerance, ±500 ms search, 1.25 maximum
stretch (0.8 minimum). Uncertain and unsafe intervals are left unchanged and
reported. The tolerance controls which corrections are ignored; it does not
guarantee recognition accuracy. Manual locked interior Warp markers retain
their source coordinates and absolute project positions.

Editable alignment is available in local projects. Recording, frozen tracks,
sample loops, layered comps, tape fades, audio with more than two channels and
pitch-changing Warp have explicit availability messages. Original media files
are retained. Clip effects after the player are not used as matching features.

## Validation

`vocal_alignment_test` exercises known shifts, local events, locked markers,
zero density, silence, distant repetitions, cancellation, 32 targets and
atomic controller Undo/Redo. `vocal_alignment_ui_test` exercises ordering,
roles, reference drops, batch application, before/after coordinates, nonuniform
Warp/trim, zoom, stale-result invalidation and closing during analysis. Screenshot
cases cover both themes, RU/EN, 100–200% scaling, empty and compact layouts.
Existing Warp, stretching and clip-drag regressions must also pass.

Real-vocal acceptance requires a separate annotated lead/double corpus with
permission to use the recordings. Compare all three methods against labelled
syllable onsets and offsets, including breathy consonants, omissions, repeated
words, noise and different registers. Measure timing errors and skipped
regions, then listen to isolated stereo doubles and the full vocal stack for
pitch, attacks and discontinuities. Synthetic timing tests alone do not
establish naturalness or that hybrid is superior on real performances.
