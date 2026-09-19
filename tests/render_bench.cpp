// End-to-end export benchmark: tracks, inserts/track, seconds, optional render block (0 = auto).
#include "EngineController.hpp"
#include "Clap/ClapFactory.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <algorithm>
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
int main(int argc, char** argv) try {
    std::cout << std::unitbuf;
    const int tracks = argc > 1 ? std::max(1, std::atoi(argv[1])) : 32;
    const int inserts = argc > 2 ? std::max(0, std::atoi(argv[2])) : 3;
    const int seconds = argc > 3 ? std::max(1, std::atoi(argv[3])) : 8;
    const unsigned renderBlock = argc > 4 ? unsigned(std::max(0, std::atoi(argv[4]))) : 0;
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
    const auto descriptors = factory.inspect(DAW_TEST_CLAP_PATH);
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
        std::vector<double> times;
        for (int round = 0; round < 3; ++round) {
            daw::rendering::Spec spec;
            spec.outputDir = temp.string(); spec.file = format;
            spec.blockSize = renderBlock;
            daw::rendering::Report report;
            const auto start = std::chrono::steady_clock::now();
            const auto result = controller.renderProject(spec, {}, report);
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            require(bool(result) && report.files.size() == 1, result.message());
            times.push_back(elapsed);
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
                      << " round=" << round << " elapsed_s=" << elapsed
                      << " max_sample_error=" << error << '\n';
        }
        std::sort(times.begin(), times.end());
        std::cout << "MEDIAN device_block=" << block << " elapsed_s=" << times[1] << '\n';
    }
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
