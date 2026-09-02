#pragma once

#include "collaboration/SharedMutationSink.hpp"

#include <cstdint>
#include <string>

namespace daw::collab {

/// Local-only source metadata for one asset-producing shared action. The
/// controller retains the action context under requestId; neither sourcePath
/// nor this request can enter a command or canonical document.
struct SharedAssetMutationRequest {
    std::string requestId;
    std::string assetId;
    std::string sourcePath;
    std::string displayName;
    std::string contentType;
    std::string codec;
    double sampleRate = 0.0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
};

/// Application-owned async gate. Submitted means cache/hash/upload/verify now
/// owns the request; Blocked guarantees that no document mutation occurred.
class SharedAssetMutationSink {
public:
    virtual ~SharedAssetMutationSink() = default;
    virtual SharedMutationResult prepare(
        SharedAssetMutationRequest request) = 0;
};

} // namespace daw::collab
