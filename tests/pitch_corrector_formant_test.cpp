#include "Internal/PitchCorrectorDSP.hpp"
#include "signalsmith-linear/fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace daw::plugins::pitch;
namespace {
constexpr double rate = 48000, pi = std::numbers::pi;
using Complex = std::complex<double>;
struct Vowel {
    const char* name;
    std::array<double, 3> formants, bandwidths;
};
constexpr std::array<Vowel, 3> vowels{{
    {"ah", {700, 1200, 2600}, {90, 120, 180}},
    {"ee", {350, 2100, 2900}, {70, 130, 170}},
    {"oo", {400, 800, 2400}, {80, 100, 170}},
}};

// Independent analytic source/filter fixture: three known resonant pole pairs
// and a tilted harmonic excitation. No coefficients or detector output are
// taken from the implementation under test.
Complex response(double hz, const Vowel& vowel) {
    const auto z = std::polar(1.0, -2*pi*hz/rate);
    Complex result = 1;
    for (std::size_t i = 0; i < vowel.formants.size(); ++i) {
        const double radius = std::exp(-pi*vowel.bandwidths[i]/rate);
        result /= 1.0-2*radius*std::cos(2*pi*vowel.formants[i]/rate)*z+radius*radius*z*z;
    }
    return result;
}
std::vector<float> synthesize(double hz, const Vowel& vowel) {
    std::vector<double> raw(std::size_t(rate), 0.0);
    const int count = int(10000/hz);
    for (int harmonic = 1; harmonic <= count; ++harmonic) {
        const auto gain = response(harmonic*hz, vowel)/std::pow(harmonic, 1.15)*Complex(0, -1);
        const auto step = std::polar(1.0, 2*pi*hz*harmonic/rate);
        Complex phase = 1;
        for (auto& sample : raw) { sample += (phase*gain).real(); phase *= step; }
    }
    double peak = 0;
    for (double x : raw) peak = std::max(peak, std::abs(x));
    std::vector<float> result(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const double envelope = std::clamp(std::min(i/(rate*.004), (raw.size()-i)/(rate*.006)), 0.0, 1.0);
        result[i] = float(raw[i]*.24/peak*envelope);
    }
    return result;
}
std::vector<float> render(const std::vector<float>& source, int quality, bool formants) {
    PitchCorrectorDSP dsp;
    dsp.prepare(rate, 128, 1, quality);
    Settings settings;
    settings.tune = 100; settings.humanize = settings.vibrato = 0;
    settings.scale = 5; settings.key = 0; settings.formants = formants;
    const auto delay = dsp.latencySamples();
    std::vector<float> input(source.size()+delay), output(input.size());
    std::copy(source.begin(), source.end(), input.begin());
    for (std::size_t i = 0; i < input.size(); i += 128) {
        const float* in[]{input.data()+i}; float* out[]{output.data()+i};
        dsp.process(in, out, std::uint32_t(std::min<std::size_t>(128, input.size()-i)), settings);
    }
    return {output.begin()+delay, output.end()};
}
double envelopeError(const std::vector<float>& samples, double fundamental, const Vowel& vowel) {
    constexpr std::size_t size = 65536, start = 24960, count = 17280;
    std::vector<Complex> time(size), spectrum(size);
    for (std::size_t i = 0; i < count; ++i)
        time[i] = samples[start+i]*(.5-.5*std::cos(2*pi*i/(count-1)));
    signalsmith::linear::SimpleFFT<double> fft(size);
    fft.fft(time.data(), spectrum.data());
    double weightSum = 0, weightedError = 0, weightedSquare = 0;
    for (int harmonic = 1; harmonic*fundamental <= 4000; ++harmonic) {
        const double hz = harmonic*fundamental;
        if (hz < 200) continue;
        // Integrate each whole partial band. Narrow FFT bins conflate phase
        // vocoder sidebands with a missing spectral envelope.
        const auto first = std::size_t(std::ceil((hz-.45*fundamental)*size/rate));
        const auto last = std::size_t(std::floor((hz+.45*fundamental)*size/rate));
        double energy = 0;
        for (auto i = first; i <= last; ++i) energy += std::norm(spectrum[i]);
        const double model = std::abs(response(hz, vowel))/std::pow(harmonic, 1.15);
        const double error = 10*std::log10(std::max(energy, 1e-30))-20*std::log10(model);
        // Weight by the independently known partial amplitude, never by the
        // processed output. This keeps a lost audible resonance visible while
        // preventing sub-noise-floor harmonics from dominating a timbre metric.
        weightSum += model; weightedError += model*error; weightedSquare += model*error*error;
    }
    const double gain = weightedError/weightSum;
    return std::sqrt(std::max(0.0, weightedSquare/weightSum-gain*gain));
}
}

int main() {
    int failures = 0;
    for (int quality : {0, 1}) {
        double preservedSum = 0, unpreservedSum = 0;
        for (double target : {82.4068892282, 164.813778456}) for (const auto& vowel : vowels) {
            const double inputHz = target*std::exp2(130./1200);
            const auto source = synthesize(inputHz, vowel);
            const double calibration = envelopeError(source, inputHz, vowel);
            const auto preserved = render(source, quality, true);
            const auto unpreserved = render(source, quality, false);
            const double on = envelopeError(preserved, target, vowel);
            const double off = envelopeError(unpreserved, target, vowel);
            preservedSum += on; unpreservedSum += off;
            const bool finite = std::ranges::all_of(preserved, [](float x) { return std::isfinite(x) && std::abs(x) < 1; });
            const bool passed = calibration < .5 && on < 3.5 && finite;
            failures += !passed;
            std::printf("%s formants mode=%s vowel=%s F0=%.2f calibration=%.2fdB preserved=%.2fdB off=%.2fdB\n",
                passed ? "PASS" : "FAIL", quality ? "HD" : "Real-Time", vowel.name, inputHz, calibration, on, off);
        }
        // A cosmetic toggle or ordinary resampling cannot pass this comparison.
        const bool effective = preservedSum < .8*unpreservedSum;
        failures += !effective;
        std::printf("%s mode=%s preserving formants reduces spectral envelope error by %.1f%%\n",
            effective ? "PASS" : "FAIL", quality ? "HD" : "Real-Time", 100*(1-preservedSum/unpreservedSum));
    }
    return failures ? 1 : 0;
}
