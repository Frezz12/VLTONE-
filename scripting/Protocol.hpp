#pragma once
#include <cstdint>
#include <istream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <vector>

namespace vlt::script {
using Json = nlohmann::json;
inline constexpr std::uint32_t protocolVersion = 1;
inline constexpr std::uint32_t maxJsonBytes = 2 * 1024 * 1024;
inline constexpr std::uint32_t maxBinaryBytes = 1024 * 1024;
// Eight byte little-endian header: JSON length followed by binary length.
// PCM travels as binary float32 blocks, never a JSON array or a whole file.
struct Message {
    Json json;
    std::vector<char> binary;
};
std::vector<char> encode(const Message &message);
bool read(std::istream &input, Message &message);
void write(std::ostream &output, const Message &message);
std::uint32_t readSize(const char *bytes);
} // namespace vlt::script
