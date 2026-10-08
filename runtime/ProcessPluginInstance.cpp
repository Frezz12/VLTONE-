#include "ProcessPluginInstance.hpp"

#include <algorithm>
#include <cmath>

namespace daw::plugins {

static_assert(std::atomic<double>::is_always_lock_free);

ProcessPluginInstance::ProcessPluginInstance(const std::string& executable)
    : m_process(executable, {512, engine::kMaxChannels, 4096, 1024 * 1024}) {}

std::unique_ptr<ProcessPluginInstance> ProcessPluginInstance::create(
    const PluginDescriptor& descriptor, const std::string& executable, std::string* error) {
    if (error) error->clear();
    auto instance = std::unique_ptr<ProcessPluginInstance>(new ProcessPluginInstance(executable));
    if (!instance->m_process.load(descriptor)) {
        if (error) *error = instance->m_process.error();
        return {};
    }
    instance->syncValues();
    if (instance->supportsState()) {
        std::vector<std::uint8_t> baseline;
        (void)instance->m_process.saveState(baseline);
        if (instance->failure() != PluginProcessFailure::None) {
            if (error) *error = instance->m_process.error();
            return {};
        }
    }
    return instance;
}

void ProcessPluginInstance::syncValues(bool newCheckpoint) {
    const auto& values = m_process.metadata().values;
    if (m_valueCount != values.size()) {
        m_values = std::make_unique<std::atomic<double>[]>(values.size());
        m_confirmedValues = std::make_unique<std::atomic<double>[]>(values.size());
        m_checkpointEdits = std::make_unique<std::atomic<bool>[]>(values.size());
        m_valueCount = values.size();
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        m_values[i].store(values[i], std::memory_order_relaxed);
        m_confirmedValues[i].store(values[i], std::memory_order_relaxed);
        if (newCheckpoint) m_checkpointEdits[i].store(false, std::memory_order_relaxed);
    }
}

bool ProcessPluginInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    const bool ok = m_process.setBusLayout(wanted);
    accepted = busLayout();
    return ok;
}
bool ProcessPluginInstance::activate(const PluginProcessInfo& info) {
    if (failure() != PluginProcessFailure::None) return false;
    auto limits = m_process.limits();
    const auto capacity = pluginBlockEventCapacity(descriptor(), info, parameters().size());
    // Reserve enough for a parameter-only restore preceding the normal block.
    const auto events = capacity + parameters().size();
    if (events > 2u * 1024u * 1024u) return false;
    const bool growing = info.maxBlockSize > limits.frames || events > limits.events;
    if (growing && !supportsState()) {
        m_restore.clear();
        for (std::uint32_t i = 0; i < m_valueCount; ++i) {
            PluginEvent event; event.paramIndex = i; event.value = parameterValue(i);
            if (!std::isfinite(event.value)) continue;
            m_restore.push_back(event);
        }
    }
    limits.frames = std::max(limits.frames, info.maxBlockSize);
    limits.events = std::max(limits.events, std::uint32_t(events));
    if (!m_process.reserve(limits) || !m_process.activate(info)) return false;
    // Growing the transport captures a fresh opaque checkpoint. Its old
    // journal must not override a preset whose controller mirror is stale.
    syncValues(growing && supportsState());
    std::erase_if(m_restore, [this](const auto& event) { return event.paramIndex >= m_valueCount; });
    for (const auto& event : m_restore) {
        m_values[event.paramIndex].store(event.value, std::memory_order_relaxed);
        m_confirmedValues[event.paramIndex].store(event.value, std::memory_order_relaxed);
        m_checkpointEdits[event.paramIndex].store(true, std::memory_order_relaxed);
    }
    m_events.reserve(limits.events);
    m_reset = false;
    return true;
}
void ProcessPluginInstance::deactivate() { (void)m_process.deactivate(); }
bool ProcessPluginInstance::isActive() const noexcept {
    return failure() == PluginProcessFailure::None && m_process.metadata().active;
}
bool ProcessPluginInstance::isProcessing() const noexcept {
    return isActive() && m_process.metadata().processing;
}
void ProcessPluginInstance::startProcessing() { (void)m_process.startProcessing(); }
void ProcessPluginInstance::stopProcessing() { (void)m_process.stopProcessing(); }
std::int32_t ProcessPluginInstance::parameterIndexForId(std::string_view id) const noexcept {
    const auto all = parameters();
    for (std::size_t i = 0; i < all.size(); ++i) if (all[i].id == id) return std::int32_t(i);
    return -1;
}
double ProcessPluginInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < m_valueCount ? (failure() == PluginProcessFailure::None
        ? m_values[index] : m_confirmedValues[index]).load(std::memory_order_relaxed) : 0;
}
std::string ProcessPluginInstance::parameterText(std::uint32_t index, double value) const {
    if (index >= parameters().size() || !std::isfinite(value)) return {};
    // Formatting must not introduce a blocking foreign call on the UI thread.
    return std::to_string(value);
}
bool ProcessPluginInstance::saveState(std::vector<std::uint8_t>& out) const {
    // A dead slot must not erase its last confirmed opaque state on project
    // save/Undo. Unconfirmed plugin-private edits cannot be reconstructed.
    if (failure() != PluginProcessFailure::None) return m_process.copyCheckpoint(out);
    if (!m_process.saveState(out)) return false;
    const auto& values = m_process.metadata().values;
    for (std::size_t i = 0; i < std::min(values.size(), m_valueCount); ++i) {
        m_confirmedValues[i].store(values[i], std::memory_order_relaxed);
        m_checkpointEdits[i].store(false, std::memory_order_relaxed);
    }
    return true;
}
bool ProcessPluginInstance::loadState(std::span<const std::uint8_t> state) {
    if (!m_process.loadState(state)) return false;
    m_restore.clear(); syncValues(true); return true;
}
std::vector<PluginEvent> ProcessPluginInstance::pendingParameterEvents() {
    auto result = m_process.pendingParameterEvents();
    result.insert(result.end(), m_restore.begin(), m_restore.end());
    return result;
}
void ProcessPluginInstance::setParameterFromHost(std::uint32_t index, double value) {
    // The matching sample-accurate event is already in PluginNode's host queue.
    // The child updates its separate editor/controller after processing it.
    if (index < m_valueCount && std::isfinite(value)) m_values[index].store(value, std::memory_order_relaxed);
}
void ProcessPluginInstance::publishNotices() noexcept {
    const auto notices = m_process.takeNotifications();
    if (!m_listener) return;
    if (notices & PluginProcess::LatencyChanged) m_listener->onLatencyChanged();
    if (notices & PluginProcess::RestartRequested) m_listener->onRestartRequested();
    if (notices & PluginProcess::ReloadRequested) m_listener->onReloadRequested();
    if (notices & PluginProcess::StateChanged) m_listener->onStateChanged();
}
void ProcessPluginInstance::pumpMainThread() {
    m_process.requestMainThread(); // no mailbox transaction, no render gate
    PluginEvent event;
    while (m_process.popNotification(event)) {
        if (event.paramIndex >= m_valueCount) {
            if (m_listener) m_listener->onRestartRequested();
            continue;
        }
        if (event.kind == PluginEvent::Kind::ParamValue) {
            m_values[event.paramIndex].store(event.value, std::memory_order_relaxed);
            m_confirmedValues[event.paramIndex].store(event.value, std::memory_order_relaxed);
            m_checkpointEdits[event.paramIndex].store(true, std::memory_order_relaxed);
            if (m_listener) m_listener->onParameterChanged(event.paramIndex, event.value);
        } else if (m_listener) {
            m_listener->onParameterGesture(event.paramIndex, event.kind == PluginEvent::Kind::ParamGestureBegin);
        }
    }
    publishNotices();
}
bool ProcessPluginInstance::openEditor(void*, PluginEditorHost*) {
    if (!hasEditor() || failure() != PluginProcessFailure::None) return false;
    m_process.requestEditor(true); return true;
}
bool ProcessPluginInstance::serviceOfflineRestart() {
    const bool reconfigure = m_process.serviceOfflineRestart();
    syncValues();
    return reconfigure;
}
PluginProcessDisposition ProcessPluginInstance::process(const PluginProcessContext&) noexcept {
    // This adapter never waits synchronously for foreign DSP. PluginNode and
    // GraphProcessor use the two-phase interface for both live and offline.
    return PluginProcessDisposition::Error;
}
bool ProcessPluginInstance::beginProcess(const PluginProcessContext& context,
                                         PluginProcessDisposition& result) noexcept {
    result = PluginProcessDisposition::Error;
    auto block = context;
    if (!m_restore.empty()) {
        if (m_restore.size() + context.inputEvents.size() > m_events.capacity()) return true;
        m_events.clear();
        m_events.insert(m_events.end(), m_restore.begin(), m_restore.end());
        m_events.insert(m_events.end(), context.inputEvents.begin(), context.inputEvents.end());
        block.inputEvents = m_events;
    }
    for (const auto& event : block.inputEvents)
        if (event.kind == PluginEvent::Kind::ParamValue && event.paramIndex < m_valueCount)
            m_values[event.paramIndex].store(event.value, std::memory_order_relaxed);
    if (!m_process.submit(block, m_reset)) return true;
    m_reset = false; m_restore.clear(); m_events.clear();
    return false;
}
bool ProcessPluginInstance::finishProcess(const PluginProcessContext& context,
    PluginProcessDisposition& result, bool expired) noexcept {
    auto block = context;
    m_output = context.outputEvents; block.outputEvents = this;
    auto state = m_process.poll(block, result);
    // The parent may be descheduled after the child completed on time. Poll
    // validates the child's completion timestamp before we expire pending work.
    if (state == PluginProcess::Poll::Pending && expired) {
        m_process.expire();
        state = m_process.poll(block, result);
    }
    m_output = nullptr;
    if (state == PluginProcess::Poll::Pending) return false;
    if (state == PluginProcess::Poll::Complete && result != PluginProcessDisposition::Error) {
        for (const auto& event : context.inputEvents)
            if (event.kind == PluginEvent::Kind::ParamValue && event.paramIndex < m_valueCount) {
                m_confirmedValues[event.paramIndex].store(event.value, std::memory_order_relaxed);
                m_checkpointEdits[event.paramIndex].store(true, std::memory_order_relaxed);
            }
        // A plugin may emit a later value for the same parameter. It wins over
        // the host input, in the same order as the actual completed block.
        for (const auto& event : m_events) {
            m_confirmedValues[event.paramIndex].store(event.value, std::memory_order_relaxed);
            m_checkpointEdits[event.paramIndex].store(true, std::memory_order_relaxed);
        }
    }
    m_events.clear();
    // Until plugin-originated wake requests are wired directly across the
    // process boundary, keep the child active. Guessing sleep would lose MIDI
    // generators that wake from their own timers rather than host input.
    if (result == PluginProcessDisposition::Sleep || result == PluginProcessDisposition::Tail)
        result = PluginProcessDisposition::Continue;
    publishNotices();
    return true;
}
void ProcessPluginInstance::push(const PluginEvent& event) noexcept {
    if (event.kind == PluginEvent::Kind::ParamValue && event.paramIndex < m_valueCount) {
        m_values[event.paramIndex].store(event.value, std::memory_order_relaxed);
        if (m_events.size() < m_events.capacity()) m_events.push_back(event);
    }
    if (m_output) m_output->push(event);
}
void ProcessPluginInstance::resetForTransport() noexcept { (void)m_process.reset(); m_reset = false; }
bool ProcessPluginInstance::restart() {
    if (!m_process.restart()) return false;
    syncValues(true); m_reset = false; return true;
}
ProcessPluginInstance::Recovery ProcessPluginInstance::recovery() const {
    Recovery result;
    result.descriptor = descriptor(); result.executable = m_process.executable();
    result.hasState = m_process.copyCheckpoint(result.state);
    for (const auto& parameter : parameters()) {
        if (parameter.index >= m_valueCount) continue;
        if (result.hasState && !m_checkpointEdits[parameter.index].load(std::memory_order_relaxed)) continue;
        const auto value = m_confirmedValues[parameter.index].load(std::memory_order_relaxed);
        if (std::isfinite(value)) result.parameters.emplace_back(parameter.id, value);
    }
    return result;
}
std::unique_ptr<ProcessPluginInstance> ProcessPluginInstance::recover(const Recovery& snapshot) {
    auto instance = std::unique_ptr<ProcessPluginInstance>(new ProcessPluginInstance(snapshot.executable));
    if (!instance->m_process.load(snapshot.descriptor) ||
        (snapshot.hasState && !instance->m_process.loadState(snapshot.state))) return {};
    instance->syncValues();
    for (const auto& [id, value] : snapshot.parameters) {
        const auto index = instance->parameterIndexForId(id);
        if (index < 0) continue;
        PluginEvent event; event.paramIndex = std::uint32_t(index); event.value = value;
        instance->m_restore.push_back(event);
        instance->m_values[index].store(value);
        instance->m_confirmedValues[index].store(value);
        instance->m_checkpointEdits[index].store(true);
    }
    return instance;
}

} // namespace daw::plugins
