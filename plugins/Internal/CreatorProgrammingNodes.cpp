#include "MiniNodeRegistry.hpp"
#include <algorithm>
#include <array>

namespace daw::plugins::mini {
const char *portTypeId(PortType type) noexcept {
  constexpr const char *names[] = {"audio", "number", "gate", "function", "integer", "array", "list", "buffer"};
  return names[unsigned(type)];
}
std::optional<PortType> parsePortType(std::string_view name) noexcept {
  for (unsigned i = 0; i < 8; ++i)
    if (name == portTypeId(PortType(i))) return PortType(i);
  return {};
}
bool isBlockNode(const NodeDefinition &n) noexcept {
  const auto *d = nodeDescription(n.type, n.version);
  return !d || d->operation == Operation::Effect || d->operation == Operation::Reverb ||
         d->operation == Operation::CppFunction || d->operation == Operation::Subgraph;
}
bool isMemoryWrite(const NodeDefinition &n, std::string_view port) noexcept {
  return (n.type == "history" && (port == "next" || port == "write" || port == "reset")) ||
         (n.type == "delay_buffer" && (port == "in" || port == "reset"));
}
void appendProgrammingNodes(std::vector<NodeDescription> &r) {
  using T = PortType;
  using O = Operation;
  const auto add = [&](const char *id, const char *name, const char *category, O op,
                       T output = T::Number) -> NodeDescription & {
    r.push_back({id, name, category, {}, 1, op});
    r.back().outputs.push_back({"out", "Output", output});
    return r.back();
  };
  const auto in = [](NodeDescription &n, const char *id, const char *name, T type, bool required = true) {
    n.inputs.push_back({id, name, type, required});
  };
  const auto value = [](NodeDescription &n, const char *id, const char *name, double lo,
                        double hi, double initial, const char *unit = "", bool log = false,
                        bool mod = true, std::vector<std::string> choices = {}) {
    const int index = int(n.parameters.size());
    n.parameters.push_back({id, name, unit, lo, hi, initial, log, mod, std::move(choices)});
    if (mod) n.inputs.push_back({id, name, T::Number, false, index});
  };
  struct Entry { const char *id, *name; O operation; };
  for (auto e : {Entry{"exp", "Exponential", O::Exp}, {"log", "Natural logarithm", O::Log},
                {"log2", "Logarithm 2", O::Log2}, {"log10", "Logarithm 10", O::Log10},
                {"tanh", "Hyperbolic tangent", O::Tanh}, {"atan", "Arctangent", O::Atan},
                {"sign", "Sign", O::Sign}, {"floor", "Floor", O::Floor}, {"ceil", "Ceiling", O::Ceil},
                {"round", "Round", O::Round}, {"fraction", "Fraction", O::Fraction},
                {"linear_to_db", "Linear to dB", O::LinearToDb}, {"db_to_linear", "dB to Linear", O::DbToLinear}})
    value(add(e.id, e.name, "Math", e.operation), "value", "Value", -1e6, 1e6, 0);
  auto &mod = add("modulo", "Modulo", "Math", O::Modulo);
  value(mod, "a", "A", -1e6, 1e6, 0); value(mod, "b", "B", -1e6, 1e6, 1);
  for (auto e : {Entry{"wrap", "Wrap", O::Wrap}, {"fold", "Fold", O::Fold}, {"smoothstep", "Smoothstep", O::Smoothstep}}) {
    auto &n = add(e.id, e.name, "Math", e.operation);
    value(n, "value", "Value", -1e6, 1e6, 0);
    value(n, "min", "Minimum", -1e6, 1e6, 0); value(n, "max", "Maximum", -1e6, 1e6, 1);
  }
  auto &lerp = add("lerp", "Linear interpolation", "Math", O::Lerp);
  value(lerp, "a", "A", -1e6, 1e6, 0); value(lerp, "b", "B", -1e6, 1e6, 1);
  value(lerp, "mix", "Position", -1e6, 1e6, .5);
  for (auto e : {Entry{"audio_add", "Add Audio", O::AudioAdd}, {"audio_subtract", "Subtract Audio", O::AudioSubtract},
                {"audio_multiply", "Multiply Audio", O::AudioMultiply}}) {
    auto &n = add(e.id, e.name, "Signal", e.operation, T::Audio);
    in(n, "a", "A", T::Audio); in(n, "b", "B", T::Audio);
  }
  auto &scale = add("audio_scale", "Scale Audio", "Signal", O::AudioScale, T::Audio);
  in(scale, "in", "Audio", T::Audio); value(scale, "gain", "Gain", -1e6, 1e6, 1);
  for (auto e : {Entry{"stereo_split", "Split Stereo", O::StereoSplit}, {"ms_encode", "Encode Mid-Side", O::MidSideEncode}}) {
    auto &n = add(e.id, e.name, "Signal", e.operation);
    in(n, "in", "Audio", T::Audio);
    n.outputs = e.operation == O::StereoSplit
      ? std::vector<PortDescription>{{"left", "Left", T::Number}, {"right", "Right", T::Number}}
      : std::vector<PortDescription>{{"mid", "Mid", T::Number}, {"side", "Side", T::Number}};
  }
  for (auto e : {Entry{"stereo_join", "Join Stereo", O::StereoJoin}, {"ms_decode", "Decode Mid-Side", O::MidSideDecode}}) {
    auto &n = add(e.id, e.name, "Signal", e.operation, T::Audio);
    value(n, e.operation == O::StereoJoin ? "left" : "mid", e.operation == O::StereoJoin ? "Left" : "Mid", -1e6, 1e6, 0);
    value(n, e.operation == O::StereoJoin ? "right" : "side", e.operation == O::StereoJoin ? "Right" : "Side", -1e6, 1e6, 0);
  }
  for (auto e : {Entry{"and", "AND", O::And}, {"or", "OR", O::Or}, {"xor", "XOR", O::Xor}}) {
    auto &n = add(e.id, e.name, "Logic", e.operation, T::Gate);
    in(n, "a", "A", T::Gate, false); in(n, "b", "B", T::Gate, false);
  }
  for (auto e : {Entry{"not", "NOT", O::Not}, {"rising_edge", "Rising Edge", O::RisingEdge}, {"falling_edge", "Falling Edge", O::FallingEdge}})
    in(add(e.id, e.name, "Logic", e.operation, T::Gate), "gate", "Gate", T::Gate, false);
  in(add("gate_to_number", "Gate to Number", "Logic", O::GateToNumber), "gate", "Gate", T::Gate, false);
  value(add("number_to_gate", "Number to Gate", "Logic", O::NumberToGate, T::Gate), "value", "Value", -1e6, 1e6, 0);
  value(add("number_to_integer", "Number to Integer", "Math", O::NumberToInteger, T::Integer), "value", "Value", -2147483648., 2147483647., 0);
  in(add("integer_to_number", "Integer to Number", "Math", O::IntegerToNumber), "value", "Value", T::Integer, false);
  auto &sel = add("audio_select", "Select Audio", "Logic", O::AudioSelect, T::Audio);
  in(sel, "gate", "Select B", T::Gate, false); in(sel, "a", "A", T::Audio); in(sel, "b", "B", T::Audio);
  in(add("peak", "Peak Detector", "Analysis", O::Peak), "in", "Audio", T::Audio);
  auto &rms = add("window_rms", "Window RMS", "Analysis", O::WindowRms);
  in(rms, "in", "Audio", T::Audio); value(rms, "window", "Window", .1, 1000, 10, "ms", true, false);
  auto &ar = add("attack_release", "Attack / Release", "Modulation", O::AttackRelease);
  value(ar, "value", "Value", -1e6, 1e6, 0);
  value(ar, "attack", "Attack", .001, 10000, 10, "ms", true); value(ar, "release", "Release", .001, 10000, 100, "ms", true);
  auto &slew = add("slew", "Slew Limiter", "Modulation", O::Slew);
  value(slew, "value", "Value", -1e6, 1e6, 0);
  value(slew, "rise", "Rise / second", 0, 1e6, 10); value(slew, "fall", "Fall / second", 0, 1e6, 10);
  auto &history = add("history", "Variable / History", "Memory", O::History);
  value(history, "initial", "Initial", -1e6, 1e6, 0, "", false, false);
  value(history, "next", "Next", -1e6, 1e6, 0);
  in(history, "write", "Write enable", T::Gate, false); in(history, "reset", "Reset", T::Gate, false);
  auto &acc = add("accumulator", "Accumulator", "Memory", O::Accumulator);
  value(acc, "increment", "Increment", -1e6, 1e6, 1); in(acc, "reset", "Reset", T::Gate, false);
  auto &counter = add("counter", "Counter", "Memory", O::Counter, T::Integer);
  in(counter, "gate", "Trigger", T::Gate, false); in(counter, "reset", "Reset", T::Gate, false);
  auto &context = add("context", "Time / Context", "Memory", O::Context);
  context.outputs = {{"sample_rate", "Sample Rate", T::Number}, {"time", "Time", T::Number},
    {"tempo", "Tempo", T::Number}, {"beat", "Beat", T::Number}, {"playing", "Playing", T::Gate}};
  auto &buffer = add("delay_buffer", "Delay Buffer", "Memory", O::DelayBuffer, T::Buffer);
  in(buffer, "in", "Audio", T::Audio); in(buffer, "reset", "Reset", T::Gate, false);
  value(buffer, "maximum", "Maximum delay", .1, 10000, 1000, "ms", true, false);
  auto &read = add("delay_read", "Read Delay Tap", "Memory", O::DelayRead, T::Audio);
  in(read, "buffer", "Buffer", T::Buffer); value(read, "time", "Delay", 0, 10000, 100, "ms");
  auto &pole = add("one_pole", "One Pole", "Filters", O::OnePole, T::Audio);
  in(pole, "in", "Audio", T::Audio); value(pole, "coefficient", "Pole", -.999999, .999999, .9);
  auto &biquad = add("biquad", "Biquad", "Filters", O::Biquad, T::Audio);
  in(biquad, "in", "Audio", T::Audio); in(biquad, "coefficients", "b0 b1 b2 a1 a2", T::Array);
  auto &coeff = add("biquad_coefficients", "Biquad Coefficients", "Filters", O::BiquadCoefficients, T::Array);
  value(coeff, "frequency", "Frequency", 1, 96000, 1000, "Hz", true);
  value(coeff, "q", "Q", .05, 100, .70710678, "", true); value(coeff, "gain", "Gain", -60, 60, 0, "dB");
  value(coeff, "type", "Filter type", 0, 7, 0, "", false, false,
    {"Low Pass", "High Pass", "Band Pass", "Notch", "All Pass", "Bell", "Low Shelf", "High Shelf"});
  auto &fir = add("fir", "FIR", "Filters", O::Fir, T::Audio);
  in(fir, "in", "Audio", T::Audio); in(fir, "coefficients", "Coefficients", T::Array);
  auto &dc = add("dc_block", "DC Block", "Filters", O::DcBlock, T::Audio);
  in(dc, "in", "Audio", T::Audio); value(dc, "frequency", "Cutoff", .1, 1000, 10, "Hz", true);
  auto &clip = add("hard_clip", "Hard Clip", "Shaping", O::HardClip, T::Audio);
  in(clip, "in", "Audio", T::Audio); value(clip, "limit", "Limit", .000001, 1000, 1);
  value(add("curve", "Transfer Curve", "Shaping", O::Curve), "value", "Value", -1e6, 1e6, 0);
  auto &lookup = add("table_lookup", "Table Lookup", "Shaping", O::TableLookup);
  in(lookup, "table", "Table", T::Array); value(lookup, "position", "Position", 0, 1, 0);
  add("array", "Array", "Collections", O::Array, T::Array);
  add("list", "List", "Collections", O::List, T::List);
  in(add("length", "Length", "Collections", O::Length, T::Integer), "collection", "Collection", T::Array);
  auto &get = add("get", "Get Element", "Collections", O::Get);
  in(get, "collection", "Collection", T::Array); in(get, "index", "Index", T::Integer, false);
  get.outputs.push_back({"valid", "Valid", T::Gate});
  for (auto e : {Entry{"set", "Set Element", O::Set}, {"append", "Append", O::Append},
                {"remove", "Remove Element", O::Remove}, {"clear", "Clear Collection", O::Clear}}) {
    auto &n = add(e.id, e.name, "Collections", e.operation, e.operation == O::Set || e.operation == O::Clear ? T::Array : T::List);
    in(n, "collection", "Collection", n.outputs.front().type);
    if (e.operation == O::Set || e.operation == O::Remove) in(n, "index", "Index", T::Integer, false);
    if (e.operation == O::Set || e.operation == O::Append) value(n, "value", "Value", -1e6, 1e6, 0);
    n.outputs.push_back({"success", "Success", T::Gate});
  }
  for (auto e : {Entry{"sum", "Sum Elements", O::Sum}, {"collection_min", "Minimum Element", O::CollectionMin},
                {"collection_max", "Maximum Element", O::CollectionMax}})
    in(add(e.id, e.name, "Collections", e.operation), "collection", "Collection", T::Array);
  in(add("map", "Map", "Collections", O::Map, T::Array), "collection", "Collection", T::Array);
  auto &reduce = add("reduce", "Reduce", "Collections", O::Reduce);
  in(reduce, "collection", "Collection", T::Array); value(reduce, "initial", "Initial", -1e6, 1e6, 0);
  add("subgraph", "Custom Node", "Custom", O::Subgraph).outputs.clear();
  add("subgraph_input", "Node Input", "Custom", O::SubgraphInput);
  in(add("subgraph_output", "Node Output", "Custom", O::SubgraphOutput), "in", "Value", T::Number, false);
  in(add("wire", "Named Value", "Memory", O::Wire), "in", "Value", T::Number, false);
}
} // namespace daw::plugins::mini
