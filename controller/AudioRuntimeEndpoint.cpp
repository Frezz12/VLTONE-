#include "AudioRuntimeEndpoint.hpp"
#include "AudioRuntimeRpc.hpp"
#include "AudioRuntimePluginFields.hpp"
#include "AudioMiniModuleCompiler.hpp"
#include "AudioRuntimeCalls.hpp"
#include "AudioRuntimeReadoutBatch.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#pragma push_macro("slots")
#undef slots

namespace daw {
namespace {
using Method = audio_rpc::Method;
using Clock = std::chrono::steady_clock;
audio::Result unsupported(std::string message) {
    return audio::Result::fail(audio::EngineError::NotSupported, std::move(message));
}
void require(const audio::Result& result) { if (!result) throw AudioEndpointError(result); }
template<class F> audio::Result guarded(F&& operation) {
    try { return operation(); }
    catch (const AudioEndpointError& error) { return error.result; }
    catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}
void mergeContent(AudioContentSpec& target, AudioContentSpec source) {
    if (source.clips) target.clips = std::move(source.clips);
    if (source.midi) target.midi = std::move(source.midi);
    if (source.plugins) target.plugins = std::move(source.plugins);
    if (source.levels) target.levels = std::move(source.levels);
}
bool changesNativeState(Method method) {
    switch (method) {
    case Method::setPluginParameter:
    case Method::restorePluginState:
    case Method::setInsertPresetReference:
    case Method::setEqualizerAnalyzer:
    case Method::captureEqualizerComparison:
    case Method::activateEqualizerComparison:
    case Method::copyEqualizerComparison:
    case Method::setGravityFrozen:
    case Method::clearGravityTail:
        return true;
    default: return false;
    }
}

struct Subscription {
    std::any value;
    std::deque<std::any> pending;
    std::function<void(audio_rpc::ReadoutBatch&, std::function<void(std::any)>)> append;
    bool consuming = false;
    unsigned copies = 1;
    Clock::time_point used = Clock::now();
};
struct EndpointSession {
    mutable std::mutex mutex;
    AudioRuntimeEndpoint::Metadata metadata;
    AudioSessionPacket packet;
    AudioProcessSnapshot snapshot;
    AudioPluginServiceResult notifications;
    std::shared_ptr<const AudioSessionPublication> publication;
    std::atomic<std::shared_ptr<const AudioInputClockReader>> inputClock;
    std::map<std::string, Subscription> subscriptions;
    std::map<std::uint64_t, AudioSessionPacket> transactions;
    struct Capture {
        AudioCaptureId nativeId = 0;
        std::uint64_t generation = 0;
        audio::RecordingSession expected;
        AudioCaptureStatus status;
    };
    std::map<AudioCaptureId, Capture> captures;
    std::uint64_t nextControl = 0;
    bool dirtyCheckpoint = false, closed = false;
    Clock::time_point checkpointAt = Clock::now();
    std::size_t nextRead = 0;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> nativeTokens;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> clientTokens;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> compileJobs;
};

std::atomic<std::uint64_t> nextInstanceToken{1};
struct IdentityValues {
    static constexpr bool reading = false;
    EndpointSession& session;
    bool incoming = false;
    void instance(std::uint64_t& value) const {
        if (!value) return;
        const auto generation = session.metadata.generation;
        if (incoming) {
            const auto found = session.clientTokens.find(value);
            if (found == session.clientTokens.end() || found->second.first != generation)
                throw AudioEndpointError(audio::Result::fail(audio::EngineError::InvalidArgument, "Plugin instance belongs to a retired audio generation."));
            value = found->second.second;
        } else {
            const auto key = std::pair{generation, value};
            const auto found = session.nativeTokens.find(key);
            if (found != session.nativeTokens.end()) { value = found->second; return; }
            const auto token = nextInstanceToken.fetch_add(1, std::memory_order_relaxed);
            if (!token) throw std::overflow_error("Audio endpoint instance token overflow.");
            session.nativeTokens.emplace(key, token);
            session.clientTokens.emplace(token, key);
            value = token;
        }
    }
    template<class... T> void operator()(T&... value) { (one(value), ...); }
    template<class T> void one(T& value) {
        if constexpr (std::is_same_v<T, AudioPluginAddress>) instance(value.instance);
        else if constexpr (std::is_same_v<T, PluginIdentity>) instance(value.instance);
        else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>) {}
        else if constexpr (requires { value.has_value(); }) { if (value) one(*value); }
        else if constexpr (requires { typename T::value_type; value.begin(); value.end(); }) {
            if constexpr (!std::is_arithmetic_v<typename T::value_type>) for (auto& item : value) one(item);
        } else if constexpr (requires { std::tuple_size<T>::value; })
            std::apply([&](auto&... part) { (*this)(part...); }, value);
        else if constexpr (requires { audio_value::fields(*this, value); }) audio_value::fields(*this, value);
    }
};

/// One serial client for primary, draft and compiler traffic. It never owns an
/// AudioRuntime. A failed poll invalidates telemetry and waits for explicit
/// restart; it cannot silently instantiate or switch to a local engine.
class EndpointBroker {
public:
    EndpointBroker(std::string executable, std::chrono::milliseconds timeout)
        : process(std::move(executable), timeout), worker([this] { run(); }) {}
    ~EndpointBroker() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }
    template<class F> auto sync(F&& operation) -> std::invoke_result_t<F, AudioRuntimeProcess&> {
        using R = std::invoke_result_t<F, AudioRuntimeProcess&>;
        if (std::this_thread::get_id() == worker.get_id()) return operation(process);
        auto task = std::make_shared<std::packaged_task<R()>>(
            [this, operation = std::forward<F>(operation)]() mutable { return operation(process); });
        auto reply = task->get_future();
        {
            std::lock_guard lock(mutex);
            if (stopping) throw AudioEndpointError(unsupported("Audio broker has stopped."));
            jobs.emplace_back([task] { (*task)(); });
        }
        wake.notify_one();
        return reply.get();
    }
    void add(const std::shared_ptr<EndpointSession>& session) {
        std::lock_guard lock(mutex); sessions.push_back(session); wake.notify_one();
    }
    audio::Result restart() {
        return sync([this](AudioRuntimeProcess& child) {
            std::vector<std::shared_ptr<EndpointSession>> family;
            { std::lock_guard lock(mutex); for (const auto& weak : sessions) if (auto state = weak.lock()) {
                std::lock_guard stateLock(state->mutex);
                if (!state->closed && state->metadata.generation) family.push_back(std::move(state));
            } }
            std::vector<std::shared_ptr<AudioInputClockReader>> clocks;
            for (const auto& state : family) {
                std::lock_guard lock(state->mutex);
                if (!state->captures.empty())
                    return unsupported("Finalize interrupted recordings before restarting the audio process.");
            }
            clocks.reserve(family.size());
            for (std::size_t i = 0; i < family.size(); ++i) clocks.push_back(std::make_shared<AudioInputClockReader>());
            const auto result = child.restart();
            std::size_t index = 0;
            for (const auto& state : family) {
                const auto clock = clocks[index++];
                std::lock_guard lock(state->mutex);
                state->subscriptions.clear(); state->transactions.clear();
                if (!result) { failure(*state, result.message()); continue; }
                state->metadata.generation = child.generation(state->metadata.sessionId);
                state->packet.generation = state->metadata.generation;
                state->metadata.connected = true;
                state->metadata.error.clear();
                state->snapshot = {}; state->notifications = {};
                state->snapshot.configuration = child.deviceConfiguration(state->metadata.sessionId);
                state->snapshot.transport = child.lastControlSnapshot(state->metadata.sessionId);
                state->snapshot.device = child.lastDeviceSnapshot(state->metadata.sessionId);
                state->nativeTokens.clear(); state->clientTokens.clear(); state->compileJobs.clear();
                *clock = child.inputClock(state->metadata.sessionId);
                state->inputClock.store(clock, std::memory_order_release);
                state->publication = child.lastPublicationOwner(state->metadata.sessionId);
                for (auto& channel : state->packet.session.graph.channels) channel.capturing = false;
            }
            return result;
        });
    }
    static void acceptSnapshot(EndpointSession& session, AudioProcessSnapshot value) {
        session.metadata.connected = true;
        session.metadata.error.clear();
        session.metadata.generation = value.generation;
        session.metadata.revision = value.revision;
        IdentityValues{session}(value.plugins.notices);
        auto& pending = session.notifications;
        pending.changed |= value.plugins.changed;
        pending.scanned |= value.plugins.scanned;
        if (!value.plugins.error.empty()) pending.error = value.plugins.error;
        pending.notices.insert(pending.notices.end(), std::make_move_iterator(value.plugins.notices.begin()),
            std::make_move_iterator(value.plugins.notices.end()));
        value.plugins = {};
        session.snapshot = std::move(value);
    }
    static void failure(EndpointSession& session, const std::string& message) {
        if (!message.empty() && (session.metadata.connected || session.metadata.error != message))
            session.notifications.error = message;
        session.metadata.connected = false;
        session.metadata.error = message;
        session.snapshot.transport.playing = false;
        session.snapshot.transport.recording = false;
        session.snapshot.transport.state = engine::TransportState::Stopped;
        session.snapshot.device.running = false;
        session.snapshot.meters.clear();
        session.subscriptions.clear();
        session.inputClock.store({}, std::memory_order_release);
    }
private:
    void poll(const std::shared_ptr<EndpointSession>& state) {
        std::uint64_t id;
        {
            std::lock_guard lock(state->mutex);
            if (state->closed || !state->metadata.connected || !state->metadata.generation) return;
            id = state->metadata.sessionId;
        }
        AudioProcessSnapshot next;
        const auto result = process.poll(next, id);
        if (!result) {
            std::lock_guard lock(state->mutex); failure(*state, result.message()); return;
        }
        audio_rpc::ReadoutBatch reads;
        bool checkpoint = false;
        {
            std::lock_guard lock(state->mutex);
            acceptSnapshot(*state, std::move(next));
            const auto now = Clock::now();
            std::erase_if(state->subscriptions, [&](const auto& pair) {
                return now - pair.second.used > std::chrono::seconds(2);
            });
            const auto count = state->subscriptions.size();
            if (count) {
                auto item = std::next(state->subscriptions.begin(), state->nextRead % count);
                for (std::size_t visited = 0; visited < count; ++visited) {
                    if (reads.size() + item->second.copies > audio_rpc::kMaxReadoutBatchItems) break;
                    const auto key = item->first;
                    for (unsigned i = 0; i < item->second.copies; ++i)
                        item->second.append(reads, [state, key](std::any value) {
                            std::lock_guard lock(state->mutex);
                            const auto found = state->subscriptions.find(key);
                            if (found == state->subscriptions.end()) return;
                            if (found->second.consuming) {
                                // Diagnostics consumption is bounded even if its window stops painting.
                                if (found->second.pending.size() == 512) found->second.pending.pop_front();
                                found->second.pending.push_back(std::move(value));
                            } else found->second.value = std::move(value);
                        });
                    ++state->nextRead;
                    if (++item == state->subscriptions.end()) item = state->subscriptions.begin();
                }
            }
            checkpoint = state->dirtyCheckpoint && now >= state->checkpointAt;
        }
        if (!reads.empty()) {
            const auto result = reads.execute(process, id);
            if (!result) {
                std::lock_guard lock(state->mutex); failure(*state, result.message()); return;
            }
        }
        if (checkpoint) {
            const auto saved = process.captureCheckpoint(id);
            std::lock_guard lock(state->mutex);
            state->checkpointAt = Clock::now() + std::chrono::seconds(1);
            if (saved) state->dirtyCheckpoint = false;
            else state->metadata.error = saved.message();
        }
    }
    void run() {
        auto nextPoll = Clock::now();
        for (;;) {
            std::function<void()> job;
            std::vector<std::shared_ptr<EndpointSession>> active;
            {
                std::unique_lock lock(mutex);
                wake.wait_until(lock, nextPoll, [&] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) break;
                // Control traffic must not starve transport/device observations.
                // Poll at its deadline even while callers keep the queue busy.
                if (!jobs.empty() && (stopping || Clock::now() < nextPoll)) {
                    job = std::move(jobs.front()); jobs.pop_front();
                }
                else {
                    std::erase_if(sessions, [](const auto& value) { return value.expired(); });
                    for (const auto& value : sessions) if (auto session = value.lock()) active.push_back(std::move(session));
                }
            }
            if (job) job();
            else {
                for (const auto& session : active) {
                    try { poll(session); }
                    catch (const std::exception& error) { std::lock_guard lock(session->mutex); failure(*session, error.what()); }
                }
                nextPoll = Clock::now() + std::chrono::milliseconds(33);
            }
        }
        process.close();
    }
    AudioRuntimeProcess process;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::vector<std::weak_ptr<EndpointSession>> sessions;
    bool stopping = false;
    std::thread worker;
};

template<class Argument, class Value> audio_rpc::detail::Owned<Argument> own(Value&& value) {
    using Owned = audio_rpc::detail::Owned<Argument>;
    if constexpr (requires { typename std::remove_cvref_t<Argument>::element_type; } &&
        !std::is_same_v<std::remove_cvref_t<Argument>, Owned>) return Owned(value.begin(), value.end());
    else return Owned(std::forward<Value>(value));
}
template<class Binding, class Tuple, std::size_t... I>
typename Binding::Inputs inputs(Tuple& values, std::index_sequence<I...>) {
    return {own<std::tuple_element_t<I, typename Binding::Arguments>>(std::get<I>(values))...};
}
template<class Binding, std::size_t I> consteval std::size_t outputIndex() {
    if constexpr (I == 0) return 0;
    else return outputIndex<Binding, I - 1>() +
        audio_rpc::detail::output<std::tuple_element_t<I - 1, typename Binding::Arguments>>;
}
template<class Binding, std::size_t I, class Tuple> void assignOutput(Tuple& actual, typename Binding::Outputs& output) {
    using Arg = std::tuple_element_t<I, typename Binding::Arguments>;
    if constexpr (audio_rpc::detail::output<Arg>) {
        auto& to = std::get<I>(actual);
        auto& from = std::get<outputIndex<Binding, I>()>(output);
        if constexpr (audio_rpc::detail::mutableSpan<Arg>) {
            if (to.size() != from.size()) throw std::runtime_error("Audio reply changed an output span length.");
            std::copy(from.begin(), from.end(), to.begin());
        } else to = std::move(from);
    }
}
template<class Binding, class Tuple, std::size_t... I>
void outputs(Tuple& actual, typename Binding::Outputs& output, std::index_sequence<I...>) {
    (assignOutput<Binding, I>(actual, output), ...);
}
template<class Binding, std::size_t... I> typename Binding::Reply emptyReply(
    const typename Binding::Inputs& args, std::index_sequence<I...>) {
    return {typename Binding::Result{}, std::tuple_cat(audio_rpc::detail::copyOutput<
        std::tuple_element_t<I, typename Binding::Arguments>>(std::get<I>(args))...)};
}
template<class Value> auto nativeResult(Value value) {
    if constexpr (std::is_same_v<Value, AudioResultValue>) return value.result();
    else if constexpr (std::is_same_v<Value, std::tuple<>>) return;
    else return value;
}
template<Method Id> typename audio_rpc::Binding<Id>::Reply exchange(AudioRuntimeProcess& process,
    std::uint64_t id, const typename audio_rpc::Binding<Id>::Inputs& args) {
    auto [result, reply] = audio_rpc::call<Id>(process, id, args);
    require(result);
    return reply;
}
template<class Reply, class... Args> Reply customExchange(AudioRuntimeProcess& process,
    std::uint64_t id, Method method, const Args&... args) {
    Reply reply{};
    require(process.invoke(std::uint32_t(method),
        [&](ProcessAudioResources& resources) { return audio_value::encodeResources(resources, args...); },
        [&](auto bytes, const auto& directory, auto& cache) {
            reply = std::get<0>(audio_value::decodeResources<Reply>(bytes, directory, &cache));
        }, id));
    return reply;
}
} // namespace

struct AudioRuntimeEndpoint::Impl {
    std::shared_ptr<EndpointBroker> broker;
    std::shared_ptr<EndpointSession> session = std::make_shared<EndpointSession>();
    std::shared_ptr<AudioRuntime> native;
    std::shared_ptr<AudioMiniModuleCompiler> compiler;

    template<Method Id, class Arguments> static void incoming(EndpointSession& state, Arguments& args) {
        IdentityValues{state, true}(args);
        if constexpr (Id == Method::stageMiniModulePreparation || Id == Method::fadeMiniModulePreparation || Id == Method::miniModulePreparationFaded) {
            auto& id = std::get<0>(args);
            const auto found = state.compileJobs.find(id);
            if (found == state.compileJobs.end() || found->second.first != state.metadata.generation)
                throw AudioEndpointError(audio::Result::fail(audio::EngineError::InvalidArgument, "MiniModule job belongs to a retired session."));
            id = found->second.second;
        }
    }

    template<Method Id, bool Cached = false, bool Consume = false, class... Args> auto call(Args&&... values) const {
        using B = audio_rpc::Binding<Id>;
        using R = typename B::Result;
        auto actual = std::forward_as_tuple(values...);
        auto args = inputs<B>(actual, std::index_sequence_for<Args...>{});
        typename B::Reply reply{};
        const auto noReadout = [&] {
            if constexpr (Id == Method::readPluginParameters)
                for (auto& value : std::get<1>(args)) { value.value = 0; value.available = false; }
            auto empty = emptyReply<B>(args, std::index_sequence_for<Args...>{});
            outputs<B>(actual, std::get<1>(empty), std::index_sequence_for<Args...>{});
            return nativeResult(std::move(std::get<0>(empty)));
        };
        if (native) reply = B::template invoke<B::function>(*native, args);
        else {
            try {
                std::uint64_t id;
                { std::lock_guard lock(session->mutex); id = session->metadata.sessionId; }
                if constexpr (Cached) {
                    if constexpr (Id == Method::readPluginParameters)
                        for (auto& value : std::get<1>(args)) { value.value = 0; value.available = false; }
                    const auto bytes = audio_value::encode(std::uint32_t(Id), args);
                    const std::string key(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    std::lock_guard lock(session->mutex);
                    if (!session->metadata.connected) return noReadout();
                    incoming<Id>(*session, args);
                    auto found = session->subscriptions.find(key);
                    if (found == session->subscriptions.end()) {
                        if (session->subscriptions.size() >= 512) {
                            auto oldest = std::min_element(session->subscriptions.begin(), session->subscriptions.end(),
                                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                            session->subscriptions.erase(oldest);
                        }
                        Subscription subscription;
                        subscription.consuming = Consume;
                        if constexpr (Id == Method::popProfile) subscription.copies = 16;
                        subscription.append = [args, weak = std::weak_ptr(session)](audio_rpc::ReadoutBatch& batch, std::function<void(std::any)> sink) {
                            batch.add<Id>(args, [weak, sink = std::move(sink)](typename B::Reply reply) mutable {
                                if constexpr (Id == Method::popProfile) { if (!std::get<0>(reply)) return; }
                                if (const auto state = weak.lock()) {
                                    std::lock_guard lock(state->mutex);
                                    IdentityValues{*state}(reply);
                                    if constexpr (Id == Method::pluginInstanceId) IdentityValues{*state}.instance(std::get<0>(reply));
                                } else return;
                                sink(std::move(reply));
                            });
                        };
                        found = session->subscriptions.emplace(key, std::move(subscription)).first;
                    }
                    found->second.used = Clock::now();
                    if constexpr (Consume) {
                        if (found->second.pending.empty()) return noReadout();
                        reply = std::move(std::any_cast<typename B::Reply&>(found->second.pending.front()));
                        found->second.pending.pop_front();
                    } else {
                        if (const auto* current = std::any_cast<typename B::Reply>(&found->second.value)) reply = *current;
                        else return noReadout();
                    }
                } else {
                    reply = broker->sync([id, state = session, args = std::move(args)](AudioRuntimeProcess& process) mutable {
                        std::string readoutKey;
                        if constexpr (audio_rpc::isReadoutMethod(Id)) {
                            const auto bytes = audio_value::encode(std::uint32_t(Id), args);
                            readoutKey.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                        }
                        { std::lock_guard lock(state->mutex); incoming<Id>(*state, args); }
                        auto result = exchange<Id>(process, id, args);
                        { std::lock_guard lock(state->mutex);
                          IdentityValues{*state}(result);
                          if constexpr (Id == Method::pluginInstanceId) IdentityValues{*state}.instance(std::get<0>(result));
                          if constexpr (audio_rpc::isReadoutMethod(Id)) {
                              // A precise edit read also advances its paint cache. The broker
                              // serializes this with batches, so an older poll cannot overwrite it.
                              const auto found = state->subscriptions.find(readoutKey);
                              if (found != state->subscriptions.end() && !found->second.consuming)
                                  found->second.value = result;
                          }
                          if (changesNativeState(Id)) {
                              const bool changed = [&] {
                                  if constexpr (std::is_same_v<R, bool>) return std::get<0>(result);
                                  else if constexpr (std::is_same_v<R, AudioResultValue>) return bool(std::get<0>(result).result());
                                  else return true;
                              }();
                              if (changed && !state->dirtyCheckpoint) {
                                  state->dirtyCheckpoint = true;
                                  state->checkpointAt = Clock::now() + std::chrono::milliseconds(200);
                              }
                          } }
                        return result;
                    });
                }
            } catch (const AudioEndpointError& error) {
                { std::lock_guard lock(session->mutex);
                  session->metadata.error = error.what(); session->notifications.error = error.what(); }
                if constexpr (std::is_same_v<R, AudioResultValue>) return error.result;
                else return noReadout();
            }
        }
        outputs<B>(actual, std::get<1>(reply), std::index_sequence_for<Args...>{});
        return nativeResult(std::move(std::get<0>(reply)));
    }
    template<class Edit> bool control(AudioControlPacket command, Edit edit) {
        bool accepted = false;
        const auto status = guarded([&] { return broker->sync([&, command = std::move(command)](AudioRuntimeProcess& process) mutable {
            std::uint64_t id;
            { std::lock_guard lock(session->mutex);
              if (!edit(session->packet.session, false)) return audio::Result::ok();
              command.generation = session->packet.generation;
              command.requestId = ++session->nextControl;
              id = session->metadata.sessionId; }
            if (const auto result = process.send(command, id); !result) return result;
            // Only this broker mutates the acknowledged projection. Scalar
            // publication cannot allocate and never copies clips/automation.
            std::lock_guard lock(session->mutex);
            (void)edit(session->packet.session, true);
            accepted = true;
            return audio::Result::ok();
        }); });
        if (!status) { std::lock_guard lock(session->mutex); session->notifications.error = status.message(); }
        return bool(status) && accepted;
    }
    audio::Result send(AudioControlPacket command) {
        return guarded([&] { return broker->sync([&](AudioRuntimeProcess& process) {
            std::uint64_t id;
            { std::lock_guard lock(session->mutex);
              command.generation = session->packet.generation;
              command.requestId = ++session->nextControl;
              id = session->metadata.sessionId; }
            const auto result = process.send(command, id);
            if (!result) { std::lock_guard lock(session->mutex); session->notifications.error = result.message(); }
            if (result && std::holds_alternative<AudioTransportCommand>(command.command)) {
                std::lock_guard lock(session->mutex);
                session->snapshot.transport = process.lastControlSnapshot(id);
            }
            return result;
        }); });
    }
};

AudioRuntimeEndpoint::AudioRuntimeEndpoint(std::string executable, std::chrono::milliseconds timeout)
    : m(std::make_shared<Impl>()) {
    m->broker = std::make_shared<EndpointBroker>(std::move(executable), timeout);
    m->broker->add(m->session);
}
AudioRuntimeEndpoint::AudioRuntimeEndpoint(std::shared_ptr<Impl> impl) : m(std::move(impl)) {}
AudioRuntimeEndpoint::~AudioRuntimeEndpoint() {
    try { if (m && m->broker && metadata().sessionId) (void)closeSession(); }
    catch (...) {} // Destruction cannot report an already failed child.
}
std::shared_ptr<AudioRuntimeEndpoint> AudioRuntimeEndpoint::forWorker(std::shared_ptr<AudioRuntime> runtime) {
    if (!runtime) throw std::invalid_argument("A worker runtime is required.");
    auto impl = std::make_shared<Impl>(); impl->native = std::move(runtime);
    return std::shared_ptr<AudioRuntimeEndpoint>(new AudioRuntimeEndpoint(std::move(impl)));
}
std::shared_ptr<AudioRuntimeEndpoint> AudioRuntimeEndpoint::forTest(std::shared_ptr<AudioRuntime> runtime) {
    return forWorker(std::move(runtime));
}
AudioRuntime& AudioRuntimeEndpoint::nativeForWorkerOrTest() const {
    if (!m->native) throw std::logic_error("A production audio endpoint has no native runtime.");
    return *m->native;
}
bool AudioRuntimeEndpoint::isRemote() const noexcept { return !m->native; }
AudioRuntimeEndpoint::Metadata AudioRuntimeEndpoint::metadata() const {
    std::lock_guard lock(m->session->mutex);
    auto result = m->session->metadata;
    if (m->native) {
        result.sampleRate = m->native->sampleRate;
        result.blockSize = m->native->bufferSize;
        result.prepared = m->native->prepared;
        result.deviceAllowed = m->native->liveDeviceAllowed;
        result.offline = m->native->preparation().offline;
        result.connected = result.prepared;
    }
    return result;
}
audio::Result AudioRuntimeEndpoint::prepare(double rate, std::uint32_t frames, bool offline) {
    if (!(rate >= 1000 && rate <= 768000) || !frames || frames > engine::kMaxBlockSize)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid audio preparation format.");
    if (m->native) { const auto result = m->native->prepare(rate, frames, offline); if (!result) return result; }
    std::lock_guard lock(m->session->mutex);
    auto& meta = m->session->metadata;
    if (meta.generation && (meta.sampleRate != rate || meta.blockSize != frames || meta.offline != offline))
        return unsupported("Prepare a changed audio format with a new complete session generation.");
    meta.sampleRate = rate; meta.blockSize = frames; meta.offline = offline; meta.prepared = true;
    return audio::Result::ok();
}

// BEGIN SESSION OPERATIONS
audio::Result AudioRuntimeEndpoint::applySession(AudioSessionSpec value, bool reconfigure,
    std::span<const AudioPluginStateEdit> restores, std::span<const AudioPluginCheckpoint> checkpoints) {
    if (m->native) {
        auto publication = std::make_shared<AudioSessionPublication>();
        const auto result = m->native->applySession(std::move(value), reconfigure, restores, checkpoints, publication.get());
        if (result) { std::lock_guard lock(m->session->mutex); m->session->publication = std::move(publication); }
        return result;
    }
    if (!metadata().generation) return replaceSession(std::move(value), restores, checkpoints);
    if (!checkpoints.empty()) return unsupported("Import checkpoints in a new session generation.");
    std::vector<AudioPluginStateEdit> owned(restores.begin(), restores.end());
    return guarded([&] { return m->broker->sync([&, value = std::move(value), owned = std::move(owned)](
        AudioRuntimeProcess& process) mutable {
        AudioSessionPacket next;
        std::uint64_t id;
        {
            std::lock_guard lock(m->session->mutex);
            next = m->session->packet; id = m->session->metadata.sessionId;
            IdentityValues{*m->session, true}(owned);
        }
        const auto revision = next.revision;
        if (revision == std::numeric_limits<std::uint64_t>::max())
            return audio::Result::fail(audio::EngineError::InvalidArgument, "Audio revision overflow.");
        // Runtime optional content sections retain old schedules. Keep that
        // same complete acknowledged value for a future process restart.
        for (auto& channel : value.channels) {
            const auto old = std::find_if(next.session.channels.begin(), next.session.channels.end(),
                [&](const auto& item) { return item.id == channel.id; });
            if (old != next.session.channels.end()) {
                auto merged = old->content; mergeContent(merged, std::move(channel.content));
                channel.content = std::move(merged);
            }
        }
        next.session = std::move(value); next.restores = std::move(owned);
        next.checkpoints.clear(); next.transport.clear(); ++next.revision;
        // Every allocation for our acknowledgment precedes publication. The
        // process separately retains compatible opaque/source restore journals.
        const auto result = process.applySession(next, revision, id, reconfigure);
        if (!result) return result;
        std::lock_guard lock(m->session->mutex);
        m->session->packet = std::move(next);
        m->session->metadata.revision = revision + 1;
        m->session->publication = process.lastPublicationOwner(id);
        m->session->subscriptions.clear();
        return audio::Result::ok();
    }); });
}
audio::Result AudioRuntimeEndpoint::replaceSession(AudioSessionSpec value,
    std::span<const AudioPluginStateEdit> restores, std::span<const AudioPluginCheckpoint> checkpoints,
    std::span<const AudioTransportCommand> transport) {
    const auto format = metadata();
    return replacePreparedSession(std::move(value), format.sampleRate, format.blockSize, format.offline,
        restores, checkpoints, transport);
}
audio::Result AudioRuntimeEndpoint::replacePreparedSession(AudioSessionSpec value, double rate,
    std::uint32_t frames, bool offline, std::span<const AudioPluginStateEdit> restores,
    std::span<const AudioPluginCheckpoint> checkpoints, std::span<const AudioTransportCommand> transport) {
    if (!(rate >= 1000 && rate <= 768000) || !frames || frames > engine::kMaxBlockSize)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid audio preparation format.");
    if (m->native) return guarded([&] {
        if (m->native->deviceOpen || m->native->hasActiveCaptures())
            return unsupported("Native worker replacement requires a stopped device and no capture writers.");
        std::vector<AudioPluginStateEdit> retainedRestores(restores.begin(), restores.end());
        std::vector<AudioPluginCheckpoint> retainedCheckpoints(checkpoints.begin(), checkpoints.end());
        if (checkpoints.empty() && m->native->prepared &&
            (m->native->sampleRate != rate || m->native->bufferSize != frames || m->native->preparation().offline != offline)) {
            if (const auto result = m->native->capturePluginCheckpoints(retainedCheckpoints, AudioPluginCheckpointPurpose::Recovery); !result)
                return result;
            const auto compatible = [&](const AudioPluginAddress& address, const std::string& uid, plugins::Format format) {
                for (const auto& chain : value.pluginChains) if (chain.channelId == address.channelId)
                    for (const auto& slot : chain.slots) if (slot.id == address.slotId && slot.uid == uid &&
                        slot.requiredFormat == format && (!address.right || slot.channelMode == PluginChannelMode::DualMono)) return true;
                return false;
            };
            std::erase_if(retainedCheckpoints, [&](const auto& checkpoint) {
                return !compatible({checkpoint.channelId, checkpoint.slotId}, checkpoint.uid, checkpoint.format);
            });
            for (const auto& address : m->native->pluginAddresses()) {
                const auto snapshot = m->native->pluginStateSnapshot(address);
                if (!snapshot.ownsSample || !compatible(address, snapshot.descriptor.uid, snapshot.descriptor.format) ||
                    std::any_of(retainedRestores.begin(), retainedRestores.end(), [&](const auto& edit) {
                        return edit.address.channelId == address.channelId && edit.address.slotId == address.slotId && edit.address.right == address.right;
                    })) continue;
                AudioPluginStateEdit edit;
                edit.address = address; edit.address.instance = 0;
                edit.state.state = snapshot.state; edit.state.source = snapshot.sample; edit.state.sourcePath = snapshot.samplePath;
                edit.parameters = snapshot.parameters;
                retainedRestores.push_back(std::move(edit));
            }
        }
        auto candidate = std::make_shared<AudioRuntime>();
        candidate->isRenderClone = m->native->isRenderClone;
        candidate->renderingPass = m->native->renderingPass;
        candidate->renderTapsPreFader = m->native->renderTapsPreFader;
        candidate->renderTapsAtSource = m->native->renderTapsAtSource;
        candidate->liveDeviceAllowed = m->native->liveDeviceAllowed;
        m->native->inheritAudioWorkers(*candidate);
        if (const auto result = candidate->prepare(rate, frames, offline); !result) return result;
        auto publication = std::make_shared<AudioSessionPublication>();
        if (const auto result = candidate->applySession(std::move(value), false, retainedRestores, retainedCheckpoints, publication.get()); !result) return result;
        if (m->native->metronome) candidate->setMetronomeSample(m->native->metronome->sample());
        for (const auto& command : transport) candidate->transportCommand(command);
        m->native = std::move(candidate);
        std::lock_guard lock(m->session->mutex);
        m->session->publication = std::move(publication);
        m->session->transactions.clear();
        return audio::Result::ok();
    });
    AudioSessionPacket packet;
    packet.session = std::move(value);
    packet.restores.assign(restores.begin(), restores.end());
    packet.checkpoints.assign(checkpoints.begin(), checkpoints.end());
    packet.transport.assign(transport.begin(), transport.end());
    return guarded([&] { return m->broker->sync([&, packet = std::move(packet)](AudioRuntimeProcess& process) mutable {
        const auto clock = std::make_shared<AudioInputClockReader>();
        Metadata meta;
        audio::AudioDeviceConfig configuration;
        { std::lock_guard lock(m->session->mutex); meta = m->session->metadata; configuration = m->session->snapshot.configuration; }
        if (!meta.prepared) return audio::Result::fail(audio::EngineError::NotInitialized, "Prepare the audio endpoint first.");
        // Only the first session may launch implicitly. A replacement after a
        // crash must not bypass capture finalization and the recovery journal.
        if (meta.generation && !process.running())
            return audio::Result::fail(audio::EngineError::NotInitialized,
                "Recover the audio process before replacing its session.");
        const bool createSecondary = meta.secondary && !meta.generation;
        const auto previous = createSecondary ? 0 : process.generation(meta.sessionId);
        if (previous == std::numeric_limits<std::uint64_t>::max())
            return audio::Result::fail(audio::EngineError::InvalidArgument, "Audio generation overflow.");
        packet.generation = previous + 1; packet.revision = 1;
        packet.sampleRate = rate; packet.blockSize = frames; packet.offline = offline;
        const bool changingFormat = meta.generation &&
            (meta.sampleRate != rate || meta.blockSize != frames || meta.offline != offline);
        if (changingFormat && packet.checkpoints.empty())
            process.retainSessionState(packet, meta.sessionId);
        const auto result = createSecondary
            ? process.createSession(packet, meta.sessionId)
            : process.replaceSession(packet, meta.deviceAllowed, configuration, meta.sessionId, changingFormat);
        if (!result) return result;
        std::lock_guard lock(m->session->mutex);
        m->session->packet = std::move(packet);
        m->session->metadata.generation = previous + 1;
        m->session->metadata.sessionId = meta.sessionId;
        m->session->metadata.sampleRate = rate; m->session->metadata.blockSize = frames;
        m->session->metadata.offline = offline;
        m->session->metadata.revision = 1;
        m->session->metadata.connected = true;
        m->session->metadata.error.clear();
        m->session->publication = process.lastPublicationOwner(meta.sessionId);
        *clock = process.inputClock(meta.sessionId);
        m->session->inputClock.store(clock, std::memory_order_release);
        m->session->transactions.clear(); m->session->subscriptions.clear();
        m->session->nativeTokens.clear(); m->session->clientTokens.clear(); m->session->compileJobs.clear();
        m->session->snapshot = {}; m->session->notifications = {};
        m->session->snapshot.configuration = process.deviceConfiguration(meta.sessionId);
        m->session->snapshot.transport = process.lastControlSnapshot(meta.sessionId);
        m->session->snapshot.device = process.lastDeviceSnapshot(meta.sessionId);
        m->session->dirtyCheckpoint = false;
        return audio::Result::ok();
    }); });
}
audio::Result AudioRuntimeEndpoint::createSecondary(std::shared_ptr<AudioRuntimeEndpoint>& out,
    AudioSessionSpec value, std::span<const AudioPluginStateEdit> restores,
    std::span<const AudioPluginCheckpoint> checkpoints) {
    if (m->native) return unsupported("Create worker runtimes through their explicit owner.");
    auto next = std::make_shared<Impl>(); next->broker = m->broker;
    next->session->metadata = metadata();
    next->session->metadata.deviceAllowed = false;
    next->session->metadata.secondary = true;
    next->session->metadata.generation = next->session->metadata.revision = 1;
    auto& packet = next->session->packet;
    packet.generation = packet.revision = 1;
    packet.sampleRate = next->session->metadata.sampleRate;
    packet.blockSize = next->session->metadata.blockSize;
    packet.offline = next->session->metadata.offline;
    packet.session = std::move(value);
    packet.restores.assign(restores.begin(), restores.end());
    packet.checkpoints.assign(checkpoints.begin(), checkpoints.end());
    // Allocate the C++ owner before the server can publish its numeric session.
    auto endpoint = std::shared_ptr<AudioRuntimeEndpoint>(new AudioRuntimeEndpoint(next));
    const auto clock = std::make_shared<AudioInputClockReader>();
    const auto result = guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        const auto result = process.createSession(packet, next->session->metadata.sessionId);
        if (result) { *clock = process.inputClock(next->session->metadata.sessionId);
            next->session->publication = process.lastPublicationOwner(next->session->metadata.sessionId); }
        return result;
    }); });
    if (!result) return result;
    next->session->metadata.connected = true;
    next->session->inputClock.store(clock, std::memory_order_release);
    m->broker->add(next->session);
    out = std::move(endpoint);
    return audio::Result::ok();
}
std::shared_ptr<AudioRuntimeEndpoint> AudioRuntimeEndpoint::createSecondaryEndpoint() {
    if (m->native) return forWorker(std::make_shared<AudioRuntime>());
    auto impl = std::make_shared<Impl>(); impl->broker = m->broker;
    auto meta = metadata();
    meta.secondary = true; meta.deviceAllowed = meta.connected = false;
    meta.sessionId = meta.generation = meta.revision = 0; meta.error.clear();
    impl->session->metadata = std::move(meta);
    auto endpoint = std::shared_ptr<AudioRuntimeEndpoint>(new AudioRuntimeEndpoint(impl));
    m->broker->add(impl->session);
    return endpoint;
}
audio::Result AudioRuntimeEndpoint::startAudition(const std::shared_ptr<AudioRuntimeEndpoint>& secondary) {
    if (!secondary) return audio::Result::fail(audio::EngineError::InvalidArgument, "An audition session is required.");
    if (m->native && secondary->m->native) return m->native->startAudition(secondary->m->native);
    if (m->broker != secondary->m->broker || metadata().sessionId != 0)
        return unsupported("Audition sessions must belong to the same primary audio process.");
    const auto id = secondary->metadata().sessionId;
    return guarded([&] { return m->broker->sync([id](AudioRuntimeProcess& process) { return process.startAudition(id); }); });
}
void AudioRuntimeEndpoint::stopAudition() {
    if (m->native) { m->native->stopAudition(); return; }
    (void)guarded([&] { return m->broker->sync([](AudioRuntimeProcess& process) {
        return process.running() ? process.stopAudition() : audio::Result::ok();
    }); });
}
audio::Result AudioRuntimeEndpoint::closeSession() {
    if (m->native) return unsupported("Worker lifetime is owned by its caller.");
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        std::uint64_t id;
        { std::lock_guard lock(m->session->mutex); if (m->session->closed) return audio::Result::ok(); id = m->session->metadata.sessionId; }
        if (!id) return unsupported("The primary session closes with its shared process owner.");
        const auto result = process.closeSession(id);
        if (result) { std::lock_guard lock(m->session->mutex); m->session->closed = true; EndpointBroker::failure(*m->session, {}); }
        return result;
    }); });
}
audio::Result AudioRuntimeEndpoint::restart() {
    if (m->native || metadata().secondary) return unsupported("Restart is owned by the primary process endpoint.");
    return guarded([&] { return m->broker->restart(); });
}
audio::Result AudioRuntimeEndpoint::captureRecoveryCheckpoint() {
    if (m->native) { std::vector<AudioPluginCheckpoint> ignored; return m->native->capturePluginCheckpoints(ignored, AudioPluginCheckpointPurpose::Recovery); }
    const auto id = metadata().sessionId;
    return guarded([&] { return m->broker->sync([id](AudioRuntimeProcess& process) { return process.captureCheckpoint(id); }); });
}
std::uint64_t AudioRuntimeEndpoint::processId() const {
    if (m->native) return 0;
    return m->broker->sync([](AudioRuntimeProcess& process) { return process.processId(); });
}
audio::Result AudioRuntimeEndpoint::advanceForTest(std::uint32_t frames, std::uint32_t blocks,
    std::span<const float> inputChannels) {
    if (m->native) return unsupported("Drive the explicit native worker directly in its tests.");
    const auto id = metadata().sessionId;
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        return process.advanceForTest(frames, blocks, id, inputChannels);
    }); });
}
AudioRuntimeEndpoint::TransactionId AudioRuntimeEndpoint::captureTransaction() {
    if (m->native) return m->native->captureTransaction();
    return m->broker->sync([&](AudioRuntimeProcess& process) {
        AudioSessionPacket packet; std::uint64_t id;
        { std::lock_guard lock(m->session->mutex); packet = m->session->packet; id = m->session->metadata.sessionId; }
        std::uint64_t token = 0; require(process.captureTransaction(token, id));
        try { std::lock_guard lock(m->session->mutex); m->session->transactions.emplace(token, std::move(packet)); }
        catch (...) { (void)process.releaseTransaction(token, id); throw; }
        return token;
    });
}
audio::Result AudioRuntimeEndpoint::restoreTransaction(TransactionId token) {
    if (m->native) return m->native->restoreTransaction(token);
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        AudioSessionPacket packet; std::uint64_t id;
        {
            std::lock_guard lock(m->session->mutex);
            const auto found = m->session->transactions.find(token);
            if (found == m->session->transactions.end()) return audio::Result::fail(audio::EngineError::InvalidArgument, "Unknown endpoint transaction.");
            packet = found->second; id = m->session->metadata.sessionId;
        }
        const auto result = process.restoreTransaction(token, id);
        if (result) {
            std::lock_guard lock(m->session->mutex);
            m->session->metadata.revision = packet.revision;
            m->session->packet = std::move(packet); m->session->subscriptions.clear();
        }
        return result;
    }); });
}
void AudioRuntimeEndpoint::releaseTransaction(TransactionId token) {
    if (m->native) { m->native->releaseTransaction(token); return; }
    const auto result = guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        const auto result = process.running() ? process.releaseTransaction(token, metadata().sessionId) : audio::Result::ok();
        return result;
    }); });
    std::lock_guard lock(m->session->mutex);
    m->session->transactions.erase(token);
    if (!result) m->session->notifications.error = result.message();
}
void AudioRuntimeEndpoint::collectTransactionRetirements(TransactionId token) {
    if (m->native) m->native->collectTransactionRetirements(token);
    // The server releases transaction retirement owners after leaving its gate.
}
bool AudioRuntimeEndpoint::hasChannel(const std::string& channelId) const {
    if (m->native) return m->native->trackNodes(channelId) != nullptr;
    std::lock_guard lock(m->session->mutex);
    if (channelId == AudioGraphSpec::masterChannelId) return m->session->metadata.generation != 0;
    const auto& channels = m->session->packet.session.graph.channels;
    return std::any_of(channels.begin(), channels.end(), [&](const auto& value) { return value.id == channelId; });
}
std::vector<AudioPluginAddress> AudioRuntimeEndpoint::retiringPlugins(std::span<const AudioPluginChainSpec> wanted) const {
    return m->call<Method::retiringPlugins>(wanted);
}
bool AudioRuntimeEndpoint::requiresPluginPreparation(const AudioPluginAddress& address, const AudioPluginSpec& spec) const {
    return m->call<Method::requiresPluginPreparation>(address, spec);
}
std::vector<AudioPluginStateSnapshot> AudioRuntimeEndpoint::lastImportedPluginStates() const {
    // Owned canonical mirrors remain valid across a concurrent recovery.
    std::lock_guard lock(m->session->mutex);
    if (!m->session->publication) return {};
    return m->session->publication->imported;
}
void AudioRuntimeEndpoint::transportCommand(const AudioTransportCommand& command) {
    if (m->native) { m->native->transportCommand(command); return; }
    AudioControlPacket packet; packet.command = command; (void)m->send(std::move(packet));
}
AudioTransportSnapshot AudioRuntimeEndpoint::transportSnapshot() const {
    if (m->native) return m->native->transportSnapshot();
    std::lock_guard lock(m->session->mutex); return m->session->snapshot.transport;
}
double AudioRuntimeEndpoint::presentationPositionSeconds() const {
    if (m->native) return m->native->engine.transport().presentationPositionSeconds();
    const auto snapshot = transportSnapshot();
    if (snapshot.playing) {
        if (const auto clock = m->session->inputClock.load(std::memory_order_acquire)) {
            double seconds;
            if (clock->readPresentation(engine::PresentationFrameTime::now(), seconds)) return seconds;
        }
    }
    return snapshot.presentationSeconds;
}
AudioRuntimeDiagnostics AudioRuntimeEndpoint::diagnostics() const {
    if (m->native) return m->native->diagnostics();
    std::lock_guard lock(m->session->mutex); return m->session->snapshot.runtime;
}
AudioDeviceSnapshot AudioRuntimeEndpoint::deviceSnapshot() const {
    if (m->native) return m->native->deviceSnapshot();
    std::lock_guard lock(m->session->mutex); return m->session->snapshot.device;
}
AudioPluginServiceResult AudioRuntimeEndpoint::servicePlugins(bool externallyActive) {
    if (m->native) return m->native->servicePlugins(externallyActive);
    std::lock_guard lock(m->session->mutex);
    auto result = std::move(m->session->notifications); m->session->notifications = {};
    return result;
}
audio::Result AudioRuntimeEndpoint::configureDevice(const audio::AudioDeviceConfig& requested, audio::AudioDeviceConfig& actual) {
    if (m->native) {
        const auto result = m->native->openDevice(requested);
        if (result) actual = m->native->deviceConfiguration();
        return result;
    }
    if (metadata().secondary) return unsupported("A secondary audio session cannot configure hardware.");
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        audio::AudioDeviceConfig configured;
        const auto result = process.configureDevice(requested, configured);
        if (result) {
            std::lock_guard lock(m->session->mutex);
            m->session->snapshot.configuration = configured;
            m->session->snapshot.device = process.lastDeviceSnapshot();
            m->session->snapshot.transport = process.lastControlSnapshot();
            m->session->metadata.deviceAllowed = true;
            actual = std::move(configured);
        }
        return result;
    }); });
}
audio::Result AudioRuntimeEndpoint::startDevice() {
    if (m->native) return m->native->startDevice();
    if (metadata().secondary) return unsupported("A secondary audio session cannot start hardware.");
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        const auto result = process.startDevice();
        if (result) { std::lock_guard lock(m->session->mutex); m->session->snapshot.device = process.lastDeviceSnapshot(); }
        return result;
    }); });
}
audio::Result AudioRuntimeEndpoint::stopDevice() {
    if (m->native) return m->native->stopDevice();
    return guarded([&] { return m->broker->sync([](AudioRuntimeProcess& process) { return process.stopDevice(); }); });
}
audio::Result AudioRuntimeEndpoint::detachDeviceCallback() {
    if (m->native) return m->native->detachDeviceCallback();
    return guarded([&] { return m->broker->sync([](AudioRuntimeProcess& process) { return process.detachDeviceCallback(); }); });
}
void AudioRuntimeEndpoint::closeDevice() {
    if (m->native) { m->native->closeDevice(); return; }
    const auto result = guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        if (!metadata().generation || !process.running()) return audio::Result::ok();
        return process.closeDevice();
    }); });
    std::lock_guard lock(m->session->mutex);
    m->session->metadata.deviceAllowed = false;
    if (!result) m->session->metadata.error = result.message();
}
bool AudioRuntimeEndpoint::setFader(const std::string& channelId, AudioFaderTarget target,
    const AudioFaderChange& change, const std::string& clipId) {
    if (m->native) return m->native->setFader(channelId, target, change, clipId);
    AudioControlPacket command; command.command = AudioFaderCommand{channelId, clipId, target, change};
    return m->control(std::move(command), [&](AudioSessionSpec& value, bool commit) {
        const auto apply = [&](float& gain, float& pan) {
            if (commit) { if (change.gain) gain = *change.gain; if (change.pan) pan = *change.pan; }
        };
        if (channelId == "master" && target == AudioFaderTarget::Channel) {
            apply(value.graph.masterVolume, value.graph.masterPan); return true;
        }
        auto channel = std::find_if(value.graph.channels.begin(), value.graph.channels.end(), [&](const auto& item) { return item.id == channelId; });
        if (channel == value.graph.channels.end()) return false;
        if (target == AudioFaderTarget::Channel) {
            apply(channel->volume, channel->pan);
            if (commit) { if (change.silent) channel->silent = *change.silent; if (change.mono) channel->mono = *change.mono; }
        } else if (target == AudioFaderTarget::Sampler) apply(channel->samplerVolume, channel->samplerPan);
        else {
            auto clip = std::find_if(channel->clipFx.begin(), channel->clipFx.end(), [&](const auto& item) { return item.id == clipId; });
            if (clip == channel->clipFx.end()) return false;
            apply(clip->gain, clip->pan);
        }
        return true;
    });
}
bool AudioRuntimeEndpoint::setInput(const std::string& channelId, const AudioGraphSpec::Input& input) {
    if (m->native) return m->native->setInput(channelId, input);
    AudioControlPacket command; command.command = AudioInputCommand{channelId, input};
    return m->control(std::move(command), [&](AudioSessionSpec& value, bool commit) {
        auto channel = std::find_if(value.graph.channels.begin(), value.graph.channels.end(), [&](const auto& item) { return item.id == channelId; });
        if (channel == value.graph.channels.end()) return false;
        if (commit) channel->input = input;
        return true;
    });
}
bool AudioRuntimeEndpoint::setSend(const std::string& channelId, const std::string& sendId, float level, bool enabled) {
    if (m->native) return m->native->setSend(channelId, sendId, level, enabled);
    AudioControlPacket command; command.command = AudioSendCommand{channelId, sendId, level, enabled};
    return m->control(std::move(command), [&](AudioSessionSpec& value, bool commit) {
        auto channel = std::find_if(value.graph.channels.begin(), value.graph.channels.end(), [&](const auto& item) { return item.id == channelId; });
        if (channel == value.graph.channels.end()) return false;
        auto send = std::find_if(channel->sends.begin(), channel->sends.end(), [&](const auto& item) { return item.id == sendId; });
        if (send == channel->sends.end()) return false;
        if (commit) { send->level = level; send->enabled = enabled; }
        return true;
    });
}
bool AudioRuntimeEndpoint::sendLiveMidi(const std::string& channelId, const engine::MidiEvent& event) {
    if (m->native) return m->native->sendLiveMidi(channelId, event);
    AudioControlPacket packet; packet.command = AudioMidiCommand{channelId, event}; return bool(m->send(std::move(packet)));
}
void AudioRuntimeEndpoint::previewCommand(const AudioPreviewCommand& command) {
    if (m->native) { m->native->previewCommand(command); return; }
    AudioControlPacket packet; packet.command = command; (void)m->send(std::move(packet));
}
bool AudioRuntimeEndpoint::openPluginEditor(const AudioPluginAddress& address, void* parent, plugins::PluginEditorHost* host) {
    if (m->native) return m->native->openPluginEditor(address, parent, host);
    const auto id = metadata().sessionId;
    const bool requested = m->broker->sync([&, target = address](AudioRuntimeProcess& process) mutable {
        { std::lock_guard lock(m->session->mutex); IdentityValues{*m->session, true}(target); }
        return customExchange<bool>(process, id, Method::openPluginEditor, target);
    });
    if (requested) {
        // Paint polling addresses the selected slot without an instance token.
        // Publish its new pending/open status before returning, so a retry
        // cannot observe an earlier view's failure in that existing cache.
        auto live = address; live.instance = 0;
        (void)m->call<Method::pluginEditorSnapshot>(live);
    }
    return requested;
}
audio::Result AudioRuntimeEndpoint::showDeviceControlPanel(const std::string& device, void* nativeWindow) {
    if (m->native) return m->native->showDeviceControlPanel(device, nativeWindow);
    const auto id = metadata().sessionId;
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        return customExchange<AudioResultValue>(process, id, Method::showDeviceControlPanel, device).result();
    }); });
}

double AudioRuntimeEndpoint::inputBeatsAt(std::uint64_t timestamp) const noexcept {
    if (m->native) return m->native->inputBeatsAt(timestamp);
    const auto clock = m->session->inputClock.load(std::memory_order_acquire);
    return clock ? clock->inputBeatsAt(timestamp) : 0;
}
AudioMeterSnapshot AudioRuntimeEndpoint::meterSnapshot(const std::string& channelId,
    const std::string& clipId, bool sampler) const {
    if (m->native) return m->native->meterSnapshot(channelId, clipId, sampler);
    if (!clipId.empty() || sampler) return m->call<Method::meterSnapshot, true>(channelId, clipId, sampler);
    std::lock_guard lock(m->session->mutex);
    const auto found = m->session->snapshot.meters.find(channelId);
    return found == m->session->snapshot.meters.end() ? AudioMeterSnapshot{} : found->second;
}
AudioTimingSnapshot AudioRuntimeEndpoint::timingSnapshot(bool callback, bool drain) {
    if (drain) return m->call<Method::timingSnapshot, true, true>(callback, drain);
    return m->call<Method::timingSnapshot, true>(callback, drain);
}
bool AudioRuntimeEndpoint::popProfile(unsigned worker, rt::ProfileEvent& event) {
    rt::ProfileEvent next{};
    if (!m->call<Method::popProfile, true, true>(worker, next)) return false;
    event = next; return true;
}
engine::PrepareInfo AudioRuntimeEndpoint::preparation() const {
    if (m->native) return m->native->preparation();
    const auto meta = metadata();
    return {meta.sampleRate, meta.blockSize, 2, meta.offline};
}
audio::AudioDeviceConfig AudioRuntimeEndpoint::deviceConfiguration() const {
    if (m->native) return m->native->deviceConfiguration();
    std::lock_guard lock(m->session->mutex); return m->session->snapshot.configuration;
}
bool AudioRuntimeEndpoint::hasInputRoute(const std::string& channelId) const {
    if (m->native) return m->native->hasInputRoute(channelId);
    std::lock_guard lock(m->session->mutex);
    const auto& channels = m->session->packet.session.graph.channels;
    const auto found = std::find_if(channels.begin(), channels.end(), [&](const auto& channel) { return channel.id == channelId; });
    return found != channels.end() && found->input.present;
}
bool AudioRuntimeEndpoint::applyContent(const std::string& channelId, AudioContentSpec content) {
    if (m->native) return m->native->applyContent(channelId, std::move(content));
    validateAudioContent(content);
    return m->broker->sync([&, content = std::move(content)](AudioRuntimeProcess&) mutable {
        AudioSessionSpec projection;
        { std::lock_guard lock(m->session->mutex); projection = m->session->packet.session; }
        auto found = std::find_if(projection.channels.begin(), projection.channels.end(), [&](const auto& channel) { return channel.id == channelId; });
        if (found == projection.channels.end()) return false;
        mergeContent(found->content, std::move(content));
        require(applySession(std::move(projection)));
        return true;
    });
}
void AudioRuntimeEndpoint::setMetronomeEnabled(bool enabled) {
    m->call<Method::setMetronomeEnabled>(enabled);
    if (!m->native) { std::lock_guard lock(m->session->mutex); m->session->packet.session.graph.metronomeEnabled = enabled; }
}
audio::Result AudioRuntimeEndpoint::restorePluginState(const AudioPluginAddress& address,
    const AudioPluginStateRestore& state, std::vector<InsertParameter>& parameters) {
    if (m->native) return m->native->restorePluginState(address, state, parameters);
    if (state.state.empty()) return m->call<Method::restorePluginState>(address, state, parameters);
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        AudioSessionSpec projection;
        { std::lock_guard lock(m->session->mutex); projection = m->session->packet.session; }
        for (auto& chain : projection.pluginChains) if (chain.channelId == address.channelId)
            for (auto& slot : chain.slots) if (slot.id == address.slotId) {
                if (slot.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly)
                    return unsupported("A blocked plugin cannot accept native state.");
                slot.loadPolicy = AudioPluginLoadPolicy::Required;
                if (slot.channelMode == PluginChannelMode::DualMono) {
                    if (const auto saved = process.captureCheckpoint(metadata().sessionId); !saved) return saved;
                }
            }
        AudioPluginStateEdit edit{address, state, parameters, true};
        const auto result = applySession(std::move(projection), false, std::span(&edit, 1));
        if (!result) return result;
        for (const auto& imported : lastImportedPluginStates())
            if (imported.address.channelId == address.channelId && imported.address.slotId == address.slotId && imported.address.right == address.right) {
                parameters = imported.parameters;
                break;
            }
        return audio::Result::ok();
    }); });
}
audio::Result AudioRuntimeEndpoint::restorePluginCheckpoints(std::span<const AudioPluginCheckpoint> checkpoints) {
    if (m->native) return m->native->restorePluginCheckpoints(checkpoints);
    return unsupported("Attach checkpoints to replaceSession before native publication.");
}

namespace {
class EndpointCompiler final : public AudioMiniModuleCompiler {
public:
    EndpointCompiler(std::shared_ptr<EndpointBroker> broker, std::weak_ptr<EndpointSession> state)
        : broker(std::move(broker)), state(std::move(state)) {}
    std::uint64_t start(AudioMiniModuleCompileRequest request) override {
        const auto current = state.lock(); if (!current) return 0;
        return broker->sync([&, request = std::move(request)](AudioRuntimeProcess& process) mutable {
            std::uint64_t sessionId, generation;
            { std::lock_guard lock(current->mutex); IdentityValues{*current, true}(request);
              sessionId = current->metadata.sessionId; generation = current->metadata.generation; }
            const auto childId = customExchange<std::uint64_t>(process, sessionId, Method::startMiniModuleCompile, request);
            const auto token = nextInstanceToken.fetch_add(1, std::memory_order_relaxed);
            try { std::lock_guard lock(current->mutex); current->compileJobs.emplace(token, std::pair{generation, childId}); }
            catch (...) { (void)customExchange<std::tuple<>>(process, sessionId, Method::forgetMiniModuleCompile, childId); throw; }
            return token;
        });
    }
    AudioMiniModuleCompileStatus poll(std::uint64_t token) override {
        const auto current = state.lock(); if (!current) return {};
        return broker->sync([&](AudioRuntimeProcess& process) {
            std::uint64_t sessionId, childId;
            { std::lock_guard lock(current->mutex);
              const auto found = current->compileJobs.find(token);
              if (found == current->compileJobs.end() || found->second.first != current->metadata.generation) return AudioMiniModuleCompileStatus{};
              sessionId = current->metadata.sessionId; childId = found->second.second; }
            auto result = customExchange<AudioMiniModuleCompileStatus>(process, sessionId, Method::pollMiniModuleCompile, childId);
            { std::lock_guard lock(current->mutex); IdentityValues{*current}(result); }
            return result;
        });
    }
    void forget(std::uint64_t token) override {
        const auto current = state.lock(); if (!current) return;
        broker->sync([&](AudioRuntimeProcess& process) {
            std::uint64_t sessionId, childId;
            { std::lock_guard lock(current->mutex);
              const auto found = current->compileJobs.find(token);
              if (found == current->compileJobs.end() || found->second.first != current->metadata.generation) return;
              sessionId = current->metadata.sessionId; childId = found->second.second; }
            (void)customExchange<std::tuple<>>(process, sessionId, Method::forgetMiniModuleCompile, childId);
            std::lock_guard lock(current->mutex); current->compileJobs.erase(token);
        });
    }
private:
    std::shared_ptr<EndpointBroker> broker;
    std::weak_ptr<EndpointSession> state;
};
}
std::weak_ptr<AudioMiniModuleCompiler> AudioRuntimeEndpoint::miniModuleCompiler() const {
    if (m->native) return m->native->miniModuleCompiler();
    std::lock_guard lock(m->session->mutex);
    if (!m->compiler) m->compiler = std::make_shared<EndpointCompiler>(m->broker, m->session);
    return m->compiler;
}
// END SESSION OPERATIONS

// BEGIN GENERATED VALUE DEFINITIONS
void AudioRuntimeEndpoint::resetMeterHold(const std::string& channelId) {
    m->call<Method::resetMeterHold>(channelId);
}
void AudioRuntimeEndpoint::setProfiling(bool enabled) {
    m->call<Method::setProfiling>(enabled);
}
engine::LoudnessLevels AudioRuntimeEndpoint::masterLoudness() const {
    return m->call<Method::masterLoudness, true>();
}
void AudioRuntimeEndpoint::resetMasterLoudness() {
    m->call<Method::resetMasterLoudness>();
}
engine::RealtimeEngine::MasterSpectrum AudioRuntimeEndpoint::masterSpectrum() const {
    return m->call<Method::masterSpectrum, true>();
}
void AudioRuntimeEndpoint::setMasterSpectrumConsumer(bool add) {
    m->call<Method::setMasterSpectrumConsumer>(add);
}
std::vector<audio::DeviceInfo> AudioRuntimeEndpoint::enumerateDevices(bool input) {
    return m->call<Method::enumerateDevices>(input);
}
audio::DeviceInfo AudioRuntimeEndpoint::currentDevice(bool input) const {
    return m->call<Method::currentDevice>(input);
}
bool AudioRuntimeEndpoint::matchesDeviceConfiguration(const audio::AudioDeviceConfig& config) const {
    return m->call<Method::matchesDeviceConfiguration>(config);
}
bool AudioRuntimeEndpoint::deviceNeedsRecovery() const {
    if (!m->native) {
        std::lock_guard lock(m->session->mutex);
        if (m->session->metadata.prepared && m->session->metadata.generation && !m->session->metadata.connected) return true;
    }
    return m->call<Method::deviceNeedsRecovery, true>();
}
audio::Result AudioRuntimeEndpoint::refreshDevices() {
    return m->call<Method::refreshDevices>();
}
audio::Result AudioRuntimeEndpoint::probeDevice(const std::string& id, bool input, audio::DeviceInfo& info) {
    return m->call<Method::probeDevice>(id, input, info);
}
float AudioRuntimeEndpoint::inputPeak(std::uint32_t channel) const {
    return m->call<Method::inputPeak, true>(channel);
}
void AudioRuntimeEndpoint::setMetronomeSample(std::shared_ptr<const engine::SampleBuffer> sample) {
    m->call<Method::setMetronomeSample>(sample);
}
void AudioRuntimeEndpoint::requestCountIn(int beats) {
    m->call<Method::requestCountIn>(beats);
}
bool AudioRuntimeEndpoint::startPreview(std::shared_ptr<const engine::SampleBuffer> sample, bool loop, double pitchSemitones) {
    return m->call<Method::startPreview>(sample, loop, pitchSemitones);
}
AudioPreviewSnapshot AudioRuntimeEndpoint::previewSnapshot() const {
    return m->call<Method::previewSnapshot, true>();
}
audio::Result AudioRuntimeEndpoint::startCapture(const AudioCaptureSpec& spec, AudioCaptureStarted& out) {
    if (m->native) return m->native->startCapture(spec, out);
    out = {};
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        const auto token = nextInstanceToken.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t session;
        {
            std::lock_guard lock(m->session->mutex);
            if (!token || m->session->captures.size() >= 4096) return unsupported("Audio capture capacity exceeded.");
            EndpointSession::Capture value;
            value.generation = m->session->metadata.generation;
            value.expected.sampleRate = m->session->metadata.sampleRate;
            value.expected.channelCount = spec.channelCount;
            value.expected.startSample = spec.startSample;
            value.expected.state = audio::RecordingSession::State::Recording;
            session = m->session->metadata.sessionId;
            m->session->captures.emplace(token, std::move(value));
        }
        try {
            auto [status, reply] = audio_rpc::call<Method::startCapture>(process, session, {spec, {}});
            if (status) status = std::get<0>(reply).result();
            if (!status) { std::lock_guard lock(m->session->mutex); m->session->captures.erase(token); return status; }
            auto started = std::move(std::get<0>(std::get<1>(reply)));
            std::lock_guard lock(m->session->mutex);
            auto& value = m->session->captures.at(token);
            value.nativeId = started.id;
            value.expected.filePath = started.path;
            value.status = {true, true, 0, spec.startSample, started.peakBucketFrames, value.expected.sampleRate};
            started.id = token; out = std::move(started);
            return audio::Result::ok();
        } catch (...) {
            process.close();
            std::lock_guard lock(m->session->mutex); m->session->captures.erase(token); throw;
        }
    }); });
}
bool AudioRuntimeEndpoint::publishCaptures(std::span<const AudioCaptureId> ids) {
    if (m->native) return m->native->publishCaptures(ids);
    return m->broker->sync([&](AudioRuntimeProcess& process) {
        if (!process.running()) return ids.empty();
        std::vector<AudioCaptureId> native; native.reserve(ids.size());
        std::uint64_t session;
        {
            std::lock_guard lock(m->session->mutex);
            session = m->session->metadata.sessionId;
            for (const auto id : ids) {
                const auto found = m->session->captures.find(id);
                if (found == m->session->captures.end() || found->second.generation != m->session->metadata.generation) return false;
                native.push_back(found->second.nativeId);
            }
        }
        return std::get<0>(exchange<Method::publishCaptures>(process, session, {std::move(native)}));
    });
}
bool AudioRuntimeEndpoint::hasActiveCaptures() const {
    if (m->native) return m->native->hasActiveCaptures();
    std::lock_guard lock(m->session->mutex); return !m->session->captures.empty();
}
AudioCaptureStatus AudioRuntimeEndpoint::captureStatus(AudioCaptureId id) const {
    if (m->native) return m->native->captureStatus(id);
    AudioCaptureId native;
    {
        std::lock_guard lock(m->session->mutex);
        const auto found = m->session->captures.find(id);
        if (found == m->session->captures.end()) return {};
        if (!m->session->metadata.connected) { auto status = found->second.status; status.recording = false; return status; }
        native = found->second.nativeId;
    }
    auto status = m->call<Method::captureStatus, true>(native);
    std::lock_guard lock(m->session->mutex);
    const auto found = m->session->captures.find(id);
    if (found == m->session->captures.end()) return {};
    if (status.available) found->second.status = status;
    return found->second.status;
}
AudioCapturePeaks AudioRuntimeEndpoint::capturePeaks(AudioCaptureId id, std::uint64_t fromBucket) const {
    if (m->native) return m->native->capturePeaks(id, fromBucket);
    AudioCaptureId native;
    {
        std::lock_guard lock(m->session->mutex);
        const auto found = m->session->captures.find(id);
        if (found == m->session->captures.end() || !m->session->metadata.connected) return {};
        native = found->second.nativeId;
    }
    auto peaks = m->call<Method::capturePeaks, true>(native, fromBucket);
    if (peaks.status.available) {
        std::lock_guard lock(m->session->mutex);
        const auto found = m->session->captures.find(id);
        if (found != m->session->captures.end()) found->second.status = peaks.status;
    }
    return peaks;
}
bool AudioRuntimeEndpoint::setCaptureInput(AudioCaptureId id, std::uint32_t first, std::uint32_t count, bool enabled) {
    if (m->native) return m->native->setCaptureInput(id, first, count, enabled);
    AudioCaptureId native;
    { std::lock_guard lock(m->session->mutex);
      const auto found = m->session->captures.find(id);
      if (found == m->session->captures.end() || !m->session->metadata.connected) return false;
      native = found->second.nativeId; }
    return m->call<Method::setCaptureInput>(native, first, count, enabled);
}
audio::Result AudioRuntimeEndpoint::stopCapture(AudioCaptureId id, audio::RecordingSession& closed) {
    if (m->native) return m->native->stopCapture(id, closed);
    closed = {};
    return guarded([&] { return m->broker->sync([&](AudioRuntimeProcess& process) {
        EndpointSession::Capture capture;
        std::uint64_t session;
        { std::lock_guard lock(m->session->mutex);
          const auto found = m->session->captures.find(id);
          if (found == m->session->captures.end()) return audio::Result::fail(audio::EngineError::InvalidArgument, "Unknown audio capture.");
          capture = found->second; session = m->session->metadata.sessionId; }
        if (process.running()) {
            auto [status, reply] = audio_rpc::call<Method::stopCapture>(process, session, {capture.nativeId, {}});
            if (status) {
                closed = std::move(std::get<0>(std::get<1>(reply)));
                if (std::get<0>(reply).result() && closed.fileWriteSucceeded &&
                    closed.state == audio::RecordingSession::State::Stopped) {
                    std::lock_guard lock(m->session->mutex); m->session->captures.erase(id);
                }
                return std::get<0>(reply).result();
            }
            if (process.running()) return status;
        }
        // running() has reaped/stopped the writer. Only now may the UI repair
        // the exact acknowledged file; no RPC timeout can race this write.
        capture.expected.startSample = capture.status.startSample;
        capture.expected.capturedFrames = capture.status.recordedFrames;
        const auto result = audio::AudioRecorder::recoverInterruptedFile(capture.expected, closed);
        if (result) { std::lock_guard lock(m->session->mutex); m->session->captures.erase(id); }
        return result;
    }); });
}
void AudioRuntimeEndpoint::interruptCapture(AudioCaptureId id) {
    if (m->native) { m->native->interruptCapture(id); return; }
    m->broker->sync([&](AudioRuntimeProcess& process) {
        if (!process.running()) return;
        AudioCaptureId native; std::uint64_t session;
        { std::lock_guard lock(m->session->mutex);
          const auto found = m->session->captures.find(id);
          if (found == m->session->captures.end()) return;
          native = found->second.nativeId; session = m->session->metadata.sessionId; }
        auto [result, ignored] = audio_rpc::call<Method::interruptCapture>(process, session, {native});
        if (!result && process.running()) require(result);
    });
}
void AudioRuntimeEndpoint::suspendRecordingClipFx(std::span<const std::string> recordingTracks) {
    m->call<Method::suspendRecordingClipFx>(recordingTracks);
}
bool AudioRuntimeEndpoint::hasPlugin(const AudioPluginAddress& address, std::string_view uid) const {
    return m->call<Method::hasPlugin>(address, uid);
}
std::vector<AudioPluginAddress> AudioRuntimeEndpoint::pluginAddresses() const {
    if (!m->native && !metadata().connected) {
        std::lock_guard lock(m->session->mutex);
        auto addresses = m->session->publication ? m->session->publication->plugins : std::vector<AudioPluginAddress>{};
        IdentityValues{*m->session}(addresses);
        return addresses;
    }
    return m->call<Method::pluginAddresses>();
}
AudioPluginStateSnapshot AudioRuntimeEndpoint::pluginStateSnapshot(const AudioPluginAddress& address, bool includeState,
    const std::optional<std::string>& packagedSample, AudioPluginSnapshotPurpose purpose) {
    return m->call<Method::pluginStateSnapshot>(address, includeState, packagedSample, purpose);
}
std::vector<AudioPluginStateSnapshot> AudioRuntimeEndpoint::pluginStateSnapshots(std::span<const AudioPluginStateRequest> requests) {
    return m->call<Method::pluginStateSnapshots>(requests);
}
std::uint64_t AudioRuntimeEndpoint::pluginInstanceId(const AudioPluginAddress& address) const {
    return m->call<Method::pluginInstanceId>(address);
}
std::vector<plugins::ParameterInfo> AudioRuntimeEndpoint::pluginParameters(const AudioPluginAddress& address) const {
    return m->call<Method::pluginParameters>(address);
}
std::optional<plugins::ParameterInfo> AudioRuntimeEndpoint::pluginParameterInfo(const AudioPluginAddress& address, const std::string& parameterId) const {
    return m->call<Method::pluginParameterInfo>(address, parameterId);
}
AudioPluginCapabilities AudioRuntimeEndpoint::pluginCapabilities(const AudioPluginAddress& address) const {
    return m->call<Method::pluginCapabilities>(address);
}
bool AudioRuntimeEndpoint::setPluginControls(const std::string& channelId, const std::string& slotId, const AudioPluginControlChange& change) {
    return m->call<Method::setPluginControls>(channelId, slotId, change);
}
AudioPluginSlideStatus AudioRuntimeEndpoint::pluginSlideStatus(const AudioPluginAddress& address) const {
    return m->call<Method::pluginSlideStatus, true>(address);
}
bool AudioRuntimeEndpoint::setPluginSlide(const AudioPluginAddress& address, int mode, double range, double reserve) {
    return m->call<Method::setPluginSlide>(address, mode, range, reserve);
}
bool AudioRuntimeEndpoint::setPluginAutomationOverride(const AudioPluginAddress& address, const std::string& parameterId) {
    return m->call<Method::setPluginAutomationOverride>(address, parameterId);
}
double AudioRuntimeEndpoint::pluginParameter(const AudioPluginAddress& address, const std::string& parameterId, Readout readout) const {
    if (readout == Readout::Current) return m->call<Method::pluginParameter>(address, parameterId);
    return m->call<Method::pluginParameter, true>(address, parameterId);
}
bool AudioRuntimeEndpoint::setPluginParameter(const AudioPluginAddress& address, const std::string& parameterId, double value) {
    return m->call<Method::setPluginParameter>(address, parameterId, value);
}
void AudioRuntimeEndpoint::readPluginParameters(const AudioPluginAddress& address, std::span<PluginParameterReadout> values) const {
    m->call<Method::readPluginParameters, true>(address, values);
}
std::string AudioRuntimeEndpoint::pluginParameterText(const AudioPluginAddress& address, const std::string& parameterId, double value, std::int32_t indexHint) const {
    return m->call<Method::pluginParameterText, true>(address, parameterId, value, indexHint);
}
EffectMeterSnapshot AudioRuntimeEndpoint::effectMeterSnapshot(const AudioPluginAddress& address) {
    return m->call<Method::effectMeterSnapshot, true>(address);
}
SamplerSnapshot AudioRuntimeEndpoint::samplerSnapshot(const AudioPluginAddress& address, Readout readout) const {
    if (readout == Readout::Current) return m->call<Method::samplerSnapshot>(address);
    return m->call<Method::samplerSnapshot, true>(address);
}
std::optional<SlicerSnapshot> AudioRuntimeEndpoint::slicerSnapshot(const AudioPluginAddress& address, bool includeActivity, Readout readout) const {
    if (readout == Readout::Current) return m->call<Method::slicerSnapshot>(address, includeActivity);
    return m->call<Method::slicerSnapshot, true>(address, includeActivity);
}
std::optional<EqualizerSnapshot> AudioRuntimeEndpoint::equalizerSnapshot(const AudioPluginAddress& address, bool consumeMeters, Readout readout) {
    if (readout == Readout::Current) return m->call<Method::equalizerSnapshot>(address, consumeMeters);
    return m->call<Method::equalizerSnapshot, true>(address, consumeMeters);
}
std::optional<EqualizerResponse> AudioRuntimeEndpoint::equalizerResponse(const AudioPluginAddress& address) const {
    return m->call<Method::equalizerResponse, true>(address);
}
std::optional<std::array<double, 180>> AudioRuntimeEndpoint::modulationResponse(const AudioPluginAddress& address) const {
    return m->call<Method::modulationResponse, true>(address);
}
std::optional<GravitySnapshot> AudioRuntimeEndpoint::gravitySnapshot(const AudioPluginAddress& address) {
    return m->call<Method::gravitySnapshot, true>(address);
}
bool AudioRuntimeEndpoint::setInsertPresetReference(const AudioPluginAddress& address, std::string kind, std::string name) {
    return m->call<Method::setInsertPresetReference>(address, kind, name);
}
bool AudioRuntimeEndpoint::setEqualizerAnalyzer(const AudioPluginAddress& address, const plugins::equalizer::AnalyzerConfig& config) {
    return m->call<Method::setEqualizerAnalyzer>(address, config);
}
bool AudioRuntimeEndpoint::auditionEqualizerBand(const AudioPluginAddress& address, int band) {
    return m->call<Method::auditionEqualizerBand>(address, band);
}
std::optional<std::array<double, plugins::equalizer::kParameterCount>> AudioRuntimeEndpoint::captureEqualizerComparison(const AudioPluginAddress& address, char slot) {
    return m->call<Method::captureEqualizerComparison>(address, slot);
}
bool AudioRuntimeEndpoint::activateEqualizerComparison(const AudioPluginAddress& address, char slot) {
    return m->call<Method::activateEqualizerComparison>(address, slot);
}
bool AudioRuntimeEndpoint::copyEqualizerComparison(const AudioPluginAddress& address) {
    return m->call<Method::copyEqualizerComparison>(address);
}
bool AudioRuntimeEndpoint::setGravityFrozen(const AudioPluginAddress& address, bool frozen) {
    return m->call<Method::setGravityFrozen>(address, frozen);
}
bool AudioRuntimeEndpoint::clearGravityTail(const AudioPluginAddress& address) {
    return m->call<Method::clearGravityTail>(address);
}
std::optional<PluginEditorSnapshot> AudioRuntimeEndpoint::pluginEditorSnapshot(const AudioPluginAddress& address, Readout readout) const {
    return readout == Readout::Current ? m->call<Method::pluginEditorSnapshot>(address)
        : m->call<Method::pluginEditorSnapshot, true>(address);
}
bool AudioRuntimeEndpoint::closePluginEditor(const AudioPluginAddress& address, bool onlyUnattached) {
    return m->call<Method::closePluginEditor>(address, onlyUnattached);
}
std::optional<PluginEditorSize> AudioRuntimeEndpoint::pluginEditorSize(const AudioPluginAddress& address) const {
    return m->call<Method::pluginEditorSize, true>(address);
}
std::optional<PluginEditorSize> AudioRuntimeEndpoint::resizePluginEditor(const AudioPluginAddress& address, PluginEditorSize requested) {
    return m->call<Method::resizePluginEditor>(address, requested);
}
bool AudioRuntimeEndpoint::pumpPluginEditor(const AudioPluginAddress& address) {
    return m->call<Method::pumpPluginEditor>(address);
}
std::uint32_t AudioRuntimeEndpoint::pollPluginEditorShortcuts(const AudioPluginAddress& address, bool enabled) {
    return m->call<Method::pollPluginEditorShortcuts>(address, enabled);
}
audio::Result AudioRuntimeEndpoint::capturePluginCheckpoints(std::vector<AudioPluginCheckpoint>& out, AudioPluginCheckpointPurpose purpose) {
    return m->call<Method::capturePluginCheckpoints>(out, purpose);
}
std::vector<InsertParameter> AudioRuntimeEndpoint::pluginParameterValues(const AudioPluginAddress& address) const {
    return m->call<Method::pluginParameterValues>(address);
}
bool AudioRuntimeEndpoint::loadInstrumentSample(const AudioPluginAddress& address, const std::string& path, std::shared_ptr<const engine::SampleBuffer> decoded) {
    return m->call<Method::loadInstrumentSample>(address, path, decoded);
}
bool AudioRuntimeEndpoint::clearInstrumentSample(const AudioPluginAddress& address) {
    return m->call<Method::clearInstrumentSample>(address);
}
bool AudioRuntimeEndpoint::restoreSlicerState(const AudioPluginAddress& address, const plugins::slicer::ControlState& state) {
    return m->call<Method::restoreSlicerState>(address, state);
}
bool AudioRuntimeEndpoint::setSlicerSlices(const AudioPluginAddress& address, std::shared_ptr<const plugins::slicer::SliceTable> table, const plugins::slicer::AnalysisSettings& settings) {
    return m->call<Method::setSlicerSlices>(address, table, settings);
}
bool AudioRuntimeEndpoint::setSlicerAnalysis(const AudioPluginAddress& address, const plugins::slicer::AnalysisSettings& settings) {
    return m->call<Method::setSlicerAnalysis>(address, settings);
}
bool AudioRuntimeEndpoint::flushSamplerPrecompute(bool wait) {
    return m->call<Method::flushSamplerPrecompute>(wait);
}
AudioPluginRuntimeStatus AudioRuntimeEndpoint::pluginRuntimeStatus(const std::string& channelId, const std::string& slotId, Readout readout) const {
    return readout == Readout::Current ? m->call<Method::pluginRuntimeStatus>(channelId, slotId)
        : m->call<Method::pluginRuntimeStatus, true>(channelId, slotId);
}
bool AudioRuntimeEndpoint::restartPlugin(const std::string& channelId, const std::string& slotId) {
    return m->call<Method::restartPlugin>(channelId, slotId);
}
bool AudioRuntimeEndpoint::advancePluginEdits() {
    return m->call<Method::advancePluginEdits>();
}
bool AudioRuntimeEndpoint::configureChannelColor(const AudioPluginAddress& address, std::uint64_t seed, std::span<const InsertParameter> parameters, bool bypassed) {
    return m->call<Method::configureChannelColor>(address, seed, parameters, bypassed);
}
bool AudioRuntimeEndpoint::stageMiniModulePreparation(std::uint64_t id) {
    return m->call<Method::stageMiniModulePreparation>(id);
}
void AudioRuntimeEndpoint::clearMiniModulePreparation() {
    m->call<Method::clearMiniModulePreparation>();
}
void AudioRuntimeEndpoint::fadeMiniModulePreparation(std::uint64_t id, bool cancel) {
    m->call<Method::fadeMiniModulePreparation>(id, cancel);
}
bool AudioRuntimeEndpoint::miniModulePreparationFaded(std::uint64_t id) const {
    return m->call<Method::miniModulePreparationFaded, true>(id);
}
// END GENERATED VALUE DEFINITIONS

} // namespace daw
#pragma pop_macro("slots")
