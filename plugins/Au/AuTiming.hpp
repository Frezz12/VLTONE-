#pragma once

#include <cmath>
#include <cstdint>

/// The arithmetic behind the Audio Unit host callbacks, kept out of the ObjC++
/// so it can be tested at all: `AuInstance.mm` is the only `.mm` in the project
/// and nothing else in the tests can include it.
namespace daw::plugins::au {

/// How many frames until the next quarter-note boundary.
///
/// What `kMusicalTimeLocation`'s `deltaSampleOffsetToNextBeat` means, and the
/// number an arpeggiator or a synced delay lines itself up on. Exactly on a
/// beat the answer is 0, not a whole beat: the boundary is now.
inline std::uint32_t samplesToNextBeat(double ppqPosition, double tempo,
                                       double sampleRate) {
    if (!(tempo > 0.0) || !(sampleRate > 0.0)) return 0;
    const double samplesPerBeat = sampleRate * 60.0 / tempo;
    const double intoBeat = ppqPosition - std::floor(ppqPosition);
    // A position a hair below the next beat (floating point rounding on a
    // playhead that never lands exactly) must read as "now", not as a whole
    // beat away — the difference is one beat of silence from a synced plugin.
    if (intoBeat <= 0.0 || 1.0 - intoBeat < 1e-9) return 0;
    const double toNextBeat = (1.0 - intoBeat) * samplesPerBeat;
    if (toNextBeat < 0.0) return 0;
    if (toNextBeat > samplesPerBeat) return std::uint32_t(samplesPerBeat);
    return std::uint32_t(toNextBeat);
}

} // namespace daw::plugins::au
