#pragma once

#include "Host/PluginInstance.hpp"
#include "Vst/VstModule.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace daw::plugins {

class VstInstance final : public PluginInstance {
public:
    static std::unique_ptr<VstInstance> create(
        std::shared_ptr<VstModule> module, const PluginDescriptor& descriptor,
        VstInt32 currentId);
    ~VstInstance() override;

    const PluginDescriptor& descriptor() const noexcept override {
        return m_descriptor;
    }
    void setListener(PluginListener* listener) noexcept override {
        m_listener.store(listener, std::memory_order_release);
    }

    bool setBusLayout(const PluginBusLayout& wanted,
                      PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override;
    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
    bool isProcessing() const noexcept override { return m_processing; }
    void startProcessing() override;
    void stopProcessing() override;

    std::span<const ParameterInfo> parameters() const noexcept override {
        return m_parameters;
    }
    std::int32_t parameterIndexForId(std::string_view id) const noexcept override;
    double parameterValue(std::uint32_t index) const noexcept override;
    std::string parameterText(std::uint32_t index,
                              double plainValue) const override;
    void setParameterFromHost(std::uint32_t index,
                              double plainValue) override;

    bool saveState(std::vector<std::uint8_t>& out) const override;
    bool loadState(std::span<const std::uint8_t> state) override;

    bool hasEditor() const noexcept override;
    bool openEditor(void* parentHandle, PluginEditorHost* host) override;
    void closeEditor() override;
    bool isEditorOpen() const noexcept override { return m_editorOpen; }
    bool editorSize(std::uint32_t& width,
                    std::uint32_t& height) const override;
    bool editorCanResize() const override { return false; }
    bool setEditorSize(std::uint32_t& width,
                       std::uint32_t& height) override;
    void pumpMainThread() override;

    PluginProcessDisposition process(
        const PluginProcessContext& context) noexcept override;
    void reset() noexcept override;

    std::uint32_t latencySamples() const noexcept override {
        return m_latency.load(std::memory_order_relaxed);
    }
    std::uint32_t tailSamples() const noexcept override {
        return m_tail.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kMaxBlockEvents = 2048;
    static constexpr VstInt32 kAudioMasterIdle = 3;
    static constexpr VstInt32 kAudioMasterIoChanged = 13;

    struct EventsBlock {
        VstInt32 numEvents = 0;
        VstIntPtr reserved = 0;
        std::array<VstEvent*, kMaxBlockEvents> events{};
    };

    VstInstance(std::shared_ptr<VstModule> module,
                PluginDescriptor descriptor, VstInt32 currentId);
    bool initialize();
    void readParameters();
    void refreshLatencyAndTail();
    void prepareTime(const PluginProcessContext& context,
                     std::uint32_t segmentStart) noexcept;
    void prepareAudio(const PluginProcessContext& context,
                      std::uint32_t start) noexcept;
    void sendMidi(std::span<const PluginEvent> events, std::uint32_t start,
                  std::uint32_t end) noexcept;
    void processSegment(const PluginProcessContext& context,
                        std::uint32_t start, std::uint32_t end) noexcept;
    void receiveEvents(const VstEvents* events) noexcept;

    static VstIntPtr hostDispatch(vst::HostContext& host, AEffect* effect,
                                  VstInt32 opcode, VstInt32 index,
                                  VstIntPtr value, void* ptr,
                                  float opt) noexcept;

    std::shared_ptr<VstModule> m_module;
    PluginDescriptor m_descriptor;
    vst::HostContext m_host;
    AEffect* m_effect = nullptr;
    std::vector<ParameterInfo> m_parameters;
    std::atomic<PluginListener*> m_listener{nullptr};
    std::atomic<std::uint32_t> m_latency{0};
    std::atomic<std::uint32_t> m_tail{0};
    std::atomic<bool> m_restartPending{false};
    std::atomic<bool> m_latencyPending{false};
    std::atomic<bool> m_idlePending{false};
    std::atomic<std::uint32_t> m_requestedWidth{0};
    std::atomic<std::uint32_t> m_requestedHeight{0};

    bool m_active = false;
    bool m_processing = false;
    bool m_editorOpen = false;
    bool m_offline = false;
    double m_sampleRate = 48000.0;
    std::uint32_t m_maxBlockSize = 0;
    PluginEditorHost* m_editorHost = nullptr;

    std::vector<float> m_zeroInput;
    std::vector<float> m_discardOutput;
    std::vector<float*> m_inputPointers;
    std::vector<float*> m_outputPointers;
    std::array<VstMidiEvent, kMaxBlockEvents> m_midiEvents{};
    EventsBlock m_events;
    VstTimeInfo m_timeInfo{};
    const PluginProcessContext* m_currentProcess = nullptr;
    EventSink* m_outputSink = nullptr;
    std::uint32_t m_segmentStart = 0;
};

} // namespace daw::plugins
