#include "TimelineBackgroundPrefs.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>

#include <algorithm>

namespace ui::timelinebackgroundprefs {
namespace {

QString key(const QString& prefix, const char* name) {
    return prefix + QLatin1String(name);
}

const QString kTimelinePrefix = QStringLiteral("theme/timelineBackground/");

QString normalizedExistingFile(const QString& candidate) {
    const QFileInfo info(candidate);
    return info.exists() && info.isFile()
               ? QDir::cleanPath(info.absoluteFilePath())
               : QString();
}

bool hasSuffix(const QString& candidate, const QStringList& suffixes) {
    return suffixes.contains(QFileInfo(candidate).suffix().toLower());
}

QString storedPath(const QString& prefix) {
    const QString stored = normalizedExistingFile(
        QSettings().value(key(prefix, "path")).toString());
    return mediaKind(stored) != MediaKind::None ? stored : QString();
}

bool storePath(const QString& prefix, const QString& candidate) {
    const QString normalized = normalizedExistingFile(candidate);
    if (normalized.isEmpty() || mediaKind(normalized) == MediaKind::None)
        return false;
    QSettings().setValue(key(prefix, "path"), normalized);
    return true;
}

int storedPercent(const QString& prefix, const char* name, int fallback) {
    return std::clamp(QSettings().value(key(prefix, name), fallback).toInt(),
                      0, 100);
}

int storedBlur(const QString& prefix, int fallback) {
    return std::clamp(QSettings().value(key(prefix, "blurRadius"), fallback)
                          .toInt(),
                      0, 32);
}

Placement normalizedPlacement(int raw) {
    return raw >= int(Placement::Fill) && raw <= int(Placement::Center)
               ? Placement(raw)
               : Placement::Fill;
}

Placement storedPlacement(const QString& prefix) {
    return normalizedPlacement(QSettings().value(
        key(prefix, "placement"), int(Placement::Fill)).toInt());
}

} // namespace

MediaKind mediaKind(const QString& candidate) {
    static const QStringList images = {
        QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("webp"),
        QStringLiteral("bmp")};
    static const QStringList videos = {
        QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("webm"), QStringLiteral("ogv"),
        QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("avi")};
    if (hasSuffix(candidate, images)) return MediaKind::Image;
    if (QFileInfo(candidate).suffix().compare(QStringLiteral("gif"),
                                              Qt::CaseInsensitive) == 0)
        return MediaKind::AnimatedImage;
    if (hasSuffix(candidate, videos)) return MediaKind::Video;
    return MediaKind::None;
}

bool isSupported(const QString& candidate) {
    return mediaKind(candidate) != MediaKind::None;
}

QString path() {
    return storedPath(kTimelinePrefix);
}

bool setPath(const QString& candidate) {
    return storePath(kTimelinePrefix, candidate);
}

void clear() { QSettings().remove(key(kTimelinePrefix, "path")); }

bool enabled() {
    return QSettings().value(key(kTimelinePrefix, "enabled"), true).toBool();
}

void setEnabled(bool value) {
    QSettings().setValue(key(kTimelinePrefix, "enabled"), value);
}

int visibility() {
    return storedPercent(kTimelinePrefix, "visibility", 32);
}

void setVisibility(int percent) {
    QSettings().setValue(key(kTimelinePrefix, "visibility"),
                         std::clamp(percent, 0, 100));
}

int blurRadius() {
    return storedBlur(kTimelinePrefix, 8);
}

void setBlurRadius(int pixels) {
    QSettings().setValue(key(kTimelinePrefix, "blurRadius"),
                         std::clamp(pixels, 0, 32));
}

bool animatedBackgroundsEnabled() {
    return QSettings().value(key(kTimelinePrefix, "animatedBackgrounds"), true)
        .toBool();
}

void setAnimatedBackgroundsEnabled(bool enabled) {
    QSettings().setValue(key(kTimelinePrefix, "animatedBackgrounds"), enabled);
}

Placement placement() { return storedPlacement(kTimelinePrefix); }

void setPlacement(Placement value) {
    QSettings().setValue(key(kTimelinePrefix, "placement"), int(value));
}

bool checkPreferencesForTest(QString* error) {
    const bool ok = mediaKind(QStringLiteral("wallpaper.JPG")) ==
                        MediaKind::Image &&
                    mediaKind(QStringLiteral("motion.GIF")) ==
                        MediaKind::AnimatedImage &&
                    mediaKind(QStringLiteral("loop.webm")) ==
                        MediaKind::Video &&
                    !isSupported(QStringLiteral("script.exe")) &&
                    normalizedPlacement(-1) == Placement::Fill &&
                    normalizedPlacement(99) == Placement::Fill;
    if (!ok && error)
        *error = QStringLiteral("timeline background validation failed");
    return ok;
}

} // namespace ui::timelinebackgroundprefs

namespace ui::headerbackgroundprefs {
namespace {
using namespace ui::timelinebackgroundprefs;
const QString kPrefix = QStringLiteral("theme/headerBackground/");
}

bool enabled() {
    return QSettings().value(key(kPrefix, "enabled"), false).toBool();
}

void setEnabled(bool value) {
    QSettings().setValue(key(kPrefix, "enabled"), value);
}

QString path() { return storedPath(kPrefix); }
bool setPath(const QString& candidate) { return storePath(kPrefix, candidate); }
void clear() { QSettings().remove(key(kPrefix, "path")); }

int visibility() { return storedPercent(kPrefix, "visibility", 28); }
void setVisibility(int percent) {
    QSettings().setValue(key(kPrefix, "visibility"),
                         std::clamp(percent, 0, 100));
}

int blurRadius() { return storedBlur(kPrefix, 6); }
void setBlurRadius(int pixels) {
    QSettings().setValue(key(kPrefix, "blurRadius"),
                         std::clamp(pixels, 0, 32));
}

bool animatedBackgroundsEnabled() {
    return QSettings().value(key(kPrefix, "animatedBackgrounds"), true)
        .toBool();
}
void setAnimatedBackgroundsEnabled(bool value) {
    QSettings().setValue(key(kPrefix, "animatedBackgrounds"), value);
}

timelinebackgroundprefs::Placement placement() {
    return storedPlacement(kPrefix);
}
void setPlacement(timelinebackgroundprefs::Placement value) {
    QSettings().setValue(key(kPrefix, "placement"), int(value));
}

} // namespace ui::headerbackgroundprefs
