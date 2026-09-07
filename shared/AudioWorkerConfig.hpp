#pragma once
#include <cstdint>
#include <memory>

namespace daw::rt {
// Owned on control threads and copied by workers only between render passes.
// The platform layer retains the native workgroup; the graph stays portable.
struct AudioWorkerConfig {
    bool active = false;
    double sampleRate = 48000;
    std::uint32_t blockFrames = 512;
    std::shared_ptr<void> workgroup;
};
} // namespace daw::rt
