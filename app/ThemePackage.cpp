#include "ThemePackage.hpp"

#include "NotebookPrefs.hpp"
#include "TimelineBackgroundPrefs.hpp"
#include "UiConstants.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

namespace ui {
namespace {

constexpr char kMagic[] = "VLTTHEME";
constexpr quint32 kFormatVersion = 1;
constexpr quint32 kMaxManifestBytes = 1024 * 1024;
constexpr int kMaxResources = 64;
constexpr qint64 kCopyBufferBytes = 1024 * 1024;

struct ResourceMeta {
    QString id;
    QString name;
    QString sourcePath;
    qint64 size = 0;
    QByteArray digest;
};

ThemePackageResult failure(const QString& message) {
    ThemePackageResult result;
    result.error = message;
    return result;
}

ThemePackageResult cancelledResult() {
    ThemePackageResult result;
    result.cancelled = true;
    return result;
}

bool keepGoing(const ThemePackageProgress& progress, qint64 completed,
               qint64 total) {
    return !progress || progress(completed, total);
}

QString dataRoot() {
    if (QCoreApplication* app = QCoreApplication::instance()) {
        const QString testRoot = app->property("dawHeadlessDataRoot").toString();
        if (!testRoot.isEmpty()) return testRoot;
    }
    QString root = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.vlt");
    return root;
}

QString storedSettingPath(const char* key) {
    return QSettings().value(QLatin1String(key)).toString();
}

bool validHexDigest(const QString& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar c) {
        const ushort u = c.unicode();
        return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f');
    });
}

QString safeSuffix(const QString& name) {
    QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix.size() > 10 ||
        !std::all_of(suffix.cbegin(), suffix.cend(), [](QChar c) {
            return c.isLetterOrNumber();
        })) {
        suffix.clear();
    }
    return suffix;
}

QString extractedName(const QString& id, const QString& originalName) {
    const QString suffix = safeSuffix(originalName);
    return suffix.isEmpty() ? id : id + QLatin1Char('.') + suffix;
}

bool readHeader(QFile& file, QJsonObject& manifest, QString& error) {
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::BigEndian);
    char magic[sizeof(kMagic) - 1]{};
    if (stream.readRawData(magic, int(sizeof(magic))) != int(sizeof(magic)) ||
        QByteArray(magic, int(sizeof(magic))) != QByteArrayLiteral("VLTTHEME")) {
        error = QCoreApplication::translate(
            "ThemePackage", "The file is not a VLTONE theme.");
        return false;
    }
    quint32 version = 0;
    quint32 manifestSize = 0;
    stream >> version >> manifestSize;
    if (stream.status() != QDataStream::Ok || version != kFormatVersion) {
        error = QCoreApplication::translate(
            "ThemePackage", "This theme uses an unsupported format version.");
        return false;
    }
    if (manifestSize == 0 || manifestSize > kMaxManifestBytes ||
        qint64(manifestSize) > file.size() - file.pos()) {
        error = QCoreApplication::translate(
            "ThemePackage", "The theme manifest is missing or damaged.");
        return false;
    }
    const QByteArray bytes = file.read(manifestSize);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QCoreApplication::translate(
            "ThemePackage", "The theme manifest is not valid JSON.");
        return false;
    }
    manifest = document.object();
    if (manifest.value(QStringLiteral("format")).toString() !=
            QLatin1String("VLTONE Theme") ||
        manifest.value(QStringLiteral("version")).toInt() !=
            int(kFormatVersion) ||
        manifest.value(QStringLiteral("name")).toString().trimmed().isEmpty() ||
        manifest.value(QStringLiteral("name")).toString().size() > 128 ||
        manifest.value(QStringLiteral("packageId")).toString().isEmpty() ||
        manifest.value(QStringLiteral("packageId")).toString().size() > 64 ||
        !manifest.value(QStringLiteral("palette")).isObject() ||
        !manifest.value(QStringLiteral("appearance")).isObject() ||
        !manifest.value(QStringLiteral("resources")).isArray() ||
        manifest.value(QStringLiteral("resources")).toArray().size() >
            kMaxResources) {
        error = QCoreApplication::translate(
            "ThemePackage", "The theme manifest contains invalid fields.");
        return false;
    }

    QSet<QString> ids;
    for (const QJsonValue& value :
         manifest.value(QStringLiteral("resources")).toArray()) {
        if (!value.isObject()) {
            error = QCoreApplication::translate(
                "ThemePackage", "The theme resource list is damaged.");
            return false;
        }
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        bool sizeOk = false;
        const qint64 size =
            object.value(QStringLiteral("size")).toString().toLongLong(&sizeOk);
        const QString name = object.value(QStringLiteral("name")).toString();
        if (!validHexDigest(id) || ids.contains(id) || !sizeOk || size < 0 ||
            name.isEmpty() || name.size() > 255 ||
            QFileInfo(name).fileName() != name) {
            error = QCoreApplication::translate(
                "ThemePackage", "The theme resource list contains invalid data.");
            return false;
        }
        ids.insert(id);
    }
    return true;
}

QVector<ResourceMeta> manifestResources(const QJsonObject& manifest) {
    QVector<ResourceMeta> resources;
    for (const QJsonValue& value :
         manifest.value(QStringLiteral("resources")).toArray()) {
        const QJsonObject object = value.toObject();
        ResourceMeta resource;
        resource.id = object.value(QStringLiteral("id")).toString();
        resource.name = object.value(QStringLiteral("name")).toString();
        resource.size =
            object.value(QStringLiteral("size")).toString().toLongLong();
        resource.digest = QByteArray::fromHex(resource.id.toLatin1());
        resources.push_back(resource);
    }
    return resources;
}

bool validateResourceLayout(QFile& file, const QJsonObject& manifest,
                            QString& error) {
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::BigEndian);
    for (const ResourceMeta& resource : manifestResources(manifest)) {
        quint32 idSize = 0;
        quint64 storedSize = 0;
        stream >> idSize;
        if (stream.status() != QDataStream::Ok || idSize != 64) {
            error = QCoreApplication::translate(
                "ThemePackage", "A theme resource header is damaged.");
            return false;
        }
        QByteArray id(int(idSize), '\0');
        if (stream.readRawData(id.data(), int(idSize)) != int(idSize)) {
            error = QCoreApplication::translate(
                "ThemePackage", "A theme resource header is incomplete.");
            return false;
        }
        stream >> storedSize;
        const qint64 available = file.size() - file.pos();
        if (stream.status() != QDataStream::Ok ||
            QString::fromLatin1(id) != resource.id ||
            storedSize != quint64(resource.size) || available < 0 ||
            storedSize > quint64(available) ||
            !file.seek(file.pos() + qint64(storedSize))) {
            error = QCoreApplication::translate(
                "ThemePackage", "A theme resource does not match its manifest.");
            return false;
        }
    }
    if (!file.atEnd()) {
        error = QCoreApplication::translate(
            "ThemePackage", "The theme contains unexpected trailing data.");
        return false;
    }
    return true;
}

QByteArray hashFile(const QString& path, const ThemePackageProgress& progress,
                    qint64& completed, qint64 total, bool& cancelled,
                    QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QCoreApplication::translate(
                    "ThemePackage", "Could not open resource: %1")
                    .arg(QDir::toNativeSeparators(path));
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kCopyBufferBytes);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            error = QCoreApplication::translate(
                        "ThemePackage", "Could not read resource: %1")
                        .arg(QDir::toNativeSeparators(path));
            return {};
        }
        hash.addData(chunk);
        completed += chunk.size();
        if (!keepGoing(progress, completed, total)) {
            cancelled = true;
            return {};
        }
    }
    return hash.result();
}

bool copyDevice(QFile& input, QIODevice& output, qint64 bytes,
                QCryptographicHash* hash, const ThemePackageProgress& progress,
                qint64& completed, qint64 total, bool& cancelled,
                QString& error) {
    qint64 remaining = bytes;
    while (remaining > 0) {
        const qint64 request = std::min(remaining, kCopyBufferBytes);
        const QByteArray chunk = input.read(request);
        if (chunk.isEmpty()) {
            error = QCoreApplication::translate(
                "ThemePackage", "The theme ended before a resource was complete.");
            return false;
        }
        if (output.write(chunk) != chunk.size()) {
            error = QCoreApplication::translate(
                "ThemePackage", "Could not write a theme resource.");
            return false;
        }
        if (hash) hash->addData(chunk);
        remaining -= chunk.size();
        completed += chunk.size();
        if (!keepGoing(progress, completed, total)) {
            cancelled = true;
            return false;
        }
    }
    return true;
}

void assignResourceRole(QJsonObject& appearance, const QString& role,
                        const QString& id) {
    if (role == QLatin1String("timelineBackground")) {
        QJsonObject section = appearance.value(QStringLiteral("timeline")).toObject();
        section.insert(QStringLiteral("resource"), id);
        appearance.insert(QStringLiteral("timeline"), section);
    } else if (role == QLatin1String("headerBackground")) {
        QJsonObject section = appearance.value(QStringLiteral("header")).toObject();
        section.insert(QStringLiteral("resource"), id);
        appearance.insert(QStringLiteral("header"), section);
    } else if (role == QLatin1String("notebookBackground")) {
        QJsonObject section = appearance.value(QStringLiteral("notebook")).toObject();
        section.insert(QStringLiteral("backgroundResource"), id);
        appearance.insert(QStringLiteral("notebook"), section);
    } else if (role == QLatin1String("interfaceFont")) {
        appearance.insert(QStringLiteral("interfaceFontResource"), id);
    } else if (role.startsWith(QLatin1String("notebookFont:"))) {
        QJsonArray fonts = appearance.value(QStringLiteral("notebookFontResources"))
                               .toArray();
        fonts.push_back(id);
        appearance.insert(QStringLiteral("notebookFontResources"), fonts);
    }
}

QString resourcePath(const QJsonObject& manifest, const QString& storageId,
                     const QString& id) {
    if (id.isEmpty()) return {};
    for (const QJsonValue& value :
         manifest.value(QStringLiteral("resources")).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("id")).toString() != id) continue;
        return QDir(ThemePackage::assetDirectory())
            .filePath(storageId + QLatin1Char('/') +
                      extractedName(id,
                          object.value(QStringLiteral("name")).toString()));
    }
    return {};
}

int boundedInt(const QJsonObject& object, const char* key, int fallback,
               int minimum, int maximum) {
    return std::clamp(object.value(QLatin1String(key)).toInt(fallback),
                      minimum, maximum);
}

bool sameDirectory(const QString& a, const QString& b) {
    const QString ca = QFileInfo(a).canonicalFilePath();
    const QString cb = QFileInfo(b).canonicalFilePath();
    return !ca.isEmpty() && !cb.isEmpty() && ca == cb;
}

} // namespace

QString ThemePackage::libraryDirectory() {
    return QDir(dataRoot()).filePath(QStringLiteral("Themes"));
}

QString ThemePackage::assetDirectory() {
    return QDir(libraryDirectory()).filePath(QStringLiteral("assets"));
}

bool ThemePackage::captureCurrent(const QString& requestedName,
                                  ThemePackageSnapshot& snapshot,
                                  QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!timelinebackgroundprefs::webSource().isEmpty()) {
        return fail(QCoreApplication::translate(
            "ThemePackage",
            "The active browser video cannot be embedded. Choose a local video "
            "for the timeline background before saving this theme."));
    }

    snapshot = {};
    snapshot.name = requestedName.trimmed().left(128);
    if (snapshot.name.isEmpty())
        snapshot.name = ThemeManager::instance().theme().name.trimmed().left(128);
    if (snapshot.name.isEmpty())
        snapshot.name = QCoreApplication::translate("ThemePackage", "Custom Theme");
    snapshot.palette = ThemeManager::instance().theme();

    const auto checkedResource = [&snapshot, &fail](const QString& role,
                                                    const QString& path) {
        if (path.isEmpty()) return true;
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            fail(QCoreApplication::translate(
                     "ThemePackage", "A theme resource is missing: %1")
                     .arg(QDir::toNativeSeparators(path)));
            return false;
        }
        snapshot.resources.push_back({role, info.absoluteFilePath(), {}});
        return true;
    };

    const QString timelineStored =
        storedSettingPath("theme/timelineBackground/path");
    const QString headerStored =
        storedSettingPath("theme/headerBackground/path");
    const QString notebookStored = storedSettingPath("notebook/background");
    if (!checkedResource(QStringLiteral("timelineBackground"), timelineStored) ||
        !checkedResource(QStringLiteral("headerBackground"), headerStored) ||
        !checkedResource(QStringLiteral("notebookBackground"), notebookStored))
        return false;

    QJsonObject timeline{
        {QStringLiteral("enabled"), timelinebackgroundprefs::enabled()},
        {QStringLiteral("visibility"), timelinebackgroundprefs::visibility()},
        {QStringLiteral("blurRadius"), timelinebackgroundprefs::blurRadius()},
        {QStringLiteral("animated"),
         timelinebackgroundprefs::animatedBackgroundsEnabled()},
        {QStringLiteral("placement"), int(timelinebackgroundprefs::placement())},
    };
    QJsonObject header{
        {QStringLiteral("enabled"), headerbackgroundprefs::enabled()},
        {QStringLiteral("visibility"), headerbackgroundprefs::visibility()},
        {QStringLiteral("blurRadius"), headerbackgroundprefs::blurRadius()},
        {QStringLiteral("animated"),
         headerbackgroundprefs::animatedBackgroundsEnabled()},
        {QStringLiteral("placement"), int(headerbackgroundprefs::placement())},
    };
    QJsonObject notebook{
        {QStringLiteral("visibility"), notebookprefs::backgroundVisibility()},
        {QStringLiteral("animated"), notebookprefs::animatedBackgroundsEnabled()},
    };
    snapshot.appearance = {
        {QStringLiteral("timeline"), timeline},
        {QStringLiteral("header"), header},
        {QStringLiteral("notebook"), notebook},
        {QStringLiteral("selectionTint"), int(ui::selectionTint())},
        {QStringLiteral("playheadWidth"), ui::playheadWidth()},
        {QStringLiteral("playheadTrail"), ui::playheadTrail()},
    };

    const ThemeManager& manager = ThemeManager::instance();
    if (manager.hasCustomFont()) {
        const QString fontPath = manager.customFontPath();
        if (fontPath.isEmpty())
            return fail(QCoreApplication::translate(
                "ThemePackage", "The active interface font file is missing."));
        if (!checkedResource(QStringLiteral("interfaceFont"),
                             fontPath))
            return false;
        snapshot.resources.back().fileName = manager.customFontFileName();
    }

    const QStringList storedNotebookFonts =
        QSettings().value(QStringLiteral("notebook/customFonts")).toStringList();
    for (qsizetype i = 0; i < storedNotebookFonts.size(); ++i) {
        if (!checkedResource(QStringLiteral("notebookFont:%1").arg(i),
                             storedNotebookFonts.at(i)))
            return false;
    }
    return true;
}

ThemePackageResult ThemePackage::write(const ThemePackageSnapshot& snapshot,
                                       const QString& destination,
                                       ThemePackageProgress progress) {
    if (snapshot.name.trimmed().isEmpty())
        return failure(QCoreApplication::translate(
            "ThemePackage", "Enter a name for the theme."));
    if (snapshot.resources.size() > kMaxResources)
        return failure(QCoreApplication::translate(
            "ThemePackage", "The theme contains too many resources."));

    qint64 total = 0;
    for (const ThemePackageResourceSource& source : snapshot.resources) {
        const QFileInfo info(source.path);
        if (!info.exists() || !info.isFile())
            return failure(QCoreApplication::translate(
                               "ThemePackage", "A theme resource is missing: %1")
                               .arg(QDir::toNativeSeparators(source.path)));
        if (info.size() < 0 ||
            info.size() > (std::numeric_limits<qint64>::max() - total) / 2)
            return failure(QCoreApplication::translate(
                "ThemePackage", "The theme resources are too large."));
        total += info.size() * 2;
    }

    QVector<ResourceMeta> resources;
    QHash<QString, int> resourceById;
    QHash<QString, QString> roleIds;
    qint64 completed = 0;
    bool cancelled = false;
    QString error;
    for (const ThemePackageResourceSource& source : snapshot.resources) {
        const QFileInfo info(source.path);
        const QByteArray digest = hashFile(source.path, progress, completed,
                                           total, cancelled, error);
        if (cancelled) return cancelledResult();
        if (digest.isEmpty()) return failure(error);
        const QString id = QString::fromLatin1(digest.toHex());
        roleIds.insert(source.role, id);
        if (resourceById.contains(id)) continue;
        ResourceMeta resource;
        resource.id = id;
        resource.name = source.fileName.isEmpty()
            ? info.fileName() : QFileInfo(source.fileName).fileName();
        if (resource.name.isEmpty()) resource.name = info.fileName();
        resource.name = resource.name.left(255);
        resource.sourcePath = info.absoluteFilePath();
        resource.size = info.size();
        resource.digest = digest;
        resourceById.insert(id, resources.size());
        resources.push_back(resource);
    }

    QJsonObject appearance = snapshot.appearance;
    for (const ThemePackageResourceSource& source : snapshot.resources)
        assignResourceRole(appearance, source.role, roleIds.value(source.role));

    QJsonArray resourceArray;
    for (const ResourceMeta& resource : std::as_const(resources)) {
        resourceArray.push_back(QJsonObject{
            {QStringLiteral("id"), resource.id},
            {QStringLiteral("name"), resource.name},
            {QStringLiteral("size"), QString::number(resource.size)},
        });
    }
    QJsonObject manifest{
        {QStringLiteral("format"), QStringLiteral("VLTONE Theme")},
        {QStringLiteral("version"), int(kFormatVersion)},
        {QStringLiteral("packageId"),
         QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("name"), snapshot.name.trimmed().left(128)},
        {QStringLiteral("palette"), ThemeManager::toJson(snapshot.palette)},
        {QStringLiteral("appearance"), appearance},
        {QStringLiteral("resources"), resourceArray},
    };
    const QByteArray manifestBytes =
        QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (manifestBytes.size() > int(kMaxManifestBytes))
        return failure(QCoreApplication::translate(
            "ThemePackage", "The theme manifest is too large."));

    QDir().mkpath(QFileInfo(destination).absolutePath());
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly))
        return failure(QCoreApplication::translate(
                           "ThemePackage", "Could not create %1")
                           .arg(QDir::toNativeSeparators(destination)));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.writeRawData(kMagic, int(sizeof(kMagic) - 1));
    stream << kFormatVersion << quint32(manifestBytes.size());
    stream.writeRawData(manifestBytes.constData(), manifestBytes.size());
    if (stream.status() != QDataStream::Ok) {
        file.cancelWriting();
        return failure(QCoreApplication::translate(
            "ThemePackage", "Could not write the theme manifest."));
    }

    for (const ResourceMeta& resource : std::as_const(resources)) {
        const QByteArray id = resource.id.toLatin1();
        stream << quint32(id.size());
        stream.writeRawData(id.constData(), id.size());
        stream << quint64(resource.size);
        if (stream.status() != QDataStream::Ok) {
            file.cancelWriting();
            return failure(QCoreApplication::translate(
                "ThemePackage", "Could not write a theme resource header."));
        }
        QFile input(resource.sourcePath);
        if (!input.open(QIODevice::ReadOnly)) {
            file.cancelWriting();
            return failure(QCoreApplication::translate(
                               "ThemePackage", "Could not open resource: %1")
                               .arg(QDir::toNativeSeparators(resource.sourcePath)));
        }
        QCryptographicHash writtenHash(QCryptographicHash::Sha256);
        if (!copyDevice(input, file, resource.size, &writtenHash, progress, completed,
                        total, cancelled, error)) {
            file.cancelWriting();
            return cancelled ? cancelledResult() : failure(error);
        }
        if (writtenHash.result() != resource.digest || !input.atEnd()) {
            file.cancelWriting();
            return failure(QCoreApplication::translate(
                "ThemePackage",
                "A theme resource changed while the theme was being saved."));
        }
    }
    if (!file.commit())
        return failure(QCoreApplication::translate(
                           "ThemePackage", "Could not finish writing %1")
                           .arg(QDir::toNativeSeparators(destination)));

    ThemePackageResult result;
    result.ok = true;
    result.filePath = QFileInfo(destination).absoluteFilePath();
    result.manifest = manifest;
    return result;
}

ThemePackageResult ThemePackage::saveToLibrary(
    const ThemePackageSnapshot& snapshot, ThemePackageProgress progress) {
    QDir().mkpath(libraryDirectory());
    const QString temporary = QDir(libraryDirectory()).filePath(
        QStringLiteral(".saving-%1.vlttheme")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const ThemePackageProgress writeProgress = progress
        ? ThemePackageProgress([progress](qint64 done, qint64 total) {
              return progress(total > 0
                                  ? qint64(double(done) / double(total) * 500.0)
                                  : 0,
                              1000);
          })
        : ThemePackageProgress{};
    const ThemePackageProgress installProgress = progress
        ? ThemePackageProgress([progress](qint64 done, qint64 total) {
              return progress(
                  500 + (total > 0
                             ? qint64(double(done) / double(total) * 500.0)
                             : 0),
                  1000);
          })
        : ThemePackageProgress{};
    ThemePackageResult result = write(snapshot, temporary, writeProgress);
    if (!result.ok) return result;
    result = install(temporary, installProgress);
    QFile::remove(temporary);
    return result;
}

ThemePackageResult ThemePackage::inspect(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return failure(QCoreApplication::translate(
                           "ThemePackage", "Could not open %1")
                           .arg(QDir::toNativeSeparators(path)));
    QJsonObject manifest;
    QString error;
    if (!readHeader(file, manifest, error)) return failure(error);
    if (!validateResourceLayout(file, manifest, error)) return failure(error);
    ThemePackageResult result;
    result.ok = true;
    result.filePath = QFileInfo(path).absoluteFilePath();
    result.manifest = manifest;
    return result;
}

ThemePackageResult ThemePackage::install(const QString& sourcePath,
                                         ThemePackageProgress progress) {
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile())
        return failure(QCoreApplication::translate(
            "ThemePackage", "The selected theme file does not exist."));

    qint64 completed = 0;
    const qint64 total = sourceInfo.size() >
                                 std::numeric_limits<qint64>::max() / 3
                             ? std::numeric_limits<qint64>::max()
                             : std::max<qint64>(1, sourceInfo.size() * 3);
    bool cancelled = false;
    QString error;
    const QByteArray packageDigest = hashFile(sourceInfo.absoluteFilePath(),
                                              progress, completed, total,
                                              cancelled, error);
    if (cancelled) return cancelledResult();
    if (packageDigest.isEmpty()) return failure(error);
    const QString storageId = QString::fromLatin1(packageDigest.toHex());

    ThemePackageResult sourceInspection = inspect(sourceInfo.absoluteFilePath());
    if (!sourceInspection.ok) return sourceInspection;

    QDir().mkpath(libraryDirectory());
    QDir().mkpath(assetDirectory());
    QString installedPath = sourceInfo.absoluteFilePath();
    const QFileInfo libraryInfo(libraryDirectory());
    const bool needsLibraryCopy =
        !sameDirectory(sourceInfo.absolutePath(), libraryInfo.absoluteFilePath()) ||
        sourceInfo.fileName().startsWith(QLatin1String(".saving-"));
    if (needsLibraryCopy) {
        installedPath = QDir(libraryDirectory())
                            .filePath(storageId + QStringLiteral(".vlttheme"));
    }

    const QString finalAssets = QDir(assetDirectory()).filePath(storageId);
    if (!QFileInfo::exists(finalAssets)) {
        const QString incoming = QDir(assetDirectory()).filePath(
            QStringLiteral(".installing-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(incoming))
            return failure(QCoreApplication::translate(
                "ThemePackage", "Could not create the theme resource folder."));

        QFile package(sourceInfo.absoluteFilePath());
        if (!package.open(QIODevice::ReadOnly)) {
            QDir(incoming).removeRecursively();
            return failure(QCoreApplication::translate(
                "ThemePackage", "Could not reopen the installed theme."));
        }
        QJsonObject manifest;
        if (!readHeader(package, manifest, error)) {
            QDir(incoming).removeRecursively();
            return failure(error);
        }
        QDataStream stream(&package);
        stream.setByteOrder(QDataStream::BigEndian);
        for (const ResourceMeta& resource : manifestResources(manifest)) {
            quint32 idSize = 0;
            quint64 storedSize = 0;
            stream >> idSize;
            if (idSize != 64) {
                QDir(incoming).removeRecursively();
                return failure(QCoreApplication::translate(
                    "ThemePackage", "A theme resource header is damaged."));
            }
            QByteArray id(int(idSize), Qt::Uninitialized);
            if (stream.readRawData(id.data(), int(idSize)) != int(idSize)) {
                QDir(incoming).removeRecursively();
                return failure(QCoreApplication::translate(
                    "ThemePackage", "A theme resource header is incomplete."));
            }
            stream >> storedSize;
            if (stream.status() != QDataStream::Ok ||
                QString::fromLatin1(id) != resource.id ||
                storedSize != quint64(resource.size)) {
                QDir(incoming).removeRecursively();
                return failure(QCoreApplication::translate(
                    "ThemePackage", "A theme resource does not match its manifest."));
            }

            const QString outputPath = QDir(incoming).filePath(
                extractedName(resource.id, resource.name));
            QSaveFile output(outputPath);
            if (!output.open(QIODevice::WriteOnly)) {
                QDir(incoming).removeRecursively();
                return failure(QCoreApplication::translate(
                    "ThemePackage", "Could not extract a theme resource."));
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!copyDevice(package, output, resource.size, &hash, progress,
                            completed, total, cancelled, error)) {
                output.cancelWriting();
                QDir(incoming).removeRecursively();
                return cancelled ? cancelledResult() : failure(error);
            }
            if (hash.result() != resource.digest || !output.commit()) {
                QDir(incoming).removeRecursively();
                return failure(QCoreApplication::translate(
                    "ThemePackage", "A theme resource failed verification."));
            }
        }
        if (!package.atEnd()) {
            QDir(incoming).removeRecursively();
            return failure(QCoreApplication::translate(
                "ThemePackage", "The theme contains unexpected trailing data."));
        }
        if (!QDir().rename(incoming, finalAssets)) {
            if (!QFileInfo::exists(finalAssets)) {
                QDir(incoming).removeRecursively();
                return failure(QCoreApplication::translate(
                    "ThemePackage", "Could not finish installing theme resources."));
            }
            QDir(incoming).removeRecursively();
        }
    }

    // Publish the library entry only after every resource has been verified.
    // A truncated or tampered package therefore never appears as installed.
    bool packageVerifiedAfterExtraction = false;
    if (needsLibraryCopy && !QFileInfo::exists(installedPath)) {
        QFile input(sourceInfo.absoluteFilePath());
        QSaveFile output(installedPath);
        if (!input.open(QIODevice::ReadOnly) ||
            !output.open(QIODevice::WriteOnly))
            return failure(QCoreApplication::translate(
                "ThemePackage", "Could not copy the theme into the library."));
        QCryptographicHash copiedHash(QCryptographicHash::Sha256);
        if (!copyDevice(input, output, input.size(), &copiedHash, progress,
                        completed, total, cancelled, error)) {
            output.cancelWriting();
            return cancelled ? cancelledResult() : failure(error);
        }
        if (copiedHash.result() != packageDigest || !input.atEnd()) {
            output.cancelWriting();
            return failure(QCoreApplication::translate(
                "ThemePackage",
                "The theme file changed while it was being installed."));
        }
        if (!output.commit())
            return failure(QCoreApplication::translate(
                "ThemePackage", "Could not finish installing the theme."));
        packageVerifiedAfterExtraction = true;
    }

    // A package that was placed directly in the library (or whose hashed
    // library copy already exists) is not copied above. Read it once more so a
    // file changed during extraction can never be paired with the old digest.
    if (!packageVerifiedAfterExtraction) {
        bool verifyCancelled = false;
        QString verifyError;
        const QByteArray verifiedDigest = hashFile(
            sourceInfo.absoluteFilePath(), progress, completed, total,
            verifyCancelled, verifyError);
        if (verifyCancelled) return cancelledResult();
        if (verifiedDigest.isEmpty()) return failure(verifyError);
        if (verifiedDigest != packageDigest)
            return failure(QCoreApplication::translate(
                "ThemePackage",
                "The theme file changed while it was being installed."));
    }

    ThemePackageResult result = sourceInspection;
    result.filePath = installedPath;
    result.storageId = storageId;
    return result;
}

QVector<ThemeLibraryEntry> ThemePackage::libraryEntries() {
    QVector<ThemeLibraryEntry> entries;
    QDir directory(libraryDirectory());
    if (!directory.exists()) return entries;
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.vlttheme")}, QDir::Files | QDir::Readable,
        QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& file : files) {
        if (file.fileName().startsWith(QLatin1Char('.'))) continue;
        const ThemePackageResult inspected = inspect(file.absoluteFilePath());
        if (!inspected.ok) continue;
        entries.push_back({
            inspected.manifest.value(QStringLiteral("packageId")).toString(),
            inspected.manifest.value(QStringLiteral("name")).toString(),
            file.absoluteFilePath(),
        });
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const ThemeLibraryEntry& a, const ThemeLibraryEntry& b) {
                         const int nameOrder = QString::localeAwareCompare(a.name, b.name);
                         return nameOrder != 0 ? nameOrder < 0 : a.packageId < b.packageId;
                     });
    return entries;
}

ThemePackageResult ThemePackage::apply(const QString& installedPath,
                                       const QString& storageId) {
    if (!validHexDigest(storageId))
        return failure(QCoreApplication::translate(
            "ThemePackage", "The installed theme identifier is invalid."));
    ThemePackageResult inspected = inspect(installedPath);
    if (!inspected.ok) return inspected;
    const QJsonObject manifest = inspected.manifest;
    const QJsonObject appearance =
        manifest.value(QStringLiteral("appearance")).toObject();
    const QJsonObject timeline =
        appearance.value(QStringLiteral("timeline")).toObject();
    const QJsonObject header =
        appearance.value(QStringLiteral("header")).toObject();
    const QJsonObject notebook =
        appearance.value(QStringLiteral("notebook")).toObject();

    const QString timelinePath = resourcePath(
        manifest, storageId, timeline.value(QStringLiteral("resource")).toString());
    const QString headerPath = resourcePath(
        manifest, storageId, header.value(QStringLiteral("resource")).toString());
    const QString notebookPath = resourcePath(
        manifest, storageId,
        notebook.value(QStringLiteral("backgroundResource")).toString());
    const QString interfaceFontPath = resourcePath(
        manifest, storageId,
        appearance.value(QStringLiteral("interfaceFontResource")).toString());

    const auto missing = [](const QString& id, const QString& path) {
        return !id.isEmpty() && (path.isEmpty() || !QFileInfo::exists(path));
    };
    if (missing(timeline.value(QStringLiteral("resource")).toString(),
                timelinePath) ||
        missing(header.value(QStringLiteral("resource")).toString(), headerPath) ||
        missing(notebook.value(QStringLiteral("backgroundResource")).toString(),
                notebookPath) ||
        missing(appearance.value(QStringLiteral("interfaceFontResource")).toString(),
                interfaceFontPath)) {
        return failure(QCoreApplication::translate(
            "ThemePackage", "An installed theme resource is missing."));
    }
    if ((!timelinePath.isEmpty() &&
         !timelinebackgroundprefs::isSupported(timelinePath)) ||
        (!headerPath.isEmpty() &&
         !timelinebackgroundprefs::isSupported(headerPath)) ||
        (!notebookPath.isEmpty() &&
         !notebookprefs::isSupportedBackground(notebookPath))) {
        return failure(QCoreApplication::translate(
            "ThemePackage", "The theme contains an unsupported background file."));
    }

    QStringList notebookFontPaths;
    for (const QJsonValue& value :
         appearance.value(QStringLiteral("notebookFontResources")).toArray()) {
        const QString id = value.toString();
        const QString path = resourcePath(manifest, storageId, id);
        if (missing(id, path) || !notebookprefs::isSupportedFont(path))
            return failure(QCoreApplication::translate(
                "ThemePackage", "The theme contains an unsupported notebook font."));
        notebookFontPaths.push_back(path);
    }

    ThemeManager& manager = ThemeManager::instance();
    if (!interfaceFontPath.isEmpty()) {
        QString fontError;
        if (!manager.importFont(interfaceFontPath, &fontError))
            return failure(fontError);
    }

    timelinebackgroundprefs::clearWebSource();
    if (timelinePath.isEmpty()) timelinebackgroundprefs::clear();
    else timelinebackgroundprefs::setPath(timelinePath);
    timelinebackgroundprefs::setEnabled(
        timeline.value(QStringLiteral("enabled")).toBool(true));
    timelinebackgroundprefs::setVisibility(
        boundedInt(timeline, "visibility", 32, 0, 100));
    timelinebackgroundprefs::setBlurRadius(
        boundedInt(timeline, "blurRadius", 8, 0, 32));
    timelinebackgroundprefs::setAnimatedBackgroundsEnabled(
        timeline.value(QStringLiteral("animated")).toBool(true));
    timelinebackgroundprefs::setPlacement(timelinebackgroundprefs::Placement(
        boundedInt(timeline, "placement", 0, 0, 3)));

    if (headerPath.isEmpty()) headerbackgroundprefs::clear();
    else headerbackgroundprefs::setPath(headerPath);
    headerbackgroundprefs::setEnabled(
        header.value(QStringLiteral("enabled")).toBool(false));
    headerbackgroundprefs::setVisibility(
        boundedInt(header, "visibility", 28, 0, 100));
    headerbackgroundprefs::setBlurRadius(
        boundedInt(header, "blurRadius", 6, 0, 32));
    headerbackgroundprefs::setAnimatedBackgroundsEnabled(
        header.value(QStringLiteral("animated")).toBool(true));
    headerbackgroundprefs::setPlacement(timelinebackgroundprefs::Placement(
        boundedInt(header, "placement", 0, 0, 3)));

    if (notebookPath.isEmpty()) notebookprefs::clearBackground();
    else notebookprefs::setBackgroundPath(notebookPath);
    notebookprefs::setBackgroundVisibility(
        boundedInt(notebook, "visibility", 38, 0, 100));
    notebookprefs::setAnimatedBackgroundsEnabled(
        notebook.value(QStringLiteral("animated")).toBool(true));
    notebookprefs::setCustomFontFiles(notebookFontPaths);

    if (interfaceFontPath.isEmpty()) manager.resetFont();
    ui::setSelectionTint(ui::SelectionTint(boundedInt(
        appearance, "selectionTint", 0, 0, 1)));
    ui::setPlayheadWidth(std::clamp(
        appearance.value(QStringLiteral("playheadWidth"))
            .toDouble(ui::kPlayheadWidthDefault),
        ui::kPlayheadWidthMin, ui::kPlayheadWidthMax));
    ui::setPlayheadTrail(
        appearance.value(QStringLiteral("playheadTrail")).toBool(true));

    Theme palette = ThemeManager::fromJson(
        manifest.value(QStringLiteral("palette")).toObject(), manager.theme());
    palette.name = manifest.value(QStringLiteral("name")).toString();
    manager.applyCustomTheme(std::move(palette), true);
    QSettings settings;
    settings.setValue(QStringLiteral("ui/activeThemePackage"),
                      manifest.value(QStringLiteral("packageId")).toString());
    settings.sync();

    inspected.ok = true;
    inspected.storageId = storageId;
    inspected.filePath = installedPath;
    return inspected;
}

bool ThemePackage::checkForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QTemporaryDir temporary;
    if (!temporary.isValid()) return fail(QStringLiteral("temporary directory failed"));
    const QString mediaPath = temporary.filePath(QStringLiteral("background.png"));
    QFile media(mediaPath);
    if (!media.open(QIODevice::WriteOnly) ||
        media.write(QByteArrayLiteral("portable-theme-resource")) <= 0)
        return fail(QStringLiteral("fixture write failed"));
    media.close();

    ThemePackageSnapshot snapshot;
    snapshot.name = QStringLiteral("Portable Test");
    snapshot.palette = ThemeManager::instance().theme();
    snapshot.appearance = {
        {QStringLiteral("timeline"), QJsonObject{}},
        {QStringLiteral("header"), QJsonObject{}},
        {QStringLiteral("notebook"), QJsonObject{}},
    };
    snapshot.resources.push_back(
        {QStringLiteral("timelineBackground"), mediaPath});
    snapshot.resources.push_back(
        {QStringLiteral("headerBackground"), mediaPath});
    const QString packagePath = temporary.filePath(QStringLiteral("test.vlttheme"));
    const ThemePackageResult written = write(snapshot, packagePath);
    if (!written.ok) return fail(written.error);

    const QString cancelledPath =
        temporary.filePath(QStringLiteral("cancelled.vlttheme"));
    const ThemePackageResult cancelledWrite = write(
        snapshot, cancelledPath,
        [](qint64, qint64) { return false; });
    if (!cancelledWrite.cancelled || QFileInfo::exists(cancelledPath))
        return fail(QStringLiteral("cancelled export left a file"));

    const QString damagedPath =
        temporary.filePath(QStringLiteral("damaged.vlttheme"));
    if (!QFile::copy(packagePath, damagedPath))
        return fail(QStringLiteral("damage fixture copy failed"));
    const ThemePackageResult installed = install(packagePath);
    if (!installed.ok) return fail(installed.error);
    const QJsonObject appearance =
        installed.manifest.value(QStringLiteral("appearance")).toObject();
    const QString id = appearance.value(QStringLiteral("timeline"))
                           .toObject().value(QStringLiteral("resource")).toString();
    const QString headerId = appearance.value(QStringLiteral("header"))
                                 .toObject().value(QStringLiteral("resource"))
                                 .toString();
    if (id != headerId ||
        installed.manifest.value(QStringLiteral("resources")).toArray().size() != 1)
        return fail(QStringLiteral("identical resources were not deduplicated"));
    const QString extracted = resourcePath(installed.manifest,
                                           installed.storageId, id);
    QFile extractedFile(extracted);
    if (!extractedFile.open(QIODevice::ReadOnly) ||
        extractedFile.readAll() != QByteArrayLiteral("portable-theme-resource"))
        return fail(QStringLiteral("resource round trip failed"));
    extractedFile.close();
    if (!QFile::remove(packagePath) || !inspect(installed.filePath).ok ||
        !QFileInfo::exists(extracted))
        return fail(QStringLiteral("installed theme depended on its source file"));
    const ThemePackageResult applied =
        apply(installed.filePath, installed.storageId);
    if (!applied.ok ||
        timelinebackgroundprefs::path() != extracted ||
        headerbackgroundprefs::path() != extracted)
        return fail(QStringLiteral("installed resources were not applied from private storage"));

    QFile damaged(damagedPath);
    if (!damaged.open(QIODevice::ReadWrite) || !damaged.seek(damaged.size() - 1) ||
        damaged.write("x", 1) != 1)
        return fail(QStringLiteral("damage fixture failed"));
    damaged.close();
    const ThemePackageResult rejected = install(damagedPath);
    if (rejected.ok || rejected.cancelled)
        return fail(QStringLiteral("damaged resource was accepted"));
    return true;
}

} // namespace ui
