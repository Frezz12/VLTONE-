#include "AudioMusicalAnalysis.hpp"
#include "analysis/Signal.hpp"
#include "DSP/Resampler.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <array>
#include <limits>
#include <stdexcept>

namespace daw::analysis {
namespace {
using detail::checkpoint;
using detail::subProgress;
constexpr std::size_t kDecodeFrames = 65536;

template<class Estimate> bool confidentTempo(const Estimate& e) noexcept {
    return e.calibrated && roundedBpm(e.bpm) >= 1 && roundedBpm(e.bpm) <= 999 &&
           e.status == decltype(e.status)::Available && e.confidence >= 0.98 &&
           e.stability >= 0.85 && !e.variable;
}
template<class Estimate> bool confidentKey(const Estimate& e) noexcept {
    return e.calibrated && e.root >= 0 && e.root < 12 &&
           (e.scale == "major" || e.scale == "natural_minor" || e.scale == "minor") &&
           e.status == decltype(e.status)::Available && e.confidence >= 0.95 && !e.variable;
}

bool validRequest(const MusicalAnalysisRequest& r) {
    return std::isfinite(r.offsetSeconds) && std::isfinite(r.durationSeconds) &&
           std::isfinite(r.stretchTime) && r.stretchTime >= 0.01 && r.stretchTime <= 100.0 &&
           std::isfinite(r.pitchShiftSemitones) && std::abs(r.pitchShiftSemitones) <= 120.0;
}

struct MixStatistics {
    std::vector<double> energy, sum;
    double meanEnergy = 0.0;
    std::size_t frames = 0;
    explicit MixStatistics(int channels) : energy(channels), sum(channels) {}
    void add(const float* input, std::size_t count) {
        const auto channels = energy.size();
        for (std::size_t i = 0; i < count; ++i) {
            double mid = 0.0;
            for (std::size_t c = 0; c < channels; ++c) {
                const double sample = input[i * channels + c];
                if (!std::isfinite(sample)) throw std::invalid_argument("audio contains non-finite samples");
                energy[c] += sample * sample;
                sum[c] += sample;
                mid += sample / double(channels);
            }
            meanEnergy += mid * mid;
        }
        frames += count;
    }
    int selectedChannel() const {
        std::vector<double> ac(energy.size());
        for (std::size_t c = 0; c < ac.size(); ++c)
            ac[c] = std::max(0.0, energy[c] - sum[c] * sum[c] / std::max<std::size_t>(1, frames));
        const auto strongest = std::max_element(ac.begin(), ac.end());
        const double meanSum = std::accumulate(sum.begin(), sum.end(), 0.0) / sum.size();
        const double meanAc = std::max(0.0, meanEnergy - meanSum * meanSum / std::max<std::size_t>(1, frames));
        // Select once using AC energy: a DC offset must not hide cancellation.
        return meanAc < *strongest * 0.05 ? int(strongest - ac.begin()) : -1;
    }
    double dc(int channel) const {
        const double s = channel >= 0 ? sum[std::size_t(channel)]
            : std::accumulate(sum.begin(), sum.end(), 0.0) / double(sum.size());
        return s / double(std::max<std::size_t>(1, frames));
    }
};

float mixSample(const float* frame, int channels, int selected, double dc) {
    if (selected >= 0) return float(frame[selected] - dc);
    double sum = 0.0;
    for (int c = 0; c < channels; ++c) sum += frame[c];
    return float(sum / channels - dc);
}

template<class Read>
std::vector<float> prepareMono(std::size_t frames, double rate, Read read,
                                const AnalysisProgress& progress) {
    std::vector<float> mono(engine::dsp::resampledFrameCount(frames, rate, detail::kAnalysisRate));
    const bool converted = engine::dsp::resampleFrames(1, frames, rate, detail::kAnalysisRate,
        [&](std::size_t frame, std::size_t) { return read(frame); },
        [&](std::size_t frame, std::size_t, float value) {
            mono[frame] = value;
            if ((frame & 16383u) == 0)
                checkpoint(progress, double(frame) / mono.size(), "preparing");
        }, [] { return true; });
    if (!converted) throw std::runtime_error("could not resample analysis audio");
    return mono;
}

void analyzePrepared(std::span<const float> mono, const MusicalAnalysisRequest& request,
                     MusicalAnalysisResult& out, const AnalysisProgress& progress) {
    MusicalAnalysisResult result;
    result.tempo.algorithmVersion = request.detectTempo ? result.kAlgorithmVersion : 0;
    result.key.algorithmVersion = request.detectKey ? result.kAlgorithmVersion : 0;
    result.analyzedSeconds = double(mono.size()) / detail::kAnalysisRate;
    checkpoint(progress, 0.0, "preparing");
    if (request.detectTempo)
        result.tempo = detail::detectTempo(mono, request, subProgress(progress, 0.0, request.detectKey ? 0.55 : 1.0));
    if (request.detectKey)
        result.key = detail::detectKey(mono, request, subProgress(progress, request.detectTempo ? 0.55 : 0.0, 1.0));
    checkpoint(progress, 1.0, "complete");
    out = std::move(result);
}

audio::Result exceptionResult() {
    try { throw; }
    catch (const detail::Cancelled&) {
        return audio::Result::fail(audio::EngineError::Unknown, "analysis cancelled");
    } catch (const std::bad_alloc&) {
        return audio::Result::fail(audio::EngineError::OutOfMemory, "audio analysis ran out of memory");
    } catch (const std::invalid_argument& e) {
        return audio::Result::fail(audio::EngineError::InvalidArgument, e.what());
    } catch (const std::exception& e) {
        return audio::Result::fail(audio::EngineError::Unknown, e.what());
    }
}
} // namespace

audio::Result analyzeAudioSamples(const float* input, std::size_t frames, int channels,
                                  double sampleRate, const MusicalAnalysisRequest& request,
                                  MusicalAnalysisResult& out, const AnalysisProgress& progress) {
    out = {};
    if (!input || !frames || channels <= 0 || channels > 64 ||
        !std::isfinite(sampleRate) || sampleRate < 1000 || sampleRate > 768000 ||
        frames > std::numeric_limits<std::size_t>::max() / std::size_t(channels) || !validRequest(request))
        return audio::Result::fail(audio::EngineError::InvalidArgument, "invalid audio analysis input");
    if (!request.detectTempo && !request.detectKey) return audio::Result::ok();
    try {
        MixStatistics stats(channels);
        for (std::size_t start = 0; start < frames; start += kDecodeFrames) {
            checkpoint(progress, 0.05 * double(start) / frames, "preparing");
            stats.add(input + start * std::size_t(channels), std::min(kDecodeFrames, frames - start));
        }
        const int channel = stats.selectedChannel();
        const double dc = stats.dc(channel);
        const auto mono = prepareMono(frames, sampleRate, [&](std::size_t i) {
            return mixSample(input + i * std::size_t(channels), channels, channel, dc);
        }, subProgress(progress, 0.05, 0.15));
        analyzePrepared(mono, request, out, subProgress(progress, 0.15, 1.0));
        return audio::Result::ok();
    } catch (...) { return exceptionResult(); }
}

audio::Result analyzeAudioFile(const std::string& path, const MusicalAnalysisRequest& request,
                               MusicalAnalysisResult& out, const AnalysisProgress& progress) {
    out = {};
    if (!validRequest(request))
        return audio::Result::fail(audio::EngineError::InvalidArgument, "invalid analysis range or transform");
    if (!request.detectTempo && !request.detectKey) return audio::Result::ok();
    try {
        checkpoint(progress, 0.0, "decoding");
        audio::platform::AudioFileReader reader;
        auto opened = reader.open(path);
        if (!opened) return opened;
        const auto info = reader.info();
        if (info.channels == 0 || info.channels > 64 || info.sampleRate < 1000 || info.sampleRate > 768000)
            return audio::Result::fail(audio::EngineError::UnsupportedFormat, "unsupported analysis sample format");
        const auto first = audio::FrameCount(std::clamp(request.offsetSeconds * info.sampleRate, 0.0, double(info.frames)));
        audio::FrameCount wanted = info.frames - first;
        if (request.durationSeconds > 0)
            wanted = audio::FrameCount(std::min(double(wanted), request.durationSeconds * info.sampleRate));
        if (!wanted) return audio::Result::fail(audio::EngineError::InvalidArgument, "the selected clip range is empty");
        auto seek = reader.seek(first);
        if (!seek) return seek;
        std::vector<float> buffer(kDecodeFrames * info.channels);
        MixStatistics stats(int(info.channels));
        while (stats.frames < wanted) {
            checkpoint(progress, 0.05 * double(stats.frames) / double(wanted), "decoding");
            const auto read = reader.read(buffer.data(), std::min<audio::FrameCount>(kDecodeFrames, wanted - stats.frames));
            if (!read) break; // Decoders may overestimate VBR stream length.
            stats.add(buffer.data(), std::size_t(read));
        }
        if (!stats.frames)
            return audio::Result::fail(audio::EngineError::UnsupportedFormat, "decoded zero frames for analysis");
        const int channel = stats.selectedChannel();
        const double dc = stats.dc(channel);
        std::size_t cachedFirst = 0, cachedCount = 0;
        // Decode into a bounded sliding cache. Resampler coordinates are global,
        // so neither filter state nor phase resets at the block boundaries.
        const auto mono = prepareMono(stats.frames, info.sampleRate, [&](std::size_t i) {
            if (i < cachedFirst || i >= cachedFirst + cachedCount) {
                cachedFirst = i > 1024 ? i - 1024 : 0;
                auto moved = reader.seek(first + cachedFirst);
                if (!moved) throw std::runtime_error(moved.message());
                cachedCount = std::size_t(reader.read(buffer.data(), std::min(kDecodeFrames, stats.frames - cachedFirst)));
                if (i >= cachedFirst + cachedCount) throw std::runtime_error("audio changed or decoding failed during analysis");
            }
            return mixSample(buffer.data() + (i - cachedFirst) * info.channels, int(info.channels), channel, dc);
        }, subProgress(progress, 0.05, 0.15));
        analyzePrepared(mono, request, out, subProgress(progress, 0.15, 1.0));
        return audio::Result::ok();
    } catch (...) { return exceptionResult(); }
}

bool TempoEstimate::highConfidence() const noexcept { return confidentTempo(*this); }
bool KeyEstimate::highConfidence() const noexcept { return confidentKey(*this); }
bool highConfidence(const ClipTempoAnalysisModel& value) noexcept { return confidentTempo(value); }
bool highConfidence(const ClipKeyAnalysisModel& value) noexcept { return confidentKey(value); }

int roundedBpm(double bpm) noexcept {
    if (!std::isfinite(bpm) || bpm <= 0.0 || bpm > double(std::numeric_limits<int>::max()) - 0.5) return 0;
    return int(std::lround(bpm));
}

std::vector<int> applicableTempos(const TempoEstimate& tempo) {
    std::vector<int> result;
    const auto add = [&](double bpm) {
        const int rounded = roundedBpm(bpm);
        if (rounded >= 1 && rounded <= 999 &&
            std::find(result.begin(), result.end(), rounded) == result.end()) result.push_back(rounded);
    };
    if (tempo.status == DetectionStatus::Unavailable) return result;
    add(tempo.bpm);
    for (double bpm : tempo.alternatives) add(bpm);
    return result;
}

std::string pitchClassName(int root) {
    static constexpr std::array<std::string_view, 12> names =
        {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    return std::string(names[std::size_t((root % 12 + 12) % 12)]);
}

std::string camelotName(int root, const std::string& scale) {
    static constexpr std::array<int, 12> major = {8,3,10,5,12,7,2,9,4,11,6,1};
    static constexpr std::array<int, 12> minor = {5,12,7,2,9,4,11,6,1,8,3,10};
    const int pc = (root % 12 + 12) % 12;
    const bool isMinor = scale == "natural_minor" || scale == "minor";
    return std::to_string((isMinor ? minor : major)[std::size_t(pc)]) +
           (isMinor ? "A" : "B");
}

std::string keyDisplayName(const KeyEstimate& key) {
    if (key.root < 0) return {};
    const bool minor = key.scale == "natural_minor" || key.scale == "minor";
    return pitchClassName(key.root) + (minor ? " minor" : " major");
}

ClipMusicalAnalysisModel toClipAnalysisModel(
    const MusicalAnalysisResult& result,
    const MusicalAnalysisRequest& request) {
    ClipMusicalAnalysisModel model;
    if (!request.detectTempo && !request.detectKey) return model;
    model.algorithmVersion = result.algorithmVersion;
    model.analyzedOffsetSeconds = std::max(0.0, request.offsetSeconds);
    model.analyzedDurationSeconds = result.analyzedSeconds;
    model.tempo.algorithmVersion = request.detectTempo ? result.tempo.algorithmVersion : 0;
    model.tempo.calibrated = result.tempo.calibrated;
    model.tempo.backend = result.tempo.backend;
    model.tempo.reason = result.tempo.reason;
    model.tempo.status = MusicalAnalysisStatus(int(result.tempo.status));
    model.tempo.bpm = result.tempo.bpm;
    model.tempo.confidence = result.tempo.confidence;
    model.tempo.stability = result.tempo.stability;
    model.tempo.alternatives = result.tempo.alternatives;
    model.tempo.variable = result.tempo.variable;
    model.key.algorithmVersion = request.detectKey ? result.key.algorithmVersion : 0;
    model.key.calibrated = result.key.calibrated;
    model.key.backend = result.key.backend;
    model.key.reason = result.key.reason;
    model.key.status = MusicalAnalysisStatus(int(result.key.status));
    model.key.variable = result.key.variable;
    model.key.root = result.key.root;
    model.key.scale = result.key.scale;
    model.key.confidence = result.key.confidence;
    model.key.alternateRoot = result.key.alternateRoot;
    model.key.alternateScale = result.key.alternateScale;
    model.key.tuningCents = result.key.tuningCents;
    return model;
}

} // namespace daw::analysis
