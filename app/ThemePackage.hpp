#pragma once

#include "Theme.hpp"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace ui {

struct ThemePackageResourceSource {
    QString role;
    QString path;
    /// Optional original display name when the private stored file has no
    /// useful extension (the interface font is stored as custom-font.data).
    QString fileName;
};

/// A value-only capture of the current appearance. It is safe to pass to a
/// worker thread because it contains no QObject or QSettings references.
struct ThemePackageSnapshot {
    QString name;
    Theme palette;
    QJsonObject appearance;
    QVector<ThemePackageResourceSource> resources;
};

struct ThemeLibraryEntry {
    QString packageId;
    QString name;
    QString filePath;
};

struct ThemePackageResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString filePath;
    QString storageId;
    QJsonObject manifest;
};

/// Return false to cancel a streaming operation.
using ThemePackageProgress = std::function<bool(qint64 completed, qint64 total)>;

/// Portable, versioned theme files. Media is stored as streaming resource
/// blocks after a small JSON manifest, so a multi-gigabyte video never has to
/// be resident in memory.
class ThemePackage final {
public:
    static constexpr const char* kExtension = "vlttheme";

    static QString libraryDirectory();
    static QString assetDirectory();

    static bool captureCurrent(const QString& name,
                               ThemePackageSnapshot& snapshot,
                               QString* error = nullptr);
    static ThemePackageResult write(const ThemePackageSnapshot& snapshot,
                                    const QString& destination,
                                    ThemePackageProgress progress = {});
    static ThemePackageResult saveToLibrary(
        const ThemePackageSnapshot& snapshot,
        ThemePackageProgress progress = {});
    static ThemePackageResult install(const QString& sourcePath,
                                      ThemePackageProgress progress = {});
    static ThemePackageResult inspect(const QString& path);
    static QVector<ThemeLibraryEntry> libraryEntries();

    /// Apply an installed package after all resources have been extracted.
    /// `storageId` is returned by install() and names the private asset folder.
    static ThemePackageResult apply(const QString& installedPath,
                                    const QString& storageId);

    static bool checkForTest(QString* error = nullptr);
};

} // namespace ui
