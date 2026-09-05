#pragma once

#include "Core/Types.hpp"
#include "Core/Result.hpp"
#include <span>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

// Portable audio-file decoding. Replaces the macOS ExtAudioFile /
// CoreFoundation path. Backed by libsndfile, which decodes WAV/AIFF/FLAC/
// Ogg-Vorbis/Opus and — when built with mpg123 — MP3.
//
// Not supported: MP4/M4A/AAC (libsndfile has no AAC decoder). Importing those
// fails with a clear error rather than silently producing nothing.
namespace audio::platform {

struct DecodedAudio {
    std::vector<float> interleaved; // frame-major: sample[frame*channels + ch]
    ChannelCount channels = 0;
    SampleRate sampleRate = 0.0;
    FrameCount frames = 0;
};

/// Decode an entire file into interleaved float samples. On failure the Result
/// carries a human-readable message from the decoder.
struct DecodeOptions {
    std::size_t maxBytes = 256u * 1024u * 1024u;
    std::function<bool()> keepGoing;
};
Result decodeAudioFile(const std::string& path, DecodedAudio& out,
                       const DecodeOptions& options = {});

/// True if the (lower-case, no dot) extension is one we can decode.
bool isDecodableExtension(const std::string& extLower);

/// Every extension `isDecodableExtension` accepts, lower-case and without the
/// dot. The one place that knows what this application can open — file dialogs,
/// drop targets and the browser all build their filters from it instead of
/// keeping private lists that drift apart.
std::span<const std::string_view> decodableExtensions();

/// What a file holds, without decoding a single sample.
struct AudioFileInfo {
    FrameCount frames = 0;
    SampleRate sampleRate = 0.0;
    ChannelCount channels = 0;

    double durationSeconds() const {
        return sampleRate > 0.0 ? double(frames) / sampleRate : 0.0;
    }
};

/// Read a file's frame count, rate and channel count. The same `sf_open` the
/// decoder does, with no read: a browser listing a folder must be able to say
/// "3:24 · 48 kHz · stereo" without pulling every file into memory.
Result probeAudioFile(const std::string& path, AudioFileInfo& out);

/// Incremental decoder for analysis and other jobs that should not retain a
/// whole song in memory. The reader always returns interleaved float frames,
/// exactly like `decodeAudioFile`, and supports sample-accurate seeks into a
/// trimmed clip source.
class AudioFileReader {
public:
    AudioFileReader();
    ~AudioFileReader();
    AudioFileReader(AudioFileReader&&) noexcept;
    AudioFileReader& operator=(AudioFileReader&&) noexcept;
    AudioFileReader(const AudioFileReader&) = delete;
    AudioFileReader& operator=(const AudioFileReader&) = delete;

    Result open(const std::string& path);
    void close();
    bool isOpen() const noexcept;
    const AudioFileInfo& info() const noexcept;
    /// Seek to an absolute source frame. Values beyond EOF clamp to EOF.
    Result seek(FrameCount frame);
    /// Decode at most `frames` into `destination`. The span must hold
    /// `frames * info().channels` samples. Returns the number of frames read.
    FrameCount read(float* destination, FrameCount frames);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ── Encoding ───────────────────────────────────────────────────────────────
//
// What a render can be written as. Deliberately a small curated set rather than
// libsndfile's full matrix: every combination offered here is one a musician
// would actually pick, and `isWriteSpecSupported` asks libsndfile whether this
// particular build can really produce it. That check is the whole point — the
// Homebrew build links FLAC, Vorbis, Opus and LAME, a bare vcpkg one may not,
// and the UI must grey out what is missing instead of failing at `sf_open`.

enum class Container { Wav, Aiff, Flac, OggVorbis, Opus, Mp3, Caf, W64 };

enum class Encoding { Int16, Int24, Int32, Float32, Float64, Vorbis, Opus, Mp3 };

struct WriteSpec {
    Container container = Container::Wav;
    Encoding encoding = Encoding::Float32;
    /// Vorbis and Opus: VBR quality, 0 (smallest) … 1 (best).
    double vbrQuality = 0.6;
    /// MP3 only. Zero means VBR steered by `vbrQuality`; otherwise a constant
    /// bit rate in kbit/s (128, 192, 256, 320).
    int bitrateKbps = 0;
    /// TPDF dither on the way down to a fixed-point word. Truncating to 16 bits
    /// without it turns a quiet fade into correlated distortion instead of
    /// noise; at 24 bits the noise floor is already below anything audible, so
    /// it is off by default there. Ignored by float and lossy encodings, which
    /// have nothing to truncate.
    bool dither = false;
};

/// What gets written into the file's own header. Empty fields are skipped, and
/// containers that cannot carry a given field simply ignore it.
struct FileTags {
    std::string title;
    std::string artist;
    std::string comment;
    std::string software;
    /// Where this file starts on the project timeline, in samples. Written as
    /// the Broadcast Wave time reference, which is how a post-production tool
    /// drops a stem back at the right place instead of at zero.
    std::uint64_t timeReferenceSamples = 0;
};

/// The file extension, without the dot, a container is written with.
std::string_view extensionFor(Container container);
/// The encodings that make sense inside a container, best first. The UI builds
/// its depth/quality menu from this and then filters it through
/// `isWriteSpecSupported`.
std::span<const Encoding> encodingsFor(Container container);
/// A short label for a menu entry — "24-bit", "32-bit float", "VBR".
std::string_view describeEncoding(Encoding encoding);
/// Can *this* build of libsndfile actually write that? Answers via
/// `sf_format_check`, so it also catches the rate limits a codec imposes —
/// Opus, for one, accepts only 8, 12, 16, 24 and 48 kHz.
bool isWriteSpecSupported(const WriteSpec& spec, ChannelCount channels,
                          SampleRate sampleRate);

/// Incremental float writer used by long exports. It owns only one interleave
/// block; libsndfile selects RF64 automatically when the expected WAV payload
/// would exceed the classic RIFF 32-bit size.
class AudioFileWriter {
public:
    AudioFileWriter();
    ~AudioFileWriter();
    AudioFileWriter(AudioFileWriter&&) noexcept;
    AudioFileWriter& operator=(AudioFileWriter&&) noexcept;
    AudioFileWriter(const AudioFileWriter&) = delete;
    AudioFileWriter& operator=(const AudioFileWriter&) = delete;

    /// Float32 WAV — what recording and the comp flatten write. Kept as its own
    /// entry point so those paths never have to name a format.
    Result open(const std::string& path, SampleRate sampleRate,
                ChannelCount channels, std::uint64_t expectedFrames = 0);
    Result open(const std::string& path, const WriteSpec& spec,
                SampleRate sampleRate, ChannelCount channels,
                std::uint64_t expectedFrames = 0);
    /// Write the header strings and the Broadcast Wave time reference. Call
    /// after `open` and before the first `write`: libsndfile can only add
    /// strings while the header is still being assembled.
    Result setTags(const FileTags& tags);
    Result write(const float* const* channels, FrameCount frames,
                 float gain = 1.0f);
    Result close();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace audio::platform
