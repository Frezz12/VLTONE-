#pragma once
#include "platform/AudioFileDecoder.hpp"

namespace audio::platform {
// OS codecs for MP4 audio/AAC. Kept behind the common streaming reader so
// imports, previews, samplers, analysis and project reloads share one path.
class NativeAudioFileReader {
public:
    NativeAudioFileReader();
    ~NativeAudioFileReader();
    Result open(const std::string& path);
    const AudioFileInfo& info() const;
    Result seek(FrameCount frame);
    FrameCount read(float* destination, FrameCount frames);
    Result readStatus() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
