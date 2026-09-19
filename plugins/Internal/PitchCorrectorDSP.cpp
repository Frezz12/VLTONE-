#include "Internal/PitchCorrectorDSP.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace daw::plugins::pitch {
namespace {
constexpr double pi = std::numbers::pi;
constexpr int detectorRate = 12000, hop = 24, maxOrder = 64;
constexpr int candidateCount = 6, lookaheadFrames = 11;
double safe(double x, double fallback = 0) noexcept {
    return std::isfinite(x) ? x : fallback;
}
double pole(double seconds, double rate) noexcept {
    return seconds > 0 ? std::exp(-1 / (seconds * rate)) : 0;
}
std::size_t powerOfTwo(std::size_t n) {
    std::size_t result = 1;
    while (result < n) result *= 2;
    return result;
}
struct Lowpass {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void prepare(double rate, double frequency, double q) noexcept {
        const double w = 2 * pi * frequency / rate, c = std::cos(w);
        const double alpha = std::sin(w) / (2 * q), inv = 1 / (1 + alpha);
        b0 = .5 * (1-c) * inv; b1 = (1-c)*inv; b2 = b0;
        a1 = -2*c*inv; a2 = (1-alpha)*inv;
        z1 = z2 = 0;
    }
    double process(double x) noexcept {
        const double y = b0*x + z1;
        z1 = b1*x - a1*y + z2;
        z2 = b2*x - a2*y;
        return y;
    }
};
struct Estimate {
    double time = -1e20, hz = 0, confidence = 0;
};
struct CandidateFrame {
    double time = -1e20;
    std::array<double, candidateCount> hz{}, confidence{};
    int count = 1;
};

std::uint16_t notes(const Settings& s) noexcept {
    // Chromatic, major, natural/harmonic/melodic minor, the two pentatonics,
    // custom. The custom mask is already absolute (C is bit zero).
    constexpr std::array<std::uint16_t, 7> masks{
        0xfff, 0xab5, 0x5ad, 0x9ad, 0xaad, 0x295, 0x4a9};
    if (s.scale == 7) return s.noteMask & 0xfff;
    const auto mask = masks[std::clamp(s.scale, 0, 6)];
    const int root = (s.key % 12 + 12) % 12;
    return std::uint16_t(((mask << root) | (mask >> (12-root))) & 0xfff);
}
int closestNote(double pitch, std::uint16_t mask, int previous, double hysteresis) noexcept {
    int best = int(std::floor(pitch));
    double distance = 100;
    for (int n = int(std::floor(pitch))-12; n <= int(std::ceil(pitch))+12; ++n) {
        if (!(mask & (1u << ((n%12+12)%12)))) continue;
        double d = std::abs(pitch-n);
        if (n == previous) d -= hysteresis;
        if (d < distance) { distance = d; best = n; }
    }
    return best;
}
} // namespace

double retuneMilliseconds(double tune) noexcept {
    const double remaining = 1-std::clamp(safe(tune, kDefaultTune), 0.0, 100.0)/100;
    return 200*remaining*remaining;
}
double tuneFromMilliseconds(double milliseconds) noexcept {
    return 100*(1-std::sqrt(std::clamp(safe(milliseconds, kDefaultRetuneMs), 0.0, 200.0)/200));
}

struct PitchCorrectorDSP::Impl {
    double rate = 48000;
    std::uint32_t channels = 2, latency = 480, tail = 480;
    int quality = 0, detectorSize = 512, order = 32, lpcLength = 1440;
    int lpcInterval = 96, lpcCountdown = 0, lpcStage = 0;
    bool prepared = false;
    std::uint64_t position = 0, detectorSamples = 0;
    std::size_t ringMask = 0, coefficientMask = 0;
    std::vector<std::array<float, 2>> original, residual;
    std::vector<float> protection, lpcWindow;
    std::vector<std::uint8_t> anchors;
    std::vector<float> coefficients, latticeHistory;
    std::array<std::vector<double>, 2> burgForward, burgBackward;
    std::array<std::array<double, maxOrder>, 2> reflection{}, wantedReflection{};
    std::array<std::array<double, maxOrder>, 2> pendingReflection{};
    std::array<double, 2> lpcEnergy{};
    std::array<std::array<double, maxOrder+1>, 2> analysisState{}, synthesisState{};
    std::array<std::array<double, maxOrder+1>, 2> alternateSynthesisState{};
    std::array<double, 1024> detector{}, ordered{};
    std::array<double, 202> difference{};
    std::array<Lowpass, 2> analysisLowpass;
    double analysisPhase = 0, previousFiltered = 0;
    std::array<CandidateFrame, lookaheadFrames> candidateFrames{};
    int candidateFill = 0;
    std::array<Estimate, 512> estimates{};
    std::uint64_t estimateCount = 0;
    Estimate current;
    CandidateFrame pendingDetection;
    int detectionMinimum = 0, detectionMaximum = 0, detectionWindow = 0;
    int detectionLag = 0, detectionTicks = 0, detectionScan = 0;
    int detectionBest = 0, detectionPrimary = -1;
    double detectionRunning = 0;
    double correction = 0, musicalCorrection = 0, baseline = 69;
    double heldSeconds = 0, gain = 1, inputPower = 0, outputPower = 0;
    double read = 0, alternateRead = 0, fade = 1, fadeStep = 1;
    double spliceLow = 0, spliceHigh = 0;
    double period = 48000.0/220, wet = 0, formantMix = 1;
    double lastPitch = 69;
    int target = -1000, voicedFrames = 0;
    int pendingTarget = -1000;
    double pendingTargetSeconds = 0, pendingConfidentSeconds = 0, pendingSnapSeconds = 0, unvoicedSeconds = 0;
    bool pitchInitialised = false;
    bool correctionEngaged = false;
    int voicedHold = 0;
    int detectorChannel = 0, transientHold = 0;
    int quietSamples = 0, anchorHold = 0;
    bool anchorActive = false;
    std::array<double, 2> channelPower{};
    double fastPower = 0, slowPower = 0, highpassLow = 0, highPower = 0, mismatchPower = 0;
    Telemetry meter;

    std::array<float, 2> at(std::int64_t index) const noexcept {
        if (index < 0 || std::uint64_t(index) > position) return {};
        return original[std::size_t(index) & ringMask];
    }
    double interpolate(const std::vector<std::array<float, 2>>& data,
                       double index, int channel) const noexcept {
        // Four-point interpolation is confined to available history. Pitch
        // correction normally moves by less than a semitone; a period splice
        // never jumps either live head without a complementary crossfade.
        index = std::min(index, double(position)-2);
        const auto n = std::int64_t(std::floor(index));
        const double t = index-double(n);
        const auto sample = [&](std::int64_t i) {
            return i < 0 ? 0.0 : double(data[std::size_t(i)&ringMask][channel]);
        };
        const double a = sample(n-1), b = sample(n), c = sample(n+1), d = sample(n+2);
        return b+t*(.5*(c-a)+t*(a-2.5*b+2*c-.5*d+t*(.5*(d-a)+1.5*(b-c))));
    }
    double matchSplice(double source, double proposed, double guard) const noexcept {
        // F0 is necessarily measured from earlier samples. During a glide,
        // its nominal period need not join the current waveform in phase.
        // Refine the splice on the actual local waveform (linked stereo).
        constexpr int count = 48, search = 10;
        const double spacing = std::clamp(.75*period, 32.0, 256.0)/(count-1);
        const double step = .12*period/search;
        std::array<double, count> reference{};
        double sumReference = 0, squareReference = 0;
        for (int i = 0; i < count; ++i) {
            const double value = interpolate(original, source-i*spacing, detectorChannel);
            reference[i] = value; sumReference += value; squareReference += value*value;
        }
        squareReference -= sumReference*sumReference/count;
        if (squareReference < 1e-8) return proposed;
        const auto similarity = [&](double candidate) {
            if (candidate > double(position)-guard || candidate < count*spacing) return -1.0;
            double sum = 0, square = 0, product = 0;
            for (int i = 0; i < count; ++i) {
                const double value = interpolate(original, candidate-i*spacing, detectorChannel);
                sum += value; square += value*value; product += value*reference[i];
            }
            square -= sum*sum/count; product -= sum*sumReference/count;
            return product/std::sqrt(std::max(1e-20, square*squareReference));
        };
        int best = 0;
        double score = similarity(proposed);
        for (int i = -search; i <= search; ++i) {
            const double value = similarity(proposed+i*step);
            if (value > score) { score = value; best = i; }
        }
        if (score < .5) return proposed;
        double selected = proposed+best*step;
        if (best > -search && best < search) {
            const double a = similarity(selected-step), b = score, c = similarity(selected+step);
            const double denominator = a-2*b+c;
            if (denominator < -1e-8) selected += step*std::clamp(.5*(a-c)/denominator, -.5, .5);
        }
        return selected;
    }
    void pushEstimate(Estimate estimate) noexcept {
        estimates[estimateCount++ % estimates.size()] = estimate;
        current = estimate;
    }
    Estimate estimateAt(double time) const noexcept {
        const auto count = std::min<std::uint64_t>(estimateCount, estimates.size());
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto& e = estimates[(estimateCount-1-i) % estimates.size()];
            if (e.time <= time) return e;
        }
        return {};
    }
    void selectPath(const CandidateFrame& frame) noexcept {
        if (candidateFill == lookaheadFrames) {
            for (int i = 1; i < lookaheadFrames; ++i) candidateFrames[i-1] = candidateFrames[i];
            --candidateFill;
        }
        candidateFrames[candidateFill++] = frame;
        if (candidateFill < lookaheadFrames) return;
        std::array<std::array<double, candidateCount>, lookaheadFrames> cost{};
        std::array<std::array<int, candidateCount>, lookaheadFrames> back{};
        for (int i = 0; i < lookaheadFrames; ++i) {
            const auto& f = candidateFrames[i];
            for (int c = 0; c < f.count; ++c) {
                double best = 0;
                if (i) {
                    best = std::numeric_limits<double>::max();
                    const auto& previous = candidateFrames[i-1];
                    for (int p = 0; p < previous.count; ++p) {
                        const double a = f.hz[c], b = previous.hz[p];
                        const double transition = a > 0 && b > 0
                            ? .15 * std::min(4.0, std::abs(1200*std::log2(a/b))/150)
                            : (a == b ? 0 : .35);
                        const double value = cost[i-1][p] + transition;
                        if (value < best) { best = value; back[i][c] = p; }
                    }
                }
                cost[i][c] = best - std::log(std::max(.001, f.confidence[c]));
            }
        }
        int selected = 0;
        const int last = lookaheadFrames-1;
        for (int c = 1; c < candidateFrames[last].count; ++c)
            if (cost[last][c] < cost[last][selected]) selected = c;
        for (int i = last; i > 0; --i) selected = back[i][selected];
        const auto& first = candidateFrames[0];
        pushEstimate({first.time, first.hz[selected], first.hz[selected] > 0 ? first.confidence[selected] : 0});
    }
    double refinePeriod(int lag, int window) const noexcept {
        // CMND's sloping normalization biases a parabolic fit, particularly
        // near the top of the vocal range (only 10--12 samples per period).
        // Refine the candidate on the actual fractional-delay waveform error.
        // This is a bounded local search, not a second pitch search.
        const auto error = [&](double shift) {
            const int integer = int(std::floor(shift));
            const double t = shift-integer;
            double result = 0;
            for (int i = 1; i < window-4; ++i) {
                const int j = i+integer;
                const double a = ordered[j-1], b = ordered[j], c = ordered[j+1], d = ordered[j+2];
                const double interpolated = b+t*(.5*(c-a)+t*(a-2.5*b+2*c-.5*d+t*(.5*(d-a)+1.5*(b-c))));
                const double delta = ordered[i]-interpolated;
                result += delta*delta;
            }
            return result;
        };
        constexpr double golden = .6180339887498948482;
        double low = lag-.75, high = lag+.75;
        double left = high-golden*(high-low), right = low+golden*(high-low);
        double a = error(left), b = error(right);
        for (int i = 0; i < 16; ++i) {
            if (a < b) { high = right; right = left; b = a; left = high-golden*(high-low); a = error(left); }
            else { low = left; left = right; a = b; right = low+golden*(high-low); b = error(right); }
        }
        return .5*(low+high);
    }
    void beginHdDetection(const Settings& settings) noexcept {
        double lo = 60, hi = 1200;
        if (settings.voice == 1) hi = 350;
        if (settings.voice == 2) { lo = 100; hi = 800; }
        if (settings.voice == 3) lo = 180;
        detectionMinimum = std::max(2, int(detectorRate/hi));
        detectionMaximum = std::min(200, int(std::ceil(detectorRate/lo)));
        detectionWindow = detectorSize-detectionMaximum;
        double energy = 0;
        // Freeze the same frame and source timestamp as the immediate detector.
        // The incoming ring may advance while this immutable snapshot is used.
        for (int i = 0; i < detectorSize; ++i) {
            ordered[i] = detector[(detectorSamples-detectorSize+i)%detector.size()];
            energy += ordered[i]*ordered[i];
        }
        pendingDetection = {};
        pendingDetection.time = double(position)-.5*detectorSize*rate/detectorRate;
        pendingDetection.confidence[0] = 1;
        if (energy/detectorSize < 1e-6) {
            voicedFrames = 0; detectionLag = 0;
            selectPath(pendingDetection);
            return;
        }
        difference[0] = 1;
        detectionLag = 1; detectionTicks = 0; detectionScan = 0;
        detectionRunning = 0; detectionPrimary = -1;
    }
    void advanceHdDetection() noexcept {
        if (!detectionLag) return;
        if (detectionLag <= detectionMaximum+1) {
            // HD has over 17 ms between finalized analysis and playback. Use
            // one millisecond of that slack to spread the exact original YIN
            // sums across callbacks; neither arithmetic order nor F0 changes.
            const int duration = std::max(1, int(std::ceil(.001*rate)));
            const int last = std::min(detectionMaximum+1,
                ++detectionTicks*(detectionMaximum+1)/duration);
            for (; detectionLag <= last; ++detectionLag) {
                double sum = 0;
                for (int i = 0; i < detectionWindow-1; ++i) {
                    const double d = ordered[i]-ordered[i+detectionLag];
                    sum += d*d;
                }
                detectionRunning += sum;
                difference[detectionLag] = detectionRunning > 1e-20
                    ? sum*detectionLag/detectionRunning : 1;
            }
            if (detectionLag <= detectionMaximum+1) return;
            detectionBest = detectionMinimum;
            for (int i = detectionMinimum+1; i <= detectionMaximum; ++i)
                if (difference[i] < difference[detectionBest]) detectionBest = i;
            detectionScan = detectionMinimum;
            return;
        }
        // Each candidate's bounded fractional refinement runs on its own
        // sample. Even at 12 kHz all five candidates and path selection finish
        // before the next two-millisecond detector hop.
        while (detectionScan <= detectionMaximum) {
            const int i = detectionScan++;
            if (difference[i] > .25 || difference[i] > difference[i-1] ||
                difference[i] > difference[i+1]) continue;
            if (detectionPrimary < 0 && difference[i] <= .12) detectionPrimary = i;
            if (pendingDetection.count < candidateCount) {
                pendingDetection.hz[pendingDetection.count] = detectorRate/refinePeriod(i, detectionWindow);
                pendingDetection.confidence[pendingDetection.count++] = std::clamp(1-difference[i], 0.0, 1.0);
                return;
            }
        }
        if (detectionPrimary < 0 && difference[detectionBest] < .22) detectionPrimary = detectionBest;
        const double confidence = detectionPrimary >= 0
            ? std::clamp(1-difference[detectionPrimary], 0.0, 1.0) : 0;
        if (detectionPrimary >= 0) {
            const double primaryHz = double(detectorRate)/detectionPrimary;
            for (int c = 1; c < pendingDetection.count; ++c) {
                const double octavesBelow = std::max(0.0, std::log2(primaryHz/pendingDetection.hz[c]));
                pendingDetection.confidence[c] *= std::exp(-.08*octavesBelow);
            }
        }
        pendingDetection.confidence[0] = std::clamp(1-confidence, .01, 1.0);
        selectPath(pendingDetection);
        detectionLag = 0;
    }
    void detect(const Settings& settings) noexcept {
        if (quality) { beginHdDetection(settings); return; }
        double lo = 60, hi = 1200;
        if (settings.voice == 1) hi = 350;
        if (settings.voice == 2) { lo = 100; hi = 800; }
        if (settings.voice == 3) lo = 180;
        const int minimum = std::max(2, int(detectorRate/hi));
        const int maximum = std::min(200, int(std::ceil(detectorRate/lo)));
        const int window = detectorSize-maximum;
        double energy = 0;
        for (int i = 0; i < detectorSize; ++i) {
            ordered[i] = detector[(detectorSamples-detectorSize+i)%detector.size()];
            energy += ordered[i]*ordered[i];
        }
        CandidateFrame frame;
        frame.time = double(position) - .5*detectorSize*rate/detectorRate;
        frame.confidence[0] = 1;
        if (energy/detectorSize < 1e-6) {
            voicedFrames = 0;
            if (quality) selectPath(frame); else pushEstimate({frame.time, 0, 0});
            return;
        }
        difference[0] = 1;
        double running = 0;
        for (int lag = 1; lag <= maximum+1; ++lag) {
            double sum = 0;
            for (int i = 0; i < window-1; ++i) {
                const double d = ordered[i] - ordered[i+lag];
                sum += d*d;
            }
            running += sum;
            difference[lag] = running > 1e-20 ? sum*lag/running : 1;
        }
        int best = minimum;
        for (int i = minimum+1; i <= maximum; ++i)
            if (difference[i] < difference[best]) best = i;
        int primary = -1;
        for (int i = minimum; i <= maximum; ++i) {
            if (difference[i] > .25 || difference[i] > difference[i-1] ||
                difference[i] > difference[i+1]) continue;
            if (primary < 0 && difference[i] <= .12) primary = i;
            if (frame.count < candidateCount) {
                frame.hz[frame.count] = detectorRate/refinePeriod(i, window);
                frame.confidence[frame.count++] = std::clamp(1-difference[i], 0.0, 1.0);
            }
        }
        if (primary < 0 && difference[best] < .22) primary = best;
        const double confidence = primary >= 0 ? std::clamp(1-difference[primary], 0.0, 1.0) : 0;
        // Integer multiples of a period often have a slightly lower YIN error
        // than its fundamental (a perfect sine is the simplest example).
        // Preserve YIN's first-threshold preference in the lookahead lattice;
        // otherwise the globally smooth path happily follows a subharmonic.
        if (primary >= 0) {
            const double primaryHz = double(detectorRate)/primary;
            for (int c = 1; c < frame.count; ++c) {
                const double octavesBelow = std::max(0.0, std::log2(primaryHz/frame.hz[c]));
                frame.confidence[c] *= std::exp(-.08*octavesBelow);
            }
        }
        frame.confidence[0] = std::clamp(1-confidence, .01, 1.0);
        if (quality) { selectPath(frame); return; }
        if (primary < 0 || confidence < .78) {
            voicedFrames = 0; pushEstimate({frame.time, 0, confidence}); return;
        }
        const double hz = detectorRate/refinePeriod(primary, window);
        ++voicedFrames;
        pushEstimate({frame.time, voicedFrames >= 2 ? hz : 0, confidence});
    }
    void analyse(double sample, const Settings& settings) noexcept {
        const double filtered = analysisLowpass[1].process(analysisLowpass[0].process(sample));
        const double increment = double(detectorRate)/rate;
        const double before = analysisPhase;
        analysisPhase += increment;
        while (analysisPhase >= 1) {
            const double fraction = std::clamp((1-before)/increment, 0.0, 1.0);
            detector[detectorSamples % detector.size()] = previousFiltered + fraction*(filtered-previousFiltered);
            ++detectorSamples;
            analysisPhase -= 1;
            if (detectorSamples >= std::uint64_t(detectorSize) && detectorSamples % hop == 0) detect(settings);
        }
        previousFiltered = filtered;
        if (quality) advanceHdDetection();
    }
    double protect(double sample) noexcept {
        const double fast = pole(.001, rate), slow = pole(.030, rate);
        fastPower = fast*fastPower+(1-fast)*sample*sample;
        slowPower = slow*slowPower+(1-slow)*sample*sample;
        highpassLow += (sample-highpassLow)*(1-pole(1/(2*pi*2000), rate));
        const double high = sample-highpassLow;
        highPower = fast*highPower+(1-fast)*high*high;
        // Short-term waveform disagreement responds to a new phoneme or note
        // before the 43 ms pitch-analysis history has forgotten its predecessor.
        double mismatch = 0;
        if (current.hz > 0) {
            const double earlier = interpolate(original, double(position)-rate/current.hz, detectorChannel);
            mismatch = sample-earlier;
        }
        mismatchPower = fast*mismatchPower+(1-fast)*mismatch*mismatch;
        if (fastPower > 1e-6 && fastPower > 4*slowPower)
            transientHold = int(.008*rate);
        if (transientHold > 0) --transientHold;
        const bool noisy = highPower > .60*std::max(fastPower, 1e-10);
        const int quietThreshold = std::max(1, int(.0005*rate));
        quietSamples = std::abs(sample) < .0001 ? std::min(quietSamples+1, quietThreshold) : 0;
        // Keep attacks and breath consonants on the fixed 10 ms timeline.
        // Pitch-confidence/period jitter deliberately does not request an
        // anchor: it must not repeatedly restart the synthesis phase.
        // A level accent inside a voiced vowel is not a new source onset.
        // Recentring its read head would abruptly rewind the waveform phase.
        anchors[position&ringMask] = noisy || quietSamples >= quietThreshold;
        // HD's finalized F0 is intentionally old: comparing today's waveform
        // against that period falsely classifies ordinary vibrato as a new
        // transient. Its lookahead path already resolves those transitions.
        const bool changing = !quality && current.hz > 0 && mismatchPower > .7*std::max(fastPower, 1e-10);
        return transientHold || noisy || changing || fastPower < 1e-8 ? 0 : 1;
    }
    void beginLpc() noexcept {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            auto& f = burgForward[channel]; auto& b = burgBackward[channel];
            double energy = 0;
            const double emphasis = std::exp(-2*pi*150/rate);
            double previous = at(std::int64_t(position)-lpcLength)[channel];
            for (int i = 0; i < lpcLength; ++i) {
                const double x = at(std::int64_t(position)-lpcLength+1+i)[channel];
                // Windowing reduces the dominance of an abrupt frame boundary.
                const double value = (x-emphasis*previous) * lpcWindow[i];
                previous = x;
                f[i] = b[i] = value; energy += value*value;
            }
            lpcEnergy[channel] = energy;
            pendingReflection[channel].fill(0);
        }
    }
    void analyseLpcOrder(int m) noexcept {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            if (lpcEnergy[channel] < lpcLength*1e-10) continue;
            auto& f = burgForward[channel]; auto& b = burgBackward[channel];
            double numerator = 0, denominator = lpcEnergy[channel]*1e-12;
            for (int i = m+1; i < lpcLength; ++i) {
                numerator += f[i]*b[i-1];
                denominator += f[i]*f[i]+b[i-1]*b[i-1];
            }
            const double k = denominator > 1e-20 ? std::clamp(-2*numerator/denominator, -.999999, .999999) : 0;
            pendingReflection[channel][m] = safe(k);
            for (int i = lpcLength-1; i > m; --i) {
                const double oldF = f[i], oldB = b[i-1];
                f[i] = oldF+k*oldB; b[i] = oldB+k*oldF;
            }
        }
    }
    void finishLpc() noexcept {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            auto& result = pendingReflection[channel];
            // A nearly periodic frame can fit vanishingly narrow poles. Their
            // cancellation is exact only before resampling the excitation;
            // after a pitch change they ring on every consonant. Expand the
            // model's bandwidth before using it for source/filter synthesis.
            // This regularises the filter itself, rather than limiting audio.
            std::array<double, maxOrder+1> polynomial{}, previousPolynomial{};
            polynomial[0] = 1;
            for (int m = 1; m <= order; ++m) {
                previousPolynomial = polynomial;
                for (int i = 1; i < m; ++i)
                    polynomial[i] = previousPolynomial[i]+result[m-1]*previousPolynomial[m-i];
                polynomial[m] = result[m-1];
            }
            const double bandwidth = std::exp(-pi*40/rate);
            double expansion = 1;
            for (int i = 1; i <= order; ++i) {
                expansion *= bandwidth;
                polynomial[i] *= expansion;
            }
            for (int m = order; m >= 1; --m) {
                const double k = std::clamp(polynomial[m], -.999999, .999999);
                result[m-1] = k;
                previousPolynomial = polynomial;
                for (int i = 1; i < m; ++i)
                    polynomial[i] = (previousPolynomial[i]-k*previousPolynomial[m-i])/(1-k*k);
            }
            wantedReflection[channel] = result;
        }
    }
    double inverseFilter(double x, int channel) noexcept {
        auto& k = reflection[channel]; auto& state = analysisState[channel];
        double past = state[0]; state[0] = x;
        const auto coefficientBase = ((std::size_t(position)&coefficientMask)*channels+channel)*order;
        for (int i = 0; i < order; ++i) {
            k[i] += (wantedReflection[channel][i]-k[i])*(1-pole(.010, rate));
            coefficients[coefficientBase+i] = float(k[i]);
            const double nextPast = state[i+1];
            const double old = x;
            x += k[i]*past;
            state[i+1] = past+k[i]*old;
            past = nextPast;
        }
        const auto stateBase = ((std::size_t(position)&coefficientMask)*channels+channel)*(order+1);
        for (int i = 0; i <= order; ++i) latticeHistory[stateBase+i] = float(state[i]);
        return safe(x);
    }
    void initialiseHead(double sourceTime, std::array<std::array<double, maxOrder+1>, 2>& states) noexcept {
        // The inverse lattice already computed the backward-error history at
        // every source sample. A new synthesis head must start with that
        // history, not the old head's resonator state from another phoneme.
        const auto earlier = std::int64_t(std::floor(sourceTime))-1;
        const double fraction = sourceTime-std::floor(sourceTime);
        for (std::uint32_t c = 0; c < channels; ++c) {
            const auto first = ((std::size_t(std::max<std::int64_t>(0, earlier))&coefficientMask)*channels+c)*(order+1);
            const auto second = ((std::size_t(std::max<std::int64_t>(0, earlier+1))&coefficientMask)*channels+c)*(order+1);
            for (int i = 0; i <= order; ++i) {
                const double a = earlier < 0 ? 0 : latticeHistory[first+i];
                const double b = earlier+1 < 0 ? 0 : latticeHistory[second+i];
                states[c][i] = a+(b-a)*fraction;
            }
        }
    }
    double synthesisFilter(double x, int channel, double sourceTime,
                           std::array<double, maxOrder+1>& state) noexcept {
        const auto delayed = std::int64_t(std::floor(sourceTime));
        const double fraction = sourceTime-std::floor(sourceTime);
        const auto coefficientBase = ((std::size_t(std::max<std::int64_t>(0, delayed))&coefficientMask)*channels+channel)*order;
        const auto nextBase = ((std::size_t(std::max<std::int64_t>(0, delayed+1))&coefficientMask)*channels+channel)*order;
        for (int i = order-1; i >= 0; --i) {
            const double first = delayed < 0 ? 0 : coefficients[coefficientBase+i];
            const double second = delayed+1 < 0 ? 0 : coefficients[nextBase+i];
            const double k = first+(second-first)*fraction;
            x -= k*state[i];
            state[i+1] = state[i]+k*x;
        }
        state[0] = x;
        if (!std::isfinite(x) || std::abs(x) > 1e4) {
            state.fill(0); return 0;
        }
        return x;
    }
    double control(const Estimate& e, const Settings& settings, double protectionAmount = 1) noexcept {
        const double q = std::clamp(safe(settings.tune, kDefaultTune)*.01, 0.0, 1.0);
        // The last part of the dial is deliberately stepped. Only very clean
        // periodic frames qualify for the faster note decision; uncertain
        // consonant excursions keep the original confirmation protection.
        const double hard = std::clamp((q-.95)/.05, 0.0, 1.0);
        const double snap = e.confidence >= .98 ? hard : 0;
        const double amount = std::clamp(safe(settings.amount, 100)*.01, 0.0, 1.0);
        const double reference = std::clamp(safe(settings.a4Hz, 440), 400.0, 480.0);
        const auto mask = notes(settings);
        const bool voiced = e.hz >= 50 && e.hz <= 1400 && e.confidence >= .75 && mask != 0;
        double desired = 0;
        bool targetPending = false;
        if (voiced) {
            const double pitch = 69 + 12*std::log2(e.hz/reference);
            const int next = closestNote(pitch, mask, target, .20-.16*snap);
            const bool previousAllowed = target != -1000 && (mask & (1u << ((target%12+12)%12)));
            bool change = !pitchInitialised || !previousAllowed;
            if (!change && next != target) {
                if (pendingTarget != next || e.confidence < .85) {
                    pendingTarget = next;
                    pendingTargetSeconds = 0;
                    pendingConfidentSeconds = 0;
                    pendingSnapSeconds = 0;
                } else {
                    pendingTargetSeconds += 1/rate;
                    pendingConfidentSeconds = e.confidence >= .95 ? pendingConfidentSeconds+1/rate : 0;
                    pendingSnapSeconds = e.confidence >= .98 ? pendingSnapSeconds+1/rate : 0;
                }
                // Confidence can dip while a consonant enters the detector's
                // history. Confirm the destination note separately from the
                // retune ramp, so that an uncertain F0 overshoot cannot create
                // a brief wrong note. Once confirmed, Hard remains immediate.
                // A later recovery in confidence must not retroactively count
                // uncertain history toward the shorter confirmation interval.
                change = pendingConfidentSeconds >= .012 || pendingTargetSeconds >= .040 ||
                         (hard > 0 && pendingSnapSeconds >= .012-.008*hard);
            } else {
                pendingTarget = next; pendingTargetSeconds = pendingConfidentSeconds = pendingSnapSeconds = 0;
            }
            if (change) {
                heldSeconds = 0;
                baseline = pitch;
                target = next;
                pendingTarget = next; pendingTargetSeconds = pendingConfidentSeconds = pendingSnapSeconds = 0;
            }
            targetPending = next != target;
            pitchInitialised = true;
            unvoicedSeconds = 0;
            heldSeconds += 1/rate;
            baseline += (pitch-baseline)*(1-pole(1/(2*pi*2), rate));
            const double stable = std::clamp((heldSeconds-.12)/.020, 0.0, 1.0);
            const double vibrato = std::clamp(safe(settings.vibrato)*.01, 0.0, 1.0)*(1-q);
            desired = amount*100*(target-pitch+vibrato*stable*std::clamp(pitch-baseline, -1.0, 1.0));
            // While a new note is being confirmed, preserve the sung glide.
            // Dragging it back to the previous note would create a much larger
            // correction and a false jump when the candidate is accepted.
            // Hard holds a clean voiced plateau until the next note is
            // confirmed, then steps straight to it instead of passing a glide.
            if (targetPending) desired *= snap;
            lastPitch = pitch;
            meter.targetHz = reference*std::exp2((target-69)/12.0);
        } else {
            heldSeconds = 0; pendingTargetSeconds = pendingConfidentSeconds = pendingSnapSeconds = 0;
            unvoicedSeconds += 1/rate;
            if (unvoicedSeconds >= .080) {
                pitchInitialised = false; target = pendingTarget = -1000;
            }
            meter.targetHz = 0;
        }
        desired = std::clamp(desired, -1200.0, 1200.0);
        const double human = std::clamp(safe(settings.humanize)*.01, 0.0, 1.0)*(1-q);
        const double held = std::clamp((heldSeconds-.12)/.2, 0.0, 1.0);
        const double seconds = .001*retuneMilliseconds(settings.tune)+.120*human*held;
        const double response = voiced ? pole(seconds, rate) : pole(.004, rate);
        musicalCorrection = desired+(musicalCorrection-desired)*response;
        // A consonant must not inherit Natural's long musical retune time.
        // Neutralise its applied ratio on the separate 1 ms artifact ramp,
        // while preserving the phrase's musical control state.
        correction += (musicalCorrection*protectionAmount-correction)*(1-pole(.001, rate));
        if (!amount) correction = musicalCorrection = 0;
        meter.inputHz = e.hz; meter.confidence = e.confidence;
        meter.voiced = voiced; meter.correctionCents = correction;
        return voiced && amount > 0 && std::abs(correction) > .025 ? 1 : 0;
    }
    void updateWet(double code, bool amountEnabled) noexcept {
        // A pitch-shifted vowel accumulates phase relative to the original.
        // Repeatedly blending between them on every confidence/protection
        // fluctuation causes severe cancellations, even at identical RMS.
        // Keep one continuous synthesis phase through a voiced phrase. Guards
        // change its pitch ratio, not its amplitude. Brief consonants and
        // uncertain frames do not restart the voice's wet/dry crossfade.
        if (code >= 1 && amountEnabled) {
            voicedHold = int(std::ceil(.080*rate));
            if (code >= 2) correctionEngaged = true;
        } else if (voicedHold > 0) {
            --voicedHold;
        } else {
            correctionEngaged = false;
        }
        if (!amountEnabled) { correctionEngaged = false; voicedHold = 0; }
        const double targetWet = correctionEngaged ? 1 : 0;
        wet += (targetWet-wet)*(1-pole(targetWet > wet ? .004 : .003, rate));
    }
    std::array<double, 2> synthesise(const std::array<float, 2>& dry,
                                   const Settings& settings, const Estimate& estimate) noexcept {
        // Keep the source/filter histories warm even while Formants is off.
        // Re-enabling it otherwise reads stale excitation during its fade-in.
        if (lpcCountdown <= 0) {
            beginLpc(); lpcCountdown = lpcInterval; lpcStage = 0;
        }
        // Spread Burg's order stages across its two-millisecond update period.
        // A 32-frame callback must not pay the entire 64-order analysis at once
        // at high sample rates. Publish only the completed model.
        const int desiredStage = (lpcInterval-lpcCountdown+1)*order/lpcInterval;
        while (lpcStage < desiredStage) {
            analyseLpcOrder(lpcStage++);
            if (lpcStage == order) finishLpc();
        }
        for (std::uint32_t c = 0; c < channels; ++c)
            residual[position&ringMask][c] = float(inverseFilter(original[position&ringMask][c], int(c)));
        --lpcCountdown;
        const double alignedProtection = position >= latency ? protection[(position-latency)&ringMask] : 0;
        const double wantedWet = control(estimate, settings, alignedProtection);
        updateWet(meter.voiced ? (wantedWet > 0 ? 2 : 1) : 0, settings.amount > 0);
        formantMix += ((settings.formants ? 1.0 : 0.0)-formantMix)*(1-pole(.005, rate));
        const auto anchorTime = std::int64_t(position)-latency+int(.008*rate);
        const bool anchorRequest = anchorTime >= 0 && anchors[std::size_t(anchorTime)&ringMask];
        if (anchorRequest) anchorHold = int(.002*rate);
        else if (anchorHold > 0) --anchorHold;
        const bool anchorWanted = anchorRequest || anchorHold > 0;
        if (anchorWanted && !anchorActive) {
            // The 10 ms history provides eight milliseconds of onset lookahead.
            // Recenter during the preceding quiet/breathy boundary, before
            // the next voiced attack reaches the output timeline.
            alternateRead = double(position)-latency;
            initialiseHead(alternateRead, alternateSynthesisState);
            fade = 0; fadeStep = 1/std::max(1.0, .006*rate);
            spliceLow = double(latency)-period*.5;
            spliceHigh = double(latency)+period*.5;
        }
        anchorActive = anchorWanted;
        const double ratio = anchorActive ? 1 : std::exp2(correction/1200);
        if (estimate.hz > 0)
            period += (rate/estimate.hz-period)*(1-pole(.005, rate));
        period = std::clamp(period, rate/1200, rate/60);
        if (wet < 1e-5) {
            read = double(position)-latency; alternateRead = read; fade = 1;
            initialiseHead(read, synthesisState);
            spliceLow = double(latency)-period*.5;
            spliceHigh = double(latency)+period*.5;
        }
        const double guard = std::max(3.0, (ratio-1)*std::min(.004*rate, .5*period)+3);
        const double low = std::max(guard, spliceLow);
        const double high = std::max(low+period*.5, spliceHigh);
        const double lag = double(position)-read;
        if (!anchorActive && fade >= 1 && (lag < low || lag > high)) {
            double periods = std::round((lag-double(latency))/period);
            // Do not move these limits as F0 changes: a shrinking interval
            // otherwise makes a completed splice jump straight back across
            // its opposite edge, even while the correction ratio is unity.
            spliceLow = double(latency)-period*.5;
            spliceHigh = double(latency)+period*.5;
            // If the period grew, the current head can already be inside the
            // new interval. Refreshing the bounds then needs no waveform jump.
            if (periods != 0 || lag < guard) {
                if (periods == 0) periods = -1;
                alternateRead = read+periods*period;
                alternateRead = std::min(alternateRead, double(position)-guard);
                alternateRead = matchSplice(read, alternateRead, guard);
                const double selectedLag = double(position)-alternateRead;
                spliceLow = std::min(spliceLow, selectedLag-.10*period);
                spliceHigh = std::max(spliceHigh, selectedLag+.10*period);
                // A period splice carries the shifted excitation's phase.
                alternateSynthesisState = synthesisState;
                fade = 0;
                fadeStep = 1/std::max(8.0, std::min(.004*rate, .5*period));
            }
        }
        const double blend = fade < 1 ? .5-.5*std::cos(pi*fade) : 0;
        std::array<double, 2> result{};
        for (std::uint32_t c = 0; c < channels; ++c) {
            const auto shifted = [&](const auto& data) {
                const double first = interpolate(data, read, int(c));
                return fade < 1 ? first+(interpolate(data, alternateRead, int(c))-first)*blend : first;
            };
            const double normal = shifted(original);
            // Resynthesize each source head before the splice crossfade.
            // Blending two excitations under one interpolated LPC model
            // does not preserve either filter's state or cancellation.
            double preserved = synthesisFilter(interpolate(residual, read, int(c)), int(c), read, synthesisState[c]);
            if (fade < 1) {
                const double alternate = synthesisFilter(interpolate(residual, alternateRead, int(c)), int(c),
                                                         alternateRead, alternateSynthesisState[c]);
                preserved += (alternate-preserved)*blend;
            }
            double processed = normal+(preserved-normal)*formantMix;
            // A pathological model should fall back to the delayed voice, not
            // clip every sample or send an unstable IIR burst to the output.
            const double ceiling = std::max(2.0, 12*std::sqrt(inputPower));
            if (!std::isfinite(processed) || std::abs(processed) > ceiling) {
                synthesisState[c].fill(0); processed = normal;
            }
            result[c] = dry[c]+wet*(processed-dry[c]);
        }
        read += ratio;
        if (fade < 1) {
            alternateRead += ratio; fade += fadeStep;
            if (fade >= 1) { read = alternateRead; synthesisState = alternateSynthesisState; }
        }
        return result;
    }
};

PitchCorrectorDSP::PitchCorrectorDSP() : m_impl(std::make_unique<Impl>()) {}
PitchCorrectorDSP::~PitchCorrectorDSP() = default;

void PitchCorrectorDSP::prepare(double sampleRate, std::uint32_t,
                                std::uint32_t channels, int quality) {
    auto& p = *m_impl;
    p.rate = std::clamp(safe(sampleRate, 48000), 12000.0, 192000.0);
    p.channels = std::clamp(channels, 1u, 2u);
    p.quality = quality > 0 ? 1 : 0;
    p.detectorSize = p.quality ? 1024 : 512;
    // Both modes preserve the vocal pulse waveform. HD's additional history
    // lets its longer detector and lookahead finish before that sample plays.
    p.latency = std::uint32_t(std::ceil((p.quality ? .080 : .010)*p.rate));
    // The LPC synthesis filter continues settling after its source ends.
    // Include its quiet decay in offline/export draining, in addition to PDC.
    p.tail = p.latency+std::uint32_t(std::ceil(.080*p.rate));
    const std::size_t ringSize = powerOfTwo(std::size_t(std::ceil(p.rate*.5))+p.latency+8);
    p.ringMask = ringSize-1;
    p.original.resize(ringSize); p.residual.resize(ringSize);
    p.protection.resize(ringSize);
    p.anchors.resize(ringSize);
    p.order = std::clamp(2*int(std::ceil(p.rate/3000)), 16, maxOrder);
    p.lpcLength = std::max(64, int(std::round(.030*p.rate)));
    p.lpcWindow.resize(p.lpcLength);
    for (int i = 0; i < p.lpcLength; ++i)
        p.lpcWindow[i] = float(.54-.46*std::cos(2*pi*i/(p.lpcLength-1)));
    p.lpcInterval = std::max(1, int(std::round(.002*p.rate)));
    for (std::uint32_t c = 0; c < p.channels; ++c) {
        p.burgForward[c].resize(p.lpcLength); p.burgBackward[c].resize(p.lpcLength);
    }
    const std::size_t coefficientSize = powerOfTwo(p.latency+std::size_t(std::ceil(p.rate/30))+8);
    p.coefficientMask = coefficientSize-1;
    p.coefficients.resize(coefficientSize*p.channels*p.order);
    p.latticeHistory.resize(coefficientSize*p.channels*(p.order+1));
    const double cutoff = std::min(4000.0, p.rate*.4);
    p.analysisLowpass[0].prepare(p.rate, cutoff, .541196100146197);
    p.analysisLowpass[1].prepare(p.rate, cutoff, 1.306562964876377);
    p.prepared = true;
    reset();
}

void PitchCorrectorDSP::reset() noexcept {
    auto& p = *m_impl;
    p.position = p.detectorSamples = p.estimateCount = 0;
    std::fill(p.original.begin(), p.original.end(), std::array<float,2>{});
    std::fill(p.residual.begin(), p.residual.end(), std::array<float,2>{});
    std::fill(p.protection.begin(), p.protection.end(), 0.0f);
    std::fill(p.anchors.begin(), p.anchors.end(), 0);
    std::fill(p.coefficients.begin(), p.coefficients.end(), 0.0f);
    std::fill(p.latticeHistory.begin(), p.latticeHistory.end(), 0.0f);
    p.reflection = {}; p.wantedReflection = {}; p.analysisState = {}; p.synthesisState = {};
    p.pendingReflection = {}; p.lpcEnergy = {}; p.lpcStage = 0;
    p.alternateSynthesisState = {};
    p.detector = {}; p.ordered = {}; p.difference = {}; p.estimates = {};
    p.candidateFrames = {}; p.candidateFill = 0; p.current = {};
    p.pendingDetection = {}; p.detectionLag = p.detectionTicks = p.detectionScan = 0;
    p.detectionMinimum = p.detectionMaximum = p.detectionWindow = p.detectionBest = 0;
    p.detectionPrimary = -1; p.detectionRunning = 0;
    p.analysisPhase = p.previousFiltered = 0;
    for (auto& filter : p.analysisLowpass) filter.z1 = filter.z2 = 0;
    p.correction = p.musicalCorrection = 0; p.baseline = p.lastPitch = 69;
    p.heldSeconds = p.inputPower = p.outputPower = p.wet = 0;
    p.gain = p.formantMix = 1;
    p.read = p.alternateRead = -double(p.latency); p.fade = p.fadeStep = 1;
    p.period = p.rate/220; p.target = -1000; p.voicedFrames = p.lpcCountdown = 0;
    p.pitchInitialised = false; p.meter = {};
    p.pendingTarget = -1000; p.pendingTargetSeconds = p.pendingConfidentSeconds = p.pendingSnapSeconds = p.unvoicedSeconds = 0;
    p.correctionEngaged = false; p.voicedHold = 0;
    p.detectorChannel = p.transientHold = 0; p.channelPower = {};
    p.quietSamples = p.anchorHold = 0; p.anchorActive = false;
    p.fastPower = p.slowPower = p.highpassLow = p.highPower = p.mismatchPower = 0;
}

void PitchCorrectorDSP::process(const float* const* input, float* const* output,
                                std::uint32_t frames, const Settings& settings) noexcept {
    auto& p = *m_impl;
    if (!output) return;
    if (!p.prepared) {
        for (std::uint32_t c = 0; c < p.channels; ++c)
            if (output[c]) std::fill_n(output[c], frames, 0.0f);
        return;
    }
    const double desiredGain = std::pow(10, std::clamp(safe(settings.outputDb), -24.0, 12.0)/20);
    const double meterPole = pole(.010, p.rate), gainPole = pole(.005, p.rate);
    for (std::uint32_t i = 0; i < frames; ++i) {
        std::array<float,2> in{};
        for (std::uint32_t c = 0; c < p.channels; ++c)
            in[c] = float(input && input[c] ? safe(input[c][i]) : 0);
        if (p.channels == 1) in[1] = in[0];
        p.original[p.position&p.ringMask] = in;
        for (std::uint32_t c = 0; c < p.channels; ++c)
            p.channelPower[c] = meterPole*p.channelPower[c]+(1-meterPole)*double(in[c])*in[c];
        // A stereo vocal can have opposite channel polarities. Taking L+R
        // would erase its pitch; a hysteretic shared reference avoids that.
        if (p.channels == 2 && p.channelPower[1-p.detectorChannel] > 1.5*p.channelPower[p.detectorChannel])
            p.detectorChannel = 1-p.detectorChannel;
        const double mono = in[p.detectorChannel];
        p.inputPower = p.channels == 2 ? .5*(p.channelPower[0]+p.channelPower[1]) : p.channelPower[0];
        p.analyse(mono, settings);
        p.protection[p.position&p.ringMask] = float(p.protect(mono));
        const auto dry = p.at(std::int64_t(p.position)-p.latency);
        const Estimate estimate = p.quality ? p.estimateAt(double(p.position)-p.latency) : p.current;
        const auto processed = p.synthesise(dry, settings, estimate);
        p.gain = desiredGain+(p.gain-desiredGain)*gainPole;
        double outputPower = 0;
        for (std::uint32_t c = 0; c < p.channels; ++c) {
            const double value = safe(processed[c]*p.gain);
            if (output[c]) output[c][i] = float(value);
            outputPower += value*value/p.channels;
        }
        p.outputPower = meterPole*p.outputPower+(1-meterPole)*outputPower;
        ++p.position;
    }
    p.meter.inputLevel = std::sqrt(p.inputPower);
    p.meter.outputLevel = std::sqrt(p.outputPower);
    p.meter.serial = p.position;
}

std::uint32_t PitchCorrectorDSP::latencySamples() const noexcept { return m_impl->latency; }
std::uint32_t PitchCorrectorDSP::tailSamples() const noexcept { return m_impl->tail; }
Telemetry PitchCorrectorDSP::telemetry() const noexcept { return m_impl->meter; }

} // namespace daw::plugins::pitch
