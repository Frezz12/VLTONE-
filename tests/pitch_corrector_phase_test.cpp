#include "Internal/PitchCorrectorDSP.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace daw::plugins::pitch;

// A vocal pulse's partials must remain related in phase after correction.
// Magnitude-spectrum and steady-F0 tests alone miss the metallic dispersion
// heard in the original spectral backend. The source below has a known pulse
// shape and a moving F0; measuring phase(h)-h*phase(1) removes arbitrary delay.
int main() {
    constexpr double pi = std::numbers::pi;
    int failures = 0;
    for (double rate : {44100., 48000., 96000.}) for (int quality : {0, 1}) {
        for (double target : {110., 220.}) {
            PitchCorrectorDSP processor;
            processor.prepare(rate, 128, 1, quality);
            Settings settings;
            settings.tune = 100;
            settings.humanize = settings.vibrato = 0;
            settings.formants = false;
            const auto count = std::size_t(rate*1.6);
            const auto delay = processor.latencySamples();
            std::vector<float> input(count+delay), output(input.size());
            double phase = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const double t = i/rate;
                const double cents = 27+9*std::sin(2*pi*4.3*t);
                phase += 2*pi*target*std::exp2(cents/1200)/rate;
                const double envelope = std::clamp(std::min(t, 1.6-t)/.02, 0., 1.);
                for (int h = 1; h <= 9; ++h)
                    input[i] += float(envelope*.16/h*std::cos(h*phase+.17*h*h));
            }
            for (std::size_t i = 0; i < input.size(); i += 128) {
                const float* in[]{input.data()+i};
                float* out[]{output.data()+i};
                processor.process(in, out, unsigned(std::min<std::size_t>(128, input.size()-i)), settings);
            }
            double worst = 0;
            for (double center : {.7, .95, 1.2}) {
                const auto begin = std::size_t((center-.08)*rate);
                const auto length = std::size_t(.16*rate);
                std::array<std::complex<double>, 9> partials{};
                for (std::size_t i = 0; i < length; ++i) {
                    const double window = .5-.5*std::cos(2*pi*i/(length-1));
                    const double angle = -2*pi*target*(begin+i)/rate;
                    for (int h = 1; h <= 9; ++h)
                        partials[h-1] += double(output[begin+i+delay])*window*std::polar(1., h*angle);
                }
                double error = 0, weight = 0;
                for (int h = 2; h <= 9; ++h) {
                    const double observed = std::arg(partials[h-1])-h*std::arg(partials[0]);
                    const double expected = .17*(h*h-h);
                    const double delta = std::remainder(observed-expected, 2*pi);
                    const double w = 1./(h*h);
                    error += w*delta*delta;
                    weight += w;
                }
                worst = std::max(worst, std::sqrt(error/weight));
            }
            const bool finite = std::ranges::all_of(output, [](float x) { return std::isfinite(x); });
            const bool passed = finite && worst < .20;
            std::printf("%s pulse phase mode=%s rate=%.0f F0=%.0f error=%.5frad\n",
                        passed ? "PASS" : "FAIL", quality ? "HD" : "Real-Time", rate, target, worst);
            failures += !passed;
        }
    }
    return failures ? 1 : 0;
}
