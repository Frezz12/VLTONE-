#pragma once

#include "Recording/RecordingEngine.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

using AudioCaptureId = std::uint64_t;

struct AudioCaptureSpec {
    std::string directory;
    std::uint32_t inputChannel = 0, channelCount = 1;
    bool inputEnabled = true;
    std::int64_t startSample = 0;
};

struct AudioCaptureStarted {
    AudioCaptureId id = 0;
    std::string path;
    std::uint32_t peakBucketFrames = 0;
};

struct AudioCaptureStatus {
    bool available = false, recording = false;
    std::int64_t recordedFrames = 0, startSample = 0;
    std::uint32_t peakBucketFrames = 0;
    double sampleRate = 0;
};

/// One bounded read contains both the capture clock and its peaks. No UI
/// request borrows recorder storage or needs one IPC call per peak bucket.
struct AudioCapturePeaks {
    AudioCaptureStatus status;
    std::uint64_t firstBucket = 0;
    std::vector<float> values;
};

} // namespace daw
