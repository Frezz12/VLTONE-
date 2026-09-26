#include "StripSilence.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace daw {

StripSilenceSettings sanitizedStripSilenceSettings(StripSilenceSettings s) {
    const auto bound = [](double value, double low, double high, double fallback) {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    s.thresholdDb = bound(s.thresholdDb, -96, 0, -42);
    s.hysteresisDb = bound(s.hysteresisDb, 0, 24, 6);
    s.minimumSilenceMs = bound(s.minimumSilenceMs, 1, 5000, 150);
    s.minimumSoundMs = bound(s.minimumSoundMs, 0, 2000, 30);
    s.preRollMs = bound(s.preRollMs, 0, 2000, 10);
    s.postRollMs = bound(s.postRollMs, 0, 3000, 50);
    s.fadeMs = bound(s.fadeMs, 0, 100, 3);
    s.gridBeats = bound(s.gridBeats, 0, 16, .25);
    return s;
}

SilenceEnvelope buildSilenceEnvelope(const engine::ClipPlayerNode::ClipList& placements,
    double duration, double rate, const std::function<bool()>& keepGoing, const std::string& renderPath) {
    if (!(duration > 0) || !std::isfinite(duration) || !(rate > 0) ||
        !std::isfinite(rate) || placements.empty())
        throw std::runtime_error("Source audio is unavailable.");
    SilenceEnvelope result;
    result.durationSeconds = duration;
    // Bound envelope memory for very long stems without allocating full PCM.
    const auto bucket = engine::SamplePos(std::max(1.0,
        std::ceil(std::max(.001, duration / 4000000.0) * rate)));
    result.stepSeconds = double(bucket) / rate;
    const auto total = engine::SamplePos(std::ceil(duration * rate));
    result.peaks.resize(std::size_t((total + bucket - 1) / bucket), 0);
    constexpr engine::FrameCount block = 2048;
    engine::ChannelCount channels = 2;
    for (const auto& placement : placements)
        if (placement.audio) channels = std::max(channels, placement.audio->channels());
    std::vector<float> scratch(std::size_t(channels) * block);
    std::vector<float*> planes(channels);
    for (engine::ChannelCount ch = 0; ch < channels; ++ch)
        planes[ch] = scratch.data() + std::size_t(ch) * block;
    engine::ClipPlayerNode player("Strip Silence analysis");
    player.prepare({rate, block, channels, true});
    player.setClips(std::make_shared<const engine::ClipPlayerNode::ClipList>(placements));
    audio::platform::AudioFileWriter writer;
    if (!renderPath.empty()) {
        const auto result = writer.open(renderPath, rate, channels, std::uint64_t(total));
        if (!result) throw std::runtime_error(result.message());
    }
    for (engine::SamplePos at = 0; at < total;) {
        if (keepGoing && !keepGoing()) return {};
        const auto frames = engine::FrameCount(std::min<engine::SamplePos>(block, total - at));
        engine::ProcessContext context;
        context.output = engine::AudioBlock(planes.data(), channels, frames);
        context.frames = frames;
        context.timelinePosition = at;
        context.sampleRate = rate;
        context.playing = true;
        context.offline = true;
        player.process(context);
        if (!renderPath.empty()) {
            const auto result = writer.write(planes.data(), frames);
            if (!result) throw std::runtime_error(result.message());
        }
        for (engine::ChannelCount ch = 0; ch < channels; ++ch) {
            for (engine::FrameCount i = 0; i < frames; ++i) {
                const float amplitude = std::abs(planes[ch][i]);
                if (!std::isfinite(amplitude))
                    throw std::runtime_error("The audio contains non-finite samples.");
                auto& peak = result.peaks[std::size_t((at + i) / bucket)];
                peak = std::max(peak, amplitude);
            }
        }
        at += frames;
    }
    if (!renderPath.empty()) {
        const auto result = writer.close();
        if (!result) throw std::runtime_error(result.message());
    }
    return result;
}

std::vector<SilenceRegion> detectSilenceRegions(const SilenceEnvelope& env,
    StripSilenceSettings settings, double start, double tempo) {
    const auto s = sanitizedStripSilenceSettings(settings);
    std::vector<SilenceRegion> regions;
    if (env.peaks.empty() || !(env.stepSeconds > 0) || !(env.durationSeconds > 0)) return regions;
    const double open = std::pow(10.0, s.thresholdDb / 20);
    const double close = std::pow(10.0, (s.thresholdDb - s.hysteresisDb) / 20);
    double begin = 0, lastSoundEnd = 0;
    bool active = false;
    const auto finish = [&] {
        if (lastSoundEnd - begin + 1e-9 >= s.minimumSoundMs / 1000)
            regions.push_back({begin, std::min(lastSoundEnd, env.durationSeconds)});
        active = false;
    };
    for (std::size_t i = 0; i < env.peaks.size(); ++i) {
        const double time = i * env.stepSeconds;
        if (!active && env.peaks[i] >= open) { active = true; begin = time; }
        if (!active) continue;
        if (env.peaks[i] >= close) lastSoundEnd = std::min(env.durationSeconds, time + env.stepSeconds);
        if (time + env.stepSeconds - lastSoundEnd + 1e-9 >= s.minimumSilenceMs / 1000)
            finish();
    }
    if (active) finish();
    if (!s.splitInternal && !regions.empty())
        regions = {{regions.front().begin, regions.back().end}};

    const double grid = tempo > 0 ? s.gridBeats * 60 / tempo : 0;
    std::vector<SilenceRegion> padded;
    for (auto region : regions) {
        region.begin = std::max(0.0, region.begin - s.preRollMs / 1000);
        region.end = std::min(env.durationSeconds, region.end + s.postRollMs / 1000);
        if (grid > 1e-9) {
            region.begin = std::max(0.0, std::floor((start + region.begin) / grid + 1e-9) * grid - start);
            region.end = std::min(env.durationSeconds, std::ceil((start + region.end) / grid - 1e-9) * grid - start);
        }
        // Padding/snapping may close a gap. Merge it instead of generating
        // overlapping clips or slivers that would play the same source twice.
        if (!padded.empty() && region.begin <= padded.back().end + 1e-9)
            padded.back().end = std::max(padded.back().end, region.end);
        else if (region.end > region.begin) padded.push_back(region);
    }
    return padded;
}

ClipModel sliceSilenceRegion(const ClipModel& source, SilenceRegion region,
    double duration, double fadeMs, double tempo) {
    ClipModel clip = source;
    clip.id = newUuid();
    clip.startSeconds += region.begin;
    clip.durationSeconds = region.end - region.begin;
    const double stretch = std::max(.001, source.sampleEdit.stretchTime);
    clip.offsetSeconds += region.begin / stretch;
    clip.musicalAnalysis = {};
    for (auto& insert : clip.inserts) insert.id = newUuid();
    std::unordered_map<std::string, std::string> takeIds;
    for (auto& take : clip.takes) {
        const auto old = take.id;
        take.id = newUuid();
        takeIds[old] = take.id;
        take.clipOffsetSeconds -= region.begin;
    }
    for (auto& segment : clip.comp) {
        segment.id = newUuid();
        segment.takeId = takeIds[segment.takeId];
        segment.startSeconds = std::max(0.0, segment.startSeconds - region.begin);
        segment.endSeconds = std::min(clip.durationSeconds, segment.endSeconds - region.begin);
    }
    std::erase_if(clip.comp, [](const auto& part) { return part.endSeconds <= part.startSeconds; });
    if (!source.warp.empty()) {
        const double first = source.warp.enabled ? secondsToBeats(region.begin, tempo) :
            warpBeatAt(source.warp, source.offsetSeconds + region.begin / stretch);
        const double last = source.warp.enabled ? secondsToBeats(region.end, tempo) :
            warpBeatAt(source.warp, source.offsetSeconds + region.end / stretch);
        clip.warp = sliceWarp(source.warp, first, last);
        clip.offsetSeconds = clip.warp.markers.front().sourceSeconds;
        for (auto& marker : clip.warp.markers) marker.id = newUuid();
    }
    const double fade = std::min(std::max(0.0, fadeMs) / 1000, clip.durationSeconds / 2);
    if (region.begin > 1e-9) {
        clip.fadeInSeconds = fade;
        clip.fadeInCurve = 0;
        clip.fadeInMode = ClipFadeMode::Gain;
    } else clip.fadeInSeconds = std::min(clip.fadeInSeconds, clip.durationSeconds / 2);
    if (region.end < duration - 1e-9) {
        clip.fadeOutSeconds = fade;
        clip.fadeOutCurve = 0;
        clip.fadeOutMode = ClipFadeMode::Gain;
    } else clip.fadeOutSeconds = std::min(clip.fadeOutSeconds, clip.durationSeconds / 2);
    return clip;
}

} // namespace daw
