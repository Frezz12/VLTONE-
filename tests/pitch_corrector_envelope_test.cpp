#include "Internal/PitchCorrectorDSP.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace daw::plugins::pitch;

// A voiced phrase with short breathy consonants. The uncertainty at their
// boundaries must not restart an anti-phase dry/wet blend and punch holes in
// the next vowel. This is an envelope test, independent of the F0 estimator.
int main() {
    constexpr double rate = 48000, pi = std::numbers::pi;
    constexpr int frames = 3*48000;
    std::vector<float> input(frames);
    std::uint32_t random = 713;
    double phase = 0;
    for (int i = 0; i < frames; ++i) {
        const double time = i/rate;
        const double hz = 237*std::exp2(.035*std::sin(2*pi*5.2*time));
        phase += 2*pi*hz/rate;
        random = random*1664525u+1013904223u;
        const double noise = (double(random)/UINT32_MAX*2-1)*.22;
        const double vowel = .2*std::sin(phase)+.07*std::sin(2*phase)+.035*std::sin(3*phase);
        const double elapsed = std::fmod(time, .187);
        const double breath = elapsed > .13 && elapsed < .148 ? 1 : 0;
        input[i] = float(vowel*(1-breath)+noise*breath);
    }
    int failed = 0;
    // A clean sung slide should become a staircase at 0 ms: the hard endpoint
    // crosses the chromatic boundary earlier and holds the old plateau while
    // confirming the next one. Uncertain excursions are tested separately.
    for (int quality : {0, 1}) {
        std::array<double, 2> firstChange{10, 10}, error{};
        std::array<int, 2> measured{};
        for (int mode = 0; mode < 2; ++mode) {
            PitchCorrectorDSP processor; processor.prepare(rate, 64, 1, quality);
            Settings settings; settings.tune = tuneFromMilliseconds(mode ? .5 : 0);
            settings.humanize = settings.vibrato = 0; settings.formants = false;
            double oscillator = 0;
            for (int i = 0; i < 72000; ++i) {
                const double time = i/rate;
                const double note = 68.1+1.8*std::clamp((time-.4)/.8, 0.0, 1.0);
                oscillator += 2*pi*440*std::exp2((note-69)/12)/rate;
                float in = float(.2*std::sin(oscillator)), out;
                const float* src[]{&in}; float* dst[]{&out}; processor.process(src,dst,1,settings);
                const auto meter = processor.telemetry();
                if (time > .4 && meter.targetHz > 430) firstChange[mode] = std::min(firstChange[mode],time);
                if (time > .5 && time < 1.15 && meter.confidence > .985 && meter.targetHz > 0) {
                    error[mode] += std::abs(1200*std::log2(meter.inputHz/meter.targetHz)+meter.correctionCents);
                    ++measured[mode];
                }
            }
            error[mode] /= std::max(1, measured[mode]);
        }
        const bool passed = firstChange[0]+.015 < firstChange[1] && measured[0] > 1000 && error[0] < error[1]*.7;
        std::printf("%s mode=%s 0ms/0.5ms destination %.4f/%.4fs plateau error %.3f/%.3fc\n",
                    passed ? "PASS" : "FAIL", quality ? "HD" : "Real-Time", firstChange[0], firstChange[1], error[0], error[1]);
        failed += !passed;
    }
    // A brief uncertain excursion must not become an extra chromatic note;
    // a subsequent intentional sustained change still has to be accepted.
    {
        PitchCorrectorDSP processor; processor.prepare(rate, 64, 1, 0);
        Settings settings; settings.tune = 100; settings.humanize = settings.vibrato = 0;
        double oscillator = 0, firstNewNote = 10;
        std::uint32_t excursionNoise = 419;
        bool falseNote = false;
        for (int i = 0; i < 72000; ++i) {
            const double time = i/rate;
            const double hz = time >= .700 && time < .718 ? 205 : time >= 1.1 ? 210 : 188;
            oscillator += 2*pi*hz/rate;
            float in = float(.16*std::sin(oscillator)+.035*std::sin(3*oscillator)), out = 0;
            excursionNoise = excursionNoise*1664525u+1013904223u;
            if (time >= .700 && time < .718)
                in += float(.10*(double(excursionNoise)/UINT32_MAX*2-1));
            const float* src[]{&in}; float* dst[]{&out}; processor.process(src, dst, 1, settings);
            const auto state = processor.telemetry();
            if (time > .68 && time < .84 && state.targetHz > 190) falseNote = true;
            if (time >= 1.1 && state.targetHz > 205) firstNewNote = std::min(firstNewNote, time);
        }
        const bool passed = !falseNote && firstNewNote < 1.18;
        std::printf("%s uncertain excursion creates no false note; sustained destination accepted at %.3fs\n",
                    passed ? "PASS" : "FAIL", firstNewNote);
        failed += !passed;
    }
    // A voiced accent changes amplitude, not phase. Recentring the read head
    // in anticipation of that level rise used to produce a short pitch chirp.
    for (double accent : {.7, .8, .9, 1.0, 1.1}) {
        PitchCorrectorDSP processor; processor.prepare(rate, 64, 1, 0);
        Settings settings; settings.tune = 100; settings.humanize = settings.vibrato = 0; settings.formants = false;
        std::vector<float> rendered(76800);
        for (int i = 0; i < int(rendered.size()); ++i) {
            const double time = i/rate;
            const double gain = .04+.16*std::clamp((time-accent)/.002, 0.0, 1.0);
            float in = float(gain*std::sin(2*pi*188*time));
            const float* src[]{&in}; float* dst[]{rendered.data()+i}; processor.process(src, dst, 1, settings);
        }
        double previous = -1, minimum = 1000, maximum = 0;
        const int first = int((accent-.04)*rate)+processor.latencySamples();
        const int last = int((accent+.04)*rate)+processor.latencySamples();
        for (int i = first; i < last; ++i) {
            if (rendered[i-1] <= 0 && rendered[i] > 0) {
                const double crossing = i-1-rendered[i-1]/double(rendered[i]-rendered[i-1]);
                if (previous >= 0) {
                    const double hz = rate/(crossing-previous);
                    minimum = std::min(minimum, hz); maximum = std::max(maximum, hz);
                }
                previous = crossing;
            }
        }
        const bool passed = minimum > 179 && maximum < 196;
        std::printf("%s voiced accent %.1fs retains continuous phase, cycle frequency %.2f..%.2fHz\n",
                    passed ? "PASS" : "FAIL", accent, minimum, maximum);
        failed += !passed;
    }
    for (int quality : {0, 1}) for (bool hard : {false, true}) {
        PitchCorrectorDSP processor;
        processor.prepare(rate, 128, 1, quality);
        Settings settings;
        settings.tune = 40; settings.humanize = 60; settings.vibrato = 70; // Natural factory style.
        if (hard) { settings.tune = 100; settings.humanize = settings.vibrato = 0; }
        const auto delay = processor.latencySamples();
        std::vector<float> output(input.size()+delay), padded(output.size());
        std::copy(input.begin(), input.end(), padded.begin());
        for (std::size_t i = 0; i < padded.size(); i += 128) {
            const float* source[]{padded.data()+i};
            float* destination[]{output.data()+i};
            processor.process(source, destination, std::uint32_t(std::min<std::size_t>(128, padded.size()-i)), settings);
        }
        double minimum = 0;
        constexpr int window = 960;
        for (int i = 14400; i+window < frames; i += 96) {
            double dry = 0, wet = 0;
            for (int j = 0; j < window; ++j) {
                dry += input[i+j]*input[i+j];
                wet += output[i+j+delay]*output[i+j+delay];
            }
            const double gain = 10*std::log10((wet+1e-20)/(dry+1e-20));
            minimum = std::min(minimum, gain);
        }
        const bool finite = std::ranges::all_of(output, [](float x) { return std::isfinite(x) && std::abs(x) < 1; });
        double peak = 0;
        for (float x : output) peak = std::max(peak, std::abs(double(x)));
        const bool passed = finite && minimum > -4.5;
        std::printf("%s mode=%s preset=%s worst20msGain=%+.3fdB peak=%.5f\n", passed ? "PASS" : "FAIL",
                    quality ? "HD" : "Real-Time", hard ? "Hard" : "Natural", minimum, peak);
        failed += !passed;
    }
    for (int quality : {0, 1}) for (int gap : {240, 720, 1440}) for (unsigned end : {12000, 16320, 24000, 36000}) {
        PitchCorrectorDSP processor;
        processor.prepare(rate, 128, 1, quality);
        Settings settings; settings.tune = 100; settings.humanize = settings.vibrato = 0;
        const unsigned start = end+gap, delay = processor.latencySamples();
        std::vector<float> source(end+24000+delay), output(source.size());
        for (unsigned i = 0; i < end; ++i)
            source[i] = float(.2*std::min({1., i/192., (end-i)/192.})*std::sin(2*pi*67.2*i/rate));
        for (unsigned i = start; i < end+20000; ++i)
            source[i] = float(.2*std::min(1., (i-start)/192.)*std::sin(2*pi*454.4*(i-start)/rate));
        for (std::size_t i = 0; i < source.size(); i += 128) {
            const float* src[]{source.data()+i}; float* dst[]{output.data()+i};
            processor.process(src, dst, std::uint32_t(std::min<std::size_t>(128, source.size()-i)), settings);
        }
        const auto crossing = [](const auto& samples, unsigned begin) {
            for (unsigned i = begin; i < begin+2400; ++i) if (std::abs(samples[i]) > .025) return i;
            return begin+2400;
        };
        // Search on both sides of the expected onset: starting at the nominal
        // onset would silently miss an early read-head attack or pre-echo.
        const int deviation = int(crossing(output, start+delay-96))-int(crossing(source, start))-int(delay);
        const bool passed = std::abs(deviation) <= 48;
        std::printf("%s held-phase onset mode=%s phase=%.0fms gap=%.1fms deviation=%+.3fms\n", passed ? "PASS" : "FAIL",
                    quality ? "HD" : "Real-Time", end/rate*1000, gap/rate*1000, deviation/rate*1000);
        failed += !passed;
    }
    return failed ? 1 : 0;
}
