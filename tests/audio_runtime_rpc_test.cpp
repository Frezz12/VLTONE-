#include "AudioRuntimeCalls.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/SamplerInstance.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {
using namespace daw;
using audio_rpc::Method;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value; return value;
}
template<Method Id> auto request(AudioRuntimeProcess& process, typename audio_rpc::Binding<Id>::Inputs arguments) {
    auto [status, reply] = audio_rpc::call<Id>(process, 0, std::move(arguments));
    if (!status) throw std::runtime_error(status.message());
    return reply;
}
AudioPluginSpec plugin(std::string id, const plugins::PluginDescriptor& descriptor) {
    AudioPluginSpec value;
    value.id = std::move(id); value.uid = descriptor.uid; value.name = descriptor.name;
    value.descriptor = descriptor; value.requiredFormat = descriptor.format;
    return value;
}
}

int main(int argc, char** argv) try {
    using namespace daw;
    if (argc != 2) return 2;
    const auto directory = std::filesystem::temp_directory_path() /
        ("vlt-runtime-values-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Cannot create private test directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{directory};
    AudioRuntimeProcess process(argv[1]);
    AudioSessionPacket packet;
    packet.generation = packet.revision = 1; packet.blockSize = 64;
    AudioGraphSpec::Channel channel; channel.id = "source"; channel.acceptsMidi = true;
    packet.session.graph.channels.push_back(channel);
    packet.session.pluginChains.push_back({"source", AudioPluginChainSpec::Kind::Inserts, {},
        {plugin("eq", plugins::equalizer::EqualizerInstance::staticDescriptor())}});
    packet.session.pluginChains.push_back({"source", AudioPluginChainSpec::Kind::Instrument, {},
        {plugin("sampler", plugins::sampler::SamplerInstance::staticDescriptor())}});
    if (!check(bool(process.replaceSession(packet)), "runtime values use a real child process")) return 1;
    auto unsafe = packet; ++unsafe.generation;
    unsafe.session.pluginChains.front().slots.front().descriptor.format = plugins::Format::Clap;
    unsafe.session.pluginChains.front().slots.front().requiredFormat = plugins::Format::Clap;
    check(!process.replaceSession(unsafe) && process.running() && process.generation() == 1,
          "audio process refuses loading an external plugin into its own address space");

    AudioPluginAddress eq{"source", "eq"};
    auto [identity, ignored] = request<Method::pluginInstanceId>(process, {eq});
    check(identity != 0, "plugin identity crosses as an integer"); eq.instance = identity;
    auto [changed, noOutputs] = request<Method::setPluginParameter>(process, {eq, "output.gain", -6.0});
    auto [parameters, noParameterOutputs] = request<Method::pluginParameters>(process, {eq});
    check(changed && !parameters.empty(), "plugin mutation and metadata use the existing runtime implementation");
    std::vector<PluginParameterReadout> values{{"output.gain"}, {"does-not-exist"}};
    auto [none, readouts] = request<Method::readPluginParameters>(process, {eq, values});
    const auto& read = std::get<0>(readouts);
    check(read.size() == 2 && read[0].available && std::abs(read[0].value + 6) < 1e-6 && !read[1].available,
          "mutable spans return one owned parameter batch with stable IDs");
    auto stale = eq; ++stale.instance;
    check(!std::get<0>(request<Method::setPluginParameter>(process, {stale, "output.gain", 12.0})),
          "a delayed command cannot mutate another native instance");
    check(!std::get<0>(request<Method::restorePluginState>(process,
          {stale, AudioPluginStateRestore{}, std::vector<InsertParameter>{}})).result(),
          "stale state restore is rejected explicitly instead of acknowledged as applied");
    check(bool(process.captureCheckpoint()) && bool(process.restart()), "native edits are included in an explicit crash checkpoint");
    eq.instance = 0;
    check(std::abs(std::get<0>(request<Method::pluginParameter>(process, {eq, "output.gain"})) + 6) < 1e-6,
          "native parameter edit survives restart through the checkpoint");
    std::uint64_t secondary = 0;
    check(bool(process.createSession(packet, secondary)), "secondary session shares the audio child");
    const auto secondaryEdit = audio_rpc::call<Method::setPluginParameter>(process, secondary,
        {eq, "output.gain", 9.0});
    check(bool(secondaryEdit.first) && std::get<0>(secondaryEdit.second) &&
          std::abs(std::get<0>(request<Method::pluginParameter>(process, {eq, "output.gain"})) + 6) < 1e-6,
          "identical slot addresses remain independent across sessions");
    process.closeSession(secondary);

    auto audio = std::make_shared<engine::SampleBuffer>(2, 512, 48000);
    std::fill_n(audio->writableChannel(0), audio->frames(), .125f);
    std::fill_n(audio->writableChannel(1), audio->frames(), -.25f);
    AudioPluginAddress sampler{"source", "sampler"};
    check(std::get<0>(request<Method::loadInstrumentSample>(process, {sampler, "mapped-only.wav", audio})),
          "sampler source is transferred through the immutable PCM registry");
    request<Method::flushSamplerPrecompute>(process, {true});
    auto [state, stateOutputs] = request<Method::pluginStateSnapshot>(process, {sampler, false, std::nullopt, AudioPluginSnapshotPurpose::Exact});
    auto [again, againOutputs] = request<Method::pluginStateSnapshot>(process, {sampler, false, std::nullopt, AudioPluginSnapshotPurpose::Exact});
    check(state.exists && state.sample && state.sample->frames() == 512 &&
          state.sample->channel(0)[20] == .125f && state.sample->channel(1)[20] == -.25f &&
          state.sample == again.sample,
          "child PCM readouts map once and retain exact channel samples");
    auto [samplerView, samplerOutputs] = request<Method::samplerSnapshot>(process, {sampler});
    check(samplerView.available && samplerView.hasSource && samplerView.sample,
          "sampler editor receives its immutable display sample");
    {
        // No checkpoint between the source edit and restart: an acknowledged
        // source must own its PCM even when it has never existed on disk.
        check(bool(process.restart()), "source edit can restart without a later periodic checkpoint");
        const auto source = [&] {
            return std::get<0>(request<Method::pluginStateSnapshot>(process,
                {sampler, true, std::nullopt, AudioPluginSnapshotPurpose::Exact}));
        };
        check(source().sample && source().sample->channel(0)[20] == .125f,
              "acknowledged mapped-only source survives a whole-process restart");
        std::uint64_t transaction = 0;
        check(bool(process.captureTransaction(transaction)), "source edit pins an audio transaction");
        auto alternate = std::make_shared<engine::SampleBuffer>(2, 256, 48000);
        std::fill_n(alternate->writableChannel(0), alternate->frames(), .75f);
        std::fill_n(alternate->writableChannel(1), alternate->frames(), -.75f);
        check(std::get<0>(request<Method::loadInstrumentSample>(process, {sampler, "alternate.wav", alternate})) &&
              source().sample->frames() == 256, "transaction changes the native sampler source");
        check(bool(process.restoreTransaction(transaction)) && source().sample && source().sample->frames() == 512 &&
              source().sample->channel(1)[20] == -.25f, "rollback restores source bytes and native sampler configuration together");
        check(bool(process.releaseTransaction(transaction)) && bool(process.restart()) && source().sample &&
              source().sample->channel(0)[20] == .125f, "rolled-back source remains the recovery baseline");
        AudioPluginControlChange partial; partial.bypassed = true;
        check(std::get<0>(request<Method::setPluginControls>(process, {"source", "eq", partial})), "host bypass acknowledged");
        partial = {}; partial.mix = .3f;
        check(std::get<0>(request<Method::setPluginControls>(process, {"source", "eq", partial})) &&
              std::get<0>(request<Method::setPluginSlide>(process, {sampler, 1, 7., 2.})) &&
              bool(process.restart()), "coalesced host controls survive restart");
        const auto slide = std::get<0>(request<Method::pluginSlideStatus>(process, {sampler}));
        check(slide.mode == 1, "slide control is restored independently of private plugin checkpoints");
    }

    values.resize(32768);
    for (std::size_t i = 0; i < values.size(); ++i) values[i].id = std::string(128, 'x') + std::to_string(i);
    auto [largeNone, largeOutputs] = request<Method::readPluginParameters>(process, {eq, values});
    const auto& large = std::get<0>(largeOutputs);
    check(large.size() == values.size() && large.back().id == values.back().id && !large.back().available,
          "large value requests and replies use bounded files without truncation");

    AudioCaptureSpec capture; capture.directory = directory.string(); capture.channelCount = 2;
    auto [startedResult, startedOutputs] = request<Method::startCapture>(process, {capture, AudioCaptureStarted{}});
    const auto started = std::get<0>(startedOutputs);
    check(bool(startedResult.result()) && started.id &&
          std::get<0>(request<Method::publishCaptures>(process, {std::vector<AudioCaptureId>{started.id}})),
          "recording starts and publishes by numeric capture ID");
    check(bool(process.advanceForTest(64, 4)), "real child callback feeds the published capture");
    const auto recorded = std::get<0>(request<Method::captureStatus>(process, {started.id}));
    auto [closedResult, closedOutputs] = request<Method::stopCapture>(process, {started.id, audio::RecordingSession{}});
    const auto& closed = std::get<0>(closedOutputs);
    check(recorded.available && recorded.recordedFrames == 256 && bool(closedResult.result()) &&
          closed.fileWriteSucceeded && closed.writtenFrames == 256 && std::filesystem::exists(closed.filePath),
          "capture reply acknowledges a closed durable WAV and exact frame count");

    const auto malformed = process.invoke(std::uint32_t(Method::setPluginParameter),
        [](auto&) { return std::vector<std::uint8_t>{0, 0, 0, 0, 255}; },
        [](auto, const auto&, auto&) {});
    const auto unknown = process.invoke(9999, [](auto& resources) {
        return audio_value::encodeResources(resources, std::tuple<>{});
    }, [](auto, const auto&, auto&) {});
    const auto trailing = process.invoke(std::uint32_t(Method::setPluginParameter), [&](auto& resources) {
        auto bytes = audio_value::encodeResources(resources,
            audio_rpc::Binding<Method::setPluginParameter>::Inputs{eq, "output.gain", 12.0});
        bytes.push_back(255); return bytes;
    }, [](auto, const auto&, auto&) {});
    check(!malformed && !unknown && !trailing && process.running() &&
          std::abs(std::get<0>(request<Method::pluginParameter>(process, {eq, "output.gain"})) + 6) < 1e-6,
          "malformed payload, trailing data and unknown operation are rejected before mutation");
    auto [retiredState, retiredOutputs] = request<Method::pluginStateSnapshot>(process, {sampler, false, std::nullopt, AudioPluginSnapshotPurpose::Exact});
    const auto savedSampler = std::get<0>(request<Method::pluginStateSnapshot>(process, {sampler, true, std::nullopt, AudioPluginSnapshotPurpose::Exact}));
    const auto savedEq = std::get<0>(request<Method::pluginStateSnapshot>(process, {eq, true, std::nullopt, AudioPluginSnapshotPurpose::Exact}));
    auto replacement = packet; replacement.generation = process.generation() + 1;
    AudioPluginStateEdit samplerRestore;
    samplerRestore.address = sampler; samplerRestore.state.state = savedSampler.state;
    samplerRestore.state.stateFile = "saved-sampler.bin";
    samplerRestore.state.sourcePath = savedSampler.samplePath;
    samplerRestore.state.source = savedSampler.sample;
    samplerRestore.parameters = savedSampler.parameters;
    replacement.restores.push_back(samplerRestore);
    const auto replacementPid = process.processId();
    check(bool(process.replaceSession(replacement)), "live generation replacement retires its previous RPC resource directory");
    const auto addresses = std::get<0>(request<Method::pluginAddresses>(process, {}));
    check(process.running() && process.processId() == replacementPid && !addresses.empty() &&
          retiredState.sample && retiredState.sample->channel(0)[20] == .125f &&
          retiredState.sample->channel(1)[20] == -.25f,
          "RPC decodes in the new generation while owned old PCM remains readable");
    auto update = replacement; update.revision = 2; update.restores.clear();
    auto fresh = plugin("restored", plugins::equalizer::EqualizerInstance::staticDescriptor());
    fresh.parameters = {{"output.gain", -20}};
    update.session.pluginChains.front().slots.push_back(fresh);
    AudioPluginStateEdit edit; edit.address = {"source", "restored"};
    edit.state.state = {0xff, 0, 0xff}; edit.parameters = fresh.parameters;
    update.restores.push_back(edit);
    const auto unchanged = std::get<0>(request<Method::pluginInstanceId>(process, {eq}));
    const auto previousPublication = process.lastPublicationOwner();
    check(!process.applySession(update, 1) && process.running() &&
          process.lastPublicationOwner() == previousPublication &&
          !std::get<0>(request<Method::hasPlugin>(process, {edit.address, ""})) &&
          std::get<0>(request<Method::pluginInstanceId>(process, {eq})) == unchanged,
          "bad staged opaque state rejects the entire process update and preserves healthy identities");
    update.restores.front().state.state = savedEq.state;
    check(bool(process.applySession(update, 1)) &&
          std::abs(std::get<0>(request<Method::pluginParameter>(process, {edit.address, "output.gain"})) + 6) < 1e-6,
          "new plugin publishes with its preset and ignores stale plain fallback values");
    const auto& publication = process.lastPublication();
    check(publication.imported.size() == 1 && publication.imported.front().address.slotId == "restored" &&
          std::any_of(publication.imported.front().parameters.begin(), publication.imported.front().parameters.end(),
              [](const auto& value) { return value.id == "output.gain" && std::abs(value.value + 6) < 1e-6; }),
          "successful graph acknowledgement already contains the restored canonical parameter mirror");
    ++update.revision; update.restores.clear();
    update.session.graph.channels.front().name = "Unrelated edit";
    check(bool(process.applySession(update, 2)) && bool(process.captureCheckpoint()) && bool(process.restart()),
          "unrelated projection retains acknowledged project state and owned source resources for restart");
    const auto recoveredSampler = std::get<0>(request<Method::pluginStateSnapshot>(process, {sampler, false, std::nullopt, AudioPluginSnapshotPurpose::Exact}));
    check(recoveredSampler.sample && recoveredSampler.sample->channel(1)[20] == -.25f &&
          std::abs(std::get<0>(request<Method::pluginParameter>(process, {edit.address, "output.gain"})) + 6) < 1e-6,
          "staged sampler PCM and native preset survive a checkpoint and whole-child restart");
    const auto brokenReply = process.invoke(std::uint32_t(Method::pluginAddresses), [](auto& resources) {
        return audio_value::encodeResources(resources, std::tuple<>{});
    }, [](auto, const auto&, auto&) { throw std::runtime_error("Rejected reply fixture"); });
    check(!brokenReply && !process.running(), "an unreadable reply stops the child before another ambiguous command");
    check(state.sample->channel(0)[20] == .125f, "owned PCM readout remains valid after child shutdown");
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::printf("FAIL %s\n", error.what()); return 1;
}
