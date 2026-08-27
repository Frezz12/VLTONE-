#include "MidiFile.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <unordered_set>

namespace daw::midifile {

namespace {

/// A cursor over the file's bytes that cannot walk off the end.
///
/// Every read is bounds-checked and sets `overrun` instead of throwing, so the
/// parser can keep the "a truncated chunk ends that track, not the file"
/// behaviour that makes a half-written file still yield its good tracks.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : m_data(data), m_size(size) {}

    bool atEnd() const { return m_pos >= m_size; }
    std::size_t position() const { return m_pos; }
    std::size_t remaining() const { return m_pos < m_size ? m_size - m_pos : 0; }
    bool overran() const { return m_overrun; }

    std::uint8_t byte() {
        if (m_pos >= m_size) {
            m_overrun = true;
            return 0;
        }
        return m_data[m_pos++];
    }

    std::uint32_t be16() { return std::uint32_t(byte()) << 8 | byte(); }
    std::uint32_t be32() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | byte();
        return value;
    }

    /// Variable-length quantity: 7 bits a byte, high bit means "more".
    /// Four bytes is the format's maximum; a fifth continuation byte is a
    /// corrupt file rather than a very large number.
    std::uint32_t vlq(bool& ok) {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = byte();
            if (m_overrun) {
                ok = false;
                return 0;
            }
            value = (value << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) {
                ok = true;
                return value;
            }
        }
        ok = false;
        return value;
    }

    void skip(std::size_t count) {
        if (count > remaining()) {
            m_pos = m_size;
            m_overrun = true;
            return;
        }
        m_pos += count;
    }

    void seek(std::size_t position) {
        m_pos = std::min(position, m_size);
        if (position > m_size) m_overrun = true;
    }

    std::string text(std::size_t count) {
        const std::size_t take = std::min(count, remaining());
        std::string out(reinterpret_cast<const char*>(m_data + m_pos), take);
        skip(count);
        return out;
    }

private:
    const std::uint8_t* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_pos = 0;
    bool m_overrun = false;
};

/// A note-on waiting for its note-off, keyed by (channel, pitch) within a track.
struct Pending {
    std::uint64_t startTick = 0;
    int velocity = 100;
};

std::uint32_t chunkType(Reader& reader) { return reader.be32(); }

constexpr std::uint32_t kMThd = 0x4D546864;   // "MThd"
constexpr std::uint32_t kMTrk = 0x4D54726B;   // "MTrk"

} // namespace

int File::tracksWithNotes() const {
    std::unordered_set<int> seen;
    if (trackCount > 0) {
        seen.reserve(std::min(notes.size(), std::size_t(trackCount)));
    }
    for (const Note& note : notes) {
        seen.insert(note.track);
    }
    return int(seen.size());
}

bool parseBytes(const std::uint8_t* data, std::size_t size, File& out,
                std::string& error) {
    out = File{};
    error.clear();
    if (!data || size < 14) {
        error = "not a MIDI file (too short)";
        return false;
    }

    Reader reader(data, size);
    if (chunkType(reader) != kMThd) {
        error = "not a MIDI file (no MThd header)";
        return false;
    }
    const std::uint32_t headerLength = reader.be32();
    if (headerLength < 6) {
        error = "MIDI header is too short";
        return false;
    }
    const std::size_t headerStart = reader.position();
    const int format = int(reader.be16());
    const int declaredTracks = int(reader.be16());
    const std::uint32_t division = reader.be16();
    // Skip by the *declared* length, not by the six bytes just read: a header
    // with extra bytes is legal and everything after it would otherwise shift.
    reader.seek(headerStart + headerLength);

    if (format == 2) {
        error = "format 2 MIDI files hold independent sequences, not one song";
        return false;
    }
    if (format != 0 && format != 1) {
        error = "unsupported MIDI format " + std::to_string(format);
        return false;
    }
    if (division & 0x8000u) {
        error = "SMPTE-timed MIDI files are not supported";
        return false;
    }
    if ((division & 0x7FFFu) == 0) {
        error = "MIDI file declares zero ticks per quarter note";
        return false;
    }

    out.format = format;
    out.trackCount = declaredTracks;
    out.ticksPerQuarter = int(division & 0x7FFFu);
    const double perQuarter = double(out.ticksPerQuarter);

    int trackIndex = 0;
    while (!reader.atEnd() && reader.remaining() >= 8) {
        const std::uint32_t type = chunkType(reader);
        const std::uint32_t length = reader.be32();
        const std::size_t chunkStart = reader.position();
        if (reader.overran()) break;

        if (type != kMTrk) {
            // The format requires tolerating chunks we do not know.
            reader.seek(chunkStart + length);
            continue;
        }

        out.trackNames.emplace_back();
        const int track = trackIndex++;

        std::uint64_t tick = 0;
        std::uint8_t runningStatus = 0;
        // Several note-ons can be open on the same key; a stack pops the most
        // recent, which is how every sequencer writes overlapping notes.
        std::map<int, std::vector<Pending>> open;
        const std::size_t chunkEnd = chunkStart + std::size_t(length);

        while (reader.position() < chunkEnd && !reader.overran()) {
            bool ok = false;
            tick += reader.vlq(ok);
            if (!ok) break;

            std::uint8_t status = reader.byte();
            if (status < 0x80) {
                // Running status: the byte we just read is data, not a status.
                if (runningStatus == 0) break;   // no status to run from
                reader.seek(reader.position() - 1);
                status = runningStatus;
            } else if (status < 0xF0) {
                runningStatus = status;
            } else {
                runningStatus = 0;   // system messages cancel it
            }

            if (status == 0xFF) {
                const std::uint8_t meta = reader.byte();
                const std::uint32_t metaLength = reader.vlq(ok);
                if (!ok) break;
                const std::size_t next = reader.position() + metaLength;
                if (meta == 0x51 && metaLength >= 3) {
                    const std::uint32_t micros =
                        (std::uint32_t(reader.byte()) << 16) |
                        (std::uint32_t(reader.byte()) << 8) |
                        std::uint32_t(reader.byte());
                    const double bpm = micros > 0 ? 60000000.0 / double(micros) : 0.0;
                    if (out.firstTempoBpm <= 0.0) {
                        out.firstTempoBpm = bpm;
                    } else if (bpm > 0.0 &&
                               std::abs(bpm - out.firstTempoBpm) > 1e-6) {
                        out.hasTempoChanges = true;
                    }
                } else if (meta == 0x03 && metaLength > 0) {
                    out.trackNames[std::size_t(track)] = reader.text(metaLength);
                }
                reader.seek(next);
                if (meta == 0x2F) break;   // end of track
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                const std::uint32_t sysexLength = reader.vlq(ok);
                if (!ok) break;
                reader.skip(sysexLength);
                continue;
            }

            const std::uint8_t kind = std::uint8_t(status & 0xF0);
            const int channel = int(status & 0x0F);
            switch (kind) {
                case 0x80:
                case 0x90: {
                    const int pitch = int(reader.byte() & 0x7F);
                    const int velocity = int(reader.byte() & 0x7F);
                    const int key = channel * 128 + pitch;
                    // A note-on with zero velocity is a note-off — a convention
                    // old enough that most files still rely on it.
                    if (kind == 0x90 && velocity > 0) {
                        open[key].push_back(Pending{tick, velocity});
                    } else {
                        auto found = open.find(key);
                        if (found == open.end() || found->second.empty()) break;
                        const Pending started = found->second.back();
                        found->second.pop_back();
                        Note note;
                        note.pitch = pitch;
                        note.velocity = started.velocity;
                        note.channel = channel;
                        note.track = track;
                        note.startBeats = double(started.startTick) / perQuarter;
                        note.lengthBeats =
                            double(tick - started.startTick) / perQuarter;
                        if (note.lengthBeats <= 0.0)
                            note.lengthBeats = 1.0 / perQuarter;
                        out.notes.push_back(note);
                    }
                    break;
                }
                case 0xA0:
                case 0xB0:
                case 0xE0:
                    reader.skip(2);
                    break;
                case 0xC0:
                case 0xD0:
                    reader.skip(1);
                    break;
                default:
                    // An unknown channel message has no length we can trust.
                    reader.seek(chunkEnd);
                    break;
            }
        }

        // Anything still held when the track ends is closed there rather than
        // dropped — a file that forgot a note-off still gives a playable note.
        for (auto& [key, pendings] : open) {
            for (const Pending& started : pendings) {
                Note note;
                note.pitch = key % 128;
                note.channel = key / 128;
                note.track = track;
                note.velocity = started.velocity;
                note.startBeats = double(started.startTick) / perQuarter;
                note.lengthBeats = double(tick - started.startTick) / perQuarter;
                if (note.lengthBeats <= 0.0) note.lengthBeats = 1.0 / perQuarter;
                out.notes.push_back(note);
            }
        }

        // Always continue from where the chunk header said it ends, so one
        // malformed track cannot derail the ones after it.
        reader.seek(chunkStart + std::size_t(length));
    }

    if (out.notes.empty() && trackIndex == 0) {
        error = "the file has no MIDI tracks";
        return false;
    }

    std::sort(out.notes.begin(), out.notes.end(),
              [](const Note& a, const Note& b) {
                  if (a.startBeats != b.startBeats)
                      return a.startBeats < b.startBeats;
                  return a.pitch < b.pitch;
              });
    for (const Note& note : out.notes) {
        out.lengthBeats =
            std::max(out.lengthBeats, note.startBeats + note.lengthBeats);
    }
    return true;
}

bool parse(const std::string& path, File& out, std::string& error) {
    std::ifstream stream(platform::pathFromUtf8(path), std::ios::binary);
    if (!stream) {
        error = "could not open the file";
        return false;
    }

    // A stream-buffer iterator performs a virtual extraction for every byte.
    // MIDI files can be tens of megabytes, so size the destination once and
    // let the stream copy the file in one bulk read instead.
    stream.seekg(0, std::ios::end);
    const std::streampos end = stream.tellg();
    if (end == std::streampos(-1)) {
        error = "could not read the file";
        return false;
    }
    const std::streamoff length = end - std::streampos(0);
    if (length < 0 ||
        static_cast<std::uintmax_t>(length) >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max()) ||
        static_cast<std::uintmax_t>(length) >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::size_t>::max())) {
        error = "MIDI file is too large";
        return false;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.seekg(0, std::ios::beg);
    if (!stream) {
        error = "could not read the file";
        return false;
    }
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
            error = "could not read the file";
            return false;
        }
    }
    return parseBytes(bytes.data(), bytes.size(), out, error);
}

} // namespace daw::midifile
