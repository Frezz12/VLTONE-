#pragma once
#include "MiniModuleDefinition.hpp"
#include <optional>
#include <span>

namespace daw::plugins::mini {
enum class Operation {
  Input,
  Output,
  Interface,
  Constant,
  Gain,
  Mix,
  Add,
  Subtract,
  Multiply,
  Divide,
  Minimum,
  Maximum,
  Abs,
  Negate,
  Power,
  Sqrt,
  Sin,
  Cos,
  Clamp,
  MapRange,
  Compare,
  Select,
  Smooth,
  SampleHold,
  Lfo,
  Random,
  Envelope,
  AudioToNumber,
  NumberToAudio,
  Oscillator,
  Noise,
  Effect,
  Reverb,
  CppFunction,
  Wire, History, Accumulator, Counter, AudioAdd, AudioSubtract, AudioMultiply,
  AudioScale, StereoSplit, StereoJoin, MidSideEncode, MidSideDecode,
  Exp, Log, Log2, Log10, Tanh, Atan, Sign, Floor, Ceil, Round, Fraction,
  Modulo, Wrap, Fold, Lerp, Smoothstep, LinearToDb, DbToLinear,
  And, Or, Xor, Not, RisingEdge, FallingEdge, GateToNumber, NumberToGate,
  NumberToInteger, IntegerToNumber, AudioSelect, Peak, WindowRms, AttackRelease,
  Slew, Context, DelayBuffer, DelayRead, OnePole, Biquad, BiquadCoefficients,
  Fir, DcBlock, HardClip, Curve, TableLookup,
  Array, List, Length, Get, Set, Append, Remove, Clear, Sum, CollectionMin,
  CollectionMax, Map, Reduce, Subgraph, SubgraphInput, SubgraphOutput
};
struct PortDescription {
  std::string id, name;
  PortType type = PortType::Number;
  bool required = false;
  // Index of the manual fallback value, or -1 for audio/gate inputs.
  int parameter = -1;
  std::string signature;
  unsigned capacity = 0;
};
struct NodeParameterDescription {
  std::string id, name, unit;
  double minimum = 0, maximum = 1, initial = 0;
  bool logarithmic = false, modulatable = true;
  std::vector<std::string> choices;
  int nativeIndex = -1;
};
struct NodeDescription {
  std::string id, name, category, description;
  unsigned version = 1;
  Operation operation = Operation::Constant;
  std::vector<PortDescription> inputs, outputs;
  std::vector<NodeParameterDescription> parameters;
};
std::span<const NodeDescription> nodeRegistry();
const NodeDescription *nodeDescription(std::string_view id,
                                       unsigned version = 1);
// Dynamic descriptions are values: no global mutation/cache from editor threads.
NodeDescription describeNode(const NodeDefinition &, const MiniModuleDefinition * = nullptr);
std::vector<PortDescription> inputPorts(const NodeDefinition &, const MiniModuleDefinition * = nullptr);
bool compatiblePorts(const PortDescription &, const PortDescription &);
std::vector<PortDescription> outputPorts(const NodeDefinition &,
                                         const MiniModuleDefinition &);
std::string validateTypedGraph(const MiniModuleDefinition &, bool nested = false);
// Includes the sink and its dependencies. Unused editor nodes are not rendered.
std::vector<bool> reachableNodes(const MiniModuleDefinition &);
NodeDefinition makeNode(std::string_view type, std::string id);
const char *portTypeName(PortType) noexcept;
const char *portTypeId(PortType) noexcept;
std::optional<PortType> parsePortType(std::string_view) noexcept;
void appendProgrammingNodes(std::vector<NodeDescription> &);
bool isBlockNode(const NodeDefinition &) noexcept;
bool isMemoryWrite(const NodeDefinition &, std::string_view port) noexcept;
// Expands ordinary groups; oversampled groups remain explicit clock domains.
bool expandSubgraphs(const MiniModuleDefinition &, MiniModuleDefinition &, std::string &);
std::string validateProgrammingGraph(const MiniModuleDefinition &);
bool programmingIslands(const MiniModuleDefinition &, std::vector<std::vector<unsigned>> &, std::string &);
} // namespace daw::plugins::mini
