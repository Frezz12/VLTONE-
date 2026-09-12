# Plugin editor and insert drag regressions

Checked on macOS, Qt 6.11.2, RelWithDebInfo; the GPU runs use threaded Metal.
The other computer's OS, build, plugin formats and vendor names were not available.
The tests reproduce defects in the host, but do not establish that every reported
vendor-specific symptom has the same cause.

## Findings and fixes

1. `SlotRow` and `DragTitle` started `QDrag` from any sufficiently displaced
   `MouseMove` carrying `LeftButton`, even without a press on that row. Native
   drag consumes releases, and a rebuilt row could therefore start another drag
   from the remainder of the previous gesture. Both sources now require a fresh
   left press, consume it before entering `QDrag::exec`, reject reentrant starts,
   and disarm on release/lost capture/hide/deactivation. Post-drag callbacks are
   guarded against deletion of the source during the native event loop.
2. The GPU bridge retained the source widget's implicit mouse capture after a
   native drop. It now clears that capture and forwards `UngrabMouse` when native
   drag takes over or the window loses capture/activation. A hardware test verifies
   exactly one accepted drop and that a subsequent stale pressed move goes to the
   current target, instead of back to the old insert.
3. `PluginEditorWindow::closeEvent` published `closing` while the native GUI was
   still owned by the plugin; detach happened only in the deferred destructor.
   Reopening during a nested event loop failed for the VST fixture. The GUI now
   detaches synchronously before publishing close. Pending initialization is
   cancelled; callbacks from an obsolete GUI generation cannot close its replacement.
   MainWindow also ignores a queued presentation of an already closing editor and
   guards registry cleanup by window identity, including direct destruction.
   VST clears its editor-open state and host callback pointer before vendor teardown.

## Reproduction and validation

The new checks were run before the corresponding fixes. The drag check failed
12 assertions; the native reopen check failed 12 assertions; the hardware bridge
check failed with `Native drop retained the source's implicit mouse capture`.
The same checks pass after the fixes.

- `--plugin-interaction-check` on Qt offscreen: 16 assertions using the actual
  slot/title widgets and `QDrag::exec`. Covers unarmed moves, secondary press,
  click threshold, repeated trailing moves, release, lost capture and a new press.
- Native fixture check: eight immediate close/reopen cycles per rendering mode,
  with the previous close event deliberately kept on the stack while the new
  editor initializes. Also checks GUI rebuild with an obsolete queued close, and
  closing before deferred initialization. Passes with GPU disabled and enabled;
  the latter explicitly requires visible GPU presentation and delivered frames.
- `gpu_scene_test --hardware`: passes with Metal, including hover/scroll regressions
  from the earlier mixer fix and the new drag capture check.
- Eight CTest targets passed: `plugin_host_test`, `plugin_insert_test`,
  `plugin_vst3_test`, `plugin_vst_test`, `au_editor_lifetime_test`,
  `gpu_scene_test`, `gpu_fallback_test`, `plugin_drag_lifecycle_test`.
  VST and drag tests were rerun successfully after adding the VST backend check
  for 16 GUI open/close cycles on a single instance and idempotent close.
- Final `daw` build and `git diff --check` passed. Executable: `build/bin/VLTONE`.

Native editor coverage uses the repository's VST shell fixture, whose GUI protocol
records the native host handle; it does not draw a commercial plugin interface.
No user project, installed plugin scan or audio device is used. This is host
lifecycle coverage, not certification of all AU/CLAP/VST3 vendor GUIs or other OSes.
The hardware scene test deliberately triggers its foreign-surface fallback at the
end; that diagnostic is expected. Existing macOS test-frame QStyle warnings and
headless translator teardown messages are present in the logs.

Commands (from the repository root):

```sh
QT_QPA_PLATFORM=offscreen VLT_GPU_WORKSPACE=0 build/bin/VLTONE --plugin-interaction-check
VLT_GPU_WORKSPACE=0 VLT_TEST_EDITOR_PLUGIN="$PWD/build/plugins_test/DawTestVstShell.vst" build/bin/VLTONE --plugin-interaction-check
VLT_GPU_WORKSPACE=1 QSG_RHI_BACKEND=metal QSG_RENDER_LOOP=threaded VLT_TEST_EDITOR_PLUGIN="$PWD/build/plugins_test/DawTestVstShell.vst" build/bin/VLTONE --plugin-interaction-check
QSG_RHI_BACKEND=metal QSG_RENDER_LOOP=threaded build/bin/gpu_scene_test --hardware
ctest --test-dir build -R 'plugin_drag_lifecycle|plugin_(vst|vst3|insert|host)_test|au_editor_lifetime|gpu_scene|gpu_fallback' --output-on-failure
```

Before/after logs are preserved in [plugin-interaction-evidence](plugin-interaction-evidence/).
