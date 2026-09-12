# Offline Render: processing layers and clip history

Offline Render is a local-project operation for one or more selected audio clips.
It prints the currently selected audio version through a temporary plugin chain,
then replaces each checked clip's audio source. Clip IDs, names, arrangement
positions, mute/color, realtime inserts and downstream mixer routing remain.

## User workflow

1. Select audio clips and open Offline Render from Edit or a clip's context menu.
2. Check the clips to process. Each row identifies the clip, track and time range.
3. Add/configure effects in the compact mixer insert rack, or load a chain template.
   Every new dialog starts empty, including when opening a previously rendered clip.
4. Render. All selected outputs are prepared before any clip changes. Cancel,
   plugin failure or a project edit during progress discards the staged batch.
5. Open the clip in the sampler and use **Clip FX → History** to choose the original
   or any rendered version. The context action **Restore Original Audio** returns
   that clip to its first version. Both operations support Undo/Redo.

Rendering an older version appends a new version and keeps the other versions.
The dropdown presents one original and numbered results in chronological order;
each result's tooltip identifies its parent version and effect names.

## Audio and storage

- A render consumes the current source, trim, stretch, sample processing, fades,
  gain and pan exactly once. Its replacement WAV has neutral playback settings.
  Realtime Clip FX and mixer effects remain live and are excluded from printing.
- Project tempo, meter and the clip's musical position reach the temporary chain.
  A previous render's tail becomes ordinary source audio for the next layer.
- `ClipModel::offlineHistory` stores immutable `ClipAudioVersionSource` values;
  `offlineVersionId` records the selected version. A source includes comp/take
  state, so the first render of a comp can also be restored.
- Save and Save As copy every version and its take media into the package's
  `Content/`. The document codec and crash snapshots preserve history, while undo
  memory accounting includes its variable-size payloads. Take deletion/cleanup
  and mixdown overwrite checks treat historical media as referenced project audio.
- The previous single `offlineProcess` cache remains readable for compatibility.
  The first render/history restore migrates its original and valid cached result.
  Old plugin chains are never preloaded into the new offline dialog.
- Offline history remains local-only, consistent with Offline Render. Cloud
  snapshot projection rejects it and removes local fields from its rejected
  projection; it never silently uploads paths or discards local history.

## Verification

`processing_render_test` exercises actual WAV output with the built-in equalizer:
multi-clip replacement/deduplication, cumulative gain, live-FX exclusion, undo/redo,
earlier-version rendering, tails, portable saves, recovery, invalid/missing media,
cancel/failure/reentrant edits and comp history protected from take cleanup.

`offline_render_ui_test` (`VLTONE --offlinecheck`) checks selection checkboxes,
empty-rack gating, template round-trip, compact geometry, the next empty dialog
and sampler history activation. Add `--theme dark --language ru --screenshot PATH`
to capture the dialog, sampler and history popup.

Related regression checks: `render_test`, `render_safety_test`,
`channel_strip_preset_test`, `cloud_project_test` and `VLTONE --samplercheck`.

Verified on macOS on 2026-09-12: the application builds; all six named CTest
suites and the sampler layout/interaction check pass. Audio assertions use real
WAV files and the built-in equalizer; third-party vendor plugin UIs are not part
of the automated fixture. Russian dark-theme captures are in
`docs/reviews/2026-09-12/offline-render/`.
