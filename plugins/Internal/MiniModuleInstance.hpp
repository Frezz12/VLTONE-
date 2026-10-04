#pragma once
#include "Host/PluginInstance.hpp"
#include "Internal/MiniModuleDefinition.hpp"
#include <array>
#include <atomic>
#include <memory>

namespace daw::engine {
class Node;
}
namespace daw::plugins::mini {
class CreatorDspRuntime;
class MiniModuleInstance final : public PluginInstance {
public:
  MiniModuleInstance();
  ~MiniModuleInstance() override;
  static const PluginDescriptor &staticDescriptor();
  // Called only before a freshly prepared instance is published in AudioGraph.
  // The previous host node remains owned until the audio thread finishes its
  // crossfade; destruction is deferred to pumpMainThread().
  void transitionFrom(std::shared_ptr<engine::Node> previous);
  void requestUpdateFadeOut() noexcept;
  void cancelUpdateFadeOut() noexcept {
    unsigned fading = 1;
    m_updateFading.compare_exchange_strong(fading, 2,
                                           std::memory_order_release);
  }
  bool updateFadeOutFinished() const noexcept;
  void pumpMainThread() override;
  bool configure(const MiniModuleDefinition &, std::uint64_t seed = 0,
                 std::string_view mode = {}, bool *audioChanged = nullptr);
  const std::string &mode() const noexcept { return m_mode; }
  const MiniModuleDefinition &definition() const noexcept {
    return m_definition;
  }
  const std::string &error() const noexcept { return m_error; }
  const PluginDescriptor &descriptor() const noexcept override {
    return m_descriptor;
  }
  bool supportsOfflinePipelining() const noexcept override { return true; }
  void setListener(PluginListener *) noexcept override {}
  bool setBusLayout(const PluginBusLayout &, PluginBusLayout &) override;
  PluginBusLayout busLayout() const override { return m_layout; }
  bool activate(const PluginProcessInfo &) override;
  void deactivate() override { m_active = m_processing = false; }
  bool isActive() const noexcept override { return m_active; }
  bool isProcessing() const noexcept override { return m_processing; }
  void startProcessing() override { m_processing = true; }
  void stopProcessing() override { m_processing = false; }
  std::span<const ParameterInfo> parameters() const noexcept override {
    return m_parameters;
  }
  std::int32_t parameterIndexForId(std::string_view) const noexcept override;
  double parameterValue(std::uint32_t) const noexcept override;
  std::string parameterText(std::uint32_t, double) const override;
  void setParameterFromHost(std::uint32_t, double) override;
  bool saveState(std::vector<std::uint8_t> &) const override;
  bool loadState(std::span<const std::uint8_t>) override;
  bool hasEditor() const noexcept override { return false; }
  bool openEditor(void *, PluginEditorHost *) override { return false; }
  void closeEditor() override {}
  bool isEditorOpen() const noexcept override { return false; }
  bool editorSize(std::uint32_t &, std::uint32_t &) const override {
    return false;
  }
  bool editorCanResize() const override { return false; }
  bool setEditorSize(std::uint32_t &, std::uint32_t &) override {
    return false;
  }
  PluginProcessDisposition
  process(const PluginProcessContext &) noexcept override;
  void reset() noexcept override;
  std::uint32_t latencySamples() const noexcept override { return m_latency; }
  std::uint32_t tailSamples() const noexcept override { return m_tail; }
  bool tailSamplesKnown() const noexcept override { return true; }

private:
  struct Runtime;
  PluginProcessDisposition
  processCore(const PluginProcessContext &,
              const std::array<int, 2> *mapping = nullptr,
              unsigned eventOffset = 0) noexcept;
  bool render(const PluginProcessContext &, unsigned, unsigned) noexcept;
  void applyControls() noexcept;
  std::unique_ptr<Runtime> m_runtime;
  std::unique_ptr<CreatorDspRuntime> m_typedRuntime;
  MiniModuleDefinition m_definition;
  MiniModuleDefinition m_graphDefinition;
  std::string m_mode;
  PluginDescriptor m_descriptor;
  std::string m_error;
  std::vector<ParameterInfo> m_parameters;
  std::array<std::atomic<double>, 2> m_values{};
  PluginBusLayout m_layout{{2}, {2}};
  PluginProcessInfo m_info;
  std::uint64_t m_seed = 0;
  std::uint32_t m_latency = 0, m_tail = 0;
  bool m_active = false, m_processing = false;
  std::shared_ptr<engine::Node> m_previousOwner;
  std::shared_ptr<engine::Node> m_pendingPredecessor;
  MiniModuleInstance *m_previous = nullptr;
  std::array<int, 2> m_previousParameters{-1, -1};
  std::vector<float> m_transitionAudio;
  std::atomic<unsigned> m_transitionRemaining{0}, m_updateFadeRemaining{0};
  std::atomic<unsigned> m_updateFading{
      0}; // idle, fade out, restore after failure
  unsigned m_transitionLength = 1;
};
} // namespace daw::plugins::mini
