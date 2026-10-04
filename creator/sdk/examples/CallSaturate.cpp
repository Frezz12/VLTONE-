#include <vlt/creator.hpp>

VLT_NODE vlt::AudioFrame process(
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("drive") float drive = 1.5f,
    VLT_PORT("saturate")
        vlt::Function<vlt::AudioFrame(vlt::AudioFrame, float)> saturate = {})
{
    return saturate(input, vlt::clamp(drive, 0.1f, 6.0f));
}
