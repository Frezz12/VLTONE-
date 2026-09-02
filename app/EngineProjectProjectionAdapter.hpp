#pragma once

#include "collaboration/EngineControllerAdapter.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace daw {
class EngineController;
}

namespace collab {

class AssetCache;

/// One shared asset that could not be resolved into the local runtime copy.
/// Locations are semantic ids only; filenames and absolute cache paths are
/// intentionally excluded from the signal surface.
struct MissingRuntimeAsset {
    daw::AssetRef asset;
    QString location;
    bool required = true;
};

/// Projects CommandGateway's optimistic materialization into EngineController.
///
/// The gateway document remains canonical and path-free.  Every callback makes
/// a disposable runtime ProjectModel, overlays this participant's session/UI
/// fields, resolves ready AssetRef values through AssetCache and transactionally
/// publishes that copy to the engine.  It never calls a legacy editing method,
/// never pushes UndoStack and never emits projectEdited().
class EngineProjectProjectionAdapter final
    : public QObject,
      public daw::collab::ProjectProjectionAdapter {
    Q_OBJECT
public:
    EngineProjectProjectionAdapter(daw::EngineController* controller,
                                   AssetCache* assetCache,
                                   QObject* parent = nullptr);
    ~EngineProjectProjectionAdapter() override;

    void projectChanged(const daw::collab::SharedProjectDocument& document,
                        const daw::collab::ChangeImpact& impact,
                        daw::collab::ProjectionOrigin origin) override;

    /// Detaches the last cloud document without touching the engine's current
    /// local project.  Late AssetCache completions are ignored until another
    /// verified snapshot is projected.
    void clearDocument();

    std::vector<MissingRuntimeAsset> missingAssets() const;
    QStringList missingAssetIds() const;
    QString lastError() const;
    bool hasMaterializedDocument() const;
    /// Metadata from the most recent projection attempt.  These accessors are
    /// intentionally read-only: they make coalesced/reentrant projection
    /// behavior observable without exposing the mutable engine transaction.
    std::optional<daw::collab::ProjectionOrigin> lastProjectionOrigin() const;
    daw::collab::ChangeImpact lastProjectionImpact() const;

signals:
    /// UI-only notification.  MainWindow refreshes existing views directly;
    /// this is deliberately not its legacy projectEdited/dirty signal.
    void projectionSucceeded();
    void projectionFailed(const QString& message);
    void missingAssetsChanged(const QStringList& assetIds);
    void missingAssetRefsChanged(const QList<daw::AssetRef>& assets);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Deterministic offscreen check used by --collaboration-selftest.
bool checkEngineProjectProjectionForTest(QString* error = nullptr);

} // namespace collab
