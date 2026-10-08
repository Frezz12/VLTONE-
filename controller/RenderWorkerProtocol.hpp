#pragma once

#include <atomic>
#include <cstdint>

namespace daw::rendering::ipc {

inline constexpr std::uint32_t kMagic = 0x5652574b, kVersion = 2;

struct ProgressRecord {
    std::uint32_t stage = 0;
    double fraction = 0, rendered = 0, total = 0;
};
inline constexpr std::uint32_t kProgressCapacity = 128;

// Internal control-thread mailbox, never an audio-block transport. Parent owns
// request/cancel, child owns progress/reply. Reply is immutable after done's
// release-store. Addresses and native DSP objects never cross this boundary.
struct Mailbox {
    std::uint32_t magic = kMagic, version = kVersion, bytes = sizeof(Mailbox);
    std::uint32_t requestSize = 0, replySize = 0;
    std::atomic<std::uint32_t> done{0}, cancel{0}, revision{0}, stage{0};
    std::atomic<double> fraction{0}, rendered{0}, total{0};
    // Offline progress is sparse (at most once per 33 ms plus pass endpoints).
    // Retain those endpoints even when a complete pass fits between UI polls.
    std::atomic<std::uint32_t> queuedProgress{0}, progressRead{0}, progressWrite{0};
    ProgressRecord progress[kProgressCapacity]{};
    char request[32768]{};
    char reply[2u * 1024u * 1024u]{};
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);

} // namespace daw::rendering::ipc
