# Quality evaluation — 2026-09-08

**The implementation does not pass the proposed release criteria.** Both primary
answers improve on the held-out loop sample, but 98% BPM / 95% key precision has
not been established at any calibrated confident coverage. Every shipped backend
has `validated: false`; suggestions require a manual choice. No threshold was
lowered after seeing the test results.

## Frozen real-audio comparison

FSLD: 400 selected, human-annotated files, grouped by uploader before splitting:
201 fit / 72 calibration / 127 held-out test. The test partition contains 121
known tempos, 67 known keys, and 59 explicitly absent keys. Missing labels are
excluded only for their corresponding task. One file in the calibration partition
could not be decoded by any backend; it remains in the corpus. There were no test
file decoding failures. Byte-identical audio does not cross partitions.

The table counts only the primary answer. BPM must round to the same integer as
the annotation; key must match both tonic and major/minor mode. Abstentions count
as incorrect on labeled tonal/rhythmic examples. Alternatives earn no credit.

| Held-out FSLD metric | Original v1 | Hybrid v2 | DSP/HPCP v2 |
|---|---:|---:|---:|
| Correct primary BPM | 35/121 (28.9%) | 48/121 (39.7%) | 31/121 (25.6%) |
| Correct primary key | 30/67 (44.8%) | 37/67 (55.2%) | 34/67 (50.7%) |
| BPM available, including ambiguous | 96.7% | 96.7% | 96.7% |
| Key available, including ambiguous | 100% | 94.0% | 94.0% |
| Mean absolute BPM error on returned answers | 27.50 | 45.32 | 56.72 |
| Half-tempo errors (1% tolerance) | 6 | 19 | 22 |
| Double-tempo errors (1% tolerance) | 10 | 16 | 21 |
| Correct BPM only as an alternative | 13 | 21 | 39 |
| Correct key only as an alternative | 12 | 10 | 10 |
| Keys incorrectly returned on 59 absent-key examples | 59 | 41 | 41 |

The increase in exact BPM matches coexists with a worse mean error and more
metrical-level mistakes. The DSP-only tempo primary answer regresses. Although
false key assignments decrease, 41/59 remains unacceptable for a strong quality
claim. These are outstanding defects, not successful release checks.

## Confidence and equal coverage

The fitted monotonic calibration never reaches either requested precision target.
Validated confident coverage is **0%** for every new backend; confident precision
is undefined, not 100%. The release gate also requires at least 20 confident test
examples and 40 calibration observations per backend. Zero coverage fails it.
See [the machine-readable gate](quality-report.json) for counts and Wilson 95%
intervals. Fusion weight 0.35 was selected on the fit partition's 58 examples with
both model and profile scores, then fixed before calibration and test.

For comparison, the old heuristics labeled 89/121 BPM answers confident, but only
31/89 were correct (34.8%). For known keys, 26/67 were confident and 18/26 correct
(69.2%); it also confidently assigned keys to 10 absent-key examples.

Ranking by each method's original confidence/evidence, at the **same retained
fraction** of all labeled examples:

| Retained fraction | BPM v1 → v2 | Key v1 → v2 |
|---|---:|---:|
| 10% | 53.8% → 84.6% | 85.7% → 71.4% |
| 25% | 54.8% → 74.2% | 76.5% → 82.4% |
| 50% | 41.0% → 54.1% | 55.9% → 70.6% |
| 75% | 35.2% → 48.4% | 49.0% → 60.8% |
| 100% | 28.9% → 39.7% | 44.8% → 55.2% |

These are descriptive ranking measurements, not additional calibrated confidence
bands. At 10% there are only 13 BPM and seven key examples.

## Separate GiantSteps test

Forty fixed two-minute excerpts with original audio MD5 verification, using
published v2 tempo and original key annotations. No GiantSteps prediction or label
was used to fit fusion or confidence.

| GiantSteps metric | Original v1 | Hybrid v2 |
|---|---:|---:|
| Correct rounded primary BPM | 8/40 (20.0%) | 7/40 (17.5%) |
| Correct primary key | 15/40 (37.5%) | 23/40 (57.5%) |
| Mean absolute BPM error | 25.61 | 18.29 |
| Half / double errors | 7 / 1 | 5 / 1 |
| Returned key coverage | 100% | 95% |

The strict BPM criterion regresses here. For transparency, several predictions
are near a neighboring integer rather than the v2 label (e.g. 175.002 versus
174). The published annotation versions also disagree on some files. Labels were
not edited, substituted with v1 labels, or inferred from the new analyzer. All
strict misses remain errors. This sample does not establish full-length-song
accuracy. [Full metrics and equal-coverage comparison](giantsteps-report.json).
The source of the v2 labels is the authors' [crowdsourced tempo
experiment](https://zenodo.org/records/1492437); both annotation versions are
retained in the [official repository](https://github.com/GiantSteps/giantsteps-tempo-dataset).

## Regression and integration checks

Eight CTest targets pass: musical analysis, input/inference contracts, missing
model fallback, controller, shared mutation routing, collaboration wire protocol,
project templates/Quick Import, and project music context. The DSP-only build also
passes all three analysis targets. The full Go collaboration test package passes,
including old/new metadata and invalid payload checks. The application builds on
macOS arm64.

- The original 128 → 129.2 and 174 → 172.3 synthetic errors are fixed. Tested
  primary tempos 72/92/110/128/140/174 are within 0.1 BPM before rounding.
- Exact integer display/application, alternative deduplication, fractional
  storage, stretch and pitch transformation are covered. Quick Import preserves
  measured 127.86 while applying 128, and cannot apply uncalibrated high scores.
- Old project and collaboration records load with optional metadata absent;
  independent tempo/key versions survive round trips. Unknown protocol fields
  remain rejected. AI context preserves each field's version/calibration state.
- Native log-mel versus Torch maximum absolute error: 0.000000715. S-KEY and Beat
  This! ONNX parity measurements, input lengths and model hashes are in
  [the asset manifest](../../models/audio-analysis/manifest.json).
- Runtime calibration rejects a different model hash and non-monotonic knots;
  valid knots match the offline step-function policy, including evidence below
  the first knot.
- Silence, noise, a bass note with eight harmonics, percussion-only material,
  opposite-polarity stereo with DC offset, source offsets at 22.05/44.1/48 kHz, decoder block
  boundaries, non-finite input, and cancellation during native/ONNX work are
  covered. Native progress is monotonic in the contract test.

The extra deterministic stress corpus is generated by
`tools/audio-analysis/generate_regressions.py` and excluded from fit/calibration.
Its observations are stored alongside the real-corpus results:

| Synthetic stress case | Expected | Hybrid observation |
|---|---|---|
| Swing | 120 BPM | 120.013 |
| Syncopated drums | 110 BPM | 109.977 |
| Dense quiet hats | 140 BPM | 140.000 |
| Two-second loop | 128 BPM | 128.000 |
| Arbitrary 4.174-second crop | 128 BPM | 127.992 |
| Opposite-polarity stereo | 128 BPM | 128.003 |
| Detuned chord progression | C major, +35 cents | C major, +35.115 cents |
| Tempo / key changes | flag changing content | both flagged in respective cases |
| Trap with dense subdivisions | 70 BPM | **279.993; 70 only an alternative** |

The trap case is an unresolved primary-answer failure. A sustained bass note's
key is correctly unavailable, but the tempo branch can still mistake its periodic
waveform for rhythm; that is another limitation. Synthetic successes are not
estimates of accuracy on real music.

## Reproduction, costs and limitations

Model weights, licenses, frontend data and frozen calibration are copied with the
app. Their copied hashes were checked. A macOS `.app` was built and installed
locally; both native test suites passed using its model directory and bundled
ONNX Runtime dylib. The loader trace confirmed that the installed dylib was used,
and the weights are present only once in Resources. Python is only needed for developer model
export/corpus tooling. Neural inference uses CPU, with two intra-op threads and
no spinning; missing models and an explicit DSP-only build remain functional.
The local Homebrew Qt deployment needed an extra framework search path for its
automatically included virtual-keyboard plugin; CMake now derives that path from
the optional Qt target. The repaired deployment has no unresolved dependencies
reported by macdeployqt. Windows/Linux packaging rules are present but were not
executed on those hosts.

Observed median per-file wall time on this macOS arm64 run: FSLD 0.065 s old,
1.196 s hybrid, 0.375 s DSP; GiantSteps 2.619 s old, 30.379 s hybrid. Jobs partly
ran concurrently, so these are run diagnostics, not controlled latency benchmarks.
Long files are decoded and inferred in overlapping blocks, but the complete
22.05 kHz mono buffer and feature sequences remain in memory. Memory therefore
still grows with recording duration; this is not a constant-memory streaming API.

Portable corpus identities, hashes, labels and permissions are frozen in
[fsld-v2.tsv](fsld-v2.tsv) and [giantsteps-v2.tsv](giantsteps-v2.tsv). Per-file
predictions are in [results](results/), with basenames replacing machine-specific
paths. `evaluate.py` can reproduce each report directly from these JSONL files.
The developer fetchers rebuild local audio manifests; audio is not redistributed.
See [README](README.md) for build, export and calibration commands.

FSLD annotations are from Antonio Ramires, Frederic Font, Dmitry Bogdanov, Jordan
B. L. Smith, Yi-Hsuan Yang, Joann Ching, Bo-Yu Chen, Yueh-Kao Wu, Hsu Wei-Han and
Xavier Serra, [Freesound Loop Dataset](https://zenodo.org/records/3967852), licensed
CC BY 4.0. Individual selected sounds use the CC0/CC-BY licenses listed in the
manifest; uploader attribution and Freesound IDs are retained. GiantSteps audio
remains research-only local evaluation material; its original authors and
annotation publications are linked in the README.

Only published training dataset names/identifiers were checked. Unknown sampled
or reuploaded overlap and overlap with S-KEY's unpublished training catalog cannot
be excluded. Human tempo annotations can also express different metrical levels.
These limitations and the small test sample prevent the requested reliability
claim. Further work needs better metrical-level selection, reliable rejection of
non-tonal material, and a new untouched test partition after any new tuning.
