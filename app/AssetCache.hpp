#pragma once

#include "model/Document.hpp"

#include <QObject>
#include <QHash>
#include <QReadWriteLock>
#include <QString>

namespace collab {

struct AssetCacheResult {
    bool ok = false;
    daw::AssetRef asset;
    QString localPath;
    QString error;

    explicit operator bool() const noexcept { return ok; }
};

/// Content-addressed local storage for cloud-project assets.
///
/// Shared documents carry only AssetRef. Absolute paths are resolved at the
/// edge through this class and therefore never become part of a snapshot or a
/// collaboration command. The hashing/copy methods perform file I/O and must
/// be invoked from a worker thread, never from the audio callback.
class AssetCache final : public QObject {
    Q_OBJECT
public:
    explicit AssetCache(QString rootDirectory = {}, QObject* parent = nullptr);

    static QString defaultRootDirectory();
    QString rootDirectory() const { return m_rootDirectory; }

    /// Resolve a ready asset without reading its contents. Empty means the
    /// bytes are not present (or their declared size no longer matches).
    QString resolve(const daw::AssetRef& asset) const;
    bool contains(const daw::AssetRef& asset) const {
        return !resolve(asset).isEmpty();
    }

    /// Verify SHA-256/size, atomically copy the bytes into the cache, and bind
    /// assetId to the content hash. Empty expected.sha256 accepts the computed
    /// hash; a non-empty value is a strict integrity precondition.
    AssetCacheResult importFile(const daw::AssetRef& expected,
                                const QString& sourcePath);

    /// Record an assetId -> SHA-256 binding after another component atomically
    /// placed verified bytes at pathForHash().
    bool registerReady(const daw::AssetRef& asset, QString* error = nullptr);

    QString pathForHash(const QString& sha256) const;

signals:
    void assetReady(const QString& assetId, const QString& sha256,
                    const QString& localPath);

private:
    bool loadIndex(QString* error = nullptr);
    bool writeIndexLocked(QString* error = nullptr) const;
    static bool validHash(const QString& value);
    static bool hashFile(const QString& path, QString* hash, quint64* bytes,
                         QString* error);

    QString m_rootDirectory;
    mutable QReadWriteLock m_lock;
    QHash<QString, QString> m_assetHashes;
};

bool checkAssetCacheForTest(QString* error = nullptr);

} // namespace collab
