#include "AudioRuntime.hpp"

#include <algorithm>

namespace daw {

class AudioRuntime::AuditionNode final : public engine::Node {
public:
    explicit AuditionNode(std::shared_ptr<AudioRuntime> runtime) : owner(std::move(runtime)) {}
    std::string_view name() const noexcept override { return "Runtime audition"; }
    bool isSource() const noexcept override { return true; }
    engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::None; }
    void process(const engine::ProcessContext& context) override {
        valid = !context.offline && context.sampleRate == owner->sampleRate && context.frames <= owner->bufferSize;
        if (!valid) {
            for (engine::ChannelCount ch = 0; ch < context.output.numChannels(); ++ch)
                std::fill(context.output.channel(ch).begin(), context.output.channel(ch).end(), 0.f);
            return;
        }
        owner->engine.renderBlock(context.output, nullptr, 0, context.frames);
    }
    engine::Status processStatus() const noexcept override {
        if (!valid || owner->engine.lastBlockResult() == engine::RealtimeEngine::BlockResult::Failed)
            return engine::fail(engine::EngineError::ProcessingFailed);
        return {};
    }
private:
    std::shared_ptr<AudioRuntime> owner;
    bool valid = true;
};

audio::Result AudioRuntime::startAudition(std::shared_ptr<AudioRuntime> secondary) {
    if (!secondary || secondary.get() == this || auditionRuntime || auditionDriven ||
        secondary->auditionRuntime || secondary->auditionDriven || secondary->deviceSnapshot().hasStream ||
        !prepared || !secondary->prepared || preparation() != secondary->preparation() ||
        preparation().offline || hasActiveCaptures() || secondary->hasActiveCaptures() ||
        transportSnapshot().recording || secondary->transportSnapshot().recording)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Audio audition session is unavailable.");
    engine::AudioGraph output;
    output.setSink(output.adoptNode(std::make_shared<AuditionNode>(secondary)));
    try {
        // Worker registration and source pre-roll can wait on OS services.
        // Complete them before gating the primary device's output.
        inheritAudioWorkers(*secondary);
        secondary->transportCommand({AudioTransportCommand::Action::StartPlayback});
        auto result = audio::Result::ok();
        {
            const engine::RealtimeEngine::RenderGate gate(engine);
            const auto published = engine.setOutputGraph(std::move(output));
            if (!published) result = audio::Result::fail(audio::EngineError::InvalidArgument,
                std::string(engine::describe(published.error())));
            else {
                transportCommand({AudioTransportCommand::Action::Pause});
                previewCommand({AudioPreviewCommand::Action::Stop});
                secondary->auditionDriven = true;
                auditionRuntime = secondary;
            }
        }
        if (!result) {
            secondary->transportCommand({AudioTransportCommand::Action::Pause});
            secondary->configureAudioWorkers({});
        }
        return result;
    } catch (const std::exception& error) {
        secondary->transportCommand({AudioTransportCommand::Action::Pause});
        secondary->configureAudioWorkers({});
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}

void AudioRuntime::stopAudition() {
    auto retired = std::move(auditionRuntime);
    if (!retired) return;
    {
        const engine::RealtimeEngine::RenderGate gate(engine);
        engine.clearOutputGraph();
        retired->transportCommand({AudioTransportCommand::Action::Pause});
        retired->auditionDriven = false;
    }
    // Keep a strong owner until the reader drains, then release workgroup
    // membership and native DSP while primary audio can run again.
    retired->configureAudioWorkers({});
}

} // namespace daw
