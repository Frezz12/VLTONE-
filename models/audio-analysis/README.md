# Bundled musical analysis assets

Beat This! `final0` and Deezer S-KEY are exported from their original checkpoints
for CPU-only ONNX Runtime inference. `manifest.json` records source identities,
SHA-256 digests, class order and export parity. `licenses/` contains distribution
notices. Python is used only by developer export/evaluation tools.

`calibration.json` contains separately validated tempo/key/backend calibrations.
An entry with `validated: false` cannot authorize automatic application, even if
its raw detector score is large. See `docs/audio-analysis/README.md` and the
quality report for exact behavior, reproduction commands and release criteria.
