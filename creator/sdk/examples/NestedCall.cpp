#include <vlt/creator.hpp>

VLT_NODE vlt::AudioFrame process(
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("drive") float drive = 1.5f,
    VLT_PORT("mix") float mix = 0.3f,
    VLT_PORT("processor")
        vlt::Function<vlt::AudioFrame(vlt::AudioFrame, float)> processor = {})
{
    // processor may itself depend on another connected Function.
    const auto wet = processor(input, drive);
    return vlt::mix(input, wet, vlt::clamp(mix, 0.0f, 1.0f));
}
