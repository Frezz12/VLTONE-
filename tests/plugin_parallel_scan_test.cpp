#include "plugins/PluginManager.hpp"
#include "plugins/ScanProcess.hpp"
#include "Scan/ScanProtocol.hpp"
#include "platform/PathUtils.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace daw;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;
using Format = plugins::Format;
using State = PluginScanState;
using Clock = std::chrono::steady_clock;
static int failures = 0;
static void check(bool ok, const char* description) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) ++failures;
}
static bool until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(5ms);
    return predicate();
}
static std::string text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
static void write(const fs::path& path, const std::string& value = "ready") {
    std::ofstream(path, std::ios::binary) << value;
}
static std::string option(int argc, char** argv, const std::string& key) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]).starts_with(key + "=")) return std::string(argv[i]).substr(key.size() + 1);
    return {};
}
static bool flag(int argc, char** argv, const std::string& key) {
    for (int i = 1; i < argc; ++i) if (argv[i] == key) return true;
    return false;
}
static long long pid() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return ::getpid();
#endif
}
static bool alive(long long id) {
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, DWORD(id));
    if (!handle) return false;
    const bool running = ::WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    ::CloseHandle(handle);
    return running;
#else
    return ::kill(pid_t(id), 0) == 0;
#endif
}
static void output(const std::string& value) {
    std::fwrite(value.data(), 1, value.size(), stdout);
    std::fflush(stdout);
}
static std::string stdinText() {
    std::string value;
    char buffer[4096];
    while (const auto n = std::fread(buffer, 1, sizeof(buffer), stdin)) value.append(buffer, n);
    return value;
}

// Marker files expose actual child lifetimes independently of manager counters.
static int childMain(int argc, char** argv) {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    const auto fixture = option(argc, argv, "--fixture");
    if (!fixture.empty()) {
        const fs::path root = option(argc, argv, "--root");
        if (fixture == "echo") { output(stdinText()); return 0; }
        if (fixture == "stderr") {
            const std::string noisy(400 * 1024, 'd');
            std::fwrite(noisy.data(), 1, noisy.size(), stderr);
            output("ok"); return 0;
        }
        if (fixture == "overflow") {
            const std::string block(65536, 'x');
            for (int i = 0; i < 1100; ++i) output(block);
            return 0;
        }
        if (fixture == "descendant") {
            const auto child = ScanProcess::spawnDetached(fs::absolute(argv[0]).string(),
                {"--fixture=no-read", "--root=" + root.string()}, nullptr);
            write(root / "descendant.pid", std::to_string(child));
            return child ? 0 : 2;
        }
        if (fixture == "close-pipes") {
            std::fclose(stdin); std::fclose(stdout); std::fclose(stderr);
        }
        if (!root.empty()) write(root / "transport.pid", std::to_string(pid()));
        std::this_thread::sleep_for(30s);
        return 0;
    }
    if (flag(argc, argv, "--protocol")) {
        output(fs::path(argv[0]).stem() == "old-wire-scanner"
            ? plugins::scan::encodeResult({}) : plugins::scan::encodeHandshake());
        return 0;
    }
    const bool discovery = flag(argc, argv, "--discover");
    plugins::PluginDescriptor descriptor;
    if (discovery) {
        descriptor.path = option(argc, argv, "--path");
        descriptor.format = plugins::formatFromString(option(argc, argv, "--format"));
    } else if (flag(argc, argv, "--validate-descriptor")) {
        if (!plugins::scan::descriptorFromJson(stdinText(), descriptor)) return 2;
    } else return 8; // No --inspect/--validate in the new pipeline.
    const fs::path path = platform::pathFromUtf8(descriptor.path);
    const fs::path root = path.parent_path();
    json config;
    try { config = json::parse(text(fs::is_directory(path) ? path / "fixture.json" : path)); }
    catch (...) { return 2; }
    const auto key = path.filename().string() + "." + (discovery ? "discover" : descriptor.uid);
    int attempt = 1;
    while (fs::exists(root / (key + "." + std::to_string(attempt) + ".start"))) ++attempt;
    const auto marker = root / (key + "." + std::to_string(attempt));
    write(marker.string() + ".start", std::to_string(pid()));
    if ((discovery && config.value("discoverHang", false)) ||
        (!discovery && descriptor.uid == "zz-hang" && !fs::exists(root / "resume")) ||
        (!discovery && descriptor.uid == "timeout" && attempt == 1) ||
        (!discovery && descriptor.uid == "hang"))
        std::this_thread::sleep_for(30s);
    if (discovery) {
        std::vector<plugins::PluginDescriptor> descriptors;
        for (const auto& uid : config.at("uids")) {
            auto item = descriptor;
            item.uid = uid.get<std::string>();
            item.name = "Component " + item.uid;
            descriptors.push_back(std::move(item));
        }
        output(plugins::scan::encodeResult(descriptors));
    } else {
        if (config.value("gate", false))
            if (!until([&] { return fs::exists(root / "release"); }, 8s)) return 3;
        if (descriptor.uid == "slow")
            if (!until([&] { return fs::exists(root / "release-slow"); }, 8s)) return 3;
        if (descriptor.uid == "crash") {
#if defined(_WIN32)
            // Avoid CRT abort/WER's interactive report machinery in this fixture.
            ::TerminateProcess(::GetCurrentProcess(), 0xC0000005);
#else
            ::raise(SIGKILL);
#endif
        }
        if (descriptor.uid == "timeout" && attempt > 1) {
            for (const auto& other : fs::directory_iterator(root)) {
                const auto name = other.path().filename().string();
                if (name.ends_with(".start") && name.find(".timeout.") == std::string::npos &&
                    name.find(".crash.") == std::string::npos &&
                    !fs::exists(other.path().string().substr(0, other.path().string().size() - 6) + ".end") &&
                    alive(std::stoll(text(other.path()))))
                    write(root / "retry-overlapped");
            }
        }
        std::this_thread::sleep_for(25ms);
        if (descriptor.uid == "bad-json") {
            const char diagnostic[] = {'\xff', '\0', 'x'};
            std::fwrite(diagnostic, 1, sizeof(diagnostic), stderr);
            output("{ broken");
            return 0;
        }
        if (descriptor.uid == "wrong-uid") descriptor.uid = "someone-else";
        if (descriptor.uid == "wrong-path") descriptor.path += ".other";
        if (descriptor.uid == "wrong-format") descriptor.format = Format::AudioUnit;
        if (descriptor.uid == "mutate") std::ofstream(path, std::ios::app) << " ";
        descriptor.parameterSchema = json{{"version", 2}, {"parameters", json::array()},
            {"inputs", {2}}, {"outputs", {2}}, {"wantsMidi", false}, {"producesMidi", false}}.dump();
        if (descriptor.uid == "bad-schema") descriptor.parameterSchema = "{}";
        output(plugins::scan::encodeResult({descriptor}));
        if (descriptor.uid == "exit-after-output") return 9;
    }
    write(marker.string() + ".end");
    return 0;
}
static void configure(PluginManager& manager, const fs::path& root, const std::string& helper) {
    manager.load();
    manager.setScannerPath(helper);
    manager.setScanTimeout(1500ms);
    for (auto format : {Format::Clap, Format::Vst3, Format::Vst, Format::AudioUnit})
        manager.setSearchPaths(format, {root.string()});
}
static std::vector<plugins::PluginDescriptor> external(const PluginManager& manager) {
    auto result = manager.plugins();
    std::erase_if(result, [](const auto& d) { return d.format == Format::Internal; });
    return result;
}
static unsigned starts(const fs::path& root, const std::string& includes = {}) {
    unsigned count = 0;
    for (const auto& file : fs::directory_iterator(root)) {
        const auto name = file.path().filename().string();
        if (name.ends_with(".start") && (includes.empty() || name.find(includes) != std::string::npos)) ++count;
    }
    return count;
}
static void setEnvironment(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (*value) ::setenv(name, value, 1); else ::unsetenv(name);
#endif
}

int main(int argc, char** argv) {
    if (argc > 1) return childMain(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto sandbox = fs::temp_directory_path() / ("daw_parallel_scan_" + std::to_string(pid()));
    fs::create_directories(sandbox);
    const std::string helper = fs::absolute(argv[0]).string();
    const auto folder = [&](const char* name) { auto root = sandbox / name; fs::create_directories(root); return root; };
    const auto module = [](const fs::path& root, const std::vector<std::string>& uids, json extra = json::object()) {
        extra["uids"] = uids;
        write(root / "fixture.clap", extra.dump());
    };
    {
        const auto root = folder("transport");
        const std::string payload(900 * 1024, 'q');
        const auto echo = ScanProcess::run(helper, {"--fixture=echo"}, 5s, {payload});
        check(echo.succeeded() && echo.output == payload, "large stdin is delivered without temporary files");
        const auto noisy = ScanProcess::run(helper, {"--fixture=stderr"}, 5s);
        check(noisy.succeeded() && noisy.output == "ok" && noisy.diagnostics.size() == kScanDiagnosticLimit,
              "stderr is drained and only its bounded tail retained");
        const auto overflow = ScanProcess::run(helper, {"--fixture=overflow"}, 5s);
        check(overflow.outputLimitExceeded && !overflow.succeeded() && overflow.output.size() <= kScanResponseLimit,
              "oversized stdout is bounded and rejected");
        const auto huge = ScanProcess::run(helper, {}, 1s, {std::string(kScanRequestLimit + 1, 'x')});
        check(!huge.started && huge.transportError, "oversized requests do not launch a child");
        const auto began = Clock::now();
        const auto closed = ScanProcess::run(helper, {"--fixture=close-pipes"}, 150ms);
        check(closed.timedOut && Clock::now() - began < 1s, "closed pipes cannot bypass the process deadline");
        std::stop_source stop;
        ScanProcessResult cancelled;
        std::thread run([&] { cancelled = ScanProcess::run(helper,
            {"--fixture=no-read", "--root=" + root.string()}, 20s, {payload, stop.get_token()}); });
        check(until([&] { return !text(root / "transport.pid").empty(); }), "stdin backpressure child started");
        const auto cancelledAt = Clock::now();
        stop.request_stop(); run.join();
        check(cancelled.cancelled && !cancelled.timedOut && Clock::now() - cancelledAt < 1s &&
              !alive(std::stoll(text(root / "transport.pid"))), "cancellation during stdin transfer stops the child within one second");
        const auto tree = ScanProcess::run(helper, {"--fixture=descendant", "--root=" + root.string()}, 2s);
        check(tree.succeeded() && until([&] { return !alive(std::stoll(text(root / "descendant.pid"))); }, 500ms),
              "scanner descendants cannot outlive their process group");
    }
    {
        const auto root = folder("parallel");
        module(root, {"a", "b", "c", "slow", "x", "y"}, {{"gate", true}});
        PluginManager manager((root / "cache.json").string());
        configure(manager, root, helper); manager.setScanTimeout(8s);
        manager.startScan(false, {.maxProcesses = 4});
        check(until([&] { return starts(root) == 5; }), "four validation processes really overlap after one discovery");
        check(manager.scanSnapshot().activeJobs.size() == 4 && starts(root, "discover") == 1,
              "one discovery and a global limit of four");
        write(root / "release");
        check(until([&] { return starts(root) == 7 && manager.scanSnapshot().componentsCompleted >= 5; }),
              "free workers finish later components while a slow component remains active");
        write(root / "release-slow"); manager.waitForScan();
        check(manager.scanSnapshot().phase == ScanPhase::Completed && external(manager).size() == 6,
              "all six independent components finish");
        const auto parallel = plugins::scan::encodeResult(external(manager));
        PluginManager serial((root / "serial.json").string());
        configure(serial, root, helper); serial.startScan(false, {.maxProcesses = 1}); serial.waitForScan();
        check(parallel == plugins::scan::encodeResult(external(serial)), "one and four workers yield the same ordered catalogue");
        const auto before = starts(root);
        serial.setScannerPath("/missing/helper"); serial.startScan(); serial.waitForScan();
        check(serial.lastScanError().empty() && starts(root) == before && serial.scanSnapshot().reusedComponents == 6,
              "a warm catalogue starts zero processes including handshake");
    }
    {
        const auto root = folder("mixed");
        std::vector<std::string> names{"a.clap", "b.clap"};
#if defined(DAW_ENABLE_VST3)
        names.push_back("c.vst3");
#endif
#if defined(DAW_ENABLE_VST)
#if defined(_WIN32)
        names.push_back("d.dll");
#else
        names.push_back("d.vst");
#endif
#endif
#if defined(DAW_ENABLE_AU)
        names.push_back("e.component");
#endif
        for (const auto& name : names) {
            auto path = root / name;
#if defined(__APPLE__)
            fs::create_directories(path);
            path /= "fixture.json";
#endif
            write(path, json{{"uids", {"a"}}, {"gate", true}}.dump());
        }
        PluginManager manager((root / "cache.json").string());
        configure(manager, root, helper);
        manager.setSearchPaths(Format::Clap, {root.string(), (root / ".").string()});
        manager.setScanTimeout(8s);
        manager.startScan(false, {.maxProcesses = 4});
        const auto slots = std::min<std::size_t>(4, names.size());
        check(until([&] { return starts(root) - starts(root, ".discover.") == slots; }),
              "mixed formats share four real running child processes");
        const auto active = manager.scanSnapshot().activeJobs;
        check(active.size() == slots && (names.size() <= 2 || std::any_of(active.begin(), active.end(), [&](const auto& job) {
            return job.format != active.front().format;
        })), "concurrency is shared across formats");
        write(root / "release"); manager.waitForScan();
        check(manager.scanSnapshot().discoveries == names.size() && external(manager).size() == names.size(),
              "overlapping search paths discover each normalized module exactly once");
    }
    {
        const auto root = folder("failures");
        module(root, {"ok", "crash", "timeout", "hang", "bad-json", "bad-schema", "wrong-uid",
                      "wrong-path", "wrong-format", "exit-after-output"});
        PluginManager manager((root / "cache.json").string());
        configure(manager, root, helper); manager.setScanTimeout(400ms);
        manager.startScan(); manager.waitForScan();
        const auto snapshot = manager.scanSnapshot();
        check(snapshot.phase == ScanPhase::Completed && snapshot.passed == 2 && snapshot.failed == 8 &&
              snapshot.retries == 2 && !fs::exists(root / "retry-overlapped"),
              "timeout retries once in isolation; crashes and invalid replies affect only their UID");
        check(starts(root, ".crash.") == 1 && starts(root, ".timeout.") == 2 && external(manager).size() == 2,
              "crashes are not retried and healthy siblings stay available");
        const auto blacklist = manager.blacklist();
        check(std::any_of(blacklist.begin(), blacklist.end(), [](const auto& entry) {
            return entry.uid == "hang" && entry.attempts == 2;
        }), "a second timeout becomes a cached failure after exactly two attempts");
        const auto before = starts(root);
        manager.unblacklist(Format::Clap, (root / "fixture.clap").string(), "bad-json");
        manager.startScan(); manager.waitForScan();
        check(starts(root) == before + 1 && manager.scanSnapshot().discoveries == 0,
              "retrying one blacklist row does not rediscover or rerun siblings");
        manager.clearBlacklist();
        check(manager.blacklist().empty() && external(manager).size() == 2, "clearing failures preserves successful descriptors");
    }
    {
        const auto root = folder("resume");
        module(root, {"ok", "zz-hang"});
        const auto cachePath = (root / "cache.json").string();
        PluginManager manager(cachePath); configure(manager, root, helper); manager.startScan();
        check(until([&] {
            PluginCache cache; cache.load(cachePath);
            return cache.allPlugins().size() == 1 && starts(root, ".zz-hang.") == 1;
        }), "the manifest and first finished component are checkpointed while another runs");
        const auto began = Clock::now();
        manager.cancelScan();
        check(manager.scanSnapshot().phase == ScanPhase::Stopping, "cancellation immediately publishes Stopping");
        manager.waitForScan();
        PluginCache cache; cache.load(cachePath);
        const auto* entry = cache.find(Format::Clap, (root / "fixture.clap").string());
        check(Clock::now() - began < 1s && manager.scanSnapshot().phase == ScanPhase::Cancelled &&
              entry && entry->components.back().state == State::Pending && entry->components.back().attempts == 0 &&
              manager.blacklist().empty(), "cancelled validations stay pending without attempts or blacklist entries");
        write(root / "resume");
        PluginManager resumed(cachePath); resumed.load(); configure(resumed, root, helper);
        resumed.startScan(); resumed.waitForScan();
        check(resumed.scanSnapshot().discoveries == 0 && resumed.scanSnapshot().validations == 1 &&
              external(resumed).size() == 2, "a new manager resumes only the pending UID");
        fs::remove(root / "resume");
        resumed.startScan(true);
        check(until([&] { return starts(root, ".zz-hang.") == 3; }) && external(resumed).size() == 2,
              "full rescan keeps verified descriptors available until replacements finish");
        resumed.cancelScan(); resumed.waitForScan();
        PluginManager again(cachePath); again.load(); configure(again, root, helper);
        check(external(again).size() == 2, "last good descriptors survive restart during a full rescan");
    }
    {
        const auto root = folder("cancel-discovery");
        module(root, {"ok"}, {{"discoverHang", true}});
        PluginManager manager((root / "cache.json").string());
        configure(manager, root, helper); manager.startScan();
        check(until([&] { return starts(root) == 1; }), "discovery fixture entered its process");
        manager.cancelScan(); manager.waitForScan();
        PluginCache cache; cache.load((root / "cache.json").string());
        check(cache.entries().size() == 1 && cache.entries().front().discovery == State::Pending &&
              cache.entries().front().attempts == 0 && manager.blacklist().empty(),
              "cancelled discovery is resumable and is not a module failure");
    }
    {
        const auto root = folder("changed"); module(root, {"mutate"});
        PluginManager manager((root / "cache.json").string()); configure(manager, root, helper);
        manager.startScan(); manager.waitForScan();
        check(manager.scanSnapshot().changedFiles == 1 && external(manager).empty() && manager.blacklist().empty(),
              "a module changed during scanning loses verdicts without blacklisting");
    }
    {
        const auto root = folder("infrastructure"); module(root, {"ok"});
        const auto cachePath = (root / "cache.json").string();
        PluginManager manager(cachePath); configure(manager, root, helper);
        manager.startScan(); manager.waitForScan();
        const auto before = plugins::scan::encodeResult(external(manager));
        const auto oldHelper = root / ("old-wire-scanner" + fs::path(helper).extension().string());
        fs::copy_file(helper, oldHelper);
        manager.setScannerPath(oldHelper.string());
        manager.startScan(true); manager.waitForScan();
        check(manager.scanSnapshot().phase == ScanPhase::Failed && manager.blacklist().empty() &&
              plugins::scan::encodeResult(external(manager)) == before,
              "a helper with current result schema but obsolete transport preserves the catalogue");
        manager.setScannerPath(helper);
        module(root, {"ok"}, {{"gate", true}});
        manager.startScan(true);
        check(until([&] { return starts(root, ".ok.") == 2; }), "save-failure fixture is waiting after discovery");
        fs::rename(root / "cache.json", root / "previous-cache.json");
        fs::create_directory(root / "cache.json");
        write(root / "release"); manager.waitForScan();
        check(manager.scanSnapshot().phase == ScanPhase::Failed && manager.blacklist().empty() &&
              fs::is_directory(root / "cache.json"), "a checkpoint failure stops scanning without blaming a UID");
    }
    {
        const auto root = folder("migration"); module(root, {"ok"});
        const auto cachePath = (root / "cache.json").string();
        PluginManager first(cachePath); configure(first, root, helper); first.startScan(); first.waitForScan();
        auto saved = json::parse(text(cachePath)); saved["version"] = 1;
        for (auto& entry : saved["entries"]) { entry.erase("components"); entry.erase("discovery"); }
        const auto paths = saved["searchPaths"]; write(cachePath, saved.dump());
        PluginManager migrated(cachePath); migrated.load(); migrated.setScannerPath("/missing/helper");
        migrated.startScan(); migrated.waitForScan();
        const auto after = json::parse(text(cachePath));
        check(migrated.lastScanError().empty() && external(migrated).size() == 1 &&
              after["version"] == 2 && after["searchPaths"] == paths && migrated.scanSnapshot().reusedComponents == 1,
              "version 1 migrates paths and current results without launching a helper");
    }
    {
        std::vector<std::pair<Format, std::string>> real{{Format::Clap, DAW_TEST_CLAP_PATH}};
#if defined(DAW_TEST_VST3_PATH)
        real.push_back({Format::Vst3, DAW_TEST_VST3_PATH});
#endif
#if defined(DAW_TEST_VST_SHELL_PATH)
        real.push_back({Format::Vst, DAW_TEST_VST_SHELL_PATH});
        real.push_back({Format::Vst, DAW_TEST_VST1_PATH});
#endif
        for (const auto& [format, path] : real) {
            setEnvironment("DAW_TEST_FORBID_CHILD_CREATION", "1");
            auto discovered = ScanProcess::run(DAW_SCAN_PATH,
                {"--discover", "--format=" + std::string(plugins::toString(format)), "--path=" + path}, 5s);
            setEnvironment("DAW_TEST_FORBID_CHILD_CREATION", "");
            std::vector<plugins::PluginDescriptor> descriptors;
            check(discovered.succeeded() && plugins::scan::decodeResult(discovered.output, descriptors) && !descriptors.empty(),
                  "real format discovers descriptors without opening shell children");
            setEnvironment("DAW_TEST_FORBID_ENUMERATION", "1");
            for (const auto& descriptor : descriptors) {
                setEnvironment("DAW_TEST_FORBID_ENUMERATION", "1");
                const auto validation = ScanProcess::run(DAW_SCAN_PATH,
                    {"--validate-descriptor", "--format=" + std::string(plugins::toString(format))},
                    5s, {plugins::scan::descriptorToJson(descriptor)});
                std::vector<plugins::PluginDescriptor> result;
                check(validation.succeeded() && plugins::scan::decodeResult(validation.output, result) &&
                      result.size() == 1 && result[0].uid == descriptor.uid && result[0].format == format &&
                      !result[0].parameterSchema.empty(), "real UID validates directly without factory enumeration");
                setEnvironment("DAW_TEST_FORBID_ENUMERATION", "");
                const auto legacy = ScanProcess::run(DAW_SCAN_PATH,
                    {"--validate", "--format=" + std::string(plugins::toString(format)),
                     "--path=" + path, "--uid=" + descriptor.uid}, 5s);
                std::vector<plugins::PluginDescriptor> legacyResult;
                check(legacy.succeeded() && plugins::scan::decodeResult(legacy.output, legacyResult) &&
                      plugins::scan::encodeResult(result) == plugins::scan::encodeResult(legacyResult),
                      "direct validation preserves classification, MIDI, editor, channels and parameter schema");
            }
            setEnvironment("DAW_TEST_FORBID_ENUMERATION", "");
        }
    }
    std::error_code error;
    if (!failures) fs::remove_all(sandbox, error);
    else std::printf("Fixtures retained at %s\n", sandbox.string().c_str());
    return failures ? 1 : 0;
}
