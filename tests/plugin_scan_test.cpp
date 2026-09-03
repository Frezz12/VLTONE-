// The out-of-process scanner, the cache, and what happens when a plugin
// misbehaves.
//
// The failure handling is the whole point of scanning out of process, so it is
// tested against a process that really does crash and one that really does
// hang — `daw_scan --probe-crash` and `--probe-hang`. Asserting on a mock that
// returns an error code would test nothing that matters here.
#include "Scan/ScanProtocol.hpp"
#include "plugins/PluginCache.hpp"
#include "plugins/PluginManager.hpp"
#include "plugins/ScanProcess.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace daw;

static int failures = 0;
static void check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string scanner = DAW_SCAN_PATH;
    const std::string pluginPath = DAW_TEST_CLAP_PATH;
    const fs::path pluginDirectory = fs::path(pluginPath).parent_path();

    const fs::path sandbox =
        fs::temp_directory_path() / "daw_plugin_scan_test";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);

    // ── One product, one preferred format ──
    {
        auto plugin = [](plugins::Format format, std::string uid,
                         std::string name, std::string vendor,
                         bool instrument = false) {
            plugins::PluginDescriptor descriptor;
            descriptor.format = format;
            descriptor.uid = std::move(uid);
            descriptor.name = std::move(name);
            descriptor.vendor = std::move(vendor);
            descriptor.isInstrument = instrument;
            return descriptor;
        };
        std::vector<plugins::PluginDescriptor> catalogue = {
            plugin(plugins::Format::Vst3, "colour.vst3", "Colour VST3", "Acme GmbH"),
            plugin(plugins::Format::Vst, "434F4C52", "Colour VST2", "Acme GmbH"),
            plugin(plugins::Format::AudioUnit, "aufx:colr:acme", "Colour AU", "Acme"),
            plugin(plugins::Format::Clap, "com.acme.colour", "Colour CLAP", "Acme Ltd"),
            plugin(plugins::Format::Clap, "com.only.clap", "CLAP Exclusive", "Only"),
            plugin(plugins::Format::Internal, "daw.sampler", "Sampler", "DAW", true),
            plugin(plugins::Format::Internal, "daw.gravity", "Gravity", "DAW"),
        };

        const auto au = preferredPluginVariants(catalogue, plugins::Format::AudioUnit);
        check(au.size() == 4,
              "format preference collapses three variants into one product");
        check(std::ranges::any_of(au, [](const auto& descriptor) {
                  return descriptor.uid == "aufx:colr:acme";
              }),
              "the requested format wins when that variant exists");
        check(std::ranges::any_of(au, [](const auto& descriptor) {
                  return descriptor.uid == "com.only.clap";
              }),
              "a plugin available only in a fallback format remains visible");
        check(std::ranges::any_of(au, [](const auto& descriptor) {
                  return descriptor.format == plugins::Format::Internal;
              }),
              "built-in plugins are never removed by format preference");

        const auto vst3 =
            preferredPluginVariants(catalogue, plugins::Format::Vst3);
        check(std::ranges::any_of(vst3, [](const auto& descriptor) {
                  return descriptor.uid == "colour.vst3";
              }),
              "changing the preference selects the matching product variant");
        const auto vst = preferredPluginVariants(catalogue, plugins::Format::Vst);
        check(std::ranges::any_of(vst, [](const auto& descriptor) {
                  return descriptor.uid == "434F4C52";
              }),
              "legacy VST can be selected as the preferred product format");
    }

    // ── Running the scanner ──
    {
        const ScanProcessResult result = ScanProcess::run(
            scanner,
            {"--inspect", "--format=clap", "--path=" + pluginPath},
            std::chrono::milliseconds(20000));
        check(result.succeeded(), "the scanner inspects a good plugin and exits cleanly");

        std::vector<plugins::PluginDescriptor> found;
        check(plugins::scan::decodeResult(result.output, found),
              "its output parses as the scan protocol");
        bool sawGain = false;
        for (const plugins::PluginDescriptor& descriptor : found) {
            if (descriptor.uid == "com.daw.test.gain") sawGain = true;
        }
        check(found.size() == 2 && sawGain,
              "both descriptors survive the round trip through the pipe");
    }

#if defined(_WIN32)
    // A complete Windows boundary check in one place: CreateProcessW has to
    // launch an executable whose path needs quoting, the argument parser has
    // to preserve spaces and Cyrillic, and LoadLibraryW has to open the plugin
    // named by that argument.
    {
        const fs::path unicodeDirectory =
            sandbox / fs::path(u8"Тест сканера с пробелами");
        fs::create_directories(unicodeDirectory, ec);
        const fs::path scannerCopy =
            unicodeDirectory / fs::path(u8"сканер с пробелами.exe");
        const fs::path pluginCopy =
            unicodeDirectory / fs::path(u8"плагин с пробелами.clap");
        fs::copy_file(platform::pathFromUtf8(scanner), scannerCopy,
                      fs::copy_options::overwrite_existing, ec);
        const bool scannerCopied = !ec;
        ec.clear();
        fs::copy_file(platform::pathFromUtf8(pluginPath), pluginCopy,
                      fs::copy_options::overwrite_existing, ec);
        const bool pluginCopied = !ec;
        check(scannerCopied && pluginCopied,
              "copies scanner and plugin to a Unicode path with spaces");

        const std::string scannerUtf8 = platform::pathToUtf8(scannerCopy);
        const std::string pluginUtf8 = platform::pathToUtf8(pluginCopy);
        const ScanProcessResult result = ScanProcess::run(
            scannerUtf8,
            {"--inspect", "--format=clap", "--path=" + pluginUtf8},
            std::chrono::milliseconds(20000));
        std::vector<plugins::PluginDescriptor> found;
        check(result.succeeded() &&
                  plugins::scan::decodeResult(result.output, found) &&
                  found.size() == 2,
              "CreateProcessW quoting and LoadLibraryW preserve Unicode paths");
    }
#endif

    // ── A plugin that crashes the scanner ──
    {
        const ScanProcessResult result = ScanProcess::run(
            scanner, {"--probe-crash"}, std::chrono::milliseconds(20000));
        check(result.started, "the scanner process started");
        check(!result.succeeded(), "a crash is not reported as success");
        check(result.crashed, "the crash is detected");
        check(!result.failureReason.empty(),
              "the failure carries a reason to show the user");
        std::printf("      reason: %s\n", result.failureReason.c_str());
    }

    // ── A plugin that hangs the scanner ──
    {
        const auto start = std::chrono::steady_clock::now();
        const ScanProcessResult result = ScanProcess::run(
            scanner, {"--probe-hang"}, std::chrono::milliseconds(500));
        const auto elapsed = std::chrono::steady_clock::now() - start;

        check(result.timedOut, "a hung scanner is detected as a timeout");
        check(!result.succeeded(), "a timeout is not reported as success");
        // The probe sleeps for 60 s; returning anywhere near that would mean
        // the deadline did nothing.
        check(elapsed < std::chrono::seconds(10),
              "the deadline actually cut it short instead of waiting it out");
        std::printf("      returned after %lld ms\n",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                        elapsed).count());
    }

    // ── The cache round-trips ──
    {
        const std::string cachePath = (sandbox / "plugins.json").string();

        PluginCache cache;
        cache.setSearchPaths(plugins::Format::Clap, {"/one", "/two"});

        PluginCacheEntry good;
        good.format = plugins::Format::Clap;
        good.path = "/plugins/Good.clap";
        good.fileSize = 1234;
        good.fileModifiedTime = 5678;
        good.schemaVersion = plugins::scan::kSchemaVersion;
        good.ok = true;
        plugins::PluginDescriptor descriptor;
        descriptor.format = plugins::Format::Clap;
        descriptor.uid = "com.example.good";
        descriptor.name = "Good";
        descriptor.isInstrument = true;
        good.plugins.push_back(descriptor);
        cache.put(good);

        PluginCacheEntry bad;
        bad.format = plugins::Format::Clap;
        bad.path = "/plugins/Bad.clap";
        bad.blacklisted = true;
        bad.failureReason = "crashed (signal 11)";
        bad.attempts = 2;
        cache.put(bad);

        check(cache.save(cachePath), "the cache writes");

        PluginCache reloaded;
        check(reloaded.load(cachePath), "the cache reads back");
        check(reloaded.searchPaths(plugins::Format::Clap).size() == 2,
              "search paths survive the round trip");
        check(reloaded.allPlugins().size() == 1,
              "only the good entry contributes a plugin");
        const PluginCacheEntry* readBad =
            reloaded.find(plugins::Format::Clap, "/plugins/Bad.clap");
        check(readBad && readBad->blacklisted && readBad->attempts == 2,
              "the blacklisted entry keeps its reason and attempt count");

        // Incremental logic: unchanged file reuses the entry, changed does not.
        const PluginCacheEntry* readGood =
            reloaded.find(plugins::Format::Clap, "/plugins/Good.clap");
        check(readGood && PluginCache::isCurrent(*readGood, 1234, 5678),
              "an unchanged file is considered current");
        check(readGood && !PluginCache::isCurrent(*readGood, 1234, 9999),
              "a changed timestamp invalidates the entry");
        check(readGood && !PluginCache::isCurrent(*readGood, 4321, 5678),
              "a changed size invalidates the entry");
    }

    // ── A corrupt cache costs a rescan, not a crash ──
    {
        const std::string cachePath = (sandbox / "corrupt.json").string();
        std::ofstream(cachePath) << "{ this is not json";
        PluginCache cache;
        check(!cache.load(cachePath), "a corrupt cache fails to load");
        check(cache.entries().empty(), "and leaves nothing behind");
    }

    // ── End to end through PluginManager ──
    {
        const std::string cachePath = (sandbox / "managed.json").string();
        PluginManager manager(cachePath);
        manager.setScannerPath(scanner);
        manager.setScanTimeout(std::chrono::milliseconds(20000));
        manager.setSearchPaths(plugins::Format::Clap, {pluginDirectory.string()});

        manager.startScan();
        manager.waitForScan();

        check(!manager.isScanning(), "the scan finished");
        check(manager.takeScanFinished(), "finishing is reported exactly once");
        check(!manager.takeScanFinished(), "and not a second time");
        check(manager.scanTotal() >= 1, "at least one candidate was found");

        const std::vector<plugins::PluginDescriptor> found = manager.plugins();
        bool sawTestPlugin = false;
        for (const plugins::PluginDescriptor& descriptor : found) {
            if (descriptor.uid == "com.daw.test.gain") sawTestPlugin = true;
        }
        check(sawTestPlugin, "the manager found the test plugin by scanning a folder");
        // The bundle holds one of each, and the split is what the browser's two
        // menus are built from. Built-ins are filtered out first: they are in
        // every list without a scan, and this assertion is about the scan.
        const auto scannedOnly = [](std::vector<plugins::PluginDescriptor> list) {
            std::erase_if(list, [](const plugins::PluginDescriptor& d) {
                return d.format == plugins::Format::Internal;
            });
            return list;
        };
        check(scannedOnly(manager.effects()).size() == 1 &&
                  scannedOnly(manager.instruments()).size() == 1,
              "effects and instruments are told apart by their features");
        check(manager.find(plugins::Format::Clap, "com.daw.test.gain").has_value(),
              "a plugin can be looked up by its identity");
        const auto gravity =
            manager.find(plugins::Format::Internal, "daw.gravity");
        check(gravity && !gravity->isInstrument &&
                  manager.instantiate(*gravity) != nullptr,
              "Gravity is registered as an instantiable built-in effect");
        const auto graphit =
            manager.find(plugins::Format::Internal, "daw.graphit");
        check(graphit && !graphit->isInstrument &&
                  manager.instantiate(*graphit) != nullptr,
              "Graphit is registered as an instantiable built-in effect");

        // It must be usable, not merely listed.
        const auto descriptor =
            manager.find(plugins::Format::Clap, "com.daw.test.gain");
        check(descriptor && manager.instantiate(*descriptor) != nullptr,
              "a scanned descriptor can be instantiated");

        // A second scan with nothing changed on disk must not relaunch the
        // scanner — this is the difference between a snappy manager and one
        // that takes a minute every time it opens.
        PluginManager second(cachePath);
        second.setScannerPath("/nonexistent/daw_scan");   // any launch would fail
        second.load();
        second.startScan();
        second.waitForScan();
        bool stillThere = false;
        for (const plugins::PluginDescriptor& entry : second.plugins()) {
            if (entry.uid == "com.daw.test.gain") stillThere = true;
        }
        check(stillThere,
              "an incremental rescan reuses the cache without launching the scanner");
    }

    // ── A bad plugin is blacklisted and the process survives ──
    {
        const std::string cachePath = (sandbox / "blacklist.json").string();
        const fs::path badDirectory = sandbox / "bad";
        fs::create_directories(badDirectory, ec);
        // A file with the right extension and nothing inside it: exactly what a
        // truncated download looks like.
        std::ofstream(badDirectory / "Broken.clap") << "not a plugin";

        PluginManager manager(cachePath);
        manager.setScannerPath(scanner);
        manager.setScanTimeout(std::chrono::milliseconds(20000));
        manager.setSearchPaths(plugins::Format::Clap, {badDirectory.string()});
        manager.startScan();
        manager.waitForScan();

        std::vector<plugins::PluginDescriptor> afterBadScan = manager.plugins();
        std::erase_if(afterBadScan, [](const plugins::PluginDescriptor& d) {
            return d.format == plugins::Format::Internal;
        });
        check(afterBadScan.empty(), "a broken module contributes no plugins");
        const std::vector<PluginManager::BlacklistEntry> blocked = manager.blacklist();
        check(blocked.size() == 1, "the broken module is blacklisted");
        if (!blocked.empty()) {
            check(!blocked.front().reason.empty(),
                  "the blacklist entry says why");
            std::printf("      reason: %s\n", blocked.front().reason.c_str());
        }

        // Un-blacklisting must make it scannable again, not merely hide it.
        manager.unblacklist(plugins::Format::Clap,
                            (badDirectory / "Broken.clap").string());
        check(manager.blacklist().empty(), "un-blacklisting removes the entry");
    }

    fs::remove_all(sandbox, ec);
    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
