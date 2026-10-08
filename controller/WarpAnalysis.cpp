#include "WarpAnalysis.hpp"
#include "MediaWorker.hpp"
#include "analysis/Signal.hpp"
#include <array>

namespace daw::analysis {
std::vector<WarpTransient> detectWarpTransients(const engine::SampleBuffer& audio,
    double begin, double end, const std::function<bool()>& keepGoing) {
    if (MediaWorker::enabled()) return MediaWorker::transients(audio, begin, end, keepGoing);
    constexpr std::size_t size = 1024, hop = 256;
    const auto first = std::size_t(std::clamp(begin * audio.sampleRate(), 0., double(audio.frames())));
    const auto last = std::size_t(std::clamp(end * audio.sampleRate(), 0., double(audio.frames())));
    if (last <= first + size || !audio.channels()) return {};
    const auto window = detail::hann(size);
    std::vector<std::complex<double>> bins(size);
    std::vector<double> previous(size / 2 + 1, 0), magnitude(size / 2 + 1, 0), flux;
    flux.reserve((last - first) / hop);
    std::vector<std::array<double, 3>> bands;
    bands.reserve((last - first) / hop);
    for (std::size_t pos = first; pos + size <= last; pos += hop) {
        if (keepGoing && !keepGoing()) return {};
        std::fill(magnitude.begin(), magnitude.end(), 0.);
        for (engine::ChannelCount ch = 0; ch < std::min<engine::ChannelCount>(2, audio.channels()); ++ch) {
            for (std::size_t i = 0; i < size; ++i) bins[i] = {audio.channel(ch)[pos + i] * window[i], 0};
            detail::fft(bins);
            for (std::size_t i = 0; i < magnitude.size(); ++i) magnitude[i] += std::abs(bins[i]);
        }
        double value = 0;
        std::array<double, 3> band{};
        for (std::size_t i = 0; i < magnitude.size(); ++i) {
            const double logged = std::log1p(magnitude[i]);
            const double rise = std::max(0., logged - previous[i]); previous[i] = logged;
            const double hz = i * audio.sampleRate() / size;
            band[hz < 250 ? 0 : hz < 2500 ? 1 : 2] += rise;
            value += rise;
        }
        flux.push_back(value);
        bands.push_back(band);
    }
    if (flux.size() < 3) return {};
    const double peak = *std::max_element(flux.begin() + 1, flux.end());
    if (peak < 1e-6) return {};
    std::vector<WarpTransient> out;
    for (std::size_t i = 1; i + 1 < flux.size(); ++i) {
        if (keepGoing && !keepGoing()) return {};
        if (flux[i] < peak * .025 || flux[i] <= flux[i - 1] || flux[i] < flux[i + 1]) continue;
        // A local floor adapts to quiet passages after loud hits. Separate
        // bands preserve bass attacks without letting treble noise dominate.
        std::array<double, 3> floor{};
        double local = 0; unsigned count = 0;
        const auto radius = std::max<std::size_t>(2, std::size_t(audio.sampleRate() * .12 / hop));
        for (auto j = i > radius ? i - radius : 0; j < std::min(flux.size(), i + radius + 1); ++j) {
            if (j + 1 >= i && j <= i + 1) continue;
            local += flux[j]; ++count;
            for (int k = 0; k < 3; ++k) floor[k] += bands[j][k];
        }
        if (!count) continue;
        local /= count;
        double evidence = 0;
        for (int k = 0; k < 3; ++k) {
            floor[k] /= count;
            if (bands[i][k] > 1e-5) evidence = std::max(evidence,
                (bands[i][k] - floor[k]) / (bands[i][k] + floor[k] * 2 + 1e-6));
        }
        if (flux[i] < local * 1.1 || evidence < .12) continue;
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
        const double contrast = std::clamp((flux[i] - local) / (flux[i] + local * 2 + 1e-6), 0., 1.);
        WarpTransient transient{double(attack) / audio.sampleRate(), std::sqrt(flux[i] / peak),
            std::clamp(.65 * evidence + .35 * contrast, 0., 1.)};
        if (!out.empty() && transient.sourceSeconds - out.back().sourceSeconds < .025) {
            if (transient.strength > out.back().strength) out.back() = transient;
        } else out.push_back(transient);
    }
    return out;
}
}
