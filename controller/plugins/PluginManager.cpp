#include "plugins/PluginManager.hpp"

#include "Internal/InternalFactory.hpp"
#include "Scan/ScanProtocol.hpp"
#include "plugins/ScanProcess.hpp"
#include "crash/CrashHandler.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

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
    : m_cachePath(std::move(cachePath)), m_scannerPath(defaultScannerPath()) {}

PluginManager::~PluginManager() {
    cancelScan();
    waitForScan();
}

std::string PluginManager::defaultScannerPath() {
    const std::string directory = executableDirectory();
    if (directory.empty()) return "daw_scan";
#if defined(_WIN32)
    return platform::pathToUtf8(platform::pathFromUtf8(directory) /
                                "daw_scan.exe");
#else
    return platform::pathToUtf8(platform::pathFromUtf8(directory) /
                                "daw_scan");
#endif
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
            // Enumeration only looks at names and bundle shapes — it never
            // opens a module — so it is safe to run here rather than paying a
            // process launch per directory.
            for (std::string& candidate : factory->enumerateCandidates(directory)) {
                candidates.push_back(Candidate{format, std::move(candidate)});
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

void PluginManager::startScan(bool rescanAll) {
    if (m_scanning.exchange(true, std::memory_order_acq_rel)) return;
    waitForScan();   // join a previous, already-finished worker
    m_cancel.store(false, std::memory_order_release);
    m_scanned.store(0, std::memory_order_relaxed);
    m_total.store(0, std::memory_order_relaxed);
    m_worker = std::thread([this, rescanAll] { scanWorker(rescanAll); });
}

void PluginManager::cancelScan() {
    m_cancel.store(true, std::memory_order_release);
}

void PluginManager::waitForScan() {
    if (m_worker.joinable()) m_worker.join();
}

void PluginManager::scanWorker(bool rescanAll) {
    const std::vector<Candidate> candidates = collectCandidates();
    m_total.store(std::uint32_t(candidates.size()), std::memory_order_relaxed);

    for (const Candidate& candidate : candidates) {
        if (m_cancel.load(std::memory_order_acquire)) break;
        {
            std::lock_guard<std::mutex> lock(m_currentMutex);
            m_currentPath = candidate.path;
        }

        std::uint64_t size = 0;
        std::int64_t modified = 0;
        statFile(candidate.path, size, modified);

        if (!rescanAll) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const PluginCacheEntry* existing =
                m_cache.find(candidate.format, candidate.path);
            const bool reusableSuccess = existing && existing->ok &&
                                         !existing->blacklisted;
            const bool repeatedlyBroken = existing && existing->blacklisted &&
                                          existing->attempts >= 3;
            if (existing && (reusableSuccess || repeatedlyBroken) &&
                PluginCache::isCurrent(*existing, size, modified)) {
                m_scanned.fetch_add(1, std::memory_order_relaxed);
                continue;   // unchanged on disk: no process launch at all
            }
        }

        PluginCacheEntry entry;
        entry.format = candidate.format;
        entry.path = candidate.path;
        entry.fileSize = size;
        entry.fileModifiedTime = modified;
        entry.schemaVersion = plugins::scan::kSchemaVersion;

        const std::vector<std::string> arguments = {
            "--inspect",
            "--format=" + std::string(plugins::toString(candidate.format)),
            "--path=" + candidate.path,
        };
        const ScanProcessResult result =
            ScanProcess::run(m_scannerPath, arguments, m_timeout);

        if (result.succeeded() &&
            plugins::scan::decodeResult(result.output, entry.plugins) &&
            !entry.plugins.empty()) {
            entry.ok = true;

            // Metadata inspection is not a compatibility proof. Validate every
            // external format in a disposable process so CLAP/AU failures are
            // contained just like VST/VST3 failures and cached readiness means
            // the class has instantiated, activated and processed one block.
            if (candidate.format != Format::Internal &&
                candidate.format != Format::Unknown) {
                std::vector<PluginDescriptor> validated;
                for (const PluginDescriptor& descriptor : entry.plugins) {
                    const std::vector<std::string> validateArguments = {
                        "--validate",
                        "--format=" + std::string(plugins::toString(candidate.format)),
                        "--path=" + candidate.path,
                        "--uid=" + descriptor.uid,
                    };
                    const ScanProcessResult validation =
                        ScanProcess::run(m_scannerPath, validateArguments, m_timeout);
                    std::vector<PluginDescriptor> confirmed;
                    if (validation.succeeded() &&
                        plugins::scan::decodeResult(validation.output, confirmed) &&
                        !confirmed.empty()) {
                        // Validation has the live answer for capabilities such
                        // as an editor; keep it instead of accidentally putting
                        // the inspect-time placeholder back into the cache.
                        validated.push_back(std::move(confirmed.front()));
                    } else if (entry.failureReason.empty()) {
                        entry.failureReason = validation.failureReason.empty()
                                                  ? "a plugin component failed initialization"
                                                  : validation.failureReason;
                    }
                }
                entry.plugins = std::move(validated);
                entry.ok = !entry.plugins.empty();
            }
        } else {
            // Anything that is not a clean, parseable success is a blacklist:
            // a plugin that crashed the scanner would crash the DAW, and one
            // that produced nothing usable has nothing to offer anyway.
            entry.ok = false;
            entry.blacklisted = true;
            entry.failureReason = result.failureReason.empty()
                                      ? "the scanner returned nothing usable"
                                      : result.failureReason;
        }

        if (!entry.ok) entry.blacklisted = true;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (const PluginCacheEntry* previous =
                    m_cache.find(candidate.format, candidate.path)) {
                entry.attempts = previous->attempts;
            }
            if (entry.ok) entry.attempts = 0;
            else ++entry.attempts;
            m_cache.put(std::move(entry));
        }
        m_scanned.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(m_currentMutex);
        m_currentPath.clear();
    }
    save();
    m_scanning.store(false, std::memory_order_release);
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
    return found;
}

std::vector<PluginDescriptor> PluginManager::effects() const {
    std::vector<PluginDescriptor> found = plugins();
    std::erase_if(found, [](const PluginDescriptor& d) { return d.isInstrument; });
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
            if (descriptor.format == format && descriptor.uid == uid) return descriptor;
        }
    }
    return std::nullopt;
}

std::vector<PluginManager::BlacklistEntry> PluginManager::blacklist() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<BlacklistEntry> found;
    for (const PluginCacheEntry& entry : m_cache.entries()) {
        if (!entry.blacklisted) continue;
        found.push_back(BlacklistEntry{entry.format, entry.path, entry.failureReason,
                                       entry.attempts});
    }
    return found;
}

void PluginManager::unblacklist(Format format, const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Drop the entry outright rather than clearing the flag: the next scan then
    // treats it as never seen and tries it again from scratch.
    m_cache.remove(format, path);
}

void PluginManager::clearBlacklist() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PluginCacheEntry> keep;
    for (const PluginCacheEntry& entry : m_cache.entries()) {
        if (!entry.blacklisted) keep.push_back(entry);
    }
    m_cache.clear();
    for (PluginCacheEntry& entry : keep) m_cache.put(std::move(entry));
}

std::unique_ptr<plugins::PluginInstance> PluginManager::instantiate(
    const PluginDescriptor& descriptor) {
    plugins::PluginFactory* factory = plugins::factoryFor(descriptor.format);
    if (!factory) return nullptr;
    // Loading is where third-party code most often takes the program down with
    // it, and it is the one moment cheap enough to mark. If the fault happens
    // here, the crash marker names the plugin instead of leaving the user with
    // an anonymous stack.
    crash::setPluginInFlight(descriptor.name);
    auto instance = factory->create(descriptor);
    crash::clearPluginInFlight();
    return instance;
}

} // namespace daw
