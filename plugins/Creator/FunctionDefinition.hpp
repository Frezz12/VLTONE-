#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace daw::plugins::mini {
inline constexpr unsigned kCreatorSdkVersion = 1;
inline constexpr unsigned kMaxFunctionPorts = 16;
inline constexpr unsigned kMaxFunctionSourceBytes = 256 * 1024;
inline constexpr unsigned kMaxCodeArtifactBytes = 8 * 1024 * 1024;

// Function connections are compile-time callable bindings, never audio edges.
struct FunctionPort {
  std::string id, name, type = "number", cppType = "float", signature;
  double initial = 0, minimum = -1e6, maximum = 1e6;
  bool operator==(const FunctionPort &) const = default;
};
struct FunctionDefinition {
  unsigned sdkVersion = kCreatorSdkVersion;
  std::string entry = "process", source, analyzedHash, returnType = "float";
  std::string stateType;
  bool hasContext = false, hasPrepare = false, hasReset = false;
  bool aggregateReturn = false;
  std::vector<FunctionPort> inputs, outputs;
  std::vector<std::string>
      arguments; // Port IDs, "$context", or "$state", in C++ order.
  bool operator==(const FunctionDefinition &) const = default;
};
struct CodeArtifact {
  unsigned abi = 1;
  std::string wasm, sourceHash;
  bool operator==(const CodeArtifact &) const = default;
};
nlohmann::json functionToJson(const FunctionDefinition &);
FunctionDefinition functionFromJson(const nlohmann::json &);
std::string callableSignature(const FunctionDefinition &);
std::string functionScaffold(const std::string &name,
                             const std::vector<FunctionPort> &inputs,
                             const std::vector<FunctionPort> &outputs);
bool isCppIdentifier(const std::string &);
} // namespace daw::plugins::mini
