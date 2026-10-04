#include <vlt/creator.hpp>

struct State {
    vlt::Buffer<vlt::AudioFrame> delay;
    unsigned write = 0;
    double phase = 0.0;
    float smoothing = 0.0f;
    float amount = 35.0f;
    float rate = 0.25f;
};

void prepare(State& state, vlt::PrepareContext& context)
{
    state.delay.prepare(unsigned(std::ceil(context.sampleRate * 0.022)) + 2);
    state.smoothing = 1.0f / float(context.sampleRate * 0.02 + 1.0);
    context.latency = 0;
    context.tail = unsigned(std::ceil(context.sampleRate * 0.018));
}

void reset(State& state)
{
    state.delay.clear();
    state.write = 0;
    state.phase = 0.0;
    state.amount = 35.0f;
    state.rate = 0.25f;
}

VLT_NODE vlt::AudioFrame process(
    State& state, const vlt::Context& context,
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("amount") float amount = 35.0f,
    VLT_PORT("rate") float rate = 0.25f)
{
    constexpr double twoPi = 6.283185307179586;
    state.amount += state.smoothing *
        (vlt::clamp(amount, 0.0f, 100.0f) - state.amount);
    state.rate += state.smoothing *
        (vlt::clamp(rate, 0.05f, 1.5f) - state.rate);
    state.delay[state.write] = input;
    const unsigned size = state.delay.size();
    float wet[2]{};
    for (unsigned channel = 0; channel < 2; ++channel) {
        const float lfo = vlt::sin(float(state.phase) +
                                   float(channel) * 1.5707963268f);
        const float distance = vlt::clamp(
            float(context.sampleRate) * (0.014f + 0.004f * lfo),
            1.0f, float(size - 2));
        float position = float(state.write) - distance;
        if (position < 0.0f) position += float(size);
        const unsigned a = unsigned(position), b = (a + 1) % size;
        const float x = channel == 0 ? state.delay[a].left : state.delay[a].right;
        const float y = channel == 0 ? state.delay[b].left : state.delay[b].right;
        wet[channel] = vlt::mix(x, y, position - float(a));
    }
    state.write = (state.write + 1) % size;
    state.phase += twoPi * double(state.rate) / context.sampleRate;
    if (state.phase >= twoPi) state.phase -= twoPi;
    return vlt::mix(input, vlt::AudioFrame{wet[0], wet[1]}, state.amount * 0.005f);
}
