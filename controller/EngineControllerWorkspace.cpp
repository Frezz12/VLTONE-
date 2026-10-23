#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "platform/PathUtils.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include "model/ClipLibrary.hpp"
#include <nlohmann/json.hpp>

namespace daw {

audio::Result EngineController::prepareProjectTransferResources(recovery::RecoverySnapshot& snapshot,
    const ProjectTransferResources& resources, bool copyMedia) {
    const auto package = platform::pathToUtf8(resources.root);
    std::unordered_map<std::string, std::string> media;
    auto result = audio::Result::ok();
    const auto ownFile = [&](std::string& path) {
        if (path.empty() || !result) return;
        if (const auto found = media.find(path); found != media.end()) { path = found->second; return; }
        const auto source = path;
        if (copyMedia) {
            std::string name;
            result = ProjectSerializer::copyContentFile(source, package, name);
            if (result) path = platform::pathToUtf8(platform::pathFromUtf8(ProjectSerializer::mediaPath(package)) /
                platform::pathFromUtf8(name));
        } else {
            std::error_code error;
            if (!std::filesystem::is_regular_file(platform::pathFromUtf8(path), error) || error)
                result = audio::Result::fail(audio::EngineError::FileNotFound, "Clipboard media is unavailable: " + path);
        }
        if (result) media.emplace(source, path);
    };
    const auto audioEdit = [&](AudioEditDocument& edit) { for (auto& source : edit.sources) ownFile(source.filePath); };
    std::unordered_set<std::string> requiredStates, samplerStates;
    const auto slot = [&](InsertModel& device) {
        audioEdit(device.audioEdit);
        for (const auto* name : {&device.stateFile, &device.rightStateFile}) if (!name->empty()) {
            requiredStates.insert(*name);
            if (device.uid == "daw.sampler" || device.uid == "daw.slicer") samplerStates.insert(*name);
        }
    };
    for (auto& track : snapshot.project.tracks) {
        visitStoredPlugins(track, slot);
        ownFile(track.freeze.filePath);
        for (auto& clip : track.clips) {
            ownFile(clip.filePath); audioEdit(clip.audioEdit);
            ownFile(clip.offlineProcess.renderedFilePath);
            for (auto& device : clip.offlineProcess.chain) slot(device);
            for (auto& take : clip.takes) ownFile(take.filePath);
            for (auto& version : clip.offlineHistory) {
                ownFile(version.source.filePath); audioEdit(version.source.audioEdit);
                for (auto& take : version.source.takes) ownFile(take.filePath);
            }
        }
    }
    for (auto& state : snapshot.pluginStates) {
        const auto name = platform::pathFromUtf8(state.fileName);
        if (name.empty() || name.filename() != name || state.bytes.empty() || state.bytes.size() > plugins::kMaxPluginStateBytes)
            return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid clipboard plugin state.");
        if (samplerStates.contains(state.fileName)) {
            auto value = nlohmann::json::parse(state.bytes.begin(), state.bytes.end(), nullptr, false);
            if (value.is_object() && value.value("sample", nlohmann::json{}).is_string()) {
                auto sample = value["sample"].get<std::string>(); ownFile(sample);
                if (copyMedia) {
                    value["sample"] = sample;
                    const auto bytes = value.dump(); state.bytes.assign(bytes.begin(), bytes.end());
                }
            }
        }
        if (!result) return result;
        requiredStates.erase(state.fileName);
        if (copyMedia) {
            const auto directory = resources.root / "State";
            std::error_code error; std::filesystem::create_directories(directory, error);
            if (error) return audio::Result::fail(audio::EngineError::FileWriteError, error.message());
            std::ofstream stream(directory / name, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(state.bytes.data()), std::streamsize(state.bytes.size()));
            stream.close();
            if (!stream) return audio::Result::fail(audio::EngineError::FileWriteError, "Cannot retain clipboard plugin state.");
        }
    }
    if (!requiredStates.empty()) return audio::Result::fail(audio::EngineError::FileNotFound, "Clipboard plugin state is unavailable.");
    return result;
}

audio::Result EngineController::copyProjectRegion(
    const std::vector<std::pair<std::string, std::string>>& clips,
    double fromSeconds, double toSeconds, ProjectTransferBundle& out) {
    if (!std::isfinite(fromSeconds) || !std::isfinite(toSeconds) || toSeconds <= fromSeconds)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Select a nonempty time range.");
    ProjectTransferBundle full;
    if (const auto copied = copyProjectObjects({}, clips, full); !copied) return copied;
    // Splitting a parked copy leaves the source and its history untouched if
    // media/state capture fails. Only a complete bundle authorizes Cut.
    EngineController draft;
    draft.useWorkspacePluginManager(pluginManager());
    draft.initializeDocumentOnly(m_sampleRate, m_bufferSize);
    if (const auto opened = draft.restoreRecoveryProject(full.snapshot.project, platform::pathToUtf8(full.resources->root)); !opened) return opened;
    std::vector<std::pair<std::string, std::string>> pieces;
    for (const auto& [trackId, clipId] : clips) {
        const auto* track = draft.project().findTrack(trackId);
        if (!track) continue;
        const auto clip = std::find_if(track->clips.begin(), track->clips.end(), [&](const auto& c) { return c.id == clipId; });
        if (clip == track->clips.end()) continue;
        const double start = clip->startSeconds, end = start + clip->durationSeconds;
        if (end <= fromSeconds || start >= toSeconds) continue;
        auto inner = clipId;
        if (start < fromSeconds) inner = draft.splitClip(trackId, inner, fromSeconds);
        if (inner.empty()) continue;
        if (end > toSeconds) draft.splitClip(trackId, inner, toSeconds);
        pieces.emplace_back(trackId, inner);
    }
    ProjectTransferBundle sliced;
    if (const auto copied = draft.copyProjectObjects({}, pieces, sliced); !copied) return copied;
    sliced.region = true; sliced.sourceDocument = m_documentIdentity; sliced.originSeconds = fromSeconds;
    for (const auto& track : sliced.snapshot.project.tracks) for (const auto& clip : track.clips) {
        if (clip.kind != ClipKind::Automation) continue;
        for (const auto& originalTrack : full.snapshot.project.tracks) for (const auto& original : originalTrack.clips) {
            if (original.kind != ClipKind::Automation || original.automation.target != clip.automation.target) continue;
            if (const auto identity = full.automationPlugins.find(original.id); identity != full.automationPlugins.end())
                sliced.automationPlugins[clip.id] = identity->second;
        }
    }
    out = std::move(sliced);
    return audio::Result::ok();
}

EngineController::ChannelSnapshot& EngineController::sharedChannelClipboard() {
    static ChannelSnapshot clipboard;
    return clipboard;
}
const EngineController::ChannelSnapshot& EngineController::channelClipboard() const { return sharedChannelClipboard(); }

bool EngineController::setChannelClipboard(ChannelSnapshot snapshot) {
    if (!ownChannelTransferResources(snapshot)) return false;
    sharedChannelClipboard() = std::move(snapshot);
    return true;
}

bool EngineController::ownChannelTransferResources(ChannelSnapshot& snapshot) {
    if (snapshot.empty()) return false;
    if (snapshot.sourceDocument.empty()) snapshot.sourceDocument = m_documentIdentity;
    auto resources = std::make_shared<ProjectTransferResources>(std::filesystem::temp_directory_path() / ("vlt-channel-" + newUuid()));
    const auto root = platform::pathToUtf8(resources->root);
    auto result = audio::Result::ok();
    const auto copyFile = [&](std::string& source) {
        if (source.empty() || !result) return;
        std::string name;
        result = ProjectSerializer::copyContentFile(source, root, name);
        if (result) source = platform::pathToUtf8(platform::pathFromUtf8(ProjectSerializer::mediaPath(root)) / platform::pathFromUtf8(name));
    };
    const auto copySlot = [&](ChainSlotSnapshot& slot) {
        for (auto& source : slot.model.audioEdit.sources) copyFile(source.filePath);
        if (slot.model.uid != "daw.sampler" && slot.model.uid != "daw.slicer") return;
        copyFile(slot.sourcePath); copyFile(slot.rightSourcePath);
        for (auto* bytes : {&slot.state, &slot.rightState}) {
            auto state = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
            if (!state.is_object() || !state.value("sample", nlohmann::json{}).is_string()) continue;
            auto sample = state["sample"].get<std::string>(); copyFile(sample);
            state["sample"] = sample;
            const auto text = state.dump(); bytes->assign(text.begin(), text.end());
        }
    };
    if (snapshot.instrument) copySlot(*snapshot.instrument);
    if (snapshot.channelColor) copySlot(*snapshot.channelColor);
    for (auto& slot : snapshot.inserts) copySlot(slot);
    for (auto& slot : snapshot.instrumentFx) copySlot(slot);
    for (auto& slot : snapshot.miniModules) copySlot(slot);
    if (!result) { reportAudioOperationFailure("Copy channel", result); return false; }
    snapshot.resources = std::move(resources);
    return true;
}

EngineController::ChannelSnapshot EngineController::prepareChannelTransfer(const ChannelSnapshot& source) const {
    auto result = source;
    result.sourceDocument = m_documentIdentity;
    result.rackCutSource.clear(); result.outputBusId.clear(); result.sends.clear();
    for (auto& source : result.modulation.sources) if (!source.audioChannelId.empty()) {
        source.enabled = false; source.audioChannelId.clear();
    }
    const auto clear = [](ChainSlotSnapshot& slot) { slot.model.sidechainTrackIds.clear(); };
    if (result.instrument) clear(*result.instrument);
    if (result.channelColor) clear(*result.channelColor);
    for (auto& slot : result.inserts) clear(slot);
    for (auto& slot : result.instrumentFx) clear(slot);
    for (auto& slot : result.miniModules) clear(slot);
    return result;
}

audio::Result EngineController::initializeDocumentOnly(double rate, std::uint32_t frames) {
    if (!std::isfinite(rate) || rate < 1000 || rate > 768000 ||
        !frames || frames > engine::kMaxBlockSize)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid audio format.");
    if (m_prepared) {
        if (const auto result = deactivateAudio(); !result) return result;
    }
    m_documentOnly = true;
    m_sampleRate = rate;
    m_bufferSize = frames;
    m_project.sampleRate = rate;
    m_recordDir = platform::pathToUtf8(std::filesystem::temp_directory_path());
    if (!m_workspacePlugins) m_pluginManager.load();
    return audio::Result::ok();
}

audio::Result EngineController::deactivateAudio() {
    if (m_documentOnly) return audio::Result::ok();
    if (m_exportInProgress)
        return audio::Result::fail(audio::EngineError::NotSupported, "Finish rendering before switching the audio engine.");
    cancelCountIn();
    if (isRecording()) {
        stopRecording();
        if (!m_recordingWarning.empty())
            return audio::Result::fail(audio::EngineError::FileWriteError, m_recordingWarning);
    }
    stopPluginAudition();
    stopPreview();
    m_documentPosition = positionSeconds();
    pause();
    resetMidiInput();
    auto saved = prepareProjectSave();
    if (!saved.captureError.empty())
        return audio::Result::fail(audio::EngineError::PluginLoadFailed, saved.captureError);
    // All native views must be gone before releasing their processors. A
    // failed state capture above deliberately leaves the old processors alive.
    announceAllRetiring();
    if (const auto detached = m_runtime->detachDeviceCallback(); !detached) return detached;
    m_runtime->closeDevice();
    cancelClipSampleBake();
    m_project = std::move(saved.project);
    m_runtime.reset();
    m_samples.clear();
    m_sourceSamples.clear();
    m_clipSampleCache.clear();
    m_sharedClipSampleCache.clear();
    m_audioEditAudio.clear();
    m_deferredClipSync.clear();
    m_liveDeviceAllowed = m_deviceOpen = m_prepared = false;
    m_documentOnly = true;
    return audio::Result::ok();
}

audio::Result EngineController::activateAudio(const audio::AudioDeviceConfig& config,
                                             bool openDevice) {
    if (audioEnabled()) return audio::Result::ok();
    const auto fail = [&](audio::Result result) {
        m_runtime->closeDevice();
        m_runtime.reset();
        m_samples.clear();
        m_sourceSamples.clear();
        m_clipSampleCache.clear();
        m_sharedClipSampleCache.clear();
        m_audioEditAudio.clear();
        m_deferredClipSync.clear();
        m_liveDeviceAllowed = m_deviceOpen = m_prepared = false;
        m_documentOnly = true;
        return result;
    };
    try {
        if (const auto ready = m_runtime->prepare(config.sampleRate, config.bufferSize, m_isRenderClone); !ready)
            return fail(ready);
        m_sampleRate = config.sampleRate;
        m_bufferSize = config.bufferSize;
        m_documentOnly = false;
        auto session = prepareAudioSession(AudioPluginLoadPolicy::Required);
        PreparedPluginStateEdits states;
        if (const auto captured = collectProjectPluginStateEdits(states, session,
                m_documentPackageDir, nullptr, true); !captured) return fail(captured);
        if (const auto built = m_runtime.applySession(std::move(session), false, states.imports); !built)
            return fail(built);
        acceptPluginStateEdits(states.imports, m_runtime.lastImportedPluginStates());
        acceptPluginStateEdits(states.retained, {});
        m_prepared = true;
        using Action = AudioTransportCommand::Action;
        m_runtime->transportCommand({.action = Action::Tempo, .value = m_project.tempo});
        m_runtime->transportCommand({.action = Action::TimeSignature,
            .numerator = m_project.timeSigNumerator, .denominator = m_project.timeSigDenominator});
        m_runtime->transportCommand({.action = Action::LoopRange,
            .position = toSamples(m_project.loopStartSeconds), .end = toSamples(m_project.loopEndSeconds)});
        m_runtime->transportCommand({.action = Action::LoopEnabled, .enabled = m_project.loopEnabled});
        m_runtime->transportCommand({.action = Action::SeekSeconds, .value = m_documentPosition, .prepare = true});
        updateTimelineDuration();
        m_liveDeviceAllowed = openDevice;
        if (openDevice) {
            if (const auto opened = applyAudioConfiguration(config); !opened) return fail(opened);
        }
        return audio::Result::ok();
    } catch (const std::exception& error) {
        return fail(audio::Result::fail(audio::EngineError::Unknown, error.what()));
    }
}

} // namespace daw
