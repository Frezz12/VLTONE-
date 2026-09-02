#include "collaboration/CollaborationState.hpp"

#include <array>
#include <bit>
#include <iomanip>
#include <sstream>

namespace daw::collab {
namespace {

std::uint64_t fnv1a(std::string_view value, std::uint64_t basis) {
    std::uint64_t hash = basis;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void appendDouble(std::string& seed, double value) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    static constexpr char hex[] = "0123456789abcdef";
    seed.push_back('|');
    for (int shift = 60; shift >= 0; shift -= 4)
        seed.push_back(hex[(bits >> shift) & 0x0f]);
}

std::string pointSeed(const std::string& owner, std::size_t index,
                      const AutomationPoint& point) {
    std::string seed = owner + "|" + std::to_string(index);
    appendDouble(seed, point.beats);
    appendDouble(seed, point.value);
    appendDouble(seed, point.curve);
    seed += "|" + std::to_string(int(point.shape));
    return seed;
}

std::string uniqueId(std::string_view domain, const std::string& seed,
                     std::unordered_set<std::string>& seen) {
    for (std::size_t collision = 0;; ++collision) {
        const std::string candidate = deterministicMigrationId(
            domain, collision == 0 ? seed : seed + "|collision|" +
                                             std::to_string(collision));
        if (seen.insert(candidate).second) return candidate;
    }
}

} // namespace

std::string deterministicMigrationId(std::string_view domain,
                                     std::string_view seed) {
    std::string input;
    input.reserve(domain.size() + seed.size() + 1);
    input.append(domain);
    input.push_back('|');
    input.append(seed);

    std::uint64_t high = fnv1a(input, 1469598103934665603ull);
    std::uint64_t low = fnv1a(input, 1099511628211ull ^ 0x9e3779b97f4a7c15ull);
    // Mark the UUID-shaped result as name-derived (version 5) with an RFC-4122
    // variant. This is not a cryptographic content hash; it is a stable key.
    high = (high & ~std::uint64_t(0x000000000000f000ull)) |
           std::uint64_t(0x0000000000005000ull);
    low = (low & ~std::uint64_t(0xc000000000000000ull)) |
          std::uint64_t(0x8000000000000000ull);

    std::array<unsigned char, 16> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[std::size_t(i)] =
            static_cast<unsigned char>(high >> (56 - i * 8));
        bytes[std::size_t(i + 8)] =
            static_cast<unsigned char>(low >> (56 - i * 8));
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0f]);
    }
    return out;
}

void ensureStableCollaborationIds(ProjectModel& project) {
    std::unordered_set<std::string> pointIds;
    std::unordered_set<std::string> compIds;

    for (std::size_t ti = 0; ti < project.tracks.size(); ++ti) {
        TrackModel& track = project.tracks[ti];
        const std::string trackKey =
            track.id.empty() ? "track-index:" + std::to_string(ti) : track.id;
        for (std::size_t ci = 0; ci < track.clips.size(); ++ci) {
            ClipModel& clip = track.clips[ci];
            const std::string clipKey = trackKey + "|" +
                (clip.id.empty() ? "clip-index:" + std::to_string(ci) : clip.id);

            for (std::size_t pi = 0; pi < clip.automation.points.size(); ++pi) {
                AutomationPoint& point = clip.automation.points[pi];
                if (!point.id.empty() && pointIds.insert(point.id).second) continue;
                point.id = uniqueId("clip-automation-point",
                                    pointSeed(clipKey, pi, point), pointIds);
            }
            for (std::size_t li = 0; li < clip.lanes.size(); ++li) {
                ControllerLane& lane = clip.lanes[li];
                const std::string laneKey = clipKey + "|" +
                    (lane.id.empty() ? "lane-index:" + std::to_string(li)
                                     : lane.id);
                for (std::size_t pi = 0; pi < lane.points.size(); ++pi) {
                    AutomationPoint& point = lane.points[pi];
                    if (!point.id.empty() && pointIds.insert(point.id).second)
                        continue;
                    point.id = uniqueId("controller-lane-point",
                                        pointSeed(laneKey, pi, point), pointIds);
                }
            }
            for (std::size_t si = 0; si < clip.comp.size(); ++si) {
                CompSegment& segment = clip.comp[si];
                if (!segment.id.empty() && compIds.insert(segment.id).second)
                    continue;
                std::string seed = clipKey + "|" + std::to_string(si) + "|" +
                                   segment.takeId;
                appendDouble(seed, segment.startSeconds);
                appendDouble(seed, segment.endSeconds);
                segment.id = uniqueId("comp-segment", seed, compIds);
            }
        }
    }
}

} // namespace daw::collab
