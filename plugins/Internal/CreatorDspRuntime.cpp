#include "CreatorDspRuntime.hpp"
#include "ChannelColorInstance.hpp"
#include "CompressorInstance.hpp"
#include "DelayInstance.hpp"
#include "EqualizerInstance.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "MiniNodeRegistry.hpp"
#include "ModulationInstance.hpp"
#include "Creator/WasmProgram.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>

namespace daw::plugins::mini {
namespace {
constexpr double pi = std::numbers::pi;
double clean(double v) noexcept {
  return std::isfinite(v) ? std::clamp(v, -1e6, 1e6) : 0.;
}
double flush(double v) noexcept { return std::abs(v) < 1e-24 ? 0. : v; }
double blep(double phase, double step) noexcept {
  if (phase < step) {
    const double t = phase / step;
    return t + t - t * t - 1;
  }
  if (phase > 1 - step) {
    const double t = (phase - 1) / step;
    return t * t + t + t + 1;
  }
  return 0;
}
struct Line {
  std::vector<double> samples;
  std::size_t at = 0;
  double low = 0;
  double feedback = 0;
  void prepare(unsigned size) {
    samples.assign(std::max(1u, size), 0);
    at = 0;
    low = 0;
  }
  void reset() noexcept {
    std::fill(samples.begin(), samples.end(), 0);
    at = 0;
    low = 0;
  }
  double read() const noexcept { return samples[at]; }
  void write(double value) noexcept {
    samples[at] = flush(value);
    if (++at == samples.size())
      at = 0;
  }
};
class TypedNode final : public engine::Node {
public:
  TypedNode(const NodeDefinition &n, std::uint64_t seed)
      : desc(describeNode(n)), initialSeed(seed) {
    slots.assign(desc.inputs.size(), -1);
    for (const auto &p : desc.parameters) {
      values.push_back(p.initial);
      parameterSlots.push_back(-1);
    }
    for (const auto &p : n.parameters)
      for (unsigned i = 0; i < desc.parameters.size(); ++i)
        if (desc.parameters[i].id == p.id)
          values[i] = p.value;
    for (unsigned i = 0; i < desc.inputs.size(); ++i)
      if (desc.inputs[i].parameter >= 0)
        parameterSlots[desc.inputs[i].parameter] = int(i);
    if (n.type == "color") {
      auto color = std::make_unique<channel_color::ChannelColorInstance>();
      color->setProfileSeed(seed);
      fx = std::move(color);
    } else if (n.type == "doubler")
      fx = std::make_unique<modulation::DoublerInstance>();
    else if (n.type == "chorus")
      fx = std::make_unique<modulation::ChorusInstance>();
    else if (n.type == "flanger")
      fx = std::make_unique<modulation::FlangerInstance>();
    else if (n.type == "phaser")
      fx = std::make_unique<modulation::PhaserInstance>();
    else if (n.type == "delay")
      fx = std::make_unique<delay::DelayInstance>();
    else if (n.type == "compressor")
      fx = std::make_unique<compressor::CompressorInstance>();
    else if (n.type == "eq_band") {
      auto eq = std::make_unique<equalizer::EqualizerInstance>();
      eq->setParameterFromHost(
          equalizer::globalParameter(equalizer::GlobalParam::ProcessingMode),
          0);
      for (unsigned band = 0; band < equalizer::kBandCount; ++band)
        eq->setParameterFromHost(
            equalizer::bandParameter(band, equalizer::BandParam::Enabled),
            band == 0 ? 1 : 0);
      fx = std::move(eq);
    }
    if (fx)
      for (unsigned i = 0; i < values.size(); ++i)
        fx->setParameterFromHost(unsigned(desc.parameters[i].nativeIndex),
                                 values[i]);
    lastValues = values;
  }
  std::string_view name() const noexcept override { return desc.id; }
  engine::MidiNodeRole midiRole() const noexcept override {
    return engine::MidiNodeRole::None;
  }
  engine::OfflineNodePolicy offlineNodePolicy() const noexcept override {
    return engine::OfflineNodePolicy::Ordered;
  }
  engine::FrameCount latencySamples() const noexcept override {
    return program ? program->latency(codeIndex) : fx ? fx->latencySamples() : 0;
  }
  engine::FrameCount tailSamples() const noexcept override {
    return program ? program->tail(codeIndex) : fx ? fx->tailSamples()
           : desc.operation == Operation::Reverb ? unsigned(sampleRate * 16)
                                                 : 0;
  }
  void prepare(const engine::PrepareInfo &info) override {
    sampleRate = info.sampleRate;
    pole = std::exp(-1 / (.010 * sampleRate));
    if (fx) {
      PluginBusLayout accepted;
      ready = fx->setBusLayout({{info.channels}, {info.channels}}, accepted) &&
              fx->activate({info.sampleRate, info.maxBlockSize});
      events.resize(std::size_t(info.maxBlockSize) * desc.parameters.size());
      if (ready)
        fx->startProcessing();
    }
    if (desc.operation == Operation::Reverb) {
      constexpr unsigned comb[] = {1116, 1188, 1277, 1356}, ap[] = {556, 441};
      const double scale = sampleRate / 44100 * (values[3] >= .5 ? 1.8 : 1.);
      for (unsigned ch = 0; ch < 2; ++ch) {
        for (unsigned i = 0; i < 4; ++i)
          combs[ch][i].prepare(unsigned((comb[i] + ch * 23) * scale));
        for (unsigned i = 0; i < 2; ++i)
          allpasses[ch][i].prepare(unsigned((ap[i] + ch * 23) * scale));
      }
    }
    reset();
  }
  void reset() override {
    state = phase = triangle = 0;
    previousGate = false;
    failed = false;
    current = desc.operation == Operation::Gain  ? values[0]
              : desc.operation == Operation::Mix ? values[0]
                                                 : 0;
    const auto op = desc.operation;
    const auto explicitSeed = op == Operation::Random  ? values[2]
                              : op == Operation::Noise ? values[1]
                                                       : 0;
    rng = std::uint64_t(explicitSeed) + initialSeed + 0x9e3779b97f4a7c15ULL;
    randomFrom = randomValue();
    randomTo = randomValue();
    lastValues =
        values; // equal sized vectors: no allocation, including RT reset
    if (fx) {
      for (unsigned i = 0; i < values.size(); ++i)
        fx->setParameterFromHost(unsigned(desc.parameters[i].nativeIndex),
                                 values[i]);
      fx->reset();
    }
    if (op == Operation::Reverb) {
      reverbValues = {values[0], values[1], values[2]};
      reverbClock = 0;
      for (auto &channel : combs)
        for (auto &line : channel)
          line.reset();
      for (auto &channel : allpasses)
        for (auto &line : channel)
          line.reset();
    }
  }
  double randomValue() noexcept {
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return double((rng * 2685821657736338717ULL) >> 11) *
               (2. / 9007199254740992.) -
           1;
  }
  double signal(unsigned port, const engine::ProcessContext &c, unsigned frame,
                unsigned channel = 0) const noexcept {
    if (port >= slots.size() || slots[port] < 0)
      return 0;
    const auto &in = c.inputs[slots[port]];
    return clean(
        in.channel(std::min<unsigned>(channel, in.numChannels() - 1))[frame]);
  }
  double parameter(unsigned index, const engine::ProcessContext &c,
                   unsigned frame) const noexcept {
    const auto port = parameterSlots[index];
    if (port < 0 || slots[port] < 0)
      return values[index];
    const auto &p = desc.parameters[index];
    return std::clamp(signal(unsigned(port), c, frame), p.minimum, p.maximum);
  }
  void process(const engine::ProcessContext &c) override {
    const auto op = desc.operation;
    const unsigned channels = c.output.numChannels();
    if (program) {
      for (unsigned p = 0; p < desc.inputs.size(); ++p) {
        if (desc.inputs[p].type == PortType::Function) continue;
        auto *destination = program->input(codeIndex, p);
        const auto parameterIndex = desc.inputs[p].parameter;
        const float fallback = parameterIndex >= 0 ? float(values[parameterIndex]) : 0.f;
        for (unsigned frame = 0; frame < c.frames; ++frame)
          for (unsigned ch = 0; ch < 2; ++ch)
            destination[frame * 2 + ch] = slots[p] < 0 ? fallback : float(signal(p, c, frame, ch));
      }
      failed = !program->render(codeIndex, c);
      for (unsigned ch = 0; ch < channels; ++ch)
        std::fill_n(c.output.channel(ch).data(), c.frames, 0.f);
      return;
    }
    if (op == Operation::Input) {
      for (unsigned ch = 0; ch < channels; ++ch)
        for (unsigned i = 0; i < c.frames; ++i)
          c.output.channel(ch)[i] =
              float(source && source->inputs && ch < source->inputChannels &&
                            source->inputs[ch]
                        ? clean(source->inputs[ch][sourceOffset + i])
                        : 0);
      return;
    }
    if (op == Operation::Interface) {
      for (unsigned ch = 0; ch < channels; ++ch)
        std::fill_n(c.output.channel(ch).data(), c.frames, float(external));
      return;
    }
    if (fx) {
      unsigned count = 0;
      for (unsigned i = 0; i < c.frames; ++i)
        for (unsigned p = 0; p < values.size(); ++p) {
          if (parameterSlots[p] < 0 || slots[parameterSlots[p]] < 0)
            continue;
          const double value = parameter(p, c, i);
          if (value != lastValues[p]) {
            auto &event = events[count++];
            event = {};
            event.frameOffset = i;
            event.paramIndex = unsigned(desc.parameters[p].nativeIndex);
            event.value = value;
            lastValues[p] = value;
          }
        }
      const float *input[2]{};
      float *output[2]{};
      for (unsigned ch = 0; ch < channels; ++ch) {
        input[ch] =
            slots[0] < 0 ? nullptr : c.inputs[slots[0]].channel(ch).data();
        output[ch] = c.output.channel(ch).data();
      }
      PluginProcessContext context;
      context.inputs = input;
      context.outputs = output;
      context.inputChannels = context.outputChannels = channels;
      context.frames = c.frames;
      context.sampleTime = c.timelinePosition;
      context.playing = c.playing;
      context.offline = c.offline;
      context.transport = c.transport;
      context.inputEvents = {events.data(), count};
      if (!ready || fx->process(context) == PluginProcessDisposition::Error) {
        failed = true;
        for (unsigned ch = 0; ch < channels; ++ch)
          std::fill_n(output[ch], c.frames, 0.f);
      }
      return;
    }
    for (unsigned i = 0; i < c.frames; ++i) {
      const auto p = [&](unsigned index) { return parameter(index, c, i); };
      double v = 0;
      switch (op) {
      case Operation::Constant:
      case Operation::NumberToAudio:
        v = p(0);
        break;
      case Operation::Add:
        v = p(0) + p(1);
        break;
      case Operation::Subtract:
        v = p(0) - p(1);
        break;
      case Operation::Multiply:
        v = p(0) * p(1);
        break;
      case Operation::Divide:
        v = std::abs(p(1)) < 1e-20 ? 0 : p(0) / p(1);
        break;
      case Operation::Minimum:
        v = std::min(p(0), p(1));
        break;
      case Operation::Maximum:
        v = std::max(p(0), p(1));
        break;
      case Operation::Abs:
        v = std::abs(p(0));
        break;
      case Operation::Negate:
        v = -p(0);
        break;
      case Operation::Power:
        v = std::pow(p(0), p(1));
        break;
      case Operation::Sqrt:
        v = std::sqrt(std::max(0., p(0)));
        break;
      case Operation::Sin:
        v = std::sin(p(0));
        break;
      case Operation::Cos:
        v = std::cos(p(0));
        break;
      case Operation::Clamp:
        v = std::clamp(p(0), std::min(p(1), p(2)), std::max(p(1), p(2)));
        break;
      case Operation::MapRange:
        v = p(2) == p(1) ? p(3)
                         : std::lerp(p(3), p(4), (p(0) - p(1)) / (p(2) - p(1)));
        break;
      case Operation::Compare:
        switch (int(p(2))) {
        case 0:
          v = p(0) > p(1);
          break;
        case 1:
          v = p(0) < p(1);
          break;
        case 2:
          v = p(0) == p(1);
          break;
        case 3:
          v = p(0) >= p(1);
          break;
        default:
          v = p(0) <= p(1);
        }
        break;
      case Operation::Select:
        v = signal(0, c, i) >= .5 ? p(1) : p(0);
        break;
      case Operation::Smooth: {
        const double a = std::exp(-1000 / (p(1) * sampleRate));
        state = flush(p(0) + a * (state - p(0)));
        v = state;
        break;
      }
      case Operation::SampleHold: {
        const bool gate = signal(1, c, i) >= .5;
        if (gate && !previousGate)
          state = p(0);
        previousGate = gate;
        v = state;
        break;
      }
      case Operation::Lfo:
      case Operation::Oscillator: {
        const bool osc = op == Operation::Oscillator;
        const bool gate = signal(unsigned(slots.size() - 1), c, i) >= .5;
        if (gate && !previousGate) {
          phase = triangle = 0;
        }
        previousGate = gate;
        const double frequency =
            !osc && p(4) >= .5 ? std::max(1., c.transport.tempo) / (60 * p(5))
                               : p(0);
        const double step = std::clamp(frequency / sampleRate, 1e-9, .45);
        const int shape = int(p(osc ? 2 : 3));
        if (shape == 0)
          v = std::sin(2 * pi * phase);
        else if (shape == 1 && !osc)
          v = 1 - 4 * std::abs(phase - .5);
        else if (shape == 2)
          v = 2 * phase - 1 - (osc ? blep(phase, step) : 0);
        else {
          const double square =
              (phase < .5 ? 1 : -1) +
              (osc ? blep(phase, step) - blep(std::fmod(phase + .5, 1), step)
                   : 0);
          if (shape == 1) {
            triangle = 4 * step * square + (1 - step) * triangle;
            v = triangle;
          } else
            v = square;
        }
        v = v * p(1) + (osc ? 0 : p(2));
        phase += step;
        phase -= std::floor(phase);
        break;
      }
      case Operation::Random: {
        const bool gate = signal(unsigned(slots.size() - 1), c, i) >= .5;
        if (gate && !previousGate) {
          rng = std::uint64_t(values[2]) + initialSeed + 0x9e3779b97f4a7c15ULL;
          phase = 0;
          randomFrom = randomValue();
          randomTo = randomValue();
        }
        previousGate = gate;
        const double t = phase * phase * (3 - 2 * phase);
        v = std::lerp(randomFrom, std::lerp(randomFrom, randomTo, t), p(1));
        phase += p(0) / sampleRate;
        if (phase >= 1) {
          phase -= 1;
          randomFrom = randomTo;
          randomTo = randomValue();
        }
        break;
      }
      case Operation::Envelope: {
        double level = 0;
        for (unsigned ch = 0; ch < channels; ++ch) {
          const double x = signal(0, c, i, ch);
          level = p(2) >= .5 ? level + x * x / channels
                             : std::max(level, std::abs(x));
        }
        const double a =
            std::exp(-1000 / ((level > state ? p(0) : p(1)) * sampleRate));
        state = flush(level + a * (state - level));
        v = p(2) >= .5 ? std::sqrt(std::max(0., state)) : state;
        break;
      }
      case Operation::AudioToNumber:
        v = p(0) < .5 ? (signal(0, c, i) + signal(0, c, i, channels - 1)) * .5
                      : signal(0, c, i, p(0) < 1.5 ? 0 : channels - 1);
        break;
      default:
        break;
      }
      if (op == Operation::Gain || op == Operation::Mix)
        current = p(0) + pole * (current - p(0));
      if (op == Operation::Reverb) {
        for (unsigned k = 0; k < 3; ++k)
          reverbValues[k] = p(k) + pole * (reverbValues[k] - p(k));
        if ((reverbClock++ & 31u) == 0)
          for (auto &channel : combs)
            for (auto &line : channel)
              line.feedback =
                  std::pow(.001, double(line.samples.size()) /
                                     (sampleRate * reverbValues[1]));
      }
      for (unsigned ch = 0; ch < channels; ++ch) {
        double y = v;
        if (op == Operation::Output)
          y = signal(0, c, i, ch);
        else if (op == Operation::Gain)
          y = signal(0, c, i, ch) * current;
        else if (op == Operation::Mix)
          y = std::lerp(signal(0, c, i, ch), signal(1, c, i, ch), current);
        else if (op == Operation::Noise)
          y = randomValue() * p(0);
        else if (op == Operation::Reverb) {
          const double x = signal(0, c, i, ch);
          double wet = 0;
          for (auto &line : combs[ch]) {
            const double delayed = line.read();
            line.low =
                flush(std::lerp(delayed, line.low, reverbValues[2] * .92));
            line.write(x + line.low * line.feedback);
            wet += delayed * .25;
          }
          for (auto &line : allpasses[ch]) {
            const double delayed = line.read(), ap = delayed - wet * .5;
            line.write(wet + ap * .5);
            wet = ap;
          }
          y = x * (1 - reverbValues[0] * .5) + wet * reverbValues[0];
        }
        c.output.channel(ch)[i] = float(clean(y));
      }
    }
  }
  NodeDescription desc;
  WasmProgram *program = nullptr;
  unsigned codeIndex = 0;
  std::vector<int> slots, parameterSlots;
  std::vector<double> values, lastValues;
  std::vector<PluginEvent> events;
  std::unique_ptr<PluginInstance> fx;
  const PluginProcessContext *source = nullptr;
  unsigned sourceOffset = 0;
  bool ready = true, failed = false, previousGate = false;
  double external = 0, sampleRate = 48000, phase = 0, state = 0, triangle = 0,
         current = 0, pole = 0;
  double randomFrom = 0, randomTo = 0;
  std::uint64_t initialSeed = 1, rng = 1;
  std::array<std::array<Line, 4>, 2> combs;
  std::array<std::array<Line, 2>, 2> allpasses;
  std::array<double, 3> reverbValues{};
  unsigned reverbClock = 0;
};
class FunctionOutput final : public engine::Node {
public:
  FunctionOutput(WasmProgram &program, unsigned node, unsigned port)
      : program(program), node(node), port(port) {}
  std::string_view name() const noexcept override { return "C++ output"; }
  engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::None; }
  engine::OfflineNodePolicy offlineNodePolicy() const noexcept override { return engine::OfflineNodePolicy::Ordered; }
  void prepare(const engine::PrepareInfo &) override {}
  void reset() override {}
  void process(const engine::ProcessContext &c) override {
    const auto *source = program.output(node, port);
    for (unsigned ch = 0; ch < c.output.numChannels(); ++ch)
      for (unsigned frame = 0; frame < c.frames; ++frame)
        c.output.channel(ch)[frame] = program.failed() ? 0.f : source[frame * 2 + ch];
  }
private:
  WasmProgram &program;
  unsigned node, port;
};
} // namespace
struct CreatorDspRuntime::Impl {
  std::unique_ptr<WasmProgram> program;
  engine::AudioGraph graph;
  engine::GraphProcessor processor{1};
  std::shared_ptr<const engine::CompiledGraph> compiled;
  std::vector<std::shared_ptr<TypedNode>> nodes;
  std::array<TypedNode *, 2> controls{};
  TypedNode *input = nullptr;
  unsigned channels = 2, latency = 0, tail = 0;
  double rate = 48000;
};
CreatorDspRuntime::CreatorDspRuntime() : m(std::make_unique<Impl>()) {}
CreatorDspRuntime::~CreatorDspRuntime() = default;
bool CreatorDspRuntime::prepare(const MiniModuleDefinition &d,
                                const PluginProcessInfo &info,
                                unsigned channels, std::uint64_t seed,
                                std::string &error) {
  if (!(error = validate(d)).empty())
    return false;
  m = std::make_unique<Impl>();
  m->channels = channels;
  m->rate = info.sampleRate;
  if (std::any_of(d.nodes.begin(), d.nodes.end(), [](const auto &n) { return n.function.has_value(); })) {
    m->program = std::make_unique<WasmProgram>();
    if (!m->program->prepare(d, info, channels, seed, error)) return false;
  }
  std::map<std::pair<std::string, std::string>, engine::NodeId> outputs;
  std::map<std::string, std::pair<TypedNode *, engine::NodeId>> consumers;
  const auto used = reachableNodes(d);
  for (unsigned i = 0; i < d.nodes.size(); ++i) {
    if (!used[i])
      continue;
    const auto &n = d.nodes[i];
    if (n.type == "interface") {
      for (unsigned k = 0; k < d.controls.size(); ++k) {
        auto node = std::make_shared<TypedNode>(n, seed);
        node->external = d.controls[k].initial;
        outputs[{n.id, d.controls[k].id}] = m->graph.adoptNode(node);
        m->controls[k] = node.get();
        m->nodes.push_back(std::move(node));
      }
    } else {
      std::uint64_t nodeSeed = seed;
      for (unsigned char ch : n.id)
        nodeSeed = (nodeSeed ^ ch) * 1099511628211ULL;
      auto node = std::make_shared<TypedNode>(n, nodeSeed);
      if (n.function) {
        node->program = m->program.get();
        node->codeIndex = i;
      }
      const auto id = m->graph.adoptNode(node);
      consumers[n.id] = {node.get(), id};
      if (n.function) {
        for (unsigned p = 0; p < n.function->outputs.size(); ++p) {
          auto output = std::make_shared<FunctionOutput>(*m->program, i, p);
          const auto outputId = m->graph.adoptNode(output);
          m->graph.connect(id, outputId);
          outputs[{n.id, n.function->outputs[p].id}] = outputId;
        }
      } else outputs[{n.id, "out"}] = id;
      if (n.type == "input")
        m->input = node.get();
      if (n.type == "output")
        m->graph.setSink(id);
      m->nodes.push_back(std::move(node));
    }
  }
  for (const auto &[id, consumer] : consumers) {
    auto &[node, target] = consumer;
    std::map<engine::NodeId, int> slots;
    for (const auto &e : d.connections)
      if (e.to == id && e.fromPort != "function") {
        const auto producer = outputs.at({e.from, e.fromPort});
        auto [slot, added] = slots.emplace(producer, int(slots.size()));
        if (added && !m->graph.connect(producer, target)) {
          error = "Unable to connect graph";
          return false;
        }
        for (unsigned p = 0; p < node->desc.inputs.size(); ++p)
          if (node->desc.inputs[p].id == e.toPort)
            node->slots[p] = slot->second;
      }
  }
  auto compiled = m->graph.compile(
      {info.sampleRate, info.maxBlockSize, engine::ChannelCount(channels)});
  if (!compiled) {
    error = engine::describe(compiled.error());
    return false;
  }
  for (const auto &n : m->nodes)
    if (!n->ready) {
      error = "Effect node preparation failed: " + std::string(n->name());
      return false;
    }
  m->compiled = *compiled;
  m->latency = m->compiled->totalLatency;
  std::vector<unsigned> tails(m->compiled->nodes.size());
  for (auto i : m->compiled->order) {
    const auto &n = m->compiled->nodes[i];
    unsigned before = 0;
    for (unsigned e = 0; e < n.inputCount; ++e)
      before = std::max(
          before, tails[m->compiled->inputEdges[n.firstInput + e].producer]);
    tails[i] = before + n.node->tailSamples();
    m->tail = std::max(m->tail, tails[i]);
  }
  m->processor.setGraph(m->compiled);
  return true;
}
void CreatorDspRuntime::setControl(unsigned i, double value) noexcept {
  if (i < 2 && m->controls[i])
    m->controls[i]->external = value;
}
bool CreatorDspRuntime::render(const PluginProcessContext &c, unsigned offset,
                               unsigned frames) noexcept {
  if (m->input) {
    m->input->source = &c;
    m->input->sourceOffset = offset;
  }
  float *outputs[2]{};
  for (unsigned ch = 0; ch < m->channels; ++ch)
    outputs[ch] = c.outputs[ch] + offset;
  auto transport = c.transport;
  transport.ppqPosition += double(offset) * transport.tempo / (60 * m->rate);
  const auto result = m->processor.processSerial(
      {outputs, engine::ChannelCount(m->channels), frames}, frames,
      c.sampleTime + offset, c.playing, c.offline, transport);
  if (m->input)
    m->input->source = nullptr;
  return bool(result) && std::none_of(m->nodes.begin(), m->nodes.end(),
                                      [](const auto &n) { return n->failed; });
}
void CreatorDspRuntime::reset() noexcept {
  if (m->program) m->program->reset();
  for (const auto &n : m->nodes)
    n->reset();
  if (m->compiled)
    for (const auto &delay : m->compiled->delays)
      delay->reset();
}
unsigned CreatorDspRuntime::latency() const noexcept { return m->latency; }
unsigned CreatorDspRuntime::tail() const noexcept { return m->tail; }
bool CreatorDspRuntime::takeError(std::string &error) {return m->program && m->program->takeError(error);}
} // namespace daw::plugins::mini
