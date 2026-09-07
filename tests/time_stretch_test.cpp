#include "DSP/TimeStretch.hpp"
#include "Nodes/PlaybackNodes.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>

// Catch accidental lazy FFT/buffer allocation, including the first note/seek.
static thread_local bool watchAllocations = false;
static thread_local int allocations = 0;
void* operator new(std::size_t n) {
    if (watchAllocations) ++allocations;
    if (void* p = std::malloc(std::max<std::size_t>(n, 1))) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { std::free(p); }

using namespace daw::engine;
using namespace daw::engine::dsp;
static int failures = 0;
static void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
static double rms(const std::vector<float>& audio, int start, int count) {
    double energy = 0;
    for (int i = start; i < start + count; ++i) energy += double(audio[i]) * audio[i];
    return std::sqrt(energy / count);
}
static double frequency(const std::vector<float>& audio, int begin, int end, double rate) {
    double first = -1, last = 0; int crossings = 0;
    for (int i = begin + 1; i < end; ++i) if (audio[i - 1] <= 0 && audio[i] > 0) {
        const double at = i - audio[i] / double(audio[i] - audio[i - 1]);
        if (first < 0) first = at;
        last = at; ++crossings;
    }
    return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0;
}
struct Audio {
    std::vector<float> l, r;
    explicit Audio(int n) : l(n), r(n) {}
};
static Audio render(TimeStretch& stretch, const StretchSource& source, double ratio,
                    int frames, int block = 512, double pitch = 0, double start = 0) {
    Audio result(frames);
    allocations = 0;
    watchAllocations = true;
    for (int i = 0; i < frames; i += block) {
        stretch.render(source, start + i / ratio * source.audio->sampleRate() / stretch.sampleRate(),
                       1 / ratio, pitch, 0, result.l.data() + i, result.r.data() + i,
                       std::min(block, frames - i));
    }
    watchAllocations = false;
    check(allocations == 0, "stretching including initial seek allocates nothing on the audio thread");
    return result;
}

int main() {
    constexpr double rate = 48000;
    auto tone = std::make_shared<SampleBuffer>(2, 144000, rate);
    for (FrameCount i = 0; i < tone->frames(); ++i) {
        tone->writableChannel(0)[i] = float(0.5 * std::sin(2 * std::numbers::pi * 220 * i / rate));
        tone->writableChannel(1)[i] = -0.7f * tone->channel(0)[i];
    }
    StretchSource source{tone.get(), 0, double(tone->frames())};
    for (int mode = 1; mode <= 4; ++mode) {
        TimeStretch stretch(rate, mode);
        auto unity = render(stretch, source, 1, 48000);
        check(std::equal(unity.l.begin(), unity.l.end(), tone->channel(0)) &&
              std::equal(unity.r.begin(), unity.r.end(), tone->channel(1)),
              "enabling Stretch at original tempo is sample-exact");
        for (double ratio : {0.5, 0.75, 1.25, 2.0, 4.0}) {
            stretch.reset();
            auto output = render(stretch, source, ratio, int(96000 * ratio));
            const int begin = 12000, end = int(output.l.size()) - 12000;
            double low = 10, high = 0, stereoError = 0, jump = 0;
            for (int i = begin; i + 2400 < end; i += 2400) {
                const double level = rms(output.l, i, 2400);
                low = std::min(low, level); high = std::max(high, level);
            }
            for (int i = begin; i < end; ++i) {
                stereoError = std::max(stereoError, std::abs(double(output.r[i] + .7f * output.l[i])));
                jump = std::max(jump, std::abs(double(output.l[i] - output.l[i - 1])));
            }
            const double hz = frequency(output.l, begin, end, rate);
            std::printf("mode=%d time=%.2f rms=%.5f..%.5f pitch=%.3f stereo=%.6f jump=%.5f\n",
                        mode, ratio, low, high, hz, stereoError, jump);
            check(std::abs(hz - 220) < 0.7, "tempo changes preserve pitch");
            check(low > .28 && high / low < 1.12, "sustained sound has no periodic volume holes");
            check(stereoError < .003, "stereo phase and balance survive stretching");
            check(jump < .08, "stretch output is continuous between processing blocks");
        }
        stretch.reset();
        auto regular = render(stretch, source, 1.37, 48000);
        TimeStretch irregular(rate, mode);
        auto split = render(irregular, source, 1.37, 48000, 73);
        check(regular.l == split.l && regular.r == split.r,
              "live/export sound is independent of host block size");
        stretch.reset();
        auto pitched = render(stretch, source, 1.25, 72000, 512, 7);
        std::printf("mode=%d shifted frequency=%.4f\n", mode, frequency(pitched.l, 16000, 60000, rate));
        check(std::abs(1200 * std::log2(frequency(pitched.l, 16000, 60000, rate) /
              (220 * std::pow(2, 7.0/12)))) < 10,
              "pitch remains independent of duration");
        stretch.reset();
        auto seeked = render(stretch, source, 1.25, 12000, 512, 0, 57321);
        check(rms(seeked.l, 0, 2400) > .2 &&
              std::abs(frequency(seeked.l, 2400, 12000, rate) - 220) < 1,
              "seeking into a stretched clip starts immediately at the right pitch");
    }

    // Looping uses the selected region, with a true reflected ping-pong path.
    {
        TimeStretch sweep(rate, 4);
        Audio output(48000);
        double position = 0;
        for (int i = 0; i < 48000; i += 128) {
            const double speed = 1.25 - .5 * i / 48000.0;
            sweep.render(source, position, speed, 0, 0,
                         output.l.data() + i, output.r.data() + i, 128);
            position += 128 * speed;
        }
        double jump = 0;
        for (int i = 12000; i < 48000; ++i)
            jump = std::max(jump, std::abs(double(output.l[i] - output.l[i - 1])));
        check(jump < .08 && std::abs(frequency(output.l, 12000, 47000, rate) - 220) < 1,
              "a moving Time control preserves pitch without resetting phase every block");
    }

    // Looping uses the selected region, with a true reflected ping-pong path.
    auto ramp = std::make_shared<SampleBuffer>(1, 128, rate);
    for (FrameCount i = 0; i < 128; ++i) ramp->writableChannel(0)[i] = float(i);
    TimeStretch loop(rate, 2);
    StretchSource loopSource{ramp.get(), 0, 128, 32, 64, 2};
    auto reflected = render(loop, loopSource, 1, 128);
    check(reflected.l[64] == 62 && reflected.l[94] == 32 && reflected.l[95] == 33,
          "ping-pong reflects without reading past the loop endpoint");

    // Exercise the actual arrangement node: exact placement, mix, duration,
    // fades, and state retained when only gain changes.
    ClipPlayerNode player;
    player.prepare({rate, 512, 2});
    auto clips = std::make_shared<ClipPlayerNode::ClipList>();
    ClipPlacement clip;
    clip.audio = tone; clip.startSample = 100; clip.lengthSamples = 48000;
    clip.sourceStartFrame = 0; clip.sourceEndFrame = 38400;
    clip.stretchMode = 4; clip.stretchTime = 1.25; clip.gain = .5;
    clips->push_back(clip); player.setClips(clips);
    const auto prepared = player.clips()->front().stretcher;
    clips->front().gain = .25; player.setClips(clips);
    check(player.clips()->front().stretcher == prepared,
          "gain edits preserve the running stretch processor");
    Audio arranged(49000);
    allocations = 0;
    watchAllocations = true;
    for (int at = 0; at < 49000; at += 512) {
        float* output[2]{arranged.l.data() + at, arranged.r.data() + at};
        ProcessContext context;
        context.frames = std::min(512, 49000 - at);
        context.output = AudioBlock(output, 2, context.frames);
        context.timelinePosition = at;
        context.playing = true;
        player.process(context);
    }
    watchAllocations = false;
    check(allocations == 0, "the arrangement stretch path allocates nothing");
    check(std::all_of(arranged.l.begin(), arranged.l.begin() + 100, [](float s) { return s == 0; }) &&
          std::all_of(arranged.l.begin() + 48100, arranged.l.end(), [](float s) { return s == 0; }),
          "stretched clips obey their exact timeline start and end");
    check(std::abs(rms(arranged.l, 12000, 12000) - .25 * .5 / std::sqrt(2.0)) < .01 &&
          std::abs(frequency(arranged.l, 12000, 36000, rate) - 220) < 1,
          "the real clip player preserves level and pitch after time stretching");

    // Transients must land on the new beat, including the first and last hit.
    auto hits = std::make_shared<SampleBuffer>(2, 96000, rate);
    for (int hit : {0, 24000, 48000, 72000, 95000})
        for (int i = 0; i < 400 && hit + i < 96000; ++i) {
            const float value = float(std::exp(-i / 80.0) * std::cos(i * .63));
            hits->writableChannel(0)[hit + i] = hits->writableChannel(1)[hit + i] = value;
        }
    for (int mode = 1; mode <= 4; ++mode) {
        TimeStretch transient(rate, mode);
        StretchSource hitSource{hits.get(), 0, 96000};
        auto output = render(transient, hitSource, 1.25, 120000);
        for (int hit : {0, 24000, 48000, 72000, 95000}) {
            const int wanted = int(hit * 1.25);
            int peakAt = wanted; float peak = 0;
            for (int i = std::max(0, wanted - 2400); i < std::min(120000, wanted + 2400); ++i)
                if (std::abs(output.l[i]) > peak) { peak = std::abs(output.l[i]); peakAt = i; }
            std::printf("transient mode=%d at=%d offset=%d peak=%.4f\n", mode, wanted, peakAt - wanted, peak);
            check(peak > .3 && std::abs(peakAt - wanted) < 96,
                  "attacks survive and remain within 2ms of the stretched beat");
        }
    }

    // A stereo chord: preserve each partial and its stereo position, not just
    // the total RMS of a mono sine. Absolute output phase may legitimately move.
    auto chord = std::make_shared<SampleBuffer>(2, 144000, rate);
    constexpr double frequencies[]{220, 329, 554};
    constexpr double amplitudes[]{.25, .20, .10};
    constexpr double balances[]{.7, 1.2, .4};
    constexpr double phases[]{.3, -.5, .8};
    for (int i = 0; i < 144000; ++i) {
        double left = 0, right = 0;
        for (int partial = 0; partial < 3; ++partial) {
            const double phase = 2 * std::numbers::pi * frequencies[partial] * i / rate;
            left += amplitudes[partial] * std::sin(phase);
            right += amplitudes[partial] * balances[partial] * std::sin(phase + phases[partial]);
        }
        chord->writableChannel(0)[i] = float(left);
        chord->writableChannel(1)[i] = float(right);
    }
    const auto partialAt = [&](const std::vector<float>& samples, double hz) {
        std::complex<double> sum{};
        const auto step = std::polar(1.0, -2 * std::numbers::pi * hz / rate);
        auto phase = std::polar(1.0, -2 * std::numbers::pi * hz * 24000 / rate);
        for (int i = 24000; i < 72000; ++i, phase *= step)
            sum += double(samples[i]) * phase;
        return sum * (2.0 / 48000);
    };
    for (int mode = 1; mode <= 4; ++mode) {
        for (double ratio : {.75, 1.5, 4.0}) {
            TimeStretch stretch(rate, mode);
            StretchSource chordSource{chord.get(), 0, 144000};
            const auto output = render(stretch, chordSource, ratio, 96000);
            bool intact = true;
            for (int partial = 0; partial < 3; ++partial) {
                const auto left = partialAt(output.l, frequencies[partial]);
                const auto right = partialAt(output.r, frequencies[partial]);
                const auto balance = right / left;
                const auto expected = std::polar(balances[partial], phases[partial]);
                const double levelError = std::abs(std::abs(left) / amplitudes[partial] - 1);
                const double stereoError = std::abs(balance - expected);
                if (levelError >= .15 || stereoError >= .08) {
                    double best = 0, bestHz = 0;
                    for (int detune = -16; detune <= 16; ++detune) {
                        const double hz = frequencies[partial] + detune * .25;
                        const double level = std::abs(partialAt(output.l, hz));
                        if (level > best) { best = level; bestHz = hz; }
                    }
                    std::printf("chord mode=%d ratio=%.2f hz=%.0f level-error=%.5f stereo-error=%.5f\n",
                                mode, ratio, frequencies[partial], levelError, stereoError);
                    std::printf("nearby peak hz=%.2f level=%.5f\n", bestHz, best);
                }
                intact &= levelError < .15 && stereoError < .08;
            }
            check(intact, "polyphonic stretching preserves chord partials and their stereo positions");
        }
    }
    std::printf("%s\n", failures ? "FAILURES PRESENT" : "ALL PASSED");
    return failures ? 1 : 0;
}
