#pragma once

#include "PluginProcess.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace daw::plugins::ipc {

inline constexpr std::uint32_t kMagic = 0x564c5450; // VLTP
inline constexpr std::uint32_t kVersion = 6;
inline constexpr std::uint32_t kNoticeCapacity = 2048;
struct Notice {
    std::uint32_t kind = 0, parameter = 0; // 0 value, 1 begin, 2 end
    double value = 0;
};
static_assert(sizeof(Notice) == 16 && std::is_trivially_copyable_v<Notice>);
inline constexpr std::size_t kMaxMappingBytes = 512u * 1024u * 1024u;

// Fixed-width wire records. SDK/local C++ objects, pointers, spans and bool
// representations never cross this boundary. Both sides check the ABI sizes.
struct Event {
    std::uint32_t kind, frame, order, parameter;
    std::int32_t channel, key, note;
    std::uint32_t flags;
    double value, pan;
    double pitch[9];
    std::uint32_t pitchFrames, pitchShape;
};
static_assert(sizeof(Event) == 128 && std::is_trivially_copyable_v<Event>);

struct Block {
    std::uint32_t frames = 0, inputs = 0, outputs = 0, sidechains = 0;
    std::uint32_t inputEvents = 0, outputEvents = 0, disposition = 0, notices = 0;
    std::uint32_t numerator = 4, denominator = 4, flags = 0, reserved = 0;
    std::uint64_t inputSilence = 0, sidechainSilence = 0;
    std::int64_t sampleTime = 0, steadyTime = 0;
    double tempo = 120, ppq = 0, bar = 0, loopStart = 0, loopEnd = 0;
};
static_assert(sizeof(Block) == 120 && std::is_trivially_copyable_v<Block>);

enum class Operation : std::uint32_t { Control = 1, Audio = 2 };
struct alignas(64) Header {
    std::uint32_t magic = kMagic, version = kVersion;
    std::uint32_t eventBytes = sizeof(Event), blockBytes = sizeof(Block);
    PluginProcessLimits limits;
    std::atomic<std::uint32_t> notices{0}, mainThreadRequest{0};
    std::atomic<std::uint32_t> latency{0}, tail{0}, tailKnown{0};
    std::byte beforeRequest[12]{};
    alignas(64) std::atomic<std::uint64_t> request{0};
    std::byte beforeResponse[56]{};
    alignas(64) std::atomic<std::uint64_t> response{0};
    std::uint32_t operation = 0, textBytes = 0, blobBytes = 0, reserved = 0;
    Block block;
    // Written after audio/events are complete, published by response's release.
    // Uses rt::nowNanos; hello verifies the peer shares our monotonic epoch.
    std::uint64_t completedNanos = 0;
    std::byte trailing[40]{};
    alignas(64) std::atomic<std::uint32_t> noticeWrite{0};
    std::atomic<std::uint32_t> noticeRead{0}, noticeOverflow{0};
    std::atomic<std::uint32_t> editorRequest{0}, editorResponse{0}, editorStatus{0};
    std::atomic<std::uint64_t> controlRequest{0}, audioRequest{0};
    std::atomic<std::uint32_t> automationShortcutEnabled{0}, automationShortcutCount{0};
    // Advanced by the GUI event loop, including responsive nested modal loops.
    std::atomic<std::uint64_t> editorHeartbeat{0};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<PluginProcessFailure>::is_always_lock_free);
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float) == 4 && sizeof(double) == 8 &&
              std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);
static_assert(sizeof(Header) == 384 && offsetof(Header, request) == 64 &&
              offsetof(Header, response) == 128 && offsetof(Header, block) == 152);

struct Layout {
    PluginProcessLimits limits;
    std::size_t bytes = 0, audio = 0, events = 0, text = 0, blob = 0, notices = 0;
    explicit Layout(PluginProcessLimits l) : limits(l) {
        if (!l.frames || l.frames > engine::kMaxBlockSize || !l.channels ||
            l.channels > engine::kMaxChannels || !l.events || l.events > 2u * 1024u * 1024u ||
            l.controlBytes < 4096 || l.controlBytes > 16u * 1024u * 1024u) return;
        audio = sizeof(Header);
        events = audio + std::size_t(l.frames) * l.channels * 3 * sizeof(float);
        events = (events + alignof(Event) - 1) & ~(alignof(Event) - 1);
        text = events + std::size_t(l.events) * sizeof(Event) * 2;
        blob = text + l.controlBytes;
        notices = (blob + l.controlBytes + alignof(Notice) - 1) & ~(alignof(Notice) - 1);
        const auto total = notices + kNoticeCapacity * sizeof(Notice);
        if (total <= kMaxMappingBytes) bytes = total;
    }
};

struct View {
    std::byte* data;
    Layout layout; // trusted local copy, never refreshed from a child response
    Header& header() const noexcept { return *reinterpret_cast<Header*>(data); }
    Notice* notices() const noexcept { return reinterpret_cast<Notice*>(data + layout.notices); }
    float* channel(unsigned bus, unsigned channel) const noexcept {
        return reinterpret_cast<float*>(data + layout.audio) +
            (std::size_t(bus) * layout.limits.channels + channel) * layout.limits.frames;
    }
    Event* events(bool output) const noexcept {
        return reinterpret_cast<Event*>(data + layout.events) +
            (output ? layout.limits.events : 0);
    }
    char* text() const noexcept { return reinterpret_cast<char*>(data + layout.text); }
    std::uint8_t* blob() const noexcept {
        return reinterpret_cast<std::uint8_t*>(data + layout.blob);
    }
};

Event encode(const PluginEvent& event) noexcept;
bool decode(const Event& wire, PluginEvent& event, std::uint32_t frames) noexcept;
bool validBlock(const Block& block, PluginProcessLimits limits) noexcept;

} // namespace daw::plugins::ipc
