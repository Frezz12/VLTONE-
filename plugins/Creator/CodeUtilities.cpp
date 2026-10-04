#include "CodeUtilities.hpp"
#include <array>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

namespace daw::plugins::mini {
std::string codeHash(std::string_view value) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(),
         bytes.data());
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (auto b : bytes) {
    result += hex[b >> 4];
    result += hex[b & 15];
  }
  return result;
}
std::string functionSourceHash(const FunctionDefinition &f) {
  return codeHash(std::to_string(f.sdkVersion) + "\n" + f.entry + "\n" +
                  f.source);
}
nlohmann::json compilationUnit(const MiniModuleDefinition &d) {
  nlohmann::json result{{"functions", nlohmann::json::array()},
                        {"bindings", nlohmann::json::array()}};
  std::vector<bool> scheduled(d.nodes.size());
  for (unsigned i = 0; i < d.nodes.size(); ++i)
    scheduled[i] = d.nodes[i].type == "output";
  for (unsigned pass = 0; pass < d.nodes.size(); ++pass)
    for (const auto &e : d.connections)
      if (e.fromPort != "function") {
        auto a = std::find_if(d.nodes.begin(), d.nodes.end(),
                              [&](const auto &n) { return n.id == e.from; });
        auto b = std::find_if(d.nodes.begin(), d.nodes.end(),
                              [&](const auto &n) { return n.id == e.to; });
        if (a != d.nodes.end() && b != d.nodes.end() &&
            scheduled[b - d.nodes.begin()])
          scheduled[a - d.nodes.begin()] = true;
      }
  for (unsigned i = 0; i < d.nodes.size(); ++i) {
    const auto &n = d.nodes[i];
    if (n.function)
      result["functions"].push_back(
          {{"id", n.id},
           {"index", i},
           {"scheduled", scheduled[i]},
           {"function", functionToJson(*n.function)}});
  }
  for (const auto &e : d.connections)
    if (e.fromPort == "function")
      result["bindings"].push_back(
          {{"from", e.from}, {"to", e.to}, {"port", e.toPort}});
  return result;
}
std::string graphCodeHash(const MiniModuleDefinition &d) {
  return codeHash(compilationUnit(d).dump());
}
std::string encodeCode(std::span<const std::uint8_t> bytes) {
  constexpr char chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const unsigned n =
        unsigned(bytes[i]) << 16 |
        (i + 1 < bytes.size() ? unsigned(bytes[i + 1]) << 8 : 0) |
        (i + 2 < bytes.size() ? bytes[i + 2] : 0);
    result += chars[n >> 18];
    result += chars[(n >> 12) & 63];
    result += i + 1 < bytes.size() ? chars[(n >> 6) & 63] : '=';
    result += i + 2 < bytes.size() ? chars[n & 63] : '=';
  }
  return result;
}
std::vector<std::uint8_t> decodeCode(std::string_view text) {
  if (text.size() % 4 || text.size() > kMaxCodeArtifactBytes * 2)
    return {};
  const auto digit = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  };
  std::vector<std::uint8_t> result;
  result.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const int a = digit(text[i]), b = digit(text[i + 1]);
    const int c = text[i + 2] == '=' ? 0 : digit(text[i + 2]);
    const int d = text[i + 3] == '=' ? 0 : digit(text[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0 ||
        (text[i + 2] == '=' && text[i + 3] != '=') ||
        ((text[i + 2] == '=' || text[i + 3] == '=') && i + 4 != text.size()))
      return {};
    result.push_back(std::uint8_t((a << 2) | (b >> 4)));
    if (text[i + 2] != '=')
      result.push_back(std::uint8_t((b << 4) | (c >> 2)));
    if (text[i + 3] != '=')
      result.push_back(std::uint8_t((c << 6) | d));
  }
  return result;
}
} // namespace daw::plugins::mini
