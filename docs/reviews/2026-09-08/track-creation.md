# Track creation dialog

The track-header action is a 24 × 24 plus icon. It opens a window-modal dialog
with type, quantity (1–64), optional name, mono/stereo, input and output. The
initial insert rack contains three slots; additional slots, plugin replacement,
ordering, bypass and removal are available in place. MIDI tracks also expose
an instrument slot. The existing searchable plugin picker is reused.

## UX review

| Before | After | Why |
| --- | --- | --- |
| Wide text button and track-type popup | Compact plus and one modal dialog | Keep the header small while making batch configuration explicit |
| One immediate track at a time | Quantity and optional numbered name | Create a group with one action |
| Configure plugins separately on every track | Edit a draft rack, copy state into independent instances | Repeat a sound without repeating the setup |
| Multiple potentially partial mutations | Atomic batch and one Undo | A failed plugin/state restore removes the entire new batch |
| Hidden combo arrows inherited from the global theme | Small chevrons in this form | Distinguish choices from editable text |

Applied apple-design-hig, apple-design and emil-design-eng to the existing Qt
Widgets architecture. Native fields, label buddies, accessible names, default
Create, Cancel/Escape and stable layouts are retained. No animation delays the
controls. The form scrolls at small window heights; actions remain visible.

## State and lifetime

- An independent controller initialized without an audio device owns the draft.
  Cancel does not change the project or its undo history.
- Draft host parameter commands receive one silent processing block when edits
  are pending. Idle preview ticks do not run DSP. This path cannot render when
  the controller was initialized for a live audio device.
- Native AU parameters are sampled on capture so stale document mirrors cannot
  override edits made directly inside an AU view. Left/right states, bypass,
  mix and channel mode are retained; IDs and plugin instances are distinct.
- Editor dialogs detach foreign views before plugins are retired. The outer
  editor follows the plugin's requested dimensions; fixed-size native editors
  remain fixed.
- Local batch creation compiles the graph once, restores every state, and adds
  one undo entry. Failed instantiation or state restoration rolls back the batch.
- Shared projects currently allow empty batch creation. The insert/instrument
  draft sections are disabled with an explanation because shared commands do
  not carry arbitrary opaque plugin state. Plugins can be added afterwards.

## Validation

Built `daw`, `track_creation_test`, `controller_test`, `plugin_insert_test`.
CTest: all four of `track_creation_test`, `track_creation_ui_test`,
`controller_test`, `plugin_insert_test` passed.

The real CLAP fixture checks opaque state, independent instances, mono input,
routing, unique names, single undo/redo, invalid request rollback, failed plugin
and state rollback, all track types, dual mono states, bypass and sampler slot
ownership. It also checks delivery of pending preview edits and no repeated DSP
processing while idle.

`--trackcreationcheck` checks Cancel, opening/closing an editor, ordering,
three-track state transfer and undo/redo. Reviewed Russian dialog renders in
dark and light themes, at 600 × 640 and 520 × 510.

A native run with the installed ValhallaUberMod AU and `NSZombieEnabled=YES`
passed editor attachment/teardown and state transfer to three instances. It
changes a parameter via the host, then directly through the AU processing API
without a host notification, and checks that the latter value wins on capture.
No use-after-release diagnostic was emitted. The Mac was locked, so interactive
inspection of the foreign native view was unavailable; QWidget screenshots do
not include that compositor surface.

All new dialog strings have Russian translations. `git diff --check` passed.
The executable is `build/bin/VLTONE`; `/Applications` was not replaced.
