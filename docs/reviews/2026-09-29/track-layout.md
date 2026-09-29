# Track layout and compact rows

Implemented the requested correction to the track redesign: the large track icon remains on the left, the name sits above small 19 × 15 px state buttons, and level and pan remain to their right. Narrow columns still switch level to its compact knob without overlapping controls.

| Interaction | Result |
| --- | --- |
| Double-click the track name or empty header space | Toggle a 24 px lane and restore that track's previous height |
| Double-click level, pan, or a state button | Preserve the control's own behavior and the lane height |
| Right-click the name → Rename Track | Start inline renaming after the menu returns focus; Return commits |
| Minimize / Restore in the track menu | Apply the same action to the selected tracks, preserving individual restore heights |
| Collapse a folder using its existing disclosure | Show its ordered descendants, nested hierarchy, waveforms, notes and automation in one overview lane |

The separate height disclosure button was removed. Compact audio and automation lanes do not retain invisible caption, fade or breakpoint hit targets. Open take rows are hidden at minimum height and restored with the track. Saved projects retain the restore height; collaboration projections keep it local.

## Stage build

Updated `build-windows/stage/bin/VLTONE.exe`, version 0.3.0.0, with the current Release configuration. Application, scanner, watchdog, reporter, Qt deployment and remaining app-local DLLs were updated through the existing CMake install rules. The staged executable matches the built executable:

`F694270FD72E2350EB1513A6CCF089A53A7BCB04A4F02C4AEC7AFF444DA010FE`

## Verification

- Release build succeeded. Runtime dependency validation passed for 86 PE files.
- Five focused CTests passed: controller, EN/RU command menus, track presentation, and track/mixer synchronization. The latter now also runs track selection and inline rename checks.
- Track presentation: 48 CPU checks passed. The staged executable passed 49 GPU checks at each of 100%, 150% and 200% scaling, using its deployed Qt runtime. GPU checks deliver pointer events through the actual Quick window and open the name's context menu.
- Dark and light layouts were visually inspected. Preview captures and logs are in `artifacts/track-redesign`.
- The broader GPU `--uiperfcheck` passed 66/67 checks; `keyboard zoom follows the live pointer over the timeline` failed on both runs. The full `--selftest` stopped at `context panel level did not reach the mixer`. These broader failures remain unresolved; this report does not claim a fully passing suite.

Evidence: [focused CTest](../../../artifacts/track-redesign/ctest.log), [CPU](../../../artifacts/track-redesign/cpu.log), [staged GPU 100%](../../../artifacts/track-redesign/stage-gpu1.log), [150%](../../../artifacts/track-redesign/stage-gpu1.5.log), [200%](../../../artifacts/track-redesign/stage-gpu2.log), [runtime dependencies](../../../artifacts/track-redesign/stage-runtime.log), [broader GPU check](../../../artifacts/track-redesign/stage-perf-recheck.log), [full selftest](../../../artifacts/track-redesign/selftest.log).
