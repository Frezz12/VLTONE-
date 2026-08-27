---
id: guitar
title: Guitar
use_when: writing guitar parts — acoustic, electric, rhythm, lead, arpeggios
tags: [guitar, strum, riff, acoustic, electric]
---
WHAT A GUITAR CAN PHYSICALLY PLAY
This is where programmed guitar goes wrong. A guitar has six strings tuned E2 A2 D3 G3 B3 E4 — in MIDI numbers 40, 45, 50, 55, 59, 64.
- Nothing below 40. Ever, unless the user says drop tuning.
- At most six notes at once, and each from a different string.
- The interval between the lowest and highest note of a chord is at most two octaves, and no two notes closer than a semitone.
- Above MIDI 76 is high fret territory: fine for a lead, wrong for a chord.

CHORD SHAPES THAT EXIST
Write these MIDI sets rather than stacking a triad:
- E minor: 40 47 52 55 59 64 · E major: 40 47 52 56 59 64
- A minor: 45 52 57 60 64 · A major: 45 52 57 61 64
- G major: 43 47 50 55 59 67 · C major: 48 52 55 60 64
- D major: 50 57 62 66 · D minor: 50 57 62 65
- Power chord (any root R): R, R+7, R+12. Three notes, nothing else. This is the whole of rock rhythm guitar.
Transpose a shape to move it up the neck; do not re-voice it.

STRUMMING
A strum is not a block chord. After writing the chord:
- transform_notes strum with 0.015–0.03 beats. Down strokes low to high; the tool spreads in one direction, which is what you want.
- Down strokes are louder (100–115) than up strokes (75–90).
- On a fast strum pattern the up strokes often only hit the top three strings. Write fewer notes on the off-beats.

ARPEGGIOS AND PICKING
- Pick one note at a time from a held chord shape; let the notes ring by overlapping their lengths.
- transform_notes arpeggiate turns a held chord into a picked pattern; then humanize at 0.3.

LEAD
- Single notes, MIDI 52–88. Bends and slides do not exist in MIDI here, so imply them with two fast notes a semitone apart, the first short and quieter.
- Vibrato is not available; use note length and velocity for expression instead.

SOUND
Electric guitar without an amp simulator sounds like a toy. list_plugins for an amp or distortion effect and add_insert it. Order: amp/drive → EQ → delay → reverb (reverb via a send).
