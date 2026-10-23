// SPDX-License-Identifier: MIT
#include <vlt/spectre/Analyzer.hpp>
#include <vlt/spectre/DisplaySmoothing.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <vector>

// Count normal allocations on the calling thread only. This covers all vector/
// complex allocations in the DSP path without counting unrelated thread startup.
thread_local bool countAllocations = false;
thread_local unsigned allocations = 0;
void* operator new(std::size_t size) {
    if (countAllocations) ++allocations;
    if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace vlt::spectre;
namespace {
constexpr double tau = 6.283185307179586;
int failures = 0, checks = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }
float maximum(const std::vector<float>& values) { return *std::max_element(values.begin(), values.end()); }

void feed(Analyzer& analyzer, double rate, double frequency, double amplitude,
          std::size_t count, double rightGain = 1.0, std::uint64_t offset = 0) {
    for (std::size_t i = 0; i < count; ++i) {
        const float value = float(amplitude * std::sin(tau * frequency * double(i + offset) / rate));
        analyzer.pushSample(value, value * float(rightGain));
    }
}

void measurementChecks() {
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        Analyzer analyzer;
        AnalyzerSettings settings;
        check(analyzer.prepare(rate, settings), "prepare supported sample rates");
        const double frequency = 173.0 * rate / double(settings.fftSize);
        feed(analyzer, rate, frequency, 0.5, settings.fftSize);
        check(analyzer.read(), "complete window publishes");
        const auto inPhase = analyzer.frame();
        check(near(inPhase.strongestLevelDb, -6.0205999, 0.015), "calibrated sine amplitude");
        check(near(inPhase.strongestFrequencyHz, frequency, 0.05), "calibrated bin frequency");
        check(near(maximum(inPhase.instantaneousDb), -6.0205999, 0.15), "log display preserves tone amplitude");
        check(!analyzer.read(), "reading unchanged capture does not advance envelopes");

        analyzer.reset();
        check(analyzer.read() && !analyzer.frame().ready, "reset publishes empty frame");
        feed(analyzer, rate, frequency, 0.5, settings.fftSize, -1.0);
        analyzer.read();
        check(near(maximum(analyzer.frame().instantaneousDb), maximum(inPhase.instantaneousDb), 0.001),
              "opposite-phase stereo retains its full spectrum");

        analyzer.reset();
        feed(analyzer, rate, frequency, 0.5, settings.fftSize, 0.0);
        analyzer.read();
        check(near(analyzer.frame().strongestLevelDb, -9.0308999, 0.015),
              "one active stereo channel measures -3 dB average power");
    }
    Analyzer analyzer;
    analyzer.prepare(48000.0);
    feed(analyzer, 48000.0, 997.3, 0.25, 8192);
    analyzer.read();
    check(near(analyzer.frame().strongestFrequencyHz, 997.3, 0.15), "off-bin peak frequency interpolation");
    check(near(analyzer.frame().strongestLevelDb, -12.0412, 0.4), "off-bin amplitude within Hann scalloping estimate");

    analyzer.reset();
    feed(analyzer, 48000.0, 17003.0, 0.1, 8192);
    analyzer.read();
    check(maximum(analyzer.frame().instantaneousDb) > -21.5f, "narrow treble peak is not lost between log points");

    analyzer.prepare(16000.0);
    feed(analyzer, 16000.0, 1000.0, 0.5, 8192);
    analyzer.read();
    bool aboveNyquistSilent = true;
    for (std::size_t i = 0; i < analyzer.frame().frequenciesHz.size(); ++i)
        if (analyzer.frame().frequenciesHz[i] > 8000.0f)
            aboveNyquistSilent &= analyzer.frame().instantaneousDb[i] == -120.0f;
    check(aboveNyquistSilent, "nothing is invented above Nyquist");
}

void lifecycleChecks() {
    Analyzer analyzer;
    check(!analyzer.read(), "unprepared analyzer is safe");
    analyzer.pushStereo(nullptr, nullptr, 10);
    analyzer.reset();
    AnalyzerSettings settings;
    std::string error;
    settings.fftSize = 3000;
    check(!analyzer.prepare(48000, settings, &error) && !error.empty(), "reject non-power-of-two FFT");
    settings = {};
    settings.hopSize = 0;
    check(!analyzer.prepare(48000, settings), "reject zero hop");
    check(!analyzer.prepare(std::numeric_limits<double>::quiet_NaN()), "reject invalid sample rate");
    analyzer.prepare(48000.0);
    feed(analyzer, 48000.0, 1000.0, 0.5, 8191);
    check(!analyzer.read(), "partial startup window does not pretend to be a measurement");
    analyzer.pushSample(0.0f, 0.0f);
    analyzer.read();
    const auto held = analyzer.frame().levelsDb;
    analyzer.setFrozen(true);
    analyzer.pushStereo(nullptr, nullptr, 16384);
    check(!analyzer.read() && analyzer.frame().levelsDb == held, "freeze preserves displayed samples");
    analyzer.setFrozen(false);
    check(analyzer.read() && maximum(analyzer.frame().levelsDb) == -120.0f,
          "unfreeze consumes the latest window without stale backlog");

    analyzer.reset();
    for (int i = 0; i < 8192; ++i)
        analyzer.pushSample(std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
    analyzer.read();
    check(maximum(analyzer.frame().levelsDb) == -120.0f, "nonfinite input is sanitized");
    Ballistics invalid;
    invalid.attackMs = -1;
    check(!analyzer.setBallistics(invalid), "reject invalid ballistics");
    check(!analyzer.prepare(0.0) && analyzer.frame().fftSize == 8192, "failed prepare preserves prior state");

    // These calls must stay allocation-free after prepare, including a reset/read.
    countAllocations = true;
    allocations = 0;
    feed(analyzer, 48000, 500, 0.2, 20000);
    analyzer.read();
    analyzer.reset();
    analyzer.read();
    countAllocations = false;
    check(allocations == 0, "capture, FFT, reset and read allocate nothing after prepare");
}

void timingChecks() {
    // Equivalent streams with different callback partitions must be identical.
    Analyzer sampleWise, blocked;
    sampleWise.prepare(48000);
    blocked.prepare(48000);
    std::vector<float> samples(20000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = float(0.4 * std::sin(tau * 937.5 * double(i) / 48000));
        sampleWise.pushSample(samples[i], samples[i]);
    }
    for (std::size_t offset = 0; offset < samples.size(); offset += 73)
        blocked.pushStereo(samples.data() + offset, nullptr, std::min<std::size_t>(73, samples.size() - offset));
    sampleWise.read(); blocked.read();
    check(sampleWise.frame().instantaneousDb == blocked.frame().instantaneousDb,
          "spectrum is invariant to callback block size");

    // Release uses elapsed AUDIO time, not the number of UI timer callbacks.
    Analyzer often, rarely;
    often.prepare(48000); rarely.prepare(48000);
    feed(often, 48000, 937.5, 0.5, 8192);
    feed(rarely, 48000, 937.5, 0.5, 8192);
    often.read(); rarely.read();
    // First establish exactly silent windows, then compare two read cadences.
    often.pushStereo(nullptr, nullptr, 8192); rarely.pushStereo(nullptr, nullptr, 8192);
    often.read(); rarely.read();
    for (int block = 0; block < 16; ++block) {
        often.pushStereo(nullptr, nullptr, 512); often.read();
        rarely.pushStereo(nullptr, nullptr, 512);
    }
    rarely.read();
    check(near(maximum(often.frame().levelsDb), maximum(rarely.frame().levelsDb), 0.001),
          "release is independent of consumer cadence for a constant target");
}

void concurrencyCheck() {
    Analyzer analyzer;
    AnalyzerSettings settings;
    settings.fftSize = 1024;
    settings.hopSize = 64;
    analyzer.prepare(48000, settings);
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        for (int cycle = 0; cycle < 120; ++cycle) {
            feed(analyzer, 48000, 1500, 0.25, 4096);
            if (cycle % 11 == 0) analyzer.reset();
            std::this_thread::yield();
        }
        // A known final window lets us check latest-window delivery after pressure.
        analyzer.reset();
        feed(analyzer, 48000, 3000, 0.5, 8192);
        finished.store(true, std::memory_order_release);
    });
    bool valid = true;
    while (!finished.load(std::memory_order_acquire)) {
        if (analyzer.read())
            for (const auto level : analyzer.frame().levelsDb) valid &= std::isfinite(level);
        std::this_thread::yield();
    }
    producer.join();
    analyzer.read();
    check(valid, "concurrent producer/reader/reset keep frames finite");
    check(near(analyzer.frame().strongestFrequencyHz, 3000, 0.1), "slow consumer gets latest complete window");
}

void smoothingChecks() {
    std::vector<float> frequencies(1024), levels(1024);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        frequencies[i] = float(20.0 * std::pow(1000.0, double(i) / 1023.0));
        levels[i] = i % 2 ? -50.0f : -52.0f;
    }
    const auto smooth = smoothDisplaySpectrum(frequencies.data(), levels.data(), levels.size(), 1.0 / 12.0);
    double rawVariation = 0, smoothVariation = 0;
    for (std::size_t i = 1; i < levels.size(); ++i) {
        rawVariation += std::abs(levels[i] - levels[i-1]);
        smoothVariation += std::abs(smooth[i] - smooth[i-1]);
    }
    check(smoothVariation < rawVariation * .1, "visual smoothing removes small jagged fluctuations");
    levels[500] = -6.0f; levels[499] = levels[501] = -12.0f;
    const auto peaks = smoothDisplaySpectrum(frequencies.data(), levels.data(), levels.size(), 1.0 / 12.0);
    check(std::abs(peaks[500] + 6.0f) < 0.5f && std::max_element(peaks.begin(), peaks.end()) - peaks.begin() == 500,
          "rounded envelope keeps isolated peaks within 0.5 dB at their measured frequency");
    check(maximum(peaks) <= maximum(levels), "smoothing never invents a higher peak");
    check(peaks[495] > levels[495] + 10, "narrow peaks get visible rounded shoulders");
    auto changing = levels;
    changing[500] += 0.02f;
    const auto next = smoothDisplaySpectrum(frequencies.data(), changing.data(), changing.size(), 1.0 / 12.0);
    float largestChange = 0;
    for (std::size_t i = 0; i < next.size(); ++i) largestChange = std::max(largestChange, std::abs(next[i] - peaks[i]));
    check(largestChange <= 0.0201f, "slow changes never trigger discontinuous peak restoration");
    check(smoothDisplaySpectrum(frequencies.data(), levels.data(), levels.size(), 0) == levels,
          "visual smoothing can be completely disabled");
    check(smoothDisplaySpectrum(frequencies.data(), levels.data(), levels.size(), -1).empty(),
          "invalid smoothing settings rejected");
}

void benchmark() {
    Analyzer analyzer;
    analyzer.prepare(48000);
    constexpr int count = 240;
    double captureTotal = 0, readTotal = 0, worstCapture = 0;
    for (int i = 0; i < count; ++i) {
        const auto start = std::chrono::steady_clock::now();
        analyzer.pushStereo(nullptr, nullptr, 1024);
        const auto captured = std::chrono::steady_clock::now();
        analyzer.read();
        const auto analyzed = std::chrono::steady_clock::now();
        const double capture = std::chrono::duration<double, std::micro>(captured - start).count();
        captureTotal += capture;
        readTotal += std::chrono::duration<double, std::micro>(analyzed - captured).count();
        worstCapture = std::max(worstCapture, capture);
    }
    std::cout << "8192 FFT / 768 display points: capture " << captureTotal / count
              << " us per 1024 frames (worst " << worstCapture << "), read "
              << readTotal / count << " us. Timing is machine-specific.\n";
}
} // namespace

int main() {
    measurementChecks(); lifecycleChecks(); timingChecks(); concurrencyCheck(); smoothingChecks(); benchmark();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}