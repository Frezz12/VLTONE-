#include "StartupProjectPrefs.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QVariant>

namespace ui::startupproject {
namespace {
constexpr auto kTemplatePath = "startup/projectTemplatePath";

QString normalized(const QString& path) {
    const QString clean = path.trimmed();
    return clean.isEmpty()
        ? QString()
        : QDir::cleanPath(QFileInfo(clean).absoluteFilePath());
}
} // namespace

QString templatePath() {
    return normalized(
        QSettings().value(QLatin1String(kTemplatePath)).toString());
}

void setTemplatePath(const QString& path) {
    QSettings settings;
    const QString clean = normalized(path);
    if (clean.isEmpty())
        settings.remove(QLatin1String(kTemplatePath));
    else
        settings.setValue(QLatin1String(kTemplatePath), clean);
}

bool checkPreferencesForTest(QString* error) {
    QSettings settings;
    const QString key = QLatin1String(kTemplatePath);
    const QVariant previous = settings.value(key);
    const auto restore = [&] {
        if (previous.isValid()) settings.setValue(key, previous);
        else settings.remove(key);
    };

    settings.remove(key);
    if (!templatePath().isEmpty()) {
        if (error) *error = QStringLiteral("startup template default is not empty");
        restore();
        return false;
    }
    const QString expected = QDir(QDir::tempPath()).absoluteFilePath(
        QStringLiteral("Startup Recording.vltt"));
    setTemplatePath(expected);
    const bool roundTrip = templatePath() == QDir::cleanPath(expected);
    setTemplatePath({});
    const bool cleared = templatePath().isEmpty() && !settings.contains(key);
    restore();
    if ((!roundTrip || !cleared) && error)
        *error = QStringLiteral("startup template preference did not round-trip");
    return roundTrip && cleared;
}

} // namespace ui::startupproject
