#include "ai/ContentCatalog.hpp"

#include "MidiFile.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace fs = std::filesystem;

namespace daw::ai {
namespace {

constexpr std::size_t kMaxSearchResults = 200;

char lowerAscii(char ch) noexcept {
    return ch >= 'A' && ch <= 'Z' ? char(ch + ('a' - 'A')) : ch;
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), lowerAscii);
    return value;
}

std::string pathKey(const fs::path& path) {
    std::string key = platform::pathToUtf8(path.lexically_normal());
#if defined(_WIN32)
    key = asciiLower(std::move(key));
#endif
    return key;
}

bool sameComponent(const fs::path& left, const fs::path& right) {
#if defined(_WIN32)
    return asciiLower(platform::pathToUtf8(left)) ==
           asciiLower(platform::pathToUtf8(right));
#else
    return left == right;
#endif
}

bool isWithin(const fs::path& candidate, const fs::path& root) {
    auto child = candidate.begin();
    for (auto parent = root.begin(); parent != root.end(); ++parent, ++child) {
        if (child == candidate.end() || !sameComponent(*child, *parent))
            return false;
    }
    return true;
}

std::optional<ContentType> contentTypeFor(const fs::path& path) {
    std::string extension = platform::pathToUtf8(path.extension());
    if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
    extension = asciiLower(std::move(extension));
    if (extension == "mid" || extension == "midi") return ContentType::Midi;
    if (audio::platform::isDecodableExtension(extension))
        return ContentType::Audio;
    return std::nullopt;
}

std::uint64_t fnv1a(std::string_view value, std::uint64_t seed) noexcept {
    std::uint64_t hash = 14695981039346656037ull ^ seed;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 16> result{};
    for (auto it = result.rbegin(); it != result.rend(); ++it) {
        *it = digits[value & 0x0f];
        value >>= 4;
    }
    return std::string(result.data(), result.size());
}

std::string opaqueId(std::string_view canonicalKey) {
    // Two independent 64-bit passes make accidental collisions irrelevant for
    // a local catalog. The id is stable, but contains no reversible path text.
    return "content_" + hex64(fnv1a(canonicalKey, 0x243f6a8885a308d3ull)) +
           hex64(fnv1a(canonicalKey, 0x13198a2e03707344ull));
}

std::optional<AudioTimbreMetadata>
readLightTimbre(const fs::path& path, std::stop_token stop) {
    audio::platform::AudioFileReader reader;
    if (!reader.open(platform::pathToUtf8(path)).isOk()) return std::nullopt;
    const audio::platform::AudioFileInfo info = reader.info();
    if (info.frames == 0 || info.channels == 0 || info.sampleRate <= 0.0)
        return std::nullopt;

    // A handful of windows is enough to separate dark pads, bright hats,
    // transient drums and sustained material without decoding a whole song.
    constexpr std::uint64_t kWindowFrames = 4096;
    constexpr std::size_t kMaxWindows = 8;
    constexpr std::size_t kEnvelopeFrames = 256;
    const std::uint64_t totalFrames = std::uint64_t(info.frames);
    const std::uint64_t windowFrames =
        std::min(kWindowFrames, totalFrames);
    const std::size_t windows = std::size_t(std::min<std::uint64_t>(
        kMaxWindows, (totalFrames + windowFrames - 1) / windowFrames));
    const std::size_t channels = std::size_t(info.channels);
    if (windowFrames > std::numeric_limits<std::size_t>::max() / channels)
        return std::nullopt;
    std::vector<float> samples(std::size_t(windowFrames) * channels);

    long double energySquares = 0.0;
    long double monoSquares = 0.0;
    long double differenceSquares = 0.0;
    long double midSquares = 0.0;
    long double sideSquares = 0.0;
    double peak = 0.0;
    double maxEnvelopeRms = 0.0;
    std::uint64_t analyzedFrames = 0;
    std::uint64_t differences = 0;
    std::uint64_t crossings = 0;

    for (std::size_t window = 0; window < windows; ++window) {
        if (stop.stop_requested()) return std::nullopt;
        const std::uint64_t start = totalFrames <= kWindowFrames * kMaxWindows
                                        ? std::uint64_t(window) * kWindowFrames
                                        : (windows == 1
                                               ? 0
                                               : std::uint64_t(window) *
                                                     (totalFrames - windowFrames) /
                                                     std::uint64_t(windows - 1));
        if (!reader.seek(audio::FrameCount(start)).isOk())
            return std::nullopt;
        const audio::FrameCount wanted = audio::FrameCount(
            std::min(windowFrames, totalFrames - start));
        const audio::FrameCount received = reader.read(samples.data(), wanted);
        if (received == 0) continue;

        bool havePrevious = false;
        double previous = 0.0;
        long double envelopeSquares = 0.0;
        std::size_t envelopeCount = 0;
        for (audio::FrameCount frame = 0; frame < received; ++frame) {
            const std::size_t offset = std::size_t(frame) * channels;
            double mono = 0.0;
            double channelSquares = 0.0;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const double sample = samples[offset + channel];
                mono += sample;
                channelSquares += sample * sample;
                peak = std::max(peak, std::abs(sample));
            }
            mono /= double(channels);

            const double square = channelSquares / double(channels);
            energySquares += square;
            monoSquares += mono * mono;
            envelopeSquares += square;
            ++envelopeCount;
            if (havePrevious) {
                const double difference = mono - previous;
                differenceSquares += difference * difference;
                if ((mono >= 0.0) != (previous >= 0.0)) ++crossings;
                ++differences;
            }
            previous = mono;
            havePrevious = true;

            if (channels >= 2) {
                const double mid =
                    0.5 * (double(samples[offset]) + samples[offset + 1]);
                const double side =
                    0.5 * (double(samples[offset]) - samples[offset + 1]);
                midSquares += mid * mid;
                sideSquares += side * side;
            }

            if (envelopeCount == kEnvelopeFrames || frame + 1 == received) {
                maxEnvelopeRms = std::max(
                    maxEnvelopeRms,
                    std::sqrt(double(envelopeSquares / envelopeCount)));
                envelopeSquares = 0.0;
                envelopeCount = 0;
            }
        }
        analyzedFrames += std::uint64_t(received);
    }

    if (analyzedFrames == 0) return std::nullopt;
    const double rms = std::sqrt(double(energySquares / analyzedFrames));
    const auto dbfs = [](double amplitude) {
        return amplitude > 1e-6
                   ? std::max(-120.0, 20.0 * std::log10(amplitude))
                   : -120.0;
    };
    const double midRms = std::sqrt(double(midSquares / analyzedFrames));
    const double sideRms = std::sqrt(double(sideSquares / analyzedFrames));

    AudioTimbreMetadata result;
    result.rmsDbfs = dbfs(rms);
    result.peakDbfs = dbfs(peak);
    result.crestFactorDb = rms > 1e-9
                               ? std::max(0.0, result.peakDbfs - result.rmsDbfs)
                               : 0.0;
    result.brightness = monoSquares > 1e-18
                            ? std::clamp(std::sqrt(double(
                                             differenceSquares /
                                             (4.0L * monoSquares))),
                                         0.0, 1.0)
                            : 0.0;
    result.transientness =
        rms > 1e-9
            ? std::clamp((maxEnvelopeRms / rms - 1.0) / 3.0, 0.0, 1.0)
            : 0.0;
    result.stereoWidth = channels >= 2 && midRms + sideRms > 1e-9
                             ? std::clamp(sideRms / (midRms + sideRms), 0.0,
                                          1.0)
                             : 0.0;
    result.zeroCrossingRate = differences > 0
                                  ? double(crossings) / double(differences)
                                  : 0.0;
    result.sampledSeconds = double(analyzedFrames) / info.sampleRate;
    return result;
}

} // namespace

std::string_view toString(ContentType type) noexcept {
    return type == ContentType::Audio ? "audio" : "midi";
}

std::string_view toString(CatalogIndexState state) noexcept {
    switch (state) {
        case CatalogIndexState::Idle: return "idle";
        case CatalogIndexState::Scanning: return "scanning";
        case CatalogIndexState::ReadingMetadata: return "reading_metadata";
        case CatalogIndexState::CancelRequested: return "cancel_requested";
        case CatalogIndexState::Ready: return "ready";
        case CatalogIndexState::Cancelled: return "cancelled";
    }
    return "idle";
}

std::optional<double> CatalogIndexStatus::progress() const noexcept {
    if (state == CatalogIndexState::Scanning ||
        state == CatalogIndexState::CancelRequested)
        return std::nullopt;
    if (state == CatalogIndexState::Idle) return 0.0;
    if (state == CatalogIndexState::Ready) return 1.0;
    if (filesDiscovered == 0)
        return state == CatalogIndexState::Cancelled ? 0.0 : 1.0;
    return std::clamp(double(filesProcessed) / double(filesDiscovered), 0.0,
                      1.0);
}

ContentCatalog::ContentCatalog(std::vector<std::string> browserRoots) {
    setBrowserRoots(std::move(browserRoots));
}

ContentCatalog::~ContentCatalog() {
    std::jthread worker;
    {
        std::scoped_lock lock(m_workerMutex);
        if (m_worker.joinable()) m_worker.request_stop();
        worker = std::move(m_worker);
    }
}

BrowserGrantReport
ContentCatalog::setBrowserRoots(std::vector<std::string> browserRoots) {
    BrowserGrantReport report;
    std::vector<fs::path> accepted;
    std::unordered_set<std::string> seen;

    for (const std::string& utf8Root : browserRoots) {
        std::error_code ec;
        const fs::path canonical =
            fs::canonical(platform::pathFromUtf8(utf8Root), ec);
        if (ec || !fs::is_directory(canonical, ec) || ec) {
            ++report.rejected;
            continue;
        }
        if (seen.insert(pathKey(canonical)).second) {
            accepted.push_back(canonical);
            ++report.accepted;
        }
    }

    std::sort(accepted.begin(), accepted.end(), [](const fs::path& a,
                                                    const fs::path& b) {
        return pathKey(a) < pathKey(b);
    });

    {
        std::scoped_lock lock(m_mutex);
        if (accepted == m_roots) return report;
    }
    cancelRefresh();
    std::scoped_lock lock(m_mutex);
    if (accepted == m_roots) return report;
    m_roots = std::move(accepted);
    m_cache.clear();
    m_items.clear();
    m_byId.clear();
    ++m_generation;
    m_status = {};
    m_status.generation = m_generation;
    return report;
}

CatalogRefreshReport ContentCatalog::refresh() {
    const CatalogIndexStatus before = status();
    if (!before.running()) startRefresh();
    {
        std::scoped_lock lock(m_workerMutex);
        if (m_worker.joinable()) m_worker.join();
    }
    return status().report;
}

bool ContentCatalog::startRefresh() {
    std::scoped_lock workerLock(m_workerMutex);
    std::vector<fs::path> roots;
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(m_mutex);
        if (m_status.running()) return false;
        roots = m_roots;
        generation = m_generation;
    }

    if (m_worker.joinable()) m_worker.join();
    {
        std::scoped_lock lock(m_mutex);
        if (generation != m_generation) return false;
        m_status = {};
        m_status.state = CatalogIndexState::Scanning;
        m_status.generation = generation;
    }
    m_worker = std::jthread(
        [this, roots = std::move(roots), generation](std::stop_token stop) {
            runRefresh(stop, roots, generation);
        });
    return true;
}

void ContentCatalog::cancelRefresh() noexcept {
    {
        std::scoped_lock lock(m_workerMutex);
        if (m_worker.joinable()) m_worker.request_stop();
    }
    std::scoped_lock lock(m_mutex);
    if (m_status.state == CatalogIndexState::Scanning ||
        m_status.state == CatalogIndexState::ReadingMetadata)
        m_status.state = CatalogIndexState::CancelRequested;
}

CatalogIndexStatus ContentCatalog::status() const {
    std::scoped_lock lock(m_mutex);
    return m_status;
}

void ContentCatalog::runRefresh(std::stop_token stop,
                                std::vector<fs::path> roots,
                                std::uint64_t generation) {
    CatalogRefreshReport report;

    struct Candidate {
        fs::path path;
        std::string key;
        ContentType type = ContentType::Audio;
    };
    std::vector<Candidate> candidates;
    std::unordered_set<std::string> seen;

    const auto cancelled = [&] {
        if (!stop.stop_requested()) return false;
        std::scoped_lock lock(m_mutex);
        if (generation == m_generation) {
            m_status.state = CatalogIndexState::Cancelled;
            m_status.report = report;
            m_status.report.indexed = m_status.filesPublished;
        }
        return true;
    };

    for (const fs::path& root : roots) {
        if (cancelled()) return;
        std::error_code ec;
        fs::recursive_directory_iterator iterator(
            root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        if (ec) {
            ++report.skippedUnsafe;
            continue;
        }

        while (iterator != end) {
            if (cancelled()) return;
            const fs::directory_entry entry = *iterator;
            std::error_code itemError;
            if (entry.is_directory(itemError)) {
                const fs::path resolved = fs::canonical(entry.path(), itemError);
                if (itemError || !isWithin(resolved, root)) {
                    iterator.disable_recursion_pending();
                    ++report.skippedUnsafe;
                }
            } else if (!itemError && entry.is_regular_file(itemError)) {
                const fs::path resolved = fs::canonical(entry.path(), itemError);
                if (itemError || !isWithin(resolved, root)) {
                    ++report.skippedUnsafe;
                } else {
                    const std::optional<ContentType> type =
                        contentTypeFor(resolved);
                    const std::string key = pathKey(resolved);
                    if (type && seen.insert(key).second) {
                        candidates.push_back({resolved, key, *type});
                        std::scoped_lock lock(m_mutex);
                        if (generation != m_generation) return;
                        m_status.filesDiscovered = candidates.size();
                    }
                }
            }

            iterator.increment(ec);
            if (ec) {
                ++report.skippedUnsafe;
                ec.clear();
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.key < b.key;
              });

    {
        std::scoped_lock lock(m_mutex);
        if (generation != m_generation) return;
        m_status.state = CatalogIndexState::ReadingMetadata;
        m_status.filesDiscovered = candidates.size();
        m_status.report = report;
    }

    std::unordered_set<std::string> liveCacheKeys;
    std::size_t processed = 0;

    for (const Candidate& candidate : candidates) {
        if (cancelled()) return;
        std::error_code ec;
        const fs::file_time_type modified =
            fs::last_write_time(candidate.path, ec);
        if (ec) {
            ++report.skippedUnsafe;
            ++processed;
            std::scoped_lock lock(m_mutex);
            if (generation != m_generation) return;
            m_status.filesProcessed = processed;
            m_status.report = report;
            continue;
        }

        CachedFacts facts;
        bool cacheHit = false;
        {
            std::scoped_lock lock(m_mutex);
            if (generation != m_generation) return;
            const auto cached = m_cache.find(candidate.key);
            if (cached != m_cache.end() &&
                cached->second.modified == modified &&
                cached->second.type == candidate.type) {
                facts = cached->second;
                cacheHit = true;
            }
        }
        if (cacheHit) {
            ++report.cacheHits;
        } else {
            facts.modified = modified;
            facts.type = candidate.type;
            facts.sizeBytes = fs::file_size(candidate.path, ec);
            if (ec) {
                facts.sizeBytes = 0;
                ec.clear();
            }

            const std::string utf8Path =
                platform::pathToUtf8(candidate.path);
            if (candidate.type == ContentType::Audio) {
                audio::platform::AudioFileInfo info;
                if (audio::platform::probeAudioFile(utf8Path, info).isOk()) {
                    facts.audio = AudioContentMetadata{
                        std::uint64_t(info.frames), info.sampleRate,
                        int(info.channels), info.durationSeconds(),
                        readLightTimbre(candidate.path, stop)};
                    facts.metadataAvailable = true;
                    if (stop.stop_requested()) {
                        cancelled();
                        return;
                    }
                    if (!facts.audio->timbre) ++report.timbreFailures;
                }
            } else {
                midifile::File file;
                std::string error;
                if (midifile::parse(utf8Path, file, error)) {
                    facts.midi = MidiContentMetadata{
                        file.format,
                        file.trackCount,
                        file.tracksWithNotes(),
                        file.ticksPerQuarter,
                        file.notes.size(),
                        file.lengthBeats,
                        file.firstTempoBpm,
                        file.hasTempoChanges};
                    facts.metadataAvailable = true;
                }
            }
        }

        if (!facts.metadataAvailable) ++report.metadataFailures;
        liveCacheKeys.insert(candidate.key);

        ContentItem visible;
        visible.contentId = opaqueId(candidate.key);
        visible.name = platform::pathToUtf8(candidate.path.filename());
        visible.type = candidate.type;
        visible.sizeBytes = facts.sizeBytes;
        visible.audio = facts.audio;
        visible.midi = facts.midi;

        {
            std::scoped_lock lock(m_mutex);
            if (generation != m_generation) return;
            m_cache[candidate.key] = facts;

            // The double hash makes this theoretical, but keep collision
            // handling deterministic instead of ever resolving an id to the
            // wrong file.
            std::size_t collision = 0;
            auto existing = m_byId.find(visible.contentId);
            while (existing != m_byId.end() &&
                   m_items[existing->second].canonicalKey != candidate.key) {
                visible.contentId = opaqueId(candidate.key + "#" +
                                             std::to_string(++collision));
                existing = m_byId.find(visible.contentId);
            }

            if (existing != m_byId.end()) {
                m_items[existing->second] =
                    {std::move(visible), candidate.path, candidate.key};
            } else {
                m_byId.emplace(visible.contentId, m_items.size());
                m_items.push_back(
                    {std::move(visible), candidate.path, candidate.key});
            }
            ++processed;
            report.indexed = m_items.size();
            m_status.filesProcessed = processed;
            m_status.filesPublished = m_items.size();
            m_status.report = report;
        }
    }

    {
        std::scoped_lock lock(m_mutex);
        if (generation != m_generation) return;
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (!liveCacheKeys.contains(it->first)) it = m_cache.erase(it);
            else ++it;
        }
        std::erase_if(m_items, [&](const IndexedItem& item) {
            return !liveCacheKeys.contains(item.canonicalKey);
        });
        m_byId.clear();
        for (std::size_t index = 0; index < m_items.size(); ++index)
            m_byId.emplace(m_items[index].visible.contentId, index);
        report.indexed = m_items.size();
        m_status.state = CatalogIndexState::Ready;
        m_status.filesProcessed = processed;
        m_status.filesPublished = m_items.size();
        m_status.report = report;
    }
}

std::vector<ContentItem>
ContentCatalog::search(std::string_view query, std::optional<ContentType> type,
                       std::size_t limit) const {
    std::scoped_lock lock(m_mutex);
    if (limit == 0) return {};
    limit = std::min(limit, kMaxSearchResults);
    const std::string needle = asciiLower(std::string(query));

    struct Match {
        const ContentItem* item = nullptr;
        std::string lowerName;
        std::size_t position = 0;
    };
    std::vector<Match> matches;
    for (const IndexedItem& indexed : m_items) {
        if (type && indexed.visible.type != *type) continue;
        std::string lowerName = asciiLower(indexed.visible.name);
        const std::size_t position = lowerName.find(needle);
        if (position == std::string::npos) continue;
        matches.push_back({&indexed.visible, std::move(lowerName), position});
    }

    std::sort(matches.begin(), matches.end(), [](const Match& a,
                                                  const Match& b) {
        if (a.position != b.position) return a.position < b.position;
        if (a.lowerName != b.lowerName) return a.lowerName < b.lowerName;
        return a.item->contentId < b.item->contentId;
    });

    std::vector<ContentItem> result;
    result.reserve(std::min(limit, matches.size()));
    for (const Match& match : matches) {
        if (result.size() == limit) break;
        result.push_back(*match.item);
    }
    return result;
}

std::optional<std::string>
ContentCatalog::resolvePath(std::string_view contentId) const {
    std::scoped_lock lock(m_mutex);
    const auto found = m_byId.find(std::string(contentId));
    if (found == m_byId.end() || found->second >= m_items.size())
        return std::nullopt;

    const IndexedItem& indexed = m_items[found->second];
    std::error_code ec;
    const fs::path current = fs::canonical(indexed.canonicalPath, ec);
    if (ec || !fs::is_regular_file(current, ec) || ec ||
        pathKey(current) != indexed.canonicalKey)
        return std::nullopt;

    bool granted = false;
    for (const fs::path& root : m_roots) {
        if (isWithin(current, root)) {
            granted = true;
            break;
        }
    }
    if (!granted) return std::nullopt;

    const std::optional<ContentType> currentType = contentTypeFor(current);
    if (!currentType || *currentType != indexed.visible.type)
        return std::nullopt;
    return platform::pathToUtf8(current);
}

std::size_t ContentCatalog::size() const {
    std::scoped_lock lock(m_mutex);
    return m_items.size();
}

} // namespace daw::ai
