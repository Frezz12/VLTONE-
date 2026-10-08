#pragma once

#include "Audio/SampleBuffer.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace daw {

/// Immutable planar float32 little-endian files in a parent-owned private job
/// directory. The parent retains that directory until every child has stopped.
/// Serialization belongs to the caller; records contain values, never pointers.
class ProcessAudioResources {
public:
    enum class Retention { Strong, WeakSources };
    struct Record {
        std::string id, fileName;
        std::uint32_t channels = 0, frames = 0;
        double sampleRate = 0;
    };
    using Cancelled = std::function<bool()>;
    using Samples = std::unordered_map<std::string, std::shared_ptr<const engine::SampleBuffer>>;
    struct Cache {
        std::filesystem::path root;
        Samples samples;
    };

    explicit ProcessAudioResources(std::filesystem::path root, Cancelled cancelled = {},
        Retention retention = Retention::Strong);
    /// Null maps to the empty ID. Session banks pin input buffers. Readout
    /// banks track weak ownership and never alias an expired source's address.
    std::string put(const std::shared_ptr<const engine::SampleBuffer>& samples);
    /// Readout banks only: call before the NEXT request, after the preceding
    /// reply has been decoded. Already-open readonly mappings survive unlink.
    /// Failed removals remain registered for another attempt.
    void collectExpired();
    const std::vector<Record>& records() const noexcept { return m_records; }
    /// Validates the complete manifest before mapping. One shared read-only
    /// mapping per ID; multiple session references reuse the returned pointers.
    static Samples load(const std::filesystem::path& root, const std::vector<Record>& records,
                        Cancelled cancelled = {}, Cache* cache = nullptr);
private:
    std::filesystem::path m_root;
    Cancelled m_cancelled;
    Retention m_retention;
    std::uint64_t m_nextId = 0;
    std::vector<Record> m_records;
    std::unordered_map<const engine::SampleBuffer*, std::string> m_ids;
    std::vector<std::shared_ptr<const engine::SampleBuffer>> m_pinned;
    struct WeakSource {
        const engine::SampleBuffer* address = nullptr;
        std::weak_ptr<const engine::SampleBuffer> source;
    };
    std::unordered_map<std::string, WeakSource> m_weakSources;
};

} // namespace daw
