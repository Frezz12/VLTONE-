#pragma once

#include "Nodes/PlaybackNodes.hpp"
#include "model/Document.hpp"
#include <functional>
#include <vector>

namespace daw {

struct StripSilenceSettings {
    double thresholdDb = -42.0;
    double hysteresisDb = 6.0;
    double minimumSilenceMs = 150.0;
    double minimumSoundMs = 30.0;
    double preRollMs = 10.0;
    double postRollMs = 50.0;
    double fadeMs = 3.0;
    double gridBeats = 0.25; // 1/16; zero disables snapping.
    bool splitInternal = true;
    friend bool operator==(const StripSilenceSettings&, const StripSilenceSettings&) = default;
};

StripSilenceSettings sanitizedStripSilenceSettings(StripSilenceSettings settings);

struct SilenceRegion {
    double begin = 0, end = 0; // seconds relative to the original clip
    friend bool operator==(const SilenceRegion&, const SilenceRegion&) = default;
};

struct SilenceEnvelope {
    double stepSeconds = 0.001;
    double durationSeconds = 0;
    std::vector<float> peaks; // channel-linked peaks; never sum stereo to mono
};

// Worker-safe: consumes immutable source placements, never the controller or
// live audio graph. The ordinary clip renderer preserves trims, comp and Warp.
SilenceEnvelope buildSilenceEnvelope(const engine::ClipPlayerNode::ClipList& placements,
    double durationSeconds, double sampleRate, const std::function<bool()>& keepGoing = {},
    const std::string& renderPath = {});

std::vector<SilenceRegion> detectSilenceRegions(const SilenceEnvelope& envelope,
    StripSilenceSettings settings, double timelineStartSeconds, double tempo);

// Intersect comp segments and rebase source offsets; original audio is never
// rewritten. Every fragment receives independent clip, take and plugin IDs.
ClipModel sliceSilenceRegion(const ClipModel& original, SilenceRegion region,
    double durationSeconds, double fadeMs, double tempo);

} // namespace daw
