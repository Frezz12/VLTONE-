#include "EngineProjectProjectionAdapter.hpp"

#include "AssetCache.hpp"
#include "EngineController.hpp"
#include "collaboration/PluginCompatibility.hpp"
#include "plugins/PluginConvert.hpp"

#include "Core/AudioBuffer.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Recording/RecordingEngine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <utility>

namespace collab {
namespace {

struct TrackLocalState {
    bool muted = false;
    bool soloed = false;
    bool armed = false;
    bool monitor = false;
    bool monitorAuto = false;
    unsigned monitorInputMask = 3;
    daw::TrackRecordMode recordMode = daw::TrackRecordMode::UseGlobal;
    bool inputEnabled = false;
    std::uint32_t inputChannel = 0;
    std::uint32_t inputChannelCount = 1;
    double height = 72.0;
    double expandedHeight = 72.0;
    bool expanded = true;
    bool automationExpanded = false;
};

struct PluginLocalState {
    daw::PluginFormat format = daw::PluginFormat::None;
    std::string uid;
    std::string modulePath;
    daw::PluginEditorChannel editorChannel =
        daw::PluginEditorChannel::Left;
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    bool windowOpen = false;
};

struct SamplerProjection {
    std::string channelId;
    std::string slotId;
    std::string localPath;
    daw::AssetRef asset;
    QString location;
    bool required = false;
};

QString assetIdentity(const daw::AssetRef& asset) {
    if (!asset.assetId.empty()) return QString::fromStdString(asset.assetId);
    return QString::fromStdString(asset.sha256);
}

QString stateProbeIdentity(const daw::plugins::PluginDescriptor& descriptor,
                          const daw::InsertModel& slot, double sampleRate) {
    // File metadata invalidates a scan cache after a same-version replacement.
    // Neither filenames nor native state bytes leave the process.
    QJsonArray parts{
        int(descriptor.format), QString::fromStdString(descriptor.uid),
        QString::fromStdString(descriptor.vendor), QString::fromStdString(descriptor.version),
        descriptor.stateSchemaVersion, QString::fromStdString(descriptor.parameterFingerprint),
        QString::fromStdString(descriptor.path), QString::number(descriptor.fileSize),
        QString::number(descriptor.fileModifiedTime), sampleRate, int(slot.channelMode),
        QString::fromStdString(slot.stateAsset.sha256), QString::fromStdString(slot.rightStateAsset.sha256)};
    const auto appendUnhashedCapture = [&](const daw::AssetRef& asset, const std::string& path) {
        if (!asset.sha256.empty() || path.empty()) return;
        const QFileInfo file(QString::fromStdString(path));
        parts.append(file.absoluteFilePath());
        parts.append(QString::number(file.size()));
        parts.append(QString::number(file.lastModified().toMSecsSinceEpoch()));
    };
    appendUnhashedCapture(slot.stateAsset, slot.stateFile);
    appendUnhashedCapture(slot.rightStateAsset, slot.rightStateFile);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(parts).toJson(QJsonDocument::Compact),
                                                      QCryptographicHash::Sha256).toHex());
}

QStringList missingIds(const std::vector<MissingRuntimeAsset>& missing) {
    QSet<QString> unique;
    for (const MissingRuntimeAsset& item : missing) {
        const QString id = assetIdentity(item.asset);
        if (!id.isEmpty()) unique.insert(id);
    }
    QStringList result(unique.begin(), unique.end());
    result.sort(Qt::CaseSensitive);
    return result;
}

bool sameMissing(const std::vector<MissingRuntimeAsset>& left,
                 const std::vector<MissingRuntimeAsset>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const MissingRuntimeAsset& a = left[index];
        const MissingRuntimeAsset& b = right[index];
        if (a.asset.assetId != b.asset.assetId ||
            a.asset.sha256 != b.asset.sha256 || a.location != b.location ||
            a.required != b.required) {
            return false;
        }
    }
    return true;
}

template <typename Project, typename Visitor>
void visitInserts(Project& project, Visitor&& visitor) {
    for (auto& insert : project.masterInserts)
        visitor(std::string(daw::EngineController::kMasterChannelId), insert,
                QStringLiteral("master/plugin:%1")
                    .arg(QString::fromStdString(insert.id)));
    for (auto& track : project.tracks) {
        const QString trackLocation =
            QStringLiteral("track:%1").arg(QString::fromStdString(track.id));
        if (track.instrument.isLoaded() || !track.instrument.id.empty()) {
            visitor(track.id, track.instrument,
                    trackLocation + QStringLiteral("/instrument"));
        }
        for (auto& insert : track.samplerFx.inserts) {
            visitor(track.id, insert,
                    trackLocation + QStringLiteral("/sampler-fx/plugin:%1")
                                        .arg(QString::fromStdString(insert.id)));
        }
        for (auto& clip : track.clips) {
            for (auto& insert : clip.inserts) {
                visitor(track.id, insert,
                        trackLocation + QStringLiteral("/clip:%1/plugin:%2")
                                            .arg(QString::fromStdString(clip.id),
                                                 QString::fromStdString(insert.id)));
            }
        }
        for (auto& insert : track.inserts) {
            visitor(track.id, insert,
                    trackLocation + QStringLiteral("/plugin:%1")
                                        .arg(QString::fromStdString(insert.id)));
        }
    }
}

void addMissing(std::vector<MissingRuntimeAsset>& missing,
                const daw::AssetRef& asset, QString location, bool required) {
    if (asset.empty()) return;
    const auto duplicate = std::find_if(
        missing.begin(), missing.end(), [&](const MissingRuntimeAsset& item) {
            return item.asset.assetId == asset.assetId &&
                   item.asset.sha256 == asset.sha256 &&
                   item.location == location;
        });
    if (duplicate == missing.end())
        missing.push_back({asset, std::move(location), required});
}

void sortMissing(std::vector<MissingRuntimeAsset>& missing) {
    std::sort(missing.begin(), missing.end(),
              [](const MissingRuntimeAsset& left,
                 const MissingRuntimeAsset& right) {
                  const QString leftId = assetIdentity(left.asset);
                  const QString rightId = assetIdentity(right.asset);
                  if (leftId != rightId) return leftId < rightId;
                  return left.location < right.location;
              });
}

} // namespace

class EngineProjectProjectionAdapter::Impl {
public:
    Impl(EngineProjectProjectionAdapter* owner,
         daw::EngineController* controller, AssetCache* cache)
        : q(owner), engine(controller), assetCache(cache) {}

    void captureLocalState() {
        if (!engine) return;
        const daw::ProjectModel& current = engine->project();
        loopStartSeconds = current.loopStartSeconds;
        loopEndSeconds = current.loopEndSeconds;
        loopEnabled = current.loopEnabled;
        notebookHtml = current.notebookHtml;
        notebookCues = current.notebookCues;

        for (const daw::TrackModel& track : current.tracks) {
            trackLocal[track.id] = {
                track.muted,
                track.soloed,
                track.armed,
                track.monitor,
                track.monitorAuto,
                track.monitorInputMask,
                track.recordMode,
                track.inputEnabled,
                track.inputChannel,
                track.inputChannelCount,
                track.height,
                track.expandedHeight,
                track.expanded,
                track.automationExpanded,
            };
            for (const daw::ClipModel& clip : track.clips)
                clipExpanded[clip.id] = clip.expanded;
        }

        visitInserts(current, [&](const std::string&, const daw::InsertModel& slot,
                                  const QString&) {
            if (slot.id.empty()) return;
            pluginLocal[slot.id] = {
                slot.format,
                slot.uid,
                slot.path,
                slot.editorChannel,
                slot.windowX,
                slot.windowY,
                slot.windowWidth,
                slot.windowHeight,
                slot.windowOpen,
            };
        });
    }

    void overlayLocalState(daw::ProjectModel& runtime) const {
        runtime.loopStartSeconds = loopStartSeconds;
        runtime.loopEndSeconds = loopEndSeconds;
        runtime.loopEnabled = loopEnabled;
        // Notebook is document content. It must come from the confirmed reducer.

        for (daw::TrackModel& track : runtime.tracks) {
            if (const auto found = trackLocal.find(track.id);
                found != trackLocal.end()) {
                const TrackLocalState& local = found->second;
                if (engine && engine->sessionAuditionEnabled()) track.muted = local.muted;
                track.soloed = local.soloed;
                track.armed = local.armed;
                track.monitor = local.monitor;
                track.monitorAuto = local.monitorAuto;
                track.monitorInputMask = local.monitorInputMask;
                track.recordMode = local.recordMode;
                track.inputEnabled = local.inputEnabled;
                track.inputChannel = local.inputChannel;
                track.inputChannelCount = local.inputChannelCount;
                track.height = local.height;
                track.expandedHeight = local.expandedHeight;
                track.expanded = local.expanded;
                track.automationExpanded = local.automationExpanded;
            }
            for (daw::ClipModel& clip : track.clips) {
                if (const auto found = clipExpanded.find(clip.id);
                    found != clipExpanded.end()) {
                    clip.expanded = found->second;
                }
            }
        }

        visitInserts(runtime, [&](const std::string&, daw::InsertModel& slot,
                                  const QString&) {
            const auto found = pluginLocal.find(slot.id);
            if (found == pluginLocal.end()) return;
            const PluginLocalState& local = found->second;
            if (local.uid != slot.uid || local.format != slot.format) return;
            // Module location and editor placement are machine-local.  Opaque
            // state paths are not copied: those are resolved from stateAsset
            // below for this exact gateway materialization.
            slot.path = local.modulePath;
            slot.editorChannel = local.editorChannel;
            slot.windowX = local.windowX;
            slot.windowY = local.windowY;
            slot.windowWidth = local.windowWidth;
            slot.windowHeight = local.windowHeight;
            slot.windowOpen = local.windowOpen;
        });
    }

    QString resolve(const daw::AssetRef& asset) const {
        if (asset.empty()) return {};
        const QString cached = assetCache ? assetCache->resolve(asset) : QString{};
        if (!cached.isEmpty()) return cached;
        const QString captured = publicationAssetSources.value(QString::fromStdString(asset.assetId));
        return !captured.isEmpty() && QFileInfo(captured).isFile() ? captured : QString{};
    }

    void resolveAssets(daw::ProjectModel& runtime,
                       std::vector<MissingRuntimeAsset>& missing,
                       std::vector<SamplerProjection>& samplers) const {
        for (daw::TrackModel& track : runtime.tracks) {
            const QString trackLocation =
                QStringLiteral("track:%1")
                    .arg(QString::fromStdString(track.id));
            track.freeze.filePath.clear();
            track.freeze.sourceFingerprint.clear();
            if (!track.freeze.asset.empty()) {
                const QString path = resolve(track.freeze.asset);
                if (path.isEmpty()) addMissing(missing, track.freeze.asset,
                    trackLocation + QStringLiteral("/freeze"), true);
                else track.freeze.filePath = path.toStdString();
            }
            for (daw::ClipModel& clip : track.clips) {
                const QString clipLocation =
                    trackLocation + QStringLiteral("/clip:%1")
                                        .arg(QString::fromStdString(clip.id));
                clip.filePath.clear();
                if (!clip.asset.empty()) {
                    const QString path = resolve(clip.asset);
                    if (path.isEmpty()) {
                        addMissing(missing, clip.asset,
                                   clipLocation + QStringLiteral("/audio"),
                                   true);
                    } else {
                        clip.filePath = path.toStdString();
                    }
                } else if (engine && clip.kind == daw::ClipKind::Audio) {
                    // A clip shared ahead of its upload carries no asset yet.
                    // On the importing machine it still plays, straight from
                    // the file that was dropped; everywhere else it stays
                    // silent until clip.setAsset arrives. This override lives
                    // only on the runtime copy — the canonical document never
                    // sees a local path.
                    clip.filePath = engine->pendingLocalAudioPath(clip.id);
                }
                for (daw::TakeModel& take : clip.takes) {
                    take.filePath.clear();
                    if (take.asset.empty()) continue;
                    const QString path = resolve(take.asset);
                    if (path.isEmpty()) {
                        addMissing(
                            missing, take.asset,
                            clipLocation + QStringLiteral("/take:%1")
                                               .arg(QString::fromStdString(take.id)),
                            true);
                    } else {
                        take.filePath = path.toStdString();
                    }
                }
                for (auto& version : clip.offlineHistory) {
                    const QString location = clipLocation + QStringLiteral("/render:%1")
                        .arg(QString::fromStdString(version.id));
                    version.source.filePath.clear();
                    if (!version.source.asset.empty()) {
                        const QString path = resolve(version.source.asset);
                        if (path.isEmpty()) addMissing(missing, version.source.asset, location, true);
                        else version.source.filePath = path.toStdString();
                    }
                    for (auto& take : version.source.takes) {
                        take.filePath.clear();
                        if (take.asset.empty()) continue;
                        const QString path = resolve(take.asset);
                        if (path.isEmpty()) addMissing(missing, take.asset,
                            location + QStringLiteral("/take:%1").arg(QString::fromStdString(take.id)), true);
                        else take.filePath = path.toStdString();
                    }
                }
            }
        }

        visitInserts(runtime, [&](const std::string& channelId,
                                  daw::InsertModel& slot,
                                  const QString& location) {
            slot.stateFile.clear();
            if (!slot.stateAsset.empty()) {
                const QString path = resolve(slot.stateAsset);
                if (path.isEmpty())
                    addMissing(missing, slot.stateAsset,
                               location + QStringLiteral("/state"), true);
                else
                    slot.stateFile = path.toStdString();
            }

            slot.rightStateFile.clear();
            if (!slot.rightStateAsset.empty()) {
                const QString path = resolve(slot.rightStateAsset);
                if (path.isEmpty())
                    addMissing(missing, slot.rightStateAsset,
                               location + QStringLiteral("/right-state"), true);
                else
                    slot.rightStateFile = path.toStdString();
            }

            SamplerProjection sampler;
            const bool isSampler = slot.uid == "daw.sampler";
            if (isSampler) {
                sampler.channelId = channelId;
                sampler.slotId = slot.id;
                sampler.location = location + QStringLiteral("/binding:sample");
            }
            for (const daw::PluginAssetBinding& binding : slot.assetBindings) {
                const QString bindingLocation =
                    location + QStringLiteral("/binding:%1")
                                   .arg(QString::fromStdString(binding.key));
                const QString path = resolve(binding.asset);
                if (path.isEmpty()) {
                    addMissing(missing, binding.asset, bindingLocation,
                               binding.required);
                }
                if (isSampler && binding.key == "sample") {
                    sampler.asset = binding.asset;
                    sampler.required = binding.required;
                    sampler.location = bindingLocation;
                    sampler.localPath = path.toStdString();
                }
            }
            // An empty/missing sample binding explicitly clears a reused
            // sampler after graph projection.  Otherwise it could keep playing
            // bytes from the previous optimistic state.
            if (isSampler) samplers.push_back(std::move(sampler));
        });
        sortMissing(missing);
    }

    void setProbeStatus(const QString& insertId, const QString& status) {
        if (probeStatuses.value(insertId) == status) return;
        probeStatuses.insert(insertId, status);
        emit q->pluginStateProbeChanged(insertId, status);
    }

    void completeProbe(const QString& key, quint64 generation, bool passed) {
        if (generation != probeGeneration || !hasDocument) return;
        pendingProbes.remove(key);
        probeResults.insert(key, passed);
        daw::collab::ChangeImpact impact;
        for (auto it = currentProbeKeys.begin(); it != currentProbeKeys.end(); ++it) {
            if (it.value() != key) continue;
            setProbeStatus(it.key(), passed ? QStringLiteral("ready") : QStringLiteral("failed"));
            impact.pluginInsertIds.insert(it.key().toStdString());
        }
        if (impact.pluginInsertIds.empty()) return; // Removed/replaced while the worker ran.
        impact.graphRebuild = true;
        impact.documentChanged = true;
        requestLatest(daw::collab::ProjectionOrigin::Rebase, std::move(impact));
    }

    void gateExternalStates(daw::ProjectModel& runtime) {
        currentProbeKeys.clear();
        if (!engine) return;
        const auto descriptors = engine->pluginManager().plugins();
        visitInserts(runtime, [&](const std::string&, daw::InsertModel& slot, const QString& location) {
            slot.runtimeStateBlocked = false;
            if (slot.format == daw::PluginFormat::Internal || slot.format == daw::PluginFormat::None) return;
            slot.runtimeStateBlocked = true;
            slot.windowOpen = false;
            const QString id = QString::fromStdString(slot.id);
            if ((!slot.stateAsset.empty() && slot.stateFile.empty()) ||
                (!slot.rightStateAsset.empty() && slot.rightStateFile.empty()) ||
                std::any_of(slot.assetBindings.begin(), slot.assetBindings.end(), [&](const auto& binding) {
                    return binding.required && resolve(binding.asset).isEmpty();
                })) {
                setProbeStatus(id, QStringLiteral("missing"));
                return;
            }
            daw::collab::PluginRequirement requirement;
            requirement.format = slot.format;
            requirement.nativeUid = slot.uid;
            requirement.vendor = slot.vendor;
            requirement.version = slot.pluginVersion;
            requirement.stateSchemaVersion = slot.stateSchemaVersion;
            requirement.instrument = location.endsWith(QLatin1String("/instrument"));
            requirement.channelMode = slot.channelMode;
            requirement.parameterFingerprint = slot.parameterFingerprint;
            const auto found = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto& descriptor) {
                return daw::collab::pluginSatisfiesRequirement(descriptor, requirement);
            });
            if (found == descriptors.end()) {
                setProbeStatus(id, QStringLiteral("missing"));
                return;
            }
            slot.path = found->path;
            const QString key = stateProbeIdentity(*found, slot, runtime.sampleRate);
            currentProbeKeys.insert(id, key);
            if (const auto cached = probeResults.constFind(key); cached != probeResults.cend()) {
                slot.runtimeStateBlocked = !cached.value();
                setProbeStatus(id, cached.value() ? QStringLiteral("ready") : QStringLiteral("failed"));
                return;
            }
            setProbeStatus(id, QStringLiteral("checking"));
            if (pendingProbes.contains(key)) return;
            pendingProbes.insert(key);
            auto left = engine->pluginManager().sharedStateProbe(*found, slot.stateFile, runtime.sampleRate);
            auto right = slot.channelMode == daw::PluginChannelMode::DualMono
                ? engine->pluginManager().sharedStateProbe(*found, slot.rightStateFile, runtime.sampleRate)
                : std::function<std::string()>{};
            const quint64 generation = probeGeneration;
            QPointer<EngineProjectProjectionAdapter> guard(q);
            QThreadPool::globalInstance()->start([guard, key, generation, left = std::move(left), right = std::move(right)] {
                if (!guard) return;
                bool passed = false;
                try { passed = left().empty() && (!right || right().empty()); } catch (...) {}
                if (guard) QMetaObject::invokeMethod(guard, [guard, key, generation, passed] {
                    if (guard) guard->m_impl->completeProbe(key, generation, passed);
                }, Qt::QueuedConnection);
            });
        });
        // Do not leave vanished slots in the status panel.
        QSet<QString> present;
        visitInserts(runtime, [&](const std::string&, const daw::InsertModel& slot, const QString&) {
            if (slot.format != daw::PluginFormat::Internal && slot.format != daw::PluginFormat::None)
                present.insert(QString::fromStdString(slot.id));
        });
        const auto oldIds = probeStatuses.keys();
        for (const auto& id : oldIds) if (!present.contains(id)) {
            probeStatuses.remove(id);
            emit q->pluginStateProbeChanged(id, {});
        }
    }

    void publishMissing(std::vector<MissingRuntimeAsset> next) {
        sortMissing(next);
        if (sameMissing(missing, next)) return;
        missing = std::move(next);
        QList<daw::AssetRef> refs;
        refs.reserve(qsizetype(missing.size()));
        for (const MissingRuntimeAsset& item : missing)
            refs.push_back(item.asset);
        emit q->missingAssetsChanged(missingIds(missing));
        emit q->missingAssetRefsChanged(refs);
    }

    void clear() {
        const bool hadMissing = !missing.empty();
        latest = {};
        hasDocument = false;
        projecting = false;
        draining = false;
        projectionPending = false;
        hydrationPending = false;
        hydrationPendingImpact = {};
        reentrancyFault = false;
        hasProjectionAttempt = false;
        latestImpact = {};
        lastImpact = {};
        lastProjectionError.clear();
        missing.clear();
        trackLocal.clear();
        clipExpanded.clear();
        pluginLocal.clear();
        publicationAssetSources.clear();
        ++probeGeneration;
        pendingProbes.clear();
        probeResults.clear();
        currentProbeKeys.clear();
        const auto oldProbeIds = probeStatuses.keys();
        probeStatuses.clear();
        for (const auto& id : oldProbeIds) emit q->pluginStateProbeChanged(id, {});
        if (hadMissing) {
            emit q->missingAssetsChanged({});
            emit q->missingAssetRefsChanged({});
        }
    }

    daw::collab::ChangeImpact hydrationImpact(const QString& assetId,
                                               const QString& sha256) const {
        daw::collab::ChangeImpact impact;
        const auto matches = [&](const daw::AssetRef& asset) {
            return (!assetId.isEmpty() &&
                    QString::fromStdString(asset.assetId) == assetId) ||
                   (!sha256.isEmpty() &&
                    QString::fromStdString(asset.sha256) == sha256);
        };
        for (const daw::TrackModel& track : latest.project.tracks) {
            if (matches(track.freeze.asset)) {
                impact.trackIds.insert(track.id);
                impact.graphRebuild = true;
            }
            for (const daw::ClipModel& clip : track.clips) {
                bool clipMatched = matches(clip.asset);
                for (const daw::TakeModel& take : clip.takes)
                    clipMatched = clipMatched || matches(take.asset);
                for (const auto& version : clip.offlineHistory) {
                    clipMatched = clipMatched || matches(version.source.asset);
                    for (const auto& take : version.source.takes)
                        clipMatched = clipMatched || matches(take.asset);
                }
                if (!clipMatched) continue;
                impact.trackIds.insert(track.id);
                impact.clipIds.insert(clip.id);
            }
        }
        visitInserts(latest.project,
                     [&](const std::string& channelId,
                         const daw::InsertModel& slot, const QString&) {
            bool slotMatched = matches(slot.stateAsset) ||
                               matches(slot.rightStateAsset);
            for (const daw::PluginAssetBinding& binding : slot.assetBindings)
                slotMatched = slotMatched || matches(binding.asset);
            if (!slotMatched || slot.id.empty()) return;
            impact.pluginInsertIds.insert(slot.id);
            if (channelId != daw::EngineController::kMasterChannelId)
                impact.trackIds.insert(channelId);
        });
        return impact;
    }

    void projectOne(const daw::collab::ChangeImpact& impact,
                    daw::collab::ProjectionOrigin origin) {
        if (!engine || !hasDocument) return;
        projecting = true;
        hasProjectionAttempt = true;
        lastOrigin = origin;
        lastImpact = impact;
        captureLocalState();

        daw::ProjectModel runtime = latest.project;
        overlayLocalState(runtime);
        std::vector<MissingRuntimeAsset> nextMissing;
        std::vector<SamplerProjection> samplerProjections;
        resolveAssets(runtime, nextMissing, samplerProjections);
        gateExternalStates(runtime);

        const bool initialSnapshot =
            origin == daw::collab::ProjectionOrigin::Snapshot;
        const bool requiresFullMaterialization =
            initialSnapshot ||
            (origin == daw::collab::ProjectionOrigin::Rebase &&
             impact.fullProjection);
        const audio::Result applied = requiresFullMaterialization
            ? engine->materializeCollaborationProject(std::move(runtime),
                                                      initialSnapshot)
            : engine->projectCollaborationChange(std::move(runtime), impact);
        if (!applied) {
            lastProjectionError = QString::fromStdString(applied.message());
            publishMissing(std::move(nextMissing));
            projecting = false;
            emit q->projectionFailed(lastProjectionError);
            return;
        }

        // Asset I/O and decoding are control-thread operations.  The graph is
        // already published, so the sampler instance exists; loading its
        // explicit `sample` binding does not mutate the shared ProjectModel and
        // the silent methods never touch UndoStack.
        for (const SamplerProjection& sampler : samplerProjections) {
            if (sampler.localPath.empty()) {
                engine->clearSamplerSampleSilently(sampler.channelId,
                                                   sampler.slotId);
                continue;
            }
            engine->loadSamplerSampleSilently(sampler.channelId,
                                              sampler.slotId,
                                              sampler.localPath);
            const auto* instance =
                engine->samplerInstance(sampler.channelId, sampler.slotId);
            if (!instance || !instance->rawSample()) {
                engine->clearSamplerSampleSilently(sampler.channelId,
                                                   sampler.slotId);
                addMissing(nextMissing, sampler.asset, sampler.location,
                           sampler.required);
            }
        }

        lastProjectionError.clear();
        publishMissing(std::move(nextMissing));
        projecting = false;
        emit q->projectionSucceeded();
    }

    void requestProjection(
        const daw::collab::SharedProjectDocument& document,
        const daw::collab::ChangeImpact& impact,
        daw::collab::ProjectionOrigin origin) {
        latest = document;
        if (projectionPending)
            latestImpact.merge(impact);
        else
            latestImpact = impact;
        latestOrigin = origin;
        hasDocument = true;
        projectionPending = true;

        // A snapshot is the only trusted way out of a callback feedback fault.
        // Non-snapshot requests are retained as `latest`, but cannot start an
        // unbounded projection loop while the bridge obtains a verified base.
        if (reentrancyFault) {
            if (origin != daw::collab::ProjectionOrigin::Snapshot) return;
            reentrancyFault = false;
        }
        drainProjections();
    }

    void requestLatest(daw::collab::ProjectionOrigin origin,
                       daw::collab::ChangeImpact impact) {
        if (!hasDocument) return;
        latestOrigin = origin;
        if (projectionPending)
            latestImpact.merge(impact);
        else
            latestImpact = std::move(impact);
        projectionPending = true;
        if (!reentrancyFault) drainProjections();
    }

    void drainProjections() {
        if (draining || reentrancyFault || !engine || !hasDocument) return;
        static constexpr int kMaximumSynchronousPasses = 8;
        draining = true;
        int passes = 0;
        while (projectionPending && passes < kMaximumSynchronousPasses) {
            projectionPending = false;
            const daw::collab::ProjectionOrigin origin = latestOrigin;
            const daw::collab::ChangeImpact impact = latestImpact;
            projectOne(impact, origin);
            ++passes;

            // AssetCache may publish readiness while missingAssetsChanged is
            // being delivered.  A newer document request already resolves the
            // same asset, so hydration only schedules an extra pass when no
            // document update is waiting.
            if (hydrationPending) {
                hydrationPending = false;
                if (projectionPending) {
                    latestImpact.merge(hydrationPendingImpact);
                } else {
                    latestOrigin = daw::collab::ProjectionOrigin::Rebase;
                    latestImpact = hydrationPendingImpact;
                    projectionPending = true;
                }
                hydrationPendingImpact = {};
            }
        }

        if (!projectionPending) {
            draining = false;
            return;
        }

        // A projection callback that synchronously requests another projection
        // on every pass is a feedback cycle, not a useful burst.  Keep the
        // latest document latched, stop the loop, and force the collaboration
        // bridge onto its verified-resync path.  A Snapshot request clears the
        // latch and provides one clean retry boundary.
        projectionPending = false;
        reentrancyFault = true;
        lastProjectionError = QStringLiteral(
            "Collaboration projection callbacks did not settle");
        draining = false;
        emit q->projectionFailed(lastProjectionError);
    }

    void assetReady(const QString& assetId, const QString& sha256) {
        if (!hasDocument) return;
        const bool awaited = std::any_of(
            missing.begin(), missing.end(), [&](const MissingRuntimeAsset& item) {
                return (!assetId.isEmpty() &&
                        QString::fromStdString(item.asset.assetId) == assetId) ||
                       (!sha256.isEmpty() &&
                        QString::fromStdString(item.asset.sha256) == sha256);
            });
        if (!awaited) return;
        daw::collab::ChangeImpact impact = hydrationImpact(assetId, sha256);
        if (projecting || draining) {
            hydrationPending = true;
            hydrationPendingImpact.merge(impact);
            return;
        }
        requestLatest(daw::collab::ProjectionOrigin::Rebase,
                      std::move(impact));
    }

    EngineProjectProjectionAdapter* q = nullptr;
    daw::EngineController* engine = nullptr;
    AssetCache* assetCache = nullptr;
    QHash<QString, QString> publicationAssetSources;
    daw::collab::SharedProjectDocument latest;
    bool hasDocument = false;
    bool projecting = false;
    bool draining = false;
    bool projectionPending = false;
    bool hydrationPending = false;
    daw::collab::ChangeImpact hydrationPendingImpact;
    bool reentrancyFault = false;
    bool hasProjectionAttempt = false;
    daw::collab::ChangeImpact latestImpact;
    daw::collab::ProjectionOrigin latestOrigin =
        daw::collab::ProjectionOrigin::Snapshot;
    daw::collab::ProjectionOrigin lastOrigin =
        daw::collab::ProjectionOrigin::Snapshot;
    daw::collab::ChangeImpact lastImpact;
    QString lastProjectionError;
    std::vector<MissingRuntimeAsset> missing;
    quint64 probeGeneration = 0;
    QSet<QString> pendingProbes;
    QHash<QString, bool> probeResults;
    QHash<QString, QString> currentProbeKeys;
    QHash<QString, QString> probeStatuses;

    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
    bool loopEnabled = false;
    std::string notebookHtml;
    std::vector<daw::NotebookCueModel> notebookCues;
    std::unordered_map<std::string, TrackLocalState> trackLocal;
    std::unordered_map<std::string, bool> clipExpanded;
    std::unordered_map<std::string, PluginLocalState> pluginLocal;
};

EngineProjectProjectionAdapter::EngineProjectProjectionAdapter(
    daw::EngineController* controller, AssetCache* assetCache, QObject* parent)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(this, controller, assetCache)) {
    if (assetCache) {
        connect(assetCache, &AssetCache::assetReady, this,
                [this](const QString& assetId, const QString& sha256,
                       const QString&) {
                    m_impl->assetReady(assetId, sha256);
                });
    }
}

EngineProjectProjectionAdapter::~EngineProjectProjectionAdapter() = default;

void EngineProjectProjectionAdapter::projectChanged(
    const daw::collab::SharedProjectDocument& document,
    const daw::collab::ChangeImpact& impact,
    daw::collab::ProjectionOrigin origin) {
    if (QThread::currentThread() != thread()) {
        const daw::collab::SharedProjectDocument copy = document;
        const daw::collab::ChangeImpact impactCopy = impact;
        QMetaObject::invokeMethod(
            this,
            [this, copy, impactCopy, origin] {
                projectChanged(copy, impactCopy, origin);
            },
            Qt::QueuedConnection);
        return;
    }
    m_impl->requestProjection(document, impact, origin);
}

void EngineProjectProjectionAdapter::clearDocument() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { clearDocument(); },
                                  Qt::QueuedConnection);
        return;
    }
    m_impl->clear();
}

void EngineProjectProjectionAdapter::retryPluginStateProbes() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { retryPluginStateProbes(); }, Qt::QueuedConnection);
        return;
    }
    ++m_impl->probeGeneration;
    m_impl->pendingProbes.clear();
    for (auto it = m_impl->probeResults.begin(); it != m_impl->probeResults.end();) {
        if (!it.value()) it = m_impl->probeResults.erase(it);
        else ++it;
    }
    daw::collab::ChangeImpact impact;
    impact.graphRebuild = true;
    impact.documentChanged = true;
    m_impl->requestLatest(daw::collab::ProjectionOrigin::Rebase, std::move(impact));
}

void EngineProjectProjectionAdapter::setPublicationAssetSources(QHash<QString, QString> localSources) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, localSources = std::move(localSources)]() mutable {
            setPublicationAssetSources(std::move(localSources));
        }, Qt::QueuedConnection);
        return;
    }
    QHash<QString, QString> captured;
    for (auto it = localSources.cbegin(); it != localSources.cend(); ++it) {
        const QFileInfo file(it.value());
        if (!it.key().isEmpty() && file.isAbsolute() && file.isFile())
            captured.insert(it.key(), file.absoluteFilePath());
    }
    if (m_impl->publicationAssetSources == captured) return;
    m_impl->publicationAssetSources = std::move(captured);
    daw::collab::ChangeImpact impact;
    impact.graphRebuild = true;
    impact.documentChanged = true;
    m_impl->requestLatest(daw::collab::ProjectionOrigin::Rebase, std::move(impact));
}

std::vector<MissingRuntimeAsset>
EngineProjectProjectionAdapter::missingAssets() const {
    return m_impl->missing;
}

QStringList EngineProjectProjectionAdapter::missingAssetIds() const {
    return missingIds(m_impl->missing);
}

QString EngineProjectProjectionAdapter::lastError() const {
    return m_impl->lastProjectionError;
}

bool EngineProjectProjectionAdapter::hasMaterializedDocument() const {
    return m_impl->hasProjectionAttempt &&
           m_impl->lastProjectionError.isEmpty();
}

std::optional<daw::collab::ProjectionOrigin>
EngineProjectProjectionAdapter::lastProjectionOrigin() const {
    if (!m_impl->hasProjectionAttempt) return std::nullopt;
    return m_impl->lastOrigin;
}

daw::collab::ChangeImpact
EngineProjectProjectionAdapter::lastProjectionImpact() const {
    return m_impl->lastImpact;
}

bool checkEngineProjectProjectionForTest(QString* error) {
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };

    QTemporaryDir temporary;
    if (!temporary.isValid())
        return fail(QStringLiteral("cannot create projection fixture"));
    const QString source = temporary.filePath(QStringLiteral("tone.wav"));
    {
        audio::AudioBuffer tone(1, 480);
        for (audio::BufferSize frame = 0; frame < tone.numFrames(); ++frame) {
            tone.getChannel(0)[frame] =
                0.25f * std::sin(float(frame) * 0.071f);
        }
        audio::AudioRecorder writer;
        if (!writer.initialize(48000.0, 1) ||
            !writer.writeWAVFile(source.toStdString(), tone, 48000.0)) {
            return fail(QStringLiteral("cannot write projection audio fixture"));
        }
    }

    QFile bytes(source);
    if (!bytes.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot hash projection audio fixture"));
    daw::AssetRef asset;
    asset.assetId = daw::newUuid();
    asset.sha256 = QCryptographicHash::hash(bytes.readAll(),
                                           QCryptographicHash::Sha256)
                       .toHex()
                       .toStdString();
    asset.kind = daw::AssetKind::Audio;
    asset.byteSize = std::uint64_t(QFileInfo(source).size());
    asset.originalName = "tone.wav";

    AssetCache cache(temporary.filePath(QStringLiteral("cache")));
    daw::EngineController engine;
    if (!engine.initialize(48000.0, 128, /*openDevice=*/false))
        return fail(QStringLiteral("cannot initialize projection engine"));

    const std::string instrument =
        engine.addTrack(daw::TrackKind::Instrument, "Local instrument");
    const std::string audioTrack =
        engine.addTrack(daw::TrackKind::Audio, "Local audio");
    if (!engine.loadInstrumentSampler(instrument, source.toStdString()))
        return fail(QStringLiteral("built-in sampler fixture is unavailable"));

    engine.setTrackSoloed(audioTrack, true);
    engine.setTrackArmed(audioTrack, true);
    engine.setTrackMonitor(audioTrack, true);
    engine.setTrackInputEnabled(audioTrack, true);
    engine.setTrackInputChannel(audioTrack, 1);
    engine.setTrackHeight(audioTrack, 137.0);
    engine.setLoopRangeSeconds(0.25, 0.75);
    engine.setLoopEnabled(true);
    engine.seekSeconds(0.5);
    engine.play();

    daw::collab::SharedProjectDocument shared;
    shared.project = engine.project();
    shared.project.name = "Cloud project";
    daw::TrackModel* sharedAudio = shared.project.findTrack(audioTrack);
    daw::TrackModel* sharedInstrument = shared.project.findTrack(instrument);
    if (!sharedAudio || !sharedInstrument)
        return fail(QStringLiteral("projection fixture tracks disappeared"));

    // What a canonical snapshot carries: no participant state or local paths.
    for (daw::TrackModel* track : {sharedAudio, sharedInstrument}) {
        track->soloed = false;
        track->armed = false;
        track->monitor = false;
        track->monitorAuto = false;
        track->recordMode = daw::TrackRecordMode::UseGlobal;
        track->inputEnabled = false;
        track->inputChannel = 0;
        track->inputChannelCount = 1;
        track->height = 72.0;
        track->expandedHeight = 72.0;
        track->expanded = true;
        track->automationExpanded = false;
    }
    shared.project.loopStartSeconds = 0.0;
    shared.project.loopEndSeconds = 0.0;
    shared.project.loopEnabled = false;

    daw::ClipModel clip;
    clip.id = daw::newUuid();
    clip.name = "Cloud audio";
    clip.kind = daw::ClipKind::Audio;
    clip.durationSeconds = 0.01;
    clip.asset = asset;
    sharedAudio->clips.push_back(clip);
    sharedInstrument->instrument.stateFile.clear();
    sharedInstrument->instrument.assetBindings = {
        daw::PluginAssetBinding{"sample", asset, true},
    };

    EngineProjectProjectionAdapter adapter(&engine, &cache);
    int projected = 0;
    QObject::connect(&adapter,
                     &EngineProjectProjectionAdapter::projectionSucceeded,
                     &adapter, [&] { ++projected; });
    daw::collab::ChangeImpact full;
    full.fullProjection = true;
    full.documentChanged = true;
    adapter.projectChanged(shared, full,
                           daw::collab::ProjectionOrigin::Snapshot);

    const daw::TrackModel* runtimeAudio = engine.project().findTrack(audioTrack);
    const daw::TrackModel* runtimeInstrument =
        engine.project().findTrack(instrument);
    if (projected != 1 || !runtimeAudio || !runtimeInstrument ||
        engine.project().name != "Cloud project" || !runtimeAudio->soloed ||
        !runtimeAudio->armed || !runtimeAudio->monitor ||
        !runtimeAudio->inputEnabled || runtimeAudio->inputChannel != 1 ||
        std::fabs(runtimeAudio->height - 137.0) > 0.001 ||
        std::fabs(runtimeAudio->expandedHeight - 137.0) > 0.001 ||
        !engine.isPlaying() || !engine.isLoopEnabled() ||
        engine.undoDepth() != 0 || runtimeAudio->clips.empty() ||
        !runtimeAudio->clips.front().filePath.empty() ||
        adapter.missingAssetIds() !=
            QStringList{QString::fromStdString(asset.assetId)}) {
        return fail(QStringLiteral(
            "snapshot projection did not preserve local state or silence a missing asset"));
    }
    const auto* missingSampler = engine.samplerInstance(
        instrument, runtimeInstrument->instrument.id);
    if (!missingSampler || missingSampler->rawSample())
        return fail(QStringLiteral("missing sampler binding was not silenced"));

    const std::uint64_t rebuildsAfterSnapshot =
        engine.graphRebuildCountForTest();
    const AssetCacheResult imported = cache.importFile(asset, source);
    runtimeAudio = engine.project().findTrack(audioTrack);
    runtimeInstrument = engine.project().findTrack(instrument);
    const auto* readySampler = runtimeInstrument
        ? engine.samplerInstance(instrument, runtimeInstrument->instrument.id)
        : nullptr;
    if (!imported || projected != 2 || !runtimeAudio ||
        runtimeAudio->clips.empty() ||
        runtimeAudio->clips.front().filePath != imported.localPath.toStdString() ||
        !adapter.missingAssets().empty() || engine.undoDepth() != 0 ||
        !runtimeAudio->soloed || !runtimeAudio->armed ||
        !engine.isPlaying() || !readySampler || !readySampler->rawSample() ||
        readySampler->samplePath() != imported.localPath.toStdString() ||
        engine.graphRebuildCountForTest() != rebuildsAfterSnapshot) {
        return fail(QStringLiteral(
            "assetReady did not hydrate targeted entities without a graph rebuild"));
    }

    // A non-snapshot projection neither clears nor appends legacy history.
    engine.setProjectKey(5, "minor");
    const std::size_t historyBefore = engine.undoDepth();
    shared.project.name = "Remote rename";
    daw::collab::ChangeImpact renameImpact;
    renameImpact.documentChanged = true;
    renameImpact.fieldKeys.insert("project:name");
    const std::uint64_t rebuildsBeforeRename =
        engine.graphRebuildCountForTest();
    adapter.projectChanged(shared, renameImpact,
                           daw::collab::ProjectionOrigin::ConfirmedRemote);
    if (engine.project().name != "Remote rename" ||
        engine.undoDepth() != historyBefore || !engine.isPlaying() ||
        engine.graphRebuildCountForTest() != rebuildsBeforeRename) {
        return fail(QStringLiteral(
            "scalar projection rebuilt the graph or changed local history/transport"));
    }

    const std::vector<daw::plugins::ParameterInfo> samplerParameters =
        engine.insertParameters(instrument,
                                sharedInstrument->instrument.id);
    const auto adjustable = std::find_if(
        samplerParameters.begin(), samplerParameters.end(),
        [](const daw::plugins::ParameterInfo& parameter) {
            return parameter.maxValue > parameter.minValue;
        });
    if (adjustable == samplerParameters.end())
        return fail(QStringLiteral("sampler parameter fixture is unavailable"));
    const double parameterValue =
        adjustable->minValue +
        (adjustable->maxValue - adjustable->minValue) * 0.37;
    sharedInstrument->instrument.parameters = {
        daw::InsertParameter{adjustable->id, parameterValue},
    };
    daw::collab::ChangeImpact parameterImpact;
    parameterImpact.documentChanged = true;
    // The reducer currently keeps this conservative bit for plugin state
    // compatibility.  The controller's topology comparison is the final gate.
    parameterImpact.graphRebuild = true;
    parameterImpact.trackIds.insert(instrument);
    parameterImpact.pluginInsertIds.insert(sharedInstrument->instrument.id);
    parameterImpact.fieldKeys.insert(
        "plugin:" + sharedInstrument->instrument.id + ":parameter:left:" +
        adjustable->id);
    const std::uint64_t rebuildsBeforeParameter =
        engine.graphRebuildCountForTest();
    adapter.projectChanged(shared, parameterImpact,
                           daw::collab::ProjectionOrigin::ConfirmedRemote);
    if (engine.graphRebuildCountForTest() != rebuildsBeforeParameter ||
        std::fabs(engine.insertParameter(instrument,
                                         sharedInstrument->instrument.id,
                                         adjustable->id) -
                  parameterValue) > 0.0001) {
        return fail(QStringLiteral(
            "plugin parameter projection rebuilt the graph or missed the live node"));
    }

    // A structural outer batch publishes one graph, even though its document
    // can contain many model changes.
    daw::collab::SharedProjectDocument structural = shared;
    daw::TrackModel added;
    added.id = daw::newUuid();
    added.kind = daw::TrackKind::Audio;
    added.name = "Remote structural track";
    structural.project.tracks.push_back(added);
    daw::collab::ChangeImpact structuralImpact;
    structuralImpact.documentChanged = true;
    structuralImpact.graphRebuild = true;
    structuralImpact.timelineChanged = true;
    structuralImpact.trackIds.insert(added.id);
    const std::uint64_t rebuildsBeforeStructure =
        engine.graphRebuildCountForTest();
    adapter.projectChanged(structural, structuralImpact,
                           daw::collab::ProjectionOrigin::ConfirmedRemote);
    if (!engine.project().findTrack(added.id) ||
        engine.graphRebuildCountForTest() != rebuildsBeforeStructure + 1) {
        return fail(QStringLiteral(
            "structural collaboration batch did not publish exactly one graph"));
    }
    const std::uint64_t rebuildsBeforeStructuralRestore =
        engine.graphRebuildCountForTest();
    adapter.projectChanged(shared, structuralImpact,
                           daw::collab::ProjectionOrigin::ConfirmedRemote);
    if (engine.project().findTrack(added.id) ||
        engine.graphRebuildCountForTest() !=
            rebuildsBeforeStructuralRestore + 1) {
        return fail(QStringLiteral(
            "structural collaboration rollback did not publish exactly one graph"));
    }

    // A callback can synchronously produce more reducer materializations while
    // the current engine transaction is still publishing missing-asset state.
    // The adapter must coalesce these requests and drain the latest one after
    // the outer transaction, rather than overwrite-and-lose it.
    daw::AssetRef nestedMissing = asset;
    nestedMissing.assetId = daw::newUuid();
    nestedMissing.sha256 = std::string(64, 'f');
    nestedMissing.originalName = "nested-missing.wav";
    daw::collab::SharedProjectDocument reentrantOuter = shared;
    reentrantOuter.project.name = "Reentrant outer";
    daw::TrackModel* reentrantAudio =
        reentrantOuter.project.findTrack(audioTrack);
    if (!reentrantAudio || reentrantAudio->clips.empty())
        return fail(QStringLiteral("reentrant projection fixture disappeared"));
    reentrantAudio->clips.front().asset = nestedMissing;
    daw::collab::SharedProjectDocument superseded = reentrantOuter;
    superseded.project.name = "Reentrant superseded";
    daw::collab::SharedProjectDocument reentrantLatest = reentrantOuter;
    reentrantLatest.project.name = "Reentrant latest";
    reentrantLatest.project.masterVolume = 0.625f;
    daw::collab::ChangeImpact supersededImpact;
    supersededImpact.timelineChanged = true;
    daw::collab::ChangeImpact latestImpact;
    latestImpact.masterGainChanged = true;
    latestImpact.fieldKeys.insert("project.masterVolume");
    bool injectedReentrantProjection = false;
    QObject::connect(
        &adapter, &EngineProjectProjectionAdapter::missingAssetsChanged,
        &adapter, [&](const QStringList& ids) {
            if (injectedReentrantProjection ||
                !ids.contains(QString::fromStdString(nestedMissing.assetId))) {
                return;
            }
            injectedReentrantProjection = true;
            adapter.projectChanged(
                superseded, supersededImpact,
                daw::collab::ProjectionOrigin::OptimisticLocal);
            adapter.projectChanged(
                reentrantLatest, latestImpact,
                daw::collab::ProjectionOrigin::ConfirmedRemote);
        });
    const int projectedBeforeReentrancy = projected;
    adapter.projectChanged(reentrantOuter, full,
                           daw::collab::ProjectionOrigin::Rebase);
    const auto reentrantOrigin = adapter.lastProjectionOrigin();
    const daw::collab::ChangeImpact reentrantImpact =
        adapter.lastProjectionImpact();
    if (!injectedReentrantProjection ||
        projected != projectedBeforeReentrancy + 2 ||
        engine.project().name != "Reentrant latest" ||
        std::fabs(engine.project().masterVolume - 0.625f) > 0.0001f ||
        !reentrantOrigin ||
        *reentrantOrigin !=
            daw::collab::ProjectionOrigin::ConfirmedRemote ||
        !reentrantImpact.masterGainChanged ||
        !reentrantImpact.fieldKeys.contains("project.masterVolume")) {
        return fail(QStringLiteral(
            "nested projection did not coalesce to the latest document and metadata"));
    }

    // Restore the stable document used by the transactional rollback check.
    adapter.projectChanged(shared, full,
                           daw::collab::ProjectionOrigin::ConfirmedRemote);
    if (engine.project().name != "Remote rename" || !adapter.lastError().isEmpty())
        return fail(QStringLiteral("projection did not settle after reentrant drain"));

    // Defense at the engine boundary: reducer validation normally prevents a
    // routing cycle, but a bad materialization must still leave both the last
    // audible graph and its model intact.
    daw::collab::SharedProjectDocument malformed = shared;
    malformed.project.name = "Must roll back";
    daw::TrackModel* malformedAudio =
        malformed.project.findTrack(audioTrack);
    daw::TrackModel* malformedInstrument =
        malformed.project.findTrack(instrument);
    if (!malformedAudio || !malformedInstrument)
        return fail(QStringLiteral("rollback fixture tracks disappeared"));
    malformedAudio->outputBusId = instrument;
    malformedInstrument->outputBusId = audioTrack;
    int rejected = 0;
    QObject::connect(&adapter,
                     &EngineProjectProjectionAdapter::projectionFailed,
                     &adapter, [&](const QString&) { ++rejected; });
    adapter.projectChanged(malformed, full,
                           daw::collab::ProjectionOrigin::ConfirmedRemote);
    if (rejected != 1 || engine.project().name != "Remote rename" ||
        engine.undoDepth() != historyBefore || !engine.routingGraph() ||
        !engine.isPlaying()) {
        return fail(QStringLiteral(
            "failed collaboration graph did not roll back transactionally"));
    }

    // Bound pathological feedback from projection observers.  Eight
    // synchronous passes are enough for normal coalescing; a callback that
    // requests another document on every success must latch a failure and wait
    // for an explicit verified Snapshot before it can materialize again.
    int feedbackRequests = 0;
    daw::collab::SharedProjectDocument feedback = shared;
    const QMetaObject::Connection feedbackConnection = QObject::connect(
        &adapter, &EngineProjectProjectionAdapter::projectionSucceeded,
        &adapter, [&] {
            if (feedbackRequests >= 100) return;
            ++feedbackRequests;
            feedback.project.name =
                "Projection feedback " + std::to_string(feedbackRequests);
            adapter.projectChanged(
                feedback, full,
                daw::collab::ProjectionOrigin::ConfirmedRemote);
        });
    const int rejectedBeforeFeedback = rejected;
    adapter.projectChanged(shared, full,
                           daw::collab::ProjectionOrigin::Snapshot);
    QObject::disconnect(feedbackConnection);
    if (feedbackRequests <= 1 || feedbackRequests >= 100 ||
        rejected != rejectedBeforeFeedback + 1 || adapter.lastError().isEmpty()) {
        return fail(QStringLiteral(
            "projection callback feedback was not bounded and latched"));
    }
    const int projectedAtFeedbackFault = projected;
    adapter.projectChanged(shared, full,
                           daw::collab::ProjectionOrigin::ConfirmedRemote);
    if (projected != projectedAtFeedbackFault) {
        return fail(QStringLiteral(
            "non-snapshot projection escaped the feedback fault latch"));
    }
    adapter.projectChanged(shared, full,
                           daw::collab::ProjectionOrigin::Snapshot);
    if (projected != projectedAtFeedbackFault + 1 ||
        engine.project().name != "Remote rename" ||
        !adapter.lastError().isEmpty()) {
        return fail(QStringLiteral(
            "verified snapshot did not clear the projection feedback latch"));
    }

    // Freeze and offline history are portable media too, including inactive
    // versions needed by undo. Never retain a foreign path while hydrating.
    auto rendered = shared.project;
    auto* renderedTrack = rendered.findTrack(audioTrack);
    renderedTrack->freeze.asset = asset;
    renderedTrack->freeze.filePath = "private/foreign-freeze.wav";
    daw::OfflineRenderVersion version;
    version.id = daw::newUuid();
    version.source.asset = asset;
    version.source.filePath = "private/foreign-history.wav";
    daw::TakeModel historyTake;
    historyTake.id = daw::newUuid();
    historyTake.asset = asset;
    historyTake.asset.assetId = daw::newUuid();
    historyTake.asset.sha256 = std::string(64, 'b');
    historyTake.filePath = "private/foreign-take.wav";
    version.source.takes.push_back(historyTake);
    renderedTrack->clips.front().offlineHistory.push_back(version);
    std::vector<MissingRuntimeAsset> renderedMissing;
    std::vector<SamplerProjection> renderedSamplers;
    adapter.m_impl->resolveAssets(rendered, renderedMissing, renderedSamplers);
    if (renderedTrack->freeze.filePath != imported.localPath.toStdString() ||
        renderedTrack->clips.front().offlineHistory.back().source.filePath != imported.localPath.toStdString() ||
        !renderedTrack->clips.front().offlineHistory.back().source.takes.front().filePath.empty() ||
        std::none_of(renderedMissing.begin(), renderedMissing.end(), [&](const auto& item) {
            return item.asset.assetId == historyTake.asset.assetId;
        })) return fail(QStringLiteral("render history/freeze hydration omitted portable media or retained a foreign path"));

    daw::InsertModel external;
    external.id = daw::newUuid();
    external.format = daw::PluginFormat::Vst3;
    external.uid = "collaboration.selftest.uninstalled";
    external.vendor = "Projection selftest";
    external.pluginVersion = "1.0";
    external.path = "private/foreign-plugin.vst3";
    renderedTrack->inserts.push_back(external);
    adapter.m_impl->gateExternalStates(rendered);
    if (!renderedTrack->inserts.back().runtimeStateBlocked ||
        !adapter.m_impl->pendingProbes.isEmpty())
        return fail(QStringLiteral("unverified external plugin escaped the projection gate"));
    daw::plugins::PluginDescriptor descriptor;
    descriptor.uid = external.uid;
    descriptor.version = "1.0";
    const QString keyBefore = stateProbeIdentity(descriptor, external, 48000);
    external.stateAsset.sha256 = std::string(64, 'c');
    if (keyBefore == stateProbeIdentity(descriptor, external, 48000) ||
        stateProbeIdentity(descriptor, external, 48000) == stateProbeIdentity(descriptor, external, 96000))
        return fail(QStringLiteral("plugin probe cache ignored state identity or sample rate"));
    const int beforeStaleProbe = projected;
    adapter.m_impl->currentProbeKeys.insert(QString::fromStdString(external.id), QStringLiteral("new-state"));
    adapter.m_impl->completeProbe(QStringLiteral("old-state"), adapter.m_impl->probeGeneration, true);
    if (projected != beforeStaleProbe)
        return fail(QStringLiteral("old plugin state completion activated a replaced slot"));
    const quint64 oldGeneration = adapter.m_impl->probeGeneration;
    adapter.clearDocument();
    adapter.m_impl->completeProbe(QStringLiteral("new-state"), oldGeneration, true);
    if (adapter.hasMaterializedDocument() || !adapter.m_impl->probeResults.isEmpty())
        return fail(QStringLiteral("late plugin state completion crossed a project boundary"));
    auto stagingAsset = asset;
    stagingAsset.assetId = daw::newUuid();
    stagingAsset.sha256.clear();
    adapter.setPublicationAssetSources({
        {QString::fromStdString(stagingAsset.assetId), source},
        {QString::fromStdString(asset.assetId), source},
        {QStringLiteral("relative-path"), QStringLiteral("tone.wav")}});
    if (adapter.m_impl->resolve(stagingAsset) != source ||
        adapter.m_impl->resolve(asset) != imported.localPath ||
        adapter.m_impl->publicationAssetSources.contains(QStringLiteral("relative-path")))
        return fail(QStringLiteral("publication sources overrode verified cache or accepted a relative path"));
    auto foreignAsset = stagingAsset;
    foreignAsset.assetId = daw::newUuid();
    if (!adapter.m_impl->resolve(foreignAsset).isEmpty())
        return fail(QStringLiteral("uncaptured asset resolved a local publication file"));
    adapter.clearDocument();
    if (!adapter.m_impl->resolve(stagingAsset).isEmpty())
        return fail(QStringLiteral("publication source crossed a project boundary"));
    return true;
}

} // namespace collab
