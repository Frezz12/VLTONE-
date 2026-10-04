#include "FunctionDefinition.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace daw::plugins::mini {
using json = nlohmann::json;
bool isCppIdentifier(const std::string &s) {
  if (s.empty() || s.size() > 128 ||
      !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
    return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  });
}
json functionToJson(const FunctionDefinition &f) {
  const auto ports = [](const auto &list) {
    json result = json::array();
    for (const auto &p : list)
      result.push_back({{"id", p.id},
                        {"name", p.name},
                        {"type", p.type},
                        {"cppType", p.cppType},
                        {"signature", p.signature},
                        {"default", p.initial},
                        {"min", p.minimum},
                        {"max", p.maximum}});
    return result;
  };
  return {{"sdk", f.sdkVersion},        {"entry", f.entry},
          {"source", f.source},         {"analyzedHash", f.analyzedHash},
          {"returnType", f.returnType}, {"stateType", f.stateType},
          {"context", f.hasContext},    {"prepare", f.hasPrepare},
          {"reset", f.hasReset},        {"aggregateReturn", f.aggregateReturn},
          {"inputs", ports(f.inputs)},  {"outputs", ports(f.outputs)},
          {"arguments", f.arguments}};
}
FunctionDefinition functionFromJson(const json &j) {
  FunctionDefinition f;
  f.sdkVersion = j.at("sdk").get<unsigned>();
  // The enclosing module reader preserves the entire original JSON when this
  // SDK cannot interpret it. Do not drop fields introduced by a future SDK.
  if (f.sdkVersion != kCreatorSdkVersion)
    throw std::runtime_error("Unavailable C++ function SDK");
  f.entry = j.at("entry").get<std::string>();
  f.source = j.at("source").get<std::string>();
  f.analyzedHash = j.value("analyzedHash", std::string{});
  f.returnType = j.value("returnType", "float");
  f.stateType = j.value("stateType", std::string{});
  f.hasContext = j.value("context", false);
  f.hasPrepare = j.value("prepare", false);
  f.hasReset = j.value("reset", false);
  f.aggregateReturn = j.value("aggregateReturn", false);
  if (f.source.size() > kMaxFunctionSourceBytes || f.entry.size() > 128 ||
      f.returnType.size() > 512 || f.stateType.size() > 128 ||
      f.analyzedHash.size() > 128)
    throw std::runtime_error("C++ function exceeds its size limit");
  const auto ports = [](const json &v) {
    if (!v.is_array() || v.size() > kMaxFunctionPorts)
      throw std::runtime_error("Too many C++ function ports");
    std::vector<FunctionPort> result;
    std::set<std::string> ids;
    for (const auto &p : v) {
      FunctionPort port{p.at("id").get<std::string>(),
                        p.at("name").get<std::string>(),
                        p.at("type").get<std::string>(),
                        p.at("cppType").get<std::string>(),
                        p.value("signature", std::string{}),
                        p.value("default", 0.),
                        p.value("min", -1e6),
                        p.value("max", 1e6)};
      if (port.id.empty() || port.id.size() > 128 || port.name.size() > 128 ||
          port.cppType.size() > 1024 || port.signature.size() > 2048 ||
          !ids.insert(port.id).second || !std::isfinite(port.initial) ||
          !std::isfinite(port.minimum) || !std::isfinite(port.maximum) ||
          port.minimum > port.maximum || port.initial < port.minimum ||
          port.initial > port.maximum)
        throw std::runtime_error("Invalid C++ function port");
      result.push_back(std::move(port));
    }
    return result;
  };
  f.inputs = ports(j.at("inputs"));
  f.outputs = ports(j.at("outputs"));
  f.arguments = j.value("arguments", std::vector<std::string>{});
  if (f.arguments.size() > kMaxFunctionPorts + 2)
    throw std::runtime_error("Too many C++ function arguments");
  return f;
}
std::string callableSignature(const FunctionDefinition &f) {
  std::string result = f.aggregateReturn ? "struct:" : "value:";
  for (const auto &p : f.outputs)
    result += p.type + ";";
  result += "(";
  for (const auto &p : f.inputs)
    if (p.type != "function")
      result += p.type + ";";
  return result + ")";
}
std::string functionScaffold(const std::string &name,
                             const std::vector<FunctionPort> &inputs,
                             const std::vector<FunctionPort> &outputs) {
  if (!isCppIdentifier(name) || outputs.empty())
    return {};
  std::string text = "#include <vlt/creator.hpp>\n\n";
  std::string resultType = outputs.front().cppType;
  if (outputs.size() > 1) {
    resultType = "Outputs";
    text += "struct Outputs {\n";
    for (const auto &p : outputs)
      text +=
          "    VLT_PORT(\"" + p.id + "\") " + p.cppType + " " + p.name + ";\n";
    text += "};\n\n";
  }
  text += "VLT_NODE " + resultType + " " + name + "(";
  if (!inputs.empty())
    text += "\n";
  for (unsigned i = 0; i < inputs.size(); ++i) {
    const auto &p = inputs[i];
    if (i)
      text += ",\n";
    text += "    VLT_PORT(\"" + p.id + "\") " + p.cppType + " " + p.name;
    if (p.type == "number")
      text += " = " + std::to_string(p.initial) + "f";
    if (p.type == "gate")
      text += p.initial != 0 ? " = true" : " = false";
    if (p.type == "audio" || p.type == "function")
      text += " = {}";
  }
  text += inputs.empty() ? ") {\n    return " : "\n) {\n    return ";
  const auto expression = [&](const auto &out) {
    for (const auto &in : inputs)
      if (in.type == out.type)
        return in.name;
    return out.cppType + "{}";
  };
  if (outputs.size() == 1)
    text += expression(outputs.front());
  else {
    text += "{";
    for (unsigned i = 0; i < outputs.size(); ++i) {
      if (i)
        text += ", ";
      text += expression(outputs[i]);
    }
    text += "}";
  }
  return text + ";\n}\n";
}
} // namespace daw::plugins::mini
