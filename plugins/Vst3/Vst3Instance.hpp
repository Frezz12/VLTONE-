#pragma once

#include "Host/PluginInstance.hpp"
#include "Common/LockFreeQueue.hpp"
#include "Vst3/Vst3Module.hpp"
#include "Vst3/Vst3Support.hpp"

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>

#include <atomic>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace daw::plugins {

namespace vst3 {
/// A VST3 class id as 32 hex characters — what a project file stores.
std::string uidToString(const char* cid);
bool uidFromString(const std::string& text, char* cid);
} // namespace vst3

/// A VST3 plugin behind the format-agnostic interface.
///
/// The awkward part of VST3, and the thing a naive host gets wrong, is that a
/// plugin is **two objects**: an `IComponent` that processes audio and an
/// `IEditController` that owns the parameters and the editor. They are
/// sometimes the same object and sometimes not, they each need initialising,
/// and when they are separate they only stay in step if the host wires their
/// `IConnectionPoint`s to each other. A host that skips that gets an editor
/// whose knobs do nothing.
class Vst3Instance final : public PluginInstance {
public:
    Vst3Instance(std::shared_ptr<Vst3Module> module,
                 Steinberg::Vst::IComponent* component, PluginDescriptor descriptor);
    ~Vst3Instance() override;

    /// Second half of construction. Separate from the constructor because it
    /// can fail, and because it needs the factory to create the controller when
    /// the plugin keeps it in a class of its own.
    bool initialize(Steinberg::IPluginFactory* factory);

    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
    void setListener(PluginListener* listener) noexcept override {
        m_listener.store(listener, std::memory_order_release);
    }

    bool setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) override;
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
    std::string parameterText(std::uint32_t index, double plainValue) const override;

    bool saveState(std::vector<std::uint8_t>& out) const override;
    bool loadState(std::span<const std::uint8_t> state) override;

    void setParameterFromHost(std::uint32_t index, double plainValue) override;
    void pumpMainThread() override;

    bool hasEditor() const noexcept override;
    bool openEditor(void* parentHandle, PluginEditorHost* host) override;
    void closeEditor() override;
    bool isEditorOpen() const noexcept override { return m_view != nullptr; }
    bool editorSize(std::uint32_t& width, std::uint32_t& height) const override;
    bool editorCanResize() const override;
    bool setEditorSize(std::uint32_t& width, std::uint32_t& height) override;

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
    void readParameters();
    void readMidiMappings();
    void readBuses();
    bool allocateAndActivateBuses(std::uint32_t maxBlockSize);
    void refreshLatency();
    /// Normalised 0…1 (what VST3 speaks) ↔ the plugin's own units (what this
    /// host's API and its automation lanes speak).
    double toPlain(std::uint32_t index, double normalized) const;
    double toNormalized(std::uint32_t index, double plain) const;

    struct QueuedEdit {
        std::uint32_t parameterIndex = 0;
        double normalized = 0.0;
    };
    struct ParameterConversion {
        double minimum = 0.0;
        double maximum = 1.0;
        bool linear = true;
        bool ascending = true;
        std::vector<float> plainSamples;
    };

    std::shared_ptr<Vst3Module> m_module;
    PluginDescriptor m_descriptor;

    Steinberg::IPtr<Steinberg::Vst::IComponent> m_component;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> m_processor;
    Steinberg::IPtr<Steinberg::Vst::IEditController> m_controller;
    Steinberg::IPtr<Steinberg::IPlugView> m_view;
    mutable Steinberg::IPtr<Steinberg::IPlugView> m_probeView;
    /// Set when the controller is a separate object we connected; the two
    /// connection points have to be disconnected in the reverse order.
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> m_componentPoint;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> m_controllerPoint;

    vst3::HostApplication m_hostContext;
    vst3::ComponentHandler m_handler;
    vst3::PlugFrame m_frame;
    Steinberg::IPtr<vst3::ParameterChanges> m_inputChanges;
    Steinberg::IPtr<vst3::ParameterChanges> m_outputChanges;
    Steinberg::IPtr<vst3::EventList> m_events;
    Steinberg::IPtr<vst3::EventList> m_outputEvents;

    std::vector<ParameterInfo> m_parameters;
    /// VST3 addresses parameters by an opaque `ParamID`, unrelated to the index
    /// they are enumerated in. Kept index-parallel with `m_parameters`.
    std::vector<Steinberg::Vst::ParamID> m_parameterIds;
    std::vector<ParameterConversion> m_parameterConversions;
    std::unordered_map<Steinberg::Vst::ParamID, std::uint32_t> m_parameterIndexById;
    std::array<std::array<std::int32_t, Steinberg::Vst::kCountCtrlNumber>, 16>
        m_midiParameterMappings{};
    engine::LockFreeSPSCQueue<QueuedEdit, 1024> m_editorEdits;
    /// A preset may replace hundreds of controller values at once and report
    /// only kParamValuesChanged. These normalized values are captured while
    /// rendering is gated, then delivered to the processor in one audio block.
    std::vector<Steinberg::Vst::ParamValue> m_pendingParameterValues;
    std::atomic<bool> m_parameterSyncPending{false};

    std::atomic<PluginListener*> m_listener{nullptr};
    std::atomic<std::uint32_t> m_latency{0};
    std::atomic<std::uint32_t> m_tail{0};
    std::atomic<bool> m_restartLatency{false};
    std::atomic<Steinberg::int32> m_restartFlags{0};

    PluginEditorHost* m_editorHost = nullptr;
    bool m_active = false;
    bool m_processing = false;
    std::uint32_t m_maxBlockSize = 0;
    double m_sampleRate = 48000.0;
    bool m_separateController = false;

    void captureControllerValuesForProcessor();

    /// Same rule as CLAP: every bus the plugin declared has to be handed over,
    /// because the plugin indexes `data.inputs[i]` itself.
    struct AudioBus {
        std::uint32_t channels = 0;
        std::vector<float*> pointers;
        std::vector<float> storage;
    };
    std::vector<AudioBus> m_inputBuses;
    std::vector<AudioBus> m_outputBuses;
    std::vector<Steinberg::Vst::AudioBusBuffers> m_inputBufferDescs;
    std::vector<Steinberg::Vst::AudioBusBuffers> m_outputBufferDescs;
};

} // namespace daw::plugins
