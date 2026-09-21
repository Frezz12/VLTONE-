#include "WarpAnalysis.hpp"
#include "analysis/Signal.hpp"

namespace daw::analysis {
std::vector<WarpTransient> detectWarpTransients(const engine::SampleBuffer& audio,
    double begin, double end, const std::function<bool()>& keepGoing) {
    constexpr std::size_t size = 1024, hop = 256;
    const auto first = std::size_t(std::clamp(begin * audio.sampleRate(), 0., double(audio.frames())));
    const auto last = std::size_t(std::clamp(end * audio.sampleRate(), 0., double(audio.frames())));
    if (last <= first + size || !audio.channels()) return {};
    const auto window = detail::hann(size);
    std::vector<std::complex<double>> bins(size);
    std::vector<double> previous(size / 2 + 1, 0), magnitude(size / 2 + 1, 0), flux;
    flux.reserve((last - first) / hop);
    for (std::size_t pos = first; pos + size <= last; pos += hop) {
        if (keepGoing && !keepGoing()) return {};
        std::fill(magnitude.begin(), magnitude.end(), 0.);
        for (engine::ChannelCount ch = 0; ch < std::min<engine::ChannelCount>(2, audio.channels()); ++ch) {
            for (std::size_t i = 0; i < size; ++i) bins[i] = {audio.channel(ch)[pos + i] * window[i], 0};
            detail::fft(bins);
            for (std::size_t i = 0; i < magnitude.size(); ++i) magnitude[i] += std::abs(bins[i]);
        }
        double value = 0;
        for (std::size_t i = 0; i < magnitude.size(); ++i) {
            const double logged = std::log1p(magnitude[i]);
            value += std::max(0., logged - previous[i]); previous[i] = logged;
        }
        flux.push_back(value);
    }
    if (flux.size() < 3) return {};
    const double peak = *std::max_element(flux.begin() + 1, flux.end());
    if (peak < 1e-6) return {};
    std::vector<WarpTransient> out;
    for (std::size_t i = 1; i + 1 < flux.size(); ++i) {
        if (flux[i] < peak * .025 || flux[i] <= flux[i - 1] || flux[i] < flux[i + 1]) continue;
        // Refine the FFT onset with the strongest short energy increase.
        const auto centre = first + i * hop;
        std::size_t attack = centre;
        double strongest = -1;
        for (std::size_t p = centre; p < std::min(last, centre + size); ++p) {
            double energy = 0;
            for (engine::ChannelCount ch = 0; ch < std::min<engine::ChannelCount>(2, audio.channels()); ++ch) {
                const double sample = audio.channel(ch)[p];
                const double before = p > first + 32 ? audio.channel(ch)[p - 32] : 0;
                energy += sample * sample - before * before;
            }
            if (energy > strongest) { strongest = energy; attack = p; }
        }
        WarpTransient transient{double(attack) / audio.sampleRate(), flux[i] / peak};
        if (!out.empty() && transient.sourceSeconds - out.back().sourceSeconds < .025) {
            if (transient.strength > out.back().strength) out.back() = transient;
        } else out.push_back(transient);
    }
    return out;
}
}
