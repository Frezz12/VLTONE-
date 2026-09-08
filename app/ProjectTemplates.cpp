#include "ProjectTemplates.hpp"

#include "ChannelStripPresets.hpp"
#include "ProjectSerializer.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace ui::projecttemplates {

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
