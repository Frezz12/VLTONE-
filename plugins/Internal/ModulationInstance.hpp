#pragma once

#include "Host/PluginInstance.hpp"
#include "Internal/ModulationDsp.hpp"
#include <array>
#include <atomic>
#include <span>
#include <string_view>

namespace daw::plugins::modulation {
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free);

enum class Kind { Doubler, Chorus, Flanger, Phaser };
using Values = std::array<double, 4>;
struct FactoryPreset {
    std::string_view name;
    Values values;
};
std::span<const ParameterInfo> parameterTable(Kind kind) noexcept;
std::span<const FactoryPreset> factoryPresets(Kind kind) noexcept;
const PluginDescriptor &descriptorFor(Kind kind) noexcept;
bool isModulationUid(std::string_view uid) noexcept;

struct Telemetry {
    std::uint64_t serial = 0;
    float level = 0, width = 0, correlation = 1;
    std::array<float, 4> positions{};
};

// Only host plumbing is shared. Each final instance owns its DSP, topology,
// parameter table and state identity. Qt never enters the processing path.
class ModulationInstance : public PluginInstance {
  public:
    explicit ModulationInstance(Kind kind);
    Kind kind() const noexcept { return m_kind; }
    const PluginDescriptor &descriptor() const noexcept override { return descriptorFor(m_kind); }
    void setListener(PluginListener *listener) noexcept override { m_listener = listener; }
    bool setBusLayout(const PluginBusLayout &, PluginBusLayout &) override;
    PluginBusLayout busLayout() const override { return m_layout; }
    bool activate(const PluginProcessInfo &) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    bool isProcessing() const noexcept override { return m_processing; }
    void startProcessing() override { m_processing = true; }
    void stopProcessing() override { m_processing = false; }
    std::span<const ParameterInfo> parameters() const noexcept override {
        return parameterTable(m_kind);
    }
    std::int32_t parameterIndexForId(std::string_view) const noexcept override;
    double parameterValue(std::uint32_t) const noexcept override;
    std::string parameterText(std::uint32_t, double) const override;
    void setParameterFromHost(std::uint32_t, double) override;
    bool saveState(std::vector<std::uint8_t> &) const override;
    bool loadState(std::span<const std::uint8_t>) override;
    void setPresetReference(std::string kind, std::string name);
    std::pair<std::string, std::string> presetReference() const {
        return {m_presetKind, m_presetName};
    }
    bool hasEditor() const noexcept override { return false; }
    bool openEditor(void *, PluginEditorHost *) override { return false; }
    void closeEditor() override {}
    bool isEditorOpen() const noexcept override { return false; }
    bool editorSize(std::uint32_t &, std::uint32_t &) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t &, std::uint32_t &) override { return false; }
    PluginProcessDisposition process(const PluginProcessContext &) noexcept override;
    void reset() noexcept override;
    std::uint32_t latencySamples() const noexcept override { return 0; }
    std::uint32_t tailSamples() const noexcept override;
    Telemetry telemetry() const noexcept;

  protected:
    virtual void prepareDsp() = 0;
    virtual void resetDsp() noexcept = 0;
    virtual std::array<double, 2> sample(double l, double r, bool stereo) noexcept = 0;
    double rate = 48000;
    Values smoothed{};
    std::array<float, 4> positions{};
    std::uint32_t controlPhase = 0;

  private:
    void render(const PluginProcessContext &, std::uint32_t begin, std::uint32_t end) noexcept;
    Kind m_kind;
    PluginListener *m_listener = nullptr;
    PluginBusLayout m_layout{{2}, {2}};
    bool m_active = false, m_processing = false;
    std::array<std::atomic<double>, 4> m_values{};
    Values m_smoothing{};
    double m_meterPole = 0, m_l2 = 0, m_r2 = 0, m_lr = 0;
    std::array<std::atomic<float>, 7> m_meters{};
    std::atomic<std::uint64_t> m_meterSerial{0};
    std::string m_presetKind = "factory", m_presetName;
};

class DoublerInstance final : public ModulationInstance {
  public:
    DoublerInstance() : ModulationInstance(Kind::Doubler) {}

  private:
    void prepareDsp() override;
    void resetDsp() noexcept override;
    std::array<double, 2> sample(double, double, bool) noexcept override;
    dsp::Delay m_delay;
    std::array<dsp::Wander, 4> m_wander;
    std::array<double, 4> m_reads{};
    dsp::Tone m_tone;
    double m_midEnergy = 0, m_sideEnergy = 0, m_addEnergy = 0, m_gain = 0;
    double m_fast = 0, m_slow = 0, m_duck = 1;
    double m_energyPole = 0, m_gainAttack = 0, m_gainRelease = 0;
    double m_fastPole = 0, m_slowPole = 0, m_duckRelease = 0;
};

class ChorusInstance final : public ModulationInstance {
  public:
    ChorusInstance() : ModulationInstance(Kind::Chorus) {}

  private:
    void prepareDsp() override;
    void resetDsp() noexcept override;
    std::array<double, 2> sample(double, double, bool) noexcept override;
    std::array<dsp::Delay, 2> m_delays;
    std::array<dsp::Tone, 2> m_tones;
    std::array<double, 4> m_phases{}, m_reads{};
};

class FlangerInstance final : public ModulationInstance {
  public:
    FlangerInstance() : ModulationInstance(Kind::Flanger) {}

  private:
    void prepareDsp() override;
    void resetDsp() noexcept override;
    std::array<double, 2> sample(double, double, bool) noexcept override;
    std::array<dsp::Delay, 2> m_delays;
    std::array<dsp::Tone, 2> m_tones, m_feedbackTones;
    std::array<double, 2> m_feedback{};
    double m_phase = 0;
};

class PhaserInstance final : public ModulationInstance {
  public:
    PhaserInstance() : ModulationInstance(Kind::Phaser) {}

  private:
    void prepareDsp() override {}
    void resetDsp() noexcept override;
    std::array<double, 2> sample(double, double, bool) noexcept override;
    std::array<std::array<dsp::Allpass, 6>, 2> m_filters{};
    std::array<dsp::Tone, 2> m_tones;
    std::array<double, 2> m_feedback{};
    double m_phase = 0;
};
} // namespace daw::plugins::modulation
