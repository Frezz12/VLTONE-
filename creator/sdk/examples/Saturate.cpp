#include <vlt/creator.hpp>

VLT_NODE vlt::AudioFrame saturate(
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("drive") float drive = 1.5f)
{
    drive = vlt::clamp(drive, 0.1f, 6.0f);
    return vlt::tanh(input * drive) * (1.0f / vlt::tanh(drive));
}
