#include "EngineController.hpp"
#include "model/ProjectMemory.hpp"
#include <stdexcept>

namespace daw {
audio::Result EngineController::applyScriptTransaction(std::uint64_t expectedRevision,
                                                       const std::string &label,
                                                       const std::function<void()> &apply,
                                                       bool captureDeviceState) {
    if (cloudProjectBound())
        return audio::Result::fail(audio::EngineError::NotSupported,
                                   "Scripts cannot edit collaborative projects");
    if (projectRevision() != expectedRevision)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "Project changed since the script snapshot; refresh and retry");
    struct State {
        ProjectModel document;
        std::vector<AudioPluginStateEdit> devices;
    };
    const auto capture = [&] {
        auto state = std::make_shared<State>();
        state->document = m_project;
        if (captureDeviceState) {
            const auto slot = [&](const std::string &channel, InsertModel &model) {
                if (!model.isLoaded())
                    return;
                ChainSlotSnapshot saved;
                const auto result = captureInsertState(channel, model, saved);
                if (!result)
                    throw std::runtime_error(result.message());
                model = saved.model;
                appendInsertStateEdits(state->devices, channel, saved);
            };
            for (auto &track : state->document.tracks) {
                slot(track.id, track.instrument);
                if (track.channelColor)
                    slot(track.id, *track.channelColor);
                for (auto &device : track.inserts)
                    slot(track.id, device);
                for (auto &device : track.miniModules)
                    slot(track.id, device);
                for (auto &device : track.samplerFx.inserts)
                    slot(track.id, device);
                for (auto &clip : track.clips)
                    for (auto &device : clip.inserts)
                        slot(track.id, device);
            }
            for (auto &device : state->document.masterInserts)
                slot(kMasterChannelId, device);
            for (auto &device : state->document.masterMiniModules)
                slot(kMasterChannelId, device);
        }
        return state;
    };
    const auto restore = [this](const std::shared_ptr<State> &state) {
        m_project = state->document;
        m_deferredClipSync.clear();
        auto devices = state->devices;
        for (auto &device : devices) {
            device.address.instance = m_runtime->pluginInstanceId(device.address);
            device.replaceExisting = true;
        }
        const auto restored =
            rebuildGraph(false, AudioPluginLoadPolicy::PreserveUnavailable, devices);
        if (!restored)
            throw std::runtime_error(restored.message());
        updateTimelineDuration();
    };
    std::shared_ptr<State> before;
    try {
        before = capture();
    } catch (const std::exception &e) {
        return audio::Result::fail(audio::EngineError::InvalidArgument, e.what());
    }
    const UndoStack history = m_undo;
    const auto rollback = [&](std::string message) {
        try {
            restore(before);
        } catch (const std::exception &e) {
            m_project = before->document;
            message += "; could not restore audio state: ";
            message += e.what();
        } catch (...) {
            m_project = before->document;
            message += "; could not restore audio state";
        }
        m_undo = history;
        return audio::Result::fail(audio::EngineError::InvalidArgument, message);
    };
    try {
        {
            UndoStack::Suspend suspended(m_undo);
            apply();
        }
        // All primitives, including live controls, are represented by the same
        // synchronous before/after transaction. No group spans IPC or user input.
        if (captureDeviceState) {
            const auto after = capture();
            std::size_t bytes =
                estimatedProjectBytes(before->document) + estimatedProjectBytes(after->document);
            for (const auto &state : {before, after})
                for (const auto &device : state->devices)
                    bytes += device.state.state.size() +
                             device.parameters.size() * sizeof(InsertParameter);
            m_undo.push(
                label, [restore, before] { restore(before); }, [restore, after] { restore(after); },
                bytes);
        } else
            pushProjectSnapshotUndo(before->document, label);
        return audio::Result::ok();
    } catch (const std::exception &error) {
        return rollback(error.what());
    } catch (...) {
        return rollback("Script edit failed");
    }
}
} // namespace daw
