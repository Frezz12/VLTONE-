#include "Protocol.hpp"
#include <stdexcept>

namespace vlt::script {
std::uint32_t readSize(const char *b) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= std::uint32_t(static_cast<unsigned char>(b[i])) << (i * 8);
    return v;
}
std::vector<char> encode(const Message &message) {
    const auto json = message.json.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (json.size() > maxJsonBytes || message.binary.size() > maxBinaryBytes)
        throw std::runtime_error("IPC message exceeds limit");
    std::vector<char> bytes(8);
    for (int i = 0; i < 4; ++i) {
        bytes[i] = char((json.size() >> (i * 8)) & 255);
        bytes[4 + i] = char((message.binary.size() >> (i * 8)) & 255);
    }
    bytes.insert(bytes.end(), json.begin(), json.end());
    bytes.insert(bytes.end(), message.binary.begin(), message.binary.end());
    return bytes;
}
bool read(std::istream &input, Message &message) {
    char header[8];
    if (!input.read(header, 8))
        return false;
    const auto jsonSize = readSize(header), binarySize = readSize(header + 4);
    if (!jsonSize || jsonSize > maxJsonBytes || binarySize > maxBinaryBytes)
        throw std::runtime_error("Invalid IPC frame");
    std::string json(jsonSize, '\0');
    message.binary.resize(binarySize);
    if (!input.read(json.data(), json.size()) ||
        (binarySize && !input.read(message.binary.data(), binarySize)))
        throw std::runtime_error("Truncated IPC frame");
    message.json = Json::parse(json);
    if (!message.json.is_object())
        throw std::runtime_error("Expected IPC object");
    return true;
}
void write(std::ostream &output, const Message &message) {
    const auto bytes = encode(message);
    output.write(bytes.data(), bytes.size());
    output.flush();
    if (!output)
        throw std::runtime_error("Host disconnected");
}
} // namespace vlt::script
