#include "WebPrefs.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace ui::webprefs {
namespace {

QString key(const char* name) {
    return QStringLiteral("web/") + QLatin1String(name);
}

QString appDataBase(QStandardPaths::StandardLocation location) {
    if (QCoreApplication* app = QCoreApplication::instance()) {
        const QString testRoot =
            app->property("dawHeadlessDataRoot").toString();
        if (!testRoot.isEmpty()) {
            return QDir(testRoot).filePath(
                location == QStandardPaths::CacheLocation
                    ? QStringLiteral("cache")
                    : QStringLiteral("data"));
        }
    }
    QString base = QStandardPaths::writableLocation(location);
    if (base.isEmpty()) base = QDir::tempPath() + QStringLiteral("/VLTStudioPro");
    return base;
}

bool ordinaryWebUrl(const QString& value) {
    const QUrl url(value);
    const QString scheme = url.scheme().toLower();
    return url.isValid() &&
           (scheme == QLatin1String("http") ||
            scheme == QLatin1String("https"));
}

QString normalizedWebUrl(const QString& value) {
    const QUrl url(value.trimmed());
    return ordinaryWebUrl(url.toString())
               ? url.toString(QUrl::FullyEncoded)
               : QString();
}

bool supportedStartBackground(const QFileInfo& info) {
    if (!info.isFile()) return false;
    const QString suffix = info.suffix().toLower();
    static const QStringList supported = {
        QStringLiteral("png"),  QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("webp"),
        QStringLiteral("bmp"),  QStringLiteral("gif"),
        QStringLiteral("avif"), QStringLiteral("mp4"),
        QStringLiteral("m4v"),  QStringLiteral("webm"),
        QStringLiteral("ogv"),  QStringLiteral("ogg"),
        QStringLiteral("mov")};
    return supported.contains(suffix);
}

} // namespace

bool visible() { return QSettings().value(key("visible"), false).toBool(); }
void setVisible(bool visible) { QSettings().setValue(key("visible"), visible); }

int width() {
    return std::clamp(QSettings().value(key("width"), 520).toInt(), kMinWidth,
                      kMaxWidth);
}

void setWidth(int width) {
    QSettings().setValue(key("width"),
                         std::clamp(width, kMinWidth, kMaxWidth));
}

QString homeUrl() {
    const QString stored = QSettings().value(key("homeUrl")).toString().trimmed();
    if (stored.isEmpty() || stored == QLatin1String("about:blank"))
        return QLatin1String(kStartUrl);
    return stored == QLatin1String(kStartUrl) || ordinaryWebUrl(stored)
               ? stored
               : QLatin1String(kStartUrl);
}

void setHomeUrl(const QString& url) {
    const QString value = url.trimmed();
    QSettings().setValue(
        key("homeUrl"),
        value.isEmpty() || value == QLatin1String("about:blank")
            ? QLatin1String(kStartUrl)
            : value);
}

QString startPageBackgroundPath() {
    const QFileInfo info(
        QSettings().value(key("startPageBackground")).toString().trimmed());
    return supportedStartBackground(info)
               ? QDir::cleanPath(info.absoluteFilePath())
               : QString();
}

bool setStartPageBackgroundPath(const QString& path) {
    const QFileInfo info(path.trimmed());
    if (!supportedStartBackground(info)) return false;
    QSettings().setValue(key("startPageBackground"),
                         QDir::cleanPath(info.absoluteFilePath()));
    return true;
}

void clearStartPageBackground() {
    QSettings().remove(key("startPageBackground"));
}

QString lastUrl() {
    const QString stored = QSettings().value(key("lastUrl")).toString().trimmed();
    if (stored.isEmpty() || stored == QLatin1String("about:blank"))
        return homeUrl();
    return stored == QLatin1String(kStartUrl) || ordinaryWebUrl(stored)
               ? stored
               : homeUrl();
}

void setLastUrl(const QString& url) {
    if (url == QLatin1String(kStartUrl) || ordinaryWebUrl(url)) {
        QSettings().setValue(key("lastUrl"), url);
    }
}

QList<Bookmark> bookmarks() {
    QList<Bookmark> result;
    const QByteArray encoded =
        QSettings().value(key("bookmarks")).toString().toUtf8();
    const QJsonDocument document = QJsonDocument::fromJson(encoded);
    if (!document.isArray()) return result;

    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        const QString url = normalizedWebUrl(object.value("url").toString());
        if (url.isEmpty()) continue;
        QString title = object.value("title").toString().trimmed();
        if (title.isEmpty()) title = QUrl(url).host();
        result.push_back({title.left(120), url});
        if (result.size() == 100) break;
    }
    return result;
}

bool isBookmarked(const QString& url) {
    const QString normalized = normalizedWebUrl(url);
    if (normalized.isEmpty()) return false;
    const QList<Bookmark> values = bookmarks();
    return std::any_of(values.cbegin(), values.cend(),
                       [&normalized](const Bookmark& bookmark) {
                           return bookmark.url == normalized;
                       });
}

bool addBookmark(const QString& title, const QString& url) {
    const QString normalized = normalizedWebUrl(url);
    if (normalized.isEmpty()) return false;
    QList<Bookmark> values = bookmarks();
    for (Bookmark& bookmark : values) {
        if (bookmark.url != normalized) continue;
        const QString replacement = title.trimmed().left(120);
        if (!replacement.isEmpty()) bookmark.title = replacement;
        return false;
    }
    QString safeTitle = title.trimmed().left(120);
    if (safeTitle.isEmpty()) safeTitle = QUrl(normalized).host();
    values.push_back({safeTitle, normalized});

    QJsonArray array;
    for (const Bookmark& bookmark : std::as_const(values)) {
        array.push_back(QJsonObject{{QStringLiteral("title"), bookmark.title},
                                    {QStringLiteral("url"), bookmark.url}});
    }
    QSettings().setValue(
        key("bookmarks"),
        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    return true;
}

bool removeBookmark(const QString& url) {
    const QString normalized = normalizedWebUrl(url);
    QList<Bookmark> values = bookmarks();
    const qsizetype oldSize = values.size();
    values.removeIf([&normalized](const Bookmark& bookmark) {
        return bookmark.url == normalized;
    });
    if (values.size() == oldSize) return false;

    QJsonArray array;
    for (const Bookmark& bookmark : std::as_const(values)) {
        array.push_back(QJsonObject{{QStringLiteral("title"), bookmark.title},
                                    {QStringLiteral("url"), bookmark.url}});
    }
    QSettings().setValue(
        key("bookmarks"),
        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    return true;
}

namespace {

void writeBookmarks(const QList<Bookmark>& values) {
    QJsonArray array;
    for (const Bookmark& bookmark : values) {
        array.push_back(QJsonObject{{QStringLiteral("title"), bookmark.title},
                                    {QStringLiteral("url"), bookmark.url}});
    }
    QSettings().setValue(
        key("bookmarks"),
        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

} // namespace

bool renameBookmark(const QString& url, const QString& title) {
    const QString normalized = normalizedWebUrl(url);
    const QString safe = title.trimmed().left(120);
    if (normalized.isEmpty() || safe.isEmpty()) return false;
    QList<Bookmark> values = bookmarks();
    for (Bookmark& bookmark : values) {
        if (bookmark.url != normalized) continue;
        if (bookmark.title == safe) return false;
        bookmark.title = safe;
        writeBookmarks(values);
        return true;
    }
    return false;
}

bool moveBookmark(int from, int to) {
    QList<Bookmark> values = bookmarks();
    if (from < 0 || to < 0 || from >= values.size() || to >= values.size() ||
        from == to) {
        return false;
    }
    values.move(from, to);
    writeBookmarks(values);
    return true;
}

bool bookmarksBarVisible() {
    return QSettings().value(key("bookmarksBarVisible"), true).toBool();
}

void setBookmarksBarVisible(bool visible) {
    QSettings().setValue(key("bookmarksBarVisible"), visible);
}

QString profileStoragePath() {
    return QDir(appDataBase(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Web Profile"));
}

QString profileCachePath() {
    return QDir(appDataBase(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("Web Profile"));
}

QString downloadDirectory() {
    if (QCoreApplication* app = QCoreApplication::instance()) {
        const QString testRoot =
            app->property("dawHeadlessDataRoot").toString();
        if (!testRoot.isEmpty())
            return QDir(testRoot).filePath(QStringLiteral("Downloads"));
    }
    QString path = QStandardPaths::writableLocation(
        QStandardPaths::DownloadLocation);
    if (path.isEmpty()) path = appDataBase(QStandardPaths::AppLocalDataLocation);
    return path;
}

QStringList sessionTabs() {
    QStringList result;
    const QStringList stored =
        QSettings().value(key("sessionTabs")).toStringList();
    for (const QString& url : stored) {
        const QString value = url.trimmed();
        if (value == QLatin1String(kStartUrl)) {
            result.push_back(value);
        } else if (const QString normalized = normalizedWebUrl(value);
                   !normalized.isEmpty()) {
            result.push_back(normalized);
        }
        if (result.size() == 40) break;
    }
    return result;
}

void setSessionTabs(const QStringList& urls) {
    QStringList keep;
    for (const QString& url : urls) {
        if (url == QLatin1String(kStartUrl) || ordinaryWebUrl(url)) {
            keep.push_back(url);
        }
        if (keep.size() == 40) break;
    }
    QSettings().setValue(key("sessionTabs"), keep);
}

int sessionActiveTab() {
    return std::max(0, QSettings().value(key("sessionActiveTab"), 0).toInt());
}

void setSessionActiveTab(int index) {
    QSettings().setValue(key("sessionActiveTab"), std::max(0, index));
}

QList<HistoryEntry> history() {
    QList<HistoryEntry> result;
    const QByteArray encoded =
        QSettings().value(key("history")).toString().toUtf8();
    const QJsonDocument document = QJsonDocument::fromJson(encoded);
    if (!document.isArray()) return result;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        const QString url = normalizedWebUrl(object.value("url").toString());
        if (url.isEmpty()) continue;
        QString title = object.value("title").toString().trimmed();
        if (title.isEmpty()) title = QUrl(url).host();
        result.push_back({title.left(160), url});
        if (result.size() == kMaxHistory) break;
    }
    return result;
}

void addHistoryEntry(const QString& title, const QString& url) {
    const QString normalized = normalizedWebUrl(url);
    if (normalized.isEmpty()) return;
    QList<HistoryEntry> values = history();
    // One entry per address: revisiting moves it to the front instead of
    // filling the list with the same page.
    values.removeIf([&normalized](const HistoryEntry& entry) {
        return entry.url == normalized;
    });
    QString safeTitle = title.trimmed().left(160);
    if (safeTitle.isEmpty()) safeTitle = QUrl(normalized).host();
    values.prepend({safeTitle, normalized});
    while (values.size() > kMaxHistory) values.removeLast();

    QJsonArray array;
    for (const HistoryEntry& entry : std::as_const(values)) {
        array.push_back(QJsonObject{{QStringLiteral("title"), entry.title},
                                    {QStringLiteral("url"), entry.url}});
    }
    QSettings().setValue(
        key("history"),
        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

void clearHistory() { QSettings().remove(key("history")); }

} // namespace ui::webprefs
