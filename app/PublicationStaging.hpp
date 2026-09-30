#pragma once

#include "CloudAssetTransferManager.hpp"
#include "CloudProjectPublisher.hpp"
#include "collaboration/CommandGateway.hpp"
#include "collaboration/ConditionalUndo.hpp"
#include "collaboration/SharedAssetMutationSink.hpp"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>

namespace daw::cloud { class CloudPublicationCapture; }
namespace collab {
class AssetCache;

/// Local publication journal. Every accepted edit reaches disk before its
/// optimistic projection; asset bytes reach AssetCache before their command
/// is constructed. None of the local manifest/path data is sent to the server.
/// Destruction, upload failure and cancellation intentionally retain recovery.
class PublicationStaging final : public QObject,
                                 public daw::collab::SharedMutationSink,
                                 public daw::collab::SharedAssetMutationSink {
    Q_OBJECT
public:
    PublicationStaging(AssetCache* cache, QString journalRoot,
                       QObject* parent = nullptr);
    bool begin(const QString& projectId, daw::ProjectModel captured,
               daw::collab::ProjectProjectionAdapter* adapter = nullptr);
    /// Explicit user recovery only. Never opens or joins a cloud project.
    bool restore(const QString& projectId,
                 daw::collab::ProjectProjectionAdapter* adapter = nullptr);
    bool replaceBase(const daw::collab::SharedProjectDocument& canonical);
    bool stageInitialAssets(std::shared_ptr<const daw::cloud::CloudPublicationCapture> capture);
    QVector<CloudAssetUploadInput> initialAssetSources() const { return m_initialAssets; }
    QString recoveryDirectory() const { return directory(); }
    std::optional<CloudProjectPublicationInput> recoveryPublicationInput() const;
    void setAccepting(bool accepting) noexcept { m_accepting = accepting; }
    bool accepting() const noexcept { return m_accepting; }
    QString projectId() const { return m_projectId; }
    QString lastError() const { return m_error; }
    quint64 revision() const noexcept { return m_revision; }
    int pendingAssetImports() const noexcept { return m_importing.size() + (m_initialImportPending ? 1 : 0); }
    bool initialAssetsReady() const noexcept { return m_initialAssetsReady; }
    qsizetype pendingAssetCount() const { return stagedAssets().size() + pendingAssetImports(); }
    bool hasPendingWork() const { return !pendingCommands().isEmpty() || pendingAssetCount() != 0; }
    const daw::collab::SharedProjectDocument* optimistic() const noexcept;

    QVector<daw::collab::ProjectCommand> pendingCommands() const;
    bool firstPendingCommandWasSubmitted() const;
    QVector<CloudAssetUploadInput> stagedAssets() const;
    /// Locks the first pending command's exact wire base before first send.
    /// Further retries return identical metadata even if the server advanced.
    std::optional<daw::collab::ProjectCommand> nextCommandForSubmission(quint64 serverSequence);
    bool acknowledgeCommand(const QString& operationId);
    bool acknowledgeAsset(const QString& uploadId);
    bool canUndo() const { return m_accepting && m_history.canUndo(); }
    bool canRedo() const { return m_accepting && m_history.canRedo(); }
    bool requestUndo();
    bool requestRedo();

    bool handlesCloudBinding() override { return !m_projectId.isEmpty(); }
    std::uint32_t commandSchemaVersion() const noexcept override { return 6; }
    daw::collab::SharedMutationResult submit(daw::collab::SharedMutationRequest request) override;
    daw::collab::SharedMutationResult prepare(daw::collab::SharedAssetMutationRequest request) override;
    daw::collab::SharedMutationResult setTimeSignature(int numerator, int denominator) override;
    daw::collab::SharedMutationResult setProjectKey(int root, std::string_view scale) override;
    daw::collab::SharedMutationResult setAiInstructions(std::string_view text) override;
    daw::collab::SharedMutationResult renameTrack(std::string_view id, std::string_view name) override;
    daw::collab::SharedMutationResult setTrackMuted(std::string_view id, bool muted) override;
    daw::collab::SharedMutationResult setTracksMuted(std::span<const std::string> ids, bool muted) override;
    daw::collab::SharedMutationResult clearAllMutes(std::span<const std::string> ids) override;

signals:
    void changed();
    void failed(const QString& message);
    /// Connect synchronously to controller.completeSharedAssetMutation. Its
    /// existing target/source guard then constructs the durable typed command.
    void assetPrepared(const QString& requestId, const daw::AssetRef& asset,
                       quint64 sourceRevision);

private:
    struct CommandEntry {
        daw::collab::ProjectCommand command;
        QString path;
        bool submitted = false;
        bool confirmed = false;
        std::string historyAction = "forward";
        std::string label;
    };
    struct AssetEntry { CloudAssetUploadInput input; bool confirmed = false; };
    bool reject(QString error);
    QString directory() const;
    bool writeBase(const daw::ProjectModel& project);
    bool writeCommand(const CommandEntry& entry);
    bool writeAsset(const AssetEntry& entry);
    bool submitCommand(daw::collab::ProjectCommand command, std::string label = {},
                       std::string historyAction = "forward", std::uint64_t historyToken = 0);
    bool requestHistory(bool redo);
    daw::collab::CommandMeta freshMeta() const;
    void finishAsset(quint64 generation, quint64 sourceRevision,
                     const daw::collab::SharedAssetMutationRequest& request,
                     const daw::AssetRef& asset, const QString& localPath,
                     const QString& error);
    QPointer<AssetCache> m_cache;
    QString m_root;
    QString m_projectId;
    QString m_error;
    bool m_accepting = false;
    quint64 m_generation = 0;
    quint64 m_revision = 0;
    daw::collab::ProjectProjectionAdapter* m_adapter = nullptr;
    std::unique_ptr<daw::collab::CommandGateway> m_gateway;
    QVector<CommandEntry> m_commands;
    daw::collab::ConditionalUndoHistory m_history;
    QVector<AssetEntry> m_assets;
    daw::ProjectModel m_initialBase;
    QVector<CloudAssetUploadInput> m_initialAssets;
    bool m_initialImportPending = false;
    bool m_initialAssetsReady = true;
    QSet<QString> m_importing;
    friend bool checkPublicationStagingForTest(QString* error);
};

bool checkPublicationStagingForTest(QString* error = nullptr);
} // namespace collab
