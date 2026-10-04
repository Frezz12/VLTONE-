#include <vlt/creator.hpp>

VLT_NODE vlt::AudioFrame process(
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("cutoff") float cutoff = 500.0f,
    VLT_PORT("mix") float mix = 0.5f,
    VLT_PORT("filter_a")
        vlt::Function<vlt::AudioFrame(vlt::AudioFrame, float)> filterA = {},
    VLT_PORT("filter_b")
        vlt::Function<vlt::AudioFrame(vlt::AudioFrame, float)> filterB = {})
{
    // Connect the SAME OnePole function output to both function inputs.
    // Each input owns independent filter history.
    const auto low = filterA(input, cutoff);
    const auto high = filterB(input, cutoff * 4.0f);
    return vlt::mix(low, high, vlt::clamp(mix, 0.0f, 1.0f));
}
