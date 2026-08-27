#include "Vst3/Vst3Instance.hpp"

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vstspeaker.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace daw::plugins {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

/// VST3 strings are UTF-16 in a fixed buffer; ours are UTF-8 `std::string`.
/// Only the BMP is handled, which covers every parameter name in practice and
/// avoids dragging a converter into the plugin host.
std::string fromVstString(const TChar* text) {
    std::string out;
    for (const TChar* c = text; c && *c; ++c) {
        const char32_t code = char32_t(*c);
        if (code < 0x80) {
            out.push_back(char(code));
        } else if (code < 0x800) {
            out.push_back(char(0xC0 | (code >> 6)));
            out.push_back(char(0x80 | (code & 0x3F)));
        } else {
            out.push_back(char(0xE0 | (code >> 12)));
            out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(char(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

SpeakerArrangement arrangementFor(std::uint32_t channels) {
    switch (channels) {
        case 0: return SpeakerArr::kEmpty;
        case 1: return SpeakerArr::kMono;
        case 2: return SpeakerArr::kStereo;
        case 3: return SpeakerArr::k30Cine;
        case 4: return SpeakerArr::k40Music;
        case 5: return SpeakerArr::k50;
        case 6: return SpeakerArr::k51;
        case 7: return SpeakerArr::k60Cine;
        case 8: return SpeakerArr::k71Music;
        default: break;
    }
    return SpeakerArr::kEmpty;
}

/// The state container this host writes. VST3 has no single blob: a plugin has
/// a processor state and a controller state, and both must be restored, in that
/// order, or a plugin comes back sounding right with a wrong-looking editor.
constexpr char kStateMagic[4] = {'D', 'V', 'S', '3'};

/// Notes one block can carry. Past this the block is not music.
constexpr std::size_t kMaxEventsPerBlock = 512;

void appendUint32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(std::uint8_t(value & 0xFF));
    out.push_back(std::uint8_t((value >> 8) & 0xFF));
    out.push_back(std::uint8_t((value >> 16) & 0xFF));
    out.push_back(std::uint8_t((value >> 24) & 0xFF));
}

bool readUint32(std::span<const std::uint8_t> data, std::size_t& at,
                std::uint32_t& value) {
    if (at + 4 > data.size()) return false;
    value = std::uint32_t(data[at]) | (std::uint32_t(data[at + 1]) << 8) |
            (std::uint32_t(data[at + 2]) << 16) | (std::uint32_t(data[at + 3]) << 24);
    at += 4;
    return true;
}

} // namespace

Vst3Instance::Vst3Instance(std::shared_ptr<Vst3Module> module,
                           IComponent* component, PluginDescriptor descriptor)
    : m_module(std::move(module)), m_descriptor(std::move(descriptor)),
      m_component(component, false) {}

Vst3Instance::~Vst3Instance() {
    closeEditor();
    if (m_processing) stopProcessing();
    if (m_active) deactivate();

    // Disconnect before terminating: a plugin whose halves are still connected
    // may message a half that has already torn its state down.
    if (m_componentPoint && m_controllerPoint) {
        m_componentPoint->disconnect(m_controllerPoint);
        m_controllerPoint->disconnect(m_componentPoint);
    }
    if (m_controller) {
        m_controller->setComponentHandler(nullptr);
        // Only terminate a controller we initialised separately. When the
        // plugin is one object, terminating it here and again below is a
        // double-terminate.
        if (m_separateController) {
            m_controller->terminate();
        }
    }
    if (m_component) m_component->terminate();
}

bool Vst3Instance::initialize(IPluginFactory* factory) {
    if (!m_component) return false;

    // A controller is allowed to call the handler synchronously from
    // setComponentHandler (preset/program discovery does this in practice), so
    // callbacks must exist before the handler is handed to third-party code.
    m_handler.onGesture = [this](ParamID id, bool begin) {
        auto* listener = m_listener.load(std::memory_order_acquire);
        if (!listener) return;
        for (std::size_t i = 0; i < m_parameterIds.size(); ++i) {
            if (m_parameterIds[i] == id) listener->onParameterGesture(std::uint32_t(i), begin);
        }
    };
    m_handler.onEdit = [this](ParamID id, ParamValue normalized) {
        auto* listener = m_listener.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < m_parameterIds.size(); ++i) {
            if (m_parameterIds[i] != id) continue;
            (void)m_editorEdits.push(QueuedEdit{std::uint32_t(i), normalized});
            if (listener) {
                listener->onParameterChanged(std::uint32_t(i),
                                             toPlain(std::uint32_t(i), normalized));
            }
            break;
        }
    };
    m_handler.onRestart = [this](int32 flags) {
        m_restartFlags.fetch_or(flags, std::memory_order_release);
        auto* listener = m_listener.load(std::memory_order_acquire);
        if (flags & kLatencyChanged) {
            m_restartLatency.store(true, std::memory_order_release);
            if (listener) listener->onLatencyChanged();
        }
        if ((flags & kReloadComponent) && listener) listener->onReloadRequested();
        if ((flags & ~kReloadComponent) != 0 && listener) listener->onRestartRequested();
    };

    auto* hostContext = static_cast<IHostApplication*>(&m_hostContext);
    if (m_component->initialize(hostContext) != kResultOk) return false;

    m_processor = FUnknownPtr<IAudioProcessor>(m_component);
    if (!m_processor) return false;
    // 32-bit float is the only sample format this engine has.
    if (m_processor->canProcessSampleSize(kSample32) != kResultOk) return false;

    // The controller is either this same object or a class of its own.
    m_controller = FUnknownPtr<IEditController>(m_component);
    m_separateController = false;
    if (!m_controller) {
        TUID controllerId{};
        if (m_component->getControllerClassId(controllerId) == kResultOk && factory) {
            IEditController* separate = nullptr;
            tresult created = factory->createInstance(
                controllerId, IEditController::iid, reinterpret_cast<void**>(&separate));

            // A few otherwise valid VST3 factories only create the controller
            // for IPluginBase and expect the host to query IEditController.
            // The SDK permits querying the returned object, so accept both
            // shapes instead of producing an editorless, parameterless synth.
            IPtr<IPluginBase> controllerBase;
            if (created != kResultOk || !separate) {
                IPluginBase* rawBase = nullptr;
                created = factory->createInstance(
                    controllerId, IPluginBase::iid, reinterpret_cast<void**>(&rawBase));
                if (created == kResultOk && rawBase) {
                    controllerBase = owned(rawBase);
                    FUnknownPtr<IEditController> queried(controllerBase);
                    if (queried) {
                        separate = queried;
                        separate->addRef();
                    }
                }
            }
            if (separate) {
                m_controller = owned(separate);
                m_separateController = true;
                if (m_controller->initialize(hostContext) != kResultOk) {
                    m_controller = nullptr;
                    m_separateController = false;
                }
            }
        }
    }

    if (m_controller) {
        m_controller->setComponentHandler(&m_handler);

        // Hand the controller the processor's state, so the editor opens
        // showing what the processor is actually doing rather than defaults.
        auto stream = owned(new vst3::MemoryStream);
        if (m_component->getState(stream) == kResultOk) {
            stream->rewind();
            m_controller->setComponentState(stream);
        }

        // Wire the two halves together when they are separate objects.
        m_componentPoint = FUnknownPtr<IConnectionPoint>(m_component);
        m_controllerPoint = FUnknownPtr<IConnectionPoint>(m_controller);
        if (m_componentPoint && m_controllerPoint && m_separateController) {
            m_componentPoint->connect(m_controllerPoint);
            m_controllerPoint->connect(m_componentPoint);
        } else {
            m_componentPoint = nullptr;
            m_controllerPoint = nullptr;
        }
    }

    m_frame.onResize = [this](int32 width, int32 height) {
        if (!m_editorHost || width <= 0 || height <= 0) return false;
        m_editorHost->onEditorResized(std::uint32_t(width), std::uint32_t(height));
        return true;
    };

    m_events = owned(new vst3::EventList);
    m_outputEvents = owned(new vst3::EventList);
    m_inputChanges = owned(new vst3::ParameterChanges);
    m_outputChanges = owned(new vst3::ParameterChanges);

    readParameters();
    readMidiMappings();
    readBuses();
    refreshLatency();
    return true;
}

void Vst3Instance::readMidiMappings() {
    for (auto& channel : m_midiParameterMappings) channel.fill(-1);
    if (!m_controller) return;
    FUnknownPtr<IMidiMapping> mapping(m_controller);
    if (!mapping) return;

    for (int16 channel = 0; channel < 16; ++channel) {
        for (int32 controller = 0; controller < kCountCtrlNumber; ++controller) {
            ParamID id = kNoParamId;
            if (mapping->getMidiControllerAssignment(
                    0, channel, CtrlNumber(controller), id) != kResultTrue) {
                continue;
            }
            const auto found = m_parameterIndexById.find(id);
            if (found != m_parameterIndexById.end()) {
                m_midiParameterMappings[std::size_t(channel)][std::size_t(controller)] =
                    std::int32_t(found->second);
            }
        }
    }
}

void Vst3Instance::readParameters() {
    m_parameters.clear();
    m_parameterIds.clear();
    m_parameterConversions.clear();
    m_parameterIndexById.clear();
    if (!m_controller) return;

    const int32 count = m_controller->getParameterCount();
    m_parameters.reserve(std::size_t(count));
    m_parameterIds.reserve(std::size_t(count));
    m_parameterConversions.reserve(std::size_t(count));
    for (int32 i = 0; i < count; ++i) {
        // Both namespaces have a `ParameterInfo` and `using namespace
        // Steinberg::Vst` puts them in the same scope, so both are spelled out.
        Vst::ParameterInfo raw{};
        if (m_controller->getParameterInfo(i, raw) != kResultOk) continue;

        daw::plugins::ParameterInfo info;
        info.index = std::uint32_t(m_parameters.size());
        // The numeric ParamID, not the enumeration index: the index moves when
        // a plugin version adds a parameter, the id does not.
        info.id = std::to_string(raw.id);
        info.name = fromVstString(raw.title);
        info.unit = fromVstString(raw.units);
        // VST3 speaks normalised 0…1 everywhere; the plain scale is what this
        // host's API and its automation lanes use.
        info.minValue = m_controller->normalizedParamToPlain(raw.id, 0.0);
        info.maxValue = m_controller->normalizedParamToPlain(raw.id, 1.0);
        info.defaultValue =
            m_controller->normalizedParamToPlain(raw.id, raw.defaultNormalizedValue);
        info.isAutomatable = (raw.flags & Vst::ParameterInfo::kCanAutomate) != 0;
        info.isStepped = raw.stepCount > 0;
        // Recorded, but the host does not drive it: `PluginNode` bypasses every
        // format the same way, with a crossfade, so a plugin's own bypass
        // parameter is left for the user to reach like any other.
        info.isBypass = (raw.flags & Vst::ParameterInfo::kIsBypass) != 0;

        m_parameters.push_back(std::move(info));
        m_parameterIds.push_back(raw.id);

        ParameterConversion conversion;
        conversion.minimum = m_controller->normalizedParamToPlain(raw.id, 0.0);
        conversion.maximum = m_controller->normalizedParamToPlain(raw.id, 1.0);
        conversion.ascending = conversion.maximum >= conversion.minimum;
        // Most VST3 parameters are affine. Detect those on the control thread;
        // only genuinely curved mappings need a lookup table in process().
        constexpr double probes[] = {0.25, 0.5, 0.75};
        const double scale = std::max({1.0, std::abs(conversion.minimum),
                                       std::abs(conversion.maximum)});
        for (double normalized : probes) {
            const double actual =
                m_controller->normalizedParamToPlain(raw.id, normalized);
            const double expected = conversion.minimum +
                                    (conversion.maximum - conversion.minimum) * normalized;
            if (std::abs(actual - expected) > scale * 1.0e-9) {
                conversion.linear = false;
                break;
            }
        }
        if (!conversion.linear) {
            constexpr std::size_t kConversionSteps = 256;
            conversion.plainSamples.resize(kConversionSteps + 1);
            for (std::size_t sample = 0; sample <= kConversionSteps; ++sample) {
                conversion.plainSamples[sample] = float(
                    m_controller->normalizedParamToPlain(
                        raw.id, double(sample) / double(kConversionSteps)));
            }
        }
        m_parameterConversions.push_back(std::move(conversion));
        m_parameterIndexById.emplace(raw.id, std::uint32_t(m_parameterIds.size() - 1));
    }
}

void Vst3Instance::captureControllerValuesForProcessor() {
    m_pendingParameterValues.clear();
    m_parameterSyncPending.store(false, std::memory_order_release);
    if (!m_controller) return;

    m_pendingParameterValues.reserve(m_parameterIds.size());
    for (ParamID id : m_parameterIds) {
        m_pendingParameterValues.push_back(
            std::clamp(m_controller->getParamNormalized(id), 0.0, 1.0));
    }
    m_parameterSyncPending.store(!m_pendingParameterValues.empty(),
                                 std::memory_order_release);
}

double Vst3Instance::toPlain(std::uint32_t index, double normalized) const {
    if (index >= m_parameterConversions.size()) return normalized;
    normalized = std::clamp(normalized, 0.0, 1.0);
    const ParameterConversion& conversion = m_parameterConversions[index];
    if (conversion.linear || conversion.plainSamples.empty()) {
        return conversion.minimum +
               (conversion.maximum - conversion.minimum) * normalized;
    }
    const double at = normalized * double(conversion.plainSamples.size() - 1);
    const std::size_t left = std::min<std::size_t>(std::size_t(at),
                                                   conversion.plainSamples.size() - 1);
    const std::size_t right = std::min(left + 1, conversion.plainSamples.size() - 1);
    const double fraction = at - double(left);
    return double(conversion.plainSamples[left]) +
           (double(conversion.plainSamples[right]) -
            double(conversion.plainSamples[left])) * fraction;
}

double Vst3Instance::toNormalized(std::uint32_t index, double plain) const {
    if (index >= m_parameterConversions.size()) return std::clamp(plain, 0.0, 1.0);
    const ParameterConversion& conversion = m_parameterConversions[index];
    const double span = conversion.maximum - conversion.minimum;
    if (conversion.linear || conversion.plainSamples.empty()) {
        if (std::abs(span) <= 1.0e-15) return 0.0;
        return std::clamp((plain - conversion.minimum) / span, 0.0, 1.0);
    }

    const auto& samples = conversion.plainSamples;
    auto it = conversion.ascending
                  ? std::lower_bound(samples.begin(), samples.end(), float(plain))
                  : std::lower_bound(samples.begin(), samples.end(), float(plain),
                                     std::greater<float>());
    if (it == samples.begin()) return 0.0;
    if (it == samples.end()) return 1.0;
    const std::size_t right = std::size_t(it - samples.begin());
    const std::size_t left = right - 1;
    const double a = samples[left];
    const double b = samples[right];
    const double fraction = std::abs(b - a) <= 1.0e-15 ? 0.0 : (plain - a) / (b - a);
    return std::clamp((double(left) + fraction) / double(samples.size() - 1), 0.0, 1.0);
}

std::int32_t Vst3Instance::parameterIndexForId(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < m_parameters.size(); ++i) {
        if (m_parameters[i].id == id) return std::int32_t(i);
    }
    return -1;
}

double Vst3Instance::parameterValue(std::uint32_t index) const noexcept {
    if (!m_controller || index >= m_parameterIds.size()) return 0.0;
    return toPlain(index, m_controller->getParamNormalized(m_parameterIds[index]));
}

std::string Vst3Instance::parameterText(std::uint32_t index, double plainValue) const {
    if (!m_controller || index >= m_parameterIds.size()) return {};
    String128 text{};
    if (m_controller->getParamStringByValue(m_parameterIds[index],
                                            toNormalized(index, plainValue),
                                            text) != kResultOk) {
        return {};
    }
    return fromVstString(text);
}

// ── Buses ──────────────────────────────────────────────────────────────────

void Vst3Instance::readBuses() {
    if (!m_component) return;
    m_descriptor.wantsMidi =
        m_descriptor.isInstrument || m_component->getBusCount(kEvent, kInput) > 0;
    const int32 inputs = m_component->getBusCount(kAudio, kInput);
    const int32 outputs = m_component->getBusCount(kAudio, kOutput);

    BusInfo info{};
    if (inputs > 0 && m_component->getBusInfo(kAudio, kInput, 0, info) == kResultOk) {
        m_descriptor.mainInputChannels = std::uint16_t(info.channelCount);
    } else {
        m_descriptor.mainInputChannels = 0;
    }
    if (outputs > 0 && m_component->getBusInfo(kAudio, kOutput, 0, info) == kResultOk) {
        m_descriptor.mainOutputChannels = std::uint16_t(info.channelCount);
    }
}

PluginBusLayout Vst3Instance::busLayout() const {
    PluginBusLayout layout;
    if (m_component) {
        const auto append = [&](BusDirection direction,
                                std::vector<std::uint16_t>& buses) {
            const int32 count = m_component->getBusCount(kAudio, direction);
            for (int32 i = 0; i < count; ++i) {
                BusInfo info{};
                if (m_component->getBusInfo(kAudio, direction, i, info) ==
                    kResultOk) {
                    buses.push_back(std::uint16_t(std::max<int32>(
                        info.channelCount, 0)));
                }
            }
        };
        append(kInput, layout.inputs);
        append(kOutput, layout.outputs);
    } else {
        if (m_descriptor.mainInputChannels > 0) {
            layout.inputs.push_back(m_descriptor.mainInputChannels);
        }
        layout.outputs.push_back(m_descriptor.mainOutputChannels);
    }
    return layout;
}

bool Vst3Instance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    accepted = busLayout();
    if (!m_processor || !m_component) return false;

    const int32 inputCount = m_component->getBusCount(kAudio, kInput);
    const int32 outputCount = m_component->getBusCount(kAudio, kOutput);
    std::vector<SpeakerArrangement> inputs(std::size_t(std::max(inputCount, 0)));
    std::vector<SpeakerArrangement> outputs(std::size_t(std::max(outputCount, 0)));

    // The main bus gets what the host asked for; every other bus keeps whatever
    // the plugin already had, so asking for a stereo main does not silently
    // reconfigure someone's sidechain.
    for (int32 i = 0; i < inputCount; ++i) {
        SpeakerArrangement current = SpeakerArr::kEmpty;
        m_processor->getBusArrangement(kInput, i, current);
        inputs[std::size_t(i)] =
            (i == 0 && !wanted.inputs.empty()) ? arrangementFor(wanted.inputs[0]) : current;
    }
    for (int32 i = 0; i < outputCount; ++i) {
        SpeakerArrangement current = SpeakerArr::kEmpty;
        m_processor->getBusArrangement(kOutput, i, current);
        outputs[std::size_t(i)] =
            (i == 0 && !wanted.outputs.empty()) ? arrangementFor(wanted.outputs[0]) : current;
    }

    // A refusal is not fatal: the plugin keeps its own layout and `readBuses`
    // below records what it actually is, which is what gets adapted around.
    const tresult result =
        m_processor->setBusArrangements(inputs.empty() ? nullptr : inputs.data(), inputCount,
                                        outputs.empty() ? nullptr : outputs.data(), outputCount);
    readBuses();
    accepted = busLayout();
    return result == kResultOk;
}

bool Vst3Instance::allocateAndActivateBuses(std::uint32_t maxBlockSize) {
    m_maxBlockSize = maxBlockSize;
    m_inputBuses.clear();
    m_outputBuses.clear();
    m_inputBufferDescs.clear();
    m_outputBufferDescs.clear();
    if (!m_component) return false;

    bool activated = true;

    auto build = [&](BusDirection direction, std::vector<AudioBus>& buses,
                     std::vector<AudioBusBuffers>& descs) {
        const int32 count = std::max<int32>(m_component->getBusCount(kAudio, direction), 0);
        buses.resize(std::size_t(count));
        descs.resize(std::size_t(count));
        for (int32 i = 0; i < count; ++i) {
            BusInfo info{};
            if (m_component->getBusInfo(kAudio, direction, i, info) != kResultOk) {
                info.channelCount = 2;
            }
            AudioBus& bus = buses[std::size_t(i)];
            bus.channels = std::uint32_t(std::max<int32>(info.channelCount, 0));
            bus.storage.assign(std::size_t(bus.channels) * maxBlockSize, 0.0f);
            bus.pointers.resize(bus.channels);
            for (std::uint32_t channel = 0; channel < bus.channels; ++channel) {
                bus.pointers[channel] =
                    bus.storage.data() + std::size_t(channel) * maxBlockSize;
            }
            // Input bus 1 is the conventional sidechain. It remains active
            // even while disconnected so the wrapper can route it without a
            // second activation transition; PluginNode feeds explicit silence.
            const bool enable = i == 0 || (direction == kInput && i == 1);
            if (m_component->activateBus(kAudio, direction, i, enable) != kResultOk &&
                enable) {
                activated = false;
            }
        }
        // Only after every bus exists: `resize` above can reallocate.
        for (int32 i = 0; i < count; ++i) {
            AudioBus& bus = buses[std::size_t(i)];
            descs[std::size_t(i)] = {};
            descs[std::size_t(i)].numChannels = int32(bus.channels);
            descs[std::size_t(i)].channelBuffers32 = bus.pointers.data();
        }
    };
    build(kInput, m_inputBuses, m_inputBufferDescs);
    build(kOutput, m_outputBuses, m_outputBufferDescs);

    // VST instruments receive notes only through an activated event bus. The
    // graph currently routes one MIDI stream, therefore bus zero is the main
    // event bus and additional event buses remain explicitly disconnected.
    for (BusDirection direction : {kInput, kOutput}) {
        const int32 count = std::max<int32>(m_component->getBusCount(kEvent, direction), 0);
        for (int32 i = 0; i < count; ++i) {
            const bool enable = i == 0;
            if (m_component->activateBus(kEvent, direction, i, enable) != kResultOk &&
                enable && direction == kInput) {
                activated = false;
            }
        }
    }
    return activated;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

bool Vst3Instance::activate(const PluginProcessInfo& info) {
    if (m_active) deactivate();
    if (!m_component || !m_processor) return false;

    m_sampleRate = info.sampleRate;

    const int32 restartFlags = m_restartFlags.exchange(0, std::memory_order_acq_rel);
    if (restartFlags & (kParamTitlesChanged | kParamValuesChanged)) {
        readParameters();
    }
    if (restartFlags & (kMidiCCAssignmentChanged | kParamTitlesChanged)) {
        readMidiMappings();
    }
    readBuses();

    if (restartFlags & kParamValuesChanged) {
        // A preset browser normally changes the controller wholesale and then
        // emits one restart flag, not one performEdit per parameter. Capture
        // that complete snapshot while the processor is inactive.
        captureControllerValuesForProcessor();
    }

    // Bus arrangements and activation belong to the inactive state. A number
    // of instruments snapshot their event/audio topology in setActive(true).
    if (!allocateAndActivateBuses(info.maxBlockSize)) return false;

    ProcessSetup setup{};
    setup.processMode = info.offline ? kOffline : kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = int32(info.maxBlockSize);
    setup.sampleRate = info.sampleRate;
    if (m_processor->setupProcessing(setup) != kResultOk) return false;

    if (m_component->setActive(true) != kResultOk) return false;
    m_active = true;

    // One queue per parameter is the worst case a block of automation can
    // produce; sized here so `process` never allocates.
    const std::size_t capacity = std::max<std::size_t>(m_parameters.size(), 32);
    m_inputChanges->reserve(capacity);
    m_outputChanges->reserve(capacity);
    m_events->reserve(kMaxEventsPerBlock);
    m_outputEvents->reserve(kMaxEventsPerBlock);

    refreshLatency();
    m_restartLatency.store(false, std::memory_order_release);
    if ((restartFlags & kParamValuesChanged) && m_controller) {
        if (auto* listener = m_listener.load(std::memory_order_acquire)) {
            for (std::uint32_t index = 0; index < m_pendingParameterValues.size(); ++index) {
                listener->onParameterChanged(index,
                                             toPlain(index, m_pendingParameterValues[index]));
            }
        }
    }
    return true;
}

void Vst3Instance::deactivate() {
    if (!m_active) return;
    if (m_processing) stopProcessing();
    if (m_component) m_component->setActive(false);
    m_active = false;
}

void Vst3Instance::startProcessing() {
    if (!m_active || m_processing || !m_processor) return;
    // `setProcessing` is **optional**, and its result must not gate anything.
    // The SDK's own `AudioEffect` base returns `kNotImplemented` unless the
    // plugin overrides it, so treating a non-ok result as "do not process"
    // silences a large share of real plugins for good — Sugar Bytes Effectrix
    // among them, which is what sent us looking. Every shipping host calls it
    // and processes regardless; so do we.
    (void)m_processor->setProcessing(true);
    m_processing = true;
}

void Vst3Instance::stopProcessing() {
    if (!m_processing) return;
    if (m_processor) m_processor->setProcessing(false);
    m_processing = false;
}

void Vst3Instance::reset() noexcept {
    // VST3 has no reset call; stopping and restarting processing is how a
    // plugin is told to drop its tails.
    if (!m_processing || !m_processor) return;
    m_processor->setProcessing(false);
    m_processor->setProcessing(true);
}

void Vst3Instance::refreshLatency() {
    if (!m_processor) return;
    m_latency.store(std::uint32_t(std::max<uint32>(m_processor->getLatencySamples(), 0)),
                    std::memory_order_relaxed);
    m_tail.store(std::uint32_t(std::max<uint32>(m_processor->getTailSamples(), 0)),
                 std::memory_order_relaxed);
}

void Vst3Instance::setParameterFromHost(std::uint32_t index, double plainValue) {
    if (!m_controller || index >= m_parameterIds.size()) return;
    m_controller->setParamNormalized(m_parameterIds[index], toNormalized(index, plainValue));
}

void Vst3Instance::pumpMainThread() {
    // Restart-sensitive data is refreshed from activate(), after the engine's
    // RenderGate has stopped process(). Reading/replacing the conversion and
    // bus tables here would race the audio thread.
}

// ── State ──────────────────────────────────────────────────────────────────

bool Vst3Instance::saveState(std::vector<std::uint8_t>& out) const {
    out.clear();
    if (!m_component) return false;

    auto componentState = owned(new vst3::MemoryStream);
    if (m_component->getState(componentState) != kResultOk) return false;

    std::vector<std::uint8_t> controllerBytes;
    if (m_controller && m_separateController) {
        auto controllerState = owned(new vst3::MemoryStream);
        if (m_controller->getState(controllerState) == kResultOk) {
            controllerBytes = controllerState->data();
        }
    }

    out.insert(out.end(), kStateMagic, kStateMagic + sizeof(kStateMagic));
    appendUint32(out, std::uint32_t(componentState->data().size()));
    out.insert(out.end(), componentState->data().begin(), componentState->data().end());
    appendUint32(out, std::uint32_t(controllerBytes.size()));
    out.insert(out.end(), controllerBytes.begin(), controllerBytes.end());
    return true;
}

bool Vst3Instance::loadState(std::span<const std::uint8_t> state) {
    if (!m_component) return false;
    if (state.size() < sizeof(kStateMagic)) return false;
    if (std::memcmp(state.data(), kStateMagic, sizeof(kStateMagic)) != 0) return false;

    std::size_t at = sizeof(kStateMagic);
    std::uint32_t componentSize = 0;
    if (!readUint32(state, at, componentSize)) return false;
    if (at + componentSize > state.size()) return false;
    std::vector<std::uint8_t> componentBytes(state.begin() + std::ptrdiff_t(at),
                                             state.begin() + std::ptrdiff_t(at + componentSize));
    at += componentSize;

    std::vector<std::uint8_t> controllerBytes;
    std::uint32_t controllerSize = 0;
    if (readUint32(state, at, controllerSize) && at + controllerSize <= state.size()) {
        controllerBytes.assign(state.begin() + std::ptrdiff_t(at),
                               state.begin() + std::ptrdiff_t(at + controllerSize));
    }

    auto componentStream = owned(new vst3::MemoryStream(componentBytes));
    if (m_component->setState(componentStream) != kResultOk) return false;

    if (m_controller) {
        // The controller gets the *processor's* state as well as its own: that
        // is what makes the editor show the preset that was just loaded.
        auto mirror = owned(new vst3::MemoryStream(componentBytes));
        m_controller->setComponentState(mirror);
        if (!controllerBytes.empty() &&
            m_separateController) {
            auto controllerStream = owned(new vst3::MemoryStream(controllerBytes));
            m_controller->setState(controllerStream);
        }
        // setComponentState/setState update the controller half. The processor
        // receives the same values through IParameterChanges on its first
        // block; this also covers vendors whose component state omits params.
        captureControllerValuesForProcessor();
    }

    const std::uint32_t before = m_latency.load(std::memory_order_relaxed);
    refreshLatency();
    if (m_latency.load(std::memory_order_relaxed) != before) {
        if (auto* listener = m_listener.load(std::memory_order_acquire)) {
            listener->onLatencyChanged();
        }
    }
    return true;
}

// ── Editor ─────────────────────────────────────────────────────────────────

namespace {
const char* editorPlatformType() {
#if defined(__APPLE__)
    return kPlatformTypeNSView;
#elif defined(_WIN32)
    return kPlatformTypeHWND;
#else
    return kPlatformTypeX11EmbedWindowID;
#endif
}
} // namespace

bool Vst3Instance::hasEditor() const noexcept {
    if (!m_controller) {
        if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
            std::fprintf(stderr, "VST3 %s: no edit controller\n", m_descriptor.name.c_str());
        }
        return false;
    }
    if (m_view) return true;
    if (!m_probeView) m_probeView = owned(m_controller->createView(ViewType::kEditor));
    if (!m_probeView) {
        if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
            std::fprintf(stderr, "VST3 %s: createView returned null\n",
                         m_descriptor.name.c_str());
        }
        return false;
    }
    const bool supported =
        m_probeView->isPlatformTypeSupported(editorPlatformType()) == kResultTrue;
    if (!supported && std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
        std::fprintf(stderr, "VST3 %s: editor platform is unsupported\n",
                     m_descriptor.name.c_str());
    }
    return supported;
}

bool Vst3Instance::openEditor(void* parentHandle, PluginEditorHost* host) {
    if (m_view) return true;
    if (!parentHandle || !m_controller) return false;

    IPtr<IPlugView> view = m_probeView;
    m_probeView = nullptr;
    if (!view) view = owned(m_controller->createView(ViewType::kEditor));
    if (!view) return false;
    if (view->isPlatformTypeSupported(editorPlatformType()) != kResultTrue) return false;

    m_editorHost = host;
    // The frame goes in before `attached`: a plugin may resize itself the
    // moment it is attached, and with no frame that request is simply lost.
    view->setFrame(&m_frame);
    if (host) {
        FUnknownPtr<IPlugViewContentScaleSupport> scaleSupport(view);
        if (scaleSupport) {
            scaleSupport->setContentScaleFactor(
                IPlugViewContentScaleSupport::ScaleFactor(
                    std::max(host->contentScaleFactor(), 0.25)));
        }
    }
    const tresult attached = view->attached(parentHandle, editorPlatformType());
    if (attached != kResultOk) {
        if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
            std::fprintf(stderr, "VST3 %s: attached failed (%d)\n",
                         m_descriptor.name.c_str(), int(attached));
        }
        view->setFrame(nullptr);
        m_editorHost = nullptr;
        return false;
    }
    if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
        std::fprintf(stderr, "VST3 %s: editor attached\n", m_descriptor.name.c_str());
    }
    m_view = view;
    return true;
}

void Vst3Instance::closeEditor() {
    if (!m_view) return;
    IPtr<IPlugView> view = m_view;
    m_view = nullptr;
    m_editorHost = nullptr;
    view->removed();
    view->setFrame(nullptr);
}

bool Vst3Instance::editorSize(std::uint32_t& width, std::uint32_t& height) const {
    IPlugView* view = m_view;
    IPtr<IPlugView> probe;
    if (!view && m_controller) {
        if (!m_probeView) {
            m_probeView = owned(m_controller->createView(ViewType::kEditor));
        }
        probe = m_probeView;
        view = probe;
    }
    if (!view) return false;

    ViewRect rect{};
    if (view->getSize(&rect) != kResultOk) return false;
    if (rect.getWidth() <= 0 || rect.getHeight() <= 0) return false;
    width = std::uint32_t(rect.getWidth());
    height = std::uint32_t(rect.getHeight());
    return true;
}

bool Vst3Instance::editorCanResize() const {
    return m_view && m_view->canResize() == kResultTrue;
}

bool Vst3Instance::setEditorSize(std::uint32_t& width, std::uint32_t& height) {
    if (!m_view) return false;
    ViewRect rect{};
    rect.right = int32(width);
    rect.bottom = int32(height);
    // `checkSizeConstraint` snaps the request to something the plugin can
    // actually draw; skipping it is what leaves a strip of garbage down one
    // edge of a resized editor.
    m_view->checkSizeConstraint(&rect);
    if (rect.getWidth() <= 0 || rect.getHeight() <= 0) return false;
    width = std::uint32_t(rect.getWidth());
    height = std::uint32_t(rect.getHeight());
    return m_view->onSize(&rect) == kResultOk;
}

// ── Audio ──────────────────────────────────────────────────────────────────

PluginProcessDisposition Vst3Instance::process(
    const PluginProcessContext& context) noexcept {
    auto silence = [&] {
        for (std::uint16_t channel = 0; channel < context.outputChannels; ++channel) {
            std::fill_n(context.outputs[channel], context.frames, 0.0f);
        }
    };
    if (!m_processing || !m_processor) {
        silence();
        return PluginProcessDisposition::Continue;
    }
    if (context.frames > m_maxBlockSize) {
        silence();
        return PluginProcessDisposition::Continue;
    }
    if (m_outputBuses.empty()) {
        silence();
        return PluginProcessDisposition::Continue;
    }

    // ── Parameter changes and notes for this block ──
    m_inputChanges->clear();
    m_outputChanges->clear();
    m_events->clear();
    m_outputEvents->clear();
    if (m_parameterSyncPending.exchange(false, std::memory_order_acq_rel)) {
        const std::size_t count =
            std::min(m_parameterIds.size(), m_pendingParameterValues.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (vst3::ParamValueQueue* queue = m_inputChanges->begin(m_parameterIds[index])) {
                queue->add(0, m_pendingParameterValues[index]);
            }
        }
    }
    QueuedEdit edit;
    while (m_editorEdits.pop(edit)) {
        if (edit.parameterIndex >= m_parameterIds.size()) continue;
        if (vst3::ParamValueQueue* queue =
                m_inputChanges->begin(m_parameterIds[edit.parameterIndex])) {
            queue->add(0, std::clamp(edit.normalized, 0.0, 1.0));
        }
    }
    for (const PluginEvent& event : context.inputEvents) {
        switch (event.kind) {
            case PluginEvent::Kind::ParamValue: {
                if (event.paramIndex >= m_parameterIds.size()) break;
                vst3::ParamValueQueue* queue =
                    m_inputChanges->begin(m_parameterIds[event.paramIndex]);
                if (!queue) break;
                queue->add(int32(event.frameOffset),
                           toNormalized(event.paramIndex, event.value));
                break;
            }
            case PluginEvent::Kind::NoteOn:
            case PluginEvent::Kind::NoteOff: {
                if (m_events->full()) break;
                Event note{};
                note.busIndex = 0;
                note.sampleOffset = int32(event.frameOffset);
                note.flags = 0;
                if (event.kind == PluginEvent::Kind::NoteOn) {
                    note.type = Event::kNoteOnEvent;
                    note.noteOn.channel = int16(event.channel);
                    note.noteOn.pitch = int16(event.key);
                    note.noteOn.velocity = float(event.value);
                    // -1, not a made-up id: VST3 lets a host address a note by
                    // pitch and channel, and inventing ids would mean tracking
                    // them across blocks for no gain.
                    note.noteOn.noteId = -1;
                } else {
                    note.type = Event::kNoteOffEvent;
                    note.noteOff.channel = int16(event.channel);
                    note.noteOff.pitch = int16(event.key);
                    note.noteOff.velocity = 0.0f;
                    note.noteOff.noteId = -1;
                }
                m_events->addEvent(note);
                break;
            }
            case PluginEvent::Kind::MidiController: {
                if (event.paramIndex >= kCountCtrlNumber || event.channel < 0 ||
                    event.channel >= 16) {
                    break;
                }
                const std::int32_t parameter =
                    m_midiParameterMappings[std::size_t(event.channel)]
                                           [std::size_t(event.paramIndex)];
                if (parameter < 0 || std::size_t(parameter) >= m_parameterIds.size()) break;
                if (vst3::ParamValueQueue* queue =
                        m_inputChanges->begin(m_parameterIds[std::size_t(parameter)])) {
                    queue->add(int32(event.frameOffset),
                               std::clamp(event.value, 0.0, 1.0));
                }
                break;
            }
            case PluginEvent::Kind::PolyPressure: {
                if (m_events->full()) break;
                Event pressure{};
                pressure.type = Event::kPolyPressureEvent;
                pressure.busIndex = 0;
                pressure.sampleOffset = int32(event.frameOffset);
                pressure.polyPressure.channel = int16(event.channel);
                pressure.polyPressure.pitch = int16(event.key);
                pressure.polyPressure.pressure = float(std::clamp(event.value, 0.0, 1.0));
                pressure.polyPressure.noteId = event.noteId;
                m_events->addEvent(pressure);
                break;
            }
            default:
                break;
        }
    }

    // ── Buses: every one the plugin declared, same rule as CLAP ──
    for (std::size_t bus = 0; bus < m_inputBuses.size(); ++bus) {
        AudioBus& target = m_inputBuses[bus];
        const float* const* source =
            bus == 0 ? context.inputs
                     : (bus == 1 ? context.sidechainInputs : nullptr);
        const std::uint16_t sourceChannels =
            bus == 0 ? context.inputChannels
                     : (bus == 1 ? context.sidechainInputChannels : 0);
        const std::uint64_t sourceSilence =
            bus == 0 ? context.inputSilenceMask
                     : (bus == 1 ? context.sidechainSilenceMask : ~std::uint64_t{0});
        const std::uint32_t supplied =
            std::min<std::uint32_t>(sourceChannels, target.channels);
        std::uint64_t silent = 0;
        for (std::uint32_t channel = 0; channel < target.channels; ++channel) {
            if (channel < supplied) {
                target.pointers[channel] = const_cast<float*>(source[channel]);
                if (channel < 64 &&
                    (sourceSilence & (std::uint64_t{1} << channel)) != 0) {
                    silent |= std::uint64_t{1} << channel;
                }
                continue;
            }
            float* own = target.storage.data() + std::size_t(channel) * m_maxBlockSize;
            target.pointers[channel] = own;
            std::fill_n(own, context.frames, 0.0f);
            if (channel < 64) silent |= std::uint64_t(1) << channel;
        }
        m_inputBufferDescs[bus].channelBuffers32 = target.pointers.data();
        m_inputBufferDescs[bus].silenceFlags = silent;
    }
    for (std::size_t bus = 0; bus < m_outputBuses.size(); ++bus) {
        AudioBus& target = m_outputBuses[bus];
        const std::uint32_t supplied =
            bus == 0 ? std::min<std::uint32_t>(context.outputChannels, target.channels) : 0;
        for (std::uint32_t channel = 0; channel < target.channels; ++channel) {
            target.pointers[channel] =
                channel < supplied
                    ? context.outputs[channel]
                    : target.storage.data() + std::size_t(channel) * m_maxBlockSize;
        }
        m_outputBufferDescs[bus].channelBuffers32 = target.pointers.data();
        m_outputBufferDescs[bus].silenceFlags = 0;
    }

    // ── Transport ──
    ProcessContext transport{};
    transport.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid |
                      ProcessContext::kBarPositionValid | ProcessContext::kTimeSigValid |
                      ProcessContext::kCycleValid;
    if (context.playing) transport.state |= ProcessContext::kPlaying;
    if (context.transport.recording) transport.state |= ProcessContext::kRecording;
    if (context.transport.looping) transport.state |= ProcessContext::kCycleActive;
    transport.sampleRate = m_sampleRate;
    transport.projectTimeSamples = context.sampleTime;
    transport.continousTimeSamples = context.sampleTime;
    transport.tempo = context.transport.tempo;
    transport.projectTimeMusic = context.transport.ppqPosition;
    transport.barPositionMusic = context.transport.barStartPpq;
    transport.cycleStartMusic = context.transport.loopStartPpq;
    transport.cycleEndMusic = context.transport.loopEndPpq;
    transport.timeSigNumerator = int32(context.transport.timeSigNumerator);
    transport.timeSigDenominator = int32(context.transport.timeSigDenominator);

    ProcessData data{};
    data.processMode = context.offline ? kOffline : kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = int32(context.frames);
    data.numInputs = int32(m_inputBufferDescs.size());
    data.numOutputs = int32(m_outputBufferDescs.size());
    data.inputs = m_inputBufferDescs.empty() ? nullptr : m_inputBufferDescs.data();
    data.outputs = m_outputBufferDescs.empty() ? nullptr : m_outputBufferDescs.data();
    data.inputParameterChanges = m_inputChanges;
    data.outputParameterChanges = m_outputChanges;
    data.inputEvents = m_events;
    data.outputEvents = m_outputEvents;
    data.processContext = &transport;

    if (m_processor->process(data) != kResultOk) {
        silence();
        return PluginProcessDisposition::Continue;
    }

    // ── What the plugin said about its own output ──
    //
    // A plugin with nothing to say sets the bus's silence flag and is entitled
    // to leave the buffers untouched — VST3 says the host must then read them
    // as silence. Ours are arena buffers still holding the previous block, so
    // taking their contents at face value replays that block for as long as the
    // plugin stays quiet: a few milliseconds of audio repeating at the block
    // rate, which is a buzz, not a tail. It begins the instant the signal stops
    // — pressing Space, most obviously — which is exactly when a plugin decides
    // it has nothing left to output.
    for (std::size_t bus = 0; bus < m_outputBuses.size(); ++bus) {
        const std::uint64_t silent = m_outputBufferDescs[bus].silenceFlags;
        if (silent == 0) continue;
        AudioBus& target = m_outputBuses[bus];
        for (std::uint32_t channel = 0;
             channel < target.channels && channel < 64; ++channel) {
            if ((silent & (std::uint64_t(1) << channel)) == 0) continue;
            if (float* data32 = target.pointers[channel]) {
                std::fill_n(data32, context.frames, 0.0f);
            }
        }
    }

    // ── What the plugin moved itself ──
    if (context.outputEvents) {
        const int32 changed = m_outputChanges->getParameterCount();
        for (int32 i = 0; i < changed; ++i) {
            IParamValueQueue* queue = m_outputChanges->getParameterData(i);
            if (!queue || queue->getPointCount() == 0) continue;
            // Only the last point in the block: the host records a value, not
            // a curve, and the end of the block is what is now true.
            int32 offset = 0;
            ParamValue value = 0.0;
            if (queue->getPoint(queue->getPointCount() - 1, offset, value) != kResultOk) {
                continue;
            }
            const ParamID id = queue->getParameterId();
            const auto found = m_parameterIndexById.find(id);
            if (found != m_parameterIndexById.end()) {
                const std::uint32_t index = found->second;
                PluginEvent out;
                out.kind = PluginEvent::Kind::ParamValue;
                out.frameOffset = std::uint32_t(std::max<int32>(offset, 0));
                out.paramIndex = index;
                out.value = toPlain(index, value);
                context.outputEvents->push(out);
            }
        }

        const int32 eventCount = m_outputEvents->getEventCount();
        for (int32 i = 0; i < eventCount; ++i) {
            Event event{};
            if (m_outputEvents->getEvent(i, event) != kResultOk) continue;
            PluginEvent out;
            out.frameOffset = std::uint32_t(std::max<int32>(event.sampleOffset, 0));
            if (event.type == Event::kNoteOnEvent) {
                out.kind = PluginEvent::Kind::NoteOn;
                out.channel = event.noteOn.channel;
                out.key = event.noteOn.pitch;
                out.noteId = event.noteOn.noteId;
                out.value = event.noteOn.velocity;
            } else if (event.type == Event::kNoteOffEvent) {
                out.kind = PluginEvent::Kind::NoteOff;
                out.channel = event.noteOff.channel;
                out.key = event.noteOff.pitch;
                out.noteId = event.noteOff.noteId;
                out.value = event.noteOff.velocity;
            } else if (event.type == Event::kPolyPressureEvent) {
                out.kind = PluginEvent::Kind::PolyPressure;
                out.channel = event.polyPressure.channel;
                out.key = event.polyPressure.pitch;
                out.noteId = event.polyPressure.noteId;
                out.value = event.polyPressure.pressure;
            } else if (event.type == Event::kLegacyMIDICCOutEvent) {
                out.kind = PluginEvent::Kind::MidiController;
                out.channel = event.midiCCOut.channel;
                out.paramIndex = event.midiCCOut.controlNumber;
                if (out.paramIndex == kPitchBend) {
                    const int bend = int(std::uint8_t(event.midiCCOut.value)) |
                                     (int(std::uint8_t(event.midiCCOut.value2)) << 7);
                    out.value = double(bend) / 16383.0;
                } else if (out.paramIndex == kCtrlPolyPressure) {
                    out.kind = PluginEvent::Kind::PolyPressure;
                    out.key = std::uint8_t(event.midiCCOut.value);
                    out.value = double(std::uint8_t(event.midiCCOut.value2)) / 127.0;
                } else {
                    out.value = double(std::uint8_t(event.midiCCOut.value)) / 127.0;
                }
            } else {
                continue;
            }
            context.outputEvents->push(out);
        }
    }
    // VST3 silenceFlags describe this block only. They are not a promise that
    // a generator/LFO cannot become audible later, and VST3 has no equivalent
    // of CLAP request_process to wake a host-slept instance. Never infer Sleep.
    return PluginProcessDisposition::Continue;
}

} // namespace daw::plugins
