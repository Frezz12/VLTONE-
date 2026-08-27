#pragma once

#include "Host/PluginTypes.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw::plugins {

/// Process-wide wake generation for plugin work that must run on the control
/// thread. A generation, rather than a consumable bool, lets multiple
/// EngineController instances observe the same request without stealing it
/// from each other. Individual nodes/instances coalesce bursts before bumping
/// this counter, so audio-thread notification storms pay one lock-free RMW.
class PluginMainThreadWork final {
public:
    static void request() noexcept {
        s_generation.fetch_add(1, std::memory_order_release);
    }

    static std::uint64_t generation() noexcept {
        return s_generation.load(std::memory_order_acquire);
    }

private:
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "plugin pending-work latches must remain realtime-safe");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "plugin control-thread wakes must remain realtime-safe");
    inline static std::atomic<std::uint64_t> s_generation{1};
};

/// Plugin → host notifications.
///
/// These can arrive from the plugin's audio thread as well as from the UI
/// thread, so every implementation must be non-blocking. The concrete one just
/// pushes onto a ring that the control thread drains on its next tick.
class PluginListener {
public:
    virtual ~PluginListener() = default;

    /// The plugin moved a parameter itself — from its own editor, or from an
    /// internal modulation source.
    virtual void onParameterChanged(std::uint32_t index, double plainValue) noexcept = 0;
    virtual void onParameterGesture(std::uint32_t index, bool begin) noexcept = 0;
    /// Reported latency changed, usually on a preset load. The host has to
    /// recompile the graph for delay compensation to follow.
    virtual void onLatencyChanged() noexcept = 0;
    /// The parameter list, bus layout or program set changed wholesale.
    virtual void onRestartRequested() noexcept = 0;
    /// The format explicitly requires destroying and recreating the component.
    virtual void onReloadRequested() noexcept = 0;
};

/// Editor → host notifications, delivered on the control thread.
///
/// Separate from `PluginListener` because these are UI-thread-only and a
/// plugin without an open editor never produces them, whereas the listener has
/// to stay valid for as long as the plugin is loaded.
class PluginEditorHost {
public:
    virtual ~PluginEditorHost() = default;

    /// The plugin wants its window to be this size, in logical pixels. The
    /// host is free to refuse, and must tell the plugin what it settled on.
    virtual void onEditorResized(std::uint32_t width, std::uint32_t height) noexcept = 0;
    /// The plugin closed its own editor. The host should destroy the window;
    /// it must not call `closeEditor` from inside this callback.
    virtual void onEditorClosed() noexcept = 0;
    virtual double contentScaleFactor() const noexcept { return 1.0; }
};

/// One loaded plugin, whatever its format.
///
/// The thread split is the same one all three formats impose, and is not
/// negotiable: `process` runs on whichever worker the graph scheduler picks and
/// must be realtime-safe, while everything else is control thread and may
/// allocate or block. The two must never overlap — that is what
/// `RealtimeEngine::RenderGate` is for.
class PluginInstance {
public:
    virtual ~PluginInstance() = default;

    virtual const PluginDescriptor& descriptor() const noexcept = 0;
    virtual void setListener(PluginListener* listener) noexcept = 0;

    // ── Control thread ──

    /// Ask for a layout; the plugin reports back what it actually accepted.
    virtual bool setBusLayout(const PluginBusLayout& wanted,
                              PluginBusLayout& accepted) = 0;
    virtual PluginBusLayout busLayout() const = 0;

    virtual bool activate(const PluginProcessInfo& info) = 0;
    virtual void deactivate() = 0;
    virtual bool isActive() const noexcept = 0;
    /// True only after the format accepted its processing transition. The
    /// default covers formats where active and processing are the same state.
    virtual bool isProcessing() const noexcept { return isActive(); }
    /// Bracket a run of blocks. Separate from activate because every format
    /// distinguishes "configured" from "currently rolling", and a plugin may
    /// clear its tails on the transition.
    virtual void startProcessing() = 0;
    virtual void stopProcessing() = 0;

    virtual std::span<const ParameterInfo> parameters() const noexcept = 0;
    virtual std::int32_t parameterIndexForId(std::string_view id) const noexcept = 0;
    virtual double parameterValue(std::uint32_t index) const noexcept = 0;
    virtual std::string parameterText(std::uint32_t index, double plainValue) const = 0;

    /// Opaque, format-defined. The host stores it and hands it back verbatim.
    virtual bool saveState(std::vector<std::uint8_t>& out) const = 0;
    virtual bool loadState(std::span<const std::uint8_t> state) = 0;

    /// Tell the plugin's *editor side* that the host moved a parameter.
    ///
    /// Control thread, and paired with the timestamped event the audio thread
    /// gets: in VST3 the processor and the controller are two objects, so an
    /// event delivered to the processor leaves the editor showing the old
    /// value and `parameterValue` reading it back wrong. Formats that keep one
    /// object need nothing here, hence the empty default.
    virtual void setParameterFromHost(std::uint32_t /*index*/, double /*plainValue*/) {}

    /// Run whatever work the plugin queued for the main thread. Plugins ask for
    /// this whether or not an editor is open, and a GUI that never gets it
    /// tends to freeze. Cheap when there is nothing to do.
    virtual void pumpMainThread() {}

    // ── Editor, control thread only ──

    /// False when the plugin ships no GUI of its own; the host then falls back
    /// to a panel built from `parameters()`.
    virtual bool hasEditor() const noexcept = 0;
    /// Parent the plugin's own view into a native window handle: an `NSView*`
    /// on macOS, an `HWND` on Windows — exactly what `QWidget::winId()` returns
    /// on each. `host` must outlive the editor.
    virtual bool openEditor(void* parentHandle, PluginEditorHost* host) = 0;
    virtual void closeEditor() = 0;
    virtual bool isEditorOpen() const noexcept = 0;
    /// The size the plugin wants, in logical pixels. False when it will not say.
    virtual bool editorSize(std::uint32_t& width, std::uint32_t& height) const = 0;
    virtual bool editorCanResize() const = 0;
    /// Offer a size; the plugin may snap it to something it prefers, which it
    /// writes back into `width`/`height`.
    virtual bool setEditorSize(std::uint32_t& width, std::uint32_t& height) = 0;

    // ── Audio thread, between startProcessing and stopProcessing ──

    virtual PluginProcessDisposition process(
        const PluginProcessContext& context) noexcept = 0;
    virtual void reset() noexcept = 0;

    /// Audio-thread sleep transition. CLAP brackets a sleeping interval with
    /// stop_processing/start_processing; formats without such a contract keep
    /// the no-op defaults and never return Sleep.
    virtual void sleepProcessing() noexcept {}
    virtual bool wakeProcessing() noexcept { return true; }
    /// Thread-safe plugin-originated wake request, consumed on the audio
    /// thread. CLAP implements host->request_process/request_flush with this.
    virtual bool takeProcessWakeRequest() noexcept { return false; }

    virtual std::uint32_t latencySamples() const noexcept = 0;
    virtual std::uint32_t tailSamples() const noexcept = 0;
};

/// Finds and opens plugins of one format.
class PluginFactory {
public:
    virtual ~PluginFactory() = default;

    virtual Format format() const noexcept = 0;
    virtual std::vector<std::string> defaultSearchPaths() const = 0;

    /// Cheap: filename and bundle shape only, never opens the module. Safe to
    /// run in-process over thousands of files.
    virtual std::vector<std::string> enumerateCandidates(const std::string& directory) const = 0;

    /// Expensive and able to crash: this opens third-party code. Only the
    /// out-of-process scanner is meant to call it.
    virtual std::vector<PluginDescriptor> inspect(const std::string& path) const = 0;

    virtual std::unique_ptr<PluginInstance> create(const PluginDescriptor& descriptor) = 0;
};

/// Factories for every format compiled into this build.
std::vector<PluginFactory*> availableFactories();
PluginFactory* factoryFor(Format format) noexcept;

} // namespace daw::plugins
