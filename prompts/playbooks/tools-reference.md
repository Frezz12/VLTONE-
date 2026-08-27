---
id: tools-reference
title: Tool reference
use_when: before using edit_clip, edit_notes, mix, arrange_tracks, channel_strip, automation or transport for the first time
tags: [tools, actions, reference]
---
Seven tools take an `action` and behave differently for each one. Their schemas list the action names; this is what each one actually needs. Read it once per session, not before every call.

edit_clip — always with trackId and clipId
| action | also needs | does |
|---|---|---|
| move | atBar | moves the clip to that bar |
| move_to_track | toTrackId | same position, different track |
| rename | name | |
| mute / unmute | — | silences the clip without deleting it. Prefer this to remove |
| gain | gainDb (-60…12) | clip gain, independent of the channel fader |
| fade | fadeInBeats, fadeOutBeats | the fix for a click at either end |
| trim | atBar, offsetBeats, lengthBars | any of the three; the others stay as they are |
| split | atBar (inside the clip) | returns firstClipId and secondClipId |
| duplicate_at | atBar | returns the new clipId. How a section is repeated |
| remove | — | asks the user first |

edit_notes — always with trackId and clipId
| action | also needs | does |
|---|---|---|
| add | notes | appends without disturbing what is there |
| remove | fromBeat, toBeat, optionally lowPitch/highPitch | deletes notes sounding in the range |
| replace_range | fromBeat, toBeat, notes | removes then adds, so one bar can be rewritten |
Beats are measured from the clip's start. lowPitch/highPitch is how you edit the hats without touching the kick.

mix
| action | needs | notes |
|---|---|---|
| set_level | channelId, levelDb | -60…+6 dB |
| set_pan | channelId, pan | -1 left … 1 right |
| mute / unmute / solo / unsolo | channelId | |
| clear_solos | — | |
| set_send | channelId, toTrackId, levelDb | creates the send if there is none; otherwise sets its level |
| remove_send | channelId, sendId | |
| set_master_volume | levelDb | |

arrange_tracks
| action | needs | returns |
|---|---|---|
| reorder | trackId, toIndex (0 is the top) | |
| create_folder | name, summing | folderId |
| move_to_folder | trackId, folderId (empty string = back to the top level) | |
| set_color | trackId, color like "#30D158" | |
| duplicate_track | trackId | the new trackId, with the same plugins |
`summing: true` makes the folder a bus everything inside is mixed through, so it can carry effects for the group. False just groups them visually.

channel_strip
| action | needs | does |
|---|---|---|
| copy_to | channelId (source), toChannelId, withSettings | copies the chain — with the plugins' current settings — onto another channel |
| clear | channelId | removes every effect. Asks the user first |

automation — read the automation playbook as well
| action | needs | does |
|---|---|---|
| list_targets | channelId | lists volume, pan, every send and every plugin parameter on that channel |
| set_points | channelId, target, points | writes the curve, making the lane if there is none |
| remove | channelId, target | deletes the lane |
`target` is volume, pan, mute, send (with sendId) or plugin (with insertId and parameterId). For a track's instrument, insertId is the EMPTY STRING, not the instrument's id. Points are `{bar, value}` with value normalised 0…1, in ascending order of bar, optionally with `shape` of linear, hold or smooth.

transport
`action` of play, stop, pause, seek (with bar), metronome_on, metronome_off. Only when the user asked.

WHICH TOOL FOR WHICH JOB
- New part → add_track, set_track_instrument, add_midi_clip, set_clip_notes.
- Change a part you wrote → transform_notes (whole clip) or edit_notes (part of it).
- Change where or how long something plays → edit_clip.
- Anything about level, pan, sends → mix, after analyze_mix.
- Anything that changes over time → automation.
- Effects on a channel → list_plugins, add_insert, list_plugin_parameters, set_insert_parameters. The instrument slot works with those too, which is how the built-in sampler is shaped.
- Something the program can do that none of these covers → say so plainly. Do not fake it by writing notes.
