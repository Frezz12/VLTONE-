#include "DSP/TimeStretch.hpp"
#include "signalsmith-stretch.h"

#include <array>
#include <cmath>

namespace daw::engine::dsp {
namespace {
constexpr int quantum = 128;

// The source adapter also supplies zero-padded look-ahead and pre-roll, without
// copying the file or allocating a buffer proportional to its duration.
struct Reader {
    StretchSource source;
    double offset = 0, step = 1;
    struct Channel {
        const Reader& reader;
        int channel;
        float operator[](int i) const {
            const auto& s = reader.source;
            const auto& audio = *s.audio;
            double p = s.begin + (reader.offset + i) * reader.step;
            if (p < s.begin) return 0;
            const double length = s.loopEnd - s.loopBegin;
            if (s.loopMode && length >= 2 && p >= s.loopEnd) {
                if (s.loopMode == 2) {
                    // Reflect about actual samples, not one frame past the end.
                    const double span = length - 1;
                    const double phase = std::fmod(p - s.loopBegin, span * 2);
                    p = s.loopBegin + (phase <= span ? phase : span * 2 - phase);
                } else {
                    p = s.loopBegin + std::fmod(p - s.loopBegin, length);
                }
            }
            if (p >= s.end || p >= audio.frames()) return 0;
            const auto index = std::int64_t(std::floor(p));
            const float fraction = float(p - index);
            const auto at = [&](std::int64_t n) {
                return audio.readSample(ChannelCount(std::min<int>(channel, audio.channels() - 1)),
                    FrameCount(std::clamp<std::int64_t>(n, std::int64_t(std::ceil(s.begin)),
                    std::min<std::int64_t>(audio.frames(), std::ceil(s.end)) - 1)));
            };
            if (fraction == 0) return at(index);
            const float a = at(index - 1), b = at(index), c = at(index + 1), d = at(index + 2);
            return b + fraction * (0.5f * (c - a) + fraction *
                (a - 2.5f*b + 2*c - 0.5f*d + fraction * (0.5f*(d - a) + 1.5f*(b - c))));
        }
    };
    Channel operator[](int c) const { return {*this, c}; }
};
} // namespace

struct TimeStretch::Impl {
    struct Stage {
        signalsmith::stretch::SignalsmithStretch<float> stretch{0};
        std::array<std::array<float, quantum>, 2> output{};
        std::array<std::vector<float>, 2> input;
        double remainder = 0;
        int readPosition = quantum;
        Stage(double rate, int mode) {
            const double seconds = mode == 1 ? 0.096 : mode == 2 ? 0.08 :
                                   mode == 3 ? 0.10 : 0.12;
            const int overlap = mode <= 2 ? 8 : 6;
            const int interval = std::max(32, int(std::ceil(rate * seconds / overlap / 2)) * 2);
            // Even hops make an exact 2x stage exact at 44.1 kHz as well.
            stretch.configure(2, interval * overlap, interval, true);
            // A stage never exceeds 2x; large edits use multiple gentle stages.
            for (auto& channel : input)
                channel.resize(std::max(stretch.outputSeekLength(2.0f) + 1, quantum * 2 + 1));
        }
    };
    std::vector<std::unique_ptr<Stage>> stages;
    double rate, maxRatio;
    int profile, activeStages = 1;
    StretchSource previousSource;
    double expectedPosition = 0, inputPosition = 0, stageSpeed = 1;
    bool ready = false;

    static int stageCount(double ratio) {
        return std::max(1, int(std::ceil(std::log2(std::max(1.0, ratio)) - 1e-10)));
    }
    Impl(double sampleRate, int mode, double maximumRatio)
        : rate(sampleRate), maxRatio(std::clamp(maximumRatio, 2.0, 1000.0)), profile(mode) {
        for (int i = 0; i < stageCount(maxRatio); ++i)
            stages.push_back(std::make_unique<Stage>(rate, profile));
    }
    void tuning(double pitch, double formant) {
        for (int i = 0; i < activeStages; ++i) {
            auto& stretch = stages[i]->stretch;
            const bool last = i == activeStages - 1;
            stretch.setTransposeSemitones(last ? float(pitch) : 0);
            stretch.setFormantSemitones(last ? float(formant) : 0, last && profile == 3);
            stretch.setFormantBase();
        }
    }
    void produce(int index, Reader& reader, float* left, float* right, int frames) {
        auto& stage = *stages[index];
        int done = 0;
        while (done < frames) {
            if (stage.readPosition == quantum) {
                const double wanted = quantum * stageSpeed + stage.remainder;
                const int inputFrames = int(std::floor(wanted));
                stage.remainder = wanted - inputFrames;
                float* output[2]{stage.output[0].data(), stage.output[1].data()};
                if (index == 0) {
                    reader.offset = inputPosition;
                    stage.stretch.process(reader, inputFrames, output, quantum);
                    inputPosition += inputFrames;
                } else {
                    float* input[2]{stage.input[0].data(), stage.input[1].data()};
                    produce(index - 1, reader, input[0], input[1], inputFrames);
                    stage.stretch.process(input, inputFrames, output, quantum);
                }
                stage.readPosition = 0;
            }
            const int count = std::min(frames - done, quantum - stage.readPosition);
            std::copy_n(stage.output[0].data() + stage.readPosition, count, left + done);
            std::copy_n(stage.output[1].data() + stage.readPosition, count, right + done);
            stage.readPosition += count;
            done += count;
        }
    }
    void seek(Reader& reader) {
        for (int i = 0; i < activeStages; ++i) {
            auto& stage = *stages[i];
            const int lead = stage.stretch.outputSeekLength(float(stageSpeed));
            if (i == 0) {
                stage.stretch.outputSeek(reader, lead);
                inputPosition = reader.offset + lead;
            } else {
                float* input[2]{stage.input[0].data(), stage.input[1].data()};
                produce(i - 1, reader, input[0], input[1], lead);
                stage.stretch.outputSeek(input, lead);
            }
            stage.remainder = 0;
            stage.readPosition = quantum;
        }
    }
};

TimeStretch::TimeStretch(double sampleRate, int mode, double maximumRatio)
    : m_impl(std::make_unique<Impl>(sampleRate, std::clamp(mode, 1, 4), maximumRatio)) {}
TimeStretch::~TimeStretch() = default;
double TimeStretch::sampleRate() const noexcept { return m_impl->rate; }
int TimeStretch::mode() const noexcept { return m_impl->profile; }
double TimeStretch::maximumRatio() const noexcept { return m_impl->maxRatio; }
void TimeStretch::reset() noexcept { m_impl->ready = false; }

void TimeStretch::render(const StretchSource& source, double sourcePosition,
                        double speed, double pitch, double formant,
                        float* left, float* right, FrameCount frames) noexcept {
    auto& p = *m_impl;
    if (!source.audio || source.audio->channels() == 0 || source.end <= source.begin) {
        std::fill_n(left, frames, 0); std::fill_n(right, frames, 0); return;
    }
    const double step = source.audio->sampleRate() > 0 ? source.audio->sampleRate() / p.rate : 1;
    speed = std::clamp(speed, 1 / p.maxRatio, p.maxRatio);
    Reader reader{source, sourcePosition / step, step};
    if (!p.ready && std::abs(speed - 1) < 1e-10 && std::abs(pitch) < 1e-10 && std::abs(formant) < 1e-10) {
        // Merely enabling tempo-follow must not colour an unchanged recording.
        for (FrameCount i = 0; i < frames; ++i) {
            left[i] = reader[0][int(i)]; right[i] = reader[1][int(i)];
        }
        p.ready = false;
        return;
    }
    const int wantedStages = Impl::stageCount(std::max(speed, 1 / speed));
    if (p.activeStages != wantedStages) p.ready = false;
    p.activeStages = wantedStages;
    p.stageSpeed = std::pow(speed, 1.0 / p.activeStages);
    p.tuning(pitch, formant);
    const auto& old = p.previousSource;
    const bool sameSource = old.audio == source.audio && old.loopMode == source.loopMode &&
        std::abs(old.begin - source.begin) < 1e-4 && std::abs(old.end - source.end) < 1e-4 &&
        std::abs(old.loopBegin - source.loopBegin) < 1e-4 && std::abs(old.loopEnd - source.loopEnd) < 1e-4;
    if (!p.ready || !sameSource ||
        std::abs(p.expectedPosition - sourcePosition) > std::max(1e-4, step)) {
        p.seek(reader);
        p.previousSource = source;
        p.ready = true;
    }
    p.produce(p.activeStages - 1, reader, left, right, int(frames));
    p.expectedPosition = sourcePosition + frames * speed * step;
}
} // namespace daw::engine::dsp
