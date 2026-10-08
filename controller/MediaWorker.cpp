#include "MediaWorker.hpp"
#include "SampleLoader.hpp"
#include "AudioValueCodec.hpp"
#include "AudioRuntimePluginFields.hpp"
#include "platform/PathUtils.hpp"
#include <atomic>
#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>
#include <semaphore>


namespace audio::platform {
template<class A> void fields(A& a, AudioFileInfo& v) { a(v.channels, v.sampleRate, v.frames, v.frameCountIsEstimate); }
}
namespace daw::analysis {
template<class A> void fields(A& a, MusicalAnalysisRequest& v) {
    a(v.detectTempo, v.detectKey, v.offsetSeconds, v.durationSeconds, v.stretchTime,
        v.pitchShiftSemitones, v.fileNameHint, v.useNeuralModels);
}
template<class A> void fields(A& a, TempoEstimate& v) {
    a(v.algorithmVersion, v.status, v.bpm, v.confidence, v.stability, v.alternatives,
        v.variable, v.reason, v.backend, v.calibrated, v.evidence);
}
template<class A> void fields(A& a, KeyEstimate& v) {
    a(v.algorithmVersion, v.status, v.root, v.scale, v.confidence, v.alternateRoot,
        v.alternateScale, v.tuningCents, v.reason, v.backend, v.calibrated,
        v.variable, v.evidence, v.profileScores, v.neuralScores);
}
template<class A> void fields(A& a, MusicalAnalysisResult& v) { a(v.algorithmVersion, v.tempo, v.key, v.analyzedSeconds); }
template<class A> void fields(A& a, WarpTransient& v) { a(v.sourceSeconds, v.strength, v.confidence); }
template<class A> void fields(A& a, SamplePitchEstimate& v) { a(v.status, v.midiNote, v.frequencyHz, v.cents); }
}
namespace daw {
namespace media_job {
enum class Kind : std::uint32_t { Probe, Decode, Musical, Transients, Pitch, Slice };
struct Request {
    std::uint32_t version = 1;
    Kind kind = Kind::Probe;
    std::string path;
    std::shared_ptr<const engine::SampleBuffer> audio;
    analysis::MusicalAnalysisRequest musical;
    double begin = 0, end = 0;
    slicer::SliceSettings slices;
};
struct Reply {
    std::uint32_t version = 1;
    audio::platform::AudioFileInfo info;
    std::shared_ptr<const engine::SampleBuffer> audio;
    analysis::MusicalAnalysisResult musical;
    std::vector<analysis::WarpTransient> transients;
    analysis::SamplePitchEstimate pitch;
    plugins::slicer::SliceTable slices;
};
template<class A> void fields(A& a, Request& v) { a(v.version, v.kind, v.path, v.audio, v.musical, v.begin, v.end, v.slices); }
template<class A> void fields(A& a, Reply& v) { a(v.version, v.info, v.audio, v.musical, v.transients, v.pitch, v.slices); }
}
namespace {
using namespace media_job;
std::atomic<std::shared_ptr<const std::string>> helper;
// A folder scan and concurrent editors cannot launch unbounded decoder/model
// processes. Waiting callers still poll their own cancellation every 20 ms.
std::counting_semaphore<2> capacity(2);
thread_local unsigned activeJobs = 0;
struct Slot { Slot() { ++activeJobs; } ~Slot() { --activeJobs; capacity.release(); } };
void require(const audio::Result& result) { if (!result) throw std::runtime_error(result.message()); }
void writePacket(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    if (bytes.size() > 16u * 1024u * 1024u) throw std::runtime_error("media packet exceeds size limit");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot write media packet");
}
std::vector<std::uint8_t> readPacket(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
        throw std::runtime_error("missing media packet");
    const auto size = std::filesystem::file_size(path);
    if (size > 16u * 1024u * 1024u) throw std::runtime_error("media packet exceeds size limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) throw std::runtime_error("cannot read media packet");
    return bytes;
}
audio::Result run(const Request& request, Reply& out, const std::function<bool(double)>& keepGoing) {
    out = {};
    try {
        const auto executable = helper.load();
        if (!executable) return audio::Result::fail(audio::EngineError::NotInitialized, "media worker is not configured");
        const auto poll = [&] { return !keepGoing || keepGoing(0); };
        if (!poll()) return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
        if (activeJobs) {
            if (!capacity.try_acquire()) return audio::Result::fail(audio::EngineError::Unknown, "background workers are busy");
        } else {
            while (!capacity.try_acquire_for(std::chrono::milliseconds(20)))
                if (!poll()) return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
        }
        Slot slot;
        WorkerJob job(std::filesystem::temp_directory_path());
        ProcessAudioResources resources(job.root / "input", [&] { return !poll(); });
        writePacket(job.root / "media.request", audio_value::encodeResources(resources, request));
        bool cancelled = false;
        nlohmann::json reply;
        const auto result = job.run(*executable, [&](const rendering::Progress& progress) {
            return !keepGoing || keepGoing(progress.fraction);
        }, reply, cancelled);
        if (cancelled) return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
        if (!result) return result;
        if (!reply.value("media", false)) throw std::runtime_error("unexpected media worker result");
        auto [decoded] = audio_value::decodeResources<Reply>(readPacket(job.root / "output" / "media.reply"), job.root / "output");
        if (decoded.version != 1) throw std::runtime_error("incompatible media worker result");
        out = std::move(decoded);
        return audio::Result::ok();
    } catch (const std::exception& error) { return audio::Result::fail(audio::EngineError::Unknown, error.what()); }
}
auto noProgress(const std::function<bool()>& keepGoing) {
    return [keepGoing](double) { return !keepGoing || keepGoing(); };
}
Request sampleRequest(Kind kind, const engine::SampleBuffer& audio, double begin = 0, double end = 0) {
    Request request; request.kind = kind; request.begin = begin; request.end = end;
    // Synchronous job scope pins its caller. The worker receives an immutable
    // file, never this borrowed process-local address.
    request.audio = {&audio, [](const engine::SampleBuffer*) {}};
    return request;
}
class DecodedSource final : public audio::platform::AudioFileSource {
public:
    explicit DecodedSource(std::shared_ptr<const engine::SampleBuffer> audio) : audio(std::move(audio)) {}
    audio::platform::AudioFileInfo info() const override { return {audio->frames(), audio->sampleRate(), audio->channels(), false}; }
    audio::Result seek(audio::FrameCount frame) override { position = std::min(frame, audio::FrameCount(audio->frames())); return audio::Result::ok(); }
    audio::FrameCount read(float* destination, audio::FrameCount frames) override {
        const auto count = std::min(frames, audio::FrameCount(audio->frames()) - position);
        for (engine::ChannelCount ch = 0; ch < audio->channels(); ++ch) {
            const auto* source = audio->channel(ch) + position;
            for (audio::FrameCount frame = 0; frame < count; ++frame) destination[frame * audio->channels() + ch] = source[frame];
        }
        position += count;
        return count;
    }
    audio::Result readStatus() const override { return audio::Result::ok(); }
private:
    std::shared_ptr<const engine::SampleBuffer> audio;
    audio::FrameCount position = 0;
};

}

void MediaWorker::install(const std::string& executable) {
    helper.store(std::make_shared<const std::string>(executable));
    auto services = std::make_shared<audio::platform::AudioFileServices>();
    services->probe = probe;
    services->open = [](const std::string& path, std::unique_ptr<audio::platform::AudioFileSource>& source,
        const std::function<bool()>& keepGoing) {
        std::shared_ptr<const engine::SampleBuffer> audio;
        const auto result = loadSampleBuffer(path, audio, keepGoing);
        if (result) source = std::make_unique<DecodedSource>(std::move(audio));
        return result;
    };
    audio::platform::setAudioFileServices(std::move(services));
    setSampleLoader(decode);
}
bool MediaWorker::enabled() noexcept { return bool(helper.load()); }
audio::Result MediaWorker::probe(const std::string& path, audio::platform::AudioFileInfo& out) {
    Request request; request.path = path;
    Reply reply;
    const auto result = run(request, reply, {});
    if (result) {
        if (!reply.info.frames || !reply.info.channels || reply.info.channels > engine::kMaxChannels ||
            reply.info.sampleRate <= 0) return audio::Result::fail(audio::EngineError::UnsupportedFormat, "invalid audio metadata");
        out = reply.info;
    }
    return result;
}
audio::Result MediaWorker::decode(const std::string& path, std::shared_ptr<const engine::SampleBuffer>& out,
    const std::function<bool()>& keepGoing) {
    if (keepGoing && !keepGoing()) return audio::Result::fail(audio::EngineError::InvalidArgument, "cancelled");
    Request request; request.kind = Kind::Decode; request.path = path;
    Reply reply;
    const auto result = run(request, reply, noProgress(keepGoing));
    if (!result) return result;
    if (!reply.audio) return audio::Result::fail(audio::EngineError::UnsupportedFormat, "decoder returned no audio");
    out = std::move(reply.audio);
    return audio::Result::ok();
}
audio::Result MediaWorker::analyze(const std::string& path, const analysis::MusicalAnalysisRequest& analysis,
    analysis::MusicalAnalysisResult& out, const analysis::AnalysisProgress& progress) {
    Request request; request.kind = Kind::Musical; request.path = path; request.musical = analysis;
    Reply reply;
    const auto result = run(request, reply, [&](double value) { return !progress || progress(value, "analysis"); });
    if (result) out = std::move(reply.musical);
    return result;
}
std::vector<analysis::WarpTransient> MediaWorker::transients(const engine::SampleBuffer& audio,
    double begin, double end, const std::function<bool()>& keepGoing) {
    Reply reply;
    return run(sampleRequest(Kind::Transients, audio, begin, end), reply, noProgress(keepGoing))
        ? std::move(reply.transients) : std::vector<analysis::WarpTransient>{};
}
analysis::SamplePitchEstimate MediaWorker::pitch(const engine::SampleBuffer& audio,
    engine::FrameCount first, engine::FrameCount end, const std::function<bool()>& keepGoing) {
    Reply reply;
    return run(sampleRequest(Kind::Pitch, audio, first, end), reply, noProgress(keepGoing)) ? reply.pitch : analysis::SamplePitchEstimate{};
}
plugins::slicer::SliceTable MediaWorker::slice(const engine::SampleBuffer& audio,
    const slicer::SliceSettings& settings, const std::function<bool()>& keepGoing) {
    auto request = sampleRequest(Kind::Slice, audio); request.slices = settings;
    Reply reply;
    return run(request, reply, noProgress(keepGoing)) ? reply.slices : plugins::slicer::SliceTable{};
}
nlohmann::json MediaWorker::execute(const std::filesystem::path& root, const WorkerJob::Progress& progress) {
    auto [request] = audio_value::decodeResources<Request>(readPacket(root / "media.request"), root / "input");
    if (request.version != 1) throw std::runtime_error("incompatible media worker request");
    const auto keepGoing = [&] { return progress({rendering::Progress::Stage::Preparing}); };
    Reply reply;
    switch (request.kind) {
    case Kind::Probe: require(audio::platform::probeAudioFile(request.path, reply.info)); break;
    case Kind::Decode: require(loadSampleBuffer(request.path, reply.audio, keepGoing)); break;
    case Kind::Musical:
        require(analysis::analyzeAudioFile(request.path, request.musical, reply.musical,
            [&](double fraction, std::string_view) { return progress({rendering::Progress::Stage::Preparing, fraction}); }));
        break;
    case Kind::Transients: case Kind::Pitch: case Kind::Slice:
        if (!request.audio) throw std::runtime_error("media analysis requires audio");
        if (request.kind == Kind::Transients) reply.transients = analysis::detectWarpTransients(*request.audio, request.begin, request.end, keepGoing);
        else if (request.kind == Kind::Pitch) {
            if (request.begin < 0 || request.end < request.begin || request.end > request.audio->frames()) throw std::runtime_error("invalid pitch analysis range");
            reply.pitch = analysis::detectSamplePitch(*request.audio, engine::FrameCount(request.begin), engine::FrameCount(request.end), keepGoing);
        } else reply.slices = slicer::cut(*request.audio, request.slices, keepGoing);
        break;
    default: throw std::runtime_error("unknown media worker operation");
    }
    if (!keepGoing()) throw std::runtime_error("cancelled");
    ProcessAudioResources resources(root / "output", [&] { return !keepGoing(); });
    writePacket(root / "output" / "media.reply", audio_value::encodeResources(resources, reply));
    return {{"media", true}};
}
} // namespace daw
