#include "AudioMusicalAnalysis.hpp"

#include "DSP/Resampler.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <numeric>
#include <regex>
#include <span>
#include <string>
#include <utility>

namespace daw::analysis {

namespace {

constexpr double kAnalysisRate = 22050.0;
constexpr double kMinTempo = 40.0;
constexpr double kMaxTempo = 250.0;
constexpr double kEpsilon = 1e-12;

bool report(const AnalysisProgress& progress, double amount,
            std::string_view phase) {
    return !progress || progress(std::clamp(amount, 0.0, 1.0), phase);
}

void fft(std::vector<std::complex<double>>& values) {
    const std::size_t n = values.size();
    if (n < 2) return;
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * std::numbers::pi / double(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t base = 0; base < n; base += length) {
            std::complex<double> phase(1.0, 0.0);
            const std::size_t half = length >> 1;
            for (std::size_t j = 0; j < half; ++j) {
                const auto even = values[base + j];
                const auto odd = values[base + j + half] * phase;
                values[base + j] = even + odd;
                values[base + j + half] = even - odd;
                phase *= step;
            }
        }
    }
}

std::vector<double> hann(std::size_t size) {
    std::vector<double> window(size);
    if (size < 2) return window;
    for (std::size_t i = 0; i < size; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) /
                                        double(size - 1));
    }
    return window;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(mid),
                     values.end());
    double result = values[mid];
    if ((values.size() & 1u) == 0) {
        const auto lower = std::max_element(values.begin(),
                                            values.begin() + std::ptrdiff_t(mid));
        result = (result + *lower) * 0.5;
    }
    return result;
}

void robustNormalize(std::vector<double>& values) {
    if (values.empty()) return;
    const double center = median(values);
    std::vector<double> deviations(values.size());
    std::transform(values.begin(), values.end(), deviations.begin(),
                   [center](double value) { return std::abs(value - center); });
    const double scale = std::max(1e-8, median(std::move(deviations)) * 1.4826);
    for (double& value : values)
        value = std::clamp((value - center) / scale, 0.0, 8.0);
}

std::vector<float> foldToAnalysisMono(const float* input, std::size_t frames,
                                      int channels, double sourceRate) {
    if (!input || frames == 0 || channels <= 0 || sourceRate <= 0.0) return {};
    std::vector<float> mono(frames, 0.0f);
    if (channels == 1) {
        std::copy(input, input + std::ptrdiff_t(frames), mono.begin());
    } else {
        // Mid is normally the musical centre. A hard out-of-phase stereo file
        // would cancel there, so pick the more energetic of mid and side for
        // each analysis block rather than silently analysing near-silence.
        constexpr std::size_t block = 4096;
        for (std::size_t first = 0; first < frames; first += block) {
            const std::size_t count = std::min(block, frames - first);
            double midEnergy = 0.0;
            double sideEnergy = 0.0;
            for (std::size_t i = 0; i < count; ++i) {
                const float left = input[(first + i) * std::size_t(channels)];
                const float right = input[(first + i) * std::size_t(channels) + 1];
                const double mid = 0.5 * (double(left) + right);
                const double side = 0.5 * (double(left) - right);
                midEnergy += mid * mid;
                sideEnergy += side * side;
            }
            const bool useSide = sideEnergy > midEnergy * 1.5;
            for (std::size_t i = 0; i < count; ++i) {
                const float left = input[(first + i) * std::size_t(channels)];
                const float right = input[(first + i) * std::size_t(channels) + 1];
                mono[first + i] = 0.5f * (useSide ? left - right : left + right);
            }
        }
    }

    // DC produces both a false low-frequency onset and a strong chroma leak.
    double mean = std::accumulate(mono.begin(), mono.end(), 0.0) /
                  double(std::max<std::size_t>(1, mono.size()));
    for (float& sample : mono) sample = float(double(sample) - mean);
    return daw::engine::dsp::resampleInterleaved(
        mono, 1, mono.size(), sourceRate, kAnalysisRate);
}

struct TempoFeatures {
    double rate = 0.0;
    std::array<std::vector<double>, 5> onset;
};

TempoFeatures tempoFeatures(const std::vector<float>& mono,
                            const AnalysisProgress& progress) {
    constexpr std::size_t size = 1024;
    constexpr std::size_t hop = 256;
    TempoFeatures result;
    result.rate = kAnalysisRate / double(hop);
    if (mono.size() < size) return result;
    const std::size_t frameCount = 1 + (mono.size() - size) / hop;
    for (auto& feature : result.onset) feature.assign(frameCount, 0.0);

    const auto window = hann(size);
    std::vector<double> previous(size / 2 + 1, 0.0);
    std::vector<double> previousPhase(size / 2 + 1, 0.0);
    std::vector<double> olderPhase(size / 2 + 1, 0.0);
    std::array<double, 40> previousBands{};
    double previousHfc = 0.0;
    double previousLow = 0.0;
    double previousRms = 0.0;
    std::vector<std::complex<double>> bins(size);

    std::array<std::size_t, 41> bandEdges{};
    const double minBin = std::max(1.0, 30.0 * size / kAnalysisRate);
    const double maxBin = std::min(double(size / 2), 16000.0 * size / kAnalysisRate);
    for (std::size_t b = 0; b < bandEdges.size(); ++b) {
        const double fraction = double(b) / double(bandEdges.size() - 1);
        bandEdges[b] = std::size_t(std::round(
            minBin * std::pow(maxBin / minBin, fraction)));
        if (b > 0) bandEdges[b] = std::max(bandEdges[b], bandEdges[b - 1] + 1);
        bandEdges[b] = std::min(bandEdges[b], size / 2);
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::size_t first = frame * hop;
        double rms = 0.0;
        for (std::size_t i = 0; i < size; ++i) {
            const double sample = mono[first + i];
            rms += sample * sample;
            bins[i] = {sample * window[i], 0.0};
        }
        fft(bins);
        rms = std::sqrt(rms / double(size));

        double flux = 0.0;
        double complexFlux = 0.0;
        double hfc = 0.0;
        double low = 0.0;
        std::array<double, 40> bands{};
        const std::size_t lowEnd = std::size_t(250.0 * size / kAnalysisRate);
        for (std::size_t k = 1; k <= size / 2; ++k) {
            const double magnitude = std::abs(bins[k]);
            const double logged = std::log1p(20.0 * magnitude);
            const double previousLogged = std::log1p(20.0 * previous[k]);
            flux += std::max(0.0, logged - previousLogged);
            hfc += double(k) * magnitude * magnitude;
            if (k <= lowEnd) low += magnitude * magnitude;

            if (frame >= 2) {
                const double expected = 2.0 * previousPhase[k] - olderPhase[k];
                const std::complex<double> predicted =
                    std::polar(previous[k], expected);
                complexFlux += std::abs(bins[k] - predicted);
            }
            olderPhase[k] = previousPhase[k];
            previousPhase[k] = std::arg(bins[k]);
            previous[k] = magnitude;
        }
        for (std::size_t b = 0; b < bands.size(); ++b) {
            double energy = 0.0;
            for (std::size_t k = bandEdges[b];
                 k < std::min(bandEdges[b + 1], size / 2 + 1); ++k) {
                energy += std::abs(bins[k]);
            }
            bands[b] = std::log1p(energy);
            result.onset[0][frame] +=
                std::max(0.0, bands[b] - previousBands[b]);
            previousBands[b] = bands[b];
        }
        result.onset[1][frame] = flux;
        result.onset[2][frame] = frame >= 2 ? complexFlux : 0.0;
        result.onset[3][frame] = std::max(0.0, std::log1p(hfc) -
                                                   std::log1p(previousHfc));
        result.onset[4][frame] =
            std::max(0.0, std::log1p(50.0 * low) -
                              std::log1p(50.0 * previousLow)) +
            0.5 * std::max(0.0, rms - previousRms);
        previousHfc = hfc;
        previousLow = low;
        previousRms = rms;
        if ((frame & 255u) == 0 &&
            !report(progress, 0.12 + 0.23 * double(frame) / frameCount,
                    "tempo_features")) {
            return {};
        }
    }
    for (auto& feature : result.onset) robustNormalize(feature);
    return result;
}

double correlationAt(const std::vector<double>& onset, int lag,
                     std::size_t begin, std::size_t end) {
    if (lag <= 0 || begin >= end || end - begin <= std::size_t(lag)) return 0.0;
    double cross = 0.0;
    double left = 0.0;
    double right = 0.0;
    for (std::size_t i = begin + std::size_t(lag); i < end; ++i) {
        const double a = onset[i];
        const double b = onset[i - std::size_t(lag)];
        cross += a * b;
        left += a * a;
        right += b * b;
    }
    return cross / std::sqrt(std::max(kEpsilon, left * right));
}

double pulseScore(const std::vector<double>& onset, double lag,
                  std::size_t begin, std::size_t end) {
    if (lag < 1.0 || end <= begin) return 0.0;
    const int phases = std::max(1, int(std::ceil(lag)));
    double best = 0.0;
    double total = std::accumulate(onset.begin() + std::ptrdiff_t(begin),
                                   onset.begin() + std::ptrdiff_t(end), 0.0);
    for (int phase = 0; phase < phases; ++phase) {
        double score = 0.0;
        int count = 0;
        for (double at = double(begin + std::size_t(phase)); at < double(end);
             at += lag) {
            const std::size_t index = std::size_t(std::round(at));
            if (index < end) {
                score += onset[index];
                if (index > begin) score += onset[index - 1] * 0.35;
                if (index + 1 < end) score += onset[index + 1] * 0.35;
                ++count;
            }
        }
        if (count >= 3) best = std::max(best, score / std::sqrt(double(count)));
    }
    return total > 1e-9 ? std::clamp(best / (0.35 * total), 0.0, 1.0) : 0.0;
}

struct TempoCandidate {
    double bpm = 0.0;
    double score = 0.0;
    double agreement = 0.0;
};

std::vector<TempoCandidate> tempoCandidates(const TempoFeatures& features,
                                            std::size_t begin,
                                            std::size_t end) {
    std::vector<TempoCandidate> candidates;
    if (features.rate <= 0.0 || end <= begin + 16) return candidates;
    const int minLag = std::max(1, int(std::floor(60.0 * features.rate / kMaxTempo)));
    const int maxLag = int(std::ceil(60.0 * features.rate / kMinTempo));
    std::vector<double> fused(end - begin, 0.0);
    for (const auto& feature : features.onset) {
        if (feature.size() < end) return {};
        for (std::size_t i = begin; i < end; ++i)
            fused[i - begin] += feature[i] / double(features.onset.size());
    }

    struct LagScore { int lag; double score; double agreement; };
    std::vector<LagScore> lagScores;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        std::array<double, 5> perFeature{};
        for (std::size_t f = 0; f < features.onset.size(); ++f) {
            const auto& onset = features.onset[f];
            double score = correlationAt(onset, lag, begin, end);
            if (lag * 2 <= maxLag)
                score += 0.35 * correlationAt(onset, lag * 2, begin, end);
            if (lag >= minLag * 2)
                score += 0.18 * correlationAt(onset, std::max(1, lag / 2), begin, end);
            perFeature[f] = score;
        }
        const double mean = std::accumulate(perFeature.begin(), perFeature.end(), 0.0) /
                            double(perFeature.size());
        int agreeing = 0;
        for (double score : perFeature)
            if (score >= mean * 0.65 && score > 0.08) ++agreeing;
        const double bpm = 60.0 * features.rate / double(lag);
        // A broad tactus prior resolves only otherwise-equal octave levels. It
        // still leaves 140 BPM trap ahead of 70, while not dragging a genuine
        // 60 BPM recording all the way to 120.
        const double tactus = std::exp(-0.5 * std::pow(std::log2(bpm / 110.0) / 1.15, 2.0));
        const double pulse = pulseScore(fused, double(lag), 0, fused.size());
        lagScores.push_back({lag, mean + 0.10 * pulse + 0.05 * tactus,
                             double(agreeing) / perFeature.size()});
    }
    std::sort(lagScores.begin(), lagScores.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });
    for (const LagScore& one : lagScores) {
        const double bpm = 60.0 * features.rate / one.lag;
        bool near = false;
        for (const auto& kept : candidates) {
            if (std::abs(std::log2(bpm / kept.bpm)) < 0.035) {
                near = true;
                break;
            }
        }
        if (!near) candidates.push_back({bpm, one.score, one.agreement});
        if (candidates.size() == 8) break;
    }
    return candidates;
}

double filenameBpmHint(const std::string& hint) {
    static const std::regex token(
        R"((?:^|[^0-9])([4-9][0-9]|1[0-9]{2}|2[0-4][0-9]|250)\s*(?:bpm)?(?:[^0-9]|$))",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_search(hint, match, token) || match.size() < 2) return 0.0;
    try { return std::stod(match[1].str()); }
    catch (...) { return 0.0; }
}

TempoEstimate detectTempo(const std::vector<float>& mono,
                          const MusicalAnalysisRequest& request,
                          const AnalysisProgress& progress) {
    TempoEstimate out;
    const double seconds = double(mono.size()) / kAnalysisRate;
    if (seconds < 1.5) {
        out.reason = "not enough rhythmic information";
        return out;
    }
    TempoFeatures features = tempoFeatures(mono, progress);
    if (features.onset[0].empty()) {
        out.reason = "analysis cancelled";
        return out;
    }
    std::vector<TempoCandidate> global =
        tempoCandidates(features, 0, features.onset[0].size());
    if (global.empty() || global.front().score < 0.10) {
        out.reason = "not enough rhythmic information";
        return out;
    }

    // A cleanly cut loop provides a powerful independent duration constraint.
    // It may boost an audio-supported candidate, never create one by itself.
    if (seconds <= 65.0) {
        for (auto& candidate : global) {
            const double beats = seconds * candidate.bpm / 60.0;
            const double nearest = std::round(beats);
            if (nearest >= 2.0) {
                const double error = std::abs(beats - nearest);
                candidate.score += 0.12 * std::exp(-error * error / 0.02);
            }
        }
    }
    const double hint = filenameBpmHint(request.fileNameHint);
    if (hint > 0.0) {
        for (auto& candidate : global) {
            const double ratio = std::abs(std::log2(candidate.bpm / hint));
            if (ratio < 0.025) candidate.score += 0.10;
        }
    }
    std::sort(global.begin(), global.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });

    // Autocorrelation naturally gives strong peaks at both the beat and the
    // bar/half-time level.  For contemporary loops a sub-100 BPM winner is
    // frequently the two-beat harmonic rhythm rather than the audible pulse.
    // Prefer its double only when that pulse is independently present in the
    // candidate set and remains close to the winning evidence.  This leaves a
    // genuinely slow recording alone when it has no reliable subdivisions.
    if (global.front().bpm < 100.0) {
        const double doubled = global.front().bpm * 2.0;
        const auto it = std::find_if(global.begin() + 1, global.end(),
            [doubled](const TempoCandidate& candidate) {
                return std::abs(std::log2(candidate.bpm / doubled)) < 0.035;
            });
        if (it != global.end() &&
            it->score >= global.front().score * 0.82 &&
            it->agreement >= global.front().agreement * 0.80) {
            std::iter_swap(global.begin(), it);
        }
    }

    const double localSeconds = 12.0;
    const std::size_t localFrames = std::size_t(localSeconds * features.rate);
    const std::size_t localHop = std::max<std::size_t>(1, localFrames / 2);
    std::vector<double> localBpms;
    if (features.onset[0].size() >= localFrames) {
        for (std::size_t begin = 0; begin + localFrames <= features.onset[0].size();
             begin += localHop) {
            auto local = tempoCandidates(features, begin, begin + localFrames);
            if (!local.empty()) localBpms.push_back(local.front().bpm);
        }
    }
    if (localBpms.empty()) localBpms.push_back(global.front().bpm);

    const double primary = global.front().bpm;
    int stable = 0;
    std::vector<double> folded;
    folded.reserve(localBpms.size());
    for (double bpm : localBpms) {
        double best = bpm;
        for (double factor : {0.5, 1.0, 2.0}) {
            const double candidate = bpm * factor;
            if (std::abs(std::log2(candidate / primary)) <
                std::abs(std::log2(best / primary))) best = candidate;
        }
        folded.push_back(best);
        if (std::abs(std::log2(best / primary)) < 0.025) ++stable;
    }
    const double stability = double(stable) / folded.size();
    const double centre = median(folded);
    std::vector<double> deviations;
    for (double bpm : folded)
        deviations.push_back(std::abs(bpm - centre) / std::max(1.0, centre));
    const double drift = median(deviations);

    const double runner = global.size() > 1 ? global[1].score : 0.0;
    const double margin = std::clamp((global.front().score - runner) /
                                         std::max(0.1, std::abs(global.front().score)),
                                     0.0, 1.0);
    const double periodic = std::clamp(global.front().score / 0.75, 0.0, 1.0);
    out.confidence = std::clamp(0.34 * periodic +
                                    0.24 * global.front().agreement +
                                    0.27 * stability + 0.15 * margin,
                                0.0, 1.0);
    out.stability = stability;
    out.variable = drift > 0.025 || stability < 0.65;
    out.bpm = std::round((primary / std::max(0.01, request.stretchTime)) * 10.0) /
              10.0;
    for (std::size_t i = 1; i < global.size() && out.alternatives.size() < 3; ++i) {
        if (global[i].score < global.front().score * 0.62) continue;
        const double bpm = global[i].bpm / std::max(0.01, request.stretchTime);
        bool duplicate = std::abs(std::log2(bpm / out.bpm)) < 0.035;
        for (double existing : out.alternatives)
            duplicate = duplicate || std::abs(std::log2(bpm / existing)) < 0.035;
        if (!duplicate) out.alternatives.push_back(std::round(bpm * 10.0) / 10.0);
    }
    // Always expose the metrical neighbour when it lies in the project range;
    // this is the correction users actually need for a 70/140 disagreement.
    for (double bpm : {out.bpm * 0.5, out.bpm * 2.0}) {
        if (bpm < 20.0 || bpm > 300.0) continue;
        bool duplicate = false;
        for (double existing : out.alternatives)
            duplicate = duplicate || std::abs(existing - bpm) < 0.2;
        if (!duplicate && out.alternatives.size() < 3)
            out.alternatives.push_back(std::round(bpm * 10.0) / 10.0);
    }
    if (out.variable) {
        out.status = DetectionStatus::Ambiguous;
        out.reason = "tempo changes across the clip";
    } else if (out.confidence >= 0.55) {
        out.status = DetectionStatus::Available;
    } else {
        out.status = DetectionStatus::Ambiguous;
        out.reason = "tempo is ambiguous";
    }
    report(progress, 0.56, "tempo_done");
    return out;
}

struct KeyScore {
    int root = 0;
    bool minor = false;
    double score = 0.0;
};

double pearson(const std::array<double, 12>& a,
               const std::array<double, 12>& b) {
    const double meanA = std::accumulate(a.begin(), a.end(), 0.0) / 12.0;
    const double meanB = std::accumulate(b.begin(), b.end(), 0.0) / 12.0;
    double cross = 0.0, aa = 0.0, bb = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double x = a[std::size_t(i)] - meanA;
        const double y = b[std::size_t(i)] - meanB;
        cross += x * y;
        aa += x * x;
        bb += y * y;
    }
    return cross / std::sqrt(std::max(kEpsilon, aa * bb));
}

std::vector<KeyScore> scoreKeys(const std::array<double, 12>& chroma) {
    static constexpr std::array<double, 12> krumMajor =
        {6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88};
    static constexpr std::array<double, 12> krumMinor =
        {6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17};
    static constexpr std::array<double, 12> temperleyMajor =
        {5.0,2.0,3.5,2.0,4.5,4.0,2.0,4.5,2.0,3.5,1.5,4.0};
    static constexpr std::array<double, 12> temperleyMinor =
        {5.0,2.0,3.5,4.5,2.0,4.0,2.0,4.5,3.5,2.0,1.5,4.0};
    static constexpr std::array<double, 12> edmMajor =
        {6.0,0.8,2.8,0.8,4.7,2.3,0.7,5.0,0.8,2.5,0.6,1.7};
    static constexpr std::array<double, 12> edmMinor =
        {6.0,0.8,2.5,4.8,0.8,2.4,0.7,5.0,3.2,0.9,2.1,1.4};
    const std::array profiles = {
        std::pair{krumMajor, krumMinor},
        std::pair{temperleyMajor, temperleyMinor},
        std::pair{edmMajor, edmMinor},
    };
    std::vector<KeyScore> scores;
    for (int root = 0; root < 12; ++root) {
        for (bool minor : {false, true}) {
            double score = 0.0;
            for (const auto& family : profiles) {
                std::array<double, 12> rotated{};
                const auto& profile = minor ? family.second : family.first;
                for (int pc = 0; pc < 12; ++pc)
                    rotated[std::size_t((pc + root) % 12)] = profile[std::size_t(pc)];
                score += pearson(chroma, rotated);
            }
            scores.push_back({root, minor, score / profiles.size()});
        }
    }
    std::sort(scores.begin(), scores.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });
    return scores;
}

KeyEstimate detectKey(const std::vector<float>& mono,
                      const MusicalAnalysisRequest& request,
                      const AnalysisProgress& progress) {
    constexpr std::size_t size = 4096;
    constexpr std::size_t hop = 2048;
    KeyEstimate out;
    if (mono.size() < size * 2) {
        out.reason = "not enough tonal information";
        return out;
    }
    const std::size_t frameCount = 1 + (mono.size() - size) / hop;
    const auto window = hann(size);
    std::vector<std::complex<double>> bins(size);
    std::vector<std::array<double, 12>> frames;
    frames.reserve(frameCount);

    // First pass: robust tuning displacement from significant spectral peaks.
    double tuneX = 0.0, tuneY = 0.0, tuneWeight = 0.0;
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::size_t first = frame * hop;
        for (std::size_t i = 0; i < size; ++i)
            bins[i] = {double(mono[first + i]) * window[i], 0.0};
        fft(bins);
        double maximum = 0.0;
        for (std::size_t k = 2; k < size / 2; ++k)
            maximum = std::max(maximum, std::abs(bins[k]));
        if (maximum < 1e-7) continue;
        const std::size_t minBin = std::max<std::size_t>(2, 45 * size / std::size_t(kAnalysisRate));
        const std::size_t maxBin = std::min<std::size_t>(size / 2 - 1,
                                                        5000 * size / std::size_t(kAnalysisRate));
        for (std::size_t k = minBin; k <= maxBin; ++k) {
            const double magnitude = std::abs(bins[k]);
            if (magnitude < maximum * 0.02 || magnitude < std::abs(bins[k - 1]) ||
                magnitude < std::abs(bins[k + 1])) continue;
            const double hz = double(k) * kAnalysisRate / size;
            const double note = 69.0 + 12.0 * std::log2(hz / 440.0);
            const double residual = note - std::round(note);
            const double angle = 2.0 * std::numbers::pi * residual;
            const double weight = std::sqrt(magnitude);
            tuneX += std::cos(angle) * weight;
            tuneY += std::sin(angle) * weight;
            tuneWeight += weight;
        }
    }
    const double tuningSemitones = tuneWeight > 0.0
        ? std::atan2(tuneY, tuneX) / (2.0 * std::numbers::pi) : 0.0;
    out.tuningCents = tuningSemitones * 100.0;

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::size_t first = frame * hop;
        double timeEnergy = 0.0;
        for (std::size_t i = 0; i < size; ++i) {
            const double sample = mono[first + i];
            timeEnergy += sample * sample;
            bins[i] = {sample * window[i], 0.0};
        }
        if (timeEnergy / size < 1e-9) continue;
        fft(bins);
        std::array<double, 12> chroma{};
        double spectralEnergy = 0.0;
        double logSum = 0.0;
        int spectralCount = 0;
        const std::size_t minBin = std::max<std::size_t>(2, 45 * size / std::size_t(kAnalysisRate));
        const std::size_t maxBin = std::min<std::size_t>(size / 2 - 1,
                                                        5000 * size / std::size_t(kAnalysisRate));
        double maximum = 0.0;
        for (std::size_t k = minBin; k <= maxBin; ++k)
            maximum = std::max(maximum, std::abs(bins[k]));
        for (std::size_t k = minBin; k <= maxBin; ++k) {
            const double magnitude = std::abs(bins[k]);
            spectralEnergy += magnitude;
            logSum += std::log(std::max(1e-12, magnitude));
            ++spectralCount;
            if (magnitude < maximum * 0.008 || magnitude < std::abs(bins[k - 1]) ||
                magnitude < std::abs(bins[k + 1])) continue;
            const double local = (std::abs(bins[k - 1]) + magnitude +
                                  std::abs(bins[k + 1])) / 3.0;
            const double whitened = magnitude / std::max(1e-9, local);
            const double hz = double(k) * kAnalysisRate / size;
            const double note = 69.0 + 12.0 * std::log2(hz / 440.0) -
                                tuningSemitones;
            const double pitchClass = note - 12.0 * std::floor(note / 12.0);
            const int lower = int(std::floor(pitchClass)) % 12;
            const double fraction = pitchClass - std::floor(pitchClass);
            const double weight = std::sqrt(magnitude) * std::min(2.0, whitened);
            chroma[std::size_t(lower)] += weight * (1.0 - fraction);
            chroma[std::size_t((lower + 1) % 12)] += weight * fraction;
        }
        const double arithmetic = spectralEnergy / std::max(1, spectralCount);
        const double geometric = std::exp(logSum / std::max(1, spectralCount));
        const double flatness = arithmetic > 0.0 ? geometric / arithmetic : 1.0;
        const double sum = std::accumulate(chroma.begin(), chroma.end(), 0.0);
        if (sum < 1e-8 || flatness > 0.72) continue;
        for (double& value : chroma) value /= sum;
        frames.push_back(chroma);
        if ((frame & 63u) == 0 &&
            !report(progress, 0.58 + 0.35 * double(frame) / frameCount,
                    "key_profiles")) {
            out.reason = "analysis cancelled";
            return out;
        }
    }
    if (frames.size() < 8) {
        out.reason = "not enough tonal information";
        return out;
    }

    std::array<double, 12> global{};
    for (const auto& frame : frames)
        for (int pc = 0; pc < 12; ++pc) global[std::size_t(pc)] += frame[std::size_t(pc)];
    for (double& value : global) value /= frames.size();
    const std::vector<KeyScore> scores = scoreKeys(global);
    if (scores.size() < 2) {
        out.reason = "key is ambiguous";
        return out;
    }

    const std::size_t segmentFrames = std::max<std::size_t>(8,
        std::size_t(15.0 * kAnalysisRate / hop));
    int agreeing = 0;
    int segmentCount = 0;
    for (std::size_t begin = 0; begin < frames.size(); begin += segmentFrames / 2) {
        const std::size_t end = std::min(frames.size(), begin + segmentFrames);
        if (end - begin < 8) break;
        std::array<double, 12> local{};
        for (std::size_t i = begin; i < end; ++i)
            for (int pc = 0; pc < 12; ++pc)
                local[std::size_t(pc)] += frames[i][std::size_t(pc)];
        const auto localScores = scoreKeys(local);
        if (!localScores.empty() && localScores.front().root == scores.front().root &&
            localScores.front().minor == scores.front().minor) ++agreeing;
        ++segmentCount;
        if (end == frames.size()) break;
    }
    const double agreement = segmentCount > 0 ? double(agreeing) / segmentCount : 0.0;
    const double top = (scores[0].score + 1.0) * 0.5;
    const double margin = std::clamp((scores[0].score - scores[1].score) / 0.22,
                                     0.0, 1.0);
    out.confidence = std::clamp(0.38 * top + 0.34 * margin +
                                    0.28 * agreement,
                                0.0, 1.0);
    const int shift = int(std::lround(request.pitchShiftSemitones));
    out.root = (scores[0].root + shift % 12 + 12) % 12;
    out.scale = scores[0].minor ? "natural_minor" : "major";
    out.alternateRoot = (scores[1].root + shift % 12 + 12) % 12;
    out.alternateScale = scores[1].minor ? "natural_minor" : "major";
    if (out.confidence >= 0.55) {
        out.status = DetectionStatus::Available;
    } else {
        out.status = DetectionStatus::Ambiguous;
        out.reason = "key is ambiguous";
    }
    report(progress, 0.96, "key_done");
    return out;
}

} // namespace

audio::Result analyzeAudioSamples(const float* interleaved,
                                  std::size_t frames, int channels,
                                  double sampleRate,
                                  const MusicalAnalysisRequest& request,
                                  MusicalAnalysisResult& out,
                                  const AnalysisProgress& progress) {
    out = {};
    if (!interleaved || frames == 0 || channels <= 0 || sampleRate <= 0.0) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "audio analysis received no samples");
    }
    if (!request.detectTempo && !request.detectKey) return audio::Result::ok();
    if (!report(progress, 0.02, "preparing"))
        return audio::Result::fail(audio::EngineError::Unknown,
                                   "analysis cancelled");
    std::vector<float> mono =
        foldToAnalysisMono(interleaved, frames, channels, sampleRate);
    if (mono.empty())
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   "audio analysis could not prepare samples");
    out.analyzedSeconds = double(mono.size()) / kAnalysisRate;
    if (request.detectTempo) {
        out.tempo = detectTempo(mono, request, progress);
        if (out.tempo.reason == "analysis cancelled")
            return audio::Result::fail(audio::EngineError::Unknown,
                                       "analysis cancelled");
    }
    if (request.detectKey) {
        out.key = detectKey(mono, request, progress);
        if (out.key.reason == "analysis cancelled")
            return audio::Result::fail(audio::EngineError::Unknown,
                                       "analysis cancelled");
    }
    report(progress, 1.0, "complete");
    return audio::Result::ok();
}

audio::Result analyzeAudioFile(const std::string& path,
                               const MusicalAnalysisRequest& request,
                               MusicalAnalysisResult& out,
                               const AnalysisProgress& progress) {
    audio::platform::AudioFileReader reader;
    audio::Result opened = reader.open(path);
    if (!opened) return opened;
    const auto& info = reader.info();
    const audio::FrameCount first = audio::FrameCount(std::clamp(
        request.offsetSeconds * info.sampleRate, 0.0, double(info.frames)));
    audio::FrameCount wanted = info.frames - first;
    if (request.durationSeconds > 0.0) {
        wanted = std::min(wanted, audio::FrameCount(std::max(
            0.0, request.durationSeconds * info.sampleRate)));
    }
    if (wanted == 0)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "the selected clip range is empty");
    audio::Result seeked = reader.seek(first);
    if (!seeked) return seeked;
    if (wanted > audio::FrameCount(std::numeric_limits<std::size_t>::max() /
                                   std::max<audio::ChannelCount>(1, info.channels))) {
        return audio::Result::fail(audio::EngineError::OutOfMemory,
                                   "audio analysis range is too large");
    }
    std::vector<float> decoded(
        std::size_t(wanted) * std::size_t(info.channels));
    constexpr audio::FrameCount chunk = 65536;
    audio::FrameCount received = 0;
    while (received < wanted) {
        const audio::FrameCount count = std::min(chunk, wanted - received);
        const audio::FrameCount read = reader.read(
            decoded.data() + std::size_t(received) * info.channels, count);
        if (read == 0) break;
        received += read;
        if (!report(progress, 0.02 + 0.08 * double(received) / double(wanted),
                    "decoding")) {
            return audio::Result::fail(audio::EngineError::Unknown,
                                       "analysis cancelled");
        }
    }
    decoded.resize(std::size_t(received) * info.channels);
    if (received == 0)
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   "decoded zero frames for analysis");
    return analyzeAudioSamples(decoded.data(), std::size_t(received),
                               int(info.channels), info.sampleRate,
                               request, out, progress);
}

std::string pitchClassName(int root) {
    static constexpr std::array<std::string_view, 12> names =
        {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    return std::string(names[std::size_t((root % 12 + 12) % 12)]);
}

std::string camelotName(int root, const std::string& scale) {
    static constexpr std::array<int, 12> major = {8,3,10,5,12,7,2,9,4,11,6,1};
    static constexpr std::array<int, 12> minor = {5,12,7,2,9,4,11,6,1,8,3,10};
    const int pc = (root % 12 + 12) % 12;
    const bool isMinor = scale == "natural_minor" || scale == "minor";
    return std::to_string((isMinor ? minor : major)[std::size_t(pc)]) +
           (isMinor ? "A" : "B");
}

std::string keyDisplayName(const KeyEstimate& key) {
    if (key.root < 0) return {};
    const bool minor = key.scale == "natural_minor" || key.scale == "minor";
    return pitchClassName(key.root) + (minor ? " minor" : " major");
}

ClipMusicalAnalysisModel toClipAnalysisModel(
    const MusicalAnalysisResult& result,
    const MusicalAnalysisRequest& request) {
    ClipMusicalAnalysisModel model;
    model.algorithmVersion = result.algorithmVersion;
    model.analyzedOffsetSeconds = std::max(0.0, request.offsetSeconds);
    model.analyzedDurationSeconds = result.analyzedSeconds;
    model.tempo.status = MusicalAnalysisStatus(int(result.tempo.status));
    model.tempo.bpm = result.tempo.bpm;
    model.tempo.confidence = result.tempo.confidence;
    model.tempo.stability = result.tempo.stability;
    model.tempo.alternatives = result.tempo.alternatives;
    model.tempo.variable = result.tempo.variable;
    model.key.status = MusicalAnalysisStatus(int(result.key.status));
    model.key.root = result.key.root;
    model.key.scale = result.key.scale;
    model.key.confidence = result.key.confidence;
    model.key.alternateRoot = result.key.alternateRoot;
    model.key.alternateScale = result.key.alternateScale;
    model.key.tuningCents = result.key.tuningCents;
    return model;
}

} // namespace daw::analysis
