#pragma once

#include "EngineController.hpp"
#include "collaboration/PluginCompatibility.hpp"
#include "plugins/PluginConvert.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <algorithm>
#include <functional>
#include <vector>

namespace collab {

// Construct immutable worker jobs on the control thread. Neither the live
// controller nor a plugin instance is accessed by the worker or retained by it.
inline void probePluginReadiness(QObject* context, const daw::ProjectModel& project,
    const daw::PluginManager& manager,
    const std::vector<daw::collab::PluginRequirement>& requirements, qint64 revision,
    std::function<void(daw::collab::PluginReadinessReport)> complete) {
    auto report = daw::collab::evaluatePluginReadiness(requirements, manager, revision);
    struct Job { std::size_t index; std::function<std::string()> run; };
    std::vector<Job> jobs;
    std::vector<const daw::InsertModel*> instances;
    const auto append = [&](const auto& inserts) { for (const auto& slot : inserts) instances.push_back(&slot); };
    append(project.masterInserts);
    for (const auto& track : project.tracks) {
        instances.push_back(&track.instrument);
        append(track.inserts);
        append(track.samplerFx.inserts);
        for (const auto& clip : track.clips) append(clip.inserts);
    }
    const auto inventory = manager.plugins();
    for (std::size_t index = 0; index < report.plugins.size(); ++index) {
        auto& result = report.plugins[index];
        if (result.status != daw::collab::PluginReadinessStatus::Ready) continue;
        const auto descriptor = std::find_if(inventory.begin(), inventory.end(), [&](const auto& candidate) {
            return daw::collab::pluginSatisfiesRequirement(candidate, result.requirement);
        });
        if (descriptor == inventory.end()) continue;
        bool found = false;
        for (const auto* slot : instances) {
            if (slot->format != result.requirement.format || slot->uid != result.requirement.nativeUid) continue;
            found = true;
            if ((!slot->stateAsset.empty() && slot->stateFile.empty()) ||
                (!slot->rightStateAsset.empty() && slot->rightStateFile.empty())) {
                result.status = daw::collab::PluginReadinessStatus::ProbeFailed;
                continue;
            }
            jobs.push_back({index, manager.sharedStateProbe(*descriptor, slot->stateFile, project.sampleRate)});
            if (slot->channelMode == daw::PluginChannelMode::DualMono)
                jobs.push_back({index, manager.sharedStateProbe(*descriptor, slot->rightStateFile, project.sampleRate)});
        }
        // A requirement that is absent from the installed document belongs to
        // another revision. Probing a default instance would falsely admit an
        // editor before the required state has arrived.
        if (!found) result.status = daw::collab::PluginReadinessStatus::ProbeFailed;
    }
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start([guard, jobs = std::move(jobs), report = std::move(report), complete = std::move(complete)]() mutable {
        for (const auto& job : jobs) {
            if (!guard) return;
            try {
                if (!job.run().empty()) report.plugins[job.index].status = daw::collab::PluginReadinessStatus::ProbeFailed;
            } catch (...) {
                report.plugins[job.index].status = daw::collab::PluginReadinessStatus::ProbeFailed;
            }
        }
        if (guard) QMetaObject::invokeMethod(guard, [guard, report = std::move(report), complete = std::move(complete)]() mutable {
            if (guard) complete(std::move(report));
        }, Qt::QueuedConnection);
    });
}
}
