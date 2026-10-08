#include "EngineController.hpp"
#include "RenderWorker.hpp"
#include "RenderWorkerProtocol.hpp"
#include "SharedProcess.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace wire = daw::rendering::ipc;

void require(bool value, const std::string& what) {
    if (!value) throw std::runtime_error(what);
    std::printf("PASS %s\n", what.c_str());
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes; out.close();
    if (!out) throw std::runtime_error("write fixture file");
}
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::set<fs::path> names(const fs::path& directory) {
    std::set<fs::path> out;
    for (const auto& entry : fs::directory_iterator(directory)) out.insert(entry.path().filename());
    return out;
}
struct TempDirectory {
    fs::path path = fs::temp_directory_path() / ("daw-render-worker-test-" +
        std::to_string(Clock::now().time_since_epoch().count()));
    TempDirectory() { fs::create_directories(path); }
    ~TempDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

// Test executable only: production daw_worker has no fault-mode switch. The
// parent supplies a normal immutable session and an alternate internal helper.
int peer(int argc, char** argv) {
    daw::plugins::ipc::SharedProcess process;
    std::string error;
    if (!process.attach(argc, argv, error) || process.size() != sizeof(wire::Mailbox)) return 2;
    auto& box = *reinterpret_cast<wire::Mailbox*>(process.data());
    if (box.magic != wire::kMagic || box.version != wire::kVersion ||
        box.requestSize == 0 || box.requestSize >= sizeof(box.request)) return 3;
    const auto root = daw::platform::pathFromUtf8(std::string(box.request, box.requestSize));
    const auto session = Json::parse(read(root / "session.json"));
    const auto mode = session.at("spec").at("baseName").get<std::string>();
    write(root / "output" / "mix.wav", mode == "crash" ? "incomplete" : "new mix");
    write(root / "output" / "stem.wav", "new stem");
    box.stage.store(std::uint32_t(daw::rendering::Progress::Stage::Rendering));
    box.fraction.store(.5); box.total.store(1); box.rendered.store(.5);
    box.revision.fetch_add(1, std::memory_order_release);
    if (mode == "crash") std::_Exit(71);
    if (mode == "ignore-cancel" || mode == "callback-throw")
        for (;;) std::this_thread::sleep_for(50ms);
    if (mode == "cancelled-reply" || mode == "cancel-races-success") {
        while (!box.cancel.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    }
    if (mode == "oversized") {
        box.replySize = sizeof(box.reply) + 1;
        box.done.store(1, std::memory_order_release);
        return 0;
    }
    Json files = Json::array({"mix.wav", "stem.wav"});
    if (mode == "missing-second") files = Json::array({"mix.wav", "missing.wav"});
    if (mode == "traversal") files = Json::array({"../mix.wav"});
    const auto text = Json{{"version", mode == "wrong-version" ? wire::kVersion + 1 : wire::kVersion},
        {"error", 0}, {"message", ""}, {"report", {{"files", files}, {"usedPipeline", false},
        {"cancelled", mode == "cancelled-reply"}, {"renderedSeconds", 1.0}}}}.dump();
    std::memcpy(box.reply, text.data(), text.size()); box.replySize = std::uint32_t(text.size());
    box.done.store(1, std::memory_order_release);
    return 0;
}
}

int main(int argc, char** argv) try {
    if (argc > 1) return peer(argc, argv);
    const auto self = daw::platform::pathToUtf8(fs::absolute(daw::platform::pathFromUtf8(argv[0])));
    TempDirectory temp;
    const auto oldMix = temp.path / "mix.wav", oldStem = temp.path / "stem.wav";
    write(oldMix, "previous completed mix"); write(oldStem, "previous completed stem");
    const auto baseline = names(temp.path);
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    require(bool(controller.initialize(48000, 128, false)), "initialize session capture without device");
    const auto capture = [&](const std::string& mode) {
        daw::rendering::Spec spec;
        spec.outputDir = daw::platform::pathToUtf8(temp.path); spec.baseName = mode;
        spec.range = daw::rendering::Range::Custom; spec.customEndSeconds = 1;
        daw::RenderSessionSpec session;
        if (!controller.captureRenderSession(spec, session)) throw std::runtime_error("capture protocol test session");
        return session;
    };
    const auto intact = [&] {
        return read(oldMix) == "previous completed mix" && read(oldStem) == "previous completed stem" &&
            names(temp.path) == baseline;
    };
    for (const std::string mode : {"crash", "oversized", "wrong-version", "traversal", "missing-second"}) {
        daw::rendering::Report report;
        const auto result = daw::RenderWorker::render(capture(mode), {}, report, self);
        require(!result && report.files.empty() && !report.cancelled && intact(),
            mode + ": failed worker leaves no output, partial file or job directory; old exports survive");
    }
    for (const std::string mode : {"ignore-cancel", "cancelled-reply", "cancel-races-success"}) {
        daw::rendering::Report report;
        bool reachedWorker = false;
        const auto start = Clock::now();
        const auto result = daw::RenderWorker::render(capture(mode), [&](const auto& progress) {
            if (progress.stage != daw::rendering::Progress::Stage::Rendering) return true;
            reachedWorker = true; return false;
        }, report, self);
        require(bool(result) && reachedWorker && report.cancelled && report.files.empty() && intact() &&
                Clock::now() - start < 10s,
            mode + ": cancellation is bounded and cannot publish a worker's completed or partial files");
    }
    {
        daw::rendering::Report report;
        const auto result = daw::RenderWorker::render(capture("callback-throw"), [](const auto& progress) {
            if (progress.stage == daw::rendering::Progress::Stage::Rendering)
                throw std::runtime_error("test progress callback failure");
            return true;
        }, report, self);
        require(!result && report.files.empty() && intact(), "callback exception reaps worker before resource cleanup");
    }
    {
        daw::rendering::Report report;
        const auto result = daw::RenderWorker::render(capture("success"), [](const auto&) { return false; }, report, self);
        require(bool(result) && report.cancelled && report.files.empty() && intact(),
                "preparation cancellation creates no worker outputs or job files");
    }
    {
        daw::rendering::Report report;
        const auto result = daw::RenderWorker::render(capture("success"), {}, report, self + ".missing");
        require(!result && report.files.empty() && intact(), "missing worker reports failure without local fallback");
    }
    {
        daw::rendering::Report report;
        const auto result = daw::RenderWorker::render(capture("success"), {}, report, self);
        require(bool(result) && !report.cancelled && report.files.size() == 2, "successful worker publishes complete output group");
        require(read(daw::platform::pathFromUtf8(report.files[0])) == "new mix" &&
                read(daw::platform::pathFromUtf8(report.files[1])) == "new stem" &&
                read(oldMix) == "previous completed mix" && read(oldStem) == "previous completed stem" &&
                names(temp.path).size() == baseline.size() + 2,
                "publication uses new unique names and preserves earlier export bytes");
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}
