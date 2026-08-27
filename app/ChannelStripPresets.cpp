#include "ChannelStripPresets.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace ui::channelstrippresets {
namespace {

QString dataFolder() {
    // Screenshot/selftest runs can keep their fixtures away from the user's
    // real library. The normal application never sets this.
    const QString override = qEnvironmentVariable("DAW_PRESET_ROOT").trimmed();
    if (!override.isEmpty()) return QDir::cleanPath(override);
    QString folder =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (folder.isEmpty()) {
        folder = QDir(QStandardPaths::writableLocation(
                          QStandardPaths::DocumentsLocation))
                     .filePath(QStringLiteral("VLT Studio Pro"));
    }
    return folder;
}

void ensureFolders() {
    QDir().mkpath(QDir(dataFolder()).filePath(
        QStringLiteral("Presets/Channel Strips")));
}

} // namespace

QString rootFolder() {
    ensureFolders();
    return QDir(dataFolder()).filePath(QStringLiteral("Presets"));
}

QString stripFolder() {
    ensureFolders();
    return QDir(rootFolder()).filePath(QStringLiteral("Channel Strips"));
}

bool isPresetFile(const QString& path) {
    return QFileInfo(path).suffix().compare(QLatin1String(kExtension),
                                            Qt::CaseInsensitive) == 0;
}

QStringList files() {
    const QFileInfoList entries =
        QDir(stripFolder()).entryInfoList(QDir::Files | QDir::Readable,
                                          QDir::Name | QDir::IgnoreCase);
    QStringList result;
    for (const QFileInfo& entry : entries) {
        if (isPresetFile(entry.absoluteFilePath()))
            result << entry.absoluteFilePath();
    }
    return result;
}

QString displayName(const QString& filePath) {
    return QFileInfo(filePath).completeBaseName();
}

QString filePathForName(const QString& name) {
    const QString clean = name.trimmed();
    if (clean.isEmpty() || clean == QLatin1String(".") ||
        clean == QLatin1String("..") || clean.endsWith(QLatin1Char('.')) ||
        clean.contains(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*])")))) {
        return {};
    }
    return QDir(stripFolder())
        .filePath(clean + QLatin1Char('.') + QLatin1String(kExtension));
}

} // namespace ui::channelstrippresets
