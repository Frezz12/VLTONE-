---
id: synth-lead
title: Synth leads and plucks
use_when: writing a synth lead, pluck, arp, acid line or any electronic hook
tags: [synth, lead, pluck, arp, acid, edm]
---
Read the melody playbook for the shape of the line; this is about making a synth sound like a synth part rather than a piano part played on a synth.

WHAT MAKES IT ELECTRONIC
- A synth lead is usually SHORT and REPEATED. One bar, repeated with a small change every fourth bar, beats four bars of new material.
- Note length carries the character: plucks are 0.1–0.2 beats whatever the grid; a supersaw lead holds notes for a whole beat or more.
- Quantize HARD. transform_notes quantize at 1.0, and skip humanize or keep it under 0.15. Unlike an acoustic part, an electronic lead should be exactly on the grid — the machine feel is the point.

ARPEGGIOS
- Write the chord, then transform_notes arpeggiate. Sixteenths at 120–128 BPM is the house default; a triplet grid gives it a rolling feel.
- Range: keep an arp inside one or two octaves. A four-octave arp is a preset demo, not a part.
- Octave jumps every other bar keep a long arp alive.

ACID / 303
- One note at a time, sixteenths, mostly the root with occasional octave jumps and one or two notes from the scale.
- Slides are accent + overlap: make two consecutive notes overlap by 0.05 beats and give the second a lower velocity.
- Accents matter more than pitch here: alternate 120 and 70 velocity irregularly.

PLUCKS AND STABS
- Off-beat placement (the "and" of each beat) is what makes a pluck sit in a house track.
- Leave the downbeat to the kick.

SOUND AND MOVEMENT
- list_plugins for a synth; otherwise the built-in sampler.
- A static synth line is boring by the second loop. Automate the filter cutoff over 4 or 8 bars with the automation tool — see the automation playbook. This is expected in electronic music, not optional.
- Chain: filter or EQ → drive → delay (dotted eighth for the classic effect) → reverb by send.
- Unison and detune live in the plugin; call list_plugin_parameters and set them rather than writing two tracks a few cents apart.
