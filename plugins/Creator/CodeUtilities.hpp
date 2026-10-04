#pragma once
#include "Internal/MiniModuleDefinition.hpp"
#include <cstdint>
#include <span>

namespace daw::plugins::mini {
std::string codeHash(std::string_view);
std::string functionSourceHash(const FunctionDefinition &);
std::string graphCodeHash(const MiniModuleDefinition &);
std::string encodeCode(std::span<const std::uint8_t>);
std::vector<std::uint8_t> decodeCode(std::string_view);
nlohmann::json compilationUnit(const MiniModuleDefinition &);
} // namespace daw::plugins::mini
