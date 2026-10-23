#include "InternalEditorFrame.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QHideEvent>
#include <QLineEdit>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>
#include <QUuid>
#include <algorithm>
#include <cstdio>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

InternalEditorFrame::InternalEditorFrame(QString settingsKey, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                     Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint),
      m_settingsKey(std::move(settingsKey)) {
    setObjectName(QStringLiteral("InternalEditorFrame"));
    setProperty("dawInternalEditor", true);
    setAttribute(Qt::WA_QuitOnClose, false);
    // Restoring the owner must not activate each editor as it reappears.
    // present() explicitly activates an editor on a user request.
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFrameStyle(QFrame::NoFrame);
    setAutoFillBackground(true);
    m_column = new QVBoxLayout(this);
    m_column->setContentsMargins(0, 0, 0, 0);
    m_column->setSpacing(0);
    connect(&ThemeManager::instance(), &ThemeManager::changed,
            this, &InternalEditorFrame::applyTheme);
    applyTheme();
    hide();
}

InternalEditorFrame::~InternalEditorFrame() {
    if (qApp) qApp->removeEventFilter(this);
    if (m_content) disconnect(m_content, nullptr, this, nullptr);
}

void InternalEditorFrame::setContent(QWidget* content) {
    if (!content || content == m_content) return;
    if (m_content) {
        disconnect(m_content, nullptr, this, nullptr);
        m_column->removeWidget(m_content);
        m_content->setParent(nullptr);
    }
    m_content = content;
    m_preferredContentSize = content->size();
    content->setParent(this, Qt::Widget);
    content->clearMask();
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_column->addWidget(content, 1);
    const auto syncTitle = [this](const QString& title) {
        const auto name = title.isEmpty() ? tr("Editor") : title;
        setWindowTitle(name);
        setAccessibleName(name);
    };
    syncTitle(content->windowTitle());
    connect(content, &QWidget::windowTitleChanged, this, syncTitle);
    connect(content, &QObject::destroyed, this, [this] {
        m_content = nullptr;
        m_lastContentFocus = nullptr;
    });
}

void InternalEditorFrame::setAccessoryWidget(QWidget* accessory) { m_accessory = accessory; }
void InternalEditorFrame::setWorkspaceArea(QWidget* area) { m_workspaceArea = area; }

void InternalEditorFrame::prepareForNativeSurface() {
    // A vendor HWND/NSView stays in this window and never promotes the shared
    // workspace (and its Quick surface) to native child widgets.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
}

void InternalEditorFrame::present() {
    restorePlacement();
    if (m_content) m_content->show();
    if (isMinimized()) showNormal();
    else show();
    raiseEditor();
    activateWindow();
    activateEditor();
    // No queued focus request: a later return to the main window must win.
    if (m_lastContentFocus && m_lastContentFocus->isVisible())
        m_lastContentFocus->setFocus(Qt::OtherFocusReason);
    else if (m_content)
        m_content->setFocus(Qt::OtherFocusReason);
}

void InternalEditorFrame::raiseEditor() { if (isVisible()) raise(); }
void InternalEditorFrame::activateEditor() { if (isVisible()) setEditorActive(true); }
void InternalEditorFrame::deactivateEditor() { setEditorActive(false); }

void InternalEditorFrame::setExpansionEnabled(bool enabled) {
    if (m_expansionEnabled == enabled) return;
    if (!enabled && isMaximized()) showNormal();
    m_expansionEnabled = enabled;
    const bool visible = isVisible();
    setWindowFlag(Qt::WindowMaximizeButtonHint, enabled);
    if (visible) show();
}

void InternalEditorFrame::setMaximized(bool maximized) {
    if (maximized && !m_expansionEnabled) return;
    restorePlacement();
    if (maximized) showMaximized();
    else showNormal();
}

QRect InternalEditorFrame::availableRect() const {
    auto* target = screen();
    if (!target) target = QGuiApplication::primaryScreen();
    return target ? target->availableGeometry().adjusted(8, 8, -8, -8)
                  : QRect(0, 0, 1280, 720);
}

QSize InternalEditorFrame::maximumContentSize() const {
    const QSize border = frameGeometry().size() - size();
    return (availableRect().size() - border).expandedTo(QSize(1, 1));
}

void InternalEditorFrame::resizeForContent(const QSize& contentSize) {
    if (!contentSize.isValid()) return;
    m_preferredContentSize = contentSize;
    restorePlacement();
    if (isMaximized()) return;
    resize(contentSize.boundedTo(maximumContentSize()));
    constrainToScreen();
    savePlacement();
}

void InternalEditorFrame::moveWithinWorkspace(const QPoint& position) {
    move(position);
    constrainToScreen();
}

void InternalEditorFrame::restorePlacement() {
    if (m_placementRestored) return;
    // Materialize the system frame before measuring its non-client margins.
    winId();
    const auto saved = QSettings().value(m_settingsKey + "/nativeGeometry").toByteArray();
    if (saved.isEmpty() || !restoreGeometry(saved)) {
        resize(m_preferredContentSize.boundedTo(maximumContentSize()));
        QWidget* anchor = m_workspaceArea ? m_workspaceArea.data() : parentWidget();
        const QRect bounds = anchor ? QRect(anchor->mapToGlobal(QPoint()), anchor->size())
                                    : availableRect();
        move(bounds.center() - QPoint(frameGeometry().width() / 2, frameGeometry().height() / 2));
    }
    if (!m_expansionEnabled && isMaximized()) setWindowState(Qt::WindowNoState);
    constrainToScreen();
    m_placementRestored = true;
}

void InternalEditorFrame::savePlacement() {
    if (m_placementRestored && !m_interacting && !m_settingsKey.isEmpty())
        QSettings().setValue(m_settingsKey + "/nativeGeometry", saveGeometry());
}

void InternalEditorFrame::constrainToScreen() {
    if (isMaximized() || isMinimized()) return;
    const QRect bounds = availableRect();
    const QSize limit = maximumContentSize();
    if (m_content) setMinimumSize(m_content->minimumSize().boundedTo(limit));
    if (size().width() > limit.width() || size().height() > limit.height())
        resize(size().boundedTo(limit));
    const QRect frame = frameGeometry();
    const QPoint position(
        std::clamp(frame.x(), bounds.left(), std::max(bounds.left(), bounds.right() - frame.width() + 1)),
        std::clamp(frame.y(), bounds.top(), std::max(bounds.top(), bounds.bottom() - frame.height() + 1)));
    if (position != frame.topLeft()) move(position);
}

bool InternalEditorFrame::event(QEvent* event) {
    if (event->type() == QEvent::NonClientAreaMouseButtonPress)
        m_interacting = true;
    else if (event->type() == QEvent::NonClientAreaMouseButtonRelease ||
             event->type() == QEvent::WindowDeactivate) {
        m_interacting = false;
        savePlacement();
    }
    return QFrame::event(event);
}

bool InternalEditorFrame::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    const auto id = static_cast<MSG*>(message)->message;
    if (id == WM_ENTERSIZEMOVE) m_interacting = true;
    else if (id == WM_EXITSIZEMOVE) {
        m_interacting = false;
        savePlacement();
    }
#endif
    return QFrame::nativeEvent(type, message, result);
}

bool InternalEditorFrame::eventFilter(QObject* watched, QEvent* event) {
    auto* target = qobject_cast<QWidget*>(watched);
    switch (event->type()) {
    case QEvent::ApplicationDeactivate:
        m_interacting = false;
        setEditorActive(false);
        break;
    case QEvent::WindowActivate:
        if (target == this) setEditorActive(true);
        else if (target && target->isWindow() && !belongsToFrame(target) &&
                 (!parentWidget() || target != parentWidget()->window()))
            setEditorActive(false);
        break;
    case QEvent::FocusIn:
        if (belongsToFrame(target)) {
            m_lastContentFocus = target;
            setEditorActive(true);
            break;
        }
        // Keyboard and programmatic focus changes back to the owner must
        // release the editor's commands just like a click in the arrangement.
        [[fallthrough]];
    case QEvent::MouseButtonPress:
        if (belongsToFrame(target) || belongsToAccessory(target)) {
            setEditorActive(true);
        } else if (target && parentWidget() && target->window() == parentWidget()->window()) {
            for (auto* ancestor = target; ancestor; ancestor = ancestor->parentWidget())
                if (ancestor->property("dawPreserveEditorFocus").toBool()) return false;
            setEditorActive(false);
        }
        break;
    default:
        break;
    }
    return false;
}

bool InternalEditorFrame::belongsToFrame(const QWidget* widget) const {
    return widget && (widget == this || isAncestorOf(widget));
}
bool InternalEditorFrame::belongsToAccessory(const QWidget* widget) const {
    return widget && m_accessory && (widget == m_accessory || m_accessory->isAncestorOf(widget));
}
void InternalEditorFrame::setEditorActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    emit activeChanged(active);
}
void InternalEditorFrame::applyTheme() {
    auto palette = this->palette();
    palette.setColor(QPalette::Window, ThemeManager::instance().theme().surface);
    setPalette(palette);
}
void InternalEditorFrame::closeEvent(QCloseEvent* event) {
    savePlacement();
    event->accept();
    emit closeRequested();
}
void InternalEditorFrame::hideEvent(QHideEvent* event) {
    m_interacting = false;
    savePlacement();
    if (qApp) qApp->removeEventFilter(this);
    setEditorActive(false);
    QFrame::hideEvent(event);
}
void InternalEditorFrame::showEvent(QShowEvent* event) {
    if (qApp) qApp->installEventFilter(this);
    constrainToScreen();
    QFrame::showEvent(event);
}
void InternalEditorFrame::moveEvent(QMoveEvent* event) {
    QFrame::moveEvent(event);
    savePlacement();
}
void InternalEditorFrame::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    savePlacement();
}

bool InternalEditorFrame::checkPlacementForTest() {
    const QString key = "tests/nativeEditor/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = true;
    const auto check = [&ok](bool passed, const char* message) {
        std::fprintf(stderr, "%s native editor: %s\n", passed ? "PASS" : "FAIL", message);
        ok &= passed;
    };
    QWidget owner;
    owner.resize(1000, 700);
    QWidget host(&owner);
    host.resize(owner.size());
    owner.show();
    {
        InternalEditorFrame frame(key, &host);
        auto* content = new QLineEdit;
        content->resize(460, 240);
        content->setWindowTitle("Editor title");
        frame.setContent(content);
        frame.prepareForNativeSurface();
        frame.present();
        QApplication::processEvents();
        check(frame.isWindow() && frame.windowType() == Qt::Tool &&
              !frame.windowFlags().testFlag(Qt::FramelessWindowHint) &&
              frame.windowHandle()->transientParent() == owner.windowHandle(),
              "native tool frame stays owned by the main window");
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == "windows") {
            const auto hwnd = reinterpret_cast<HWND>(frame.winId());
            check(GetWindow(hwnd, GW_OWNER) == reinterpret_cast<HWND>(owner.winId()) &&
                  (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION &&
                  (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) &&
                  !(GetWindowLongPtr(hwnd, GWL_EXSTYLE) & (WS_EX_APPWINDOW | WS_EX_TOPMOST)),
                  "Windows supplies the caption without a taskbar entry or topmost priority");
        }
#endif
        check(!host.testAttribute(Qt::WA_NativeWindow) &&
              !frame.findChild<QWidget*>("InternalEditorTitleBar") &&
              !frame.findChild<QWidget*>("InternalEditorResizeHandle"),
              "no custom chrome or native workspace promotion remains");
        check(content->geometry() == frame.rect() && content->mask().isEmpty(),
              "the entire client area is available to the editor and its footer");
        content->setWindowTitle("Changed title");
        check(frame.windowTitle() == "Changed title", "system title follows the editor");
        frame.setExpansionEnabled(false);
        frame.setMaximized(true);
        check(!frame.isMaximized() && !frame.windowFlags().testFlag(Qt::WindowMaximizeButtonHint),
              "plugin windows retain only the supported system controls");
        frame.resizeForContent(QSize(620, 390));
        frame.moveWithinWorkspace(QPoint(-50000, -50000));
        check(frame.availableRect().contains(frame.frameGeometry()), "offscreen placement is recovered");
        int closes = 0;
        connect(&frame, &InternalEditorFrame::closeRequested, &owner, [&] { ++closes; });
        frame.close();
        check(closes == 1 && !frame.isVisible(), "system Close uses the editor lifecycle");
        frame.present();
        check(frame.isVisible() && content->isVisible(), "a hidden editor can be reopened");
    }
    {
        InternalEditorFrame restored(key, &host);
        restored.setContent(new QWidget);
        restored.present();
        check(restored.size() == QSize(620, 390), "native placement survives reopen");
    }
    QSettings().remove(key);
    return ok;
}
