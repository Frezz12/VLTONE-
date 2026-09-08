#include "Signal.hpp"
#include "NeuralModels.hpp"
#include "Calibration.hpp"
#include "DSP/Resampler.hpp"

namespace daw::analysis::detail {
namespace {
using Chroma = std::array<double, 36>;
struct SpectralPeak { double frequency, magnitude; };
struct TonalWindow {
    Chroma chroma{};
    double tuning = 0, weight = 0, diversity = 0;
};

double smallMedian(std::vector<double>& values) {
    const auto mid = values.begin() + std::ptrdiff_t(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

TonalWindow tonalFeatures(std::span<const float> audio, const AnalysisProgress& progress) {
    constexpr std::size_t n = 4096, hop = 512, binsCount = n / 2 + 1;
    TonalWindow out;
    if (audio.size() < n) return out;
    const auto frameCount = 1 + (audio.size() - n) / hop;
    const auto window = hann(n);
    std::vector<std::complex<double>> bins(n);
    std::vector<std::vector<double>> spectra(frameCount, std::vector<double>(binsCount));
    std::vector<double> energy(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::size_t i = 0; i < n; ++i) {
            const double x = audio[frame * hop + i];
            bins[i] = x * window[i]; energy[frame] += x * x / n;
        }
        fft(bins);
        for (std::size_t k = 0; k < binsCount; ++k) spectra[frame][k] = std::abs(bins[k]);
        if ((frame & 31u) == 0) checkpoint(progress, 0.25 * double(frame) / frameCount, "key_profiles");
    }
    std::vector<std::vector<SpectralPeak>> peaks(frameCount);
    std::vector<double> harmonic(binsCount), scratch;
    scratch.reserve(33);
    double tuneX = 0, tuneY = 0;
    const double maxEnergy = *std::max_element(energy.begin(), energy.end());
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        checkpoint(progress, 0.25 + 0.55 * double(frame) / frameCount, "key_profiles");
        if (energy[frame] < std::max(1e-10, maxEnergy * 1e-4)) continue;
        // Median masks (time vs frequency) remove vertical drum transients
        // without a separate stem-separation model or changing the neural input.
        for (std::size_t k = 7; k + 1 < binsCount; ++k) {
            scratch.clear();
            for (std::size_t t = frame > 4 ? frame - 4 : 0; t <= std::min(frameCount - 1, frame + 4); ++t) scratch.push_back(spectra[t][k]);
            const double h = smallMedian(scratch);
            scratch.clear();
            for (std::size_t b = k > 8 ? k - 8 : 0; b <= std::min(binsCount - 1, k + 8); ++b) scratch.push_back(spectra[frame][b]);
            const double p = smallMedian(scratch);
            harmonic[k] = spectra[frame][k] * h * h / std::max(1e-12, h * h + 4 * p * p);
        }
        const auto low = std::size_t(std::ceil(45 * n / kAnalysisRate));
        const auto high = std::size_t(std::floor(5000 * n / kAnalysisRate));
        const double maximum = *std::max_element(harmonic.begin() + low, harmonic.begin() + high);
        double arithmetic = 0, logSum = 0;
        for (std::size_t k = low; k < high; ++k) { arithmetic += harmonic[k]; logSum += std::log(std::max(1e-12, harmonic[k])); }
        const double flatness = std::exp(logSum / (high - low)) / std::max(1e-12, arithmetic / (high - low));
        if (flatness > 0.3 || maximum < 1e-6) continue;
        for (std::size_t k = low; k < high; ++k) {
            const double m = harmonic[k];
            if (m < maximum * 0.035 || m < harmonic[k - 1] || m <= harmonic[k + 1]) continue;
            const double offset = peakOffset(std::log(std::max(1e-12, harmonic[k - 1])), std::log(m), std::log(std::max(1e-12, harmonic[k + 1])));
            const double hz = (double(k) + offset) * kAnalysisRate / n;
            const double note = 69 + 12 * std::log2(hz / 440.0);
            const double angle = 2 * std::numbers::pi * (note - std::round(note));
            const double weight = std::sqrt(m);
            tuneX += weight * std::cos(angle); tuneY += weight * std::sin(angle);
            peaks[frame].push_back({hz, m});
        }
    }
    out.tuning = std::atan2(tuneY, tuneX) / (2 * std::numbers::pi);
    std::array<double, 12> independent{};
    std::size_t tonalCount = 0;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        if ((frame & 31u) == 0) checkpoint(progress, 0.8 + 0.2 * double(frame) / frameCount, "key_profiles");
        Chroma chroma{};
        const auto& spectralPeaks = peaks[frame];
        double strongest = 0;
        for (const auto& p : spectralPeaks) strongest = std::max(strongest, p.magnitude);
        for (const auto& p : spectralPeaks) {
            const double note = 69 + 12 * std::log2(p.frequency / 440.0) - out.tuning;
            const double pc = note - 12 * std::floor(note / 12);
            double weight = std::sqrt(p.magnitude);
            bool overtone = false;
            for (const auto& fundamental : spectralPeaks) {
                if (fundamental.frequency >= p.frequency * 0.7) break;
                const double ratio = p.frequency / fundamental.frequency;
                if (fundamental.magnitude > strongest * 0.1 && ratio < 9 && std::abs(ratio - std::round(ratio)) < 0.025) {
                    overtone = true; break;
                }
            }
            if (overtone) weight *= 0.35;
            else if (p.magnitude >= strongest * 0.15) independent[std::size_t(int(std::lround(pc)) % 12)] += 1;
            // Three bins per semitone; cosine support preserves detuned peaks.
            for (int bin = 0; bin < 36; ++bin) {
                double d = std::abs(pc - double(bin) / 3);
                d = std::min(d, 12 - d);
                if (d < 0.5) chroma[bin] += weight * std::pow(std::cos(std::numbers::pi * d), 2);
            }
        }
        const double sum = std::accumulate(chroma.begin(), chroma.end(), 0.0);
        if (sum <= 1e-8) continue;
        for (std::size_t k = 0; k < 36; ++k) out.chroma[k] += chroma[k] / sum;
        ++tonalCount;
    }
    if (tonalCount < 4) return {};
    for (auto& x : out.chroma) x /= tonalCount;
    const double strongest = *std::max_element(independent.begin(), independent.end());
    for (double strength : independent) if (strength >= std::max(2.0, strongest * 0.12)) out.diversity += 1;
    out.weight = double(tonalCount) / frameCount;
    if (out.diversity < 3) out.weight = 0;
    return out;
}

double correlation(const std::array<double, 12>& a, const std::array<double, 12>& b) {
    const double ma = std::accumulate(a.begin(), a.end(), 0.0) / 12;
    const double mb = std::accumulate(b.begin(), b.end(), 0.0) / 12;
    double cross = 0, aa = 0, bb = 0;
    for (int i = 0; i < 12; ++i) { const double x = a[i] - ma, y = b[i] - mb; cross += x * y; aa += x * x; bb += y * y; }
    return cross / std::sqrt(std::max(kEpsilon, aa * bb));
}

std::vector<double> profileScores(const Chroma& hpcp) {
    // Published Krumhansl-Kessler and Temperley profiles. No hand-made EDM prior.
    static constexpr std::array<std::array<double, 12>, 4> profiles = {{
        {6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88},
        {6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17},
        {5,2,3.5,2,4.5,4,2,4.5,2,3.5,1.5,4},
        {5,2,3.5,4.5,2,4,2,4.5,3.5,2,1.5,4}
    }};
    std::array<double, 12> chroma{};
    for (int pc = 0; pc < 12; ++pc)
        chroma[pc] = hpcp[pc * 3] + 0.5 * (hpcp[(pc * 3 + 35) % 36] + hpcp[(pc * 3 + 1) % 36]);
    std::vector<double> result(24, 0);
    for (int family = 0; family < 2; ++family) {
        std::array<double, 24> scores{};
        double sum = 0;
        for (int mode = 0; mode < 2; ++mode) {
            for (int root = 0; root < 12; ++root) {
                std::array<double, 12> rotated{};
                for (int pc = 0; pc < 12; ++pc) rotated[(pc + root) % 12] = profiles[family * 2 + mode][pc];
                scores[mode * 12 + root] = std::exp(correlation(chroma, rotated) / 0.15);
                sum += scores[mode * 12 + root];
            }
        }
        for (int k = 0; k < 24; ++k) result[k] += 0.5 * scores[k] / sum;
    }
    return result;
}
int winner(const std::vector<double>& values) { return int(std::max_element(values.begin(), values.end()) - values.begin()); }
} // namespace

KeyEstimate detectKey(std::span<const float> mono, const MusicalAnalysisRequest& request,
                      const AnalysisProgress& progress) {
    KeyEstimate out;
    constexpr std::size_t maxWindow = 15 * 22050, hop = maxWindow / 2;
    if (mono.size() < 4096) { out.reason = "not enough tonal information"; return out; }
    const auto window = std::min(mono.size(), maxWindow);
    const auto segmentCount = 1 + (mono.size() - window + hop - 1) / hop;
    std::vector<double> global(24, 0), globalProfile(24, 0), globalNeural(24, 0);
    std::vector<std::pair<int, double>> votes;
    std::vector<std::vector<double>> tonalProfiles;
    double weight = 0, neuralWeight = 0, tuningX = 0, tuningY = 0;
    bool disagreement = false;
    const double modelWeight = keyModelWeight();
    for (std::size_t segment = 0; segment < segmentCount; ++segment) {
        const auto begin = std::min(segment * hop, mono.size() - window);
        const auto audio = mono.subspan(begin, window);
        const double from = double(segment) / segmentCount, to = double(segment + 1) / segmentCount;
        const auto features = tonalFeatures(audio, subProgress(progress, from, from + (to - from) * 0.75));
        if (features.weight < 0.15) { checkpoint(progress, to, "key_profiles"); continue; }
        const auto profiles = profileScores(features.chroma);
        tonalProfiles.push_back(profiles);
        std::vector<double> neural;
        if (request.useNeuralModels && audio.size() >= 3 * 22050) {
            std::vector<float> tuned(audio.begin(), audio.end());
            if (std::abs(features.tuning) > 0.1) {
                // Tuning correction is confined to the key branch. Beat timing
                // and the user's source samples remain in their original time.
                tuned = engine::dsp::resampleInterleaved(tuned, 1, tuned.size(), kAnalysisRate,
                    kAnalysisRate * std::pow(2.0, features.tuning / 12));
            }
            neural = neuralKey(tuned, subProgress(progress, from + (to - from) * 0.75, to));
        }
        std::vector<double> scores = profiles;
        if (!neural.empty()) {
            const int modelTop = winner(neural), profileTop = winner(profiles);
            if (modelTop != profileTop && neural[modelTop] > 0.3 && profiles[profileTop] > 0.3) disagreement = true;
            for (int k = 0; k < 24; ++k) {
                scores[k] = modelWeight * neural[k] + (1 - modelWeight) * profiles[k];
                globalNeural[k] += features.weight * neural[k];
            }
            neuralWeight += features.weight;
        }
        for (int k = 0; k < 24; ++k) {
            global[k] += features.weight * scores[k];
            globalProfile[k] += features.weight * profiles[k];
        }
        votes.emplace_back(winner(scores), features.weight);
        weight += features.weight;
        tuningX += features.weight * std::cos(features.tuning * 2 * std::numbers::pi);
        tuningY += features.weight * std::sin(features.tuning * 2 * std::numbers::pi);
        checkpoint(progress, to, "key_profiles");
    }
    if (weight == 0) { out.reason = "not enough tonal information"; return out; }
    out.backend = neuralWeight > 0 ? "skey+hpcp" : "hpcp";
    for (int k = 0; k < 24; ++k) {
        global[k] /= weight; globalProfile[k] /= weight;
        if (neuralWeight > 0) globalNeural[k] /= neuralWeight;
    }
    out.profileScores = globalProfile;
    if (neuralWeight > 0) out.neuralScores = globalNeural;
    const int top = winner(global);
    const double strongest = global[top];
    global[top] = -1;
    const int second = winner(global);
    double agreement = 0;
    for (const auto& [key, w] : votes) if (key == top) agreement += w / weight;
    std::array<double, 24> support{};
    std::array<int, 24> count{};
    for (const auto& [key, w] : votes) { support[key] += w / weight; ++count[key]; }
    for (int key = 0; key < 24; ++key)
        if (key != top && support[key] >= 0.25 && count[key] >= 2 && votes.size() >= 3)
            out.variable = true;
    if (mono.size() >= 24 * 22050 && tonalProfiles.size() >= 2) {
        const auto& first = tonalProfiles.front();
        const auto& last = tonalProfiles.back();
        double dot = 0, aa = 0, bb = 0;
        for (int key = 0; key < 24; ++key) { dot += first[key] * last[key]; aa += first[key] * first[key]; bb += last[key] * last[key]; }
        if (first[winner(first)] >= 0.25 && last[winner(last)] >= 0.25 &&
            dot / std::sqrt(std::max(1e-12, aa * bb)) < 0.35)
            out.variable = true;
    }
    const double tuning = std::atan2(tuningY, tuningX) / (2 * std::numbers::pi);
    const int shift = int(std::lround(request.pitchShiftSemitones + tuning));
    out.tuningCents = (request.pitchShiftSemitones + tuning - shift) * 100;
    out.root = (top % 12 + shift % 12 + 12) % 12;
    out.scale = top < 12 ? "major" : "natural_minor";
    out.alternateRoot = (second % 12 + shift % 12 + 12) % 12;
    out.alternateScale = second < 12 ? "major" : "natural_minor";
    const double margin = (strongest - global[second]) / std::max(1e-9, strongest);
    out.evidence = std::clamp(0.45 * strongest + 0.35 * margin + 0.2 * agreement, 0.0, 1.0);
    out.confidence = out.evidence;
    out.status = !out.variable && !disagreement && margin >= 0.2 && agreement >= 0.6 ? DetectionStatus::Available : DetectionStatus::Ambiguous;
    if (out.variable) out.reason = "key changes across the clip";
    else if (out.status == DetectionStatus::Ambiguous) out.reason = "key is ambiguous";
    calibrate(out);
    checkpoint(progress, 1.0, "key_done");
    return out;
}
} // namespace daw::analysis::detail
