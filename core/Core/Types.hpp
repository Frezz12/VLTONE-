#pragma once

#include "Result.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>

namespace audio {

using Sample = float;
using SampleRate = double;
using BufferSize = uint32_t;
using ChannelCount = uint32_t;
using FrameCount = uint64_t;
using TimeSec = double;
using TimeSamples = int64_t;
using TrackID = uint32_t;
using ClipID = uint32_t;
using PluginID = uint32_t;
using BusID = uint32_t;
using BeatCount = double;
using TickCount = int64_t;

constexpr Sample kMaxSampleValue = 1.0f;
constexpr Sample kMinSampleValue = -1.0f;
constexpr int kMaxChannels = 64;
constexpr int kDefaultBufferSize = 512;
constexpr double kDefaultSampleRate = 44100.0;
constexpr double kMinSampleRate = 8000.0;
constexpr double kMaxSampleRate = 384000.0;
constexpr double kDefaultTempo = 120.0;
constexpr int kDefaultBeatsPerBar = 4;
constexpr int kDefaultBeatUnit = 4;
constexpr int kTicksPerQuarterNote = 960;

enum class EngineState {
    Uninitialized,
    Initialized,
    Running,
    Error
};

enum class TransportState {
    Stopped,
    Playing,
    Recording,
    Paused
};

enum class TrackType {
    Audio,
    Midi,
    Bus,
    Master,
    Folder
};

enum class BusType {
    Master,
    Aux,
    Group,
    Sidechain,
    External
};

enum class PluginType {
    Unknown,
    AudioUnit,
    VST3
};

enum class SampleFormat {
    Unknown,
    Float32,
    Float64,
    Int16,
    Int24,
    Int32
};

struct AudioTime {
    TimeSec seconds = 0.0;
    TimeSamples samples = 0;
    SampleRate sampleRate = kDefaultSampleRate;

    void setFromSeconds(TimeSec s) {
        seconds = s;
        samples = static_cast<TimeSamples>(s * sampleRate);
    }

    void setFromSamples(TimeSamples s) {
        samples = s;
        seconds = static_cast<TimeSec>(s) / sampleRate;
    }
};

inline TimeSec samplesToSeconds(TimeSamples samples, SampleRate rate) {
    return static_cast<TimeSec>(samples) / rate;
}

inline TimeSamples secondsToSamples(TimeSec seconds, SampleRate rate) {
    return static_cast<TimeSamples>(seconds * rate);
}

struct RenderResult {
    float progress = 0.0f;
    bool completed = false;
    bool cancelled = false;
    std::string outputPath;
    Result result = Result::ok();
};

} // namespace audio
