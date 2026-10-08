#pragma once

#include "AudioRuntimeRpc.hpp"
#include "AudioRuntimePluginFields.hpp"
#include "AudioRuntimeProcess.hpp"

namespace daw::audio_rpc {

/// A typed control-thread call. Output references/spans become owned reply
/// values; the caller publishes them only after the complete reply validates.
template<Method Id>
auto call(AudioRuntimeProcess& process, std::uint64_t sessionId, typename Binding<Id>::Inputs values) {
    using Reply = typename Binding<Id>::Reply;
    Reply reply{};
    auto status = process.invoke(std::uint32_t(Id),
        [&](ProcessAudioResources& resources) { return audio_value::encodeResources(resources, values); },
        [&](std::span<const std::uint8_t> bytes, const std::filesystem::path& directory,
            ProcessAudioResources::Cache& cache) {
            auto [decoded] = audio_value::decodeResources<Reply>(bytes, directory, &cache);
            reply = std::move(decoded);
        }, sessionId);
    return std::pair{std::move(status), std::move(reply)};
}

} // namespace daw::audio_rpc
