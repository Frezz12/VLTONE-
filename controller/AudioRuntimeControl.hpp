#pragma once

#include "Common/Types.hpp"
#include "Device/AudioDeviceManager.hpp"
#include "Transport/Transport.hpp"
#include "RealtimeMetrics.hpp"

#include <array>
#include <optional>
#include <vector>

namespace daw {

/// Control-thread messages contain values, never a borrowed Transport or node.
struct AudioTransportCommand {
    enum class Action {
        Play, StartPlayback, Pause, Stop, Record, Seek, SeekSeconds,
        Tempo, TimeSignature, LoopRange, LoopEnabled, Duration
    };
    Action action = Action::Stop;
    engine::SamplePos position = 0, end = 0;
    double value = 0;
    int numerator = 4, denominator = 4;
    bool enabled = false, prepare = false;
};

struct AudioTransportSnapshot {
    engine::TransportState state = engine::TransportState::Stopped;
    engine::SamplePos position = 0, duration = 0, loopStart = 0, loopEnd = 0;
    double positionSeconds = 0, presentationSeconds = 0, tempo = 120;
    bool playing = false, recording = false, loopEnabled = false;
};

struct AudioMeterSnapshot { float left = 0, right = 0, hold = 0; };

struct AudioRuntimeDiagnostics {
    float load = 0;
    std::uint32_t latency = 0;
    int lastRenderError = 0;
    std::uint64_t failedBlocks = 0, gatedBlocks = 0, droppedProfileEvents = 0;
    unsigned workers = 0, realtimeWorkers = 0, workgroupWorkers = 0;
};

struct AudioDeviceSnapshot {
    bool running = false, hasStream = false, inputUsesDeviceTime = false;
    audio::AudioDeviceState state = audio::AudioDeviceState::Created;
    double sampleRate = 0;
    std::uint32_t bufferSize = 0;
    std::uint64_t callbacks = 0, renderCalls = 0, lastCallbackNs = 0;
    int lastRenderStatus = 0;
    std::array<std::uint64_t, 4> xruns{};
};

/// An owned diagnostics batch. Draining remains bounded and off the callback.
struct AudioTimingSnapshot {
    rt::TimingCounters counters;
    std::vector<rt::BlockTiming> events;
};

enum class AudioFaderTarget { Channel, Sampler, Clip };
struct AudioFaderChange {
    std::optional<float> gain, pan;
    std::optional<bool> silent, mono;
};

struct AudioPreviewCommand {
    enum class Action { Stop, Loop, Gain, SeekSeconds };
    Action action = Action::Stop;
    double value = 0;
};
struct AudioPreviewSnapshot { bool playing = false; double positionSeconds = 0; };

} // namespace daw
