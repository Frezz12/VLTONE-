#include "ProjectTemplates.hpp"

#include "ChannelStripPresets.hpp"
#include "ProjectSerializer.hpp"
#include "TimelineBackgroundPrefs.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace ui::projecttemplates {

namespace {

constexpr auto kArtworkDirectory = "Artwork";
constexpr auto kInfoFile = "TemplateInfo.json";

QString safeArtworkPath(const QString& packagePath, const QString& relative) {
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)) return {};
    const QString clean = QDir::cleanPath(relative);
    if (clean == QLatin1String("..") || clean.startsWith(QStringLiteral("../")))
        return {};
    const QString root = QDir::cleanPath(QFileInfo(packagePath).absoluteFilePath());
    const QString candidate = QDir::cleanPath(QDir(root).filePath(clean));
    const QString back = QDir(root).relativeFilePath(candidate);
    if (back == QLatin1String("..") || back.startsWith(QStringLiteral("../")))
        return {};
    return candidate;
}

bool writeInfo(const QString& packagePath, const QString& relative,
               const QString& displayName, QString* error) {
    QJsonObject info;
    info.insert(QStringLiteral("version"), 1);
    info.insert(QStringLiteral("media"), relative);
    info.insert(QStringLiteral("sourceName"), displayName);
    QSaveFile file(QDir(packagePath).filePath(QLatin1String(kInfoFile)));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    if (file.write(QJsonDocument(info).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace

QString folder() {
    const QString path = QDir(ui::channelstrippresets::rootFolder())
                             .filePath(QStringLiteral("Templates"));
    QDir().mkpath(path);
    return path;
}

bool isTemplatePackage(const QString& path) {
    return QFileInfo(path).suffix().compare(QLatin1String(kExtension),
                                            Qt::CaseInsensitive) == 0;
}

QVector<AudioTarget> audioTargets(const QString& packagePath, QString* error) {
    if (error) error->clear();
    daw::ProjectModel project;
    const audio::Result result = daw::ProjectSerializer::load(
        project, packagePath.toStdString());
    if (!result) {
        if (error) *error = QString::fromStdString(result.message());
        return {};
    }

    const auto displayPath = [&project](const daw::TrackModel& track) {
        QStringList parts;
        QSet<QString> visited;
        const daw::TrackModel* current = &track;
        while (current) {
            const QString id = QString::fromStdString(current->id);
            if (visited.contains(id)) break;
            visited.insert(id);
            const QString name = QString::fromStdString(current->name).trimmed();
            parts.prepend(name.isEmpty() ? QObject::tr("Untitled Track") : name);
            current = current->parentId.empty()
                ? nullptr : project.findTrack(current->parentId);
        }
        return parts.join(QStringLiteral(" / "));
    };

    QVector<AudioTarget> targets;
    QSet<QString> ids;
    for (const daw::TrackModel& track : project.tracks) {
        if (track.kind != daw::TrackKind::Audio) continue;
        const QString id = QString::fromStdString(track.id);
        if (id.isEmpty() || ids.contains(id)) continue;
        ids.insert(id);
        targets.push_back({id, displayPath(track)});
    }
    return targets;
}

QStringList files() {
    const QFileInfoList entries =
        QDir(folder()).entryInfoList(QDir::Dirs | QDir::Readable |
                                         QDir::NoDotAndDotDot,
                                     QDir::Name | QDir::IgnoreCase);
    QStringList result;
    for (const QFileInfo& entry : entries) {
        if (isTemplatePackage(entry.absoluteFilePath()))
            result << entry.absoluteFilePath();
    }
    return result;
}

QString displayName(const QString& packagePath) {
    return QFileInfo(packagePath).completeBaseName();
}

ArtworkInfo artwork(const QString& packagePath) {
    ArtworkInfo result;
    QFile file(QDir(packagePath).filePath(QLatin1String(kInfoFile)));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) {
            const QJsonObject info = document.object();
            const QString path = safeArtworkPath(
                packagePath, info.value(QStringLiteral("media")).toString());
            if (QFileInfo(path).isFile() &&
                timelinebackgroundprefs::isSupported(path)) {
                result.path = path;
                result.displayName =
                    info.value(QStringLiteral("sourceName")).toString().trimmed();
                if (result.displayName.isEmpty())
                    result.displayName = QFileInfo(path).fileName();
                return result;
            }
        }
    }

    // Additive fallback for early packages that may contain artwork but no
    // metadata. It also keeps a hand-repaired template useful.
    const QFileInfoList candidates =
        QDir(QDir(packagePath).filePath(QLatin1String(kArtworkDirectory)))
            .entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& candidate : candidates) {
        if (!timelinebackgroundprefs::isSupported(candidate.absoluteFilePath()))
            continue;
        result.path = candidate.absoluteFilePath();
        result.displayName = candidate.fileName();
        break;
    }
    return result;
}

bool installArtwork(const QString& packagePath, const QString& sourcePath,
                    QString* error) {
    if (error) error->clear();
    const QFileInfo package(packagePath);
    const QFileInfo source(sourcePath);
    if (!package.isDir() || !isTemplatePackage(packagePath)) {
        if (error) *error = QObject::tr("The template package is unavailable.");
        return false;
    }
    if (!source.isFile() || !source.isReadable() ||
        !timelinebackgroundprefs::isSupported(source.absoluteFilePath())) {
        if (error) *error = QObject::tr("Choose a supported photo, GIF or video file.");
        return false;
    }

    const QString artworkDir =
        QDir(packagePath).filePath(QLatin1String(kArtworkDirectory));
    QDir(artworkDir).removeRecursively();
    if (!QDir().mkpath(artworkDir)) {
        if (error) *error = QObject::tr("The template artwork folder could not be created.");
        return false;
    }
    const QString suffix = source.suffix().toLower();
    const QString storedName = QStringLiteral("preview.%1").arg(suffix);
    const QString destination = QDir(artworkDir).filePath(storedName);
    if (!QFile::copy(source.absoluteFilePath(), destination)) {
        if (error) *error = QObject::tr("The selected media could not be copied into the template.");
        QDir(artworkDir).removeRecursively();
        return false;
    }
    const QString relative = QStringLiteral("%1/%2")
                                 .arg(QLatin1String(kArtworkDirectory), storedName);
    if (!writeInfo(packagePath, relative, source.fileName(), error)) {
        QDir(artworkDir).removeRecursively();
        return false;
    }
    return true;
}

bool remove(const QString& packagePath, QString* error) {
    if (error) error->clear();
    const QFileInfo candidate(packagePath);
    const QFileInfo managed(folder());
    const Qt::CaseSensitivity pathCase =
#ifdef Q_OS_WIN
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif
    if (!candidate.isDir() || candidate.isSymLink() ||
        !isTemplatePackage(candidate.absoluteFilePath()) ||
        candidate.absoluteDir().canonicalPath().compare(
            managed.canonicalFilePath(), pathCase) != 0) {
        if (error) *error = QObject::tr("Only saved VLTONE templates can be deleted here.");
        return false;
    }
    if (!QDir(candidate.absoluteFilePath()).removeRecursively()) {
        if (error) *error = QObject::tr("The template could not be deleted.");
        return false;
    }
    return true;
}

QString filePathForName(const QString& name) {
    const QString clean = name.trimmed();
    if (clean.isEmpty() || clean == QLatin1String(".") ||
        clean == QLatin1String("..") || clean.endsWith(QLatin1Char('.')) ||
        clean.contains(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*])")))) {
        return {};
    }
    return QDir(folder())
        .filePath(clean + QLatin1Char('.') + QLatin1String(kExtension));
}

} // namespace ui::projecttemplates
