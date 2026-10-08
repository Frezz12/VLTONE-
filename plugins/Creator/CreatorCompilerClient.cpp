#include "CreatorCompilerClient.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "CodeUtilities.hpp"
#include "CompilerProcess.hpp"
#include <cstdlib>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace daw::plugins::mini {
using json = nlohmann::json;
static thread_local const std::atomic<bool> *activeCancellation = nullptr;
CreatorCancellationScope::CreatorCancellationScope(
    const std::atomic<bool> *value)
    : previous(activeCancellation) {
  activeCancellation = value;
}
CreatorCancellationScope::~CreatorCancellationScope() {
  activeCancellation = previous;
}
std::filesystem::path creatorToolsDirectory() {
  if (const auto *override = std::getenv("VLT_CREATOR_TOOLS"))
    return fromUtf8(override);
  return creatorExecutableDirectory() / "CreatorTools";
}
static std::filesystem::path helper() {
#ifdef _WIN32
  return creatorToolsDirectory() / "daw_creator_compiler.exe";
#else
  return creatorToolsDirectory() / "daw_creator_compiler";
#endif
}
bool creatorCompilerAvailable() {
  return std::filesystem::is_regular_file(helper());
}
json FunctionAnalysis::details() const {
  return json::parse(detailsJson, nullptr, false);
}
json creatorCompilerRequest(const json &request,
                            const std::atomic<bool> *cancel) {
  if (!cancel)
    cancel = activeCancellation;
  std::filesystem::path job;
  try {
    if (!creatorCompilerAvailable())
      return {{"ok", false},
              {"error", "Creator compiler is missing. Repair the application "
                        "installation."}};
    job = creatorJobDirectory();
    auto input = request;
    input["tools"] = pathUtf8(creatorToolsDirectory());
    if (!writeCreatorText(job / "request.json", input.dump()))
      throw std::runtime_error("Cannot write compiler request");
    std::string error;
    const bool ran = runCreatorProcess(
        helper(),
        {pathUtf8(job / "request.json"), pathUtf8(job / "response.json")},
        job / "log.txt", 120000, cancel, error);
    auto reply =
        json::parse(readCreatorText(job / "response.json", 32 * 1024 * 1024),
                    nullptr, false);
    if (!ran || !reply.is_object())
      reply = {{"ok", false},
               {"error", error.empty() ? "Invalid compiler response" : error}};
    std::error_code ignored;
    std::filesystem::remove_all(job, ignored);
    return reply;
  } catch (const std::exception &e) {
    if (!job.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(job, ignored);
    }
    return {{"ok", false}, {"error", e.what()}};
  }
}
static std::vector<CodeDiagnostic> diagnostics(const json &j) {
  std::vector<CodeDiagnostic> result;
  for (const auto &d : j.value("diagnostics", json::array()))
    result.push_back({d.value("node", ""), d.value("message", ""),
                      d.value("severity", "error"), d.value("line", 0u),
                      d.value("column", 0u)});
  return result;
}
FunctionAnalysis analyzeFunction(const FunctionDefinition &f,
                                 const std::atomic<bool> *cancel) {
  FunctionAnalysis result;
  const auto reply = creatorCompilerRequest(
      {{"action", "analyze"}, {"function", functionToJson(f)}}, cancel);
  result.diagnostics = diagnostics(reply);
  result.detailsJson = reply.value("details", json::object()).dump();
  if (!reply.value("ok", false))
    result.error = reply.value("error", "C++ analysis failed");
  else {
    try {
      result.function = functionFromJson(reply.at("function"));
    } catch (const std::exception &e) {
      result.error = e.what();
    }
  }
  return result;
}
bool compileCppGraph(MiniModuleDefinition &d, std::string &error,
                     std::vector<CodeDiagnostic> &out,
                     const std::atomic<bool> *cancel) {
  MiniModuleDefinition expanded;
  if (!expandSubgraphs(d, expanded, error)) return false;
  if (std::none_of(expanded.nodes.begin(), expanded.nodes.end(),
                   [](const auto &n) { return n.function.has_value(); })) {
    d.code = {};
    return true;
  }
  for (const auto &n : expanded.nodes)
    if (n.function &&
        n.function->analyzedHash != functionSourceHash(*n.function)) {
      error = "Update C++ function ports: " + n.id;
      return false;
    }
  auto request = compilationUnit(expanded);
  request["action"] = "compile";
  const auto reply = creatorCompilerRequest(request, cancel);
  out = diagnostics(reply);
  if (!reply.value("ok", false)) {
    error = reply.value("error", "C++ compilation failed");
    return false;
  }
  d.version = std::max(4u, d.version);
  expanded.version = d.version;
  d.code = {1, reply.at("wasm").get<std::string>(), graphCodeHash(expanded)};
  return true;
}
bool prepareCodeArtifact(const CodeArtifact &artifact,
                         std::filesystem::path &aot, std::string &error,
                         const std::atomic<bool> *cancel) {
  // Only locally generated AOT enters the runtime. Module files contain Wasm.
  static std::mutex cacheMutex;
  static std::map<std::string, std::pair<std::filesystem::path, std::string>>
      cache;
  std::lock_guard lock(cacheMutex);
  if (artifact.abi != 1 || artifact.wasm.empty()) {
    error = "Unavailable C++ code artifact";
    return false;
  }
  const auto key = codeHash(pathUtf8(creatorToolsDirectory()) + artifact.wasm);
  if (auto entry = cache.find(key); entry != cache.end()) {
    const auto contents =
        readCreatorText(entry->second.first, kMaxCodeArtifactBytes * 4);
    if (!contents.empty() && codeHash(contents) == entry->second.second) {
      aot = entry->second.first;
      return true;
    }
    cache.erase(entry);
  }
  const auto reply = creatorCompilerRequest(
      {{"action", "prepare"}, {"wasm", artifact.wasm}}, cancel);
  if (!reply.value("ok", false)) {
    error = reply.value("error", "C++ preparation failed");
    return false;
  }
  aot = fromUtf8(reply.at("aot").get<std::string>());
  const auto digest = reply.at("digest").get<std::string>();
  const auto contents = readCreatorText(aot, kMaxCodeArtifactBytes * 4);
  if (contents.empty() || digest != codeHash(contents)) {
    error = "Invalid local Creator code cache";
    return false;
  }
  cache[key] = {aot, digest};
  return true;
}
} // namespace daw::plugins::mini
