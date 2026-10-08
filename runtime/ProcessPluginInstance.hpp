#pragma once

#include "PluginProcess.hpp"

namespace daw::plugins {

/// Format-independent graph adapter. All foreign calls stay in daw_plugin_host.
/// Native editors are owned by the helper, never embedded across processes.
class ProcessPluginInstance final : public PluginInstance, private EventSink {
public:
    static std::unique_ptr<ProcessPluginInstance> create(
        const PluginDescriptor& descriptor, const std::string& executable, std::string* error = nullptr);
    const PluginDescriptor& descriptor() const noexcept override { return m_process.metadata().descriptor; }
    void setListener(PluginListener* listener) noexcept override { m_listener = listener; }
    PitchCapabilities pitchCapabilities() const noexcept override { return m_process.metadata().pitch; }
    bool hasDeferredProcess() const noexcept override { return true; }

    bool setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override { return m_process.metadata().buses; }
    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override;
    bool isProcessing() const noexcept override;
    bool hasFailed() const noexcept override { return failure() != PluginProcessFailure::None; }
    void startProcessing() override;
    void stopProcessing() override;
    std::span<const ParameterInfo> parameters() const noexcept override { return m_process.metadata().parameters; }
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override;
    double parameterValue(std::uint32_t index) const noexcept override;
    std::string parameterText(std::uint32_t index, double value) const override;
    bool supportsState() const noexcept override { return m_process.metadata().supportsState; }
    bool saveState(std::vector<std::uint8_t>& out) const override;
    bool loadState(std::span<const std::uint8_t> state) override;
    bool parameterNeedsStateRestore(std::uint32_t index) const noexcept override {
        return hasFailed() && index < m_valueCount && m_checkpointEdits[index].load(std::memory_order_relaxed);
    }
    std::vector<PluginEvent> pendingParameterEvents() override;
    void setParameterFromHost(std::uint32_t index, double value) override;
    void pumpMainThread() override;
    bool serviceOfflineRestart() override;

    bool hasEditor() const noexcept override { return m_process.metadata().hasEditor; }
    bool openEditor(void*, PluginEditorHost*) override;
    void closeEditor() override { m_process.requestEditor(false); }
    bool isEditorOpen() const noexcept override { return m_process.editorStatus() == 1; }
    std::uint32_t editorStatus() const noexcept { return m_process.editorStatus(); }
    void setAutomationShortcutEnabled(bool enabled) { m_process.setAutomationShortcutEnabled(enabled); }
    std::uint32_t takeAutomationShortcuts() { return m_process.takeAutomationShortcuts(); }
    bool editorSize(std::uint32_t&, std::uint32_t&) const override { return false; }
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t&, std::uint32_t&) override { return false; }

    PluginProcessDisposition process(const PluginProcessContext&) noexcept override;
    bool beginProcess(const PluginProcessContext&, PluginProcessDisposition&) noexcept override;
    bool finishProcess(const PluginProcessContext&, PluginProcessDisposition&, bool expired) noexcept override;
    void reset() noexcept override { m_reset = true; }
    void resetForTransport() noexcept override;
    bool supportsRealtimeReset() const noexcept override { return m_process.metadata().realtimeReset; }
    std::uint32_t latencySamples() const noexcept override { return m_process.latencySamples(); }
    std::uint32_t tailSamples() const noexcept override { return m_process.tailSamples(); }
    bool tailSamplesKnown() const noexcept override { return m_process.tailSamplesKnown(); }

    PluginProcessFailure failure() const noexcept { return m_process.failure(); }
    std::string error() const { return m_process.error(); }
    std::uint64_t processId() const noexcept { return m_process.processId(); }
    /// Control thread with rendering stopped. Re-preparation of the owning
    /// PluginNode is required after restart (latency, MIDI and bypass history).
    bool restart();
    bool service() { return m_process.service(); }
    bool checkHealth() { return m_process.checkHealth(); }
    bool hasMainThreadWork() const noexcept { return m_process.hasMainThreadWork(); }
    struct Recovery {
        PluginDescriptor descriptor;
        std::string executable;
        std::vector<std::uint8_t> state;
        bool hasState = false;
        std::vector<std::pair<std::string, double>> parameters;
    };
    Recovery recovery() const;
    static std::unique_ptr<ProcessPluginInstance> recover(const Recovery& snapshot);

private:
    explicit ProcessPluginInstance(const std::string& executable);
    void syncValues(bool newCheckpoint = false);
    void push(const PluginEvent& event) noexcept override;
    void publishNotices() noexcept;
    mutable PluginProcess m_process;
    PluginListener* m_listener = nullptr;
    std::unique_ptr<std::atomic<double>[]> m_values;
    std::unique_ptr<std::atomic<double>[]> m_confirmedValues;
    std::unique_ptr<std::atomic<bool>[]> m_checkpointEdits;
    std::size_t m_valueCount = 0;
    std::vector<PluginEvent> m_restore, m_events;
    EventSink* m_output = nullptr;
    bool m_reset = false;
};

} // namespace daw::plugins
