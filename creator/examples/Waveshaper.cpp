#include <vlt/creator.hpp>

// Two ordinary numerical inputs can be wired to Interface controls or LFOs.
VLT_NODE vlt::AudioFrame process(VLT_PORT("input") vlt::AudioFrame input,
                                 VLT_PORT("drive") float drive = 1.5f,
                                 VLT_PORT("amount") float amount = .5f) {
  drive = vlt::clamp(drive, .1f, 6.f);
  const auto wet = vlt::tanh(input * drive) * (1.f / drive);
  return vlt::mix(input, wet, vlt::clamp(amount, 0.f, 1.f));
}
