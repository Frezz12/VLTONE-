// SPDX-License-Identifier: MIT
#include <vlt/spectre/Analyzer.hpp>
#include <cmath>
#include <iostream>

int main() {
    vlt::spectre::Analyzer analyzer;
    if (!analyzer.prepare(48000.0)) return 1;

    // In a plugin, replace this generator with pushStereo() in your audio callback.
    // Opposite phases intentionally demonstrate that the display does not cancel.
    for (int i = 0; i < 48000; ++i) {
        const auto sample = float(0.5 * std::sin(6.283185307179586 * 1000.0 * i / 48000.0));
        analyzer.pushSample(sample, -sample);
    }
    // In a real-time host, this belongs on the UI or analysis worker thread.
    analyzer.read();
    const auto& frame = analyzer.frame();
    std::cout << "Peak: " << frame.strongestFrequencyHz << " Hz, "
              << frame.strongestLevelDb << " dBFS\n"
              << "Resolution: " << frame.sampleRate / double(frame.fftSize) << " Hz/bin\n";
}