#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace daw::ai {

enum class ContentType { Audio, Midi };

std::string_view toString(ContentType type) noexcept;

enum class CatalogIndexState {
    Idle,
    Scanning,
    ReadingMetadata,
    CancelRequested,
    Ready,
    Cancelled,
};

std::string_view toString(CatalogIndexState state) noexcept;

struct AudioTimbreMetadata {
    double rmsDbfs = -120.0;
    double peakDbfs = -120.0;
    double crestFactorDb = 0.0;
    /// Zero is smooth/low-frequency, one approaches alternating samples.
    double brightness = 0.0;
    double transientness = 0.0;
    double stereoWidth = 0.0;
    double zeroCrossingRate = 0.0;
    double sampledSeconds = 0.0;
};

struct AudioContentMetadata {
    std::uint64_t frames = 0;
    double sampleRate = 0.0;
    int channels = 0;
    double durationSeconds = 0.0;
    std::optional<AudioTimbreMetadata> timbre;
};

struct MidiContentMetadata {
    int format = 0;
    int trackCount = 0;
    int tracksWithNotes = 0;
    int ticksPerQuarter = 0;
    std::size_t noteCount = 0;
    double lengthBeats = 0.0;
    double firstTempoBpm = 0.0;
    bool hasTempoChanges = false;
};

/// Safe, model-facing description of a browser item. It deliberately has no
/// path: native code must exchange `contentId` for one through `resolvePath`.
struct ContentItem {
    std::string contentId;
    std::string name;
    ContentType type = ContentType::Audio;
    std::uintmax_t sizeBytes = 0;
    std::optional<AudioContentMetadata> audio;
    std::optional<MidiContentMetadata> midi;
};

struct BrowserGrantReport {
    std::size_t accepted = 0;
    std::size_t rejected = 0;
};

struct CatalogRefreshReport {
    std::size_t indexed = 0;
    std::size_t cacheHits = 0;
    std::size_t metadataFailures = 0;
    std::size_t timbreFailures = 0;
    std::size_t skippedUnsafe = 0;
};

struct CatalogIndexStatus {
    CatalogIndexState state = CatalogIndexState::Idle;
    std::uint64_t generation = 0;
    std::size_t filesDiscovered = 0;
    std::size_t filesProcessed = 0;
    std::size_t filesPublished = 0;
    CatalogRefreshReport report;

    bool running() const noexcept {
        return state == CatalogIndexState::Scanning ||
               state == CatalogIndexState::ReadingMetadata ||
               state == CatalogIndexState::CancelRequested;
    }

    /// Returns 0...1 once discovery has established a total. During directory
    /// scanning the total is unknown, so callers should show an indeterminate
    /// progress indicator.
    std::optional<double> progress() const noexcept;
};

/// In-memory index over the folders the user explicitly added to the browser.
/// Every path is canonicalised before indexing and again before resolution, so
/// a symlink/junction cannot turn an already-issued id into an escape.
class ContentCatalog {
public:
    ContentCatalog() = default;
    explicit ContentCatalog(std::vector<std::string> browserRoots);
    ~ContentCatalog();

    ContentCatalog(const ContentCatalog&) = delete;
    ContentCatalog& operator=(const ContentCatalog&) = delete;

    /// Replaces all grants and revokes every id issued for the previous set.
    /// Paths use the controller's UTF-8 convention.
    BrowserGrantReport setBrowserRoots(std::vector<std::string> browserRoots);

    /// Legacy blocking refresh, retained for tests and headless callers.
    CatalogRefreshReport refresh();

    /// Starts one background refresh. Search continues to read the last
    /// published index while folders are scanned, then sees new items as their
    /// metadata becomes available. Returns false when a refresh is running.
    bool startRefresh();

    /// Requests cooperative cancellation and returns immediately.
    void cancelRefresh() noexcept;

    CatalogIndexStatus status() const;

    /// ASCII case-insensitive filename search. An empty query lists by name.
    std::vector<ContentItem> search(
        std::string_view query = {},
        std::optional<ContentType> type = std::nullopt,
        std::size_t limit = 40) const;

    /// Trusted native-code boundary. Returns no path when the id is unknown,
    /// its grant was revoked, or its target now resolves outside a grant.
    std::optional<std::string> resolvePath(std::string_view contentId) const;

    std::size_t size() const;

private:
    struct CachedFacts {
        std::filesystem::file_time_type modified{};
        ContentType type = ContentType::Audio;
        std::uintmax_t sizeBytes = 0;
        bool metadataAvailable = false;
        std::optional<AudioContentMetadata> audio;
        std::optional<MidiContentMetadata> midi;
    };

    struct IndexedItem {
        ContentItem visible;
        std::filesystem::path canonicalPath;
        std::string canonicalKey;
    };

    mutable std::mutex m_mutex;
    std::vector<std::filesystem::path> m_roots;
    std::unordered_map<std::string, CachedFacts> m_cache;
    std::vector<IndexedItem> m_items;
    std::unordered_map<std::string, std::size_t> m_byId;
    std::uint64_t m_generation = 0;
    CatalogIndexStatus m_status;

    mutable std::mutex m_workerMutex;
    std::jthread m_worker;

    void runRefresh(std::stop_token stop,
                    std::vector<std::filesystem::path> roots,
                    std::uint64_t generation);
};

} // namespace daw::ai
