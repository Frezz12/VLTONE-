# VLTONE 0.2.5 — release preparation, 2026-09-21

## Artifacts

- Installer: `build-windows/artifacts/VLTONE-0.2.5-x64-Setup.exe` (235,370,678 bytes).
- SHA-256: `c39c8f0a5e4d8329d75a0080820e3889bd65e3ae1aa4b932c29291bf2c3cdca8`.
- Portable archive: `build-windows/artifacts/VLTONE-0.2.5-windows-x64.zip`.
- Windows application and installer metadata: `0.2.5.0`.
- Built from the current working tree, including existing uncommitted changes, using the pinned Windows Release toolchain.
- Collaboration is disabled, matching 0.2.4. No production backend deployment or database migration was performed.
- Installer is unsigned; no signing certificate was supplied.
- Packaging used the existing build script with a temporary staging-path override to `build-windows/stage-0.2.5`, because the old `stage/bin` directory was locked. The temporary script is `.tmp/release-0.2.5/build.ps1`.

## Validation

All 92 registered CTest tests passed across the initial run and environment-specific retries:

- Initial run: 86/92 passed.
- Five failures passed with the Qt runtime on PATH and local socket access available: update checker, browser navigation, Quick navigation, browser proxy and account restoration.
- VLT Pitch UI failed after Chromium lost its D3D/Skia context; its unchanged test passed with `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`.
- Warp DSP/controller/UI/panel, MIDI recording/protocol/input UI, Modulation DSP/UI, waveform, transport scrubbing, mixer synchronization and audio timeline performance tests passed.
- Runtime dependency validation passed for all 86 PE files.
- Fresh screenshots were captured from the staged application and modulation UI test and visually inspected.

**Unresolved additional self-test:** the full deployed `--selftest --language en` from a directory containing Cyrillic and spaces exits 11 at `checkContextSyncForTest`: simulated context-panel level dragging leaves both document and mixer gain at 1.0. A repeat with the Widgets workspace also exits 11. This is separate from the 92 registered tests; the overall packaged-app self-test is not green. The Russian full self-test was not reached after the English failure. No application source or test thresholds were changed to hide this failure.

Packaging was completed with `-SkipTests` after the independent CTest run; this does not imply the additional self-test passed. No clean-machine installer installation or physical audio/MIDI hardware qualification was performed.

Logs: `.tmp/release-0.2.5/windows-package.log`, `ctest-recheck.log`, `ctest-pitch-software.log`, `installer-isolated.log`, `capture.log`, `selftest-en.stderr.log`, `selftest-widgets-en.stderr.log`.

## Saved release draft

- Admin: https://admin.vltstudio.ru/releases
- ID: `c321276d-6695-4c27-bf50-275a2c9b7d23`.
- Version: 0.2.5; status: draft; publication timestamp: null.
- Russian and English summaries, eight features, six changes and seven fixes per language.
- Five screenshots: Warp, Modulation Rack, expanded Filter & EQ, Piano Roll and Plugin Manager. Each has Russian and English captions; downloaded SHA-256 hashes matched server metadata.
- Installer artifacts: zero. The user will attach the installer.
- Public version endpoint returns 404 while the release remains a draft.
- Editable local template: `admin/release-template.json`.
- Server preparation copy: `/var/backups/vlt/release-0.2.5-preparation/`.

The release notes explain Warp support limits, the local project format update and possible sound changes in existing modulation presets. Existing published releases were preserved.
