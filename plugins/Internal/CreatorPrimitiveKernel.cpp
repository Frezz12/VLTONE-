#include "CreatorPrimitiveKernel.hpp"
#include "DSP/HalfBandFir.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>

namespace daw::plugins::mini {
bool reserveGraphMemory(std::size_t bytes, std::size_t &used, std::string &error) {
  if (bytes > kMaxGraphStateBytes || used > kMaxGraphStateBytes - bytes) {
    error = "Creator graph exceeds 16 MiB of prepared DSP memory";
    return false;
  }
  used += bytes;
  return true;
}
struct DelayMemory {
  struct Sample { std::array<double, 2> value{}; std::uint64_t generation = 0; };
  std::vector<Sample> samples;
  std::uint64_t generation = 1;
  unsigned write = 0, maximum = 1, block = 1;
  std::array<double, 2> read(unsigned at, double delay, std::uint64_t epoch) const noexcept {
    delay = std::clamp(delay, 1., double(maximum));
    const unsigned integer = unsigned(delay);
    const auto a = (at + samples.size() - integer) % samples.size();
    const auto b = (a + samples.size() - 1) % samples.size();
    const auto left = samples[a].generation == epoch ? samples[a].value : std::array<double,2>{};
    const auto right = samples[b].generation == epoch ? samples[b].value : std::array<double,2>{};
    return {std::lerp(left[0], right[0], delay - integer), std::lerp(left[1], right[1], delay - integer)};
  }
};
namespace {
constexpr double pi = std::numbers::pi;
bool collection(PortType t) { return t == PortType::Array || t == PortType::List; }
bool continuous(PortType t) { return t == PortType::Audio || t == PortType::Number; }
PrimitiveValue scalar(double x) noexcept { return {x, x}; }
double flush(double x) noexcept { return std::abs(x) < 1e-24 ? 0. : x; }
double blep(double p, double step) noexcept {
  if (p < step) { auto t = p / step; return t + t - t * t - 1; }
  if (p > 1 - step) { auto t = (p - 1) / step; return t * t + t + t + 1; }
  return 0;
}
struct StoredValue {
  PrimitiveValue value;
  PortType type = PortType::Number;
  std::vector<double> data;
  void assign(const PrimitiveValue &source) noexcept {
    if (collection(type)) {
      value.size = std::min(unsigned(data.size()), source.size);
      if (value.size && source.elements) std::copy_n(source.elements, value.size, data.data());
      value.elements = data.data(); value.capacity = unsigned(data.size());
    } else value = source;
  }
};
struct Input {
  const PrimitiveValue *source = nullptr;
  const PrimitiveValue *undelayed = nullptr;
  PrimitiveValue fallback;
  std::vector<StoredValue> compensation;
  unsigned cursor = 0;
  int parameter = -1;
  PrimitiveValue get() const noexcept { return source ? *source : fallback; }
};
struct Resampler {
  std::array<engine::dsp::HalfBandFir65, 2> first, second;
};
struct Atom {
  NodeDefinition model;
  NodeDescription desc;
  std::vector<Input> inputs;
  std::vector<StoredValue> outputs;
  std::vector<double> parameters;
  std::vector<int> parameterInput;
  std::array<double, 8> state{};
  std::vector<std::array<double, 2>> ring;
  std::vector<double> history;
  std::unique_ptr<DelayMemory> delay;
  std::vector<std::unique_ptr<CreatorPrimitiveKernel>> children;
  std::vector<std::pair<unsigned, unsigned>> childInputs, childOutputs;
  std::vector<Resampler> up, down;
  std::vector<std::array<double, 2>> at2, down4;
  std::vector<std::vector<StoredValue>> outputAlignment;
  std::vector<unsigned> outputCursors;
  std::vector<PrimitiveValue> fractionalPrevious;
  double fractionalDelay = 0;
  unsigned cursor = 0, historySize = 0, factor = 1;
  unsigned latency = 0;
  std::uint64_t rng = 1, initialRng = 1;
  bool gate = false;
  double phase = 0, triangle = 0, randomFrom = 0, randomTo = 0;
  double p(unsigned i) const noexcept {
    const int input = parameterInput[i];
    return input < 0 || !inputs[input].source ? parameters[i]
      : std::clamp(inputs[input].get().left, desc.parameters[i].minimum, desc.parameters[i].maximum);
  }
  PrimitiveValue v(unsigned i) const noexcept { return inputs[i].get(); }
  double x(unsigned i) const noexcept { return v(i).left; }
  void out(double x, unsigned port = 0) noexcept { outputs[port].value = scalar(x); }
  void audio(double l, double r, unsigned port = 0) noexcept { outputs[port].value = {l, r}; }
  double random() noexcept {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return double((rng * 2685821657736338717ULL) >> 11) * (2. / 9007199254740992.) - 1;
  }
  bool childTick(unsigned index, const PrimitiveContext &ctx) noexcept;
  bool tick(const PrimitiveContext &ctx) noexcept;
  void commit() noexcept;
  void reset() noexcept;
};
}
struct CreatorPrimitiveKernel::Impl {
  std::vector<Atom> atoms;
  std::vector<unsigned> order;
  std::vector<PrimitiveInput> imports;
  std::vector<PrimitiveOutput> exports;
  int failed = -1;
  std::atomic<int> diagnostic{-1};
  bool knownTail = true;
  unsigned tail = 0, latency = 0;
};
CreatorPrimitiveKernel::CreatorPrimitiveKernel() : m(std::make_unique<Impl>()) {}
CreatorPrimitiveKernel::~CreatorPrimitiveKernel() = default;
std::span<PrimitiveInput> CreatorPrimitiveKernel::inputs() { return m->imports; }
std::span<const PrimitiveOutput> CreatorPrimitiveKernel::outputs() const { return m->exports; }
bool CreatorPrimitiveKernel::prepare(const MiniModuleDefinition &d, std::span<const unsigned> selected,
    double rate, unsigned maxFrames, unsigned channels, std::uint64_t seed, std::size_t &memory,
    std::string &error, unsigned depth) {
  if (depth > kMaxGraphDepth) { error = "Nested DSP exceeds eight levels"; return false; }
  m = std::make_unique<Impl>();
  std::map<std::string, unsigned> local;
  m->atoms.resize(selected.size());
  if (!reserveGraphMemory(m->atoms.size() * sizeof(Atom), memory, error)) return false;
  for (unsigned i = 0; i < selected.size(); ++i) {
    auto &a = m->atoms[i]; a.model = d.nodes[selected[i]]; a.desc = describeNode(a.model, &d);
    if (a.model.type == "interface") a.desc.outputs = outputPorts(a.model, d);
    if (a.model.type == "output") a.desc.outputs = {{"out", "Output", PortType::Audio}};
    local[a.model.id] = i;
    a.inputs.resize(a.desc.inputs.size()); a.outputs.resize(a.desc.outputs.size());
    if (!reserveGraphMemory(a.inputs.size() * sizeof(Input) + a.outputs.size() * sizeof(StoredValue) +
                            a.desc.parameters.size() * (sizeof(double) + sizeof(int)), memory, error)) return false;
    a.initialRng = seed;
    for (unsigned char c : a.model.stateKey.empty() ? a.model.id : a.model.stateKey) a.initialRng = (a.initialRng ^ c) * 1099511628211ULL;
    for (const auto &p : a.desc.parameters) { a.parameters.push_back(p.initial); a.parameterInput.push_back(-1); }
    for (const auto &p : a.model.parameters)
      for (unsigned k = 0; k < a.desc.parameters.size(); ++k) if (a.desc.parameters[k].id == p.id) a.parameters[k] = p.value;
    for (unsigned k = 0; k < a.inputs.size(); ++k) {
      auto &input = a.inputs[k]; input.parameter = a.desc.inputs[k].parameter;
      if (input.parameter >= 0) {
        a.parameterInput[input.parameter] = int(k);
        input.fallback = scalar(a.parameters[input.parameter]);
      }
      if (a.model.type == "history" && a.desc.inputs[k].id == "write") input.fallback = scalar(1);
    }
    for (unsigned k = 0; k < a.outputs.size(); ++k) a.outputs[k].type = a.desc.outputs[k].type;
  }
  // Compute collection capacities before binding addresses or allocating state.
  std::map<std::pair<std::string, std::string>, unsigned> capacities;
  for (const auto &node : d.nodes) for (const auto &p : outputPorts(node, d))
    if (collection(p.type)) capacities[{node.id, p.id}] = p.capacity ? p.capacity : node.capacity;
  for (unsigned pass = 0; pass < d.nodes.size(); ++pass) {
    bool changed = false;
    for (const auto &n : d.nodes) if (n.type == "set" || n.type == "append" || n.type == "remove" || n.type == "clear" || n.type == "map" || n.type == "wire")
      for (const auto &e : d.connections) if (e.to == n.id && (e.toPort == "collection" || e.toPort == "in")) {
        auto source = capacities.find({e.from, e.fromPort});
        if (source != capacities.end() && capacities[{n.id, "out"}] != source->second) {
          capacities[{n.id, "out"}] = source->second; changed = true;
        }
      }
    if (!changed) break;
  }
  std::vector<std::vector<unsigned>> next(m->atoms.size());
  std::vector<unsigned> degree(m->atoms.size());
  struct Link { unsigned atom, input, import; };
  std::vector<Link> imports;
  for (unsigned i = 0; i < m->atoms.size(); ++i) {
    auto &a = m->atoms[i];
    for (unsigned p = 0; p < a.outputs.size(); ++p) {
      auto &out = a.outputs[p];
      if (collection(out.type)) {
        const unsigned capacity = capacities[{a.model.id, a.desc.outputs[p].id}];
        if (!capacity || capacity > kMaxCollection || !reserveGraphMemory(capacity * sizeof(double), memory, error)) return false;
        out.data.resize(capacity); out.value.elements = out.data.data(); out.value.capacity = capacity;
      }
      m->exports.push_back({a.model.id, a.desc.outputs[p].id, out.type, unsigned(out.data.size()), &out.value});
    }
    for (unsigned p = 0; p < a.inputs.size(); ++p) {
      const auto &port = a.desc.inputs[p];
      auto edge = std::find_if(d.connections.begin(), d.connections.end(), [&](const auto &e) { return e.to == a.model.id && e.toPort == port.id; });
      if (edge == d.connections.end()) continue;
      if (auto source = local.find(edge->from); source != local.end()) {
        auto &producer = m->atoms[source->second];
        auto out = std::find_if(producer.desc.outputs.begin(), producer.desc.outputs.end(), [&](const auto &p) { return p.id == edge->fromPort; });
        if (out == producer.desc.outputs.end()) { error = "Missing primitive output"; return false; }
        a.inputs[p].source = &producer.outputs[out - producer.desc.outputs.begin()].value;
        if (!isMemoryWrite(a.model, port.id)) { next[source->second].push_back(i); ++degree[i]; }
      } else {
        imports.push_back({i, p, unsigned(m->imports.size())});
        m->imports.push_back({a.model.id, port.id, edge->from, edge->fromPort, port.type,
                             capacities[{edge->from, edge->fromPort}], a.inputs[p].fallback});
      }
    }
    if (a.model.type == "subgraph_input") {
      const auto initial = a.parameters.empty() ? 0 : a.parameters[0];
      m->imports.push_back({a.model.id, "in", {}, {}, parsePortType(a.model.valueType).value(), a.model.capacity, scalar(initial)});
      a.inputs.resize(1);
      imports.push_back({i, 0, unsigned(m->imports.size() - 1)});
    }
  }
  for (const auto &link : imports) m->atoms[link.atom].inputs[link.input].source = &m->imports[link.import].value;
  for (unsigned i = 0; i < degree.size(); ++i) if (!degree[i]) m->order.push_back(i);
  for (unsigned i = 0; i < m->order.size(); ++i) for (auto n : next[m->order[i]]) if (!--degree[n]) m->order.push_back(n);
  if (m->order.size() != m->atoms.size()) { error = "Primitive feedback has no previous-sample memory"; return false; }
  for (auto &a : m->atoms) {
    const auto op = a.desc.operation;
    unsigned ring = 0;
    if (op == Operation::WindowRms) {
      const auto count = std::max(1., std::ceil(rate * a.p(0) * .001));
      if (count > kMaxGraphStateBytes / sizeof(std::array<double, 2>)) { error = "RMS window exceeds prepared memory limit"; return false; }
      ring = unsigned(count);
    }
    if (op == Operation::Fir) {
      ring = kMaxCollection;
      for (const auto &e : d.connections) if (e.to == a.model.id && e.toPort == "coefficients") ring = capacities[{e.from, e.fromPort}];
    }
    if (op == Operation::Biquad)
      for (const auto &e : d.connections) if (e.to == a.model.id && e.toPort == "coefficients" && capacities[{e.from,e.fromPort}] != 5) {
        error = "Biquad requires an Array with exactly five coefficients: " + a.model.id; return false;
      }
    if (ring) {
      if (!reserveGraphMemory(ring * sizeof(std::array<double, 2>), memory, error)) return false;
      a.ring.resize(ring); m->tail = std::max(m->tail, ring);
    }
    if (op == Operation::History) {
      m->knownTail = false;
      if (collection(a.outputs[0].type)) {
        if (!reserveGraphMemory(a.model.capacity * sizeof(double), memory, error)) return false;
        a.history.resize(a.model.capacity);
      }
    }
    if (op == Operation::OnePole || op == Operation::Biquad || op == Operation::DcBlock) m->knownTail = false;
    if (op == Operation::DelayBuffer) {
      a.delay = std::make_unique<DelayMemory>();
      const double maximum = std::max(1., std::ceil(a.p(0) * .001 * rate));
      if (maximum + maxFrames + 2 > kMaxGraphStateBytes / sizeof(DelayMemory::Sample)) { error = "Delay Buffer exceeds prepared memory limit"; return false; }
      a.delay->maximum = unsigned(maximum);
      a.delay->block = maxFrames;
      const auto count = std::size_t(a.delay->maximum) + maxFrames + 2;
      if (!reserveGraphMemory(count * sizeof(DelayMemory::Sample), memory, error)) return false;
      a.delay->samples.resize(count); m->knownTail = false;
    }
    if (op == Operation::Subgraph || op == Operation::Map || op == Operation::Reduce) {
      auto g = std::find_if(d.subgraphs.begin(), d.subgraphs.end(), [&](const auto &g) { return g.id == a.model.subgraph; });
      if (g == d.subgraphs.end()) { error = "Choose a custom node for " + a.model.id; return false; }
      a.factor = op == Operation::Subgraph ? g->oversampling : 1;
      if ((op == Operation::Map || op == Operation::Reduce) && g->oversampling != 1) { error = "Map/Reduce bodies use the current sample rate"; return false; }
      MiniModuleDefinition child; child.version = 5; child.nodes = g->nodes; child.connections = g->connections; child.subgraphs = d.subgraphs;
      for (auto &n : child.nodes) if (n.type == "subgraph_input")
        for (const auto &p : g->inputs) if (p.id == n.port) { n.parameters = {{"default", p.initial}}; n.capacity = p.capacity; }
      MiniModuleDefinition flat;
      if (!expandSubgraphs(child, flat, error)) return false;
      std::vector<std::vector<unsigned>> childIslands;
      if (!programmingIslands(flat, childIslands, error)) return false;
      for (const auto &n : flat.nodes) if (isBlockNode(n) && n.type != "subgraph") {
        error = "Oversampling and collection bodies support elementary nodes only: " + n.type; return false;
      }
      std::vector<unsigned> selectedChild(flat.nodes.size()); std::iota(selectedChild.begin(), selectedChild.end(), 0u);
      unsigned count = 1;
      if (op == Operation::Map || op == Operation::Reduce) {
        for (const auto &e : d.connections) if (e.to == a.model.id && e.toPort == "collection") count = capacities[{e.from, e.fromPort}];
        const auto arg = [&](const char *id, const char *type) {
          return std::any_of(g->inputs.begin(), g->inputs.end(), [&](const auto &p) { return p.id == id && p.type == type; });
        };
        if (g->inputs.size() != (op == Operation::Reduce ? 3u : 2u) || !arg("item", "number") || !arg("index", "integer") ||
            (op == Operation::Reduce && !arg("accumulator", "number")) || g->outputs.size() != 1 || g->outputs[0].type != "number") {
          error = "Map/Reduce needs item:Number, index:Integer, accumulator:Number (Reduce), and one Number output"; return false;
        }
      }
      for (unsigned i = 0; i < count; ++i) {
        auto kernel = std::make_unique<CreatorPrimitiveKernel>();
        if (!kernel->prepare(flat, selectedChild, rate * a.factor, maxFrames * a.factor, channels,
                             seed ^ (i + 1) * 7919u, memory, error, depth + 1)) return false;
        m->knownTail &= kernel->tailKnown();
        m->tail = std::max(m->tail, kernel->tail() / a.factor);
        a.children.push_back(std::move(kernel));
      }
      if (a.children.empty()) { error = "Invalid collection capacity"; return false; }
      auto &kernel = *a.children[0];
      for (unsigned p = 0; p < g->inputs.size(); ++p) {
        auto node = std::find_if(flat.nodes.begin(), flat.nodes.end(), [&](const auto &n) { return n.type == "subgraph_input" && n.port == g->inputs[p].id; });
        if (node == flat.nodes.end()) { error = "Missing custom input boundary"; return false; }
        for (unsigned k = 0; k < kernel.inputs().size(); ++k) if (kernel.inputs()[k].node == node->id) {
          unsigned source = p;
          if (op != Operation::Subgraph) source = g->inputs[p].id == "item" ? 0u : g->inputs[p].id == "index" ? 1u : 2u;
          a.childInputs.emplace_back(source, k);
        }
      }
      for (unsigned p = 0; p < g->outputs.size(); ++p) {
        auto node = std::find_if(flat.nodes.begin(), flat.nodes.end(), [&](const auto &n) { return n.type == "subgraph_output" && n.port == g->outputs[p].id; });
        if (node == flat.nodes.end()) { error = "Missing custom output boundary"; return false; }
        for (unsigned k = 0; k < kernel.outputs().size(); ++k) if (kernel.outputs()[k].node == node->id) a.childOutputs.emplace_back(p, k);
      }
      if (a.factor > 1) {
        for (const auto &p : g->inputs) if (p.type == "buffer" || p.type == "function") { error = "Buffer and Function handles cannot cross sample-rate boundaries"; return false; }
        for (const auto &p : g->outputs) if (p.type == "buffer" || p.type == "function") { error = "Buffer and Function handles cannot cross sample-rate boundaries"; return false; }
        if (!reserveGraphMemory((a.inputs.size() + a.outputs.size()) * sizeof(Resampler), memory, error)) return false;
        a.up.resize(a.inputs.size()); a.down.resize(a.outputs.size());
        a.at2.resize(a.inputs.size()); a.down4.resize(a.outputs.size());
        (void)engine::dsp::halfBand65Taps();
        a.latency = (a.factor == 2 ? 32 : 48) + (kernel.latency() + a.factor - 1) / a.factor;
        const auto childFrames = double(kernel.latency()) / a.factor;
        a.fractionalDelay = std::ceil(childFrames) - childFrames;
        a.fractionalPrevious.resize(a.outputs.size());
        a.outputAlignment.resize(a.outputs.size()); a.outputCursors.resize(a.outputs.size());
        for (unsigned p = 0; p < a.outputs.size(); ++p) if (!continuous(a.outputs[p].type)) {
          const unsigned count = (a.factor == 2 ? 32 : 48) + 1;
          const unsigned capacity = unsigned(a.outputs[p].data.size());
          if (!reserveGraphMemory(std::size_t(count) * (sizeof(StoredValue) + capacity * sizeof(double)), memory, error)) return false;
          a.outputAlignment[p].resize(count);
          for (auto &slot : a.outputAlignment[p]) { slot.type = a.outputs[p].type; slot.data.resize(capacity); }
        }
        m->tail += a.latency * 2;
      }
    }
  }
  // Align parallel paths within an oversampled elementary body. Algorithmic
  // History and Delay Buffer state has zero technical latency.
  std::vector<unsigned> arrival(m->atoms.size());
  const auto delayInput = [&](Input &input, PortType type, unsigned count, unsigned capacity) {
    if (!count || !input.source) return true;
    if (!reserveGraphMemory(std::size_t(count + 1) * (sizeof(StoredValue) + capacity * sizeof(double)), memory, error)) return false;
    input.undelayed = input.source; input.compensation.resize(count + 1);
    for (auto &slot : input.compensation) { slot.type = type; slot.data.resize(capacity); slot.value.elements = slot.data.data(); slot.value.capacity = capacity; }
    return true;
  };
  for (auto i : m->order) {
    auto &a = m->atoms[i]; unsigned maximum = 0;
    for (const auto &e : d.connections) if (e.to == a.model.id && !isMemoryWrite(a.model, e.toPort) && local.contains(e.from)) maximum = std::max(maximum, arrival[local[e.from]]);
    for (unsigned p = 0; p < a.inputs.size(); ++p) for (const auto &e : d.connections)
      if (e.to == a.model.id && e.toPort == a.desc.inputs[p].id && !isMemoryWrite(a.model, e.toPort) && local.contains(e.from))
        if (!delayInput(a.inputs[p], a.desc.inputs[p].type, maximum - arrival[local[e.from]], capacities[{e.from,e.fromPort}])) return false;
    arrival[i] = maximum + a.latency; m->latency = std::max(m->latency, arrival[i]);
  }
  for (unsigned i = 0; i < m->atoms.size(); ++i) {
    auto &a = m->atoms[i];
    if (a.model.type == "subgraph_output" && !a.inputs.empty() && m->latency > arrival[i]) {
      if (!a.inputs[0].compensation.empty()) { error = "Invalid output compensation plan"; return false; }
      unsigned cap = a.outputs[0].type == PortType::Array || a.outputs[0].type == PortType::List ? unsigned(a.outputs[0].data.size()) : 0;
      if (!delayInput(a.inputs[0], a.outputs[0].type, m->latency - arrival[i], cap)) return false;
    }
  }
  if (!retainDelayHistory(m->latency, memory, error)) return false;
  reset();
  return true;
}
bool CreatorPrimitiveKernel::retainDelayHistory(unsigned samples, std::size_t &memory, std::string &error) {
  for (auto &a : m->atoms) if (a.delay) {
    const auto size = std::size_t(a.delay->maximum) + a.delay->block + samples + 2;
    if (size > a.delay->samples.size()) {
      if (!reserveGraphMemory((size - a.delay->samples.size()) * sizeof(DelayMemory::Sample), memory, error)) return false;
      a.delay->samples.resize(size);
    }
  }
  return true;
}
bool CreatorPrimitiveKernel::tick(const PrimitiveContext &ctx) noexcept {
  if (m->failed >= 0) return false;
  for (auto i : m->order) {
    auto &atom = m->atoms[i];
    for (auto &input : atom.inputs) if (!input.compensation.empty()) {
      const auto next = (input.cursor + 1) % input.compensation.size();
      input.compensation[input.cursor].assign(*input.undelayed);
      input.source = &input.compensation[next].value; input.cursor = unsigned(next);
    }
    if (!atom.tick(ctx)) { m->failed = int(i); m->diagnostic.store(int(i), std::memory_order_release); return false; }
  }
  return true;
}
void CreatorPrimitiveKernel::commit() noexcept { for (auto &a : m->atoms) a.commit(); }
void CreatorPrimitiveKernel::reset() noexcept { m->failed = -1; for (auto &a : m->atoms) a.reset(); }
bool CreatorPrimitiveKernel::tailKnown() const noexcept { return m->knownTail; }
unsigned CreatorPrimitiveKernel::tail() const noexcept { return m->tail; }
unsigned CreatorPrimitiveKernel::latency() const noexcept { return m->latency; }
std::string CreatorPrimitiveKernel::error() const { const auto i = m->diagnostic.load(std::memory_order_acquire); return i < 0 ? "" : "Invalid DSP result at node " + m->atoms[i].model.id; }

namespace {
void Atom::reset() noexcept {
  state = {}; cursor = 0; gate = false; phase = triangle = 0;
  for (auto &input : inputs) {
    input.cursor = 0;
    for (auto &slot : input.compensation) {
      std::fill(slot.data.begin(), slot.data.end(), 0.); slot.value = {};
      slot.value.elements = slot.data.data(); slot.value.capacity = unsigned(slot.data.size());
      slot.value.size = slot.type == PortType::Array ? slot.value.capacity : 0;
    }
  }
  rng = initialRng;
  if (desc.operation == Operation::Noise) rng += std::uint64_t(parameters[1]);
  if (desc.operation == Operation::Random) rng += std::uint64_t(parameters[2]);
  rng += 0x9e3779b97f4a7c15ULL;
  randomFrom = random(); randomTo = random();
  if (desc.operation == Operation::Gain || desc.operation == Operation::Mix || desc.operation == Operation::History) state[0] = parameters[0];
  state[1] = state[0];
  std::fill(ring.begin(), ring.end(), std::array<double, 2>{});
  std::fill(history.begin(), history.end(), 0.);
  historySize = model.valueType == "array" ? unsigned(history.size()) : unsigned(std::min(model.values.size(), history.size()));
  if (!history.empty()) std::copy_n(model.values.begin(), std::min(model.values.size(), history.size()), history.begin());
  if (delay) { delay->write = 0; delay->generation = 1; std::fill(delay->samples.begin(), delay->samples.end(), DelayMemory::Sample{}); }
  for (auto &out : outputs) {
    out.value = {}; std::fill(out.data.begin(), out.data.end(), 0.);
    if (collection(out.type)) { out.value.elements = out.data.data(); out.value.capacity = unsigned(out.data.size()); }
  }
  for (auto &c : children) c->reset();
  for (auto &r : up) r = {};
  for (auto &r : down) r = {};
  std::fill(fractionalPrevious.begin(), fractionalPrevious.end(), PrimitiveValue{});
  std::fill(outputCursors.begin(), outputCursors.end(), 0u);
  for (auto &port : outputAlignment) for (auto &slot : port) {
    std::fill(slot.data.begin(),slot.data.end(),0.); slot.value={}; slot.value.elements=slot.data.data();slot.value.capacity=unsigned(slot.data.size());
    slot.value.size=slot.type==PortType::Array?slot.value.capacity:0;
  }
}
void Atom::commit() noexcept {
  if (desc.operation == Operation::History) {
    if (x(2) != 0) {
      state[0] = state[1] = parameters[0];
      if (!history.empty()) {
        std::fill(history.begin(), history.end(), 0.);
        std::copy_n(model.values.begin(), std::min(model.values.size(), history.size()), history.begin());
        historySize = model.valueType == "array" ? unsigned(history.size()) : unsigned(std::min(model.values.size(), history.size()));
      }
    } else if (x(1) != 0) {
      const auto next = v(0);
      if (!history.empty()) {
        historySize = std::min(unsigned(history.size()), next.size);
        if (historySize && next.elements) std::copy_n(next.elements, historySize, history.data());
      } else { state[0] = next.left; state[1] = next.right; }
    }
  }
  if (delay) {
    // Epochs make reset O(1), and preserve pre-reset block snapshots used by a
    // later island. Old epochs remain readable until the retention ring wraps.
    if (x(1) != 0 && !gate) ++delay->generation;
    gate = x(1) != 0;
    auto input = v(0); delay->samples[delay->write] = {{input.left, input.right}, delay->generation};
    if (++delay->write == delay->samples.size()) delay->write = 0;
  }
}
bool Atom::childTick(unsigned index, const PrimitiveContext &ctx) noexcept {
  auto &child = *children[index];
  if (!child.tick(ctx)) return false;
  child.commit();
  return true;
}

bool Atom::tick(const PrimitiveContext &c) noexcept {
  using O = Operation;
  const auto op = desc.operation;
  double y = 0;
  switch (op) {
  case O::Input: outputs[0].value = c.audio; break;
  case O::Output: case O::Wire: case O::SubgraphInput: case O::SubgraphOutput: outputs[0].assign(v(0)); break;
  case O::Interface:
    for (unsigned k = 0; k < outputs.size(); ++k) out(c.controls[k], k);
    break;
  case O::Constant: case O::NumberToAudio: out(p(0)); break;
  case O::Add: out(p(0) + p(1)); break;
  case O::Subtract: out(p(0) - p(1)); break;
  case O::Multiply: out(p(0) * p(1)); break;
  case O::Divide: out(std::abs(p(1)) < 1e-20 ? 0 : p(0) / p(1)); break;
  case O::Minimum: out(std::min(p(0), p(1))); break;
  case O::Maximum: out(std::max(p(0), p(1))); break;
  case O::Abs: out(std::abs(p(0))); break;
  case O::Negate: out(-p(0)); break;
  case O::Power: out(std::pow(p(0), p(1))); break;
  case O::Sqrt: out(std::sqrt(std::max(0., p(0)))); break;
  case O::Sin: out(std::sin(p(0))); break;
  case O::Cos: out(std::cos(p(0))); break;
  case O::Exp: out(std::exp(std::clamp(p(0), -80., 13.8155105579))); break;
  case O::Log: out(std::log(std::max(p(0), 1e-20))); break;
  case O::Log2: out(std::log2(std::max(p(0), 1e-20))); break;
  case O::Log10: out(std::log10(std::max(p(0), 1e-20))); break;
  case O::Tanh: out(std::tanh(p(0))); break;
  case O::Atan: out(std::atan(p(0))); break;
  case O::Sign: out((p(0) > 0) - (p(0) < 0)); break;
  case O::Floor: out(std::floor(p(0))); break;
  case O::Ceil: out(std::ceil(p(0))); break;
  case O::Round: out(std::round(p(0))); break;
  case O::Fraction: out(p(0) - std::floor(p(0))); break;
  case O::Modulo: out(p(1) == 0 ? 0 : std::fmod(p(0), p(1))); break;
  case O::Clamp: out(std::clamp(p(0), std::min(p(1), p(2)), std::max(p(1), p(2)))); break;
  case O::MapRange: out(p(2) == p(1) ? p(3) : std::lerp(p(3), p(4), (p(0) - p(1)) / (p(2) - p(1)))); break;
  case O::Lerp: out(std::lerp(p(0), p(1), p(2))); break;
  case O::Wrap: case O::Fold: {
    double lo = std::min(p(1), p(2)), range = std::abs(p(2) - p(1));
    if (range == 0) { out(lo); break; }
    double period = op == O::Fold ? range * 2 : range;
    double t = std::fmod(p(0) - lo, period); if (t < 0) t += period;
    out(lo + (op == O::Fold && t > range ? period - t : t)); break;
  }
  case O::Smoothstep: {
    double t = p(2) == p(1) ? (p(0) >= p(2) ? 1. : 0.) : std::clamp((p(0) - p(1)) / (p(2) - p(1)), 0., 1.);
    out(t * t * (3 - 2 * t)); break;
  }
  case O::LinearToDb: out(20 * std::log10(std::max(1e-9, std::abs(p(0))))); break;
  case O::DbToLinear: out(std::pow(10., std::clamp(p(0), -180., 120.) / 20)); break;
  case O::Compare:
    switch (int(p(2))) { case 0: y = p(0) > p(1); break; case 1: y = p(0) < p(1); break;
      case 2: y = p(0) == p(1); break; case 3: y = p(0) >= p(1); break; default: y = p(0) <= p(1); }
    out(y); break;
  case O::Select: out(x(0) != 0 ? p(1) : p(0)); break;
  case O::AudioSelect: outputs[0].value = v(x(0) != 0 ? 2 : 1); break;
  case O::And: out(x(0) != 0 && x(1) != 0); break;
  case O::Or: out(x(0) != 0 || x(1) != 0); break;
  case O::Xor: out((x(0) != 0) != (x(1) != 0)); break;
  case O::Not: out(x(0) == 0); break;
  case O::RisingEdge: case O::FallingEdge: {
    bool now = x(0) != 0; out(op == O::RisingEdge ? now && !gate : !now && gate); gate = now; break;
  }
  case O::GateToNumber: out(x(0) != 0); break;
  case O::NumberToGate: out(p(0) != 0); break;
  case O::NumberToInteger: out(std::trunc(std::clamp(p(0), -2147483648., 2147483647.))); break;
  case O::IntegerToNumber: out(x(0)); break;
  case O::AudioAdd: audio(v(0).left + v(1).left, v(0).right + v(1).right); break;
  case O::AudioSubtract: audio(v(0).left - v(1).left, v(0).right - v(1).right); break;
  case O::AudioMultiply: audio(v(0).left * v(1).left, v(0).right * v(1).right); break;
  case O::AudioScale: audio(v(0).left * p(0), v(0).right * p(0)); break;
  case O::StereoSplit: out(v(0).left, 0); out(v(0).right, 1); break;
  case O::StereoJoin: audio(p(0), p(1)); break;
  case O::MidSideEncode: out((v(0).left + v(0).right) * .5, 0); out((v(0).left - v(0).right) * .5, 1); break;
  case O::MidSideDecode: audio(p(0) + p(1), p(0) - p(1)); break;
  case O::AudioToNumber: out(p(0) < .5 ? (v(0).left + v(0).right) * .5 : p(0) < 1.5 ? v(0).left : v(0).right); break;
  case O::Gain: case O::Mix: {
    state[0] = p(0) + std::exp(-1 / (.01 * c.sampleRate)) * (state[0] - p(0));
    if (op == O::Gain) audio(v(0).left * state[0], v(0).right * state[0]);
    else audio(std::lerp(v(0).left, v(1).left, state[0]), std::lerp(v(0).right, v(1).right, state[0]));
    break;
  }
  case O::History:
    if (!history.empty()) outputs[0].assign({0, 0, history.data(), historySize, unsigned(history.size())});
    else audio(state[0], state[1]);
    break;
  case O::Accumulator:
    state[0] = x(1) != 0 ? 0 : state[0] + p(0); out(state[0]); break;
  case O::Counter: {
    bool now = x(0) != 0;
    if (x(1) != 0) state[0] = 0;
    else if (now && !gate) state[0] = state[0] >= 2147483647. ? -2147483648. : state[0] + 1;
    gate = now; out(state[0]); break;
  }
  case O::Context: out(c.sampleRate, 0); out(c.time, 1); out(c.tempo, 2); out(c.beat, 3); out(c.playing, 4); break;
  case O::Smooth: case O::AttackRelease: {
    const double ms = op == O::Smooth ? p(1) : p(0) > state[0] ? p(1) : p(2);
    state[0] = flush(p(0) + std::exp(-1000 / (ms * c.sampleRate)) * (state[0] - p(0)));
    out(state[0]); break;
  }
  case O::Slew: state[0] += std::clamp(p(0) - state[0], -p(2) / c.sampleRate, p(1) / c.sampleRate); out(state[0]); break;
  case O::SampleHold: { bool now = x(1) >= .5; if (now && !gate) state[0] = p(0); gate = now; out(state[0]); break; }
  case O::Peak: out(std::max(std::abs(v(0).left), std::abs(v(0).right))); break;
  case O::Envelope: {
    const auto input = v(0);
    const double level = p(2) >= .5 ? (input.left * input.left + input.right * input.right) * .5
      : std::max(std::abs(input.left), std::abs(input.right));
    state[0] = flush(level + std::exp(-1000 / ((level > state[0] ? p(0) : p(1)) * c.sampleRate)) * (state[0] - level));
    out(p(2) >= .5 ? std::sqrt(std::max(0., state[0])) : state[0]); break;
  }
  case O::WindowRms: {
    const auto input = v(0); const double square = (input.left * input.left + input.right * input.right) * .5;
    state[0] += square - ring[cursor][0]; ring[cursor][0] = square; cursor = (cursor + 1) % unsigned(ring.size());
    out(std::sqrt(std::max(0., state[0]) / ring.size())); break;
  }
  case O::Lfo: case O::Oscillator: {
    const bool osc = op == O::Oscillator, now = x(unsigned(inputs.size() - 1)) >= .5;
    if (now && !gate) phase = triangle = 0;
    gate = now;
    const double freq = !osc && p(4) >= .5 ? std::max(1., c.tempo) / (60 * p(5)) : p(0);
    const double step = std::clamp(freq / c.sampleRate, 1e-9, .45);
    const int shape = int(p(osc ? 2 : 3));
    if (shape == 0) y = std::sin(2 * pi * phase);
    else if (shape == 1 && !osc) y = 1 - 4 * std::abs(phase - .5);
    else if (shape == 2) y = 2 * phase - 1 - (osc ? blep(phase, step) : 0);
    else {
      auto square = (phase < .5 ? 1 : -1) + (osc ? blep(phase, step) - blep(std::fmod(phase + .5, 1), step) : 0);
      if (shape == 1) { triangle = 4 * step * square + (1 - step) * triangle; y = triangle; } else y = square;
    }
    out(y * p(1) + (osc ? 0 : p(2))); phase += step; phase -= std::floor(phase); break;
  }
  case O::Noise: { double left = random() * p(0); audio(left, c.channels == 1 ? left : random() * p(0)); break; }
  case O::Random: {
    bool now = x(unsigned(inputs.size() - 1)) >= .5;
    if (now && !gate) { rng = initialRng + std::uint64_t(parameters[2]) + 0x9e3779b97f4a7c15ULL; phase = 0; randomFrom = random(); randomTo = random(); }
    gate = now; auto t = phase * phase * (3 - 2 * phase);
    out(std::lerp(randomFrom, std::lerp(randomFrom, randomTo, t), p(1)));
    phase += p(0) / c.sampleRate; if (phase >= 1) { phase -= 1; randomFrom = randomTo; randomTo = random(); } break;
  }
  case O::DelayBuffer: outputs[0].value = {}; outputs[0].value.buffer = delay.get(); outputs[0].value.position = delay->write; outputs[0].value.generation = delay->generation; break;
  case O::DelayRead: {
    const auto input = v(0);
    // A compensated Buffer edge begins with empty snapshots, just like an
    // audio edge begins with silence. The graph validator rejects missing
    // connections; this is the valid PDC warm-up interval.
    if (!input.buffer) { audio(0, 0); break; }
    auto sample = input.buffer->read(input.position, p(0) * c.sampleRate * .001, input.generation); audio(sample[0], sample[1]); break;
  }
  case O::OnePole: {
    auto input = v(0); state[0] = flush((1 - p(0)) * input.left + p(0) * state[0]);
    state[1] = flush((1 - p(0)) * input.right + p(0) * state[1]); audio(state[0], state[1]); break;
  }
  case O::Biquad: {
    const auto input = v(0), coeff = v(1); if (!coeff.elements || coeff.size != 5) return false;
    const auto *b = coeff.elements;
    const double l = b[0] * input.left + state[0], r = b[0] * input.right + state[2];
    state[0] = flush(b[1] * input.left - b[3] * l + state[1]); state[1] = flush(b[2] * input.left - b[4] * l);
    state[2] = flush(b[1] * input.right - b[3] * r + state[3]); state[3] = flush(b[2] * input.right - b[4] * r);
    audio(l, r); break;
  }
  case O::BiquadCoefficients: {
    if (outputs[0].value.size == 5 && state[0] == p(0) && state[1] == p(1) && state[2] == p(2) && state[3] == p(3) && state[4] == c.sampleRate) break;
    state[0] = p(0); state[1] = p(1); state[2] = p(2); state[3] = p(3); state[4] = c.sampleRate;
    auto &b = outputs[0].data;
    const double omega = 2 * pi * std::clamp(p(0), 1., c.sampleRate * .49) / c.sampleRate;
    const double cosine = std::cos(omega), sine = std::sin(omega), alpha = sine / (2 * p(1)), A = std::pow(10., p(2) / 40);
    double a0 = 1 + alpha, a1 = -2 * cosine, a2 = 1 - alpha, b0 = 0, b1 = 0, b2 = 0;
    switch (int(p(3))) {
    case 0: b0 = b2 = (1 - cosine) * .5; b1 = 1 - cosine; break;
    case 1: b0 = b2 = (1 + cosine) * .5; b1 = -(1 + cosine); break;
    case 2: b0 = alpha; b2 = -alpha; break;
    case 3: b0 = b2 = 1; b1 = -2 * cosine; break;
    case 4: b0 = a2; b1 = a1; b2 = a0; break;
    case 5: b0 = 1 + alpha * A; b1 = -2 * cosine; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a2 = 1 - alpha / A; break;
    case 6: {
      const double beta = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cosine + beta); b1 = 2 * A * ((A - 1) - (A + 1) * cosine);
      b2 = A * ((A + 1) - (A - 1) * cosine - beta); a0 = (A + 1) + (A - 1) * cosine + beta;
      a1 = -2 * ((A - 1) + (A + 1) * cosine); a2 = (A + 1) + (A - 1) * cosine - beta; break;
    }
    default: {
      const double beta = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cosine + beta); b1 = -2 * A * ((A - 1) + (A + 1) * cosine);
      b2 = A * ((A + 1) + (A - 1) * cosine - beta); a0 = (A + 1) - (A - 1) * cosine + beta;
      a1 = 2 * ((A - 1) - (A + 1) * cosine); a2 = (A + 1) - (A - 1) * cosine - beta;
    }
    }
    b[0] = b0 / a0; b[1] = b1 / a0; b[2] = b2 / a0; b[3] = a1 / a0; b[4] = a2 / a0;
    outputs[0].value.size = 5; break;
  }
  case O::Fir: {
    auto input = v(0), coeff = v(1); if (!coeff.elements || !coeff.size || coeff.size > ring.size()) return false;
    ring[cursor] = {input.left, input.right}; std::array<double, 2> sum{};
    for (unsigned k = 0; k < coeff.size; ++k) {
      const auto &s = ring[(cursor + ring.size() - k) % ring.size()];
      sum[0] += s[0] * coeff.elements[k]; sum[1] += s[1] * coeff.elements[k];
    }
    cursor = (cursor + 1) % unsigned(ring.size()); audio(sum[0], sum[1]); break;
  }
  case O::DcBlock: {
    const auto input = v(0); const double pole = std::exp(-2 * pi * p(0) / c.sampleRate);
    double l = input.left - state[0] + pole * state[2], r = input.right - state[1] + pole * state[3];
    state = {input.left, input.right, flush(l), flush(r)}; audio(l, r); break;
  }
  case O::HardClip: audio(std::clamp(v(0).left, -p(0), p(0)), std::clamp(v(0).right, -p(0), p(0))); break;
  case O::Curve: {
    const auto &points = model.values; const double value = p(0);
    y = points[1];
    for (unsigned i = 2; i < points.size(); i += 2) {
      if (value <= points[i - 2]) break;
      y = std::lerp(points[i - 1], points[i + 1], std::clamp((value - points[i - 2]) / (points[i] - points[i - 2]), 0., 1.));
      if (value <= points[i]) break;
    }
    out(y); break;
  }
  case O::TableLookup: {
    auto a = v(0); if (!a.elements || a.size == 0) { out(0); break; }
    double pos = p(0) * (a.size - 1); unsigned at = unsigned(pos);
    out(std::lerp(a.elements[at], a.elements[std::min(at + 1, a.size - 1)], pos - at)); break;
  }
  case O::Array: case O::List: {
    auto &o = outputs[0]; o.value.size = op == O::Array ? unsigned(o.data.size()) : unsigned(model.values.size());
    std::copy(model.values.begin(), model.values.end(), o.data.begin()); break;
  }
  case O::Length: out(v(0).size); break;
  case O::Get: {
    auto a = v(0); double at = x(1); bool valid = at >= 0 && at < a.size && a.elements;
    out(valid ? a.elements[unsigned(at)] : 0); out(valid, 1); break;
  }
  case O::Set: case O::Append: case O::Remove: case O::Clear: {
    auto a = v(0); auto &o = outputs[0]; o.assign(a); bool valid = a.size <= o.data.size();
    if (op == O::Set || op == O::Remove) valid &= x(1) >= 0 && x(1) < a.size;
    if (op == O::Append) valid &= a.size < o.data.size();
    if (valid) {
      if (op == O::Set) o.data[unsigned(x(1))] = p(0);
      if (op == O::Append) o.data[o.value.size++] = p(0);
      if (op == O::Remove) {
        for (unsigned k = unsigned(x(1)) + 1; k < o.value.size; ++k) o.data[k - 1] = o.data[k];
        --o.value.size;
      }
      if (op == O::Clear) {
        if (o.type == PortType::List) o.value.size = 0;
        else std::fill(o.data.begin(), o.data.end(), 0.);
      }
    }
    out(valid, 1); break;
  }
  case O::Sum: case O::CollectionMin: case O::CollectionMax: {
    auto a = v(0); y = a.size && op != O::Sum ? a.elements[0] : 0;
    for (unsigned k = 0; k < a.size; ++k) y = op == O::Sum ? y + a.elements[k] : op == O::CollectionMin ? std::min(y, a.elements[k]) : std::max(y, a.elements[k]);
    out(y); break;
  }
  case O::Map: case O::Reduce: {
    auto a = v(0); if (a.size > children.size()) return false;
    double accumulator = op == O::Reduce ? p(0) : 0;
    for (unsigned i = 0; i < a.size; ++i) {
      for (auto [arg, index] : childInputs) children[i]->inputs()[index].value = scalar(arg == 0 ? a.elements[i] : arg == 1 ? double(i) : accumulator);
      if (!childTick(i, c)) return false;
      accumulator = children[i]->outputs()[childOutputs[0].second].value->left;
      if (op == O::Map) outputs[0].data[i] = accumulator;
    }
    if (op == O::Map) outputs[0].value.size = a.size; else out(accumulator);
    break;
  }
  case O::Subgraph: {
    auto &child = *children[0]; const auto &taps = engine::dsp::halfBand65Taps();
    for (unsigned phase2 = 0; phase2 < (factor == 1 ? 1u : 2u); ++phase2) {
      if (factor > 1) for (unsigned p = 0; p < inputs.size(); ++p) if (continuous(desc.inputs[p].type)) {
        auto sample = v(p); at2[p] = {up[p].first[0].tick(phase2 ? 0 : 2 * sample.left, taps), up[p].first[1].tick(phase2 ? 0 : 2 * sample.right, taps)};
      }
      for (unsigned phase4 = 0; phase4 < (factor == 4 ? 2u : 1u); ++phase4) {
        for (auto [arg, index] : childInputs) {
          auto value = v(arg);
          if (factor > 1 && continuous(desc.inputs[arg].type)) {
            value.left = at2[arg][0]; value.right = at2[arg][1];
            if (factor == 4) {
              value.left = up[arg].second[0].tick(phase4 ? 0 : 2 * value.left, taps);
              value.right = up[arg].second[1].tick(phase4 ? 0 : 2 * value.right, taps);
            }
          }
          child.inputs()[index].value = value;
        }
        auto ctx = c; ctx.sampleRate *= factor;
        const auto offset = (phase2 * (factor == 4 ? 2 : 1) + phase4) / ctx.sampleRate;
        ctx.time += offset; if (ctx.playing) ctx.beat += offset * ctx.tempo / 60;
        if (!childTick(0, ctx)) return false;
        for (auto [p, index] : childOutputs) {
          auto value = *child.outputs()[index].value;
          if (factor > 1 && continuous(desc.outputs[p].type)) {
            if (factor == 4) {
              value.left = down[p].second[0].tick(value.left, taps); value.right = down[p].second[1].tick(value.right, taps);
              if (phase4 == 0) down4[p] = {value.left, value.right};
            } else down4[p] = {value.left, value.right};
          } else outputs[p].assign(value);
        }
      }
      if (factor > 1) for (unsigned p = 0; p < outputs.size(); ++p) if (continuous(desc.outputs[p].type)) {
        auto l = down[p].first[0].tick(down4[p][0], taps), r = down[p].first[1].tick(down4[p][1], taps);
        if (phase2 == 0) audio(l, r, p);
      }
    }
    for (unsigned p = 0; p < outputAlignment.size(); ++p) {
      auto &ring = outputAlignment[p];
      if (!ring.empty()) {
        auto &at = outputCursors[p]; const auto next = (at + 1) % ring.size();
        ring[at].assign(outputs[p].value); outputs[p].assign(ring[next].value); at = unsigned(next);
      } else if (fractionalDelay > 0) {
        const auto current = outputs[p].value; auto &previous = fractionalPrevious[p];
        audio(std::lerp(current.left,previous.left,fractionalDelay),std::lerp(current.right,previous.right,fractionalDelay),p); previous=current;
      }
    }
    break;
  }
  default: return false;
  }
  for (const auto &o : outputs) {
    if (!std::isfinite(o.value.left) || !std::isfinite(o.value.right)) return false;
    if (collection(o.type)) for (unsigned k = 0; k < o.value.size; ++k) if (!std::isfinite(o.value.elements[k])) return false;
  }
  return true;
}
} // namespace
} // namespace daw::plugins::mini
