#include <vlt/creator.hpp>

// Open this source in a function node, then Extract function > saturate.
// Creator preserves both calls and supplies an amber Function connection.
float saturate(float value, float drive) {
  return vlt::tanh(value * drive) / drive;
}

VLT_NODE vlt::AudioFrame process(VLT_PORT("input") vlt::AudioFrame input,
                                 VLT_PORT("drive") float drive = 1.5f) {
  drive = vlt::clamp(drive, .1f, 6.f);
  return {saturate(input.left, drive), saturate(input.right, drive)};
}
