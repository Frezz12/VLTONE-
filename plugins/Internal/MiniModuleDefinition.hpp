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
inline constexpr unsigned kMaxModules = 3, kMaxNodes = 512, kMaxEdges = 2048;
inline constexpr unsigned kMaxGraphDepth = 8, kMaxCollection = 4096;
inline constexpr std::size_t kMaxGraphStateBytes = 16 * 1024 * 1024;
enum class PortType { Audio, Number, Gate, Function, Integer, Array, List, Buffer };
struct GraphPort {
  std::string id, name, type = "number", unit, signature;
  double minimum = -1e6, maximum = 1e6, initial = 0;
  unsigned capacity = 64;
  bool logarithmic = false;
  bool operator==(const GraphPort &) const = default;
};
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
  std::string subgraph, valueType = "number", port, label, signature;
  // Preserves deterministic random state when a node is moved into/out of a group.
  std::string stateKey;
  unsigned capacity = 64;
  std::vector<double> values;
  bool operator==(const NodeDefinition &other) const {
    // Parameters are keyed JSON properties; their object order is not part of
    // the graph, its sound, or its identity after saving and reopening.
    return id == other.id && type == other.type && version == other.version &&
           function == other.function && subgraph == other.subgraph &&
           valueType == other.valueType && port == other.port &&
           label == other.label && signature == other.signature && stateKey == other.stateKey && capacity == other.capacity && values == other.values &&
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
struct SubgraphDefinition {
  std::string id, name;
  std::vector<GraphPort> inputs, outputs;
  std::vector<NodeDefinition> nodes;
  std::vector<Connection> connections;
  unsigned oversampling = 1;
  bool operator==(const SubgraphDefinition &) const = default;
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
  std::vector<SubgraphDefinition> subgraphs;
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
nlohmann::json subgraphToJson(const SubgraphDefinition &);
SubgraphDefinition subgraphFromJson(const nlohmann::json &);
} // namespace daw::plugins::mini
