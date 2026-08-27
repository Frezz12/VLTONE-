#pragma once

#include <cstdint>

#include "daw/audio/AudioBuffer.h"

namespace daw::graph {

using SampleCount = std::int64_t;

struct ProcessContext {
    daw::audio::AudioBufferView* inputs = nullptr;
    daw::audio::AudioBufferView* outputs = nullptr;
    int numInputs = 0;
    int numOutputs = 0;
    int numFrames = 0;
    SampleCount playPosition = 0;
    bool playing = false;
};

} // namespace daw::graph
