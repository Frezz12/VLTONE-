#include "WorkerJob.hpp"
#include "RenderWorkerProtocol.hpp"
#include "SharedProcess.hpp"
#include "model/Document.hpp"
#include "platform/PathUtils.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <nlohmann/json.hpp>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace daw {
namespace {
using namespace rendering::ipc;
using Clock = std::chrono::steady_clock;
using plugins::ipc::SharedProcess;
using Json = nlohmann::json;
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
rendering::Progress snapshot(const Mailbox& box) {
    const auto stage = box.stage.load(std::memory_order_relaxed);
    require(stage <= std::uint32_t(rendering::Progress::Stage::Rendering), "invalid worker progress stage");
    return {rendering::Progress::Stage(stage), box.fraction.load(std::memory_order_relaxed),
        box.rendered.load(std::memory_order_relaxed), box.total.load(std::memory_order_relaxed)};
}
}

WorkerJob::WorkerJob(const std::filesystem::path& parent) {
    require(!parent.empty(), "worker directory is empty");
    const auto destination = std::filesystem::weakly_canonical(std::filesystem::absolute(parent));
    std::filesystem::create_directories(destination);
    root = destination / (".vlt-render-job-" + newUuid());
    require(std::filesystem::create_directory(root), "cannot create private worker directory");
    try { std::filesystem::create_directory(root / "output"); }
    catch (...) { std::error_code ignored; std::filesystem::remove(root, ignored); throw; }
}
WorkerJob::~WorkerJob() {
    // Only the directory created by this owner; never a path from a child reply.
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

audio::Result WorkerJob::run(const std::string& executable, const Progress& progress,
    Json& reply, bool& cancelled) {
    reply = {};
    try {
        require(std::filesystem::is_regular_file(platform::pathFromUtf8(executable)),
            "background worker is unavailable; reinstall VLTONE");
        // This scope always kills/reaps the child before our owner removes files.
        SharedProcess process;
        std::string error;
        require(process.create(sizeof(Mailbox), error), error);
        auto& box = *new (process.data()) Mailbox;
        const auto request = platform::pathToUtf8(root);
        require(request.size() < sizeof(box.request), "worker job path is too long");
        box.requestSize = std::uint32_t(request.size());
        std::memcpy(box.request, request.data(), request.size());
        require(process.launch(executable, error), error);
        auto changedAt = Clock::now(), cancelledAt = changedAt;
        std::uint32_t revision = 0;
        const auto deliver = [&](const rendering::Progress& value) {
            require(std::isfinite(value.fraction) && std::isfinite(value.renderedSeconds) &&
                std::isfinite(value.totalSeconds), "invalid worker progress values");
            if (!cancelled && progress && !progress(value)) {
                cancelled = true;
                cancelledAt = Clock::now();
                box.cancel.store(1, std::memory_order_release);
                process.signal();
            }
        };
        const auto drain = [&] {
            if (!box.queuedProgress.load(std::memory_order_acquire)) { deliver(snapshot(box)); return; }
            auto read = box.progressRead.load(std::memory_order_relaxed);
            const auto write = box.progressWrite.load(std::memory_order_acquire);
            require(write - read <= kProgressCapacity, "invalid worker progress queue");
            while (read != write) {
                const auto value = box.progress[read % kProgressCapacity];
                require(value.stage <= std::uint32_t(rendering::Progress::Stage::Rendering), "invalid worker progress record");
                deliver({rendering::Progress::Stage(value.stage), value.fraction, value.rendered, value.total});
                box.progressRead.store(++read, std::memory_order_release);
            }
            // Cancellation remains responsive even while a decoder produces no
            // progress. A quiet/blocked child must not silence the caller's poll.
            deliver(snapshot(box));
        };
        while (!box.done.load(std::memory_order_acquire)) {
            const auto now = Clock::now();
            if (const auto current = box.revision.load(std::memory_order_acquire); current != revision) {
                revision = current; changedAt = now;
            }
            drain();
            if (cancelled && now - cancelledAt >= std::chrono::seconds(2)) return audio::Result::ok();
            require(now - changedAt < std::chrono::seconds(60), "background worker stopped responding");
            if (!process.running()) {
                require(box.done.load(std::memory_order_acquire) != 0, "background worker exited unexpectedly");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        drain();
        require(box.replySize > 0 && box.replySize <= sizeof(box.reply), "invalid worker reply size");
        auto received = Json::parse(box.reply, box.reply + box.replySize);
        process.stop();
        require(received.at("version") == kVersion, "incompatible worker reply");
        cancelled |= received.value("cancelled", false);
        if (cancelled) return audio::Result::ok();
        const auto code = received.at("error").get<int>();
        require(code >= 0 && code <= int(audio::EngineError::FileWriteError), "invalid worker error");
        if (code) return audio::Result::fail(audio::EngineError(code), received.at("message").get<std::string>());
        reply = std::move(received);
        return audio::Result::ok();
    } catch (const std::exception& error) {
        return cancelled ? audio::Result::ok() : audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}

int WorkerJob::serve(int argc, char** argv, const Handler& handler) {
    SharedProcess process;
    std::string error;
    if (!process.attach(argc, argv, error) || process.size() != sizeof(Mailbox)) return 2;
    auto& box = *reinterpret_cast<Mailbox*>(process.data());
    if (box.magic != kMagic || box.version != kVersion || box.bytes != sizeof(Mailbox) ||
        !box.requestSize || box.requestSize >= sizeof(box.request)) return 3;
    // Offline decoding/model work yields scheduling priority to live audio.
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
    (void)setpriority(PRIO_PROCESS, 0, 5);
#endif
    Json reply = Json::object();
    auto result = audio::Result::ok();
    try {
        const auto root = platform::pathFromUtf8(std::string(box.request, box.requestSize));
        require(root.is_absolute() && std::filesystem::is_directory(root) &&
            !std::filesystem::is_symlink(std::filesystem::symlink_status(root)) &&
            platform::pathToUtf8(root.filename()).starts_with(".vlt-render-job-"), "invalid worker job directory");
        box.queuedProgress.store(1, std::memory_order_release);
        auto lastPreparing = Clock::time_point{};
        const auto progress = [&](const rendering::Progress& p) {
            box.stage.store(std::uint32_t(p.stage), std::memory_order_relaxed);
            box.fraction.store(p.fraction, std::memory_order_relaxed);
            box.rendered.store(p.renderedSeconds, std::memory_order_relaxed);
            box.total.store(p.totalSeconds, std::memory_order_relaxed);
            box.revision.fetch_add(1, std::memory_order_release);
            if (box.cancel.load(std::memory_order_acquire)) return false;
            const auto now = Clock::now();
            if (p.stage == rendering::Progress::Stage::Preparing) {
                if (now - lastPreparing < std::chrono::milliseconds(33)) return true;
                lastPreparing = now;
            }
            const auto write = box.progressWrite.load(std::memory_order_relaxed);
            while (write - box.progressRead.load(std::memory_order_acquire) >= kProgressCapacity) {
                if (box.cancel.load(std::memory_order_acquire)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            box.progress[write % kProgressCapacity] = {std::uint32_t(p.stage), p.fraction, p.renderedSeconds, p.totalSeconds};
            box.progressWrite.store(write + 1, std::memory_order_release);
            return !box.cancel.load(std::memory_order_acquire);
        };
        reply = handler(root, progress);
        require(reply.is_object(), "invalid worker result");
    } catch (const std::exception& e) {
        result = audio::Result::fail(audio::EngineError::Unknown, e.what());
    } catch (...) { result = audio::Result::fail(audio::EngineError::Unknown, "background worker failed"); }
    reply["version"] = kVersion;
    if (!result || !reply.contains("error")) {
        reply["error"] = int(result.error());
        reply["message"] = result.message();
    }
    reply["cancelled"] = box.cancel.load(std::memory_order_acquire) != 0;
    auto bytes = reply.dump();
    if (bytes.size() > sizeof(box.reply)) bytes = Json{{"version", kVersion},
        {"error", int(audio::EngineError::OutOfMemory)}, {"message", "worker reply exceeds size limit"}}.dump();
    std::memcpy(box.reply, bytes.data(), bytes.size());
    box.replySize = std::uint32_t(bytes.size());
    box.done.store(1, std::memory_order_release);
    process.signal();
    return 0;
}
} // namespace daw
