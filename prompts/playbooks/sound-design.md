---
id: sound-design
title: Sound design and the built-in sampler
use_when: designing a sound, shaping a patch, using the built-in sampler, choosing or editing samples, risers and effects
tags: [sound design, sampler, patch, synthesis, fx, riser]
---
THE BUILT-IN SAMPLER
load_sampler puts a sample on a track without needing any plugin installed. It is the fallback whenever list_plugins has nothing suitable, and the right choice for one-shots and drums.
- Find material with search_files — it only looks in the folders the user added to the browser.
- Shape it like any other plugin: list_plugin_parameters on the instrument slot, then set_insert_parameters. The envelope is what turns a sample into an instrument — a fast attack and a short decay for a pluck, a slow attack for a pad, a long release for anything that should ring.
- Pitch the sample rather than looking for another one: one good hit tuned to the key beats three untuned ones.

SUBTRACTIVE SYNTHESIS, IN THE ORDER THAT MATTERS
When you have a synth plugin, call list_plugin_parameters first and then work in this order — the names differ per plugin but the roles do not:
1. OSCILLATOR — the raw material. Saw for anything bright and full, square for hollow and reedy, sine for sub and bells, noise for percussion and air.
2. FILTER — where the character is. Low-pass cutoff decides how bright, resonance how vocal. Most "wrong" sounds are a cutoff problem.
3. ENVELOPE — attack decides whether it is a pluck or a pad, release whether it stops or blooms.
4. LFO / MODULATION — motion. A sound with no movement is dead by the second bar.
5. EFFECTS — last.

MAKE IT MOVE
A static patch is the mark of a preset. Give one parameter a life over the section — filter cutoff opening across 8 bars, a slow LFO on the pitch of a pad — with the automation tool. See the automation playbook.

TRANSITION EFFECTS
- RISER: a long note with the filter cutoff automated from closed to open across 2–4 bars, plus a pitch rise if the plugin allows it.
- IMPACT: a low sine or a reversed crash on the downbeat of the new section, with a long reverb.
- SWEEP: white noise through a band-pass whose frequency is automated up, then down.
- Send these to a reverb bus rather than adding a reverb insert to each.

LAYERING
Two sounds make a third: a sub sine under a bright saw, a click on top of a soft kick. Give each layer its own frequency job with EQ before you decide the layering failed — most of the time they are simply fighting.
