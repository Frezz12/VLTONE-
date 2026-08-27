---
id: mastering
title: Mastering and loudness
use_when: mastering, making it louder, final polish, preparing to export or release
tags: [master, mastering, loudness, limiter, export]
---
FIRST, CHECK THE MIX IS READY
Call analyze_mix. Mastering cannot fix a mix.
- master peakDb should be between -6 and -3 before you start. Higher and there is nothing to work with — go back and lower the channels.
- If any single channel is clipping, fix that first.
- If lowShare on the master is above 0.55, the mix is bass-heavy; fix it in the mix, not on the master.
Say so plainly if the mix is not ready, then fix the mix.

THE MASTER CHAIN
Everything on the master channel, in this order, all subtle:
1. EQ — gentle, wide, never more than 2 dB. A high-pass at 20–30 Hz to remove what nobody can hear but every limiter reacts to.
2. COMPRESSION — 1.5:1 to 2:1, 1–2 dB of reduction, slow attack, release timed to the tempo. This is glue, not level.
3. SATURATION — optional, a trace, for density.
4. LIMITER — last, always last. Ceiling at -1.0 dBTP for streaming, -0.3 for a file the user is only going to listen to.

LOUDNESS
- Streaming targets about -14 LUFS integrated; the platforms turn anything louder back down, so a master crushed to -8 just sounds squashed at the same volume.
- Aim for 3–6 dB of limiting at the loudest section, not 12.
- If the user asks for "as loud as possible", say what it costs (the transients and the dynamics) and give them a -9 to -10 LUFS master rather than refusing.

CHECK
analyze_mix again on the master: peak should be at the ceiling you set, and no clippedSamples. Compare lowShare/midShare/highShare before and after — mastering should not have changed the balance much. If it did, the limiter is doing too much.

EXPORT
Only when the user asks. export_audio with 24-bit WAV for a master, 16-bit 44.1 kHz WAV for CD, MP3 only for a rough. Confirm the path before overwriting anything.
