---
id: bass
title: Bass
use_when: writing, replacing or fixing a bass part — bass guitar, sub, 808, synth bass
tags: [bass, 808, sub, low end]
---
THE RULE THAT MATTERS MOST
A bass part follows the ROOT NOTE of the harmony that is already playing. Before you write a single note, call inspect_music_context when available, or analyze_harmony on the clip the user means. `analyze_harmony` gives one segment per chord with `root`, `rootPitchClass` and `suggestedBassPitch`. Those roots are the skeleton of the part. If analysis finds nothing — an empty project, or only drums — then you are writing the harmony yourself, and you say which key you chose.

Every chord segment gets its root on its own downbeat. Anything else you add hangs off that.

Use compose_candidates with role bass so those chord roots are copied into the validated request and each alternative stays monophonic. Prefer the candidate with the best harmony/rhythm balance, apply it by candidateId, then revise only if the user's style calls for a different groove.

RANGE
- Write between MIDI 28 and 55 (E1 to G3 in this program's labels: E2–G4 as shown in the piano roll, since 60 is labelled C5).
- The fundamental of a mix lives around 40–55; below 33 is sub territory that only survives on good speakers.
- Never double the melody an octave down and call it a bass. Two parts moving in parallel is not a bass line.

MONOPHONIC
One note at a time. No overlapping notes, no chords, no octave stacks unless the user asked for an octave bass. Overlapping bass notes turn to mud on any real system, and most bass synth patches are monophonic anyway. If you need weight, use a lower octave or an insert, not a second simultaneous note.

RHYTHM
- If drums exist, call get_clip_notes on the drum clip and put the bass where the kick is (GM kick is pitch 36). Locking the two is what makes a groove feel solid. Landing bass notes between kicks is a deliberate style choice, not a default.
- If there are no drums, put the root on beat 1 of each chord and let the rest follow the style: straight eighths for house and rock, syncopated sixteenths with long slides for trap and drill, walking quarters for jazz and blues.
- Length: leave a small gap before the next note (a note of 0.9 beats in a one-beat slot). Bass notes that touch each other smear.

WHAT TO PUT BETWEEN THE ROOTS
- The fifth, the octave and the flat seventh above the root are always safe.
- Passing notes belong to the project's scale — use transform_notes with snap_to_scale if you are unsure.
- Approach the next chord from a semitone or a scale step below on the last eighth of the bar. This is the single most effective thing you can add to a plain root-note line.
- Do not run a scale up and down. A bass line is a rhythm with pitch, not a melody.

VELOCITY AND FEEL
- Roots on the downbeat 100–115, notes off the beat 70–90. Ghost notes 40–55.
- After writing: transform_notes quantize at 0.85, then humanize at 0.3. Anything more than that and it stops locking with the drums.

SOUND
- Ask list_plugins for an instrument. If nothing suitable is installed, load_sampler with a bass sample, or the built-in sampler with a sine-ish patch.
- Typical chain: EQ with a high-pass around 30 Hz to clear the rumble, then a compressor with a slow attack so the note starts before it is squashed. No reverb on the bass — send it to a bus if the user asks for space.

CHECK
After writing, call analyze_harmony again on the bass clip. The root of every segment should match the roots you were given. If it does not, you wrote the wrong notes; fix them rather than explaining them.
