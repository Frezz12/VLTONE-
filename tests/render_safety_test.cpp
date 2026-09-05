#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "SampleLoader.hpp"
#include "UndoStack.hpp"
#include "Engine/RealtimeEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace en = daw::engine;
namespace ap = audio::platform;
static int failures = 0;
static void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Source final : en::Node {
    bool failOffline = false;
    int blocks = 0;
    std::string_view name() const noexcept override { return "source"; }
    bool isSource() const noexcept override { return true; }
    void prepare(const en::PrepareInfo& info) override {
        if (failOffline && info.offline) throw std::runtime_error("prepare failed");
    }
    void process(const en::ProcessContext& context) override {
        ++blocks;
        for (en::ChannelCount ch = 0; ch < context.output.numChannels(); ++ch)
            std::fill_n(context.output.data(ch), context.frames, 0.25f);
    }
};
static std::string bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
int main() {
    const fs::path dir = fs::temp_directory_path() / "daw-render-safety-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        en::RealtimeEngine engine(1);
        check(bool(engine.prepare(48000, 64, 2)), "engine prepares");
        auto source = std::make_unique<Source>();
        auto* counter = source.get();
        engine.graph().setSink(engine.graph().addNode(std::move(source)));
        check(bool(engine.commitGraph()), "graph compiles");
        float left[64]{}, right[64]{};
        float* channels[] = {left, right};
        const en::AudioBlock block(channels, 2, 64);
        const auto live = [&] { engine.renderBlock(block, nullptr, 0, 64); };
        live();
        const int before = counter->blocks;
        {
            en::RealtimeEngine::RenderGate outer(engine);
            { en::RealtimeEngine::RenderGate inner(engine); live(); }
            live();
            check(counter->blocks == before && left[0] == 0.0f,
                  "destroying an inner gate keeps the live renderer parked");
        }
        live();
        check(counter->blocks == before + 1 && left[0] == 0.25f,
              "outer gate reopens the live renderer");
        bool refused = false;
        check(bool(engine.renderOffline(0, 128, 64, [&](const auto&, auto) {
            refused = !engine.commitGraph() && !engine.prepare(96000, 64, 2);
            en::RealtimeEngine::RenderGate nested(engine);
            live();
            check(left[0] == 0, "nested offline callback cannot admit live DSP");
            return false;
        })), "offline pass completes");
        check(refused && engine.sampleRate() == 48000, "reentrant reconfiguration is refused");
        counter->failOffline = true;
        bool threw = false;
        try { engine.renderOffline(0, 128, 64, [](const auto&, auto) { return true; }); }
        catch (const std::exception&) { threw = true; }
        counter->failOffline = false;
        live();
        check(threw && left[0] == 0.25f, "offline prepare exceptions restore live DSP and release the gate");
    }
    const auto sourcePath = (dir / "source.wav").string();
    {
        ap::AudioFileWriter writer;
        check(bool(writer.open(sourcePath, 48000, 2, 192000)), "fixture opens");
        float data[1024];
        std::fill_n(data, 1024, 0.25f);
        const float* channels[] = {data, data};
        for (int i = 0; i < 188; ++i) check(bool(writer.write(channels, 1024)), "fixture writes");
        check(bool(writer.close()), "fixture closes");
        std::shared_ptr<const en::SampleBuffer> sample;
        check(bool(daw::loadSampleBuffer(sourcePath, sample)) && sample && sample->fileBacked(),
              "large PCM uses disk backing");
        check(sample && sample->channel(1)[sample->frames() - 1] == 0.25f,
              "block decoding preserves the final sample");
        std::shared_ptr<const en::SampleBuffer> shared;
        check(bool(daw::loadSampleBuffer(sourcePath, shared)) && shared == sample,
              "independent controllers can share an immutable decoded source");
        int calls = 0;
        std::shared_ptr<const en::SampleBuffer> cancelled;
        const auto uncachedPath = (dir / "uncached.wav").string();
        fs::copy_file(sourcePath, uncachedPath);
        check(!daw::loadSampleBuffer(uncachedPath, cancelled, [&] { return ++calls < 4; }) && !cancelled,
              "large decode can be cancelled between blocks");
        ap::DecodedAudio decoded;
        decoded.frames = 17;
        check(!ap::decodeAudioFile(sourcePath, decoded, ap::DecodeOptions{32, {}}) && decoded.frames == 17,
              "decoder rejects allocations over budget without changing output");
    }
    {
        daw::EngineController controller;
        check(bool(controller.initialize(48000, 64, false)), "controller initializes");
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Source");
        check(!controller.importAudio(sourcePath, track, 0).empty(), "source imports");
        daw::rendering::Spec spec;
        spec.outputDir = (dir / "renders").string();
        spec.baseName = "mix";
        daw::rendering::Report first;
        check(bool(controller.renderProject(spec, {}, first)) && first.files.size() == 1,
              "first export completes");
        if (!first.files.empty()) {
            const auto original = bytes(first.files[0]);
            daw::rendering::Report cancelled;
            check(bool(controller.renderProject(spec, [](const auto& p) {
                return p.stage == daw::rendering::Progress::Stage::Preparing;
            }, cancelled)) && cancelled.cancelled,
                  "cancel is honored after the DSP pass starts");
            check(bytes(first.files[0]) == original && !original.empty(), "cancel preserves earlier output bytes");
        }
        bool edited = false;
        daw::rendering::Report isolated;
        check(bool(controller.renderProject(spec, [&](const auto& p) {
            if (!edited && p.stage == daw::rendering::Progress::Stage::Rendering) {
                edited = true;
                controller.removeTrack(track);
            }
            return true;
        }, isolated)) && edited && isolated.files.size() == 1,
              "live document edits during progress cannot change the offline graph");
        if (!isolated.files.empty()) {
            ap::DecodedAudio output;
            check(bool(ap::decodeAudioFile(isolated.files[0], output)) &&
                  output.interleaved[output.interleaved.size() / 2] > 0.1f,
                  "isolated export retains the removed source throughout its pass");
        }
        check(!controller.project().findTrack(track), "render never restores over a live document edit");
        const auto newTrack = controller.addTrack(daw::TrackKind::Audio, "After");
        controller.importAudio(sourcePath, newTrack, 0);
        daw::rendering::Report failed;
        check(!controller.renderProject(spec, [](const auto& p) {
            if (p.stage == daw::rendering::Progress::Stage::Rendering)
                throw std::runtime_error("progress failed");
            return true;
        }, failed) && !controller.offlineRenderInProgress(), "progress exceptions return an error and release the controller");
        for (const auto& file : fs::directory_iterator(spec.outputDir))
            check(file.path().string().find(".partial-") == std::string::npos,
                  "failed and cancelled renders remove their staging files");
    }
    {
        daw::ProjectModel project, loaded;
        project.name = "case regression";
        for (const auto* name : {"project.vlt", "Project.vlt", "session.vlt"}) {
            const auto package = (dir / name).string();
            check(bool(daw::ProjectSerializer::save(project, package)) &&
                  bool(daw::ProjectSerializer::load(loaded, package)) && loaded.name == project.name,
                  "saving a case-alias package keeps its manifest");
        }
        const auto malformed = (dir / "malformed.json").string();
        { std::ofstream file(malformed); file << R"({"format":7})"; }
        check(!daw::ProjectSerializer::loadDocument(loaded, malformed, ""),
              "non-string format returns a failure instead of throwing");
    }
    {
        daw::UndoStack history(100, 1000);
        int value = 0;
        for (int i = 0; i < 5; ++i) history.push("snapshot", [&] { --value; }, [&] { ++value; }, 600);
        check(history.depth() == 1 && history.estimatedBytes() == 600, "history evicts by retained bytes");
        const auto group = history.beginGroup();
        history.push("a", [] {}, [] {}, 400);
        history.push("b", [] {}, [] {}, 400);
        history.collapseGroup(group, "group");
        check(history.depth() == 1 && history.estimatedBytes() == 800, "grouped snapshots retain their byte cost");
        history.undo(); history.redo();
        check(history.canUndo() && !history.canRedo(), "budgeted history still undoes and redoes");
        const auto revision = history.revision();
        history.clear();
        check(history.revision() != revision, "document history resets invalidate in-flight jobs");
    }
    {
        std::vector<std::shared_ptr<en::SampleBuffer>> shortSamples;
        for (int i = 0; i < 120; ++i)
            shortSamples.push_back(std::make_shared<en::SampleBuffer>(1, 160000, 48000));
        check(shortSamples.back()->fileBacked(),
              "many short samples also obey the shared heap budget");
    }
    fs::remove_all(dir);
    std::cout << (failures ? "FAILURES PRESENT\n" : "ALL PASSED\n");
    return failures ? 1 : 0;
}
