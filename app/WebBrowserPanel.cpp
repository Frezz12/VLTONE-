#include "WebBrowserPanel.hpp"
#include "Typography.hpp"

#include "Controls.hpp"
#include "FileTypes.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "WebPrefs.hpp"
#include "WebPermissionPolicy.hpp"
#include "WebStartPage.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QBuffer>
#include <QCoreApplication>
#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFileInfo>
#include <QImage>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QIcon>
#include <QInputDialog>
#include <QShortcut>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringListModel>
#include <QTabBar>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEngineLoadingInfo>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

QString startPageIcon(icons::Glyph glyph) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    icons::icon(glyph, th().textPrimary, 40).pixmap(40, 40).save(&buffer, "PNG");
    return QStringLiteral("<img alt='' aria-hidden='true' src='data:image/png;base64,%1'>")
        .arg(QString::fromLatin1(png.toBase64()));
}

using BrowserIconButton = ui::IconButton;

/// The application binds bare Return to transport navigation. Explicitly
/// reserve it while the address field has focus so QLineEdit reliably emits
/// `returnPressed` for both searches and URLs on every Qt/platform pair.
class BrowserAddressEdit final : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<void()> openInNewTab;

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            const auto blocked = Qt::ControlModifier | Qt::AltModifier |
                                 Qt::MetaModifier;
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                (!(key->modifiers() & blocked) ||
                 (openInNewTab && key->modifiers() == Qt::AltModifier))) {
                event->accept();
                return true;
            }
        }
        return QLineEdit::event(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (openInNewTab && event->modifiers() == Qt::AltModifier &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            openInNewTab();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
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

class BrowserLibraryDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const bool selected = option.state & QStyle::State_Selected;
        const QRectF rect = option.rect.adjusted(0, 2, -1, -2);
        painter->setPen(selected ? th().accent : th().separator());
        painter->setBrush(selected ? mixColors(th().surface, th().accent, 0.16)
            : option.state & QStyle::State_MouseOver ? th().surfaceElevated : th().surface);
        painter->drawRoundedRect(rect, Theme::cornerRadius, Theme::cornerRadius);
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        icon.paint(painter, QRect(option.rect.left() + 12, option.rect.center().y() - 9, 18, 18));
        const QRect textRect = option.rect.adjusted(42, 8, -12, -8);
        QFont title = option.font;
        title.setWeight(QFont::Medium);
        painter->setFont(title);
        painter->setPen(th().textPrimary);
        painter->drawText(textRect, Qt::AlignTop | Qt::AlignLeft,
            QFontMetrics(title).elidedText(index.data().toString(), Qt::ElideRight, textRect.width()));
        painter->setFont(option.font);
        painter->setPen(th().textSecondary);
        painter->drawText(textRect, Qt::AlignBottom | Qt::AlignLeft,
            QFontMetrics(option.font).elidedText(index.data(Qt::UserRole + 1).toString(), Qt::ElideRight, textRect.width()));
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {100, std::max(60, QFontMetrics(option.font).height() * 2 + 22)};
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

bool isVideoBackground(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QStringList videoSuffixes = {
        QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("webm"), QStringLiteral("ogv"),
        QStringLiteral("ogg"), QStringLiteral("mov")};
    return videoSuffixes.contains(suffix);
}

bool sameLocalPath(const QUrl& a, const QUrl& b) {
    if (!a.isLocalFile() || !b.isLocalFile()) return false;
    const QString left = QFileInfo(a.toLocalFile()).canonicalFilePath();
    const QString right = QFileInfo(b.toLocalFile()).canonicalFilePath();
    return !left.isEmpty() && left == right;
}

using RestrictedWebPage = ui::graphics::BrowserPage;
using BrowserSurface = ui::graphics::BrowserSurface;

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
    BrowserSurface* view = nullptr;
    QString failedUrl;
    QString pendingUrl;
    bool showingStartPage = false;
    bool showingErrorPage = false;
    bool loading = false;
    bool userStoppedLoading = false;
    bool pinned = false;
    bool muted = false;
    int progress = 0;
    QIcon favicon;
};

WebBrowserPanel::Tab* WebBrowserPanel::currentTab() const {
    const int index = m_tabBar ? m_tabBar->currentIndex() : -1;
    return index >= 0 && index < m_tabs.size() ? m_tabs.at(index) : nullptr;
}

BrowserSurface* WebBrowserPanel::view() const {
    Tab* tab = currentTab();
    return tab ? tab->view : nullptr;
}

int WebBrowserPanel::indexOfTab(const Tab* tab) const {
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i) == tab) return i;
    return -1;
}

WebBrowserPanel::WebBrowserPanel(QWidget* parent, QWebEngineProfile* profile)
    : QWidget(parent) {
    setObjectName(QStringLiteral("WebBrowserPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("dawWebInput", true);
    setMinimumWidth(ui::webprefs::kMinWidth);

    m_profile = new ui::graphics::BrowserProfile(this, profile);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(buildTabStrip());
    column->addWidget(buildToolbar());

    m_pageProgress = new QProgressBar(this);
    m_pageProgress->setObjectName(QStringLiteral("WebPageProgress"));
    m_pageProgress->setTextVisible(false);
    m_pageProgress->setFixedHeight(2);
    m_pageProgress->setAccessibleName(tr("Page loading progress"));
    column->addWidget(m_pageProgress);
    column->addWidget(buildBookmarksBar());
    column->addWidget(buildFindBar());

    m_viewFrame = new QWidget(this);
    m_viewFrame->setObjectName(QStringLiteral("WebViewFrame"));
    m_viewFrame->setAttribute(Qt::WA_StyledBackground, true);
    auto* viewLayout = new QHBoxLayout(m_viewFrame);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    viewLayout->setSpacing(0);
    viewLayout->addWidget(buildSidebar());
    m_stack = new QStackedWidget(m_viewFrame);
    m_stack->setObjectName(QStringLiteral("WebViewStack"));
    viewLayout->addWidget(m_stack, 1);
    viewLayout->addWidget(buildLibrary(), 1);
    column->addWidget(m_viewFrame, 1);
    column->addWidget(buildDownloadBar());

    connect(m_profile, &ui::graphics::BrowserProfile::downloadRequested, this,
            &WebBrowserPanel::acceptDownload, Qt::DirectConnection);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        applyTheme();
        // Refresh our start pages. Chromium owns network error documents;
        // replacing those would lose their diagnostics and navigation history.
        for (Tab* tab : std::as_const(m_tabs)) {
            if (tab->showingStartPage) {
                tab->view->setHtml(startPageHtml(),
                                   startPageBaseUrl());

            }
        }
    });
    installShortcuts();
    applyTheme();

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(16);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] {
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
    // The coalescing timer may not have fired when the application is closed.
    if (m_sessionTimer) m_sessionTimer->stop();
    saveSession();
    for (const auto& download : std::as_const(m_downloads)) {
        if (download) {
            download->disconnect(this);
            if (!download->isFinished()) download->cancel();
        }
    }
    // Every page keeps a reference to the profile. QObject child order would
    // otherwise tear down the older profile first and WebEngine warns (and can
    // race its helper process) during application shutdown — so all the views
    // go first, explicitly, and the profile last.
    for (Tab* tab : std::as_const(m_tabs)) {
        if (auto* page = static_cast<RestrictedWebPage*>(tab->view->page())) {
            page->navigationPolicy = {};
            page->navigationRejected = {};
            page->internalNavigationAllowed = {};
        }
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
    row->setContentsMargins(8, 6, 8, 4);
    row->setSpacing(4);

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
    m_tabBar->setTabsClosable(false); // Each ordinary tab supplies its own close button.
    m_tabBar->setElideMode(Qt::ElideRight);
    m_tabBar->setUsesScrollButtons(true);
    m_tabBar->setFocusPolicy(Qt::StrongFocus);
    m_tabBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_tabBar->setMinimumWidth(0);
    m_tabBar->setIconSize(QSize(14, 14));
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabBar->setAccessibleName(tr("Browser tabs"));

    m_newTab = new BrowserIconButton(icons::Glyph::Plus, tr("New tab (Ctrl+T)"),
                                  m_tabStrip);
    m_newTab->setButtonSize(27, 27);
    m_newTab->setFocusPolicy(Qt::StrongFocus);
    m_newTab->setAccessibleName(tr("New tab"));
    m_newTab->setIdleColor(th().textSecondary);
    m_tabSearch = new BrowserIconButton(icons::Glyph::Chevron, tr("Search tabs (Ctrl+Shift+A)"), m_tabStrip);
    m_tabSearch->setObjectName(QStringLiteral("WebTabSearch"));
    m_tabSearch->setCheckable(true);
    m_tabSearch->setButtonSize(28, 28);
    m_tabSearch->setFocusPolicy(Qt::StrongFocus);
    m_tabSearch->setAccessibleName(tr("Search tabs"));
    connect(m_tabSearch, &QAbstractButton::clicked, this, [this] { showLibrary(Library::Tabs); });

    connect(m_newTab, &QAbstractButton::clicked, this,
            [this] { openTab(QLatin1String(ui::webprefs::kStartUrl)); });
    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int index) {
        closeLibrary();
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
        if (m_tabs.at(from)->pinned || m_tabs.at(to)->pinned) {
            const QSignalBlocker blocker(m_tabBar);
            m_tabBar->moveTab(to, from);
            return;
        }
        m_tabs.move(from, to);
        scheduleSessionSave();
    });
    connect(m_tabBar, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& at) {
                showTabContextMenu(m_tabBar->tabAt(at), m_tabBar->mapToGlobal(at));
            });

    row->addWidget(m_tabBar, 1);
    row->addWidget(m_newTab);
    row->addWidget(m_tabSearch);
    return m_tabStrip;
}

int WebBrowserPanel::openTab(const QString& url, bool activate, bool navigateNow, bool pinned) {
    if (activate) closeLibrary();
    auto* tab = new Tab;
    tab->pinned = pinned;
    tab->view = new BrowserSurface(m_profile, m_stack);
    tab->view->setObjectName(QStringLiteral("WebBrowserView"));
    tab->view->setProperty("dawWebInput", true);

    auto* page = tab->view->page();
    page->navigationRejected = [this, tab](const QUrl& rejected) {
        if (rejected.scheme() == QLatin1String("vltone")) { emit externalNavigationRequested(rejected,tab->view->url()); return; }
        emit statusMessage(
            tr("Blocked unsupported web address: %1").arg(rejected.toDisplayString()));
    };
    page->internalNavigationAllowed = [this, tab](const QUrl& candidate) {
        const bool internalData =
            candidate.scheme() == QLatin1String("data") &&
            (tab->showingStartPage || tab->showingErrorPage ||
             !m_testDownloadPath.isEmpty());
        const QUrl startBase = startPageBaseUrl();
        return internalData ||
               (!m_testDownloadPath.isEmpty() && candidate.isLocalFile() &&
                sameLocalPath(candidate, QUrl::fromLocalFile(QFileInfo(m_testDownloadPath).absolutePath() + QLatin1Char('/')))) ||
               (tab->showingStartPage && startBase.isLocalFile() &&
                sameLocalPath(candidate, startBase));
    };
    page->setWebAttribute(
        QWebEngineSettings::JavascriptCanOpenWindows, true);
    page->setWebAttribute(
        QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    // Start pages with a local background also load the embedded font scheme.
    // Their CSP limits subresources to the explicit local/font sources.
    page->setWebAttribute(
        QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    page->setWebAttribute(
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
    if (!pinned) {
        auto* close = new BrowserIconButton(icons::Glyph::Close, tr("Close tab"), m_tabBar);
        close->setButtonSize(24, 24);
        close->setFocusPolicy(Qt::NoFocus);
        close->setIdleColor(th().textSecondary);
        close->setAccessibleName(tr("Close tab"));
        connect(close, &QAbstractButton::clicked, this, [this, tab] {
            const int at = indexOfTab(tab);
            if (at >= 0) closeTab(at);
        });
        m_tabBar->setTabButton(index, QTabBar::RightSide, close);
    }
    wireTab(tab);

    if (activate) {
        m_tabBar->setCurrentIndex(index);
    } else {
        // A background tab still has to load, and it still has to end up
        // showing something — the chrome simply does not follow it.
        updateTabLabel(tab);
    }

    const QString destination = url.trimmed();
    if (!navigateNow) {
        // newWindowRequested transfers the original navigation via openIn().
    } else if (isStartDestination(destination)) {
        tab->showingStartPage = true;
        tab->view->setHtml(startPageHtml(), startPageBaseUrl());
        if (activate) {
            updateAddress();
            updateBookmarkState();
        }
    } else if (const QUrl target = ordinaryUrlFromInput(destination);
               allowedMainFrameUrl(target)) {
        tab->pendingUrl = target.toString();
        if (activate && m_address) m_address->setText(target.toDisplayString());
        tab->view->setUrl(target);
    } else {
        tab->showingStartPage = true;
        tab->view->setHtml(startPageHtml(), startPageBaseUrl());
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
    if (tab->pinned) return;

    // Remembered before it goes, so Ctrl+Shift+T has something to reopen.
    const QString address = tabUrl(tab);
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
    if (auto* page = static_cast<RestrictedWebPage*>(tab->view->page())) {
        page->navigationPolicy = {};
        page->navigationRejected = {};
        page->internalNavigationAllowed = {};
    }
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
    BrowserSurface* target = tab->view;

    connect(target, &BrowserSurface::loadStarted, this, [this, tab] {
        tab->loading = true;
        tab->progress = 0;
        updateTabLabel(tab);
        if (!tab->showingStartPage) {
            tab->showingErrorPage = false;
            tab->failedUrl.clear();
        }
        if (tab != currentTab()) return;
        m_reloadStop->setGlyph(icons::Glyph::Close);
        m_reloadStop->setToolTip(tr("Stop loading"));
        m_pageProgress->setRange(0, 100);
        m_pageProgress->setValue(0);
        m_pageProgress->show();
    });
    connect(target, &BrowserSurface::loadProgress, this, [this, tab](int progress) {
        tab->progress = progress;
        if (tab == currentTab()) m_pageProgress->setValue(progress);
    });
    connect(target, &BrowserSurface::loadFinished, this, [this, tab](bool ok) {
        tab->loading = false;
        if (tab == currentTab()) {
            m_reloadStop->setGlyph(icons::Glyph::Reload);
            m_reloadStop->setToolTip(tr("Reload page"));
            m_pageProgress->setValue(0);
            updateNavigationState();
        }
        tab->userStoppedLoading = false;
        if (tab == currentTab() && m_findPending && m_findBar->isVisible()) {
            m_findPending = false;
            findText();
        }
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
    connect(target, &BrowserSurface::urlChanged, this, [this, tab](const QUrl& url) {
        const bool internalData =
            url.scheme() == QLatin1String("data") &&
            (tab->showingStartPage || tab->showingErrorPage ||
             !m_testDownloadPath.isEmpty());
        const bool internalStartBase =
            tab->showingStartPage && sameLocalPath(url, startPageBaseUrl());
        if (!internalData && !internalStartBase &&
            url != QUrl(QStringLiteral("about:blank"))) {
            tab->showingStartPage = false;
            tab->showingErrorPage = false;
            tab->failedUrl.clear();
            tab->pendingUrl.clear();
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
    connect(target, &BrowserSurface::titleChanged, this,
            [this, tab](const QString& title) {
                tab->view->setToolTip(title);
                updateTabLabel(tab);
                if (tab == currentTab()) updateBookmarkState();
            });
    connect(target, &BrowserSurface::iconChanged, this,
            [this, tab](const QIcon& icon) {
                tab->favicon = icon;
                updateTabLabel(tab);
            });

    auto* page = target->page();
    connect(page, &ui::graphics::BrowserPage::findTextFinished, this,
            [this, tab](int active, int total) {
        if (tab != currentTab() || !m_findBar->isVisible() || m_findText->text().isEmpty()) return;
        m_findResult->setText(total ? tr("%1 of %2").arg(active).arg(total) : tr("No matches"));
        m_findResult->setAccessibleName(m_findResult->text());
    });
    connect(page, &ui::graphics::BrowserPage::loadingChanged, this,
            [this, tab](const QWebEngineLoadingInfo& info) {
        // loadFinished(false) also means a cancelled/superseded navigation or
        // a download handoff. Never navigate from that signal: it destroys
        // the next page and the site's own HTTP error response.
        if (info.status() != QWebEngineLoadingInfo::LoadFailedStatus ||
            info.errorCode() == -3 || tab->userStoppedLoading ||
            !info.isErrorPage() || tab->showingStartPage ||
            !allowedMainFrameUrl(info.url()) || info.url().scheme() == "about") return;
        tab->showingErrorPage = true;
        tab->failedUrl = info.url().toString();
        if (tab == currentTab()) {
            emit statusMessage(tr("Could not load %1: %2 (%3). Check your connection and system proxy.")
                .arg(info.url().host(), info.errorString()).arg(info.errorCode()));
            updateAddress();
        }
        updateTabLabel(tab);
    });
    connect(page, &ui::graphics::BrowserPage::renderProcessTerminated, this,
            [this, tab](QWebEnginePage::RenderProcessTerminationStatus status, int code) {
        if (status == QWebEnginePage::NormalTerminationStatus) return;
        tab->loading = false;
        if (!tab->showingStartPage) {
            tab->failedUrl = tab->view->url().toString();
            tab->showingErrorPage = true;
        }
        if (tab == currentTab()) {
            updateChromeForCurrentTab();
            emit statusMessage(tr("The web page process stopped (%1). Reload the tab to try again.").arg(code));
        }
    });
    connect(page, &ui::graphics::BrowserPage::newWindowRequested, this,
            [this](QWebEngineNewWindowRequest& request) {
                // Every "open in a new window" gesture the page can make —
                // target=_blank, a middle click, Ctrl+click — lands here. In a
                // panel this narrow they all mean the same thing: a new tab.
                const QUrl url = request.requestedUrl();
                if (!url.isEmpty() && !allowedMainFrameUrl(url)) return;
                const bool background =
                    request.destination() ==
                    QWebEngineNewWindowRequest::InNewBackgroundTab;
                const int index = openTab(QString(), !background, false);
                m_tabs.at(index)->view->page()->openRequest(request);
            });
}

void WebBrowserPanel::updateTabLabel(Tab* tab) {
    const int index = indexOfTab(tab);
    if (index < 0) return;
    if (tab->pinned) {
        m_tabBar->setTabText(index, QStringLiteral("VLT Studio"));
        m_tabBar->setTabIcon(index, tab->muted ? icons::icon(icons::Glyph::NoteMute, th().textSecondary, 14)
                                             : QIcon(QStringLiteral(":/vlt/icon-1024.png")));
        m_tabBar->setTabToolTip(index, QLatin1String(ui::webprefs::kStudioUrl));
        return;
    }
    QString label = tab->view->title().trimmed();
    if (tab->showingStartPage) {
        label = tr("New tab");
    } else if (tab->showingErrorPage) {
        label = QUrl(tab->failedUrl).host();
    } else if (label.isEmpty() || label.startsWith(QLatin1String("data:"))) {
        label = tab->view->url().host();
    }
    if (label.isEmpty()) label = tr("New tab");
    m_tabBar->setTabIcon(index, tab->muted ? icons::icon(icons::Glyph::NoteMute, th().textSecondary, 14)
        : tab->loading ? icons::icon(icons::Glyph::Spinner, th().accentHighlight, 14)
        : tab->showingStartPage ? icons::icon(icons::Glyph::Home, th().accentHighlight, 14)
        : tab->showingErrorPage ? icons::icon(icons::Glyph::Warning, th().textSecondary, 14)
        : tab->favicon.isNull() ? icons::icon(icons::Glyph::Globe, th().textSecondary, 14) : tab->favicon);
    m_tabBar->setTabText(index, label);
    m_tabBar->setTabToolTip(index, tab->showingStartPage
                                       ? tr("Start page")
                                       : tab->view->url().toDisplayString());
    if (m_libraryKind == Library::Tabs) refreshLibrary();
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
    if (m_pageProgress) m_pageProgress->setValue(tab->loading ? tab->progress : 0);
    if (m_zoom) {
        m_zoom->setText(QStringLiteral("%1%").arg(qRound(tab->view->zoomFactor() * 100)));
        m_zoom->setVisible(!qFuzzyCompare(tab->view->zoomFactor(), 1.0));
    }
    updateAddress();
    updateNavigationState();
    updateBookmarkState();
    // The find bar belongs to the page that was in front, so it goes — but
    // quietly. `closeFindBar` puts the focus back into the view, and switching
    // tabs is not a reason to take focus away from whatever the user is
    // typing in, address bar included.
    if (m_findBar && m_findBar->isVisible()) {
        m_findPending = false;
        for (Tab* candidate : std::as_const(m_tabs)) candidate->view->page()->findText(QString());
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
            openTab(tabUrl(tab));
        });
        QAction* mute = menu.addAction(tab->muted ? tr("Unmute tab") : tr("Mute tab"));
        mute->setObjectName(QStringLiteral("WebMuteTab"));
        connect(mute, &QAction::triggered, this, [this, tab] { setTabMuted(tab, !tab->muted); });
        QAction* muteOthers = menu.addAction(tr("Mute other tabs"));
        connect(muteOthers, &QAction::triggered, this, [this, tab] {
            for (Tab* other : std::as_const(m_tabs)) if (other != tab) setTabMuted(other, true);
        });
        menu.addSeparator();
        QAction* close = menu.addAction(tr("Close tab"));
        close->setEnabled(!tab->pinned);
        connect(close, &QAction::triggered, this,
                [this, index] { closeTab(index); });
        QAction* others = menu.addAction(tr("Close other tabs"));
        others->setEnabled(std::any_of(m_tabs.cbegin(), m_tabs.cend(),
                                      [tab](const Tab* other) { return other != tab && !other->pinned; }));
        connect(others, &QAction::triggered, this, [this, tab] {
            for (int i = m_tabs.size() - 1; i >= 0; --i) {
                if (m_tabs.at(i) != tab) closeTab(i);
            }
        });
        QAction* right = menu.addAction(tr("Close tabs to the right"));
        right->setEnabled(index < m_tabs.size() - 1);
        connect(right, &QAction::triggered, this, [this, tab] {
            const int at = indexOfTab(tab);
            if (at < 0) return;
            for (int i = int(m_tabs.size()) - 1; i > at; --i) closeTab(i);
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
    int active = std::clamp(ui::webprefs::sessionActiveTab(), 0, int(urls.size()) - 1);
    const int studio = urls.indexOf(QLatin1String(ui::webprefs::kStudioUrl));
    if (studio >= 0) urls.removeAt(studio);
    active = studio == active ? 0 : active + (studio < 0 || studio > active ? 1 : 0);
    openTab(QLatin1String(ui::webprefs::kStudioUrl), false, true, true);
    for (const QString& url : std::as_const(urls)) openTab(url, /*activate=*/false);
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
        const QString url = tab->pinned ? QLatin1String(ui::webprefs::kStudioUrl) : tabUrl(tab);
        urls.push_back(url.isEmpty() ? QLatin1String(ui::webprefs::kStartUrl) : url);
    }
    ui::webprefs::setSessionTabs(urls);
    ui::webprefs::setSessionActiveTab(m_tabBar ? m_tabBar->currentIndex() : 0);
}

void WebBrowserPanel::recordHistory(const QString& title, const QString& url) {
    ui::webprefs::addHistoryEntry(title, url);
    refreshCompletions();
    if (m_libraryKind == Library::History) refreshLibrary();
}

QWidget* WebBrowserPanel::buildToolbar() {
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("WebToolbar"));
    toolbar->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(toolbar);
    row->setContentsMargins(8, 2, 8, 6);
    row->setSpacing(4);

    const auto button = [toolbar](icons::Glyph glyph, const QString& tip) {
        auto* result = new BrowserIconButton(glyph, tip, toolbar);
        result->setButtonSize(28, 30);
        result->setFocusPolicy(Qt::StrongFocus);
        result->setAccessibleName(tip);
        result->setIdleColor(th().textSecondary);
        result->setActiveColor(th().accentHighlight);
        return result;
    };

    m_back = button(icons::Glyph::ArrowLeft, tr("Back (Alt+Left)"));
    m_forward = button(icons::Glyph::ArrowRight, tr("Forward (Alt+Right)"));
    m_reloadStop = button(icons::Glyph::Reload, tr("Reload page"));
    m_bookmark = button(icons::Glyph::Star, tr("Bookmark this page (Ctrl+D)"));
    m_bookmark->setCheckable(true);
    m_menu = new QPushButton(toolbar);
    QPixmap menuDots(32, 32);
    menuDots.fill(Qt::transparent);
    {
        QPainter painter(&menuDots);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(th().textPrimary);
        for (int y : {8, 16, 24}) painter.drawEllipse(QPointF(16, y), 1.8, 1.8);
    }
    m_menu->setIcon(QIcon(menuDots));
    m_menu->setIconSize(QSize(16, 16));
    m_menu->setObjectName(QStringLiteral("WebMenuButton"));
    m_menu->setFixedSize(28, 30);
    m_menu->setToolTip(tr("Browser menu"));
    m_menu->setAccessibleName(tr("Browser menu"));
    m_menu->setCursor(Qt::PointingHandCursor);

    auto* address = new BrowserAddressEdit(toolbar);
    address->openInNewTab = [this] { openTab(m_address->text()); };
    m_address = address;
    m_address->setMinimumWidth(50);
    m_address->setObjectName(QStringLiteral("WebAddress"));
    m_address->setPlaceholderText(tr("Search or enter address"));
    m_address->setClearButtonEnabled(true);
    m_addressIcon = m_address->addAction(
        icons::icon(icons::Glyph::Globe, th().textSecondary, 14),
        QLineEdit::LeadingPosition);
    m_address->setAccessibleName(tr("Web address"));
    m_address->setProperty("dawWebInput", true);
    m_zoom = new QPushButton(toolbar);
    m_zoom->setObjectName(QStringLiteral("WebZoom"));
    m_zoom->setToolTip(tr("Reset page zoom (Ctrl+0)"));
    m_zoom->setAccessibleName(tr("Reset page zoom"));
    m_zoom->setFixedSize(46, 28);
    m_zoom->hide();
    connect(m_zoom, &QPushButton::clicked, this, [this] { setZoom(1.0); });

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
        if (BrowserSurface* target = view()) target->back();
    });
    connect(m_forward, &QAbstractButton::clicked, this, [this] {
        if (BrowserSurface* target = view()) target->forward();
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
    connect(m_bookmark, &QAbstractButton::clicked, this,
            &WebBrowserPanel::toggleCurrentBookmark);
    connect(m_menu, &QAbstractButton::clicked, this,
            &WebBrowserPanel::showBrowserMenu);
    connect(m_address, &QLineEdit::returnPressed, this,
            [this] { navigate(m_address->text()); });

    row->addWidget(m_back);
    row->addWidget(m_forward);
    row->addWidget(m_reloadStop);
    row->addWidget(m_address, 1);
    row->addWidget(m_zoom);
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

QWidget* WebBrowserPanel::buildSidebar() {
    auto* sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("WebSidebar"));
    sidebar->setAttribute(Qt::WA_StyledBackground, true);
    sidebar->setFixedWidth(44);
    auto* column = new QVBoxLayout(sidebar);
    column->setContentsMargins(5, 10, 5, 10);
    column->setSpacing(6);
    const auto button = [this, sidebar, column](icons::Glyph glyph, const QString& name,
                                         const char* role) {
        auto* result = new QPushButton(sidebar);
        result->setObjectName(QStringLiteral("WebRailButton"));
        result->setProperty("railRole", role);
        result->setProperty("webGlyph", int(glyph));
        result->setFixedSize(34, 34);
        result->setIcon(icons::icon(glyph, th().textSecondary, 18));
        result->setIconSize(QSize(18, 18));
        result->setToolTip(name);
        result->setAccessibleName(name);
        result->setCursor(Qt::PointingHandCursor);
        result->setCheckable(true);
        m_railButtons.push_back(result);
        column->addWidget(result);
        return result;
    };
    auto* home = button(icons::Glyph::Home, tr("Home (Alt+Home)"), "home");
    auto* bookmarks = button(icons::Glyph::Star, tr("Bookmarks"), "bookmarks");
    auto* history = button(icons::Glyph::Clock, tr("Recent pages"), "history");
    auto* downloads = button(icons::Glyph::Download, tr("Downloads (Ctrl+J)"), "downloads");
    column->addStretch(1);
    auto* settings = button(icons::Glyph::Gear, tr("Browser settings…"), "settings");
    connect(home, &QPushButton::clicked, this, [this] { navigate(ui::webprefs::homeUrl()); });
    connect(bookmarks, &QPushButton::clicked, this,
            [this] { showLibrary(Library::Bookmarks); });
    connect(history, &QPushButton::clicked, this,
            [this] { showLibrary(Library::History); });
    connect(downloads, &QPushButton::clicked, this, [this] { showLibrary(Library::Downloads); });
    settings->setCheckable(false);
    connect(settings, &QPushButton::clicked, this, &WebBrowserPanel::settingsRequested);
    return sidebar;
}



QWidget* WebBrowserPanel::buildLibrary() {
    m_library = new QWidget(m_viewFrame);
    m_library->setObjectName(QStringLiteral("WebLibrary"));
    m_library->setAttribute(Qt::WA_StyledBackground, true);
    m_library->setMinimumWidth(0);
    auto* column = new QVBoxLayout(m_library);
    column->setContentsMargins(12, 16, 12, 12);
    column->setSpacing(10);
    auto* header = new QHBoxLayout;
    m_libraryTitle = new QLabel(m_library);
    m_libraryTitle->setObjectName(QStringLiteral("WebLibraryTitle"));
    m_libraryTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(m_libraryTitle, 1);
    auto* close = new BrowserIconButton(icons::Glyph::Close, tr("Back to page (Esc)"), m_library);
    close->setObjectName(QStringLiteral("WebLibraryClose"));
    close->setButtonSize(28, 28);
    close->setAccessibleName(tr("Back to page"));
    close->setFocusPolicy(Qt::StrongFocus);
    connect(close, &QAbstractButton::clicked, this, [this] {
        closeLibrary();
        if (view()) view()->setFocus();
    });
    header->addWidget(close);
    column->addLayout(header);
    m_librarySearch = new BrowserAddressEdit(m_library);
    m_librarySearch->setObjectName(QStringLiteral("WebLibrarySearch"));
    m_librarySearch->setAccessibleName(tr("Filter browser library"));
    m_librarySearch->setPlaceholderText(tr("Search by name or address"));
    m_librarySearch->setClearButtonEnabled(true);
    m_librarySearch->setProperty("dawWebInput", true);
    m_librarySearch->setMinimumWidth(0);
    column->addWidget(m_librarySearch);
    m_libraryAction = new QPushButton(m_library);
    m_libraryAction->setObjectName(QStringLiteral("WebLibraryAction"));
    column->addWidget(m_libraryAction, 0, Qt::AlignLeft);
    m_libraryEmpty = new QLabel(m_library);
    m_libraryEmpty->setObjectName(QStringLiteral("WebLibraryEmpty"));
    m_libraryEmpty->setWordWrap(true);
    m_libraryEmpty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    column->addWidget(m_libraryEmpty);
    m_libraryList = new QListWidget(m_library);
    m_libraryList->setObjectName(QStringLiteral("WebLibraryList"));
    m_libraryList->setItemDelegate(new BrowserLibraryDelegate(m_libraryList));
    m_libraryList->setMouseTracking(true);
    m_libraryList->setFrameShape(QFrame::NoFrame);
    m_libraryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_libraryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_libraryList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_libraryList->setProperty("dawWebInput", true);
    column->addWidget(m_libraryList, 1);
    connect(m_librarySearch, &QLineEdit::textChanged, this, &WebBrowserPanel::refreshLibrary);
    connect(m_librarySearch, &QLineEdit::returnPressed, this, &WebBrowserPanel::activateLibraryItem);
    connect(m_libraryList, &QListWidget::itemActivated, this, [this] { activateLibraryItem(); });
    connect(m_libraryList, &QListWidget::itemClicked, this, [this] { activateLibraryItem(); });
    connect(m_libraryList, &QWidget::customContextMenuRequested, this, &WebBrowserPanel::showLibraryContextMenu);
    connect(m_libraryAction, &QPushButton::clicked, this, [this] {
        if (m_libraryKind == Library::Bookmarks) addBookmark();
        else if (m_libraryKind == Library::Tabs) reopenClosedTab();
        else if (m_libraryKind == Library::Downloads) openDownloadsFolder();
        else if (m_libraryKind == Library::History && QMessageBox::question(this, tr("Clear history"),
                     tr("Clear the list of visited pages? Your bookmarks and downloads will be kept."),
                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes) {
            ui::webprefs::clearHistory();
            refreshCompletions();
            rebuildBookmarksBar();
        }
        refreshLibrary();
    });
    m_downloadRefreshTimer = new QTimer(this);
    m_downloadRefreshTimer->setSingleShot(true);
    m_downloadRefreshTimer->setInterval(150);
    connect(m_downloadRefreshTimer, &QTimer::timeout, this, [this] {
        if (m_libraryKind == Library::Downloads) refreshLibrary();
    });
    m_library->hide();
    return m_library;
}

void WebBrowserPanel::showLibrary(Library kind) {
    if (m_libraryKind == kind) { closeLibrary(); return; }
    if (m_findBar->isVisible()) closeFindBar();
    m_libraryKind = kind;
    m_stack->hide();
    m_library->show();
    m_tabSearch->setChecked(kind == Library::Tabs);
    const QString role = kind == Library::Bookmarks ? QStringLiteral("bookmarks")
        : kind == Library::History ? QStringLiteral("history") : kind == Library::Downloads ? QStringLiteral("downloads") : QString();
    for (auto* button : m_railButtons) button->setChecked(button->property("railRole").toString() == role);
    m_librarySearch->clear();
    refreshLibrary();
    m_librarySearch->setFocus(Qt::ShortcutFocusReason);
}

void WebBrowserPanel::closeLibrary() {
    if (!m_library) return;
    m_libraryKind = Library::None;
    m_library->hide();
    m_stack->show();
    m_tabSearch->setChecked(false);
    for (auto* button : m_railButtons)
        button->setChecked(button->property("railRole").toString() == QLatin1String("home") &&
                           currentTab() && currentTab()->showingStartPage);
}

void WebBrowserPanel::refreshLibrary() {
    if (!m_library || m_libraryKind == Library::None) return;
    const QVariant selected = m_libraryList->currentItem() ? m_libraryList->currentItem()->data(Qt::UserRole) : QVariant();
    const int scroll = m_libraryList->verticalScrollBar()->value();
    const QString query = m_librarySearch->text().trimmed();
    m_libraryList->clear();
    const auto add = [&](const QString& title, const QString& detail, const QVariant& key, const QIcon& icon) {
        if (!query.isEmpty() && !title.contains(query, Qt::CaseInsensitive) && !detail.contains(query, Qt::CaseInsensitive)) return;
        auto* item = new QListWidgetItem(icon, title, m_libraryList);
        item->setData(Qt::UserRole, key);
        item->setData(Qt::UserRole + 1, detail);
        item->setData(Qt::AccessibleTextRole, title + QStringLiteral(". ") + detail);
        item->setToolTip(title + QLatin1Char('\n') + detail);
        if (key == selected) m_libraryList->setCurrentItem(item);
    };
    QString title, empty;
    m_libraryAction->setEnabled(true);
    if (m_libraryKind == Library::Tabs) {
        title = tr("Open tabs"); empty = tr("No matching tabs");
        m_libraryAction->setText(tr("Reopen closed tab"));
        m_libraryAction->setEnabled(!m_closedTabs.isEmpty());
        for (int i = 0; i < m_tabs.size(); ++i) {
            const Tab* tab = m_tabs.at(i);
            add(m_tabBar->tabText(i) + (tab->muted ? tr(" · Muted") : QString()),
                tab->showingStartPage ? tr("Start page") : tab->view->url().toDisplayString(),
                QVariant::fromValue(static_cast<QObject*>(tab->view)), m_tabBar->tabIcon(i));
        }
    } else if (m_libraryKind == Library::Bookmarks) {
        title = tr("Bookmarks"); empty = tr("Save a page with Ctrl+D, or add a bookmark here.");
        m_libraryAction->setText(tr("Add bookmark"));
        for (const auto& entry : ui::webprefs::bookmarks())
            add(bookmarkLabel(entry), entry.url, entry.url, icons::icon(icons::Glyph::Star, th().accentHighlight, 18));
    } else if (m_libraryKind == Library::History) {
        title = tr("History"); empty = tr("Pages you visit will appear here.");
        m_libraryAction->setText(tr("Clear history"));
        const auto entries = ui::webprefs::history();
        m_libraryAction->setEnabled(!entries.isEmpty());
        for (const auto& entry : entries)
            add(entry.title, entry.url, entry.url, icons::icon(icons::Glyph::Clock, th().textSecondary, 18));
    } else {
        title = tr("Downloads"); empty = tr("Downloads from this session appear here. Audio files are offered for import into your project.");
        m_libraryAction->setText(tr("Open downloads folder"));
        for (const auto& entry : std::as_const(m_downloadLog)) {
            QString detail = entry.detail;
            if (const auto* request = entry.request.data()) {
                detail = request->isPaused() ? tr("Paused") : tr("Downloading");
                detail += QStringLiteral(" · ") + QLocale().formattedDataSize(request->receivedBytes());
                if (request->totalBytes() > 0) detail += QStringLiteral(" / ") + QLocale().formattedDataSize(request->totalBytes());
            }
            add(QFileInfo(entry.path).fileName(), detail, entry.path,
                icons::icon(entry.completed ? icons::Glyph::Check : icons::Glyph::Download, th().accentHighlight, 18));
        }
    }
    m_libraryTitle->setText(title + QStringLiteral(" · %1").arg(m_libraryList->count()));
    m_libraryList->setAccessibleName(title);
    m_libraryEmpty->setText(query.isEmpty() ? empty : tr("No results. Try another name or address."));
    m_libraryEmpty->setVisible(m_libraryList->count() == 0);
    if (!m_libraryList->currentItem() && m_libraryList->count()) m_libraryList->setCurrentRow(0);
    m_libraryList->verticalScrollBar()->setValue(scroll);
}

void WebBrowserPanel::activateLibraryItem() {
    const auto* item = m_libraryList->currentItem();
    if (!item) return;
    const QVariant key = item->data(Qt::UserRole);
    if (m_libraryKind == Library::Tabs) {
        for (int i = 0; i < m_tabs.size(); ++i) if (m_tabs.at(i)->view == key.value<QObject*>()) {
            m_tabBar->setCurrentIndex(i);
            closeLibrary();
            view()->setFocus();
            return;
        }
    } else if (m_libraryKind == Library::Downloads) {
        showLibraryContextMenu(m_libraryList->visualItemRect(item).center());
    } else {
        if (QApplication::keyboardModifiers().testFlag(Qt::ControlModifier)) openTab(key.toString());
        else navigate(key.toString());
    }
}

void WebBrowserPanel::showLibraryContextMenu(const QPoint& position) {
    auto* item = m_libraryList->itemAt(position);
    if (!item) item = m_libraryList->currentItem();
    if (!item) return;
    const QVariant key = item->data(Qt::UserRole);
    const QPoint global = m_libraryList->viewport()->mapToGlobal(position);
    if (m_libraryKind == Library::Tabs) {
        for (int i = 0; i < m_tabs.size(); ++i) if (m_tabs.at(i)->view == key.value<QObject*>()) {
            showTabContextMenu(i, global); refreshLibrary(); return;
        }
    } else if (m_libraryKind == Library::Bookmarks) {
        const auto bookmarks = ui::webprefs::bookmarks();
        for (int i = 0; i < bookmarks.size(); ++i) if (bookmarks[i].url == key.toString()) {
            showBookmarkContextMenu(i, global); refreshLibrary(); return;
        }
    } else if (m_libraryKind == Library::History) {
        QMenu menu(this);
        connect(menu.addAction(tr("Open in new tab")), &QAction::triggered, this, [this, key] { openTab(key.toString()); });
        connect(menu.addAction(tr("Copy page address")), &QAction::triggered, this, [key] { QApplication::clipboard()->setText(key.toString()); });
        menu.exec(global);
    } else if (m_libraryKind == Library::Downloads) {
        for (const auto& entry : std::as_const(m_downloadLog)) if (entry.path == key.toString()) {
            QMenu menu(this);
            const QPointer<QWebEngineDownloadRequest> request = entry.request;
            if (request) {
                auto* pause = menu.addAction(request->isPaused() ? tr("Resume download") : tr("Pause download"));
                pause->setObjectName(QStringLiteral("WebPauseDownload"));
                connect(pause, &QAction::triggered, this, [request] {
                    if (request) { if (request->isPaused()) request->resume(); else request->pause(); }
                });
                auto* cancel = menu.addAction(tr("Cancel download"));
                cancel->setObjectName(QStringLiteral("WebCancelDownload"));
                connect(cancel, &QAction::triggered, this, [request] { if (request) request->cancel(); });
            }
            if (entry.completed && QFileInfo::exists(entry.path) && ui::isAudioFile(entry.path))
                connect(menu.addAction(tr("Import audio into project")), &QAction::triggered, this,
                        [this, path = entry.path] { probeCompletedDownload(path, {}); });
            connect(menu.addAction(tr("Open downloads folder")), &QAction::triggered, this, &WebBrowserPanel::openDownloadsFolder);
            connect(menu.addAction(tr("Copy file path")), &QAction::triggered, this,
                    [path = entry.path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
            menu.exec(global);
            refreshLibrary();
            return;
        }
    }
}

void WebBrowserPanel::setTabMuted(Tab* tab, bool muted) {
    if (indexOfTab(tab) < 0) return;
    tab->muted = muted;
    tab->view->page()->setAudioMuted(muted);
    updateTabLabel(tab);
}

void WebBrowserPanel::setZoom(qreal zoom) {
    if (!view()) return;
    const qreal value = std::clamp(zoom, 0.25, 5.0);
    view()->setZoomFactor(value);
    m_zoom->setText(QStringLiteral("%1%").arg(qRound(value * 100)));
    m_zoom->setVisible(!qFuzzyCompare(value, 1.0));
}

void WebBrowserPanel::openDownloadsFolder() {
    const QString directory = ui::webprefs::downloadDirectory();
    if (!QDir().mkpath(directory) || !QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
        emit statusMessage(tr("Could not open the downloads folder"));
}

void WebBrowserPanel::addBookmark() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add bookmark"));
    dialog.setMinimumWidth(340);
    auto* form = new QFormLayout(&dialog);
    auto* title = new BrowserAddressEdit(&dialog);
    title->setObjectName(QStringLiteral("WebShortcutTitle"));
    auto* address = new BrowserAddressEdit(&dialog);
    address->setObjectName(QStringLiteral("WebShortcutAddress"));
    address->setPlaceholderText(QStringLiteral("https://"));
    title->setProperty("dawWebInput", true);
    address->setProperty("dawWebInput", true);
    form->addRow(tr("Name"), title);
    form->addRow(tr("Web address"), address);
    auto* error = new QLabel(&dialog);
    error->setWordWrap(true);
    error->hide();
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto save = [this, &dialog, title, address, error] {
        const QString input = address->text().trimmed();
        const QUrl url = QUrl::fromUserInput(input);
        if (input.isEmpty() || input.contains(QRegularExpression(QStringLiteral("\\s"))) ||
            !url.isValid() || url.host().isEmpty() ||
            (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
            error->setText(tr("Enter a valid http or https address."));
            error->show();
            address->setFocus();
            return;
        }
        if (ui::webprefs::bookmarks().size() >= 100 ||
            !ui::webprefs::addBookmark(title->text(), url.toString(QUrl::FullyEncoded))) {
            error->setText(tr("This shortcut is already saved or the bookmark list is full."));
            error->show();
            return;
        }
        refreshCompletions();
        rebuildBookmarksBar();
        updateBookmarkState();
        dialog.accept();
    };
    connect(buttons, &QDialogButtonBox::accepted, &dialog, save);
    connect(address, &QLineEdit::returnPressed, &dialog, save);
    connect(title, &QLineEdit::returnPressed, address, QOverload<>::of(&QWidget::setFocus));
    title->setFocus();
    dialog.exec();
}

void WebBrowserPanel::openHistoryMenu(QWidget* anchor) {
    QMenu menu(this);
    populateHistoryMenu(&menu);
    menu.exec(anchor->mapToGlobal(QPoint(anchor->width(), 0)));
}

void WebBrowserPanel::populateHistoryMenu(QMenu* menu) {
    const auto visited = ui::webprefs::history();
    if (visited.isEmpty()) menu->addAction(tr("No recent pages"))->setEnabled(false);
    for (int i = 0; i < std::min(15, int(visited.size())); ++i) {
        const auto entry = visited.at(i);
        auto* action = menu->addAction(QFontMetrics(menu->font()).elidedText(entry.title, Qt::ElideRight, 260));
        action->setToolTip(entry.url);
        connect(action, &QAction::triggered, this, [this, entry] { navigate(entry.url); });
    }
    if (!visited.isEmpty()) {
        menu->addSeparator();
        connect(menu->addAction(tr("Clear history")), &QAction::triggered, this, [this] {
            ui::webprefs::clearHistory();
            refreshCompletions();
            rebuildBookmarksBar();
            emit statusMessage(tr("Browsing history cleared"));
        });
    }
}

QWidget* WebBrowserPanel::buildFindBar() {
    m_findBar = new QWidget(this);
    m_findBar->setObjectName(QStringLiteral("WebFindBar"));
    m_findBar->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(m_findBar);
    row->setContentsMargins(8, 4, 6, 4);
    row->setSpacing(4);

    auto* label = new QLabel(m_findBar);
    label->setPixmap(icons::icon(icons::Glyph::Search, th().textSecondary, 16).pixmap(16, 16));
    label->setObjectName(QStringLiteral("WebFindLabel"));
    m_findText = new BrowserAddressEdit(m_findBar);
    m_findText->setMinimumWidth(0);
    m_findText->setObjectName(QStringLiteral("WebFindText"));
    m_findText->setPlaceholderText(tr("Find in page"));
    m_findText->setClearButtonEnabled(true);
    m_findText->setProperty("dawWebInput", true);
    m_findText->setAccessibleName(tr("Find in web page"));
    m_findResult = new QLabel(m_findBar);
    m_findResult->setObjectName(QStringLiteral("WebFindResult"));
    m_findResult->setMinimumWidth(48);
    m_findResult->setAlignment(Qt::AlignCenter);

    const auto button = [this](icons::Glyph glyph, const QString& tip) {
        auto* result = new BrowserIconButton(glyph, tip, m_findBar);
        result->setButtonSize(27, 25);
        result->setFocusPolicy(Qt::StrongFocus);
        result->setAccessibleName(tip);
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
    row->addWidget(m_findResult);
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
    m_downloadName->setMinimumWidth(0);
    m_downloadName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_downloadName->setObjectName(QStringLiteral("WebDownloadName"));
    m_downloadProgress = new QProgressBar(m_downloadBar);
    m_downloadProgress->setObjectName(QStringLiteral("WebDownloadProgress"));
    m_downloadProgress->setTextVisible(false);
    m_downloadProgress->setAccessibleName(tr("Download progress"));
    m_downloadProgress->setFixedWidth(92);
    m_downloadProgress->setFixedHeight(6);
    m_cancelDownload = new BrowserIconButton(icons::Glyph::Close,
                                           tr("Cancel download"), m_downloadBar);
    m_cancelDownload->setButtonSize(26, 24);
    m_cancelDownload->setFocusPolicy(Qt::StrongFocus);
    connect(m_cancelDownload, &QAbstractButton::clicked, this, [this] {
        if (m_visibleDownload) m_visibleDownload->cancel();
        else m_downloadBar->hide();
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
    closeLibrary();
    if (tab->pinned && url != QUrl(QLatin1String(ui::webprefs::kStudioUrl))) {
        openTab(url.toString());
        return;
    }
    tab->showingStartPage = false;
    tab->showingErrorPage = false;
    tab->failedUrl.clear();
    tab->pendingUrl = url.toString();
    m_address->setText(url.toDisplayString());
    tab->view->setUrl(url);
    tab->view->setFocus(Qt::OtherFocusReason);
}

QString WebBrowserPanel::startPageHtml() const {
    QString shortcuts;
    const auto tile = [](const QString& title, const QString& url,
                         const QString& mark, const QString& color) {
        return QStringLiteral("<a class='shortcut' href='%1' title='%2'><span class='mark' "
                              "style='--mark:%3'>%4</span><span class='name'>%5</span></a>")
            .arg(QUrl(url).toString(QUrl::FullyEncoded).toHtmlEscaped(),
                 title.toHtmlEscaped(), color, mark, title.toHtmlEscaped());
    };
    struct Shortcut { const char* title; const char* url; icons::Glyph glyph; const char* color; };
    const Shortcut pinned[] = {
        {"YouTube", "https://www.youtube.com/", icons::Glyph::Play, "#d64549"},
        {"SoundCloud", "https://soundcloud.com/", icons::Glyph::Cloud, "#d86c35"},
        {"Splice", "https://splice.com/", icons::Glyph::Layers, "#5d63c5"},
        {"Spotify", "https://open.spotify.com/", icons::Glyph::Headphones, "#16883e"}
    };
    for (const auto& shortcut : pinned) {
        const QString mark = QLatin1String(shortcut.title) == QLatin1String("Spotify")
            ? QStringLiteral("<svg viewBox='0 0 24 24' aria-hidden='true' fill='none' "
                "stroke='currentColor' stroke-width='1.8' stroke-linecap='round'>"
                "<path d='M4 8c5-2 11-1.7 16 1M5 12c4-1.5 9-1.2 14 1M6 16c4-1.2 7-1 11 1'/></svg>")
            : startPageIcon(shortcut.glyph);
        shortcuts += tile(QLatin1String(shortcut.title), QLatin1String(shortcut.url),
                          mark, QLatin1String(shortcut.color));
    }
    // Reuse the application's bundled brand asset; keep the document small
    // enough for WebEngine's setHtml limit, including on high-DPI screens.
    const QImage logo(QStringLiteral(":/vlt/icon-1024.png"));
    QByteArray logoPng;
    QBuffer logoBuffer(&logoPng);
    logoBuffer.open(QIODevice::WriteOnly);
    logo.scaled(112, 112, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&logoBuffer, "PNG");
    const QString backgroundPath = ui::webprefs::startPageBackgroundPath();
    const QFileInfo background(backgroundPath);
    const bool video = isVideoBackground(backgroundPath);
    const bool animated = video || background.suffix().compare(
                                      QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
    QString media;
    const bool reduceMotion =
        QSettings().value(QStringLiteral("ui/reduceMotion"), false).toBool();
    if (!backgroundPath.isEmpty() && !(animated && reduceMotion)) {
        const QString source =
            QUrl::fromLocalFile(backgroundPath)
                .toString(QUrl::FullyEncoded)
                .toHtmlEscaped();
        media = video
                    ? QStringLiteral(
                          "<video class='backdrop-media motion' aria-hidden='true' "
                          "autoplay muted loop playsinline preload='metadata' "
                          "src='%1'></video>")
                          .arg(source)
                    : QStringLiteral(
                          "<img class='backdrop-media%1' aria-hidden='true' "
                          "alt='' draggable='false' src='%2'>")
                          .arg(animated ? QStringLiteral(" motion") : QString(),
                               source);
        media += QStringLiteral(
            "<div class='backdrop-scrim' aria-hidden='true'></div>");
    }

    QString html = QString::fromUtf8(kWebStartPageHtml);
    const auto replace = [&html](const char* key, const QString& text) {
        html.replace(QLatin1String(key), text.toHtmlEscaped());
    };
    replace("%LANG%", QLocale().name().section(QLatin1Char('_'), 0, 0));
    replace("%EDITION%", tr("Browser"));
    replace("%SPACE%", tr("YOUR WORKSPACE"));
    replace("%HEADING%", tr("A space for your next idea."));
    replace("%SUBTITLE%", tr("Find sounds, explore music and keep your references close."));
    replace("%SEARCH_LABEL%", tr("Web search"));
    replace("%SEARCH_PLACEHOLDER%", tr("Search the web"));
    replace("%SEARCH_BUTTON%", tr("Search"));
    replace("%SHORTCUTS_LABEL%", tr("Quick access"));
    replace("%MUSIC_LABEL%", tr("Music & inspiration"));
    replace("%IMPORT_TITLE%", tr("From the web to your track"));
    replace("%FOOTER%", tr("Download audio to bring it into your project"));
    replace("%KEY_ADDRESS%", tr("Address"));
    replace("%KEY_TAB%", tr("New tab"));
    replace("%KEY_DOWNLOADS%", tr("Downloads"));
    const auto& theme = th();
    html.replace(QStringLiteral("%PALETTE%"), QStringLiteral(
        "color-scheme:%1;--bg:%2;--surface:%3;--hover:%4;--line:%5;--text:%6;--muted:%7;--accent:%8;--accent-ink:%9;")
        .arg(theme.dark ? QStringLiteral("dark") : QStringLiteral("light"), theme.background.name(),
             theme.surface.name(), theme.surfaceElevated.name(), theme.separator().name(),
             theme.textPrimary.name(), theme.textSecondary.name(), theme.accent.name(), theme.accentText().name())
        + QStringLiteral("--well:%1;--top:%2;--edge:%3;--scrim:rgba(%4,%5,%6,.92);")
            .arg(theme.well().name(), theme.panelTop().name(), theme.edgeLight(theme.surface).name())
            .arg(theme.background.red()).arg(theme.background.green()).arg(theme.background.blue()));
    const auto savedLink = [](const QString& title, const QString& url) {
        const QString host = QUrl(url).host();
        return QStringLiteral("<a class='saved-link' href='%1' title='%2'><span aria-hidden='true'>%3</span>"
                              "<span class='saved-copy'><strong>%4</strong><small>%5</small></span></a>")
            .arg(QUrl(url).toString(QUrl::FullyEncoded).toHtmlEscaped(), url.toHtmlEscaped(),
                 host.left(1).toUpper().toHtmlEscaped(), title.toHtmlEscaped(), host.toHtmlEscaped());
    };
    const auto section = [](const QString& title, const QString& content) {
        return content.isEmpty() ? QString() : QStringLiteral("<section><div class='section-head'><h2>%1</h2></div><div class='saved-grid'>%2</div></section>")
            .arg(title.toHtmlEscaped(), content);
    };
    QString saved, recent;
    const auto bookmarks = ui::webprefs::bookmarks();
    for (int i = 0; i < std::min(6, int(bookmarks.size())); ++i)
        saved += savedLink(bookmarkLabel(bookmarks[i]), bookmarks[i].url);
    const auto visited = ui::webprefs::history();
    for (int i = 0; i < std::min(4, int(visited.size())); ++i)
        recent += savedLink(visited[i].title, visited[i].url);
    html.replace(QStringLiteral("%SAVED%"), section(tr("Your bookmarks"), saved));
    html.replace(QStringLiteral("%RECENT%"), section(tr("Recently visited"), recent));
    html.replace(QStringLiteral("%FONTS%"), ui::bundledFontFaceCss());
    html.replace(QStringLiteral("%LOGO%"), QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(logoPng.toBase64())));
    html.replace(QStringLiteral("%SHORTCUTS%"), shortcuts);
    html.replace(QStringLiteral("%MEDIA%"), media);
    return html;
}

QUrl WebBrowserPanel::startPageBaseUrl() const {
    const QString path = ui::webprefs::startPageBackgroundPath();
    if (path.isEmpty()) return QUrl(QStringLiteral("about:blank"));
    return QUrl::fromLocalFile(path);
}

void WebBrowserPanel::showStartPage() {
    Tab* tab = currentTab();
    if (!tab) return;
    closeLibrary();
    if (tab->pinned) { openTab(QLatin1String(ui::webprefs::kStartUrl)); return; }
    tab->showingStartPage = true;
    tab->showingErrorPage = false;
    tab->failedUrl.clear();
    tab->pendingUrl.clear();
    ui::webprefs::setLastUrl(QLatin1String(ui::webprefs::kStartUrl));
    updateAddress();
    updateBookmarkState();
    updateTabLabel(tab);
    tab->view->setHtml(startPageHtml(), startPageBaseUrl());
}

void WebBrowserPanel::updateNavigationState() {
    BrowserSurface* target = view();
    if (!target) return;
    m_back->setEnabled((target->history() && target->history()->canGoBack()));
    m_forward->setEnabled((target->history() && target->history()->canGoForward()));
}

QString WebBrowserPanel::currentPageUrl() const {
    return tabUrl(currentTab());
}

QString WebBrowserPanel::tabUrl(const Tab* tab) const {
    if (!tab) return {};
    if (tab->showingStartPage) return QLatin1String(ui::webprefs::kStartUrl);
    if (tab->showingErrorPage) return tab->failedUrl;
    if (!tab->pendingUrl.isEmpty()) return tab->pendingUrl;
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
    const bool secure = !tab->showingStartPage && !tab->showingErrorPage &&
                        tab->view->url().scheme() == QLatin1String("https");
    m_addressIcon->setIcon(icons::icon(tab->showingStartPage ? icons::Glyph::Search
        : secure ? icons::Glyph::Key : icons::Glyph::Globe, th().textSecondary, 14));
    m_addressIcon->setToolTip(tab->showingStartPage ? tr("Search or enter address")
        : tab->showingErrorPage ? tr("Page could not be loaded")
        : secure ? tr("Connection uses HTTPS") : tr("Connection is not encrypted"));
    if (tab->showingStartPage) {
        m_address->clear();
        return;
    }
    const QString value = QUrl(tabUrl(tab)).toDisplayString();
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
    refreshCompletions();
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
        hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        hint->setMinimumWidth(0);
        hint->setToolTip(hint->text());
        m_bookmarksLayout->addWidget(hint);
    }
    m_bookmarksLayout->addStretch(1);
    if (m_libraryKind == Library::Bookmarks || m_libraryKind == Library::History) refreshLibrary();

    if (refreshStartPage) {
        // The start page mirrors the same persistent list — in every tab that
        // is showing one, not only the visible tab.
        for (Tab* tab : std::as_const(m_tabs)) {
            if (tab->showingStartPage) {
                tab->view->setHtml(startPageHtml(),
                                   startPageBaseUrl());
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
    auto* add = menu.addAction(tr("Add bookmark"));
    add->setObjectName(QStringLiteral("WebAddBookmark"));
    connect(add, &QAction::triggered, this, &WebBrowserPanel::addBookmark);
    QAction* start = menu.addAction(tr("Open start page"));
    connect(start, &QAction::triggered, this,
            &WebBrowserPanel::showStartPage);
    menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void WebBrowserPanel::showBrowserMenu() {
    QMenu menu(this);
    auto* tabs = menu.addAction(tr("Search tabs"));
    tabs->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    connect(tabs, &QAction::triggered, this, [this] { showLibrary(Library::Tabs); });
    auto* library = menu.addAction(tr("Bookmarks"));
    connect(library, &QAction::triggered, this, [this] { showLibrary(Library::Bookmarks); });
    auto* history = menu.addAction(tr("History"));
    history->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(history, &QAction::triggered, this, [this] { showLibrary(Library::History); });
    auto* downloads = menu.addAction(tr("Downloads"));
    downloads->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    connect(downloads, &QAction::triggered, this, [this] { showLibrary(Library::Downloads); });
    menu.addSeparator();
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
    closeTabAction->setEnabled(currentTab() && !currentTab()->pinned);
    closeTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closeTabAction, &QAction::triggered, this,
            [this] { closeTab(m_tabBar->currentIndex()); });
    QAction* reopen = menu.addAction(tr("Reopen closed tab"));
    reopen->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    reopen->setEnabled(!m_closedTabs.isEmpty());
    connect(reopen, &QAction::triggered, this, &WebBrowserPanel::reopenClosedTab);

    QMenu* recent = menu.addMenu(tr("Recent pages"));
    populateHistoryMenu(recent);
    menu.addSeparator();

    QAction* zoomIn = menu.addAction(tr("Zoom in"));
    QAction* zoomOut = menu.addAction(tr("Zoom out"));
    QAction* actualSize = menu.addAction(tr("Actual size"));
    connect(zoomIn, &QAction::triggered, this, [this] {
        if (view()) setZoom(view()->zoomFactor() + 0.1);
    });
    connect(zoomOut, &QAction::triggered, this, [this] {
        if (view()) setZoom(view()->zoomFactor() - 0.1);
    });
    connect(actualSize, &QAction::triggered, this,
            [this] {
                setZoom(1.0);
            });

    QAction* copyAddress = menu.addAction(tr("Copy page address"));
    copyAddress->setEnabled(canBookmark ||
                            (currentTab() && currentTab()->showingErrorPage));
    connect(copyAddress, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(currentPageUrl());
    });
    menu.addSeparator();
    if (contentDownloadCompleted) {
        auto* package = menu.addAction(tr("Package saved project for forum…"));
        connect(package, &QAction::triggered, this, &WebBrowserPanel::exportForumPackageRequested);
    }
    QAction* settings = menu.addAction(tr("Browser settings…"));
    connect(settings, &QAction::triggered, this,
            &WebBrowserPanel::settingsRequested);
    menu.exec(m_menu->mapToGlobal(QPoint(0, m_menu->height())));
}

void WebBrowserPanel::showFindBar() {
    closeLibrary();
    m_findBar->show();
    m_findText->setFocus(Qt::ShortcutFocusReason);
    m_findText->selectAll();
    findText();
}

void WebBrowserPanel::closeFindBar() {
    m_findPending = false;
    if (BrowserSurface* target = view()) {
        target->page()->findText(QString());
        target->setFocus(Qt::ShortcutFocusReason);
    }
    m_findBar->hide();
}

void WebBrowserPanel::findText(bool backward) {
    BrowserSurface* target = view();
    if (!target) return;
    const QString needle = m_findText->text();
    if (needle.isEmpty()) {
        m_findPending = false;
        m_findResult->clear();
        target->page()->findText(QString());
        return;
    }
    if (currentTab() && currentTab()->loading) {
        m_findPending = true;
        m_findResult->setText(tr("Searching…"));
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
    add(QKeySequence(Qt::Key_F6), [this] { m_address->setFocus(); m_address->selectAll(); });
    add(QKeySequence(Qt::Key_F5), [this] { if (m_reloadStop) m_reloadStop->click(); });
    add(QKeySequence(Qt::Key_F3), [this] { if (!m_findBar->isVisible()) showFindBar(); else findText(); });
    add(QKeySequence(QStringLiteral("Shift+F3")), [this] { if (!m_findBar->isVisible()) showFindBar(); else findText(true); });
    add(QKeySequence(QStringLiteral("Ctrl+J")), [this] { showLibrary(Library::Downloads); });
    add(QKeySequence(QStringLiteral("Ctrl+H")), [this] { showLibrary(Library::History); });
    add(QKeySequence(QStringLiteral("Ctrl+Shift+A")), [this] { showLibrary(Library::Tabs); });
    add(QKeySequence(QStringLiteral("Ctrl+Shift+B")), [this] {
        const bool show = !ui::webprefs::bookmarksBarVisible();
        ui::webprefs::setBookmarksBarVisible(show);
        m_bookmarksBar->setVisible(show);
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
        if (BrowserSurface* target = view()) target->back();
    });
    add(QKeySequence(QStringLiteral("Alt+Right")), [this] {
        if (BrowserSurface* target = view()) target->forward();
    });
    add(QKeySequence(QStringLiteral("Alt+Home")),
        [this] { navigate(ui::webprefs::homeUrl()); });
    add(QKeySequence(QStringLiteral("Ctrl++")), [this] {
        if (view()) setZoom(view()->zoomFactor() + 0.1);
    });
    add(QKeySequence(QStringLiteral("Ctrl+=")), [this] { if (view()) setZoom(view()->zoomFactor() + 0.1); });
    add(QKeySequence(QStringLiteral("Ctrl+-")), [this] {
        if (view()) setZoom(view()->zoomFactor() - 0.1);
    });
    add(QKeySequence(QStringLiteral("Ctrl+0")),
        [this] {
            setZoom(1.0);
        });
    add(QKeySequence(Qt::Key_Escape), [this] {
        if (m_libraryKind != Library::None) { closeLibrary(); if (view()) view()->setFocus(); }
        else if (m_findBar->isVisible()) closeFindBar();
        else if (m_address->hasFocus()) {
            updateAddress();
            if (view()) view()->setFocus();
        }
        else if (Tab* tab = currentTab(); tab && tab->loading) {
            tab->userStoppedLoading = true;
            tab->view->stop();
        }
    });
}

bool WebBrowserPanel::ownsFocus() const {
    for (const auto* tab : m_tabs) if (tab->view->ownsQuickFocus()) return true;
    QWidget* focus = QApplication::focusWidget();
    // Deliberately not falling back to `window()->focusWidget()` when there is
    // no application focus widget. It looks like it would make Copy reliable
    // while activation is in flux, and it does — but the main window also
    // remembers a focus widget inside this panel while a *different* window of
    // the program (a piano roll, a plugin editor) is the one being typed into,
    // and then this panel swallows that window's edit keys.
    if (!focus) return false;
    for (QWidget* current = focus; current; current = current->parentWidget()) {
        if (current == this)
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
    if (BrowserSurface* target = view()) target->page()->triggerAction(action);
    return true;
}

bool WebBrowserPanel::handleUndoRedo(bool redo) {
    if (!ownsFocus()) return false;
    if (auto* line = qobject_cast<QLineEdit*>(QApplication::focusWidget())) {
        if (redo) line->redo();
        else line->undo();
        return true;
    }
    if (BrowserSurface* target = view())
        target->page()->triggerAction(redo ? QWebEnginePage::Redo
                                           : QWebEnginePage::Undo);
    return true;
}

void WebBrowserPanel::acceptDownload(QWebEngineDownloadRequest* request) {
    if (!request) return;
    QString directory = contentDownloadDirectory ? contentDownloadDirectory(request) : QString();
    if (directory.isEmpty()) directory = ui::webprefs::downloadDirectory();
    if (!QDir().mkpath(directory)) {
        request->cancel();
        emit statusMessage(tr("Could not create the Downloads folder"));
        return;
    }

    const QString name = uniqueDownloadName(directory, initialDownloadName(request));
    request->setDownloadDirectory(directory);
    request->setDownloadFileName(name);
    m_downloads.push_back(request);
    m_downloadLog.prepend({QDir(directory).filePath(name), tr("Downloading"), request, false});
    if (m_downloadLog.size() > 100) {
        for (int i = int(m_downloadLog.size()) - 1; i >= 100; --i)
            if (!m_downloadLog.at(i).request) m_downloadLog.removeAt(i);
    }
    showDownload(request);

    connect(request, &QWebEngineDownloadRequest::receivedBytesChanged, this,
            &WebBrowserPanel::refreshDownloadProgress);
    connect(request, &QWebEngineDownloadRequest::totalBytesChanged, this,
            &WebBrowserPanel::refreshDownloadProgress);
    connect(request, &QWebEngineDownloadRequest::isPausedChanged, this, &WebBrowserPanel::refreshDownloadProgress);
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
    if (!request->isSavePageDownload() && !(contentDownloadCompleted && contentDownloadCompleted(request,path))) probeCompletedDownload(path, mime);
    removeDownload(request);
}

void WebBrowserPanel::removeDownload(QWebEngineDownloadRequest* request) {
    if (request->state() != QWebEngineDownloadRequest::DownloadCompleted) {
        const auto staging = request->property("vltForumStaging").toString();
        if (!staging.isEmpty()) QDir(staging).removeRecursively();
    }
    for (auto& entry : m_downloadLog) if (entry.request == request) {
        entry.completed = request->state() == QWebEngineDownloadRequest::DownloadCompleted;
        entry.detail = entry.completed ? tr("Completed") + QStringLiteral(" · ") + QLocale().formattedDataSize(request->receivedBytes())
            : request->state() == QWebEngineDownloadRequest::DownloadCancelled ? tr("Cancelled")
            : tr("Failed: %1").arg(request->interruptReasonString());
        entry.request.clear();
    }
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
    if (m_libraryKind == Library::Downloads) refreshLibrary();
}

void WebBrowserPanel::showDownload(QWebEngineDownloadRequest* request) {
    m_visibleDownload = request;
    m_downloadName->setText(request->downloadFileName());
    m_downloadName->setToolTip(request->downloadFileName());
    m_downloadBar->show();
    refreshDownloadProgress();
}

void WebBrowserPanel::refreshDownloadProgress() {
    if (m_libraryKind == Library::Downloads && !m_downloadRefreshTimer->isActive()) m_downloadRefreshTimer->start();
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
    // Quick executes the anchor inside the loaded document. Wait for its
    // loadFinished handler above instead of racing navigation with a timer.
    if (tab->view->page()->isQuick()) return;
    // Offscreen Chromium can suppress loadFinished while a preceding internal
    // page is being replaced. The download itself is still initiated by the
    // real QWebEnginePage and must travel through downloadRequested.
    QTimer::singleShot(100, this, [this, expected = m_testDownloadPath] {
        if (expected.isEmpty() || m_testDownloadPath != expected) return;
        const QString fixture = std::exchange(m_testDownloadPath, {});
        if (BrowserSurface* target = view()) {
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
    return tab && tab->showingStartPage && !tab->loading &&
           tab->view->title() == QLatin1String("VLTONE Start") && m_address &&
           m_address->text().isEmpty();
}

void WebBrowserPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // At most one responsive bookmark pass per frame. The old zero-timeout
    // scheduling queued one pass per mouse move after the pointer had stopped.
    if (m_resizeTimer && !m_resizeTimer->isActive()) m_resizeTimer->start();
}

void WebBrowserPanel::applyTheme() {
    const auto& theme = th();
    QPalette colors = palette();
    colors.setColor(QPalette::Window, theme.background);
    colors.setColor(QPalette::Base, theme.well());
    colors.setColor(QPalette::WindowText, theme.textPrimary);
    colors.setColor(QPalette::Text, theme.textPrimary);
    colors.setColor(QPalette::PlaceholderText, theme.textSecondary);
    colors.setColor(QPalette::ButtonText, theme.textPrimary);
    colors.setColor(QPalette::Highlight, theme.accent);
    colors.setColor(QPalette::HighlightedText, theme.accentText());
    setPalette(colors);
    for (auto* button : findChildren<ui::IconButton*>()) {
        button->setIdleColor(theme.textSecondary);
        button->setActiveColor(theme.accentHighlight);
    }
    for (auto* button : m_railButtons)
        button->setIcon(icons::icon(static_cast<icons::Glyph>(button->property("webGlyph").toInt()), theme.textPrimary, 18));
    for (Tab* tab : std::as_const(m_tabs)) {
        tab->view->page()->setBackgroundColor(theme.background);
        updateTabLabel(tab);
    }
    if (m_menu) {
        QPixmap dots(32, 32);
        dots.fill(Qt::transparent);
        QPainter painter(&dots);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.textPrimary);
        for (int x : {8, 16, 24}) painter.drawEllipse(QPointF(x, 16), 1.7, 1.7);
        painter.end();
        m_menu->setIcon(QIcon(dots));
    }
    QString style = QStringLiteral(R"(
#WebBrowserPanel, #WebViewFrame, #WebViewStack, #WebLibrary { background: %BG%; border: none; }
#WebTabStrip { background: %HEADER%; border: none; }
#WebToolbar { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %TOP%,stop:1 %BOTTOM%); border: none; }
#WebBookmarksBar, #WebFindBar { background: %BOTTOM%; border: none; border-bottom: 1px solid %LINE%; }
#WebSidebar { background: %HEADER%; border: none; border-right: 1px solid %LINE%; }
QTabBar#WebTabBar { background: transparent; qproperty-drawBase: 0; }
QTabBar#WebTabBar::tab {
    color: %MUTED%; background: transparent; border: 1px solid transparent; border-radius: 6px;
    padding: 2px 5px 2px 10px; margin: 0 3px 0 0;
    min-width: 48px; max-width: 170px; height: 28px; font-size: 11px;
}
QTabBar#WebTabBar::tab:!selected:hover { background: %SURFACE%; color: %TEXT%; }
QTabBar#WebTabBar::tab:selected { background: %TOP%; color: %TEXT%; border-color: %EDGE%; }
QTabBar#WebTabBar::scroller { width: 28px; }
QTabBar#WebTabBar QToolButton { background: %TOP%; border: 1px solid %LINE%; border-radius: 4px; color: %TEXT%; }
#WebAddress, #WebFindText, #WebLibrarySearch {
    color: %TEXT%; background: %WELL%; border: 1px solid %LINE%; border-radius: 6px;
    padding: 4px 7px; min-height: 22px; font-size: 12px;
    selection-background-color: %ACCENT%; selection-color: %ACCENTINK%;
}
#WebAddress:hover, #WebFindText:hover, #WebLibrarySearch:hover { border-color: %EDGE%; }
#WebMenuButton, #WebZoom { color: %TEXT%; background: transparent; border: 1px solid transparent; border-radius: 6px; font-size: 11px; padding: 0; }
#WebMenuButton:hover, #WebZoom:hover { background: %HOVER%; border-color: %LINE%; }
#WebMenuButton:pressed, #WebZoom:pressed { background: %WELL%; }
QPushButton#WebRailButton { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 0; }
QPushButton#WebRailButton:hover { background: %HOVER%; border-color: %LINE%; }
QPushButton#WebRailButton:checked { background: %TINT%; }
QPushButton#WebRailButton:pressed { background: %WELL%; }
#WebDownloadBar { background: %BOTTOM%; border: none; border-top: 1px solid %LINE%; }
#WebPageProgress, #WebDownloadProgress { border: none; background: %BOTTOM%; border-radius: 1px; }
#WebPageProgress::chunk, #WebDownloadProgress::chunk { background: %ACCENT%; border-radius: 1px; }
#WebDownloadName { color: %TEXT%; font-size: 11px; }
#WebFindLabel, #WebFindResult, #WebBookmarksHint { color: %MUTED%; font-size: 11px; }
QPushButton#WebBookmarksButton, QPushButton#WebBookmarkChip, QPushButton#WebBookmarksMore {
    min-height: 23px; max-height: 23px; padding: 0 8px;
    color: %MUTED%; background: transparent; border: 1px solid transparent; border-radius: 5px; font-size: 11px;
}
QPushButton#WebBookmarksButton { color: %TEXT%; }
QPushButton#WebBookmarksButton:hover, QPushButton#WebBookmarkChip:hover,
QPushButton#WebBookmarksMore:hover { color: %TEXT%; background: %HOVER%; }
#WebLibraryTitle { color: %TEXT%; font-size: 17px; font-weight: 600; }
#WebLibraryEmpty { color: %MUTED%; font-size: 12px; padding: 8px 0; }
#WebLibraryList { background: transparent; border: none; color: %TEXT%; outline: none; font-size: 12px; }
#WebLibraryList::item { border: none; }
#WebLibraryAction { color: %TEXT%; background: %SURFACE%; border: 1px solid %LINE%; border-radius: 6px; padding: 6px 10px; font-size: 11px; }
#WebLibraryAction:hover { background: %HOVER%; }
#WebLibraryAction:disabled { color: %MUTED%; background: %BG%; }
)");
    const auto color = [&style](const char* key, const QColor& value) { style.replace(QLatin1String(key), value.name()); };
    color("%BG%", theme.background);
    color("%HEADER%", theme.headerBackground);
    color("%SURFACE%", theme.surface);
    color("%TOP%", theme.panelTop());
    color("%BOTTOM%", theme.panelBottom());
    color("%TEXT%", theme.textPrimary);
    color("%MUTED%", theme.textSecondary);
    color("%LINE%", theme.separator());
    color("%EDGE%", theme.edgeLight(theme.surface));
    color("%HOVER%", theme.surfaceElevated);
    color("%WELL%", theme.well());
    color("%ACCENT%", theme.accent);
    color("%ACCENTINK%", theme.accentText());
    color("%TINT%", mixColors(theme.surface, theme.accent, 0.16));
    setStyleSheet(style);
    if (m_addressIcon && currentTab()) updateAddress();
    if (m_libraryKind != Library::None) refreshLibrary();
    update();
}

void WebBrowserPanel::refreshSite(const QUrl& origin) {
    for (auto* tab:m_tabs) {const auto url=tab->view->url();if(url.scheme()==origin.scheme()&&url.host()==origin.host()&&url.port()==origin.port())tab->view->reload();}
}
void WebBrowserPanel::showContentResult(const QString& message) {
    m_downloadName->setText(message);m_downloadName->setToolTip(message);
    m_downloadProgress->setRange(0,1000);m_downloadProgress->setValue(1000);
    m_downloadBar->show();
}
