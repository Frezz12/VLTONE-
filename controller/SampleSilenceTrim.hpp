#pragma once

#include "Audio/SampleBuffer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace daw {

// Exact frame boundaries for each whole-dB threshold. Build on a worker once;
// moving the Trim knob then only reads this small immutable table.
struct SampleSilenceTrim {
    static constexpr int minimumDb = -96;
    struct Range {
        engine::FrameCount first = 0, end = 0;
        bool audible = false;
    };
    std::array<Range, 97> ranges{};
    engine::FrameCount frames = 0;
    double sampleRate = 0;

    Range at(double thresholdDb) const {
        if (thresholdDb <= minimumDb) return {0, frames, true}; // Off restores the source.
        return ranges[std::clamp(int(std::lround(thresholdDb)) - minimumDb, 0, 96)];
    }
};

inline SampleSilenceTrim analyzeSampleSilenceTrim(const engine::SampleBuffer& audio,
    engine::FrameCount frames, const std::function<bool()>& keepGoing = {}) {
    SampleSilenceTrim result;
    result.frames = std::min(frames, audio.frames());
    result.sampleRate = audio.sampleRate();
    if (!result.frames || !audio.channels() || !(result.sampleRate > 0)) return result;
    std::array<float, 97> levels;
    for (int i = 0; i < int(levels.size()); ++i)
        levels[i] = float(std::pow(10., (SampleSilenceTrim::minimumDb + i) / 20.));
    // A stereo signal may cancel in mono. Link absolute channel peaks instead.
    for (bool backwards : {false, true}) {
        int threshold = 0;
        for (engine::FrameCount n = 0; n < result.frames && threshold < int(levels.size()); ++n) {
            if ((n & 4095) == 0 && keepGoing && !keepGoing()) return {};
            const auto frame = backwards ? result.frames - 1 - n : n;
            float peak = 0;
            for (engine::ChannelCount ch = 0; ch < audio.channels(); ++ch)
                peak = std::max(peak, std::abs(audio.readSample(ch, frame)));
            while (threshold < int(levels.size()) && peak >= levels[threshold]) {
                auto& range = result.ranges[threshold++];
                if (backwards) range.end = frame + 1;
                else { range.first = frame; range.audible = true; }
            }
        }
    }
    return result;
}

} // namespace daw
