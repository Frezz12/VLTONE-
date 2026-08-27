---
id: arrangement
title: Arrangement and structure
use_when: laying out a song, adding sections, building an intro or a drop, deciding what plays where
tags: [arrangement, structure, song, sections, intro, drop]
---
STRUCTURES
Bars, in 4/4. Use these as the default when the user asks for "a track" or "a song" without saying more.
- POP: intro 8 · verse 16 · pre-chorus 8 · chorus 16 · verse 16 · pre-chorus 8 · chorus 16 · bridge 8 · chorus 16
- HOUSE / TECHNO: intro 16 · build 16 · drop 32 · breakdown 16 · build 16 · drop 32 · outro 16
- HIP HOP: intro 8 · verse 16 · hook 8 · verse 16 · hook 8 · outro 8
- AMBIENT / CINEMATIC: no repeats — one continuous shape that adds a layer every 16 bars.

Sections change on multiples of 4 bars, and the big ones on 8 or 16. A section that starts on bar 13 sounds like a mistake to everybody.

THE ONE RULE OF ARRANGING
Energy comes from what is NOT playing. A chorus is only big because the verse before it was smaller. To build a section:
- Take parts AWAY for the 8 bars before a drop rather than adding more. Silence the bass for the last bar.
- Add one new element per section. Four new elements at once is noise.
- Filter a full loop rather than deleting it: automate a low-pass down over 8 bars for a breakdown, then open it on the downbeat.

HOW TO DO IT WITH THE TOOLS
- Copy a section: edit_clip duplicate_at with the target bar, once per clip. Duplicating and then editing beats writing new notes.
- Mute a part for a section: split the clip at the section boundary with edit_clip split, then edit_clip mute on the piece you want silent. Do not delete it — the user will want it back.
- Group the parts of a section with arrange_tracks create_folder so the arrangement stays readable, and give each family a colour with arrange_tracks set_color (drums one colour, harmony another, vocals a third).
- Automate the transitions rather than editing the notes: see the automation playbook.

TRANSITIONS
- A crash on the downbeat of a new section.
- A reverse cymbal or a riser over the last 2 bars before a drop.
- One bar of drums alone, or one bar of nothing at all, immediately before the biggest section.
- Never crossfade two sections that are in different keys without a bar of transition.

WHEN THE USER SAYS "MAKE IT LONGER"
Do not loop the same 8 bars. Duplicate them, then change one thing per repeat: drop an element, add a fill, move a part up an octave.
