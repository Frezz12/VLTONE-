#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace daw::engine {
// Large immutable samples have file-backed pages. The working set follows the
// portions being played instead of pinning every imported recording in RAM.
class SampleStorage {
public:
    explicit SampleStorage(std::size_t samples);
    /// Map an existing, immutable planar PCM file. Exact size is required;
    /// the mapping owns its pages independently of the caller's file handle.
    SampleStorage(const std::filesystem::path& path, std::size_t samples);
    ~SampleStorage();
    SampleStorage(const SampleStorage&) = delete;
    SampleStorage& operator=(const SampleStorage&) = delete;
    float* data() noexcept;
    const float* data() const noexcept;
    bool fileBacked() const noexcept;
    bool readOnly() const noexcept;
private:
    struct Mapping;
    std::unique_ptr<Mapping> m_mapping;
    std::vector<float> m_memory;
};
} // namespace daw::engine
