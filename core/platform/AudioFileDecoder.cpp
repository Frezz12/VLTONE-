#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace audio::platform {

namespace {
// libsndfile's own container list, minus the ones nothing produces. `caf` and
// `w64` are here because the sampler already offered them and libsndfile does
// read both — platform_test writes and decodes one of each rather than taking
// that on trust.
constexpr std::array<std::string_view, 11> kSupported = {
    "wav", "aiff", "aif", "aifc", "flac", "ogg",
    "oga", "opus", "mp3",  "caf",  "w64"};

SNDFILE* openSoundFile(const std::string& path, int mode, SF_INFO* info) {
#if defined(_WIN32)
    const std::filesystem::path native = daw::platform::pathFromUtf8(path);
    return sf_wchar_open(native.c_str(), mode, info);
#else
    return sf_open(path.c_str(), mode, info);
#endif
}
} // namespace

std::span<const std::string_view> decodableExtensions() { return kSupported; }

bool isDecodableExtension(const std::string& extLower) {
    for (std::string_view e : kSupported) {
        if (extLower == e) return true;
    }
    return false;
}

Result probeAudioFile(const std::string& path, AudioFileInfo& out) {
    SF_INFO info{};
    SNDFILE* file = openSoundFile(path, SFM_READ, &info);
    if (!file) {
        return Result::fail(EngineError::UnsupportedFormat,
                            std::string("sf_open failed: ") +
                                sf_strerror(nullptr));
    }
    sf_close(file);

    if (info.channels <= 0 || info.frames <= 0) {
        return Result::fail(EngineError::UnsupportedFormat,
                            "file has no decodable audio");
    }
    out.frames = static_cast<FrameCount>(info.frames);
    out.sampleRate = static_cast<SampleRate>(info.samplerate);
    out.channels = static_cast<ChannelCount>(info.channels);
    return Result::ok();
}

Result decodeAudioFile(const std::string& path, DecodedAudio& out,
                       const DecodeOptions& options) {
    try {
        if (options.keepGoing && !options.keepGoing())
            return Result::fail(EngineError::InvalidArgument, "cancelled");
        AudioFileReader reader;
        if (const auto opened = reader.open(path); !opened) return opened;
        const auto info = reader.info();
        if (info.channels > 32 || info.frames >
            options.maxBytes / sizeof(float) / info.channels)
            return Result::fail(EngineError::UnsupportedFormat,
                                "audio exceeds the in-memory decode budget; use the streaming reader");
        DecodedAudio decoded;
        decoded.channels = info.channels;
        decoded.sampleRate = info.sampleRate;
        decoded.interleaved.resize(std::size_t(info.frames) * info.channels);
        while (decoded.frames < info.frames) {
            if (options.keepGoing && !options.keepGoing())
                return Result::fail(EngineError::InvalidArgument, "cancelled");
            const auto count = std::min<FrameCount>(8192, info.frames - decoded.frames);
            const auto read = reader.read(decoded.interleaved.data() +
                std::size_t(decoded.frames) * info.channels, count);
            if (read == 0)
                return Result::fail(EngineError::UnsupportedFormat, "truncated audio file");
            decoded.frames += read;
        }
        out = std::move(decoded);
        return Result::ok();
    } catch (const std::exception& error) {
        return Result::fail(EngineError::UnsupportedFormat, error.what());
    }
}

struct AudioFileReader::Impl {
    SNDFILE* file = nullptr;
    AudioFileInfo info;
};

AudioFileReader::AudioFileReader() : m_impl(std::make_unique<Impl>()) {}
AudioFileReader::~AudioFileReader() { close(); }
AudioFileReader::AudioFileReader(AudioFileReader&&) noexcept = default;
AudioFileReader& AudioFileReader::operator=(AudioFileReader&& other) noexcept {
    if (this != &other) { close(); m_impl = std::move(other.m_impl); }
    return *this;
}

Result AudioFileReader::open(const std::string& path) {
    close();
    if (!m_impl) m_impl = std::make_unique<Impl>();
    SF_INFO opened{};
    m_impl->file = openSoundFile(path, SFM_READ, &opened);
    if (!m_impl->file) {
        return Result::fail(EngineError::UnsupportedFormat,
                            std::string("sf_open failed: ") +
                                sf_strerror(nullptr));
    }
    if (opened.channels <= 0 || opened.frames <= 0 || opened.samplerate <= 0) {
        close();
        return Result::fail(EngineError::UnsupportedFormat,
                            "file has no decodable audio");
    }
    m_impl->info.frames = static_cast<FrameCount>(opened.frames);
    m_impl->info.sampleRate = static_cast<SampleRate>(opened.samplerate);
    m_impl->info.channels = static_cast<ChannelCount>(opened.channels);
    return Result::ok();
}

void AudioFileReader::close() {
    if (!m_impl || !m_impl->file) return;
    sf_close(m_impl->file);
    m_impl->file = nullptr;
    m_impl->info = {};
}

bool AudioFileReader::isOpen() const noexcept {
    return m_impl && m_impl->file;
}

const AudioFileInfo& AudioFileReader::info() const noexcept {
    static const AudioFileInfo empty;
    return m_impl ? m_impl->info : empty;
}

Result AudioFileReader::seek(FrameCount frame) {
    if (!isOpen())
        return Result::fail(EngineError::InvalidArgument, "audio reader is closed");
    const FrameCount clamped = std::min(frame, m_impl->info.frames);
    if (sf_seek(m_impl->file, static_cast<sf_count_t>(clamped), SEEK_SET) < 0) {
        return Result::fail(EngineError::UnsupportedFormat,
                            std::string("audio seek failed: ") +
                                sf_strerror(m_impl->file));
    }
    return Result::ok();
}

FrameCount AudioFileReader::read(float* destination, FrameCount frames) {
    if (!isOpen() || !destination || frames == 0) return 0;
    const sf_count_t requested = static_cast<sf_count_t>(std::min<FrameCount>(
        frames, FrameCount(std::numeric_limits<sf_count_t>::max())));
    const sf_count_t received = sf_readf_float(m_impl->file, destination, requested);
    return received > 0 ? static_cast<FrameCount>(received) : 0;
}

// ── Format mapping ─────────────────────────────────────────────────────────

namespace {

int majorFormatFor(Container container) {
    switch (container) {
        case Container::Wav: return SF_FORMAT_WAV;
        case Container::Aiff: return SF_FORMAT_AIFF;
        case Container::Flac: return SF_FORMAT_FLAC;
        case Container::OggVorbis: return SF_FORMAT_OGG;
        case Container::Opus: return SF_FORMAT_OGG;
        case Container::Mp3: return SF_FORMAT_MPEG;
        case Container::Caf: return SF_FORMAT_CAF;
        case Container::W64: return SF_FORMAT_W64;
    }
    return SF_FORMAT_WAV;
}

int minorFormatFor(Encoding encoding) {
    switch (encoding) {
        case Encoding::Int16: return SF_FORMAT_PCM_16;
        case Encoding::Int24: return SF_FORMAT_PCM_24;
        case Encoding::Int32: return SF_FORMAT_PCM_32;
        case Encoding::Float32: return SF_FORMAT_FLOAT;
        case Encoding::Float64: return SF_FORMAT_DOUBLE;
        case Encoding::Vorbis: return SF_FORMAT_VORBIS;
        case Encoding::Opus: return SF_FORMAT_OPUS;
        case Encoding::Mp3: return SF_FORMAT_MPEG_LAYER_III;
    }
    return SF_FORMAT_FLOAT;
}

/// Integer targets are the only ones that can be damaged by a sample outside
/// [-1, 1]: libsndfile wraps such a value round the modulus by default, turning
/// one hot peak into full-scale noise. Float targets store it faithfully and
/// need no help.
bool isIntegerEncoding(Encoding encoding) {
    return encoding == Encoding::Int16 || encoding == Encoding::Int24 ||
           encoding == Encoding::Int32;
}

/// Whether the payload alone would overflow a classic RIFF size field. Only
/// meaningful for the uncompressed containers, which is where it is asked.
std::size_t bytesPerSampleFor(Encoding encoding) {
    switch (encoding) {
        case Encoding::Int16: return 2;
        case Encoding::Int24: return 3;
        case Encoding::Int32:
        case Encoding::Float32: return 4;
        case Encoding::Float64: return 8;
        default: return 0;
    }
}

constexpr std::array<Encoding, 5> kPcmWide = {
    Encoding::Int24, Encoding::Int16, Encoding::Int32, Encoding::Float32,
    Encoding::Float64};
constexpr std::array<Encoding, 4> kPcmAiff = {
    Encoding::Int24, Encoding::Int16, Encoding::Int32, Encoding::Float32};
constexpr std::array<Encoding, 2> kPcmFlac = {Encoding::Int24, Encoding::Int16};
constexpr std::array<Encoding, 3> kPcmCaf = {Encoding::Int24, Encoding::Int16,
                                             Encoding::Float32};
constexpr std::array<Encoding, 1> kVorbisOnly = {Encoding::Vorbis};
constexpr std::array<Encoding, 1> kOpusOnly = {Encoding::Opus};
constexpr std::array<Encoding, 1> kMp3Only = {Encoding::Mp3};

/// The only rates the Opus encoder accepts. Everything else is refused at
/// `sf_open`, so the caller has to know before it offers the choice.
bool isOpusSampleRate(SampleRate rate) {
    const int hz = int(rate);
    return hz == 8000 || hz == 12000 || hz == 16000 || hz == 24000 || hz == 48000;
}

/// libsndfile takes a compression level, not a bit rate, so a bit rate the user
/// picked has to be turned into one. The MPEG encoder spreads 0…1 linearly over
/// its 320…32 kbit/s range; anything outside that clamps to an end.
double compressionForBitrate(int kbps) {
    const double clamped = std::clamp(double(kbps), 32.0, 320.0);
    return (320.0 - clamped) / (320.0 - 32.0);
}

/// Bits in the fixed-point word an encoding writes, or zero when it is not
/// fixed point at all.
int integerBitsFor(Encoding encoding) {
    switch (encoding) {
        case Encoding::Int16: return 16;
        case Encoding::Int24: return 24;
        case Encoding::Int32: return 32;
        default: return 0;
    }
}

/// xorshift32 — small, fast and repeatable.
inline std::uint32_t nextRandom(std::uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/// Fill an SF_INFO the way `open` would, so the format check and the open agree
/// on exactly one description of the file.
SF_INFO describeFile(const WriteSpec& spec, ChannelCount channels,
                     SampleRate sampleRate, std::uint64_t expectedFrames) {
    SF_INFO info{};
    info.channels = int(channels);
    info.samplerate = int(sampleRate);

    int major = majorFormatFor(spec.container);
    if (spec.container == Container::Wav) {
        // RF64 is the same audio in a container whose size field is 64-bit.
        // Switching automatically means a four-hour stem set never silently
        // truncates, and a short render still gets a plain WAV everything reads.
        const std::uint64_t bytes = expectedFrames * std::uint64_t(channels) *
                                    bytesPerSampleFor(spec.encoding);
        if (bytes > std::numeric_limits<std::uint32_t>::max()) major = SF_FORMAT_RF64;
    }
    info.format = major | minorFormatFor(spec.encoding);
    return info;
}

} // namespace

std::string_view extensionFor(Container container) {
    switch (container) {
        case Container::Wav: return "wav";
        case Container::Aiff: return "aiff";
        case Container::Flac: return "flac";
        case Container::OggVorbis: return "ogg";
        case Container::Opus: return "opus";
        case Container::Mp3: return "mp3";
        case Container::Caf: return "caf";
        case Container::W64: return "w64";
    }
    return "wav";
}

std::span<const Encoding> encodingsFor(Container container) {
    switch (container) {
        case Container::Wav: return kPcmWide;
        case Container::Aiff: return kPcmAiff;
        case Container::Flac: return kPcmFlac;
        case Container::OggVorbis: return kVorbisOnly;
        case Container::Opus: return kOpusOnly;
        case Container::Mp3: return kMp3Only;
        case Container::Caf: return kPcmCaf;
        case Container::W64: return kPcmAiff;
    }
    return kPcmWide;
}

std::string_view describeEncoding(Encoding encoding) {
    switch (encoding) {
        case Encoding::Int16: return "16-bit";
        case Encoding::Int24: return "24-bit";
        case Encoding::Int32: return "32-bit integer";
        case Encoding::Float32: return "32-bit float";
        case Encoding::Float64: return "64-bit float";
        case Encoding::Vorbis: return "Vorbis VBR";
        case Encoding::Opus: return "Opus VBR";
        case Encoding::Mp3: return "MP3";
    }
    return "";
}

bool isWriteSpecSupported(const WriteSpec& spec, ChannelCount channels,
                          SampleRate sampleRate) {
    if (channels == 0 || sampleRate <= 0.0) return false;
    // Measured, not assumed: `sf_format_check` happily approves Opus at 44.1 kHz
    // and then `sf_open` fails with "Opus only supports sample rates of 8000,
    // 12000, 16000, 24000, and 48000". The check has to be made here or the UI
    // offers a combination that cannot open.
    if (spec.encoding == Encoding::Opus && !isOpusSampleRate(sampleRate)) {
        return false;
    }
    SF_INFO info = describeFile(spec, channels, sampleRate, 0);
    return sf_format_check(&info) != 0;
}

namespace {

std::string_view coverMime(const std::vector<std::uint8_t>& data) {
    constexpr std::array<std::uint8_t, 8> png{137, 80, 78, 71, 13, 10, 26, 10};
    if (data.size() >= png.size() && std::equal(png.begin(), png.end(), data.begin()))
        return "image/png";
    if (data.size() >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return "image/jpeg";
    return {};
}

// libsndfile's MP3 writer can emit only ID3v1 text. Write ID3v2.4 for a front
// cover and full UTF-8 titles (https://id3.org/id3v2.4.0-frames).
// The render owns an unpublished temporary file until close succeeds. Move
// its audio backwards in bounded blocks; even a long render stays bounded.
Result embedMp3Tags(const std::string& path, const FileTags& tags) {
    if (tags.title.empty() && tags.artist.empty() && tags.album.empty()
        && tags.comment.empty() && tags.software.empty() && tags.coverArt.empty()) return Result::ok();
    const auto fail = [] {
        return Result::fail(EngineError::FileWriteError, "could not embed MP3 metadata");
    };
    std::fstream file(daw::platform::pathFromUtf8(path),
                      std::ios::binary | std::ios::in | std::ios::out);
    if (!file) return fail();
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    file.seekg(0);
    std::array<unsigned char, 10> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return fail();
    std::uint32_t oldSize = 0;
    std::streamoff audioStart = 0;
    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        // These are fresh libsndfile files, never arbitrary imported tags.
        // Refuse flags we cannot preserve rather than damage their metadata.
        if ((header[3] != 3 && header[3] != 4) || header[4] || header[5]) return fail();
        for (int i = 6; i < 10; ++i) {
            if (header[i] & 0x80) return fail();
            oldSize = (oldSize << 7) | header[i];
        }
        audioStart = 10 + oldSize;
        if (audioStart > length || oldSize > 32 * 1024 * 1024) return fail();
    }
    header = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
    std::vector<char> body;
    auto addFrame = [&](std::string_view id, const std::string& payload) {
        body.insert(body.end(), id.begin(), id.end());
        for (int i = 3; i >= 0; --i) body.push_back(char((payload.size() >> (i * 7)) & 0x7f));
        body.insert(body.end(), {0, 0});
        body.insert(body.end(), payload.begin(), payload.end());
    };
    auto textFrame = [&](std::string_view id, const std::string& value) {
        if (!value.empty()) addFrame(id, std::string(1, char(3)) + value);
    };
    textFrame("TIT2", tags.title);
    textFrame("TPE1", tags.artist);
    textFrame("TALB", tags.album);
    textFrame("TSSE", tags.software);
    if (!tags.comment.empty()) addFrame("COMM", std::string("\3eng\0", 5) + tags.comment);
    if (!tags.coverArt.empty()) {
        std::string payload(1, '\0');
        payload += coverMime(tags.coverArt);
        payload.append("\0\3\0", 3); // MIME terminator, front cover, empty description
        payload.append(reinterpret_cast<const char*>(tags.coverArt.data()), tags.coverArt.size());
        addFrame("APIC", payload);
    }
    if (body.empty()) return Result::ok();
    // Retain space if a future codec writes a larger tag, so the copy only
    // ever moves forward in the file and can safely proceed back to front.
    body.resize(std::max<std::size_t>(body.size(), oldSize), 0);
    const auto tagSize = std::uint32_t(body.size());
    for (int i = 0; i < 4; ++i) header[6 + i] = (tagSize >> ((3 - i) * 7)) & 0x7f;

    const std::streamoff shift = 10 + tagSize - audioStart;
    std::array<char, 64 * 1024> block;
    for (std::streamoff end = length; end > audioStart;) {
        const auto count = std::min<std::streamoff>(block.size(), end - audioStart);
        end -= count;
        file.seekg(end);
        if (!file.read(block.data(), count)) return fail();
        file.seekp(end + shift);
        if (!file.write(block.data(), count)) return fail();
    }
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    file.write(body.data(), body.size());
    file.flush();
    if (!file) return fail();
    file.close();
    return file.fail() ? fail() : Result::ok();
}

} // namespace

struct AudioFileWriter::Impl {
    SNDFILE* file = nullptr;
    ChannelCount channels = 0;
    std::vector<float> interleaved;
    std::string error;
    std::string path;
    bool mp3 = false;
    FileTags tags;
    /// Bits in the target fixed-point word, or zero for float and lossy
    /// encodings, which are written straight through as floats.
    int integerBits = 0;
    /// Whether to add TPDF dither on the way into that word.
    bool dither = false;
    std::vector<int> quantized;
    /// Deterministic on purpose: rendering the same project twice has to give
    /// the same file, and a clock- or entropy-seeded generator would break that
    /// for no benefit — dither only has to be uncorrelated with the signal.
    std::uint32_t ditherState = 0x9E3779B9u;
};

AudioFileWriter::AudioFileWriter() : m_impl(std::make_unique<Impl>()) {}
AudioFileWriter::~AudioFileWriter() { (void)close(); }
AudioFileWriter::AudioFileWriter(AudioFileWriter&&) noexcept = default;
AudioFileWriter& AudioFileWriter::operator=(AudioFileWriter&&) noexcept = default;

Result AudioFileWriter::open(const std::string& path, SampleRate sampleRate,
                             ChannelCount channels, std::uint64_t expectedFrames) {
    return open(path, WriteSpec{}, sampleRate, channels, expectedFrames);
}

Result AudioFileWriter::open(const std::string& path, const WriteSpec& spec,
                             SampleRate sampleRate, ChannelCount channels,
                             std::uint64_t expectedFrames) {
    if (!m_impl) m_impl = std::make_unique<Impl>();
    if (m_impl->file) return Result::fail(EngineError::AlreadyRendering);
    if (channels == 0 || sampleRate <= 0.0) {
        return Result::fail(EngineError::InvalidArgument);
    }

    SF_INFO info = describeFile(spec, channels, sampleRate, expectedFrames);
    // Asked before opening so the caller gets "this build cannot write Opus at
    // 44.1 kHz" rather than libsndfile's generic "format not recognised".
    if (!sf_format_check(&info)) {
        return Result::fail(EngineError::UnsupportedFormat,
                            "this build cannot write that format at " +
                                std::to_string(int(sampleRate)) + " Hz");
    }

    m_impl->file = openSoundFile(path, SFM_WRITE, &info);
    if (!m_impl->file) {
        return Result::fail(EngineError::FileWriteError,
                            std::string("sf_open failed: ") + sf_strerror(nullptr));
    }

    if (isIntegerEncoding(spec.encoding)) {
        // Without this a sample past full scale wraps round the modulus, so one
        // hot peak becomes a burst of full-scale noise. Clamping is not
        // mastering, it is refusing to corrupt the file.
        sf_command(m_impl->file, SFC_SET_CLIPPING, nullptr, SF_TRUE);
    }

    // `SFC_SET_VBR_ENCODING_QUALITY` runs the way you would hope — 1 is best,
    // 0 is smallest. Measured across Vorbis, Opus and MP3 rather than taken from
    // the docs, because `SFC_SET_COMPRESSION_LEVEL` right next to it runs the
    // other way and the two are easy to transpose.
    if (spec.encoding == Encoding::Vorbis || spec.encoding == Encoding::Opus) {
        double quality = std::clamp(spec.vbrQuality, 0.0, 1.0);
        sf_command(m_impl->file, SFC_SET_VBR_ENCODING_QUALITY, &quality,
                   sizeof(quality));
    } else if (spec.encoding == Encoding::Mp3) {
        int mode = spec.bitrateKbps > 0 ? SF_BITRATE_MODE_CONSTANT
                                        : SF_BITRATE_MODE_VARIABLE;
        sf_command(m_impl->file, SFC_SET_BITRATE_MODE, &mode, sizeof(mode));
        if (spec.bitrateKbps > 0) {
            // Compression level is the inverse scale: 0 is the best quality.
            // The linear 320…32 kbit/s mapping was verified end to end — asking
            // for 128 gives ~130 kbit/s, asking for 320 gives ~324.
            double compression = compressionForBitrate(spec.bitrateKbps);
            sf_command(m_impl->file, SFC_SET_COMPRESSION_LEVEL, &compression,
                       sizeof(compression));
        } else {
            double quality = std::clamp(spec.vbrQuality, 0.0, 1.0);
            sf_command(m_impl->file, SFC_SET_VBR_ENCODING_QUALITY, &quality,
                       sizeof(quality));
        }
    }

    m_impl->channels = channels;
    m_impl->path = path;
    m_impl->mp3 = spec.container == Container::Mp3;
    m_impl->tags = {};
    m_impl->integerBits = integerBitsFor(spec.encoding);
    // 32-bit integer resolution is already far below any noise floor, so dither
    // there would only add audible noise for nothing.
    m_impl->dither = spec.dither && m_impl->integerBits > 0 &&
                     m_impl->integerBits < 32;
    m_impl->interleaved.reserve(std::size_t(channels) * 8192);
    return Result::ok();
}

Result AudioFileWriter::setTags(const FileTags& tags) {
    if (!m_impl || !m_impl->file) return Result::fail(EngineError::NotInitialized);
    if (m_impl->mp3 && !tags.coverArt.empty()) {
        if (tags.coverArt.size() > 10 * 1024 * 1024 || coverMime(tags.coverArt).empty())
            return Result::fail(EngineError::InvalidArgument, "MP3 cover must be PNG or JPEG up to 10 MiB");
    }
    if (m_impl->mp3) {
        for (const auto* text : {&tags.title, &tags.artist, &tags.album, &tags.comment, &tags.software})
            if (text->size() > 1024 * 1024)
                return Result::fail(EngineError::InvalidArgument, "MP3 text tag is too large");
        m_impl->tags = tags;
    }

    // Return values are ignored on purpose: a container that cannot carry a
    // given string says so, and that is not a reason to fail a render.
    auto put = [this](int field, const std::string& value) {
        if (!value.empty()) sf_set_string(m_impl->file, field, value.c_str());
    };
    put(SF_STR_TITLE, tags.title);
    put(SF_STR_ARTIST, tags.artist);
    put(SF_STR_COMMENT, tags.comment);
    put(SF_STR_SOFTWARE, tags.software);
    put(SF_STR_ALBUM, tags.album);

    // The Broadcast Wave time reference is what lets a stem be dropped back at
    // its own timeline position rather than at zero. Only the RIFF-family
    // containers have somewhere to put it; the rest refuse the command.
    SF_BROADCAST_INFO broadcast{};
    std::snprintf(broadcast.description, sizeof(broadcast.description), "%s",
                  tags.title.c_str());
    std::snprintf(broadcast.originator, sizeof(broadcast.originator), "%s",
                  tags.software.c_str());
    broadcast.version = 1;
    broadcast.time_reference_low =
        std::uint32_t(tags.timeReferenceSamples & 0xFFFFFFFFull);
    broadcast.time_reference_high =
        std::uint32_t(tags.timeReferenceSamples >> 32);
    sf_command(m_impl->file, SFC_SET_BROADCAST_INFO, &broadcast,
               sizeof(broadcast));
    return Result::ok();
}

Result AudioFileWriter::write(const float* const* channels, FrameCount frames,
                              float gain) {
    if (!m_impl || !m_impl->file) return Result::fail(EngineError::NotInitialized);
    if (!channels || frames == 0) return Result::ok();

    const std::size_t samples = std::size_t(frames) * m_impl->channels;
    m_impl->interleaved.resize(samples);
    for (FrameCount frame = 0; frame < frames; ++frame) {
        for (ChannelCount channel = 0; channel < m_impl->channels; ++channel) {
            const float* source = channels[channel];
            m_impl->interleaved[std::size_t(frame) * m_impl->channels + channel] =
                source ? source[frame] * gain : 0.0f;
        }
    }
    sf_count_t written = 0;
    if (m_impl->integerBits > 0) {
        // Quantise here rather than handing floats to libsndfile.
        //
        // Measured, not assumed: libsndfile's float-to-integer conversion
        // *floors*. An input of -0.1 LSB comes back as -1 LSB, so every integer
        // export picks up a systematic half-LSB negative offset and twice the
        // quantisation error round-to-nearest would give. Writing whole words
        // through `sf_writef_int` avoids that path entirely — libsndfile then
        // only narrows by a shift, which is exact — and it puts the rounding and
        // the dither where they can be reasoned about.
        const int bits = m_impl->integerBits;
        const double scale = double(std::int64_t(1) << (bits - 1));
        const auto highest = std::int64_t(std::int64_t(1) << (bits - 1)) - 1;
        const auto lowest = -std::int64_t(std::int64_t(1) << (bits - 1));
        const int shift = 32 - bits;

        m_impl->quantized.resize(samples);
        constexpr double kToUnit = 1.0 / 4294967296.0;
        for (std::size_t i = 0; i < samples; ++i) {
            double value = double(m_impl->interleaved[i]) * scale;
            if (m_impl->dither) {
                // Triangular PDF, the difference of two uniform draws, one LSB
                // either way. That shape is what decorrelates the quantisation
                // error from the signal: a fade should end in steady noise, not
                // in a staircase whose steps follow the music.
                const double a = double(nextRandom(m_impl->ditherState)) * kToUnit;
                const double b = double(nextRandom(m_impl->ditherState)) * kToUnit;
                value += a - b;
            }
            const std::int64_t word =
                std::clamp(std::int64_t(std::llround(value)), lowest, highest);
            m_impl->quantized[i] = int(word << shift);
        }
        written = sf_writef_int(m_impl->file, m_impl->quantized.data(), frames);
    } else {
        written = sf_writef_float(m_impl->file, m_impl->interleaved.data(), frames);
    }
    if (written != sf_count_t(frames)) {
        return Result::fail(EngineError::FileWriteError,
                            std::string("audio write failed: ") +
                                sf_strerror(m_impl->file));
    }
    return Result::ok();
}

Result AudioFileWriter::close() {
    if (!m_impl || !m_impl->file) return Result::ok();
    SNDFILE* file = m_impl->file;
    m_impl->file = nullptr;
    const int result = sf_close(file);
    if (result != 0) return Result::fail(EngineError::FileWriteError,
                                       "failed to finalize audio file");
    if (m_impl->mp3) return embedMp3Tags(m_impl->path, m_impl->tags);
    return Result::ok();
}

} // namespace audio::platform
