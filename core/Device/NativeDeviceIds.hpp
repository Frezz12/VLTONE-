#pragma once
#include <string>
#include <vector>
namespace audio {
// Cached alongside PortAudio's enumeration, never called by the audio thread.
std::vector<std::string> nativeDeviceIds();
bool nativeDeviceAlive(int index, void* stream, bool input);
std::string nativeStreamDeviceId(void* stream, bool input);
}
