#include "recovery/CloudRecordingRecovery.hpp"

#include "platform/PathUtils.hpp"
#include "recovery/SessionFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace daw::recovery {
namespace {

using json = nlohmann::json;

constexpr double kMaximumTimeSeconds = 365.0 * 24.0 * 60.0 * 60.0;
constexpr double kMaximumSampleRate = 768000.0;
constexpr std::uint32_t kMaximumChannels = 1024;

CloudRecordingRecoveryResult result(CloudRecordingRecoveryCode code,
                                    std::string message = {}) {
    return {code, std::move(message)};
}

CloudRecordingRunCleanupResult cleanupResult(
    CloudRecordingRunCleanupCode code, std::string message = {}) {
    return {code, std::move(message)};
}

std::recursive_mutex recoveryStoreMutex;

struct RunCleanupIntent {
    std::string projectId;
    std::string sessionId;
    std::string runId;
    std::vector<std::string> wavPaths;
};

bool canonicalUuid(std::string_view value) {
    if (value.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
            continue;
        }
        const char ch = value[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
        nonzero = nonzero || ch != '0';
    }
    return nonzero;
}

bool boundedFinite(double value, double minimum, double maximum,
                   bool minimumExclusive = false) {
    if (!std::isfinite(value) || value > maximum) return false;
    return minimumExclusive ? value > minimum : value >= minimum;
}

bool validWavPathShape(const std::string& utf8Path) {
    if (utf8Path.empty() || utf8Path.size() > kMaxCloudRecordingPathBytes)
        return false;
    const fs::path path = platform::pathFromUtf8(utf8Path);
    if (!path.is_absolute()) return false;
    std::string extension = platform::pathToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    if (extension != ".wav") return false;
    return true;
}

bool wavPathExists(const std::string& utf8Path, bool requireBytes) {
    if (!validWavPathShape(utf8Path)) return false;
    const fs::path path = platform::pathFromUtf8(utf8Path);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return false;
    const std::uintmax_t bytes = fs::file_size(path, ec);
    return !ec && (!requireBytes || bytes > 0);
}

const char* modeName(CloudRecordingMode mode) {
    return mode == CloudRecordingMode::Layers ? "layers" : "overwrite";
}

std::optional<CloudRecordingMode> parseMode(const json& value) {
    if (!value.is_string()) return std::nullopt;
    const std::string text = value.get<std::string>();
    if (text == "overwrite") return CloudRecordingMode::Overwrite;
    if (text == "layers") return CloudRecordingMode::Layers;
    return std::nullopt;
}

const char* captureStatusName(CloudRecordingCaptureStatus status) {
    switch (status) {
        case CloudRecordingCaptureStatus::Ready: return "ready";
        case CloudRecordingCaptureStatus::ZeroFrames: return "zero_frames";
        case CloudRecordingCaptureStatus::Unreadable: return "unreadable";
        case CloudRecordingCaptureStatus::WriteFailed: return "write_failed";
    }
    return "unreadable";
}

std::optional<CloudRecordingCaptureStatus> parseCaptureStatus(
    const json& value) {
    if (!value.is_string()) return std::nullopt;
    const std::string text = value.get<std::string>();
    if (text == "ready") return CloudRecordingCaptureStatus::Ready;
    if (text == "zero_frames")
        return CloudRecordingCaptureStatus::ZeroFrames;
    if (text == "unreadable")
        return CloudRecordingCaptureStatus::Unreadable;
    if (text == "write_failed")
        return CloudRecordingCaptureStatus::WriteFailed;
    return std::nullopt;
}

const char* uploadPhaseName(CloudRecordingUploadPhase phase) {
    return phase == CloudRecordingUploadPhase::Captured ? "captured" : "";
}

std::optional<CloudRecordingUploadPhase> parseUploadPhase(
    const json& value) {
    if (!value.is_string()) return std::nullopt;
    if (value.get<std::string>() == "captured")
        return CloudRecordingUploadPhase::Captured;
    return std::nullopt;
}

const char* commitPhaseName(CloudRecordingCommitPhase phase) {
    return phase == CloudRecordingCommitPhase::Pending ? "pending" : "";
}

std::optional<CloudRecordingCommitPhase> parseCommitPhase(
    const json& value) {
    if (!value.is_string()) return std::nullopt;
    if (value.get<std::string>() == "pending")
        return CloudRecordingCommitPhase::Pending;
    return std::nullopt;
}

template <std::size_t N>
bool exactKeys(const json& object, const std::array<const char*, N>& keys) {
    if (!object.is_object() || object.size() != N) return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) {
        return object.contains(key);
    });
}

json passToJson(const CloudRecordingPass& pass) {
    return {{"startSeconds", pass.startSeconds},
            {"endSeconds", pass.endSeconds},
            {"captureOffsetSeconds", pass.captureOffsetSeconds}};
}

json semanticsToJson(const CloudRecordingSemantics& semantics) {
    return {{"mode", modeName(semantics.mode)},
            {"complete", semantics.complete},
            {"loopEnabled", semantics.loopEnabled},
            {"loopStartSeconds", semantics.loopStartSeconds},
            {"loopEndSeconds", semantics.loopEndSeconds},
            {"loopCreatesTakes", semantics.loopCreatesTakes},
            {"trimTakesToRegion", semantics.trimTakesToRegion},
            {"autoExpandAfterRecord", semantics.autoExpandAfterRecord},
            {"compCrossfadeMs", semantics.compCrossfadeMs}};
}

json captureToJson(const CloudRecordingCapture& capture) {
    json passes = json::array();
    for (const CloudRecordingPass& pass : capture.passes)
        passes.push_back(passToJson(pass));
    return {{"captureId", capture.captureId},
            {"trackId", capture.trackId},
            {"leaseId", capture.leaseId},
            {"uploadId", capture.uploadId},
            {"assetId", capture.assetId},
            {"uploadPhase", uploadPhaseName(capture.uploadPhase)},
            {"status", captureStatusName(capture.status)},
            {"localWavPath", capture.localWavPath},
            {"startSeconds", capture.startSeconds},
            {"durationSeconds", capture.durationSeconds},
            {"sampleRate", capture.sampleRate},
            {"channels", capture.channels},
            {"frames", capture.frames},
            {"passes", std::move(passes)},
            {"semantics", semanticsToJson(capture.semantics)}};
}

json runToJson(const CloudRecordingRecoveryRun& run) {
    json captures = json::array();
    for (const CloudRecordingCapture& capture : run.captures)
        captures.push_back(captureToJson(capture));
    return {{"runId", run.runId},
            {"opId", run.opId},
            {"transactionId", run.transactionId},
            {"createdAt", run.createdAtUnixMs},
            {"recoveryOnly", run.recoveryOnly},
            {"lostLease", run.lostLease},
            {"commitPhase", commitPhaseName(run.commitPhase)},
            {"captures", std::move(captures)}};
}

json manifestToJson(const CloudRecordingRecoveryManifest& manifest) {
    json runs = json::array();
    for (const CloudRecordingRecoveryRun& run : manifest.runs)
        runs.push_back(runToJson(run));
    return {{"format", kCloudRecordingRecoveryFormat},
            {"version", manifest.version},
            {"projectId", manifest.projectId},
            {"sessionId", manifest.sessionId},
            {"createdAt", manifest.createdAtUnixMs},
            {"runs", std::move(runs)}};
}

json cleanupIntentToJson(const RunCleanupIntent& intent) {
    return {{"format", kCloudRecordingRunCleanupFormat},
            {"version", kCloudRecordingRunCleanupVersion},
            {"projectId", intent.projectId},
            {"sessionId", intent.sessionId},
            {"runId", intent.runId},
            {"wavPaths", intent.wavPaths}};
}

bool readDouble(const json& object, const char* key, double& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return false;
    try {
        out = found->get<double>();
    } catch (const json::exception&) {
        return false;
    }
    return std::isfinite(out);
}

template <typename T>
bool readUnsigned(const json& object, const char* key, T& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_unsigned()) return false;
    try {
        const std::uint64_t value = found->get<std::uint64_t>();
        if (value > std::uint64_t(std::numeric_limits<T>::max())) return false;
        out = static_cast<T>(value);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool readSigned(const json& object, const char* key, std::int64_t& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) return false;
    try {
        out = found->get<std::int64_t>();
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool readString(const json& object, const char* key, std::string& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return false;
    try {
        out = found->get<std::string>();
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool readBool(const json& object, const char* key, bool& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_boolean()) return false;
    out = found->get<bool>();
    return true;
}

bool parseCleanupIntent(const json& value, RunCleanupIntent& out) {
    static constexpr std::array keys{
        "format", "version", "projectId", "sessionId", "runId",
        "wavPaths"};
    std::uint32_t version = 0;
    if (!exactKeys(value, keys) ||
        !value.at("format").is_string() ||
        value.at("format").get<std::string>() !=
            kCloudRecordingRunCleanupFormat ||
        !readUnsigned(value, "version", version) ||
        version != kCloudRecordingRunCleanupVersion ||
        !readString(value, "projectId", out.projectId) ||
        !readString(value, "sessionId", out.sessionId) ||
        !readString(value, "runId", out.runId) ||
        !canonicalUuid(out.projectId) || !canonicalUuid(out.sessionId) ||
        !canonicalUuid(out.runId) || !value.at("wavPaths").is_array() ||
        value.at("wavPaths").empty() ||
        value.at("wavPaths").size() > kMaxCloudRecordingCaptures) {
        return false;
    }

    std::set<std::string> uniquePaths;
    out.wavPaths.clear();
    out.wavPaths.reserve(value.at("wavPaths").size());
    for (const json& encoded : value.at("wavPaths")) {
        if (!encoded.is_string()) return false;
        std::string path;
        try {
            path = encoded.get<std::string>();
        } catch (const json::exception&) {
            return false;
        }
        if (!validWavPathShape(path) ||
            !uniquePaths.insert(path).second) {
            return false;
        }
        out.wavPaths.push_back(std::move(path));
    }
    return true;
}

bool parsePass(const json& value, CloudRecordingPass& out) {
    static constexpr std::array keys{
        "startSeconds", "endSeconds", "captureOffsetSeconds"};
    return exactKeys(value, keys) &&
           readDouble(value, "startSeconds", out.startSeconds) &&
           readDouble(value, "endSeconds", out.endSeconds) &&
           readDouble(value, "captureOffsetSeconds",
                      out.captureOffsetSeconds);
}

bool parseSemantics(const json& value, CloudRecordingSemantics& out) {
    static constexpr std::array keys{
        "mode", "complete", "loopEnabled", "loopStartSeconds",
        "loopEndSeconds", "loopCreatesTakes", "trimTakesToRegion",
        "autoExpandAfterRecord", "compCrossfadeMs"};
    if (!exactKeys(value, keys)) return false;
    const auto mode = parseMode(value.at("mode"));
    if (!mode || !readBool(value, "complete", out.complete) ||
        !readBool(value, "loopEnabled", out.loopEnabled) ||
        !readDouble(value, "loopStartSeconds", out.loopStartSeconds) ||
        !readDouble(value, "loopEndSeconds", out.loopEndSeconds) ||
        !readBool(value, "loopCreatesTakes", out.loopCreatesTakes) ||
        !readBool(value, "trimTakesToRegion", out.trimTakesToRegion) ||
        !readBool(value, "autoExpandAfterRecord",
                  out.autoExpandAfterRecord) ||
        !readDouble(value, "compCrossfadeMs", out.compCrossfadeMs)) {
        return false;
    }
    out.mode = *mode;
    return true;
}

bool parseCaptureV2(const json& value, CloudRecordingCapture& out) {
    static constexpr std::array keys{
        "captureId", "trackId", "leaseId", "uploadId", "assetId",
        "uploadPhase", "status", "localWavPath", "startSeconds",
        "durationSeconds", "sampleRate", "channels", "frames", "passes",
        "semantics"};
    if (!exactKeys(value, keys) ||
        !readString(value, "captureId", out.captureId) ||
        !readString(value, "trackId", out.trackId) ||
        !readString(value, "leaseId", out.leaseId) ||
        !readString(value, "uploadId", out.uploadId) ||
        !readString(value, "assetId", out.assetId) ||
        !readString(value, "localWavPath", out.localWavPath) ||
        !readDouble(value, "startSeconds", out.startSeconds) ||
        !readDouble(value, "durationSeconds", out.durationSeconds) ||
        !readDouble(value, "sampleRate", out.sampleRate) ||
        !readUnsigned(value, "channels", out.channels) ||
        !readUnsigned(value, "frames", out.frames)) {
        return false;
    }
    const auto uploadPhase = parseUploadPhase(value.at("uploadPhase"));
    const auto status = parseCaptureStatus(value.at("status"));
    if (!uploadPhase || !status ||
        !parseSemantics(value.at("semantics"), out.semantics) ||
        !value.at("passes").is_array() ||
        value.at("passes").size() > kMaxCloudRecordingPassesPerCapture) {
        return false;
    }
    out.uploadPhase = *uploadPhase;
    out.status = *status;
    out.passes.clear();
    out.passes.reserve(value.at("passes").size());
    for (const json& encoded : value.at("passes")) {
        CloudRecordingPass pass;
        if (!parsePass(encoded, pass)) return false;
        out.passes.push_back(pass);
    }
    return true;
}

bool parseRunV2(const json& value, CloudRecordingRecoveryRun& out) {
    static constexpr std::array keys{
        "runId", "opId", "transactionId", "createdAt", "recoveryOnly",
        "lostLease", "commitPhase", "captures"};
    if (!exactKeys(value, keys) ||
        !readString(value, "runId", out.runId) ||
        !readString(value, "opId", out.opId) ||
        !readString(value, "transactionId", out.transactionId) ||
        !readSigned(value, "createdAt", out.createdAtUnixMs) ||
        !readBool(value, "recoveryOnly", out.recoveryOnly) ||
        !readBool(value, "lostLease", out.lostLease)) {
        return false;
    }
    const auto phase = parseCommitPhase(value.at("commitPhase"));
    if (!phase || !value.at("captures").is_array() ||
        value.at("captures").empty() ||
        value.at("captures").size() > kMaxCloudRecordingCaptures) {
        return false;
    }
    out.commitPhase = *phase;
    out.captures.clear();
    out.captures.reserve(value.at("captures").size());
    for (const json& encoded : value.at("captures")) {
        CloudRecordingCapture capture;
        if (!parseCaptureV2(encoded, capture)) return false;
        out.captures.push_back(std::move(capture));
    }
    return true;
}

std::uint64_t stableHash(std::string_view text, std::uint64_t seed) {
    std::uint64_t value = seed;
    for (const unsigned char ch : text) {
        value ^= std::uint64_t(ch);
        value *= 1099511628211ULL;
    }
    return value;
}

std::string deterministicUuid(std::string_view domain,
                              std::string_view identity) {
    std::array<unsigned char, 16> bytes{};
    const std::uint64_t first = stableHash(
        domain, stableHash(identity, 1469598103934665603ULL));
    const std::uint64_t second = stableHash(
        identity, stableHash(domain, 7809847782465536322ULL));
    for (int index = 0; index < 8; ++index) {
        bytes[std::size_t(index)] =
            static_cast<unsigned char>(first >> (56 - index * 8));
        bytes[std::size_t(index + 8)] =
            static_cast<unsigned char>(second >> (56 - index * 8));
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x50U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    static constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            uuid.push_back('-');
        uuid.push_back(hex[bytes[index] >> 4]);
        uuid.push_back(hex[bytes[index] & 0x0fU]);
    }
    return uuid;
}

bool parseCaptureV1(const json& value, CloudRecordingCapture& out) {
    static constexpr std::array keys{
        "trackId", "leaseId", "localWavPath", "startSeconds",
        "durationSeconds", "sampleRate", "channels", "frames", "passes",
        "mode"};
    if (!exactKeys(value, keys) ||
        !readString(value, "trackId", out.trackId) ||
        !readString(value, "leaseId", out.leaseId) ||
        !readString(value, "localWavPath", out.localWavPath) ||
        !readDouble(value, "startSeconds", out.startSeconds) ||
        !readDouble(value, "durationSeconds", out.durationSeconds) ||
        !readDouble(value, "sampleRate", out.sampleRate) ||
        !readUnsigned(value, "channels", out.channels) ||
        !readUnsigned(value, "frames", out.frames)) {
        return false;
    }
    const auto mode = parseMode(value.at("mode"));
    if (!mode || !value.at("passes").is_array() ||
        value.at("passes").size() > kMaxCloudRecordingPassesPerCapture) {
        return false;
    }
    out.status = CloudRecordingCaptureStatus::Ready;
    out.uploadPhase = CloudRecordingUploadPhase::Captured;
    out.semantics.mode = *mode;
    out.semantics.complete = false;
    out.passes.reserve(value.at("passes").size());
    for (const json& encoded : value.at("passes")) {
        CloudRecordingPass pass;
        if (!parsePass(encoded, pass)) return false;
        out.passes.push_back(pass);
    }
    return true;
}

CloudRecordingRecoveryResult parseManifestV1(
    const json& root, CloudRecordingRecoveryManifest& out) {
    static constexpr std::array keys{
        "format", "version", "projectId", "sessionId", "createdAt",
        "recoveryOnly", "lostLease", "captures"};
    if (!exactKeys(root, keys)) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "legacy cloud recording recovery shape is invalid");
    }
    std::string projectId;
    std::string sessionId;
    std::int64_t createdAt = 0;
    bool recoveryOnly = false;
    bool lostLease = false;
    if (!readString(root, "projectId", projectId) ||
        !readString(root, "sessionId", sessionId) ||
        !readSigned(root, "createdAt", createdAt) ||
        !readBool(root, "recoveryOnly", recoveryOnly) ||
        !readBool(root, "lostLease", lostLease) ||
        !root.at("captures").is_array() || root.at("captures").empty() ||
        root.at("captures").size() > kMaxCloudRecordingCaptures) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "legacy cloud recording recovery fields are invalid");
    }

    CloudRecordingRecoveryManifest parsed;
    parsed.version = kCloudRecordingRecoveryVersion;
    parsed.projectId = std::move(projectId);
    parsed.sessionId = std::move(sessionId);
    parsed.createdAtUnixMs = createdAt;
    parsed.runs.reserve(root.at("captures").size());
    std::size_t index = 0;
    for (const json& encoded : root.at("captures")) {
        CloudRecordingCapture capture;
        if (!parseCaptureV1(encoded, capture)) {
            return result(CloudRecordingRecoveryCode::Invalid,
                          "legacy cloud recording recovery capture is invalid");
        }
        const std::string identity =
            parsed.projectId + "|" + parsed.sessionId + "|" +
            std::to_string(parsed.createdAtUnixMs) + "|" +
            std::to_string(index++) + "|" + capture.trackId + "|" +
            capture.leaseId + "|" + capture.localWavPath;
        CloudRecordingRecoveryRun run;
        run.runId = deterministicUuid("legacy-run", identity);
        run.opId = deterministicUuid("legacy-op", identity);
        run.transactionId = deterministicUuid("legacy-transaction", identity);
        run.createdAtUnixMs = parsed.createdAtUnixMs;
        run.recoveryOnly = recoveryOnly;
        run.lostLease = lostLease;
        capture.captureId = deterministicUuid("legacy-capture", identity);
        capture.uploadId = deterministicUuid("legacy-upload", identity);
        capture.assetId = deterministicUuid("legacy-asset", identity);
        run.captures.push_back(std::move(capture));
        parsed.runs.push_back(std::move(run));
    }
    CloudRecordingRecoveryResult validated =
        validateCloudRecordingRecoveryManifest(parsed);
    if (!validated) return validated;
    out = std::move(parsed);
    return result(CloudRecordingRecoveryCode::Ok);
}

CloudRecordingRecoveryResult parseManifestV2(
    const json& root, CloudRecordingRecoveryManifest& out) {
    static constexpr std::array keys{
        "format", "version", "projectId", "sessionId", "createdAt", "runs"};
    if (!exactKeys(root, keys)) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery manifest shape is invalid");
    }
    CloudRecordingRecoveryManifest parsed;
    if (!readUnsigned(root, "version", parsed.version) ||
        !readString(root, "projectId", parsed.projectId) ||
        !readString(root, "sessionId", parsed.sessionId) ||
        !readSigned(root, "createdAt", parsed.createdAtUnixMs) ||
        !root.at("runs").is_array() || root.at("runs").empty() ||
        root.at("runs").size() > kMaxCloudRecordingRuns) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery manifest fields are invalid");
    }
    parsed.runs.reserve(root.at("runs").size());
    for (const json& encoded : root.at("runs")) {
        CloudRecordingRecoveryRun run;
        if (!parseRunV2(encoded, run)) {
            return result(CloudRecordingRecoveryCode::Invalid,
                          "cloud recording recovery run is invalid");
        }
        parsed.runs.push_back(std::move(run));
    }
    CloudRecordingRecoveryResult validated =
        validateCloudRecordingRecoveryManifest(parsed);
    if (!validated) return validated;
    out = std::move(parsed);
    return result(CloudRecordingRecoveryCode::Ok);
}

CloudRecordingRecoveryResult parseManifest(
    const json& root, CloudRecordingRecoveryManifest& out) {
    if (!root.is_object() || !root.contains("format") ||
        !root.at("format").is_string() ||
        root.at("format").get<std::string>() !=
            kCloudRecordingRecoveryFormat) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery manifest shape is invalid");
    }
    std::uint32_t version = 0;
    if (!readUnsigned(root, "version", version)) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery version is invalid");
    }
    if (version == kCloudRecordingRecoveryLegacyVersion)
        return parseManifestV1(root, out);
    if (version == kCloudRecordingRecoveryVersion)
        return parseManifestV2(root, out);
    return result(CloudRecordingRecoveryCode::Invalid,
                  "cloud recording recovery version is unsupported");
}

fs::path nativeManifestPath(std::string_view sessionDirectory) {
    return platform::pathFromUtf8(sessionDirectory) /
           kCloudRecordingRecoveryFile;
}

fs::path nativeCleanupIntentPath(std::string_view sessionDirectory) {
    return platform::pathFromUtf8(sessionDirectory) /
           kCloudRecordingRunCleanupFile;
}

#if defined(_WIN32)
bool writeDurably(const fs::path& path, std::string_view bytes) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - written, std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        if (!::WriteFile(file, bytes.data() + written, wanted, &count,
                         nullptr) || count == 0) {
            ok = false;
            break;
        }
        written += count;
    }
    if (ok) ok = ::FlushFileBuffers(file) != FALSE;
    if (!::CloseHandle(file)) ok = false;
    return ok;
}

bool replaceAtomically(const fs::path& temporary, const fs::path& target) {
    return ::MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING |
                             MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool syncDirectory(const fs::path&) { return true; }
#else
bool writeDurably(const fs::path& path, std::string_view bytes) {
    const int descriptor =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (descriptor < 0) return false;
    std::size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        const ssize_t count =
            ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        written += std::size_t(count);
    }
    if (ok) ok = ::fsync(descriptor) == 0;
    if (::close(descriptor) != 0) ok = false;
    return ok;
}

bool replaceAtomically(const fs::path& temporary, const fs::path& target) {
    return ::rename(temporary.c_str(), target.c_str()) == 0;
}

bool syncDirectory(const fs::path& directory) {
#if defined(O_DIRECTORY)
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
    const int descriptor = ::open(directory.c_str(), O_RDONLY);
#endif
    if (descriptor < 0) return false;
    const bool ok = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return ok;
}
#endif

CloudRecordingRecoveryResult validateSessionDirectory(
    const std::string& sessionDirectory) {
    if (sessionDirectory.empty()) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "recovery session directory is empty");
    }
    const fs::path directory = platform::pathFromUtf8(sessionDirectory);
    std::error_code ec;
    if (!directory.is_absolute() || !fs::is_directory(directory, ec) || ec) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "recovery session directory is unavailable");
    }
    return result(CloudRecordingRecoveryCode::Ok);
}

CloudRecordingRunCleanupResult readCleanupIntent(
    const fs::path& path, RunCleanupIntent& out) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if ((ec == std::errc::no_such_file_or_directory) ||
        (!ec && !fs::exists(status))) {
        return cleanupResult(CloudRecordingRunCleanupCode::AlreadyAbsent);
    }
    if (ec) {
        return cleanupResult(CloudRecordingRunCleanupCode::IoError,
                             "recording cleanup intent could not be inspected");
    }
    if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
        return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                             "recording cleanup intent is not a regular file");
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxCloudRecordingRecoveryBytes) {
        return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                             "recording cleanup intent size is invalid");
    }

    std::string bytes(std::size_t(size), '\0');
    std::ifstream input(path, std::ios::binary);
    if (!input.read(bytes.data(), std::streamsize(bytes.size())) ||
        input.peek() != std::ifstream::traits_type::eof()) {
        return cleanupResult(CloudRecordingRunCleanupCode::IoError,
                             "recording cleanup intent could not be read");
    }
    const json encoded = json::parse(bytes, nullptr, false);
    RunCleanupIntent parsed;
    if (encoded.is_discarded() || !parseCleanupIntent(encoded, parsed)) {
        return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                             "recording cleanup intent is invalid");
    }
    out = std::move(parsed);
    return cleanupResult(CloudRecordingRunCleanupCode::Removed);
}

CloudRecordingRunCleanupResult publishCleanupIntent(
    const fs::path& target, const RunCleanupIntent& intent) {
    std::string bytes;
    try {
        bytes = cleanupIntentToJson(intent).dump();
    } catch (const json::exception&) {
        return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                             "recording cleanup intent could not be encoded");
    }
    if (bytes.empty() || bytes.size() > kMaxCloudRecordingRecoveryBytes) {
        return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                             "recording cleanup intent is too large");
    }

    static std::atomic<std::uint64_t> temporarySequence{0};
    fs::path temporary = target;
    temporary += ".tmp-" + std::to_string(currentProcessId()) + "-" +
                 std::to_string(temporarySequence.fetch_add(1));
    std::error_code ec;
    fs::remove(temporary, ec);
    if (!writeDurably(temporary, bytes)) {
        fs::remove(temporary, ec);
        return cleanupResult(CloudRecordingRunCleanupCode::IoError,
                             "recording cleanup intent could not be written");
    }
    if (!replaceAtomically(temporary, target)) {
        fs::remove(temporary, ec);
        return cleanupResult(CloudRecordingRunCleanupCode::IoError,
                             "recording cleanup intent could not be published");
    }
    if (!syncDirectory(target.parent_path())) {
        return cleanupResult(
            CloudRecordingRunCleanupCode::IoError,
            "recording cleanup intent could not be synchronized");
    }
    return cleanupResult(CloudRecordingRunCleanupCode::Removed);
}

CloudRecordingRunCleanupResult deleteIntentWavs(
    const fs::path& intentPath, const RunCleanupIntent& intent) {
    std::set<fs::path> parentDirectories;
    for (const std::string& rawPath : intent.wavPaths) {
        if (!validWavPathShape(rawPath)) {
            return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                                 "recording cleanup WAV path is invalid");
        }
        const fs::path path = platform::pathFromUtf8(rawPath);
        parentDirectories.insert(path.parent_path());
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory ||
            (!ec && !fs::exists(status))) {
            continue;
        }
        if (ec) {
            return cleanupResult(CloudRecordingRunCleanupCode::IoError,
                                 "recording cleanup WAV could not be inspected");
        }
        if (fs::is_symlink(status) || !fs::is_regular_file(status)) {
            return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                                 "recording cleanup WAV identity changed");
        }
        if (!fs::remove(path, ec) || ec) {
            return cleanupResult(CloudRecordingRunCleanupCode::IoError,
                                 "recording cleanup WAV could not be removed");
        }
    }

    // Sync even directories whose WAV was already absent: a prior process may
    // have stopped after unlinking it but before making that unlink durable.
    for (const fs::path& directory : parentDirectories) {
        if (!syncDirectory(directory)) {
            return cleanupResult(
                CloudRecordingRunCleanupCode::IoError,
                "recording cleanup WAV removal could not be synchronized");
        }
    }

    std::error_code ec;
    const bool removed = fs::remove(intentPath, ec);
    if (ec) {
        return cleanupResult(CloudRecordingRunCleanupCode::IoError,
                             "recording cleanup intent could not be removed");
    }
    if (removed && !syncDirectory(intentPath.parent_path())) {
        return cleanupResult(
            CloudRecordingRunCleanupCode::IoError,
            "recording cleanup completion could not be synchronized");
    }
    return cleanupResult(CloudRecordingRunCleanupCode::Removed);
}

} // namespace

CloudRecordingCaptureStatus classifyCloudRecordingCaptureStatus(
    bool fileWriteSucceeded,
    std::uint64_t capturedFrames,
    std::uint64_t writtenFrames,
    std::uint64_t droppedFrames,
    bool audioReadable,
    std::uint64_t readableFrames) noexcept {
    // Treat impossible over-reporting as a writer failure too. In normal
    // operation writtenFrames can only trail capturedFrames, never exceed it.
    if (!fileWriteSucceeded || droppedFrames > 0 ||
        writtenFrames != capturedFrames) {
        return CloudRecordingCaptureStatus::WriteFailed;
    }
    if (capturedFrames == 0 && readableFrames == 0)
        return CloudRecordingCaptureStatus::ZeroFrames;
    if (audioReadable && readableFrames > 0)
        return CloudRecordingCaptureStatus::Ready;
    return CloudRecordingCaptureStatus::Unreadable;
}

CloudRecordingRecoveryResult validateCloudRecordingRecoveryManifest(
    const CloudRecordingRecoveryManifest& manifest) {
    if (manifest.version != kCloudRecordingRecoveryVersion) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery version is unsupported");
    }
    if (!canonicalUuid(manifest.projectId) ||
        !canonicalUuid(manifest.sessionId)) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery identifiers are invalid");
    }
    if (manifest.createdAtUnixMs <= 0 || manifest.runs.empty() ||
        manifest.runs.size() > kMaxCloudRecordingRuns) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery bounds are invalid");
    }

    std::set<std::string> runIds;
    std::set<std::string> opIds;
    std::set<std::string> transactionIds;
    std::set<std::string> captureIds;
    std::set<std::string> leaseIds;
    std::set<std::string> uploadIds;
    std::set<std::string> assetIds;
    std::set<std::string> localWavPaths;
    std::size_t totalCaptures = 0;
    for (const CloudRecordingRecoveryRun& run : manifest.runs) {
        if (!canonicalUuid(run.runId) || !canonicalUuid(run.opId) ||
            !canonicalUuid(run.transactionId) ||
            !runIds.insert(run.runId).second ||
            !opIds.insert(run.opId).second ||
            !transactionIds.insert(run.transactionId).second ||
            run.createdAtUnixMs <= 0 ||
            run.commitPhase != CloudRecordingCommitPhase::Pending ||
            run.captures.empty() ||
            run.captures.size() > kMaxCloudRecordingCaptures) {
            return result(CloudRecordingRecoveryCode::Invalid,
                          "cloud recording recovery run is invalid");
        }
        totalCaptures += run.captures.size();
        if (totalCaptures > kMaxCloudRecordingCaptures) {
            return result(CloudRecordingRecoveryCode::Invalid,
                          "cloud recording recovery has too many captures");
        }

        for (const CloudRecordingCapture& capture : run.captures) {
            const bool knownStatus =
                capture.status == CloudRecordingCaptureStatus::Ready ||
                capture.status == CloudRecordingCaptureStatus::ZeroFrames ||
                capture.status == CloudRecordingCaptureStatus::Unreadable ||
                capture.status == CloudRecordingCaptureStatus::WriteFailed;
            if (!canonicalUuid(capture.captureId) ||
                !canonicalUuid(capture.trackId) ||
                !canonicalUuid(capture.leaseId) ||
                !canonicalUuid(capture.uploadId) ||
                !canonicalUuid(capture.assetId) ||
                !captureIds.insert(capture.captureId).second ||
                !leaseIds.insert(capture.leaseId).second ||
                !uploadIds.insert(capture.uploadId).second ||
                !assetIds.insert(capture.assetId).second ||
                !localWavPaths.insert(capture.localWavPath).second ||
                capture.uploadPhase != CloudRecordingUploadPhase::Captured ||
                !knownStatus ||
                !wavPathExists(
                    capture.localWavPath,
                    capture.status == CloudRecordingCaptureStatus::Ready)) {
                return result(
                    CloudRecordingRecoveryCode::Invalid,
                    "cloud recording recovery capture identity is invalid");
            }

            const CloudRecordingSemantics& semantics = capture.semantics;
            const bool modeKnown =
                semantics.mode == CloudRecordingMode::Overwrite ||
                semantics.mode == CloudRecordingMode::Layers;
            if (!modeKnown ||
                !boundedFinite(semantics.loopStartSeconds, 0.0,
                               kMaximumTimeSeconds) ||
                !boundedFinite(semantics.loopEndSeconds, 0.0,
                               kMaximumTimeSeconds) ||
                !boundedFinite(semantics.compCrossfadeMs, 0.0, 20.0) ||
                (semantics.complete && semantics.loopEnabled &&
                 semantics.loopEndSeconds <= semantics.loopStartSeconds) ||
                (!semantics.complete &&
                 (semantics.loopEnabled ||
                  semantics.loopStartSeconds != 0.0 ||
                  semantics.loopEndSeconds != 0.0 ||
                  !semantics.loopCreatesTakes ||
                  !semantics.trimTakesToRegion ||
                  semantics.autoExpandAfterRecord ||
                  semantics.compCrossfadeMs != 5.0))) {
                return result(CloudRecordingRecoveryCode::Invalid,
                              "cloud recording recovery semantics are invalid");
            }

            if (!boundedFinite(capture.startSeconds, 0.0,
                               kMaximumTimeSeconds) ||
                !boundedFinite(capture.durationSeconds, 0.0,
                               kMaximumTimeSeconds) ||
                (capture.sampleRate != 0.0 &&
                 !boundedFinite(capture.sampleRate, 0.0,
                                kMaximumSampleRate, true)) ||
                capture.channels > kMaximumChannels ||
                capture.frames > std::uint64_t(kMaximumSampleRate *
                                               kMaximumTimeSeconds) ||
                capture.passes.size() >
                    kMaxCloudRecordingPassesPerCapture) {
                return result(CloudRecordingRecoveryCode::Invalid,
                              "cloud recording recovery capture bounds are invalid");
            }

            double tolerance = 0.001;
            if (capture.sampleRate > 0.0)
                tolerance = std::max(tolerance, 2.0 / capture.sampleRate);
            if (capture.status == CloudRecordingCaptureStatus::Ready) {
                if (capture.frames == 0 || capture.sampleRate <= 0.0 ||
                    capture.channels == 0 || capture.durationSeconds <= 0.0 ||
                    std::abs(double(capture.frames) / capture.sampleRate -
                             capture.durationSeconds) > tolerance) {
                    return result(
                        CloudRecordingRecoveryCode::Invalid,
                        "ready cloud recording metadata is inconsistent");
                }
            } else if (capture.status ==
                       CloudRecordingCaptureStatus::ZeroFrames) {
                if (capture.frames != 0 || capture.durationSeconds != 0.0 ||
                    !capture.passes.empty()) {
                    return result(
                        CloudRecordingRecoveryCode::Invalid,
                        "zero-frame cloud recording metadata is inconsistent");
                }
            }

            double previousCaptureEnd = -1.0;
            for (const CloudRecordingPass& pass : capture.passes) {
                if (!boundedFinite(pass.startSeconds, 0.0,
                                   kMaximumTimeSeconds) ||
                    !boundedFinite(pass.endSeconds, pass.startSeconds,
                                   kMaximumTimeSeconds, true) ||
                    !boundedFinite(pass.captureOffsetSeconds, 0.0,
                                   capture.durationSeconds) ||
                    pass.captureOffsetSeconds +
                            (pass.endSeconds - pass.startSeconds) >
                        capture.durationSeconds + tolerance ||
                    pass.captureOffsetSeconds + tolerance <
                        previousCaptureEnd) {
                    return result(CloudRecordingRecoveryCode::Invalid,
                                  "cloud recording recovery pass is invalid");
                }
                previousCaptureEnd =
                    pass.captureOffsetSeconds +
                    (pass.endSeconds - pass.startSeconds);
            }
            if (capture.status == CloudRecordingCaptureStatus::Unreadable &&
                !capture.passes.empty()) {
                return result(CloudRecordingRecoveryCode::Invalid,
                              "unreadable cloud recording cannot have passes");
            }
        }
    }
    return result(CloudRecordingRecoveryCode::Ok);
}

CloudRecordingRecoveryStore::CloudRecordingRecoveryStore(
    std::string sessionDirectory)
    : m_sessionDirectory(std::move(sessionDirectory)) {}

std::string CloudRecordingRecoveryStore::manifestPath() const {
    return platform::pathToUtf8(nativeManifestPath(m_sessionDirectory));
}

CloudRecordingRecoveryResult CloudRecordingRecoveryStore::write(
    const CloudRecordingRecoveryManifest& manifest) const {
    const std::lock_guard<std::recursive_mutex> lock(recoveryStoreMutex);
    CloudRecordingRecoveryResult directory =
        validateSessionDirectory(m_sessionDirectory);
    if (!directory) return directory;
    CloudRecordingRecoveryResult validated =
        validateCloudRecordingRecoveryManifest(manifest);
    if (!validated) return validated;

    std::string bytes;
    try {
        bytes = manifestToJson(manifest).dump();
    } catch (const json::exception&) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery could not be encoded");
    }
    if (bytes.empty() || bytes.size() > kMaxCloudRecordingRecoveryBytes) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery manifest is too large");
    }

    const fs::path target = nativeManifestPath(m_sessionDirectory);
    static std::atomic<std::uint64_t> temporarySequence{0};
    fs::path temporary = target;
    temporary += ".tmp-" + std::to_string(currentProcessId()) + "-" +
                 std::to_string(temporarySequence.fetch_add(1));
    std::error_code ec;
    fs::remove(temporary, ec);
    if (!writeDurably(temporary, bytes)) {
        fs::remove(temporary, ec);
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be written");
    }
    if (!replaceAtomically(temporary, target)) {
        fs::remove(temporary, ec);
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be published");
    }
    if (!syncDirectory(target.parent_path())) {
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be synchronized");
    }
    return result(CloudRecordingRecoveryCode::Ok);
}

CloudRecordingRecoveryResult CloudRecordingRecoveryStore::read(
    CloudRecordingRecoveryManifest& out) const {
    const std::lock_guard<std::recursive_mutex> lock(recoveryStoreMutex);
    CloudRecordingRecoveryResult directory =
        validateSessionDirectory(m_sessionDirectory);
    if (!directory) return directory;
    const fs::path path = nativeManifestPath(m_sessionDirectory);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return result(CloudRecordingRecoveryCode::NotFound,
                      "cloud recording recovery manifest was not found");
    if (!fs::is_regular_file(path, ec) || ec) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery manifest is not a file");
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxCloudRecordingRecoveryBytes) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery manifest size is invalid");
    }

    std::string bytes(std::size_t(size), '\0');
#if defined(_WIN32)
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be read");
    DWORD count = 0;
    const bool readOk =
        ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                   &count, nullptr) != FALSE && count == bytes.size();
    ::CloseHandle(file);
    if (!readOk)
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be read");
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0)
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be read");
    std::size_t offset = 0;
    bool readOk = true;
    while (offset < bytes.size()) {
        const ssize_t count =
            ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            readOk = false;
            break;
        }
        offset += std::size_t(count);
    }
    ::close(descriptor);
    if (!readOk)
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be read");
#endif

    const json root = json::parse(bytes, nullptr, false);
    if (root.is_discarded()) {
        return result(CloudRecordingRecoveryCode::Invalid,
                      "cloud recording recovery JSON is invalid");
    }
    return parseManifest(root, out);
}

bool CloudRecordingRecoveryStore::exists() const {
    const std::lock_guard<std::recursive_mutex> lock(recoveryStoreMutex);
    const auto present = [](const fs::path& path) {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        return !ec && fs::exists(status);
    };
    return present(nativeManifestPath(m_sessionDirectory)) ||
           present(nativeCleanupIntentPath(m_sessionDirectory));
}

CloudRecordingRecoveryResult
CloudRecordingRecoveryStore::removeAfterCommit() const {
    const std::lock_guard<std::recursive_mutex> lock(recoveryStoreMutex);
    CloudRecordingRecoveryResult directory =
        validateSessionDirectory(m_sessionDirectory);
    if (!directory) return directory;
    const fs::path path = nativeManifestPath(m_sessionDirectory);
    std::error_code ec;
    const bool removed = fs::remove(path, ec);
    if (ec) {
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery could not be removed");
    }
    if (removed && !syncDirectory(path.parent_path())) {
        return result(CloudRecordingRecoveryCode::IoError,
                      "cloud recording recovery removal could not be synchronized");
    }
    return result(CloudRecordingRecoveryCode::Ok);
}

CloudRecordingRunCleanupResult
CloudRecordingRecoveryStore::removeRunAfterCommit(
    std::string_view runId) const {
    const std::lock_guard<std::recursive_mutex> lock(recoveryStoreMutex);
    if (!canonicalUuid(runId)) {
        return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                             "recording cleanup run id is invalid");
    }
    const CloudRecordingRecoveryResult directory =
        validateSessionDirectory(m_sessionDirectory);
    if (!directory) {
        return cleanupResult(CloudRecordingRunCleanupCode::Invalid,
                             directory.message);
    }

    const fs::path manifestPath = nativeManifestPath(m_sessionDirectory);
    const fs::path intentPath = nativeCleanupIntentPath(m_sessionDirectory);
    const std::string requestedRunId(runId);

    const auto mapFailure = [](const CloudRecordingRecoveryResult& failure) {
        return cleanupResult(
            failure.code == CloudRecordingRecoveryCode::Invalid
                ? CloudRecordingRunCleanupCode::Invalid
                : CloudRecordingRunCleanupCode::IoError,
            failure.message);
    };

    const auto finishIntent = [&](const RunCleanupIntent& intent) {
        CloudRecordingRecoveryManifest current;
        const CloudRecordingRecoveryResult loaded = read(current);
        const bool manifestExists = loaded.ok();
        if (!manifestExists &&
            loaded.code != CloudRecordingRecoveryCode::NotFound) {
            return mapFailure(loaded);
        }

        if (manifestExists &&
            (current.projectId != intent.projectId ||
             current.sessionId != intent.sessionId)) {
            return cleanupResult(
                CloudRecordingRunCleanupCode::Invalid,
                "recording cleanup intent belongs to another manifest");
        }

        if (manifestExists) {
            const auto target = std::find_if(
                current.runs.begin(), current.runs.end(),
                [&intent](const CloudRecordingRecoveryRun& run) {
                    return run.runId == intent.runId;
                });
            if (target != current.runs.end()) {
                std::vector<std::string> manifestPaths;
                manifestPaths.reserve(target->captures.size());
                for (const CloudRecordingCapture& capture :
                     target->captures) {
                    manifestPaths.push_back(capture.localWavPath);
                }
                if (manifestPaths != intent.wavPaths) {
                    return cleanupResult(
                        CloudRecordingRunCleanupCode::Invalid,
                        "recording cleanup intent no longer matches its run");
                }

                current.runs.erase(target);
                CloudRecordingRecoveryResult published;
                if (current.runs.empty()) {
                    published = removeAfterCommit();
                } else {
                    published = write(current);
                }
                if (!published) return mapFailure(published);
            } else if (!syncDirectory(manifestPath.parent_path())) {
                // The replacement may have been renamed just before a crash.
                // Seal that already-visible generation before any WAV unlink.
                return cleanupResult(
                    CloudRecordingRunCleanupCode::IoError,
                    "recording cleanup manifest could not be synchronized");
            }

            const std::set<std::string> retiredPaths(
                intent.wavPaths.begin(), intent.wavPaths.end());
            for (const CloudRecordingRecoveryRun& remainingRun :
                 current.runs) {
                for (const CloudRecordingCapture& remainingCapture :
                     remainingRun.captures) {
                    if (retiredPaths.contains(
                            remainingCapture.localWavPath)) {
                        return cleanupResult(
                            CloudRecordingRunCleanupCode::Invalid,
                            "recording cleanup WAV is referenced by a live run");
                    }
                }
            }
        } else if (!syncDirectory(manifestPath.parent_path())) {
            // The last-run sidecar removal has the same crash window as an
            // atomic manifest replacement. Absence becomes authoritative only
            // after its parent directory has been synchronized.
            return cleanupResult(
                CloudRecordingRunCleanupCode::IoError,
                "recording cleanup sidecar removal could not be synchronized");
        }

        return deleteIntentWavs(intentPath, intent);
    };

    RunCleanupIntent pending;
    const CloudRecordingRunCleanupResult pendingState =
        readCleanupIntent(intentPath, pending);
    if (pendingState.code == CloudRecordingRunCleanupCode::Invalid ||
        pendingState.code == CloudRecordingRunCleanupCode::IoError) {
        return pendingState;
    }
    if (pendingState.removed()) {
        const bool requestedIntent = pending.runId == requestedRunId;
        const CloudRecordingRunCleanupResult resumed = finishIntent(pending);
        if (!resumed) return resumed;
        if (requestedIntent) return resumed;
    }

    CloudRecordingRecoveryManifest current;
    const CloudRecordingRecoveryResult loaded = read(current);
    if (loaded.code == CloudRecordingRecoveryCode::NotFound) {
        return cleanupResult(CloudRecordingRunCleanupCode::AlreadyAbsent);
    }
    if (!loaded) return mapFailure(loaded);

    const auto target = std::find_if(
        current.runs.begin(), current.runs.end(),
        [&requestedRunId](const CloudRecordingRecoveryRun& run) {
            return run.runId == requestedRunId;
        });
    if (target == current.runs.end()) {
        return cleanupResult(CloudRecordingRunCleanupCode::AlreadyAbsent);
    }

    RunCleanupIntent intent;
    intent.projectId = current.projectId;
    intent.sessionId = current.sessionId;
    intent.runId = target->runId;
    intent.wavPaths.reserve(target->captures.size());
    for (const CloudRecordingCapture& capture : target->captures)
        intent.wavPaths.push_back(capture.localWavPath);

    const CloudRecordingRunCleanupResult publishedIntent =
        publishCleanupIntent(intentPath, intent);
    if (!publishedIntent) return publishedIntent;
    return finishIntent(intent);
}

} // namespace daw::recovery
