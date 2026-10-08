#include "AudioRuntimeReadoutBatch.hpp"

namespace daw::audio_rpc {
namespace readout_detail {

template<class A, Method Id, bool Reply>
void itemValue(A& archive, Item& item) {
    using Call = Binding<Id>;
    using Value = std::conditional_t<Reply, typename Call::Reply, typename Call::Inputs>;
    if constexpr (A::reading) {
        // The shared Reader charges all nested allocations across all items.
        // Account for std::any's owner as well, before allocating it.
        archive.chargeAllocation(1, sizeof(Value));
        Value value{};
        archive(value);
        item.value = std::move(value);
    } else {
        archive(std::any_cast<Value&>(item.value));
    }
}

template<class A, bool Reply>
void fields(A& archive, Packet<Reply>& packet) {
    std::size_t count = packet.items.size();
    if constexpr (A::reading) {
        count = archive.count();
        if (count > kMaxReadoutBatchItems) audio_value::invalid("Audio readout batch exceeds item limit.");
        archive.chargeAllocation(count, sizeof(Item));
        packet.items.resize(count);
    } else {
        if (count > kMaxReadoutBatchItems) audio_value::invalid("Audio readout batch exceeds item limit.");
        archive.count(count);
    }
    for (auto& item : packet.items) {
        archive(item.method);
        switch (item.method) {
#define DAW_AUDIO_READOUT(name) case Method::name: itemValue<A, Method::name, Reply>(archive, item); break;
#include "AudioRuntimeReadoutMethods.def"
#undef DAW_AUDIO_READOUT
        default: audio_value::invalid("Operation is not an audio readout.");
        }
        if constexpr (!A::reading)
            if (archive.bytes.size() > kMaxReadoutBatchBytes)
                audio_value::invalid("Audio readout batch exceeds byte limit.");
    }
}

template<Method Id> void observe(audio_value::Writer& archive, AudioRuntime& runtime, Item& input) {
    using Call = Binding<Id>;
    auto& arguments = std::any_cast<typename Call::Inputs&>(input.value);
    const auto reply = Call::template invoke<Call::function>(runtime, arguments);
    archive(reply);
}

struct Observations { AudioRuntime& runtime; Packet<false>& request; };
void fields(audio_value::Writer& archive, Observations& observations) {
    archive.count(observations.request.items.size());
    for (auto& item : observations.request.items) {
        archive(item.method);
        switch (item.method) {
#define DAW_AUDIO_READOUT(name) case Method::name: observe<Method::name>(archive, observations.runtime, item); break;
#include "AudioRuntimeReadoutMethods.def"
#undef DAW_AUDIO_READOUT
        default: audio_value::invalid("Operation is not an audio readout.");
        }
        // Encode and release one native result at a time. A large history
        // query cannot multiply its allocation by the number of subscribers.
        if (archive.bytes.size() > kMaxReadoutBatchBytes)
            audio_value::invalid("Audio readout reply exceeds byte limit.");
    }
}

} // namespace readout_detail

audio::Result ReadoutBatch::execute(AudioRuntimeProcess& process, std::uint64_t sessionId) {
    if (empty()) return audio::Result::ok();
    readout_detail::Packet<true> response;
    const auto result = process.invoke(std::uint32_t(Method::readoutBatch),
        [&](ProcessAudioResources& resources) {
            auto bytes = audio_value::encodeResources(resources, m_requests);
            if (bytes.size() > kMaxReadoutBatchBytes) audio_value::invalid("Audio readout batch exceeds byte limit.");
            return bytes;
        },
        [&](std::span<const std::uint8_t> bytes, const std::filesystem::path& directory,
            ProcessAudioResources::Cache& cache) {
            if (bytes.size() > kMaxReadoutBatchBytes) audio_value::invalid("Audio readout reply exceeds byte limit.");
            auto [decoded] = audio_value::decodeResources<readout_detail::Packet<true>>(bytes, directory, &cache);
            if (decoded.items.size() != m_requests.items.size())
                audio_value::invalid("Audio readout reply count does not match its request.");
            for (std::size_t i = 0; i < decoded.items.size(); ++i)
                if (decoded.items[i].method != m_requests.items[i].method)
                    audio_value::invalid("Audio readout reply order does not match its request.");
            response = std::move(decoded);
        }, sessionId);
    if (!result) return result;
    // Keep user code outside invoke's protocol/error guard. Every value is
    // already owned and validated; callbacks cannot expose a partial decode.
    for (std::size_t i = 0; i < response.items.size(); ++i)
        m_sinks[i](std::move(response.items[i].value));
    return result;
}

std::vector<std::uint8_t> dispatchReadoutBatch(AudioRuntime& runtime,
    std::span<const std::uint8_t> payload, const std::filesystem::path& inputDirectory,
    ProcessAudioResources::Cache& inputCache, ProcessAudioResources& outputResources) {
    if (payload.size() > kMaxReadoutBatchBytes) audio_value::invalid("Audio readout batch exceeds byte limit.");
    auto [request] = audio_value::decodeResources<readout_detail::Packet<false>>(payload, inputDirectory, &inputCache);
    // No runtime call occurs above this line, including for a trailing unknown
    // method, truncated item, invalid enum/number or oversized allocation.
    readout_detail::Observations observations{runtime, request};
    auto bytes = audio_value::encodeResources(outputResources, observations);
    if (bytes.size() > kMaxReadoutBatchBytes) audio_value::invalid("Audio readout reply exceeds byte limit.");
    return bytes;
}

} // namespace daw::audio_rpc
