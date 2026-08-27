#pragma once

#include <QList>
#include <QString>

/// Persistent state for the integrated web browser. Kept separate from
/// `browserprefs`: that namespace describes the local sample/preset browser,
/// while this one owns a Chromium profile and ordinary web navigation.
namespace ui::webprefs {

struct Bookmark {
    QString title;
    QString url;
};

/// Internal destination rendered by WebBrowserPanel without contacting the
/// network. Kept as a URL-shaped value so Home/last-page settings can share
/// the same storage as ordinary pages.
inline constexpr char kStartUrl[] = "vlt:start";

bool visible();
void setVisible(bool visible);

int width();
void setWidth(int width);
inline constexpr int kMinWidth = 320;
inline constexpr int kMaxWidth = 960;

/// The Home button's destination. Empty and legacy `about:blank` values are
/// migrated to the built-in VLT start page.
QString homeUrl();
void setHomeUrl(const QString& url);

/// The last ordinary page visited. Data/blob URLs are session details and are
/// not restored across launches.
QString lastUrl();
void setLastUrl(const QString& url);

/// A compact, ordered bookmark list shared by the toolbar, bookmarks bar and
/// built-in start page. URLs are restricted to HTTP(S), matching navigation.
QList<Bookmark> bookmarks();
bool isBookmarked(const QString& url);
bool addBookmark(const QString& title, const QString& url);
bool removeBookmark(const QString& url);

/// Rename an existing bookmark in place, keeping its position in the bar.
bool renameBookmark(const QString& url, const QString& title);
/// Move a bookmark to a new position, which is what dragging a chip does.
bool moveBookmark(int from, int to);

bool bookmarksBarVisible();
void setBookmarksBarVisible(bool visible);

/// The tabs that were open when the program last closed, in their bar order,
/// so reopening the panel restores the session rather than a blank page. The
/// start page is stored as `kStartUrl`, the same as every other setting here.
QStringList sessionTabs();
void setSessionTabs(const QStringList& urls);
int sessionActiveTab();
void setSessionActiveTab(int index);

struct HistoryEntry {
    QString title;
    QString url;
};

/// Where the browser has been, newest first, one entry per address — visiting
/// a page again moves it to the top rather than adding a duplicate. Feeds the
/// address bar's completion and the menu's recent list. Capped; nothing here
/// is a substitute for a real history window.
QList<HistoryEntry> history();
void addHistoryEntry(const QString& title, const QString& url);
void clearHistory();
inline constexpr int kMaxHistory = 200;

/// Browser-owned disk state. Downloads remain in the user's Downloads folder;
/// these paths are only cookies, local storage and Chromium cache.
QString profileStoragePath();
QString profileCachePath();
QString downloadDirectory();

} // namespace ui::webprefs
