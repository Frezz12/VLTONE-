---
id: synth-pad
title: Pads and atmospheres
use_when: writing a pad, a drone, an atmosphere, strings-like beds or ambience
tags: [pad, atmosphere, ambient, drone, texture]
---
A pad is not a chord part played slowly. It is the background the rest of the track sits on, and the two things it must do are last and not get in the way.

WRITING IT
- Hold notes for a whole bar or more. Overlap them across chord changes by 0.1–0.25 beats so the pad never gaps.
- Three or four notes, spread wide: one around MIDI 48–55, the rest between 60 and 79. Do not double the bass root at the bottom.
- Voice-lead: change one or two notes between chords and keep the rest. A pad that jumps its whole voicing every bar draws attention to itself, which is exactly wrong.
- Velocity low and even: 60–80. A pad does not accent.

MAKING ROOM
The pad occupies the same frequencies as the vocal and the lead. Cut it there rather than turning it down:
- add_insert an EQ and take out 2–5 kHz where the vocal lives.
- High-pass it at 150–250 Hz so it stops fighting the bass.
- Sit it 6–10 dB below the lead. Use analyze_mix and compare the mid share.

MOVEMENT
- A pad that does not move is a held chord. Automate the filter cutoff, or the reverb send, slowly over 8 or 16 bars — see the automation playbook.
- A slow attack (in the instrument's own parameters) makes the pad swell into each chord instead of starting with it.

CHAIN
Reverb and delay are the instrument here, not an effect: a long reverb via add_send with the send high, chorus before it for width. If the user wants "ambient", the reverb should be longer than the bar.
