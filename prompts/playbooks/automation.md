---
id: automation
title: Automation
use_when: automating a parameter, fades, filter sweeps, volume rides, sidechain-style ducking, movement over time
tags: [automation, fade, sweep, filter, volume, movement]
---
Automation is how a static loop becomes a track. Anything the user describes as "it gets brighter", "fade it in", "duck the music under the vocal" or "make it move" is automation, not a note edit.

HOW IT WORKS HERE
An automation lane is a track of its own that sits under the track it controls, and the curve is a clip on that lane.
1. `automation list_targets` with a channel id tells you what can be automated on it: its volume and pan, and every parameter of every plugin on it. Call it first — parameter ids differ per plugin and cannot be guessed.
2. `automation create` makes the lane and returns the lane and clip ids.
3. `automation set_points` writes the curve: a list of points, each with a position in bars and a value from 0 to 1 (normalized), and optionally a curve shape.
4. `automation remove` deletes a lane the user no longer wants.

Values are NORMALIZED 0–1, not the parameter's own units: 0 is the bottom of its range and 1 the top. `list_targets` reports what the ends mean.

SHAPES THAT ARE WORTH WRITING
- FADE IN: two points, 0 at the start bar and 1 at the end bar, on the channel's volume.
- FILTER SWEEP / BUILD: cutoff from about 0.15 at the start of the build to 1.0 exactly on the downbeat of the drop. The last point must land ON the downbeat, not a beat before it.
- BREAKDOWN: cutoff from 1.0 down to 0.2 over 8 bars, then a jump back to 1.0 at the new section.
- DUCKING (sidechain by hand): on the music bus volume, four points per bar — 0.35 on the beat, back to 1.0 a quarter of a beat later, for every kick. Do this when no sidechain compressor is installed.
- RIDE: small volume moves, ±0.05, to hold a vocal steady where a compressor would pump.
- MOVEMENT ON A PAD: one slow curve over 16 bars on the cutoff or the reverb send. Two points are enough.

RULES
- Land the important point exactly on a bar line. Automation that resolves half a beat early is audible and sounds like a mistake.
- Three or four points do most jobs. Twenty points on a fade is not smoother, it is just harder to edit afterwards.
- Automate ONE parameter per idea. A filter and a volume and a reverb send all moving together is mud.
- Do not automate the master volume to make something louder — that is a mix or mastering problem.
- Say what you automated and over which bars, because the user has to be able to find it.
