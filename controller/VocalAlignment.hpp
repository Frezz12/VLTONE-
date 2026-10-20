#pragma once

#include "Audio/SampleBuffer.hpp"
#include "Common/WarpMap.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace daw::alignment {

enum class AlignmentMethod { Hybrid, Attacks, Shift };
struct AlignmentSettings {
    AlignmentMethod method = AlignmentMethod::Hybrid;
    double strength = 1;
    double toleranceSeconds = .005;
    double maximumShiftSeconds = .5;
    double maximumStretch = 1.25;
};

// Immutable, audible pre-insert source. Map coordinates refer to this buffer;
// project positions remain seconds until the final conversion to Warp beats.
struct AlignmentSource {
    std::shared_ptr<const engine::SampleBuffer> audio;
    ClipWarpModel baseline;
    double startSeconds = 0, durationSeconds = 0, tempo = 120;
    double pitch = 0, formant = 0;
};
enum class AlignmentStatus { Aligned, Partial, Skipped };
struct AlignmentProposal {
    ClipWarpModel map;
    double startSeconds = 0, durationSeconds = 0;
    AlignmentStatus status = AlignmentStatus::Skipped;
    std::size_t matched = 0, skipped = 0;
    double maximumShiftSeconds = 0;
    std::string reason;
    bool rigidShift = false;
};
using Progress = std::function<bool(double)>;

// Targets run sequentially; the guide is analyzed once. Cancellation throws
// std::runtime_error and never returns a partially completed batch.
std::vector<AlignmentProposal> align(const AlignmentSource& guide,
    const std::vector<AlignmentSource>& targets, const AlignmentSettings& settings,
    const Progress& progress = {});

} // namespace daw::alignment
