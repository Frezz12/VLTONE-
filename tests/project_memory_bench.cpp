// Read-only project load/play/close probe. No audio device or project writes.
// project_memory_bench package [cycles=3] [all|none|plugin name|!excluded name] [seconds=2]
// project_memory_bench --catalog input.json output.json
#include "EngineController.hpp"
#include "plugins/PluginCache.hpp"
#include "Core/AudioBuffer.hpp"
#include "Memory/PcmReadCache.hpp"
#include "platform/PathUtils.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#endif

namespace {
#ifdef _WIN32
LONG WINAPI captureCrash(EXCEPTION_POINTERS* exception) {
    wchar_t path[32768];
    const auto length = GetEnvironmentVariableW(L"DAW_MEMORY_CRASH_DUMP", path, DWORD(std::size(path)));
    if (length && length < std::size(path)) {
        const auto file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &info, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void pump(daw::EngineController& controller) {
#ifdef _WIN32
    MSG message;
    for (unsigned n = 0; n < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
        if (message.message == WM_QUIT) { PostQuitMessage(int(message.wParam)); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
#endif
    controller.pumpPluginEvents();
}
void memory(const char* phase, unsigned cycle) {
    std::cout << "MEMORY cycle=" << cycle << " phase=" << phase;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX usage{};
    usage.cb = sizeof(usage);
    require(GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&usage), sizeof(usage)) != 0, "memory counters");
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    std::cout << " resident_mib=" << double(usage.WorkingSetSize) / 1048576
        << " private_mib=" << double(usage.PrivateUsage) / 1048576
        << " peak_resident_mib=" << double(usage.PeakWorkingSetSize) / 1048576
        << " handles=" << handles;
#endif
    if (const auto* cache = daw::engine::PcmReadCache::existing()) {
        const auto counters = cache->counters();
        std::cout << " pcm_capacity_mib=" << double(counters.capacityBytes) / 1048576
            << " pcm_allocated_mib=" << double(counters.allocatedBytes) / 1048576
            << " pcm_misses=" << counters.misses;
    }
    std::cout << '\n';
}
}
int main(int argc, char** argv) try {
    std::cout << std::unitbuf;
#ifdef _WIN32
    if (GetEnvironmentVariableW(L"DAW_MEMORY_CRASH_DUMP", nullptr, 0))
        SetUnhandledExceptionFilter(captureCrash);
#endif
    if (argc == 4 && std::string(argv[1]) == "--catalog") {
        memory("start", 0);
        daw::PluginCache cache;
        require(cache.load(argv[2]), "load catalogue");
        memory("catalog_loaded", 0);
        std::vector<std::vector<daw::plugins::PluginDescriptor>> views;
        for (unsigned i = 0; i < 8; ++i) views.push_back(cache.allPlugins());
        std::cout << "CATALOG plugins=" << views.front().size() << " copies=" << views.size() << '\n';
        memory("catalog_copied", 0);
        require(cache.save(argv[3]), "save catalogue");
        memory("catalog_saved", 0);
        return 0;
    }
    require(argc >= 2, "package [cycles=3] [all|none|plugin name] [seconds=2], or --catalog input.json output.json");
    auto path = daw::platform::pathFromUtf8(argv[1]);
    if (std::filesystem::is_regular_file(path)) path = path.parent_path();
    const unsigned cycles = argc > 2 ? std::stoul(argv[2]) : 3;
    const std::string filter = argc > 3 ? argv[3] : "all";
    const double seconds = argc > 4 ? std::stod(argv[4]) : 2;
    require(cycles > 0 && cycles <= 100 && std::isfinite(seconds) && seconds >= 0 && seconds <= 30,
            "invalid cycle/duration");
    memory("start", 0);
    {
        daw::EngineController controller;
        memory("constructed", 0);
        require(bool(controller.initialize(44100, 256, false)), "initialize");
        controller.setPluginLoadProgress([](std::string_view name, std::size_t done, std::size_t total) {
            if (!name.empty()) std::cout << "PLUGIN loading=" << name << " completed=" << done << " total=" << total << '\n';
        });
        memory("initialized", 0);
        for (unsigned cycle = 1; cycle <= cycles; ++cycle) {
            daw::EngineController::PreparedProject prepared;
            require(bool(daw::EngineController::prepareProjectOpen(
                daw::platform::pathToUtf8(path), 44100, prepared)), "prepare project");
            std::unordered_set<const daw::engine::SampleBuffer*> samples;
            std::vector<std::weak_ptr<const daw::engine::SampleBuffer>> observed;
            std::size_t sampleBytes = 0;
            for (const auto& audio : prepared.audio) {
                for (const auto& buffer : {audio.source, audio.playback}) {
                    if (!buffer || !samples.insert(buffer.get()).second) continue;
                    sampleBytes += std::size_t(buffer->channels()) * buffer->frames() * sizeof(float);
                    observed.push_back(buffer);
                }
            }
            std::cout << "MEDIA cycle=" << cycle << " files=" << prepared.audio.size()
                << " missing=" << prepared.failedPaths.size() << " buffers=" << samples.size()
                << " pcm_mib=" << double(sampleBytes) / 1048576 << '\n';
            require(prepared.failedPaths.empty(), "missing project media");
            memory("decoded", cycle);
            const auto keep = [&](const daw::InsertModel& slot) {
                return filter == "all" || slot.name == filter ||
                    (filter.starts_with('!') && slot.name != filter.substr(1));
            };
            const auto prune = [&](auto& slots) { std::erase_if(slots, [&](const auto& slot) { return !keep(slot); }); };
            prune(prepared.document.masterInserts);
            prune(prepared.document.masterMiniModules);
            for (auto& track : prepared.document.tracks) {
                if (!keep(track.instrument)) track.instrument = {};
                prune(track.inserts); prune(track.samplerFx.inserts); prune(track.miniModules);
                for (auto& clip : track.clips) prune(clip.inserts);
            }
            require(bool(controller.openPreparedProject(std::move(prepared))), "activate project");
            memory("opened", cycle);
            audio::AudioBuffer input(2, 256), output(2, 256);
            input.clear();
            controller.seekSeconds(23); controller.play();
            const auto blocks = unsigned(std::ceil(seconds * 44100 / 256));
            double squares = 0;
            for (unsigned i = 0; i < blocks; ++i) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(5805);
                require(controller.processDeviceBlockForTest(input, output, 256), "audio callback");
                for (unsigned ch = 0; ch < 2; ++ch) for (unsigned n = 0; n < 256; ++n) {
                    const auto value = output.getChannel(ch)[n];
                    require(std::isfinite(value), "nonfinite audio");
                    squares += double(value) * value;
                }
                if (i % 3 == 0) pump(controller);
                std::this_thread::sleep_until(until);
            }
            controller.stop(); pump(controller);
            std::cout << "AUDIO cycle=" << cycle << " rms="
                << (blocks ? std::sqrt(squares / (double(blocks) * 512)) : 0) << '\n';
            memory("played", cycle);
            require(bool(controller.newProject()), "close project");
            for (unsigned i = 0; i < 10; ++i) {
                pump(controller); std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            memory("closed", cycle);
            std::size_t retained = 0;
            for (const auto& sample : observed) retained += !sample.expired();
            std::cout << "LIFETIME cycle=" << cycle << " retained_samples=" << retained << '\n';
            require(retained == 0, "closed project retains decoded samples");
        }
        controller.shutdown();
        memory("shutdown", 0);
    }
    memory("destroyed", 0);
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
