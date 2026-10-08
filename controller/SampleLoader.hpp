#pragma once
#include "Audio/SampleBuffer.hpp"
#include "Core/Result.hpp"
#include <functional>
#include <string>

namespace daw {
using SampleLoader = std::function<audio::Result(const std::string&,
    std::shared_ptr<const engine::SampleBuffer>&, const std::function<bool()>&)>;
void setSampleLoader(SampleLoader loader);
// Bounded scratch decoding. Large results use SampleBuffer's disk backing.
audio::Result loadSampleBuffer(const std::string& path,
    std::shared_ptr<const engine::SampleBuffer>& out,
    const std::function<bool()>& keepGoing = {});
audio::Result convertSampleBuffer(std::shared_ptr<const engine::SampleBuffer> source,
    double rate, std::shared_ptr<const engine::SampleBuffer>& out,
    const std::function<bool()>& keepGoing = {});
} // namespace daw
