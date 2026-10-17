// Interpose the AU entry points, as au_editor_lifetime_test does. Exercise the
// real adapter without installing a component or depending on third-party DSP.
#include "Au/AuInstance.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
struct Unit {
    bool supportsOffline = true, refuseOffline = false;
    bool offline = false, modeSet = false, active = false;
    AudioStreamBasicDescription format{};
    AURenderCallbackStruct input{};
    UInt32 delay = 0, position = 0;
    std::vector<float> ring;
    Unit() { format.mSampleRate = 48000; format.mChannelsPerFrame = 2; }
};
Unit& unit(AudioUnit instance) { return *reinterpret_cast<Unit*>(instance); }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

extern "C" OSStatus AudioUnitGetPropertyInfo(AudioUnit instance, AudioUnitPropertyID property,
    AudioUnitScope, AudioUnitElement, UInt32* size, Boolean* writable) {
    if (property != kAudioUnitProperty_OfflineRender || !unit(instance).supportsOffline)
        return kAudioUnitErr_InvalidProperty;
    *size = sizeof(UInt32); *writable = true;
    return noErr;
}

extern "C" OSStatus AudioUnitGetProperty(AudioUnit instance, AudioUnitPropertyID property,
    AudioUnitScope, AudioUnitElement, void* data, UInt32* size) {
    auto& self = unit(instance);
    switch (property) {
        case kAudioUnitProperty_ElementCount:
            *static_cast<UInt32*>(data) = 1; *size = sizeof(UInt32); return noErr;
        case kAudioUnitProperty_StreamFormat:
            *static_cast<AudioStreamBasicDescription*>(data) = self.format;
            *size = sizeof(self.format); return noErr;
        case kAudioUnitProperty_Latency:
        case kAudioUnitProperty_TailTime:
            *static_cast<Float64*>(data) = property == kAudioUnitProperty_Latency
                ? self.delay / self.format.mSampleRate : 0;
            *size = sizeof(Float64); return noErr;
        default: return kAudioUnitErr_InvalidProperty;
    }
}

extern "C" OSStatus AudioUnitSetProperty(AudioUnit instance, AudioUnitPropertyID property,
    AudioUnitScope, AudioUnitElement, const void* data, UInt32 size) {
    auto& self = unit(instance);
    if (property == kAudioUnitProperty_OfflineRender) {
        if (self.active || self.refuseOffline || size != sizeof(UInt32))
            return kAudioUnitErr_InvalidPropertyValue;
        self.offline = *static_cast<const UInt32*>(data) != 0;
        self.modeSet = true;
    } else if (property == kAudioUnitProperty_StreamFormat) {
        self.format = *static_cast<const AudioStreamBasicDescription*>(data);
    } else if (property == kAudioUnitProperty_SetRenderCallback) {
        self.input = *static_cast<const AURenderCallbackStruct*>(data);
    }
    return noErr;
}

extern "C" OSStatus AudioUnitInitialize(AudioUnit instance) {
    auto& self = unit(instance);
    if (self.supportsOffline && !self.modeSet) return kAudioUnitErr_FailedInitialization;
    self.active = true;
    self.delay = self.offline ? 137 : 19;
    self.ring.assign(2 * self.delay, 0.f); self.position = 0;
    return noErr;
}
extern "C" OSStatus AudioUnitUninitialize(AudioUnit instance) {
    unit(instance).active = false; unit(instance).modeSet = false; return noErr;
}
extern "C" OSStatus AudioUnitReset(AudioUnit instance, AudioUnitScope, AudioUnitElement) {
    auto& self = unit(instance);
    std::fill(self.ring.begin(), self.ring.end(), 0.f); self.position = 0; return noErr;
}
extern "C" OSStatus AudioUnitRender(AudioUnit instance, AudioUnitRenderActionFlags* flags,
    const AudioTimeStamp* time, UInt32, UInt32 frames, AudioBufferList* data) {
    auto& self = unit(instance);
    if (!self.active || *flags != 0 || !self.input.inputProc)
        return kAudioUnitErr_CannotDoInCurrentContext;
    const auto status = self.input.inputProc(self.input.inputProcRefCon, flags, time, 0, frames, data);
    if (status != noErr) return status;
    for (UInt32 frame = 0; frame < frames; ++frame) {
        for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch) {
            auto* samples = static_cast<float*>(data->mBuffers[ch].mData);
            auto& delayed = self.ring[ch * self.delay + self.position];
            std::swap(delayed, samples[frame]);
        }
        self.position = (self.position + 1) % self.delay;
    }
    return noErr;
}
extern "C" OSStatus AudioUnitAddPropertyListener(
    AudioUnit, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void*) { return noErr; }
extern "C" OSStatus AudioUnitRemovePropertyListenerWithUserData(
    AudioUnit, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void*) { return noErr; }
extern "C" OSStatus AudioComponentInstanceDispose(AudioComponentInstance) { return noErr; }

int main() try {
    for (bool supported : {true, false}) for (double rate : {44100., 48000., 96000.}) {
        Unit native; native.supportsOffline = supported;
        daw::plugins::AuInstance plugin(reinterpret_cast<AudioComponentInstance>(&native), {});
        require(plugin.initialize(), "initialize AU fixture");
        for (bool offline : {false, true, false}) for (unsigned block : {32u, 1024u}) {
            require(plugin.activate({rate, block, offline}), "set render mode before initialize");
            plugin.startProcessing();
            const unsigned delay = supported && offline ? 137 : 19;
            require(plugin.latencySamples() == delay, "PDC reads latency after selecting the mode");
            std::vector<float> left(block), right(block), result;
            std::array<float*, 2> output{left.data(), right.data()};
            std::vector<float> input(block, 1.f);
            std::array<const float*, 2> inputs{input.data(), input.data()};
            daw::plugins::PluginProcessContext context{};
            context.inputs = inputs.data(); context.inputChannels = 2;
            context.outputs = output.data(); context.outputChannels = 2;
            context.frames = block; context.offline = offline;
            for (unsigned at = 0; at < 512; at += block) {
                context.sampleTime = at;
                require(plugin.process(context) != daw::plugins::PluginProcessDisposition::Error,
                        "ordinary AU does not receive auol render flags");
                result.insert(result.end(), left.begin(), left.end());
            }
            for (unsigned at = 0; at < result.size(); ++at)
                require(result[at] == (at < delay ? 0.f : 1.f), "reported AU latency matches audio");
            plugin.deactivate();
        }
    }
    Unit refusing; refusing.refuseOffline = true;
    daw::plugins::AuInstance plugin(reinterpret_cast<AudioComponentInstance>(&refusing), {});
    require(plugin.initialize() && !plugin.activate({48000, 128, true}),
            "an advertised but rejected mode cannot silently export");
    std::puts("PASS AU offline activation, flags, latency and mode restoration");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
}
