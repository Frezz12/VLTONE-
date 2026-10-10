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
    struct Record {
        std::string id, fileName;
        std::uint32_t channels = 0, frames = 0;
        double sampleRate = 0;
    };
    using Cancelled = std::function<bool()>;
    using Samples = std::unordered_map<std::string, std::shared_ptr<const engine::SampleBuffer>>;

    explicit ProcessAudioResources(std::filesystem::path root, Cancelled cancelled = {});
    /// Null maps to the empty ID. Input buffers are pinned until the job ends
    /// so repeated references reuse one immutable file and ID.
    std::string put(const std::shared_ptr<const engine::SampleBuffer>& samples);
    const std::vector<Record>& records() const noexcept { return m_records; }
    /// Validates the complete manifest before mapping. One shared read-only
    /// mapping per ID; multiple session references reuse the returned pointers.
    static Samples load(const std::filesystem::path& root, const std::vector<Record>& records,
                        Cancelled cancelled = {});
private:
    std::filesystem::path m_root;
    Cancelled m_cancelled;
    std::uint64_t m_nextId = 0;
    std::vector<Record> m_records;
    std::unordered_map<const engine::SampleBuffer*, std::string> m_ids;
    std::vector<std::shared_ptr<const engine::SampleBuffer>> m_pinned;

};

} // namespace daw
