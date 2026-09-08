# Musical analysis version 2

The implementation retains `analyzeAudioFile` and `analyzeAudioSamples`. It runs
locally on CPU, outside the audio callback. No Python, credentials, downloads or
network connection are needed in the installed application.

## Runtime

- Preparation: two-pass, bounded source decoding; one stable stereo mix selected
  by AC energy for the requested range, DC removal, globally phased resampling
  to 22,050 Hz. Fully opposite-polarity channels select the strongest channel instead of
  cancelling the analysis signal.
- Tempo: five onset features, percentile normalization, interpolated
  autocorrelation candidates, and robust phase/period grid fitting. Separate low
  frequency attacks and accent strength check subdivision candidates. Beat This!
  `final0` supplies beat/downbeat activations; inference uses the author's
  overlapping 1,500-frame windows and keeps original frame positions. Local
  16-second grids measure consistency and distinguish octave ambiguity from
  tempo changes. Filenames and unverified loop lengths provide no tempo prior.
- Key: harmonic/percussive median masks, interpolated spectral peaks, circular
  tuning correction, 36-bin HPCP, and published Krumhansl–Kessler/Temperley
  profiles. S-KEY analyzes overlapping 15-second windows; inputs below three
  seconds use HPCP. Three seconds is above the network's 128-frame downsampling
  limit and is included in ONNX parity tests. A single bass note, silence and
  noise cannot authorize a key. Window disagreement and distant tonal centers
  produce ambiguity rather than confident automatic application.
- Results preserve fractional BPM. `roundedBpm` is the shared nearest-integer
  policy for display and application; alternatives are deduplicated after
  rounding. Stretch and pitch transformations precede output construction.
- Each field stores its algorithm version, backend, reason and calibration
  provenance. Old project values remain intact. Reanalyzing one field preserves
  the other field's version and result. No existing project is rewritten on load.

`DetectionStatus::Available` means that the detector found usable evidence, not
that an uncalibrated numeric score is a probability. The UI displays “confident”
only through `highConfidence()`. This requires a validated calibration, a stable
unambiguous result, and at least 0.98/0.95 estimated precision for tempo/key.
Without that validation the result remains a manual suggestion. Missing or
unloadable models select DSP/HPCP, with separate calibration entries.

## Models and reproducible builds

`models/audio-analysis/manifest.json` pins the exported assets, upstream
checkpoints, preprocessing contracts, class mapping and parity measurements.
S-KEY's major classes start at A and its minor classes start at B; the host
explicitly converts both to C-based ordering. Its output already contains
probabilities and is not softmaxed again.

The models are bundled with the app by `cmake/AudioAnalysis.cmake`. CMake checks
their SHA-256 digests before packaging. It uses ONNX Runtime 1.22.0 on CPU and
fetches a pinned SDK for macOS arm64/universal, Windows x64 or Linux x64 when a
local SDK is not supplied. Other platforms can supply `DAW_ONNXRUNTIME_ROOT`.
`DAW_ENABLE_NEURAL_ANALYSIS=OFF` builds the independent DSP backend.

Developer-only regeneration, using Python 3.12:

```sh
python3.12 -m venv .cache/audio-analysis/venv
.cache/audio-analysis/venv/bin/pip install -r tools/audio-analysis/requirements.txt
.cache/audio-analysis/venv/bin/python tools/audio-analysis/fetch_models.py
.cache/audio-analysis/venv/bin/python tools/audio-analysis/export_models.py
cmake --build build --target audio_musical_analysis_test audio_analysis_contract_test audio_analysis_bench daw
```

ONNX export is checked against a freshly loaded, untouched upstream model at
3/6/10/15/31 seconds for S-KEY and 100/777/1,500 frames for Beat This!. Native
log-mel parity is tested separately, including reflection at the signal edges.
The test waveform is deterministic synthetic noise, not copyrighted audio.

Runtime asset discovery checks `VLT_AUDIO_ANALYSIS_MODELS` (an explicit developer
override), then assets beside the executable and macOS Resources. Development
builds also allow the source asset directory. Model licenses and ONNX Runtime's
third-party notices accompany the assets.

## Corpus and release protocol

`tests/analysis_v1` freezes the original implementation. The benchmark can run
`legacy`, `hybrid`, or `dsp` against identical TSV rows and writes complete JSONL
predictions, including failures and unrounded BPM. Alternatives never count as a
correct primary prediction. A decoder failure remains in the denominator.

`fsld-v2.tsv` freezes 400 human-annotated FSLD examples, audio hashes and individual
CC0/CC-BY licenses. Discarded/deferred annotations and contradictory annotators
are excluded. Only human BPM/key/mode labels are used; filename and automatic
metadata labels are not targets. All files from one uploader share a partition
(201 fit / 72 calibration / 127 test), before any derived variants. This is more
conservative than splitting individual loops. Audio stays in `.cache` and is
not redistributed with the application.

The published Beat This! training datasets do not list FSLD or GiantSteps; the
GiantSteps fetcher also checks published training identifiers. These checks do
not prove absence of sampled/reuploaded recordings or overlap with S-KEY's
unpublished Deezer training catalog. The report must retain that limitation.

GiantSteps uses the authors' current official backup and verifies original MD5
checksums. It is a separate test of 40 two-minute excerpts, never a source of
fusion weights or calibration parameters. These are excerpts, not complete
songs; full-track accuracy requires further annotated full-length material.

Typical evaluation commands (set `VLT_AUDIO_ANALYSIS_MODELS` to the source asset
directory when iterating, so an older build-directory copy is not selected):

```sh
build/bin/audio_analysis_bench manifest.tsv --backend legacy --output legacy.jsonl
build/bin/audio_analysis_bench manifest.tsv --backend hybrid --split fit --output fit.jsonl
build/bin/audio_analysis_bench manifest.tsv --backend hybrid --split calibration --output calibration-hybrid.jsonl
build/bin/audio_analysis_bench manifest.tsv --backend dsp --split calibration --output calibration-dsp.jsonl
python3 tools/audio-analysis/calibrate.py --predictions fit.jsonl calibration-hybrid.jsonl calibration-dsp.jsonl --fit-fusion --output calibration-draft.json
```

Fit fusion weights on `fit` first, then rerun calibration with those weights.
Freeze both before running `test`. `evaluate.py` reports rounded exact accuracy,
absolute BPM error, half/double errors, exact tonic+mode accuracy, confident
coverage/precision, and comparisons at the same coverage. `release_gate.py`
checks held-out group separation and both primary improvements. It requires the
98%/95% goals and at least 20 confident test examples before enabling a backend;
95% Wilson intervals are also published. Insufficient coverage is a failed
gate, never “100% accuracy”. A failed gate leaves automatic application disabled.

The numeric targets are release criteria, not guarantees or observed accuracy.
See `quality-report.json` and `QUALITY.md` for the actual run and limitations.
`run-provenance.json` records source, model, corpus and prediction hashes.
The corpus has not met the gate; this implementation is a manual suggestion
backend, not a validated automatic detector.

## Sources

- [Beat This! code, checkpoints and MIT terms](https://github.com/CPJKU/beat_this)
- [Published Beat This! training annotations](https://github.com/CPJKU/beat_this_annotations/tree/v1.0)
- [S-KEY implementation and checkpoint](https://github.com/deezer/skey/tree/918b83d273568d5041569bb8068843d19a335726)
- [S-KEY research and evaluation protocol](https://arxiv.org/html/2501.12907v2)
- [HPCP and tonal profiles](https://essentia.upf.edu/tutorial_tonal_hpcpkeyscale.html)
- [Freesound Loop Dataset and human annotations](https://zenodo.org/records/3967852)
- [GiantSteps key annotations](https://github.com/GiantSteps/giantsteps-key-dataset)
- [GiantSteps tempo annotations v2](https://github.com/GiantSteps/giantsteps-tempo-dataset)

## Collaboration rollout

The Go payload validator and the shared command schema accept optional per-field
analysis metadata and fractional transformed values. Deploy the compatible server
validator before distributing clients that send these fields; older servers
reject unknown properties. Old command payloads remain accepted. No backend
deployment is performed by the analysis build.

## Synthetic stress corpus

```sh
python3 tools/audio-analysis/generate_regressions.py
build/bin/audio_analysis_bench .cache/audio-analysis/regressions/manifest.tsv --backend hybrid --output regression.jsonl
python3 tools/audio-analysis/evaluate.py regression.jsonl
```

This corpus records difficult patterns and known failures, including a trap
metrical-level error. It is not used for release calibration.
