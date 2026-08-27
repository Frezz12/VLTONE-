#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

/// The rebuilt audio engine. Everything here is graph-oriented and
/// framework-free: the realtime path owns no locks, allocates nothing, and
/// knows nothing about tracks, projects or UI — those are expressed as nodes.
namespace daw::engine {

using SampleRate  = double;
using FrameCount  = std::uint32_t;
using ChannelCount = std::uint16_t;
using SamplePos   = std::int64_t;

/// Handle into the graph's dense node array. Stable while the graph lives.
using NodeId = std::uint32_t;
inline constexpr NodeId kInvalidNode = 0xFFFF'FFFFu;

inline constexpr std::size_t kCacheLine = 64;
inline constexpr ChannelCount kMaxChannels = 32;
inline constexpr FrameCount kMaxBlockSize = 8192;

enum class EngineError : std::uint8_t {
    InvalidArgument,
    UnknownNode,
    CycleDetected,
    ChannelMismatch,
    NotCompiled,
    BlockTooLarge,
    OutOfCapacity,
};

constexpr std::string_view describe(EngineError error) noexcept {
    switch (error) {
        case EngineError::InvalidArgument: return "invalid argument";
        case EngineError::UnknownNode:     return "unknown node";
        case EngineError::CycleDetected:   return "the routing contains a feedback cycle";
        case EngineError::ChannelMismatch: return "channel layouts do not match";
        case EngineError::NotCompiled:     return "graph has not been compiled";
        case EngineError::BlockTooLarge:   return "block size exceeds the prepared maximum";
        case EngineError::OutOfCapacity:   return "out of preallocated capacity";
    }
    return "unknown error";
}

template <class T>
using Result = std::expected<T, EngineError>;
using Status = std::expected<void, EngineError>;

[[nodiscard]] constexpr std::unexpected<EngineError> fail(EngineError e) noexcept {
    return std::unexpected(e);
}

/// Musical time for one block, as every plugin format wants to be told it.
///
/// "ppq" is quarter notes from the timeline origin — the unit VST3, CLAP and AU
/// all speak, and the one the document already stores notes in. A beat here is
/// always a quarter note regardless of the time signature's denominator, so
/// `tempo` means quarter notes per minute even in 6/8.
struct TransportInfo {
    double tempo = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    /// Position at the *start* of the block. Every node in the block sees the
    /// same value, exactly as with `timelinePosition`.
    double ppqPosition = 0.0;
    /// Where the bar containing `ppqPosition` began. What a plugin needs to
    /// line a synced delay or an arpeggiator up with the bar.
    double barStartPpq = 0.0;
    double loopStartPpq = 0.0;
    double loopEndPpq = 0.0;
    bool looping = false;
    bool recording = false;
};

/// A multi-channel, non-owning view of planar (channel-major) audio. Planar
/// keeps each channel contiguous, which is what SIMD kernels and plugin APIs
/// both want, and avoids the de-interleave shuffling of a packed layout.
class AudioBlock {
public:
    AudioBlock() = default;
    AudioBlock(float* const* channels, ChannelCount numChannels, FrameCount frames)
        : m_channels(channels), m_numChannels(numChannels), m_frames(frames) {}

    ChannelCount numChannels() const noexcept { return m_numChannels; }
    FrameCount frames() const noexcept { return m_frames; }
    bool isValid() const noexcept { return m_channels != nullptr && m_numChannels > 0; }

    /// View semantics, like std::span: a const AudioBlock is a const *handle*,
    /// not a promise that the samples behind it are read-only. Nodes receive
    /// their output block by const reference and still write through it.
    std::span<float> channel(ChannelCount index) const noexcept {
        return {m_channels[index], m_frames};
    }
    float* data(ChannelCount index) const noexcept { return m_channels[index]; }

    /// Same buffers, fewer frames — used when a block is processed in slices.
    AudioBlock withFrames(FrameCount frames) const noexcept {
        return AudioBlock(m_channels, m_numChannels, frames);
    }

private:
    float* const* m_channels = nullptr;
    ChannelCount m_numChannels = 0;
    FrameCount m_frames = 0;
};

} // namespace daw::engine
