#include "AudioRuntimeRpc.hpp"
#include "AudioRuntimeReadoutBatch.hpp"
#include "AudioRuntimeCoreFields.hpp"
#include "AudioRuntimePluginFields.hpp"
#include "AudioMiniModuleCompiler.hpp"

namespace daw::audio_rpc {
namespace {
template<Method Id> std::vector<std::uint8_t> invoke(AudioRuntime& runtime,
    std::span<const std::uint8_t> payload, const std::filesystem::path& directory,
    ProcessAudioResources::Cache& cache, ProcessAudioResources& resources) {
    using Call = Binding<Id>;
    auto [arguments] = audio_value::decodeResources<typename Call::Inputs>(payload, directory, &cache);
    const auto result = Call::template invoke<Call::function>(runtime, arguments);
    return audio_value::encodeResources(resources, result);
}
}

std::vector<std::uint8_t> dispatch(AudioRuntime& runtime, Method method,
    std::span<const std::uint8_t> payload, const std::filesystem::path& inputDirectory,
    ProcessAudioResources::Cache& inputCache, ProcessAudioResources& outputResources) {
    switch (method) {
    case Method::readoutBatch:
        return dispatchReadoutBatch(runtime, payload, inputDirectory, inputCache, outputResources);
#define DAW_AUDIO_METHOD(id, name) case Method::name: \
    return invoke<Method::name>(runtime, payload, inputDirectory, inputCache, outputResources);
#include "AudioRuntimeMethods.def"
#undef DAW_AUDIO_METHOD
    case Method::openPluginEditor: {
        const auto [address] = audio_value::decodeResources<AudioPluginAddress>(payload, inputDirectory, &inputCache);
        const auto state = runtime.pluginEditorSnapshot(address);
        const bool opened = state && state->remote && runtime.openPluginEditor(address, nullptr, nullptr);
        return audio_value::encodeResources(outputResources, opened);
    }
    case Method::showDeviceControlPanel: {
        const auto [id] = audio_value::decodeResources<std::string>(payload, inputDirectory, &inputCache);
        return audio_value::encodeResources(outputResources, AudioResultValue(runtime.showDeviceControlPanel(id)));
    }
    case Method::startMiniModuleCompile: {
        auto [request] = audio_value::decodeResources<AudioMiniModuleCompileRequest>(payload, inputDirectory, &inputCache);
        const auto compiler = runtime.miniModuleCompiler().lock();
        if (!compiler) throw std::runtime_error("Audio compilation service is unavailable.");
        return audio_value::encodeResources(outputResources, compiler->start(std::move(request)));
    }
    case Method::pollMiniModuleCompile:
    case Method::forgetMiniModuleCompile: {
        const auto [id] = audio_value::decodeResources<std::uint64_t>(payload, inputDirectory, &inputCache);
        const auto compiler = runtime.miniModuleCompiler().lock();
        if (!compiler) throw std::runtime_error("Audio compilation service is unavailable.");
        if (method == Method::pollMiniModuleCompile)
            return audio_value::encodeResources(outputResources, compiler->poll(id));
        compiler->forget(id);
        return audio_value::encodeResources(outputResources, std::tuple<>{});
    }
    }
    throw std::invalid_argument("Unknown audio runtime operation.");
}
} // namespace daw::audio_rpc
