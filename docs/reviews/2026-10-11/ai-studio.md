# Main DAW AI assistant

The main application's Qt assistant now has a compact studio header, conversation menu, explicit Auto / Ask / Learn / Do / Compose modes, and four draft starters. It follows the application theme, has Russian translations, and keeps the selected track visible beside the composer. Starters fill a draft; they do not execute project edits. Explicit Ask and Learn modes are enforced by the session's tool permissions.

## Project control

- `create_pattern`, `add_pattern_lane`, `get_sequence` and `set_sequence` let the assistant create actual Pattern lanes and edit the Sequence grid. Both views retain the existing shared MIDI storage. Edits preserve other pitches and hidden source material, reject invalid batches before mutation, and respect trimmed source offsets. Sequence view is restored by Redo for AI edits; ordinary manual view navigation still stays out of history.
- Plugin parameters retain their exact host IDs and plain units, with formatted values and labels for small stepped controls. Instruments, channel/master inserts, sampler FX and audio-clip FX use the same parameter API. The project snapshot now exposes the local chains too.
- `get_plugin_state` and `configure_plugin` expose wet/dry, supported channel modes, sidechain sources and insert order. `add_insert` can target channel, clip or sampler FX; removal and reordering resolve the correct chain.
- Mixing tools support main output routing and pre/post-fader and enabled send states. Existing send levels survive flag-only edits. Send settings and Sequence creation survive Undo/Redo.
- `focus_in_app` selects actual tracks/clips or opens their plugin, Pattern, piano-roll, sample or automation editor. It complements the existing command catalog; live UI context reports selection and open editors.

## Production instructions

The main prompt and drum, mixing, vocal and tool-reference playbooks were updated. Drums prefer the step sequencer, distinguish GM kits from single-sample instruments and avoid a duplicate piano-roll part. Processing uses installed plugin discovery and actual parameter metadata. Vocal guidance covers gain rides, peak control and levelling, de-essing, parallel compression, tonal processing, effect sends and tempo-related delays, with settings treated as starting points for the material.

The vocal guide paraphrases primary practitioner material:

- [Chris Lord-Alge's plugin demonstration](https://www.waves.com/chris-lord-alge-mixing-with-all-cla-plugins) and [depth lesson](https://www.waves.com/mixing-with-depth-chris-lord-alge-ep-3-the-sound).
- [Andrew Scheps on parallel compression and serving the song](https://www.waves.com/andrew-scheps-on-serving-the-song).
- [Universal Audio's collection of vocal chains](https://www.uaudio.com/blogs/ua/top-uad-vocal-chains).

Sources were checked on 2026-10-11. These are instruction and reference updates, not model fine-tuning. C++ and backend Go prompt bundles are generated from the same Markdown sources. Relevant downloaded playbooks also receive the current host addendum, so an older server pack cannot hide the new workflow.

## Verification and limits

`ai_studio_test` exercises real controller mutations, sample loading, Sequence cells and trimming, protected modes, local effect chains, routing and send history. A compiled VST3 fixture verifies discovery, every exposed parameter's plain-unit round trip, wet/dry, processing order, channel support and actual sidechain connections. The existing AI tools, musical context, content catalog and composition tests are also run. The generated prompt check passes; the backend prompt package compiles (it has no Go test cases).

All five CTest suites passed. The focused desktop AI selftest covers real input routing, mode selection, draft-only starters, local HTTP retry/cancellation fixtures, attachments, copying, Find, streaming scroll anchors and a scripted request with Undo/Redo. CPU at 240 px and GPU at 150% scale pass in Russian without critical Qt rendering warnings. Inspected CPU screenshots of the dark, light/narrow and populated states are saved under `artifacts/ai-studio`; the hidden-window GPU screenshot returned an empty frame, so it is not used as visual evidence. The focused AI entry point now runs independently of unrelated collaboration checks; a broader selftest attempt stopped at the collaboration sampler fixture before reaching AI.

The packaged `build-windows/stage/bin/VLTONE.exe` passes the GPU/150% focused AI check with a clean runtime PATH and matches the built executable (SHA-256 `249BEC654F50092771BB11BEFC6CF3EBA145E6A938920C0E198460744B512923`). Qt deployment completed; its optional DX compiler/Visual Studio discovery warnings did not prevent the packaged runtime check.

No live external LLM request or commercial plugin session was used. The tests prove host/tool behavior, not the musical judgement of every provider or compatibility with every third-party plugin. Vendor-private GUI controls that are not exposed as host parameters remain unavailable; the assistant is instructed to report that limit. Audio metrics do not give the text model human listening ability. Existing program commands remain subject to their availability and interaction-mode permissions.
