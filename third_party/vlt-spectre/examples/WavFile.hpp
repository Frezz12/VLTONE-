// SPDX-License-Identifier: MIT
#pragma once
// A deliberately small WAV reader for the demo, not part of the analyzer library.
// Accepts little-endian PCM 8/16/24/32-bit and IEEE float32, mono or multichannel.
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

struct WavFile {
    double sampleRate = 0;
    std::vector<float> left, right;

    static std::optional<WavFile> load(const QString& path, QString& error) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return {}; }
        if (file.size() > 128 * 1024 * 1024) { error = QStringLiteral("The demo accepts WAV files up to 128 MiB."); return {}; }
        const auto bytes = file.readAll();
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        const auto size = std::size_t(bytes.size());
        const auto u16 = [&](std::size_t at) { return qFromLittleEndian<quint16>(data + at); };
        const auto u32 = [&](std::size_t at) { return qFromLittleEndian<quint32>(data + at); };
        if (size < 12 || std::memcmp(data, "RIFF", 4) || std::memcmp(data + 8, "WAVE", 4)) {
            error = QStringLiteral("Choose a little-endian RIFF/WAVE file."); return {};
        }
        unsigned format = 0, channels = 0, bits = 0, alignment = 0, rate = 0;
        std::size_t audioOffset = 0, audioSize = 0;
        for (std::size_t at = 12; at + 8 <= size;) {
            const auto length = std::size_t(u32(at + 4));
            const auto begin = at + 8;
            if (length > size - begin) { error = QStringLiteral("Truncated WAV chunk."); return {}; }
            if (!std::memcmp(data + at, "fmt ", 4) && length >= 16) {
                format = u16(begin); channels = u16(begin + 2); rate = u32(begin + 4);
                alignment = u16(begin + 12); bits = u16(begin + 14);
                if (format == 0xfffe) {
                    static const unsigned char guidTail[14]{0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                    if (length < 40 || u16(begin + 16) < 22 ||
                        std::memcmp(data + begin + 26, guidTail, 14) != 0 ||
                        u16(begin + 18) != bits) {
                        error = QStringLiteral("Unsupported WAV extensible format."); return {};
                    }
                    format = u16(begin + 24);
                }
            } else if (!std::memcmp(data + at, "data", 4)) { audioOffset = begin; audioSize = length; }
            at = begin + length + (length & 1);
        }
        const bool integerPcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        const bool floatPcm = format == 3 && bits == 32;
        if ((!integerPcm && !floatPcm) || channels == 0 || channels > 32 || rate < 1000 || rate > 768000 ||
            alignment != channels * (bits / 8) || audioOffset == 0 || audioSize < alignment) {
            error = QStringLiteral("Use uncompressed PCM 8/16/24/32-bit or float32 WAV audio."); return {};
        }
        const auto frames = audioSize / alignment;
        WavFile result;
        result.sampleRate = rate;
        result.left.resize(frames); result.right.resize(frames);
        const auto sampleAt = [&](std::size_t at) -> float {
            if (floatPcm) {
                const auto word = u32(at);
                float value = 0; std::memcpy(&value, &word, sizeof(value));
                return std::isfinite(value) ? value : 0.0f;
            }
            if (bits == 8) return (int(data[at]) - 128) / 128.0f;
            // Read unsigned bytes, then explicitly sign-extend; no unaligned casts.
            std::int64_t value = 0;
            for (unsigned byte = 0; byte < bits / 8; ++byte) value |= std::int64_t(data[at + byte]) << (8 * byte);
            if (value & (std::int64_t(1) << (bits - 1))) value -= std::int64_t(1) << bits;
            return float(double(value) / double(std::int64_t(1) << (bits - 1)));
        };
        for (std::size_t i = 0; i < frames; ++i) {
            result.left[i] = sampleAt(audioOffset + i * alignment);
            result.right[i] = channels == 1 ? result.left[i] : sampleAt(audioOffset + i * alignment + bits / 8);
        }
        return result;
    }
};