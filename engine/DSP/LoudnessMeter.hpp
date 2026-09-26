#pragma once

#include "Common/Types.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

namespace daw::engine {

struct LoudnessLevels {
    float momentary = std::numeric_limits<float>::quiet_NaN();
    float shortTerm = std::numeric_limits<float>::quiet_NaN();
    float integrated = std::numeric_limits<float>::quiet_NaN();
};

/// Stereo/mono K-weighted loudness. Audio-thread work and storage are bounded:
/// 100 ms energy slices for M (400 ms), S (3 s), and overlapping I blocks.
/// A 0.01 LU energy histogram approximates the relative gate's boundary; sums
/// retain the actual energies, rather than using bin-centre approximations.
/// Reference: ITU-R BS.1770-5 Annex 1 and EBU Tech 3341 §2.2–2.3.
class LoudnessMeter {
public:
    void prepare(double sampleRate);
    void process(const AudioBlock& audio, FrameCount frames, bool integrating) noexcept;
    LoudnessLevels levels() const noexcept;
    void requestReset() noexcept { m_reset.store(true, std::memory_order_release); }

private:
    struct Filter {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::array<std::array<double, 2>, 2> state{};
        double tick(double x, std::size_t channel) noexcept;
    };
    struct Bin { double energy = 0; std::uint64_t count = 0; };
    static constexpr int kBins = 87001; // -70 … +800 LUFS, including finite float overloads
    void clear() noexcept;
    void clearWindows() noexcept;
    void finishSlice(bool integrating) noexcept;
    void addIntegrated(double energy) noexcept;
    Bin prefix(int bins) const noexcept;
    static float loudness(double energy) noexcept;

    Filter m_shelf, m_highpass;
    std::array<double, 30> m_slices{};
    std::vector<Bin> m_tree;
    Bin m_total;
    std::uint64_t m_sliceCount = 0;
    unsigned m_sliceFrames = 4800, m_frames = 0;
    double m_energy = 0;
    bool m_integrating = false;
    std::atomic<bool> m_reset{false};
    std::atomic<float> m_momentary{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<float> m_shortTerm{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<float> m_integrated{std::numeric_limits<float>::quiet_NaN()};
};

} // namespace daw::engine
