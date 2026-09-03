#include "CloudProjectCache.hpp"

#include "ProjectSerializer.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <cmath>
#include <initializer_list>
#include <utility>

namespace collab {
namespace {

constexpr int kCacheSchemaVersion = 1;
constexpr quint64 kLargestExactJsonInteger = 9007199254740991ULL;

QString canonicalUuid(const QString& value) {
    const QUuid uuid(value);
    return uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
}

bool validHash(const QString& value) {
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') ||
              (code >= 'a' && code <= 'f'))) return false;
    }
    return true;
}

bool exactKeys(const QJsonObject& object,
               std::initializer_list<const char*> keys) {
    if (object.size() != qsizetype(keys.size())) return false;
    for (const char* key : keys) {
        if (!object.contains(QString::fromLatin1(key))) return false;
    }
    return true;
}

std::optional<quint64> exactUnsigned(const QJsonValue& value) {
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble(-1.0);
    if (!std::isfinite(number) || number < 0.0 ||
        number > double(kLargestExactJsonInteger) ||
        std::floor(number) != number) return std::nullopt;
    return quint64(number);
}

QString digest(const std::string& bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QByteArray(bytes.data(), qsizetype(bytes.size())),
            QCryptographicHash::Sha256).toHex());
}

void setError(QString* error, const QString& message) {
    if (error) *error = message;
}

QString snapshotName(quint64 sequence, const QString& hash) {
    return QStringLiteral("%1-%2.snapshot").arg(sequence).arg(hash);
}

} // namespace

CloudProjectCache::CloudProjectCache(QString rootDirectory)
    : m_rootDirectory(rootDirectory.isEmpty() ? defaultRootDirectory()
                                               : std::move(rootDirectory)) {}

QString CloudProjectCache::defaultRootDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("collaboration/projects-v2"));
}

bool CloudProjectCache::store(
    const QString& projectId,
    const daw::collab::SharedProjectDocument& document,
    const QString& canonicalHash, QString* error) const {
    const QString project = canonicalUuid(projectId);
    const QString hash = canonicalHash.trimmed().toLower();
    if (project.isEmpty() || !validHash(hash) ||
        document.confirmedSequence > kLargestExactJsonInteger) {
        setError(error, QStringLiteral("Cloud cache identity is invalid"));
        return false;
    }
    std::string bytes;
    if (!daw::collab::serializeSharedProjectSnapshot(document, bytes) ||
        digest(bytes) != hash) {
        setError(error, QStringLiteral("Cloud cache snapshot is invalid"));
        return false;
    }

    const QString directory = QDir(m_rootDirectory).filePath(project);
    if (!QDir().mkpath(directory)) {
        setError(error, QStringLiteral("Cloud cache is unavailable"));
        return false;
    }
    QFile::setPermissions(m_rootDirectory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner);
    QFile::setPermissions(directory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner);
    QLockFile lock(QDir(directory).filePath(QStringLiteral("cache.lock")));
    lock.setStaleLockTime(30'000);
    if (!lock.tryLock(2'000)) {
        setError(error, QStringLiteral("Cloud cache is busy"));
        return false;
    }

    const QString fileName = snapshotName(document.confirmedSequence, hash);
    const QString snapshotPath = QDir(directory).filePath(fileName);
    QSaveFile snapshot(snapshotPath);
    snapshot.setDirectWriteFallback(false);
    if (!snapshot.open(QIODevice::WriteOnly) ||
        snapshot.write(bytes.data(), qint64(bytes.size())) !=
            qint64(bytes.size()) ||
        !snapshot.commit()) {
        setError(error, QStringLiteral("Cloud cache snapshot could not be saved"));
        return false;
    }
    QFile::setPermissions(snapshotPath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    const QJsonObject manifest{
        {QStringLiteral("schemaVersion"), kCacheSchemaVersion},
        {QStringLiteral("projectFormatVersion"),
         daw::collab::kSharedProjectFormatVersion},
        {QStringLiteral("projectId"), project},
        {QStringLiteral("serverSequence"),
         double(document.confirmedSequence)},
        {QStringLiteral("sha256"), hash},
        {QStringLiteral("snapshot"), fileName},
    };
    const QString manifestPath =
        QDir(directory).filePath(QStringLiteral("head.json"));
    QSaveFile manifestFile(manifestPath);
    manifestFile.setDirectWriteFallback(false);
    const QByteArray manifestBytes =
        QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (!manifestFile.open(QIODevice::WriteOnly) ||
        manifestFile.write(manifestBytes) != manifestBytes.size() ||
        !manifestFile.commit()) {
        setError(error, QStringLiteral("Cloud cache manifest could not be saved"));
        return false;
    }
    QFile::setPermissions(manifestPath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    // Retain the current verified generation and one rollback generation.
    const QFileInfoList snapshots = QDir(directory).entryInfoList(
        {QStringLiteral("*.snapshot")}, QDir::Files,
        QDir::Time);
    int retainedOther = 0;
    for (const QFileInfo& candidate : snapshots) {
        if (candidate.fileName() == fileName) continue;
        if (retainedOther++ == 0) continue;
        QFile::remove(candidate.absoluteFilePath());
    }
    return true;
}

std::optional<CachedCloudProject> CloudProjectCache::load(
    const QString& projectId, QString* error) const {
    const QString project = canonicalUuid(projectId);
    if (project.isEmpty()) {
        setError(error, QStringLiteral("Cloud cache project id is invalid"));
        return std::nullopt;
    }
    const QString directory = QDir(m_rootDirectory).filePath(project);
    QFile manifestFile(QDir(directory).filePath(QStringLiteral("head.json")));
    if (!manifestFile.open(QIODevice::ReadOnly) ||
        manifestFile.size() <= 0 || manifestFile.size() > 16 * 1024) {
        setError(error, QStringLiteral("No verified offline project is cached"));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument parsed =
        QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        setError(error, QStringLiteral("Cloud cache manifest is invalid"));
        return std::nullopt;
    }
    const QJsonObject manifest = parsed.object();
    if (!exactKeys(manifest,
                   {"schemaVersion", "projectFormatVersion", "projectId",
                    "serverSequence", "sha256", "snapshot"}) ||
        manifest.value(QStringLiteral("schemaVersion")).toInt(-1) !=
            kCacheSchemaVersion ||
        manifest.value(QStringLiteral("projectFormatVersion")).toInt(-1) !=
            daw::collab::kSharedProjectFormatVersion ||
        canonicalUuid(manifest.value(QStringLiteral("projectId")).toString()) !=
            project) {
        setError(error, QStringLiteral("Cloud cache manifest is invalid"));
        return std::nullopt;
    }
    const auto sequence = exactUnsigned(
        manifest.value(QStringLiteral("serverSequence")));
    const QString hash = manifest.value(QStringLiteral("sha256"))
                             .toString().trimmed().toLower();
    const QString fileName = manifest.value(QStringLiteral("snapshot")).toString();
    if (!sequence || !validHash(hash) ||
        fileName != snapshotName(*sequence, hash)) {
        setError(error, QStringLiteral("Cloud cache manifest is invalid"));
        return std::nullopt;
    }

    QFile snapshot(QDir(directory).filePath(fileName));
    if (!snapshot.open(QIODevice::ReadOnly) || snapshot.size() <= 0 ||
        quint64(snapshot.size()) >
            daw::collab::kMaximumSharedProjectSnapshotBytes) {
        setError(error, QStringLiteral("Cached cloud snapshot is unavailable"));
        return std::nullopt;
    }
    const QByteArray raw = snapshot.readAll();
    const std::string bytes(raw.constData(), std::size_t(raw.size()));
    if (raw.size() != snapshot.size() || digest(bytes) != hash) {
        setError(error, QStringLiteral("Cached cloud snapshot failed integrity check"));
        return std::nullopt;
    }
    daw::collab::SharedProjectDocument document;
    if (!daw::collab::deserializeSharedProjectSnapshot(document, bytes) ||
        document.confirmedSequence != *sequence) {
        setError(error, QStringLiteral("Cached cloud snapshot is invalid"));
        return std::nullopt;
    }
    std::string canonical;
    if (!daw::collab::serializeSharedProjectSnapshot(document, canonical) ||
        canonical != bytes) {
        setError(error, QStringLiteral("Cached cloud snapshot is not canonical"));
        return std::nullopt;
    }
    return CachedCloudProject{project, *sequence, hash, std::move(document)};
}

bool checkCloudProjectCacheForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QTemporaryDir directory;
    CloudProjectCache cache(directory.path());
    const QString project =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    daw::collab::SharedProjectDocument document;
    document.confirmedSequence = 7;
    std::string bytes;
    if (!daw::collab::serializeSharedProjectSnapshot(document, bytes))
        return fail(QStringLiteral("could not encode cache fixture"));
    const QString hash = digest(bytes);
    QString cacheError;
    if (!cache.store(project, document, hash, &cacheError))
        return fail(cacheError);
    auto loaded = cache.load(project, &cacheError);
    if (!loaded || loaded->serverSequence != 7 ||
        loaded->canonicalHash != hash)
        return fail(QStringLiteral("offline cloud cache did not round-trip"));

    const QString snapshotPath = QDir(directory.path())
        .filePath(project + QLatin1Char('/') + snapshotName(7, hash));
    QFile tampered(snapshotPath);
    if (!tampered.open(QIODevice::Append) || tampered.write("x", 1) != 1)
        return fail(QStringLiteral("could not tamper cache fixture"));
    tampered.close();
    if (cache.load(project, &cacheError))
        return fail(QStringLiteral("tampered offline snapshot was accepted"));
    return true;
}

} // namespace collab
