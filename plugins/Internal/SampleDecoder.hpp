#pragma once

#include "Audio/SampleBuffer.hpp"

#include <functional>
#include <memory>
#include <string>

/// How the built-in sampler gets audio off disk.
///
/// A hook rather than a direct call, because decoding lives in `daw_core`
/// (libsndfile, PortAudio) and this library deliberately links neither — the
/// out-of-process scanner links `daw_pluginhost` too, and it has no business
/// pulling in an audio device layer. The application installs the real decoder
/// once at start-up; without it the sampler simply loads nothing, which is what
/// a test that only exercises the parameter surface wants anyway.
namespace daw::plugins::sampler {

using SampleDecodeFn =
    std::function<std::shared_ptr<const engine::SampleBuffer>(const std::string& path)>;

/// Control thread, once at start-up. Passing an empty function removes it.
void setSampleDecoder(SampleDecodeFn decoder);

/// Null when no decoder is installed or the file could not be read.
std::shared_ptr<const engine::SampleBuffer> decodeSample(const std::string& path);

} // namespace daw::plugins::sampler
