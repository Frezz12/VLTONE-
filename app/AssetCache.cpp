#include "AssetCache.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <utility>

namespace collab {
namespace {

constexpr qsizetype kCopyBlockBytes = 1024 * 1024;

QString indexPath(const QString& root) {
    return QDir(root).filePath(QStringLiteral("index-v1.json"));
}

QString ioError(const QString& operation, const QFileDevice& file) {
    return QStringLiteral("%1: %2").arg(operation, file.errorString());
}

} // namespace

AssetCache::AssetCache(QString rootDirectory, QObject* parent)
    : QObject(parent),
      m_rootDirectory(rootDirectory.isEmpty() ? defaultRootDirectory()
                                               : std::move(rootDirectory)) {
    QDir().mkpath(QDir(m_rootDirectory).filePath(QStringLiteral("blobs")));
    loadIndex();
}

QString AssetCache::defaultRootDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("collaboration/assets-v1"));
}

bool AssetCache::validHash(const QString& value) {
    if (value.size() != 64)
        return false;
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') ||
              (code >= 'a' && code <= 'f')))
            return false;
    }
    return true;
}

QString AssetCache::pathForHash(const QString& sha256) const {
    const QString hash = sha256.trimmed().toLower();
    if (!validHash(hash))
        return {};
    return QDir(m_rootDirectory)
        .filePath(QStringLiteral("blobs/%1/%2").arg(hash.left(2), hash));
}

QString AssetCache::resolve(const daw::AssetRef& asset) const {
    QString hash = QString::fromStdString(asset.sha256).trimmed().toLower();
    {
        QReadLocker locker(&m_lock);
        if (!validHash(hash) && !asset.assetId.empty())
            hash = m_assetHashes.value(QString::fromStdString(asset.assetId));
    }
    const QString path = pathForHash(hash);
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    if (asset.byteSize > 0 && quint64(info.size()) != asset.byteSize)
        return {};
    return info.absoluteFilePath();
}

bool AssetCache::hashFile(const QString& path, QString* hash, quint64* bytes,
                          QString* error) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = ioError(QStringLiteral("Open asset"), input);
        return false;
    }

    QCryptographicHash digest(QCryptographicHash::Sha256);
    quint64 total = 0;
    QByteArray block;
    block.resize(kCopyBlockBytes);
    while (true) {
        const qint64 count = input.read(block.data(), block.size());
        if (count < 0) {
            if (error)
                *error = ioError(QStringLiteral("Read asset"), input);
            return false;
        }
        if (count == 0)
            break;
        digest.addData(QByteArrayView(block.constData(), count));
        total += quint64(count);
    }
    if (hash)
        *hash = QString::fromLatin1(digest.result().toHex());
    if (bytes)
        *bytes = total;
    return true;
}

AssetCacheResult AssetCache::importFile(const daw::AssetRef& expected,
                                        const QString& sourcePath) {
    AssetCacheResult result;
    result.asset = expected;

    QString computedHash;
    quint64 computedBytes = 0;
    if (!hashFile(sourcePath, &computedHash, &computedBytes, &result.error))
        return result;

    const QString expectedHash =
        QString::fromStdString(expected.sha256).trimmed().toLower();
    if (!expectedHash.isEmpty() &&
        (!validHash(expectedHash) || expectedHash != computedHash)) {
        result.error = QStringLiteral("Asset checksum mismatch");
        return result;
    }
    if (expected.byteSize > 0 && expected.byteSize != computedBytes) {
        result.error = QStringLiteral("Asset size mismatch");
        return result;
    }

    result.asset.sha256 = computedHash.toStdString();
    result.asset.byteSize = computedBytes;
    const QString destination = pathForHash(computedHash);
    const QFileInfo destinationInfo(destination);
    if (!QDir().mkpath(destinationInfo.absolutePath())) {
        result.error = QStringLiteral("Could not create the asset cache directory");
        return result;
    }

    bool destinationReady = false;
    if (destinationInfo.isFile()) {
        QString existingHash;
        quint64 existingBytes = 0;
        destinationReady = hashFile(destination, &existingHash, &existingBytes,
                                    &result.error) &&
                           existingHash == computedHash &&
                           existingBytes == computedBytes;
        // A partial/corrupt cache entry is recoverable: QSaveFile below
        // atomically replaces it after verifying the bytes copied from source.
        if (!destinationReady)
            result.error.clear();
    }
    if (!destinationReady) {
        QSaveFile output(destination);
        output.setDirectWriteFallback(false);
        QFile input(sourcePath);
        if (!input.open(QIODevice::ReadOnly)) {
            result.error = ioError(QStringLiteral("Open asset"), input);
            return result;
        }
        if (!output.open(QIODevice::WriteOnly)) {
            result.error = ioError(QStringLiteral("Create cached asset"), output);
            return result;
        }
        QByteArray block;
        block.resize(kCopyBlockBytes);
        QCryptographicHash copiedDigest(QCryptographicHash::Sha256);
        quint64 copiedBytes = 0;
        destinationReady = true;
        while (true) {
            const qint64 count = input.read(block.data(), block.size());
            if (count < 0) {
                result.error = ioError(QStringLiteral("Read asset"), input);
                destinationReady = false;
                break;
            }
            if (count == 0)
                break;
            if (output.write(block.constData(), count) != count) {
                result.error = ioError(QStringLiteral("Write cached asset"), output);
                destinationReady = false;
                break;
            }
            copiedDigest.addData(QByteArrayView(block.constData(), count));
            copiedBytes += quint64(count);
        }
        if (destinationReady &&
            (copiedBytes != computedBytes ||
             QString::fromLatin1(copiedDigest.result().toHex()) != computedHash)) {
            result.error = QStringLiteral(
                "Asset changed while it was being copied into the cache");
            destinationReady = false;
        }
        if (destinationReady && !output.commit()) {
            result.error = ioError(QStringLiteral("Commit cached asset"), output);
            destinationReady = false;
        }
        if (!destinationReady)
            output.cancelWriting();
    }
    if (!destinationReady)
        return result;

    if (!registerReady(result.asset, &result.error))
        return result;

    result.ok = true;
    result.localPath = destination;
    emit assetReady(QString::fromStdString(result.asset.assetId), computedHash,
                    destination);
    return result;
}

bool AssetCache::registerReady(const daw::AssetRef& asset, QString* error) {
    const QString hash = QString::fromStdString(asset.sha256).trimmed().toLower();
    if (!validHash(hash)) {
        if (error)
            *error = QStringLiteral("Invalid SHA-256 asset identity");
        return false;
    }
    const QFileInfo info(pathForHash(hash));
    if (!info.isFile() ||
        (asset.byteSize > 0 && quint64(info.size()) != asset.byteSize)) {
        if (error)
            *error = QStringLiteral("Asset bytes are not ready in the cache");
        return false;
    }

    const QString assetId = QString::fromStdString(asset.assetId);
    if (assetId.isEmpty())
        return true;

    QWriteLocker locker(&m_lock);
    const auto previous = m_assetHashes.constFind(assetId);
    if (previous != m_assetHashes.cend() && previous.value() != hash) {
        if (error)
            *error = QStringLiteral("Asset id is already bound to different bytes");
        return false;
    }
    m_assetHashes.insert(assetId, hash);
    return writeIndexLocked(error);
}

bool AssetCache::loadIndex(QString* error) {
    QFile input(indexPath(m_rootDirectory));
    if (!input.exists())
        return true;
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = ioError(QStringLiteral("Open asset index"), input);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid asset cache index");
        return false;
    }

    const QJsonObject assets = document.object().value(QStringLiteral("assets")).toObject();
    QWriteLocker locker(&m_lock);
    for (auto it = assets.constBegin(); it != assets.constEnd(); ++it) {
        const QString hash = it.value().toString().toLower();
        if (!it.key().isEmpty() && validHash(hash) && QFileInfo::exists(pathForHash(hash)))
            m_assetHashes.insert(it.key(), hash);
    }
    return true;
}

bool AssetCache::writeIndexLocked(QString* error) const {
    QJsonObject assets;
    for (auto it = m_assetHashes.constBegin(); it != m_assetHashes.constEnd(); ++it)
        assets.insert(it.key(), it.value());
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("assets"), assets);

    if (!QDir().mkpath(m_rootDirectory)) {
        if (error)
            *error = QStringLiteral("Could not create the asset cache directory");
        return false;
    }
    QSaveFile output(indexPath(m_rootDirectory));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error)
            *error = ioError(QStringLiteral("Create asset index"), output);
        return false;
    }
    if (output.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0) {
        if (error)
            *error = ioError(QStringLiteral("Write asset index"), output);
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        if (error)
            *error = ioError(QStringLiteral("Commit asset index"), output);
        return false;
    }
    return true;
}

bool checkAssetCacheForTest(QString* error) {
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        if (error) *error = QStringLiteral("Could not create an asset-cache fixture");
        return false;
    }

    const QByteArray bytes("VLT asset cache selftest\n");
    const QString sourcePath = QDir(temporary.path()).filePath(QStringLiteral("source.wav"));
    QFile source(sourcePath);
    if (!source.open(QIODevice::WriteOnly) || source.write(bytes) != bytes.size()) {
        if (error) *error = QStringLiteral("Could not write an asset-cache fixture");
        return false;
    }
    source.close();

    daw::AssetRef expected;
    expected.assetId = "11111111-1111-4111-8111-111111111111";
    expected.sha256 =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
            .toHex().toStdString();
    expected.byteSize = quint64(bytes.size());
    expected.kind = daw::AssetKind::Audio;

    const QString cachePath =
        QDir(temporary.path()).filePath(QStringLiteral("cache"));
    AssetCache cache(cachePath);
    const AssetCacheResult imported = cache.importFile(expected, sourcePath);
    if (!imported || imported.localPath.isEmpty() ||
        cache.resolve(expected) != imported.localPath) {
        if (error)
            *error = imported.error.isEmpty()
                         ? QStringLiteral("Imported asset did not resolve")
                         : imported.error;
        return false;
    }

    AssetCache reopened(cachePath);
    if (reopened.resolve(expected) != imported.localPath) {
        if (error) *error = QStringLiteral("Asset cache index did not round-trip");
        return false;
    }

    daw::AssetRef mismatched = expected;
    mismatched.sha256.assign(64, '0');
    const AssetCacheResult rejected = reopened.importFile(mismatched, sourcePath);
    if (rejected.ok || rejected.error.isEmpty()) {
        if (error) *error = QStringLiteral("Checksum mismatch was not rejected");
        return false;
    }
    return true;
}

} // namespace collab
