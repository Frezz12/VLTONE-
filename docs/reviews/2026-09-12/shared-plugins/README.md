# Shared Plugins — verification, 2026-09-12

macOS development build: `build/bin/VLTONE`.

Eight focused CTest suites passed: plugin_batch_test, plugin_batch_ui_test,
processing_render_test, offline_render_ui_test, track_creation_test,
track_creation_ui_test, channel_strip_preset_test, render_safety_test.

The Russian dark-theme screenshot was captured from the real Qt dialog.
Its UI check also exercises audio Clip FX, source switching, cancellation,
independent copies and context-panel eligibility.

The additional general `--selftest` run under `QT_QPA_PLATFORM=offscreen`
did not complete: `Notebook check failed: notebook disabled the main GPU scene`.
That broad test cannot be reported as passed. Its initial sandboxed run also
hit Chromium Mach-port permission restrictions; the logged retry ran outside
the tool sandbox and reached the Notebook check.

The feature tests use built-in effects and generated audio; a licensed vendor
Auto-Tune GUI was not exercised.
