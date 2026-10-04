#include <vlt/creator.hpp>

VLT_NODE vlt::AudioFrame process(
    VLT_PORT("input") vlt::AudioFrame input,
    VLT_PORT("gain") float gain = 1.0f)
{
    return input * vlt::clamp(gain, 0.0f, 2.0f);
}
