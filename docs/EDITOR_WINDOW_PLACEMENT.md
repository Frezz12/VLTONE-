# Editor window placement

All hosted editors (plugins, Pattern, piano roll, Settings and other internal windows) share `InternalEditorFrame`.

| Before | After | Why |
| --- | --- | --- |
| Frames were children of the body below the transport and tool strip | Frames are siblings of the transport, tool strip and body in the full workspace | A dragged editor can cover the header, including receiving mouse input there |
| The whole frame was clamped inside the body | The full title bar stays inside the workspace; the content can extend below its bottom edge | Users can put an editor out of the way and still retrieve or close it |
| Resize limits assumed the bottom edge was visible | Resize constraints allow a parked body while retaining valid limits and an accessible title | Resizing a parked editor cannot lose its title or create inverted clamp bounds |
| A minimum frame size could exceed a reduced host | Effective minimum dimensions shrink with the host | The title and its controls remain accessible after resizing the main window |

Movement remains direct mouse tracking. Horizontal bounds retain the full title and both window buttons. The lowest position leaves the 36 px title plus the frame contour visible. Qt clips the parked content at the application workspace boundary.

The editing body remains the default placement/maximize area. Expanding a plugin's parameter dock keeps an editor fully on screen if it was fully visible before; a deliberately parked editor keeps its title position.

Saved positions carry `placementVersion = 2`. Legacy body-relative positions are translated once into workspace coordinates, preserving their visual position on upgrade. Reopening and host resizing reapply title visibility constraints.

The content-destruction callback is disconnected before `InternalEditorFrame` members are destroyed. QWidget deletes children during base-class teardown; allowing that callback to clear already-destroyed QPointers corrupted memory when a later editor was opened. Replacing a frame's content also disconnects callbacks from the old content.

## Validation

- `build/bin/VLTONE --editorcheck`: legacy position migration, upward/downward mouse drags, title hit testing, retrieval, save/reopen, maximize/restore, resizing parked windows, narrow and short hosts, and opening Pattern/piano-roll editors after destroying a frame. On a native Qt platform the fixture includes a native child surface like a plugin editor.
- `build/bin/VLTONE --selftest`: the same placement checks plus actual MainWindow/piano-roll interaction across the header and at the bottom, followed by the application's editor and plugin checks.
- `VLTONE_EDITOR_CHECK_SHOTS=/tmp ... --selftest` saves `editor-over-header.png` and `editor-parked.png` from the integrated UI.

Applied design guidance: Apple Design direct manipulation and stable grab positions; Apple HIG accessible controls and predictable pointer interactions; Emil Design Engineering immediate response for frequently repeated editing gestures.
