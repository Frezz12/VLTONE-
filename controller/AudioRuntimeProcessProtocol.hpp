#pragma once
#include "AudioInputClock.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace daw::audioipc {
inline constexpr std::uint32_t kMagic = 0x41544c56, kVersion = 11;
inline constexpr std::size_t kCapacity = 4u * 1024u * 1024u;
// Leave capacity for the runtime's own short-lived reconciliation lease.
inline constexpr std::size_t kMaxTransactions = 32;
/// One control-thread writer in each process. Release/acquire publication
/// protects the bounded byte payload. The audio thread touches only its own
/// separate clock slot, never the request/response buffers.
struct Mailbox {
    std::uint32_t magic = kMagic, version = kVersion, bytes = sizeof(Mailbox);
    std::atomic<std::uint64_t> request{0}, response{0};
    // Advanced only by completed session preparation steps on the control
    // thread. A live timer/audio callback must never keep a stuck loader alive.
    std::atomic<std::uint64_t> preparationProgress{0};
    // One bounded nested plugin RPC, published once at send and cleared on
    // return. It cannot renew the preparation lease while that RPC is stuck.
    std::atomic<std::int64_t> pluginControlDeadlineNs{0};
    std::uint32_t requestSize = 0, responseSize = 0;
    InputClockSlot inputClocks[kInputClockSlots];
    std::uint8_t input[kCapacity]{}, output[kCapacity]{};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
} // namespace daw::audioipc
