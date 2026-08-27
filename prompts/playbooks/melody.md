---
id: melody
title: Melody and lead lines
use_when: writing a melody, a lead line, a topline, a hook, a riff or a counter-melody
tags: [melody, lead, hook, topline, riff]
---
BEFORE YOU WRITE
Call analyze_harmony. A melody is heard against the chords under it: the same note is a resolution over one chord and a clash over another. If nothing is there, choose a key and say so.

SHAPE
- A melody is a SHAPE, not a run of scale notes. Give it one high point and put it about two thirds of the way through, then come down from it.
- Write a motif of one or two bars and then repeat it changed — same rhythm on different notes, or the same notes with a different ending. Repetition is what makes something a hook; four bars of new material is an exercise.
- Leave silence. A melody that plays on every beat has nowhere to breathe and cannot be sung. End phrases early and let the gap sit.

PITCH
- Keep it inside about an octave and a half. Vocals and most leads live between MIDI 60 and 84.
- Move mostly by step. Every leap of a fourth or more should be answered by stepwise motion in the opposite direction.
- Land on a chord tone on strong beats (the root, third or fifth of the chord underneath at that moment — analyze_harmony gives you these). Notes off the chord are passing colour and belong on weak beats.
- Use transform_notes snap_to_scale at 1.0 if you are unsure, but only after the shape is right.

RHYTHM
- Do not start every phrase on beat 1. Starting on the last eighth of the previous bar (a pickup) makes it sound written rather than placed.
- Vary the note lengths: long notes at the ends of phrases, short notes inside them.
- Syncopation against the drums is what makes a line modern; a melody that lands with every kick sounds like a nursery rhyme.

EXPRESSION
- Velocity follows the shape: the high point gets 110–120, the phrase endings fall away to 70–80.
- transform_notes articulate at 0.4–0.6 for phrasing, legato where the line should be sung in one breath.
- humanize at 0.3 last. A lead is the most exposed part in a mix and a perfectly quantized one is the most obviously fake.

COUNTER-MELODY
If a melody already exists and the user asks for another line, write it to move when the first line holds and hold when the first line moves. Two lines moving together in the same rhythm is a harmony part, not a counter-melody — and if that is what they wanted, write it a third or a sixth above, in the scale.
