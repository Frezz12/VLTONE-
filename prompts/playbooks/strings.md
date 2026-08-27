---
id: strings
title: Strings and orchestral
use_when: writing string sections, orchestral parts, cinematic beds or brass
tags: [strings, orchestra, cinematic, violin, brass]
---
RANGES — writing outside them is the giveaway
Violin 55–96 · Viola 48–84 · Cello 36–72 · Double bass 28–55 · Flute 60–96 · Clarinet 50–89 · Trumpet 55–82 · French horn 41–77 · Trombone 40–72.
Write each instrument as its own track when the user wants a section; one track with everything stacked will not sound like an ensemble however good the library is.

VOICING A SECTION
- Give each part its own line rather than stacking a chord: cellos on the root, violas on the third or fifth, violins on the top note or the melody.
- Keep the parts inside an octave of each other except the bass.
- Do not cross parts, and do not let two parts play the same note unless you mean an octave doubling.
- Wide spacing low down turns to mud: below MIDI 48 keep it to octaves and fifths.

ARTICULATION
Sample libraries switch articulation by key or by parameter; MIDI note length is what you actually control here:
- Legato/sustain: notes overlap slightly, 1.02–1.1 of their slot.
- Staccato: 0.3–0.4 of the slot, velocity 90–110.
- Spiccato runs: short and fast, velocity varying 70–110 note to note.
Choose one per part and stay with it; a line that alternates randomly sounds like a keyboard.

DYNAMICS ARE THE MUSIC
Strings live on swells. Velocity alone is not enough — most libraries map the swell to a controller or a parameter.
- Write the shape with transform_notes ramp_velocity across a phrase (from 60 to 110 into the high point, back down after it).
- Then automate the instrument's dynamics/expression parameter over the same bars — see the automation playbook. Without this, a string part is flat and obviously fake.

FEEL
- humanize at 0.4–0.6: a section is never together, and that spread is what makes it sound like people.
- Lower parts enter fractionally before higher ones on a big chord.
- Leave silence between phrases. Orchestral writing breathes in 4- and 8-bar sentences.
