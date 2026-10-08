#pragma once
#include "CreatorPrimitiveKernel.hpp"
#include "Graph/AudioGraph.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace daw::plugins::mini {
// Typed buses are block snapshots, not pointers to an atom's mutable output.
// Collections and Buffer cursors therefore keep their per-sample meaning when
// a downstream island runs after an entire upstream block has completed.
struct KernelBus {
  PrimitiveOutput port;
  std::vector<PrimitiveValue> frames;
  std::vector<double> elements;
  bool prepare(unsigned count, std::size_t &memory, std::string &error) {
    if (!reserveGraphMemory(std::size_t(count) * (sizeof(PrimitiveValue) + port.capacity * sizeof(double)), memory, error)) return false;
    frames.resize(count); elements.resize(std::size_t(count) * port.capacity);
    reset(); return true;
  }
  void reset() noexcept {
    std::fill(elements.begin(), elements.end(), 0.);
    for (unsigned i = 0; i < frames.size(); ++i) {
      frames[i] = {};
      if (port.capacity) {
        frames[i].elements = elements.data() + std::size_t(i) * port.capacity;
        frames[i].capacity = port.capacity;
        frames[i].size = port.type == PortType::Array ? port.capacity : 0;
      }
    }
  }
  void store(unsigned i, const PrimitiveValue &value) noexcept {
    if (port.capacity) {
      auto *to = elements.data() + std::size_t(i) * port.capacity;
      const auto count = std::min(port.capacity, value.size);
      if (count && value.elements) std::copy_n(value.elements, count, to);
      frames[i].size = count;
    } else frames[i] = value;
  }
};
class KernelNode final : public engine::Node {
public:
  struct Link {
    int slot = -1;
    KernelBus *bus = nullptr;
    KernelBus delay;
    unsigned cursor = 0;
  };
  CreatorPrimitiveKernel kernel;
  std::vector<Link> links;
  std::vector<std::shared_ptr<KernelBus>> buses;
  const PluginProcessContext *source = nullptr;
  const std::array<double, 2> *controls = nullptr;
  unsigned sourceOffset = 0;
  bool failed = false;
  std::atomic<bool> errorPending{false};
  std::string_view name() const noexcept override { return "Creator operations"; }
  engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::None; }
  engine::OfflineNodePolicy offlineNodePolicy() const noexcept override { return engine::OfflineNodePolicy::Ordered; }
  engine::FrameCount latencySamples() const noexcept override { return kernel.latency(); }
  engine::FrameCount tailSamples() const noexcept override { return kernel.tail(); }
  void reset() override {
    kernel.reset(); failed = false;
    for (auto &link : links) { link.delay.reset(); link.cursor = 0; }
    for (auto &bus : buses) bus->reset();
  }
  bool prepareDelays(const engine::CompiledGraph &graph, std::size_t &memory, std::string &error) {
    auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto &n) { return n.node == this; });
    if (node == graph.nodes.end()) return true;
    for (auto &link : links) if (link.bus && link.slot >= 0) {
      const auto &edge = graph.inputEdges[node->firstInput + link.slot];
      if (edge.delayIndex != engine::kInvalidNode) {
        link.delay.port = link.bus->port;
        if (!link.delay.prepare(graph.delays[edge.delayIndex]->delaySamples() + 1, memory, error)) return false;
      }
    }
    return true;
  }
  void process(const engine::ProcessContext &c) override {
    for (unsigned ch = 0; ch < c.output.numChannels(); ++ch) std::fill_n(c.output.channel(ch).data(), c.frames, 0.f);
    auto inputs = kernel.inputs();
    PrimitiveContext ctx;
    ctx.sampleRate = c.sampleRate; ctx.channels = c.output.numChannels();
    ctx.tempo = c.transport.tempo; ctx.playing = c.playing;
    if (controls) ctx.controls = *controls;
    for (unsigned i = 0; i < c.frames; ++i) {
      for (unsigned k = 0; k < links.size(); ++k) {
        auto &link = links[k];
        if (link.bus) {
          if (link.delay.frames.empty()) inputs[k].value = link.bus->frames[i];
          else {
            const auto next = (link.cursor + 1) % link.delay.frames.size();
            link.delay.store(link.cursor, link.bus->frames[i]);
            inputs[k].value = link.delay.frames[next];
            link.cursor = unsigned(next);
          }
        } else if (link.slot >= 0) {
          const auto &in = c.inputs[link.slot];
          inputs[k].value = {in.channel(0)[i], in.channel(in.numChannels() - 1)[i]};
        }
      }
      ctx.time = double(c.timelinePosition + i) / c.sampleRate;
      ctx.beat = c.ppqAtOffset(i);
      if (source && source->inputChannels) {
        const auto at = i + sourceOffset;
        ctx.audio = {source->inputs[0][at], source->inputs[std::min(1u, unsigned(source->inputChannels - 1))][at]};
      }
      if (!failed && !kernel.tick(ctx)) { failed = true; errorPending.store(true, std::memory_order_release); }
      if (!failed) for (const auto &bus : buses)
        if (!std::isfinite(float(bus->port.value->left)) || !std::isfinite(float(bus->port.value->right))) {
          failed = true; errorPending.store(true, std::memory_order_release); break;
        }
      for (auto &bus : buses) bus->store(i, failed ? PrimitiveValue{} : *bus->port.value);
      if (!failed) kernel.commit();
    }
  }
};
class KernelOutput final : public engine::Node {
public:
  explicit KernelOutput(std::shared_ptr<KernelBus> bus) : bus(std::move(bus)) {}
  std::string_view name() const noexcept override { return "Creator port"; }
  engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::None; }
  engine::OfflineNodePolicy offlineNodePolicy() const noexcept override { return engine::OfflineNodePolicy::Ordered; }
  void process(const engine::ProcessContext &c) override {
    for (unsigned ch = 0; ch < c.output.numChannels(); ++ch)
      for (unsigned i = 0; i < c.frames; ++i)
        c.output.channel(ch)[i] = float(ch ? bus->frames[i].right : bus->frames[i].left);
  }
  std::shared_ptr<KernelBus> bus;
};
} // namespace daw::plugins::mini
