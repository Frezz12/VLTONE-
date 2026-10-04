#include <vlt/creator.hpp>

// This helper can also be moved with Extract function > saturate.
float saturate(float sample, float drive)
{
    drive = vlt::clamp(drive, 0.1f, 6.0f);
    return vlt::tanh(sample * drive) / vlt::tanh(drive);
}

VLT_NODE vlt::AudioFrame process(
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("drive") float drive = 1.5f,
    VLT_PORT("mix") float mix = 0.3f)
{
    const vlt::AudioFrame wet{
        saturate(input.left, drive), saturate(input.right, drive)};
    return vlt::mix(input, wet, vlt::clamp(mix, 0.0f, 1.0f));
}
