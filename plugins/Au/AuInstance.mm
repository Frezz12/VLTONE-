#include "Au/AuInstance.hpp"

#include "Au/AuTiming.hpp"

#include <cmath>

#import <AppKit/AppKit.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstring>

namespace daw::plugins {

namespace {

/// AU reports latency and tail in **seconds**, not samples — the one unit
/// mismatch in the format that silently produces a plausible wrong answer.
std::uint32_t secondsToSamples(Float64 seconds, double sampleRate) {
    if (!(seconds > 0.0) || !(sampleRate > 0.0)) return 0;
    return std::uint32_t(seconds * sampleRate + 0.5);
}

std::string toUtf8(CFStringRef text) {
    if (!text) return {};
    const CFIndex length = CFStringGetLength(text);
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(std::size_t(capacity), '\0');
    if (!CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

/// Bytes an `AudioBufferList` needs for this many channels.
std::size_t bufferListBytes(std::uint32_t channels) {
    return sizeof(AudioBufferList) +
           (channels > 0 ? (channels - 1) * sizeof(AudioBuffer) : 0);
}

} // namespace

AuInstance::AuInstance(AudioComponentInstance unit, PluginDescriptor descriptor)
    : m_unit(unit), m_descriptor(std::move(descriptor)) {}

AuInstance::~AuInstance() {
    // The whole teardown inside a pool, for the reason spelled out in
    // `closeEditor`: nothing the view queues on its way out may still be
    // pending when the unit underneath it is disposed.
    @autoreleasepool {
        closeEditor();
        if (m_processing) stopProcessing();
        if (m_active) deactivate();
        if (m_unit) {
            AudioUnitRemovePropertyListenerWithUserData(m_unit, kAudioUnitProperty_Latency,
                                                        &AuInstance::propertyChanged, this);
            AudioComponentInstanceDispose(m_unit);
        }
    }
}

bool AuInstance::initialize() {
    if (!m_unit) return false;
    // `MusicDeviceMIDIEvent` on a plain `aufx` is not merely ignored — it is a
    // call into a component that does not implement it, so the check is the
    // descriptor's rather than a return code's.
    m_acceptsMidi = m_descriptor.wantsMidi || m_descriptor.isInstrument;
    readParameters();
    readBuses();
    refreshLatency();
    // The unit tells the host when its latency moves — usually on a preset
    // load. Without this the graph's delay compensation goes quietly stale.
    AudioUnitAddPropertyListener(m_unit, kAudioUnitProperty_Latency,
                                 &AuInstance::propertyChanged, this);
    return true;
}

void AuInstance::propertyChanged(void* context, AudioUnit, AudioUnitPropertyID property,
                                 AudioUnitScope, AudioUnitElement) {
    auto* self = static_cast<AuInstance*>(context);
    if (!self || property != kAudioUnitProperty_Latency) return;
    self->refreshLatency();
    if (auto* listener = self->m_listener.load(std::memory_order_acquire)) {
        listener->onLatencyChanged();
    }
}

void AuInstance::refreshLatency() {
    if (!m_unit) return;
    Float64 seconds = 0.0;
    UInt32 size = sizeof(seconds);
    if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global,
                             0, &seconds, &size) == noErr) {
        m_latency.store(secondsToSamples(seconds, m_sampleRate), std::memory_order_relaxed);
    }
    seconds = 0.0;
    size = sizeof(seconds);
    if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_TailTime, kAudioUnitScope_Global,
                             0, &seconds, &size) == noErr) {
        m_tail.store(secondsToSamples(seconds, m_sampleRate), std::memory_order_relaxed);
    }
}

// ── Parameters ─────────────────────────────────────────────────────────────

void AuInstance::readParameters() {
    m_parameters.clear();
    m_parameterIds.clear();
    if (!m_unit) return;

    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo(m_unit, kAudioUnitProperty_ParameterList,
                                 kAudioUnitScope_Global, 0, &size, &writable) != noErr ||
        size == 0) {
        return;
    }

    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_ParameterList,
                             kAudioUnitScope_Global, 0, ids.data(), &size) != noErr) {
        return;
    }

    for (AudioUnitParameterID id : ids) {
        // The parameter id is the *element* of this property, not an argument.
        AudioUnitParameterInfo raw{};
        UInt32 infoSize = sizeof(raw);
        if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_ParameterInfo,
                                 kAudioUnitScope_Global, id, &raw, &infoSize) != noErr) {
            continue;
        }

        ParameterInfo info;
        info.index = std::uint32_t(m_parameters.size());
        // The numeric id, not the position: a plugin update can reorder the
        // list, and the id is what a project file and an automation lane hold.
        info.id = std::to_string(id);
        if ((raw.flags & kAudioUnitParameterFlag_HasCFNameString) && raw.cfNameString) {
            info.name = toUtf8(raw.cfNameString);
            // Ours to release only when the plugin says so.
            if (raw.flags & kAudioUnitParameterFlag_CFNameRelease) {
                CFRelease(raw.cfNameString);
            }
        }
        if (info.name.empty()) info.name = raw.name;
        info.minValue = raw.minValue;
        info.maxValue = raw.maxValue;
        info.defaultValue = raw.defaultValue;
        // AU is already in plain units — no normalisation anywhere in this file.
        info.isAutomatable = (raw.flags & kAudioUnitParameterFlag_NonRealTime) == 0;
        info.isStepped = raw.unit == kAudioUnitParameterUnit_Indexed ||
                         raw.unit == kAudioUnitParameterUnit_Boolean;
        info.isBypass = false;
        if (raw.unit == kAudioUnitParameterUnit_Decibels) info.unit = "dB";
        else if (raw.unit == kAudioUnitParameterUnit_Hertz) info.unit = "Hz";
        else if (raw.unit == kAudioUnitParameterUnit_Percent) info.unit = "%";
        else if (raw.unit == kAudioUnitParameterUnit_Seconds) info.unit = "s";
        else if (raw.unit == kAudioUnitParameterUnit_Milliseconds) info.unit = "ms";

        m_parameters.push_back(std::move(info));
        m_parameterIds.push_back(id);
    }
}

std::int32_t AuInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < m_parameters.size(); ++i) {
        if (m_parameters[i].id == id) return std::int32_t(i);
    }
    return -1;
}

double AuInstance::parameterValue(std::uint32_t index) const noexcept {
    if (!m_unit || index >= m_parameterIds.size()) return 0.0;
    AudioUnitParameterValue value = 0.0f;
    if (AudioUnitGetParameter(m_unit, m_parameterIds[index], kAudioUnitScope_Global, 0,
                              &value) != noErr) {
        return 0.0;
    }
    return double(value);
}

std::string AuInstance::parameterText(std::uint32_t index, double plainValue) const {
    if (!m_unit || index >= m_parameterIds.size()) return {};
    AudioUnitParameterValue value = AudioUnitParameterValue(plainValue);
    AudioUnitParameterStringFromValue query{};
    query.inParamID = m_parameterIds[index];
    query.inValue = &value;
    query.outString = nullptr;
    UInt32 size = sizeof(query);
    if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_ParameterStringFromValue,
                             kAudioUnitScope_Global, 0, &query, &size) != noErr ||
        !query.outString) {
        // Most units have no string formatter; the caller falls back to the
        // number, which is better than an empty label.
        return {};
    }
    const std::string text = toUtf8(query.outString);
    CFRelease(query.outString);
    return text;
}

// ── Buses ──────────────────────────────────────────────────────────────────

bool AuInstance::setStreamFormat(AudioUnitScope scope, AudioUnitElement element,
                                 std::uint32_t channels) {
    if (!m_unit || channels == 0) return true;
    AudioStreamBasicDescription format{};
    format.mSampleRate = m_sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    // Non-interleaved float is what the engine's planar buffers already are;
    // asking for anything else would mean a conversion on every block.
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                          kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    return AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat, scope,
                                element,
                                &format, sizeof(format)) == noErr;
}

void AuInstance::readBuses() {
    if (!m_unit) return;

    auto elementCount = [&](AudioUnitScope scope) -> std::uint32_t {
        UInt32 count = 0;
        UInt32 size = sizeof(count);
        if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_ElementCount, scope, 0,
                                 &count, &size) != noErr) {
            return 0;
        }
        return count;
    };
    auto channelsOf = [&](AudioUnitScope scope,
                          AudioUnitElement element) -> std::uint32_t {
        AudioStreamBasicDescription format{};
        UInt32 size = sizeof(format);
        if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_StreamFormat, scope,
                                 element,
                                 &format, &size) != noErr) {
            return 0;
        }
        return format.mChannelsPerFrame;
    };

    const std::uint32_t inputCount = elementCount(kAudioUnitScope_Input);
    m_inputBusChannels.clear();
    for (std::uint32_t element = 0; element < inputCount; ++element) {
        m_inputBusChannels.push_back(
            channelsOf(kAudioUnitScope_Input, element));
    }
    m_inputChannels = m_inputBusChannels.empty() ? 0 : m_inputBusChannels.front();
    m_outputChannels = elementCount(kAudioUnitScope_Output) > 0
                           ? channelsOf(kAudioUnitScope_Output, 0)
                           : 0;
    // An instrument has no audio input and the graph has to know that, or it
    // waits for a producer that will never be connected.
    m_descriptor.mainInputChannels = std::uint16_t(m_inputChannels);
    m_descriptor.mainOutputChannels = std::uint16_t(m_outputChannels);
}

PluginBusLayout AuInstance::busLayout() const {
    PluginBusLayout layout;
    for (const std::uint32_t channels : m_inputBusChannels) {
        layout.inputs.push_back(std::uint16_t(channels));
    }
    layout.outputs.push_back(std::uint16_t(m_outputChannels));
    return layout;
}

bool AuInstance::setBusLayout(const PluginBusLayout& wanted, PluginBusLayout& accepted) {
    if (!wanted.inputs.empty() && m_inputChannels > 0) {
        if (setStreamFormat(kAudioUnitScope_Input, 0, wanted.inputs[0])) {
            m_inputChannels = wanted.inputs[0];
        }
    }
    if (!wanted.outputs.empty()) {
        if (setStreamFormat(kAudioUnitScope_Output, 0, wanted.outputs[0])) {
            m_outputChannels = wanted.outputs[0];
        }
    }
    readBuses();
    accepted = busLayout();
    return true;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

bool AuInstance::activate(const PluginProcessInfo& info) {
    if (m_active) deactivate();
    if (!m_unit) return false;

    m_sampleRate = info.sampleRate;
    m_maxBlockSize = info.maxBlockSize;
    m_renderTime = 0;

    // Reapply the layout negotiated by PluginNode. The graph arena itself is
    // stereo, but a mono track may have selected the unit's mono processing
    // variant; forcing two channels here used to silently undo that choice at
    // activation time. A unit that refused negotiation keeps its own counts.
    if (m_inputChannels > 0) {
        setStreamFormat(kAudioUnitScope_Input, 0, m_inputChannels);
    }
    if (m_outputChannels > 0) {
        setStreamFormat(kAudioUnitScope_Output, 0, m_outputChannels);
    }
    readBuses();

    UInt32 maxFrames = info.maxBlockSize;
    AudioUnitSetProperty(m_unit, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));

    // The unit pulls its input rather than being handed it — this is where the
    // host says who to pull from.
    for (std::uint32_t bus = 0; bus < m_inputBusChannels.size(); ++bus) {
        if (m_inputBusChannels[bus] == 0) continue;
        AURenderCallbackStruct callback{};
        callback.inputProc = &AuInstance::renderInput;
        callback.inputProcRefCon = this;
        AudioUnitSetProperty(m_unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, bus, &callback,
                             sizeof(callback));
    }

    // Before initialize: a unit reads the host's musical-time callbacks when it
    // sets itself up, and one installed afterwards can be missed for good.
    installHostCallbacks();

    if (AudioUnitInitialize(m_unit) != noErr) return false;
    m_active = true;

    // Sized once, here: `process` must not allocate. The scratch covers output
    // channels the caller has not supplied, the silence buffer covers input
    // channels it has not supplied.
    m_outputListStorage.assign(bufferListBytes(std::max<std::uint32_t>(m_outputChannels, 1)),
                               0);
    m_scratch.assign(std::size_t(std::max<std::uint32_t>(m_outputChannels, 1)) *
                         info.maxBlockSize,
                     0.0f);
    const std::uint32_t widestInput = m_inputBusChannels.empty()
        ? 1
        : *std::max_element(m_inputBusChannels.begin(), m_inputBusChannels.end());
    m_silence.assign(std::size_t(std::max<std::uint32_t>(widestInput, 1)) *
                         info.maxBlockSize,
                     0.0f);

    refreshLatency();
    return true;
}

void AuInstance::deactivate() {
    if (!m_active) return;
    if (m_processing) stopProcessing();
    if (m_unit) AudioUnitUninitialize(m_unit);
    m_active = false;
}

void AuInstance::startProcessing() {
    if (!m_active || m_processing) return;
    m_processing = true;
}

void AuInstance::stopProcessing() {
    if (!m_processing) return;
    m_processing = false;
    // AU has no "stop processing"; resetting is how a unit is told to drop the
    // tail it is still ringing out.
    if (m_unit) AudioUnitReset(m_unit, kAudioUnitScope_Global, 0);
}

void AuInstance::reset() noexcept {
    if (m_unit) AudioUnitReset(m_unit, kAudioUnitScope_Global, 0);
    // The render clock restarts with the unit's own state; a reset is the one
    // place where going back to zero cannot look like a repeated block.
    m_renderTime = 0;
}

// ── State ──────────────────────────────────────────────────────────────────

bool AuInstance::saveState(std::vector<std::uint8_t>& out) const {
    out.clear();
    if (!m_unit) return false;

    CFPropertyListRef classInfo = nullptr;
    UInt32 size = sizeof(classInfo);
    if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                             0, &classInfo, &size) != noErr ||
        !classInfo) {
        return false;
    }

    // Binary rather than XML: an AU's class info holds the whole preset, and
    // for a sample-based instrument the XML form is many times the size.
    CFErrorRef error = nullptr;
    CFDataRef data = CFPropertyListCreateData(nullptr, classInfo,
                                              kCFPropertyListBinaryFormat_v1_0, 0, &error);
    CFRelease(classInfo);
    if (!data) {
        if (error) CFRelease(error);
        return false;
    }
    const UInt8* bytes = CFDataGetBytePtr(data);
    out.assign(bytes, bytes + CFDataGetLength(data));
    CFRelease(data);
    return true;
}

bool AuInstance::loadState(std::span<const std::uint8_t> state) {
    if (!m_unit || state.empty()) return false;

    CFDataRef data = CFDataCreate(nullptr, state.data(), CFIndex(state.size()));
    if (!data) return false;
    CFPropertyListRef classInfo =
        CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (!classInfo) return false;

    const OSStatus status =
        AudioUnitSetProperty(m_unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                             0, &classInfo, sizeof(classInfo));
    CFRelease(classInfo);
    if (status != noErr) return false;

    // A preset can move the latency, and it can change every parameter — the
    // editor and the graph both have to be told.
    const std::uint32_t before = m_latency.load(std::memory_order_relaxed);
    refreshLatency();
    if (auto* listener = m_listener.load(std::memory_order_acquire)) {
        if (m_latency.load(std::memory_order_relaxed) != before) listener->onLatencyChanged();
    }
    return true;
}

// ── Editor ─────────────────────────────────────────────────────────────────

bool AuInstance::hasEditor() const noexcept {
    if (!m_unit) return false;
    UInt32 size = 0;
    Boolean writable = false;
    return AudioUnitGetPropertyInfo(m_unit, kAudioUnitProperty_CocoaUI,
                                    kAudioUnitScope_Global, 0, &size, &writable) == noErr &&
           size >= sizeof(AudioUnitCocoaViewInfo);
}

bool AuInstance::openEditor(void* parentHandle, PluginEditorHost* host) {
    if (m_editorView) return true;
    if (!m_unit || !parentHandle) return false;

    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo(m_unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global,
                                 0, &size, &writable) != noErr ||
        size < sizeof(AudioUnitCocoaViewInfo)) {
        return false;
    }

    // The property is variable-length: one bundle URL followed by one class
    // name per view the unit offers. Only the first is used.
    //
    // The storage is sized from the *first* query, and the get below writes
    // back how much the unit actually produced — which can be less. Reading
    // `mCocoaAUViewClass[0]` without re-checking that is a read past the end
    // of this vector, and the garbage that comes back is handed straight to
    // ARC: the crash lands inside `objc_retain` with no hint of where the
    // pointer came from. It has happened.
    std::vector<std::uint8_t> storage(std::max<UInt32>(size, sizeof(AudioUnitCocoaViewInfo)), 0);
    UInt32 produced = size;
    auto* viewInfo = reinterpret_cast<AudioUnitCocoaViewInfo*>(storage.data());
    if (AudioUnitGetProperty(m_unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                             viewInfo, &produced) != noErr) {
        return false;
    }
    if (produced < sizeof(AudioUnitCocoaViewInfo)) return false;

    // Every view class the unit named, so the ones past the first are released
    // rather than leaked. The property hands over +1 references.
    const std::size_t classCount =
        1 + (produced - sizeof(AudioUnitCocoaViewInfo)) / sizeof(CFStringRef);
    const auto releaseClasses = [&](std::size_t from) {
        for (std::size_t i = from; i < classCount; ++i) {
            if (viewInfo->mCocoaAUViewClass[i]) CFRelease(viewInfo->mCocoaAUViewClass[i]);
        }
    };

    // Typed before bridged. A unit that fills the struct with something that
    // is not a CFURL/CFString is a broken unit, but it must not be able to
    // take the whole program down with it.
    CFURLRef bundleUrl = viewInfo->mCocoaAUViewBundleLocation;
    CFStringRef classNameRef = viewInfo->mCocoaAUViewClass[0];
    const bool urlOk = bundleUrl && CFGetTypeID(bundleUrl) == CFURLGetTypeID();
    const bool classOk = classNameRef && CFGetTypeID(classNameRef) == CFStringGetTypeID();
    if (!urlOk || !classOk) {
        if (bundleUrl) CFRelease(bundleUrl);
        releaseClasses(0);
        return false;
    }

    NSBundle* bundle = [NSBundle bundleWithURL:(__bridge NSURL*)bundleUrl];
    NSString* className = (__bridge NSString*)classNameRef;
    // Released here rather than at the end: nothing below can fail in a way
    // that would want them again.
    CFRelease(bundleUrl);

    if (!bundle) {
        releaseClasses(0);
        return false;
    }
    Class factoryClass = [bundle classNamed:className];
    releaseClasses(0);
    if (!factoryClass) return false;
    if (![factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)]) return false;

    id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
    if (!factory) return false;
    NSView* view = [factory uiViewForAudioUnit:m_unit withSize:NSZeroSize];
    if (!view) return false;

    NSView* parent = (__bridge NSView*)parentHandle;
    [view setFrame:NSMakeRect(0, 0, view.frame.size.width, view.frame.size.height)];
    [parent addSubview:view];

    // Retained explicitly: the parent holds a reference, but the host outlives
    // any particular parent and must be able to detach the view itself.
    m_editorView = (__bridge_retained void*)view;
    m_editorFactory = (__bridge_retained void*)factory;
    m_editorHost = host;
    return true;
}

void AuInstance::closeEditor() {
    if (!m_editorView) return;
    // Inside a pool of its own, and this is not tidiness.
    //
    // An AU's Cocoa view keeps a timer on the main run loop that reads the
    // unit it is showing. The view is supposed to stop that timer when it is
    // deallocated — but ARC's release here only *queues* the deallocation if
    // anything on the way out autoreleases the view, and the enclosing pool
    // may not drain until well after the caller has gone on to dispose the
    // AudioUnit. The timer then fires against freed memory. That is not a
    // theoretical window: it reproduces on the first plugin whose view uses
    // one, and it lands as "memory corruption of free block" somewhere else
    // entirely, minutes later.
    //
    // Draining here makes the view's dealloc — and the timer it invalidates —
    // happen before this function returns, while the unit is still alive to be
    // read one last time.
    @autoreleasepool {
        NSView* view = (__bridge_transfer NSView*)m_editorView;
        id factory = (__bridge_transfer id)m_editorFactory;
        m_editorView = nullptr;
        m_editorFactory = nullptr;
        m_editorHost = nullptr;
        [view removeFromSuperview];
        // The view goes first and the factory second, explicitly. ARC destroys
        // locals in reverse order of declaration, which would have released the
        // factory — and with it whatever its bundle owns — while the view it
        // built was still alive and about to run its own dealloc.
        view = nil;
        factory = nil;
    }
}

bool AuInstance::editorSize(std::uint32_t& width, std::uint32_t& height) const {
    if (!m_editorView) return false;
    NSView* view = (__bridge NSView*)m_editorView;
    const NSSize size = view.frame.size;
    if (size.width <= 0 || size.height <= 0) return false;
    width = std::uint32_t(size.width);
    height = std::uint32_t(size.height);
    return true;
}

bool AuInstance::editorCanResize() const {
    // AU views size themselves; a host that stretches one gets a stretched
    // control layout rather than a bigger editor.
    return false;
}

bool AuInstance::setEditorSize(std::uint32_t&, std::uint32_t&) { return false; }

// ── Audio ──────────────────────────────────────────────────────────────────

OSStatus AuInstance::renderInput(void* context, AudioUnitRenderActionFlags* flags,
                                 const AudioTimeStamp*, UInt32 bus, UInt32 frames,
                                 AudioBufferList* data) {
    auto* self = static_cast<AuInstance*>(context);
    if (!self || !data) return noErr;

    const std::uint64_t silenceMask =
        bus == 0 ? self->m_blockInputSilenceMask
                 : (bus == 1 ? self->m_blockSidechainSilenceMask
                             : ~std::uint64_t{0});
    bool allSilent = true;
    for (UInt32 channel = 0; channel < data->mNumberBuffers; ++channel) {
        AudioBuffer& buffer = data->mBuffers[channel];
        const float* source = nullptr;
        const float* const* inputs =
            bus == 0 ? self->m_blockInputs
                     : (bus == 1 ? self->m_blockSidechainInputs : nullptr);
        const std::uint16_t inputChannels =
            bus == 0 ? self->m_blockInputChannels
                     : (bus == 1 ? self->m_blockSidechainInputChannels : 0);
        if (inputs && channel < inputChannels) {
            source = inputs[channel];
            if (channel >= 64 ||
                (silenceMask & (std::uint64_t{1} << channel)) == 0) {
                allSilent = false;
            }
        } else {
            // A channel the caller did not supply reads as silence, never as
            // whatever was in the buffer last block.
            source = self->m_silence.data() + std::size_t(channel) * self->m_maxBlockSize;
        }
        const UInt32 count = std::min<UInt32>(frames, self->m_blockFrames);
        if (buffer.mData) {
            // The unit gave us a buffer to fill; anything else is its memory.
            std::memcpy(buffer.mData, source, std::size_t(count) * sizeof(float));
            buffer.mDataByteSize = count * sizeof(float);
        } else {
            // The unit is happy to read ours, which saves the copy.
            buffer.mData = const_cast<float*>(source);
            buffer.mDataByteSize = count * sizeof(float);
        }
        buffer.mNumberChannels = 1;   // non-interleaved: one channel per buffer
    }
    if (flags) {
        if (allSilent) {
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
        } else {
            *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
        }
    }
    return noErr;
}

// ── Musical time ────────────────────────────────────────────────────────────

OSStatus AuInstance::beatAndTempo(void* context, Float64* outBeat, Float64* outTempo) {
    auto* self = static_cast<AuInstance*>(context);
    if (!self) return kAudioUnitErr_Uninitialized;
    // AU counts beats in quarter notes, which is what `ppqPosition` already is.
    if (outBeat) *outBeat = self->m_blockTransport.ppqPosition;
    if (outTempo) *outTempo = self->m_blockTransport.tempo;
    return noErr;
}

OSStatus AuInstance::musicalTimeLocation(void* context,
                                         UInt32* outDeltaSampleOffsetToNextBeat,
                                         Float32* outTimeSigNumerator,
                                         UInt32* outTimeSigDenominator,
                                         Float64* outCurrentMeasureDownBeat) {
    auto* self = static_cast<AuInstance*>(context);
    if (!self) return kAudioUnitErr_Uninitialized;
    const engine::TransportInfo& transport = self->m_blockTransport;
    if (outTimeSigNumerator) *outTimeSigNumerator = Float32(transport.timeSigNumerator);
    if (outTimeSigDenominator) *outTimeSigDenominator = UInt32(transport.timeSigDenominator);
    // The bar this block is in, in quarter notes — an arpeggiator lines up on
    // this, not on the beat count.
    if (outCurrentMeasureDownBeat) *outCurrentMeasureDownBeat = transport.barStartPpq;
    if (outDeltaSampleOffsetToNextBeat) {
        *outDeltaSampleOffsetToNextBeat = UInt32(au::samplesToNextBeat(
            transport.ppqPosition,
            transport.tempo > 0.0 ? transport.tempo : 120.0, self->m_sampleRate));
    }
    return noErr;
}

OSStatus AuInstance::transportState(void* context, Boolean* outIsPlaying,
                                    Boolean* outTransportStateChanged,
                                    Float64* outCurrentSampleInTimeLine,
                                    Boolean* outIsCycling, Float64* outCycleStartBeat,
                                    Float64* outCycleEndBeat) {
    auto* self = static_cast<AuInstance*>(context);
    if (!self) return kAudioUnitErr_Uninitialized;
    if (outIsPlaying) *outIsPlaying = self->m_blockPlaying ? 1 : 0;
    // "Changed" means since the unit last asked. Start and stop are what a
    // plugin resets its own sequencer on, so a stale answer here is heard.
    if (outTransportStateChanged)
        *outTransportStateChanged = self->m_blockPlaying != self->m_wasPlaying ? 1 : 0;
    if (outCurrentSampleInTimeLine)
        *outCurrentSampleInTimeLine = Float64(self->m_blockSampleTime);
    if (outIsCycling) *outIsCycling = self->m_blockTransport.looping ? 1 : 0;
    if (outCycleStartBeat) *outCycleStartBeat = self->m_blockTransport.loopStartPpq;
    if (outCycleEndBeat) *outCycleEndBeat = self->m_blockTransport.loopEndPpq;
    return noErr;
}

void AuInstance::installHostCallbacks() {
    if (!m_unit) return;
    HostCallbackInfo callbacks{};
    callbacks.hostUserData = this;
    callbacks.beatAndTempoProc = &AuInstance::beatAndTempo;
    callbacks.musicalTimeLocationProc = &AuInstance::musicalTimeLocation;
    callbacks.transportStateProc = &AuInstance::transportState;
    // Failure is not fatal: an effect that never asks about musical time does
    // not care whether the property took.
    AudioUnitSetProperty(m_unit, kAudioUnitProperty_HostCallbacks,
                         kAudioUnitScope_Global, 0, &callbacks, sizeof(callbacks));
}

PluginProcessDisposition AuInstance::process(
    const PluginProcessContext& context) noexcept {
    auto silence = [&] {
        for (std::uint16_t channel = 0; channel < context.outputChannels; ++channel) {
            std::fill_n(context.outputs[channel], context.frames, 0.0f);
        }
    };
    if (!m_processing || !m_unit) {
        // Not writing is not the same as writing nothing: the caller's buffer
        // is recycled and still holds the previous block.
        silence();
        return PluginProcessDisposition::Continue;
    }
    if (context.frames > m_maxBlockSize || m_outputChannels == 0) {
        silence();
        return PluginProcessDisposition::Continue;
    }

    // Parameters and notes both go in before the render, each with its frame
    // offset. AU takes plain parameter values and does the ramping itself, and
    // it takes MIDI as raw status bytes — there is no event list to build.
    for (const PluginEvent& event : context.inputEvents) {
        switch (event.kind) {
            case PluginEvent::Kind::ParamValue:
                if (event.paramIndex >= m_parameterIds.size()) break;
                AudioUnitSetParameter(m_unit, m_parameterIds[event.paramIndex],
                                      kAudioUnitScope_Global, 0,
                                      AudioUnitParameterValue(event.value),
                                      UInt32(event.frameOffset));
                break;
            case PluginEvent::Kind::NoteOn:
            case PluginEvent::Kind::NoteOff: {
                if (!m_acceptsMidi) break;
                const bool on = event.kind == PluginEvent::Kind::NoteOn;
                const UInt32 status =
                    UInt32((on ? 0x90u : 0x80u) | (unsigned(event.channel) & 0x0Fu));
                const UInt32 velocity =
                    on ? UInt32(std::clamp(event.value, 0.0, 1.0) * 127.0 + 0.5) : 0u;
                MusicDeviceMIDIEvent(m_unit, status, UInt32(event.key), velocity,
                                     UInt32(event.frameOffset));
                break;
            }
            default:
                break;
        }
    }

    // Park the transport where the three host callbacks will look for it. A
    // tempo-synced unit asks for these from inside AudioUnitRender, below.
    m_blockTransport = context.transport;
    m_blockSampleTime = context.sampleTime;
    m_blockPlaying = context.playing;

    // Park the input where the render callback will look for it. Valid only
    // for the duration of the AudioUnitRender below.
    m_blockInputs = context.inputs;
    m_blockInputChannels = context.inputChannels;
    m_blockInputSilenceMask = context.inputSilenceMask;
    m_blockSidechainInputs = context.sidechainInputs;
    m_blockSidechainInputChannels = context.sidechainInputChannels;
    m_blockSidechainSilenceMask = context.sidechainSilenceMask;
    m_blockFrames = context.frames;

    auto* list = reinterpret_cast<AudioBufferList*>(m_outputListStorage.data());
    list->mNumberBuffers = m_outputChannels;
    for (std::uint32_t channel = 0; channel < m_outputChannels; ++channel) {
        float* destination =
            channel < context.outputChannels
                ? context.outputs[channel]
                : m_scratch.data() + std::size_t(channel) * m_maxBlockSize;
        list->mBuffers[channel].mNumberChannels = 1;
        list->mBuffers[channel].mDataByteSize = context.frames * sizeof(float);
        list->mBuffers[channel].mData = destination;
    }

    AudioTimeStamp timeStamp{};
    // The render clock, not the timeline: see `m_renderTime`. It has to advance
    // for every call, including the calls a stopped transport makes.
    timeStamp.mSampleTime = Float64(m_renderTime);
    timeStamp.mFlags = kAudioTimeStampSampleTimeValid;

    AudioUnitRenderActionFlags flags =
        context.offline ? kAudioOfflineUnitRenderAction_Render : 0;
    if (AudioUnitRender(m_unit, &flags, &timeStamp, 0, context.frames, list) != noErr) {
        silence();
    } else if ((flags & kAudioUnitRenderAction_OutputIsSilence) != 0) {
        // The unit is telling us it produced silence, and Core Audio is explicit
        // that the buffer contents are then undefined — a unit is free to skip
        // writing entirely. Believing what is in it replays the last block for
        // as long as the unit stays quiet, which is what a stopped transport
        // makes it do.
        silence();
    }

    m_renderTime += std::int64_t(context.frames);

    m_blockInputs = nullptr;
    m_blockInputChannels = 0;
    m_blockInputSilenceMask = 0;
    m_blockSidechainInputs = nullptr;
    m_blockSidechainInputChannels = 0;
    m_blockSidechainSilenceMask = 0;
    m_blockFrames = 0;
    // Only now:  reports "changed" by comparing the two, and
    // the unit asks for it during the render above.
    m_wasPlaying = m_blockPlaying;
    // OutputIsSilence describes only this render call. Audio Unit exposes no
    // plugin-to-host wake request that would make skipping future calls safe.
    return PluginProcessDisposition::Continue;
}

} // namespace daw::plugins
