#include "WebBrowserPanel.hpp"

#include "Controls.hpp"
#include "FileTypes.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "WebPrefs.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegion>
#include <QIcon>
#include <QInputDialog>
#include <QShortcut>
#include <QStackedWidget>
#include <QStringListModel>
#include <QTabBar>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

/// The application binds bare Return to transport navigation. Explicitly
/// reserve it while the address field has focus so QLineEdit reliably emits
/// `returnPressed` for both searches and URLs on every Qt/platform pair.
class BrowserAddressEdit final : public QLineEdit {
public:
    using QLineEdit::QLineEdit;

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            const auto blocked = Qt::ControlModifier | Qt::AltModifier |
                                 Qt::MetaModifier;
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                !(key->modifiers() & blocked)) {
                event->accept();
                return true;
            }
        }
        return QLineEdit::event(event);
    }
};

/// The two mouse gestures every browser's tab strip answers and QTabBar does
/// not: a middle click closes the tab under the pointer, and a double click on
/// the empty part of the strip opens a new one.
class BrowserTabBar final : public QTabBar {
public:
    using QTabBar::QTabBar;

    std::function<void(int)> closeRequested;
    std::function<void()> newTabRequested;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::MiddleButton) {
            const int index = tabAt(event->position().toPoint());
            if (index >= 0 && closeRequested) {
                closeRequested(index);
                event->accept();
                return;
            }
        }
        QTabBar::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton &&
            tabAt(event->position().toPoint()) < 0 && newTabRequested) {
            newTabRequested();
            event->accept();
            return;
        }
        QTabBar::mouseDoubleClickEvent(event);
    }
};

bool allowedMainFrameUrl(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
           (scheme == QLatin1String("about") &&
            url.toString() == QLatin1String("about:blank"));
}

bool isStartDestination(const QString& value) {
    return value.trimmed().isEmpty() ||
           value == QLatin1String(ui::webprefs::kStartUrl) ||
           value == QLatin1String("about:blank");
}

QUrl ordinaryUrlFromInput(QString input) {
    input = input.trimmed();
    if (input.isEmpty()) return {};

    const bool hasExplicitScheme = input.contains(QStringLiteral("://"));
    const bool looksLikeHost = input.contains(QLatin1Char('.')) ||
                               input.startsWith(QLatin1String("localhost"),
                                                Qt::CaseInsensitive) ||
                               QRegularExpression(
                                   QStringLiteral("^\\d{1,3}(?:\\.\\d{1,3}){3}(?::\\d+)?(?:/|$)"))
                                   .match(input)
                                   .hasMatch();
    if (!hasExplicitScheme &&
        (input.contains(QRegularExpression(QStringLiteral("\\s"))) ||
         !looksLikeHost)) {
        QUrl search(QStringLiteral("https://duckduckgo.com/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"), input);
        search.setQuery(query);
        return search;
    }

    if (!hasExplicitScheme) input.prepend(QStringLiteral("https://"));
    return QUrl::fromUserInput(input);
}

QString bookmarkLabel(const ui::webprefs::Bookmark& bookmark) {
    const QString title = bookmark.title.trimmed();
    return title.isEmpty() ? QUrl(bookmark.url).host() : title;
}

class RestrictedWebPage final : public QWebEnginePage {
public:
    RestrictedWebPage(QWebEngineProfile* profile, QObject* parent)
        : QWebEnginePage(profile, parent) {}

    std::function<void(const QUrl&)> navigationRejected;
    std::function<bool(const QUrl&)> internalNavigationAllowed;

protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type,
                                 bool isMainFrame) override {
        (void)type;
        if (!isMainFrame || allowedMainFrameUrl(url) ||
            (internalNavigationAllowed && internalNavigationAllowed(url))) {
            return true;
        }
        if (navigationRejected) navigationRejected(url);
        return false;
    }

    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString& message, int lineNumber,
                                  const QString& sourceId) override {
        const QString scheme = url().scheme().toLower();
        if (scheme == QLatin1String("http") ||
            scheme == QLatin1String("https")) {
            // QWebEnginePage's default implementation copies every external
            // site's console into the host process. Twitch alone emits WebGPU
            // fallback, EventEmitter, cross-origin and unauthenticated GraphQL
            // diagnostics during successful playback. They belong to the web
            // page, not to the DAW, and can otherwise hide real engine errors.
            return;
        }
        QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber,
                                                 sourceId);
    }
};

QString safeDownloadName(QString name) {
    name = QFileInfo(name).fileName().trimmed();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")),
                 QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    return name.isEmpty() ? QStringLiteral("download") : name;
}

QString uniqueDownloadName(const QString& directory, const QString& suggested) {
    const QString safe = safeDownloadName(suggested);
    if (!QFileInfo::exists(QDir(directory).filePath(safe))) return safe;

    const QFileInfo info(safe);
    const QString stem = info.completeBaseName().isEmpty()
                             ? QStringLiteral("download")
                             : info.completeBaseName();
    const QString suffix = info.completeSuffix();
    for (int copy = 2; copy < 10000; ++copy) {
        const QString candidate = suffix.isEmpty()
                                      ? QStringLiteral("%1 (%2)").arg(stem).arg(copy)
                                      : QStringLiteral("%1 (%2).%3")
                                            .arg(stem).arg(copy).arg(suffix);
        if (!QFileInfo::exists(QDir(directory).filePath(candidate)))
            return candidate;
    }
    return QStringLiteral("%1-%2%3")
        .arg(stem)
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
}

QString initialDownloadName(QWebEngineDownloadRequest* request) {
    QString name = request->suggestedFileName();
    if (name.isEmpty()) name = request->downloadFileName();
    if (name.isEmpty()) name = request->url().fileName();
    return safeDownloadName(name);
}

} // namespace

/// One tab. The view is parented to the stack; the flags are the state the
/// toolbar shows while this tab is in front — kept per tab because a page
/// loading in the background must not turn the visible tab's reload button
/// into a stop button.
struct WebBrowserPanel::Tab {
    QWebEngineView* view = nullptr;
    QString failedUrl;
    bool showingStartPage = false;
    bool showingErrorPage = false;
    bool loading = false;
    bool userStoppedLoading = false;
};

WebBrowserPanel::Tab* WebBrowserPanel::currentTab() const {
    const int index = m_tabBar ? m_tabBar->currentIndex() : -1;
    return index >= 0 && index < m_tabs.size() ? m_tabs.at(index) : nullptr;
}

QWebEngineView* WebBrowserPanel::view() const {
    Tab* tab = currentTab();
    return tab ? tab->view : nullptr;
}

int WebBrowserPanel::indexOfTab(const Tab* tab) const {
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i) == tab) return i;
    return -1;
}

WebBrowserPanel::WebBrowserPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("WebBrowserPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("dawWebInput", true);
    setMinimumWidth(ui::webprefs::kMinWidth);

    QDir().mkpath(ui::webprefs::profileStoragePath());
    QDir().mkpath(ui::webprefs::profileCachePath());
    m_profile = new QWebEngineProfile(QStringLiteral("VLTStudioWeb"), this);
    m_profile->setPersistentStoragePath(ui::webprefs::profileStoragePath());
    m_profile->setCachePath(ui::webprefs::profileCachePath());
    m_profile->setPersistentCookiesPolicy(
        QWebEngineProfile::ForcePersistentCookies);
    m_profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);

    auto* column = new QVBoxLayout(this);
    // A small safe inset keeps the controls below the shell's rounded crown.
    // Everything beneath it is one continuous browser surface rather than a
    // glass card containing several smaller cards.
    column->setContentsMargins(1, 7, 1, 1);
    column->setSpacing(0);
    column->addWidget(buildTabStrip());
    column->addWidget(buildToolbar());

    m_pageProgress = new QProgressBar(this);
    m_pageProgress->setObjectName(QStringLiteral("WebPageProgress"));
    m_pageProgress->setTextVisible(false);
    m_pageProgress->setFixedHeight(2);
    m_pageProgress->setAccessibleName(tr("Page loading progress"));
    m_pageProgress->hide();
    column->addWidget(m_pageProgress);
    column->addWidget(buildBookmarksBar());
    column->addWidget(buildFindBar());

    m_viewFrame = new QWidget(this);
    m_viewFrame->setObjectName(QStringLiteral("WebViewFrame"));
    m_viewFrame->setAttribute(Qt::WA_StyledBackground, true);
    auto* viewLayout = new QVBoxLayout(m_viewFrame);
    viewLayout->setContentsMargins(1, 1, 1, 1);
    viewLayout->setSpacing(0);
    m_stack = new QStackedWidget(m_viewFrame);
    m_stack->setObjectName(QStringLiteral("WebViewStack"));
    viewLayout->addWidget(m_stack);
    column->addWidget(m_viewFrame, 1);
    column->addWidget(buildDownloadBar());

    connect(m_profile, &QWebEngineProfile::downloadRequested, this,
            &WebBrowserPanel::acceptDownload, Qt::DirectConnection);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        applyTheme();
        // Both internal pages are rendered with the theme's own colours, so a
        // theme change has to redraw them — in every tab that is showing one,
        // not only the visible tab.
        for (Tab* tab : std::as_const(m_tabs)) {
            if (tab->showingStartPage) {
                tab->view->setHtml(startPageHtml(),
                                   QUrl(QStringLiteral("about:blank")));
            } else if (tab->showingErrorPage && !tab->failedUrl.isEmpty()) {
                tab->view->setHtml(errorPageHtml(QUrl(tab->failedUrl)),
                                   QUrl(QStringLiteral("about:blank")));
            }
        }
    });
    installShortcuts();
    applyTheme();

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(16);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] {
        updateViewMask();
        const int visibleSlots =
            std::clamp((std::max(width(), 320) - 125) / 105, 1, 6);
        if (visibleSlots != m_bookmarkSlots)
            rebuildBookmarksBar(/*refreshStartPage=*/false);
    });

    m_sessionTimer = new QTimer(this);
    m_sessionTimer->setSingleShot(true);
    m_sessionTimer->setInterval(400);
    connect(m_sessionTimer, &QTimer::timeout, this, &WebBrowserPanel::saveSession);

    restoreSession();

    QTimer::singleShot(0, this, [this] {
        if (!std::exchange(m_initialNavigationPending, false)) return;
        updateChromeForCurrentTab();
    });
}

WebBrowserPanel::~WebBrowserPanel() {
    for (const auto& download : std::as_const(m_downloads)) {
        if (download && !download->isFinished()) download->cancel();
    }
    // Every page keeps a reference to the profile. QObject child order would
    // otherwise tear down the older profile first and WebEngine warns (and can
    // race its helper process) during application shutdown — so all the views
    // go first, explicitly, and the profile last.
    for (Tab* tab : std::as_const(m_tabs)) {
        tab->view->disconnect(this);
        if (tab->view->page()) tab->view->page()->disconnect(this);
        delete tab->view;
        delete tab;
    }
    m_tabs.clear();
    // Views from tabs the user closed are still on the deferred-delete queue.
    // The profile must not go before them — WebEngine says so itself, in a
    // warning, right before things stop working.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    delete m_profile;
    m_profile = nullptr;
}

QWidget* WebBrowserPanel::buildTabStrip() {
    m_tabStrip = new QWidget(this);
    m_tabStrip->setObjectName(QStringLiteral("WebTabStrip"));
    m_tabStrip->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(m_tabStrip);
    row->setContentsMargins(6, 0, 5, 0);
    row->setSpacing(3);

    auto* tabs = new BrowserTabBar(m_tabStrip);
    tabs->closeRequested = [this](int index) { closeTab(index); };
    tabs->newTabRequested = [this] {
        openTab(QLatin1String(ui::webprefs::kStartUrl));
    };
    m_tabBar = tabs;
    m_tabBar->setObjectName(QStringLiteral("WebTabBar"));
    m_tabBar->setDrawBase(false);
    m_tabBar->setExpanding(false);
    m_tabBar->setMovable(true);
    m_tabBar->setTabsClosable(true);
    m_tabBar->setElideMode(Qt::ElideRight);
    m_tabBar->setUsesScrollButtons(true);
    m_tabBar->setFocusPolicy(Qt::NoFocus);
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabBar->setAccessibleName(tr("Browser tabs"));

    m_newTab = new ui::IconButton(icons::Glyph::Plus, tr("New tab (Ctrl+T)"),
                                  m_tabStrip);
    m_newTab->setButtonSize(24, 24);
    m_newTab->setFocusPolicy(Qt::StrongFocus);

    connect(m_newTab, &QAbstractButton::clicked, this,
            [this] { openTab(QLatin1String(ui::webprefs::kStartUrl)); });
    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_tabs.size() && m_stack) {
            m_stack->setCurrentWidget(m_tabs.at(index)->view);
        }
        updateChromeForCurrentTab();
        scheduleSessionSave();
    });
    connect(m_tabBar, &QTabBar::tabCloseRequested, this,
            [this](int index) { closeTab(index); });
    connect(m_tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || to < 0 || from >= m_tabs.size() || to >= m_tabs.size())
            return;
        m_tabs.move(from, to);
        scheduleSessionSave();
    });
    connect(m_tabBar, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& at) {
                showTabContextMenu(m_tabBar->tabAt(at), m_tabBar->mapToGlobal(at));
            });

    row->addWidget(m_tabBar, 1);
    row->addWidget(m_newTab);
    return m_tabStrip;
}

int WebBrowserPanel::openTab(const QString& url, bool activate) {
    auto* tab = new Tab;
    tab->view = new QWebEngineView(m_stack);
    tab->view->setObjectName(QStringLiteral("WebBrowserView"));
    tab->view->setProperty("dawWebInput", true);

    auto* page = new RestrictedWebPage(m_profile, tab->view);
    page->navigationRejected = [this](const QUrl& rejected) {
        emit statusMessage(
            tr("Blocked unsupported web address: %1").arg(rejected.toDisplayString()));
    };
    page->internalNavigationAllowed = [this, tab](const QUrl& candidate) {
        return candidate.scheme() == QLatin1String("data") &&
               (tab->showingStartPage || tab->showingErrorPage ||
                !m_testDownloadPath.isEmpty());
    };
    tab->view->setPage(page);
    tab->view->settings()->setAttribute(
        QWebEngineSettings::JavascriptCanOpenWindows, true);
    tab->view->settings()->setAttribute(
        QWebEngineSettings::FullScreenSupportEnabled, false);
    page->setBackgroundColor(th().background);

    m_tabs.push_back(tab);
    m_stack->addWidget(tab->view);
    const int index = m_tabBar->addTab(tr("New tab"));
    m_tabBar->setTabIcon(index,
                         icons::icon(icons::Glyph::Globe, th().textSecondary, 14));

    // The close button is ours rather than the style's: a stylesheet can only
    // give QTabBar's built-in one an image from a file, and every other button
    // in the program is drawn from the same glyph set.
    auto* close = new ui::IconButton(icons::Glyph::Close, tr("Close tab"), m_tabBar);
    close->setButtonSize(15, 15);
    close->setFocusPolicy(Qt::NoFocus);
    connect(close, &QAbstractButton::clicked, this, [this, tab] {
        const int at = indexOfTab(tab);
        if (at >= 0) closeTab(at);
    });
    m_tabBar->setTabButton(index, QTabBar::RightSide, close);
    wireTab(tab);

    if (activate) {
        m_tabBar->setCurrentIndex(index);
    } else {
        // A background tab still has to load, and it still has to end up
        // showing something — the chrome simply does not follow it.
        updateTabLabel(tab);
    }

    const QString destination = url.trimmed();
    if (isStartDestination(destination)) {
        tab->showingStartPage = true;
        tab->view->setHtml(startPageHtml(), QUrl(QStringLiteral("about:blank")));
        if (activate) {
            updateAddress();
            updateBookmarkState();
        }
    } else if (const QUrl target = ordinaryUrlFromInput(destination);
               allowedMainFrameUrl(target)) {
        tab->view->setUrl(target);
    } else {
        tab->showingStartPage = true;
        tab->view->setHtml(startPageHtml(), QUrl(QStringLiteral("about:blank")));
    }
    updateTabLabel(tab);
    scheduleSessionSave();

    // Where the cursor lands is decided here rather than left to whichever
    // widget WebEngine focuses when its native view is shown. A tab opened on
    // the start page has nothing to read yet, so the address bar takes it —
    // which is both what every browser does and what makes the focus state
    // deterministic instead of a race with the render process.
    if (activate && m_address) {
        if (tab->showingStartPage) m_address->setFocus(Qt::OtherFocusReason);
        else tab->view->setFocus(Qt::OtherFocusReason);
    }
    return index;
}

void WebBrowserPanel::closeTab(int index) {
    if (index < 0 || index >= m_tabs.size()) return;
    Tab* tab = m_tabs.at(index);

    // Remembered before it goes, so Ctrl+Shift+T has something to reopen.
    const QString address = tab->showingStartPage
                                ? QLatin1String(ui::webprefs::kStartUrl)
                                : tab->view->url().toString();
    if (!address.isEmpty()) {
        m_closedTabs.push_back(address);
        while (m_closedTabs.size() > 12) m_closedTabs.removeFirst();
    }

    m_tabs.removeAt(index);
    m_tabBar->removeTab(index);
    m_stack->removeWidget(tab->view);
    // Every signal from this view into the panel goes first. The view outlives
    // this call — it has to, because WebEngine has work queued on it and this
    // can run from inside the tab bar's own close-button handler — but `tab`
    // does not, and a `titleChanged` arriving in between would hand a lambda a
    // pointer to freed memory. It does happen: the page keeps talking while it
    // is being torn down.
    tab->view->disconnect(this);
    if (tab->view->page()) tab->view->page()->disconnect(this);
    // The page is a child of the view and goes with it — detaching it here
    // would leave it parented to nothing and outliving the profile, which
    // WebEngine warns about and then crashes on.
    tab->view->deleteLater();
    delete tab;

    // The panel always has a tab. Closing the last one leaves a fresh start
    // page rather than an empty grey rectangle with a dead toolbar.
    if (m_tabs.isEmpty()) {
        openTab(QLatin1String(ui::webprefs::kStartUrl));
        return;
    }
    updateChromeForCurrentTab();
    scheduleSessionSave();
}

void WebBrowserPanel::reopenClosedTab() {
    if (m_closedTabs.isEmpty()) {
        emit statusMessage(tr("No recently closed tabs"));
        return;
    }
    openTab(m_closedTabs.takeLast());
}

void WebBrowserPanel::wireTab(Tab* tab) {
    QWebEngineView* target = tab->view;

    connect(target, &QWebEngineView::loadStarted, this, [this, tab] {
        tab->loading = true;
        if (tab != currentTab()) return;
        m_reloadStop->setGlyph(icons::Glyph::Close);
        m_reloadStop->setToolTip(tr("Stop loading"));
        m_pageProgress->setRange(0, 100);
        m_pageProgress->setValue(0);
        m_pageProgress->show();
    });
    connect(target, &QWebEngineView::loadProgress, this, [this, tab](int progress) {
        if (tab == currentTab()) m_pageProgress->setValue(progress);
    });
    connect(target, &QWebEngineView::loadFinished, this, [this, tab](bool ok) {
        tab->loading = false;
        if (tab == currentTab()) {
            m_reloadStop->setGlyph(icons::Glyph::Reload);
            m_reloadStop->setToolTip(tr("Reload page"));
            m_pageProgress->hide();
            updateNavigationState();
        }
        if (!ok && !tab->userStoppedLoading && !tab->showingStartPage &&
            !tab->showingErrorPage && allowedMainFrameUrl(tab->view->url()) &&
            tab->view->url() != QUrl(QStringLiteral("about:blank"))) {
            const QUrl failed = tab->view->url();
            if (tab == currentTab()) {
                emit statusMessage(
                    tr("The page could not be loaded: %1").arg(failed.host()));
            }
            tab->showingStartPage = false;
            tab->showingErrorPage = true;
            tab->failedUrl = failed.toString();
            tab->view->setHtml(errorPageHtml(failed),
                               QUrl(QStringLiteral("about:blank")));
            if (tab == currentTab()) {
                updateAddress();
                updateBookmarkState();
            }
        }
        tab->userStoppedLoading = false;
        if (ok && !tab->showingStartPage && !tab->showingErrorPage) {
            recordHistory(tab->view->title(), tab->view->url().toString());
        }
        if (ok && !m_testDownloadPath.isEmpty() && tab == currentTab()) {
            const QString fixture = std::exchange(m_testDownloadPath, {});
            tab->view->page()->download(QUrl::fromLocalFile(fixture),
                                        QFileInfo(fixture).fileName());
        }
        updateTabLabel(tab);
    });
    connect(target, &QWebEngineView::urlChanged, this, [this, tab](const QUrl& url) {
        const bool internalData =
            url.scheme() == QLatin1String("data") &&
            (tab->showingStartPage || tab->showingErrorPage ||
             !m_testDownloadPath.isEmpty());
        if (!internalData && url != QUrl(QStringLiteral("about:blank"))) {
            tab->showingStartPage = false;
            tab->showingErrorPage = false;
            tab->failedUrl.clear();
            if (tab == currentTab()) ui::webprefs::setLastUrl(url.toString());
        } else if (tab->showingStartPage && tab == currentTab()) {
            ui::webprefs::setLastUrl(QLatin1String(ui::webprefs::kStartUrl));
        }
        if (tab == currentTab()) {
            updateAddress();
            updateNavigationState();
            updateBookmarkState();
        }
        updateTabLabel(tab);
        scheduleSessionSave();
    });
    connect(target, &QWebEngineView::titleChanged, this,
            [this, tab](const QString& title) {
                tab->view->setToolTip(title);
                updateTabLabel(tab);
                if (tab == currentTab()) updateBookmarkState();
            });
    connect(target, &QWebEngineView::iconChanged, this,
            [this, tab](const QIcon& icon) {
                const int index = indexOfTab(tab);
                if (index >= 0) {
                    m_tabBar->setTabIcon(
                        index, icon.isNull()
                                   ? icons::icon(icons::Glyph::Globe,
                                                 th().textSecondary, 14)
                                   : icon);
                }
            });

    auto* page = target->page();
    connect(page, &QWebEnginePage::permissionRequested, this,
            [](QWebEnginePermission permission) { permission.deny(); });
    connect(page, &QWebEnginePage::newWindowRequested, this,
            [this](QWebEngineNewWindowRequest& request) {
                // Every "open in a new window" gesture the page can make —
                // target=_blank, a middle click, Ctrl+click — lands here. In a
                // panel this narrow they all mean the same thing: a new tab.
                const QUrl url = request.requestedUrl();
                if (!allowedMainFrameUrl(url)) return;
                const bool background =
                    request.destination() ==
                    QWebEngineNewWindowRequest::InNewBackgroundTab;
                openTab(url.toString(), !background);
            });
}

void WebBrowserPanel::updateTabLabel(Tab* tab) {
    const int index = indexOfTab(tab);
    if (index < 0) return;
    QString label = tab->view->title().trimmed();
    if (tab->showingStartPage) {
        label = tr("Start");
    } else if (tab->showingErrorPage) {
        label = QUrl(tab->failedUrl).host();
    } else if (label.isEmpty() || label.startsWith(QLatin1String("data:"))) {
        label = tab->view->url().host();
    }
    if (label.isEmpty()) label = tr("New tab");
    m_tabBar->setTabText(index, label);
    m_tabBar->setTabToolTip(index, tab->showingStartPage
                                       ? tr("Start page")
                                       : tab->view->url().toDisplayString());
}

void WebBrowserPanel::updateChromeForCurrentTab() {
    Tab* tab = currentTab();
    if (!tab) return;
    if (m_reloadStop) {
        m_reloadStop->setGlyph(tab->loading ? icons::Glyph::Close
                                            : icons::Glyph::Reload);
        m_reloadStop->setToolTip(tab->loading ? tr("Stop loading")
                                              : tr("Reload page"));
    }
    if (m_pageProgress) m_pageProgress->setVisible(tab->loading);
    updateAddress();
    updateNavigationState();
    updateBookmarkState();
    // The find bar belongs to the page that was in front, so it goes — but
    // quietly. `closeFindBar` puts the focus back into the view, and switching
    // tabs is not a reason to take focus away from whatever the user is
    // typing in, address bar included.
    if (m_findBar && m_findBar->isVisible()) {
        if (QWebEngineView* target = view()) target->page()->findText(QString());
        m_findBar->hide();
    }
}

void WebBrowserPanel::showTabContextMenu(int index, const QPoint& globalPos) {
    QMenu menu(this);
    QAction* newTab = menu.addAction(tr("New tab"));
    connect(newTab, &QAction::triggered, this,
            [this] { openTab(QLatin1String(ui::webprefs::kStartUrl)); });

    if (index >= 0 && index < m_tabs.size()) {
        Tab* tab = m_tabs.at(index);
        QAction* reload = menu.addAction(tr("Reload"));
        connect(reload, &QAction::triggered, this,
                [tab] { tab->view->reload(); });
        QAction* duplicate = menu.addAction(tr("Duplicate tab"));
        connect(duplicate, &QAction::triggered, this, [this, tab] {
            openTab(tab->showingStartPage
                        ? QLatin1String(ui::webprefs::kStartUrl)
                        : tab->view->url().toString());
        });
        menu.addSeparator();
        QAction* close = menu.addAction(tr("Close tab"));
        connect(close, &QAction::triggered, this,
                [this, index] { closeTab(index); });
        QAction* others = menu.addAction(tr("Close other tabs"));
        others->setEnabled(m_tabs.size() > 1);
        connect(others, &QAction::triggered, this, [this, tab] {
            for (int i = m_tabs.size() - 1; i >= 0; --i) {
                if (m_tabs.at(i) != tab) closeTab(i);
            }
        });
    }
    QAction* reopen = menu.addAction(tr("Reopen closed tab"));
    reopen->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    reopen->setEnabled(!m_closedTabs.isEmpty());
    connect(reopen, &QAction::triggered, this, &WebBrowserPanel::reopenClosedTab);
    menu.exec(globalPos);
}

void WebBrowserPanel::restoreSession() {
    m_restoringSession = true;
    QStringList urls = ui::webprefs::sessionTabs();
    if (urls.isEmpty()) {
        // Nothing saved: fall back to the single page the old, tabless panel
        // would have opened, so an existing install does not lose its place.
        urls.push_back(ui::webprefs::lastUrl());
    }
    for (const QString& url : std::as_const(urls)) openTab(url, /*activate=*/false);
    const int active =
        std::clamp(ui::webprefs::sessionActiveTab(), 0, int(m_tabs.size()) - 1);
    m_tabBar->setCurrentIndex(active);
    if (m_stack && active >= 0 && active < m_tabs.size())
        m_stack->setCurrentWidget(m_tabs.at(active)->view);
    m_restoringSession = false;
    updateChromeForCurrentTab();
}

void WebBrowserPanel::scheduleSessionSave() {
    if (m_restoringSession || !m_sessionTimer) return;
    m_sessionTimer->start();
}

void WebBrowserPanel::saveSession() {
    QStringList urls;
    urls.reserve(m_tabs.size());
    for (const Tab* tab : std::as_const(m_tabs)) {
        urls.push_back(tab->showingStartPage
                           ? QLatin1String(ui::webprefs::kStartUrl)
                           : tab->view->url().toString());
    }
    ui::webprefs::setSessionTabs(urls);
    ui::webprefs::setSessionActiveTab(m_tabBar ? m_tabBar->currentIndex() : 0);
}

void WebBrowserPanel::recordHistory(const QString& title, const QString& url) {
    ui::webprefs::addHistoryEntry(title, url);
    refreshCompletions();
}

QWidget* WebBrowserPanel::buildToolbar() {
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("WebToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(toolbar);
    row->setContentsMargins(6, 5, 6, 5);
    row->setSpacing(2);

    const auto button = [toolbar](icons::Glyph glyph, const QString& tip) {
        auto* result = new ui::IconButton(glyph, tip, toolbar);
        result->setButtonSize(28, 26);
        result->setFocusPolicy(Qt::StrongFocus);
        return result;
    };

    m_back = button(icons::Glyph::ArrowLeft, tr("Back (Alt+Left)"));
    m_forward = button(icons::Glyph::ArrowRight, tr("Forward (Alt+Right)"));
    m_reloadStop = button(icons::Glyph::Reload, tr("Reload page"));
    auto* home = button(icons::Glyph::Home, tr("Home (Alt+Home)"));
    m_bookmark = button(icons::Glyph::Star, tr("Bookmark this page (Ctrl+D)"));
    m_bookmark->setCheckable(true);
    m_menu = button(icons::Glyph::Gear, tr("Browser menu"));

    m_address = new BrowserAddressEdit(toolbar);
    m_address->setObjectName(QStringLiteral("WebAddress"));
    m_address->setPlaceholderText(tr("Search or enter address"));
    m_address->setClearButtonEnabled(true);
    m_address->addAction(
        icons::icon(icons::Glyph::Globe, th().textSecondary, 14),
        QLineEdit::LeadingPosition);
    m_address->setAccessibleName(tr("Web address"));
    m_address->setProperty("dawWebInput", true);

    // Typing an address offers what has already been saved or visited. The
    // popup is a plain list of URLs — no search suggestions leave the machine.
    m_completionModel = new QStringListModel(this);
    auto* completer = new QCompleter(m_completionModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(8);
    m_address->setCompleter(completer);
    refreshCompletions();

    connect(m_back, &QAbstractButton::clicked, this, [this] {
        if (QWebEngineView* target = view()) target->back();
    });
    connect(m_forward, &QAbstractButton::clicked, this, [this] {
        if (QWebEngineView* target = view()) target->forward();
    });
    connect(m_reloadStop, &QAbstractButton::clicked, this, [this] {
        Tab* tab = currentTab();
        if (!tab) return;
        if (tab->loading) {
            tab->userStoppedLoading = true;
            tab->view->stop();
        } else if (tab->showingStartPage) {
            showStartPage();
        } else if (tab->showingErrorPage && !tab->failedUrl.isEmpty()) {
            navigate(tab->failedUrl);
        } else {
            tab->view->reload();
        }
    });
    connect(home, &QAbstractButton::clicked, this,
            [this] { navigate(ui::webprefs::homeUrl()); });
    connect(m_bookmark, &QAbstractButton::clicked, this,
            &WebBrowserPanel::toggleCurrentBookmark);
    connect(m_menu, &QAbstractButton::clicked, this,
            &WebBrowserPanel::showBrowserMenu);
    connect(m_address, &QLineEdit::returnPressed, this,
            [this] { navigate(m_address->text()); });

    row->addWidget(m_back);
    row->addWidget(m_forward);
    row->addWidget(m_reloadStop);
    row->addWidget(home);
    row->addWidget(m_address, 1);
    row->addWidget(m_bookmark);
    row->addWidget(m_menu);
    return toolbar;
}

QWidget* WebBrowserPanel::buildBookmarksBar() {
    m_bookmarksBar = new QWidget(this);
    m_bookmarksBar->setObjectName(QStringLiteral("WebBookmarksBar"));
    m_bookmarksBar->setAttribute(Qt::WA_StyledBackground, true);
    m_bookmarksLayout = new QHBoxLayout(m_bookmarksBar);
    m_bookmarksLayout->setContentsMargins(7, 3, 7, 3);
    m_bookmarksLayout->setSpacing(4);
    m_bookmarksBar->setFixedHeight(29);
    m_bookmarksBar->setVisible(ui::webprefs::bookmarksBarVisible());
    rebuildBookmarksBar();
    return m_bookmarksBar;
}

QWidget* WebBrowserPanel::buildFindBar() {
    m_findBar = new QWidget(this);
    m_findBar->setObjectName(QStringLiteral("WebFindBar"));
    m_findBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(m_findBar);
    row->setContentsMargins(8, 4, 6, 4);
    row->setSpacing(4);

    auto* label = new QLabel(tr("FIND"), m_findBar);
    label->setObjectName(QStringLiteral("WebFindLabel"));
    m_findText = new QLineEdit(m_findBar);
    m_findText->setObjectName(QStringLiteral("WebFindText"));
    m_findText->setPlaceholderText(tr("Find in page"));
    m_findText->setClearButtonEnabled(true);
    m_findText->setProperty("dawWebInput", true);
    m_findText->setAccessibleName(tr("Find in web page"));

    const auto button = [this](icons::Glyph glyph, const QString& tip) {
        auto* result = new ui::IconButton(glyph, tip, m_findBar);
        result->setButtonSize(27, 25);
        result->setFocusPolicy(Qt::StrongFocus);
        return result;
    };
    auto* previous = button(icons::Glyph::ArrowUp, tr("Previous match"));
    auto* next = button(icons::Glyph::ArrowDown, tr("Next match"));
    auto* close = button(icons::Glyph::Close, tr("Close find"));

    connect(m_findText, &QLineEdit::textChanged, this,
            [this] { findText(false); });
    connect(m_findText, &QLineEdit::returnPressed, this,
            [this] { findText(false); });
    connect(previous, &QAbstractButton::clicked, this,
            [this] { findText(true); });
    connect(next, &QAbstractButton::clicked, this,
            [this] { findText(false); });
    connect(close, &QAbstractButton::clicked, this,
            &WebBrowserPanel::closeFindBar);

    row->addWidget(label);
    row->addWidget(m_findText, 1);
    row->addWidget(previous);
    row->addWidget(next);
    row->addWidget(close);
    m_findBar->hide();
    return m_findBar;
}

QWidget* WebBrowserPanel::buildDownloadBar() {
    m_downloadBar = new QWidget(this);
    m_downloadBar->setObjectName(QStringLiteral("WebDownloadBar"));
    m_downloadBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(m_downloadBar);
    row->setContentsMargins(8, 5, 5, 5);
    row->setSpacing(7);

    auto* mark = new QLabel(m_downloadBar);
    mark->setPixmap(
        icons::icon(icons::Glyph::Download, th().accentHighlight, 15).pixmap(15, 15));
    mark->setAccessibleName(tr("Download"));
    m_downloadName = new QLabel(m_downloadBar);
    m_downloadName->setObjectName(QStringLiteral("WebDownloadName"));
    m_downloadProgress = new QProgressBar(m_downloadBar);
    m_downloadProgress->setObjectName(QStringLiteral("WebDownloadProgress"));
    m_downloadProgress->setTextVisible(false);
    m_downloadProgress->setAccessibleName(tr("Download progress"));
    m_downloadProgress->setFixedWidth(92);
    m_downloadProgress->setFixedHeight(6);
    m_cancelDownload = new ui::IconButton(icons::Glyph::Close,
                                           tr("Cancel download"), m_downloadBar);
    m_cancelDownload->setButtonSize(26, 24);
    m_cancelDownload->setFocusPolicy(Qt::StrongFocus);
    connect(m_cancelDownload, &QAbstractButton::clicked, this, [this] {
        if (m_visibleDownload) m_visibleDownload->cancel();
    });

    row->addWidget(mark);
    row->addWidget(m_downloadName, 1);
    row->addWidget(m_downloadProgress);
    row->addWidget(m_cancelDownload);
    m_downloadBar->hide();
    return m_downloadBar;
}

void WebBrowserPanel::reloadSettings() {
    if (m_bookmarksBar)
        m_bookmarksBar->setVisible(ui::webprefs::bookmarksBarVisible());
    rebuildBookmarksBar();
    Tab* tab = currentTab();
    if (tab && (tab->showingStartPage || tab->view->url().isEmpty())) {
        navigate(ui::webprefs::homeUrl());
    }
    refreshCompletions();
}

void WebBrowserPanel::navigate(const QString& text) {
    QString input = text.trimmed();
    if (isStartDestination(input)) {
        showStartPage();
        return;
    }

    const QUrl url = ordinaryUrlFromInput(input);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() ||
        (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
        emit statusMessage(tr("Enter an HTTP or HTTPS address"));
        m_address->setFocus();
        m_address->selectAll();
        return;
    }
    Tab* tab = currentTab();
    if (!tab) return;
    tab->showingStartPage = false;
    tab->showingErrorPage = false;
    tab->failedUrl.clear();
    m_address->setText(url.toDisplayString());
    tab->view->setUrl(url);
}

QString WebBrowserPanel::startPageHtml() const {
    const Theme& t = th();
    QString saved;
    const QList<ui::webprefs::Bookmark> values = ui::webprefs::bookmarks();
    for (const ui::webprefs::Bookmark& bookmark : values) {
        const QUrl url(bookmark.url);
        QString initial = bookmarkLabel(bookmark).left(1).toUpper().toHtmlEscaped();
        if (initial.isEmpty()) initial = QStringLiteral("•");
        saved += QStringLiteral(
                     "<a class='saved' href='%1'><span class='mark'>%2</span>"
                     "<span><strong>%3</strong><small>%4</small></span></a>")
                     .arg(url.toString(QUrl::FullyEncoded).toHtmlEscaped(),
                          initial, bookmarkLabel(bookmark).toHtmlEscaped(),
                          url.host().toHtmlEscaped());
    }
    if (saved.isEmpty()) {
        saved = QStringLiteral(
            "<div class='empty'><b>No bookmarks yet</b><span>Open a page and "
            "press Ctrl+D to keep it here.</span></div>");
    }

    QString html = QStringLiteral(R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VLT Start</title><style>
:root{color-scheme:%MODE%;--bg:%BG%;--surface:%SURFACE%;--raised:%RAISED%;
--text:%TEXT%;--muted:%MUTED%;--accent:%ACCENT%;--line:%LINE%;}
*{box-sizing:border-box}html,body{margin:0;min-height:100%;background:var(--bg);
color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
body{display:flex;justify-content:center}.page{width:min(720px,100%);padding:48px 34px 42px}
.eyebrow{color:var(--accent);font-size:11px;font-weight:750;letter-spacing:.18em}
h1{margin:9px 0 8px;font-size:clamp(30px,6vw,54px);line-height:.98;letter-spacing:-.045em}
.lead{margin:0 0 25px;color:var(--muted);font-size:13px;line-height:1.55}
form{display:flex;align-items:center;height:46px;padding:0 7px 0 15px;background:var(--surface);
border:1px solid var(--line);border-radius:13px}input{min-width:0;flex:1;border:0;outline:0;
background:transparent;color:var(--text);font-size:14px}button{height:32px;padding:0 15px;
border:0;border-radius:9px;background:var(--accent);color:#fff;font-weight:700;cursor:pointer}
button:focus-visible,a:focus-visible,input:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
.section{margin-top:27px}.section-head{display:flex;justify-content:space-between;align-items:center;
margin-bottom:10px}.section h2{margin:0;font-size:11px;letter-spacing:.12em;text-transform:uppercase}
.section small{color:var(--muted)}.quick,.saved-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:7px}
a{color:inherit;text-decoration:none}.quick a,.saved{min-width:0;display:flex;align-items:center;gap:10px;
padding:11px 12px;background:var(--surface);border:1px solid var(--line);border-radius:11px;
transition:background .16s ease,border-color .16s ease}.quick a:hover,.saved:hover{background:var(--raised);border-color:var(--accent)}
.quick b,.mark{display:grid;place-items:center;width:25px;height:25px;flex:0 0 25px;border-radius:7px;
background:var(--raised);color:var(--accent);font-size:11px}.saved span:last-child{min-width:0;display:flex;flex-direction:column}
.saved strong,.saved small{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.saved strong{font-size:12px}
.saved small{margin-top:2px;font-size:10px;color:var(--muted)}.empty{grid-column:1/-1;padding:17px;
border:1px dashed var(--line);border-radius:11px;color:var(--muted);display:flex;flex-direction:column;gap:4px;font-size:12px}
.empty b{color:var(--text)}.privacy{margin-top:24px;color:var(--muted);font-size:10px;text-align:center}
@media(max-width:430px){.page{padding:34px 18px}.quick,.saved-grid{grid-template-columns:1fr}h1{font-size:34px}}
@media(prefers-reduced-motion:reduce){*{transition:none!important}}
</style></head><body><main class="page"><div class="eyebrow">VLT WEB</div>
<h1>Your sound starts here.</h1><p class="lead">Browse, download audio and bring it into the project without leaving the DAW.</p>
<form action="https://duckduckgo.com/" method="get"><input name="q" autofocus autocomplete="off"
placeholder="Search the web"><button type="submit">Search</button></form>
<section class="section"><div class="section-head"><h2>Quick access</h2><small>Single-tab workspace</small></div>
<div class="quick"><a href="https://www.google.com"><b>G</b>Google</a>
<a href="https://www.youtube.com"><b>YT</b>YouTube</a>
<a href="https://soundcloud.com"><b>SC</b>SoundCloud</a>
<a href="https://bandcamp.com"><b>BC</b>Bandcamp</a></div></section>
<section class="section"><div class="section-head"><h2>Bookmarks</h2><small>Ctrl+D on any page</small></div>
<div class="saved-grid">%BOOKMARKS%</div></section>
<div class="privacy">Cookies and site data stay in a separate VLT browser profile.</div>
</main></body></html>)HTML");
    html.replace(QStringLiteral("%MODE%"),
                 t.dark ? QStringLiteral("dark") : QStringLiteral("light"));
    html.replace(QStringLiteral("%BG%"), t.background.name());
    html.replace(QStringLiteral("%SURFACE%"), t.surface.name());
    html.replace(QStringLiteral("%RAISED%"), t.surfaceElevated.name());
    html.replace(QStringLiteral("%TEXT%"), t.textPrimary.name());
    html.replace(QStringLiteral("%MUTED%"), t.textSecondary.name());
    html.replace(QStringLiteral("%ACCENT%"), t.accentHighlight.name());
    html.replace(QStringLiteral("%LINE%"), t.separator().name());
    html.replace(QStringLiteral("%BOOKMARKS%"), saved);
    return html;
}

QString WebBrowserPanel::errorPageHtml(const QUrl& failedUrl) const {
    const Theme& t = th();
    QString html = QStringLiteral(R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Page unavailable</title><style>:root{color-scheme:%MODE%}*{box-sizing:border-box}body{margin:0;min-height:100vh;
display:grid;place-items:center;padding:28px;background:%BG%;color:%TEXT%;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
main{width:min(460px,100%);padding:25px;background:%SURFACE%;border:1px solid %LINE%;border-radius:16px}
.code{color:%ACCENT%;font-size:11px;font-weight:750;letter-spacing:.14em}h1{font-size:25px;margin:8px 0}
p{color:%MUTED%;font-size:13px;line-height:1.55;word-break:break-word}a{display:inline-flex;margin-top:7px;padding:9px 14px;
border-radius:9px;background:%ACCENT%;color:#fff;text-decoration:none;font-weight:700;font-size:12px}a:focus-visible{outline:2px solid %ACCENT%;outline-offset:3px}
</style></head><body><main role="alert"><div class="code">CONNECTION ERROR</div><h1>Page unavailable</h1>
<p>Check the address and your internet connection, then try again.</p><p>%URL%</p>
<a href="%HREF%">Try again</a></main></body></html>)HTML");
    html.replace(QStringLiteral("%MODE%"),
                 t.dark ? QStringLiteral("dark") : QStringLiteral("light"));
    html.replace(QStringLiteral("%BG%"), t.background.name());
    html.replace(QStringLiteral("%SURFACE%"), t.surface.name());
    html.replace(QStringLiteral("%TEXT%"), t.textPrimary.name());
    html.replace(QStringLiteral("%MUTED%"), t.textSecondary.name());
    html.replace(QStringLiteral("%ACCENT%"), t.accentHighlight.name());
    html.replace(QStringLiteral("%LINE%"), t.separator().name());
    html.replace(QStringLiteral("%URL%"), failedUrl.toDisplayString().toHtmlEscaped());
    html.replace(QStringLiteral("%HREF%"),
                 failedUrl.toString(QUrl::FullyEncoded).toHtmlEscaped());
    return html;
}

void WebBrowserPanel::showStartPage() {
    Tab* tab = currentTab();
    if (!tab) return;
    tab->showingStartPage = true;
    tab->showingErrorPage = false;
    tab->failedUrl.clear();
    ui::webprefs::setLastUrl(QLatin1String(ui::webprefs::kStartUrl));
    updateAddress();
    updateBookmarkState();
    updateTabLabel(tab);
    tab->view->setHtml(startPageHtml(), QUrl(QStringLiteral("about:blank")));
}

void WebBrowserPanel::showLoadError(const QUrl& failedUrl) {
    if (!failedUrl.isValid()) return;
    Tab* tab = currentTab();
    if (!tab) return;
    tab->showingStartPage = false;
    tab->showingErrorPage = true;
    tab->failedUrl = failedUrl.toString();
    updateAddress();
    updateBookmarkState();
    updateTabLabel(tab);
    tab->view->setHtml(errorPageHtml(failedUrl), QUrl(QStringLiteral("about:blank")));
}

void WebBrowserPanel::updateNavigationState() {
    QWebEngineView* target = view();
    if (!target) return;
    m_back->setEnabled(target->history()->canGoBack());
    m_forward->setEnabled(target->history()->canGoForward());
}

QString WebBrowserPanel::currentPageUrl() const {
    Tab* tab = currentTab();
    if (!tab) return {};
    if (tab->showingStartPage) return QLatin1String(ui::webprefs::kStartUrl);
    if (tab->showingErrorPage) return tab->failedUrl;
    const QUrl url = tab->view->url();
    return allowedMainFrameUrl(url) &&
                   url != QUrl(QStringLiteral("about:blank"))
               ? url.toString()
               : QString();
}

void WebBrowserPanel::updateAddress() {
    if (!m_address) return;
    Tab* tab = currentTab();
    if (!tab) return;
    if (tab->showingStartPage) {
        m_address->clear();
        return;
    }
    const QString value = tab->showingErrorPage
                              ? tab->failedUrl
                              : tab->view->url().toDisplayString();
    m_address->setText(value == QLatin1String("about:blank") ? QString() : value);
}

void WebBrowserPanel::toggleCurrentBookmark() {
    const QString url = currentPageUrl();
    if (url.isEmpty() || url == QLatin1String(ui::webprefs::kStartUrl)) return;
    if (ui::webprefs::isBookmarked(url)) {
        ui::webprefs::removeBookmark(url);
        emit statusMessage(tr("Bookmark removed"));
    } else {
        QString title = view() ? view()->title().trimmed() : QString();
        if (title.isEmpty()) title = QUrl(url).host();
        ui::webprefs::addBookmark(title, url);
        emit statusMessage(tr("Bookmarked %1").arg(title));
    }
    updateBookmarkState();
    rebuildBookmarksBar();
}

void WebBrowserPanel::updateBookmarkState() {
    if (!m_bookmark) return;
    const QString url = currentPageUrl();
    const bool available = !url.isEmpty() &&
                           url != QLatin1String(ui::webprefs::kStartUrl) &&
                           !(currentTab() && currentTab()->showingErrorPage);
    const bool saved = available && ui::webprefs::isBookmarked(url);
    m_bookmark->setEnabled(available);
    m_bookmark->setChecked(saved);
    m_bookmark->setToolTip(saved ? tr("Remove bookmark (Ctrl+D)")
                                 : tr("Bookmark this page (Ctrl+D)"));
}

void WebBrowserPanel::rebuildBookmarksBar(bool refreshStartPage) {
    if (!m_bookmarksLayout || !m_bookmarksBar) return;
    while (QLayoutItem* item = m_bookmarksLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }

    auto* all = new QPushButton(tr("Bookmarks"), m_bookmarksBar);
    all->setObjectName(QStringLiteral("WebBookmarksButton"));
    all->setCursor(Qt::PointingHandCursor);
    all->setAccessibleName(tr("Open all bookmarks"));
    connect(all, &QPushButton::clicked, this,
            [this, all] { openBookmarksMenu(all); });
    m_bookmarksLayout->addWidget(all);

    const QList<ui::webprefs::Bookmark> values = ui::webprefs::bookmarks();
    const int visibleSlots =
        std::clamp((std::max(width(), 320) - 125) / 105, 1, 6);
    m_bookmarkSlots = visibleSlots;
    for (int index = 0;
         index < std::min(visibleSlots, int(values.size())); ++index) {
        const ui::webprefs::Bookmark bookmark = values.at(index);
        auto* chip = new QPushButton(m_bookmarksBar);
        chip->setObjectName(QStringLiteral("WebBookmarkChip"));
        const QString label = bookmarkLabel(bookmark);
        chip->setText(QFontMetrics(chip->font()).elidedText(
            label, Qt::ElideRight, 88));
        chip->setToolTip(QStringLiteral("%1\n%2").arg(label, bookmark.url));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setMaximumWidth(100);
        chip->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(chip, &QPushButton::clicked, this,
                [this, url = bookmark.url] { navigate(url); });
        connect(chip, &QWidget::customContextMenuRequested, this,
                [this, chip, index](const QPoint& at) {
                    showBookmarkContextMenu(index, chip->mapToGlobal(at));
                });
        m_bookmarksLayout->addWidget(chip);
    }
    if (values.size() > visibleSlots) {
        auto* overflow = new QPushButton(tr("More"), m_bookmarksBar);
        overflow->setObjectName(QStringLiteral("WebBookmarksMore"));
        overflow->setCursor(Qt::PointingHandCursor);
        connect(overflow, &QPushButton::clicked, this,
                [this, overflow] { openBookmarksMenu(overflow); });
        m_bookmarksLayout->addWidget(overflow);
    }
    if (values.isEmpty()) {
        auto* hint = new QLabel(tr("Ctrl+D saves the current page"), m_bookmarksBar);
        hint->setObjectName(QStringLiteral("WebBookmarksHint"));
        m_bookmarksLayout->addWidget(hint);
    }
    m_bookmarksLayout->addStretch(1);

    if (refreshStartPage) {
        // The start page mirrors the same persistent list — in every tab that
        // is showing one, not only the visible tab.
        for (Tab* tab : std::as_const(m_tabs)) {
            if (tab->showingStartPage) {
                tab->view->setHtml(startPageHtml(),
                                   QUrl(QStringLiteral("about:blank")));
            }
        }
    }
}

void WebBrowserPanel::showBookmarkContextMenu(int bookmarkIndex,
                                              const QPoint& globalPos) {
    const QList<ui::webprefs::Bookmark> values = ui::webprefs::bookmarks();
    if (bookmarkIndex < 0 || bookmarkIndex >= values.size()) return;
    const ui::webprefs::Bookmark bookmark = values.at(bookmarkIndex);

    QMenu menu(this);
    QAction* open = menu.addAction(tr("Open"));
    connect(open, &QAction::triggered, this,
            [this, url = bookmark.url] { navigate(url); });
    QAction* openInTab = menu.addAction(tr("Open in new tab"));
    connect(openInTab, &QAction::triggered, this,
            [this, url = bookmark.url] { openTab(url); });
    menu.addSeparator();

    QAction* rename = menu.addAction(tr("Rename…"));
    connect(rename, &QAction::triggered, this, [this, bookmark] {
        bool accepted = false;
        const QString title = QInputDialog::getText(
            this, tr("Rename bookmark"), tr("Name"), QLineEdit::Normal,
            bookmark.title, &accepted);
        if (!accepted || title.trimmed().isEmpty()) return;
        if (ui::webprefs::renameBookmark(bookmark.url, title)) {
            rebuildBookmarksBar();
            emit statusMessage(tr("Renamed to %1").arg(title.trimmed()));
        }
    });
    QAction* moveLeft = menu.addAction(tr("Move left"));
    moveLeft->setEnabled(bookmarkIndex > 0);
    connect(moveLeft, &QAction::triggered, this, [this, bookmarkIndex] {
        if (ui::webprefs::moveBookmark(bookmarkIndex, bookmarkIndex - 1))
            rebuildBookmarksBar();
    });
    QAction* moveRight = menu.addAction(tr("Move right"));
    moveRight->setEnabled(bookmarkIndex + 1 < values.size());
    connect(moveRight, &QAction::triggered, this, [this, bookmarkIndex] {
        if (ui::webprefs::moveBookmark(bookmarkIndex, bookmarkIndex + 1))
            rebuildBookmarksBar();
    });
    menu.addSeparator();
    QAction* remove = menu.addAction(tr("Remove"));
    connect(remove, &QAction::triggered, this, [this, bookmark] {
        ui::webprefs::removeBookmark(bookmark.url);
        updateBookmarkState();
        rebuildBookmarksBar();
        emit statusMessage(tr("Bookmark removed"));
    });
    menu.exec(globalPos);
}

void WebBrowserPanel::refreshCompletions() {
    if (!m_completionModel) return;
    QStringList entries;
    // Bookmarks first: a saved page is a likelier destination than one that
    // merely happened to be visited.
    for (const ui::webprefs::Bookmark& bookmark : ui::webprefs::bookmarks())
        entries.push_back(bookmark.url);
    for (const ui::webprefs::HistoryEntry& entry : ui::webprefs::history()) {
        if (!entries.contains(entry.url)) entries.push_back(entry.url);
    }
    m_completionModel->setStringList(entries);
}

void WebBrowserPanel::openBookmarksMenu(QWidget* anchor) {
    QMenu menu(this);
    menu.setTitle(tr("Bookmarks"));
    const QList<ui::webprefs::Bookmark> values = ui::webprefs::bookmarks();
    if (values.isEmpty()) {
        QAction* empty = menu.addAction(tr("No bookmarks yet"));
        empty->setEnabled(false);
    } else {
        for (const ui::webprefs::Bookmark& bookmark : values) {
            QAction* action = menu.addAction(
                icons::icon(icons::Glyph::Star, th().textSecondary, 14),
                bookmarkLabel(bookmark));
            action->setToolTip(bookmark.url);
            connect(action, &QAction::triggered, this,
                    [this, url = bookmark.url] { navigate(url); });
        }
        menu.addSeparator();
        QMenu* remove = menu.addMenu(tr("Remove bookmark"));
        for (const ui::webprefs::Bookmark& bookmark : values) {
            QAction* action = remove->addAction(bookmarkLabel(bookmark));
            connect(action, &QAction::triggered, this,
                    [this, url = bookmark.url] {
                        ui::webprefs::removeBookmark(url);
                        rebuildBookmarksBar();
                        updateBookmarkState();
                    });
        }
    }
    menu.addSeparator();
    QAction* start = menu.addAction(tr("Open start page"));
    connect(start, &QAction::triggered, this,
            &WebBrowserPanel::showStartPage);
    menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void WebBrowserPanel::showBrowserMenu() {
    QMenu menu(this);
    const QString url = currentPageUrl();
    const bool canBookmark = !url.isEmpty() &&
                             url != QLatin1String(ui::webprefs::kStartUrl) &&
                             !(currentTab() && currentTab()->showingErrorPage);
    QAction* bookmark = menu.addAction(
        ui::webprefs::isBookmarked(url) ? tr("Remove bookmark")
                                        : tr("Bookmark this page"));
    bookmark->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    bookmark->setEnabled(canBookmark);
    connect(bookmark, &QAction::triggered, this,
            &WebBrowserPanel::toggleCurrentBookmark);

    QAction* showBookmarks = menu.addAction(tr("Show bookmarks bar"));
    showBookmarks->setCheckable(true);
    showBookmarks->setChecked(m_bookmarksBar->isVisible());
    connect(showBookmarks, &QAction::toggled, this, [this](bool visible) {
        ui::webprefs::setBookmarksBarVisible(visible);
        m_bookmarksBar->setVisible(visible);
    });

    QAction* find = menu.addAction(tr("Find in page"));
    find->setShortcut(QKeySequence::Find);
    connect(find, &QAction::triggered, this, &WebBrowserPanel::showFindBar);
    menu.addSeparator();

    QAction* newTab = menu.addAction(tr("New tab"));
    newTab->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(newTab, &QAction::triggered, this,
            [this] { openTab(QLatin1String(ui::webprefs::kStartUrl)); });
    QAction* closeTabAction = menu.addAction(tr("Close tab"));
    closeTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closeTabAction, &QAction::triggered, this,
            [this] { closeTab(m_tabBar->currentIndex()); });
    QAction* reopen = menu.addAction(tr("Reopen closed tab"));
    reopen->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    reopen->setEnabled(!m_closedTabs.isEmpty());
    connect(reopen, &QAction::triggered, this, &WebBrowserPanel::reopenClosedTab);

    // Where it has been. A short list in a submenu, not a history window —
    // enough to get back to this morning's page without leaving the panel.
    const QList<ui::webprefs::HistoryEntry> visited = ui::webprefs::history();
    QMenu* recent = menu.addMenu(tr("Recent pages"));
    recent->setEnabled(!visited.isEmpty());
    for (int i = 0; i < std::min<int>(visited.size(), 15); ++i) {
        const ui::webprefs::HistoryEntry entry = visited.at(i);
        QAction* item = recent->addAction(
            QFontMetrics(recent->font()).elidedText(entry.title, Qt::ElideRight, 260));
        item->setToolTip(entry.url);
        connect(item, &QAction::triggered, this,
                [this, url = entry.url] { navigate(url); });
    }
    if (!visited.isEmpty()) {
        recent->addSeparator();
        QAction* clear = recent->addAction(tr("Clear history"));
        connect(clear, &QAction::triggered, this, [this] {
            ui::webprefs::clearHistory();
            refreshCompletions();
            emit statusMessage(tr("Browsing history cleared"));
        });
    }
    menu.addSeparator();

    QAction* zoomIn = menu.addAction(tr("Zoom in"));
    QAction* zoomOut = menu.addAction(tr("Zoom out"));
    QAction* actualSize = menu.addAction(tr("Actual size"));
    connect(zoomIn, &QAction::triggered, this, [this] {
        if (QWebEngineView* target = view())
            target->setZoomFactor(std::min(5.0, target->zoomFactor() + 0.1));
    });
    connect(zoomOut, &QAction::triggered, this, [this] {
        if (QWebEngineView* target = view())
            target->setZoomFactor(std::max(0.25, target->zoomFactor() - 0.1));
    });
    connect(actualSize, &QAction::triggered, this,
            [this] {
                if (QWebEngineView* target = view()) target->setZoomFactor(1.0);
            });

    QAction* copyAddress = menu.addAction(tr("Copy page address"));
    copyAddress->setEnabled(canBookmark ||
                            (currentTab() && currentTab()->showingErrorPage));
    connect(copyAddress, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(currentPageUrl());
    });
    menu.addSeparator();
    QAction* settings = menu.addAction(tr("Browser settings…"));
    connect(settings, &QAction::triggered, this,
            &WebBrowserPanel::settingsRequested);
    menu.exec(m_menu->mapToGlobal(QPoint(0, m_menu->height())));
}

void WebBrowserPanel::showFindBar() {
    m_findBar->show();
    m_findText->setFocus(Qt::ShortcutFocusReason);
    m_findText->selectAll();
}

void WebBrowserPanel::closeFindBar() {
    if (QWebEngineView* target = view()) {
        target->page()->findText(QString());
        target->setFocus(Qt::ShortcutFocusReason);
    }
    m_findBar->hide();
}

void WebBrowserPanel::findText(bool backward) {
    QWebEngineView* target = view();
    if (!target) return;
    const QString needle = m_findText->text();
    if (needle.isEmpty()) {
        target->page()->findText(QString());
        return;
    }
    target->page()->findText(needle,
                             backward ? QWebEnginePage::FindBackward
                                      : QWebEnginePage::FindFlags());
}

void WebBrowserPanel::installShortcuts() {
    const auto add = [this](const QKeySequence& sequence,
                            const std::function<void()>& handler) {
        auto* shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, handler);
    };
    add(QKeySequence(QStringLiteral("Ctrl+L")), [this] {
        m_address->setFocus(Qt::ShortcutFocusReason);
        m_address->selectAll();
    });
    add(QKeySequence(QStringLiteral("Ctrl+T")),
        [this] { openTab(QLatin1String(ui::webprefs::kStartUrl)); });
    add(QKeySequence(QStringLiteral("Ctrl+W")),
        [this] { closeTab(m_tabBar->currentIndex()); });
    add(QKeySequence(QStringLiteral("Ctrl+Shift+T")),
        [this] { reopenClosedTab(); });
    add(QKeySequence(QStringLiteral("Ctrl+Tab")), [this] {
        if (m_tabs.size() < 2) return;
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % m_tabs.size());
    });
    add(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), [this] {
        if (m_tabs.size() < 2) return;
        const int count = int(m_tabs.size());
        m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + count - 1) % count);
    });
    // Ctrl+1…8 select that tab, Ctrl+9 the last one — the convention every
    // browser shares, and the reason 9 is not simply "the ninth tab".
    for (int slot = 1; slot <= 9; ++slot) {
        add(QKeySequence(QStringLiteral("Ctrl+%1").arg(slot)), [this, slot] {
            if (m_tabs.isEmpty()) return;
            const int index = slot == 9 ? int(m_tabs.size()) - 1 : slot - 1;
            if (index < m_tabs.size()) m_tabBar->setCurrentIndex(index);
        });
    }
    add(QKeySequence(QStringLiteral("Ctrl+D")),
        [this] { toggleCurrentBookmark(); });
    add(QKeySequence::Find, [this] { showFindBar(); });
    add(QKeySequence(QStringLiteral("Ctrl+R")), [this] {
        Tab* tab = currentTab();
        if (!tab) return;
        if (tab->showingStartPage) showStartPage();
        else if (tab->showingErrorPage) navigate(tab->failedUrl);
        else tab->view->reload();
    });
    add(QKeySequence(QStringLiteral("Alt+Left")), [this] {
        if (QWebEngineView* target = view()) target->back();
    });
    add(QKeySequence(QStringLiteral("Alt+Right")), [this] {
        if (QWebEngineView* target = view()) target->forward();
    });
    add(QKeySequence(QStringLiteral("Alt+Home")),
        [this] { navigate(ui::webprefs::homeUrl()); });
    add(QKeySequence(QStringLiteral("Ctrl++")), [this] {
        if (QWebEngineView* target = view())
            target->setZoomFactor(std::min(5.0, target->zoomFactor() + 0.1));
    });
    add(QKeySequence(QStringLiteral("Ctrl+-")), [this] {
        if (QWebEngineView* target = view())
            target->setZoomFactor(std::max(0.25, target->zoomFactor() - 0.1));
    });
    add(QKeySequence(QStringLiteral("Ctrl+0")),
        [this] {
            if (QWebEngineView* target = view()) target->setZoomFactor(1.0);
        });
    add(QKeySequence(Qt::Key_Escape), [this] {
        if (m_findBar->isVisible()) closeFindBar();
        else if (Tab* tab = currentTab(); tab && tab->loading) {
            tab->userStoppedLoading = true;
            tab->view->stop();
        }
    });
}

bool WebBrowserPanel::ownsFocus() const {
    QWidget* focus = QApplication::focusWidget();
    // Deliberately not falling back to `window()->focusWidget()` when there is
    // no application focus widget. It looks like it would make Copy reliable
    // while activation is in flux, and it does — but the main window also
    // remembers a focus widget inside this panel while a *different* window of
    // the program (a piano roll, a plugin editor) is the one being typed into,
    // and then this panel swallows that window's edit keys.
    if (!focus) return false;
    for (QWidget* current = focus; current; current = current->parentWidget()) {
        if (current == this || current->property("dawWebInput").toBool())
            return true;
    }
    return false;
}

bool WebBrowserPanel::handleEditCommand(EditCommand command) {
    if (!ownsFocus()) return false;
    if (auto* line = qobject_cast<QLineEdit*>(QApplication::focusWidget())) {
        if (command == EditCommand::Cut) line->cut();
        else if (command == EditCommand::Copy) line->copy();
        else line->paste();
        return true;
    }
    using Action = QWebEnginePage::WebAction;
    const Action action = command == EditCommand::Cut    ? Action::Cut
                          : command == EditCommand::Copy ? Action::Copy
                                                         : Action::Paste;
    if (QWebEngineView* target = view()) target->page()->triggerAction(action);
    return true;
}

bool WebBrowserPanel::handleUndoRedo(bool redo) {
    if (!ownsFocus()) return false;
    if (auto* line = qobject_cast<QLineEdit*>(QApplication::focusWidget())) {
        if (redo) line->redo();
        else line->undo();
        return true;
    }
    if (QWebEngineView* target = view())
        target->page()->triggerAction(redo ? QWebEnginePage::Redo
                                           : QWebEnginePage::Undo);
    return true;
}

void WebBrowserPanel::acceptDownload(QWebEngineDownloadRequest* request) {
    if (!request) return;
    const QString directory = ui::webprefs::downloadDirectory();
    if (!QDir().mkpath(directory)) {
        request->cancel();
        emit statusMessage(tr("Could not create the Downloads folder"));
        return;
    }

    const QString name = uniqueDownloadName(directory, initialDownloadName(request));
    request->setDownloadDirectory(directory);
    request->setDownloadFileName(name);
    m_downloads.push_back(request);
    showDownload(request);

    connect(request, &QWebEngineDownloadRequest::receivedBytesChanged, this,
            &WebBrowserPanel::refreshDownloadProgress);
    connect(request, &QWebEngineDownloadRequest::totalBytesChanged, this,
            &WebBrowserPanel::refreshDownloadProgress);
    connect(request, &QWebEngineDownloadRequest::stateChanged, this,
            [this, request](QWebEngineDownloadRequest::DownloadState state) {
                if (state == QWebEngineDownloadRequest::DownloadCompleted)
                    finishDownload(request);
                else if (state == QWebEngineDownloadRequest::DownloadCancelled) {
                    emit statusMessage(tr("Download cancelled"));
                    removeDownload(request);
                } else if (state ==
                           QWebEngineDownloadRequest::DownloadInterrupted) {
                    emit statusMessage(
                        tr("Download failed: %1").arg(request->interruptReasonString()));
                    removeDownload(request);
                }
            });
    request->accept();
    emit statusMessage(tr("Downloading %1").arg(name));
}

void WebBrowserPanel::finishDownload(QWebEngineDownloadRequest* request) {
    const QString path = QDir(request->downloadDirectory())
                             .filePath(request->downloadFileName());
    const QString mime = request->mimeType();
    emit statusMessage(tr("Downloaded %1").arg(QFileInfo(path).fileName()));
    if (!request->isSavePageDownload()) probeCompletedDownload(path, mime);
    removeDownload(request);
}

void WebBrowserPanel::removeDownload(QWebEngineDownloadRequest* request) {
    m_downloads.removeAll(request);
    if (m_visibleDownload == request) {
        m_visibleDownload.clear();
        for (auto it = m_downloads.crbegin(); it != m_downloads.crend(); ++it) {
            if (*it && !(*it)->isFinished()) {
                showDownload(*it);
                break;
            }
        }
    }
    if (!m_visibleDownload) m_downloadBar->hide();
    request->deleteLater();
}

void WebBrowserPanel::showDownload(QWebEngineDownloadRequest* request) {
    m_visibleDownload = request;
    m_downloadName->setText(request->downloadFileName());
    m_downloadName->setToolTip(request->downloadFileName());
    m_downloadBar->show();
    refreshDownloadProgress();
}

void WebBrowserPanel::refreshDownloadProgress() {
    if (!m_visibleDownload) return;
    const qint64 total = m_visibleDownload->totalBytes();
    const qint64 received = m_visibleDownload->receivedBytes();
    if (total <= 0) {
        m_downloadProgress->setRange(0, 0);
        m_downloadProgress->setAccessibleDescription(tr("Download in progress"));
    } else {
        m_downloadProgress->setRange(0, 1000);
        m_downloadProgress->setValue(
            int(std::clamp<qint64>(received * 1000 / total, 0, 1000)));
        m_downloadProgress->setAccessibleDescription(
            tr("Download %1 percent complete").arg(received * 100 / total));
    }
}

void WebBrowserPanel::probeCompletedDownload(const QString& path,
                                             const QString& mimeType) {
    if (!ui::isAudioFile(path) &&
        !mimeType.startsWith(QLatin1String("audio/"), Qt::CaseInsensitive)) {
        return;
    }

    const QPointer<WebBrowserPanel> guard(this);
    QThreadPool::globalInstance()->start([guard, path] {
        audio::platform::AudioFileInfo info;
        const audio::Result result =
            audio::platform::probeAudioFile(path.toStdString(), info);
        const bool ok = bool(result);
        const QString error = ok ? QString()
                                 : QString::fromStdString(result.message());
        QMetaObject::invokeMethod(
            qApp,
            [guard, path, info, ok, error] {
                if (!guard) return;
                if (ok) {
                    emit guard->audioDownloadReady(path, info);
                } else {
                    emit guard->statusMessage(QCoreApplication::translate(
                        "WebBrowserPanel",
                        "Downloaded audio could not be read: %1").arg(error));
                }
            },
            Qt::QueuedConnection);
    });
}

void WebBrowserPanel::handleDownloadedFileForTest(const QString& path,
                                                  const QString& mimeType) {
    probeCompletedDownload(path, mimeType);
}

void WebBrowserPanel::downloadAudioForTest(const QString& path) {
    if (!QFileInfo::exists(path)) return;
    m_initialNavigationPending = false;
    Tab* tab = currentTab();
    if (!tab) return;
    tab->showingStartPage = false;
    tab->showingErrorPage = false;
    m_testDownloadPath = QFileInfo(path).absoluteFilePath();
    tab->view->setHtml(
        QStringLiteral(
            "<!doctype html><meta charset='utf-8'><title>Offline download "
            "test</title><p>Preparing local audio download…</p>"),
        QUrl::fromLocalFile(QFileInfo(path).absolutePath() + QLatin1Char('/')));
    // Offscreen Chromium can suppress loadFinished while a preceding internal
    // page is being replaced. The download itself is still initiated by the
    // real QWebEnginePage and must travel through downloadRequested.
    QTimer::singleShot(100, this, [this, expected = m_testDownloadPath] {
        if (expected.isEmpty() || m_testDownloadPath != expected) return;
        const QString fixture = std::exchange(m_testDownloadPath, {});
        if (QWebEngineView* target = view()) {
            target->page()->download(QUrl::fromLocalFile(fixture),
                                     QFileInfo(fixture).fileName());
        }
    });
}

void WebBrowserPanel::openUrlForTest(const QString& url) {
    m_initialNavigationPending = false;
    navigate(url);
}

int WebBrowserPanel::tabCountForTest() const { return int(m_tabs.size()); }

QStringList WebBrowserPanel::tabTitlesForTest() const {
    QStringList titles;
    for (int i = 0; i < m_tabs.size(); ++i) titles << m_tabBar->tabText(i);
    return titles;
}

void WebBrowserPanel::openTabForTest(const QString& url) { openTab(url); }

void WebBrowserPanel::closeCurrentTabForTest() {
    closeTab(m_tabBar ? m_tabBar->currentIndex() : -1);
}

void WebBrowserPanel::reopenClosedTabForTest() { reopenClosedTab(); }

bool WebBrowserPanel::startPageReadyForTest() const {
    Tab* tab = currentTab();
    return tab && tab->showingStartPage &&
           tab->view->title() == QLatin1String("VLT Start") && m_address &&
           m_address->text().isEmpty();
}

void WebBrowserPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // At most one native-mask/bookmark pass per frame. The old zero-timeout
    // scheduling queued one pass per mouse move, so Chromium kept processing
    // stale resizes after the pointer had already stopped.
    if (m_resizeTimer && !m_resizeTimer->isActive()) m_resizeTimer->start();
}

void WebBrowserPanel::updateViewMask() {
    if (size().isEmpty()) return;
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), 22, 22);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
    if (m_viewFrame) m_viewFrame->clearMask();
}

void WebBrowserPanel::applyTheme() {
    const Theme& t = th();
    if (m_address) {
        if (QAction* mark = m_address->actions().isEmpty()
                                ? nullptr
                                : m_address->actions().front()) {
            mark->setIcon(icons::icon(icons::Glyph::Globe, t.textSecondary, 14));
        }
    }
    if (m_downloadBar) {
        const auto labels = m_downloadBar->findChildren<QLabel*>();
        if (!labels.isEmpty())
            labels.front()->setPixmap(
                icons::icon(icons::Glyph::Download, t.accentHighlight, 15)
                    .pixmap(15, 15));
    }
    for (Tab* tab : std::as_const(m_tabs)) {
        if (tab->view && tab->view->page())
            tab->view->page()->setBackgroundColor(t.background);
    }
    setStyleSheet(QString(R"(
#WebBrowserPanel {
    background: %SURFACE%; border: 1px solid %DIVIDER%; border-radius: 22px;
}
#WebTabStrip {
    background: transparent; border: none;
}
/* The strip is chrome, not a document widget: no frame, no base line, and the
   selected tab is lifted onto the toolbar's own surface so the two read as one
   piece rather than as a tab bar sitting on a panel. */
QTabBar#WebTabBar { background: transparent; qproperty-drawBase: 0; }
/* The tab a page is on is marked the way a selected track is marked
   everywhere else in the program: an accent edge along the top, and the
   panel's own surface underneath so the tab and the toolbar read as one
   continuous piece of chrome. */
QTabBar#WebTabBar::tab {
    color: %MUTED%; background: transparent;
    border: none; border-top: 2px solid transparent;
    border-top-left-radius: 8px; border-top-right-radius: 8px;
    padding: 2px 3px 3px 7px; margin-right: 1px; margin-top: 1px;
    min-width: 46px; max-width: 156px; height: 21px; font-size: 11px;
}
QTabBar#WebTabBar::tab:!selected { margin-top: 3px; }
QTabBar#WebTabBar::tab:!selected:hover {
    color: %TEXT%; background: %RAISED%;
}
QTabBar#WebTabBar::tab:selected {
    color: %TEXT%; background: %SURFACE%;
    border-top-color: %ACCENT_HI%; font-weight: 600;
}
QTabBar#WebTabBar::scroller { width: 22px; }
QTabBar#WebTabBar QToolButton {
    background: %SURFACE%; border: 1px solid %SEP%; border-radius: 5px;
    margin: 3px 0; color: %TEXT%;
}
/* One seam under the whole chrome block, not one under each row. */
#WebToolbar {
    background: %SURFACE%; border: none;
}
#WebBookmarksBar, #WebFindBar {
    background: %SURFACE%; border: none; border-top: 1px solid %SEP%;
}
#WebDownloadBar {
    background: %SURFACE%; border: none; border-top: 1px solid %SEP%;
}
#WebAddress, #WebFindText {
    color: %TEXT%; background: %SURFACE%; border: 1px solid %SEP%;
    border-radius: 8px; padding: 4px 7px; selection-background-color: %ACCENT%;
}
#WebAddress:focus, #WebFindText:focus {
    border: 2px solid %ACCENT_HI%; padding: 3px 6px;
}
#WebViewFrame { background: %BG%; border: none; border-top: 1px solid %SEP%; }
#WebViewStack { background: %BG%; }
#WebPageProgress, #WebDownloadProgress {
    border: none; background: %SURFACE%; border-radius: 2px;
}
#WebPageProgress::chunk, #WebDownloadProgress::chunk {
    background: %ACCENT_HI%; border-radius: 2px;
}
#WebDownloadName { color: %TEXT%; font-size: 10px; }
#WebFindLabel, #WebBookmarksHint { color: %MUTED%; font-size: 10px; }
QPushButton#WebBookmarksButton, QPushButton#WebBookmarkChip,
QPushButton#WebBookmarksMore {
    min-height: 23px; max-height: 23px; padding: 0 8px;
    color: %MUTED%; background: transparent; border: 1px solid transparent;
    border-radius: 6px; font-size: 10px;
}
QPushButton#WebBookmarksButton { color: %TEXT%; font-weight: 650; }
QPushButton#WebBookmarksButton:hover, QPushButton#WebBookmarkChip:hover,
QPushButton#WebBookmarksMore:hover {
    color: %TEXT%; background: %RAISED%; border-color: %SEP%;
}
QPushButton#WebBookmarksButton:focus, QPushButton#WebBookmarkChip:focus,
QPushButton#WebBookmarksMore:focus { border-color: %ACCENT_HI%; }
)")
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%RAISED%", t.surfaceElevated.name())
        .replace("%SEP%", t.separator().name())
        .replace("%DIVIDER%", t.sectionDivider().name())
        .replace("%ACCENT_HI%", t.accentHighlight.name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%MUTED%", t.textSecondary.name()));
    updateViewMask();
    update();
}
