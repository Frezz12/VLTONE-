#include <aeffect.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

struct State {
    AEffect effect{};
    float gain = 0.5f;
};

State& state(AEffect* effect) { return *static_cast<State*>(effect->object); }

VstIntPtr dispatch(AEffect* effect, VstInt32 opcode, VstInt32 index,
                   VstIntPtr, void* ptr, float) {
    switch (opcode) {
        case effOpen: return 1;
        case effClose:
            delete &state(effect);
            return 1;
        case effGetParamName:
            if (index == 0 && ptr) std::strcpy(static_cast<char*>(ptr), "Gain");
            return 1;
        case effGetParamDisplay:
            if (index == 0 && ptr)
                std::snprintf(static_cast<char*>(ptr), 24, "%.2f",
                              state(effect).gain);
            return 1;
        case effCanBeAutomated: return index == 0;
        case effGetProgram: return 0;
        case effSetSampleRate:
        case effSetBlockSize:
        case effMainsChanged: return 1;
        default: return 0; // Deliberately no VST2 metadata opcodes.
    }
}

void setParameter(AEffect* effect, VstInt32 index, float value) {
    if (index == 0) state(effect).gain = std::clamp(value, 0.0f, 1.0f);
}

float getParameter(AEffect* effect, VstInt32 index) {
    return index == 0 ? state(effect).gain : 0.0f;
}

// VST1 accumulate-style processing. The host must clear outputs before this.
void process(AEffect* effect, float** inputs, float** outputs, VstInt32 frames) {
    for (int channel = 0; channel < 2; ++channel)
        for (VstInt32 frame = 0; frame < frames; ++frame)
            outputs[channel][frame] += inputs[channel][frame] * state(effect).gain;
}

AEffect* createLegacy() {
    auto* self = new State;
    AEffect& effect = self->effect;
    effect.magic = kEffectMagic;
    effect.dispatcher = &dispatch;
    effect.process = &process;
    effect.setParameter = &setParameter;
    effect.getParameter = &getParameter;
    effect.numPrograms = 1;
    effect.numParams = 1;
    effect.numInputs = 2;
    effect.numOutputs = 2;
    effect.object = self;
    effect.uniqueID = CCONST('V', '1', 'F', 'X');
    effect.version = 1;
    return &effect;
}

} // namespace

#if defined(_WIN32)
extern "C" __declspec(dllexport) AEffect* LegacyVstMain(audioMasterCallback) {
    return createLegacy();
}
#elif defined(__APPLE__)
extern "C" __attribute__((visibility("default")))
AEffect* main_macho(audioMasterCallback) {
    return createLegacy();
}
#else
extern "C" __attribute__((visibility("default")))
AEffect* VSTPluginMain(audioMasterCallback) {
    return createLegacy();
}
#endif
