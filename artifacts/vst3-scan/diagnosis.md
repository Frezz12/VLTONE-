# VST3 blacklist investigation — 2026-09-22

The installed VLTONE 0.2.5 Build 2 scanner was compared against a rebuilt scanner, using the actual installed plugins and the normal `--inspect` / `--validate` path. The live plugin cache was read without editing it.

## Findings and fixes

- **ZENOLOGY 2.0.2.20108:** the installed scanner crashed with `0xC0000005` (`-1073741819`) in the controller's `setComponentState`. The host called this before connecting the processor and controller through `IConnectionPoint`. Moving the initial state transfer after both connections fixes the crash. The instrument now initializes, activates, processes a MIDI note, reports its editor, and exits successfully. An old/new comparison took 15.98 / 17.83 seconds; another successful run took 25.52 seconds, within the existing 30-second scan deadline.
- **MiniFreak V 1.0.1.385:** its factory returned `E_NOINTERFACE` (`0x80004002`) when asked to create `IComponent`. A direct diagnostic confirmed that creating `IPluginBase`, then querying `IComponent`, works. The host now uses that fallback on `kNoInterface`. Full validation succeeds in about five seconds.
- **17 other Arturia instruments, including Pigments 4.1.1.3513:** current installed and rebuilt scanners both validated them successfully. Their saved blacklist entries had three failures, which normal incremental scans skipped indefinitely while file size, timestamp and schema remained unchanged. This establishes that the old blacklist was stale; it does not establish which older failure originally created those entries.
- Scan/cache compatibility version increased from 1 to 2. The next scan by the rebuilt application rechecks old entries, including entries with three failed attempts. This also causes a one-time rescan of previously successful entries. Search paths and settings are preserved.

The corrected connection/state order matches Steinberg's [Edit Controller Call Sequence](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Workflow%2BDiagrams/Edit%2BController%2BCall%2BSequence.html).

## Verification

- All 19 distinct blacklisted VST3 instruments in the tested Arturia/Roland set passed full scanner validation after the applicable fixes. See `comparison.json`; its MiniFreak row records the intermediate result before the additional factory fix. The final MiniFreak success is in `minifreak-final.json` and `minifreak-final.err`.
- The VST3 fixture now requires connections before the initial state transfer. The installed scanner aborts against this fixture; the corrected scanner exits with code 0.
- The fixture also has an `IPluginBase`-only factory mode, exercised by `plugin_contract_test`.
- A cache test verifies that a compatibility revision invalidates a blacklisted entry even after three attempts.
- Test results: `ctest-final.log`. Build results: `build-all.log`.

These are scanner and host contract checks, not exhaustive testing of every preset or editor interaction.

## Using the fix

The rebuilt application and its scanner are in `D:\Code\DAW\build-windows\bin`. The installation under `C:\Program Files\VLT Studio Pro` was not replaced, and the running application was not closed. Use the rebuilt application and run its plugin scan to refresh the old cache.
