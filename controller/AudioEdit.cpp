#include "AudioEdit.hpp"
#include "model/Document.hpp"
#include "DSP/Curve.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace daw::audioedit {
namespace {
constexpr AudioEditFrame maxFrames = AudioEditFrame(192000) * 60 * 60 * 24;
const AudioEditSource* source(const AudioEditDocument& d, const std::string& id) {
    const auto it = std::find_if(d.sources.begin(), d.sources.end(), [&](const auto& s) { return s.id == id; });
    return it == d.sources.end() ? nullptr : &*it;
}
AudioEditRegion slice(const AudioEditDocument& d, const AudioEditRegion& r,
                      AudioEditFrame first, AudioEditFrame last) {
    auto out = r;
    const auto delta = first - r.start;
    out.start = first;
    out.length = last - first;
    if (const auto* s = source(d, r.sourceId))
        out.sourceOffset += double(delta) * s->sampleRate / d.sampleRate * (r.reversed ? -1 : 1);
    for (auto& e : out.envelopes) { e.first -= delta; e.last -= delta; }
    return out;
}
bool range(const AudioEditDocument& d, AudioEditFrame& first, AudioEditFrame& last) {
    first = std::clamp(first, AudioEditFrame(0), d.frames);
    last = std::clamp(last, first, d.frames);
    return d.initialized && last > first;
}
void sort(AudioEditDocument& d) {
    std::stable_sort(d.regions.begin(), d.regions.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
}
void boundaries(AudioEditDocument& d, AudioEditFrame first, AudioEditFrame last) {
    split(d, first); split(d, last);
}
}

AudioEditDocument fromSource(AudioEditSource s) {
    AudioEditDocument d;
    if (s.id.empty()) s.id = newUuid();
    d.sampleRate = s.sampleRate; d.channels = s.channels; d.frames = s.frames;
    // Revision keys are opaque: a shared document must never expose a local
    // filesystem path in its revision when media references are projected.
    std::uint64_t identity=1469598103934665603ull;
    for(const unsigned char byte:s.filePath+":"+s.sha256+":"+std::to_string(s.frames)+":"+
        std::to_string(s.sampleRate)+":"+std::to_string(s.channels)) {
        identity^=byte;identity*=1099511628211ull;
    }
    d.initialized = true; d.revision = "source:" + s.id + ":" + std::to_string(identity);
    if (s.frames > 0) d.regions.push_back({s.id + "-region", s.id, 0, s.frames});
    d.sources.push_back(std::move(s));
    return d;
}

bool valid(const AudioEditDocument& d) {
    if (!std::isfinite(d.sampleRate) || d.sampleRate < 8000 || d.sampleRate > 384000 ||
        d.channels < 1 || d.channels > 64 || d.frames < 0 || d.frames > maxFrames ||
        d.sources.size() > 65536 || d.regions.size() > 1000000) return false;
    std::unordered_set<std::string> sources, regions;
    for (const auto& s : d.sources) {
        if (s.id.empty() || !sources.insert(s.id).second || !std::isfinite(s.sampleRate) ||
            s.sampleRate < 8000 || s.sampleRate > 384000 || s.frames < 0 || s.frames > maxFrames ||
            s.channels < 1 || s.channels > 64) return false;
    }
    AudioEditFrame end = 0;
    for (const auto& r : d.regions) {
        if (r.id.empty() || !regions.insert(r.id).second || !sources.contains(r.sourceId) ||
            r.start < end || r.length <= 0 || r.length > d.frames || r.start > d.frames - r.length ||
            !std::isfinite(r.sourceOffset) || !std::isfinite(r.gain) || r.gain < 0 || r.gain > 1e12 ||
            r.envelopes.size() > 4096) return false;
        const auto* s = source(d, r.sourceId);
        const double sourceEnd = r.sourceOffset + double(r.length - 1) * s->sampleRate / d.sampleRate * (r.reversed ? -1 : 1);
        if (r.sourceOffset < -1e-6 || sourceEnd < -1e-6 ||
            r.sourceOffset > s->frames || sourceEnd > s->frames) return false;
        for (const auto& e : r.envelopes)
            if (!std::isfinite(e.first) || !std::isfinite(e.last) || e.last <= e.first ||
                !std::isfinite(e.from) || !std::isfinite(e.to) || e.from < 0 || e.to < 0 ||
                e.from > 1e12 || e.to > 1e12 || !std::isfinite(e.curve) || std::abs(e.curve) > 1) return false;
        end = r.start + r.length;
    }
    return true;
}

bool split(AudioEditDocument& d, AudioEditFrame at) {
    for (std::size_t i = 0; i < d.regions.size(); ++i) {
        auto r = d.regions[i];
        if (at <= r.start || at >= r.start + r.length) continue;
        auto right = slice(d, r, at, r.start + r.length); right.id = newUuid();
        d.regions[i] = slice(d, r, r.start, at);
        d.regions.insert(d.regions.begin() + std::ptrdiff_t(i + 1), std::move(right));
        d.revision = newUuid(); return true;
    }
    return false;
}
bool silence(AudioEditDocument& d, AudioEditFrame first, AudioEditFrame last) {
    if (!range(d, first, last)) return false;
    if (std::none_of(d.regions.begin(), d.regions.end(), [&](const auto& r) { return r.start < last && r.start + r.length > first; })) return false;
    boundaries(d, first, last);
    std::erase_if(d.regions, [&](const auto& r) { return r.start >= first && r.start < last; });
    d.revision = newUuid(); return true;
}
AudioEditDocument copy(const AudioEditDocument& d, AudioEditFrame first, AudioEditFrame last) {
    auto out = d; out.revision = newUuid(); out.regions.clear();
    if (!range(d, first, last)) { out.frames = 0; return out; }
    out.frames = last - first;
    for (const auto& r : d.regions) {
        const auto a = std::max(first, r.start), b = std::min(last, r.start + r.length);
        if (b <= a) continue;
        auto cut = slice(d, r, a, b); cut.start -= first; cut.id = newUuid();
        out.regions.push_back(std::move(cut));
    }
    return out;
}
bool paste(AudioEditDocument& d, const AudioEditDocument& clip, AudioEditFrame at, AudioEditFrame replaceEnd) {
    if (!clip.initialized || clip.frames <= 0 || !valid(clip) || !valid(d) || at < 0) return false;
    const double ratio = d.sampleRate / clip.sampleRate;
    const auto length = AudioEditFrame(std::llround(clip.frames * ratio));
    if (length <= 0 || at > maxFrames - length) return false;
    silence(d, at, std::max(at + length, replaceEnd));
    std::unordered_map<std::string, std::string> ids;
    for (auto s : clip.sources) {
        const auto* existing = source(d, s.id);
        if (existing && *existing == s) { ids[s.id] = s.id; continue; }
        const auto old = s.id; s.id = newUuid(); ids[old] = s.id;
        d.sources.push_back(std::move(s));
    }
    for (auto r : clip.regions) {
        const auto oldLength=r.length;
        const auto end = AudioEditFrame(std::llround((r.start + r.length) * ratio));
        r.start = AudioEditFrame(std::llround(r.start * ratio));
        r.length = end - r.start;
        if (r.length <= 0) continue;
        if (r.reversed) {
            const auto* original = source(clip, r.sourceId);
            r.sourceOffset += (r.length-1)*original->sampleRate/d.sampleRate -
                              (oldLength-1)*original->sampleRate/clip.sampleRate;
        }
        r.start += at; r.id = newUuid(); r.sourceId = ids.at(r.sourceId);
        for (auto& e : r.envelopes) { e.first *= ratio; e.last *= ratio; }
        d.regions.push_back(std::move(r));
    }
    d.frames = std::max(d.frames, at + length);
    d.channels = std::max(d.channels, clip.channels);
    d.revision = newUuid(); sort(d); return true;
}
bool move(AudioEditDocument& d, const std::string& id, AudioEditFrame at) {
    const auto it = std::find_if(d.regions.begin(), d.regions.end(), [&](const auto& r) { return r.id == id; });
    if (it == d.regions.end() || at < 0 || at == it->start || at > maxFrames - it->length) return false;
    auto r = *it; d.regions.erase(it);
    silence(d, at, at + r.length);
    r.start = at; d.frames = std::max(d.frames, at + r.length);
    d.regions.push_back(std::move(r)); d.revision = newUuid(); sort(d); return true;
}
bool reverse(AudioEditDocument& d, AudioEditFrame first, AudioEditFrame last) {
    if (!range(d, first, last)) return false;
    boundaries(d, first, last); bool changed = false;
    for (auto& r : d.regions) {
        if (r.start < first || r.start >= last) continue;
        const auto* s = source(d, r.sourceId);
        r.sourceOffset += (r.length - 1) * s->sampleRate / d.sampleRate * (r.reversed ? -1 : 1);
        r.reversed = !r.reversed;
        r.start = first + last - r.start - r.length;
        for (auto& e : r.envelopes) {
            const auto a = e.first; e.first = r.length - 1 - e.last;
            e.last = r.length - 1 - a; std::swap(e.from, e.to); e.mirrored = !e.mirrored;
        }
        changed = true;
    }
    if (changed) d.revision = newUuid(); sort(d); return changed;
}
bool gain(AudioEditDocument& d, AudioEditFrame first, AudioEditFrame last, double db) {
    if (!std::isfinite(db) || db < -240 || db > 240 || db == 0 || !range(d, first, last)) return false;
    boundaries(d, first, last); bool changed = false;
    const double multiplier = std::pow(10.0, db / 20.0);
    for (auto& r : d.regions) if (r.start >= first && r.start < last) {
        r.gain = std::min(1e12, r.gain * multiplier); changed = true;
    }
    if (changed) d.revision = newUuid(); return changed;
}
bool fade(AudioEditDocument& d, AudioEditFrame first, AudioEditFrame last,
          bool in, AudioEditFrame length, double curve) {
    if (!range(d, first, last) || length <= 0 || !std::isfinite(curve)) return false;
    length = std::min(length, last - first);
    boundaries(d, first, last); bool changed = false;
    const auto start = in ? first : last - length;
    for (auto& r : d.regions) if (r.start >= first && r.start < last) {
        r.envelopes.push_back({double(start - r.start), double(start - r.start + std::max<AudioEditFrame>(1, length - 1)),
                              in ? 0.0 : 1.0, in ? 1.0 : 0.0, std::clamp(curve, -1.0, 1.0)});
        changed = true;
    }
    if (changed) d.revision = newUuid(); return changed;
}

std::shared_ptr<const engine::SampleBuffer> render(const AudioEditDocument& d, const Decoder& decode, const Continue& keepGoing) {
    if (!d.initialized || !valid(d) || d.frames == 0 ||
        std::uint64_t(d.frames) > std::numeric_limits<engine::FrameCount>::max()) return {};
    if(keepGoing && !keepGoing())return {};
    auto out = std::make_shared<engine::SampleBuffer>(d.channels, d.frames, d.sampleRate);
    for(int ch=0;ch<d.channels;++ch)for(AudioEditFrame at=0;at<d.frames;at+=16384) {
        if(keepGoing && !keepGoing())return {};
        std::fill_n(out->writableChannel(ch)+at,std::min<AudioEditFrame>(16384,d.frames-at),0.f);
    }
    std::unordered_map<std::string, std::shared_ptr<const engine::SampleBuffer>> decoded;
    for (const auto& r : d.regions) {
        if (keepGoing && !keepGoing()) return {};
        const auto* s = source(d, r.sourceId);
        auto& raw = decoded[r.sourceId]; if (!raw) raw = decode(*s);
        if (!raw) return {}; // never silently publish missing media as a successful edit
        const auto ratio = raw->sampleRate() / d.sampleRate;
        const auto origin = r.sourceOffset * raw->sampleRate() / s->sampleRate;
        for (AudioEditFrame i = 0; i < r.length; ++i) {
            if ((i & 16383) == 0 && keepGoing && !keepGoing()) return {};
            const double pos = origin + i * ratio * (r.reversed ? -1 : 1);
            const auto frame = AudioEditFrame(std::floor(pos));
            const auto fraction = pos - frame;
            double amplitude = r.gain;
            for (const auto& e : r.envelopes) {
                double t = std::clamp((double(i) - e.first) / (e.last - e.first), 0.0, 1.0);
                t = e.mirrored ? 1 - engine::curve::shapeT(1 - t, engine::curve::Shape::Linear, e.curve)
                               : engine::curve::shapeT(t, engine::curve::Shape::Linear, e.curve);
                amplitude *= e.from + (e.to - e.from) * t;
            }
            for (int ch = 0; ch < d.channels; ++ch) {
                const auto* channel = raw->channel(ch);
                const float a = frame >= 0 && frame < AudioEditFrame(raw->frames()) ? channel[frame] : 0;
                const float b = frame + 1 >= 0 && frame + 1 < AudioEditFrame(raw->frames()) ? channel[frame + 1] : a;
                const double value=(a + (b - a) * fraction) * amplitude;
                if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())return {};
                out->writableChannel(ch)[r.start + i] = float(value);
            }
        }
    }
    return out;
}
double peak(const engine::SampleBuffer& buffer, AudioEditFrame first, AudioEditFrame last) {
    first = std::clamp(first, AudioEditFrame(0), AudioEditFrame(buffer.frames()));
    last = std::clamp(last, first, AudioEditFrame(buffer.frames()));
    double result = 0;
    for (unsigned ch = 0; ch < buffer.channels(); ++ch)
        for (auto i = first; i < last; ++i) result = std::max(result, double(std::abs(buffer.channel(ch)[i])));
    return result;
}
nlohmann::json toJson(const AudioEditDocument& d) {
    using nlohmann::json;
    json sources = json::array(), regions = json::array();
    for (const auto& s : d.sources) sources.push_back({{"id",s.id},{"file",s.filePath},{"sampleRate",s.sampleRate},{"frames",s.frames},{"channels",s.channels},{"assetId",s.assetId},{"sha256",s.sha256}});
    for (const auto& r : d.regions) {
        json envelopes = json::array();
        for (const auto& e : r.envelopes) envelopes.push_back({{"first",e.first},{"last",e.last},{"from",e.from},{"to",e.to},{"curve",e.curve},{"mirrored",e.mirrored}});
        regions.push_back({{"id",r.id},{"sourceId",r.sourceId},{"start",r.start},{"length",r.length},{"sourceOffset",r.sourceOffset},{"reversed",r.reversed},{"gain",r.gain},{"envelopes",std::move(envelopes)}});
    }
    return {{"version",1},{"revision",d.revision},{"initialized",d.initialized},{"sampleRate",d.sampleRate},{"channels",d.channels},{"frames",d.frames},{"sources",std::move(sources)},{"regions",std::move(regions)}};
}
bool fromJson(const nlohmann::json& j, AudioEditDocument& out) {
    try {
        if (!j.is_object() || j.value("version",0) != 1) return false;
        AudioEditDocument d;
        d.revision = j.value("revision",newUuid());
        if(!j.at("frames").is_number_integer() || !j.at("channels").is_number_integer()) return false;
        d.initialized = j.value("initialized",true); d.sampleRate = j.at("sampleRate"); d.channels = j.at("channels"); d.frames = j.at("frames");
        if (!j.at("sources").is_array() || !j.at("regions").is_array() || j.at("sources").size() > 65536 || j.at("regions").size() > 1000000) return false;
        for (const auto& s : j.at("sources")) {
            if(!s.at("frames").is_number_integer() || !s.at("channels").is_number_integer())return false;
            d.sources.push_back({s.at("id"),s.value("file",std::string()),s.at("sampleRate"),s.at("frames"),s.at("channels"),s.value("assetId",std::string()),s.value("sha256",std::string())});
        }
        for (const auto& r : j.at("regions")) {
            if(!r.at("start").is_number_integer() || !r.at("length").is_number_integer() ||
                (r.contains("envelopes") && (!r.at("envelopes").is_array() || r.at("envelopes").size()>4096)))return false;
            AudioEditRegion region{r.at("id"),r.at("sourceId"),r.at("start"),r.at("length"),r.at("sourceOffset"),r.value("reversed",false),r.value("gain",1.0)};
            if (r.contains("envelopes")) for (const auto& e : r.at("envelopes")) region.envelopes.push_back({e.at("first"),e.at("last"),e.at("from"),e.at("to"),e.value("curve",0.0),e.value("mirrored",false)});
            d.regions.push_back(std::move(region));
        }
        if (!valid(d)) return false;
        out = std::move(d); return true;
    } catch (const nlohmann::json::exception&) { return false; }
}
} // namespace daw::audioedit
