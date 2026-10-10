#include "SampleLoader.hpp"
#include "DSP/Resampler.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"
#include <limits>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace daw {
namespace {
std::shared_ptr<const SampleLoader> sourceLoader;
struct CachedSource {
    std::weak_ptr<const engine::SampleBuffer> sample;
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes = 0;
};
std::mutex sourceMutex;
std::unordered_map<std::string, CachedSource> sourceCache;
}
void setSampleLoader(SampleLoader loader) {
    std::atomic_store(&sourceLoader, loader ? std::make_shared<const SampleLoader>(std::move(loader)) : nullptr);
}
audio::Result loadSampleBuffer(const std::string& path,
    std::shared_ptr<const engine::SampleBuffer>& out,
    const std::function<bool()>& keepGoing) {
    try {
        if (keepGoing && !keepGoing())
            return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
        std::error_code error;
        const auto file = platform::pathFromUtf8(path);
        const auto modified = std::filesystem::last_write_time(file, error);
        const auto bytes = error ? 0 : std::filesystem::file_size(file, error);
        if (!error) {
            const std::lock_guard lock(sourceMutex);
            const auto cached = sourceCache.find(path);
            if (cached != sourceCache.end() && cached->second.modified == modified && cached->second.bytes == bytes)
                if (auto sample = cached->second.sample.lock()) { out = std::move(sample); return audio::Result::ok(); }
        }
        std::shared_ptr<const engine::SampleBuffer> decoded;
        if (const auto loader = std::atomic_load(&sourceLoader)) {
            if (const auto result = (*loader)(path, decoded, keepGoing); !result) return result;
        } else {
            audio::platform::AudioFileReader reader;
            if (const auto opened = reader.open(path, keepGoing); !opened) return opened;
            const auto info = reader.info();
            if (!info.channels || info.channels > engine::kMaxChannels ||
                info.frames > std::numeric_limits<engine::FrameCount>::max())
                return audio::Result::fail(audio::EngineError::UnsupportedFormat, "audio file exceeds supported dimensions");
            auto buffer = std::make_shared<engine::SampleBuffer>(
                engine::ChannelCount(info.channels), engine::FrameCount(info.frames), info.sampleRate);
            constexpr std::size_t block = 8192;
            std::vector<float> scratch(block * info.channels);
            audio::FrameCount position = 0;
            while (position < info.frames) {
                if (keepGoing && !keepGoing())
                    return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
                const auto count = std::min<audio::FrameCount>(block, info.frames - position);
                const auto read = reader.read(scratch.data(), count);
                if (const auto status = reader.readStatus(); !status) return status;
                if (!read) {
                    if (info.frameCountIsEstimate && position > 0) break;
                    return audio::Result::fail(audio::EngineError::UnsupportedFormat, "truncated audio file");
                }
                for (engine::ChannelCount ch = 0; ch < info.channels; ++ch) {
                    float* destination = buffer->writableChannel(ch) + position;
                    for (std::size_t frame = 0; frame < read; ++frame)
                        destination[frame] = scratch[frame * info.channels + ch];
                }
                position += read;
            }
            buffer->trimFrames(engine::FrameCount(position));
            decoded = std::move(buffer);
        }
        if (!decoded) return audio::Result::fail(audio::EngineError::UnsupportedFormat, "decoder returned no audio");
        if (!error) {
            const std::lock_guard lock(sourceMutex);
            if (sourceCache.size() >= 4096) std::erase_if(sourceCache, [](const auto& entry) { return entry.second.sample.expired(); });
            if (sourceCache.size() >= 4096) sourceCache.erase(sourceCache.begin());
            sourceCache[path] = CachedSource{decoded, modified, bytes};
        }
        out = std::move(decoded);
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
