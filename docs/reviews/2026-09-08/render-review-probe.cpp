// Diagnostic reproductions, deliberately outside the normal passing test suite.
#include "EngineController.hpp"
#include "Clap/ClapFactory.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <cmath>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace ap = audio::platform;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void tone(const fs::path& path, double frequency) {
    ap::AudioFileWriter writer;
    ap::WriteSpec spec;
    spec.container = ap::Container::Wav; spec.encoding = ap::Encoding::Float32;
    require(bool(writer.open(path.string(), spec, 48000, 2, 48000)), "open source");
    std::vector<float> data(48000);
    for (unsigned i = 0; i < data.size(); ++i)
        data[i] = float(0.2 * std::sin(2 * 3.141592653589793 * frequency * i / 48000));
    const float* channels[]{data.data(), data.data()};
    require(bool(writer.write(channels, data.size())) && bool(writer.close()), "write source");
}
double rms(const ap::DecodedAudio& audio, unsigned begin = 4096, unsigned end = 45000) {
    double energy = 0; unsigned count = 0;
    for (unsigned i = begin; i < end && i < audio.frames; ++i) {
        double value = audio.interleaved[i * audio.channels];
        energy += value * value; ++count;
    }
    return count ? std::sqrt(energy / count) : 0;
}
ap::DecodedAudio decode(const std::string& path) {
    ap::DecodedAudio result;
    require(bool(ap::decodeAudioFile(path, result)), "decode output");
    return result;
}
int main(int argc, char** argv) try {
    require(argc == 4, "usage: probe custom-plugin existing-test-plugin output-directory");
    const fs::path dir = argv[3]; fs::create_directories(dir);
    tone(dir / "source.wav", 1000);
    tone(dir / "stem-source.wav", 375);
    daw::plugins::ClapFactory factory;
    const auto plugins = factory.inspect(argv[1]);
    require(plugins.size() == 4, "inspect review fixture");
    for (const auto& plugin : plugins) {
        daw::EngineController c;
        require(bool(c.initialize(48000, 64, false)), "initialize controller");
        const auto a = c.addTrack(daw::TrackKind::Audio, "A");
        require(!c.importAudio((dir / "source.wav").string(), a, 0).empty(), "import");
        const auto slot = c.addInsert(a, plugin);
        require(!slot.empty(), "load review plugin");
        if (plugin.uid == "review.latency" || plugin.uid == "review.process") {
            const auto b = c.addTrack(daw::TrackKind::Audio, "Dry parallel");
            require(!c.importAudio((dir / "source.wav").string(), b, 0).empty(), "import parallel");
        }
        daw::rendering::Spec spec;
        spec.outputDir = dir.string(); spec.baseName = plugin.uid;
        spec.file.encoding = ap::Encoding::Float32;
        daw::rendering::Report report;
        unsigned progressCalls = 0;
        const auto started = std::chrono::steady_clock::now();
        const auto result = c.renderProject(spec, [&](const auto&) { ++progressCalls; return true; }, report);
        require(bool(result) && report.files.size() == 1, "export completes (observed defect requires success)");
        const auto audio = decode(report.files.front());
        std::cout << plugin.uid << " success=1 rms=" << rms(audio)
                  << " early_rms=" << rms(audio, 512, 4000)
                  << " later_rms=" << rms(audio, 5000, 8000)
                  << " progress_calls=" << progressCalls
                  << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started).count() << '\n';
    }
    // Real existing fixture: dry and delayed stems must sum to the same mix.
    {
        auto fixture = factory.inspect(argv[2]);
        const auto found = std::find_if(fixture.begin(), fixture.end(), [](const auto& p) {
            return p.uid == "com.daw.test.gain";
        });
        require(found != fixture.end(), "existing gain fixture");
        daw::EngineController c;
        require(bool(c.initialize(48000, 64, false)), "initialize stems");
        const auto a = c.addTrack(daw::TrackKind::Audio, "Delayed");
        const auto b = c.addTrack(daw::TrackKind::Audio, "Dry");
        for (const auto& track : {a, b})
            require(!c.importAudio((dir / "stem-source.wav").string(), track, 0).empty(), "stem source");
        require(!c.addInsert(a, *found).empty(), "stem insert");
        daw::rendering::Spec spec;
        spec.outputDir = dir.string(); spec.baseName = "stems";
        spec.file.encoding = ap::Encoding::Float32; spec.stemChannelIds = {a, b};
        daw::rendering::Report report;
        require(bool(c.renderProject(spec, {}, report)) && report.files.size() == 3, "stem render");
        const auto mix = decode(report.files[0]);
        auto sum = decode(report.files[1]); const auto second = decode(report.files[2]);
        require(sum.interleaved.size() == second.interleaved.size(), "stem sizes");
        for (unsigned i = 0; i < sum.interleaved.size(); ++i) sum.interleaved[i] += second.interleaved[i];
        std::cout << "stems mix_rms=" << rms(mix) << " sum_stems_rms=" << rms(sum) << '\n';
    }
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
