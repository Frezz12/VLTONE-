#pragma once

#include "platform/AudioFileDecoder.hpp"
#include "Audio/SampleBuffer.hpp"
#include <functional>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace daw {

/// A min/max peak envelope for one audio file: two values per bucket, computed
/// from the mono sum of all channels. This is what the arrangement draws inside
/// a clip — reading the file per repaint would be far too slow.
struct WaveformPeaks {
    struct Level {
        std::vector<float> minima;
        std::vector<float> maxima;
        double bucketsPerSecond = 0.0;
    };
    std::vector<float> minima;
    std::vector<float> maxima;
    /// Successively 4x coarser envelopes for zoomed-out drawing.
    std::vector<Level> levels;
    double durationSeconds = 0.0;
    double bucketsPerSecond = 0.0;
    int channels = 0;             // source channel count

    bool isValid() const { return !minima.empty() && bucketsPerSecond > 0.0; }
    size_t bucketCount() const { return minima.size(); }
};

/// Buckets per second of audio (~1 ms per bucket). Dense enough that even at
/// the maximum timeline zoom there is roughly one peak per screen pixel, so the
/// drawn waveform stays smooth and detailed instead of blocky.
inline constexpr double kPeakBucketsPerSecond = 1000.0;

/// Build an envelope from audio somebody else decoded.
///
/// Free-standing so a worker thread can produce peaks without going near the
/// cache below, which is unsynchronised and read from paint handlers. The
/// browser decodes a file once on a worker and gets both the audio and its
/// envelope out of that single decode.
void buildPeaks(const audio::platform::DecodedAudio& decoded, WaveformPeaks& out);
void buildPeaks(const engine::SampleBuffer& buffer, WaveformPeaks& out,
                const std::function<bool()>& keepGoing = {});

/// Decodes files once and keeps their envelopes, keyed by absolute path.
/// Control-thread only: `peaks()` decodes on a miss, which is slow for long
/// files, so warm it at import/load time rather than from a paint handler.
class WaveformCache {
public:
    static constexpr std::size_t kDefaultByteBudget = 256u * 1024u * 1024u;

    explicit WaveformCache(std::size_t byteBudget = kDefaultByteBudget)
        : m_byteBudget(byteBudget) {}

    /// Envelope for `filePath`, computing it on first use. Returns nullptr when
    /// the file cannot be decoded (the failure is remembered, so a broken path
    /// is not re-decoded on every call).
    const WaveformPeaks* peaks(const std::string& filePath);

    /// Already-computed envelope, or nullptr — never decodes. Safe to call from
    /// drawing code.
    const WaveformPeaks* cached(const std::string& filePath) const;

    /// Seed the cache from a decode the playback path already performed.
    /// Avoids reading and allocating the same full file again for its envelope.
    const WaveformPeaks* storeDecoded(
        const std::string& filePath,
        const audio::platform::DecodedAudio& decoded);

    void clear();
    const WaveformPeaks* storeSample(const std::string& filePath,
                                     const engine::SampleBuffer& buffer);
    std::size_t byteSize() const noexcept { return m_bytes; }
    std::size_t entryCount() const noexcept { return m_cache.size(); }

    /// Kept as a member name for the code that already says
    /// `WaveformCache::kBucketsPerSecond`; the value lives at namespace scope
    /// so `buildPeaks` can use it too.
    static constexpr double kBucketsPerSecond = kPeakBucketsPerSecond;

private:
    struct Entry {
        std::shared_ptr<WaveformPeaks> peaks = std::make_shared<WaveformPeaks>();
        std::size_t bytes = 0;
        mutable std::uint64_t lastUse = 0;
    };
    static std::size_t waveformBytes(const WaveformPeaks& peaks) noexcept;
    void touch(Entry& entry) const noexcept;
    void evictToBudget(const std::string& protectedPath);

    mutable std::unordered_map<std::string, Entry> m_cache;
    std::size_t m_byteBudget = kDefaultByteBudget;
    std::size_t m_bytes = 0;
    mutable std::uint64_t m_useClock = 0;
};

} // namespace daw
