#include "NativeDeviceIds.hpp"
#include <portaudio.h>
#include <algorithm>
#include <cstddef>
#if defined(__APPLE__) && __has_include(<pa_mac_core.h>)
#include <pa_mac_core.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#if __has_include(<pa_win_wasapi.h>)
#include <pa_win_wasapi.h>
#define DAW_NATIVE_WASAPI 1
#endif
#endif

namespace audio {
namespace {
#if defined(__APPLE__) && __has_include(<pa_mac_core.h>)
std::string propertyString(AudioDeviceID device, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr || !value) return {};
    std::string result(CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1, '\0');
    if (CFStringGetCString(value, result.data(), result.size(), kCFStringEncodingUTF8)) result.resize(std::char_traits<char>::length(result.c_str()));
    else result.clear();
    CFRelease(value);
    return result;
}
int channels(AudioDeviceID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr) return -1;
    std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    if (!size || AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, storage.data()) != noErr) return -1;
    auto* buffers = reinterpret_cast<AudioBufferList*>(storage.data());
    int count = 0;
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) count += buffers->mBuffers[i].mNumberChannels;
    return count;
}
#elif defined(_WIN32)
std::string utf8(const wchar_t* value) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
    result.pop_back(); return result;
}
#endif
}

std::vector<std::string> nativeDeviceIds() {
    const int count = std::max(0, Pa_GetDeviceCount());
    std::vector<std::string> ids(count);
#if defined(__APPLE__) && __has_include(<pa_mac_core.h>)
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) return ids;
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr) return ids;
    struct Native { std::string name, uid; int input, output; };
    std::vector<Native> native;
    for (auto device : devices) {
        Native item{propertyString(device, kAudioDevicePropertyDeviceNameCFString),
                    propertyString(device, kAudioDevicePropertyDeviceUID),
                    channels(device, kAudioObjectPropertyScopeInput), channels(device, kAudioObjectPropertyScopeOutput)};
        if (!item.name.empty() && item.input >= 0 && item.output >= 0) native.push_back(std::move(item));
    }
    // PortAudio's CoreAudio backend enumerates this ordered hardware list and
    // omits unreadable devices. Validate the whole sequence before associating
    // any index; never guess using the first matching display name.
    std::vector<int> indices;
    for (int i = 0; i < count; ++i) {
        const auto* d = Pa_GetDeviceInfo(i);
        const auto* api = d ? Pa_GetHostApiInfo(d->hostApi) : nullptr;
        if (api && api->type == paCoreAudio) indices.push_back(i);
    }
    if (indices.size() != native.size()) return ids;
    for (size_t n = 0; n < indices.size(); ++n) {
        const auto* d = Pa_GetDeviceInfo(indices[n]);
        if (!d->name || native[n].name != d->name || native[n].input != d->maxInputChannels || native[n].output != d->maxOutputChannels) return ids;
    }
    for (size_t n = 0; n < indices.size(); ++n)
        if (!native[n].uid.empty()) ids[indices[n]] = "coreaudio:" + native[n].uid;
#elif defined(_WIN32)
    for (int i = 0; i < count; ++i) {
        const auto* d = Pa_GetDeviceInfo(i);
        const auto* api = d ? Pa_GetHostApiInfo(d->hostApi) : nullptr;
        if (!api) continue;
#if DAW_NATIVE_WASAPI
        if (api->type == paWASAPI) {
            void* object = nullptr;
            // Borrowed IMMDevice; PortAudio retains ownership (no Release).
            if (PaWasapi_GetIMMDevice(i, &object) == paNoError && object) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(static_cast<IMMDevice*>(object)->GetId(&id)) && id) {
                    ids[i] = "wasapi:" + utf8(id);
                    CoTaskMemFree(id);
                }
            }
        }
#endif
        if (api->type == paASIO && d->name) {
            const int length = MultiByteToWideChar(CP_UTF8, 0, d->name, -1, nullptr, 0);
            if (length <= 1) continue;
            std::wstring name(length, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, d->name, -1, name.data(), length);
            name.pop_back();
            const std::wstring key = L"SOFTWARE\\ASIO\\" + name;
            wchar_t clsid[128]{}; DWORD bytes = sizeof(clsid);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &bytes) == ERROR_SUCCESS)
                ids[i] = "asio:" + utf8(clsid);
        }
    }
#endif
    return ids;
}

bool nativeDeviceAlive(int index, void* stream, bool input) {
    const auto* d = Pa_GetDeviceInfo(index);
    const auto* api = d ? Pa_GetHostApiInfo(d->hostApi) : nullptr;
    if (!api || !stream) return false;
#if defined(__APPLE__) && __has_include(<pa_mac_core.h>)
    if (api->type == paCoreAudio) {
        const auto id = input ? PaMacCore_GetStreamInputDevice(static_cast<PaStream*>(stream))
                              : PaMacCore_GetStreamOutputDevice(static_cast<PaStream*>(stream));
        AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 alive = 0, size = sizeof(alive);
        return AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &alive) == noErr && alive;
    }
#elif defined(_WIN32) && DAW_NATIVE_WASAPI
    if (api->type == paWASAPI) {
        void* object = nullptr; DWORD state = 0;
        return PaWasapi_GetIMMDevice(index, &object) == paNoError && object &&
            SUCCEEDED(static_cast<IMMDevice*>(object)->GetState(&state)) && (state & DEVICE_STATE_ACTIVE);
    }
#endif
    return true;
}

std::string nativeStreamDeviceId(void* stream, bool input) {
#if defined(__APPLE__) && __has_include(<pa_mac_core.h>)
    if (!stream) return {};
    const auto device = input ? PaMacCore_GetStreamInputDevice(static_cast<PaStream*>(stream))
                              : PaMacCore_GetStreamOutputDevice(static_cast<PaStream*>(stream));
    const auto uid = propertyString(device, kAudioDevicePropertyDeviceUID);
    if (!uid.empty()) return "coreaudio:" + uid;
#endif
    return {};
}
}
