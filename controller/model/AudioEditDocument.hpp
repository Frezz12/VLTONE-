#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

using AudioEditFrame = std::int64_t;

/// All positions are in document-rate frames. Source offsets may be fractional
/// after importing another sample rate, but editing boundaries are integral.
struct AudioEditSource {
    std::string id;
    std::string filePath;
    double sampleRate = 48000;
    AudioEditFrame frames = 0;
    int channels = 1;
    std::string assetId;
    std::string sha256;
    friend bool operator==(const AudioEditSource&, const AudioEditSource&) = default;
};

struct AudioEditEnvelope {
    double first = 0, last = 1; // region-local frames; can extend outside a slice
    double from = 1, to = 1;
    double curve = 0;
    bool mirrored = false;
    friend bool operator==(const AudioEditEnvelope&, const AudioEditEnvelope&) = default;
};

struct AudioEditRegion {
    std::string id;
    std::string sourceId;
    AudioEditFrame start = 0;
    AudioEditFrame length = 0;
    double sourceOffset = 0; // source-rate frames at the first output frame
    bool reversed = false;
    double gain = 1;
    std::vector<AudioEditEnvelope> envelopes;
    friend bool operator==(const AudioEditRegion&, const AudioEditRegion&) = default;
};

/// A sparse, non-ripple composition. Absent regions are silence. The duration
/// is explicit so deleting every region doesn't shorten the document.
struct AudioEditDocument {
    double sampleRate = 48000;
    int channels = 1;
    AudioEditFrame frames = 0;
    std::vector<AudioEditSource> sources;
    std::vector<AudioEditRegion> regions;
    bool initialized = false;
    std::string revision;
    friend bool operator==(const AudioEditDocument&, const AudioEditDocument&) = default;
};

} // namespace daw
