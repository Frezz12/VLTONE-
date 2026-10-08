#pragma once

#include "Host/PluginInstance.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace daw::plugins {

inline constexpr auto kPluginControlTimeout = std::chrono::seconds(10);
inline constexpr auto kPluginCloseTimeout = std::chrono::milliseconds(100);

/// A control-thread scope observes actual nested RPCs, never a timer or DSP
/// heartbeat. Zero remaining ends the RPC; completed means its reply arrived.
class ScopedPluginControlProgress final {
public:
    using Callback = void (*)(void*, std::chrono::nanoseconds, bool) noexcept;
    ScopedPluginControlProgress(void* context, Callback callback) noexcept
        : m_context(context), m_callback(callback), m_previous(s_current) { s_current = this; }
    ~ScopedPluginControlProgress() { s_current = m_previous; }
    ScopedPluginControlProgress(const ScopedPluginControlProgress&) = delete;
    ScopedPluginControlProgress& operator=(const ScopedPluginControlProgress&) = delete;
    static void report(std::chrono::nanoseconds remaining, bool completed) noexcept {
        if (s_current) s_current->m_callback(s_current->m_context, remaining, completed);
    }
private:
    void* m_context;
    Callback m_callback;
    ScopedPluginControlProgress* m_previous;
    inline static thread_local ScopedPluginControlProgress* s_current = nullptr;
};

/// Capacity is fixed for the lifetime of a child. Exceeding it fails explicitly;
/// neither channels nor sample-accurate events are silently truncated.
struct PluginProcessLimits {
    std::uint32_t frames = 512;
    std::uint32_t channels = 8;
    std::uint32_t events = 32768;
    std::uint32_t controlBytes = 1024 * 1024;
};

enum class PluginProcessFailure : std::uint32_t {
    None, Launch, Protocol, Exited, ControlTimeout, AudioDeadline,
    InvalidBlock, InvalidOutput, Processor, EditorUnresponsive,
};

struct PluginProcessMetadata {
    PluginDescriptor descriptor;
    PluginBusLayout buses;
    std::vector<ParameterInfo> parameters;
    std::vector<double> values;
    std::uint32_t latency = 0;
    std::uint32_t tail = 0;
    bool tailKnown = false;
    bool supportsState = false;
    bool active = false;
    bool processing = false;
    bool realtimeReset = false;
    bool hasEditor = false;
    PitchCapabilities pitch;
};

/// One native plugin in a disposable process. This is the transport boundary,
/// not a synchronous PluginInstance::process adapter. A graph integration must
/// submit ready work, run independent nodes, poll, and expire at a common block
/// deadline. No lifecycle/control call may overlap an outstanding audio block.
/// Control methods (including destruction) belong off the realtime thread.
/// ProcessPluginInstance integrates this transport with the graph. Editor
/// requests and notifications use an independent nonblocking mailbox.
class PluginProcess final {
public:
    enum class Poll { Pending, Complete, Failed };
    enum Notification : std::uint32_t {
        LatencyChanged = 1, RestartRequested = 2, ReloadRequested = 4,
        StateChanged = 8,
    };

    explicit PluginProcess(std::string executable, PluginProcessLimits limits = {},
        std::chrono::milliseconds controlTimeout = kPluginControlTimeout);
    ~PluginProcess();
    PluginProcess(const PluginProcess&) = delete;
    PluginProcess& operator=(const PluginProcess&) = delete;

    bool load(const PluginDescriptor& descriptor);
    /// Grow only, while inactive. Recreates the child after capturing state.
    /// Parameter-only plugins return their values through metadata before this
    /// call; their PluginInstance adapter replays them on the first new block.
    bool reserve(PluginProcessLimits limits);
    PluginProcessLimits limits() const noexcept;
    bool setBusLayout(const PluginBusLayout& layout);
    bool activate(const PluginProcessInfo& info);
    bool startProcessing();
    bool stopProcessing();
    bool deactivate();
    bool reset();
    bool pumpMainThread();
    bool refreshMetadata();
    bool serviceOfflineRestart();
    bool setParameterFromHost(std::uint32_t index, double value);
    std::vector<PluginEvent> pendingParameterEvents();
    bool saveState(std::vector<std::uint8_t>& out);
    bool loadState(std::span<const std::uint8_t> state);
    /// Fresh child, last successful opaque checkpoint, layout and processing
    /// configuration. Uncheckpointed plugin-private edits cannot be recovered.
    bool restart();
    void close();
    /// Detect an exited child and reap a faulted one. Control thread only;
    /// the renderer must have stopped polling this instance first.
    bool service();

    const PluginProcessMetadata& metadata() const noexcept;
    PluginProcessFailure failure() const noexcept;
    std::string error() const;
    std::uint64_t processId() const noexcept;
    bool hasCheckpoint() const noexcept;
    bool copyCheckpoint(std::vector<std::uint8_t>& out) const;
    const std::string& executable() const noexcept;
    /// Nonblocking control-thread health check; leaves mapping/reaping to service().
    bool checkHealth();
    bool hasMainThreadWork() const noexcept;
    bool popNotification(PluginEvent& event) noexcept;
    void requestEditor(bool open) noexcept;
    /// 0 closed, 1 open, 2 rejected, 3 request awaiting the GUI thread.
    std::uint32_t editorStatus() const noexcept;
    void setAutomationShortcutEnabled(bool enabled) noexcept;
    std::uint32_t takeAutomationShortcuts() noexcept;
    void requestMainThread() noexcept;
    std::uint32_t takeNotifications() noexcept;
    std::uint32_t latencySamples() const noexcept;
    std::uint32_t tailSamples() const noexcept;
    bool tailSamplesKnown() const noexcept;

    // One producer, one outstanding block. Bounded copying, no allocation,
    // locking, sleeping or waiting. poll publishes only a fully valid result,
    // completed before context.deadlineNanos when that deadline is nonzero.
    // A delayed parent poll does not change the child's completion time.
    bool submit(const PluginProcessContext& context, bool resetBefore = false) noexcept;
    Poll poll(const PluginProcessContext& destination,
              PluginProcessDisposition& disposition) noexcept;
    /// Marks this generation unusable; never reuses a timed-out child's memory.
    void expire() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/// Entry point of daw_plugin_host. Arguments contain inherited OS resources,
/// never plugin code or shell commands. Only that executable calls this.
/// Optional platform UI supplied by the helper executable. All methods run on
/// its main thread; audio has an independent dispatcher. No native handle or UI
/// object crosses the process boundary.
class PluginHostGui {
public:
    virtual ~PluginHostGui() = default;
    virtual bool open(PluginInstance&) = 0;
    virtual void close() = 0;
    virtual void pump() = 0;
    virtual bool isOpen() const = 0;
    /// Clear before releasing the shared mailbox. The GUI thread invokes this
    /// periodically, even while a plugin runs a responsive nested event loop.
    virtual void setHeartbeat(std::function<void()> heartbeat) = 0;
    virtual void setAutomationShortcutEnabled(bool) {}
    virtual std::uint32_t takeAutomationShortcuts() { return 0; }
};
int runPluginHost(int argc, char** argv, PluginHostGui* gui = nullptr);

} // namespace daw::plugins
