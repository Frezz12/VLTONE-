#pragma once

#include "AudioSessionSpec.hpp"

namespace daw {
/// Reject malformed schedules before mutating a live content publication.
void validateAudioContent(const AudioContentSpec& content);
} // namespace daw
