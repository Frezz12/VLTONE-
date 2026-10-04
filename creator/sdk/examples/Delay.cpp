#include <vlt/creator.hpp>

struct State {
    vlt::Buffer<vlt::AudioFrame> delay;
    unsigned write = 0;
    float smoothing = 0.0f;
    float milliseconds = 120.0f;
    float mix = 0.25f;
};

void prepare(State& state, vlt::PrepareContext& context)
{
    state.delay.prepare(unsigned(std::ceil(context.sampleRate * 0.5)) + 2);
    state.smoothing = 1.0f / float(context.sampleRate * 0.02 + 1.0);
    context.latency = 0; // The dry path is immediate.
    context.tail = unsigned(std::ceil(context.sampleRate * 0.5));
}

void reset(State& state)
{
    state.delay.clear();
    state.write = 0;
    state.milliseconds = 120.0f;
    state.mix = 0.25f;
}

VLT_NODE vlt::AudioFrame process(
    State& state, const vlt::Context& context,
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("time") float timeMs = 120.0f,
    VLT_PORT("mix") float mix = 0.25f)
{
    state.milliseconds += state.smoothing *
        (vlt::clamp(timeMs, 1.0f, 500.0f) - state.milliseconds);
    state.mix += state.smoothing *
        (vlt::clamp(mix, 0.0f, 1.0f) - state.mix);
    const unsigned size = state.delay.size();
    state.delay[state.write] = input;
    const float distance = vlt::clamp(
        state.milliseconds * float(context.sampleRate * 0.001),
        1.0f, float(size - 2));
    float position = float(state.write) - distance;
    if (position < 0.0f) position += float(size);
    const unsigned a = unsigned(position);
    const unsigned b = (a + 1) % size;
    const auto wet = vlt::mix(state.delay[a], state.delay[b],
                              position - float(a));
    state.write = (state.write + 1) % size;
    return vlt::mix(input, wet, state.mix);
}
