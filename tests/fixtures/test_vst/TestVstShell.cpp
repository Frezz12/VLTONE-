#include <aeffectx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr VstInt32 kEffectId = CCONST('T', 'F', 'X', '1');
constexpr VstInt32 kInstrumentId = CCONST('T', 'I', 'N', '1');

enum class Kind { Shell, Effect, Instrument };

struct State {
    AEffect effect{};
    audioMasterCallback host = nullptr;
    Kind kind = Kind::Effect;
    float gain = 0.5f;
    float observedTempo = 0.0f;
    float observedPpq = 0.0f;
    float observedTransport = 0.0f;
    bool noteOn = false;
    bool processing = false;
    bool automated = false;
    int shellIndex = 0;
    void* editorParent = nullptr;
    ERect editorRect{0, 0, 120, 320};
};

State& state(AEffect* effect) { return *static_cast<State*>(effect->object); }

void copyText(void* target, const char* text) {
    if (target) std::strcpy(static_cast<char*>(target), text);
}

VstIntPtr dispatch(AEffect* effect, VstInt32 opcode, VstInt32 index,
                   VstIntPtr value, void* ptr, float) {
    State& self = state(effect);
    switch (opcode) {
        case effOpen: return 1;
        case effClose:
            delete &self;
            return 1;
        case effGetPlugCategory:
            if (self.kind == Kind::Shell) return kPlugCategShell;
            return self.kind == Kind::Instrument ? kPlugCategSynth : kPlugCategEffect;
        case effShellGetNextPlugin:
            if (self.kind != Kind::Shell) return 0;
            if (self.shellIndex++ == 0) {
                copyText(ptr, "DAW Test Legacy Effect");
                return kEffectId;
            }
            if (self.shellIndex == 2) {
                copyText(ptr, "DAW Test Legacy Instrument");
                return kInstrumentId;
            }
            return 0;
        case effGetEffectName:
            copyText(ptr, self.kind == Kind::Instrument
                              ? "DAW Test Legacy Instrument"
                              : (self.kind == Kind::Effect
                                     ? "DAW Test Legacy Effect"
                                     : "DAW Test VST Shell"));
            return 1;
        case effGetVendorString:
            copyText(ptr, "VLT Tests");
            return 1;
        case effGetVendorVersion: return 100;
        case effGetVstVersion: return kVstVersion;
        case effCanDo:
            if (!ptr) return 0;
            return std::strcmp(static_cast<const char*>(ptr),
                               "receiveVstMidiEvent") == 0 ||
                           std::strcmp(static_cast<const char*>(ptr),
                                       "sendVstMidiEvent") == 0
                       ? 1
                       : 0;
        case effGetParamName:
            if (index == 0) copyText(ptr, "Gain");
            else if (index == 1) copyText(ptr, "Host Tempo");
            else if (index == 2) copyText(ptr, "Host PPQ");
            else if (index == 3) copyText(ptr, "Transport");
            return 1;
        case effGetParamLabel:
            copyText(ptr, index == 1 ? "BPM" : "");
            return 1;
        case effGetParamDisplay:
            if (ptr) std::snprintf(static_cast<char*>(ptr), 24, "%.3f",
                                   effect->getParameter(effect, index));
            return 1;
        case effCanBeAutomated: return index == 0;
        case effGetProgram: return 0;
        case effSetProgram: return value == 0;
        case effGetChunk:
            if (self.kind != Kind::Effect || !ptr) return 0;
            *static_cast<void**>(ptr) = &self.gain;
            return sizeof(self.gain);
        case effSetChunk:
            if (self.kind != Kind::Effect || value != sizeof(self.gain) || !ptr)
                return 0;
            std::memcpy(&self.gain, ptr, sizeof(self.gain));
            return 1;
        case effProcessEvents: {
            if (!ptr) return 0;
            auto* events = static_cast<VstEvents*>(ptr);
            for (VstInt32 i = 0; i < events->numEvents; ++i) {
                if (!events->events[i] || events->events[i]->type != kVstMidiType)
                    continue;
                const auto* midi =
                    reinterpret_cast<const VstMidiEvent*>(events->events[i]);
                const unsigned char type = midi->midiData[0] & 0xF0;
                if (type == 0x90 && midi->midiData[2] != 0) self.noteOn = true;
                if (type == 0x80 || (type == 0x90 && midi->midiData[2] == 0))
                    self.noteOn = false;
            }
            // Echo every supported MIDI event to exercise audioMasterProcessEvents.
            if (self.host) self.host(effect, audioMasterProcessEvents, 0, 0, ptr, 0.0f);
            return 1;
        }
        case effSetSampleRate:
        case effSetBlockSize:
        case effMainsChanged: return 1;
        case effStartProcess:
            self.processing = true;
            return 1;
        case effStopProcess:
            self.processing = false;
            return 1;
        case effGetTailSize: return 256;
        case effEditGetRect:
            if (ptr) *static_cast<ERect**>(ptr) = &self.editorRect;
            return 1;
        case effEditOpen:
            self.editorParent = ptr;
            if (self.host) self.host(effect, audioMasterSizeWindow, 320, 120,
                                     nullptr, 0.0f);
            return ptr ? 1 : 0;
        case effEditClose:
            self.editorParent = nullptr;
            return 1;
        case effEditIdle: return self.editorParent ? 1 : 0;
        default: return 0;
    }
}

void setParameter(AEffect* effect, VstInt32 index, float value) {
    State& self = state(effect);
    if (index == 0) self.gain = std::clamp(value, 0.0f, 1.0f);
}

float getParameter(AEffect* effect, VstInt32 index) {
    const State& self = state(effect);
    if (index == 0) return self.gain;
    if (index == 1) return self.observedTempo;
    if (index == 2) return self.observedPpq;
    if (index == 3) return self.observedTransport;
    return 0.0f;
}

void processReplacing(AEffect* effect, float** inputs, float** outputs,
                      VstInt32 frames) {
    State& self = state(effect);
    if (self.host) {
        const auto* time = reinterpret_cast<const VstTimeInfo*>(
            self.host(effect, audioMasterGetTime, 0, 0, nullptr, 0.0f));
        if (time) {
            self.observedTempo = float(time->tempo / 300.0);
            self.observedPpq = float(time->ppqPos / 16.0);
            self.observedTransport =
                (time->flags & kVstTransportPlaying) != 0 &&
                        (time->flags & kVstTransportCycleActive) != 0 &&
                        time->timeSigNumerator == 3 &&
                        time->timeSigDenominator == 4
                    ? 1.0f
                    : 0.0f;
        }
        if (!self.automated) {
            self.host(effect, audioMasterBeginEdit, 0, 0, nullptr, 0.0f);
            self.host(effect, audioMasterAutomate, 0, 0, nullptr, self.gain);
            self.host(effect, audioMasterEndEdit, 0, 0, nullptr, 0.0f);
            self.automated = true;
        }
    }
    for (int channel = 0; channel < effect->numOutputs; ++channel) {
        for (VstInt32 frame = 0; frame < frames; ++frame) {
            if (self.kind == Kind::Instrument)
                outputs[channel][frame] = self.noteOn ? self.gain : 0.0f;
            else
                outputs[channel][frame] = inputs[channel][frame] * self.gain;
        }
    }
}

AEffect* makeEffect(audioMasterCallback host, Kind kind) {
    auto* self = new State;
    self->host = host;
    self->kind = kind;
    AEffect& effect = self->effect;
    effect.magic = kEffectMagic;
    effect.dispatcher = &dispatch;
    effect.setParameter = &setParameter;
    effect.getParameter = &getParameter;
    effect.numPrograms = 1;
    effect.numParams = kind == Kind::Effect ? 4 : (kind == Kind::Instrument ? 1 : 0);
    effect.numInputs = kind == Kind::Instrument ? 0 : 2;
    effect.numOutputs = kind == Kind::Shell ? 0 : 2;
    effect.flags = effFlagsCanReplacing;
    if (kind == Kind::Effect)
        effect.flags |= effFlagsProgramChunks | effFlagsHasEditor;
    if (kind == Kind::Instrument) effect.flags |= effFlagsIsSynth;
    effect.initialDelay = kind == Kind::Effect ? 17 : 0;
    effect.object = self;
    effect.uniqueID = kind == Kind::Effect
                          ? kEffectId
                          : (kind == Kind::Instrument ? kInstrumentId
                                                      : CCONST('S', 'H', 'L', 'L'));
    effect.version = 100;
    effect.processReplacing = &processReplacing;
    return &effect;
}

} // namespace

#if defined(_WIN32)
#define VST_EXPORT extern "C" __declspec(dllexport)
#else
#define VST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VST_EXPORT AEffect* VSTPluginMain(audioMasterCallback host) {
    if (!host || host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0)
        return nullptr;
    const VstInt32 requested =
        static_cast<VstInt32>(host(nullptr, audioMasterCurrentId, 0, 0, nullptr, 0.0f));
    if (requested == kEffectId) return makeEffect(host, Kind::Effect);
    if (requested == kInstrumentId) return makeEffect(host, Kind::Instrument);
    return makeEffect(host, Kind::Shell);
}
