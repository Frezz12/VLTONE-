#pragma once

#include "Audio/SampleBuffer.hpp"
#include "Internal/SlicerParams.hpp"

#include <cstdint>
#include <functional>
#include <memory>

/// Turning a sample into chops.
///
/// This lives in `daw_controller` and not inside the plugin for one reason:
/// the transient detector and the FFT helpers it needs are here too, and
/// `daw_pluginhost` deliberately depends on neither. The plugin receives the
/// finished table through `SlicerInstance::setSliceTable`, so nothing in the
/// audio path ever has to know how a chop was found.
///
/// Every mode produces the same contract — chops that tile the sample from its
/// first frame to its last, each parked on its own key, in ascending frame
/// order — so the panel, the project file and the tests can all rely on it
/// whatever the user picked.
namespace daw::slicer {

/// How the boundaries between chops are chosen.
using SliceMode = plugins::slicer::SliceMode;
using SliceSettings = plugins::slicer::AnalysisSettings;

/// Remap without analyzing or replacing slice sound settings. Returns true
/// when the requested scale cannot provide enough distinct MIDI keys.
bool assignKeys(plugins::slicer::SliceTable&, const SliceSettings&);
bool moveBoundary(plugins::slicer::SliceTable&, int rightSlice, engine::FrameCount,
                  engine::FrameCount minimum = 1);
bool split(plugins::slicer::SliceTable&, engine::FrameCount, engine::FrameCount minimum = 1);
bool merge(plugins::slicer::SliceTable&, int rightSlice);
engine::FrameCount snapToZero(const engine::SampleBuffer&, engine::FrameCount frame,
                              engine::FrameCount low, engine::FrameCount high);

/// Builds the chop table for `audio`.
///
/// Control or worker thread only — this is O(sample length) and allocates.
/// `keepGoing` is polled throughout; cancelling returns an empty table, which
/// is the signal to leave the previous one alone. A result that fits the
/// contract above always has `count > 0` for a non-empty sample.
plugins::slicer::SliceTable cut(const engine::SampleBuffer& audio,
                                const SliceSettings& settings,
                                const std::function<bool()>& keepGoing = {});

} // namespace daw::slicer
