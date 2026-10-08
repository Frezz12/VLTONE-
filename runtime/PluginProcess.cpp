#include "PluginProcess.hpp"
#include "PluginProcessProtocol.hpp"
#include "SharedProcess.hpp"
#include "Scan/ScanProtocol.hpp"
#include "RealtimeMetrics.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <thread>

namespace daw::plugins {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
bool readMetadata(const Json& j, PluginProcessMetadata& out, PluginProcessLimits limits, std::string& error) {
    PluginProcessMetadata next;
    if (!scan::descriptorFromJson(j.at("descriptor").dump(), next.descriptor)) return false;
    next.buses.inputs = j.at("inputs").get<std::vector<std::uint16_t>>();
    next.buses.outputs = j.at("outputs").get<std::vector<std::uint16_t>>();
    if (next.buses.inputs.size() > 256 || next.buses.outputs.size() > 256) return false;
    for (auto channels : next.buses.inputs) if (channels > limits.channels) return false;
    for (auto channels : next.buses.outputs) if (channels > limits.channels) return false;
    const auto& parameters = j.at("parameters");
    if (!parameters.is_array() || parameters.size() > 65536) return false;
    for (const auto& value : parameters) {
        ParameterInfo p;
        p.index = value.at("index"); p.id = value.at("id"); p.name = value.at("name");
        p.unit = value.at("unit"); p.minValue = value.at("min"); p.maxValue = value.at("max");
        p.defaultValue = value.at("default"); p.isAutomatable = value.at("automatable");
        p.isStepped = value.at("stepped"); p.isBypass = value.at("bypass");
        // Null is the existing cache representation of a plugin's unknown/NaN
        // readback (e.g. placeholder VST3 parameters). Preserve that distinction.
        const auto& readback = value.at("value");
        const double current = readback.is_null() ? std::numeric_limits<double>::quiet_NaN()
                                                  : readback.get<double>();
        if (p.index != next.parameters.size() || (!readback.is_null() && !std::isfinite(current)) ||
            !std::isfinite(p.minValue) || !std::isfinite(p.maxValue) ||
            !std::isfinite(p.defaultValue) || p.minValue > p.maxValue) {
            error = "invalid parameter metadata: " + p.id + " (" + p.name + "), range " +
                std::to_string(p.minValue) + ".." + std::to_string(p.maxValue);
            return false;
        }
        next.parameters.push_back(std::move(p)); next.values.push_back(current);
    }
    next.latency = j.at("latency"); next.tail = j.at("tail");
    next.tailKnown = j.at("tailKnown"); next.supportsState = j.at("supportsState");
    next.active = j.at("active"); next.processing = j.at("processing");
    next.realtimeReset = j.at("realtimeReset");
    next.hasEditor = j.at("hasEditor");
    const auto& pitch = j.at("pitch");
    next.pitch = {pitch.at("perNote"), pitch.at("mpe"), pitch.at("bend"), pitch.at("continuous")};
    out = std::move(next);
    return true;
}
void silence(const PluginProcessContext& context) noexcept {
    if (!context.outputs) return;
    for (std::uint16_t ch = 0; ch < context.outputChannels; ++ch)
        if (context.outputs[ch]) std::fill_n(context.outputs[ch], context.frames, 0.0f);
}
}

struct PluginProcess::Impl {
    std::string executable, detail;
    ipc::Layout layout;
    std::chrono::milliseconds timeout;
    std::unique_ptr<ipc::SharedProcess> child;
    std::atomic<PluginProcessFailure> fault{PluginProcessFailure::None};
    PluginDescriptor descriptor;
    PluginProcessMetadata metadata;
    PluginBusLayout wantedLayout;
    PluginProcessInfo info;
    std::vector<std::uint8_t> checkpoint;
    bool checkpointValid = false, layoutSet = false, wantActive = false, wantProcessing = false;
    bool loaded = false, inFlight = false;
    std::uint64_t sequence = 0;
    std::uint64_t blockDeadlineNanos = 0;
    std::uint32_t blockFrames = 0, blockOutputs = 0;
    std::uint32_t requestedEditor = 0;
    Clock::time_point editorDeadline = Clock::time_point::max();
    std::uint64_t editorHeartbeat = 0;
    Clock::time_point editorHeartbeatAt{}, editorHealthPollAt{};
    bool watchingEditor = false;

    Impl(std::string path, PluginProcessLimits limits, std::chrono::milliseconds wait)
        : executable(std::move(path)), layout(limits), timeout(wait) {}
    ipc::View view() const noexcept { return {child->data(), layout}; }
    void fail(PluginProcessFailure value) noexcept {
        auto none = PluginProcessFailure::None;
        if (fault.compare_exchange_strong(none, value, std::memory_order_release))
            PluginMainThreadWork::request();
    }
    Json request(Json command, std::span<const std::uint8_t> blob = {},
                 Clock::time_point deadline = Clock::time_point::max()) {
        if (!child || fault.load() != PluginProcessFailure::None) return {};
        if (inFlight) { detail = "control call overlaps an outstanding audio block"; return {}; }
        try {
            const auto text = command.dump();
            if (text.size() > layout.limits.controlBytes || blob.size() > layout.limits.controlBytes) {
                detail = "control message exceeds negotiated capacity"; return {};
            }
            auto v = view();
            std::memcpy(v.text(), text.data(), text.size());
            if (!blob.empty()) std::memcpy(v.blob(), blob.data(), blob.size());
            auto& h = v.header();
            h.operation = std::uint32_t(ipc::Operation::Control);
            h.textBytes = std::uint32_t(text.size()); h.blobBytes = std::uint32_t(blob.size());
            ++sequence;
            if (deadline == Clock::time_point::max()) deadline = Clock::now() + timeout;
            struct Progress {
                bool completed = false;
                ~Progress() { ScopedPluginControlProgress::report({}, completed); }
            } progress;
            ScopedPluginControlProgress::report(std::max(std::chrono::nanoseconds(1),
                std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now())), false);
            h.request.store(sequence, std::memory_order_release);
            h.controlRequest.store(sequence, std::memory_order_release);
            if (!child->signal()) { fail(PluginProcessFailure::Exited); return {}; }
            while (h.response.load(std::memory_order_acquire) != sequence) {
                if (!child->running()) { fail(PluginProcessFailure::Exited); return {}; }
                if (Clock::now() >= deadline) { fail(PluginProcessFailure::ControlTimeout); return {}; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            progress.completed = true;
            if (h.textBytes > layout.limits.controlBytes || h.blobBytes > layout.limits.controlBytes) {
                fail(PluginProcessFailure::Protocol); return {};
            }
            auto result = Json::parse(v.text(), v.text() + h.textBytes);
            if (!result.at("ok").get<bool>()) {
                detail = result.value("error", std::string("plugin rejected the operation"));
                return {};
            }
            if (result.contains("metadata") && !readMetadata(result.at("metadata"), metadata, layout.limits, detail)) {
                if (detail.empty()) detail = "invalid plugin metadata";
                fail(PluginProcessFailure::Protocol); return {};
            }
            detail.clear();
            return result;
        } catch (const std::exception& e) {
            detail = e.what(); fail(PluginProcessFailure::Protocol); return {};
        }
    }
    bool open() {
        child.reset(); loaded = inFlight = false; sequence = 0;
        requestedEditor = 0; editorDeadline = Clock::time_point::max();
        watchingEditor = false; editorHeartbeat = 0;
        fault.store(PluginProcessFailure::None); detail.clear();
        if (!layout.bytes || timeout.count() <= 0) {
            detail = "invalid transport limits or timeout"; fail(PluginProcessFailure::Launch); return false;
        }
        child = std::make_unique<ipc::SharedProcess>();
        if (!child->create(layout.bytes, detail)) { fail(PluginProcessFailure::Launch); return false; }
        auto& header = *::new (child->data()) ipc::Header{};
        header.limits = layout.limits;
        if (!child->launch(executable, detail)) { fail(PluginProcessFailure::Launch); return false; }
        const auto beforeHello = rt::nowNanos();
        const auto hello = request({{"op", "hello"}, {"version", ipc::kVersion}});
        const auto afterHello = rt::nowNanos();
        if (hello.is_null()) return false;
        if (!hello.contains("version") || !hello["version"].is_number_unsigned() ||
            hello["version"].get<std::uint64_t>() != ipc::kVersion ||
            !hello.contains("clockNanos") || !hello["clockNanos"].is_number_unsigned() ||
            hello["clockNanos"].get<std::uint64_t>() < beforeHello ||
            hello["clockNanos"].get<std::uint64_t>() > afterHello) {
            fail(PluginProcessFailure::Protocol); return false;
        }
        const auto result = request({{"op", "load"},
            {"descriptor", Json::parse(scan::descriptorToJson(descriptor))}});
        loaded = !result.is_null();
        return loaded;
    }
};

PluginProcess::PluginProcess(std::string executable, PluginProcessLimits limits,
                             std::chrono::milliseconds timeout)
    : m(std::make_unique<Impl>(std::move(executable), limits, timeout)) {}
PluginProcess::~PluginProcess() { close(); }
const PluginProcessMetadata& PluginProcess::metadata() const noexcept { return m->metadata; }
PluginProcessFailure PluginProcess::failure() const noexcept { return m->fault.load(std::memory_order_acquire); }
std::uint64_t PluginProcess::processId() const noexcept { return m->child ? m->child->processId() : 0; }
PluginProcessLimits PluginProcess::limits() const noexcept { return m->layout.limits; }
bool PluginProcess::hasCheckpoint() const noexcept { return m->checkpointValid; }
bool PluginProcess::copyCheckpoint(std::vector<std::uint8_t>& out) const {
    if (!m->checkpointValid) return false;
    out = m->checkpoint; return true;
}
const std::string& PluginProcess::executable() const noexcept { return m->executable; }
bool PluginProcess::checkHealth() {
    if (m->child && failure() == PluginProcessFailure::None && !m->child->running())
        m->fail(PluginProcessFailure::Exited);
    if (m->requestedEditor && m->child && m->child->data() &&
        m->view().header().editorResponse.load(std::memory_order_acquire) != m->requestedEditor &&
        Clock::now() >= m->editorDeadline) m->fail(PluginProcessFailure::ControlTimeout);
    if (m->child && m->child->data() && failure() == PluginProcessFailure::None) {
        const auto& header = m->view().header();
        if (header.editorStatus.load(std::memory_order_acquire) == 1) {
            const auto now = Clock::now();
            const auto heartbeat = header.editorHeartbeat.load(std::memory_order_acquire);
            // A suspended or delayed parent cannot prove the child's GUI was
            // unresponsive during that gap. Start a fresh observation window.
            if (!m->watchingEditor || heartbeat != m->editorHeartbeat ||
                now - m->editorHealthPollAt >= m->timeout) {
                m->editorHeartbeat = heartbeat;
                m->editorHeartbeatAt = now;
            } else if (now - m->editorHeartbeatAt >= m->timeout) {
                m->fail(PluginProcessFailure::EditorUnresponsive);
            }
            m->watchingEditor = true;
            m->editorHealthPollAt = now;
        } else m->watchingEditor = false;
    }
    return failure() == PluginProcessFailure::None;
}
bool PluginProcess::hasMainThreadWork() const noexcept {
    if (!m->child || !m->child->data() || failure() != PluginProcessFailure::None) return false;
    const auto& h = m->view().header();
    return h.notices.load(std::memory_order_acquire) || h.noticeOverflow.load() ||
        h.noticeRead.load() != h.noticeWrite.load(std::memory_order_acquire);
}
bool PluginProcess::popNotification(PluginEvent& event) noexcept {
    if (!m->child || !m->child->data() || failure() != PluginProcessFailure::None) return false;
    auto v = m->view(); auto& h = v.header();
    const auto read = h.noticeRead.load(std::memory_order_relaxed);
    const auto write = h.noticeWrite.load(std::memory_order_acquire);
    if (h.noticeOverflow.load(std::memory_order_acquire) || write - read > ipc::kNoticeCapacity) {
        m->detail = "plugin parameter notification queue overflow";
        m->fail(PluginProcessFailure::Protocol); return false;
    }
    if (read == write) return false;
    const auto notice = v.notices()[read % ipc::kNoticeCapacity];
    if (notice.kind > 2 || notice.parameter >= m->metadata.parameters.size() ||
        !std::isfinite(notice.value)) {
        m->detail = "invalid parameter notification: index " + std::to_string(notice.parameter) +
            ", kind " + std::to_string(notice.kind) + ", value " + std::to_string(notice.value);
        m->fail(PluginProcessFailure::Protocol); return false;
    }
    event = {};
    event.kind = notice.kind == 0 ? PluginEvent::Kind::ParamValue :
        notice.kind == 1 ? PluginEvent::Kind::ParamGestureBegin : PluginEvent::Kind::ParamGestureEnd;
    event.paramIndex = notice.parameter; event.value = notice.value;
    h.noticeRead.store(read + 1, std::memory_order_release);
    return true;
}
void PluginProcess::requestEditor(bool open) noexcept {
    if (!m->child || !m->child->data() || failure() != PluginProcessFailure::None) return;
    auto& h = m->view().header();
    const auto sequence = (h.editorRequest.load(std::memory_order_relaxed) & ~1u) + 2;
    m->requestedEditor = sequence | unsigned(open);
    m->editorDeadline = Clock::now() + m->timeout;
    h.editorRequest.store(sequence | unsigned(open), std::memory_order_release);
    (void)m->child->signal();
}
std::uint32_t PluginProcess::editorStatus() const noexcept {
    if (!m->child || !m->child->data() || failure() != PluginProcessFailure::None) return 0;
    const auto& header = m->view().header();
    // A close/reopen request must not inherit the previous editor's open or
    // failed result while the helper has yet to service the new generation.
    if (header.editorResponse.load(std::memory_order_acquire) !=
        header.editorRequest.load(std::memory_order_relaxed)) return 3;
    return header.editorStatus.load(std::memory_order_acquire);
}
void PluginProcess::setAutomationShortcutEnabled(bool enabled) noexcept {
    if (m->child && m->child->data() && failure() == PluginProcessFailure::None)
        m->view().header().automationShortcutEnabled.store(enabled, std::memory_order_release);
}
std::uint32_t PluginProcess::takeAutomationShortcuts() noexcept {
    if (!m->child || !m->child->data() || failure() != PluginProcessFailure::None) return 0;
    // A faulty child cannot enqueue unbounded UI work in the host.
    return std::min(32u, m->view().header().automationShortcutCount.exchange(0));
}
std::uint32_t PluginProcess::takeNotifications() noexcept {
    return m->child && m->child->data() ? m->view().header().notices.exchange(0) : 0;
}
void PluginProcess::requestMainThread() noexcept {
    if (m->child && m->child->data() && failure() == PluginProcessFailure::None) {
        m->view().header().mainThreadRequest.store(1, std::memory_order_release);
        (void)m->child->signal();
    }
}
std::uint32_t PluginProcess::latencySamples() const noexcept {
    return m->child && m->child->data() ? m->view().header().latency.load() : m->metadata.latency;
}
std::uint32_t PluginProcess::tailSamples() const noexcept {
    return m->child && m->child->data() ? m->view().header().tail.load() : m->metadata.tail;
}
bool PluginProcess::tailSamplesKnown() const noexcept {
    return m->child && m->child->data() ? m->view().header().tailKnown.load() != 0 : m->metadata.tailKnown;
}
std::string PluginProcess::error() const {
    std::string value;
    switch (failure()) {
        case PluginProcessFailure::None: return m->detail;
        case PluginProcessFailure::Launch: value = "plugin host could not start"; break;
        case PluginProcessFailure::Protocol: value = "invalid plugin host response"; break;
        case PluginProcessFailure::Exited: value = "plugin host exited"; break;
        case PluginProcessFailure::ControlTimeout: value = "plugin control operation timed out"; break;
        case PluginProcessFailure::AudioDeadline: value = "plugin missed its audio deadline"; break;
        case PluginProcessFailure::InvalidBlock: value = "audio block exceeds the negotiated contract"; break;
        case PluginProcessFailure::InvalidOutput: value = "plugin returned invalid audio or events"; break;
        case PluginProcessFailure::Processor: value = "plugin processing failed"; break;
        case PluginProcessFailure::EditorUnresponsive: value = "plugin editor stopped responding"; break;
    }
    if (!m->detail.empty()) value += ": " + m->detail;
    return value;
}

bool PluginProcess::load(const PluginDescriptor& descriptor) {
    close();
    m->descriptor = descriptor; m->checkpoint.clear(); m->checkpointValid = false;
    m->layoutSet = m->wantActive = m->wantProcessing = false;
    m->metadata = {};
    return m->open();
}
bool PluginProcess::setBusLayout(const PluginBusLayout& layout) {
    const bool ok = !m->request({{"op", "layout"}, {"inputs", layout.inputs}, {"outputs", layout.outputs}}).is_null();
    if (ok) { m->wantedLayout = layout; m->layoutSet = true; }
    return ok;
}
bool PluginProcess::reserve(PluginProcessLimits limits) {
    const auto previous = m->layout.limits;
    limits.frames = std::max(limits.frames, previous.frames);
    limits.channels = std::max(limits.channels, previous.channels);
    limits.events = std::max(limits.events, previous.events);
    limits.controlBytes = std::max(limits.controlBytes, previous.controlBytes);
    const ipc::Layout next(limits);
    if (!next.bytes || m->inFlight || m->wantActive) return false;
    if (next.bytes == m->layout.bytes) return true;
    if (m->metadata.supportsState) {
        std::vector<std::uint8_t> state;
        if (!saveState(state)) return false;
    }
    m->layout = next;
    return restart();
}
bool PluginProcess::activate(const PluginProcessInfo& info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate <= 0 || !info.maxBlockSize ||
        info.maxBlockSize > m->layout.limits.frames) { m->detail = "invalid process configuration"; return false; }
    const bool ok = !m->request({{"op", "activate"}, {"rate", info.sampleRate},
        {"frames", info.maxBlockSize}, {"offline", info.offline}, {"sidechain", info.sidechainConnected}}).is_null();
    if (ok) { m->info = info; m->wantActive = true; m->wantProcessing = false; }
    return ok;
}
bool PluginProcess::startProcessing() {
    const bool ok = !m->request({{"op", "start"}}).is_null();
    if (ok) m->wantProcessing = true;
    return ok;
}
bool PluginProcess::stopProcessing() {
    const bool ok = !m->request({{"op", "stop"}}).is_null();
    if (ok) m->wantProcessing = false;
    return ok;
}
bool PluginProcess::deactivate() {
    const bool ok = !m->request({{"op", "deactivate"}}).is_null();
    if (ok) m->wantActive = m->wantProcessing = false;
    return ok;
}
bool PluginProcess::reset() { return !m->request({{"op", "reset"}}).is_null(); }
bool PluginProcess::pumpMainThread() { return !m->request({{"op", "pump"}}).is_null(); }
bool PluginProcess::refreshMetadata() { return !m->request({{"op", "metadata"}}).is_null(); }
bool PluginProcess::serviceOfflineRestart() {
    const auto response = m->request({{"op", "offline_restart"}});
    if (response.is_null()) return true;
    if (!response.contains("reconfigure") || !response["reconfigure"].is_boolean()) {
        m->fail(PluginProcessFailure::Protocol); return true;
    }
    return response["reconfigure"].get<bool>();
}
bool PluginProcess::setParameterFromHost(std::uint32_t index, double value) {
    if (!std::isfinite(value)) return false;
    return !m->request({{"op", "parameter"}, {"index", index}, {"value", value}}).is_null();
}
std::vector<PluginEvent> PluginProcess::pendingParameterEvents() {
    const auto response = m->request({{"op", "pending_parameters"}});
    if (response.is_null()) return {};
    std::vector<PluginEvent> events;
    try {
        const auto& values = response.at("events");
        if (!values.is_array() || values.size() > m->layout.limits.events) throw std::runtime_error("invalid parameter queue");
        events.reserve(values.size());
        for (const auto& value : values) {
            PluginEvent event;
            event.paramIndex = value.at("index"); event.value = value.at("value");
            if (event.paramIndex >= m->metadata.parameters.size() || !std::isfinite(event.value))
                throw std::runtime_error("invalid parameter event");
            events.push_back(event);
        }
    } catch (...) { m->fail(PluginProcessFailure::Protocol); return {}; }
    return events;
}
bool PluginProcess::saveState(std::vector<std::uint8_t>& out) {
    const auto deadline = Clock::now() + m->timeout;
    const auto response = m->request({{"op", "save"}}, {}, deadline);
    if (response.is_null()) return false;
    if (!response.contains("bytes") || !response["bytes"].is_number_unsigned()) {
        m->fail(PluginProcessFailure::Protocol); return false;
    }
    const auto bytes = response["bytes"].get<std::uint64_t>();
    if (bytes > kMaxPluginStateBytes) { m->fail(PluginProcessFailure::Protocol); return false; }
    std::vector<std::uint8_t> state(static_cast<std::size_t>(bytes));
    for (std::size_t offset = 0; offset < state.size();) {
        const auto count = std::min<std::size_t>(m->layout.limits.controlBytes, state.size() - offset);
        if (m->request({{"op", "state_read"}, {"offset", offset}, {"count", count}}, {}, deadline).is_null()) return false;
        const auto v = m->view();
        if (v.header().blobBytes != count) { m->fail(PluginProcessFailure::Protocol); return false; }
        std::memcpy(state.data() + offset, v.blob(), count);
        offset += count;
    }
    m->checkpoint = state; m->checkpointValid = true;
    out = std::move(state); // caller's previous bytes survive every failed save
    return true;
}
bool PluginProcess::loadState(std::span<const std::uint8_t> state) {
    if (state.size() > kMaxPluginStateBytes) { m->detail = "plugin state is too large"; return false; }
    const auto deadline = Clock::now() + m->timeout;
    if (m->request({{"op", "state_begin"}, {"bytes", state.size()}}, {}, deadline).is_null()) return false;
    for (std::size_t offset = 0; offset < state.size();) {
        const auto count = std::min<std::size_t>(m->layout.limits.controlBytes, state.size() - offset);
        if (m->request({{"op", "state_write"}, {"offset", offset}}, state.subspan(offset, count), deadline).is_null()) return false;
        offset += count;
    }
    if (m->request({{"op", "state_commit"}}, {}, deadline).is_null()) return false;
    m->checkpoint.assign(state.begin(), state.end()); m->checkpointValid = true;
    return true;
}
bool PluginProcess::restart() {
    // Retain the last confirmed bytes even when load/create/save/destroy fails.
    const auto state = m->checkpoint;
    const bool haveState = m->checkpointValid;
    const auto layout = m->wantedLayout;
    const bool layoutSet = m->layoutSet, active = m->wantActive, processing = m->wantProcessing;
    const auto info = m->info;
    if (m->child) m->child->stop();
    if (!m->open()) return false;
    if (layoutSet && !setBusLayout(layout)) return false;
    if (haveState && !loadState(state)) return false;
    if (active && !activate(info)) return false;
    return !processing || startProcessing();
}
void PluginProcess::close() {
    if (!m->child) return;
    if (!m->inFlight && failure() == PluginProcessFailure::None && m->child->running())
        (void)m->request({{"op", "quit"}}, {}, Clock::now() + kPluginCloseTimeout);
    m->child->stop(); m->child.reset(); m->loaded = m->inFlight = false;
}
bool PluginProcess::service() {
    if (!m->child) return false;
    if (!m->child->running()) m->fail(PluginProcessFailure::Exited);
    if (failure() != PluginProcessFailure::None) { m->child->stop(); m->inFlight = false; return false; }
    return true;
}

bool PluginProcess::submit(const PluginProcessContext& c, bool resetBefore) noexcept {
    if (!m->loaded || !m->wantProcessing || m->inFlight || failure() != PluginProcessFailure::None) return false;
    ipc::Block b;
    b.frames = c.frames; b.inputs = c.inputChannels; b.outputs = c.outputChannels;
    b.sidechains = c.sidechainInputChannels;
    if (c.inputEvents.size() > m->layout.limits.events) { m->fail(PluginProcessFailure::InvalidBlock); return false; }
    b.inputEvents = std::uint32_t(c.inputEvents.size());
    b.inputSilence = c.inputSilenceMask; b.sidechainSilence = c.sidechainSilenceMask;
    b.sampleTime = c.sampleTime; b.steadyTime = c.steadyTime;
    b.tempo = c.transport.tempo; b.ppq = c.transport.ppqPosition; b.bar = c.transport.barStartPpq;
    b.loopStart = c.transport.loopStartPpq; b.loopEnd = c.transport.loopEndPpq;
    b.numerator = std::uint32_t(c.transport.timeSigNumerator);
    b.denominator = std::uint32_t(c.transport.timeSigDenominator);
    b.flags = (c.playing ? 1u : 0u) | (c.offline ? 2u : 0u) |
        (c.transport.looping ? 4u : 0u) | (c.transport.recording ? 8u : 0u) | (resetBefore ? 16u : 0u);
    if (!ipc::validBlock(b, m->layout.limits) || c.frames > m->info.maxBlockSize ||
        c.offline != m->info.offline || (c.inputChannels && !c.inputs) ||
        (c.sidechainInputChannels && !c.sidechainInputs)) {
        m->fail(PluginProcessFailure::InvalidBlock); return false;
    }
    auto v = m->view();
    for (std::uint32_t i = 0; i < b.inputEvents; ++i) {
        const auto wire = ipc::encode(c.inputEvents[i]);
        PluginEvent check;
        if (!ipc::decode(wire, check, c.frames) || (i && wire.frame < c.inputEvents[i - 1].frameOffset)) {
            m->fail(PluginProcessFailure::InvalidBlock); return false;
        }
        v.events(false)[i] = wire;
    }
    for (unsigned bus = 0; bus < 2; ++bus) {
        const auto count = bus ? c.sidechainInputChannels : c.inputChannels;
        const auto pointers = bus ? c.sidechainInputs : c.inputs;
        for (unsigned ch = 0; ch < count; ++ch) {
            if (!pointers[ch]) { m->fail(PluginProcessFailure::InvalidBlock); return false; }
            std::copy_n(pointers[ch], c.frames, v.channel(bus, ch));
        }
    }
    auto& h = v.header();
    h.block = b; h.operation = std::uint32_t(ipc::Operation::Audio);
    m->blockFrames = c.frames; m->blockOutputs = c.outputChannels;
    m->blockDeadlineNanos = c.deadlineNanos;
    h.completedNanos = 0;
    m->inFlight = true;
    h.request.store(++m->sequence, std::memory_order_release);
    h.audioRequest.store(m->sequence, std::memory_order_release);
    if (!m->child->signal(true)) { m->fail(PluginProcessFailure::Exited); return false; }
    return true;
}

PluginProcess::Poll PluginProcess::poll(const PluginProcessContext& destination,
                                       PluginProcessDisposition& disposition) noexcept {
    disposition = PluginProcessDisposition::Error;
    if (failure() != PluginProcessFailure::None || !m->inFlight) { silence(destination); return Poll::Failed; }
    auto v = m->view();
    if (v.header().response.load(std::memory_order_acquire) != m->sequence) return Poll::Pending;
    const auto b = v.header().block;
    m->inFlight = false;
    const auto completed = v.header().completedNanos;
    if (!completed) {
        m->fail(PluginProcessFailure::Protocol); silence(destination); return Poll::Failed;
    }
    if (m->blockDeadlineNanos && completed >= m->blockDeadlineNanos) {
        m->fail(PluginProcessFailure::AudioDeadline); silence(destination); return Poll::Failed;
    }
    if (!ipc::validBlock(b, m->layout.limits) || b.frames != m->blockFrames || b.outputs != m->blockOutputs ||
        destination.frames != m->blockFrames || destination.outputChannels != m->blockOutputs ||
        (b.outputs && !destination.outputs) || b.disposition > std::uint32_t(PluginProcessDisposition::Error)) {
        m->fail(PluginProcessFailure::Protocol); silence(destination); return Poll::Failed;
    }
    if (b.disposition == std::uint32_t(PluginProcessDisposition::Error)) {
        m->fail(PluginProcessFailure::Processor); silence(destination); return Poll::Failed;
    }
    for (unsigned ch = 0; ch < b.outputs; ++ch) {
        if (!destination.outputs[ch] || !std::all_of(v.channel(2, ch), v.channel(2, ch) + b.frames,
                [](float value) { return std::isfinite(value); })) {
            m->fail(PluginProcessFailure::InvalidOutput); silence(destination); return Poll::Failed;
        }
    }
    PluginEvent event;
    // Validate the whole response before emitting even its first MIDI event.
    for (unsigned i = 0; i < b.outputEvents; ++i) {
        if (!ipc::decode(v.events(true)[i], event, b.frames)) {
            m->fail(PluginProcessFailure::InvalidOutput); silence(destination); return Poll::Failed;
        }
    }
    for (unsigned ch = 0; ch < b.outputs; ++ch)
        std::copy_n(v.channel(2, ch), b.frames, destination.outputs[ch]);
    if (destination.outputEvents) for (unsigned i = 0; i < b.outputEvents; ++i) {
        ipc::decode(v.events(true)[i], event, b.frames);
        destination.outputEvents->push(event);
    }
    disposition = PluginProcessDisposition(b.disposition);
    return Poll::Complete;
}
void PluginProcess::expire() noexcept { if (m->inFlight) m->fail(PluginProcessFailure::AudioDeadline); }

} // namespace daw::plugins
