#include "MiniModuleInstance.hpp"
#include "ChannelColorInstance.hpp"
#include "CreatorDspRuntime.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Host/PluginNode.hpp"
#include "ModulationInstance.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>
#include <utility>

namespace daw::plugins::mini {
namespace {
class MiniNode final : public engine::Node {
public:
  explicit MiniNode(const NodeDefinition &d, std::uint64_t seed)
      : type(d.type) {
    if (type == "color") {
      auto color = std::make_unique<channel_color::ChannelColorInstance>();
      color->setProfileSeed(seed);
      fx = std::move(color);
    }
    if (type == "doubler")
      fx = std::make_unique<modulation::DoublerInstance>();
    if (type == "chorus")
      fx = std::make_unique<modulation::ChorusInstance>();
    target = type == "mix" ? .5 : 1.;
    for (const auto &p : d.parameters)
      set(index(p.id), p.value);
  }
  std::string_view name() const noexcept override { return type; }
  engine::MidiNodeRole midiRole() const noexcept override {
    return engine::MidiNodeRole::None;
  }
  engine::OfflineNodePolicy offlineNodePolicy() const noexcept override {
    return engine::OfflineNodePolicy::Ordered;
  }
  engine::FrameCount latencySamples() const noexcept override {
    return fx ? fx->latencySamples() : 0;
  }
  engine::FrameCount tailSamples() const noexcept override {
    return fx ? fx->tailSamples() : 0;
  }
  int index(std::string_view key) const noexcept {
    return fx ? fx->parameterIndexForId(key) : 0;
  }
  void set(int index, double value) noexcept {
    if (fx)
      fx->setParameterFromHost(unsigned(index), value);
    else
      target = value;
  }
  void prepare(const engine::PrepareInfo &p) override {
    pole = std::exp(-1 / (.010 * p.sampleRate));
    current = target;
    if (fx) {
      PluginBusLayout accepted;
      ready = fx->setBusLayout({{p.channels}, {p.channels}}, accepted) &&
              fx->activate({p.sampleRate, p.maxBlockSize});
      if (ready)
        fx->startProcessing();
    }
  }
  void reset() override {
    current = target;
    if (fx)
      fx->reset();
  }
  void process(const engine::ProcessContext &c) override {
    const auto channels = c.output.numChannels();
    if (type == "input") {
      for (unsigned ch = 0; ch < channels; ++ch)
        for (unsigned i = 0; i < c.frames; ++i) {
          const double x = source && source->inputs &&
                                   ch < source->inputChannels &&
                                   source->inputs[ch]
                               ? source->inputs[ch][sourceOffset + i]
                               : 0;
          c.output.channel(ch)[i] = std::isfinite(x) ? float(x) : 0.f;
        }
    } else if (fx) {
      const float *input[2]{};
      float *output[2]{};
      for (unsigned ch = 0; ch < channels; ++ch) {
        input[ch] = c.inputs.empty() ? nullptr : c.inputs[0].channel(ch).data();
        output[ch] = c.output.channel(ch).data();
      }
      PluginProcessContext p;
      p.inputs = input;
      p.outputs = output;
      p.inputChannels = p.outputChannels = channels;
      p.frames = c.frames;
      p.sampleTime = c.timelinePosition;
      p.playing = c.playing;
      p.offline = c.offline;
      p.transport = c.transport;
      if (!ready || fx->process(p) == PluginProcessDisposition::Error) {
        failed = true;
        for (unsigned ch = 0; ch < channels; ++ch)
          std::fill_n(output[ch], c.frames, 0.f);
      }
    } else {
      for (unsigned i = 0; i < c.frames; ++i) {
        current = target + pole * (current - target);
        for (unsigned ch = 0; ch < channels; ++ch) {
          const float a = c.inputs.empty() ? 0 : c.inputs[0].channel(ch)[i];
          c.output.channel(ch)[i] =
              type == "mix"
                  ? float(std::lerp(
                        double(a), double(c.inputs[1].channel(ch)[i]), current))
              : type == "gain" ? float(a * current)
                               : a;
        }
      }
    }
  }
  std::string type;
  std::unique_ptr<PluginInstance> fx;
  const PluginProcessContext *source = nullptr;
  unsigned sourceOffset = 0;
  bool ready = true, failed = false;
  double current = 1, target = 1, pole = 0;
};
} // namespace
struct MiniModuleInstance::Runtime {
  engine::AudioGraph graph;
  engine::GraphProcessor processor{1};
  std::shared_ptr<const engine::CompiledGraph> compiled;
  std::vector<std::shared_ptr<MiniNode>> nodes;
  struct Target {
    MiniNode *node;
    int index;
    Binding binding;
  };
  std::array<std::vector<Target>, 2> controls;
  MiniNode *input = nullptr;
};
MiniModuleInstance::MiniModuleInstance() : m_descriptor(staticDescriptor()) {}
MiniModuleInstance::~MiniModuleInstance() = default;
void MiniModuleInstance::transitionFrom(
    std::shared_ptr<engine::Node> previous) {
  auto *host = dynamic_cast<PluginNode *>(previous.get());
  auto *instance =
      host ? dynamic_cast<MiniModuleInstance *>(host->instance()) : nullptr;
  if (!instance || !instance->isActive())
    return;
  if (!m_active) {
    m_pendingPredecessor = std::move(previous);
    return;
  }
  m_transitionLength = std::max(1u, unsigned(m_info.sampleRate * .020));
  if (m_latency == instance->m_latency &&
      m_layout.outputs == instance->m_layout.outputs) {
    m_previousOwner = std::move(previous);
    m_previous = instance;
    for (unsigned i = 0; i < m_parameters.size(); ++i)
      m_previousParameters[i] =
          instance->parameterIndexForId(m_parameters[i].id);
    m_transitionAudio.assign(std::size_t(m_info.maxBlockSize) * 2, 0);
  }
  // A changed latency uses a fade-in after the controller's pre-publication
  // fade-out. The new graph's PDC and its new node become visible together.
  m_transitionRemaining.store(m_transitionLength, std::memory_order_release);
}
void MiniModuleInstance::requestUpdateFadeOut() noexcept {
  m_updateFadeRemaining.store(std::max(1u, unsigned(m_info.sampleRate * .020)),
                              std::memory_order_relaxed);
  m_updateFading.store(1, std::memory_order_release);
}
bool MiniModuleInstance::updateFadeOutFinished() const noexcept {
  return m_updateFading.load(std::memory_order_acquire) == 1 &&
         m_updateFadeRemaining.load(std::memory_order_acquire) == 0;
}
void MiniModuleInstance::pumpMainThread() {
  if(m_typedRuntime)m_typedRuntime->takeError(m_error);
  if (m_transitionRemaining.load(std::memory_order_acquire) == 0) {
    m_previousOwner.reset();
    // Keep the raw pointer and storage untouched: process() reads them only
    // while remaining > 0, and never destroys an owner on its last sample.
  }
}
const PluginDescriptor &MiniModuleInstance::staticDescriptor() {
  static const auto d = [] {
    PluginDescriptor d;
    d.format = Format::Internal;
    d.uid = d.path = std::string(kUid);
    d.name = "Mini module";
    d.vendor = "VLTONE";
    d.version = "1.0";
    d.stateSchemaVersion = 1;
    d.category = "Effect|Mini";
    return d;
  }();
  return d;
}
bool MiniModuleInstance::configure(const MiniModuleDefinition &d,
                                   std::uint64_t seed, std::string_view mode,
                                   bool *audioChanged) {
  if (audioChanged)
    *audioChanged = false;
  if (d == m_definition && seed == m_seed && mode == m_mode)
    return m_error.empty();
  const bool active = m_active;
  auto graph = resolved(d, mode);
  if (graph.version < 3)
    graph.version = 1;
  for (auto &c : graph.controls)
    c.style.clear();
  const bool sameAudio =
      sameAudioGraph(d, mode, m_definition, m_mode) && seed == m_seed;
  m_definition = d;
  m_mode = mode;
  m_seed = seed;
  m_error = validate(d, mode);
  m_descriptor.name = d.name;
  if (!m_error.empty()) {
    m_active = false;
    m_runtime.reset();
    m_typedRuntime.reset();
    return false;
  }
  if (sameAudio) {
    for (unsigned i = 0; i < m_parameters.size(); ++i) {
      m_parameters[i].name = d.controls[i].name;
      m_parameters[i].unit = d.controls[i].unit;
    }
    return true;
  }
  if (audioChanged)
    *audioChanged = true;
  m_active = false;
  m_graphDefinition = std::move(graph);
  m_parameters.clear();
  for (unsigned i = 0; i < d.controls.size(); ++i) {
    const auto &c = d.controls[i];
    m_parameters.push_back(
        {i, c.id, c.name, c.unit, c.minimum, c.maximum, c.initial});
    m_values[i] = c.initial;
  }
  return !active || activate(m_info);
}
bool MiniModuleInstance::setBusLayout(const PluginBusLayout &p,
                                      PluginBusLayout &accepted) {
  if (p.inputs.size() != 1 || p.outputs.size() != 1 ||
      p.inputs[0] != p.outputs[0] || p.inputs[0] < 1 || p.inputs[0] > 2)
    return false;
  accepted = m_layout = p;
  return true;
}
void MiniModuleInstance::applyControls() noexcept {
  if (m_typedRuntime) {
    for (unsigned i = 0; i < m_parameters.size(); ++i)
      m_typedRuntime->setControl(i, parameterValue(i));
    return;
  }
  for (unsigned i = 0; i < m_parameters.size(); ++i) {
    const auto &c = m_graphDefinition.controls[i];
    const double value = parameterValue(i);
    const double t = std::clamp(
        c.logarithmic
            ? std::log(value / c.minimum) / std::log(c.maximum / c.minimum)
            : (value - c.minimum) / (c.maximum - c.minimum),
        0., 1.);
    for (const auto &target : m_runtime->controls[i]) {
      const auto &b = target.binding;
      const double mapped = b.bipolarMagnitude ? std::abs(2 * t - 1) : t;
      target.node->set(target.index,
                       b.logarithmic
                           ? b.minimum * std::pow(b.maximum / b.minimum, mapped)
                           : std::lerp(b.minimum, b.maximum, mapped));
    }
  }
}
bool MiniModuleInstance::activate(const PluginProcessInfo &info) {
  m_active = false;
  m_info = info;
  if (!m_error.empty() || m_definition.nodes.empty() ||
      !std::isfinite(info.sampleRate) || info.sampleRate < 8000 ||
      info.sampleRate > 384000 || !info.maxBlockSize)
    return false;
  m_typedRuntime.reset();
  if (m_graphDefinition.version >= 3) {
    m_runtime.reset();
    auto runtime = std::make_unique<CreatorDspRuntime>();
    if (!runtime->prepare(m_graphDefinition, info, m_layout.outputs[0], m_seed,
                          m_error))
      return false;
    m_latency = runtime->latency();
    m_tail = runtime->tail();
    m_typedRuntime = std::move(runtime);
    m_active = true;
    applyControls();
    if (m_pendingPredecessor)
      transitionFrom(std::exchange(m_pendingPredecessor, {}));
    return true;
  }
  m_runtime = std::make_unique<Runtime>();
  std::map<std::string, unsigned> ids;
  for (const auto &d : m_graphDefinition.nodes) {
    auto node = std::make_shared<MiniNode>(d, m_seed);
    auto id = m_runtime->graph.adoptNode(node);
    ids[d.id] = id;
    if (d.type == "input")
      m_runtime->input = node.get();
    if (d.type == "output")
      m_runtime->graph.setSink(id);
    m_runtime->nodes.push_back(std::move(node));
  }
  for (const auto &c : m_graphDefinition.connections)
    if (!m_runtime->graph.connect(ids[c.from], ids[c.to]))
      return false;
  for (unsigned i = 0; i < m_graphDefinition.controls.size(); ++i)
    for (const auto &b : m_graphDefinition.controls[i].bindings) {
      auto *node = m_runtime->nodes[ids[b.node]].get();
      m_runtime->controls[i].push_back({node, node->index(b.parameter), b});
    }
  applyControls();
  auto compiled = m_runtime->graph.compile(
      {info.sampleRate, info.maxBlockSize, m_layout.outputs[0]});
  if (!compiled) {
    m_error = std::string(engine::describe(compiled.error()));
    return false;
  }
  for (const auto &n : m_runtime->nodes)
    if (!n->ready) {
      m_error = "DSP node could not be prepared";
      return false;
    }
  m_latency = (*compiled)->totalLatency;
  std::vector<unsigned> tails(m_runtime->nodes.size());
  m_tail = 0;
  for (auto i : (*compiled)->order) {
    const auto &n = (*compiled)->nodes[i];
    unsigned before = 0;
    for (unsigned e = 0; e < n.inputCount; ++e)
      before = std::max(
          before, tails[(*compiled)->inputEdges[n.firstInput + e].producer]);
    tails[i] = before + n.node->tailSamples();
    m_tail = std::max(m_tail, tails[i]);
  }
  m_runtime->compiled = *compiled;
  m_runtime->processor.setGraph(*compiled);
  m_active = true;
  reset();
  if (m_pendingPredecessor)
    transitionFrom(std::exchange(m_pendingPredecessor, {}));
  return true;
}
std::int32_t
MiniModuleInstance::parameterIndexForId(std::string_view id) const noexcept {
  for (const auto &p : m_parameters)
    if (p.id == id)
      return std::int32_t(p.index);
  return -1;
}
double MiniModuleInstance::parameterValue(std::uint32_t i) const noexcept {
  return i < m_parameters.size() ? m_values[i].load(std::memory_order_relaxed)
                                 : 0;
}
void MiniModuleInstance::setParameterFromHost(std::uint32_t i, double value) {
  if (i < m_parameters.size()) {
    const auto &p = m_parameters[i];
    m_values[i].store(std::isfinite(value)
                          ? std::clamp(value, p.minValue, p.maxValue)
                          : p.defaultValue,
                      std::memory_order_relaxed);
  }
}
std::string MiniModuleInstance::parameterText(std::uint32_t i, double v) const {
  if (i >= m_parameters.size())
    return {};
  char out[64];
  const auto &p = m_parameters[i];
  std::snprintf(out, sizeof(out),
                p.unit == "Hz"  ? "%.2f Hz"
                : p.unit == "%" ? "%.0f%%"
                                : "%.1f",
                p.unit == "%" && p.minValue >= 0 && p.maxValue <= 1 ? 100 * v : v);
  return std::string(out) + (p.unit.empty() || p.unit == "%" || p.unit == "Hz" ? "" : " " + p.unit);
}
bool MiniModuleInstance::saveState(std::vector<std::uint8_t> &out) const {
  nlohmann::json j{{"version", 2},
                   {"definition", toJson(m_definition)},
                   {"mode", m_mode},
                   {"seed", m_seed},
                   {"parameters", nlohmann::json::object()}};
  for (const auto &p : m_parameters)
    j["parameters"][p.id] = parameterValue(p.index);
  const auto text = j.dump();
  out.assign(text.begin(), text.end());
  return true;
}
bool MiniModuleInstance::loadState(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > 2 * 1024 * 1024)
    return false;
  const auto j =
      nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
  try {
    if ((j.at("version") != 1 && j.at("version") != 2) ||
        !j.at("parameters").is_object())
      return false;
    auto d = fromJson(j.at("definition"));
    const auto mode = j.value("mode", std::string{});
    if (!validate(d, mode).empty())
      return false;
    for (const auto &[id, value] : j.at("parameters").items())
      if (!value.is_number() || !std::isfinite(value.get<double>()))
        return false;
    const bool active = m_active;
    m_active = false;
    if (!configure(d, j.at("seed").get<std::uint64_t>(), mode))
      return false;
    for (const auto &p : m_parameters)
      setParameterFromHost(p.index,
                           j.at("parameters").value(p.id, p.defaultValue));
    return !active || activate(m_info);
  } catch (const nlohmann::json::exception &) {
    return false;
  }
}
void MiniModuleInstance::reset() noexcept {
  m_transitionRemaining.store(0, std::memory_order_release);
  m_updateFading.store(false, std::memory_order_release);
  if (m_typedRuntime) {
    applyControls();
    m_typedRuntime->reset();
    return;
  }
  if (!m_runtime)
    return;
  applyControls();
  for (auto &n : m_runtime->nodes) {
    n->reset();
    n->failed = false;
  }
  if (const auto &graph = m_runtime->compiled)
    for (const auto &delay : graph->delays)
      delay->reset();
}
bool MiniModuleInstance::render(const PluginProcessContext &c, unsigned offset,
                                unsigned count) noexcept {
  if (!count)
    return true;
  if (m_typedRuntime)
    return m_typedRuntime->render(c, offset, count);
  m_runtime->input->source = &c;
  m_runtime->input->sourceOffset = offset;
  float *output[2]{};
  for (unsigned ch = 0; ch < m_layout.outputs[0]; ++ch)
    output[ch] = c.outputs[ch] + offset;
  auto transport = c.transport;
  transport.ppqPosition +=
      double(offset) * transport.tempo / (60 * m_info.sampleRate);
  const auto result = m_runtime->processor.processSerial(
      {output, m_layout.outputs[0], count}, count, c.sampleTime + offset,
      c.playing, c.offline, transport);
  m_runtime->input->source = nullptr;
  return bool(result) &&
         std::none_of(m_runtime->nodes.begin(), m_runtime->nodes.end(),
                      [](const auto &n) { return n->failed; });
}
PluginProcessDisposition
MiniModuleInstance::processCore(const PluginProcessContext &c,
                                const std::array<int, 2> *mapping,
                                unsigned eventOffset) noexcept {
  if (!m_active || !c.outputs || c.outputChannels < m_layout.outputs[0])
    return PluginProcessDisposition::Error;
  for (unsigned ch = 0; ch < m_layout.outputs[0]; ++ch)
    if (!c.outputs[ch])
      return PluginProcessDisposition::Error;
  applyControls();
  unsigned at = 0;
  const auto run = [&](unsigned end) {
    while (at < end) {
      unsigned n = std::min(end - at, m_info.maxBlockSize);
      if (!render(c, at, n))
        return false;
      at += n;
    }
    return true;
  };
  for (const auto &e : c.inputEvents) {
    const int index = mapping
                          ? (e.paramIndex < 2 ? (*mapping)[e.paramIndex] : -1)
                          : int(e.paramIndex);
    if (e.kind == PluginEvent::Kind::ParamValue && index >= 0 &&
        unsigned(index) < m_parameters.size()) {
      if (!run(std::clamp(
              e.frameOffset >= eventOffset ? e.frameOffset - eventOffset : 0u,
              at, c.frames)))
        return PluginProcessDisposition::Error;
      setParameterFromHost(unsigned(index), e.value);
      applyControls();
    }
  }
  if (!run(c.frames))
    return PluginProcessDisposition::Error;
  for (unsigned ch = m_layout.outputs[0]; ch < c.outputChannels; ++ch)
    if (c.outputs[ch])
      std::fill_n(c.outputs[ch], c.frames, 0.f);
  return PluginProcessDisposition::Continue;
}
PluginProcessDisposition
MiniModuleInstance::process(const PluginProcessContext &c) noexcept {
  auto remaining = m_transitionRemaining.load(std::memory_order_acquire);
  if (!remaining && !m_updateFading.load(std::memory_order_acquire))
    return processCore(c);
  if (!m_active || !c.outputs || c.outputChannels < m_layout.outputs[0])
    return PluginProcessDisposition::Error;
  for (unsigned ch = 0; ch < m_layout.outputs[0]; ++ch)
    if (!c.outputs[ch])
      return PluginProcessDisposition::Error;
  std::size_t eventAt = 0;
  for (unsigned offset = 0; offset < c.frames;) {
    const unsigned count = std::min(m_info.maxBlockSize, c.frames - offset);
    const float *input[2]{};
    float *output[2]{}, *oldOutput[2]{};
    for (unsigned ch = 0; ch < m_layout.outputs[0]; ++ch) {
      input[ch] = c.inputs && ch < c.inputChannels && c.inputs[ch]
                      ? c.inputs[ch] + offset
                      : nullptr;
      output[ch] = c.outputs[ch] + offset;
      if (remaining && m_previous)
        oldOutput[ch] = m_transitionAudio.data() + ch * m_info.maxBlockSize;
    }
    auto block = c;
    block.inputs = input;
    block.outputs = output;
    block.frames = count;
    block.inputChannels = std::min(c.inputChannels, m_layout.inputs[0]);
    block.outputChannels = m_layout.outputs[0];
    block.sampleTime += offset;
    block.transport.ppqPosition +=
        double(offset) * block.transport.tempo / (60 * m_info.sampleRate);
    const auto firstEvent = eventAt;
    while (eventAt < c.inputEvents.size() &&
           (c.inputEvents[eventAt].frameOffset < offset + count ||
            offset + count == c.frames))
      ++eventAt;
    block.inputEvents = c.inputEvents.subspan(firstEvent, eventAt - firstEvent);
    if (remaining && m_previous) {
      for (unsigned i = 0; i < m_parameters.size(); ++i)
        if (m_previousParameters[i] >= 0)
          m_previous->setParameterFromHost(unsigned(m_previousParameters[i]),
                                           parameterValue(i));
      auto old = block;
      old.outputs = oldOutput;
      old.outputChannels = m_layout.outputs[0];
      if (m_previous->processCore(old, &m_previousParameters, offset) ==
          PluginProcessDisposition::Error)
        for (unsigned ch = 0; ch < m_layout.outputs[0]; ++ch)
          std::fill_n(oldOutput[ch], count, 0.f);
    }
    if (processCore(block, nullptr, offset) == PluginProcessDisposition::Error)
      return PluginProcessDisposition::Error;
    const auto fadeState = m_updateFading.load(std::memory_order_acquire);
    auto fadeRemaining = m_updateFadeRemaining.load(std::memory_order_relaxed);
    const double fadeLength = std::max(1., m_info.sampleRate * .020);
    for (unsigned i = 0; i < count; ++i) {
      const double mix =
          remaining ? 1 - double(remaining) / m_transitionLength : 1;
      const double fade = fadeState ? double(fadeRemaining) / fadeLength : 1;
      for (unsigned ch = 0; ch < m_layout.outputs[0]; ++ch) {
        const double previous = remaining && m_previous ? oldOutput[ch][i] : 0;
        output[ch][i] =
            float((remaining ? std::lerp(previous, double(output[ch][i]), mix)
                             : output[ch][i]) *
                  fade);
      }
      if (remaining)
        --remaining;
      if (fadeState == 1 && fadeRemaining)
        --fadeRemaining;
      else if (fadeState == 2 && fadeRemaining < unsigned(fadeLength))
        ++fadeRemaining;
    }
    if (fadeState)
      m_updateFadeRemaining.store(fadeRemaining, std::memory_order_release);
    if (fadeState == 2 && fadeRemaining >= unsigned(fadeLength)) {
      unsigned restoring = 2;
      m_updateFading.compare_exchange_strong(restoring, 0, std::memory_order_release);
    }
    offset += count;
  }
  m_transitionRemaining.store(remaining, std::memory_order_release);
  for (unsigned ch = m_layout.outputs[0]; ch < c.outputChannels; ++ch)
    if (c.outputs[ch]) std::fill_n(c.outputs[ch], c.frames, 0.f);
  if (!remaining)
    PluginMainThreadWork::request();
  return PluginProcessDisposition::Continue;
}
} // namespace daw::plugins::mini
