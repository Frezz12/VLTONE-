#include "SlideNotes.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_set>

namespace daw::slides {
bool valid(const SlideNoteModel &s) noexcept {
    if (s.id.empty() || !std::isfinite(s.startBeats) || s.startBeats < 0 ||
        !std::isfinite(s.lengthBeats) || s.lengthBeats <= 0 || s.points.size() < 2 ||
        s.points.size() > maxPoints || s.points.front().beats != 0 || s.points.back().beats != 1 ||
        s.targetNoteIds.size() > 128)
        return false;
    double previous = -1;
    for (const auto &p : s.points) {
        if (!std::isfinite(p.beats) || p.beats <= previous || p.beats > 1 ||
            !std::isfinite(p.value) || p.value < 0 || p.value > 127 || !std::isfinite(p.curve) ||
            std::abs(p.curve) > 1 || (int(p.shape) < 0 || int(p.shape) > 2))
            return false;
        previous = p.beats;
    }
    return true;
}

void normalize(SlideNoteModel &s) {
    if (s.id.empty())
        s.id = newUuid();
    s.startBeats = std::isfinite(s.startBeats) ? std::max(0.0, s.startBeats) : 0.0;
    s.lengthBeats = std::isfinite(s.lengthBeats) ? std::max(1.0 / 960.0, s.lengthBeats) : 1.0;
    std::erase_if(s.points,
                  [](const auto &p) { return !std::isfinite(p.beats) || !std::isfinite(p.value); });
    for (auto &p : s.points) {
        p.beats = std::clamp(p.beats, 0.0, 1.0);
        p.value = std::clamp(p.value, 0.0, 127.0);
        p.curve = std::isfinite(p.curve) ? std::clamp(p.curve, -1.0, 1.0) : 0;
        if ((int(p.shape) < 0 || int(p.shape) > 2))
            p.shape = AutomationSegment::Linear;
    }
    std::stable_sort(s.points.begin(), s.points.end(),
                     [](const auto &a, const auto &b) { return a.beats < b.beats; });
    s.points.erase(std::unique(s.points.begin(), s.points.end(),
                               [](const auto &a, const auto &b) { return a.beats == b.beats; }),
                   s.points.end());
    if (s.points.empty())
        s.points = {{0, 60}, {1, 60}};
    if (s.points.front().beats != 0) {
        auto p = s.points.front();
        p.beats = 0;
        s.points.insert(s.points.begin(), p);
    }
    if (s.points.back().beats != 1) {
        auto p = s.points.back();
        p.beats = 1;
        s.points.push_back(p);
    }
    // Ramer-Douglas-Peucker uses vertical pitch error, measured against the
    // original drawing. Repeated greedy removals would accumulate cent error.
    if (s.points.size() > 2) {
        std::vector<bool> keep(s.points.size(), true);
        const auto simplify = [&](auto &&self, std::size_t a, std::size_t b) -> void {
            double error = 0;
            std::size_t selected = a;
            for (std::size_t i = a + 1; i < b; ++i) {
                const double t = (s.points[i].beats - s.points[a].beats) /
                                 (s.points[b].beats - s.points[a].beats);
                double e =
                    std::abs(s.points[i].value -
                             (s.points[a].value + (s.points[b].value - s.points[a].value) * t));
                if (e > error) {
                    error = e;
                    selected = i;
                }
            }
            if (error > .009) {
                keep[selected] = true;
                self(self, a, selected);
                self(self, selected, b);
            }
        };
        // Simplify only straight runs; retain every shaped segment and measure
        // against the complete original stroke, never a previously reduced copy.
        std::size_t first = 0;
        while (first + 1 < s.points.size()) {
            std::size_t last = first;
            while (last + 1 < s.points.size() &&
                   s.points[last].shape == AutomationSegment::Linear && s.points[last].curve == 0)
                ++last;
            if (last > first + 1) {
                std::fill(keep.begin() + first + 1, keep.begin() + last, false);
                simplify(simplify, first, last);
            }
            first = last > first ? last : first + 1;
        }
        std::vector<AutomationPoint> compact;
        for (std::size_t i = 0; i < s.points.size(); ++i)
            if (keep[i])
                compact.push_back(s.points[i]);
        s.points = std::move(compact);
    }
    // A drawing that cannot meet both limits is rejected, never silently
    // degraded. The pencil retains raw input and publishes at most 256 vertices.
    std::sort(s.targetNoteIds.begin(), s.targetNoteIds.end());
    s.targetNoteIds.erase(std::unique(s.targetNoteIds.begin(), s.targetNoteIds.end()),
                          s.targetNoteIds.end());
    if (s.targetNoteIds.size() > 128)
        s.targetNoteIds.resize(128);
}

void preset(SlideNoteModel &s, double from, double to, int type) {
    s.points.clear();
    const int count = type == 2 ? 32 : (type == 3 ? 2 : 1);
    for (int i = 0; i <= count; ++i) {
        const double t = double(i) / count;
        double value = from + (to - from) * t;
        if (type == 2)
            value += std::sin(t * 4 * std::numbers::pi) * std::max(1.0, std::abs(to - from) * .25) *
                     std::sin(t * std::numbers::pi);
        if (type == 3)
            value = from + (to - from) * (t <= .5 ? 2 * t : 2 * (1 - t));
        s.points.push_back(
            {t, std::clamp(value, 0.0, 127.0),
             type == 1 || type == 3 ? AutomationSegment::SCurve : AutomationSegment::Linear});
    }
}

SlideNoteModel create(const std::vector<NoteModel> &notes, double start, double length,
                      double pitch, const std::vector<std::string> &selected, bool chord) {
    SlideNoteModel s;
    s.id = newUuid();
    s.startBeats = start;
    s.lengthBeats = length;
    s.chord = chord;
    const auto chosen = [&](const NoteModel &n) {
        return std::find(selected.begin(), selected.end(), n.id) != selected.end();
    };
    const NoteModel *anchor = nullptr;
    for (const auto &n : notes) {
        if (n.muted || n.startBeats > start || n.startBeats + n.lengthBeats <= start)
            continue;
        if (!anchor || (chosen(n) != chosen(*anchor) ? chosen(n)
                        : std::abs(n.pitch - pitch) != std::abs(anchor->pitch - pitch)
                            ? std::abs(n.pitch - pitch) < std::abs(anchor->pitch - pitch)
                        : n.startBeats != anchor->startBeats ? n.startBeats > anchor->startBeats
                                                             : n.id < anchor->id))
            anchor = &n;
    }
    if (anchor) {
        s.referenceNoteId = anchor->id;
        for (const auto &n : notes) {
            if (n.id == anchor->id ||
                (chord && !n.muted && n.startBeats <= start &&
                 n.startBeats + n.lengthBeats > start && (selected.empty() || chosen(n))))
                s.targetNoteIds.push_back(n.id);
        }
    }
    preset(s, anchor ? anchor->pitch : pitch, pitch, 1);
    normalize(s);
    return s;
}

PitchCurves compile(const std::vector<NoteModel> &notes,
                    const std::vector<SlideNoteModel> &gestures) {
    PitchCurves result;
    std::unordered_map<std::string, const NoteModel *> byId;
    for (const auto &n : notes)
        if (!n.muted)
            byId.emplace(n.id, &n);
    std::vector<const SlideNoteModel *> ordered;
    for (const auto &s : gestures)
        if (!s.muted && valid(s))
            ordered.push_back(&s);
    std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) {
        return a->startBeats != b->startBeats ? a->startBeats < b->startBeats : a->id < b->id;
    });
    for (const auto *s : ordered) {
        auto anchor = byId.find(s->referenceNoteId);
        if (anchor == byId.end())
            continue;
        const auto &ref = *anchor->second;
        if (ref.startBeats > s->startBeats || ref.startBeats + ref.lengthBeats <= s->startBeats)
            continue;
        const double from =
            s->resumed ? s->points.front().value
                       : ref.pitch + engine::curve::valueAt(result[ref.id], s->startBeats, 0);
        for (const auto &id : s->targetNoteIds) {
            const auto target = byId.find(id);
            if (target == byId.end())
                continue;
            const auto &n = *target->second;
            if (n.startBeats > s->startBeats || n.startBeats + n.lengthBeats <= s->startBeats)
                continue;
            auto &points = result[id];
            const double offset =
                s->resumed ? from - ref.pitch : engine::curve::valueAt(points, s->startBeats, 0);
            // Restrict the preceding shaped segment without changing its history.
            auto cut = std::lower_bound(points.begin(), points.end(), s->startBeats,
                                        [](const auto &p, double t) { return p.beats < t; });
            if (cut != points.begin() && cut != points.end()) {
                auto &prior = *std::prev(cut);
                const double t = (s->startBeats - prior.beats) / (cut->beats - prior.beats);
                prior.phaseTo = prior.phaseFrom + (prior.phaseTo - prior.phaseFrom) * t;
            }
            // Preserve the old curve up to the takeover, then never resume it.
            std::erase_if(points, [&](const auto &p) { return p.beats >= s->startBeats; });
            points.push_back({s->startBeats, offset, toCurveShape(s->points.front().shape),
                              s->points.front().curve, 0, 1, s->startBeats});
            for (std::size_t i = 1; i < s->points.size(); ++i) {
                const auto &p = s->points[i];
                points.push_back({s->startBeats + p.beats * s->lengthBeats, offset + p.value - from,
                                  toCurveShape(p.shape), p.curve, 0, 1, s->startBeats});
            }
        }
    }
    return result;
}

void reidentify(std::vector<NoteModel> &notes, std::vector<SlideNoteModel> &gestures) {
    std::unordered_map<std::string, std::string> ids;
    for (auto &n : notes) {
        auto old = n.id;
        n.id = newUuid();
        ids.emplace(old, n.id);
    }
    for (auto &s : gestures) {
        s.id = newUuid();
        if (ids.contains(s.referenceNoteId))
            s.referenceNoteId = ids.at(s.referenceNoteId);
        for (auto &id : s.targetNoteIds)
            if (ids.contains(id))
                id = ids.at(id);
    }
}
void followNotes(std::vector<SlideNoteModel> &gestures, const std::vector<NoteModel> &before,
                 const std::vector<NoteModel> &after, bool stretch) {
    for (auto &s : gestures) {
        auto a = std::find_if(before.begin(), before.end(),
                              [&](const auto &n) { return n.id == s.referenceNoteId; });
        auto b = std::find_if(after.begin(), after.end(),
                              [&](const auto &n) { return n.id == s.referenceNoteId; });
        if (a == before.end() || b == after.end())
            continue;
        s.startBeats = std::max(0., s.startBeats + b->startBeats - a->startBeats);
        if (stretch && a->lengthBeats > 0) {
            const double factor = b->lengthBeats / a->lengthBeats;
            s.startBeats = std::max(0., b->startBeats + (s.startBeats - b->startBeats) * factor);
            s.lengthBeats *= factor;
        }
        double delta = b->pitch - a->pitch;
        for (auto &p : s.points)
            p.value = std::clamp(p.value + delta, 0., 127.);
    }
}
namespace {
std::vector<AutomationPoint> sampleCurve(const std::vector<engine::curve::Point> &points,
                                         double start, double end, double basePitch) {
    std::vector<AutomationPoint> sampled;
    const auto value = [&](double t) { return basePitch + engine::curve::valueAt(points, t, 0); };
    const auto subdivide = [&](auto &&self, double a, double b, double va, double vb,
                               int depth) -> void {
        const auto next =
            std::upper_bound(points.begin(), points.end(), a,
                             [](double time, const auto &p) { return time < p.beats; });
        if (next != points.begin() && std::prev(next)->shape == engine::curve::Shape::Hold) {
            sampled.back().shape = AutomationSegment::Hold;
            sampled.push_back({b, vb});
            return;
        }
        double error = 0;
        for (double f : {.25, .5, .75})
            error = std::max(error, std::abs(value(a + (b - a) * f) - (va + (vb - va) * f)));
        if (error > .004 && depth < 24) {
            double mid = (a + b) * .5, vm = value(mid);
            self(self, a, mid, va, vm, depth + 1);
            self(self, mid, b, vm, vb, depth + 1);
        } else
            sampled.push_back({b, vb});
    };
    sampled.push_back({start, value(start)});
    double previous = start;
    for (const auto &p : points)
        if (p.beats > start && p.beats < end) {
            subdivide(subdivide, previous, p.beats, value(previous), value(p.beats), 0);
            previous = p.beats;
        }
    subdivide(subdivide, previous, end, value(previous), value(end), 0);
    return sampled;
}
} // namespace
std::vector<SlideNoteModel> crop(const std::vector<NoteModel> &before,
                                 const std::vector<SlideNoteModel> &gestures,
                                 const std::vector<NoteModel> &after, double from, double to,
                                 const PitchCurves *sourceCurves) {
    std::vector<SlideNoteModel> result;
    if (gestures.empty())
        return result;
    const auto compiled = sourceCurves ? PitchCurves{} : compile(before, gestures);
    const auto &curves = sourceCurves ? *sourceCurves : compiled;
    std::size_t nextNote = 0;
    for (const auto &note : before) {
        const double begin = std::max(from, note.startBeats),
                     end = std::min(to, note.startBeats + note.lengthBeats);
        if (end <= begin)
            continue;
        if (nextNote >= after.size())
            break;
        const auto &target = after[nextNote++];
        auto curve = curves.find(note.id);
        if (curve == curves.end())
            continue;
        const auto &points = curve->second;
        const double start = std::max(begin, points.front().beats);
        if (start >= end)
            continue;
        auto sampled = sampleCurve(points, start, end, note.pitch);
        for (std::size_t offset = 0; offset + 1 < sampled.size();) {
            const auto count = std::min(maxPoints, sampled.size() - offset);
            SlideNoteModel s;
            s.id = newUuid();
            s.referenceNoteId = target.id;
            s.targetNoteIds = {target.id};
            s.resumed = true;
            const double a = sampled[offset].beats, b = sampled[offset + count - 1].beats;
            s.startBeats = a - from;
            s.lengthBeats = b - a;
            for (std::size_t i = 0; i < count; ++i) {
                auto p = sampled[offset + i];
                p.beats = (p.beats - a) / (b - a);
                s.points.push_back(p);
            }
            result.push_back(std::move(s));
            offset += count - 1;
        }
    }
    // Muted/orphaned gestures and tails beyond the base note remain editable.
    // Rebase their shape as data, without accidentally making them sound.
    std::unordered_map<std::string, std::string> identities;
    nextNote = 0;
    for (const auto &n : before)
        if (n.startBeats < to && n.startBeats + n.lengthBeats > from && nextNote < after.size())
            identities.emplace(n.id, after[nextNote++].id);
    for (const auto &original : gestures) {
        auto anchor = std::find_if(before.begin(), before.end(),
                                   [&](const auto &n) { return n.id == original.referenceNoteId; });
        const bool inactive =
            original.muted || original.targetNoteIds.empty() || anchor == before.end() ||
            anchor->muted || anchor->startBeats > original.startBeats ||
            anchor->startBeats + anchor->lengthBeats <= std::max(from, original.startBeats);
        if (!inactive || !valid(original))
            continue;
        const double start = std::max(from, original.startBeats);
        const double end = std::min(to, original.startBeats + original.lengthBeats);
        if (!(end > start))
            continue;
        std::vector<engine::curve::Point> shape;
        for (const auto &p : original.points)
            shape.push_back({original.startBeats + p.beats * original.lengthBeats, p.value,
                             toCurveShape(p.shape), p.curve});
        auto sampled = sampleCurve(shape, start, end, 0);
        for (std::size_t offset = 0; offset + 1 < sampled.size();) {
            const auto count = std::min(maxPoints, sampled.size() - offset);
            auto s = original;
            s.id = newUuid();
            s.points.clear();
            if (identities.contains(s.referenceNoteId))
                s.referenceNoteId = identities.at(s.referenceNoteId);
            for (auto &id : s.targetNoteIds)
                if (identities.contains(id))
                    id = identities.at(id);
            const double a = sampled[offset].beats, b = sampled[offset + count - 1].beats;
            s.startBeats = a - from;
            s.lengthBeats = b - a;
            s.resumed = original.resumed || a > original.startBeats;
            for (std::size_t i = 0; i < count; ++i) {
                auto p = sampled[offset + i];
                p.beats = (p.beats - a) / (b - a);
                s.points.push_back(p);
            }
            result.push_back(std::move(s));
            offset += count - 1;
        }
    }
    return result;
}

std::vector<SlideNoteModel> &editable(ClipModel &c) {
    auto &notes = midiNotes(c);
    for (auto &t : c.takes)
        if (&notes == &t.notes)
            return t.slideNotes;
    return c.slideNotes;
}
const std::vector<SlideNoteModel> &editable(const ClipModel &c) {
    return editable(const_cast<ClipModel &>(c));
}
} // namespace daw::slides
