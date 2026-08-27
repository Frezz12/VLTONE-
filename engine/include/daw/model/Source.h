#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw::model {

class Source {
public:
    Source() = default;

    bool loadFromFile(const std::string& path);

    const std::string& filePath() const noexcept { return filePath_; }
    int channels() const noexcept { return channels_; }
    int sampleRate() const noexcept { return sampleRate_; }
    std::int64_t frames() const noexcept { return totalFrames_; }
    double durationSeconds() const noexcept;

    const float* readFrames(std::int64_t offset, int numFrames) const noexcept;

    bool isValid() const noexcept { return channels_ > 0 && totalFrames_ > 0 && !data_.empty(); }

private:
    std::string    filePath_;
    int            channels_ = 0;
    int            sampleRate_ = 0;
    std::int64_t   totalFrames_ = 0;
    std::vector<float> data_;
};

} // namespace daw::model
