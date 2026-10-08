#pragma once

#include "AudioRuntimeCalls.hpp"

#include <algorithm>
#include <any>

namespace daw::audio_rpc {

inline constexpr std::size_t kMaxReadoutBatchItems = 256;
inline constexpr std::size_t kMaxReadoutBatchBytes = 8 * 1024 * 1024;

constexpr bool isReadoutMethod(Method method) {
    switch (method) {
#define DAW_AUDIO_READOUT(name) case Method::name: return true;
#include "AudioRuntimeReadoutMethods.def"
#undef DAW_AUDIO_READOUT
    default: return false;
    }
}

namespace readout_detail {
// std::any owns local typed values, never wire bytes or native addresses. The
// serializer uses Binding<Id>::Inputs/Reply for every whitelisted method.
struct Item { Method method; std::any value; };
template<bool Reply> struct Packet { std::vector<Item> items; };
}

/// Reusable subscriptions for one session, submitted by the serialized broker
/// on its control thread. No audio-thread or UI-thread callback is introduced.
/// All replies validate before any sink runs. Sinks must not throw or re-enter
/// this batch; publication into UI caches belongs to the broker.
class ReadoutBatch final {
public:
    template<Method Id, class Sink>
    void add(typename Binding<Id>::Inputs inputs, Sink sink) {
        static_assert(isReadoutMethod(Id), "Only explicitly whitelisted observations can be batched.");
        if (m_requests.items.size() == kMaxReadoutBatchItems)
            throw std::length_error("Audio readout batch exceeds item limit.");
        using Reply = typename Binding<Id>::Reply;
        std::function<void(std::any&&)> publish = [sink = std::move(sink)](std::any&& value) mutable {
            std::invoke(sink, std::move(std::any_cast<Reply&>(value)));
        };
        // Allocate both owners before making the new subscription visible.
        if (m_sinks.size() == m_sinks.capacity())
            m_sinks.reserve(std::min(kMaxReadoutBatchItems, std::max(std::size_t{8}, m_sinks.capacity() * 2)));
        m_requests.items.push_back({Id, std::move(inputs)});
        m_sinks.push_back(std::move(publish));
    }
    audio::Result execute(AudioRuntimeProcess& process, std::uint64_t sessionId = 0);
    void clear() { m_requests.items.clear(); m_sinks.clear(); }
    std::size_t size() const { return m_requests.items.size(); }
    bool empty() const { return m_requests.items.empty(); }
private:
    readout_detail::Packet<false> m_requests;
    std::vector<std::function<void(std::any&&)>> m_sinks;
};

/// Child control thread only. Decodes the complete request before the first
/// read, so a malformed later item cannot consume earlier telemetry.
std::vector<std::uint8_t> dispatchReadoutBatch(AudioRuntime& runtime,
    std::span<const std::uint8_t> payload, const std::filesystem::path& inputDirectory,
    ProcessAudioResources::Cache& inputCache, ProcessAudioResources& outputResources);

} // namespace daw::audio_rpc
