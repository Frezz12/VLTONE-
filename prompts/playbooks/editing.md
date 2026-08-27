---
id: editing
title: Editing clips, notes and takes
use_when: cutting, trimming, moving, copying, fading, muting or repairing existing material
tags: [edit, clip, cut, trim, fade, copy, comping, takes]
---
The user's existing material is theirs. Edit it; do not re-create it. Rewriting a part you were asked to shorten is the most damaging thing you can do here, and it cannot be told apart from a bug.

CLIPS — edit_clip
One tool, many actions. Every one of them takes the track id and the clip id.
- `move` to another bar · `move_to_track` to another track
- `rename` · `mute` (leave it in place and silent — always prefer this to removing)
- `gain` in dB, per clip, without touching the channel fader
- `fade` with fadeInBeats / fadeOutBeats — the fix for a click at the start or end of an audio clip
- `trim` to change where the clip starts and how much of the material it shows
- `split` at a bar, which returns the id of the new second half
- `duplicate_at` a bar — this is how a section is repeated; never rewrite the notes
- `remove` — only when the user asked for it in so many words

Common jobs:
- "Make it 8 bars instead of 4": duplicate_at bar 5.
- "Cut the last bar": split at the bar line, then remove or mute the tail.
- "It clicks": fade with 0.01–0.05 beats at each end.
- "Move the chorus later": move each clip of that section by the same number of bars, in one call each, and check nothing now overlaps.

NOTES — edit_notes
To change part of a clip, use edit_notes rather than sending the whole list back through set_clip_notes:
- `add` a few notes without disturbing the rest
- `remove` the notes inside a beat range, optionally limited to a pitch range
- `replace_range` to rewrite one bar and leave the others alone
Read the clip with get_clip_notes first when you did not write it yourself. Positions are in beats from the clip's start.

REPAIRS THAT COME UP CONSTANTLY
- Timing is loose: transform_notes quantize at 0.7–0.9, not 1.0, unless it is electronic.
- Everything is one velocity: transform_notes ramp_velocity or scale_velocity, then humanize.
- Notes run into each other: transform_notes scale_length at 0.9.
- Part is in the wrong key: transform_notes transpose for semitones, snap_to_scale to pull it into the project's key.
- Part is too low or too high: transform_notes transpose by 12 or -12 rather than rewriting.

TAKES
A recorded track can hold several takes with a comp on top. Do not touch the comp unless the user asks — choosing between takes is a judgement about a performance you cannot hear.

BEFORE ANY OF IT
Say what you are about to change if it removes anything. Deleting is confirmed with the user by the program itself, and a confirmation the user did not expect is a sign you should have asked first.
