#pragma once

#include <string>
#include <system_error>

namespace audio {

enum class EngineError {
    None = 0,
    Unknown,
    InvalidArgument,
    FileNotFound,
    UnsupportedFormat,
    DeviceError,
    DeviceNotFound,
    DeviceBusy,
    SampleRateMismatch,
    BufferSizeMismatch,
    EngineNotInitialized,
    EngineAlreadyInitialized,
    EngineRunning,
    TrackNotFound,
    ClipNotFound,
    BusNotFound,
    PluginNotFound,
    PluginLoadFailed,
    OutOfMemory,
    NotSupported,
    NotYetImplemented,
    AudioThreadError,
    Overload,
    Underrun,
    Timeout,
    NotInitialized,
    AlreadyRecording,
    NotRecording,
    AlreadyRendering,
    FileWriteError
};

class Result {
public:
    static Result ok() { return Result(); }
    static Result fail(EngineError err, std::string msg = "") {
        return Result(err, std::move(msg));
    }

    bool isOk() const { return m_error == EngineError::None; }
    bool isError() const { return m_error != EngineError::None; }
    EngineError error() const { return m_error; }
    const std::string& message() const { return m_message; }
    explicit operator bool() const { return isOk(); }

private:
    Result() : m_error(EngineError::None) {}
    Result(EngineError err, std::string msg)
        : m_error(err), m_message(std::move(msg)) {}

    EngineError m_error;
    std::string m_message;
};

inline std::string toString(EngineError err) {
    switch (err) {
        case EngineError::None: return "No error";
        case EngineError::Unknown: return "Unknown error";
        case EngineError::InvalidArgument: return "Invalid argument";
        case EngineError::FileNotFound: return "File not found";
        case EngineError::UnsupportedFormat: return "Unsupported format";
        case EngineError::DeviceError: return "Device error";
        case EngineError::DeviceNotFound: return "Device not found";
        case EngineError::DeviceBusy: return "Device busy";
        case EngineError::SampleRateMismatch: return "Sample rate mismatch";
        case EngineError::BufferSizeMismatch: return "Buffer size mismatch";
        case EngineError::EngineNotInitialized: return "Engine not initialized";
        case EngineError::EngineAlreadyInitialized: return "Already initialized";
        case EngineError::EngineRunning: return "Engine is running";
        case EngineError::TrackNotFound: return "Track not found";
        case EngineError::ClipNotFound: return "Clip not found";
        case EngineError::BusNotFound: return "Bus not found";
        case EngineError::PluginNotFound: return "Plugin not found";
        case EngineError::PluginLoadFailed: return "Plugin load failed";
        case EngineError::OutOfMemory: return "Out of memory";
        case EngineError::NotSupported: return "Not supported";
        case EngineError::NotYetImplemented: return "Not yet implemented";
        case EngineError::AudioThreadError: return "Audio thread error";
        case EngineError::Overload: return "Overload";
        case EngineError::Underrun: return "Underrun";
        case EngineError::Timeout: return "Timeout";
        case EngineError::NotInitialized: return "Not initialized";
        case EngineError::AlreadyRecording: return "Already recording";
        case EngineError::NotRecording: return "Not recording";
        case EngineError::AlreadyRendering: return "Already rendering";
        case EngineError::FileWriteError: return "File write error";
    }
    return "Unknown";
}

} // namespace audio
