---
id: piano
title: Piano and keys
use_when: writing piano, electric piano, Rhodes, organ or keyboard parts
tags: [piano, keys, rhodes, organ, epiano]
---
Read the chords playbook for the harmony; this is about making it sound like hands on a keyboard.

TWO HANDS
Write a piano part as two things at once, in the same clip:
- LEFT HAND, MIDI 36–55: the root, or root and fifth, or an octave. Long notes. If a bass part exists, keep the left hand above 48 or drop it entirely — two instruments on the same low note is mud.
- RIGHT HAND, MIDI 60–84: the voicing, or the melody, or a rhythmic comp.
The gap between the hands matters more than the notes: leave at least a fifth between the top of the left hand and the bottom of the right.

WHAT REAL PLAYING LOOKS LIKE
- Hands are not perfectly together. After writing, transform_notes humanize at 0.3–0.5. A piano quantized to the grid is instantly recognisable as programmed.
- Rolled chords: transform_notes strum with 0.02–0.04 beats, on ballads and anywhere the chord is exposed.
- The thumb notes are quieter than the outer notes. Set the top and bottom notes of a voicing 10–15 velocity above the inner ones.
- Sustain: overlap notes slightly across a chord change rather than cutting them. Note lengths of 1.05 beats in a one-beat slot read as pedal.

STYLES
- BALLAD: broken chords in the right hand, whole notes in the left.
- POP COMP: eighth-note chords on the off-beats, left hand on 1 and 3.
- HOUSE / DEEP HOUSE: stabs of a seventh or ninth chord, short (0.2 beats), landing off the beat, nothing on the downbeat.
- JAZZ: rootless voicings — third and seventh in the left hand, extensions in the right. Never root position.
- ORGAN: no velocity dynamics (a real organ has none); express with note length and by holding a drone under the changes.

SOUND
list_plugins for a piano or Rhodes; otherwise load_sampler. A dry acoustic piano usually wants a short reverb via a send. An electric piano wants chorus or tremolo before the reverb.
