#pragma once

#include "Audio/SampleBuffer.hpp"
#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"
#include "Common/RealtimeSnapshot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace daw::engine {

/// One clip on the timeline: a slice of a decoded file placed at a position.
struct ClipPlacement {
    std::shared_ptr<const SampleBuffer> audio;
    SamplePos startSample = 0;     // timeline position of the clip's first sample
    SamplePos offsetSamples = 0;   // where in the file the clip starts
    SamplePos lengthSamples = 0;   // 0 = to the end of the file
    SamplePos fadeInSamples = 0;   // ramp up over the clip's first N samples
    SamplePos fadeOutSamples = 0;  // ramp down over the clip's last N samples
    bool fadeEqualPower = false;   // true = equal-power curve (auto-crossfades)
    float fadeInCurve = 0.0f;      // -1 convex … +1 concave
    float fadeOutCurve = 0.0f;
    SamplePos tapeStartSamples = 0; // user fade-in also winds 0x to 1x
    SamplePos tapeStopSamples = 0;  // user fade-out also winds 1x to 0x
    /// Exact source region in decoded-file frames. Negative keeps the legacy
    /// offset/length path. Sample Editor placements set these so time, pitch
    /// and loop can be resolved independently from timeline geometry.
    double sourceStartFrame = -1.0;
    double sourceEndFrame = -1.0;
    int stretchMode = 0;           // ClipStretchMode
    double stretchTime = 1.0;
    double stretchPitch = 0.0;
    double formant = 0.0;
    int loopMode = 0;
    double loopStart = 0.0;        // normalized within sourceStart…sourceEnd
    double loopEnd = 1.0;
    float gain = 1.0f;
    float pan = 0.0f;
    bool muted = false;
};

/// The audio source of one track: plays whatever clips overlap the block.
///
/// The clip list is published as an immutable snapshot, so editing the
/// arrangement never locks the audio thread, and a clip that is being removed
/// stays alive (through the shared_ptr) until the block using it has finished.
class ClipPlayerNode : public Node {
public:
    using ClipList = std::vector<ClipPlacement>;

    explicit ClipPlayerNode(std::string name = "Clips")
        : m_name(std::move(name)),
          m_clips(std::make_shared<const ClipList>()) {}

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    /// Control thread: swap in a new arrangement for this track.
    void setClips(std::shared_ptr<const ClipList> clips) {
        m_clips.publish(std::move(clips));
    }
    std::shared_ptr<const ClipList> clips() const {
        return m_clips.controlCopy();
    }

    void prepare(const PrepareInfo& info) override {
        m_sampleRate = info.sampleRate;
        m_hasPosition = false;
        m_activeCount = 0;
        m_clipCursor = 0;
    }

    void reset() override {
        m_hasPosition = false;
        m_activeCount = 0;
        m_clipCursor = 0;
    }

    void process(const ProcessContext& context) override {
        const ChannelCount channels = context.output.numChannels();
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            dsp::clear(context.output.channel(ch));
        }
        if (!context.playing) return;

        auto clips = m_clips.read();
        if (!clips) return;

        const SamplePos blockStart = context.timelinePosition;
        const SamplePos blockEnd = blockStart + SamplePos(context.frames);

        const bool snapshotChanged = m_clipsFor != clips.get();
        const bool jumped = m_hasPosition && blockStart != m_expectedPosition;
        if (snapshotChanged || jumped || !m_hasPosition) {
            m_activeCount = 0;
            m_activeOverflow = false;
            m_clipCursor = std::size_t(std::lower_bound(
                clips->begin(), clips->end(), blockStart,
                [](const ClipPlacement& clip, SamplePos position) {
                    return clip.startSample < position;
                }) - clips->begin());
            for (std::size_t i = 0; i < m_clipCursor; ++i) {
                if (clipEnd((*clips)[i]) > blockStart) addActive(i);
            }
            m_clipsFor = clips.get();
        } else {
            for (std::size_t i = 0; i < m_activeCount;) {
                if (clipEnd((*clips)[m_active[i]]) <= blockStart) {
                    m_active[i] = m_active[--m_activeCount];
                } else {
                    ++i;
                }
            }
        }
        while (m_clipCursor < clips->size() &&
               (*clips)[m_clipCursor].startSample < blockEnd) {
            if (clipEnd((*clips)[m_clipCursor]) > blockStart) addActive(m_clipCursor);
            ++m_clipCursor;
        }
        m_expectedPosition = blockEnd;
        m_hasPosition = true;

        auto renderClip = [&](const ClipPlacement& clip) {
            if (clip.muted || !clip.audio) return;

            // Preserve unity at centre while attenuating only the side the
            // source is panned away from.  Clip gain/pan belong before the
            // private clip insert chain, matching the editor's strip.
            auto channelGain = [&](ChannelCount channel) {
                if (channels < 2) return clip.gain;
                const float pan = std::clamp(clip.pan, -1.0f, 1.0f);
                const float balance = channel == 0 ? std::min(1.0f, 1.0f - pan)
                                                   : std::min(1.0f, 1.0f + pan);
                return clip.gain * balance;
            };

            const SampleRate fileRate = clip.audio->sampleRate();
            // Files recorded at another rate are read with a step, so a 44.1 kHz
            // clip in a 48 kHz session plays at the right pitch instead of
            // being transposed.
            const double step = (fileRate > 0.0 && m_sampleRate > 0.0)
                                    ? fileRate / m_sampleRate
                                    : 1.0;
            const SamplePos available =
                clip.lengthSamples > 0
                    ? clip.lengthSamples
                    : SamplePos(double(clip.audio->frames()) / step) - clip.offsetSamples;
            const SamplePos clipEnd = clip.startSample + available;
            if (clipEnd <= blockStart || clip.startSample >= blockEnd) return;

            const SamplePos from = std::max(blockStart, clip.startSample);
            const SamplePos to = std::min(blockEnd, clipEnd);
            const FrameCount count = FrameCount(to - from);
            const FrameCount destinationOffset = FrameCount(from - blockStart);
            const SamplePos clipRelStart = from - clip.startSample;
            const bool tape = clip.tapeStartSamples > 0 || clip.tapeStopSamples > 0;

            // Integrate the tape-speed ramps rather than multiplying an
            // absolute source position by speed. That keeps the source phase
            // continuous at both ends of the fade (no jump/click when the
            // speed reaches 1x or begins to wind down).
            auto sourceProgressAt = [&](SamplePos pos) -> double {
                const double p = std::clamp(double(pos), 0.0, double(available));
                double mapped = p;
                double headLag = 0.0;
                if (clip.tapeStartSamples > 0) {
                    const double length = double(clip.tapeStartSamples);
                    if (p < length) mapped = p * p / (2.0 * length);
                    else mapped = p - length * 0.5;
                    headLag = length * 0.5;
                }
                if (clip.tapeStopSamples > 0) {
                    const double length = double(clip.tapeStopSamples);
                    const double start = std::max(0.0, double(available) - length);
                    if (p > start) {
                        const double q = p - start;
                        mapped = start - headLag + q - q * q / (2.0 * length);
                    }
                }
                return std::max(0.0, mapped);
            };

            // Gain of the fade envelope at position `pos` samples into the clip
            // (0 … available). Linear for user fades, sqrt for equal-power
            // crossfades so an overlap keeps constant loudness.
            const bool fading =
                clip.fadeInSamples > 0 || clip.fadeOutSamples > 0;
            auto fadeAt = [&](SamplePos pos) -> float {
                float t = 1.0f;
                float curve = 0.0f;
                if (clip.fadeInSamples > 0 && pos < clip.fadeInSamples) {
                    t = float(double(pos) / double(clip.fadeInSamples));
                    curve = clip.fadeInCurve;
                }
                if (clip.fadeOutSamples > 0 &&
                    pos > available - clip.fadeOutSamples) {
                    const float o =
                        float(double(available - pos) / double(clip.fadeOutSamples));
                    if (o < t) {
                        t = o;
                        curve = clip.fadeOutCurve;
                    }
                }
                t = std::clamp(t, 0.0f, 1.0f);
                if (clip.fadeEqualPower) return std::sqrt(t);
                const float exponent = std::pow(4.0f, -std::clamp(curve, -1.0f, 1.0f));
                return std::pow(t, exponent);
            };

            const bool sampleEdited = clip.sourceStartFrame >= 0.0 &&
                                      clip.sourceEndFrame > clip.sourceStartFrame;
            if (sampleEdited) {
                const double sourceBegin = clip.sourceStartFrame;
                const double sourceEnd = std::min(
                    clip.sourceEndFrame, double(clip.audio->frames()));
                const double sourceSpan = std::max(1.0, sourceEnd - sourceBegin);
                const double loopBegin = sourceBegin +
                    std::clamp(clip.loopStart, 0.0, 1.0) * sourceSpan;
                const double loopEnd = sourceBegin +
                    std::clamp(clip.loopEnd, 0.0, 1.0) * sourceSpan;
                const bool looping = clip.loopMode != 0 && loopEnd - loopBegin >= 16.0;
                const double pitchRatio = std::pow(2.0, clip.stretchPitch / 12.0);
                const double timeRatio = std::max(clip.stretchTime, 0.01);
                const bool granular = !tape && (clip.stretchMode != 0 ||
                                      std::abs(clip.stretchPitch) > 0.001);
                double grainLength = 1024.0;
                switch (clip.stretchMode) {
                    case 1: grainLength = 384.0; break;
                    case 2: grainLength = 1024.0; break;
                    case 3: grainLength = 2048.0; break;
                    case 4: grainLength = 4096.0; break;
                    default: break;
                }
                const double hop = grainLength * 0.5;
                // One knob and one sample rate decide these, so they are
                // resolved per clip rather than per sample.
                // Any mode: the tilt is a filter over whatever the stage above
                // produced, so it applies to a resampled clip as readily as to
                // a granular one. Kept in step with SamplerVoice, which makes
                // the same call for the same reason.
                const bool shifting = std::abs(clip.formant) > 0.001;
                const double formantTilt = std::tanh(clip.formant / 12.0);
                const double windowStep = 2.0 * std::numbers::pi / grainLength;

                auto wrap = [&](double position) {
                    if (!looping) return position;
                    const double length = loopEnd - loopBegin;
                    if (clip.loopMode == 2) {
                        const double cycle = length * 2.0;
                        double phase = std::fmod(position - loopBegin, cycle);
                        if (phase < 0.0) phase += cycle;
                        return phase <= length ? loopBegin + phase
                                               : loopEnd - (phase - length);
                    }
                    double phase = std::fmod(position - loopBegin, length);
                    if (phase < 0.0) phase += length;
                    return loopBegin + phase;
                };
                auto read = [&](ChannelCount channel, double position) {
                    position = wrap(position);
                    if (!looping && (position < sourceBegin || position >= sourceEnd))
                        return 0.0f;
                    const FrameCount frames = clip.audio->frames();
                    if (frames == 0) return 0.0f;
                    position = std::clamp(position, 0.0, double(frames - 1));
                    const std::int64_t index = std::int64_t(position);
                    const float fraction = float(position - double(index));
                    const ChannelCount sourceChannel =
                        std::min<ChannelCount>(channel, clip.audio->channels() - 1);
                    const float* data = clip.audio->channel(sourceChannel);
                    auto at = [&](std::int64_t i) {
                        return data[std::clamp<std::int64_t>(
                            i, 0, std::int64_t(frames) - 1)];
                    };
                    const float y0 = at(index - 1), y1 = at(index);
                    const float y2 = at(index + 1), y3 = at(index + 2);
                    const float c0 = y1;
                    const float c1 = 0.5f * (y2 - y0);
                    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                    return ((c3 * fraction + c2) * fraction + c1) * fraction + c0;
                };

                for (ChannelCount ch = 0; ch < channels; ++ch) {
                    float* destination = context.output.data(ch) + destinationOffset;
                    for (FrameCount i = 0; i < count; ++i) {
                        const SamplePos timelineFrame = clipRelStart + i;
                        const double outputFrame = tape
                            ? sourceProgressAt(timelineFrame)
                            : double(timelineFrame);
                        double value = 0.0;
                        if (!granular) {
                            value = read(ch, sourceBegin + outputFrame * step / timeRatio);
                        } else {
                            const std::int64_t newest =
                                std::int64_t(std::floor(outputFrame / hop));
                            for (std::int64_t grain = newest - 1; grain <= newest; ++grain) {
                                if (grain < 0) continue;
                                const double phase = outputFrame - double(grain) * hop;
                                if (phase < 0.0 || phase >= grainLength) continue;
                                const double window =
                                    0.5 - 0.5 * std::cos(windowStep * phase);
                                const double anchor = sourceBegin +
                                    double(grain) * hop * step / timeRatio;
                                value += window * read(ch, anchor + phase * step * pitchRatio);
                            }
                        }

                        if (shifting) {
                            const double basePos = sourceBegin + outputFrame * step / timeRatio;
                            const double low = (read(ch, basePos - 1.0) +
                                                read(ch, basePos) +
                                                read(ch, basePos + 1.0)) / 3.0;
                            const double high = value - low;
                            value = low * (1.0 - 0.45 * formantTilt) +
                                    high * (1.0 + 0.75 * formantTilt);
                        }
                        destination[i] += float(value) * channelGain(ch) *
                                          (fading ? fadeAt(clipRelStart + i) : 1.0f);
                    }
                }
                return;
            }

            for (ChannelCount ch = 0; ch < channels; ++ch) {
                const float* source = clip.audio->channel(
                    std::min<ChannelCount>(ch, clip.audio->channels() - 1));
                float* destination = context.output.data(ch) + destinationOffset;
                const FrameCount sourceFrames = clip.audio->frames();

                if (step == 1.0 && !tape) {
                    const SamplePos base = clip.offsetSamples + clipRelStart;
                    const FrameCount usable =
                        FrameCount(std::min<SamplePos>(count, std::max<SamplePos>(
                            0, SamplePos(sourceFrames) - base)));
                    if (!fading) {
                        // The overwhelmingly common case — a clip at the session
                        // rate, not inside a fade — is exactly a scaled
                        // accumulate, so it runs on the SIMD kernel rather than
                        // a scalar loop that re-tests `fading` every sample.
                        dsp::addScaled({destination, usable},
                                       {source + base, usable}, channelGain(ch));
                        continue;
                    }
                    for (FrameCount i = 0; i < usable; ++i) {
                        destination[i] +=
                            source[base + i] * channelGain(ch) * fadeAt(clipRelStart + i);
                    }
                } else {
                    for (FrameCount i = 0; i < count; ++i) {
                        const SamplePos timelineFrame = clipRelStart + i;
                        const double progress = tape
                            ? sourceProgressAt(timelineFrame)
                            : double(timelineFrame);
                        const double pos = (double(clip.offsetSamples) + progress) * step;
                        const SamplePos index = SamplePos(pos);
                        if (index + 1 >= SamplePos(sourceFrames)) break;
                        const float fraction = float(pos - double(index));
                        const float a = source[index];
                        const float b = source[index + 1];
                        const float fg = fading ? fadeAt(clipRelStart + i) : 1.0f;
                        destination[i] +=
                            (a + (b - a) * fraction) * channelGain(ch) * fg;
                    }
                }
            }
        };

        if (m_activeOverflow) {
            // Pathological sessions with thousands of simultaneously
            // overlapping clips retain correctness via a bounded fallback.
            for (std::size_t i = 0; i < m_clipCursor; ++i) renderClip((*clips)[i]);
        } else {
            for (std::size_t i = 0; i < m_activeCount; ++i) {
                renderClip((*clips)[m_active[i]]);
            }
        }
    }

private:
    SamplePos clipEnd(const ClipPlacement& clip) const noexcept {
        if (!clip.audio) return clip.startSample;
        const SampleRate fileRate = clip.audio->sampleRate();
        const double step = (fileRate > 0.0 && m_sampleRate > 0.0)
                                ? fileRate / m_sampleRate
                                : 1.0;
        const SamplePos available =
            clip.lengthSamples > 0
                ? clip.lengthSamples
                : SamplePos(double(clip.audio->frames()) / step) - clip.offsetSamples;
        return clip.startSample + std::max<SamplePos>(available, 0);
    }

    void addActive(std::size_t index) noexcept {
        if (m_activeCount < m_active.size()) {
            m_active[m_activeCount++] = index;
        } else {
            m_activeOverflow = true;
        }
    }

    std::string m_name;
    RealtimeSnapshot<ClipList> m_clips;
    SampleRate m_sampleRate = 48000.0;
    static constexpr std::size_t kMaxActiveClips = 2048;
    std::array<std::size_t, kMaxActiveClips> m_active{};
    std::size_t m_activeCount = 0;
    std::size_t m_clipCursor = 0;
    const ClipList* m_clipsFor = nullptr;
    SamplePos m_expectedPosition = 0;
    bool m_hasPosition = false;
    bool m_activeOverflow = false;
};

/// Live hardware input, for monitoring and for feeding record-armed tracks.
/// The engine points `bus` at the device's input block for the duration of a
/// render; outside a render it is null and the node is silent.
struct InputBus {
    const float* const* channels = nullptr;
    ChannelCount channelCount = 0;
    FrameCount frames = 0;
};

class InputNode : public Node {
public:
    InputNode(std::string name, const InputBus* bus, ChannelCount firstChannel,
              ChannelCount channelCount)
        : m_name(std::move(name)), m_bus(bus), m_firstChannel(firstChannel),
          m_channelCount(std::max<ChannelCount>(1, channelCount)) {}

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    void setEnabled(bool enabled) noexcept {
        m_enabled.store(enabled, std::memory_order_relaxed);
    }

    void process(const ProcessContext& context) override {
        const ChannelCount outChannels = context.output.numChannels();
        if (!m_enabled.load(std::memory_order_relaxed) || !m_bus ||
            !m_bus->channels) {
            for (ChannelCount ch = 0; ch < outChannels; ++ch) {
                dsp::clear(context.output.channel(ch));
            }
            return;
        }
        const FrameCount frames = std::min(context.frames, m_bus->frames);
        for (ChannelCount ch = 0; ch < outChannels; ++ch) {
            const std::span<float> destination = context.output.channel(ch);
            // A mono input feeds both sides; a stereo pair maps straight across.
            const ChannelCount sourceChannel =
                m_firstChannel + (m_channelCount >= 2 ? ch : 0);
            const float* source = sourceChannel < m_bus->channelCount
                                      ? m_bus->channels[sourceChannel]
                                      : nullptr;
            if (!source) {
                dsp::clear(destination);
                continue;
            }
            // Straight memcpy, and only the tail the device did not fill needs
            // zeroing — the old version cleared the whole block first and then
            // overwrote nearly all of it a sample at a time.
            dsp::copy(destination, {source, frames});
            if (frames < context.frames) {
                dsp::clear(destination.subspan(frames));
            }
        }
    }

private:
    std::string m_name;
    const InputBus* m_bus = nullptr;
    ChannelCount m_firstChannel = 0;
    ChannelCount m_channelCount = 1;
    std::atomic<bool> m_enabled{true};
};

} // namespace daw::engine
