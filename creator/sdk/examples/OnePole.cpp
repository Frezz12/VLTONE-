#include <vlt/creator.hpp>

struct State {
    vlt::AudioFrame history{};
};

void reset(State& state)
{
    state.history = {};
}

VLT_NODE vlt::AudioFrame process(
    State& state, const vlt::Context& context,
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("cutoff") float cutoff = 1500.0f)
{
    cutoff = vlt::clamp(cutoff, 20.0f, float(context.sampleRate * 0.45));
    const float alpha = 1.0f - vlt::exp(
        -6.28318530718f * cutoff / float(context.sampleRate));
    state.history = state.history + (input - state.history) * alpha;
    return state.history;
}
