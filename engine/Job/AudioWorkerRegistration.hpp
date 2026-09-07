#pragma once
#include "AudioWorkerConfig.hpp"
#include <memory>

namespace daw::engine {
class AudioWorkerRegistration {
public:
    AudioWorkerRegistration();
    ~AudioWorkerRegistration();
    // Calling worker only, outside a render pass. Returns bit 0 for realtime
    // scheduling and bit 1 for workgroup membership. Zero is a usable fallback.
    unsigned configure(const rt::AudioWorkerConfig& config) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace daw::engine
