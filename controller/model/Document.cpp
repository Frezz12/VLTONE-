#include "model/Document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace daw {

std::string toString(PluginFormat format) {
    switch (format) {
        case PluginFormat::Clap: return "clap";
        case PluginFormat::Vst3: return "vst3";
        case PluginFormat::Vst: return "vst";
        case PluginFormat::AudioUnit: return "au";
        case PluginFormat::Internal: return "internal";
        case PluginFormat::None: break;
    }
    return "none";
}

PluginFormat pluginFormatFromString(const std::string& name) {
    if (name == "clap") return PluginFormat::Clap;
    if (name == "vst3") return PluginFormat::Vst3;
    if (name == "vst") return PluginFormat::Vst;
    if (name == "au") return PluginFormat::AudioUnit;
    if (name == "internal") return PluginFormat::Internal;
    return PluginFormat::None;
}

std::string toString(PluginChannelMode mode) {
    switch (mode) {
        case PluginChannelMode::Mono: return "mono";
        case PluginChannelMode::Stereo: return "stereo";
        case PluginChannelMode::DualMono: return "dual-mono";
        case PluginChannelMode::Auto: break;
    }
    return "auto";
}

PluginChannelMode pluginChannelModeFromString(const std::string& name) {
    if (name == "mono") return PluginChannelMode::Mono;
    if (name == "stereo") return PluginChannelMode::Stereo;
    if (name == "dual-mono") return PluginChannelMode::DualMono;
    return PluginChannelMode::Auto;
}

std::string toString(PluginEditorChannel channel) {
    return channel == PluginEditorChannel::Right ? "right" : "left";
}

PluginEditorChannel pluginEditorChannelFromString(const std::string& name) {
    return name == "right" ? PluginEditorChannel::Right
                           : PluginEditorChannel::Left;
}

std::string newUuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    const uint64_t a = dist(rng);
    const uint64_t b = dist(rng);

    static constexpr char hex[] = "0123456789abcdef";
    auto nibble = [&](uint64_t v, int shift) {
        return hex[(v >> shift) & 0xF];
    };

    std::string out;
    out.reserve(36);
    // 8-4-4-4-12
    for (int i = 15; i >= 8; --i) out += nibble(a, i * 4);
    out += '-';
    for (int i = 7; i >= 4; --i) out += nibble(a, i * 4);
    out += '-';
    for (int i = 3; i >= 0; --i) out += nibble(a, i * 4);
    out += '-';
    for (int i = 15; i >= 12; --i) out += nibble(b, i * 4);
    out += '-';
    for (int i = 11; i >= 0; --i) out += nibble(b, i * 4);
    return out;
}

std::string toString(TrackKind kind) {
    switch (kind) {
        case TrackKind::Audio: return "audio";
        case TrackKind::Instrument: return "instrument";
        case TrackKind::Midi: return "midi";
        case TrackKind::Pattern: return "pattern";
        case TrackKind::Automation: return "automation";
        case TrackKind::Bus: return "bus";
        case TrackKind::Aux: return "aux";
        case TrackKind::Group: return "group";
        case TrackKind::Master: return "master";
        case TrackKind::Folder: return "folder";
    }
    return "audio";
}

TrackKind trackKindFromString(const std::string& s) {
    if (s == "instrument") return TrackKind::Instrument;
    if (s == "midi") return TrackKind::Midi;
    if (s == "pattern") return TrackKind::Pattern;
    if (s == "automation") return TrackKind::Automation;
    if (s == "bus") return TrackKind::Bus;
    if (s == "aux") return TrackKind::Aux;
    if (s == "group") return TrackKind::Group;
    if (s == "master") return TrackKind::Master;
    if (s == "folder") return TrackKind::Folder;
    return TrackKind::Audio;
}

std::string toString(AutomationSegment segment) {
    switch (segment) {
        case AutomationSegment::Linear: return "linear";
        case AutomationSegment::Hold: return "hold";
        case AutomationSegment::SCurve: return "scurve";
    }
    return "linear";
}

engine::curve::Shape toCurveShape(AutomationSegment segment) {
    static_assert(int(AutomationSegment::Linear) == int(engine::curve::Shape::Linear));
    static_assert(int(AutomationSegment::Hold) == int(engine::curve::Shape::Hold));
    static_assert(int(AutomationSegment::SCurve) == int(engine::curve::Shape::SCurve));
    return engine::curve::Shape(segment);
}

AutomationSegment automationSegmentFromString(const std::string& s) {
    if (s == "hold") return AutomationSegment::Hold;
    if (s == "scurve") return AutomationSegment::SCurve;
    return AutomationSegment::Linear;
}

std::string toString(AutomationTargetKind kind) {
    switch (kind) {
        case AutomationTargetKind::TrackVolume: return "volume";
        case AutomationTargetKind::TrackPan: return "pan";
        case AutomationTargetKind::TrackMute: return "mute";
        case AutomationTargetKind::SendLevel: return "send";
        case AutomationTargetKind::PluginParameter: return "parameter";
    }
    return "volume";
}

AutomationTargetKind automationTargetKindFromString(const std::string& s) {
    if (s == "pan") return AutomationTargetKind::TrackPan;
    if (s == "mute") return AutomationTargetKind::TrackMute;
    if (s == "send") return AutomationTargetKind::SendLevel;
    if (s == "parameter") return AutomationTargetKind::PluginParameter;
    return AutomationTargetKind::TrackVolume;
}

std::string toString(ClipKind kind) {
    switch (kind) {
        case ClipKind::Audio: return "audio";
        case ClipKind::Midi: return "midi";
        case ClipKind::Pattern: return "pattern";
        case ClipKind::Automation: return "automation";
    }
    return "audio";
}

ClipKind clipKindFromString(const std::string& s) {
    if (s == "midi") return ClipKind::Midi;
    if (s == "pattern") return ClipKind::Pattern;
    if (s == "automation") return ClipKind::Automation;
    return ClipKind::Audio;
}

std::string toString(RecordMode mode) {
    switch (mode) {
        case RecordMode::Overwrite: return "overwrite";
        case RecordMode::Layers: return "layers";
    }
    return "overwrite";
}

RecordMode recordModeFromString(const std::string& s) {
    if (s == "layers") return RecordMode::Layers;
    return RecordMode::Overwrite;
}

std::string toString(TrackRecordMode mode) {
    switch (mode) {
        case TrackRecordMode::UseGlobal: return "global";
        case TrackRecordMode::Overwrite: return "overwrite";
        case TrackRecordMode::Layers: return "layers";
    }
    return "global";
}

TrackRecordMode trackRecordModeFromString(const std::string& s) {
    if (s == "overwrite") return TrackRecordMode::Overwrite;
    if (s == "layers") return TrackRecordMode::Layers;
    return TrackRecordMode::UseGlobal;
}

// ── Takes and comping ──────────────────────────────────────────────────────

namespace {
/// Shorter than this and a comp segment is a rounding artefact of a swipe, not
/// something the user meant to hear. Also keeps a crossfade from being longer
/// than the piece it fades.
constexpr double kMinCompSegment = 0.001;
}

bool isLayered(const ClipModel& clip) { return !clip.takes.empty(); }

TakeModel* findTake(ClipModel& clip, const std::string& takeId) {
    for (auto& take : clip.takes) {
        if (take.id == takeId) return &take;
    }
    return nullptr;
}

const TakeModel* findTake(const ClipModel& clip, const std::string& takeId) {
    for (const auto& take : clip.takes) {
        if (take.id == takeId) return &take;
    }
    return nullptr;
}

void normalizeComp(ClipModel& clip) {
    if (clip.takes.empty()) {
        clip.comp.clear();
        return;
    }

    const double limit = clip.durationSeconds > 0.0 ? clip.durationSeconds
                                                    : std::numeric_limits<double>::max();

    std::vector<CompSegment> kept;
    kept.reserve(clip.comp.size());
    for (CompSegment segment : clip.comp) {
        if (!findTake(clip, segment.takeId)) continue;   // take was deleted
        segment.startSeconds = std::max(0.0, segment.startSeconds);
        segment.endSeconds = std::min(limit, segment.endSeconds);
        if (segment.endSeconds - segment.startSeconds < kMinCompSegment) continue;
        kept.push_back(segment);
    }

    std::sort(kept.begin(), kept.end(), [](const CompSegment& a, const CompSegment& b) {
        return a.startSeconds < b.startSeconds;
    });

    // Later segments win an overlap: `setCompRange` appends the new swipe and
    // leaves the trimming to this pass, so "most recently painted" is exactly
    // the rule the brush should have.
    clip.comp.clear();
    for (const CompSegment& segment : kept) {
        if (!clip.comp.empty()) {
            CompSegment& previous = clip.comp.back();
            if (segment.startSeconds < previous.endSeconds) {
                previous.endSeconds = segment.startSeconds;
                if (previous.endSeconds - previous.startSeconds < kMinCompSegment) {
                    clip.comp.pop_back();
                }
            }
        }
        if (!clip.comp.empty() && clip.comp.back().takeId == segment.takeId &&
            std::abs(clip.comp.back().endSeconds - segment.startSeconds) <
                kMinCompSegment) {
            clip.comp.back().endSeconds = segment.endSeconds;   // merge
            continue;
        }
        clip.comp.push_back(segment);
    }
}

std::string activeTakeAt(const ClipModel& clip, double seconds) {
    for (const CompSegment& segment : clip.comp) {
        if (seconds >= segment.startSeconds && seconds < segment.endSeconds) {
            return segment.takeId;
        }
    }
    return {};
}

void setCompRange(ClipModel& clip, const std::string& takeId, double fromSeconds,
                  double toSeconds) {
    if (!findTake(clip, takeId)) return;
    if (toSeconds < fromSeconds) std::swap(fromSeconds, toSeconds);
    if (toSeconds - fromSeconds < kMinCompSegment) return;

    // A swipe that lands inside an existing segment has to split it, which the
    // sort-and-trim in `normalizeComp` cannot do on its own — it only trims a
    // segment's tail. So the split happens here, before the new piece is added.
    std::vector<CompSegment> rebuilt;
    rebuilt.reserve(clip.comp.size() + 2);
    for (const CompSegment& segment : clip.comp) {
        const bool coversStart = segment.startSeconds < fromSeconds &&
                                 segment.endSeconds > fromSeconds;
        const bool coversEnd = segment.startSeconds < toSeconds &&
                               segment.endSeconds > toSeconds;
        if (coversStart) {
            rebuilt.push_back({segment.takeId, segment.startSeconds, fromSeconds});
        }
        if (coversEnd) {
            rebuilt.push_back({segment.takeId, toSeconds, segment.endSeconds});
        }
        if (!coversStart && !coversEnd) {
            const bool swallowed = segment.startSeconds >= fromSeconds &&
                                   segment.endSeconds <= toSeconds;
            if (!swallowed) rebuilt.push_back(segment);
        }
    }
    rebuilt.push_back({takeId, fromSeconds, toSeconds});

    clip.comp = std::move(rebuilt);
    normalizeComp(clip);
}

void selectWholeTake(ClipModel& clip, const std::string& takeId) {
    const TakeModel* take = findTake(clip, takeId);
    if (!take) return;
    // A clip with no duration of its own (nothing on the timeline sets one to
    // zero, but a hand-written project could) falls back to the take's length
    // rather than an infinite segment, which would serialise as a bad number.
    const double end = clip.durationSeconds > 0.0 ? clip.durationSeconds
                                                  : take->lengthSeconds;
    if (end <= 0.0) return;
    clip.comp.clear();
    clip.comp.push_back({takeId, 0.0, end});
    normalizeComp(clip);
}

void promoteToTake(ClipModel& clip) {
    if (!clip.takes.empty()) return;
    const bool hasSource = clip.kind == ClipKind::Audio
                               ? !clip.filePath.empty()
                               : clip.kind == ClipKind::Midi && !clip.notes.empty();
    if (!hasSource) return;

    TakeModel take;
    take.id = newUuid();
    take.name = "Take 1";
    take.filePath = clip.filePath;
    take.offsetSeconds = clip.offsetSeconds;
    take.lengthSeconds = clip.durationSeconds;
    take.gain = 1.0f;
    take.channels = clip.channels;
    take.color = clip.color;
    take.notes = clip.notes;
    clip.takes.push_back(std::move(take));
    selectWholeTake(clip, clip.takes.front().id);
}

bool trackAccepts(TrackKind kind, ClipKind clipKind) {
    switch (clipKind) {
        case ClipKind::Audio:
            return kind == TrackKind::Audio;
        case ClipKind::Midi:
            // An instrument track is a MIDI track that will grow a synth, so it
            // takes the same clips.
            return kind == TrackKind::Midi || kind == TrackKind::Instrument;
        case ClipKind::Pattern:
            return kind == TrackKind::Pattern;
        case ClipKind::Automation:
            return kind == TrackKind::Automation;
    }
    return false;
}

namespace {
constexpr double kMinDb = -60.0;
constexpr double kMaxDb = 6.0;
constexpr double kRangeDb = kMaxDb - kMinDb;
constexpr double kTaper = 1.5;
} // namespace

double gainFromNormalized(double position) {
    const double x = std::clamp(position, 0.0, 1.0);
    if (x <= 0.0005) return 0.0;
    return std::pow(10.0, (kMaxDb - kRangeDb * std::pow(1.0 - x, kTaper)) / 20.0);
}

double normalizedFromGain(double gain) {
    if (gain <= 0.00001) return 0.0;
    const double db = std::clamp(20.0 * std::log10(gain), kMinDb, kMaxDb);
    return 1.0 - std::pow((kMaxDb - db) / kRangeDb, 1.0 / kTaper);
}

void normalizeAutomation(std::vector<AutomationPoint>& points) {
    for (auto& point : points) {
        point.beats = std::max(0.0, point.beats);
        point.value = std::clamp(point.value, 0.0, 1.0);
        point.curve = std::clamp(point.curve, -1.0, 1.0);
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const AutomationPoint& a, const AutomationPoint& b) {
                         return a.beats < b.beats;
                     });
    points.erase(std::unique(points.begin(), points.end(),
                             [](const AutomationPoint& a, const AutomationPoint& b) {
                                 return std::abs(a.beats - b.beats) < 1e-9;
                             }),
                 points.end());
}

double automationValueAt(const std::vector<AutomationPoint>& points, double beats,
                         double fallback) {
    // Straight through the engine's evaluator rather than a second
    // implementation of the same maths — a curve that is drawn differently from
    // the way it is played is worse than no curve at all.
    if (points.empty()) return fallback;
    if (beats < points.front().beats) return fallback;
    if (beats >= points.back().beats) return points.back().value;

    // Curves are normalised into beat order. Paint paths ask for one value per
    // horizontal pixel, so restarting a linear scan here made drawing a dense
    // lane O(width * point_count). Find the enclosing segment logarithmically;
    // sequential realtime evaluation still uses its own cursor-based path.
    const auto right = std::upper_bound(
        points.begin(), points.end(), beats,
        [](double value, const AutomationPoint& point) {
            return value < point.beats;
        });
    if (right == points.begin()) return fallback;
    if (right == points.end()) return points.back().value;
    const AutomationPoint& from = *std::prev(right);
    const AutomationPoint& to = *right;
    const double span = to.beats - from.beats;
    if (!(span > 0.0)) return to.value;
    const double t = engine::curve::shapeT((beats - from.beats) / span,
                                           toCurveShape(from.shape), from.curve);
    return from.value + (to.value - from.value) * t;
}

uint32_t takeColor(uint32_t base, size_t index) {
    // Â±18% in steps, cycling, so take 1 is the track colour and the next few
    // walk away from it and back.
    static constexpr double kSteps[] = {0.0, 0.14, -0.12, 0.26, -0.22, 0.08};
    const double amount = kSteps[index % (sizeof(kSteps) / sizeof(kSteps[0]))];
    auto channel = [&](int shift) {
        const double v = double((base >> shift) & 0xFF);
        const double lifted = amount >= 0.0 ? v + (255.0 - v) * amount
                                            : v * (1.0 + amount);
        return uint32_t(std::clamp(lifted, 0.0, 255.0)) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

double beatsToSeconds(double beats, double tempo) {
    return beats * 60.0 / (tempo > 0.0 ? tempo : 1.0);
}

double secondsToBeats(double seconds, double tempo) {
    return seconds * (tempo > 0.0 ? tempo : 1.0) / 60.0;
}


TrackModel* ProjectModel::findTrack(const std::string& id) {
    const size_t index = static_cast<const ProjectModel&>(*this).indexOf(id);
    return index == std::string::npos ? nullptr : &tracks[index];
}

const TrackModel* ProjectModel::findTrack(const std::string& id) const {
    const size_t index = indexOf(id);
    return index == std::string::npos ? nullptr : &tracks[index];
}

size_t ProjectModel::indexOf(const std::string& id) const {
    // `tracks` remains public data for serialization and batch editing. Size
    // changes are detected immediately; reorder/id edits are detected by
    // validating the indexed slot. A miss rebuilds as well, covering insertion
    // into a vector whose allocation and size happened to be reused.
    if (m_trackIndex.size() == tracks.size()) {
        if (const auto found = m_trackIndex.find(id); found != m_trackIndex.end() &&
            found->second < tracks.size() && tracks[found->second].id == id) {
            return found->second;
        }
    }
    rebuildTrackIndex();
    const auto found = m_trackIndex.find(id);
    return found == m_trackIndex.end() ? std::string::npos : found->second;
}

void ProjectModel::rebuildTrackIndex() const {
    m_trackIndex.clear();
    m_trackIndex.reserve(tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) {
        m_trackIndex[tracks[i].id] = i;
    }
}

// ── Folder hierarchy ───────────────────────────────────────────────────────
//
// The document keeps tracks in one flat, ordered vector; folders are expressed
// with `parentId`. Display order is that vector order, with the children of a
// collapsed folder skipped.

namespace {
constexpr int kMaxNesting = 32;   // guard against a corrupted parent cycle
}

int trackDepth(const ProjectModel& project, const std::string& trackId) {
    int depth = 0;
    const TrackModel* track = project.findTrack(trackId);
    while (track && !track->parentId.empty() && depth < kMaxNesting) {
        track = project.findTrack(track->parentId);
        ++depth;
    }
    return depth;
}

std::string summingParent(const ProjectModel& project,
                          const std::string& trackId) {
    const TrackModel* track = project.findTrack(trackId);
    if (!track) return {};
    // Start at the parent: a summing folder does not sum into itself.
    const TrackModel* parent =
        track->parentId.empty() ? nullptr : project.findTrack(track->parentId);
    for (int guard = 0; parent && guard < kMaxNesting; ++guard) {
        if (isSummingFolder(*parent)) return parent->id;
        parent = parent->parentId.empty() ? nullptr
                                          : project.findTrack(parent->parentId);
    }
    return {};
}

const std::vector<TrackRow>& visibleTracks(const ProjectModel& project) {
    std::uint64_t signature = 1469598103934665603ull;
    const auto mix = [&signature](std::string_view value) {
        for (const unsigned char byte : value) {
            signature ^= byte;
            signature *= 1099511628211ull;
        }
        signature ^= 0xffu;
        signature *= 1099511628211ull;
    };
    for (const TrackModel& track : project.tracks) {
        mix(track.id);
        mix(track.parentId);
        signature ^= std::uint64_t(track.expanded);
        signature *= 1099511628211ull;
        signature ^= std::uint64_t(track.automationExpanded);
        signature *= 1099511628211ull;
    }
    if (signature == project.m_visibleRowsSignature &&
        project.m_visibleRows.size() <= project.tracks.size()) {
        return project.m_visibleRows;
    }

    auto& rows = project.m_visibleRows;
    rows.clear();
    rows.reserve(project.tracks.size());
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const TrackModel& track = project.tracks[i];

        bool hidden = false;
        const TrackModel* child = &track;
        const TrackModel* parent =
            track.parentId.empty() ? nullptr : project.findTrack(track.parentId);
        for (int guard = 0; parent && guard < kMaxNesting; ++guard) {
            // Only the direct owner controls an automation disclosure. Folder
            // ancestors still use their ordinary expanded state, so hiding
            // automation cannot fold a Pattern's instruments or a summing
            // folder's channels along with it.
            const bool childVisible = isAutomationLane(*child)
                                          ? parent->automationExpanded
                                          : parent->expanded;
            if (!childVisible) {
                hidden = true;
                break;
            }
            child = parent;
            parent = parent->parentId.empty() ? nullptr
                                              : project.findTrack(parent->parentId);
        }
        if (hidden) continue;

        rows.push_back({i, trackDepth(project, track.id)});
    }
    project.m_visibleRowsSignature = signature;
    return rows;
}

bool isDescendantOf(const ProjectModel& project, const std::string& candidateId,
                    const std::string& folderId) {
    if (candidateId == folderId) return true;
    const TrackModel* track = project.findTrack(candidateId);
    for (int guard = 0; track && !track->parentId.empty() && guard < kMaxNesting;
         ++guard) {
        if (track->parentId == folderId) return true;
        track = project.findTrack(track->parentId);
    }
    return false;
}

std::vector<std::string> subtreeOf(const ProjectModel& project,
                                   const std::string& folderId) {
    std::vector<std::string> ids;
    for (const auto& t : project.tracks) {
        if (t.id != folderId && isDescendantOf(project, t.id, folderId)) {
            ids.push_back(t.id);
        }
    }
    return ids;
}

} // namespace daw
