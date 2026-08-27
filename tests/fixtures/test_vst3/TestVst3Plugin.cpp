// A real VST3 plugin, used as the fixture for the host tests.
//
// Written straight against `pluginterfaces` — the same headers the host uses —
// rather than against `public.sdk`, which this project deliberately does not
// vendor. `U::Implements` supplies the COM boilerplate, so what is left is the
// plugin itself.
//
// Two objects, not one: a separate `IComponent` and `IEditController` wired
// together through `IConnectionPoint`. That is the shape most commercial
// plugins actually have, and it is the shape a host gets wrong — a
// single-object fixture would never exercise the connection, the controller's
// `setComponentState`, or the host's `IMessage` allocation.
//
// The DSP mirrors the CLAP fixture on purpose, so the two formats can be held
// to identical numbers: out[i] = in[i - 64] * gain + offset, and 64 samples of
// reported latency. It declares a **sidechain** input as well as a main one and
// reads both, so a host that hands over only the main bus is caught here rather
// than by a segfault inside somebody's commercial plugin.

#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr int32 kLatency = 64;
/// Written to the output when the host did not supply every declared input
/// bus. Far outside any signal the tests generate.
constexpr float kMissingBusSentinel = -999.0f;

constexpr ParamID kGainId = 100;
constexpr ParamID kOffsetId = 101;
constexpr double kGainMax = 2.0;

// Fixed class ids. Arbitrary, but they must never change: a project file that
// loaded this plugin stores them.
DECLARE_UID(kProcessorUID, 0x11112222, 0x33334444, 0x55556666, 0x77778888);
DECLARE_UID(kControllerUID, 0x99990000, 0xAAAABBBB, 0xCCCCDDDD, 0xEEEEFFFF);
// A 5.1 variant of the same processor. It exists so the host is forced to deal
// with a plugin that has more output channels than its buffer arena is wide —
// every surround plugin does, and writing those channels into a buffer sized
// for the *input* is a heap overflow on the audio thread.
DECLARE_UID(kSurroundUID, 0x12345678, 0x9ABCDEF0, 0x0FEDCBA9, 0x87654321);
DECLARE_UID(kInstrumentUID, 0xABCDEF01, 0x23456789, 0x98765432, 0x10FEDCBA);

void copyUtf16(TChar* destination, const char* text, std::size_t capacity) {
    std::size_t i = 0;
    for (; text[i] && i + 1 < capacity; ++i) destination[i] = TChar(text[i]);
    destination[i] = 0;
}

/// Plain ↔ normalised, matching what the host will compute back.
double gainFromNormalized(double normalized) { return normalized * kGainMax; }
double offsetFromNormalized(double normalized) { return normalized * 2.0 - 1.0; }

// ── Processor ──────────────────────────────────────────────────────────────

class TestProcessor : public U::Implements<U::Directly<IComponent, IAudioProcessor>,
                                           U::Indirectly<IPluginBase>> {
public:
    explicit TestProcessor(int32 outputChannels = 2, bool instrument = false)
        : m_outputChannels(outputChannels), m_instrument(instrument) {}

    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    tresult PLUGIN_API getControllerClassId(TUID classId) override {
        std::memcpy(classId, kControllerUID, sizeof(TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(IoMode) override { return kNotImplemented; }

    int32 PLUGIN_API getBusCount(MediaType type, BusDirection direction) override {
        if (type == kEvent) return m_instrument && direction == kInput ? 1 : 0;
        if (type != kAudio) return 0;
        if (m_instrument) return direction == kInput ? 0 : 1;
        // Two inputs — main and sidechain — and one output.
        return direction == kInput ? 2 : 1;
    }

    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection direction, int32 index,
                                  BusInfo& info) override {
        if (type == kEvent && m_instrument && direction == kInput && index == 0) {
            info = {};
            info.mediaType = kEvent;
            info.direction = kInput;
            info.channelCount = 16;
            info.busType = kMain;
            info.flags = BusInfo::kDefaultActive;
            copyUtf16(info.name, "MIDI In", 128);
            return kResultOk;
        }
        if (type != kAudio) return kInvalidArgument;
        if (index < 0 || index >= getBusCount(type, direction)) return kInvalidArgument;
        info.mediaType = kAudio;
        info.direction = direction;
        info.channelCount = direction == kOutput ? m_outputChannels : 2;
        info.busType = (direction == kInput && index == 1) ? kAux : kMain;
        info.flags = index == 0 ? BusInfo::kDefaultActive : 0;
        copyUtf16(info.name,
                  direction == kOutput ? "Out" : (index == 0 ? "In" : "Sidechain"),
                  128);
        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus(MediaType type, BusDirection direction,
                                   int32 index, TBool state) override {
        // The instrument deliberately refuses late activation. This turns the
        // VST3 lifecycle rule into a regression test instead of a comment.
        if (m_active) return kResultFalse;
        if (index == 0 && state) {
            if (type == kEvent && direction == kInput) m_eventInputActive = true;
            if (type == kAudio && direction == kOutput) m_audioOutputActive = true;
        }
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            if (!m_audioOutputActive || (m_instrument && !m_eventInputActive)) {
                return kResultFalse;
            }
            m_delay.assign(std::size_t(kLatency) * 2, 0.0f);
            m_writePosition = 0;
        }
        m_active = state;
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* stream) override {
        if (!stream) return kInvalidArgument;
        double values[2] = {1.0, 0.0};
        int32 read = 0;
        if (stream->read(values, sizeof(values), &read) != kResultOk ||
            read != int32(sizeof(values))) {
            return kResultFalse;
        }
        m_gain = values[0];
        m_offset = values[1];
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* stream) override {
        if (!stream) return kInvalidArgument;
        const double values[2] = {m_gain, m_offset};
        int32 written = 0;
        return stream->write(const_cast<double*>(values), sizeof(values), &written);
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement*, int32,
                                          SpeakerArrangement*, int32) override {
        // Stereo everywhere, always. Saying so plainly is more useful in a
        // fixture than pretending to negotiate.
        return kResultOk;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32,
                                         SpeakerArrangement& arrangement) override {
        arrangement = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return uint32(kLatency); }
    uint32 PLUGIN_API getTailSamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    // Deliberately `kNotImplemented`, which is what the SDK's own base class
    // answers and what a large share of shipping plugins answer. A host that
    // reads this as "do not process" silences them; this fixture is the
    // regression test for that, so it must not be made polite.
    tresult PLUGIN_API setProcessing(TBool) override { return kNotImplemented; }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.numOutputs < 1 || !data.outputs) return kResultFalse;
        const int32 frames = data.numSamples;
        float** out = data.outputs[0].channelBuffers32;

        if (m_instrument) {
            const int32 queueCount = data.inputParameterChanges
                                         ? data.inputParameterChanges->getParameterCount()
                                         : 0;
            for (int32 q = 0; q < queueCount; ++q) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(q);
                if (!queue || queue->getPointCount() == 0) continue;
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk &&
                    queue->getParameterId() == kGainId) {
                    m_gain = gainFromNormalized(value);
                }
            }
            if (data.inputEvents) {
                for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
                    Event event{};
                    if (data.inputEvents->getEvent(i, event) != kResultOk) continue;
                    if (event.type == Event::kNoteOnEvent) m_voice = event.noteOn.velocity;
                    if (event.type == Event::kNoteOffEvent) m_voice = 0.0f;
                }
            }
            for (int32 ch = 0; ch < m_outputChannels; ++ch) {
                for (int32 i = 0; i < frames; ++i) out[ch][i] = m_voice * float(m_gain);
            }
            return kResultOk;
        }

        // A host that did not supply both declared input buses gets a sentinel
        // rather than an out-of-bounds read, so the failure is a visible wrong
        // number in a test instead of a crash that reproduces only sometimes.
        if (data.numInputs < 2 || !data.inputs) {
            for (int32 ch = 0; ch < m_outputChannels; ++ch) {
                for (int32 i = 0; i < frames; ++i) out[ch][i] = kMissingBusSentinel;
            }
            return kResultFalse;
        }
        // Parameter changes that land at the top of the block are applied
        // before deciding whether there is anything to do — otherwise a plugin
        // that went silent could never be told to come back.
        {
            const int32 queues = data.inputParameterChanges
                                     ? data.inputParameterChanges->getParameterCount()
                                     : 0;
            for (int32 q = 0; q < queues; ++q) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(q);
                if (!queue) continue;
                for (int32 pt = 0; pt < queue->getPointCount(); ++pt) {
                    int32 offset = 0;
                    ParamValue value = 0.0;
                    if (queue->getPoint(pt, offset, value) != kResultOk) continue;
                    if (offset != 0) continue;
                    if (queue->getParameterId() == kGainId) m_gain = gainFromNormalized(value);
                    if (queue->getParameterId() == kOffsetId) m_offset = offsetFromNormalized(value);
                }
            }
        }

        // Test-only silence-flag diagnostic. 0.0625 is otherwise unused: the
        // output identifies whether the host marked the exact-zero main bus and
        // disconnected sidechain correctly for this block. This verifies the
        // VST3 boundary itself, rather than merely checking PluginNode's mask.
        if (m_gain == 0.0625) {
            const Steinberg::uint64 stereo = Steinberg::uint64(0x3);
            const Steinberg::uint64 mainSilent = data.inputs[0].silenceFlags & stereo;
            const Steinberg::uint64 sidechainSilent =
                data.inputs[1].silenceFlags & stereo;
            const float diagnostic =
                sidechainSilent != stereo ? -1.0f
                : (mainSilent == stereo ? 0.25f
                                        : (mainSilent == 0 ? 0.5f : -1.0f));
            for (int32 ch = 0; ch < m_outputChannels; ++ch) {
                std::fill_n(out[ch], frames, diagnostic);
            }
            data.outputs[0].silenceFlags = 0;
            return kResultOk;
        }

        // At zero gain there is nothing to output, and VST3 lets a plugin say
        // so with the bus's silence flag and skip writing the buffers
        // altogether. Plenty of shipping plugins take that shortcut the moment
        // their input goes quiet, so the fixture takes it too: a host that
        // reads the buffer anyway plays back whatever was in it.
        if (m_gain == 0.0) {
            data.outputs[0].silenceFlags = ~Steinberg::uint64(0);
            std::fill(m_delay.begin(), m_delay.end(), 0.0f);
            return kResultOk;
        }

        float** input = data.inputs[0].channelBuffers32;
        float** sidechain = data.inputs[1].channelBuffers32;

        // Parameter changes, applied at their sample offset — that is what
        // makes them worth being events rather than polled values.
        int32 nextQueue = 0;
        const int32 queueCount =
            data.inputParameterChanges ? data.inputParameterChanges->getParameterCount() : 0;

        for (int32 i = 0; i < frames; ++i) {
            for (int32 q = 0; q < queueCount; ++q) {
                IParamValueQueue* queue = data.inputParameterChanges->getParameterData(q);
                if (!queue) continue;
                const int32 points = queue->getPointCount();
                for (int32 p = 0; p < points; ++p) {
                    int32 offset = 0;
                    ParamValue value = 0.0;
                    if (queue->getPoint(p, offset, value) != kResultOk) continue;
                    if (offset > i) continue;
                    if (queue->getParameterId() == kGainId) m_gain = gainFromNormalized(value);
                    if (queue->getParameterId() == kOffsetId) {
                        m_offset = offsetFromNormalized(value);
                    }
                }
            }
            (void)nextQueue;

            const int32 slot = m_writePosition;
            // Every declared output channel is written, including the ones the
            // host has no arena for — a real surround plugin does exactly this,
            // and the host has to have somewhere for them to go.
            for (int32 ch = 0; ch < m_outputChannels; ++ch) {
                if (ch >= 2) {
                    out[ch][i] = float(m_gain);   // filler, but it must be written
                    continue;
                }
                float* ring = m_delay.data() + std::size_t(ch) * kLatency;
                // The sidechain is read, not merely accepted: a bus handed over
                // as a dangling pointer has to show up as wrong output.
                const float sample = input[ch][i] + sidechain[ch][i];
                const float delayed = ring[slot];
                ring[slot] = sample;
                out[ch][i] = float(delayed * m_gain + m_offset);
            }
            m_writePosition = (slot + 1) % kLatency;
        }
        return kResultOk;
    }

    double gain() const { return m_gain; }
    double offset() const { return m_offset; }

private:
    std::vector<float> m_delay;
    int32 m_outputChannels = 2;
    int32 m_writePosition = 0;
    double m_gain = 1.0;
    double m_offset = 0.0;
    bool m_instrument = false;
    bool m_active = false;
    bool m_eventInputActive = false;
    bool m_audioOutputActive = false;
    float m_voice = 0.0f;
};

// ── Controller ─────────────────────────────────────────────────────────────

class TestController : public U::Implements<U::Directly<IEditController, IMidiMapping>,
                                            U::Indirectly<IPluginBase>> {
public:
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    tresult PLUGIN_API setComponentState(IBStream* stream) override {
        if (!stream) return kInvalidArgument;
        double values[2] = {1.0, 0.0};
        int32 read = 0;
        if (stream->read(values, sizeof(values), &read) != kResultOk ||
            read != int32(sizeof(values))) {
            return kResultFalse;
        }
        m_gain = values[0];
        m_offset = values[1];
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    int32 PLUGIN_API getParameterCount() override {
        if (m_pendingPreset && m_handler) {
            m_pendingPreset = false;
            m_gain = 0.25;
            m_offset = 0.5;
            m_handler->restartComponent(kParamValuesChanged);
        }
        return 2;
    }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        std::memset(&info, 0, sizeof(info));
        if (index == 0) {
            info.id = kGainId;
            copyUtf16(info.title, "Gain", 128);
            copyUtf16(info.units, "x", 128);
            info.defaultNormalizedValue = 1.0 / kGainMax;   // plain 1.0
        } else if (index == 1) {
            info.id = kOffsetId;
            copyUtf16(info.title, "Offset", 128);
            info.defaultNormalizedValue = 0.5;              // plain 0.0
        } else {
            return kInvalidArgument;
        }
        info.stepCount = 0;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue normalized,
                                             String128 string) override {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f",
                      id == kGainId ? gainFromNormalized(normalized)
                                    : offsetFromNormalized(normalized));
        copyUtf16(string, text, 128);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override {
        return kNotImplemented;
    }

    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue normalized) override {
        return id == kGainId ? gainFromNormalized(normalized)
                             : offsetFromNormalized(normalized);
    }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plain) override {
        return id == kGainId ? plain / kGainMax : (plain + 1.0) / 2.0;
    }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
        return plainParamToNormalized(id, id == kGainId ? m_gain : m_offset);
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        if (id == kGainId) m_gain = gainFromNormalized(value);
        else if (id == kOffsetId) m_offset = offsetFromNormalized(value);
        else return kInvalidArgument;
        return kResultOk;
    }

    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override {
        m_handler = handler;
        // Test-only preset browser: it changes the controller wholesale and
        // reports one kParamValuesChanged, exactly as commercial synth preset
        // menus do (there are no individual performEdit calls).
        m_pendingPreset = m_handler && std::getenv("DAW_TEST_VST3_PRESET_RESTART");
        return kResultOk;
    }
    // No GUI: the host must fall back to its generic parameter panel, and that
    // fallback needs a plugin that genuinely has no editor to be tested with.
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16,
                                                    CtrlNumber controller,
                                                    ParamID& id) override {
        if (busIndex == 0 && controller == kCtrlModWheel) {
            id = kGainId;
            return kResultTrue;
        }
        return kResultFalse;
    }

private:
    IComponentHandler* m_handler = nullptr;
    bool m_pendingPreset = false;
    double m_gain = 1.0;
    double m_offset = 0.0;
};

// ── Factory ────────────────────────────────────────────────────────────────

class TestFactory : public U::Implements<U::Directly<IPluginFactory2>,
                                         U::Indirectly<IPluginFactory>> {
public:
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (!info) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::snprintf(info->vendor, sizeof(info->vendor), "DAW");
        std::snprintf(info->url, sizeof(info->url), "");
        std::snprintf(info->email, sizeof(info->email), "");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override { return 4; }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (!info) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        if (index == 0) {
            std::memcpy(info->cid, kProcessorUID, sizeof(TUID));
            std::snprintf(info->category, sizeof(info->category), kVstAudioEffectClass);
            std::snprintf(info->name, sizeof(info->name), "DAW Test Gain VST3");
        } else if (index == 1) {
            std::memcpy(info->cid, kControllerUID, sizeof(TUID));
            std::snprintf(info->category, sizeof(info->category), kVstComponentControllerClass);
            std::snprintf(info->name, sizeof(info->name), "DAW Test Gain VST3 Controller");
        } else if (index == 2) {
            std::memcpy(info->cid, kSurroundUID, sizeof(TUID));
            std::snprintf(info->category, sizeof(info->category), kVstAudioEffectClass);
            std::snprintf(info->name, sizeof(info->name), "DAW Test Gain VST3 5.1");
        } else if (index == 3) {
            std::memcpy(info->cid, kInstrumentUID, sizeof(TUID));
            std::snprintf(info->category, sizeof(info->category), kVstAudioEffectClass);
            std::snprintf(info->name, sizeof(info->name), "DAW Test Instrument VST3");
        } else {
            return kInvalidArgument;
        }
        info->cardinality = PClassInfo::kManyInstances;
        return kResultOk;
    }

    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (!info) return kInvalidArgument;
        PClassInfo base{};
        if (getClassInfo(index, &base) != kResultOk) return kInvalidArgument;
        std::memset(info, 0, sizeof(*info));
        std::memcpy(info->cid, base.cid, sizeof(TUID));
        info->cardinality = base.cardinality;
        std::snprintf(info->category, sizeof(info->category), "%s", base.category);
        std::snprintf(info->name, sizeof(info->name), "%s", base.name);
        std::snprintf(info->vendor, sizeof(info->vendor), "DAW");
        std::snprintf(info->version, sizeof(info->version), "1.0.0");
        std::snprintf(info->sdkVersion, sizeof(info->sdkVersion), kVstVersionString);
        std::snprintf(info->subCategories, sizeof(info->subCategories),
                      index == 3 ? "Instrument|Synth" : "Fx");
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        // The cast to the interface matters: `U::Implements` puts its own base
        // first, so the most-derived pointer is not the interface pointer.
        if (std::memcmp(cid, kProcessorUID, sizeof(TUID)) == 0) {
            auto* processor = new TestProcessor;
            if (processor->queryInterface(iid, obj) == kResultOk) {
                processor->release();   // queryInterface added its own reference
                return kResultOk;
            }
            processor->release();
            return kNoInterface;
        }
        if (std::memcmp(cid, kSurroundUID, sizeof(TUID)) == 0) {
            auto* processor = new TestProcessor(6);
            if (processor->queryInterface(iid, obj) == kResultOk) {
                processor->release();
                return kResultOk;
            }
            processor->release();
            return kNoInterface;
        }
        if (std::memcmp(cid, kInstrumentUID, sizeof(TUID)) == 0) {
            auto* processor = new TestProcessor(2, true);
            if (processor->queryInterface(iid, obj) == kResultOk) {
                processor->release();
                return kResultOk;
            }
            processor->release();
            return kNoInterface;
        }
        if (std::memcmp(cid, kControllerUID, sizeof(TUID)) == 0) {
            auto* controller = new TestController;
            if (controller->queryInterface(iid, obj) == kResultOk) {
                controller->release();
                return kResultOk;
            }
            controller->release();
            return kNoInterface;
        }
        return kNoInterface;
    }
};

TestFactory* g_factory = nullptr;

} // namespace

extern "C" {

#if defined(__APPLE__)
__attribute__((visibility("default"))) bool bundleEntry(CFBundleRef) { return true; }
__attribute__((visibility("default"))) bool bundleExit() { return true; }
#elif defined(_WIN32)
__declspec(dllexport) bool InitDll() { return true; }
__declspec(dllexport) bool ExitDll() { return true; }
#else
__attribute__((visibility("default"))) bool ModuleEntry(void*) { return true; }
__attribute__((visibility("default"))) bool ModuleExit() { return true; }
#endif

#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
IPluginFactory* PLUGIN_API GetPluginFactory() {
    // The factory outlives every call, so it is created once and never
    // released — releasing it on the host's behalf is how a module gets
    // unloaded out from under an open plugin.
    if (!g_factory) g_factory = new TestFactory;
    else g_factory->addRef();
    return static_cast<IPluginFactory*>(g_factory);
}

} // extern "C"
