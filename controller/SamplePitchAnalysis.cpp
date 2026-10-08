#include "SamplePitchAnalysis.hpp"
#include "MediaWorker.hpp"
#include "DSP/Resampler.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace daw::analysis {
namespace {

// YIN cumulative-mean normalized difference, with fractional-period refinement.
// https://www.ee.columbia.edu/~dpwe/papers/deChevK02-yin.pdf
double windowPitch(const std::vector<float>& audio, double rate,
    const std::vector<float>& source, double sourceRate) {
    const int maximum = std::min(int(rate / 20), int(audio.size() / 3));
    const int minimum = std::max(2, int(rate / 4000));
    if (maximum <= minimum + 2) return 0;
    const int window = int(audio.size()) - maximum - 2;
    std::vector<double> difference(std::size_t(maximum) + 2, 1.0);
    double running = 0;
    for (int lag = 1; lag <= maximum + 1; ++lag) {
        double sum = 0;
        for (int i = 0; i < window; ++i) {
            const double delta = double(audio[i]) - audio[i + lag];
            sum += delta * delta;
        }
        running += sum;
        difference[lag] = running > 1e-20 ? sum * lag / running : 1;
    }
    int period = 0;
    for (int lag = minimum; lag <= maximum; ++lag) {
        if (difference[lag] < .12) {
            while (lag < maximum && difference[lag + 1] < difference[lag]) ++lag;
            period = lag;
            break;
        }
    }
    if (!period) return 0;
    const double scale = sourceRate / rate;
    const int refinementWindow = int(source.size()) - int(std::ceil((maximum + 2) * scale)) - 4;

    // The normalized curve slopes: fitting it directly biases the cents.
    // Minimize the actual waveform error using the same cubic fractional-delay
    // refinement as the existing vocal pitch detector.
    const auto error = [&](double shift) {
        const int integer = int(std::floor(shift));
        const double t = shift - integer;
        double sum = 0;
        for (int i = 1; i < refinementWindow; ++i) {
            const int j = i + integer;
            const double a = source[j - 1], b = source[j], c = source[j + 1], d = source[j + 2];
            const double interpolated = b + t * (.5 * (c - a) + t *
                (a - 2.5 * b + 2 * c - .5 * d + t * (.5 * (d - a) + 1.5 * (b - c))));
            const double delta = source[i] - interpolated;
            sum += delta * delta;
        }
        return sum;
    };
    const auto refine = [&](double center) {
        constexpr double golden = .6180339887498948482;
        double low = center - .75 * scale, high = center + .75 * scale;
        double left = high - golden * (high - low), right = low + golden * (high - low);
        double a = error(left), b = error(right);
        for (int i = 0; i < 18; ++i) {
            if (a < b) {
                high = right; right = left; b = a;
                left = high - golden * (high - low); a = error(left);
            } else {
                low = left; left = right; a = b;
                right = low + golden * (high - low); b = error(right);
            }
        }
        return .5 * (low + high);
    };
    double refined = refine(period * scale);
    double energy = 0;
    for (int i = 1; i < refinementWindow; ++i) energy += double(source[i]) * source[i];
    const double residual = error(refined);
    // A dominant second/third harmonic can pass YIN's first threshold. Only
    // prefer a longer period when it removes a substantial real residual;
    // pure tones must not drop an octave because an integer lag fits better.
    if (residual > energy * .001) {
        for (int multiple : {2, 3}) {
            const double candidate = refined * multiple;
            if (candidate > maximum * scale) break;
            const double alternative = refine(candidate);
            if (error(alternative) < residual * .1) { refined = alternative; break; }
        }
    }
    return sourceRate / refined;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : .5 * (values[middle - 1] + values[middle]);
}

} // namespace

SamplePitchEstimate detectSamplePitch(const engine::SampleBuffer& audio,
    engine::FrameCount first, engine::FrameCount end,
    const std::function<bool()>& keepGoing) {
    if (MediaWorker::enabled()) return MediaWorker::pitch(audio, first, end, keepGoing);
    SamplePitchEstimate result;
    const double rate = audio.sampleRate();
    end = std::min(end, audio.frames());
    if (!std::isfinite(rate) || rate < 8000 || rate > 384000 ||
        !audio.channels() || first >= end) return result;
    if (double(end - first) / rate < .04) {
        result.status = SamplePitchStatus::TooShort;
        return result;
    }
    const double analysisRate = std::min(rate, 24000.0);
    const auto windowFrames = std::min(end - first,
        engine::FrameCount(std::ceil(rate * (8192.0 / analysisRate))));
    const int windows = std::min(16, 1 + int(std::ceil(
        double(end - first - windowFrames) / (windowFrames / 2.0))));

    // Select one channel for the whole analysis by AC energy. Opposite-phase
    // stereo must not cancel; stereo balance must not switch the reference
    // halfway through a note. Energy sampling is bounded even for long files.
    int channel = 0;
    double bestEnergy = 0;
    const auto probes = std::min<engine::FrameCount>(8192, end - first);
    for (int ch = 0; ch < audio.channels(); ++ch) {
        const auto* source = audio.channel(engine::ChannelCount(ch));
        double sum = 0, squares = 0;
        for (engine::FrameCount i = 0; i < probes; ++i) {
            const auto at = first + engine::FrameCount(
                std::uint64_t(i) * (end - first - 1) / std::max(1u, probes - 1));
            const double value = source[at];
            if (!std::isfinite(value)) return result;
            sum += value; squares += value * value;
        }
        const double energy = squares - sum * sum / probes;
        if (energy > bestEnergy) { bestEnergy = energy; channel = ch; }
    }
    if (bestEnergy / probes < 1e-10) return result;
    const auto* source = audio.channel(engine::ChannelCount(channel));
    std::vector<std::vector<float>> prepared;
    std::vector<std::vector<float>> originals;
    std::vector<double> levels;
    double maximumLevel = 0;
    for (int w = 0; w < windows; ++w) {
        if (keepGoing && !keepGoing()) return {};
        const auto from = first + engine::FrameCount(std::uint64_t(w) *
            (end - first - windowFrames) / std::max(1, windows - 1));
        std::vector<float> samples(source + from, source + from + windowFrames);
        for (float value : samples) if (!std::isfinite(value)) return {};
        const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
        double energy = 0;
        for (float& value : samples) { value = float(value - mean); energy += double(value) * value; }
        const double level = energy / samples.size();
        maximumLevel = std::max(maximumLevel, level);
        levels.push_back(level);
        prepared.push_back(engine::dsp::resampleInterleaved(samples, 1, samples.size(), rate, analysisRate));
        originals.push_back(std::move(samples));
    }
    if (maximumLevel < 1e-10) return result;
    std::vector<double> pitches;
    int audible = 0;
    for (int w = 0; w < windows; ++w) {
        if (keepGoing && !keepGoing()) return {};
        // Ignore silence and late tails, but retain attacks as evidence when
        // deciding whether the selected region really holds one steady note.
        if (levels[w] < std::max(1e-10, maximumLevel * .001)) continue;
        ++audible;
        const double hz = windowPitch(prepared[w], analysisRate, originals[w], rate);
        if (hz > 0) pitches.push_back(69 + 12 * std::log2(hz / 440.0));
    }
    if (pitches.empty()) return result;
    const double pitch = median(pitches);
    std::vector<double> deviations;
    int consistent = 0;
    for (double value : pitches) {
        const double deviation = std::abs(value - pitch);
        deviations.push_back(deviation);
        if (deviation <= .35) ++consistent;
    }
    if (pitches.size() < std::size_t(std::max(1, (audible * 2 + 2) / 3)) ||
        consistent < int((pitches.size() * 4 + 4) / 5) || median(deviations) > .12) {
        result.status = SamplePitchStatus::Unstable;
        return result;
    }
    const int note = int(std::lround(pitch));
    if (note < 0 || note > 127) return result;
    result.status = SamplePitchStatus::Detected;
    result.midiNote = note;
    result.frequencyHz = 440 * std::exp2((pitch - 69) / 12);
    result.cents = 100 * (pitch - note);
    return result;
}

} // namespace daw::analysis
