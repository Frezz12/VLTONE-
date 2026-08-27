#include "ProjectTemplates.hpp"

#include "ChannelStripPresets.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

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
