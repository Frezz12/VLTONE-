#include "Internal/PitchCorrectorDSP.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace daw::plugins::pitch;
namespace {
constexpr double pi = std::numbers::pi;
int failures = 0;
void check(bool passed, const char* message) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}
double cents(double actual, double expected) {
    return actual > 0 ? 1200*std::log2(actual/expected) : 1e9;
}
double measuredFrequency(const std::vector<float>& audio, double rate) {
    double first = -1, last = -1;
    int count = 0;
    for (std::size_t i = audio.size()*2/3+1; i < audio.size(); ++i) {
        if (audio[i-1] <= 0 && audio[i] > 0) {
            const double crossing = i-1-audio[i-1]/double(audio[i]-audio[i-1]);
            if (first < 0) first = crossing;
            last = crossing; ++count;
        }
    }
    return count > 2 ? (count-1)*rate/(last-first) : 0;
}
void registers() {
    // Non-integer periods exercise the short-lag estimator, not just tones
    // whose periods happen to coincide with its downsampled sample grid.
    for (double rate : {12000., 44100., 48000., 96000., 192000.}) for (int quality : {0, 1}) for (double retune : {0., 20.}) {
        for (double target : {1318.51022765148, 1760., 2349.31814333926, 3135.96348785399, 3729.31009214472}) {
            PitchCorrectorDSP dsp;
            dsp.prepare(rate, 128, 1, quality);
            Settings settings; settings.tune = tuneFromMilliseconds(retune);
            settings.voice = target > 3000 ? 3 : 0;
            // Also cover the default formant-preserving path at normal rates.
            settings.formants = rate == 48000;
            const double hz = target*std::exp2(29./1200);
            std::vector<float> input(std::size_t(.35*rate)), output(input.size());
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = float(.2*std::sin(2*pi*hz*i/rate));
            for (std::size_t i = 0; i < input.size(); i += 128) {
                const float* in[]{input.data()+i}; float* out[]{output.data()+i};
                dsp.process(in, out, unsigned(std::min<std::size_t>(128, input.size()-i)), settings);
            }
            const auto state = dsp.telemetry();
            const double detected = cents(state.inputHz, hz);
            const double rendered = cents(measuredFrequency(output, rate), target);
            std::printf("  rate=%.0f quality=%d retune=%.0f input=%.2f detected=%+.2fc output=%+.2fc correction=%+.2fc confidence=%.3f\n",
                rate, quality, retune, hz, detected, rendered, state.correctionCents, state.confidence);
            check(state.voiced && std::abs(detected) < 5 && std::abs(rendered) < 5 &&
                  std::abs(state.correctionCents+29) < 5,
                  "high notes are detected in the right octave and actually corrected");
        }
    }
}
void difficultVoices() {
    constexpr double rate = 48000;
    for (int quality : {0, 1}) for (int shape = 0; shape < 6; ++shape) {
        PitchCorrectorDSP dsp; dsp.prepare(rate, 64, 1, quality);
        Settings settings; settings.tune = 100; settings.formants = false;
        const double target = shape == 0 ? 220. : shape == 1 ? 880. : shape == 5 ? 110. : 1760.;
        const double hz = target*std::exp2(-37./1200);
        double detectionError = 0, correctionError = 0;
        int voiced = 0, observations = 0;
        std::uint32_t random = 1729;
        std::vector<float> rendered(24000);
        bool finite = true;
        for (int i = 0; i < 24000; ++i) {
            const double phase = 2*pi*hz*i/rate;
            random = random*1664525u+1013904223u;
            const double noise = double(random)/UINT32_MAX*2-1;
            double sample = 0;
            if (shape == 0) sample = .01*std::sin(phase)+.15*std::sin(2*phase)+.1*std::sin(3*phase);
            if (shape == 1) sample = .02*std::sin(phase)+.1*std::sin(3*phase)+.15*std::sin(4*phase);
            if (shape == 2) sample = .15*std::sin(phase)+.06*noise;
            if (shape == 3 || shape == 5) sample = .0005*std::sin(phase); // Quiet, but perfectly pitched.
            if (shape == 4) sample = .3+.04*std::sin(phase)+.006*noise; // DC must not inflate confidence.
            float input = float(sample), output = 0;
            const float* in[]{&input}; float* out[]{&output}; dsp.process(in, out, 1, settings);
            rendered[i] = output;
            finite &= std::isfinite(output) && std::abs(output) < 1;
            if (i > 12000 && i%96 == 0) {
                const auto state = dsp.telemetry(); ++observations;
                voiced += state.voiced;
                detectionError = std::max(detectionError, std::abs(cents(state.inputHz, hz)));
                correctionError = std::max(correctionError, std::abs(state.correctionCents-37));
            }
        }
        std::printf("  quality=%d shape=%d voiced=%d/%d detection=%.2fc correction=%.2fc\n",
            quality, shape, voiced, observations, detectionError, correctionError);
        const bool tunedOutput = (shape != 3 && shape != 5) || std::abs(cents(measuredFrequency(rendered, rate), target)) < 5;
        check(finite && voiced == observations && detectionError < 8 && correctionError < 8 && tunedOutput,
              "weak fundamentals, bright vowels, breath, quiet tones and DC retain full correction");
    }
}
void startup() {
    for (double rate : {12000., 44100., 48000., 96000.}) for (int quality : {0, 1}) {
        PitchCorrectorDSP dsp; dsp.prepare(rate, 64, 1, quality);
        Settings settings; settings.tune = 100; settings.formants = false;
        const double target = 1760, hz = target*std::exp2(32./1200);
        const auto latency = dsp.latencySamples();
        const int duration = int(.04*rate);
        int matched = 0, measured = 0;
        for (int i = 0; i < duration+int(latency); ++i) {
            float input = i < duration ? float(.2*std::sin(2*pi*hz*i/rate)) : 0, output = 0;
            const float* in[]{&input}; float* out[]{&output}; dsp.process(in, out, 1, settings);
            if (i < int(latency+.025*rate)) continue;
            const auto state = dsp.telemetry(); ++measured;
            matched += state.voiced && std::abs(cents(state.inputHz, hz)) < 5 &&
                std::abs(state.correctionCents+32) < 5;
        }
        check(measured > 100 && matched > .95*measured,
              "a first 40 ms high note is acquired without filling the entire bass history");
    }
}
void fastNotes() {
    constexpr double rate = 48000, noteSeconds = .050;
    constexpr std::array<double, 6> targets{880., 1760., 1318.51022765148, 2637.02045530296, 987.76660251225, 1975.53320502450};
    for (int quality : {0, 1}) {
        PitchCorrectorDSP dsp; dsp.prepare(rate, 64, 1, quality);
        Settings settings; settings.tune = 100; settings.formants = false;
        double phase = 0;
        int matched = 0, observations = 0;
        for (int i = 0; i < 48000; ++i) {
            const double time = i/rate;
            const int note = int(std::max(0., time-.25)/noteSeconds)%targets.size();
            phase += 2*pi*targets[note]*std::exp2(32./1200)/rate;
            float input = float(.17*std::sin(phase)+.035*std::sin(2*phase)), output = 0;
            const float* in[]{&input}; float* out[]{&output}; dsp.process(in, out, 1, settings);
            const double sourceTime = time-dsp.latencySamples()/rate;
            if (sourceTime < .28 || std::fmod(sourceTime-.25, noteSeconds) < .025 || i%96) continue;
            const int expected = int((sourceTime-.25)/noteSeconds)%targets.size();
            const auto state = dsp.telemetry(); ++observations;
            matched += state.voiced && std::abs(cents(state.targetHz, targets[expected])) < 1 &&
                std::abs(state.correctionCents+32) < 8;
        }
        std::printf("  quality=%d short notes corrected=%d/%d\n", quality, matched, observations);
        check(observations > 100 && matched >= .95*observations,
              "zero retune follows 50 ms notes and octave jumps without confirmation gaps");
    }
}
void unpitched() {
    constexpr double rate = 48000;
    for (int quality : {0, 1}) for (int shape = 0; shape < 3; ++shape) {
        PitchCorrectorDSP dsp; dsp.prepare(rate, 64, 1, quality);
        Settings settings; settings.tune = 100;
        std::uint32_t random = 71;
        int voiced = 0;
        bool finite = true;
        for (int i = 0; i < 24000; ++i) {
            random = random*1664525u+1013904223u;
            float input = shape == 0 ? 0.f : shape == 1 ? .1f : float(.2*(double(random)/UINT32_MAX*2-1));
            float output = 0;
            const float* in[]{&input}; float* out[]{&output}; dsp.process(in, out, 1, settings);
            if (i > 12000) voiced += dsp.telemetry().voiced;
            finite &= std::isfinite(output) && std::abs(output) < 1;
        }
        check(finite && voiced < 120,
              "silence, DC and noise do not acquire a musical pitch");
    }
}
void tailLevel() {
    constexpr double rate = 48000;
    constexpr unsigned frames = 24073;
    for (int quality : {0, 1}) {
        PitchCorrectorDSP dsp; dsp.prepare(rate, 128, 1, quality);
        Settings settings; settings.tune = 100;
        const auto delay = dsp.latencySamples();
        std::vector<float> input(frames+dsp.tailSamples()), output(input.size());
        for (unsigned i = 0; i < frames; ++i) {
            const double phase = 2*pi*171*i/rate;
            input[i] = float(.14*(std::sin(phase)+.3*std::sin(2*phase)+.17*std::sin(3*phase)));
        }
        for (std::size_t i = 0; i < input.size(); i += 128) {
            const float* in[]{input.data()+i}; float* out[]{output.data()+i};
            dsp.process(in, out, unsigned(std::min<std::size_t>(128, input.size()-i)), settings);
        }
        double dry = 0, wet = 0;
        for (unsigned i = frames-2400; i < frames; ++i) {
            dry += double(input[i])*input[i];
            wet += double(output[i+delay])*output[i+delay];
        }
        const double gainDb = 10*std::log10(wet/dry);
        std::printf("  quality=%d last-syllable gain=%+.3fdB\n", quality, gainDb);
        check(std::isfinite(gainDb) && std::abs(gainDb) < 3,
              "formant protection uses the delayed voice level through the final syllable");
    }
}
} // namespace

int main() {
    registers(); difficultVoices(); fastNotes(); startup(); unpitched(); tailLevel();
    std::printf("Pitch tracking: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
