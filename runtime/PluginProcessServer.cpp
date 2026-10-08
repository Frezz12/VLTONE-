#include "PluginProcess.hpp"
#include "PluginProcessProtocol.hpp"
#include "SharedProcess.hpp"
#include "Host/PluginInstance.hpp"
#include "Scan/ScanProtocol.hpp"
#include "Job/AudioWorkerRegistration.hpp"
#include "Common/LockFreeQueue.hpp"
#include "RealtimeMetrics.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <semaphore>
#include <thread>

namespace daw::plugins {
namespace {
using Json = nlohmann::json;
thread_local bool hostProcessing = false;

struct Listener final : PluginListener {
    std::atomic<std::uint32_t> flags{0};
    // Audio events have their own timestamped mailbox. These notifications
    // mirror editor/controller values: a preset can refresh every parameter at
    // once, so retain the latest value per parameter until the parent has room.
    std::unique_ptr<std::atomic<double>[]> values;
    std::size_t parameterCount = 0;
    std::atomic<bool> valuesPending{false};
    engine::LockFreeMPSCQueue<ipc::Notice, ipc::kNoticeCapacity> edits;
    std::atomic<bool> overflow{false};
    void prepare(std::size_t count) {
        if (count > 65536) throw std::runtime_error("too many plugin parameters");
        values = std::make_unique<std::atomic<double>[]>(count);
        parameterCount = count;
        for (std::size_t i = 0; i < count; ++i)
            values[i].store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
    }
    void onParameterChanged(std::uint32_t index, double value) noexcept override {
        if (hostProcessing) return;
        if (index >= parameterCount || !std::isfinite(value)) { overflow.store(true); return; }
        values[index].store(value, std::memory_order_release);
        valuesPending.store(true, std::memory_order_release);
    }
    void onParameterGesture(std::uint32_t index, bool begin) noexcept override {
        if (hostProcessing) return;
        if (index >= parameterCount) { overflow.store(true); return; }
        // Keep each completed drag's own value even if a second drag changes
        // the same parameter before the main-thread publisher runs.
        const double value = begin ? 0 : values[index].load(std::memory_order_acquire);
        if (!edits.push({begin ? 1u : 2u, index, value})) overflow.store(true);
    }
    bool takeValue(std::uint32_t index, ipc::Notice& notice) noexcept {
        const auto value = values[index].exchange(std::numeric_limits<double>::quiet_NaN(),
            std::memory_order_acq_rel);
        if (std::isnan(value)) return false;
        notice = {0, index, value};
        return true;
    }
    void onLatencyChanged() noexcept override { flags.fetch_or(PluginProcess::LatencyChanged); }
    void onRestartRequested() noexcept override { flags.fetch_or(PluginProcess::RestartRequested); }
    void onReloadRequested() noexcept override { flags.fetch_or(PluginProcess::ReloadRequested); }
    void onStateChanged() noexcept override { flags.fetch_or(PluginProcess::StateChanged); }
};
static_assert(std::atomic<double>::is_always_lock_free);

struct OutputEvents final : EventSink {
    std::vector<PluginEvent> events;
    std::uint32_t count = 0;
    bool overflow = false;
    explicit OutputEvents(std::uint32_t capacity) : events(capacity) {}
    void push(const PluginEvent& event) noexcept override {
        if (count == events.size()) { overflow = true; return; }
        events[count++] = event;
    }
};

// start/stop/reset/process execute on one dedicated thread. Lifecycle/state
// commands remain serialized with DSP; GUI callbacks run on main, following
// the same main/audio-thread contract as the existing local format adapters.
class AudioWorker {
public:
    enum class Task { Configure, Start, Stop, Reset, Process, Quit };
    explicit AudioWorker(ipc::View view)
        : m_view(view), m_audio(std::size_t(view.layout.limits.frames) * view.layout.limits.channels * 3),
          m_inputs(view.layout.limits.events), m_output(view.layout.limits.events),
          m_thread([this] { loop(); }) {}
    ~AudioWorker() {
        run(Task::Quit, nullptr);
        m_thread.join();
    }
    void run(Task task, PluginInstance* plugin) {
        m_plugin = plugin; m_task = task;
        m_wake.release(); m_done.acquire();
    }
    PluginProcessInfo info;
private:
    void loop() {
        engine::AudioWorkerRegistration registration;
        for (;;) {
            m_wake.acquire();
            const auto task = m_task;
            if (task == Task::Configure) {
                rt::AudioWorkerConfig config;
                config.active = !info.offline;
                config.sampleRate = info.sampleRate;
                config.blockFrames = info.maxBlockSize;
                (void)registration.configure(config);
            }
            if (task == Task::Start) m_plugin->startProcessing();
            if (task == Task::Stop) m_plugin->stopProcessing();
            if (task == Task::Reset) m_plugin->reset();
            if (task == Task::Process) process();
            m_done.release();
            if (task == Task::Quit) return;
        }
    }
    void process() noexcept {
        auto& h = m_view.header();
        const auto b = h.block; // no plugin sees shared protocol/audio pointers
        h.block.disposition = std::uint32_t(PluginProcessDisposition::Error);
        h.block.outputEvents = 0;
        const auto limits = m_view.layout.limits;
        if (!m_plugin->isActive() || !m_plugin->isProcessing() ||
            !ipc::validBlock(b, limits) || b.frames > info.maxBlockSize ||
            ((b.flags & 2) != 0) != info.offline) return;
        if (b.flags & 16) {
            if (!m_plugin->supportsRealtimeReset()) return;
            m_plugin->reset();
        }
        for (unsigned i = 0; i < b.inputEvents; ++i) {
            if (!ipc::decode(m_view.events(false)[i], m_inputs[i], b.frames) ||
                (i && m_inputs[i].frameOffset < m_inputs[i - 1].frameOffset)) return;
            if (m_inputs[i].kind == PluginEvent::Kind::ParamValue &&
                m_inputs[i].paramIndex >= m_plugin->parameters().size()) return;
        }
        std::array<const float*, engine::kMaxChannels> inputs{}, sidechains{};
        std::array<float*, engine::kMaxChannels> outputs{};
        for (unsigned bus = 0; bus < 3; ++bus) {
            const unsigned count = bus == 0 ? b.inputs : bus == 1 ? b.sidechains : b.outputs;
            for (unsigned ch = 0; ch < count; ++ch) {
                float* local = m_audio.data() + (std::size_t(bus) * limits.channels + ch) * limits.frames;
                if (bus < 2) std::copy_n(m_view.channel(bus, ch), b.frames, local);
                else std::fill_n(local, b.frames, 0.f);
                if (bus == 0) inputs[ch] = local;
                if (bus == 1) sidechains[ch] = local;
                if (bus == 2) outputs[ch] = local;
            }
        }
        PluginProcessContext c;
        c.inputs = inputs.data(); c.inputChannels = std::uint16_t(b.inputs);
        c.sidechainInputs = sidechains.data(); c.sidechainInputChannels = std::uint16_t(b.sidechains);
        c.outputs = outputs.data(); c.outputChannels = std::uint16_t(b.outputs);
        c.frames = b.frames; c.inputSilenceMask = b.inputSilence; c.sidechainSilenceMask = b.sidechainSilence;
        c.sampleTime = b.sampleTime; c.steadyTime = b.steadyTime;
        c.playing = (b.flags & 1) != 0; c.offline = (b.flags & 2) != 0;
        c.transport = {b.tempo, int(b.numerator), int(b.denominator), b.ppq, b.bar,
                       b.loopStart, b.loopEnd, (b.flags & 4) != 0, (b.flags & 8) != 0};
        c.inputEvents = std::span(m_inputs.data(), b.inputEvents);
        m_output.count = 0; m_output.overflow = false; c.outputEvents = &m_output;
        hostProcessing = true;
        const auto disposition = m_plugin->process(c);
        hostProcessing = false;
        if (m_output.overflow) return;
        for (unsigned ch = 0; ch < b.outputs; ++ch)
            std::copy_n(outputs[ch], b.frames, m_view.channel(2, ch));
        for (unsigned i = 0; i < m_output.count; ++i)
            m_view.events(true)[i] = ipc::encode(m_output.events[i]);
        h.block.outputEvents = m_output.count;
        h.block.disposition = std::uint32_t(disposition);
    }
    ipc::View m_view;
    std::vector<float> m_audio;
    std::vector<PluginEvent> m_inputs;
    OutputEvents m_output;
    PluginInstance* m_plugin = nullptr;
    Task m_task = Task::Quit;
    std::binary_semaphore m_wake{0}, m_done{0};
    std::thread m_thread;
};

Json metadata(PluginInstance& plugin) {
    const auto buses = plugin.busLayout();
    Json parameters = Json::array();
    const auto all = plugin.parameters();
    const auto pitch = plugin.pitchCapabilities();
    if (all.size() > 65536) throw std::runtime_error("too many plugin parameters");
    for (const auto& p : all) parameters.push_back({
        {"index", p.index}, {"id", p.id}, {"name", p.name}, {"unit", p.unit},
        {"min", p.minValue}, {"max", p.maxValue}, {"default", p.defaultValue},
        {"automatable", p.isAutomatable}, {"stepped", p.isStepped}, {"bypass", p.isBypass},
        {"value", plugin.parameterValue(p.index)}});
    return {{"descriptor", Json::parse(scan::descriptorToJson(plugin.descriptor()))},
        {"inputs", buses.inputs}, {"outputs", buses.outputs}, {"parameters", parameters},
        {"latency", plugin.latencySamples()}, {"tail", plugin.tailSamples()},
        {"tailKnown", plugin.tailSamplesKnown()}, {"supportsState", plugin.supportsState()},
        {"active", plugin.isActive()}, {"processing", plugin.isProcessing()},
        {"realtimeReset", plugin.supportsRealtimeReset()}, {"hasEditor", plugin.hasEditor()},
        {"pitch", {{"perNote", pitch.perNote}, {"mpe", pitch.mpe},
                   {"bend", pitch.pitchBend}, {"continuous", pitch.continuous}}}};
}
bool validLayout(const PluginBusLayout& buses, PluginProcessLimits limits) {
    if (buses.inputs.size() > 256 || buses.outputs.size() > 256) return false;
    for (auto count : buses.inputs) if (count > limits.channels) return false;
    for (auto count : buses.outputs) if (count > limits.channels) return false;
    return true;
}
}

int runPluginHost(int argc, char** argv, PluginHostGui* gui) {
    ipc::SharedProcess channel;
    std::string error;
    if (!channel.attach(argc, argv, error)) return 2;
    auto& header = *reinterpret_cast<ipc::Header*>(channel.data());
    const ipc::Layout layout(header.limits);
    if (header.magic != ipc::kMagic || header.version != ipc::kVersion ||
        header.eventBytes != sizeof(ipc::Event) || header.blockBytes != sizeof(ipc::Block) ||
        !layout.bytes || layout.bytes != channel.size()) return 3;
    const ipc::View view{channel.data(), layout};
    Listener listener; // must outlive the instance, including plugin destruction
    std::unique_ptr<PluginInstance> plugin;
    AudioWorker audio(view);
    struct CloseGui {
        PluginHostGui* gui;
        ~CloseGui() { if (gui) { gui->setHeartbeat({}); gui->close(); } }
    } closeGui{gui};
    if (gui) gui->setHeartbeat([&header] {
        header.editorHeartbeat.fetch_add(1, std::memory_order_release);
    });
    std::vector<std::uint8_t> savedState, incomingState;
    std::size_t incomingOffset = 0;
    bool incoming = false, handshake = false;
    std::vector<std::uint8_t> parameterTouched;
    std::unique_ptr<std::atomic<double>[]> editorValues;
    std::unique_ptr<std::atomic<bool>[]> editorDirty;
    std::atomic<bool> editorValuesPending{false};
    std::uint64_t lastRequest = 0;
    std::uint64_t servicedGeneration = PluginMainThreadWork::generation();
    auto nextGuiPump = std::chrono::steady_clock::now();
    std::optional<ipc::Notice> pendingGesture;
    std::uint32_t nextParameterNotice = 0;
    const auto mirrorParameters = [&] {
        if (!plugin || !editorValuesPending.exchange(false, std::memory_order_acquire)) return;
        for (std::uint32_t i = 0; i < parameterTouched.size(); ++i) if (editorDirty[i].exchange(false)) {
            hostProcessing = true;
            plugin->setParameterFromHost(i, editorValues[i].load(std::memory_order_relaxed));
            hostProcessing = false;
        }
    };
    const auto publish = [&] {
        if (!plugin) return;
        header.latency.store(plugin->latencySamples(), std::memory_order_relaxed);
        header.tail.store(plugin->tailSamples(), std::memory_order_relaxed);
        header.tailKnown.store(plugin->tailSamplesKnown(), std::memory_order_relaxed);
        header.notices.fetch_or(listener.flags.exchange(0), std::memory_order_release);
        if (listener.overflow.exchange(false)) header.noticeOverflow.store(1, std::memory_order_release);
        auto write = header.noticeWrite.load(std::memory_order_relaxed);
        const auto read = header.noticeRead.load(std::memory_order_acquire);
        const auto emit = [&](const ipc::Notice& notice) {
            view.notices()[write++ % ipc::kNoticeCapacity] = notice;
        };
        while (write - read < ipc::kNoticeCapacity) {
            if (!pendingGesture) {
                ipc::Notice gesture;
                if (!listener.edits.pop(gesture)) break;
                pendingGesture = gesture;
            }
            // A completed drag must deliver its last value before gesture-end.
            // If the ring fills, keep that end for the next publication turn.
            if (pendingGesture->kind == 2 && std::isfinite(pendingGesture->value)) {
                auto value = pendingGesture->value;
                emit({0, pendingGesture->parameter, value});
                listener.values[pendingGesture->parameter].compare_exchange_strong(value,
                    std::numeric_limits<double>::quiet_NaN(), std::memory_order_acq_rel);
                pendingGesture->value = std::numeric_limits<double>::quiet_NaN();
                if (write - read == ipc::kNoticeCapacity) break;
            }
            emit({pendingGesture->kind, pendingGesture->parameter, 0});
            pendingGesture.reset();
        }
        if (listener.valuesPending.exchange(false, std::memory_order_acquire)) {
            std::size_t scanned = 0;
            while (scanned < listener.parameterCount && write - read < ipc::kNoticeCapacity) {
                const auto index = nextParameterNotice++ % listener.parameterCount;
                ipc::Notice value;
                if (listener.takeValue(std::uint32_t(index), value)) emit(value);
                ++scanned;
            }
            if (scanned < listener.parameterCount) listener.valuesPending.store(true, std::memory_order_release);
        }
        header.noticeWrite.store(write, std::memory_order_release);
    };
    const auto pump = [&] {
        mirrorParameters();
        const auto generation = PluginMainThreadWork::generation();
        if (plugin && (header.mainThreadRequest.exchange(0, std::memory_order_acquire) ||
                       generation != servicedGeneration)) {
            servicedGeneration = generation;
            plugin->pumpMainThread();
        }
        if (gui && plugin) {
            const auto requested = header.editorRequest.load(std::memory_order_acquire);
            if (requested != header.editorResponse.load(std::memory_order_relaxed)) {
                if (requested & 1) header.editorStatus.store(gui->open(*plugin) ? 1u : 2u);
                else { gui->close(); header.editorStatus.store(0); }
                header.editorResponse.store(requested, std::memory_order_release);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextGuiPump) {
                gui->setAutomationShortcutEnabled(header.automationShortcutEnabled.load(std::memory_order_acquire) != 0);
                gui->pump();
                header.automationShortcutCount.fetch_add(gui->takeAutomationShortcuts(), std::memory_order_release);
                if (gui->isOpen()) plugin->pumpMainThread(); // legacy editor idle
                else if (header.editorStatus.load() == 1) header.editorStatus.store(0);
                nextGuiPump = now + std::chrono::milliseconds(16);
            }
        }
        publish();
    };
    // GUI callbacks may block in a vendor toolkit. The audio mailbox has its
    // own wake and dispatcher, so a slow paint/modal loop cannot stall DSP.
    // Lifecycle/state commands remain serial with audio at the parent boundary.
    std::atomic<bool> configureDispatcher{false};
    std::binary_semaphore dispatcherConfigured{0};
    std::jthread dispatcher([&](std::stop_token stop) {
        const std::stop_callback wakeOnStop(stop, [&] { (void)channel.wakeAudioThread(); });
        engine::AudioWorkerRegistration registration;
        std::uint64_t previous = 0;
        while (channel.wait(-1, true)) {
            if (stop.stop_requested()) return;
            if (configureDispatcher.exchange(false, std::memory_order_acquire)) {
                rt::AudioWorkerConfig config;
                config.active = !audio.info.offline;
                config.sampleRate = audio.info.sampleRate;
                config.blockFrames = audio.info.maxBlockSize;
                (void)registration.configure(config);
                dispatcherConfigured.release();
            }
            const auto request = header.audioRequest.load(std::memory_order_acquire);
            if (request == previous) continue;
            if (request < previous || !plugin || !handshake) std::_Exit(4);
            previous = request;
            audio.run(AudioWorker::Task::Process, plugin.get());
            std::fill(parameterTouched.begin(), parameterTouched.end(), 0);
            const auto b = header.block;
            if (ipc::validBlock(b, layout.limits)) for (std::uint32_t i = b.inputEvents; i > 0; --i) {
                PluginEvent event;
                if (!ipc::decode(view.events(false)[i - 1], event, b.frames) ||
                    event.kind != PluginEvent::Kind::ParamValue || event.paramIndex >= parameterTouched.size() ||
                    parameterTouched[event.paramIndex]) continue;
                parameterTouched[event.paramIndex] = 1;
                editorValues[event.paramIndex].store(event.value, std::memory_order_relaxed);
                editorDirty[event.paramIndex].store(true, std::memory_order_release);
                editorValuesPending.store(true, std::memory_order_release);
            }
            header.latency.store(plugin->latencySamples(), std::memory_order_relaxed);
            header.tail.store(plugin->tailSamples(), std::memory_order_relaxed);
            header.tailKnown.store(plugin->tailSamplesKnown(), std::memory_order_relaxed);
            header.notices.fetch_or(listener.flags.exchange(0), std::memory_order_release);
            header.block.notices = 0;
            header.completedNanos = rt::nowNanos();
            header.response.store(request, std::memory_order_release);
        }
    });
    while (channel.wait(16)) {
        const auto request = header.controlRequest.load(std::memory_order_acquire);
        if (request == lastRequest) { pump(); continue; } // nonblocking control-thread wake
        if (request < lastRequest) return 4;
        lastRequest = request;
        if (header.operation != std::uint32_t(ipc::Operation::Control) ||
            header.textBytes > layout.limits.controlBytes || header.blobBytes > layout.limits.controlBytes) return 4;
        Json response{{"ok", true}};
        bool quit = false;
        std::uint32_t outputBytes = 0;
        try {
            mirrorParameters();
            const auto command = Json::parse(view.text(), view.text() + header.textBytes);
            const std::string op = command.at("op");
            if (!handshake && op != "hello") throw std::runtime_error("handshake required");
            bool changed = false;
            if (op == "hello") {
                if (command.at("version").get<unsigned>() != ipc::kVersion) throw std::runtime_error("protocol version mismatch");
                handshake = true; response["version"] = ipc::kVersion;
                response["clockNanos"] = rt::nowNanos();
            } else if (op == "load") {
                if (plugin) throw std::runtime_error("one plugin per process");
                PluginDescriptor descriptor;
                if (!scan::descriptorFromJson(command.at("descriptor").dump(), descriptor) ||
                    descriptor.format == Format::Internal) throw std::runtime_error("invalid external plugin descriptor");
                auto* factory = factoryFor(descriptor.format);
                if (!factory) throw std::runtime_error("plugin format is unavailable");
                plugin = factory->create(descriptor);
                if (!plugin) {
                    const auto available = factory->inspect(descriptor.path);
                    if (!available.empty() && std::none_of(available.begin(), available.end(),
                        [&](const auto& candidate) { return candidate.uid == descriptor.uid; })) {
                        std::string error = "installed module does not contain plugin class " + descriptor.uid + "; available: ";
                        for (std::size_t i = 0; i < std::min<std::size_t>(available.size(), 3); ++i) {
                            if (i) error += ", ";
                            error += available[i].name.substr(0, 80);
                        }
                        if (available.size() > 3) error += ", ...";
                        throw std::runtime_error(error);
                    }
                    throw std::runtime_error("could not load plugin");
                }
                listener.prepare(plugin->parameters().size());
                plugin->setListener(&listener);
                changed = true;
            } else if (op == "quit") {
                if (gui) gui->close();
                if (plugin) {
                    audio.run(AudioWorker::Task::Stop, plugin.get());
                    plugin->deactivate(); plugin->setListener(nullptr); plugin.reset();
                }
                quit = true;
            } else {
                if (!plugin) throw std::runtime_error("no plugin loaded");
                if (op == "layout") {
                    if (plugin->isActive()) throw std::runtime_error("deactivate before changing layout");
                    PluginBusLayout wanted{command.at("inputs").get<std::vector<std::uint16_t>>(),
                        command.at("outputs").get<std::vector<std::uint16_t>>()}, accepted;
                    if (!validLayout(wanted, layout.limits) || !plugin->setBusLayout(wanted, accepted) ||
                        !validLayout(accepted, layout.limits)) throw std::runtime_error("unsupported bus layout");
                    changed = true;
                } else if (op == "activate") {
                    PluginProcessInfo info{command.at("rate"), command.at("frames"),
                        command.at("offline"), command.at("sidechain")};
                    if (!std::isfinite(info.sampleRate) || info.sampleRate <= 0 || !info.maxBlockSize ||
                        info.maxBlockSize > layout.limits.frames) throw std::runtime_error("invalid process configuration");
                    audio.run(AudioWorker::Task::Stop, plugin.get());
                    if (plugin->isActive()) plugin->deactivate();
                    if (!plugin->activate(info) || !validLayout(plugin->busLayout(), layout.limits))
                        throw std::runtime_error("plugin activation failed");
                    audio.info = info;
                    audio.run(AudioWorker::Task::Configure, plugin.get());
                    configureDispatcher.store(true, std::memory_order_release);
                    if (!channel.wakeAudioThread()) throw std::runtime_error("could not configure audio dispatcher");
                    dispatcherConfigured.acquire();
                    changed = true;
                } else if (op == "start") {
                    if (!plugin->isActive()) throw std::runtime_error("plugin is not active");
                    audio.run(AudioWorker::Task::Start, plugin.get());
                    if (!plugin->isProcessing()) throw std::runtime_error("plugin refused processing");
                    changed = true;
                } else if (op == "stop" || op == "deactivate") {
                    audio.run(AudioWorker::Task::Stop, plugin.get());
                    if (op == "deactivate") plugin->deactivate();
                    changed = true;
                } else if (op == "reset") {
                    if (!plugin->isActive()) throw std::runtime_error("reset requires activation");
                    if (plugin->supportsRealtimeReset()) audio.run(AudioWorker::Task::Reset, plugin.get());
                    else {
                        const bool processing = plugin->isProcessing();
                        audio.run(AudioWorker::Task::Stop, plugin.get());
                        plugin->resetForTransport();
                        if (processing) audio.run(AudioWorker::Task::Start, plugin.get());
                    }
                } else if (op == "metadata" || op == "pump") {
                    if (op == "pump") plugin->pumpMainThread();
                    changed = true;
                } else if (op == "offline_restart") {
                    response["reconfigure"] = plugin->serviceOfflineRestart();
                    changed = true;
                } else if (op == "parameter") {
                    const auto index = command.at("index").get<std::uint32_t>();
                    const double value = command.at("value");
                    const auto parameters = plugin->parameters();
                    if (index >= parameters.size() || !std::isfinite(value) ||
                        value < parameters[index].minValue || value > parameters[index].maxValue)
                        throw std::runtime_error("invalid parameter edit");
                    hostProcessing = true;
                    plugin->setParameterFromHost(index, value);
                    hostProcessing = false; changed = true;
                } else if (op == "pending_parameters") {
                    const auto pending = plugin->pendingParameterEvents();
                    if (pending.size() > layout.limits.events) throw std::runtime_error("parameter queue exceeds capacity");
                    response["events"] = Json::array();
                    for (const auto& event : pending) if (event.kind == PluginEvent::Kind::ParamValue)
                        response["events"].push_back({{"index", event.paramIndex}, {"value", event.value}});
                } else if (op == "save") {
                    std::vector<std::uint8_t> fresh;
                    if (!plugin->supportsState() || !plugin->saveState(fresh) || fresh.size() > kMaxPluginStateBytes)
                        throw std::runtime_error("plugin state could not be saved");
                    savedState = std::move(fresh); response["bytes"] = savedState.size(); changed = true;
                } else if (op == "state_read") {
                    const auto offset = command.at("offset").get<std::uint64_t>();
                    const auto count = command.at("count").get<std::uint64_t>();
                    if (offset > savedState.size() || count > savedState.size() - offset ||
                        count > layout.limits.controlBytes) throw std::runtime_error("invalid state read");
                    if (count) std::memcpy(view.blob(), savedState.data() + offset, std::size_t(count));
                    outputBytes = std::uint32_t(count);
                } else if (op == "state_begin") {
                    const auto bytes = command.at("bytes").get<std::uint64_t>();
                    if (bytes > kMaxPluginStateBytes) throw std::runtime_error("plugin state is too large");
                    incomingState.resize(std::size_t(bytes)); incomingOffset = 0; incoming = true;
                } else if (op == "state_write") {
                    const auto offset = command.at("offset").get<std::uint64_t>();
                    if (!incoming || offset != incomingOffset || header.blobBytes > incomingState.size() - incomingOffset)
                        throw std::runtime_error("invalid state write");
                    if (header.blobBytes) std::memcpy(incomingState.data() + incomingOffset, view.blob(), header.blobBytes);
                    incomingOffset += header.blobBytes;
                } else if (op == "state_commit") {
                    if (!incoming || incomingOffset != incomingState.size()) throw std::runtime_error("incomplete state");
                    incoming = false;
                    if (!plugin->loadState(incomingState)) throw std::runtime_error("plugin state was rejected");
                    incomingState.clear(); changed = true;
                } else throw std::runtime_error("unknown host command");
            }
            if (changed) {
                // Creation/activation can request deferred setup. Settle that
                // main-thread turn before returning latency/parameter metadata
                // to the offline preparation loop, not after its first block.
                pump();
                response["metadata"] = metadata(*plugin);
                if (!gui) response["metadata"]["hasEditor"] = false;
                if (parameterTouched.size() != plugin->parameters().size()) {
                    parameterTouched.resize(plugin->parameters().size());
                    editorValues = std::make_unique<std::atomic<double>[]>(parameterTouched.size());
                    editorDirty = std::make_unique<std::atomic<bool>[]>(parameterTouched.size());
                }
            }
            publish();
        } catch (const std::exception& e) {
            hostProcessing = false;
            response = {{"ok", false}, {"error", std::string(e.what()).substr(0, 1024)}};
        }
        auto text = response.dump();
        if (text.size() > layout.limits.controlBytes) {
            text = R"({"ok":false,"error":"host response exceeds negotiated capacity"})";
            outputBytes = 0;
        }
        std::memcpy(view.text(), text.data(), text.size());
        header.textBytes = std::uint32_t(text.size()); header.blobBytes = outputBytes;
        header.response.store(request, std::memory_order_release);
        if (quit) return 0;
    }
    return 0;
}

} // namespace daw::plugins
