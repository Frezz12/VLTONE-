#include "platform/NativeAudioFileReader.hpp"
#include "platform/PathUtils.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#endif

namespace audio::platform {
namespace {
Result failure(const char* operation, long long code) {
    return Result::fail(EngineError::UnsupportedFormat,
        std::string(operation) + " (system audio decoder error " + std::to_string(code) + ")");
}
}
struct NativeAudioFileReader::Impl {
    AudioFileInfo info;
    Result status = Result::ok();
#if defined(__APPLE__)
    ExtAudioFileRef file = nullptr;
    ~Impl() { if (file) ExtAudioFileDispose(file); }
#elif defined(_WIN32)
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    std::vector<float> pending;
    std::size_t offset = 0;
    bool eof = false;
    FrameCount seekTarget = 0;
    bool seeking = false;
#endif
};
NativeAudioFileReader::NativeAudioFileReader() : m_impl(std::make_unique<Impl>()) {}
NativeAudioFileReader::~NativeAudioFileReader() = default;
const AudioFileInfo& NativeAudioFileReader::info() const { return m_impl->info; }
Result NativeAudioFileReader::readStatus() const { return m_impl->status; }

Result NativeAudioFileReader::open(const std::string& path) {
    m_impl = std::make_unique<Impl>();
    std::error_code error;
    const auto filePath = daw::platform::pathFromUtf8(path);
    if (!std::filesystem::is_regular_file(filePath, error))
        return Result::fail(EngineError::UnsupportedFormat, "audio file is missing or unreadable");
#if defined(__APPLE__)
    const auto absolute = daw::platform::pathToUtf8(std::filesystem::absolute(filePath));
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(absolute.data()), absolute.size(), false);
    if (!url) return failure("Cannot open audio path", -1);
    OSStatus status = ExtAudioFileOpenURL(url, &m_impl->file);
    CFRelease(url);
    if (status != noErr) return failure("Cannot open MP4/AAC audio", status);
    AudioStreamBasicDescription source{};
    UInt32 size = sizeof(source);
    status = ExtAudioFileGetProperty(m_impl->file, kExtAudioFileProperty_FileDataFormat, &size, &source);
    if (status != noErr) return failure("Cannot read audio format", status);
    SInt64 frames = 0;
    size = sizeof(frames);
    status = ExtAudioFileGetProperty(m_impl->file, kExtAudioFileProperty_FileLengthFrames, &size, &frames);
    if (status != noErr) return failure("Cannot read audio duration", status);
    if (frames <= 0 || source.mChannelsPerFrame == 0 || source.mChannelsPerFrame > 32 ||
        !std::isfinite(source.mSampleRate) || source.mSampleRate <= 0)
        return failure("Invalid audio dimensions", -1);
    AudioStreamBasicDescription client{};
    client.mSampleRate = source.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    client.mChannelsPerFrame = source.mChannelsPerFrame;
    client.mBitsPerChannel = 32;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = client.mBytesPerPacket = sizeof(float) * client.mChannelsPerFrame;
    status = ExtAudioFileSetProperty(m_impl->file, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client);
    if (status != noErr) return failure("Cannot configure audio decoder", status);
    m_impl->info = {FrameCount(frames), source.mSampleRate, ChannelCount(source.mChannelsPerFrame), true};
    return Result::ok();
#elif defined(_WIN32)
    // COM belongs to the calling worker, while Media Foundation belongs to the
    // process. Avoid tearing either down while another audio file is open.
    struct ComThread {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComThread() { if (SUCCEEDED(result)) CoUninitialize(); }
    };
    thread_local ComThread com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        return failure("Cannot initialize audio decoder", com.result);
    struct MediaFoundation {
        HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        ~MediaFoundation() { if (SUCCEEDED(result)) MFShutdown(); }
    };
    static MediaFoundation mediaFoundation;
    if (FAILED(mediaFoundation.result)) return failure("Cannot initialize Media Foundation", mediaFoundation.result);
    HRESULT hr = MFCreateSourceReaderFromURL(std::filesystem::absolute(filePath).c_str(), nullptr, &m_impl->reader);
    if (FAILED(hr)) return failure("Cannot open MP4/AAC audio", hr);
    hr = m_impl->reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = m_impl->reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (SUCCEEDED(hr)) hr = m_impl->reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get());
    if (FAILED(hr)) return failure("Cannot configure audio decoder", hr);
    type.Reset();
    hr = m_impl->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type);
    UINT32 channels = 0, rate = 0;
    if (SUCCEEDED(hr)) hr = type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (SUCCEEDED(hr)) hr = type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (FAILED(hr) || channels == 0 || channels > 32 || rate == 0)
        return failure("Cannot read audio format", FAILED(hr) ? hr : E_INVALIDARG);
    PROPVARIANT duration;
    PropVariantInit(&duration);
    hr = m_impl->reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration);
    const long double frames = SUCCEEDED(hr) && duration.vt == VT_UI8
        ? std::ceil(static_cast<long double>(duration.uhVal.QuadPart) * rate / 10000000.0L) : 0;
    PropVariantClear(&duration);
    if (frames <= 0 || frames > std::numeric_limits<FrameCount>::max())
        return failure("Cannot read audio duration", E_INVALIDARG);
    m_impl->info = {FrameCount(frames), SampleRate(rate), ChannelCount(channels), true};
    return Result::ok();
#else
    return failure("MP4/AAC audio is not supported on this platform", -1);
#endif
}

Result NativeAudioFileReader::seek(FrameCount frame) {
    frame = std::min(frame, m_impl->info.frames);
#if defined(__APPLE__)
    const auto status = ExtAudioFileSeek(m_impl->file, SInt64(frame));
    m_impl->status = status == noErr ? Result::ok() : failure("Cannot seek audio", status);
#elif defined(_WIN32)
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = LONGLONG(static_cast<long double>(frame) * 10000000.0L / m_impl->info.sampleRate);
    const HRESULT hr = m_impl->reader->SetCurrentPosition(GUID_NULL, position);
    m_impl->status = SUCCEEDED(hr) ? Result::ok() : failure("Cannot seek audio", hr);
    if (SUCCEEDED(hr)) {
        m_impl->pending.clear(); m_impl->offset = 0; m_impl->eof = false;
        m_impl->seekTarget = frame; m_impl->seeking = true;
    }
#else
    m_impl->status = failure("Cannot seek audio", -1);
#endif
    return m_impl->status;
}

FrameCount NativeAudioFileReader::read(float* destination, FrameCount frames) {
    if (!m_impl->status || !destination || frames == 0) return 0;
#if defined(__APPLE__)
    const UInt32 stride = sizeof(float) * m_impl->info.channels;
    UInt32 count = UInt32(std::min<FrameCount>(frames, std::numeric_limits<UInt32>::max() / stride));
    AudioBufferList buffers{};
    buffers.mNumberBuffers = 1;
    buffers.mBuffers[0] = {UInt32(m_impl->info.channels), count * stride, destination};
    const auto status = ExtAudioFileRead(m_impl->file, &count, &buffers);
    if (status != noErr) { m_impl->status = failure("Cannot decode audio", status); return 0; }
    return count;
#elif defined(_WIN32)
    FrameCount written = 0;
    const auto channels = m_impl->info.channels;
    while (written < frames) {
        if (m_impl->offset < m_impl->pending.size()) {
            const auto count = std::min<FrameCount>(frames - written,
                (m_impl->pending.size() - m_impl->offset) / channels);
            std::memcpy(destination + written * channels, m_impl->pending.data() + m_impl->offset,
                        std::size_t(count) * channels * sizeof(float));
            m_impl->offset += count * channels; written += count;
            continue;
        }
        if (m_impl->eof) break;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        HRESULT hr = m_impl->reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))) {
            m_impl->status = failure("Cannot decode audio", FAILED(hr) ? hr : E_FAIL); break;
        }
        m_impl->eof = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (!sample) continue;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data = nullptr;
        DWORD size = 0;
        if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, &size);
        if (FAILED(hr)) { m_impl->status = failure("Cannot read decoded audio", hr); break; }
        if (size % (channels * sizeof(float)) != 0) {
            buffer->Unlock(); m_impl->status = failure("Invalid decoded audio block", E_FAIL); break;
        }
        // Allocate before locking would require another size query. Ensure an
        // allocation failure still releases the decoder's locked buffer.
        try {
            m_impl->pending.resize(size / sizeof(float));
            std::memcpy(m_impl->pending.data(), data, size);
        } catch (...) { buffer->Unlock(); throw; }
        buffer->Unlock();
        m_impl->offset = 0;
        if (m_impl->seeking) {
            const auto start = std::llround(static_cast<long double>(timestamp) * m_impl->info.sampleRate / 10000000.0L);
            const auto skip = std::max<long long>(0, static_cast<long long>(m_impl->seekTarget) - start);
            m_impl->offset = std::min<std::size_t>(skip, m_impl->pending.size() / channels) * channels;
            if (m_impl->offset < m_impl->pending.size()) m_impl->seeking = false;
        }
    }
    return written;
#else
    return 0;
#endif
}
}
