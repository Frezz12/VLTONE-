#pragma once

#include "Clap/ClapModule.hpp"
#include "Host/PluginInstance.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace daw::plugins {

/// A CLAP plugin behind the format-agnostic interface.
///
/// Event conversion happens here rather than in `PluginNode`, because it is the
/// one part that is genuinely format-specific: CLAP wants one ordered stream of
/// tagged structs, VST3 wants an `IEventList` plus an `IParameterChanges`, and
/// AU wants raw MIDI bytes.
class ClapInstance final : public PluginInstance {
public:
    ClapInstance(std::shared_ptr<ClapModule> module, const clap_plugin_t* plugin,
                 PluginDescriptor descriptor);
    ~ClapInstance() override;

    const PluginDescriptor& descriptor() const noexcept override { return m_descriptor; }
    void setListener(PluginListener* listener) noexcept override {
        m_listener.store(listener, std::memory_order_release);
    }

    bool setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) override;
    PluginBusLayout busLayout() const override;

    bool activate(const PluginProcessInfo& info) override;
    void deactivate() override;
    bool isActive() const noexcept override { return m_active; }
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

    bool hasEditor() const noexcept override;
    bool openEditor(void* parentHandle, PluginEditorHost* host) override;
    void closeEditor() override;
    bool isEditorOpen() const noexcept override { return m_editorOpen; }
    bool editorSize(std::uint32_t& width, std::uint32_t& height) const override;
    bool editorCanResize() const override;
    bool setEditorSize(std::uint32_t& width, std::uint32_t& height) override;

    PluginProcessDisposition process(
        const PluginProcessContext& context) noexcept override;
    void reset() noexcept override;
    void sleepProcessing() noexcept override { stopProcessing(); }
    bool wakeProcessing() noexcept override {
        startProcessing();
        return m_processing;
    }
    bool takeProcessWakeRequest() noexcept override {
        return m_processRequested.exchange(false, std::memory_order_acq_rel);
    }

    std::uint32_t latencySamples() const noexcept override {
        return m_latency.load(std::memory_order_relaxed);
    }
    std::uint32_t tailSamples() const noexcept override {
        return m_tail.load(std::memory_order_relaxed);
    }

    /// Control thread: re-read what the plugin reports. Called after a preset
    /// load, and when the plugin says its latency moved.
    void refreshLatency();
    void refreshTail() noexcept;
    /// Run the plugin's queued main-thread work. CLAP plugins ask for this
    /// whether or not an editor is open.
    void pumpMainThread() override;

    /// Fill in a `clap_host_t`. Static because the factory must hand one to
    /// `create_plugin` before any instance exists to own it.
    static void fillHost(clap_host_t& host, void* hostData) noexcept;

    /// Take ownership of that host struct. It has to stay at a fixed address
    /// for as long as the plugin lives, since the plugin holds the pointer.
    void adoptHost(std::unique_ptr<clap_host_t> host) noexcept {
        m_host = std::move(host);
    }

private:
    void readParameters();
    void readDescriptorPorts();
    /// Size the per-bus buffers for `maxBlockSize`. Main thread, from
    /// `activate` — `process` must not allocate.
    void allocateBuses(std::uint32_t maxBlockSize);

    // ── clap_host callbacks ──
    static const void* hostGetExtension(const clap_host_t* host, const char* id) noexcept;
    static void hostRequestRestart(const clap_host_t* host) noexcept;
    static void hostRequestProcess(const clap_host_t* host) noexcept;
    static void hostRequestCallback(const clap_host_t* host) noexcept;
    static void hostParamsRescan(const clap_host_t* host,
                                 clap_param_rescan_flags flags) noexcept;
    static void hostParamsClear(const clap_host_t* host, clap_id parameter,
                                clap_param_clear_flags flags) noexcept;
    static void hostParamsRequestFlush(const clap_host_t* host) noexcept;

    // ── clap_host_gui / clap_host_latency callbacks ──
    static void hostGuiResizeHintsChanged(const clap_host_t* host) noexcept;
    static bool hostGuiRequestResize(const clap_host_t* host, std::uint32_t width,
                                     std::uint32_t height) noexcept;
    static bool hostGuiRequestShow(const clap_host_t* host) noexcept;
    static bool hostGuiRequestHide(const clap_host_t* host) noexcept;
    static void hostGuiClosed(const clap_host_t* host, bool wasDestroyed) noexcept;
    static void hostLatencyChanged(const clap_host_t* host) noexcept;
    static void hostTailChanged(const clap_host_t* host) noexcept;

    /// The window API string this platform's plugins are parented into.
    static const char* editorApi() noexcept;

    std::shared_ptr<ClapModule> m_module;
    const clap_plugin_t* m_plugin = nullptr;
    PluginDescriptor m_descriptor;

    const clap_plugin_params_t* m_params = nullptr;
    const clap_plugin_state_t* m_state = nullptr;
    const clap_plugin_latency_t* m_latencyExt = nullptr;
    const clap_plugin_tail_t* m_tailExt = nullptr;
    const clap_plugin_audio_ports_t* m_audioPorts = nullptr;
    const clap_plugin_gui_t* m_gui = nullptr;
    const clap_plugin_render_t* m_render = nullptr;

    std::vector<ParameterInfo> m_parameters;
    /// CLAP addresses parameters by an opaque `clap_id`, not by index, and the
    /// two are unrelated. Kept index-parallel with `m_parameters`.
    std::vector<clap_id> m_parameterIds;

    /// Owned by pointer, not by value: the plugin was handed this address at
    /// creation and keeps it, so it must not move when the instance does.
    std::unique_ptr<clap_host_t> m_host;
    std::atomic<PluginListener*> m_listener{nullptr};
    std::atomic<std::uint32_t> m_latency{0};
    std::atomic<std::uint32_t> m_tail{0};
    std::atomic<bool> m_processRequested{false};
    /// Set from any thread by `request_callback`, drained by `pumpMainThread`.
    std::atomic<bool> m_callbackRequested{false};
    std::atomic<clap_param_rescan_flags> m_paramRescanFlags{0};

    bool m_active = false;
    bool m_processing = false;
    bool m_editorOpen = false;
    /// Non-owning, valid only while the editor is open.
    PluginEditorHost* m_editorHost = nullptr;

    /// Scratch for one block's events, in CLAP's own layout. Sized in
    /// `activate` so `process` allocates nothing.
    /// Every PluginEvent becomes exactly one of these. A union keeps the
    /// realtime scratch proportional to the total block-event limit rather
    /// than reserving that limit separately for each CLAP event kind.
    union InputEventStorage {
        clap_event_param_value_t parameter;
        clap_event_note_t note;
        clap_event_note_expression_t noteExpression;
        clap_event_midi_t midi;
    };
    std::vector<InputEventStorage> m_inputEventScratch;
    std::vector<const clap_event_header_t*> m_eventOrder;

    /// One of the plugin's declared audio buses, with somewhere for its samples
    /// to go when the host has nothing to put there.
    ///
    /// The host must hand the plugin **every** bus it declared, not just the
    /// main one: the plugin indexes `process->audio_inputs[i]` directly, so a
    /// short array is an out-of-bounds read inside third-party code. Most real
    /// effects declare a sidechain input they are perfectly happy to receive as
    /// silence, and passing one bus to a plugin that declared two is a
    /// segfault, not a missing feature.
    struct AudioBus {
        std::uint32_t channels = 0;
        /// Exactly `channels` entries, so the plugin can read them all.
        std::vector<float*> pointers;
        /// Backing for channels the host did not supply. Planar, per channel.
        std::vector<float> storage;
    };
    std::vector<AudioBus> m_inputBuses;
    std::vector<AudioBus> m_outputBuses;
    /// Handed to the plugin as-is; only bus 0's `data32` changes per block.
    std::vector<clap_audio_buffer_t> m_inputBufferDescs;
    std::vector<clap_audio_buffer_t> m_outputBufferDescs;
    std::uint32_t m_maxBlockSize = 0;
};

} // namespace daw::plugins
