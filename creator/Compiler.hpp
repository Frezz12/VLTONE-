#pragma once
#include "Creator/FunctionDefinition.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>

namespace creator {
using json = nlohmann::json;
using namespace daw::plugins::mini;
struct Analysis {
  FunctionDefinition function;
  json diagnostics = json::array(), details = json::object();
  std::string error;
};
Analysis analyze(FunctionDefinition, const std::filesystem::path &tools,
                 const std::string &node = "node", bool annotate = true);
json extractFunction(const json &, const std::filesystem::path &tools);
json bindFunction(const json &, const std::filesystem::path &tools);
void emitObject(const std::string &, const std::filesystem::path &tools,
                const std::filesystem::path &output, json &diagnostics);
std::vector<std::string> compilerFlags(const std::filesystem::path &tools);
json compile(const json &, const std::filesystem::path &tools,
             const std::filesystem::path &work);
json prepare(const json &, const std::filesystem::path &tools,
             const std::filesystem::path &work);
std::vector<std::uint8_t> instrument(std::vector<std::uint8_t> wasm);
} // namespace creator
