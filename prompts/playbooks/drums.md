---
id: drums
title: Drums and beats
use_when: programming a beat, a drum loop, percussion, fills or changing a groove
tags: [drums, beat, percussion, groove, kick, snare, hats]
---
THE MAP
Drum instruments are addressed by MIDI pitch. The General MIDI map is a useful starting point ONLY for a kit that follows GM; third-party drum instruments may use another map. The built-in single-sample sampler plays its untransposed sample at MIDI 60 (C5), not at GM pitches:
36 kick · 37 side stick · 38 snare · 39 clap · 40 rim snare · 41 low tom · 42 closed hat · 44 pedal hat · 45 mid tom · 46 open hat · 47 high tom · 48 high tom · 49 crash · 51 ride · 54 tambourine · 56 cowbell · 57 crash 2 · 75 clave

For a new drum pattern, prefer create_pattern and add_pattern_lane: one sampler lane for kick, snare/clap and hats, each at pitch 60, routed through the Pattern bus. search_files supplies authorized contentIds. A mapped drum-kit instrument can use one lane with separate pitches. Never transpose a single sample across GM pitches and call it a kit.

WRITE INTO THE SEQUENCER
Use set_sequence on each returned trackId/clipId, pitch=60 for a sampler, stepBeats=0.25 for sixteenths. In one 4/4 bar, steps 1,5,9,13 are the quarter-note kicks; steps 5,13 the backbeat; steps 3,7,11,15 the offbeat hats. Indices are 1-based from source beat zero; bar two starts at step 17. Use the actual meter, not a hardcoded 16 steps per bar. Each step is {step, velocity}, with optional offsetBeats within the cell and lengthBeats for the gate. Velocity 0 means a rest. The default gate is half a step; keep gates long enough for gated samples.
Read get_sequence before editing. mode=merge touches only named cells; mode=replace replaces only the visible selected pitch lane and preserves other pitches and source notes outside the clip window. For a trimmed clip inspect sourceOffsetBeats/firstStep/lastStep; do not overwrite hidden source material. Omit steps to change only Sequence view/pitch/grid. Both MIDI and Sequence views share one note source: never make a duplicate MIDI version.
compose_candidates role=drums may supply rhythmic alternatives, but its GM pitches must be mapped to the actual instrument. For separate samples use the chosen rhythm as a plan and write each sampler lane through set_sequence. Do not force a variation-selection round when the user asked you to make the beat directly.

THE GRID
A bar of 4/4 is 4 beats; a sixteenth is 0.25 beats. Positions inside a bar: beat 1 = 0.0, beat 2 = 1.0, beat 3 = 2.0, beat 4 = 3.0.

Styles, as a starting point rather than a law:
- HOUSE / TECHNO (120–128): kick on every beat (0, 1, 2, 3). Closed hat on every off-eighth (0.5, 1.5, 2.5, 3.5). Clap or snare on 1 and 3. Open hat on the last off-eighth of the bar.
- TRAP (130–150, felt as half time): kick on 0 and around 1.75, snare or clap on 2 only. Hats in sixteenths with rolls — bursts of 32nds (0.125 spacing) for half a beat. 808 handled by the bass playbook, not here.
- BOOM BAP (85–95): kick on 0 and 2.5, snare on 1 and 3, swung eighth hats. Ghost snares at 35–50 velocity between the backbeats are the whole style.
- DRUM AND BASS (170–175): kick on 0 and 2.5, snare on 1 and 3, broken sixteenth hats, heavy use of ghost notes.
- ROCK (110–140): kick on 0 and 2, snare on 1 and 3, eighth hats all through, crash on the first beat of a section.
- LATIN / AFRO: build from a clave (75) and let the kick answer it; do not put a snare on every backbeat.

WHAT MAKES IT NOT SOUND TYPED IN
- VELOCITY IS THE BEAT. Accents 105–120, normal hits 80–95, ghost notes 30–55. A hat line where every hit is the same velocity is the clearest sign of a machine. Alternate loud/soft on eighth hats; on sixteenths use a 3-step pattern rather than 2 so it does not sound like a tremolo.
- SWING: for boom bap, shuffle and most hip hop, call apply_groove after writing, or transform_notes quantize with triplet true at 0.6 amount. Straight sixteenths sound like a demo.
- HUMANIZE last, at 0.2–0.4. More than that and the kick stops locking with the bass.
- Leave the kick and the snare exactly on the grid and humanize only the hats if the user wants a tight modern beat.

LENGTH AND VARIATION
- Write 4 bars, not 1, when the user asks for "a beat": bars 1–3 the same, bar 4 with a fill or a change. A one-bar loop repeated is what a drum machine does, not a producer.
- A fill is not a tom run by default. Dropping the kick for a bar, doubling the hats, or a single crash lands better than four toms.
- Crash on the downbeat of the bar where a new section starts.

SOUND
- Choose a suitable installed kit or individual library samples according to the desired sound. Keep an existing kit/mapping when editing. One-shot sample patterns should use separate Pattern lanes in Sequence view.
- Consider envelopes, tuning and useful effects as part of production. An installed third-party compressor or saturation on the drum bus can add cohesion; use individual processing when it solves a specific issue. Reverb on kick is a stylistic choice, not forbidden; use restrained sends when space is wanted and preserve low-end clarity.

CHECK
Call analyze_track on the drum channel afterwards. A beat with peaks above -3 dB before anything else is in the project will not leave room for the rest; bring the channel down rather than the notes.
