---
id: mixing
title: Mixing
use_when: mixing, balancing levels, EQ, compression, panning, "make it sound better", fixing a muddy or quiet mix
tags: [mix, mixing, eq, compression, levels, balance, pan]
---
MEASURE FIRST. ALWAYS.
You cannot hear the project. Guessing at levels is the one thing that makes this feature useless. Call analyze_mix before you touch anything: it gives every channel and the master in one call — peak, RMS, headroom, and how the energy splits into low, mid and high.

Read the numbers like this:
- peakDb above -1 → clipping or about to. Bring it down.
- master peakDb above -6 with more parts still to come → no headroom left.
- One channel whose rmsDb is more than 6 dB above every other → it is the mix, and the rest are decoration.
- lowShare above 0.6 on anything that is not a kick or a bass → it is muddying the mix; high-pass it.
- highShare above 0.5 on a bass or a pad → wrong, something is thin or distorted.

ORDER OF WORK
1. LEVELS FIRST. Get the balance right with mix set_level before any effect. Most "it sounds bad" is a balance problem, and every effect you add first has to be redone afterwards.
   Rough starting point: drums peak around -6, bass -8, chords and pads -14, lead -10, vocal -6.
2. PANNING. Kick, bass, snare and lead vocal stay centred (pan 0). Everything else gets somewhere to sit: doubled guitars hard left and right, keys at -0.3, backing vocals at ±0.5. A mix where everything is centred cannot sound wide.
3. EQ, subtractive before additive. High-pass everything that is not the kick or the bass — 80–120 Hz on most things, 150–250 Hz on pads. Cut where two parts fight rather than boosting the one you want to hear.
4. DYNAMICS. Compression on what moves too much: vocals, bass, drum bus. Slow attack keeps the transient, fast release keeps it forward. Do not compress a pad.
5. SPACE LAST. One reverb on a bus, fed by add_send from several channels — not a reverb insert on every track. Different reverbs on every channel is what makes a mix sound like separate recordings.

DOING IT WITH THE TOOLS
- mix set_level / set_pan for balance. analyze_mix again after.
- list_plugins kind=effect to find an EQ or compressor, then add_insert, then list_plugin_parameters (uids and ranges differ per plugin — never guess), then set_insert_parameters.
- Reverb bus: add_track kind=bus, add_insert the reverb on it, add_send from each source channel.
- channel_strip copy/paste to give two similar channels the same treatment instead of rebuilding it.

FINISH
Call analyze_mix again and say what changed in numbers, not adjectives: "the master went from -0.4 to -6.2 dB peak and the bass no longer eats the kick". If a change made a number worse, undo that step rather than adding another effect to cover it.
