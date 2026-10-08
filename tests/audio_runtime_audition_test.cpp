#include "AudioRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace daw;
constexpr std::uint32_t frames = 64;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
AudioSessionSpec session(float amplitude, bool bus = false) {
    AudioSessionSpec value;
    AudioGraphSpec::Channel channel; channel.id = "source";
    if (bus) channel.outputBusId = "bus";
    value.graph.channels.push_back(channel);
    if (bus) { channel.id = "bus"; channel.outputBusId.clear(); channel.volume = .3f; value.graph.channels.push_back(channel); }
    auto audio = std::make_shared<engine::SampleBuffer>(2, 16384, 48000);
    for (unsigned ch = 0; ch < 2; ++ch) std::fill_n(audio->writableChannel(ch), audio->frames(), amplitude);
    engine::ClipPlacement placement;
    placement.audio = audio; placement.clipId = "clip"; placement.lengthSamples = audio->frames();
    auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>(); clips->push_back(placement);
    AudioSessionSpec::Channel content; content.id = "source";
    content.content.clips = AudioContentSpec::Clips{clips, {}};
    value.channels.push_back(std::move(content));
    return value;
}
std::shared_ptr<AudioRuntime> runtime(float amplitude) {
    auto result = std::make_shared<AudioRuntime>();
    require(bool(result->prepare(48000, frames)), "prepare audition runtime");
    result->buildSession(session(amplitude));
    require(bool(result->commitGraph()), "commit audition runtime");
    result->transportCommand({.action = AudioTransportCommand::Action::Duration, .position = 16384});
    return result;
}
float render(AudioRuntime& runtime) {
    audio::AudioBuffer output(2, frames);
    audio::AudioCallbackContext block;
    block.outputBuffer = &output; block.numFrames = frames; block.sampleRate = 48000;
    for (unsigned i = 0; i < 16; ++i) {
        runtime.processDeviceBlock(block);
        require(block.renderStatus == audio::AudioCallbackContext::RenderStatus::Complete, "audition callback completes");
    }
    return output.getChannel(0)[frames - 1];
}
void contract() {
    auto primary = runtime(.5f), secondary = runtime(.2f);
    const auto defaultSecondaryWorkers = secondary->diagnostics().workers;
    primary->configureWorkersForTest(false, 1);
    const auto original = primary->routingGraph();
    primary->transportCommand({AudioTransportCommand::Action::StartPlayback});
    require(std::abs(render(*primary) - .5f) < 1e-5f, "initial primary signal");
    require(bool(primary->startAudition(secondary)) && primary->routingGraph() == original,
        "audition preserves the primary publication identity");
    require(secondary->diagnostics().workers == 1, "secondary inherits the primary worker limit before audition");
    primary->configureWorkersForTest(false, 2);
    require(secondary->diagnostics().workers == primary->diagnostics().workers,
        "active secondary follows device-control worker policy changes");
    require(std::abs(render(*primary) - .2f) < 1e-5f, "same callback drives the runtime-owned secondary");
    require(!secondary->startAudition(primary), "nested audition cannot create a runtime ownership cycle");

    require(bool(primary->applySession(session(.5f, true))), "transaction commits a new primary route during audition");
    const auto edited = primary->routingGraph();
    require(edited && edited != original, "primary graph publication advances independently of output selection");
    require(std::abs(render(*primary) - .2f) < 1e-5f, "primary transaction cannot steal audition output");
    primary->stopAudition();
    require(secondary->diagnostics().workers == defaultSecondaryWorkers,
        "detached secondary releases device worker policy after its final block drains");
    require(bool(primary->startAudition(secondary)), "secondary can rejoin audition with current worker policy");
    std::weak_ptr<AudioRuntime> lifetime = secondary;
    secondary.reset();
    require(!lifetime.expired(), "output selection owns the secondary until its final block completes");
    primary->stopAudition();
    require(lifetime.expired(), "stop drains graph readers and releases the complete secondary runtime");
    require(primary->routingGraph() == edited, "stop retains the latest primary publication rather than the original route");
    primary->transportCommand({AudioTransportCommand::Action::StartPlayback});
    require(std::abs(render(*primary) - .15f) < 1e-5f, "return to primary renders the newly committed bus topology");
}
}
int main() {
    try { contract(); std::puts("PASS runtime audition / primary edits / publication / lifetime"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
