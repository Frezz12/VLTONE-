#pragma once
#include "MidiTools.hpp"
#include "WarpAnalysis.hpp"
#include <limits>
#include <set>

namespace daw::warptools {
struct AlignParams {
    miditools::QuantizeParams timing;
    double originBeats = 0;
    double tempo = 120;
    double beginBeats = 0, endBeats = std::numeric_limits<double>::infinity();
    double minimumStrength = .5, minimumConfidence = .65;
    bool addTransients = false, includeUncertain = false;
    std::set<std::string> selected;
};
struct Proposal {
    ClipWarpModel map;
    std::size_t moved = 0, added = 0, uncertain = 0, constrained = 0;
    double maximumShiftBeats = 0;
};
// Pure and repeatable: every strength change starts from the confirmed map.
Proposal align(const ClipWarpModel& baseline, const std::vector<analysis::WarpTransient>& attacks,
               const AlignParams& params);
// Onsets relative to the selected region. Empty slots remain straight, nearby
// hits are averaged. Audio extraction deliberately contains no velocity data.
miditools::Groove extractGroove(const std::vector<double>& onsets, double lengthBeats,
                              double gridBeats, const std::string& name);
}
