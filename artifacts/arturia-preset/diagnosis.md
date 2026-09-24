# Arturia VST3: silence after changing presets

Confirmed on the installed Pigments 4.1.1.3513. Its native Next Preset parameter produces `restartComponent(kParamValuesChanged)` (0x4).

## Cause

VLT interpreted that notification as a request to restart DSP and send every controller value back to the processor. Pigments exposes 4,139 parameters, including MIDI event helpers. Its CC7 helper (ParamID 2066, channel 1) reads zero; sending the readback creates a real MIDI volume-zero event. Other helper readbacks also generate unwanted controller/program-change commands.

An isolated diagnostic using the installed plugin and our VST3 support objects reproduced the failure:

| Input after a sounding note | RMS of a subsequent note |
| --- | ---: |
| No parameter replay (control) | 0.051044430 |
| Replay all 4,139 readbacks (old behavior) | 0.000002948 |
| Replay only automatable parameters (diagnostic control) | 0.052762740 |
| Replay only the zero CC7 readback | 0.000083051 |

The residual signal is the previous sound's tail. Logs: `replay-all.log`, `replay-none.log`, `replay-auto.log`, `replay-cc7.log`; source: `replay-probe.cpp`.

## Fix

`kParamValuesChanged` now refreshes host values without generating processor input or restarting audio. Combined metadata/structural notifications retain the existing gated handling. Removed the same synthetic replay from offline restart and pending-edit snapshots. Explicit `performEdit`, automation and MIDI events still reach the processor.

This follows the [Steinberg parameter notification contract](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Parameters%2BAutomation/Index.html): the flag invalidates host parameter-value caches. The [JUCE host](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h) likewise refreshes values without updating the processor.

## Verification

- Regression fixture reproduces a MIDI helper with a zero readback and independent DSP volume; preset refresh must preserve audio, explicit CC7 must still mute/unmute.
- Covers initial activation, value-only notifications while active, combined value/metadata refresh, host cache notification, pending offline snapshots and uninterrupted delay tails.
- `plugin_vst3_test`, `plugin_contract_test`, `plugin_render_state_test`, `plugin_scan_test`, `plugin_vst_test`, `plugin_editor_idle_ui_test`: passed. Application build completed successfully.
- Actual Pigments through the rebuilt VLT hosting library: default RMS 0.052535760; after two native Next Preset operations, 0.064860037 and 0.095837208. Host received the preset notifications; no processing restart; subsequent notes remained audible.
- Actual Analog Lab V: default RMS 0.039840221; after two native Next Preset operations, 0.111791288 and 0.037389977. Subsequent notes remained audible; this plugin also requests other restarts, which are still honored.
- MiniFreak V remained audible in the diagnostic, but emitted no preset notifications, so that run alone does not establish that its preset actually changed.

These are headless audio tests using the plugin's own exposed Next Preset control, not a manual GUI comparison. See `host-preset-probe.cpp` and `host-*.log`.

The rebuilt application is `D:\Code\DAW\build-windows\bin\VLTONE.exe`. The installed copy in Program Files was not replaced.

