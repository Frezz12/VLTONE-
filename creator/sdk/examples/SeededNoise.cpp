#include <vlt/creator.hpp>

struct State {
    vlt::Random random;
};

void prepare(State& state, vlt::PrepareContext& context)
{
    state.random.seed(context.seed);
}

VLT_NODE vlt::AudioFrame process(
    State& state, VLT_PORT("level") float level = 0.03f)
{
    level = vlt::clamp(level, 0.0f, 0.2f);
    return {(state.random.next() * 2.0f - 1.0f) * level,
            (state.random.next() * 2.0f - 1.0f) * level};
}
