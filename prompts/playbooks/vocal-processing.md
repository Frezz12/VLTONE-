---
id: vocal-processing
title: Vocal processing
use_when: processing, mixing or tuning a vocal — lead vocal, backing vocals, rap, ad-libs
tags: [vocal, voice, vox, autotune, deesser, rap, effects]
---
START FROM THE RECORDING
Inspect the selected channel, its clips, existing plugins and sends. Measure a representative verse/chorus and the master with analyze_track/analyze_mix. These tools measure audio; you do not hear it. A high peak is not proof of recorded clipping, a high lowShare is not proof of mud, and no fixed vocal-to-instrument RMS difference makes every mix work. Do not claim sibilance, resonance, gain reduction or improved intelligibility unless the available evidence supports it.

Ask about style only when it changes the task materially; otherwise state a sensible first-pass aim. Preserve takes, timing, key and current processing unless changes are requested. Use clip gain/automation for large phrase differences and input headroom: lowering the post-insert fader does not fix overloaded plugins upstream.

CHOOSE TOOLS THAT ACTUALLY EXIST
Use list_plugins kind=effect to find suitable installed third-party plugins before defaulting to built-ins. Reuse an existing EQ/compressor rather than stacking duplicates. For every loaded plugin, list_plugin_parameters with nameContains and pagination; inspect units, current display and choices. Batch related settings and verify returned values. configure_plugin controls host mix, order, channel mode and sidechain. Set thresholds against input level; numbers below are starting ranges, not measurements or promises of gain reduction.

A FLEXIBLE FIRST PASS
1. Cleanup/gain rides first if needed. Keep breaths and word endings. Use edits/fades for isolated noises; a gate is optional and can damage a quiet performance.
2. Corrective EQ only where justified. Start a high-pass below the useful fundamental, then adjust for the actual voice and proximity effect, not the singer's gender. Broad analysis cannot locate a precise narrow resonance. Avoid automatic severe cuts or air boosts.
3. Compression for peak control and/or levelling. One stage may suffice; for a denser pop/rap vocal try a fast FET stage followed by a slower optical stage. Moderate 2:1–4:1 on a suitable compressor is a starting point. Attack/release depend on consonants, tempo and the model; a 1176's numbered controls are not milliseconds. Gain-match with output trim.
4. De-ess where needed before compression, after it, or lightly in both places. Search for the actual troublesome band (often around 4–10 kHz), keep articulation, and avoid lisping. A broadband treble cut is not selective de-essing. Ask for listening feedback if detection/metering is unavailable.
5. Optional tonal EQ/saturation for presence or body. Start subtle, compensate output gain and check bypass at similar level. Do not add distortion just because a famous chain includes it.
6. Space on buses is a useful default: a short plate/room for depth, a tempo-related delay for movement. Use add_track kind=bus, add_insert, then mix set_send. Effect returns should normally be fully wet; check both the plugin's internal dry/wet and host mix. Preserve the dry lead. Filter returns when useful, automate delay throws at phrase endings, and keep feedback conservative. Quarter-note delay in milliseconds = 60000/BPM; eighth = half, dotted eighth = 0.75 times that. Plugin sync divisions must come from its own labels.

TECHNIQUES FROM PRACTITIONERS (PARAPHRASED; VERIFIED 2026-10-11)
- Chris Lord-Alge's Waves demonstration uses CLA MixHub, CLA-2A/CLA-3A, CLA Epic and additional vocal effects on a lead. His separate depth lesson demonstrates reverbs and delays on vocals and the mix. Adapt the idea of a controlled dry centre plus deliberate depth; these examples do not establish a mandatory insert order or exact settings for this recording. Sources: https://www.waves.com/chris-lord-alge-mixing-with-all-cla-plugins and https://www.waves.com/mixing-with-depth-chris-lord-alge-ep-3-the-sound
- Andrew Scheps describes blending a strongly compressed parallel signal while retaining the uncompressed character. For this application, create a parallel bus, keep its processing wet, feed it with a send and blend cautiously; verify level and phase/latency behaviour. This is an adaptation of the principle, not a claim to reproduce his session. Source: https://www.waves.com/andrew-scheps-on-serving-the-song
- Universal Audio's producer collection includes 1176 into LA-2A, de-essing in multiple stages, and alternatives including Neve 1073 with Tube-Tech CL 1B, EMT 140, Capitol Chambers and delays. Use these as alternatives selected for the performance and installed inventory, not a shopping list. Source: https://www.uaudio.com/blogs/ua/top-uad-vocal-chains

TUNING, DOUBLES AND BACKGROUNDS
Tune only when requested. Confirm the intended key/scale and how audible correction should be; inferred key with low confidence is not enough for destructive pitch decisions. Find a real installed corrector and inspect its scale/retune controls.
Keep the lead intelligible. Real double takes can add width; duplicating an identical take is not the same performance. Ad-libs may carry more delay, filtering or saturation. Backgrounds can be darker and more compressed when appropriate; avoid a fixed high-cut, pan or track count for every song. Check mono compatibility and leave the lead's centre stable.

FINISH
Measure the same time range again, report factual peak/RMS/headroom changes and the processors/routing actually applied. Do not equate louder with better. If a plugin/control is missing or rejects a value, explain the limit and use an available equivalent with the same purpose. Offer a concrete listening comparison; musical edits remain undoable.
