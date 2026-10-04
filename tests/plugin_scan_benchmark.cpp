// An opt-in local benchmark. Only modules listed in the manifest execute code;
// other files in their search folders are excluded in a separate temporary cache.
#include "plugins/PluginManager.hpp"
#include "plugins/ScanProcess.hpp"
#include "Scan/ScanProtocol.hpp"
#include "platform/PathUtils.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using namespace daw;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static void fingerprint(PluginCacheEntry& entry) {
    const auto path = platform::pathFromUtf8(entry.path);
    const auto file = [&](const fs::path& item) {
        entry.fileSize += fs::file_size(item);
        entry.fileModifiedTime = std::max<std::int64_t>(
            entry.fileModifiedTime, fs::last_write_time(item).time_since_epoch().count());
    };
    if (fs::is_regular_file(path)) {
        entry.fileSize = fs::file_size(path);
        entry.fileModifiedTime = fs::last_write_time(path).time_since_epoch().count();
    } else {
        for (const auto& child : fs::recursive_directory_iterator(path))
            if (child.is_regular_file()) file(child.path());
    }
    entry.schemaVersion = plugins::scan::kSchemaVersion;
    entry.scannerVerified = entry.scanStatePresent = true;
}
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: plugin_scan_benchmark manifest.json\n");
        return 2;
    }
    try {
        json manifest;
        std::ifstream(platform::pathFromUtf8(argv[1])) >> manifest;
        const auto outputRoot = fs::absolute(platform::pathFromUtf8(manifest.at("outputDirectory").get<std::string>()));
        fs::create_directories(outputRoot);
        const std::string scanner = manifest.value("scanner", std::string(DAW_SCAN_PATH));
        PluginCache seed;
        std::set<std::pair<plugins::Format, std::string>> selected;
        std::uint64_t setupDiscoveryMs = 0;
        for (auto format : {plugins::Format::Clap, plugins::Format::Vst3,
                           plugins::Format::Vst, plugins::Format::AudioUnit})
            seed.setSearchPaths(format, {});
        seed.markSearchPathsInitialized();
        for (const auto& module : manifest.at("modules")) {
            PluginCacheEntry entry;
            entry.format = plugins::formatFromString(module.at("format").get<std::string>());
            entry.path = PluginCache::normalizedPath(module.at("path").get<std::string>());
            if (entry.format == plugins::Format::Internal || !plugins::factoryFor(entry.format) ||
                !fs::exists(platform::pathFromUtf8(entry.path)))
                throw std::runtime_error("Unsupported or missing benchmark module: " + entry.path);
            selected.emplace(entry.format, entry.path);
            const auto parent = platform::pathToUtf8(platform::pathFromUtf8(entry.path).parent_path());
            auto paths = seed.searchPaths(entry.format);
            if (std::find(paths.begin(), paths.end(), parent) == paths.end()) paths.push_back(parent);
            seed.setSearchPaths(entry.format, std::move(paths));
            const auto limit = module.value("limit", 0u);
            if (!limit) continue; // Cold discovery is timed inside PluginManager.
            fingerprint(entry);
            const auto began = Clock::now();
            const auto discovery = ScanProcess::run(scanner,
                {"--discover", "--format=" + std::string(plugins::toString(entry.format)), "--path=" + entry.path},
                std::chrono::seconds(30));
            setupDiscoveryMs += std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
            std::vector<plugins::PluginDescriptor> descriptors;
            if (!discovery.succeeded() || !plugins::scan::decodeResult(discovery.output, descriptors))
                throw std::runtime_error("Benchmark discovery failed: " + discovery.failureReason);
            entry.discovery = PluginScanState::Passed;
            for (std::size_t i = 0; i < std::min<std::size_t>(limit, descriptors.size()); ++i)
                entry.components.push_back({descriptors[i].uid, descriptors[i]});
            seed.put(std::move(entry));
        }
        for (auto* factory : plugins::availableFactories()) {
            for (const auto& directory : seed.searchPaths(factory->format())) {
                for (const auto& candidate : factory->enumerateCandidates(directory)) {
                    const auto path = PluginCache::normalizedPath(candidate);
                    if (selected.contains({factory->format(), path})) continue;
                    PluginCacheEntry excluded;
                    excluded.format = factory->format(); excluded.path = path;
                    fingerprint(excluded);
                    excluded.discovery = PluginScanState::Failed;
                    excluded.blacklisted = true;
                    excluded.failureReason = "Excluded from the benchmark manifest";
                    seed.put(std::move(excluded));
                }
            }
        }
        json report{{"modules", manifest["modules"]}, {"setupDiscoveryMs", setupDiscoveryMs},
                    {"runs", json::array()}};
        std::string firstCatalogue;
        bool equalCatalogues = true;
        for (const unsigned workers : manifest.value("processCounts", std::vector<unsigned>{1, 4})) {
            if (workers < 1 || workers > 4) throw std::runtime_error("Process count must be 1..4");
            const auto cachePath = platform::pathToUtf8(outputRoot / ("cache-" + std::to_string(workers) + ".json"));
            if (!seed.save(cachePath)) throw std::runtime_error("Cannot write benchmark cache");
            PluginManager manager(cachePath);
            manager.load(); manager.setScannerPath(scanner);
            const auto began = Clock::now();
            manager.startScan(false, {.maxProcesses = workers});
            manager.waitForScan();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
            const auto snapshot = manager.scanSnapshot();
            if (!snapshot.error.empty()) throw std::runtime_error(snapshot.error);
            auto descriptors = manager.plugins();
            std::erase_if(descriptors, [](const auto& d) { return d.format == plugins::Format::Internal; });
            const auto catalogue = plugins::scan::encodeResult(descriptors);
            if (firstCatalogue.empty()) firstCatalogue = catalogue;
            else equalCatalogues &= firstCatalogue == catalogue;
            report["runs"].push_back({{"processes", workers}, {"elapsedMs", elapsed},
                {"discoveryMs", snapshot.discoveryMs}, {"validationMs", snapshot.validationMs},
                {"discoveries", snapshot.discoveries}, {"validations", snapshot.validations},
                {"passed", snapshot.passed}, {"failed", snapshot.failed}, {"retries", snapshot.retries}});
            std::printf("%u processes: %lld ms, %u passed, %u failed\n", workers,
                static_cast<long long>(elapsed), snapshot.passed, snapshot.failed);
        }
        report["equalCatalogues"] = equalCatalogues;
        std::ofstream(outputRoot / "report.json") << report.dump(2);
        return report.value("equalCatalogues", false) ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
