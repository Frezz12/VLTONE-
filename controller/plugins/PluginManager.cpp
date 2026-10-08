#include "plugins/PluginManager.hpp"

#include "Internal/InternalFactory.hpp"
#include "Scan/ScanProtocol.hpp"
#include "plugins/ScanProcess.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <cmath>
#include <unordered_map>
#include <condition_variable>
#include <deque>
#include <set>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace daw {
namespace fs = std::filesystem;
using plugins::Format;
using plugins::PluginDescriptor;

namespace {

std::atomic<std::uint64_t> gNextPluginManagerId{0};

bool isExternalFormat(Format format) noexcept {
    return format == Format::Clap || format == Format::Vst3 ||
           format == Format::Vst || format == Format::AudioUnit;
}

void appendOnce(std::vector<Format>& formats, Format format) {
    if (!isExternalFormat(format)) return;
    if (std::find(formats.begin(), formats.end(), format) == formats.end())
        formats.push_back(format);
}

std::vector<Format> formatPriority(Format preferred) {
    std::vector<Format> formats;
    appendOnce(formats, preferred);
#if defined(__APPLE__)
    appendOnce(formats, Format::AudioUnit);
    appendOnce(formats, Format::Vst3);
    appendOnce(formats, Format::Vst);
    appendOnce(formats, Format::Clap);
#elif defined(_WIN32)
    appendOnce(formats, Format::Vst3);
    appendOnce(formats, Format::Vst);
    appendOnce(formats, Format::Clap);
    appendOnce(formats, Format::AudioUnit);
#else
    appendOnce(formats, Format::Vst3);
    appendOnce(formats, Format::Vst);
    appendOnce(formats, Format::Clap);
    appendOnce(formats, Format::AudioUnit);
#endif
    return formats;
}

int formatRank(Format format, const std::vector<Format>& priority) {
    const auto found = std::find(priority.begin(), priority.end(), format);
    return found == priority.end() ? 100 : int(found - priority.begin());
}

std::vector<std::string> wordsOf(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    auto flush = [&] {
        if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    };
    for (const unsigned char c : text) {
        if (c >= 0x80) {
            // Keep UTF-8 bytes untouched. Identical Unicode metadata remains
            // identical without pulling a UI/string framework into controller.
            word.push_back(char(c));
        } else if (std::isalnum(c)) {
            word.push_back(char(std::tolower(c)));
        } else {
            flush();
        }
    }
    flush();
    return words;
}

void removeFormatSuffix(std::vector<std::string>& words) {
    if (words.empty()) return;
    const std::string& last = words.back();
    if (last == "au" || last == "vst" || last == "vst2" ||
        last == "vst3" || last == "clap") {
        words.pop_back();
        return;
    }
    if (words.size() >= 2 &&
        ((words[words.size() - 2] == "audio" && last == "unit") ||
         (words[words.size() - 2] == "vst" &&
          (last == "2" || last == "3")))) {
        words.resize(words.size() - 2);
    }
}

void removeCompanySuffixes(std::vector<std::string>& words) {
    static constexpr std::string_view suffixes[] = {
        "ag", "bv", "co", "company", "corp", "corporation", "gmbh",
        "inc", "incorporated", "limited", "llc", "ltd", "plc", "sa", "sas"};
    while (!words.empty() &&
           std::find(std::begin(suffixes), std::end(suffixes), words.back()) !=
               std::end(suffixes)) {
        words.pop_back();
    }
}

std::string joinWords(const std::vector<std::string>& words) {
    std::string joined;
    for (const std::string& word : words) {
        if (!joined.empty()) joined.push_back('\x1f');
        joined += word;
    }
    return joined;
}

std::string productKey(const PluginDescriptor& descriptor) {
    if (!isExternalFormat(descriptor.format)) {
        return "unique\x1f" + std::string(plugins::toString(descriptor.format)) +
               "\x1f" + descriptor.uid + "\x1f" + descriptor.path;
    }

    std::vector<std::string> name = wordsOf(descriptor.name);
    removeFormatSuffix(name);
    std::vector<std::string> vendor = wordsOf(descriptor.vendor);
    removeCompanySuffixes(vendor);

    // Empty scanner metadata must never merge a whole format into one row.
    const std::string normalName = joinWords(name);
    if (normalName.empty()) {
        return "unique\x1f" + std::string(plugins::toString(descriptor.format)) +
               "\x1f" + descriptor.uid + "\x1f" + descriptor.path;
    }
    return std::string(descriptor.isInstrument ? "instrument\x1f" : "effect\x1f") +
           joinWords(vendor) + "\x1f" + normalName;
}

/// Fingerprint what the scanner opens. Bundle directory mtimes are not updated
/// reliably when only an inner binary/resource is replaced, therefore include
/// every regular file's size and newest timestamp.
void statFile(const std::string& path, std::uint64_t& size, std::int64_t& modified) {
    std::error_code ec;
    size = 0;
    modified = 0;
    const fs::path candidate = platform::pathFromUtf8(path);
    if (fs::is_regular_file(candidate, ec)) {
        size = fs::file_size(candidate, ec);
        const auto written = fs::last_write_time(candidate, ec);
        modified = ec ? 0 : written.time_since_epoch().count();
        return;
    }
    ec.clear();
    if (!fs::is_directory(candidate, ec)) return;
    for (fs::recursive_directory_iterator it(
             candidate, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::uint64_t fileSize = it->file_size(ec);
        if (!ec) size += fileSize;
        ec.clear();
        const auto written = it->last_write_time(ec);
        if (!ec) modified = std::max<std::int64_t>(modified,
                                                   written.time_since_epoch().count());
        ec.clear();
    }
}

std::string executableDirectory() {
#if defined(__APPLE__)
    char buffer[4096];
    std::uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) return {};
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(fs::path(buffer), ec);
    return ec ? fs::path(buffer).parent_path().string() : resolved.parent_path().string();
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(512);
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD count = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (count == 0) return {};
        if (count < buffer.size()) {
            return platform::pathToUtf8(
                fs::path(std::wstring_view(buffer.data(), count)).parent_path());
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
#else
    std::error_code ec;
    const fs::path resolved = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : resolved.parent_path().string();
#endif
}

} // namespace

bool samePluginProduct(const PluginDescriptor& a, const PluginDescriptor& b) {
    // Do not merge distinct same-format shell components with identical names.
    if (a.format == b.format) return a.uid == b.uid;
    return productKey(a) == productKey(b);
}

std::vector<PluginDescriptor> preferredPluginVariants(
    std::vector<PluginDescriptor> descriptors, Format preferredFormat) {
    struct Product {
        std::vector<PluginDescriptor> variants;
    };

    std::vector<Product> products;
    std::unordered_map<std::string, std::size_t> productByKey;
    productByKey.reserve(descriptors.size());
    for (PluginDescriptor& descriptor : descriptors) {
        const std::string key = productKey(descriptor);
        const auto [found, inserted] =
            productByKey.emplace(key, products.size());
        if (inserted) products.emplace_back();
        products[found->second].variants.push_back(std::move(descriptor));
    }

    const std::vector<Format> priority = formatPriority(preferredFormat);
    std::vector<PluginDescriptor> selected;
    selected.reserve(products.size());
    for (Product& product : products) {
        int best = 100;
        for (const PluginDescriptor& variant : product.variants)
            best = std::min(best, formatRank(variant.format, priority));

        // Preserve more than one component in the chosen format. Some shells
        // expose distinct components with the same display metadata; format
        // de-duplication must not silently throw those away.
        for (PluginDescriptor& variant : product.variants) {
            if (formatRank(variant.format, priority) == best)
                selected.push_back(std::move(variant));
        }
    }
    return selected;
}

PluginManager::PluginManager(std::string cachePath)
    : m_cachePath(std::move(cachePath)),
      m_instanceId(gNextPluginManagerId.fetch_add(
                       1, std::memory_order_relaxed) + 1),
      m_scannerPath(helperPath("daw_scan")),
      m_pluginHostPath(helperPath("daw_plugin_host")) {}

PluginManager::~PluginManager() {
    cancelScan();
    waitForScan();
}

std::string PluginManager::helperPath(std::string name) {
    const std::string directory = executableDirectory();
#if defined(_WIN32)
    name += ".exe";
#endif
    return directory.empty() ? name
        : platform::pathToUtf8(platform::pathFromUtf8(directory) / platform::pathFromUtf8(name));
}

void PluginManager::setScannerPath(std::string path) {
    m_scannerPath = std::move(path);
}

void PluginManager::load() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.load(m_cachePath);
    // A cache that has never been written has no paths; start from what the
    // platform says rather than from nothing, or the first scan finds zero.
    if (!m_cache.searchPathsInitialized()) {
        // One-time migration for caches written by the old all-or-nothing
        // default logic. Merge rather than replace so custom folders survive,
        // while a missing per-user VST3 folder is restored.
        for (plugins::PluginFactory* factory : plugins::availableFactories()) {
            std::vector<std::string> paths = m_cache.searchPaths(factory->format());
            for (const std::string& fallback : factory->defaultSearchPaths()) {
                if (std::find(paths.begin(), paths.end(), fallback) == paths.end()) {
                    paths.push_back(fallback);
                }
            }
            m_cache.setSearchPaths(factory->format(), std::move(paths));
        }
        m_cache.markSearchPathsInitialized();
    }
    // Old caches already marked their other format paths as initialized. Add
    // only the newly introduced VST defaults; never reset CLAP/VST3/AU paths
    // or overwrite an intentionally empty VST list from a newer cache.
    if (!m_cache.vstSearchPathsPresent()) {
        if (plugins::PluginFactory* factory = plugins::factoryFor(Format::Vst)) {
            m_cache.setSearchPaths(Format::Vst, factory->defaultSearchPaths());
        }
    }

    // Removed plugins must disappear without requiring a destructive full
    // rescan. In particular, stale shell entries otherwise live forever.
    std::vector<std::pair<Format, std::string>> missing;
    for (const PluginCacheEntry& entry : m_cache.entries()) {
        std::error_code ec;
        if (!fs::exists(platform::pathFromUtf8(entry.path), ec))
            missing.emplace_back(entry.format, entry.path);
    }
    for (const auto& [format, path] : missing) m_cache.remove(format, path);
    m_catalogueRevision.fetch_add(1, std::memory_order_release);
}

bool PluginManager::save() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.save(m_cachePath);
}

std::vector<std::string> PluginManager::searchPaths(Format format) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.searchPaths(format);
}

void PluginManager::setSearchPaths(Format format, std::vector<std::string> paths) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.setSearchPaths(format, std::move(paths));
}

void PluginManager::addSearchPath(Format format, const std::string& directory) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> paths = m_cache.searchPaths(format);
    if (std::find(paths.begin(), paths.end(), directory) != paths.end()) return;
    paths.push_back(directory);
    m_cache.setSearchPaths(format, std::move(paths));
}

void PluginManager::removeSearchPath(Format format, const std::string& directory) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> paths = m_cache.searchPaths(format);
    std::erase(paths, directory);
    m_cache.setSearchPaths(format, std::move(paths));
}

void PluginManager::resetSearchPathsToDefaults() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (plugins::PluginFactory* factory : plugins::availableFactories()) {
        m_cache.setSearchPaths(factory->format(), factory->defaultSearchPaths());
    }
}

float PluginManager::scanProgress() const noexcept {
    const std::uint32_t total = m_total.load(std::memory_order_relaxed);
    if (total == 0) return 0.0f;
    return float(m_scanned.load(std::memory_order_relaxed)) / float(total);
}

std::string PluginManager::currentScanPath() const {
    std::lock_guard<std::mutex> lock(m_currentMutex);
    return m_currentPath;
}

std::string PluginManager::lastScanError() const {
    std::lock_guard<std::mutex> lock(m_currentMutex);
    return m_scanError;
}

ScanSnapshot PluginManager::scanSnapshot() const {
    std::lock_guard lock(m_currentMutex);
    return m_snapshot;
}

std::vector<PluginManager::Candidate> PluginManager::collectCandidates() const {
    std::vector<Candidate> candidates;
    for (plugins::PluginFactory* factory : plugins::availableFactories()) {
        const Format format = factory->format();
        std::vector<std::string> paths;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            paths = m_cache.searchPaths(format);
        }
        for (const std::string& directory : paths) {
            if (m_cancel.load(std::memory_order_acquire)) break;
            // Enumeration only looks at names and bundle shapes — it never
            // opens a module — so it is safe to run here rather than paying a
            // process launch per directory.
            for (std::string& candidate : factory->enumerateCandidates(directory)) {
                if (m_cancel.load(std::memory_order_acquire)) break;
                candidates.push_back(Candidate{format, PluginCache::normalizedPath(candidate)});
            }
        }
    }
    // Two search paths can nest, and a plugin found twice would be scanned
    // twice and listed twice.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.format != b.format ? a.format < b.format : a.path < b.path;
              });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const Candidate& a, const Candidate& b) {
                                     return a.format == b.format && a.path == b.path;
                                 }),
                     candidates.end());
    return candidates;
}

void PluginManager::startScan(bool rescanAll, ScanOptions options) {
    if (m_scanning.exchange(true, std::memory_order_acq_rel)) return;
    waitForScan();   // join a previous, already-finished worker
    m_cancel.store(false, std::memory_order_release);
    m_finished.store(false, std::memory_order_release);
    m_scanned.store(0, std::memory_order_relaxed);
    m_total.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_currentMutex);
        m_currentPath.clear();
        m_scanError.clear();
        m_snapshot = {};
        m_snapshot.phase = ScanPhase::Collecting;
        m_scanCancellation = std::stop_source{};
    }
    m_worker = std::thread([this, rescanAll, options] { scanWorker(rescanAll, options); });
}

void PluginManager::cancelScan() {
    m_cancel.store(true, std::memory_order_release);
    std::lock_guard lock(m_currentMutex);
    if (isScanning()) m_snapshot.phase = ScanPhase::Stopping;
    m_scanCancellation.request_stop();
}

void PluginManager::waitForScan() {
    if (m_worker.joinable()) m_worker.join();
}

void PluginManager::scanWorker(bool rescanAll, ScanOptions options) {
    using Clock = std::chrono::steady_clock;
    using State = PluginScanState;
    const auto startedAt = Clock::now();
    ScanSnapshot snapshot;
    snapshot.phase = ScanPhase::Collecting;
    const unsigned cpus = std::thread::hardware_concurrency();
    snapshot.maxProcesses = options.maxProcesses == 0
        ? std::clamp(cpus ? cpus : 2u, 1u, 4u)
        : std::clamp(options.maxProcesses, 1u, 4u);
    const auto cancellation = m_scanCancellation.get_token();

    struct Module { PluginCacheEntry entry; bool dirty = false; bool changed = false; };
    struct Job {
        std::size_t module = 0;
        PluginDescriptor descriptor; // empty UID means discovery
        ScanJobInfo info;
    };
    struct Completion { Job job; ScanProcessResult process; std::uint64_t durationMs = 0; };
    std::vector<Module> modules;
    std::deque<Job> ready, retries;
    std::uint64_t nextId = 0;
    unsigned unsaved = 0;
    bool published = false, cacheWriteFailed = false;
    auto lastSavedAt = startedAt;

    const auto failScan = [&](std::string error) {
        if (snapshot.error.empty()) {
            snapshot.error = std::move(error);
            std::fprintf(stderr, "Plugin scan stopped: %s\n", snapshot.error.c_str());
        }
        m_scanCancellation.request_stop();
    };
    const auto publish = [&] {
        snapshot.filesCompleted = snapshot.componentsTotal = snapshot.componentsCompleted = 0;
        snapshot.passed = snapshot.failed = 0;
        snapshot.discoveryComplete = true;
        snapshot.results.clear();
        for (const auto& module : modules) {
            const auto& entry = module.entry;
            if (entry.complete() || module.changed) ++snapshot.filesCompleted;
            if (entry.discovery == State::Pending) snapshot.discoveryComplete = false;
            if (entry.discovery == State::Failed)
                snapshot.results.push_back({entry.format, entry.path, {}, {}, entry.failureReason,
                    State::Failed, entry.attempts, entry.discoveryDurationMs});
            for (const auto& component : entry.components) {
                ++snapshot.componentsTotal;
                if (component.state != State::Pending) ++snapshot.componentsCompleted;
                if (component.state == State::Passed) ++snapshot.passed;
                if (component.state == State::Failed) ++snapshot.failed;
                snapshot.results.push_back({entry.format, entry.path, component.uid,
                    component.descriptor.name, component.failureReason, component.state,
                    component.attempts, component.durationMs});
            }
        }
        snapshot.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - startedAt).count();
        std::lock_guard lock(m_currentMutex);
        m_snapshot = snapshot;
        if (m_cancel.load(std::memory_order_acquire) &&
            snapshot.phase != ScanPhase::Cancelled && snapshot.phase != ScanPhase::Failed)
            m_snapshot.phase = ScanPhase::Stopping;
        m_scanError = snapshot.error;
        m_currentPath = snapshot.activeJobs.empty() ? std::string{} : snapshot.activeJobs.front().path;
        m_total.store(snapshot.filesTotal, std::memory_order_relaxed);
        m_scanned.store(snapshot.filesCompleted, std::memory_order_relaxed);
    };
    const auto checkpoint = [&] {
        if (cacheWriteFailed) return false;
        bool saved;
        {
            std::lock_guard lock(m_mutex);
            for (auto& module : modules) {
                if (!module.dirty) continue;
                m_cache.put(module.entry);
                module.dirty = false;
            }
            saved = m_cache.save(m_cachePath);
        }
        m_catalogueRevision.fetch_add(1, std::memory_order_release);
        if (!saved) {
            cacheWriteFailed = true;
            failScan("Could not save the plugin cache. Check that its folder is writable.");
            return false;
        }
        unsaved = 0;
        lastSavedAt = Clock::now();
        return true;
    };
    const auto enqueue = [&](std::size_t index, const PluginDescriptor* descriptor, bool retry) {
        Job job;
        job.module = index;
        const auto& entry = modules[index].entry;
        job.info = {++nextId, entry.format, entry.path,
            descriptor ? descriptor->uid : std::string{},
            descriptor ? descriptor->name : std::string{}, !descriptor, retry};
        if (descriptor) {
            job.descriptor = *descriptor;
            // A prior schema is unnecessary input to create(), and can dwarf
            // the discovery metadata in resumed scans of large instruments.
            job.descriptor.parameterSchema.clear();
            job.descriptor.parameterFingerprint.clear();
        }
        (retry ? retries : ready).push_back(std::move(job));
    };
    const auto validateResponse = [&](const Job& job, const ScanProcessResult& process,
                                      std::vector<PluginDescriptor>& descriptors) {
        if (!process.succeeded() ||
            !plugins::scan::decodeResult(process.output, descriptors, true) || descriptors.empty())
            return false;
        if (!job.info.discovery && descriptors.size() != 1) return false;
        std::set<std::string> identities;
        for (const auto& descriptor : descriptors) {
            if (descriptor.format != job.info.format ||
                PluginCache::normalizedPath(descriptor.path) != job.info.path ||
                !identities.insert(descriptor.uid).second ||
                (!job.info.discovery && descriptor.uid != job.info.uid))
                return false;
            if (!job.info.discovery) {
                try {
                    const auto schema = nlohmann::json::parse(descriptor.parameterSchema);
                    if (!schema.is_object() || schema.value("version", 0) != 2 ||
                        !schema.at("parameters").is_array() || !schema.at("inputs").is_array() ||
                        !schema.at("outputs").is_array() ||
                        schema.at("wantsMidi").get<bool>() != descriptor.wantsMidi ||
                        schema.at("producesMidi").get<bool>() != descriptor.producesMidi) return false;
                    for (const auto& parameter : schema["parameters"]) {
                        if (!parameter.at("id").is_string() || !parameter.at("unit").is_string() ||
                            !parameter.at("minimum").is_number() || !parameter.at("maximum").is_number() ||
                            !parameter.at("default").is_number() || !parameter.at("stepped").is_boolean() ||
                            !parameter.at("automatable").is_boolean() || !parameter.at("bypass").is_boolean())
                            return false;
                    }
                    for (const char* side : {"inputs", "outputs"})
                        for (const auto& channels : schema[side])
                            if (!channels.is_number_unsigned() || channels.get<std::uint64_t>() > 65535)
                                return false;
                } catch (const std::exception&) { return false; }
            }
        }
        return true;
    };

    try {
        const auto candidates = collectCandidates();
        snapshot.filesTotal = std::uint32_t(candidates.size());
        modules.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            if (cancellation.stop_requested()) break;
            std::uint64_t size;
            std::int64_t modified;
            statFile(candidate.path, size, modified);
            std::optional<PluginCacheEntry> old;
            {
                std::lock_guard lock(m_mutex);
                if (const auto* cached = m_cache.find(candidate.format, candidate.path)) old = *cached;
            }
            const bool sameFile = old && old->fileSize == size && old->fileModifiedTime == modified;
            const bool legacyFailure = old && !old->scannerVerified &&
                old->failureReason == "the scanner returned nothing usable";
            Module module;
            if (!rescanAll && old && PluginCache::isCurrent(*old, size, modified) && !legacyFailure) {
                module.entry = *old;
                module.entry.path = candidate.path;
                if (old->complete()) ++snapshot.reusedFiles;
                for (const auto& component : old->components)
                    if (component.state != State::Pending) ++snapshot.reusedComponents;
            } else {
                auto& entry = module.entry;
                entry.format = candidate.format;
                entry.path = candidate.path;
                entry.fileSize = size;
                entry.fileModifiedTime = modified;
                entry.schemaVersion = plugins::scan::kSchemaVersion;
                entry.scannerVerified = true;
                entry.scanStatePresent = true;
                if (old && !legacyFailure && old->discovery == State::Failed)
                    entry.attempts = old->attempts;
                if (sameFile && old->ok && !old->blacklisted) {
                    entry.plugins = old->plugins;
                    entry.ok = !entry.plugins.empty();
                }
                module.dirty = true;
            }
            modules.push_back(std::move(module));
            const auto index = modules.size() - 1;
            const auto& entry = modules.back().entry;
            if (entry.discovery == State::Pending)
                enqueue(index, nullptr, entry.timeoutRetryPending);
            else if (entry.discovery == State::Passed)
                for (const auto& component : entry.components)
                    if (component.state == State::Pending)
                        enqueue(index, &component.descriptor, component.timeoutRetryPending);
        }
        snapshot.phase = ScanPhase::Discovering;
        publish();

        const auto runQueue = [&] {
            if (cancellation.stop_requested()) return;
            if (ready.empty() && retries.empty()) {
                // Includes writing a v1 migration; there is no helper launch.
                published = true;
                checkpoint();
                return;
            }
            const auto probe = ScanProcess::run(m_scannerPath, {"--protocol"},
                std::min(m_timeout, std::chrono::milliseconds(5000)), {{}, cancellation});
            if (probe.cancelled) return;
            if (!probe.succeeded() || !plugins::scan::decodeHandshake(probe.output)) {
                failScan("The plugin scanner is missing or incompatible. "
                         "Rebuild or reinstall VLTONE together with daw_scan. " + probe.failureReason);
                return;
            }
            // Do not replace the old catalogue until the helper is verified.
            published = true;
            if (!checkpoint()) return;

            std::mutex queueMutex;
            std::condition_variable_any wake;
            std::deque<Job> dispatched;
            std::deque<Completion> completed;
            std::vector<std::jthread> workers;
            struct CancelBeforeJoin {
                std::stop_source& source;
                ~CancelBeforeJoin() { source.request_stop(); }
            } cancelBeforeJoin{m_scanCancellation};
            for (unsigned i = 0; i < snapshot.maxProcesses; ++i) {
                workers.emplace_back([&](std::stop_token stop) {
                    for (;;) {
                        Job job;
                        {
                            std::unique_lock lock(queueMutex);
                            if (!wake.wait(lock, stop, [&] { return !dispatched.empty(); })) return;
                            job = std::move(dispatched.front());
                            dispatched.pop_front();
                        }
                        const auto began = Clock::now();
                        Completion completion;
                        completion.job = std::move(job);
                        try {
                            const auto& info = completion.job.info;
                            std::vector<std::string> arguments{
                                info.discovery ? "--discover" : "--validate-descriptor",
                                "--format=" + std::string(plugins::toString(info.format))};
                            std::string request;
                            if (info.discovery) arguments.push_back("--path=" + info.path);
                            else request = plugins::scan::descriptorToJson(completion.job.descriptor);
                            completion.process = ScanProcess::run(m_scannerPath, arguments, m_timeout,
                                                                   {request, cancellation});
                        } catch (const std::exception& error) {
                            completion.process.transportError = true;
                            completion.process.failureReason = error.what();
                        }
                        completion.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - began).count();
                        {
                            std::lock_guard lock(queueMutex);
                            completed.push_back(std::move(completion));
                        }
                        wake.notify_all();
                    }
                });
            }
            bool firstResult = true;
            for (;;) {
                if (cancellation.stop_requested()) {
                    ready.clear();
                    retries.clear();
                }
                // Retry only after every ordinary job has left its process.
                const bool retryActive = std::any_of(snapshot.activeJobs.begin(), snapshot.activeJobs.end(),
                    [](const auto& info) { return info.retry; });
                if (!retryActive && !cancellation.stop_requested()) {
                    while (snapshot.activeJobs.size() < snapshot.maxProcesses &&
                           (!ready.empty() || (snapshot.activeJobs.empty() && !retries.empty()))) {
                        const bool retry = ready.empty();
                        auto& queue = retry ? retries : ready;
                        Job job = std::move(queue.front());
                        queue.pop_front();
                        job.info.startedAt = Clock::now();
                        snapshot.activeJobs.push_back(job.info);
                        if (job.info.retry) ++snapshot.retries;
                        {
                            std::lock_guard lock(queueMutex);
                            dispatched.push_back(std::move(job));
                        }
                        wake.notify_all();
                        if (retry) break;
                    }
                }
                snapshot.phase = cancellation.stop_requested() ? ScanPhase::Stopping :
                    std::any_of(snapshot.activeJobs.begin(), snapshot.activeJobs.end(),
                        [](const auto& info) { return info.retry; }) ? ScanPhase::Retrying :
                    std::any_of(modules.begin(), modules.end(), [](const auto& module) {
                        return module.entry.discovery == State::Pending;
                    }) ? ScanPhase::Discovering : ScanPhase::Validating;
                publish();
                if (snapshot.activeJobs.empty() && ready.empty() && retries.empty()) break;

                std::deque<Completion> batch;
                {
                    std::unique_lock lock(queueMutex);
                    wake.wait_for(lock, std::chrono::milliseconds(20), [&] { return !completed.empty(); });
                    batch.swap(completed);
                }
                for (auto& completion : batch) {
                    auto& job = completion.job;
                    auto& process = completion.process;
                    std::erase_if(snapshot.activeJobs, [&](const auto& info) { return info.id == job.info.id; });
                    if (process.cancelled) continue;
                    if (!process.started || process.transportError) {
                        failScan("Could not run the plugin scanner. " + process.failureReason);
                        continue;
                    }
                    auto& module = modules[job.module];
                    auto& entry = module.entry;
                    std::vector<PluginDescriptor> descriptors;
                    const bool passed = validateResponse(job, process, descriptors);
                    std::string reason;
                    if (!passed) {
                        reason = process.failureReason.empty()
                            ? "the scanner returned an invalid or mismatched result" : process.failureReason;
                        if (!process.diagnostics.empty()) {
                            const auto tail = process.diagnostics.substr(
                                process.diagnostics.size() > 4096 ? process.diagnostics.size() - 4096 : 0);
                            reason += "\n" + tail;
                        }
                    }
                    const bool retry = process.timedOut && !job.info.retry;
                    if (job.info.discovery) {
                        ++snapshot.discoveries;
                        snapshot.discoveryMs += completion.durationMs;
                        entry.discoveryDurationMs += completion.durationMs;
                        ++entry.attempts;
                        entry.timeoutRetryPending = retry;
                        entry.discovery = passed ? State::Passed : retry ? State::Pending : State::Failed;
                        entry.failureReason = reason;
                        entry.blacklisted = !passed && !retry;
                        if (passed) {
                            entry.components.clear();
                            std::sort(descriptors.begin(), descriptors.end(),
                                [](const auto& a, const auto& b) { return a.uid < b.uid; });
                            for (auto& descriptor : descriptors) {
                                descriptor.path = entry.path;
                                descriptor.fileSize = entry.fileSize;
                                descriptor.fileModifiedTime = entry.fileModifiedTime;
                                entry.components.push_back({descriptor.uid, std::move(descriptor)});
                            }
                            std::erase_if(entry.plugins, [&](const auto& previous) {
                                return std::none_of(entry.components.begin(), entry.components.end(),
                                    [&](const auto& c) { return c.uid == previous.uid; });
                            });
                            for (const auto& component : entry.components)
                                enqueue(job.module, &component.descriptor, false);
                        } else if (!retry) {
                            entry.components.clear();
                            entry.plugins.clear();
                        }
                    } else {
                        ++snapshot.validations;
                        snapshot.validationMs += completion.durationMs;
                        auto component = std::find_if(entry.components.begin(), entry.components.end(),
                            [&](const auto& c) { return c.uid == job.info.uid; });
                        if (component == entry.components.end()) {
                            failScan("Scanner queue lost a component identity.");
                            continue;
                        }
                        ++component->attempts;
                        component->durationMs += completion.durationMs;
                        component->timeoutRetryPending = retry;
                        component->state = passed ? State::Passed : retry ? State::Pending : State::Failed;
                        component->failureReason = reason;
                        if (!retry) {
                            std::erase_if(entry.plugins, [&](const auto& d) { return d.uid == component->uid; });
                            if (passed) {
                                component->descriptor = std::move(descriptors.front());
                                component->descriptor.path = entry.path;
                                component->descriptor.fileSize = entry.fileSize;
                                component->descriptor.fileModifiedTime = entry.fileModifiedTime;
                                entry.plugins.push_back(component->descriptor);
                            }
                        }
                    }
                    if (retry) enqueue(job.module, job.info.discovery ? nullptr : &job.descriptor, true);
                    entry.ok = !entry.plugins.empty();
                    // A generation whose module changed is never a compatibility
                    // verdict. Discard its successes and failures together.
                    if (entry.complete()) {
                        std::uint64_t size;
                        std::int64_t modified;
                        statFile(entry.path, size, modified);
                        if (size != entry.fileSize || modified != entry.fileModifiedTime) {
                            entry.discovery = State::Pending;
                            entry.components.clear();
                            entry.plugins.clear();
                            entry.ok = entry.blacklisted = entry.timeoutRetryPending = false;
                            entry.attempts = 0;
                            entry.failureReason = "file changed during scan; pending a new scan";
                            module.changed = true;
                            ++snapshot.changedFiles;
                        }
                    }
                    module.dirty = true;
                    ++unsaved;
                    if (job.info.discovery || firstResult || unsaved >= 8 ||
                        Clock::now() - lastSavedAt >= std::chrono::seconds(2)) {
                        checkpoint();
                    }
                    if (!job.info.discovery) firstResult = false;
                }
                if (unsaved && Clock::now() - lastSavedAt >= std::chrono::seconds(2)) checkpoint();
            }
            for (auto& worker : workers) worker.request_stop();
            wake.notify_all();
            // jthread destruction joins all helpers before scan completion.
        };
        runQueue();
    } catch (const std::exception& error) {
        failScan("Plugin scan failed: " + std::string(error.what()));
    } catch (...) {
        failScan("Plugin scan failed with an unexpected controller error.");
    }

    if (published && !cacheWriteFailed) checkpoint();
    snapshot.activeJobs.clear();
    snapshot.phase = !snapshot.error.empty() ? ScanPhase::Failed :
        m_cancel.load(std::memory_order_acquire) ? ScanPhase::Cancelled : ScanPhase::Completed;
    publish();
    std::fprintf(stderr,
        "Plugin scan: %u cached files, %u reused components, %u discoveries (%llu ms), "
        "%u validations (%llu ms), %u retries, %llu ms total%s\n",
        snapshot.reusedFiles, snapshot.reusedComponents, snapshot.discoveries,
        static_cast<unsigned long long>(snapshot.discoveryMs), snapshot.validations,
        static_cast<unsigned long long>(snapshot.validationMs), snapshot.retries,
        static_cast<unsigned long long>(snapshot.elapsedMs),
        snapshot.phase == ScanPhase::Cancelled ? " (cancelled)" : "");
    m_catalogueRevision.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard lock(m_currentMutex);
        if (m_cancel.load(std::memory_order_acquire) && m_snapshot.error.empty())
            m_snapshot.phase = ScanPhase::Cancelled;
        m_scanning.store(false, std::memory_order_release);
    }
    m_finished.store(true, std::memory_order_release);
}

namespace {

const std::vector<plugins::PluginDescriptor>& builtins() {
    static const std::vector<plugins::PluginDescriptor> list = plugins::builtinPlugins();
    return list;
}

} // namespace

std::vector<PluginDescriptor> PluginManager::plugins() const {
    // Built-ins first: they need no scan, they are always present, and a user
    // who has never scanned should still find an instrument in the menu.
    std::vector<PluginDescriptor> found = builtins();
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::vector<PluginDescriptor> scanned = m_cache.allPlugins();
    found.insert(found.end(), scanned.begin(), scanned.end());
    if (m_parameterFingerprint)
        for (auto& descriptor : found)
            if (!descriptor.parameterSchema.empty())
                descriptor.parameterFingerprint = m_parameterFingerprint(descriptor.parameterSchema);
    return found;
}

std::vector<PluginDescriptor> PluginManager::effects() const {
    std::vector<PluginDescriptor> found = plugins();
    std::erase_if(found, [](const PluginDescriptor& d) { return d.isInstrument || d.uid=="daw.channel-color"; });
    return found;
}

std::vector<PluginDescriptor> PluginManager::instruments() const {
    std::vector<PluginDescriptor> found = plugins();
    std::erase_if(found, [](const PluginDescriptor& d) { return !d.isInstrument; });
    return found;
}

std::optional<PluginDescriptor> PluginManager::find(Format format,
                                                    const std::string& uid) const {
    if (format == Format::Internal) {
        for (const PluginDescriptor& descriptor : builtins()) {
            if (descriptor.uid == uid) return descriptor;
        }
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const PluginCacheEntry& entry : m_cache.entries()) {
        if (!entry.ok || entry.blacklisted) continue;
        for (const PluginDescriptor& descriptor : entry.plugins) {
            if (descriptor.format == format && descriptor.uid == uid) {
                auto result = descriptor;
                if (m_parameterFingerprint && !result.parameterSchema.empty())
                    result.parameterFingerprint = m_parameterFingerprint(result.parameterSchema);
                return result;
            }
        }
    }
    return std::nullopt;
}

std::string PluginManager::probeSharedState(const PluginDescriptor& descriptor,
    const std::string& absoluteStatePath, double sampleRate) const {
    return sharedStateProbe(descriptor, absoluteStatePath, sampleRate)();
}

std::function<std::string()> PluginManager::sharedStateProbe(PluginDescriptor descriptor,
    std::string absoluteStatePath, double sampleRate) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return [descriptor = std::move(descriptor), absoluteStatePath = std::move(absoluteStatePath),
            sampleRate, scannerPath = m_scannerPath, timeout = m_timeout]() -> std::string {
    if (descriptor.format == Format::Internal) return {};
    if (descriptor.uid.empty() || descriptor.path.empty() || !std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 768000)
        return "Invalid plugin probe configuration";
    if (!absoluteStatePath.empty() && !platform::pathFromUtf8(absoluteStatePath).is_absolute())
        return "Plugin state cache path must be absolute";
    const auto probe = ScanProcess::run(scannerPath,
        {"--validate", "--format=" + std::string(plugins::toString(descriptor.format)),
         "--path=" + descriptor.path, "--uid=" + descriptor.uid,
         "--state=" + absoluteStatePath, "--sample-rate=" + std::to_string(sampleRate), "--shared-state"}, timeout);
    if (!probe.succeeded()) return probe.failureReason.empty() ? "Plugin state probe failed" : probe.failureReason;
    std::vector<PluginDescriptor> verified;
    if (!plugins::scan::decodeResult(probe.output, verified) || verified.size() != 1)
        return "Invalid plugin state probe response";
    const auto& actual = verified.front();
    if (actual.format != descriptor.format || actual.uid != descriptor.uid ||
        actual.vendor != descriptor.vendor || actual.version != descriptor.version ||
        actual.stateSchemaVersion != descriptor.stateSchemaVersion ||
        actual.parameterSchema.empty() || actual.parameterSchema != descriptor.parameterSchema)
        return "Plugin installation or parameter schema changed; rescan before joining";
    return {};
    };
}

std::vector<PluginManager::BlacklistEntry> PluginManager::blacklist() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<BlacklistEntry> found;
    for (const PluginCacheEntry& entry : m_cache.entries()) {
        if (entry.discovery == PluginScanState::Failed)
            found.push_back({entry.format, entry.path, entry.failureReason, entry.attempts, {}, {}});
        for (const auto& component : entry.components) {
            if (component.state == PluginScanState::Failed)
                found.push_back({entry.format, entry.path, component.failureReason,
                    component.attempts, component.uid, component.descriptor.name});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        if (a.format != b.format) return a.format < b.format;
        if (a.path != b.path) return a.path < b.path;
        return a.uid < b.uid;
    });
    return found;
}

namespace {
void resetFailures(PluginCacheEntry& entry, const std::string& uid) {
    if (uid.empty() && entry.discovery == PluginScanState::Failed) {
        entry.discovery = PluginScanState::Pending;
        entry.blacklisted = entry.timeoutRetryPending = false;
        entry.failureReason.clear();
        entry.attempts = 0;
        entry.discoveryDurationMs = 0;
    }
    for (auto& component : entry.components) {
        if (component.state != PluginScanState::Failed || (!uid.empty() && component.uid != uid)) continue;
        component.state = PluginScanState::Pending;
        component.failureReason.clear();
        component.attempts = 0;
        component.durationMs = 0;
        component.timeoutRetryPending = false;
    }
}
}

void PluginManager::unblacklist(Format format, const std::string& path, const std::string& uid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const auto* current = m_cache.find(format, path)) {
        auto entry = *current;
        resetFailures(entry, uid);
        m_cache.put(std::move(entry));
    }
    m_catalogueRevision.fetch_add(1, std::memory_order_release);
}

void PluginManager::clearBlacklist() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto entries = m_cache.entries();
    for (auto& entry : entries) {
        resetFailures(entry, {});
        m_cache.put(std::move(entry));
    }
    m_catalogueRevision.fetch_add(1, std::memory_order_release);
}

std::unique_ptr<plugins::PluginInstance> PluginManager::instantiate(
    const PluginDescriptor& descriptor) {
    return plugins::createHostedPlugin(descriptor, {m_hostingMode, m_pluginHostPath});
}

} // namespace daw
