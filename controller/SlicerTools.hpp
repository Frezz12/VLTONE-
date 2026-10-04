#pragma once

#include "Internal/SlicerInstance.hpp"
#include "MidiFile.hpp"

#include <functional>
#include <span>

namespace daw::slicer {

enum class PhraseOrder { Source, Reverse, Shuffle };
struct RandomSettings {
    std::uint64_t seed = 1;
    bool pitch = true, gain = false, pan = false, reverse = false, filter = false, keys = false;
    double pitchRange = 12.0, gainRangeDb = 6.0, panRange = 1.0;
    double cutoffMin = 0.2, cutoffMax = 0.9;
};

void randomize(plugins::slicer::SliceTable&, std::span<const std::uint32_t> ids,
               const RandomSettings&);
bool normalize(const engine::SampleBuffer&, plugins::slicer::SliceTable&,
               std::span<const std::uint32_t> ids, const std::function<bool()>& keepGoing = {});
midifile::File midiPhrase(const plugins::slicer::ControlState&, PhraseOrder,
                         std::uint64_t seed = 1);

// Both preset operations are staged. A failed/cancelled import never changes out
// and never publishes a partially decoded sample to the live instrument.
bool savePreset(const std::string& path, const plugins::slicer::ControlState&,
                std::string& error, const std::function<bool()>& keepGoing = {});
bool loadPreset(const std::string& path, const std::string& mediaCache,
                plugins::slicer::ControlState& out, std::string& error,
                const std::function<bool()>& keepGoing = {});

// Writes one finite pass, at the source rate, without channel effects. The caller
// uses a fresh cache filename and atomically copies the completed file to an
// export destination; drag/drop keeps the durable cache copy as its source.
bool renderWav(const std::string& path, const plugins::slicer::ControlState&,
               std::uint32_t sliceId, bool processed, std::string& error,
               const std::function<bool()>& keepGoing = {});

} // namespace daw::slicer
