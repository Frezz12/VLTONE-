#include "SampleLoader.hpp"
#include "DSP/Resampler.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"
#include <limits>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace daw {
namespace {
struct CachedSource {
    std::weak_ptr<const engine::SampleBuffer> sample;
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes = 0;
};
std::mutex sourceMutex;
std::unordered_map<std::string, CachedSource> sourceCache;
}
audio::Result loadSampleBuffer(const std::string& path,
    std::shared_ptr<const engine::SampleBuffer>& out,
    const std::function<bool()>& keepGoing) {
    try {
        if (keepGoing && !keepGoing())
            return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
        audio::platform::AudioFileReader reader;
        if (const auto opened = reader.open(path); !opened) return opened;
        const auto info = reader.info();
        std::error_code ec;
        const auto file = platform::pathFromUtf8(path);
        const auto modified = std::filesystem::last_write_time(file, ec);
        const auto bytes = ec ? 0 : std::filesystem::file_size(file, ec);
        if (!ec) {
            const std::lock_guard lock(sourceMutex);
            const auto cached = sourceCache.find(path);
            if (cached != sourceCache.end() && cached->second.modified == modified &&
                cached->second.bytes == bytes) {
                if (auto sample = cached->second.sample.lock(); sample &&
                    sample->frames() == info.frames && sample->channels() == info.channels &&
                    sample->sampleRate() == info.sampleRate) {
                    out = std::move(sample);
                    return audio::Result::ok();
                }
            }
        }
        if (info.channels > engine::kMaxChannels ||
            info.frames > std::numeric_limits<engine::FrameCount>::max())
            return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                       "audio file exceeds supported dimensions");
        auto buffer = std::make_shared<engine::SampleBuffer>(
            engine::ChannelCount(info.channels), engine::FrameCount(info.frames), info.sampleRate);
        constexpr std::size_t block = 8192;
        std::vector<float> scratch(block * info.channels);
        for (audio::FrameCount position = 0; position < info.frames;) {
            if (keepGoing && !keepGoing())
                return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
            const auto count = std::min<audio::FrameCount>(block, info.frames - position);
            const auto read = reader.read(scratch.data(), count);
            if (read == 0)
                return audio::Result::fail(audio::EngineError::UnsupportedFormat, "truncated audio file");
            for (engine::ChannelCount ch = 0; ch < info.channels; ++ch) {
                float* destination = buffer->writableChannel(ch) + position;
                for (std::size_t frame = 0; frame < read; ++frame)
                    destination[frame] = scratch[frame * info.channels + ch];
            }
            position += read;
        }
        out = std::move(buffer);
        if (!ec) {
            const std::lock_guard lock(sourceMutex);
            // Weak ownership shares decoded sampler/clip/preview sources
            // across controllers without retaining their PCM after use.
            if (sourceCache.size() >= 4096) sourceCache.erase(sourceCache.begin());
            sourceCache[path] = CachedSource{out, modified, bytes};
        }
        return audio::Result::ok();
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat, error.what());
    }
}
audio::Result convertSampleBuffer(std::shared_ptr<const engine::SampleBuffer> source,
    double rate, std::shared_ptr<const engine::SampleBuffer>& out,
    const std::function<bool()>& keepGoing) {
    if (!source || !std::isfinite(rate) || rate < 1000 || rate > 768000)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "invalid sample rate");
    if (std::abs(source->sampleRate() - rate) <= 0.01) {
        out = std::move(source);
        return audio::Result::ok();
    }
    try {
        const auto frames = engine::dsp::resampledFrameCount(
            source->frames(), source->sampleRate(), rate);
        if (frames > std::numeric_limits<engine::FrameCount>::max())
            return audio::Result::fail(audio::EngineError::UnsupportedFormat, "resampled file is too long");
        auto result = std::make_shared<engine::SampleBuffer>(source->channels(),
            engine::FrameCount(frames), rate);
        const bool completed = engine::dsp::resampleFrames(
            source->channels(), source->frames(), source->sampleRate(), rate,
            [&](std::size_t frame, std::size_t ch) { return source->channel(ch)[frame]; },
            [&](std::size_t frame, std::size_t ch, float value) {
                result->writableChannel(ch)[frame] = value;
            }, [&] { return !keepGoing || keepGoing(); });
        if (!completed)
            return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
        out = std::move(result);
        return audio::Result::ok();
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat, error.what());
    }
}
} // namespace daw
