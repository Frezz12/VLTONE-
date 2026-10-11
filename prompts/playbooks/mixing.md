---
id: mixing
title: Mixing
use_when: mixing, balancing levels, EQ, compression, panning, "make it sound better", fixing a muddy or quiet mix
tags: [mix, mixing, eq, compression, levels, balance, pan]
---
MEASURE FIRST. ALWAYS.
You cannot hear the project. Guessing at levels is the one thing that makes this feature useless. Call analyze_mix before you touch anything: it gives every channel and the master in one call — peak, RMS, headroom, and how the energy splits into low, mid and high.

Interpret measurements in context. Peaks near 0 dBFS leave little output headroom but do not prove recorded clipping. Leaving several dB for later parts is useful, not a mandatory -6 dB target. RMS differences depend on arrangement and role. Broad spectral shares suggest what to inspect; they cannot prove mud, sibilance, distortion or the frequency of a resonance. Never report that you heard a problem or that a number proves the mix sounds better.

ORDER OF WORK
1. LEVELS FIRST. Get the balance right with mix set_level before any effect. Most "it sounds bad" is a balance problem, and every effect you add first has to be redone afterwards.
   Use the existing balance and musical priorities, not fixed peak targets per instrument. Clip gain/plugin input controls upstream headroom; the channel fader is downstream of inserts.
2. PANNING. A centred lead vocal and low end are common starting points. Consider existing stereo width, real double takes and mono compatibility before moving pan; do not pan everything just to fill the field.
3. EQ. Correct only identified problems. A high-pass can remove unwanted lows but must preserve useful fundamentals. Choose cuts/boosts for role and context; never automatically high-pass every non-bass track.
4. DYNAMICS. Use compression for a purpose: levelling, transient shape, colour, groove or ducking. Attack/release depend on material and the processor. Check parameters and input levels rather than applying one preset everywhere.
5. SPACE. Shared reverb/delay buses can provide cohesion and easy rides. Multiple spaces or insert effects are valid artistic choices. Keep returns wet, manage feedback, filter if needed, and preserve intelligibility. For vocals load vocal-processing.

DOING IT WITH THE TOOLS
- mix set_level / set_pan for balance. analyze_mix again after.
- list_plugins kind=effect to find an EQ or compressor, then add_insert, then list_plugin_parameters (uids and ranges differ per plugin — never guess), then set_insert_parameters.
- Reverb bus: add_track kind=bus, add_insert the reverb on it, add_send from each source channel.
- Prefer suitable installed third-party processors when they help the brief. get_plugin_state/configure_plugin expose host wet/dry, sidechain, channel layout and effect order. mix set_output changes main routing; mix set_send can set levelDb, preFader and enabled. Inspect current routing to avoid duplicate sends or feedback.
- channel_strip copy/paste to give two similar channels the same treatment instead of rebuilding it.

FINISH
Measure the same region again and report factual differences such as master peak and RMS. Keep measured statements separate from listening judgements. Loudness alone is not quality: compensate gain and compare bypass when the user requests audition. Restore a rejected setting with its recorded previous value instead of invoking a broad undo that might remove other work.
