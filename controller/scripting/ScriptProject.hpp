#pragma once
#include "EngineController.hpp"
#include <nlohmann/json.hpp>

namespace daw::scripting {
using Json = nlohmann::json;
Json projectSnapshot(EngineController &, const Json &query = Json::object());
Json applyEdits(EngineController &, const Json &arguments);
Json parameters(EngineController &, const Json &arguments);
Json parameter(EngineController &, const Json &arguments, bool write);
Json deviceCatalog(EngineController &, const Json &arguments);
std::string parameterIdentity(EngineController &, const std::string &track,
                              const std::string &device);
} // namespace daw::scripting
