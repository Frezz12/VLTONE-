#pragma once

#include "Audio/SampleBuffer.hpp"
#include "DSP/Simd.hpp"
#include "DSP/TimeStretch.hpp"
#include "Graph/Node.hpp"
#include "Common/RealtimeSnapshot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <limits>
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
    // Prepared by setClips on the control thread.
    float fadeInExponent = 1.0f, fadeOutExponent = 1.0f;
    std::shared_ptr<dsp::TimeStretch> stretcher; // prepared off the audio thread
};

/// The audio source of one track: plays whatever clips overlap the block.
///
/// The clip list is published as an immutable snapshot, so editing the
/// arrangement never locks the audio thread, and a clip that is being removed
/// stays alive (through the shared_ptr) until the block using it has finished.
class ClipPlayerNode : public Node {
public:
    using ClipList = std::vector<ClipPlacement>;
private:
    struct ClipSchedule {
        std::shared_ptr<const ClipList> clips = std::make_shared<const ClipList>();
        std::vector<SamplePos> subtreeMaxEnd;
    };
public:

    explicit ClipPlayerNode(std::string name = "Clips")
        : m_name(std::move(name)),
          m_clips(std::make_shared<const ClipSchedule>()) {}

    std::string_view name() const noexcept override { return m_name; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::None; }

    /// Control thread: swap in a new arrangement for this track.
    void setClips(std::shared_ptr<const ClipList> clips) {
        if (!clips) { m_clips.publish({}); return; }
        auto previous = this->clips();
        auto prepared = std::make_shared<ClipList>(*clips);
        for (std::size_t index = 0; index < prepared->size(); ++index) {
            auto& clip = (*prepared)[index];
            clip.fadeInExponent = std::pow(4.0f, -std::clamp(clip.fadeInCurve, -1.0f, 1.0f));
            clip.fadeOutExponent = std::pow(4.0f, -std::clamp(clip.fadeOutCurve, -1.0f, 1.0f));
            const int mode = clip.stretchMode ? clip.stretchMode : 4;
            const double ratio = std::max(clip.stretchTime, 1.0 / std::max(clip.stretchTime, .001));
            if (clip.audio && clip.sourceStartFrame >= 0 &&
                (clip.stretchMode != 0 || std::abs(clip.stretchPitch) > 0.001)) {
                // Reuse one processor per placement across control edits.
                // Matching by index is one-to-one even for overlapping copies
                // of the same file. The DSP detects source changes/seeks itself.
                clip.stretcher.reset();
                if (previous && index < previous->size()) {
                    const auto& old = (*previous)[index];
                    if (old.stretcher && old.audio == clip.audio &&
                        old.stretcher->mode() == mode &&
                        old.stretcher->sampleRate() == m_sampleRate &&
                        old.stretcher->maximumRatio() >= ratio)
                        clip.stretcher = old.stretcher;
                }
                if (!clip.stretcher)
                    clip.stretcher = std::make_shared<dsp::TimeStretch>(m_sampleRate, mode, ratio);
            } else clip.stretcher.reset();
        }
        std::stable_sort(prepared->begin(), prepared->end(), [](const auto& a, const auto& b) {
            return a.startSample < b.startSample;
        });
        auto schedule = std::make_shared<ClipSchedule>();
        schedule->clips = std::move(prepared);
        schedule->subtreeMaxEnd.resize(schedule->clips->size());
        auto build = [&](auto&& self, std::size_t first, std::size_t last) -> SamplePos {
            if (first == last) return std::numeric_limits<SamplePos>::min();
            const auto mid = first + (last - first) / 2;
            return schedule->subtreeMaxEnd[mid] = std::max({clipEnd((*schedule->clips)[mid]),
                self(self, first, mid), self(self, mid + 1, last)});
        };
        build(build, 0, schedule->clips->size());
        m_clips.publish(std::move(schedule));
        preparePlayback(0);
    }
    std::shared_ptr<const ClipList> clips() const {
        const auto schedule = m_clips.controlCopy();
        return schedule ? schedule->clips : nullptr;
    }

    void prepare(const PrepareInfo& info) override {
        m_sampleRate = info.sampleRate;
        setClips(clips());
        m_fadeBuffer.resize(info.maxBlockSize);

    }

    void preparePlayback(SamplePos position) override {
        const auto schedule = m_clips.controlCopy();
        prepareRange(schedule.get(), position, true);
    }
private:
    void prepareRange(const ClipSchedule* schedule, SamplePos position, bool synchronous) const {
        if (!schedule) return;
        const auto end = position + SamplePos(m_sampleRate * .35);
        auto warm = [&](auto&& self, std::size_t first, std::size_t last) -> void {
            if (first == last) return;
            const auto mid = first + (last - first) / 2;
            if (schedule->subtreeMaxEnd[mid] <= position) return;
            self(self, first, mid);
            const auto& clip = (*schedule->clips)[mid];
            if (clip.startSample >= end) return;
            if (clip.audio && clipEnd(clip) > position) {
                const double rate = clip.audio->sampleRate() / m_sampleRate;
                const double rel = double(std::max<SamplePos>(0, position - clip.startSample));
                const double start = clip.sourceStartFrame >= 0 ? clip.sourceStartFrame + rel * rate / std::max(.001, clip.stretchTime)
                                                               : (clip.offsetSamples + rel) * rate;
                const auto request = [&](double frame) {
                    const auto first = FrameCount(std::clamp(frame, 0., double(clip.audio->frames())));
                    if (synchronous) clip.audio->prepareRead(first); else clip.audio->hintRead(first);
                };
                request(start);
                // The stretch window also reads before its centre.
                request(std::max(0., start - 8192.));
                if (clip.loopMode && clip.sourceStartFrame >= 0)
                    request(clip.sourceStartFrame + clip.loopStart *
                        (clip.sourceEndFrame - clip.sourceStartFrame));
            }
            self(self, mid + 1, last);
        };
        warm(warm, 0, schedule->clips->size());
    }
public:

    void reset() override {

        auto schedule = m_clips.read();
        if (schedule) for (const auto& clip : *schedule->clips)
            if (clip.stretcher) clip.stretcher->reset();
    }

    void process(const ProcessContext& context) override {
        const ChannelCount channels = context.output.numChannels();
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            dsp::clear(context.output.channel(ch));
        }
        if (!context.playing) return;

        auto schedule = m_clips.read();
        if (!schedule) return;
        const auto* clips = schedule->clips.get();

        const SamplePos blockStart = context.timelinePosition;
        const SamplePos blockEnd = blockStart + SamplePos(context.frames);
        if (!context.offline && (schedule.get() != m_hintSchedule ||
            blockStart < m_lastHint || blockStart - m_lastHint >= SamplePos(m_sampleRate * .05))) {
            prepareRange(schedule.get(), blockStart, false);
            if (context.transport.looping && context.transport.tempo > 0) {
                const double beatSamples = 60.0 * m_sampleRate / context.transport.tempo;
                if (blockEnd >= SamplePos(context.transport.loopEndPpq * beatSamples - m_sampleRate * .5))
                    prepareRange(schedule.get(), SamplePos(context.transport.loopStartPpq * beatSamples), false);
            }
            m_lastHint = blockStart; m_hintSchedule = schedule.get();
        }

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
            const double inStep = clip.fadeInSamples > 0 ? 1.0 / double(clip.fadeInSamples) : 0.0;
            const double outStep = clip.fadeOutSamples > 0 ? 1.0 / double(clip.fadeOutSamples) : 0.0;
            const auto shapeFade = [&](double in, double out) {
                float t = 1.0f, exponent = 1.0f;
                if (clip.fadeInSamples > 0 && in < 1.0) {
                    t = float(in); exponent = clip.fadeInExponent;
                }
                if (clip.fadeOutSamples > 0 && out < 1.0 && float(out) < t) {
                    t = float(out); exponent = clip.fadeOutExponent;
                }
                t = std::clamp(t, 0.0f, 1.0f);
                if (clip.fadeEqualPower) return std::sqrt(t);
                return exponent == 1.0f ? t : std::pow(t, exponent);
            };
            const bool cachedFade = fading && count <= m_fadeBuffer.size();
            if (cachedFade) {
                double in = double(clipRelStart) * inStep;
                double out = double(available - clipRelStart) * outStep;
                for (FrameCount i = 0; i < count; ++i, in += inStep, out -= outStep)
                    m_fadeBuffer[i] = shapeFade(in, out);
            }
            const auto fadeAt = [&](SamplePos pos) {
                if (!fading) return 1.0f;
                return cachedFade ? m_fadeBuffer[std::size_t(pos - clipRelStart)]
                                  : shapeFade(double(pos) * inStep, double(available - pos) * outStep);
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
                const double timeRatio = std::max(clip.stretchTime, 0.001);
                if (!tape && clip.stretcher) {
                    const dsp::StretchSource source{clip.audio.get(), sourceBegin, sourceEnd,
                                                   loopBegin, loopEnd, looping ? clip.loopMode : 0};
                    for (FrameCount done = 0; done < count;) {
                        const auto n = std::min<FrameCount>(count - done, m_stretchLeft.size());
                        clip.stretcher->render(source, double(clipRelStart + done) * step / timeRatio,
                            1.0 / timeRatio, clip.stretchPitch, clip.formant,
                            m_stretchLeft.data(), m_stretchRight.data(), n);
                        for (ChannelCount ch = 0; ch < channels; ++ch) {
                            const float* stretched = ch == 0 ? m_stretchLeft.data() : m_stretchRight.data();
                            float* destination = context.output.data(ch) + destinationOffset + done;
                            const float gain = channelGain(ch);
                            for (FrameCount i = 0; i < n; ++i)
                                destination[i] += stretched[i] * gain * fadeAt(clipRelStart + done + i);
                        }
                        done += n;
                    }
                    return;
                }
                const bool shifting = std::abs(clip.formant) > 0.001;
                const double formantTilt = std::tanh(clip.formant / 12.0);

                // The controller supplies source bounds for ordinary clips too.
                // At integral positions with no rate/edit transform, cubic
                // interpolation is exactly the source sample. Read contiguous
                // pinned spans instead of four cache lookups per output sample.
                if (step == 1.0 && timeRatio == 1.0 && !tape && !looping &&
                    !shifting && sourceBegin == std::floor(sourceBegin)) {
                    const double first = sourceBegin + double(clipRelStart);
                    const auto usable = FrameCount(std::clamp(
                        std::ceil(sourceEnd) - first, 0.0, double(count)));
                    for (ChannelCount ch = 0; ch < channels; ++ch) {
                        const auto sourceChannel = std::min<ChannelCount>(
                            ch, clip.audio->channels() - 1);
                        const float gain = channelGain(ch);
                        float* destination = context.output.data(ch) + destinationOffset;
                        for (FrameCount done = 0; done < usable;) {
                            const auto part = clip.audio->readSpan(
                                sourceChannel, FrameCount(first) + done, usable - done);
                            if (part.empty()) break;
                            if (!fading) {
                                dsp::addScaled({destination + done, part.size()}, part, gain);
                            } else {
                                for (std::size_t i = 0; i < part.size(); ++i)
                                    destination[done + i] += part[i] * gain *
                                        fadeAt(clipRelStart + done + SamplePos(i));
                            }
                            done += FrameCount(part.size());
                        }
                    }
                    return;
                }

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
                    auto at = [&](std::int64_t i) {
                        return clip.audio->readSample(sourceChannel, FrameCount(std::clamp<std::int64_t>(
                            i, 0, std::int64_t(frames) - 1)));
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
                    const float gain = channelGain(ch);
                    float* destination = context.output.data(ch) + destinationOffset;
                    for (FrameCount i = 0; i < count; ++i) {
                        const SamplePos timelineFrame = clipRelStart + i;
                        const double outputFrame = tape
                            ? sourceProgressAt(timelineFrame)
                            : double(timelineFrame);
                        double value = read(ch, sourceBegin + outputFrame * step / timeRatio);

                        if (shifting) {
                            const double basePos = sourceBegin + outputFrame * step / timeRatio;
                            const double low = (read(ch, basePos - 1.0) +
                                                read(ch, basePos) +
                                                read(ch, basePos + 1.0)) / 3.0;
                            const double high = value - low;
                            value = low * (1.0 - 0.45 * formantTilt) +
                                    high * (1.0 + 0.75 * formantTilt);
                        }
                        destination[i] += float(value) * gain *
                                          (fading ? fadeAt(clipRelStart + i) : 1.0f);
                    }
                }
                return;
            }

            for (ChannelCount ch = 0; ch < channels; ++ch) {
                const float gain = channelGain(ch);
                const auto sourceChannel = std::min<ChannelCount>(ch, clip.audio->channels() - 1);
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
                        for (FrameCount done = 0; done < usable;) {
                            const auto part = clip.audio->readSpan(sourceChannel, FrameCount(base + done), usable - done);
                            if (part.empty()) break;
                            dsp::addScaled({destination + done, part.size()}, part, gain);
                            done += FrameCount(part.size());
                        }
                        continue;
                    }
                    for (FrameCount i = 0; i < usable; ++i) {
                        destination[i] +=
                            clip.audio->readSample(sourceChannel, FrameCount(base + i)) * gain * fadeAt(clipRelStart + i);
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
                        const float a = clip.audio->readSample(sourceChannel, FrameCount(index));
                        const float b = clip.audio->readSample(sourceChannel, FrameCount(index + 1));
                        const float fg = fading ? fadeAt(clipRelStart + i) : 1.0f;
                        destination[i] +=
                            (a + (b - a) * fraction) * gain * fg;
                    }
                }
            }
        };

        // In-order traversal keeps floating-point summation deterministic.
        // Expired subtrees are pruned even after a seek or a snapshot edit;
        // thousands of overlaps require no fixed-capacity fallback or RT resize.
        auto visit = [&](auto&& self, std::size_t first, std::size_t last) -> void {
            if (first == last) return;
            const auto mid = first + (last - first) / 2;
            if (schedule->subtreeMaxEnd[mid] <= blockStart) return;
            self(self, first, mid);
            const auto& clip = (*clips)[mid];
            if (clip.startSample >= blockEnd) return;
            if (clipEnd(clip) > blockStart) renderClip(clip);
            self(self, mid + 1, last);
        };
        visit(visit, 0, clips->size());
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

    std::string m_name;
    RealtimeSnapshot<ClipSchedule> m_clips;
    SamplePos m_lastHint = 0;
    const ClipSchedule* m_hintSchedule = nullptr;
    SampleRate m_sampleRate = 48000.0;
    std::vector<float> m_fadeBuffer;
    std::array<float, 256> m_stretchLeft{}, m_stretchRight{};

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
