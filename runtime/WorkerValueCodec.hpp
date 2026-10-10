#pragma once

#include "ProcessAudioResources.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_set>

namespace daw::worker_value {

inline constexpr std::size_t maxBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t maxDecodedBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t maxElements = 2u * 1024u * 1024u;
inline constexpr std::size_t maxString = 16u * 1024u * 1024u;
[[noreturn]] inline void invalid(const char* message) { throw std::invalid_argument(message); }

/// Define fields(Archive&, Value&) beside the value type or in worker_value.
/// The writer only visits field references; it never mutates their values.
/// Variable containers consume one shared budget for the complete message.
template<class Archive> void fields(Archive& archive, ProcessAudioResources::Record& value) {
    archive(value.id, value.fileName, value.channels, value.frames, value.sampleRate);
}

struct Writer {
    static constexpr bool reading = false;
    explicit Writer(ProcessAudioResources* resourceStore = nullptr) : resources(resourceStore) {}
    std::vector<std::uint8_t> bytes;
    ProcessAudioResources* resources = nullptr;
    std::size_t elements = 0;
    std::unordered_set<std::string> usedResources;
    void count(std::size_t n) {
        if (n > maxElements - elements) invalid("worker message collection exceeds limit");
        elements += n; one(std::uint32_t(n));
    }
    void append(std::span<const std::uint8_t> data) {
        if (data.size() > maxBytes - bytes.size()) invalid("worker message exceeds byte limit");
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    template<class... T> void operator()(const T&... value) { (one(value), ...); }
    template<class T> void one(const T& value) {
        if constexpr (std::is_same_v<T, bool>) one(std::uint8_t(value));
        else if constexpr (std::is_enum_v<T>) one(std::uint32_t(value));
        else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            const U bits = std::bit_cast<U>(value);
            std::uint8_t data[sizeof(T)];
            for (unsigned i = 0; i < sizeof(T); ++i) data[i] = std::uint8_t(bits >> (8 * i));
            append(data);
        } else if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) invalid("nonfinite worker message number");
            using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            one(std::bit_cast<U>(value));
        } else fields(*this, const_cast<T&>(value));
    }
    void one(const std::string& value) {
        if (value.size() > maxString) invalid("worker message string exceeds limit");
        one(std::uint32_t(value.size()));
        append({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    template<class T> void one(const std::vector<T>& value) {
        count(value.size());
        for (const auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(const std::array<T, N>& value) {
        for (const auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(const T (&value)[N]) {
        for (const auto& item : value) one(item);
    }
    void one(const std::shared_ptr<const engine::SampleBuffer>& value) {
        if (!value) { one(std::string{}); return; }
        if (!resources) invalid("audio resources are unavailable");
        auto id = resources->put(value);
        if (!id.empty()) usedResources.insert(id);
        one(id);
    }
};

struct Reader {
    static constexpr bool reading = true;
    explicit Reader(std::span<const std::uint8_t> data, const ProcessAudioResources::Samples* samples = nullptr)
        : bytes(data), resources(samples) {
        if (bytes.size() > maxBytes) invalid("worker message exceeds byte limit");
    }
    std::span<const std::uint8_t> bytes;
    const ProcessAudioResources::Samples* resources = nullptr;
    std::size_t cursor = 0, elements = 0, allocatedBytes = 0;
    // Encoded length cannot bound native storage: a vector of rich structs
    // may occupy far more bytes than its count and truncated wire fields.
    // Charge all dynamic owners cumulatively before allocating. PCM mappings
    // have their own validated resource limits and do not enter this budget.
    void chargeAllocation(std::size_t count, std::size_t elementBytes) {
        if (elementBytes && count > (maxDecodedBytes - allocatedBytes) / elementBytes)
            invalid("decoded audio values exceed allocation limit");
        allocatedBytes += count * elementBytes;
    }
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > bytes.size() - cursor) invalid("truncated worker message");
        const auto data = bytes.subspan(cursor, count); cursor += count; return data;
    }
    std::uint32_t count() {
        std::uint32_t n; one(n);
        if (n > maxElements - elements || n > bytes.size() - cursor) invalid("worker message collection exceeds limit");
        elements += n; return n;
    }
    template<class... T> void operator()(T&... value) { (one(value), ...); }
    template<class T> void one(T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t byte; one(byte); if (byte > 1) invalid("invalid worker message boolean"); value = byte != 0;
        } else if constexpr (std::is_enum_v<T>) {
            std::uint32_t number; one(number);
            if (number > std::uint64_t(std::numeric_limits<std::underlying_type_t<T>>::max()))
                invalid("audio enum representation overflow");
            value = T(number);
        } else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            U bits = 0; const auto data = take(sizeof(T));
            for (unsigned i = 0; i < sizeof(T); ++i) bits |= U(data[i]) << (8 * i);
            value = std::bit_cast<T>(bits);
        } else if constexpr (std::is_floating_point_v<T>) {
            using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            U bits; one(bits); value = std::bit_cast<T>(bits);
            if (!std::isfinite(value)) invalid("nonfinite worker message number");
        } else fields(*this, value);
    }
    void one(std::string& value) {
        std::uint32_t n; one(n); if (n > maxString) invalid("worker message string exceeds limit");
        const auto data = take(n);
        if (n) chargeAllocation(std::size_t(n) + 1, sizeof(char));
        value.assign(reinterpret_cast<const char*>(data.data()), data.size());
    }
    template<class T> void one(std::vector<T>& value) {
        const auto n = count(); chargeAllocation(n, sizeof(T));
        value.resize(n); for (auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(std::array<T, N>& value) {
        for (auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(T (&value)[N]) {
        for (auto& item : value) one(item);
    }
    void one(std::shared_ptr<const engine::SampleBuffer>& value) {
        std::string id; one(id); if (id.empty()) { value.reset(); return; }
        if (!resources) invalid("audio resources are unavailable");
        const auto found = resources->find(id);
        if (found == resources->end()) invalid("unknown audio resource ID");
        value = found->second;
    }
    void finish() const { if (cursor != bytes.size()) invalid("trailing worker message data"); }
};

/// These helpers have no framing/version of their own. The enclosing protocol
/// supplies its own version so incompatible workers can reject the message.
/// Decoding returns fully owned values and rejects any unconsumed trailing data.
template<class... Values>
std::vector<std::uint8_t> encode(const Values&... values) {
    Writer writer; writer(values...); return std::move(writer.bytes);
}
template<class... Values>
std::tuple<std::remove_cvref_t<Values>...> decode(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes);
    std::tuple<std::remove_cvref_t<Values>...> values;
    std::apply([&](auto&... value) { reader(value...); }, values);
    reader.finish(); return values;
}

/// Only referenced immutable PCM records enter this message's manifest. The
/// job owns its source buffers until the worker finishes.
template<class... Values>
std::vector<std::uint8_t> encodeResources(ProcessAudioResources& resources, const Values&... values) {
    Writer payload(&resources); payload(values...);
    Writer output; output.elements = payload.elements;
    std::vector<ProcessAudioResources::Record> records;
    records.reserve(payload.usedResources.size());
    for (const auto& record : resources.records())
        if (payload.usedResources.contains(record.id)) records.push_back(record);
    output(records); output.append(payload.bytes);
    return std::move(output.bytes);
}
template<class... Values>
std::tuple<std::remove_cvref_t<Values>...> decodeResources(std::span<const std::uint8_t> bytes,
    const std::filesystem::path& directory) {
    Reader reader(bytes);
    std::vector<ProcessAudioResources::Record> records; reader(records);
    const auto resources = ProcessAudioResources::load(directory, records);
    reader.resources = &resources;
    std::tuple<std::remove_cvref_t<Values>...> values;
    std::apply([&](auto&... value) { reader(value...); }, values);
    reader.finish();
    return values;
}

} // namespace daw::worker_value
