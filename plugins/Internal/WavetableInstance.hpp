#pragma once
#include "Host/PluginInstance.hpp"
#include "Internal/WavetableProgram.hpp"
#include <memory>

namespace daw::plugins::wavetable {
struct Telemetry { unsigned voices = 0; float peak = 0; unsigned latency = 0; };
class WavetableInstance final : public PluginInstance {
public:
    WavetableInstance();
    ~WavetableInstance() override;
    static const PluginDescriptor& staticDescriptor();
    const PluginDescriptor& descriptor() const noexcept override { return staticDescriptor(); }
    void setListener(PluginListener* listener) noexcept override { m_listener=listener; }
    PitchCapabilities pitchCapabilities() const noexcept override { return {true,true,true,true}; }
    bool supportsOfflinePipelining() const noexcept override { return true; }
    bool setBusLayout(const PluginBusLayout&, PluginBusLayout& accepted) override { accepted=busLayout();return true; }
    PluginBusLayout busLayout() const override { return {{},{2}}; }
    bool activate(const PluginProcessInfo&) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    bool isProcessing() const noexcept override { return m_processing; }
    void startProcessing() override { m_processing=true; }
    void stopProcessing() override { m_processing=false;reset(); }
    std::span<const ParameterInfo> parameters() const noexcept override { return parameterTable(); }
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override { return parameterIndex(id); }
    double parameterValue(std::uint32_t) const noexcept override;
    std::string parameterText(std::uint32_t,double) const override;
    void setParameterFromHost(std::uint32_t,double) override;
    bool saveState(std::vector<std::uint8_t>&) const override;
    bool loadState(std::span<const std::uint8_t>) override;
    State state() const;
    bool setState(const State&);
    Telemetry telemetry() const noexcept;
    bool needsMainThreadPump() const noexcept override { return false; }
    bool hasEditor() const noexcept override { return false; }
    bool openEditor(void*,PluginEditorHost*) override { return false; }
    void closeEditor() override {}
    bool isEditorOpen() const noexcept override { return false; }
    bool editorSize(std::uint32_t&,std::uint32_t&) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t&,std::uint32_t&) override { return false; }
    PluginProcessDisposition process(const PluginProcessContext&) noexcept override;
    void reset() noexcept override;
    std::uint32_t latencySamples() const noexcept override;
    std::uint32_t tailSamples() const noexcept override;
    bool tailSamplesKnown() const noexcept override { return false; }
private:
    struct Impl;
    std::unique_ptr<Impl> m;
    PluginListener* m_listener=nullptr;
    bool m_active=false,m_processing=false;
};
}
