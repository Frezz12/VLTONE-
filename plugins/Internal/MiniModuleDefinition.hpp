#pragma once
#include <algorithm>
#include "Creator/FunctionDefinition.hpp"
#include <optional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace daw::plugins::mini {
inline constexpr std::string_view kUid = "daw.mini-module";
inline constexpr unsigned kMaxModules = 3, kMaxNodes = 64, kMaxEdges = 128;
struct NodeParameter {
  std::string id;
  double value = 0;
  bool operator==(const NodeParameter &) const = default;
};
struct NodeDefinition {
  std::string id, type;
  unsigned version = 1;
  std::vector<NodeParameter> parameters;
  std::optional<FunctionDefinition> function;
  bool operator==(const NodeDefinition &other) const {
    // Parameters are keyed JSON properties; their object order is not part of
    // the graph, its sound, or its identity after saving and reopening.
    return id == other.id && type == other.type && version == other.version &&
           function == other.function &&
           parameters.size() == other.parameters.size() &&
           std::is_permutation(parameters.begin(), parameters.end(),
                               other.parameters.begin());
  }
};
struct Connection {
  std::string from, to;
  // Empty ports identify the legacy v1/v2 audio-only connection format.
  std::string fromPort, toPort;
  bool operator==(const Connection &) const = default;
};
struct Binding {
  std::string node, parameter;
  double minimum = 0, maximum = 1;
  bool logarithmic = false;
  bool bipolarMagnitude = false;
  bool operator==(const Binding &) const = default;
};
struct Control {
  std::string id, name, unit;
  double minimum = 0, maximum = 1, initial = 0;
  bool logarithmic = false;
  std::vector<Binding> bindings;
  std::string style;
  bool operator==(const Control &) const = default;
};
struct Appearance {
  std::string theme = "studio", controlStyle = "machined";
  std::string backgroundColor, backgroundImage;
  bool operator==(const Appearance &) const = default;
};
struct Mode {
  std::string id, name;
  std::vector<NodeDefinition> nodes;
  std::vector<Connection> connections;
  std::vector<Control> controls;
  CodeArtifact code;
  bool operator==(const Mode &) const = default;
};
struct MiniModuleDefinition {
  std::string id, name;
  unsigned version = 1;
  std::vector<NodeDefinition> nodes;
  std::vector<Connection> connections;
  std::vector<Control> controls;
  Appearance appearance;
  std::vector<Mode> modes;
  std::string defaultMode;
  // Preserve malformed/future documents losslessly, without executing them.
  std::string unavailableSource;
  CodeArtifact code;
  bool operator==(const MiniModuleDefinition &) const = default;
};
struct NodeType {
  std::string_view id;
  unsigned version;
};
const std::vector<NodeType> &nodeTypes();
std::string validate(const MiniModuleDefinition &, std::string_view mode = {});
MiniModuleDefinition resolved(const MiniModuleDefinition &,
                              std::string_view mode = {});
bool sameAudioGraph(const MiniModuleDefinition &, std::string_view,
                    const MiniModuleDefinition &, std::string_view);
bool validControlStyle(std::string_view);
MiniModuleDefinition builtin(std::string_view id);
nlohmann::json toJson(const MiniModuleDefinition &);
MiniModuleDefinition fromJson(const nlohmann::json &);
} // namespace daw::plugins::mini
