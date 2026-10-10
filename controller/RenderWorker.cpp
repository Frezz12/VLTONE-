#include "RenderWorker.hpp"
#include "RenderWorkerProtocol.hpp"
#include "RenderSessionData.hpp"
#include "RenderOutput.hpp"
#include "AudioAnalysis.hpp"
#include "ProcessAudioResources.hpp"
#include "ProjectSerializer.hpp"
#include "WorkerJob.hpp"
#include "MediaWorker.hpp"
#include <nlohmann/json.hpp>
#include <fstream>

namespace audio::platform {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WriteSpec, container, encoding, vbrQuality, bitrateKbps, dither)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileTags, title, artist, comment, software, timeReferenceSamples, album, coverArt)
}
namespace daw::rendering {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Spec, outputDir, baseName, file, sampleRate, blockSize,
    pipeline, forcePipeline, channels, range, customStartSeconds, customEndSeconds,
    preRollSeconds, tail, tailSeconds, tailSilenceDb, tailHoldSeconds, tailMaxSeconds,
    writeMixdown, stemChannelIds, tags, bypassChannelInserts, bypassClipInserts,
    bypassTrackInserts, bypassSummingInserts, bypassSends, bypassMasterChain,
    sourceTrackIds, independentTrackId, sourceClipIds, soloChannelId, ignoreMuteSolo, stemsPreFader, stemsAtSource)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Report, files, usedPipeline, cancelled, renderedSeconds)
}
namespace daw::plugins::sampler {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PrecomputeSettings, boost, eqLow, eqMid, eqHigh,
    ringMix, ringFreq, cut, res, reverbType, reverb, stereoDelay, pogo,
    removeDc, reversePolarity, normalize, fadeStereo, reverse, swapStereo)
}
namespace daw::analysis {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Metrics, peak, peakDb, rms, rmsDb,
    lowFraction, midFraction, highFraction, clipped, seconds, silent)
}

namespace daw {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using rendering::ipc::kVersion;
constexpr std::size_t kMaxManifest = 256u * 1024u * 1024u;


void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
void require(const audio::Result& result) { require(bool(result), result.message()); }

fs::path privateFile(const fs::path& root, const std::string& name) {
    const auto leaf = platform::pathFromUtf8(name);
    require(!name.empty() && name != "." && name != ".." && leaf == leaf.filename() &&
            name.find_first_of("/\\:") == std::string::npos, "invalid worker resource name");
    const auto path = root / leaf;
    require(!fs::is_symlink(fs::symlink_status(path)), "worker resource is a symbolic link");
    return path;
}
std::string readFile(const fs::path& file, std::size_t limit = kMaxManifest) {
    require(fs::is_regular_file(file), "missing worker resource");
    const auto size = fs::file_size(file);
    require(size <= limit, "worker resource exceeds size limit");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream stream(file, std::ios::binary);
    require(bool(stream.read(bytes.data(), std::streamsize(bytes.size()))), "cannot read worker resource");
    return bytes;
}
void writeFile(const fs::path& file, const char* bytes, std::size_t size) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(bytes, std::streamsize(size));
    stream.close();
    require(bool(stream), "cannot write worker resource");
}
// Deducing Data keeps its private controller cache types out of this transport
// interface. Only sample values/resources are serialized; waveform UI caches
// and already-prepared DSP objects have no meaning in the worker.
void writeSession(const auto& data, const fs::path& root,
                  const ProcessAudioResources::Cancelled& cancelled,
                  bool normalize, bool analyze) {
    ProcessAudioResources resources(root, cancelled);
    Json manifest{{"version", kVersion}, {"spec", data.spec}, {"rate", data.sampleRate},
        {"block", data.blockSize}, {"revision", data.revision}, {"generation", data.generation},
        {"normalize", normalize}, {"analyze", analyze}};
    std::string document;
    require(ProjectSerializer::serializeDocument(data.snapshot.project, document, MediaPaths::Absolute));
    writeFile(root / "document.json", document.data(), document.size());
    require(data.catalog.cache.save(platform::pathToUtf8(root / "catalog.json")), "cannot save render plugin catalogue");
    auto& states = manifest["states"] = Json::array();
    for (const auto& state : data.snapshot.pluginStates) {
        require(!cancelled(), "render cancelled");
        const auto file = "state-" + std::to_string(states.size()) + ".bin";
        require(state.bytes.size() <= kMaxManifest, "plugin state exceeds worker limit");
        writeFile(root / file, reinterpret_cast<const char*>(state.bytes.data()), state.bytes.size());
        states.push_back({{"key", state.fileName}, {"file", file}});
    }
    auto& pending = manifest["pending"] = Json::object();
    for (const auto& [key, parameters] : data.pendingParameterOverrides) {
        auto& values = pending[key] = Json::array();
        for (const auto& p : parameters)
            values.push_back({{"id", p.id}, {"value", p.value}, {"restore", p.restoreAfterState}});
    }
    const auto samples = [&](const auto& input) {
        Json out = Json::object();
        for (const auto& [key, audio] : input) out[key] = resources.put(audio);
        return out;
    };
    manifest["sources"] = samples(data.sourceSamples);
    manifest["samples"] = samples(data.samples);
    const auto sampleData = [&](const auto& value) -> Json {
        if (!value) return nullptr;
        return {{"audio", resources.put(value->audio)}, {"baseFrames", value->baseFrames},
                {"path", value->path}, {"name", value->name}};
    };
    auto& clips = manifest["clips"] = Json::object();
    for (const auto& [key, entry] : data.clipSamples)
        clips[key] = {{"path", entry.path}, {"settings", entry.settings}, {"data", sampleData(entry.data)}};
    auto& shared = manifest["shared"] = Json::array();
    for (const auto& entry : data.sharedClipSamples)
        if (auto value = entry.data.lock())
            shared.push_back({{"path", entry.path}, {"settings", entry.settings}, {"data", sampleData(value)}});
    auto& records = manifest["resources"] = Json::array();
    for (const auto& record : resources.records()) records.push_back({{"id", record.id},
        {"file", record.fileName}, {"channels", record.channels}, {"frames", record.frames}, {"rate", record.sampleRate}});
    const auto bytes = manifest.dump();
    require(bytes.size() <= kMaxManifest, "render session exceeds worker limit");
    writeFile(root / "session.json", bytes.data(), bytes.size());
}

std::pair<bool, bool> readSession(auto& data, const fs::path& root, const ProcessAudioResources::Cancelled& cancelled) {
    const auto manifest = Json::parse(readFile(privateFile(root, "session.json")));
    require(manifest.at("version") == kVersion, "incompatible render session");
    manifest.at("spec").get_to(data.spec);
    data.spec.outputDir = platform::pathToUtf8(root / "output");
    manifest.at("rate").get_to(data.sampleRate);
    manifest.at("block").get_to(data.blockSize);
    manifest.at("revision").get_to(data.revision);
    manifest.at("generation").get_to(data.generation);
    require(std::isfinite(data.sampleRate) && data.sampleRate >= 1000 && data.sampleRate <= 768000 &&
            data.blockSize > 0 && data.blockSize <= engine::kMaxBlockSize, "invalid render session format");
    require(ProjectSerializer::deserializeDocument(data.snapshot.project,
        readFile(privateFile(root, "document.json"))));
    require(data.catalog.cache.load(platform::pathToUtf8(privateFile(root, "catalog.json"))),
            "cannot read render plugin catalogue");
    for (const auto& state : manifest.at("states")) {
        require(!cancelled(), "render cancelled");
        const auto bytes = readFile(privateFile(root, state.at("file").get<std::string>()));
        data.snapshot.pluginStates.push_back({state.at("key").get<std::string>(), {bytes.begin(), bytes.end()}});
    }
    for (const auto& [key, values] : manifest.at("pending").items())
        for (const auto& value : values) {
            const double number = value.at("value").get<double>();
            require(std::isfinite(number), "invalid render parameter");
            data.pendingParameterOverrides[key].push_back({value.at("id").get<std::string>(),
                number, value.at("restore").get<bool>()});
        }
    std::vector<ProcessAudioResources::Record> records;
    for (const auto& r : manifest.at("resources")) records.push_back({r.at("id").get<std::string>(),
        r.at("file").get<std::string>(), r.at("channels").get<std::uint32_t>(),
        r.at("frames").get<std::uint32_t>(), r.at("rate").get<double>()});
    const auto resources = ProcessAudioResources::load(root, records, cancelled);
    const auto audio = [&](const Json& value) -> std::shared_ptr<const engine::SampleBuffer> {
        const auto id = value.get<std::string>();
        return id.empty() ? nullptr : resources.at(id);
    };
    for (const auto& [key, value] : manifest.at("sources").items()) data.sourceSamples[key] = audio(value);
    for (const auto& [key, value] : manifest.at("samples").items()) data.samples[key] = audio(value);
    const auto sampleData = [&](const Json& value) -> std::shared_ptr<const plugins::sampler::SampleData> {
        if (value.is_null()) return nullptr;
        auto sample = std::make_shared<plugins::sampler::SampleData>();
        sample->audio = audio(value.at("audio"));
        value.at("baseFrames").get_to(sample->baseFrames);
        value.at("path").get_to(sample->path);
        value.at("name").get_to(sample->name);
        return sample;
    };
    for (const auto& [key, value] : manifest.at("clips").items()) {
        auto& entry = data.clipSamples[key];
        value.at("path").get_to(entry.path);
        value.at("settings").get_to(entry.settings);
        entry.data = sampleData(value.at("data"));
    }
    // Shared entries are weak references in the controller. Reuse the strong
    // clip entry whenever possible; otherwise source audio still remains pinned
    // and the worker can cheaply rebuild the processed cache on demand.
    for (const auto& value : manifest.at("shared")) {
        auto sample = sampleData(value.at("data"));
        if (!sample) continue;
        for (const auto& [key, clip] : data.clipSamples)
            if (clip.data && clip.data->audio == sample->audio) { sample = clip.data; break; }
        auto& entry = data.sharedClipSamples.emplace_back();
        value.at("path").get_to(entry.path);
        value.at("settings").get_to(entry.settings);
        entry.data = sample;
    }
    return {manifest.value("normalize", false), manifest.value("analyze", false)};
}

// Analysis and normalization consume the captured render in bounded blocks.
// Normalization never runs a second DSP pass, whose state could differ.
analysis::Metrics processOutput(const std::string& path, bool normalize,
    const std::function<bool(const rendering::Progress&)>& progress) {
    audio::platform::AudioFileReader reader;
    require(reader.open(path));
    const auto info = reader.info();
    require(info.channels > 0 && info.channels <= 2 && info.sampleRate > 0,
            "invalid worker mixdown format");
    constexpr std::uint32_t block = 8192;
    std::vector<float> interleaved(block * info.channels);
    std::vector<float> planar(block * info.channels);
    std::vector<const float*> channels(info.channels);
    for (unsigned ch = 0; ch < info.channels; ++ch) channels[ch] = planar.data() + ch * block;
    const auto read = [&]() {
        const auto frames = reader.read(interleaved.data(), block);
        require(reader.readStatus());
        for (unsigned ch = 0; ch < info.channels; ++ch)
            for (std::uint32_t frame = 0; frame < frames; ++frame)
                planar[ch * block + frame] = interleaved[frame * info.channels + ch];
        require(!progress || progress({rendering::Progress::Stage::Rendering, 1.0,
            info.durationSeconds(), info.durationSeconds()}), "render cancelled");
        return frames;
    };
    analysis::MetricsAccumulator accumulator(info.sampleRate, info.channels);
    while (const auto frames = read()) accumulator.add(channels.data(), frames);
    const auto metrics = accumulator.result();
    if (normalize && metrics.peak > 0.0001) {
        require(reader.seek(0));
        rendering::OutputTransaction normalized;
        audio::platform::AudioFileWriter writer;
        require(writer.open(normalized.stage(path), info.sampleRate, info.channels, info.frames));
        while (const auto frames = read())
            require(writer.write(channels.data(), frames, float(0.99 / metrics.peak)));
        require(writer.close());
        reader.close();
        require(normalized.replaceSingle());
    }
    return metrics;
}
} // namespace

audio::Result RenderWorker::render(const RenderSessionSpec& session,
    const std::function<bool(const rendering::Progress&)>& progress, rendering::Report& out,
    const std::string& executable) {
    return run(session, progress, out, {}, executable);
}

audio::Result RenderWorker::exportMixdown(const RenderSessionSpec& session,
    const std::string& outputPath, bool normalize) {
    rendering::Report report;
    return run(session, {}, report, {outputPath, normalize});
}

audio::Result RenderWorker::analyze(const RenderSessionSpec& session, analysis::Metrics& out) {
    rendering::Report report;
    out = {};
    return run(session, {}, report, {{}, false, &out});
}

audio::Result RenderWorker::run(const RenderSessionSpec& session,
    const std::function<bool(const rendering::Progress&)>& progress, rendering::Report& out,
    const Options& options, const std::string& executable) {
    out = {};
    // Progress may edit/close the source document or reset the caller's session.
    // Pin it before the first callback, including preparation/resource transfer.
    const auto data = session.m_data;
    if (!data)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "invalid render session");
    bool cancelled = false;
    try {
        const auto preparing = [&] {
            if (!cancelled && progress) cancelled = !progress({rendering::Progress::Stage::Preparing});
            return cancelled;
        };
        if (preparing()) { out.cancelled = true; return audio::Result::ok(); }
        const auto helper = executable.empty() ? PluginManager::helperPath("daw_worker") : executable;
        require(fs::is_regular_file(platform::pathFromUtf8(helper)), "render worker is unavailable; reinstall VLTONE");
        WorkerJob job(platform::pathFromUtf8(data->spec.outputDir));
        writeSession(*data, job.root, preparing, options.normalize, options.metrics != nullptr);
        Json reply;
        const auto result = job.run(helper, progress, reply, cancelled);
        if (cancelled) { out.cancelled = true; return audio::Result::ok(); }
        if (!result) return result;
        reply.at("report").get_to(out);
        if (out.cancelled) { out = {}; out.cancelled = true; return audio::Result::ok(); }
        require(std::isfinite(out.renderedSeconds) && out.renderedSeconds >= 0, "invalid render worker duration");
        if (options.metrics) {
            auto metrics = reply.at("metrics").get<analysis::Metrics>();
            for (double value : {metrics.peak, metrics.peakDb, metrics.rms, metrics.rmsDb,
                 metrics.lowFraction, metrics.midFraction, metrics.highFraction, metrics.seconds})
                require(std::isfinite(value), "invalid render worker metrics");
            *options.metrics = metrics;
            out.files.clear();
            return audio::Result::ok();
        }
        if (!options.exactOutput.empty()) require(out.files.size() == 1, "expected one mixdown");
        rendering::OutputTransaction outputs;
        std::unordered_set<std::string> taken;
        const auto outputDir = platform::pathToUtf8(job.root.parent_path());
        std::vector<std::string> published;
        for (const auto& name : out.files) {
            const auto prepared = privateFile(job.root / "output", name);
            require(fs::is_regular_file(prepared), "render worker output is missing");
            const auto extension = platform::pathToUtf8(prepared.extension());
            require(extension.size() > 1, "invalid render output extension");
            const auto target = options.exactOutput.empty()
                ? rendering::uniquePath(outputDir, platform::pathToUtf8(prepared.stem()), extension.substr(1), taken)
                : options.exactOutput;
            fs::rename(prepared, platform::pathFromUtf8(outputs.stage(target)));
            published.push_back(target);
        }
        require(options.exactOutput.empty() ? outputs.commit() : outputs.replaceSingle());
        out.files = std::move(published);
        return audio::Result::ok();
    } catch (const std::exception& error) {
        out = {};
        out.cancelled = cancelled;
        return cancelled ? audio::Result::ok() : audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}

int RenderWorker::main(int argc, char** argv) {
    return WorkerJob::serve(argc, argv, [](const fs::path& root, const WorkerJob::Progress& progress) -> Json {
        if (fs::exists(root / "media.request")) return MediaWorker::execute(root, progress);
        rendering::Report report;
        analysis::Metrics metrics;
        auto data = std::make_shared<RenderSessionSpec::Data>();
        const auto [normalize, analyze] = readSession(*data, root,
            [&] { return !progress({rendering::Progress::Stage::Preparing}); });
        RenderSessionSpec session;
        session.m_data = std::move(data);
        const auto result = EngineController::renderSessionInWorker(session, progress, report);
        if (result && !report.cancelled && (normalize || analyze)) {
            require(report.files.size() == 1, "expected one mixdown for analysis");
            metrics = processOutput(report.files.front(), normalize, progress);
        }
        for (auto& path : report.files) {
            const auto file = platform::pathFromUtf8(path);
            require(file.parent_path() == root / "output", "render escaped its job directory");
            path = platform::pathToUtf8(file.filename());
        }
        return {{"report", report}, {"metrics", metrics},
            {"error", int(result.error())}, {"message", result.message()}};
    });
}
} // namespace daw
