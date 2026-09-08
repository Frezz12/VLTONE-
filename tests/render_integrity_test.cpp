#include "EngineController.hpp"
#include "Clap/ClapFactory.hpp"
#include "Host/PluginNode.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace ap = audio::platform;
namespace {
struct SineNode final : daw::engine::Node {
    std::string_view name() const noexcept override { return "Sine"; }
    bool isSource() const noexcept override { return true; }
    void process(const daw::engine::ProcessContext& context) override {
        for (unsigned i = 0; i < context.frames; ++i)
            for (unsigned ch = 0; ch < context.output.numChannels(); ++ch)
                context.output.data(ch)[i] = float(0.2 * std::sin(
                    2 * 3.141592653589793 * 1000 * (context.timelinePosition + i) / 48000));
    }
};
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
struct TempDirectory {
    fs::path path = fs::temp_directory_path() /
        ("daw-render-integrity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { fs::create_directories(path); }
    ~TempDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};
void writeTone(const fs::path& path, double frequency) {
    ap::AudioFileWriter writer;
    ap::WriteSpec spec;
    spec.encoding = ap::Encoding::Float32;
    require(bool(writer.open(path.string(), spec, 48000, 2, 48000)), "open source");
    std::vector<float> data(48000);
    for (unsigned i = 0; i < data.size(); ++i)
        data[i] = float(0.2 * std::sin(2 * 3.141592653589793 * frequency * i / 48000));
    const float* channels[]{data.data(), data.data()};
    require(bool(writer.write(channels, data.size())) && bool(writer.close()), "write source");
}
ap::DecodedAudio decode(const std::string& path) {
    ap::DecodedAudio audio;
    require(bool(ap::decodeAudioFile(path, audio)), "decode " + path);
    return audio;
}
double rms(const ap::DecodedAudio& audio) {
    double energy = 0;
    for (float sample : audio.interleaved) energy += double(sample) * sample;
    return std::sqrt(energy / audio.interleaved.size());
}
double difference(const ap::DecodedAudio& a, const ap::DecodedAudio& b) {
    require(a.channels == b.channels && a.frames == b.frames, "audio lengths and channels match");
    double worst = 0;
    for (std::size_t i = 0; i < a.interleaved.size(); ++i)
        worst = std::max(worst, std::abs(double(a.interleaved[i]) - b.interleaved[i]));
    return worst;
}
daw::plugins::PluginDescriptor plugin(const char* path, const std::string& uid) {
    daw::plugins::ClapFactory factory;
    const auto catalog = factory.inspect(path);
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& p) { return p.uid == uid; });
    require(found != catalog.end(), "inspect " + uid);
    return *found;
}
std::string track(daw::EngineController& c, const fs::path& source, const char* name) {
    const auto id = c.addTrack(daw::TrackKind::Audio, name);
    require(!id.empty() && !c.importAudio(source.string(), id, 0).empty(), "import track");
    return id;
}
daw::rendering::Spec specFor(const fs::path& directory) {
    daw::rendering::Spec spec;
    spec.outputDir = directory.string(); spec.file.encoding = ap::Encoding::Float32;
    return spec;
}
ap::DecodedAudio render(daw::EngineController& c, const daw::rendering::Spec& spec) {
    daw::rendering::Report report;
    const auto result = c.renderProject(spec, {}, report);
    require(bool(result) && report.files.size() == 1, "render: " + result.message());
    return decode(report.files.front());
}
}

int main() try {
    TempDirectory temp;
    const auto source = temp.path / "tone.wav";
    const auto stemSource = temp.path / "stem-tone.wav";
    writeTone(source, 1000); writeTone(stemSource, 375);

    // The shared engine API also switches back to a correct realtime graph,
    // including after a failed activation. No controller clone masks this.
    for (const auto* uid : {"review.latency", "review.activation"}) {
        daw::engine::RealtimeEngine engine(2);
        require(bool(engine.prepare(48000, 64, 2)), "prepare shared engine");
        auto& graph = engine.graph();
        const auto input = graph.addNode(std::make_unique<SineNode>());
        daw::plugins::ClapFactory factory;
        auto node = std::make_shared<daw::plugins::PluginNode>(uid,
            factory.create(plugin(DAW_TEST_RENDER_CLAP_PATH, uid)));
        const auto effect = graph.adoptNode(node);
        const auto sum = graph.addNode(std::make_unique<daw::engine::SumNode>());
        require(bool(graph.connect(input, effect)) && bool(graph.connect(input, sum)) &&
                bool(graph.connect(effect, sum)), "connect shared graph");
        graph.setSink(sum);
        require(bool(engine.commitGraph()) && engine.latencySamples() == 0, "initial realtime latency");
        std::vector<float> output;
        const auto status = engine.renderOffline(0, 104, 64, [&](const auto& block, auto frames) {
            output.insert(output.end(), block.data(0), block.data(0) + frames); return true;
        });
        if (std::string(uid) == "review.latency") {
            require(bool(status) && output.size() == 104, "shared offline pass succeeds");
            for (unsigned i = 0; i < output.size(); ++i) {
                const double expected = i < 24 ? 0 : 0.4 * std::sin(2 * 3.141592653589793 * 1000 * (i - 24) / 48000);
                require(std::abs(output[i] - expected) < 1e-6, "shared offline PDC is accurate");
            }
        } else {
            require(!status && output.empty() && !engine.offlineError().empty(), "no sink call on activation error");
        }
        require(engine.latencySamples() == 0 && node->isReady(), "restores realtime activation and compensation");
        float left[64]{}, right[64]{}; float* channels[]{left, right};
        engine.renderBlock(daw::engine::AudioBlock(channels, 2, 64), nullptr, 0, 64);
        const double amplitude = std::string(uid) == "review.activation" ? 0.3 : 0.4;
        require(std::abs(left[12] - amplitude) < 1e-6, "live renderer resumes with correct sound");
    }

    // Mode changes and a deferred callback/restart must settle before PDC,
    // pre-roll and output lengths are calculated. Compare every file sample.
    for (const auto* uid : {"review.latency", "review.prepare", "review.callback"}) {
        daw::EngineController c;
        require(bool(c.initialize(48000, 64, false)), "initialize");
        const auto a = track(c, source, "Effect");
        track(c, source, "Parallel");
        auto spec = specFor(temp.path / uid);
        const auto reference = render(c, spec);
        const auto slot = c.addInsert(a, plugin(DAW_TEST_RENDER_CLAP_PATH, uid));
        require(!slot.empty(), "create fixture");
        const auto actual = render(c, spec);
        require(difference(actual, reference) < 1e-6, std::string(uid) + " matches reference sample for sample");
        require(c.insertInstance(a, slot)->isActive(), "original live processor remains active");
        std::cout << "PASS " << uid << " matches unprocessed parallel reference\n";
    }

    // A successful file is impossible after DSP/configuration failure. The
    // same live controller can immediately retry with an explicit FX bypass.
    for (const auto* uid : {"review.activation", "review.process", "review.clone", "review.dual",
                            "review.restart", "review.mode", "review.hardware", "review.unstable"}) {
        daw::EngineController c;
        require(bool(c.initialize(48000, 64, false)), "initialize failure case");
        const auto a = track(c, source, "Failure source");
        // Include a wide graph: the failure must cross a worker-thread boundary.
        for (int i = 0; i < 8; ++i) c.addTrack(daw::TrackKind::Audio, "Empty");
        const auto slot = c.addInsert(a, plugin(DAW_TEST_RENDER_CLAP_PATH, uid));
        require(!slot.empty(), "create failure fixture");
        if (std::string(uid) == "review.dual")
            require(c.setInsertChannelMode(a, slot, daw::PluginChannelMode::DualMono), "dual mono live instances load");
        auto spec = specFor(temp.path / uid);
        fs::create_directories(spec.outputDir);
        const auto previous = fs::path(spec.outputDir) / "mixdown.wav";
        { std::ofstream file(previous); file << "previous-export"; }
        daw::rendering::Report report;
        const auto failed = c.renderProject(spec, {}, report);
        require(!failed && report.files.empty() && !report.cancelled, std::string(uid) + " must fail explicitly");
        require(!failed.message().empty(), "failure has a diagnostic");
        require(std::distance(fs::directory_iterator(spec.outputDir), fs::directory_iterator{}) == 1,
                "no published or partial output after failure");
        { std::ifstream file(previous); std::string text; file >> text;
          require(text == "previous-export", "previous output is preserved"); }
        require(!c.offlineRenderInProgress() && c.insertInstance(a, slot)->isActive(), "failure releases render and keeps live instance");
        spec.bypassChannelInserts = true;
        const auto dry = render(c, spec);
        require(difference(dry, decode(source.string())) < 1e-6, "explicit bypass exports the dry source");
        std::cout << "PASS " << uid << " rejects corrupt export and permits bypass retry\n";
    }

    // Different track latency plus additional master latency. Custom windows,
    // pre-roll, partial blocks, mono and stereo all retain the same origin.
    for (unsigned block : {8u, 64u, 512u}) {
        daw::EngineController c;
        require(bool(c.initialize(48000, block, false)), "initialize stems");
        const auto a = track(c, stemSource, "Delayed");
        const auto b = track(c, stemSource, "Dry");
        const auto gain = plugin(DAW_TEST_CLAP_PATH, "com.daw.test.gain");
        require(!c.addInsert(a, gain).empty(), "track delay");
        require(!c.addInsert(daw::EngineController::kMasterChannelId, gain).empty(), "master delay");
        auto spec = specFor(temp.path / ("stems-" + std::to_string(block)));
        spec.range = daw::rendering::Range::Custom;
        spec.customStartSeconds = 0.023; spec.customEndSeconds = 0.6813;
        spec.preRollSeconds = 0.015;
        if (block == 512) spec.channels = daw::rendering::Channels::Mono;
        const auto reference = render(c, spec);
        spec.stemChannelIds = {a, b};
        daw::rendering::Report report;
        require(bool(c.renderProject(spec, {}, report)) && report.files.size() == 3, "export three files");
        const auto mix = decode(report.files[0]);
        auto sum = decode(report.files[1]); const auto other = decode(report.files[2]);
        require(sum.frames == other.frames && sum.channels == other.channels, "stems agree on length");
        for (std::size_t i = 0; i < sum.interleaved.size(); ++i) sum.interleaved[i] += other.interleaved[i];
        require(difference(mix, reference) < 1e-6, "adding taps cannot change mixdown");
        require(difference(sum, mix) < 1e-6 && rms(sum) > 0.2, "stems sum to audible mixdown including both boundaries");
        std::cout << "PASS stems align at block " << block << '\n';
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n'; return 1;
}
