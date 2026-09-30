#include "PublicationStaging.hpp"

#include "AssetCache.hpp"
#include "ProjectSerializer.hpp"
#include "cloud/CloudPublicationCapture.hpp"
#include "collaboration/CommandJson.hpp"
#include "serialization/AssetJson.hpp"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace collab {
namespace {
using json = nlohmann::json;
using namespace daw::collab;
constexpr qsizetype maximumCommands = 4096;
constexpr qint64 maximumRecordBytes = 2 * 1024 * 1024;
bool canonicalId(const QString& id) {
    return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
bool saveJson(const QString& path, const json& value) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    const auto bytes = value.dump();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()) && file.commit();
}
json readJson(const QString& path, qint64 limit = maximumRecordBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return json();
    const auto bytes = file.readAll();
    return json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
}
CloudAssetKind uploadKind(daw::AssetKind kind) {
    if (kind == daw::AssetKind::PluginState) return CloudAssetKind::PluginState;
    if (kind == daw::AssetKind::PluginResource || kind == daw::AssetKind::Freeze) return CloudAssetKind::Other;
    return CloudAssetKind::Audio;
}
void bindInitialAssets(daw::ProjectModel& project, const std::vector<AssetCacheResult>& assets) {
    const auto bind=[&](daw::AssetRef& asset,std::string* path=nullptr){
        for(const auto& source:assets)if(!asset.assetId.empty()&&asset.assetId==source.asset.assetId){asset=source.asset;if(path)*path=source.localPath.toStdString();break;}
    };
    const auto insert=[&](daw::InsertModel& slot){bind(slot.stateAsset,&slot.stateFile);bind(slot.rightStateAsset,&slot.rightStateFile);for(auto& binding:slot.assetBindings)bind(binding.asset);};
    for(auto& slot:project.masterInserts)insert(slot);
    for(auto& track:project.tracks){insert(track.instrument);for(auto& slot:track.samplerFx.inserts)insert(slot);for(auto& slot:track.inserts)insert(slot);bind(track.freeze.asset,&track.freeze.filePath);
        for(auto& clip:track.clips){bind(clip.asset,&clip.filePath);for(auto& slot:clip.inserts)insert(slot);for(auto& take:clip.takes)bind(take.asset,&take.filePath);
            for(auto& version:clip.offlineHistory){bind(version.source.asset,&version.source.filePath);for(auto& take:version.source.takes)bind(take.asset,&take.filePath);}
        }
    }
}
} // namespace

PublicationStaging::PublicationStaging(AssetCache* cache, QString root, QObject* parent)
    : QObject(parent), m_cache(cache), m_root(std::move(root)) {}

QString PublicationStaging::directory() const { return QDir(m_root).filePath(m_projectId); }
bool PublicationStaging::reject(QString error) {
    m_error = std::move(error);
    emit failed(m_error);
    return false;
}
const SharedProjectDocument* PublicationStaging::optimistic() const noexcept {
    return m_gateway ? &m_gateway->optimistic() : nullptr;
}
bool PublicationStaging::writeBase(const daw::ProjectModel& project) {
    std::string serialized;
    if (!daw::ProjectSerializer::serializeDocument(project, serialized).isOk())
        return reject(QStringLiteral("Publication recovery document could not be serialized"));
    const auto doc = json::parse(serialized, nullptr, false);
    if (doc.is_discarded() || !saveJson(QDir(directory()).filePath(QStringLiteral("base.json")),
            {{"version", 1}, {"projectId", m_projectId.toStdString()}, {"project", doc}}))
        return reject(QStringLiteral("Publication recovery document could not be saved"));
    return true;
}
bool PublicationStaging::begin(const QString& id, daw::ProjectModel captured,
                               ProjectProjectionAdapter* adapter) {
    if (!canonicalId(id) || m_accepting || pendingAssetImports()!=0)
        return reject(QStringLiteral("Publication staging is already active or its identity is invalid"));
    const auto path = QDir(QDir(m_root).filePath(id)).filePath(QStringLiteral("base.json"));
    if (QFileInfo::exists(path))
        return reject(QStringLiteral("A publication recovery journal already exists for this project"));
    m_projectId = id;
    if (!writeBase(captured)) { m_projectId.clear(); return false; }
    ++m_generation;
    m_revision = 0;
    m_commands.clear(); m_assets.clear(); m_error.clear();
    m_initialAssets.clear();m_initialBase=captured;m_initialAssetsReady=true;
    m_history.clear();
    m_adapter = adapter;
    SharedProjectDocument base; base.project = std::move(captured);
    m_gateway = std::make_unique<CommandGateway>(std::move(base), adapter);
    m_accepting = true;
    emit changed();
    return true;
}
bool PublicationStaging::writeCommand(const CommandEntry& entry) {
    return saveJson(entry.path, {{"version", 1}, {"submitted", entry.submitted},
        {"confirmed", entry.confirmed}, {"historyAction",entry.historyAction}, {"label",entry.label},
        {"command", projectCommandToJson(entry.command)}}) ||
        reject(QStringLiteral("Publication edit could not be saved; no change was applied"));
}
bool PublicationStaging::submitCommand(ProjectCommand command, std::string label,
                                     std::string historyAction, std::uint64_t historyToken) {
    if (!m_accepting || !m_gateway || m_commands.size() >= maximumCommands)
        return reject(QStringLiteral("Publication edits are paused or the recovery queue is full"));
    auto trial = m_gateway->optimistic();
    const auto result = ProjectReducer::apply(trial, command);
    if (!result.accepted()) return reject(QString::fromStdString(result.message));
    if (result.code == ApplyCode::Duplicate) return true;
    const QString filename = QStringLiteral("commands/%1-%2.json")
        .arg(m_commands.size() + 1, 8, 10, QLatin1Char('0'))
        .arg(QString::fromStdString(command.meta.operationId));
    CommandEntry entry{command, QDir(directory()).filePath(filename)};
    entry.historyAction=std::move(historyAction);entry.label=std::move(label);
    if (serializedProjectCommandPayloadSize(command) > kMaxProjectCommandBatchBytes ||
        !writeCommand(entry)) return false;
    // No callbacks can run between durable save and reducer application.
    const auto submitted = m_gateway->submit(std::move(command));
    if (!submitted.accepted()) return reject(QStringLiteral("Saved publication edit needs recovery"));
    if(historyToken)m_history.complete(historyToken,result);
    else m_history.record(entry.command,result,entry.label);
    m_commands.push_back(std::move(entry));
    ++m_revision;
    emit changed();
    return true;
}
SharedMutationResult PublicationStaging::submit(SharedMutationRequest request) {
    if (m_projectId.isEmpty()) return SharedMutationResult::LocalFallback;
    ProjectCommand command;
    command.meta=freshMeta();
    command.meta.transactionId = request.transactionId.value_or(command.meta.operationId);
    command.body = std::move(request.body);
    return submitCommand(std::move(command),std::move(request.undoLabel)) ? SharedMutationResult::Submitted : SharedMutationResult::Blocked;
}
CommandMeta PublicationStaging::freshMeta()const{
    CommandMeta meta;meta.schemaVersion=6;meta.projectId=m_projectId.toStdString();
    meta.operationId=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();meta.transactionId=meta.operationId;return meta;
}
bool PublicationStaging::requestHistory(bool redo){
    if(!m_accepting)return false;
    auto prepared=redo?m_history.prepareRedo(freshMeta()):m_history.prepareUndo(freshMeta());
    if(!prepared)return false;
    if(submitCommand(prepared->command,prepared->label,redo?"redo":"undo",prepared->token))return true;
    m_history.cancel(prepared->token);return false;
}
bool PublicationStaging::requestUndo(){return requestHistory(false);}
bool PublicationStaging::requestRedo(){return requestHistory(true);}
QVector<ProjectCommand> PublicationStaging::pendingCommands() const {
    QVector<ProjectCommand> result;
    for (const auto& entry : m_commands) if (!entry.confirmed) result.push_back(entry.command);
    return result;
}
bool PublicationStaging::firstPendingCommandWasSubmitted() const {
    for(const auto& entry:m_commands)if(!entry.confirmed)return entry.submitted;
    return false;
}
std::optional<ProjectCommand> PublicationStaging::nextCommandForSubmission(quint64 sequence) {
    for (auto& entry : m_commands) {
        if (entry.confirmed) continue;
        if (!entry.submitted) {
            auto prepared = entry;
            prepared.command.meta.baseServerSequence = sequence;
            prepared.submitted = true;
            if (!writeCommand(prepared)) return std::nullopt;
            entry = std::move(prepared);
        }
        return entry.command;
    }
    return std::nullopt;
}
bool PublicationStaging::acknowledgeCommand(const QString& id) {
    for (auto& entry : m_commands) if (QString::fromStdString(entry.command.meta.operationId) == id) {
        if (entry.confirmed) return true;
        auto confirmed = entry; confirmed.confirmed = true;
        if (!writeCommand(confirmed)) return false;
        entry = std::move(confirmed); emit changed(); return true;
    }
    return reject(QStringLiteral("Unknown publication operation acknowledgement"));
}
bool PublicationStaging::writeAsset(const AssetEntry& entry) {
    const auto& input = entry.input;
    return saveJson(QDir(directory()).filePath(QStringLiteral("assets/") + input.uploadId + QStringLiteral(".json")),
        {{"version", 1}, {"confirmed", entry.confirmed}, {"uploadId", input.uploadId.toStdString()},
         {"assetId", input.assetId.toStdString()}, {"sourcePath", input.sourcePath.toStdString()},
         {"sha256", input.sha256.toStdString()}, {"byteSize", input.byteSize},
         {"kind", int(input.kind)}, {"contentType", input.contentType.toStdString()},
         {"displayName", input.displayName.toStdString()}}) ||
        reject(QStringLiteral("Staged publication asset could not be saved"));
}
QVector<CloudAssetUploadInput> PublicationStaging::stagedAssets() const {
    QVector<CloudAssetUploadInput> result;
    for (const auto& entry : m_assets) if (!entry.confirmed) result.push_back(entry.input);
    return result;
}
bool PublicationStaging::acknowledgeAsset(const QString& id) {
    for (auto& entry : m_assets) if (entry.input.uploadId == id) {
        if (entry.confirmed) return true;
        auto confirmed = entry; confirmed.confirmed = true;
        if (!writeAsset(confirmed)) return false;
        entry = std::move(confirmed); emit changed(); return true;
    }
    return reject(QStringLiteral("Unknown staged asset acknowledgement"));
}
SharedMutationResult PublicationStaging::prepare(SharedAssetMutationRequest request) {
    const QString requestId = QString::fromStdString(request.requestId);
    if (!m_accepting || !m_cache || !canonicalId(requestId) ||
        !canonicalId(QString::fromStdString(request.assetId)) || request.sourcePath.empty() ||
        request.kind == daw::AssetKind::Unknown || m_importing.contains(requestId) ||
        m_importing.size() + m_assets.size() >= 1024) {
        reject(QStringLiteral("Publication asset could not be staged"));
        return SharedMutationResult::Blocked;
    }
    m_importing.insert(requestId);
    const auto generation = m_generation, revision = m_revision;
    QPointer<PublicationStaging> guard(this);
    QPointer<AssetCache> cache(m_cache);
    QThreadPool::globalInstance()->start([guard, cache, generation, revision, request = std::move(request)] {
        if (!cache) return;
        daw::AssetRef expected;
        expected.assetId = request.assetId; expected.kind = request.kind;
        expected.originalName = QFileInfo(QString::fromStdString(request.displayName)).fileName().toStdString();
        expected.mimeType = request.contentType; expected.codec = request.codec;
        expected.sampleRate = request.sampleRate; expected.channels = request.channels; expected.frames = request.frames;
        const auto result = cache->importFile(expected, QString::fromStdString(request.sourcePath));
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, revision, request, result] {
            if (guard) guard->finishAsset(generation, revision, request, result.asset, result.localPath,
                                         result ? QString() : result.error);
        }, Qt::QueuedConnection);
    });
    emit changed();
    return SharedMutationResult::Submitted;
}
void PublicationStaging::finishAsset(quint64 generation, quint64 revision,
                                     const SharedAssetMutationRequest& request,
                                     const daw::AssetRef& asset, const QString& path,
                                     const QString& error) {
    if (generation != m_generation) return;
    const auto requestId = QString::fromStdString(request.requestId);
    m_importing.remove(requestId);
    if (!error.isEmpty() || path.isEmpty()) { reject(error.isEmpty() ? QStringLiteral("Asset import failed") : error); emit changed(); return; }
    CloudAssetUploadInput input;
    input.projectId = m_projectId; input.uploadId = requestId;
    input.assetId = QString::fromStdString(asset.assetId); input.sourcePath = path;
    input.sha256 = QString::fromStdString(asset.sha256); input.byteSize = asset.byteSize;
    input.kind = uploadKind(asset.kind); input.contentType = QString::fromStdString(request.contentType);
    input.displayName = QString::fromStdString(asset.originalName);
    AssetEntry entry{input};
    if (!writeAsset(entry)) { emit changed(); return; }
    m_assets.push_back(std::move(entry));
    // Pausing during a copy preserves bytes but prevents an unexpected late edit.
    if (m_accepting) emit assetPrepared(requestId, asset, revision);
    emit changed();
}
bool PublicationStaging::replaceBase(const SharedProjectDocument& canonical) {
    if (!m_gateway || canonical.confirmedSequence != 0)
        return reject(QStringLiteral("Publication base must be the verified initial snapshot"));
    auto replacement = std::make_unique<CommandGateway>(canonical);
    for (const auto& entry : m_commands) if (!replacement->submit(entry.command).accepted())
        return reject(QStringLiteral("Publication edits do not apply to the verified snapshot"));
    if (!writeBase(canonical.project)) return false;
    m_initialBase=canonical.project;
    replacement->setAdapter(m_adapter); m_gateway = std::move(replacement);
    if (m_adapter && m_accepting) { ChangeImpact impact; impact.documentChanged = impact.fullProjection = true;
        m_adapter->projectChanged(m_gateway->optimistic(), impact, ProjectionOrigin::Rebase); }
    emit changed(); return true;
}
bool PublicationStaging::restore(const QString& id, ProjectProjectionAdapter* adapter) {
    if (!canonicalId(id) || m_accepting || pendingAssetImports()!=0) return reject(QStringLiteral("Cannot restore publication while edits are active"));
    const auto dir = QDir(QDir(m_root).filePath(id));
    const auto base = readJson(dir.filePath(QStringLiteral("base.json")), 128 * 1024 * 1024);
    try {
        if (!base.is_object() || base.at("version") != 1 || base.at("projectId") != id.toStdString())
            return reject(QStringLiteral("Publication recovery identity is invalid"));
        SharedProjectDocument document;
        if (!daw::ProjectSerializer::deserializeDocument(document.project, base.at("project").dump()).isOk())
            return reject(QStringLiteral("Publication recovery document is invalid"));
        QVector<CloudAssetUploadInput> initialAssets;
        std::vector<AssetCacheResult> initialBindings;
        const auto initialPath = dir.filePath(QStringLiteral("initial-assets.json"));
        const auto initialRecord = readJson(initialPath, 128 * 1024 * 1024);
        if (initialRecord.is_object()) {
            if (initialRecord.at("version") != 1 || initialRecord.value("pending", false) ||
                !initialRecord.at("assets").is_array() || initialRecord.at("assets").size() > 10000)
                return reject(QStringLiteral("Initial publication asset copying did not complete; the local project is required to retry"));
            for (const auto& entry : initialRecord.at("assets")) {
                const auto asset = daw::serialization::assetRefFromJson(entry.at("asset"));
                CloudAssetUploadInput input;
                input.projectId = id;
                input.uploadId = QString::fromStdString(asset.assetId);
                input.assetId = input.uploadId;
                input.sourcePath = QString::fromStdString(entry.at("sourcePath").get<std::string>());
                input.sha256 = QString::fromStdString(asset.sha256);
                input.byteSize = asset.byteSize;
                input.kind = uploadKind(asset.kind);
                input.contentType = asset.mimeType.empty() ? QStringLiteral("application/octet-stream") : QString::fromStdString(asset.mimeType);
                input.displayName = QString::fromStdString(asset.originalName);
                if (!canonicalId(input.assetId) || input.sha256.size() != 64 || !input.byteSize ||
                    !QFileInfo::exists(input.sourcePath))
                    return reject(QStringLiteral("An initial publication asset is invalid or its cached bytes are unavailable"));
                initialBindings.push_back({true, asset, input.sourcePath, {}});
                initialAssets.push_back(std::move(input));
            }
            // The manifest is committed before base.json. Rebinding here also
            // recovers a process exit between those two atomic writes.
            bindInitialAssets(document.project, initialBindings);
        } else if (QFileInfo::exists(initialPath)) {
            return reject(QStringLiteral("Initial publication assets could not be restored"));
        }
        const auto initialBase=document.project;
        auto gateway = std::make_unique<CommandGateway>(std::move(document));
        ConditionalUndoHistory history;
        QVector<CommandEntry> commands; QVector<AssetEntry> assets;
        const QDir commandDir(dir.filePath(QStringLiteral("commands")));
        const auto files = commandDir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        if (files.size() > maximumCommands) return reject(QStringLiteral("Publication recovery queue exceeds its limit"));
        for (const auto& file : files) {
            const auto record = readJson(commandDir.filePath(file));
            auto command = projectCommandFromJson(record.at("command"));
            if (!command || record.at("version") != 1) return reject(QStringLiteral("Publication recovery command is invalid"));
            command->meta.projectId = id.toStdString();
            const auto result=gateway->submit(*command);
            if (!result.accepted()) return reject(QStringLiteral("Publication recovery command cannot be replayed"));
            const auto action=record.value("historyAction",std::string("forward"));
            const auto label=record.value("label",std::string());
            if(action=="forward")history.record(*command,result.apply,label);
            else {
                if(action!="undo"&&action!="redo")return reject(QStringLiteral("Invalid publication recovery history"));
                auto prepared=action=="undo"?history.prepareUndo(command->meta):history.prepareRedo(command->meta);
                if(!prepared||projectCommandToJson(prepared->command)!=projectCommandToJson(*command)||!history.complete(prepared->token,result.apply))
                    return reject(QStringLiteral("Publication recovery undo history is inconsistent"));
            }
            commands.push_back({*command, commandDir.filePath(file), record.at("submitted").get<bool>(), record.at("confirmed").get<bool>(),action,label});
        }
        const QDir assetDir(dir.filePath(QStringLiteral("assets")));
        const auto assetFiles = assetDir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        if (assetFiles.size() > 1024) return reject(QStringLiteral("Publication recovery asset queue exceeds its limit"));
        for (const auto& file : assetFiles) {
            const auto record = readJson(assetDir.filePath(file));
            CloudAssetUploadInput input; input.projectId = id;
            input.uploadId = QString::fromStdString(record.at("uploadId").get<std::string>());
            input.assetId = QString::fromStdString(record.at("assetId").get<std::string>());
            input.sourcePath = QString::fromStdString(record.at("sourcePath").get<std::string>());
            input.sha256 = QString::fromStdString(record.at("sha256").get<std::string>());
            input.byteSize = record.at("byteSize").get<quint64>();
            const int kind = record.at("kind").get<int>();
            if (record.at("version") != 1 || !canonicalId(input.uploadId) || !canonicalId(input.assetId) ||
                input.sha256.size() != 64 || !input.byteSize || kind < 0 || kind > int(CloudAssetKind::Other))
                return reject(QStringLiteral("Publication recovery asset is invalid"));
            input.kind = CloudAssetKind(kind);
            input.contentType = QString::fromStdString(record.at("contentType").get<std::string>());
            input.displayName = QString::fromStdString(record.at("displayName").get<std::string>());
            assets.push_back({input, record.at("confirmed").get<bool>()});
        }
        ++m_generation; m_projectId = id; m_adapter = adapter;
        m_initialBase=initialBase;m_initialAssets=std::move(initialAssets);m_initialAssetsReady=true;
        m_revision = quint64(commands.size()); m_commands = std::move(commands); m_assets = std::move(assets);
        m_history=std::move(history);
        gateway->setAdapter(adapter); m_gateway = std::move(gateway); m_accepting = true; m_error.clear();
        if (adapter) { ChangeImpact impact; impact.documentChanged = impact.fullProjection = true;
            adapter->projectChanged(m_gateway->optimistic(), impact, ProjectionOrigin::Snapshot); }
        emit changed(); return true;
    } catch (const json::exception&) { return reject(QStringLiteral("Publication recovery journal is incomplete or corrupted")); }
}

bool PublicationStaging::stageInitialAssets(std::shared_ptr<const daw::cloud::CloudPublicationCapture> capture){
    if(!capture||!m_gateway||!m_cache||m_initialImportPending||capture->sources.size()>10000)
        return reject(QStringLiteral("Initial publication assets cannot be staged"));
    if (!saveJson(QDir(directory()).filePath(QStringLiteral("initial-assets.json")),
                  {{"version", 1}, {"pending", true}, {"assets", json::array()}}))
        return reject(QStringLiteral("Initial publication asset recovery could not be started"));
    m_initialImportPending=true;m_initialAssetsReady=false;const auto generation=m_generation;
    const auto manifestPath=QDir(directory()).filePath(QStringLiteral("initial-assets.json"));
    QPointer<PublicationStaging> guard(this);QPointer<AssetCache> cache(m_cache);
    QThreadPool::globalInstance()->start([guard,cache,generation,manifestPath,capture=std::move(capture)]{
        std::vector<AssetCacheResult> imported;QString error;
        if(!cache)error=QStringLiteral("Publication asset cache is unavailable");
        else for(const auto& source:capture->sources){auto result=cache->importFile(source.asset,QString::fromStdString(source.localPath));if(!result){error=result.error;break;}imported.push_back(std::move(result));}
        if(error.isEmpty()){
            json records=json::array();
            for(const auto& result:imported)records.push_back({{"asset",daw::serialization::assetRefToJson(result.asset)},{"sourcePath",result.localPath.toStdString()}});
            // Commit from the worker before releasing the capture, even if
            // the window closed and its queued callback cannot run anymore.
            if(!saveJson(manifestPath,{{"version",1},{"pending",false},{"assets",records}}))
                error=QStringLiteral("Initial publication assets could not be saved");
        }
        if(!guard)return;
        QMetaObject::invokeMethod(guard,[guard,generation,imported=std::move(imported),error,capture]{
            if(!guard||guard->m_generation!=generation)return;
            auto& self=*guard;
            if(!error.isEmpty()){self.m_initialImportPending=false;self.reject(error);emit self.changed();return;}
            QVector<CloudAssetUploadInput> inputs;
            for(const auto& result:imported){
                CloudAssetUploadInput input;input.projectId=self.m_projectId;input.uploadId=QString::fromStdString(result.asset.assetId);input.assetId=input.uploadId;input.sourcePath=result.localPath;input.sha256=QString::fromStdString(result.asset.sha256);input.byteSize=result.asset.byteSize;input.kind=uploadKind(result.asset.kind);input.contentType=result.asset.mimeType.empty()?QStringLiteral("application/octet-stream"):QString::fromStdString(result.asset.mimeType);input.displayName=QString::fromStdString(result.asset.originalName);inputs.push_back(std::move(input));}
            auto base=self.m_initialBase;bindInitialAssets(base,imported);
            SharedProjectDocument document;document.project=std::move(base);
            self.m_initialAssets=std::move(inputs);
            if(!self.replaceBase(document)){self.m_initialImportPending=false;emit self.changed();return;}
            self.m_initialAssetsReady=true;
            self.m_initialImportPending=false;
            emit self.changed();
        },Qt::QueuedConnection);
    });
    emit changed();return true;
}

std::optional<CloudProjectPublicationInput> PublicationStaging::recoveryPublicationInput() const {
    if (!m_gateway || !m_initialAssetsReady || pendingAssetImports() != 0) return std::nullopt;
    const auto metadata=readJson(QDir(directory()).filePath(QStringLiteral("publication-metadata.json")));
    try {
        if (!metadata.is_object() || metadata.at("version") != 1 || metadata.at("projectId") != m_projectId.toStdString()) return std::nullopt;
        CloudProjectPublicationInput input;
        input.project=m_initialBase;
        input.metadata.projectId=m_projectId;
        input.metadata.title=QString::fromStdString(metadata.at("title").get<std::string>());
        input.metadata.engineVersion=QString::fromStdString(metadata.at("engineVersion").get<std::string>());
        input.metadata.minimumAppVersion=QString::fromStdString(metadata.at("minimumAppVersion").get<std::string>());
        input.metadata.pluginPolicy=QString::fromStdString(metadata.at("pluginPolicy").get<std::string>());
        input.metadata.formatVersion=metadata.at("formatVersion").get<int>();
        input.recoveryDirectory=directory();
        for(const auto& asset:m_initialAssets)
            input.assetSources.push_back({asset.assetId,asset.sourcePath,asset.contentType});
        return input;
    } catch(const json::exception&) { return std::nullopt; }
}

SharedMutationResult PublicationStaging::setTimeSignature(int numerator,int denominator) { return submit({SetTimeSignature{numerator,denominator},{},{}}); }
SharedMutationResult PublicationStaging::setProjectKey(int root,std::string_view scale) { return submit({SetProjectKey{root,std::string(scale)},{},{}}); }
SharedMutationResult PublicationStaging::setAiInstructions(std::string_view text) { return submit({SetProjectScalar{ProjectScalar::AiInstructions,std::string(text)},{},{}}); }
SharedMutationResult PublicationStaging::renameTrack(std::string_view id,std::string_view name) { return submit({SetTrackProperty{std::string(id),TrackProperty::Name,std::string(name)},{},{}}); }
SharedMutationResult PublicationStaging::setTrackMuted(std::string_view,bool) { return SharedMutationResult::Blocked; }
SharedMutationResult PublicationStaging::setTracksMuted(std::span<const std::string>,bool) { return SharedMutationResult::Blocked; }
SharedMutationResult PublicationStaging::clearAllMutes(std::span<const std::string> ids) { return setTracksMuted(ids,false); }

bool checkPublicationStagingForTest(QString* error) {
    const auto fail=[error](const QString& text){if(error)*error=text;return false;};
    QTemporaryDir dir;if(!dir.isValid())return fail(QStringLiteral("staging test directory unavailable"));
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    PublicationStaging staging(nullptr,dir.path()); daw::ProjectModel project;
    if(!staging.begin(id,project)||staging.setTimeSignature(7,8)!=SharedMutationResult::Submitted||
       staging.setAiInstructions("retained edit")!=SharedMutationResult::Submitted)return fail(staging.lastError());
    const auto commands=staging.pendingCommands();
    const auto first=staging.nextCommandForSubmission(0);const auto retry=staging.nextCommandForSubmission(9);
    if(!first||!retry||serializeProjectCommand(*first)!=serializeProjectCommand(*retry))return fail(QStringLiteral("staged retry changed wire identity"));
    if(!staging.acknowledgeCommand(QString::fromStdString(first->meta.operationId)))return fail(staging.lastError());
    staging.setAccepting(false);
    if(staging.setTimeSignature(3,4)!=SharedMutationResult::Blocked||staging.pendingCommands().size()!=1)return fail(QStringLiteral("paused staging accepted an edit"));
    PublicationStaging recovered(nullptr,dir.path());
    if(!recovered.restore(id)||recovered.pendingCommands().size()!=1||recovered.optimistic()->project.aiInstructions!="retained edit")return fail(recovered.lastError());
    SharedProjectDocument canonical;canonical.project=project;
    if(!recovered.replaceBase(canonical)||recovered.pendingCommands().front().meta.operationId!=commands.back().meta.operationId)return fail(QStringLiteral("snapshot handoff discarded publication edits"));
    if(!recovered.canUndo()||!recovered.requestUndo()||!recovered.canRedo()||!recovered.optimistic()->project.aiInstructions.empty())return fail(QStringLiteral("publication conditional undo failed"));
    recovered.setAccepting(false);
    PublicationStaging historyRecovery(nullptr,dir.path());
    if(!historyRecovery.restore(id)||!historyRecovery.canRedo()||!historyRecovery.requestRedo()||historyRecovery.optimistic()->project.aiInstructions!="retained edit")return fail(QStringLiteral("publication redo was lost across recovery"));
    if(!saveJson(QDir(QDir(dir.path()).filePath(id)).filePath(QStringLiteral("commands/99999999-broken.json")),{{"broken",true}}))return fail(QStringLiteral("could not write recovery fixture"));
    PublicationStaging corrupt(nullptr,dir.path());if(corrupt.restore(id))return fail(QStringLiteral("corrupt recovery was silently skipped"));
    AssetCache cache(dir.filePath(QStringLiteral("cache")));
    PublicationStaging assetStaging(&cache,dir.path());
    const auto assetProjectId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    daw::TrackModel track;track.id=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    project.tracks.push_back(track);
    if(!assetStaging.begin(assetProjectId,project))return fail(assetStaging.lastError());
    const std::vector<std::string> trackIds{track.id};
    if(assetStaging.setTrackMuted(track.id,true)!=SharedMutationResult::Blocked||
       assetStaging.setTracksMuted(trackIds,true)!=SharedMutationResult::Blocked||
       assetStaging.clearAllMutes(trackIds)!=SharedMutationResult::Blocked||
       !assetStaging.pendingCommands().isEmpty())
        return fail(QStringLiteral("publication audition mute became a durable document edit"));
    const QString source=dir.filePath(QStringLiteral("source.wav"));
    QFile sourceFile(source);if(!sourceFile.open(QIODevice::WriteOnly)||sourceFile.write("rendered-test-audio")!=19)return fail(QStringLiteral("staging source fixture failed"));sourceFile.close();
    SharedAssetMutationRequest request;request.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();request.assetId=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();request.kind=daw::AssetKind::Audio;request.sourcePath=source.toStdString();request.displayName="render.wav";request.contentType="audio/wav";
    QEventLoop loop;bool assetSubmitted=false;
    QObject::connect(&assetStaging,&PublicationStaging::assetPrepared,&loop,[&](const QString&,const daw::AssetRef& asset,quint64){
        assetSubmitted=cache.contains(asset)&&assetStaging.submit({SetTrackFreeze{track.id,asset,1,48000},{},{}})==SharedMutationResult::Submitted;loop.quit();
    });
    if(assetStaging.prepare(request)!=SharedMutationResult::Submitted)return fail(assetStaging.lastError());
    QTimer::singleShot(5000,&loop,&QEventLoop::quit);loop.exec();
    if(!assetSubmitted||assetStaging.pendingAssetCount()!=1||assetStaging.pendingCommands().size()!=1)return fail(QStringLiteral("asset edit was not journaled after durable cache import"));
    QFile::remove(source);assetStaging.setAccepting(false);
    PublicationStaging assetRecovery(&cache,dir.path());
    if(!assetRecovery.restore(assetProjectId)||assetRecovery.stagedAssets().size()!=1||!QFileInfo::exists(assetRecovery.stagedAssets().front().sourcePath)||assetRecovery.pendingCommands().size()!=1)return fail(QStringLiteral("asset source or mutation lost after publication interruption"));
    if(!assetRecovery.acknowledgeAsset(QString::fromStdString(request.requestId))||assetRecovery.pendingAssetCount()!=0)return fail(QStringLiteral("verified asset was not retired"));
    // A crash may leave the complete asset manifest beside the older base.
    // Recovery must replace temporary paths before replaying journaled edits.
    const auto cachedAsset=std::get<SetTrackFreeze>(assetRecovery.pendingCommands().front().body).asset;
    const auto initialId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto initialProject=project;
    initialProject.tracks.front().freeze.asset=cachedAsset;
    initialProject.tracks.front().freeze.asset.sha256.clear();
    initialProject.tracks.front().freeze.filePath="removed-private-plugin-capture";
    PublicationStaging initial(nullptr,dir.path());
    if(!initial.begin(initialId,initialProject))return fail(initial.lastError());
    initial.setAccepting(false);
    const auto initialPath=QDir(QDir(dir.path()).filePath(initialId)).filePath(QStringLiteral("initial-assets.json"));
    if(!saveJson(initialPath,{{"version",1},{"pending",false},{"assets",json::array({{{"asset",daw::serialization::assetRefToJson(cachedAsset)},{"sourcePath",cache.resolve(cachedAsset).toStdString()}}})}}))
        return fail(QStringLiteral("could not write initial asset recovery fixture"));
    PublicationStaging initialRecovery(&cache,dir.path());
    if(!initialRecovery.restore(initialId)||initialRecovery.initialAssetSources().size()!=1||
       initialRecovery.optimistic()->project.tracks.front().freeze.asset.sha256!=cachedAsset.sha256||
       initialRecovery.optimistic()->project.tracks.front().freeze.filePath!=cache.resolve(cachedAsset).toStdString())
        return fail(QStringLiteral("initial publication assets did not recover from the committed manifest"));
    if(!saveJson(QDir(initialRecovery.directory()).filePath(QStringLiteral("publication-metadata.json")),
        {{"version",1},{"projectId",initialId.toStdString()},{"title","Original publication"},
         {"engineVersion","fixture-engine"},{"minimumAppVersion","1.0.0"},{"formatVersion",7},{"pluginPolicy","builtin_only"}}))
        return fail(QStringLiteral("could not save publication recovery metadata fixture"));
    const auto recoveredInput=initialRecovery.recoveryPublicationInput();
    if(!recoveredInput||recoveredInput->metadata.projectId!=initialId||recoveredInput->assetSources.size()!=1||
       recoveredInput->assetSources.front().sourcePath!=cache.resolve(cachedAsset)||
       recoveredInput->metadata.title!=QStringLiteral("Original publication"))
        return fail(QStringLiteral("restarted publication lost its original identity or durable source"));
    initialRecovery.setAccepting(false);
    if(!saveJson(initialPath,{{"version",1},{"pending",true},{"assets",json::array()}})||initialRecovery.restore(initialId))
        return fail(QStringLiteral("unfinished initial asset copy was silently accepted"));
    return true;
}
} // namespace collab
