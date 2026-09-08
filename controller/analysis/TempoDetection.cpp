#include "Signal.hpp"
#include "NeuralModels.hpp"
#include "Calibration.hpp"
#include <limits>
namespace daw::analysis::detail {
namespace {
constexpr double kMinTempo = 20.0;
constexpr double kMaxTempo = 300.0;
struct TempoFeatures {
    double rate = 0.0;
    std::array<std::vector<double>, 5> onset;
};

TempoFeatures tempoFeatures(std::span<const float> mono,
                            const AnalysisProgress& progress) {
    constexpr std::size_t size = 1024;
    constexpr std::size_t hop = 128;
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
        const std::size_t lowEnd = std::size_t(110.0 * size / kAnalysisRate);
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
        if ((frame & 127u) == 0)
            checkpoint(progress, double(frame) / frameCount, "tempo_features");
    }
    for (auto& feature : result.onset) {
        const double center = median(feature);
        auto ordered = feature;
        const auto at = ordered.begin() + std::ptrdiff_t((ordered.size() - 1) * 98 / 100);
        std::nth_element(ordered.begin(), at, ordered.end());
        const double scale = std::max(1e-8, *at - center);
        // MAD is almost zero on sustained notes: clipping by MAD turns tiny
        // spectral fluctuations into attacks as strong as actual drum hits.
        for (auto& value : feature) value = std::clamp((value - center) / scale, 0.0, 4.0);
    }
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

struct Peak { double at, strength; };
std::vector<Peak> peaks(const std::vector<double>& values, double threshold) {
    std::vector<Peak> out;
    for (std::size_t i = 1; i + 1 < values.size(); ++i) {
        if (values[i] <= threshold || values[i] < values[i - 1] || values[i] <= values[i + 1]) continue;
        const double at = double(i) + peakOffset(values[i - 1], values[i], values[i + 1]);
        out.push_back({at, values[i]});
    }
    return out;
}

struct Grid {
    double bpm = 0, score = 0, coverage = 0, residual = 1, phase = 0;
    int matched = 0;
};

Grid fitGrid(const std::vector<double>& onset, double rate, double bpm) {
    Grid best;
    if (onset.empty() || bpm < kMinTempo || bpm > kMaxTempo) return best;
    const double initialLag = 60 * rate / bpm;
    const double maximum = *std::max_element(onset.begin(), onset.end());
    const auto events = peaks(onset, std::max(0.08, maximum * 0.12));
    if (events.size() < 3) return best;
    const double total = std::accumulate(events.begin(), events.end(), 0.0,
        [](double s, const Peak& p) { return s + p.strength; });
    // Start on actual attacks, avoiding quantization of the grid phase.
    std::vector<double> phases;
    for (const auto& p : events) {
        if (p.at > initialLag * 4) break;
        const double phase = std::fmod(p.at, initialLag);
        if (std::none_of(phases.begin(), phases.end(), [&](double v) { return std::abs(v - phase) < 1.0; })) phases.push_back(phase);
    }
    for (double phase : phases) {
        double lag = initialLag;
        double coverage = 0, residual = 1, strength = 0;
        int matched = 0;
        for (int iteration = 0; iteration < 5; ++iteration) {
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, error = 0;
            matched = 0; strength = 0;
            int possible = 0;
            for (int n = int(std::ceil(-phase / lag)); phase + n * lag < double(onset.size()); ++n) {
                const double predicted = phase + n * lag;
                if (predicted < 1 || predicted > double(onset.size()) - 2) continue;
                ++possible;
                auto at = std::lower_bound(events.begin(), events.end(), predicted,
                    [](const Peak& p, double value) { return p.at < value; });
                const Peak* nearest = at != events.end() ? &*at : nullptr;
                if (at != events.begin() && (!nearest || std::abs((at - 1)->at - predicted) < std::abs(nearest->at - predicted))) nearest = &*(at - 1);
                const double tolerance = std::min(0.16 * lag, 0.065 * rate);
                if (!nearest || std::abs(nearest->at - predicted) > tolerance) continue;
                const double distance = std::abs(nearest->at - predicted);
                const double weight = std::sqrt(nearest->strength) * std::min(1.0, 0.025 * rate / std::max(1e-9, distance));
                sw += weight; sx += weight * n; sy += weight * nearest->at;
                sxx += weight * n * n; sxy += weight * n * nearest->at;
                strength += nearest->strength;
                error += distance / lag;
                ++matched;
            }
            if (matched < 3 || sw * sxx - sx * sx <= 1e-9) break;
            const double fittedLag = (sw * sxy - sx * sy) / (sw * sxx - sx * sx);
            if (std::abs(fittedLag / initialLag - 1.0) > 0.04) break;
            lag = fittedLag;
            phase = (sy - lag * sx) / sw;
            coverage = double(matched) / std::max(1, possible);
            residual = error / matched;
        }
        const double explained = std::clamp(strength / std::max(1e-9, total), 0.0, 1.0);
        // A grid through every quiet hi-hat should not win just by counting
        // more events. Retain the mean accent strength as well as coverage.
        const double accent = strength / std::max(1e-9, matched * maximum);
        const double score = std::sqrt(coverage * explained * accent) * std::exp(-8 * residual);
        if (matched >= 3 && score > best.score)
            best = {60.0 * rate / lag, score, coverage, residual, phase / rate, matched};
    }
    return best;
}

struct Candidate { double bpm, periodicity; Grid grid; double score = 0; };
std::vector<Candidate> candidates(const TempoFeatures& features, std::size_t begin, std::size_t end,
                                  const AnalysisProgress& progress) {
    const int minLag = int(std::floor(60.0 * features.rate / kMaxTempo));
    const int maxLag = std::min(int(std::ceil(60.0 * features.rate / kMinTempo)), int((end - begin) / 3));
    std::vector<double> scores(std::max(0, maxLag) + 2, 0.0);
    for (int lag = std::max(1, minLag - 1); lag <= maxLag + 1; ++lag) {
        for (const auto& onset : features.onset) scores[lag] += correlationAt(onset, lag, begin, end) / 5.0;
        if ((lag & 31) == 0) checkpoint(progress, double(lag) / (maxLag + 1), "tempo_grid");
    }
    std::vector<Candidate> out;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        if (scores[lag] < 0.08 || scores[lag] < scores[lag - 1] || scores[lag] <= scores[lag + 1]) continue;
        const double refined = lag + peakOffset(scores[lag - 1], scores[lag], scores[lag + 1]);
        const double bpm = 60.0 * features.rate / refined;
        if (bpm >= kMinTempo && bpm <= kMaxTempo) out.push_back({bpm, scores[lag], {}});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.periodicity > b.periodicity; });
    if (out.size() > 8) out.resize(8);
    return out;
}

std::vector<Peak> neuralPeaks(const std::vector<double>& values) {
    auto out = peaks(values, 0.5);
    std::erase_if(out, [&](const Peak& p) {
        const auto index = std::size_t(std::lround(p.at));
        const auto first = index > 3 ? index - 3 : 0;
        const auto last = std::min(values.size(), index + 4);
        return p.strength < *std::max_element(values.begin() + first, values.begin() + last);
    });
    return out;
}

double neuralTempo(const BeatActivations& a) {
    const auto p = neuralPeaks(a.beats);
    std::vector<double> intervals;
    for (std::size_t i = 1; i < p.size(); ++i) {
        const double seconds = (p[i].at - p[i - 1].at) / a.rate;
        if (seconds >= 60.0 / kMaxTempo && seconds <= 60.0 / kMinTempo) intervals.push_back(seconds);
    }
    if (intervals.size() < 3) return 0.0;
    return 60.0 / median(intervals);
}
} // namespace

TempoEstimate detectTempo(std::span<const float> mono, const MusicalAnalysisRequest& request,
                          const AnalysisProgress& progress) {
    TempoEstimate out;
    const double seconds = double(mono.size()) / kAnalysisRate;
    if (seconds < 1.0) { out.reason = "not enough rhythmic information"; return out; }
    auto features = tempoFeatures(mono, subProgress(progress, 0.0, 0.15));
    if (features.onset[0].empty()) { out.reason = "not enough rhythmic information"; return out; }
    BeatActivations neural;
    if (request.useNeuralModels) neural = neuralBeats(mono, subProgress(progress, 0.15, 0.65));
    checkpoint(progress, 0.65, "tempo_grid");
    const double modelBpm = neuralTempo(neural);
    out.backend = modelBpm > 0 ? "beat-this+grid" : "dsp";
    const auto total = features.onset[0].size();
    // Local analysis limits quadratic grid work and measures true time variation.
    const std::size_t window = std::min(total, std::size_t(16.0 * features.rate));
    const std::size_t hop = std::max<std::size_t>(1, window / 2);
    std::vector<std::vector<Candidate>> segments;
    const std::size_t segmentCount = 1 + (total - window + hop - 1) / hop;
    for (std::size_t begin = 0;; begin += hop) {
        if (begin + window > total) begin = total - window;
        const std::size_t end = begin + window;
        auto local = candidates(features, begin, end,
            subProgress(progress, 0.65 + 0.3 * segments.size() / segmentCount,
                        0.65 + 0.3 * (segments.size() + 1) / segmentCount));
        std::vector<double> fused(window, 0.0);
        constexpr std::array<double, 5> weights{0.3, 0.3, 0.0, 0.15, 0.25};
        for (std::size_t band = 0; band < weights.size(); ++band)
            for (std::size_t i = 0; i < window; ++i) fused[i] += features.onset[band][begin + i] * weights[band];
        BeatActivations localNeural;
        if (!neural.beats.empty()) {
            const auto first = std::min(neural.beats.size(), std::size_t(double(begin) / features.rate * neural.rate));
            const auto last = std::min(neural.beats.size(), std::size_t(double(end) / features.rate * neural.rate));
            localNeural.beats.assign(neural.beats.begin() + first, neural.beats.begin() + last);
            localNeural.downbeats.assign(neural.downbeats.begin() + first, neural.downbeats.begin() + last);
        }
        const double localModelBpm = neuralTempo(localNeural);
        const auto downbeats = neuralPeaks(localNeural.downbeats);
        std::vector<double> barIntervals;
        for (std::size_t i = 1; i < downbeats.size(); ++i)
            barIntervals.push_back((downbeats[i].at - downbeats[i - 1].at) / localNeural.rate);
        const double barSeconds = barIntervals.size() >= 2 ? median(barIntervals) : 0;
        for (double factor : {1.0, 0.5, 2.0}) {
            const double bpm = localModelBpm * factor;
            if (bpm < kMinTempo || bpm > kMaxTempo) continue;
            if (std::none_of(local.begin(), local.end(), [&](const auto& c) { return std::abs(std::log2(c.bpm / bpm)) < 0.008; }))
                local.push_back({bpm, 0, {}});
        }
        const Grid modelGrid = fitGrid(localNeural.beats, localNeural.rate, localModelBpm);
        const std::vector<double> lowAttacks(features.onset[4].begin() + begin, features.onset[4].begin() + end);
        double bestDsp = 0, modelDsp = 0;
        for (auto& c : local) {
            c.grid = fitGrid(fused, features.rate, c.bpm);
            if (c.grid.bpm > 0) c.bpm = c.grid.bpm;
            const Grid lowGrid = fitGrid(lowAttacks, features.rate, c.bpm);
            c.score = 0.15 * c.periodicity + 0.60 * c.grid.score + 0.25 * lowGrid.score;
            bestDsp = std::max(bestDsp, c.score);
            if (localModelBpm > 0 && std::abs(std::log2(c.bpm / localModelBpm)) < 0.04)
                modelDsp = std::max(modelDsp, c.score);
        }
        for (auto& c : local) {
            if (localModelBpm > 0) {
                const double distance = std::log2(c.bpm / localModelBpm);
                const double modelWeight = 0.35 * std::exp(-60 * modelGrid.residual - 12 * std::max(0.0, bestDsp - modelDsp));
                c.score = (1 - modelWeight) * c.score + modelWeight * std::exp(-0.5 * std::pow(distance / 0.035, 2));
                if (barSeconds > 0) {
                    const double beatsPerBar = barSeconds * c.bpm / 60;
                    const double meterError = std::min(std::abs(beatsPerBar - 3), std::abs(beatsPerBar - 4));
                    c.score *= 0.95 + 0.05 * std::exp(-8 * meterError * meterError);
                }
            }
        }
        std::sort(local.begin(), local.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        // Distinct tempos only; nearby autocorrelation/model candidates aren't runners-up.
        std::vector<Candidate> distinct;
        for (const auto& c : local) {
            if (c.bpm > 0 && std::none_of(distinct.begin(), distinct.end(), [&](const auto& d) { return std::abs(std::log2(c.bpm / d.bpm)) < 0.025; })) distinct.push_back(c);
        }
        if (!distinct.empty() && distinct.front().score >= 0.18 && distinct.front().grid.matched >= 3) {
            segments.push_back(std::move(distinct));
        }
        if (end == total) break;
    }
    checkpoint(progress, 0.96, "tempo_done");
    if (segments.empty()) { out.reason = "not enough rhythmic information"; return out; }
    // Pick the supported tempo cluster, then aggregate its precise local fits.
    double primary = 0, support = -1;
    for (const auto& segment : segments) {
        for (const auto& candidate : segment) {
            double score = 0;
            for (const auto& votes : segments) {
                for (const auto& vote : votes) {
                    if (std::abs(std::log2(vote.bpm / candidate.bpm)) < 0.025) { score += vote.score; break; }
                }
            }
            if (score > support) { support = score; primary = candidate.bpm; }
        }
    }
    std::vector<double> consistent;
    double scoreSum = 0, marginSum = 0;
    int stable = 0;
    for (const auto& segment : segments) {
        if (std::abs(std::log2(segment.front().bpm / primary)) < 0.025) ++stable;
        for (const auto& c : segment) {
            if (std::abs(std::log2(c.bpm / primary)) >= 0.025) continue;
            consistent.push_back(c.bpm);
            scoreSum += c.score;
            const double runner = segment.size() > 1 ? segment[1].score : 0.0;
            marginSum += std::clamp((c.score - runner) / std::max(0.01, c.score), 0.0, 1.0);
            break;
        }
    }
    out.stability = double(stable) / segments.size();
    // Octave disagreements describe meter ambiguity, not tempo drift.
    int metricalStable = 0;
    for (const auto& segment : segments) {
        double d = std::abs(std::log2(segment.front().bpm / primary));
        if (std::min({d, std::abs(d - 1.0), std::abs(d - 2.0)}) < 0.04) ++metricalStable;
    }
    out.variable = double(metricalStable) / segments.size() < 0.8;
    out.bpm = median(consistent) / request.stretchTime;
    const double meanScore = scoreSum / consistent.size();
    const double margin = marginSum / consistent.size();
    out.evidence = std::clamp(0.65 * meanScore + 0.2 * out.stability + 0.15 * margin, 0.0, 1.0);
    out.confidence = out.evidence;
    out.status = !out.variable && out.stability >= 0.75 && margin >= 0.06 ? DetectionStatus::Available : DetectionStatus::Ambiguous;
    if (out.variable) out.reason = "tempo changes across the clip";
    else if (out.status == DetectionStatus::Ambiguous) out.reason = "tempo is ambiguous";
    // Expose only alternatives supported by rhythmic evidence.
    for (const auto& segment : segments) {
        for (const auto& c : segment) {
            const double bpm = c.bpm / request.stretchTime;
            if (c.score < segment.front().score * 0.7 || std::abs(std::log2(bpm / out.bpm)) < 0.04) continue;
            if (std::none_of(out.alternatives.begin(), out.alternatives.end(), [&](double other) { return std::abs(std::log2(bpm / other)) < 0.04; })) out.alternatives.push_back(bpm);
            if (out.alternatives.size() == 3) break;
        }
        if (out.alternatives.size() == 3) break;
    }
    calibrate(out);
    checkpoint(progress, 1.0, "tempo_done");
    return out;
}
} // namespace daw::analysis::detail
