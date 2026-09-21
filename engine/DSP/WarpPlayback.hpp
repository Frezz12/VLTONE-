#pragma once
#include "Common/WarpMap.hpp"
#include "DSP/TimeStretch.hpp"
#include <array>

namespace daw::engine::dsp {

// Prepared on the control thread. Only render/reset touch playback state.
// A replacement owns one previous processor, never a chain of old schedules.
class WarpPlayback {
    struct Stream {
        std::array<TimeStretch, 2> processors;
        int active = 0, segment = -1, fade = 0, fadeLength = 0;
        double speed = 1, outgoingSpeed = 1, outgoingPosition = 0;
        std::array<float, 128> tailLeft{}, tailRight{}, cachedLeft{}, cachedRight{};
        int readPosition = 128;
        double expectedSeconds = -1;
        Stream(double rate, int mode, double ratio)
            : processors{TimeStretch(rate, mode, ratio), TimeStretch(rate, mode, ratio)} {}
        void reset() noexcept {
            for (auto& processor : processors) processor.reset();
            segment = -1; fade = 0; readPosition = 128; expectedSeconds = -1;
        }
    };
public:
    const ClipWarpModel map;
    const double tempo;
    double sampleRate() const noexcept { return rate; }
    WarpPlayback(const ClipWarpModel& value, double bpm, double sampleRate,
                 const std::shared_ptr<WarpPlayback>& previous = {})
        : map(value), tempo(bpm), rate(sampleRate),
          stretch(std::make_shared<Stream>(sampleRate, value.mode, warpMaximumRatio(value, bpm))) {
        if (previous) {
            oldMap = previous->map;
            oldTempo = previous->tempo;
            oldStretch = previous->stretch;
            transition = std::max(1, int(sampleRate * .005));
        }
    }
    void reset() noexcept {
        stretch->reset();
        if (oldStretch) oldStretch->reset();
        transition = 0;
    }
    void render(const SampleBuffer& audio, double seconds, double pitch, double formant,
                float* left, float* right, FrameCount frames) noexcept {
        if (stretch->expectedSeconds >= 0 && std::abs(seconds - stretch->expectedSeconds) > .5 / rate) reset();
        for (FrameCount done = 0; done < frames;) {
            const auto n = std::min<FrameCount>(128, frames - done);
            renderStream(map, tempo, *stretch, audio, seconds + done / rate, pitch, formant,
                         left + done, right + done, n);
            if (transition > 0) {
                const auto overlap = std::min<FrameCount>(n, transition);
                renderStream(oldMap, oldTempo, *oldStretch, audio, seconds + done / rate, pitch, formant,
                             oldLeft.data(), oldRight.data(), overlap);
                const int total = std::max(1, int(rate * .005));
                for (FrameCount i = 0; i < overlap; ++i, --transition) {
                    const float mix = 1.f - float(transition) / total;
                    left[done + i] = oldLeft[i] * (1 - mix) + left[done + i] * mix;
                    right[done + i] = oldRight[i] * (1 - mix) + right[done + i] * mix;
                }
            }
            done += n;
        }
    }

private:
    double rate;
    std::shared_ptr<Stream> stretch, oldStretch;
    ClipWarpModel oldMap;
    double oldTempo = 120;
    int transition = 0;
    std::array<float, 128> oldLeft{}, oldRight{};

    void renderStream(const ClipWarpModel& value, double bpm, Stream& stream,
                      const SampleBuffer& audio, double seconds, double pitch, double formant,
                      float* left, float* right, FrameCount frames) noexcept {
        if (stream.expectedSeconds >= 0 && std::abs(seconds - stream.expectedSeconds) > .5 / rate) stream.reset();
        for (FrameCount done = 0; done < frames;) {
            if (stream.readPosition == 128) {
                // Fixed quanta make host block sizes irrelevant. The cache is
                // shared with a replacement so its fade starts at the exact
                // next audible sample, including buffered DSP output.
                renderMap(value, bpm, stream, audio, seconds + done / rate, pitch, formant,
                          stream.cachedLeft.data(), stream.cachedRight.data(), 128);
                stream.readPosition = 0;
            }
            const auto n = std::min<FrameCount>(frames - done, 128 - stream.readPosition);
            std::copy_n(stream.cachedLeft.data() + stream.readPosition, n, left + done);
            std::copy_n(stream.cachedRight.data() + stream.readPosition, n, right + done);
            stream.readPosition += n; done += n;
        }
        stream.expectedSeconds = seconds + frames / rate;
    }

    void renderMap(const ClipWarpModel& value, double bpm, Stream& stream,
                   const SampleBuffer& audio, double seconds, double pitch, double formant,
                   float* left, float* right, FrameCount frames) noexcept {
        const double fileRate = audio.sampleRate();
        const StretchSource source{&audio, value.markers.front().sourceSeconds * fileRate,
            std::min(double(audio.frames()), value.markers.back().sourceSeconds * fileRate)};
        for (FrameCount done = 0; done < frames;) {
            const double beats = (seconds + done / rate) * bpm / 60.;
            const auto segment = warpSegment(value, beats);
            const double boundary = value.markers[segment + 1].targetBeats * 60. / bpm;
            const auto n = FrameCount(std::clamp(std::ceil((boundary - seconds) * rate - done - 1e-7),
                                                1., double(frames - done)));
            const double position = warpSourceAt(value, beats) * fileRate;
            const double speed = warpSpeedAt(value, beats, bpm);
            if (value.preservePitch || std::abs(pitch) > 1e-9 || std::abs(formant) > 1e-9) {
                // Each segment has its own latency-compensated source origin.
                // Carrying a variable-rate processor across a boundary would
                // retain the previous rate's look-ahead and shift later attacks.
                // Alternate two prepared processors and blend their overlap.
                if (stream.segment >= 0 && stream.segment != int(segment) &&
                    std::abs(stream.speed - speed) > 1e-9) {
                    stream.outgoingSpeed = stream.speed;
                    stream.outgoingPosition = position - source.begin;
                    stream.active = 1 - stream.active;
                    stream.processors[stream.active].reset();
                    stream.fadeLength = std::max(1, int(std::min(.005,
                        (value.markers[segment + 1].targetBeats - value.markers[segment].targetBeats) * 60. / bpm) * rate));
                    stream.fade = stream.fadeLength;
                }
                stream.segment = int(segment); stream.speed = speed;
                stream.processors[stream.active].render(source, position - source.begin, speed,
                    pitch + (value.preservePitch ? 0 : 12 * std::log2(speed)), formant,
                    left + done, right + done, n, true);
                if (stream.fade > 0) {
                    const auto overlap = std::min<FrameCount>(n, stream.fade);
                    stream.processors[1 - stream.active].render(source, stream.outgoingPosition,
                        stream.outgoingSpeed, pitch + (value.preservePitch ? 0 : 12 * std::log2(stream.outgoingSpeed)),
                        formant, stream.tailLeft.data(), stream.tailRight.data(), overlap, true);
                    for (FrameCount i = 0; i < overlap; ++i, --stream.fade) {
                        const float mix = 1.f - float(stream.fade) / stream.fadeLength;
                        left[done + i] = stream.tailLeft[i] * (1 - mix) + left[done + i] * mix;
                        right[done + i] = stream.tailRight[i] * (1 - mix) + right[done + i] * mix;
                    }
                    stream.outgoingPosition += overlap * stream.outgoingSpeed * fileRate / rate;
                }
            } else {
                for (FrameCount i = 0; i < n; ++i) {
                    const double p = position + i * speed * fileRate / rate;
                    const auto sample = [&](int channel) {
                        if (p < source.begin || p >= source.end || !audio.frames()) return 0.f;
                        const auto at = [&](std::int64_t frame) {
                            return audio.readSample(ChannelCount(std::min<int>(channel, audio.channels() - 1)),
                                FrameCount(std::clamp<std::int64_t>(frame, std::int64_t(std::ceil(source.begin)),
                                                                  std::int64_t(std::ceil(source.end)) - 1)));
                        };
                        const auto frame = std::int64_t(std::floor(p));
                        const float f = float(p - frame);
                        const float a = at(frame - 1), b = at(frame), c = at(frame + 1), d = at(frame + 2);
                        return b + f * (.5f * (c - a) + f * (a - 2.5f*b + 2*c - .5f*d +
                            f * (.5f*(d - a) + 1.5f*(b - c))));
                    };
                    left[done + i] = sample(0); right[done + i] = sample(1);
                }
            }
            done += n;
        }
    }
};
} // namespace daw::engine::dsp
