#pragma once
#include "Protocol.hpp"
#include <string_view>

namespace vlt::script {
// The same contract is used by runtime bindings, host validation, SDK and AI.
// Permissions describe actual host operations, never author-supplied risk tags.
const Json &apiCatalog();
const Json *apiMethod(std::string_view name);
std::string validateArguments(const Json &method, const Json &arguments);
std::string validateManifest(const Json &manifest);
bool safeRelativePath(std::string_view path);
} // namespace vlt::script
