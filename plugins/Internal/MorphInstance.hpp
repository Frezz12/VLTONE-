#pragma once
#include "Host/PluginInstance.hpp"
#include "Internal/ModulationDsp.hpp"
#include "Internal/MorphProgram.hpp"
#include <atomic>

namespace daw::plugins::morph {
class MorphInstance final : public PluginInstance {
public:
  MorphInstance();
  static const PluginDescriptor &staticDescriptor();
  const PluginDescriptor &descriptor() const noexcept override {
    return staticDescriptor();
  }
  bool supportsOfflinePipelining() const noexcept override { return true; }
  void setListener(PluginListener *l) noexcept override { m_listener = l; }
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
  bool setCanvas(const MorphCanvasProgram &,
                 const std::string *category =
                     nullptr); // control-thread, SPSC publication
  void setPresetReference(std::string kind, std::string name);
  Telemetry telemetry() const noexcept;
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
  std::uint32_t latencySamples() const noexcept override { return 0; }
  std::uint32_t tailSamples() const noexcept override;
  std::size_t dspMemoryBytes() const noexcept;

private:
  using Delay = modulation::dsp::Delay;
  struct Core {
    std::array<Delay, 2> chorus, flanger, doubler;
    std::array<std::array<modulation::dsp::Allpass, 8>, 2> phaser;
    std::array<double, 2> chorusTone{}, flangerTone{}, flangerFeedback{},
        phaserFeedback{};
    std::array<double, 4> pitchPhase{};
    void prepare(double);
    void reset() noexcept;
  };
  void control(const PluginProcessContext &, unsigned offset) noexcept;
  std::array<double, 2> modules(Core &, std::array<double, 2>,
                                bool serial) noexcept;
  std::array<double, 2> sample(std::array<double, 2>) noexcept;
  void adoptProgram() noexcept;
  double saturate(double, unsigned channel, bool feedback) noexcept;
  std::array<std::atomic<double>, Count> m_values{};
  Values m_targets{}, m_current{}, m_steps{};
  State m_state;
  // Classic SPSC triple buffer: front belongs to audio, back to control,
  // middle changes owners only through the exchange. No reclamation in RT.
  std::array<MorphCanvasProgram, 3> m_programs;
  std::atomic<unsigned> m_middle{1};
  unsigned m_front = 0, m_back = 2;
  PluginListener *m_listener = nullptr;
  PluginBusLayout m_layout{{2}, {2}};
  double m_rate = 48000, m_tempo = 120, m_freeBeat = 0;
  bool m_active = false, m_processing = false;
  unsigned m_controlPhase = 0, m_route = 0;
  std::array<Core, 2> m_cores;
  double m_routeBlend = 0;
  std::array<double, 4> m_phases{}, m_motion{}, m_weights{}, m_weightSteps{};
  std::array<double, 2> m_lfoPhase{}, m_lfo{}, m_previousDrive{},
      m_previousFeedbackDrive{};
  std::array<std::array<double, 8>, 2> m_allpassCoefficients{},
      m_allpassSteps{};
  std::array<std::array<double, 4>, 2> m_chorusDelays{}, m_chorusDelaySteps{};
  std::array<double, 4> m_pitchSteps{};
  double m_chorusPole = .5, m_flangerPole = .5, m_echoPole = .5,
         m_bloomPole = .5;
  double m_hpPole = 0, m_lpPole = 0, m_attackPole = 0, m_releasePole = 0,
         m_gainPole = 0, m_gainDownPole = 0, m_gainUpPole = 0;
  std::array<double, 2> m_hp{}, m_lp{}, m_echoTone{}, m_bloomTone{},
      m_feedbackDc{};
  std::array<Delay, 2> m_echo;
  std::array<Delay, 4> m_bloom;
  std::array<double, 4> m_bloomGain{}, m_bloomDamping{};
  double m_echoFrom = 4800, m_echoTo = 4800, m_echoFade = 1;
  double m_fast = 0, m_envelope = 0, m_duck = 1, m_autoGain = 1,
         m_inputEnergy = 0, m_wetEnergy = 0, m_dryEnergy = 0, m_crossEnergy = 0,
         m_autoGainTarget = 1;
  std::atomic<float> m_gainTelemetry{1};
  double m_scanX = 0, m_scanY = 0, m_scanPhase = 0;
  double m_outputGain = 1, m_outputGainStep = 0, m_doublerGain = 1,
         m_doublerGainStep = 0;
  double m_driveGain = 1, m_driveNorm = 1;
  double m_dcPole = .997;
  bool m_controlInitialized = false;
  modulation::dsp::Wander m_random;
  std::array<std::atomic<float>, 14 + MatrixStart> m_telemetry{};
};
} // namespace daw::plugins::morph
