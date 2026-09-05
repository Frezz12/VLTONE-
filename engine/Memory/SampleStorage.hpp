#pragma once
#include <cstddef>
#include <memory>
#include <vector>

namespace daw::engine {
// Large immutable samples have file-backed pages. The working set follows the
// portions being played instead of pinning every imported recording in RAM.
class SampleStorage {
public:
    explicit SampleStorage(std::size_t samples);
    ~SampleStorage();
    SampleStorage(const SampleStorage&) = delete;
    SampleStorage& operator=(const SampleStorage&) = delete;
    float* data() noexcept;
    const float* data() const noexcept;
    bool fileBacked() const noexcept;
private:
    struct Mapping;
    std::unique_ptr<Mapping> m_mapping;
    std::vector<float> m_memory;
};
} // namespace daw::engine
