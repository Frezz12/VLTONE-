#include "DSP/LoudnessMeter.hpp"
#include <algorithm>
#include <cmath>

namespace daw::engine {

double LoudnessMeter::Filter::tick(double x, std::size_t channel) noexcept {
    auto& s = state[channel];
    const double y = b0 * x + s[0];
    s[0] = b1 * x - a1 * y + s[1];
    s[1] = b2 * x - a2 * y;
    return y;
}

void LoudnessMeter::prepare(double rate) {
    if (!std::isfinite(rate) || rate < 8000) rate = 48000;
    m_sliceFrames = unsigned(std::llround(rate * .1));
    // Inverse/forward bilinear transform of the published 48 kHz biquads.
    // This preserves the analogue response when the device rate changes.
    const auto atRate = [rate](double b0, double b1, double b2, double a1, double a2) {
        const double r = rate / 48000;
        const auto remap = [r](double c0, double c1, double c2) {
            const double p = c0 + c1 + c2, q = 2 * (c0 - c2) * r,
                         t = (c0 - c1 + c2) * r * r;
            return std::array<double, 3>{p + q + t, 2 * (p - t), p - q + t};
        };
        const auto b = remap(b0, b1, b2), a = remap(1, a1, a2);
        Filter filter;
        filter.b0 = b[0] / a[0]; filter.b1 = b[1] / a[0]; filter.b2 = b[2] / a[0];
        filter.a1 = a[1] / a[0]; filter.a2 = a[2] / a[0];
        return filter;
    };
    // https://www.itu.int/rec/R-REC-BS.1770
    m_shelf = atRate(1.53512485958697, -2.69169618940638, 1.19839281085285,
                    -1.69065929318241, .73248077421585);
    m_highpass = atRate(1, -2, 1, -1.99004745483398, .99007225036621);
    m_tree.resize(kBins + 1);
    clear(); m_reset.store(false, std::memory_order_relaxed);
}

void LoudnessMeter::clearWindows() noexcept {
    m_slices.fill(0); m_sliceCount = 0; m_frames = 0; m_energy = 0;
    m_momentary.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
    m_shortTerm.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
}

void LoudnessMeter::clear() noexcept {
    clearWindows();
    m_shelf.state = {}; m_highpass.state = {};
    std::fill(m_tree.begin(), m_tree.end(), Bin{});
    m_total = {}; m_integrating = false;
    m_integrated.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
}

float LoudnessMeter::loudness(double energy) noexcept {
    return energy > 1e-30 ? float(-.691 + 10 * std::log10(energy))
                         : -std::numeric_limits<float>::infinity();
}

LoudnessLevels LoudnessMeter::levels() const noexcept {
    if (m_reset.load(std::memory_order_acquire)) return {};
    return {m_momentary.load(std::memory_order_relaxed),
            m_shortTerm.load(std::memory_order_relaxed),
            m_integrated.load(std::memory_order_relaxed)};
}

LoudnessMeter::Bin LoudnessMeter::prefix(int bins) const noexcept {
    Bin result;
    for (int i = std::clamp(bins, 0, kBins); i > 0; i -= i & -i) {
        result.energy += m_tree[i].energy; result.count += m_tree[i].count;
    }
    return result;
}

void LoudnessMeter::addIntegrated(double energy) noexcept {
    constexpr double absoluteGate = 1.1724653045822983e-7; // -70 LUFS
    if (energy > absoluteGate) {
        const int bin = std::clamp(int(std::floor((loudness(energy) + 70.0) * 100)), 0, kBins - 1);
        for (int i = bin + 1; i <= kBins; i += i & -i) {
            m_tree[i].energy += energy; ++m_tree[i].count;
        }
        m_total.energy += energy; ++m_total.count;
    }
    if (!m_total.count) {
        m_integrated.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
        return;
    }
    const double relativeGate = loudness(m_total.energy / double(m_total.count)) - 10;
    const int below = int(std::floor((relativeGate + 70) * 100));
    const auto excluded = prefix(below);
    const auto count = m_total.count - excluded.count;
    m_integrated.store(count ? loudness((m_total.energy - excluded.energy) / double(count))
                             : -std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
}

void LoudnessMeter::finishSlice(bool integrating) noexcept {
    m_slices[m_sliceCount % m_slices.size()] = m_energy / m_sliceFrames;
    ++m_sliceCount;
    double momentary = 0, shortTerm = 0;
    for (unsigned ago = 0; ago < std::min<std::uint64_t>(m_sliceCount, m_slices.size()); ++ago) {
        const double energy = m_slices[(m_sliceCount - 1 - ago) % m_slices.size()];
        shortTerm += energy;
        if (ago < 4) momentary += energy;
    }
    if (m_sliceCount >= 4) {
        m_momentary.store(loudness(momentary / 4), std::memory_order_relaxed);
        if (integrating) addIntegrated(momentary / 4);
    }
    if (m_sliceCount >= 30) m_shortTerm.store(loudness(shortTerm / 30), std::memory_order_relaxed);
    m_frames = 0; m_energy = 0;
}

void LoudnessMeter::process(const AudioBlock& audio, FrameCount frames, bool integrating) noexcept {
    if (m_tree.empty()) return;
    if (m_reset.exchange(false, std::memory_order_acq_rel)) clear();
    if (integrating && !m_integrating) clearWindows();
    m_integrating = integrating;
    const auto channels = std::min<ChannelCount>(2, audio.numChannels());
    frames = std::min(frames, audio.frames());
    for (FrameCount frame = 0; frame < frames; ++frame) {
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            const double raw = audio.data(ch)[frame];
            const double sample = m_highpass.tick(m_shelf.tick(std::isfinite(raw) ? raw : 0, ch), ch);
            m_energy += sample * sample;
        }
        if (++m_frames == m_sliceFrames) finishSlice(integrating);
    }
}

} // namespace daw::engine
