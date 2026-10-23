#pragma once
#include "Internal/WavetableProgram.hpp"
#include "Internal/FluxDsp.hpp"
#include <complex>
#include <memory>

namespace daw::plugins::wavetable {
struct Table {
    unsigned frames = 0;
    // Each octave has the same frame layout. Guard samples make interpolation branch-free.
    std::array<std::vector<float>, 11> levels;
    float sample(double phase, double position, double increment) const noexcept;
};
struct PreparedRoute { unsigned source = 0, target = 0; double amount = 0; bool bipolar = false; Curve response; };
struct PreparedPatch {
    State state;
    std::array<std::shared_ptr<const Table>, oscillatorCount> tables;
    std::array<int, oscillatorCount> resources{};
    std::vector<PreparedRoute> routes;
    std::array<bool, lfoCount> usedLfos{};
    std::array<std::unique_ptr<flux::dsp::Processor>, fxCount> effects;
    std::array<unsigned, fxCount> effectLatency{};
    std::array<unsigned, 3> busLatency{};
    std::array<flux::dsp::History, 3> busAlign;
    unsigned latency = 0;
    std::uint64_t revision = 0;
};
std::shared_ptr<PreparedPatch> preparePatch(const State&, double sampleRate, std::uint64_t revision);
void fft(std::span<std::complex<double>> data, bool inverse);
class Renderer {
public:
    Renderer();
    ~Renderer();
    void prepare(double sampleRate);
    void reset() noexcept;
    void event(const PluginEvent&, const PreparedPatch&, std::array<double, parameterCount>&) noexcept;
    void render(PreparedPatch&, const PluginProcessContext&, std::array<double, parameterCount>&,
                unsigned start, unsigned end) noexcept;
    unsigned activeVoices() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
