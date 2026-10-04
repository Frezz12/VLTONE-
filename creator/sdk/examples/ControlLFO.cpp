#include <vlt/creator.hpp>

struct State {
    double phase = 0.0;
    bool previousReset = false;
};

// Number output: wire it to Gain.gain for a tremolo effect.
VLT_NODE float process(
    State& state, const vlt::Context& context,
    VLT_PORT("rate") float rate = 1.0f,
    VLT_PORT("depth") float depth = 0.5f,
    VLT_PORT("reset") bool restart = false)
{
    constexpr double twoPi = 6.283185307179586;
    if (restart && !state.previousReset) state.phase = 0.0;
    state.previousReset = restart;
    const float wave = 0.5f + 0.5f * vlt::sin(float(state.phase));
    state.phase += twoPi * double(vlt::clamp(rate, 0.01f, 20.0f))
                   / context.sampleRate;
    if (state.phase >= twoPi) state.phase -= twoPi;
    return 1.0f - vlt::clamp(depth, 0.0f, 1.0f) * wave;
}
