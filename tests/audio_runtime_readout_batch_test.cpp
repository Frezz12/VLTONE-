#include "AudioRuntimeReadoutBatch.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/SamplerInstance.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
using namespace daw;
using namespace daw::audio_rpc;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
    return value;
}

AudioPluginSpec plugin(const char* id, const plugins::PluginDescriptor& descriptor) {
    AudioPluginSpec result;
    result.id = id; result.uid = descriptor.uid; result.name = descriptor.name;
    result.descriptor = descriptor; result.requiredFormat = descriptor.format;
    return result;
}

void malformedRequestDoesNotConsume(const std::filesystem::path& directory) {
    AudioRuntime runtime;
    if (!check(bool(runtime.prepare(48000, 64)), "prepare local readout protocol fixture")) return;
    AudioSessionSpec session;
    AudioGraphSpec::Channel channel; channel.id = "source"; channel.input = {true, true, 0, 2, 3};
    session.graph.channels.push_back(channel);
    runtime.buildSession(std::move(session));
    if (!check(bool(runtime.commitGraph()), "publish local readout graph")) return;
    audio::AudioBuffer output(2, 64);
    audio::AudioCallbackContext context;
    context.outputBuffer = &output; context.numFrames = 64; context.sampleRate = 48000;
    ProcessAudioResources inputs(directory / "input"), outputs(directory / "output");
    ProcessAudioResources::Cache cache;
    const auto rejectWithoutDrain = [&](std::vector<std::uint8_t> bytes, const char* message) {
        runtime.processDeviceBlock(context);
        bool rejected = false;
        try { (void)dispatchReadoutBatch(runtime, bytes, directory / "input", cache, outputs); }
        catch (const std::exception&) { rejected = true; }
        const auto timing = runtime.timingSnapshot(false, true);
        check(rejected && !timing.events.empty(), message);
    };
    using Timing = Binding<Method::timingSnapshot>::Inputs;
    rejectWithoutDrain(audio_value::encodeResources(inputs, std::uint32_t{2},
        Method::timingSnapshot, Timing{false, true}, Method::setPluginParameter),
        "later mutation is rejected before an earlier timing read can drain samples");
    rejectWithoutDrain(audio_value::encodeResources(inputs, std::uint32_t{2},
        Method::timingSnapshot, Timing{false, true}, Method::pluginParameter,
        AudioPluginAddress{"source", "missing"}),
        "truncated later typed input preserves earlier telemetry");
    auto trailing = audio_value::encodeResources(inputs, std::uint32_t{1}, Method::timingSnapshot, Timing{false, true});
    trailing.push_back(7);
    rejectWithoutDrain(std::move(trailing), "trailing wire data is rejected before any native observation");
    rejectWithoutDrain(audio_value::encodeResources(inputs, std::uint32_t(kMaxReadoutBatchItems + 1)),
        "oversized batch count is rejected before observation");
    std::vector<std::uint8_t> excessive(kMaxReadoutBatchBytes + 1);
    rejectWithoutDrain(std::move(excessive), "oversized batch bytes are rejected before observation");
}

void childBatch(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    AudioSessionPacket packet; packet.generation = packet.revision = 1; packet.blockSize = 64;
    AudioGraphSpec::Channel channel; channel.id = "source"; channel.acceptsMidi = true;
    packet.session.graph.channels.push_back(channel);
    packet.session.pluginChains.push_back({"source", AudioPluginChainSpec::Kind::Inserts, {},
        {plugin("eq", plugins::equalizer::EqualizerInstance::staticDescriptor())}});
    packet.session.pluginChains.push_back({"source", AudioPluginChainSpec::Kind::Instrument, {},
        {plugin("sampler", plugins::sampler::SamplerInstance::staticDescriptor())}});
    if (!check(bool(process.replaceSession(packet)), "typed readout batch uses a real audio child")) return;
    const AudioPluginAddress eq{"source", "eq"}, sampler{"source", "sampler"};
    auto [changed, changeReply] = call<Method::setPluginParameter>(process, 0, {eq, "output.gain", -6.0});
    if (!check(bool(changed) && std::get<0>(changeReply), "set builtin parameter for batch observation")) return;
    auto audio = std::make_shared<engine::SampleBuffer>(2, 512, 48000);
    std::fill_n(audio->writableChannel(0), audio->frames(), .125f);
    std::fill_n(audio->writableChannel(1), audio->frames(), -.25f);
    const auto [loaded, loadReply] = call<Method::loadInstrumentSample>(process, 0, {sampler, "mapped.wav", audio});
    if (!check(bool(loaded) && std::get<0>(loadReply), "load immutable PCM for readout batch")) return;
    (void)call<Method::flushSamplerPrecompute>(process, 0, {true});

    ReadoutBatch batch;
    unsigned callbacks = 0;
    const auto caller = std::this_thread::get_id();
    bool callbackThread = true;
    const auto observed = [&] { ++callbacks; callbackThread &= std::this_thread::get_id() == caller; };
    double gain = 0;
    std::uint64_t instance = 0;
    std::vector<PluginParameterReadout> parameters;
    std::shared_ptr<const engine::SampleBuffer> first, second;
    batch.add<Method::pluginParameter>({eq, "output.gain"}, [&](auto reply) { observed(); gain = std::get<0>(reply); });
    batch.add<Method::pluginInstanceId>({eq}, [&](auto reply) { observed(); instance = std::get<0>(reply); });
    batch.add<Method::readPluginParameters>({eq, std::vector<PluginParameterReadout>{{"output.gain"}, {"absent"}}},
        [&](auto reply) { observed(); parameters = std::move(std::get<0>(std::get<1>(reply))); });
    batch.add<Method::samplerSnapshot>({sampler}, [&](auto reply) {
        observed(); const auto& value = std::get<0>(reply); if (value.sample) first = value.sample->audio;
    });
    batch.add<Method::samplerSnapshot>({sampler}, [&](auto reply) {
        observed(); const auto& value = std::get<0>(reply); if (value.sample) second = value.sample->audio;
    });
    check(bool(batch.execute(process)) && callbacks == 5 && callbackThread && instance && std::abs(gain + 6) < 1e-6 &&
          parameters.size() == 2 && parameters[0].available && !parameters[1].available &&
          first && first == second && first->frames() == 512 && first->channel(0)[20] == .125f,
          "one typed batch returns ordered owned values and one shared PCM mapping on the calling control thread");
    const auto retained = first;
    check(bool(batch.execute(process)) && callbacks == 10 && first == retained,
          "reused subscriptions reuse the session PCM cache without rebuilding mappings");
    const auto beforeFailure = callbacks;
    check(!batch.execute(process, 9999) && callbacks == beforeFailure,
          "failed session request publishes no callback values");

    // The master strip subscribes even on an empty project. After 0.4 s of
    // silence the meter reports -infinity; after 3 s all three readings do.
    AudioControlPacket play;
    play.generation = packet.generation; play.requestId = 1;
    play.command = AudioTransportCommand{.action = AudioTransportCommand::Action::Play};
    check(bool(process.send(play)) && bool(process.advanceForTest(64, 2251)),
          "silent production callback fills every loudness measurement window");
    engine::LoudnessLevels silence;
    ReadoutBatch silentBatch;
    silentBatch.add<Method::masterLoudness>({}, [&](auto reply) { silence = std::get<0>(reply); });
    silentBatch.add<Method::pluginEditorSnapshot>({eq}, [&](auto reply) {
        observed(); check(std::get<0>(reply).has_value(), "plugin editor readout survives silent master meter");
    });
    check(bool(silentBatch.execute(process)) &&
          silence.momentary == -std::numeric_limits<float>::infinity() &&
          silence.shortTerm == -std::numeric_limits<float>::infinity() &&
          silence.integrated == -std::numeric_limits<float>::infinity(),
          "silence is a valid batched observation and leaves plugin reads connected");
    AudioProcessSnapshot afterSilence;
    check(bool(process.poll(afterSilence)) && afterSilence.transport.playing &&
          afterSilence.transport.position >= 144000,
          "silent loudness polling preserves the running transport");

    ReadoutBatch bounded;
    for (std::size_t i = 0; i < kMaxReadoutBatchItems; ++i)
        bounded.add<Method::pluginInstanceId>({eq}, [](auto) {});
    bool rejected = false;
    try { bounded.add<Method::pluginInstanceId>({eq}, [](auto) {}); }
    catch (const std::length_error&) { rejected = true; }
    check(rejected && bounded.size() == kMaxReadoutBatchItems, "subscription count is bounded before IPC");
    process.close();
    check(retained && retained->channel(1)[20] == -.25f, "caller-owned PCM readout survives child and registry teardown");
}
}

int main(int argc, char** argv) try {
    static_assert(isReadoutMethod(Method::readPluginParameters));
    static_assert(!isReadoutMethod(Method::setPluginParameter));
    static_assert(!isReadoutMethod(Method::pluginStateSnapshot));
    static_assert(!isReadoutMethod(Method::pumpPluginEditor));
    if (argc != 2) return 2;
    const auto directory = std::filesystem::temp_directory_path() /
        ("vlt-readout-batch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory / "input");
    std::filesystem::create_directories(directory / "output");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{directory};
    malformedRequestDoesNotConsume(directory);
    childBatch(argv[1]);
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what()); return 1;
}
