# EZdrummer editor idle investigation

The installed plugin cache identifies EZdrummer as a legacy VST at `C:\Program Files\VstPlugins\Toontrack\EZdrummer.dll`, UID `64666865`, reported version `130`.

## Cause

`VstInstance::pumpMainThread()` sends `effEditIdle` to an open VST editor. However, its normal caller, `EngineController::pumpPluginEvents()`, skips the slot traversal when no plugin has published a new work generation. Its fallback compatibility sweep runs every 64 calls.

`MainWindow::refreshUi()` calls that function every 33 ms during project playback and every 100 ms while stopped. Without other notifications, a legacy editor could therefore wait approximately 2.1–6.4 seconds between idle callbacks. Groove preview inside a plugin does not require the DAW transport to be playing. The slow parameter-panel poll did not service native editors either.

This path is independent of GPU rendering. The code defect matches the reported sparse animation triggers; an interactive visual comparison with EZdrummer/REAPER has not been performed.

## Change

The visible native VST editor now has its own precise 20 ms Qt timer on the UI thread. It pumps only the currently attached instance, leaving the project-wide event traversal and the 200 ms wrapper/parameter poll unchanged. The timer stops on hide, minimize, detach or close, and resumes on showing the editor. Each callback re-resolves the slot and checks its identity before calling the plugin.

Periodic editor idle service is also used by [JUCE's VST host](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_processors/format_types/juce_VSTPluginFormat.cpp), separately from ordinary host notifications.

## Verification

`plugin_editor_idle_ui_test` uses a real VST fixture that counts `effEditIdle` calls without sending its own wake notifications. With the transport stopped, it compares the old generation-gated path against the new timer, checks hide/show and detach, and verifies that regular editor service does not cause continuous full-project slot scans.

Build: `build.log`. Host tests: `host-tests.log`. UI timing test: `ui-test.log`.

All three checks passed: `plugin_vst_test`, `plugin_contract_test`, and
`plugin_editor_idle_ui_test`. The UI test measured 0 old-path calls / 400 ms,
20 visible calls / 400 ms, 0 hidden calls / 200 ms, and 20 restored calls /
400 ms. Full-project slot scans during the active 400 ms measurement: 0.

The corrected build is `D:\Code\DAW\build-windows\bin\VLTONE.exe`. The installed copy has not been replaced.
