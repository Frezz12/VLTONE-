#pragma once

#include "platform/AudioFileDecoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

/// What one render job is. Pure data with no engine or UI types in it, so the
/// dialog that fills it in, the settings that remember it and the controller
/// that executes it all agree on one description without depending on each
/// other.
namespace daw::rendering {

enum class Range {
    WholeProject,  ///< Zero to the end of the last clip.
    CycleRegion,   ///< The loop markers, whether or not looping is switched on.
    Custom         ///< Whatever the caller worked out — a time selection, say.
};

enum class Tail {
    None,         ///< Stop dead at the end of the range.
    Fixed,        ///< Carry on for a set number of seconds.
    UntilSilence  ///< Carry on until the decay falls below a threshold.
};

enum class Channels { Stereo, Mono };

struct Spec {
    // ── Destination ──
    std::string outputDir;
    /// The mixdown is `<baseName>.<ext>`; a stem is `<baseName> - <channel>.<ext>`.
    std::string baseName = "mixdown";

    // ── Format ──
    audio::platform::WriteSpec file;
    /// Zero means "whatever the project runs at". Anything else re-prepares the
    /// engine for the pass, so clips resample once into the target rate rather
    /// than being converted twice.
    double sampleRate = 0.0;
    Channels channels = Channels::Stereo;

    // ── Range ──
    Range range = Range::WholeProject;
    double customStartSeconds = 0.0;
    double customEndSeconds = 0.0;

    /// Audio rendered *before* the range and then thrown away, so that a range
    /// starting mid-project begins with the reverb and delay that were already
    /// ringing rather than from silence. Costs render time and nothing else.
    double preRollSeconds = 0.0;

    // ── Tail ──
    Tail tail = Tail::None;
    double tailSeconds = 2.0;        ///< Tail::Fixed.
    double tailSilenceDb = -96.0;    ///< Tail::UntilSilence — the threshold.
    double tailHoldSeconds = 0.3;    ///< …and how long it must stay under it.
    double tailMaxSeconds = 30.0;    ///< Hard ceiling for either tail mode.

    // ── What to write ──
    bool writeMixdown = true;
    /// Channels to capture separately: track uuids, bus/group/folder uuids, or
    /// `EngineController::kMasterChannelId`. Empty means no stems.
    std::vector<std::string> stemChannelIds;

    /// Written into each file's header. The title is per-file — a stem gets its
    /// own — so the controller fills that in; the rest come from the caller.
    audio::platform::FileTags tags;

    // ── Processing ──
    /// Render dry: every insert on every channel bypassed for the pass. The
    /// instruments stay, so a MIDI track still makes its sound.
    bool bypassChannelInserts = false;
    /// More precise internal-render switches used by Bounce in Place. Export
    /// leaves all of them false and retains its historical behaviour.
    bool bypassClipInserts = false;
    bool bypassTrackInserts = false;
    bool bypassSummingInserts = false;
    bool bypassSends = false;
    bool bypassMasterChain = false;
    /// Optional source isolation. Clip ids win when supplied; track ids mean
    /// every musical clip on those tracks. Automation clips remain active so
    /// the isolated signal is rendered with its current automation.
    std::vector<std::string> sourceTrackIds;
    std::vector<std::string> sourceClipIds;
    /// Render every channel as though nothing were muted or soloed.
    bool ignoreMuteSolo = false;
    /// Take stems from ahead of the fader, so each one arrives at unity with
    /// pan centred and mute ignored. The mixdown written in the same pass is
    /// unaffected — it still goes through every fader.
    bool stemsPreFader = false;
    /// Internal bounce capture directly ahead of a channel's insert chain.
    /// Mutually exclusive with `stemsPreFader`; ordinary Export never sets it.
    bool stemsAtSource = false;
};

/// Where a render has got to, for a progress bar.
struct Progress {
    double fraction = 0.0;
    /// The range being rendered, in seconds — for a "3:12 of 4:05" readout.
    double renderedSeconds = 0.0;
    double totalSeconds = 0.0;
};

struct Report {
    std::vector<std::string> files;
    bool cancelled = false;
    /// Everything actually written, including any tail.
    double renderedSeconds = 0.0;
};

} // namespace daw::rendering
