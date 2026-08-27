---
id: vocal-processing
title: Vocal processing
use_when: processing, mixing or tuning a vocal — lead vocal, backing vocals, rap, ad-libs
tags: [vocal, voice, vox, autotune, deesser, rap]
---
Measure first: analyze_track on the vocal channel and on the master. A vocal that peaks at -0.2 dB has already been recorded too hot and everything you add will make it worse — bring the channel down before you start.

THE CHAIN, IN THIS ORDER
Order is not a preference here; each stage depends on the one before it. Use add_insert in this sequence, then set the parameters with list_plugin_parameters + set_insert_parameters.

1. GATE or manual cleanup — remove the room between phrases. Skip on a quiet recording.
2. SUBTRACTIVE EQ — high-pass at 80–100 Hz (male) or 100–120 Hz (female). Cut the boxiness around 250–400 Hz if lowShare is high. Narrow cuts only.
3. COMPRESSOR #1 — moderate: ratio 3:1, 3–6 dB of reduction, medium attack so the consonants survive. Evens the performance.
4. DE-ESSER — after compression, because compression brings the sibilance up. Target 5–8 kHz.
5. COMPRESSOR #2 — fast, ratio 4:1, another 2–4 dB. Two gentle stages sound better than one heavy one, and this is what makes a vocal sit forward without pumping.
6. ADDITIVE EQ — air above 10 kHz, presence around 3 kHz if it is still buried. Wide, gentle boosts.
7. SATURATION — optional, brings the vocal forward without raising its level.
8. SPACE BY SEND, not insert — add_send to a reverb bus and to a delay bus. A vocal reverb insert kills the dryness that keeps it intelligible. Short plate for pop, longer hall for ballads, slapback or eighth-note delay for rap.

TUNING
If the user asks for tuning or "autotune", find a pitch correction plugin with list_plugins. Set the key from the project (get_project gives it, or analyze_harmony finds it) — a corrector on the wrong key makes it worse. Hard retune speed for the modern effect, slow for transparent correction.

RAP AND AD-LIBS
- Lead centred, dry-ish, forward. Doubles panned ±0.4 at 6–10 dB below the lead.
- Ad-libs panned wide, more reverb and delay than the lead, and low enough to be an answer rather than a competitor.
- Ducking the music under the vocal is done with automation on the music bus — see the automation playbook.

BACKING VOCALS
Three or more takes, panned across the field, EQ'd darker than the lead (cut above 8 kHz), compressed harder, and always lower than you think. They are texture; the lead carries the words.

FINISH
analyze_track the vocal channel again. It should sit 3–6 dB above the instrument bus in RMS and no longer clip. Say what you changed and why in one sentence.
