#include <vlt/creator.hpp>

struct State {
    float envelope = 0.0f;
};

struct Outputs {
    VLT_PORT("audio") vlt::AudioFrame audio;
    VLT_PORT("envelope") float envelope;
    VLT_PORT("gate") bool gate;
};

VLT_NODE Outputs process(
    State& state, const vlt::Context& context,
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("threshold") float threshold = 0.1f)
{
    const float peak = std::max(std::abs(input.left), std::abs(input.right));
    const float seconds = peak > state.envelope ? 0.005f : 0.1f;
    const float alpha = 1.0f - vlt::exp(
        -1.0f / (seconds * float(context.sampleRate)));
    state.envelope += alpha * (peak - state.envelope);
    return {input, state.envelope,
            state.envelope > vlt::clamp(threshold, 0.0f, 1.0f)};
}
