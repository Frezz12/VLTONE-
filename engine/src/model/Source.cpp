#include "daw/model/Source.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <sndfile.h>

namespace daw::model {

bool Source::loadFromFile(const std::string& path)
{
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f)
        return false;

    filePath_ = path;
    channels_ = info.channels;
    sampleRate_ = info.samplerate;
    totalFrames_ = info.frames;

    const auto totalSamples =
        static_cast<std::size_t>(info.frames) * static_cast<std::size_t>(info.channels);

    std::vector<double> tmp(totalSamples);

    sf_count_t read = sf_readf_double(f, tmp.data(), info.frames);
    sf_close(f);

    if (read != info.frames) {
        channels_ = 0;
        sampleRate_ = 0;
        totalFrames_ = 0;
        return false;
    }

    data_.resize(totalSamples);
    for (std::size_t i = 0; i < totalSamples; ++i)
        data_[i] = static_cast<float>(tmp[i]);

    return true;
}

const float* Source::readFrames(std::int64_t offset, int /*numFrames*/) const noexcept
{
    if (!isValid() || offset < 0 || offset >= totalFrames_)
        return nullptr;

    const auto idx = static_cast<std::size_t>(offset) * static_cast<std::size_t>(channels_);
    if (idx >= data_.size())
        return nullptr;

    return data_.data() + idx;
}

double Source::durationSeconds() const noexcept
{
    if (sampleRate_ <= 0 || totalFrames_ <= 0)
        return 0.0;
    return static_cast<double>(totalFrames_) / static_cast<double>(sampleRate_);
}

} // namespace daw::model
