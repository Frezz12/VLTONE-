#pragma once
#include "Internal/MiniModuleDefinition.hpp"
#include <atomic>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>

namespace daw::plugins::mini {
class CreatorCancellationScope {
public:
  explicit CreatorCancellationScope(const std::atomic<bool> *);
  ~CreatorCancellationScope();

private:
  const std::atomic<bool> *previous;
};
struct CodeDiagnostic {
  std::string node, message, severity = "error";
  unsigned line = 0, column = 0;
};
struct FunctionAnalysis {
  FunctionDefinition function;
  std::vector<CodeDiagnostic> diagnostics;
  std::string error;
  nlohmann::json details() const;
  std::string detailsJson;
  explicit operator bool() const { return error.empty(); }
};
std::filesystem::path creatorToolsDirectory();
bool creatorCompilerAvailable();
FunctionAnalysis analyzeFunction(const FunctionDefinition &,
                                 const std::atomic<bool> *cancel = nullptr);
nlohmann::json
creatorCompilerRequest(const nlohmann::json &,
                       const std::atomic<bool> *cancel = nullptr);
bool compileCppGraph(MiniModuleDefinition &, std::string &error,
                     std::vector<CodeDiagnostic> &diagnostics,
                     const std::atomic<bool> *cancel = nullptr);
bool prepareCodeArtifact(const CodeArtifact &, std::filesystem::path &aot,
                         std::string &error,
                         const std::atomic<bool> *cancel = nullptr);
} // namespace daw::plugins::mini
