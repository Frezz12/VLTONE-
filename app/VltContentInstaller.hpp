#pragma once
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <functional>

namespace ui {
struct VltInstallResult {
    bool ok = false;
    bool duplicate = false;
    bool cancelled = false;
    bool languageConflict = false;
    QString path, resourceType, error;
};
class VltContentInstaller final {
public:
    using KeepGoing = std::function<bool()>;
    static QString resourceType(const QString& filename);
    static QString libraryDirectory(const QString& resourceType);
    static QString attachmentId(const QUrl& download, const QUrl& trustedOrigin);
    // Verifies bytes and SHA-256, parses the declared format without executing
    // plugins/Creator code, then atomically publishes into a native-owned path.
    // Runs on a worker thread except language packs (bounded to 5 MiB), which
    // the browser dispatches on Qt's thread.
    static VltInstallResult install(const QString& source, const QJsonObject& metadata,
                                    KeepGoing keepGoing = {}, bool replaceLanguage = false);
    static bool exportPackage(const QString& savedDirectory, const QString& destination,
                              QString& error, KeepGoing keepGoing = {});
};
}
