#pragma once
#include "Host/PluginInstance.hpp"
#include "Internal/FluxProgram.hpp"
#include <atomic>
#include <memory>

namespace daw::plugins::flux {
struct FluxTelemetry {
  double elapsedBeats = 0;
  std::array<float, 2> input{}, output{};
  std::array<float, slotCount> fill{}, step{};
  std::array<std::array<float, 2>, slotCount> phases{};
  std::array<std::array<float, fieldCount>, slotCount> effective{};
  std::array<std::array<float, 256>, 3> waveform{};
  float correlation = 1, envelope = 0, sidechain = 0, x = .5f, y = .5f,
        beat = 0, barBeats = 4;
  unsigned playingScene = 0, queuedScene = sceneCount, latency = 0,
           revision = 0, midiRevision = 0, parameterRevision = 0;
  int lastCC = -1, lastChannel = 0;
};
struct FluxSnapshot {
  State state;
  FluxTelemetry telemetry;
};
class FluxInstance final : public PluginInstance {
public:
  FluxInstance();
  ~FluxInstance() override;
  static const PluginDescriptor &staticDescriptor();
  const PluginDescriptor &descriptor() const noexcept override {
    return staticDescriptor();
  }
  void setListener(PluginListener *l) noexcept override { m_listener = l; }
  bool supportsOfflinePipelining() const noexcept override { return true; }
  bool setBusLayout(const PluginBusLayout &, PluginBusLayout &) override;
  PluginBusLayout busLayout() const override { return m_layout; }
  bool activate(const PluginProcessInfo &) override;
  void deactivate() override { m_active = m_processing = false; }
  bool isActive() const noexcept override { return m_active; }
  bool isProcessing() const noexcept override { return m_processing; }
  void startProcessing() override { m_processing = true; }
  void stopProcessing() override { m_processing = false; }
  std::span<const ParameterInfo> parameters() const noexcept override {
    return parameterTable();
  }
  std::int32_t parameterIndexForId(std::string_view) const noexcept override;
  double parameterValue(std::uint32_t) const noexcept override;
  std::string parameterText(std::uint32_t, double) const override;
  void setParameterFromHost(std::uint32_t, double) override;
  bool saveState(std::vector<std::uint8_t> &) const override;
  bool loadState(std::span<const std::uint8_t>) override;
  State state() const;
  bool setState(
      const State &); // control thread; DSP preparation never runs in process()
  void setPresetReference(std::string kind, std::string name);
  FluxTelemetry telemetry() const noexcept;
  std::size_t dspMemoryBytes() const noexcept;
  bool hasEditor() const noexcept override { return false; }
  bool openEditor(void *, PluginEditorHost *) override { return false; }
  void closeEditor() override {}
  bool isEditorOpen() const noexcept override { return false; }
  bool editorSize(std::uint32_t &, std::uint32_t &) const noexcept override {
    return false;
  }
  bool editorCanResize() const override { return false; }
  bool setEditorSize(std::uint32_t &, std::uint32_t &) override {
    return false;
  }
  PluginProcessDisposition
  process(const PluginProcessContext &) noexcept override;
  void reset() noexcept override;
  std::uint32_t latencySamples() const noexcept override;
  std::uint32_t tailSamples() const noexcept override;
  bool tailSamplesKnown() const noexcept override { return false; }

private:
  struct Impl;
  std::unique_ptr<Impl> m;
  PluginListener *m_listener = nullptr;
  PluginBusLayout m_layout{{2, 2}, {2}};
  bool m_active = false, m_processing = false;
};
} // namespace daw::plugins::flux
