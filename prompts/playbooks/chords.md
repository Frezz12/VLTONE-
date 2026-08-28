---
id: chords
title: Chords and progressions
use_when: writing a chord progression, a pad part, comping, or reharmonising
tags: [chords, harmony, progression, pads, comping]
---
BEFORE YOU WRITE
If the project already has material, call inspect_music_context when available, otherwise analyze_harmony. Adding a progression that fights the melody already there is the most common way this goes wrong. If the project is empty, choose a key, write the progression, and record the key with set_project_key so everything after it agrees.

Use compose_candidates with role chords for the first draft. Its stored harmony and voice-leading scores catch the two failures that matter most here: wrong chord tones and block voicings that jump. Apply by candidateId, then strum or edit only for the requested playing style.

PROGRESSIONS THAT WORK
Written as scale degrees; convert to the project's key.
- i–VI–III–VII (minor, the default for most modern pop and EDM)
- vi–IV–I–V (major, the "four chords" progression)
- I–V–vi–IV (major, brighter)
- ii–V–I (jazz; each chord takes a seventh)
- i–iv–v (minor blues and rock)
- I–IV (two chords, for house and anything that has to loop for eight bars)

One chord per bar is the default. Two chords per bar doubles the sense of movement and halves the room for a melody.

VOICING — WHAT SEPARATES A REAL PART FROM A TYPED ONE
- Do not stack every triad in root position in the same octave. Invert them so the top note moves as little as possible between changes: that is voice leading, and it is the whole difference.
- Spread the notes over an octave or more. A close triad below MIDI 55 turns to mud; keep chord tones above 55 and let the bass own what is below.
- Leave the root to the bass when a bass part exists. The chord instrument does not need to double it.
- Add the seventh or the ninth to any chord that sounds plain. On a minor chord the seventh is almost always right.
- On a pad, hold notes across the whole bar. On a comp, play short stabs off the beat and leave the downbeat empty.

WRITING IT
- Write the notes yourself when the voicing matters. Use transform_notes build_chords only when you already have a melody line and want a chord under every note of it.
- transform_notes strum with 0.02–0.05 beats turns a block chord into something played by hands. Use it on guitar and harp; not on a pad.
- Velocity: the top note of a voicing 5–10 higher than the inner notes so the melody of the chords is audible.

CHECK
Call analyze_harmony on what you wrote. The roots it reports should be the progression you intended. If it reports something else, your voicing is ambiguous — put the intended root at the bottom of the voicing or leave it to the bass.
