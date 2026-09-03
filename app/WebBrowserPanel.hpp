#pragma once

#include "platform/AudioFileDecoder.hpp"

#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QHBoxLayout;
class QLineEdit;
class QProgressBar;
class QResizeEvent;
class QShortcut;
class QStackedWidget;
class QStringListModel;
class QTabBar;
class QTimer;
class QUrl;
class QWebEngineDownloadRequest;
class QWebEngineProfile;
class QWebEngineView;
namespace ui { class IconButton; }

/// An ordinary tabbed web browser that lives beside the arrangement.
///
/// Chromium is constructed with the panel, and the shell constructs the panel
/// lazily on its first open. Hiding it afterwards keeps downloads alive.
///
/// Every tab owns a view of its own and all of them share one profile, so
/// cookies, cache and logins are the same session across tabs. What is *not*
/// shared is the loading, start-page and error state each tab is in: those
/// live on `Tab` rather than on the panel, because the toolbar has to show the
/// state of the tab in front and not of whichever tab happened to load last.
class WebBrowserPanel final : public QWidget {
    Q_OBJECT
public:
    enum class EditCommand { Cut, Copy, Paste };

    explicit WebBrowserPanel(QWidget* parent = nullptr);
    ~WebBrowserPanel() override;

    void reloadSettings();
    bool ownsFocus() const;
    bool handleEditCommand(EditCommand command);
    bool handleUndoRedo(bool redo);

    /// Deterministic headless hook: feed the same completion/probe path used by
    /// a real QWebEngineDownloadRequest, without contacting the network.
    void handleDownloadedFileForTest(const QString& path,
                                     const QString& mimeType = {});
    /// Loads an offline HTML page, then starts a genuine WebEngine download of
    /// the fixture through the profile's downloadRequested signal.
    void downloadAudioForTest(const QString& path);
    void openUrlForTest(const QString& url);
    bool startPageReadyForTest() const;
    int tabCountForTest() const;
    QStringList tabTitlesForTest() const;
    void openTabForTest(const QString& url);
    void closeCurrentTabForTest();
    void reopenClosedTabForTest();

signals:
    void statusMessage(const QString& text);
    void settingsRequested();
    void audioDownloadReady(
        const QString& path,
        const audio::platform::AudioFileInfo& info);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    /// One tab: its view and the state the toolbar reflects when it is in
    /// front. Owned by `m_tabs`; the view is parented to the stack.
    struct Tab;

    QWidget* buildTabStrip();
    QWidget* buildToolbar();
    QWidget* buildBookmarksBar();
    QWidget* buildFindBar();
    QWidget* buildDownloadBar();
    void installShortcuts();
    void applyTheme();

    // ── Tabs ──
    Tab* currentTab() const;
    QWebEngineView* view() const;
    int indexOfTab(const Tab* tab) const;
    /// Open a tab on `url` and return its index. `activate` false opens it in
    /// the background, which is what a middle-clicked link wants.
    int openTab(const QString& url, bool activate = true);
    void closeTab(int index);
    void reopenClosedTab();
    void wireTab(Tab* tab);
    void updateTabLabel(Tab* tab);
    void updateChromeForCurrentTab();
    void showTabContextMenu(int index, const QPoint& globalPos);
    void restoreSession();
    void scheduleSessionSave();
    void saveSession();

    void navigate(const QString& text);
    void showStartPage();
    void showLoadError(const QUrl& failedUrl);
    QString startPageHtml() const;
    QString errorPageHtml(const QUrl& failedUrl) const;
    void updateNavigationState();
    void updateAddress();
    QString currentPageUrl() const;

    void toggleCurrentBookmark();
    void updateBookmarkState();
    void rebuildBookmarksBar(bool refreshStartPage = true);
    void showBookmarkContextMenu(int bookmarkIndex, const QPoint& globalPos);
    void showBrowserMenu();
    void refreshCompletions();
    void recordHistory(const QString& title, const QString& url);
    void openBookmarksMenu(QWidget* anchor);
    void showFindBar();
    void closeFindBar();
    void findText(bool backward = false);

    void acceptDownload(QWebEngineDownloadRequest* request);
    void finishDownload(QWebEngineDownloadRequest* request);
    void removeDownload(QWebEngineDownloadRequest* request);
    void showDownload(QWebEngineDownloadRequest* request);
    void refreshDownloadProgress();
    void probeCompletedDownload(const QString& path, const QString& mimeType);

    QWebEngineProfile* m_profile = nullptr;
    QTabBar* m_tabBar = nullptr;
    QWidget* m_tabStrip = nullptr;
    ui::IconButton* m_newTab = nullptr;
    QStackedWidget* m_stack = nullptr;
    QList<Tab*> m_tabs;
    /// Addresses of tabs the user closed, oldest first, for Ctrl+Shift+T.
    QStringList m_closedTabs;
    QWidget* m_viewFrame = nullptr;
    QLineEdit* m_address = nullptr;
    ui::IconButton* m_back = nullptr;
    ui::IconButton* m_forward = nullptr;
    ui::IconButton* m_reloadStop = nullptr;
    ui::IconButton* m_bookmark = nullptr;
    ui::IconButton* m_menu = nullptr;
    QProgressBar* m_pageProgress = nullptr;

    QWidget* m_bookmarksBar = nullptr;
    QHBoxLayout* m_bookmarksLayout = nullptr;
    int m_bookmarkSlots = -1;

    QWidget* m_findBar = nullptr;
    QLineEdit* m_findText = nullptr;

    QWidget* m_downloadBar = nullptr;
    QLabel* m_downloadName = nullptr;
    QProgressBar* m_downloadProgress = nullptr;
    ui::IconButton* m_cancelDownload = nullptr;
    QList<QPointer<QWebEngineDownloadRequest>> m_downloads;
    QPointer<QWebEngineDownloadRequest> m_visibleDownload;
    QString m_testDownloadPath;
    QStringListModel* m_completionModel = nullptr;
    /// Coalesces session writes: a burst of tab edits is one settings write.
    QTimer* m_sessionTimer = nullptr;
    /// Coalesces responsive bookmark work during a drag.
    QTimer* m_resizeTimer = nullptr;
    bool m_initialNavigationPending = true;
    bool m_restoringSession = false;
};
