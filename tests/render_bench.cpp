// End-to-end export benchmark: tracks, inserts/track, seconds, optional render block (0 = auto).
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Clap/ClapFactory.hpp"
#include "Internal/InternalFactory.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace ap = audio::platform;
static void require(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}
// Opt-in real-project reproduction. Never saves the project or opens a device.
// render_bench --project manifest output-dir float|16|24|mp3 block pipeline name [seconds]
static int renderProject(int argc, char** argv) {
    require(argc >= 8, "usage: --project manifest output-dir float|16|24|mp3 block pipeline name [seconds]");
    daw::ProjectModel project;
    const fs::path inputPath = argv[2];
    const auto package = fs::is_regular_file(inputPath) ? inputPath.parent_path().string() : inputPath.string();
    auto loaded = daw::ProjectSerializer::load(project, package);
    require(bool(loaded), loaded.message());
    daw::EngineController controller;
    require(bool(controller.initialize(project.sampleRate, 256, false)), "initialize project");
    controller.pluginManager().load();
    const auto opened = controller.openProject(package);
    require(bool(opened), opened.message());
    unsigned plugins = 0;
    for (const auto& node : controller.routingGraph()->nodes)
        if (dynamic_cast<daw::plugins::PluginNode*>(node.node)) ++plugins;
    std::cout << "PROJECT tracks=" << controller.project().tracks.size()
              << " plugins=" << plugins << " rate=" << project.sampleRate << '\n';
    daw::rendering::Spec spec;
    spec.outputDir = argv[3];
    const std::string format = argv[4];
    require(format == "float" || format == "16" || format == "24" || format == "mp3", "format");
    spec.file.encoding = format == "16" ? ap::Encoding::Int16
        : format == "24" ? ap::Encoding::Int24 : ap::Encoding::Float32;
    spec.file.dither = format == "16";
    if (format == "mp3") {
        spec.file.container = ap::Container::Mp3;
        spec.file.encoding = ap::Encoding::Mp3;
        spec.file.bitrateKbps = 320;
    }
    const int block = std::stoi(argv[5]);
    require(block >= 0 && block <= 8192, "block size");
    spec.blockSize = unsigned(block);
    spec.pipeline = std::stoi(argv[6]) != 0;
    spec.baseName = std::string(argv[7]);
    if (argc > 8) {
        spec.range = daw::rendering::Range::Custom;
        spec.customEndSeconds = std::stod(argv[8]);
    }
    daw::rendering::Report report;
    const auto start = std::chrono::steady_clock::now();
    int lastPercent = -1;
    const auto result = controller.renderProject(spec, [&](const auto& progress) {
        const int percent = int(progress.fraction * 100);
        if (progress.stage == daw::rendering::Progress::Stage::Rendering && percent / 10 != lastPercent / 10) {
            std::cout << "render=" << percent << "%\n"; lastPercent = percent;
        }
        return true;
    }, report);
    require(bool(result) && !report.cancelled && report.files.size() == 1, "render: " + result.message());
    ap::DecodedAudio audio;
    const auto decoded = ap::decodeAudioFile(report.files.front(), audio);
    require(bool(decoded), decoded.message());
    double sum = 0, peak = 0;
    std::size_t nonfinite = 0;
    for (const float sample : audio.interleaved) {
        if (!std::isfinite(sample)) { ++nonfinite; continue; }
        sum += double(sample) * sample; peak = std::max(peak, std::abs(double(sample)));
    }
    std::cout << "RESULT file=" << report.files.front() << " frames=" << audio.frames
              << " pipeline=" << report.usedPipeline << " peak=" << peak
              << " rms=" << std::sqrt(sum / std::max(std::size_t{1}, audio.interleaved.size()))
              << " nonfinite=" << nonfinite << " elapsed_s="
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << '\n';
    require(nonfinite == 0, "non-finite output");
    return 0;
}

static int convertAudio(int argc, char** argv) {
    require(argc==5,"usage: --convert input output 16|24|mp3");
    require(!fs::exists(argv[3]),"output already exists");
    ap::DecodedAudio audio;
    const auto decoded=ap::decodeAudioFile(argv[2],audio);
    require(bool(decoded),decoded.message());
    ap::WriteSpec spec;
    const std::string format=argv[4];
    require(format=="16" || format=="24" || format=="mp3","conversion format");
    spec.encoding=format=="16"?ap::Encoding::Int16:ap::Encoding::Int24;
    spec.dither=format=="16";
    if(format=="mp3"){spec.container=ap::Container::Mp3;spec.encoding=ap::Encoding::Mp3;spec.bitrateKbps=320;}
    ap::AudioFileWriter writer;
    const auto opened=writer.open(argv[3],spec,audio.sampleRate,audio.channels,audio.frames);
    require(bool(opened),opened.message());
    std::vector<std::vector<float>> planar(audio.channels,std::vector<float>(4096));
    std::vector<const float*> pointers(audio.channels);
    for(std::uint64_t offset=0;offset<audio.frames;offset+=4096){
        const auto count=unsigned(std::min<std::uint64_t>(4096,audio.frames-offset));
        for(unsigned ch=0;ch<audio.channels;++ch){
            for(unsigned i=0;i<count;++i) planar[ch][i]=audio.interleaved[(offset+i)*audio.channels+ch];
            pointers[ch]=planar[ch].data();
        }
        require(bool(writer.write(pointers.data(),count)),"convert write");
    }
    require(bool(writer.close()),"convert close");
    std::cout<<"CONVERT file="<<argv[3]<<" frames="<<audio.frames<<'\n';
    return 0;
}

int main(int argc, char** argv) try {
    std::cout << std::unitbuf;
    if (argc > 1 && std::string(argv[1]) == "--project") return renderProject(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--convert") return convertAudio(argc, argv);
    const int tracks = argc > 1 ? std::max(1, std::atoi(argv[1])) : 32;
    const int inserts = argc > 2 ? std::max(0, std::atoi(argv[2])) : 3;
    const int seconds = argc > 3 ? std::max(1, std::atoi(argv[3])) : 8;
    const unsigned renderBlock = argc > 4 ? unsigned(std::max(0, std::atoi(argv[4]))) : 0;
    const bool internal = argc > 5 && std::string(argv[5]) == "internal";
    const fs::path temp = fs::temp_directory_path() / ("daw-render-bench-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{temp};
    const auto source = (temp / "source.wav").string();
    ap::WriteSpec format; format.encoding = ap::Encoding::Float32;
    ap::AudioFileWriter writer;
    std::vector<float> samples(48000 * seconds);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = float(0.01 * std::sin(2 * 3.141592653589793 * 375 * i / 48000));
    const float* audio[]{samples.data(), samples.data()};
    require(bool(writer.open(source, format, 48000, 2, samples.size())) &&
            bool(writer.write(audio, unsigned(samples.size()))) && bool(writer.close()), "write source");
    daw::plugins::ClapFactory factory;
    auto descriptors = internal ? daw::plugins::builtinPlugins() : factory.inspect(DAW_TEST_CLAP_PATH);
    if (internal) std::erase_if(descriptors, [](const auto& p) { return p.uid != "daw.compressor"; });
    require(!descriptors.empty(), "fixture");
    std::vector<float> reference;
    for (unsigned block : {128u, 512u, 1024u}) {
        daw::EngineController controller;
        require(bool(controller.initialize(48000, block, false)), "initialize");
        for (int i = 0; i < tracks; ++i) {
            const auto track = controller.addTrack(daw::TrackKind::Audio, "Track");
            require(!controller.importAudio(source, track, 0).empty(), "import");
            for (int j = 0; j < inserts; ++j)
                require(!controller.addInsert(track, descriptors.front()).empty(), "insert");
        }
        std::array<std::vector<double>, 2> times;
        for (int round = 0; round < 4; ++round) {
        // Alternate order and discard the first pair, so startup and CPU boost
        // do not systematically favour either renderer.
        for (const bool pipeline : std::array<bool, 2>{round % 2 == 0, round % 2 != 0}) {
            daw::rendering::Spec spec;
            spec.outputDir = temp.string(); spec.file = format;
            spec.blockSize = renderBlock;
            spec.pipeline = pipeline;
            daw::rendering::Report report;
            const auto start = std::chrono::steady_clock::now();
            const auto result = controller.renderProject(spec, {}, report);
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            require(bool(result) && report.files.size() == 1, result.message());
            if (round != 0) times[pipeline].push_back(elapsed);
            ap::DecodedAudio decoded;
            require(bool(ap::decodeAudioFile(report.files.front(), decoded)), "decode");
            if (reference.empty()) reference = decoded.interleaved;
            require(reference.size() == decoded.interleaved.size(), "length mismatch");
            double error = 0;
            for (std::size_t i = 0; i < reference.size(); ++i)
                error = std::max(error, double(std::abs(reference[i] - decoded.interleaved[i])));
            require(error < 1e-6, "audio mismatch across blocks");
            std::cout << "tracks=" << tracks << " inserts=" << tracks * inserts
                      << " seconds=" << seconds << " device_block=" << block
                      << " render_block=" << renderBlock
                      << " internal=" << internal << " pipeline=" << pipeline
                      << " used_pipeline=" << report.usedPipeline
                      << " round=" << round << " elapsed_s=" << elapsed
                      << " max_sample_error=" << error << '\n';
        }
        }
        for (const bool pipeline : {false, true}) {
        std::sort(times[pipeline].begin(), times[pipeline].end());
        std::cout << "MEDIAN device_block=" << block << " pipeline=" << pipeline
                  << " elapsed_s=" << times[pipeline][1] << '\n';
        }
    }
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
