---
id: drums
title: Drums and beats
use_when: programming a beat, a drum loop, percussion, fills or changing a groove
tags: [drums, beat, percussion, groove, kick, snare, hats]
---
THE MAP
Drum instruments are addressed by MIDI pitch. Use the General MIDI map — every drum plugin and the built-in sampler follow it:
36 kick · 37 side stick · 38 snare · 39 clap · 40 rim snare · 41 low tom · 42 closed hat · 44 pedal hat · 45 mid tom · 46 open hat · 47 high tom · 48 high tom · 49 crash · 51 ride · 54 tambourine · 56 cowbell · 57 crash 2 · 75 clave

With a real drum instrument, write the whole GM kit into ONE MIDI clip on ONE track. With the built-in single-sample sampler, one track can only play one loaded file: make separate sampler tracks for kick, snare/clap and hats, play each original hit at MIDI 60, and group those tracks. Never transpose one sample across GM pitches and call it a kit.

For a real drum instrument, use compose_candidates with role drums to get 3–5 scored GM patterns and apply the chosen candidate. For separate single-sample sampler tracks, use those candidates only as a rhythmic plan: split kick, snare and hat events onto their own MIDI-60 clips.

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
- Prefer an installed drum instrument (list_plugins, kind instrument). If none is suitable, search_files for the individual pieces, create one instrument track per piece, load_sampler once on each, and group the tracks with arrange_tracks.
- Chain: no reverb on the kick. Compression on the whole kit, not on each drum. If the user wants room, send the snare to a reverb bus with add_send.

CHECK
Call analyze_track on the drum channel afterwards. A beat with peaks above -3 dB before anything else is in the project will not leave room for the rest; bring the channel down rather than the notes.
