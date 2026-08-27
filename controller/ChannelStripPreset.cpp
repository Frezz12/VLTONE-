#include "ChannelStripPreset.hpp"

#include "serialization/InsertJson.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace daw {
namespace {

audio::Result writeAtomically(const std::vector<std::uint8_t>& bytes,
                              const fs::path& file) {
    std::error_code ec;
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            return audio::Result::fail(audio::EngineError::FileWriteError,
                                       "cannot create preset folder: " +
                                           ec.message());
        }
    }

    fs::path temporary = file;
    temporary += ".tmp-" + newUuid();
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot open preset for writing");
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 std::streamsize(bytes.size()));
    stream.flush();
    if (!stream.good()) {
        stream.close();
        fs::remove(temporary, ec);
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "failed to write preset");
    }
    stream.close();

    fs::rename(temporary, file, ec);
    if (ec) {
        std::error_code removeError;
        fs::remove(file, removeError);
        ec.clear();
        fs::rename(temporary, file, ec);
    }
    if (ec) {
        const std::string reason = ec.message();
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot publish preset: " + reason);
    }
    return audio::Result::ok();
}

void makePortable(InsertModel& model) {
    // These fields address the project the preset came from, not the sound of
    // the plugin. The opaque chunks travel inline below instead of pointing at
    // a project's state directory.
    model.stateFile.clear();
    model.rightStateFile.clear();
    model.sidechainTrackId.clear();
    model.windowX = model.windowY = model.windowWidth = model.windowHeight = 0;
    model.windowOpen = false;
}

} // namespace

audio::Result ChannelStripPreset::save(
    const EngineController::ChannelSnapshot& snapshot,
    const std::string& filePath) {
    if (!snapshot.hasSettings) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "channel snapshot has no volume or pan");
    }

    json plugins = json::array();
    for (const EngineController::ChainSlotSnapshot& source : snapshot.inserts) {
        InsertModel model = source.model;
        makePortable(model);
        json slot;
        slot["plugin"] = serialization::insertToJson(model);
        if (!source.state.empty()) slot["state"] = json::binary(source.state);
        if (!source.rightState.empty())
            slot["rightState"] = json::binary(source.rightState);
        plugins.push_back(std::move(slot));
    }

    json root{
        {"format", kFormat},
        {"version", kFormatVersion},
        {"name", snapshot.sourceName},
        {"volume", snapshot.volume},
        {"pan", snapshot.pan},
        {"plugins", std::move(plugins)},
    };
    return writeAtomically(json::to_cbor(root), platform::pathFromUtf8(filePath));
}

audio::Result ChannelStripPreset::load(EngineController::ChannelSnapshot& out,
                                       const std::string& filePath) {
    std::ifstream stream(platform::pathFromUtf8(filePath), std::ios::binary);
    if (!stream) {
        return audio::Result::fail(audio::EngineError::FileNotFound,
                                   "preset file was not found");
    }
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   "preset file is empty");
    }

    try {
        const json root = json::from_cbor(bytes);
        if (!root.is_object() || root.value("format", std::string()) != kFormat) {
            return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                       "not a VLTS channel-strip preset");
        }
        const int version = root.value("version", 0);
        if (version < 1 || version > kFormatVersion) {
            return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                       "unsupported VLTS version");
        }
        if (!root.contains("plugins") || !root.at("plugins").is_array()) {
            return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                       "VLTS preset has no plugin list");
        }

        EngineController::ChannelSnapshot loaded;
        loaded.sourceName = root.value("name", std::string());
        loaded.hasSettings = true;
        loaded.volume = root.value("volume", 1.0f);
        loaded.pan = root.value("pan", 0.0f);
        for (const json& entry : root.at("plugins")) {
            if (!entry.is_object() || !entry.contains("plugin") ||
                !entry.at("plugin").is_object()) {
                return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                           "VLTS preset contains a bad plugin");
            }
            EngineController::ChainSlotSnapshot slot;
            slot.model = serialization::insertFromJson(entry.at("plugin"));
            makePortable(slot.model);
            if (entry.contains("state") && entry.at("state").is_binary()) {
                const auto& state = entry.at("state").get_binary();
                slot.state.assign(state.begin(), state.end());
            }
            if (entry.contains("rightState") &&
                entry.at("rightState").is_binary()) {
                const auto& state = entry.at("rightState").get_binary();
                slot.rightState.assign(state.begin(), state.end());
            }
            loaded.inserts.push_back(std::move(slot));
        }
        out = std::move(loaded);
        return audio::Result::ok();
    } catch (const json::exception& error) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   std::string("cannot read VLTS preset: ") +
                                       error.what());
    }
}

} // namespace daw
