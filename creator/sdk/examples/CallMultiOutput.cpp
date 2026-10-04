#include <vlt/creator.hpp>

struct Levels {
    vlt::AudioFrame audio;
    float envelope;
    bool gate;
};

VLT_NODE vlt::AudioFrame process(
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("threshold") float threshold = 0.1f,
    VLT_PORT("analyze")
        vlt::Function<Levels(vlt::AudioFrame, float)> analyze = {})
{
    // One call computes all three fields. Do not call analyze again per field.
    const auto result = analyze(input, threshold);
    return result.audio * (1.0f / (1.0f + result.envelope));
}
