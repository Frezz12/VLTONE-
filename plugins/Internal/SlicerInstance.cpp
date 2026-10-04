#include "Internal/SlicerInstance.hpp"

#include "Internal/SampleDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <unordered_map>

namespace daw::plugins::slicer {
namespace {

using json = nlohmann::json;

constexpr std::string_view kUid = "daw.slicer";
/// id → index, built once. `parameterIndexForId` is called per parameter on
/// every project load and by every automation lane, so a linear scan of a
/// dozen strings is not the right shape either.
const std::unordered_map<std::string_view, std::int32_t>& indexById() {
    static const std::unordered_map<std::string_view, std::int32_t> map = [] {
        std::unordered_map<std::string_view, std::int32_t> built;
        const std::span<const ParameterInfo> table = parameterTable();
        built.reserve(table.size());
        for (const ParameterInfo& info : table) {
            built.emplace(std::string_view(info.id), std::int32_t(info.index));
        }
        return built;
    }();
    return map;
}

double clampToRange(std::uint32_t index, double value) {
    const ParameterInfo& info = parameterTable()[index];
    if (!std::isfinite(value)) value = info.defaultValue;
    const double clamped = std::clamp(value, info.minValue, info.maxValue);
    return info.isStepped ? std::round(clamped) : clamped;
}

double number(const json& object, const char* name, double fallback, double low, double high) {
    const auto it = object.find(name);
    if (it == object.end() || !it->is_number()) return fallback;
    const double value = it->get<double>();
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

json encodeSlice(const Slice& s) {
    return {{"start",s.start},{"end",s.end},{"key",s.key},{"transpose",s.transpose},
        {"gain",s.gain},{"pan",s.pan},{"cutoff",s.cutoff},{"resonance",s.resonance},
        {"filter",s.filter},{"choke",s.chokeGroup},{"flags",s.flags},{"id",s.id},
        {"fine",s.fineTune},{"normalization",s.normalization},{"fadeIn",s.fadeInMs},
        {"fadeOut",s.fadeOutMs},{"crossfade",s.crossfadeMs},{"loop",s.loopMode},
        {"locked",s.locked},{"globalEnvelope",s.useGlobalEnvelope},
        {"attack",s.attack},{"decay",s.decay},{"sustain",s.sustain},{"release",s.release},
        {"effect",s.effect},{"effectX",s.effectX},{"effectY",s.effectY},{"effectMix",s.effectMix}};
}

bool decodeSlice(const json& entry, Slice& out) {
    json object = entry;
    if (entry.is_array()) {
        if (entry.size() < 11) return false;
        const char* names[]{"start","end","key","transpose","gain","pan","cutoff","resonance","filter","choke","flags"};
        object = json::object();
        for (int i=0;i<11;++i) { if (!entry[i].is_number()) return false; object[names[i]]=entry[i]; }
    }
    if (!object.is_object()) return false;
    const double start=number(object,"start",-1,0,UINT32_MAX);
    const double end=number(object,"end",-1,0,UINT32_MAX);
    if (start<0 || end<=start) return false;
    out.start=engine::FrameCount(start); out.end=engine::FrameCount(end);
    out.key=std::int16_t(number(object,"key",48,0,127));
    out.transpose=std::int16_t(number(object,"transpose",0,-96,96));
    out.gain=float(number(object,"gain",1,0,16)); out.pan=float(number(object,"pan",0,-1,1));
    out.cutoff=float(number(object,"cutoff",1,0,1)); out.resonance=float(number(object,"resonance",0,0,1));
    out.filter=std::uint8_t(number(object,"filter",0,0,3)); out.chokeGroup=std::uint8_t(number(object,"choke",0,0,255));
    out.flags=std::uint8_t(number(object,"flags",0,0,7)); out.id=std::uint32_t(number(object,"id",0,0,UINT32_MAX));
    out.fineTune=float(number(object,"fine",0,-100,100));
    out.normalization=float(number(object,"normalization",1,0,1e12));
    out.fadeInMs=float(number(object,"fadeIn",0,0,10000)); out.fadeOutMs=float(number(object,"fadeOut",-1,-1,10000));
    out.crossfadeMs=float(number(object,"crossfade",0,0,1000));
    out.loopMode=std::uint8_t(number(object,"loop",(out.flags & kSliceLoop)?1:0,0,2));
    if (object.contains("locked") && object["locked"].is_boolean()) out.locked=object["locked"].get<bool>();
    if (object.contains("globalEnvelope") && object["globalEnvelope"].is_boolean()) out.useGlobalEnvelope=object["globalEnvelope"].get<bool>();
    out.attack=float(number(object,"attack",0,0,5)); out.decay=float(number(object,"decay",0,0,5));
    out.sustain=float(number(object,"sustain",1,0,1)); out.release=float(number(object,"release",0.05,0,5));
    out.effect=std::uint8_t(number(object,"effect",0,0,3));
    out.effectX=float(number(object,"effectX",0.5,0,1));
    out.effectY=float(number(object,"effectY",0.5,0,1));
    out.effectMix=float(number(object,"effectMix",1,0,1));
    return true;
}

json encodeAnalysis(const AnalysisSettings& a) {
    return {{"mode",int(a.mode)},{"count",a.targetCount},{"threshold",a.sensitivity},{"seed",a.seed},
        {"preAttack",a.preAttackMs},{"root",a.rootNote},{"scale",a.scale},{"descending",a.descending},
        {"minimum",a.minimumMs},{"zeroCrossing",a.zeroCrossing},{"spread",a.randomSpread},
        {"bpm",a.sourceBpm},{"grid",a.gridBeats},{"start",a.rangeStart},{"end",a.rangeEnd}};
}
AnalysisSettings decodeAnalysis(const json& j) {
    AnalysisSettings a;
    if (!j.is_object()) return a;
    a.mode=SliceMode(int(number(j,"mode",0,0,3))); a.targetCount=int(number(j,"count",16,1,128));
    a.sensitivity=number(j,"threshold",0.5,0,1); a.preAttackMs=number(j,"preAttack",2,0,100);
    if (j.contains("seed") && j["seed"].is_number_unsigned()) a.seed=j["seed"].get<std::uint64_t>();
    a.rootNote=int(number(j,"root",48,0,127)); a.scale=int(number(j,"scale",0,0,kScaleCount-1));
    if (j.contains("descending") && j["descending"].is_boolean()) a.descending=j["descending"].get<bool>();
    a.minimumMs=number(j,"minimum",0,0,10000); a.randomSpread=number(j,"spread",0.45,0,0.49);
    if (j.contains("zeroCrossing") && j["zeroCrossing"].is_boolean()) a.zeroCrossing=j["zeroCrossing"].get<bool>();
    a.sourceBpm=number(j,"bpm",120,20,999); a.gridBeats=number(j,"grid",0,0,16);
    a.rangeStart=engine::FrameCount(number(j,"start",0,0,UINT32_MAX)); a.rangeEnd=engine::FrameCount(number(j,"end",0,0,UINT32_MAX));
    return a;
}

} // namespace

SlicerInstance::SlicerInstance() {
    m_descriptor = staticDescriptor();
    const std::span<const ParameterInfo> table = parameterTable();
    for (std::uint32_t i = 0; i < kParameterCount; ++i)
        m_values[i].store(table[i].defaultValue, std::memory_order_relaxed);
}

const PluginDescriptor& SlicerInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal;
        d.uid = std::string(kUid);
        // No file behind it. The path is set to the uid rather than left empty
        // so anything that keys on a path — the cache, a project written by an
        // older build — still has something stable to hold.
        d.path = std::string(kUid);
        d.name = "Slicer";
        d.vendor = "VLTONE";
        d.version = "2.0";
        d.stateSchemaVersion = kStateVersion;
        d.category = "Instrument";
        d.isInstrument = true;
        d.hasEditor = false;
        d.wantsMidi = true;
        d.mainInputChannels = 0;
        d.mainOutputChannels = 2;
        return d;
    }();
    return descriptor;
}

std::string_view SlicerInstance::uid() noexcept { return kUid; }

// ── Configuration ──────────────────────────────────────────────────────────

bool SlicerInstance::setBusLayout(const PluginBusLayout&, PluginBusLayout& accepted) {
    accepted = busLayout();
    return true;
}

PluginBusLayout SlicerInstance::busLayout() const {
    PluginBusLayout layout;
    layout.outputs.push_back(2);
    return layout;
}

bool SlicerInstance::activate(const PluginProcessInfo& info) {
    m_sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 48000.0;
    m_maxBlockSize = info.maxBlockSize;
    m_active = true;
    reset();
    return true;
}

void SlicerInstance::deactivate() {
    m_active = false;
    m_processing = false;
    reset();
}

void SlicerInstance::stopProcessing() {
    m_processing = false;
    // Every format may clear its tails here, and a slicer that kept its voices
    // would resume a chop the transport has already left behind.
    for (Voice& voice : m_voices) voice.kill();
}

// ── Parameters ─────────────────────────────────────────────────────────────

std::span<const ParameterInfo> SlicerInstance::parameters() const noexcept {
    return parameterTable();
}

std::int32_t SlicerInstance::parameterIndexForId(std::string_view id) const noexcept {
    const auto& map = indexById();
    const auto found = map.find(id);
    return found == map.end() ? -1 : found->second;
}

double SlicerInstance::parameterValue(std::uint32_t index) const noexcept {
    if (index >= kParameterCount) return 0.0;
    return m_values[index].load(std::memory_order_relaxed);
}

std::string SlicerInstance::parameterText(std::uint32_t index, double plainValue) const {
    // Qualified: an unqualified call would find this member and recurse.
    return ::daw::plugins::slicer::parameterText(index, plainValue);
}

void SlicerInstance::setParameterFromHost(std::uint32_t index, double plainValue) {
    setParameter(index, plainValue);
}

void SlicerInstance::setParameter(std::uint32_t index, double plainValue) {
    if (index >= kParameterCount) return;
    m_values[index].store(clampToRange(index, plainValue), std::memory_order_relaxed);
}

// ── State ──────────────────────────────────────────────────────────────────

bool SlicerInstance::saveState(std::vector<std::uint8_t>& out) const {
    return saveProjectState(out, m_samplePath);
}

bool SlicerInstance::saveProjectState(
    std::vector<std::uint8_t>& out,
    const std::string& packagedSampleName) const {
    json document;
    document["version"] = kStateVersion;
    document["sample"] = packagedSampleName;
    json values = json::object();
    const std::span<const ParameterInfo> table = parameterTable();
    for (const ParameterInfo& info : table) {
        values[info.id] = m_values[info.index].load(std::memory_order_relaxed);
    }
    document["params"] = std::move(values);

    document["analysis"] = encodeAnalysis(m_analysis);
    json slices = json::array();
    if (const std::shared_ptr<const SliceTable> chops = sliceTable()) {
        for (std::uint32_t i = 0; i < chops->count; ++i)
            slices.push_back(encodeSlice(chops->slices[i]));
    }
    document["slices"] = std::move(slices);
    document["nextSliceId"] = sliceTable() ? sliceTable()->nextId : 1;
    document["chromaticFallback"] = sliceTable() && sliceTable()->chromaticFallback;

    const std::string text = document.dump();
    out.assign(text.begin(), text.end());
    return true;
}

bool SlicerInstance::loadState(std::span<const std::uint8_t> state) {
    return loadProjectState(state, {});
}

bool SlicerInstance::loadProjectState(
    std::span<const std::uint8_t> state,
    const std::string& contentDirectory) {
    if (state.empty()) return false;
    json document = json::parse(state.begin(), state.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;

    const double version = document.contains("version") && document["version"].is_number_integer() ? document["version"].get<double>() : 1.0;
    if ((document.contains("version") && !document["version"].is_number_integer()) || version < 1 || version > kStateVersion) return false;
    ControlState next;
    for (const auto& info : parameterTable()) next.parameters[info.index]=info.defaultValue;
    if (document.contains("params") && document["params"].is_object()) {
        for (const auto& [id,v] : document["params"].items()) {
            const auto index=parameterIndexForId(id);
            if (index>=0 && v.is_number()) next.parameters[index]=clampToRange(std::uint32_t(index),v.get<double>());
        }
    }
    if (document.contains("sample") && !document["sample"].is_string()) return false;
    next.path=document.value("sample",std::string{});
    if (!next.path.empty() && !contentDirectory.empty() && platform::pathFromUtf8(next.path).is_relative())
        next.path=platform::pathToUtf8(platform::pathFromUtf8(contentDirectory)/platform::pathFromUtf8(next.path).filename());
    if (!next.path.empty()) next.audio=next.path==m_samplePath && m_raw ? m_raw : sampler::decodeSample(next.path);
    next.analysis=decodeAnalysis(document.value("analysis",json::object()));
    auto table=std::make_shared<SliceTable>();
    table->nextId = std::uint32_t(number(document,"nextSliceId",1,1,UINT32_MAX));
    if (document.contains("slices") && !document["slices"].is_array()) return false;
    if (document.contains("slices")) for (const auto& entry : document["slices"]) {
        if (table->count>=kMaxSlices) break;
        Slice slice;
        if (!decodeSlice(entry,slice)) return false;
        if (next.audio && !slice.valid(next.audio->frames())) { if (version >= 2) return false; continue; }
        if (version >= 2 && table->count > 0 && table->slices[table->count-1].end != slice.start) return false;
        table->slices[table->count++]=slice;
    }
    if (next.audio) table->frames=next.audio->frames();
    if (document.contains("chromaticFallback") && document["chromaticFallback"].is_boolean()) table->chromaticFallback = document["chromaticFallback"].get<bool>();
    table->rebuild(); next.table=std::move(table);
    restoreState(next);
    return true;
}

// ── The sample and the chop table ──────────────────────────────────────────

bool SlicerInstance::loadSample(const std::string& path) {
    return adoptSample(path, sampler::decodeSample(path));
}

bool SlicerInstance::adoptSample(const std::string& path,
    std::shared_ptr<const engine::SampleBuffer> decoded) {
    if (!decoded || decoded->frames()==0) return false;
    auto next=captureState();
    if (path!=next.path) { next.table.reset(); next.analysis.rangeStart=next.analysis.rangeEnd=0; }
    next.path=path; next.audio=std::move(decoded); restoreState(next); return true;
}

void SlicerInstance::clearSample() {
    auto next=captureState(); next.audio.reset(); next.path.clear(); next.table.reset();
    next.analysis.rangeStart=next.analysis.rangeEnd=0; restoreState(next);
}
std::string SlicerInstance::samplePath() const { return m_samplePath; }
std::string SlicerInstance::sampleName() const { return m_sampleName; }
std::shared_ptr<const SampleData> SlicerInstance::sample() const {
    const auto state=m_runtime.controlCopy(); return state ? state->sample : nullptr;
}
std::shared_ptr<const engine::SampleBuffer> SlicerInstance::rawSample() const { return m_raw; }
void SlicerInstance::setSliceTable(std::shared_ptr<const SliceTable> table) {
    if (table && table->count>kMaxSlices) {
        auto trimmed=std::make_shared<SliceTable>(*table); trimmed->count=kMaxSlices;
        trimmed->rebuild(); table=std::move(trimmed);
    }
    auto state=m_runtime.controlCopy();
    auto next=state ? std::make_shared<RuntimeState>(*state) : std::make_shared<RuntimeState>();
    next->table=std::move(table);
    double localRelease = 0.0;
    if (next->table) for (std::uint32_t i = 0; i < next->table->count; ++i)
        if (!next->table->slices[i].useGlobalEnvelope) localRelease = std::max(localRelease, double(next->table->slices[i].release));
    m_sliceRelease.store(localRelease, std::memory_order_relaxed);
    m_runtime.publish(std::move(next));
}
std::shared_ptr<const SliceTable> SlicerInstance::sliceTable() const {
    const auto state=m_runtime.controlCopy(); return state ? state->table : nullptr;
}
ControlState SlicerInstance::captureState() const {
    ControlState s; s.path=m_samplePath; s.audio=m_raw; s.table=sliceTable(); s.analysis=m_analysis;
    for (std::uint32_t i=0;i<kParameterCount;++i) s.parameters[i]=parameterValue(i);
    return s;
}
void SlicerInstance::restoreState(const ControlState& s) {
    const auto previous=m_runtime.controlCopy();
    const bool sourceChanged=!previous || m_raw!=s.audio || m_samplePath!=s.path;
    auto next=previous ? std::make_shared<RuntimeState>(*previous) : std::make_shared<RuntimeState>();
    if (sourceChanged) { next->sourceRevision=++m_nextSourceRevision; next->sample.reset(); }
    m_raw=s.audio; m_samplePath=s.path; m_analysis=s.analysis;
    m_sampleName=platform::pathToUtf8(platform::pathFromUtf8(s.path).filename());
    if (sourceChanged && s.audio) {
        s.audio->prepareRead(); auto sample=std::make_shared<SampleData>();
        sample->audio=s.audio; sample->baseFrames=s.audio->frames(); sample->path=s.path; sample->name=m_sampleName;
        next->sample=std::move(sample);
    }
    next->table=s.table;
    for (std::uint32_t i=0;i<kParameterCount;++i) setParameter(i,s.parameters[i]);
    double localRelease = 0.0;
    if (next->table) for (std::uint32_t i = 0; i < next->table->count; ++i)
        if (!next->table->slices[i].useGlobalEnvelope) localRelease = std::max(localRelease, double(next->table->slices[i].release));
    m_sliceRelease.store(localRelease, std::memory_order_relaxed);
    m_runtime.publish(std::move(next));
}
std::uint64_t SlicerInstance::sourceRevision() const {
    const auto state=m_runtime.controlCopy(); return state ? state->sourceRevision : 0;
}
bool SlicerInstance::keyActive(int key) const noexcept {
    return key>=0 && key<128 && (m_activeKeys[key/64].load(std::memory_order_relaxed)&(std::uint64_t(1)<<(key%64)));
}

// ── Audio ──────────────────────────────────────────────────────────────────

SlicerSettings SlicerInstance::snapshot() const noexcept {
    SlicerSettings s;
    s.attack = value(Param::Attack);
    s.release = value(Param::Release);
    s.gate = value(Param::Gate);
    s.keyTrack = value(Param::KeyTrack);
    s.rootNote = int(std::lround(value(Param::RootNote)));
    s.velocityDepth = value(Param::VelocityDepth);
    s.transpose=value(Param::Transpose)+value(Param::FineTune)/100.0;
    s.pan=value(Param::Pan); s.oneShot=value(Param::PlayMode)>=0.5;

    s.ampEnv.delay = 0.0;
    s.ampEnv.attack = s.attack;
    s.ampEnv.hold = 0.0;
    s.ampEnv.decay = value(Param::Decay);
    s.ampEnv.sustain = value(Param::Sustain);
    // A zero release would finish the voice on the same sample the key lifts
    // and leave exactly the click a fade exists to prevent. Three milliseconds
    // is below anything heard as a tail and above anything heard as a step.
    s.ampEnv.release = std::max(s.release, 0.003);
    return s;
}

void SlicerInstance::noteOn(int key, int channel, float velocity,
                            float pan, std::int32_t id) noexcept {
    const auto* table = m_renderRuntime ? m_renderRuntime->table.get() : nullptr;
    const auto* sample = m_renderRuntime ? m_renderRuntime->sample.get() : nullptr;
    if (!table || !sample || !sample->audio) return;

    const std::int32_t index = table->indexForKey(key);
    if (index < 0) return;
    const Slice& slice = table->slices[std::uint32_t(index)];
    // A muted chop still owns its key, but there is no voice to bookkeep for
    // it and nothing for a choke to cut, so the note stops here.
    if (slice.flags & kSliceMuted) return;

    const SlicerSettings settings = snapshot();
    const int chokeMode = int(std::lround(value(Param::ChokeMode)));
    if (chokeMode == 1) {
        for (Voice& voice : m_voices)
            if (voice.active()) voice.choke();
    } else if (chokeMode == 2 && slice.chokeGroup != 0) {
        // Group zero means ungrouped: it never chokes, so a table that never
        // assigned groups plays every chop to its own end.
        for (Voice& voice : m_voices)
            if (voice.active() && voice.chokeGroup() == slice.chokeGroup)
                voice.choke();
    }

    const int limit=int(value(Param::Polyphony));
    for (int i=limit;i<int(kMaxVoices);++i) if (m_voices[i].active()) m_voices[i].choke();
    Voice* chosen = nullptr;
    for (Voice& voice : std::span(m_voices, std::size_t(limit))) {
        if (!voice.active()) { chosen = &voice; break; }
    }
    if (!chosen) {
        // Steal a released voice before a held one: the note whose key is still
        // down is the one the player expects to keep hearing.
        for (Voice& voice : std::span(m_voices, std::size_t(limit))) {
            if (!voice.releasing()) continue;
            if (!chosen || voice.startedAt() < chosen->startedAt()) chosen = &voice;
        }
    }
    if (!chosen) {
        chosen = &m_voices[0];
        for (Voice& voice : std::span(m_voices, std::size_t(limit)))
            if (voice.startedAt() < chosen->startedAt()) chosen = &voice;
    }

    if (chosen->active() && !m_renderOffline) m_sourceTransition.begin(m_sampleRate);
    if (chosen->start(slice, key, channel, velocity, pan, settings, *sample,
                      m_sampleRate, std::uint32_t(index), id)) {
        chosen->setBend(m_channelBend[channel & 15] * value(Param::BendRange), 8.0);
        chosen->setStartedAt(++m_voiceStamp);
    }
}

void SlicerInstance::noteOff(int key, int channel, std::int32_t id) noexcept {
    for (Voice& voice : m_voices) {
        if (!voice.active() || voice.releasing()) continue;
        if (id >= 0 ? voice.noteId() != id
                    : voice.key() != key || voice.channel() != channel)
            continue;
        if (!voice.oneShot()) voice.release();
    }
}

void SlicerInstance::applyEvent(const PluginEvent& event, std::uint32_t) noexcept {
    switch (event.kind) {
        case PluginEvent::Kind::ParamValue:
            if (event.paramIndex < kParameterCount) {
                m_values[event.paramIndex].store(
                    clampToRange(event.paramIndex, event.value),
                    std::memory_order_relaxed);
            }
            break;
        case PluginEvent::Kind::NoteOn:
            noteOn(int(event.key), int(event.channel), float(event.value),
                   float(event.notePan), event.noteId);
            break;
        case PluginEvent::Kind::NoteOff:
            noteOff(int(event.key), int(event.channel), event.noteId);
            break;
        case PluginEvent::Kind::NoteChoke:
            for (Voice& voice : m_voices) {
                if (voice.active() && (event.channel < 0 || voice.channel() == event.channel) &&
                    (event.noteId >= 0 ? voice.noteId() == event.noteId
                                       : voice.key() == int(event.key)))
                    voice.choke();
            }
            break;
        case PluginEvent::Kind::MidiController:
            if (event.paramIndex == 129 && event.channel >= 0 && event.channel < 16) {
                const double raw = std::clamp(event.value, 0.0, 1.0) * 16383.0 - 8192.0;
                const double bend = raw / (raw < 0.0 ? 8192.0 : 8191.0);
                m_channelBend[event.channel] = bend;
                for (Voice& voice : m_voices)
                    if (voice.active() && voice.channel() == event.channel)
                        voice.setBend(bend * value(Param::BendRange), 8.0);
            } else if (event.paramIndex == 120 || event.paramIndex == 123) {
                // All Sound Off / All Notes Off: the tail has to go too, not
                // just the held state, or a panic button leaves audio running.
                for (Voice& voice : m_voices)
                    if (voice.active() && voice.channel() == event.channel) voice.choke();
            }
            break;
        case PluginEvent::Kind::NotePitch:
        case PluginEvent::Kind::ParamGestureBegin:
        case PluginEvent::Kind::ParamGestureEnd:
        case PluginEvent::Kind::PolyPressure:
        case PluginEvent::Kind::NoteEnd:
            break;
    }
}

void SlicerInstance::applyOutputStage(float* const* out, engine::ChannelCount channels,
                                      engine::FrameCount frames) noexcept {
    const double volume = value(Param::Volume), drive = value(Param::Drive), mix = value(Param::CrushMix);
    if (!m_outputInitialized) { m_outputVolume = volume; m_outputDrive = drive; m_outputCrushMix = mix; m_outputInitialized = true; }
    if (volume == 1.0 && drive == 0.0 && mix == 0.0 && m_outputVolume == volume && m_outputDrive == drive && m_outputCrushMix == mix) return;
    const double smoothing = 1.0 - std::exp(-1.0 / (std::max(1.0, m_sampleRate) * .005));
    const double levels = std::ldexp(1.0, int(value(Param::CrushBits)) - 1);
    const int hold = int(value(Param::CrushRate));
    for (engine::FrameCount i = 0; i < frames; ++i) {
        m_outputVolume += smoothing * (volume - m_outputVolume);
        m_outputDrive += smoothing * (drive - m_outputDrive);
        m_outputCrushMix += smoothing * (mix - m_outputCrushMix);
        if (std::abs(m_outputVolume - volume) < 1e-8) m_outputVolume = volume;
        if (std::abs(m_outputDrive - drive) < 1e-8) m_outputDrive = drive;
        if (std::abs(m_outputCrushMix - mix) < 1e-8) m_outputCrushMix = mix;
        const double amount = m_outputDrive * 6.0, divisor = std::min(amount, 1.0);
        for (engine::ChannelCount ch = 0; ch < channels; ++ch) {
            double sample = double(out[ch][i]) * m_outputVolume;
            if (amount > 1e-4) sample = std::tanh(amount * sample) / divisor;
            if (m_outputCrushMix > 0.0) {
                if (m_crushCounter == 0) m_crushHeld[ch] = std::round(std::clamp(sample, -1.0, 1.0) * levels) / levels;
                sample += m_outputCrushMix * (m_crushHeld[ch] - sample);
            }
            out[ch][i] = std::isfinite(sample) ? float(sample) : 0.0f;
        }
        m_crushCounter = (m_crushCounter + 1) % hold;
    }
}

void SlicerInstance::renderSlice(const PluginProcessContext& context,
                                 const SlicerSettings& settings,
                                 std::uint32_t offset,
                                 std::uint32_t frames) noexcept {
    if (frames == 0) return;
    const auto* sample=m_renderRuntime ? m_renderRuntime->sample.get() : nullptr;
    float* slice[engine::kMaxChannels];
    const std::uint16_t channels =
        std::min<std::uint16_t>(context.outputChannels, engine::kMaxChannels);
    for (std::uint16_t ch = 0; ch < channels; ++ch)
        slice[ch] = context.outputs[ch] + offset;

    if (!sample || !sample->audio) {
        if (!context.offline)
            m_sourceTransition.process(engine::AudioBlock(slice, channels, frames), frames);
        return;
    }

    for (Voice& voice : m_voices) {
        if (!voice.active()) continue;
        voice.setBend(m_channelBend[voice.channel() & 15] * value(Param::BendRange), 8.0);
        const auto* table=m_renderRuntime->table.get();
        const int index=table ? table->indexForId(voice.sliceId()) : -1;
        if (index>=0) voice.updateSound(table->slices[index],settings,m_sampleRate);
        voice.render(*sample, settings, slice, channels, frames, m_sampleRate);
    }

    // Level and Drive belong to the instrument output, so they are applied once
    // here — to the sum — rather than to each voice's copy of a signal that has
    // not been mixed yet. Running per slice rather than per block keeps a
    // mid-block automation move audible in the block it arrived in; saturating
    // contiguous segments of one buffer is the same arithmetic as saturating
    // them together.
    applyOutputStage(slice, channels, frames);
    if (!context.offline)
        m_sourceTransition.process(engine::AudioBlock(slice, channels, frames), frames);
}

PluginProcessDisposition SlicerInstance::process(
    const PluginProcessContext& context) noexcept {
    const std::uint32_t frames = context.frames;
    if (!context.outputs || context.outputChannels == 0) {
        return PluginProcessDisposition::Continue;
    }

    // The arena hands over recycled buffers, so every channel is written here
    // before a single voice adds into it.
    for (std::uint16_t ch = 0; ch < context.outputChannels; ++ch) {
        std::fill_n(context.outputs[ch], frames, 0.0f);
    }

    const auto runtime=m_runtime.read();
    m_renderRuntime=runtime.get();
    m_renderOffline=context.offline;
    if (runtime && runtime->sourceRevision!=m_renderSourceRevision) {
        resetVoices();
        if (!context.offline) m_sourceTransition.begin(m_sampleRate);
        m_renderSourceRevision=runtime->sourceRevision;
    }
    SlicerSettings settings = snapshot();
    std::uint32_t cursor = 0;
    for (const PluginEvent& event : context.inputEvents) {
        const std::uint32_t at = std::min(event.frameOffset, frames);
        if (at > cursor) {
            renderSlice(context, settings, cursor, at - cursor);
            cursor = at;
        }
        applyEvent(event, at);
        // A parameter that moved mid-block changes what the rest of it sounds
        // like — including the settings a note started here is given.
        if (event.kind == PluginEvent::Kind::ParamValue) settings = snapshot();
    }
    if (cursor < frames) renderSlice(context, settings, cursor, frames - cursor);
    std::uint64_t keys[2]{};
    for (const auto& voice:m_voices) if (voice.active() && voice.key()>=0 && voice.key()<128)
        keys[voice.key()/64]|=std::uint64_t(1)<<(voice.key()%64);
    for (int i=0;i<2;++i) m_activeKeys[i].store(keys[i],std::memory_order_relaxed);
    m_renderRuntime=nullptr;
    return PluginProcessDisposition::Continue;
}

void SlicerInstance::reset() noexcept {
    m_sourceTransition.reset();
    resetVoices();
}

void SlicerInstance::resetVoices() noexcept {
    for (Voice& voice : m_voices) voice.kill();
    std::fill(std::begin(m_channelBend), std::end(m_channelBend), 0.0);
    m_voiceStamp = 0;
    m_crushCounter=0; m_outputInitialized=false;
    for (auto& keys:m_activeKeys) keys.store(0,std::memory_order_relaxed);
}

std::uint32_t SlicerInstance::tailSamples() const noexcept {
    // The release is what keeps sounding after the last note-off; a chop has no
    // loop of its own that outlives the key unless the Loop flag says so, and
    // that tail is inside the sample either way.
    const double release = std::max({value(Param::Release), m_sliceRelease.load(std::memory_order_relaxed), 0.003});
    return std::uint32_t(release * m_sampleRate);
}

} // namespace daw::plugins::slicer
