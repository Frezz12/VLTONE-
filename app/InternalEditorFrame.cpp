#include "InternalEditorFrame.hpp"
#include "PluginEditorChrome.hpp"
#include "EqualizerStyle.hpp"

#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "graphics/WidgetUpdates.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "UiFrameClock.hpp"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QRasterWindow>
#include <QEventLoop>
#include <QResizeEvent>
#include <QRegion>
#include <QSettings>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

constexpr int kTitleHeight = 22;
constexpr int kCornerRadius = 8;
constexpr int kResizeBand = 6;
constexpr int kCornerBand = 12;
constexpr int kMinimumWidth = 420;
constexpr int kMinimumHeight = 300;
constexpr int kInteractiveResizeFrameMs = 16;

// Give the antialiased fringe its own alpha backing store. Both native windows
// move in the same input turn, without waiting for a busy workspace to render.
class EditorChrome final : public QRasterWindow {
public:
    EditorChrome(QWindow* parent, std::function<void(QPainter&)> paint)
        : m_paint(std::move(paint)) {
        setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                 Qt::WindowDoesNotAcceptFocus);
        auto alphaFormat = format();
        alphaFormat.setAlphaBufferSize(8);
        setFormat(alphaFormat);
        setParent(parent);
    }
protected:
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray&, void* message, qintptr* result) override {
        // Qt 6.8 only applies WS_EX_TRANSPARENT to top-level windows.
        // This child fringe must also pass resize/drag hits to its editor.
        if (static_cast<MSG*>(message)->message == WM_NCHITTEST) {
            *result = HTTRANSPARENT;
            return true;
        }
        return false;
    }
#endif
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRect(QPoint(), size()), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        m_paint(painter);
    }
private:
    std::function<void(QPainter&)> m_paint;
};

int titleHeight(const QWidget* content) {
    return content && content->property("vlt.pluginChrome").toBool() ? 26 : kTitleHeight;
}

QSize minimumFrameSize(const QWidget* content, const QRect& bounds) {
    QSize minimum(kMinimumWidth, kMinimumHeight);
    if (content && content->property("vlt.compactEditor").toBool()) minimum = QSize(340, 220);
    // Vendor GUIs and narrow instrument panels own their minimum dimensions.
    // The content's explicit minimum still applies below.
    if (content && (content->property("vlt.nativeEditor").toBool() ||
                    content->property("vlt.contentSizedEditor").toBool())) minimum = QSize(0, 0);
    if (content)
        minimum = minimum.expandedTo(content->minimumSize() + QSize(2, titleHeight(content) + 2));
    return minimum.boundedTo(bounds.size());
}

struct HandleSpec {
    int edges;
    Qt::CursorShape cursor;
};

constexpr std::array<HandleSpec, 8> kHandleSpecs{{
    {1, Qt::SizeHorCursor},
    {4, Qt::SizeHorCursor},
    {2, Qt::SizeVerCursor},
    {8, Qt::SizeVerCursor},
    {1 | 2, Qt::SizeFDiagCursor},
    {4 | 2, Qt::SizeBDiagCursor},
    {1 | 8, Qt::SizeBDiagCursor},
    {4 | 8, Qt::SizeFDiagCursor},
}};

} // namespace

InternalEditorFrame::InternalEditorFrame(QString settingsKey, QWidget* parent)
    : QFrame(parent), m_settingsKey(std::move(settingsKey)) {
    setObjectName(QStringLiteral("InternalEditorFrame"));
    setProperty("dawInternalEditor", true);
    // A widget mask is binary, even at fractional DPI. Keep the outer contour
    // in the painter so its partially covered pixels survive compositing.
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_column = new QVBoxLayout(this);
    m_column->setContentsMargins(1, 1, 1, 1);
    m_column->setSpacing(0);

    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName(QStringLiteral("InternalEditorTitleBar"));
    m_titleBar->setFixedHeight(kTitleHeight);
    m_titleBar->setCursor(Qt::SizeAllCursor);
    m_titleBar->installEventFilter(this);
    auto* titleRow = new QHBoxLayout(m_titleBar);
    titleRow->setContentsMargins(8, 1, 8, 1);
    titleRow->setSpacing(2);

    m_title = new QLabel(tr("Editor"), m_titleBar);
    m_title->setObjectName(QStringLiteral("InternalEditorTitle"));
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    titleRow->addWidget(m_title, 1);

    m_detachButton = new ui::IconButton(icons::Glyph::Detach, tr("Detach editor"), m_titleBar);
    m_detachButton->setObjectName(QStringLiteral("InternalEditorDetach"));
    m_detachButton->setButtonSize(24, 20);
    m_detachButton->setFocusPolicy(Qt::StrongFocus);
    m_detachButton->setAccessibleName(tr("Detach editor"));
    m_detachButton->setAccessibleDescription(tr("Move this editor to its own window, or return it to the workspace."));
    connect(m_detachButton, &QAbstractButton::clicked, this, [this] { setDetached(!m_detached); });
    titleRow->addWidget(m_detachButton);

    m_maximizeButton = new ui::IconButton(
        icons::Glyph::WindowMaximize, tr("Maximize editor"), m_titleBar);
    m_maximizeButton->setObjectName(QStringLiteral("InternalEditorMaximize"));
    m_maximizeButton->setButtonSize(24, 20);
    m_maximizeButton->setCheckable(true);
    m_maximizeButton->setFocusPolicy(Qt::StrongFocus);
    m_maximizeButton->setAccessibleName(tr("Maximize editor"));
    m_maximizeButton->setAccessibleDescription(
        tr("Fills the workspace with this editor; activate again to restore it."));
    connect(m_maximizeButton, &QAbstractButton::clicked, this, [this] {
        setMaximized(!m_maximized);
        restoreContentFocus();
    });
    titleRow->addWidget(m_maximizeButton);

    m_closeButton = new ui::IconButton(
        icons::Glyph::Close, tr("Close editor"), m_titleBar);
    m_closeButton->setObjectName(QStringLiteral("InternalEditorClose"));
    m_closeButton->setButtonSize(24, 20);
    m_closeButton->setGlyphSize(14);
    m_closeButton->setFocusPolicy(Qt::StrongFocus);
    m_closeButton->setAccessibleName(tr("Close editor"));
    m_closeButton->setAccessibleDescription(
        tr("Hides this editor without closing the project."));
    connect(m_closeButton, &QAbstractButton::clicked, this,
            &InternalEditorFrame::closeRequested);
    titleRow->addWidget(m_closeButton);

    m_column->addWidget(m_titleBar);

    for (std::size_t i = 0; i < m_resizeHandles.size(); ++i) {
        auto* handle = new QWidget(this);
        handle->setObjectName(QStringLiteral("InternalEditorResizeHandle"));
        handle->setProperty("edges", kHandleSpecs[i].edges);
        handle->setCursor(kHandleSpecs[i].cursor);
        handle->setMouseTracking(true);
        handle->installEventFilter(this);
        m_resizeHandles[i] = handle;
    }

    m_resizeApplyTimer.setSingleShot(true);
    m_resizeApplyTimer.setTimerType(Qt::PreciseTimer);
    m_resizeApplyTimer.setInterval(kInteractiveResizeFrameMs);
    connect(&m_resizeApplyTimer, &QTimer::timeout, this,
            &InternalEditorFrame::applyPendingInteractiveResize);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &InternalEditorFrame::applyTheme);
    applyTheme();
    updateMaximizeButton();
    hide();
}

InternalEditorFrame::~InternalEditorFrame() {
    setProperty("vlt.nativeOverlayChrome", QVariant());
    delete m_nativeChrome.data();
    delete m_detachedSurface.data();
    cancelPendingInteractiveResize();
    uninstallApplicationEventFilter();
    // QWidget deletes its children after our QPointer members have already
    // been destroyed. The content's destroyed callback below must not write
    // into those members during that base-class teardown.
    if (m_content) disconnect(m_content, nullptr, this, nullptr);
}

void InternalEditorFrame::setContent(QWidget* content) {
    if (!content || content == m_content) return;
    if (m_content) {
        disconnect(m_content, nullptr, this, nullptr);
        disconnect(m_content, nullptr, m_title, nullptr);
        m_column->removeWidget(m_content);
        m_content->clearMask();
        m_content->setParent(nullptr);
    }

    m_content = content;
    const bool pluginChrome = content->property("vlt.pluginChrome").toBool();
    const int inset = !pluginChrome && content->property("vlt.eqChrome").toBool() ? 0 : 1;
    m_column->setContentsMargins(inset, inset, inset, inset);
    m_titleBar->setFixedHeight(titleHeight(content));
    m_titleBar->layout()->setContentsMargins(8, 1, pluginChrome ? 4 : 8, 1);
    m_closeButton->setButtonSize(24, pluginChrome ? 24 : 20);
    m_closeButton->setGlyphSize(pluginChrome ? 10 : 14);
    m_closeButton->setProperty("pluginWindowClose", pluginChrome);
    // Only this application-owned editor opts into one continuous dark shell.
    // Placement, resize, detach and close continue to use the common host.
    m_pitchChrome = content->property("vlt.pitchChrome").toBool();
    applyTheme();
    content->setParent(this);
    m_preferredContentSize = content->property("vlt.compactEditor").toBool()
        ? content->size() : content->size().expandedTo(QSize(640, 360));
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_column->addWidget(content, 1);
    const auto syncTitle = [this, pluginChrome](const QString& title) {
        const auto name = title.isEmpty() ? tr("Editor") : title;
        m_title->setText(pluginChrome ? QString() : name);
        setWindowTitle(name);
        setAccessibleName(name);
    };
    syncTitle(content->windowTitle());
    connect(content, &QWidget::windowTitleChanged, this, syncTitle);
    connect(content, &QObject::destroyed, this, [this] {
        m_content = nullptr;
        m_lastContentFocus = nullptr;
    });
    updateClipping();
}

void InternalEditorFrame::setAccessoryWidget(QWidget* accessory) {
    m_accessory = accessory;
}

void InternalEditorFrame::setWorkspaceArea(QWidget* area) {
    m_workspaceArea = area;
    if (m_placementRestored) constrainToParent();
}

void InternalEditorFrame::prepareForNativeSurface() {
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_NoSystemBackground, false);
    // This must precede WA_NativeWindow. Otherwise QWidget promotes every
    // ancestor to a native view, an irreversible change for the lifetime of
    // the main window which leaves the Quick scene in a broken stacking tree.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
    setProperty("vlt.nativeOverlay", true);
    if (!m_nativeChrome) {
        m_nativeChrome = new EditorChrome(nativeParentWidget()->windowHandle(), [this](QPainter& painter) {
            painter.setClipRegion(QRegion(rect()).subtracted(mask()));
            paintChrome(painter);
        });
        m_nativeChrome->setObjectName(QStringLiteral("InternalEditorNativeChrome"));
        setProperty("vlt.nativeOverlayChrome", QVariant::fromValue<QWindow*>(m_nativeChrome));
    }
    updateClipping();
    applyTheme();
}

void InternalEditorFrame::present() {
    if (!m_placementRestored) restorePlacement();
    if (m_content) m_content->show();
    show();
    activateEditor();
    restoreContentFocus();
}

void InternalEditorFrame::raiseEditor() {
    if (!isVisible()) return;
    // QWidget::raise() returns early when its QObject sibling order already
    // looks correct. Native activation can change the actual HWND/NSView order
    // independently, so explicitly restore that order too.
    ui::graphics::raiseNativeOverlay(this);
}

void InternalEditorFrame::activateEditor() {
    if (!isVisible()) return;
    installApplicationEventFilter();
    raiseEditor();
    setEditorActive(true);
}

void InternalEditorFrame::moveWithinWorkspace(const QPoint& position) {
    const QRect wanted = constrainedGeometry(QRect(position, size()));
    if (wanted.size() != size() || !ui::graphics::moveNativeOverlay(this, wanted.topLeft()))
        setGeometry(wanted);
}

void InternalEditorFrame::setExpansionEnabled(bool enabled) {
    if (m_expansionEnabled == enabled) return;
    if (!enabled) {
        if (m_detached) setDetached(false);
        if (m_maximized) setMaximized(false);
    }
    m_expansionEnabled = enabled;
    m_maximizeButton->setVisible(enabled);
    m_detachButton->setVisible(enabled);
    updateResizeHandles();
}

void InternalEditorFrame::setMaximized(bool maximized) {
    if (maximized && !m_expansionEnabled) return;
    if (!m_placementRestored) restorePlacement();
    if (m_maximized == maximized) return;
    cancelPendingInteractiveResize();
    m_resizing = false;
    m_resizeEdges = NoEdge;
    if (maximized) m_restoreGeometry = geometry();
    m_maximized = maximized;
    const QRect limits = m_maximized ? workspaceRect() : availableRect();
    setMinimumSize(minimumFrameSize(m_content, limits));
    setGeometry(maximized ? workspaceRect()
                          : constrainedGeometry(m_restoreGeometry));
    updateMaximizeButton();
    updateResizeHandles();
    savePlacement();
    activateEditor();
}

void InternalEditorFrame::setDetached(bool detached) {
    if (detached && !m_expansionEnabled) return;
    if (m_detached == detached || m_dragging || m_resizing) return;
    cancelPendingInteractiveResize();
    const bool visible = isVisible();
    const QPoint global = mapToGlobal(QPoint());
    if (m_maximized) setMaximized(false);
    // Release the old window's render resources before changing native hosts.
    // The editor, command handlers and project model keep their identities.
    delete m_detachedSurface.data();
    m_detachedSurface = nullptr;
    if (detached) { savePlacement(); m_dockedGeometry = geometry(); }
    hide();
    m_detached = detached;
    setAttribute(Qt::WA_TranslucentBackground, detached || !m_nativeChrome);
    setParent(parentWidget(), detached ? Qt::Window | Qt::FramelessWindowHint : Qt::Widget);
    setGeometry(detached ? QRect(global, size()) : m_dockedGeometry);
    updateClipping();
    if (!detached) m_restoreGeometry = m_dockedGeometry;
    m_detachButton->setToolTip(detached ? tr("Return editor to workspace") : tr("Detach editor"));
    m_detachButton->setAccessibleName(m_detachButton->toolTip());
    if (detached && ui::graphics::gpuWorkspaceEnabled()) {
        m_detachedSurface = new ui::graphics::WorkspaceSurface(this);
        connect(&ui::FrameClock::instance(), &ui::FrameClock::preferenceChanged,
                m_detachedSurface, &ui::graphics::WorkspaceSurface::refreshPresentationMode);
        connect(&ThemeManager::instance(), &ThemeManager::changed,
                m_detachedSurface, &ui::graphics::WorkspaceSurface::invalidate);
        connect(m_detachedSurface, &ui::graphics::WorkspaceSurface::failed,
                m_detachedSurface, &QObject::deleteLater, Qt::QueuedConnection);
    }
    constrainToParent();
    if (visible) { show(); raise(); activateWindow(); restoreContentFocus(); }
}

void InternalEditorFrame::resizeForContent(const QSize& contentSize) {
    if (!contentSize.isValid()) return;
    if (!m_placementRestored) restorePlacement();
    m_preferredContentSize = contentSize;
    if (m_maximized) return;

    const QSize chrome(2, titleHeight(m_content) + 2);
    setMinimumSize(minimumFrameSize(m_content, availableRect()));
    QRect wanted(geometry().topLeft(), contentSize + chrome);
    QRect constrained = constrainedGeometry(wanted);
    // A plugin opening its parameter dock should stay fully visible if it was
    // fully visible before. An intentionally parked editor keeps its title in
    // place when the plugin requests another content size.
    const QRect bounds = availableRect();
    if (!isInteracting() && bounds.contains(geometry()) && constrained.bottom() > bounds.bottom())
        constrained.moveBottom(bounds.bottom());
    setGeometry(constrained);
    m_restoreGeometry = constrained;
    updateResizeHandles();
    savePlacement();
}

QSize InternalEditorFrame::maximumContentSize() const {
    const QSize chrome(2, titleHeight(m_content) + 2);
    const QSize outer = availableRect().size();
    return QSize(std::max(1, outer.width() - chrome.width()),
                 std::max(1, outer.height() - chrome.height()));
}

bool InternalEditorFrame::eventFilter(QObject* watched, QEvent* event) {
    // Visible editors observe application-wide focus/gesture events. Paint,
    // timer and style traffic from a large mixer does not concern this frame.
    switch (event->type()) {
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate:
    case QEvent::ApplicationDeactivate:
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
    case QEvent::WindowActivate:
    case QEvent::ApplicationActivate:
    case QEvent::FocusIn:
        break;
    default:
        return false;
    }
    auto* target = qobject_cast<QWidget*>(watched);
    // Application filters see every editor's handles, in reverse installation
    // order. Only this frame's own handles may start or continue its resize.
    const bool ownHandle = target &&
        std::find(m_resizeHandles.begin(), m_resizeHandles.end(), target) !=
            m_resizeHandles.end();
    if ((event->type() == QEvent::UngrabMouse &&
         (watched == m_titleBar || ownHandle)) ||
        (event->type() == QEvent::WindowDeactivate && target == window()) ||
        event->type() == QEvent::ApplicationDeactivate) {
        if (m_dragging || m_resizing) {
            cancelPendingInteractiveResize();
            m_dragging = m_resizing = false;
            if (QWidget::mouseGrabber() == m_titleBar) m_titleBar->releaseMouse();
            m_resizeEdges = NoEdge;
            updateMaximizeButton();
            m_restoreGeometry = geometry();
            savePlacement();
        }
    }
    if ((watched == parentWidget() && event->type() == QEvent::Resize) ||
        (watched == m_workspaceArea &&
         (event->type() == QEvent::Resize || event->type() == QEvent::Move))) {
        constrainToParent();
    }

    if (watched == m_titleBar) {
        auto* mouse = dynamic_cast<QMouseEvent*>(event);
        if (mouse && event->type() == QEvent::MouseButtonDblClick &&
            mouse->button() == Qt::LeftButton) {
            if (m_expansionEnabled) setMaximized(!m_maximized);
            restoreContentFocus();
            mouse->accept();
            return true;
        }
        if (mouse && event->type() == QEvent::MouseButtonPress &&
            mouse->button() == Qt::LeftButton) {
            activateEditor();
            if (!m_maximized) {
                m_dragging = true;
                m_pressGlobal = mouse->globalPosition().toPoint();
                m_pressGeometry = geometry();
                m_titleBar->setCursor(Qt::SizeAllCursor);
                if (testAttribute(Qt::WA_NativeWindow)) m_titleBar->grabMouse();
            }
            mouse->accept();
            return true;
        }
        if (mouse && event->type() == QEvent::MouseMove && m_dragging &&
            (mouse->buttons() & Qt::LeftButton)) {
            const QPoint delta = mouse->globalPosition().toPoint() - m_pressGlobal;
            moveWithinWorkspace(pos() + delta);
            // A late editor resize or a workspace edge must not leave a dead
            // zone measured from the old opening geometry on direction change.
            m_pressGlobal = mouse->globalPosition().toPoint();
            mouse->accept();
            return true;
        }
        if (mouse && event->type() == QEvent::MouseButtonRelease && m_dragging &&
            mouse->button() == Qt::LeftButton) {
            moveWithinWorkspace(pos() + mouse->globalPosition().toPoint() - m_pressGlobal);
            m_dragging = false;
            if (QWidget::mouseGrabber() == m_titleBar) m_titleBar->releaseMouse();
            m_titleBar->setCursor(Qt::SizeAllCursor);
            m_restoreGeometry = geometry();
            savePlacement();
            mouse->accept();
            return true;
        }
    }

    if (ownHandle) {
        auto* mouse = dynamic_cast<QMouseEvent*>(event);
        if (mouse && event->type() == QEvent::MouseButtonPress &&
            mouse->button() == Qt::LeftButton && !m_maximized) {
            activateEditor();
            cancelPendingInteractiveResize();
            m_resizing = true;
            m_resizeEdges = target->property("edges").toInt();
            m_pressGlobal = mouse->globalPosition().toPoint();
            m_pressGeometry = geometry();
            mouse->accept();
            return true;
        }
        if (mouse && event->type() == QEvent::MouseMove && m_resizing &&
            (mouse->buttons() & Qt::LeftButton)) {
            queueInteractiveResize(interactiveResizeGeometry(
                mouse->globalPosition().toPoint()));
            mouse->accept();
            return true;
        }
        if (mouse && event->type() == QEvent::MouseButtonRelease && m_resizing &&
            mouse->button() == Qt::LeftButton) {
            // The coalescer may still be waiting when the button comes up. Use
            // the release position as the final sample and apply it synchronously
            // so persisted placement never lags one frame behind the pointer.
            m_pendingResizeGeometry = interactiveResizeGeometry(
                mouse->globalPosition().toPoint());
            m_resizeApplyTimer.stop();
            applyPendingInteractiveResize();
            m_resizing = false;
            m_resizeEdges = NoEdge;
            m_restoreGeometry = geometry();
            savePlacement();
            mouse->accept();
            return true;
        }
    }

    if (event->type() == QEvent::WindowActivate && target && target->isWindow()) {
        if (belongsToFrame(target)) {
            activateEditor();
        } else if (target == window()) {
            setEditorActive(belongsToFrame(QApplication::focusWidget()));
        } else {
            setEditorActive(false);
        }
    } else if (event->type() == QEvent::ApplicationActivate) {
        setEditorActive(belongsToFrame(QApplication::focusWidget()));
    } else if (event->type() == QEvent::ApplicationDeactivate) {
        setEditorActive(false);
    } else if (event->type() == QEvent::FocusIn && target &&
               belongsToFrame(target)) {
        if (m_content &&
            (target == m_content || m_content->isAncestorOf(target))) {
            m_lastContentFocus = target;
        }
        activateEditor();
    } else if (event->type() == QEvent::MouseButtonPress && target) {
        if (belongsToFrame(target) || belongsToAccessory(target)) {
            activateEditor();
        } else if (isVisible() && target->window() == window()) {
            setEditorActive(false);
        }
    }

    return QFrame::eventFilter(watched, event);
}

void InternalEditorFrame::hideEvent(QHideEvent* event) {
    if (m_nativeChrome) m_nativeChrome->hide();
    // A hidden frame must not remain on QApplication's global filter list, and
    // no delayed resize is allowed to wake it after it has left the workspace.
    uninstallApplicationEventFilter();
    cancelPendingInteractiveResize();
    m_resizing = false;
    m_resizeEdges = NoEdge;
    m_dragging = false;
    if (QWidget::mouseGrabber() == m_titleBar) m_titleBar->releaseMouse();
    if (m_titleBar && !m_maximized)
        m_titleBar->setCursor(Qt::SizeAllCursor);
    setEditorActive(false);
    QFrame::hideEvent(event);
}

void InternalEditorFrame::moveEvent(QMoveEvent* event) {
    QFrame::moveEvent(event);
    if (m_nativeChrome && !m_detached)
        m_nativeChrome->setPosition(parentWidget()->mapTo(nativeParentWidget(), pos()));
}

void InternalEditorFrame::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    paintChrome(painter);
}

void InternalEditorFrame::paintChrome(QPainter& painter) {
    const Theme& theme = ThemeManager::instance().theme();
    const bool pluginChrome = m_content && m_content->property("vlt.pluginChrome").toBool();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal radius = pluginChrome ? 10 : kCornerRadius;
    QPainterPath outline;
    outline.addRoundedRect(QRectF(rect()), radius, radius);
    const int headerBottom = m_titleBar ? m_titleBar->geometry().bottom() + 1 : 0;
    // Header and body own disjoint parts of one circular outline. Clipping a
    // rectangular fill with a path loses coverage in the retained renderer.
    painter.save();
    painter.setClipRect(QRect(0, headerBottom, width(), height() - headerBottom), Qt::IntersectClip);
    const bool eqCanvas = m_content && m_content->property("vlt.eqChrome").toBool();
    if (pluginChrome) {
        painter.fillPath(outline, pluginEditorChromeSurface(theme));
    } else if (eqCanvas) {
        painter.fillPath(outline, eqUi::canvas());
    } else if (m_pitchChrome) {
        QLinearGradient shell(rect().topLeft(), rect().bottomLeft());
        const QColor top = mixColors(QColor("#2b2d31"), theme.accent, .06);
        shell.setColorAt(0, top); shell.setColorAt(.18, top);
        shell.setColorAt(1, mixColors(QColor("#24262b"), theme.accent, .06));
        painter.fillPath(outline, shell);
    } else {
        painter.fillPath(outline, theme.surface);
    }
    painter.restore();

    if (m_titleBar) {
        const QRect title = m_titleBar->geometry();
        QLinearGradient header(title.topLeft(), title.bottomLeft());
        const QColor base = pluginChrome ? pluginEditorChromeSurface(theme) : theme.toolbarBackground;
        header.setColorAt(0, mixColors(base, theme.surfaceElevated, pluginChrome ? .025 : .40));
        header.setColorAt(1, base);
        painter.save();
        painter.setClipRect(QRect(0, 0, width(), headerBottom), Qt::IntersectClip);
        painter.fillPath(outline, header);
        painter.restore();
        if (!pluginChrome) {
            painter.setPen(theme.edgeLight(theme.toolbarBackground));
            painter.drawLine(QPoint(int(radius), title.top()), QPoint(width() - int(radius), title.top()));
        }
        painter.setPen(pluginChrome ? mixColors(base, theme.textPrimary, .08) : theme.edgeDark(theme.toolbarBackground));
        painter.drawLine(m_titleBar->geometry().bottomLeft(),
                         m_titleBar->geometry().bottomRight());
    }

    if (eqCanvas && !pluginChrome) return;
    painter.setBrush(Qt::NoBrush);
    // Plugin frames indicate focus through a slightly stronger neutral contour.
    const QColor edge = pluginChrome
        ? mixColors(pluginEditorChromeSurface(theme), theme.textPrimary, m_active ? .36 : .24)
        : theme.edgeDark(theme.surface);
    painter.setPen(QPen(edge, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            radius - 0.5, radius - 0.5);
}

void InternalEditorFrame::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    updateClipping();
    updateResizeHandles();
}

void InternalEditorFrame::updateClipping() {
    // Only the child artwork needs a hard clip; it stays inside the opaque
    // part of the shell and must never trim the shell's antialiased exterior.
    const int radius = m_content && m_content->property("vlt.pluginChrome").toBool() ? 10 : kCornerRadius;
    QRegion interior(rect().adjusted(1, 1, -1, -1));
    // QRegion(path.toPolygon()) rounds the curve through pixel centres and
    // can let opaque child pixels spill into the exterior at fractional DPI.
    // Keep whole logical cells inside the inset circle, with half a logical
    // pixel for independent child-layer snapping at fractional DPI. There are
    // at most nine short rows per corner; the rectangle stays unchanged.
    for (int y = 1; y < radius; ++y) {
        const qreal dy = radius - y;
        const qreal innerRadius = radius - 1.5;
        const int cut = qCeil(radius - std::sqrt(std::max(0., innerRadius * innerRadius - dy * dy))) - 1;
        for (const int row : {y, height() - 1 - y}) {
            interior -= QRect(1, row, cut, 1);
            interior -= QRect(width() - 1 - cut, row, cut, 1);
        }
    }
    if (m_nativeChrome && !m_detached) {
        // The opaque view and alpha fringe are native siblings. Neither is
        // recorded into the workspace's asynchronously presented scene.
        setMask(interior);
        m_nativeChrome->setGeometry(QRect(parentWidget()->mapTo(nativeParentWidget(), pos()), size()));
        m_nativeChrome->setVisible(isVisible());
        raiseEditor();
        m_nativeChrome->update();
    } else {
        clearMask();
        if (m_nativeChrome) m_nativeChrome->hide();
    }
    if (m_content) {
        m_column->activate();
        m_content->setMask(interior.translated(-m_content->pos()));
    }
}

void InternalEditorFrame::showEvent(QShowEvent* event) {
    // While hidden the parent may have changed size. Re-constrain before the
    // first exposed paint, then opt into the global focus/activation stream.
    if (m_placementRestored) constrainToParent();
    updateClipping();
    installApplicationEventFilter();
    QFrame::showEvent(event);
}

void InternalEditorFrame::applyTheme() {
    const Theme& theme = ThemeManager::instance().theme();
    setAttribute(Qt::WA_OpaquePaintEvent, m_nativeChrome && !m_detached &&
                 (m_pitchChrome ? theme.accent : theme.surface).alpha() == 255);
    if (m_nativeChrome) m_nativeChrome->update();
    if (m_title) {
        QPalette palette = m_title->palette();
        palette.setColor(QPalette::WindowText,
                         m_active ? theme.textPrimary : theme.textSecondary);
        m_title->setPalette(palette);
    }
    update();
}

void InternalEditorFrame::setEditorActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    applyTheme();
    emit activeChanged(active);
}

void InternalEditorFrame::restorePlacement() {
    if (m_placementRestored) return;
    m_placementRestored = true;

    QSettings settings;
    QRect saved = settings.value(m_settingsKey + "/geometry").toRect();
    // Older releases stored coordinates in the body below the header. Keep
    // those windows at the same visual location when adopting the larger host.
    if (saved.isValid() && settings.value(m_settingsKey + "/placementVersion", 1).toInt() < 2)
        saved.translate(workspaceRect().topLeft());
    m_maximized = m_expansionEnabled &&
        settings.value(m_settingsKey + "/maximized", false).toBool();
    const QRect movementBounds = m_maximized ? workspaceRect() : availableRect();
    setMinimumSize(minimumFrameSize(m_content, movementBounds));

    if (saved.isValid()) {
        m_restoreGeometry = constrainedGeometry(saved);
    } else {
        const QRect bounds = workspaceRect();
        const QSize chrome(2, titleHeight(m_content) + 2);
        const QSize minimum = minimumFrameSize(m_content, bounds);
        QSize wanted = m_preferredContentSize + chrome;
        wanted.setWidth(std::clamp(wanted.width(),
                                   minimum.width(),
                                   bounds.width()));
        wanted.setHeight(std::clamp(wanted.height(),
                                    minimum.height(),
                                    bounds.height()));
        m_restoreGeometry = QRect(
            bounds.x() + (bounds.width() - wanted.width()) / 2,
            bounds.y() + (bounds.height() - wanted.height()) / 2,
            wanted.width(), wanted.height());
    }

    setGeometry(m_maximized ? workspaceRect() : m_restoreGeometry);
    updateMaximizeButton();
    updateResizeHandles();
}

void InternalEditorFrame::savePlacement() {
    if (m_detached) return; // Keep the docked placement in workspace coordinates.
    if (!m_placementRestored || m_settingsKey.isEmpty()) return;
    QSettings settings;
    settings.setValue(m_settingsKey + "/geometry", m_restoreGeometry);
    settings.setValue(m_settingsKey + "/maximized", m_maximized);
    settings.setValue(m_settingsKey + "/placementVersion", 2);
}

void InternalEditorFrame::constrainToParent() {
    if (!m_placementRestored) return;
    const QRect bounds = m_maximized ? workspaceRect() : availableRect();
    setMinimumSize(minimumFrameSize(m_content, bounds));
    if (m_maximized) {
        setGeometry(workspaceRect());
    } else {
        const QRect constrained = constrainedGeometry(geometry());
        setGeometry(constrained);
        m_restoreGeometry = constrained;
    }
    if (m_pendingResizeGeometry.isValid()) {
        m_pendingResizeGeometry =
            constrainedGeometry(m_pendingResizeGeometry);
    }
}

QRect InternalEditorFrame::availableRect() const {
    if (m_detached && screen()) return screen()->availableGeometry();
    QWidget* host = parentWidget();
    if (!host) return QRect(0, 0, std::max(1, width()), std::max(1, height()));
    // Floating editors share the transport's parent, so Qt neither clips their
    // title bars at the old body boundary nor puts the header above them.
    const QRect bounds = host->rect();
    if (bounds.width() < 1 || bounds.height() < 1) return QRect(0, 0, 1, 1);
    return bounds;
}

QRect InternalEditorFrame::workspaceRect() const {
    const QRect bounds = availableRect();
    if (m_detached) return bounds;
    if (!m_workspaceArea || !parentWidget()) return bounds;
    const QRect body(m_workspaceArea->mapTo(parentWidget(), QPoint()),
                     m_workspaceArea->size());
    const QRect visible = bounds.intersected(body);
    return visible.isEmpty() ? bounds : visible;
}

QRect InternalEditorFrame::constrainedGeometry(const QRect& wanted) const {
    const QRect bounds = availableRect();
    const QSize minimum = minimumFrameSize(m_content, bounds);
    const int minW = minimum.width();
    const int minH = minimum.height();
    const int width = std::clamp(wanted.width(), minW, bounds.width());
    const int height = std::clamp(wanted.height(), minH, bounds.height());
    const int maxX = bounds.x() + bounds.width() - width;
    // The entire title bar (including its buttons) remains recoverable. Only
    // the body may disappear below the application viewport.
    const int visibleTitleHeight = std::min(titleHeight(m_content) + 2, bounds.height());
    const int maxY = bounds.y() + bounds.height() - visibleTitleHeight;
    const int x = std::clamp(wanted.x(), bounds.x(), maxX);
    const int y = std::clamp(wanted.y(), bounds.y(), maxY);
    return QRect(x, y, width, height);
}

void InternalEditorFrame::updateResizeHandles() {
    if (m_resizeHandles[0] == nullptr) return;
    const int w = width();
    const int h = height();
    const int sideLength = std::max(0, h - 2 * kCornerBand);
    const int topLength = std::max(0, w - 2 * kCornerBand);

    m_resizeHandles[0]->setGeometry(0, kCornerBand, kResizeBand, sideLength);
    m_resizeHandles[1]->setGeometry(w - kResizeBand, kCornerBand,
                                    kResizeBand, sideLength);
    m_resizeHandles[2]->setGeometry(kCornerBand, 0, topLength, kResizeBand);
    m_resizeHandles[3]->setGeometry(kCornerBand, h - kResizeBand,
                                    topLength, kResizeBand);
    m_resizeHandles[4]->setGeometry(0, 0, kCornerBand, kCornerBand);
    m_resizeHandles[5]->setGeometry(w - kCornerBand, 0,
                                    kCornerBand, kCornerBand);
    m_resizeHandles[6]->setGeometry(0, h - kCornerBand,
                                    kCornerBand, kCornerBand);
    m_resizeHandles[7]->setGeometry(w - kCornerBand, h - kCornerBand,
                                    kCornerBand, kCornerBand);
    for (QWidget* handle : m_resizeHandles) {
        // Compact title buttons keep their entire click area, including the
        // part that would otherwise sit under the top/corner resize bands.
        QRegion hitArea(handle->rect());
        for (QWidget* button : {m_detachButton, m_maximizeButton, m_closeButton}) {
            if (!button->isHidden())
                hitArea -= QRect(button->mapTo(this, QPoint()) - handle->pos(),
                                 button->size());
        }
        handle->setMask(hitArea);
        handle->setVisible(!m_maximized);
        if (!m_maximized) handle->raise();
    }
}

void InternalEditorFrame::updateMaximizeButton() {
    if (!m_maximizeButton) return;
    m_maximizeButton->setChecked(m_maximized);
    m_maximizeButton->setGlyph(m_maximized ? icons::Glyph::Detach
                                           : icons::Glyph::WindowMaximize);
    const QString label = m_maximized ? tr("Restore editor")
                                      : tr("Maximize editor");
    m_maximizeButton->setToolTip(label);
    m_maximizeButton->setAccessibleName(label);
    if (m_titleBar)
        m_titleBar->setCursor(m_maximized ? Qt::ArrowCursor
                                          : Qt::SizeAllCursor);
}

void InternalEditorFrame::restoreContentFocus() {
    QTimer::singleShot(0, this, [this] {
        if (!isVisible() || !m_content || isInteracting()) return;
        if (m_lastContentFocus && m_lastContentFocus->isVisible()) {
            m_lastContentFocus->setFocus(Qt::OtherFocusReason);
        } else {
            m_content->setFocus(Qt::OtherFocusReason);
        }
        // Presentation is an explicit user request and wins over any delayed
        // WindowActivate event left in the queue by the previously active
        // native editor (the offscreen platform exposes this ordering too).
        activateEditor();
    });
}

bool InternalEditorFrame::belongsToFrame(const QWidget* widget) const {
    return widget && (widget == this || isAncestorOf(widget));
}

bool InternalEditorFrame::belongsToAccessory(const QWidget* widget) const {
    return m_accessory && widget &&
           (widget == m_accessory || m_accessory->isAncestorOf(widget));
}

void InternalEditorFrame::installApplicationEventFilter() {
    // showEvent is delivered while QWidget is transitioning to visible; do not
    // gate this on isVisible(), whose value is platform/order dependent there.
    if (m_applicationEventFilterInstalled || !qApp) return;
    qApp->installEventFilter(this);
    m_applicationEventFilterInstalled = true;
}

void InternalEditorFrame::uninstallApplicationEventFilter() {
    if (!m_applicationEventFilterInstalled) return;
    if (qApp) qApp->removeEventFilter(this);
    m_applicationEventFilterInstalled = false;
}

QRect InternalEditorFrame::interactiveResizeGeometry(
    const QPoint& globalPosition) const {
    const QPoint delta = globalPosition - m_pressGlobal;
    const QRect bounds = availableRect();
    // An asynchronously loaded editor can raise its minimum during a drag.
    // Reconcile the starting rectangle before deriving anchored edge limits.
    const QRect pressGeometry = constrainedGeometry(m_pressGeometry);
    int left = pressGeometry.left();
    int top = pressGeometry.top();
    int right = pressGeometry.x() + pressGeometry.width();
    int bottom = pressGeometry.y() + pressGeometry.height();
    const QSize minimum = minimumFrameSize(m_content, bounds);
    const int minW = minimum.width();
    const int minH = minimum.height();

    if (m_resizeEdges & LeftEdge) {
        left = std::clamp(pressGeometry.left() + delta.x(),
                          bounds.left(), right - minW);
    }
    if (m_resizeEdges & RightEdge) {
        right = std::clamp(pressGeometry.x() +
                               pressGeometry.width() + delta.x(),
                           left + minW, bounds.x() + bounds.width());
    }
    if (m_resizeEdges & TopEdge) {
        const int latestTitleTop = bounds.y() + bounds.height() -
                                   std::min(titleHeight(m_content) + 2, bounds.height());
        top = std::clamp(pressGeometry.top() + delta.y(),
                         std::max(bounds.top(), bottom - bounds.height()),
                         std::min(bottom - minH, latestTitleTop));
    }
    if (m_resizeEdges & BottomEdge) {
        bottom = std::clamp(pressGeometry.y() +
                                pressGeometry.height() + delta.y(),
                            top + minH, top + bounds.height());
    }
    return QRect(left, top, right - left, bottom - top);
}

void InternalEditorFrame::queueInteractiveResize(const QRect& geometry) {
    if (!m_resizing || !geometry.isValid()) return;
    m_pendingResizeGeometry = geometry;
    if (!m_resizeApplyTimer.isActive()) m_resizeApplyTimer.start();
}

void InternalEditorFrame::applyPendingInteractiveResize() {
    if (!m_pendingResizeGeometry.isValid()) return;
    const QRect geometry = m_pendingResizeGeometry;
    m_pendingResizeGeometry = QRect();
    if (this->geometry() != geometry) setGeometry(geometry);
}

void InternalEditorFrame::cancelPendingInteractiveResize() {
    m_resizeApplyTimer.stop();
    m_pendingResizeGeometry = QRect();
}

bool InternalEditorFrame::checkPlacementForTest() {
    const QString key = QStringLiteral("tests/editorPlacement/") +
                        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings settings;
    const QRect legacy(70, 80, 620, 380);
    settings.setValue(key + "/geometry", legacy);
    bool ok = true;
    const auto check = [&ok](bool condition, const char* message) {
        if (!condition) std::fprintf(stderr, "editor placement: %s\n", message);
        ok &= condition;
    };
    QWidget host;
    host.resize(1000, 700);
    QWidget body(&host);
    body.setGeometry(0, 120, 1000, 580);
    host.show();
    QWidget overlayHost(&host);
    overlayHost.setGeometry(host.rect());
    overlayHost.show();
    {
        InternalEditorFrame nativeFrame(key + QStringLiteral("/native"),
                                        &overlayHost);
        nativeFrame.prepareForNativeSurface();
        nativeFrame.setContent(new QWidget);
        nativeFrame.present();
        QApplication::processEvents();
        check(nativeFrame.testAttribute(Qt::WA_NativeWindow) &&
                  nativeFrame.property("vlt.nativeOverlay").toBool() &&
                  !overlayHost.testAttribute(Qt::WA_NativeWindow),
              "a native plugin frame does not promote the shared workspace");
        const auto chromeGeometry = [&] {
            return QRect(nativeFrame.parentWidget()->mapTo(nativeFrame.nativeParentWidget(), nativeFrame.pos()), nativeFrame.size());
        };
        check(nativeFrame.m_nativeChrome && nativeFrame.m_nativeChrome->isVisible() &&
                  nativeFrame.m_nativeChrome->geometry() == chromeGeometry() &&
                  nativeFrame.m_nativeChrome->mask().isEmpty() &&
                  nativeFrame.m_nativeChrome->requestedFormat().hasAlpha() &&
                  nativeFrame.m_nativeChrome->flags().testFlag(Qt::WindowTransparentForInput),
              "native editor has an alpha, input-transparent native fringe");
        nativeFrame.moveWithinWorkspace(QPoint(35, 45));
        check(nativeFrame.m_nativeChrome->geometry() == chromeGeometry(),
              "native editor fringe moves without a workspace presentation turn");
        nativeFrame.hide();
        check(!nativeFrame.m_nativeChrome->isVisible(), "hiding a native editor also hides its fringe");
    }
    overlayHost.hide();
    // Opt-in hardware check: compare the exterior against QPainter coverage,
    // including the fringe below a native child. Keep it outside offscreen runs.
    if (qEnvironmentVariableIntValue("DAW_EDITOR_GPU_CHECK")) {
        const QColor backdrop(17, 133, 231);
        auto palette = host.palette(); palette.setColor(QPalette::Window, backdrop);
        host.setPalette(palette); host.setAutoFillBackground(true);
        host.raise(); host.activateWindow();
        ui::graphics::WorkspaceSurface surface(&host);
        bool failed = false;
        connect(&surface, &ui::graphics::WorkspaceSurface::failed, &host,
                [&failed](const QString&) { failed = true; });
        const auto settle = [] {
            QEventLoop loop; QTimer::singleShot(200, &loop, &QEventLoop::quit); loop.exec();
        };
        for (const bool native : {false, true}) {
            InternalEditorFrame frame(key + (native ? "/gpu-native" : "/gpu"), &host);
            auto* content = new QWidget;
            content->setProperty("vlt.pluginChrome", true);
            content->setStyleSheet("background: #dc3050;");
            frame.setContent(content);
            if (native) frame.prepareForNativeSurface();
            frame.present();
            for (const QSize size : {QSize(440, 310), QSize(520, 360)}) {
                frame.resize(size); frame.moveWithinWorkspace(QPoint(60, 70));
                settle();
                if (native) {
                    // A pure move must preserve the cached native pixels, and
                    // a workspace resize must raise the fringe with the view.
                    frame.moveWithinWorkspace(QPoint(110, 95));
                    host.resize(host.width() + 1, host.height());
                    settle();
                }
                const QImage actual = native
                    ? host.screen()->grabWindow(0,
                        host.mapToGlobal(QPoint()).x() - host.screen()->geometry().x(),
                        host.mapToGlobal(QPoint()).y() - host.screen()->geometry().y(),
                        host.width(), host.height()).toImage()
                    : surface.quickWindow()->grabWindow();
                check(!failed && !actual.isNull() && surface.quickWindow()->isExposed(),
                      "rounded editor reaches the hardware compositor");
                if (actual.isNull()) continue;
                const qreal dpr = actual.devicePixelRatio();
                QImage expected(frame.size() * dpr, QImage::Format_ARGB32_Premultiplied);
                expected.setDevicePixelRatio(dpr); expected.fill(Qt::transparent);
                { QPainter painter(&expected); frame.paintChrome(painter); }
                const QColor rim = expected.pixelColor(qRound(30 * dpr), 0);
                const QColor shell = expected.pixelColor(expected.rect().center());
                int smoothCorners = 0;
                bool coverageMatches = true;
                const QPoint origin(qRound(frame.x() * dpr), qRound(frame.y() * dpr));
                if (native)
                    check(actual.pixelColor(origin + QPoint(qRound(100 * dpr), qRound(100 * dpr))) == QColor("#dc3050"),
                          "native content survives movement and workspace restacking");
                for (const bool right : {false, true}) for (const bool bottom : {false, true}) {
                    int smoothPixels = 0;
                    for (int y = 0; y < qCeil(10 * dpr); ++y)
                        for (int x = 0; x < qCeil(10 * dpr); ++x) {
                            const qreal distance = std::hypot(10 - (x + .5) / dpr, 10 - (y + .5) / dpr);
                            if (distance < 10 - 1 / dpr) continue;
                            const QPoint point(right ? expected.width() - 1 - x : x,
                                               bottom ? expected.height() - 1 - y : y);
                            const QColor pixel = actual.pixelColor(origin + point);
                            int contrast = 0;
                            qreal coverage = 0;
                            for (const auto channel : {&QColor::red, &QColor::green, &QColor::blue}) {
                                const int value = (pixel.*channel)();
                                const int background = (backdrop.*channel)();
                                const int edge = (rim.*channel)();
                                const int body = (shell.*channel)();
                                const int delta = edge - background;
                                if (std::abs(delta) > contrast) {
                                    contrast = std::abs(delta);
                                    coverage = qreal(value - background) / delta;
                                }
                                coverageMatches &= value >= std::min({background, edge, body}) - 3 &&
                                                   value <= std::max({background, edge, body}) + 3;
                                if (distance > 10 + 1 / dpr)
                                    coverageMatches &= std::abs(value - background) <= 2;
                            }
                            smoothPixels += coverage > .05 && coverage < .85;
                        }
                    smoothCorners += smoothPixels >= 2;
                }
                // MSAA and raster coverage differ. Require actual blending at
                // every corner, no artwork in the fringe and no paint outside
                // the circle, rather than requiring identical edge samples.
                if (!coverageMatches || smoothCorners != 4)
                    std::fprintf(stderr, "editor GPU corner native=%d DPR=%.2f coverage=%d smoothCorners=%d\n",
                                 native, double(dpr), coverageMatches, smoothCorners);
                check(coverageMatches && smoothCorners == 4,
                      "GPU editor corners retain smooth coverage after movement and resize");
                const auto shot = qEnvironmentVariable("DAW_EDITOR_CHECK_SCREENSHOT");
                if (!shot.isEmpty()) actual.save(shot + QString("-gpu-%1-%2.png").arg(native).arg(size.width()));
            }
        }
        settings.remove(key);
        return ok;
    }
    {
        InternalEditorFrame rounded(key + QStringLiteral("/corners"), &host);
        auto* content = new QWidget;
        content->setStyleSheet(QStringLiteral("background: #dc3050;"));
        rounded.setContent(content);
        rounded.present();
        for (const QSize size : {QSize(520, 320), QSize(640, 410)}) {
            rounded.resize(size);
            QApplication::processEvents();
            QImage image(rounded.size() * rounded.devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(rounded.devicePixelRatioF());
            image.fill(Qt::transparent);
            rounded.render(&image);
            const QString screenshot = qEnvironmentVariable("DAW_EDITOR_CHECK_SCREENSHOT");
            if (!screenshot.isEmpty())
                image.save(screenshot + QStringLiteral("-%1x%2.png")
                    .arg(size.width()).arg(size.height()));
            check(rounded.mask().isEmpty(), "editor exterior is not quantized to a binary mask");
            check(!rounded.testAttribute(Qt::WA_OpaquePaintEvent),
                  "editor preserves partial coverage at its exterior");
            check(rounded.m_closeButton->visibleRegion() == QRegion(rounded.m_closeButton->rect()),
                  "the complete close button stays visible inside the rounded header");
            for (const QPoint point : {image.rect().topLeft(), image.rect().topRight(),
                                       image.rect().bottomLeft(), image.rect().bottomRight()})
                check(image.pixelColor(point).alpha() == 0,
                      "rounded editor clips all four corners and child artwork");
            check(image.pixelColor(image.rect().center()) == QColor("#dc3050"),
                  "rounded editor preserves its content");
            for (const bool right : {false, true}) for (const bool bottom : {false, true}) {
                bool antialiased = false;
                const int reach = qCeil(kCornerRadius * image.devicePixelRatio());
                for (int y = 0; y < reach; ++y) for (int x = 0; x < reach; ++x) {
                    const int alpha = image.pixelColor(right ? image.width() - 1 - x : x,
                        bottom ? image.height() - 1 - y : y).alpha();
                    antialiased |= alpha > 0 && alpha < 255;
                }
                check(antialiased, "all four editor corners retain subpixel coverage");
            }
        }
    }
    for (const bool equalizer : {false, true}) {
        InternalEditorFrame plugin(key + (equalizer ? "/eq-shell" : "/plugin-shell"), &host);
        auto* content = new QWidget;
        content->setProperty("vlt.pluginChrome", true);
        content->setProperty("vlt.eqChrome", equalizer);
        content->setWindowTitle("Reverb test editor");
        content->setStyleSheet("background: #dc3050;");
        plugin.setContent(content);
        plugin.setExpansionEnabled(false);
        plugin.present();
        plugin.resize(440, 310);
        QApplication::processEvents();
        check(plugin.m_title->text().isEmpty() && plugin.windowTitle() == "Reverb test editor" &&
                  plugin.accessibleName() == "Reverb test editor", "plugin title is semantic only");
        check(plugin.m_titleBar->height() == 26 && plugin.m_closeButton->size() == QSize(24, 24),
              "plugin drag strip and close hit target follow shell metrics");
        check(plugin.m_column->contentsMargins() == QMargins(1, 1, 1, 1), "EQ shares continuous plugin border inset");
        check(!content->mask().isEmpty(), "plugin artwork leaves the curved contour visible");
        check(plugin.mask().isEmpty(), "plugin exterior retains antialiasing");
        check(plugin.m_closeButton->visibleRegion() == QRegion(plugin.m_closeButton->rect()), "plugin close target remains unclipped");
        check(plugin.width() - (plugin.m_closeButton->mapTo(&plugin, QPoint()).x() +
                  plugin.m_closeButton->width()) == 5, "plugin close target sits four pixels inside the contour");
        content->setWindowTitle("Renamed plugin");
        check(plugin.m_title->text().isEmpty() && plugin.accessibleName() == "Renamed plugin", "plugin semantic title follows renaming");
        QImage image(plugin.size() * plugin.devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(plugin.devicePixelRatioF());image.fill(Qt::transparent);plugin.render(&image);
        const double dpr = image.devicePixelRatio();
        check(image.pixelColor(int(220*dpr), image.height() - 1) != QColor("#dc3050"), "content does not paint over lower plugin contour");
        const QPointF closeCenter = QPointF(plugin.m_closeButton->mapTo(&plugin, QPoint())) + QPointF(12, 12);
        const QColor backdrop = image.pixelColor(qRound((closeCenter.x() - 8) * dpr), qRound(closeCenter.y() * dpr));
        for (const QPointF tip : {QPointF(-3.5, -3.5), QPointF(3.5, -3.5), QPointF(-3.5, 3.5), QPointF(3.5, 3.5)}) {
            const QPointF at = (closeCenter + tip) * dpr;
            bool inked = false;
            const int reach = int(std::ceil(dpr));
            for (int y = qRound(at.y()) - reach; y <= qRound(at.y()) + reach; ++y)
                for (int x = qRound(at.x()) - reach; x <= qRound(at.x()) + reach; ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    inked |= std::abs(pixel.red() - backdrop.red()) + std::abs(pixel.green() - backdrop.green()) +
                        std::abs(pixel.blue() - backdrop.blue()) > 90;
                }
            check(inked, "all four close-glyph tips remain visible in the plugin frame");
        }
    }
    {
        InternalEditorFrame first(key + "/first", &host);
        InternalEditorFrame second(key + "/second", &host);
        first.setContent(new QWidget);
        second.setContent(new QWidget);
        first.present();
        second.present();
        QApplication::processEvents();
        const auto mouse = [](QWidget* target, QEvent::Type type,
                              QPoint global, Qt::MouseButton button,
                              Qt::MouseButtons buttons) {
            QMouseEvent event(type, target->mapFromGlobal(global), global,
                              button, buttons, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
        };
        // Both installation orders, all edges/corners, including release before
        // the resize timer fires. The overlapping peer must never move or resize.
        for (InternalEditorFrame* target : {&first, &second}) {
            InternalEditorFrame* peer = target == &first ? &second : &first;
            for (std::size_t i = 0; i < kHandleSpecs.size(); ++i) {
                const QRect start(80, 150, 620, 380);
                target->setGeometry(start);
                peer->setGeometry(240, 180, 640, 400);
                peer->activateEditor();
                const QRect peerBefore = peer->geometry();
                QWidget* handle = target->m_resizeHandles[i];
                const QPoint origin = handle->mapToGlobal(handle->rect().center());
                const QPoint delta(24, 18);
                mouse(handle, QEvent::MouseButtonPress, origin,
                      Qt::LeftButton, Qt::LeftButton);
                mouse(handle, QEvent::MouseMove, origin + delta,
                      Qt::NoButton, Qt::LeftButton);
                mouse(handle, QEvent::MouseButtonRelease, origin + delta,
                      Qt::LeftButton, Qt::NoButton);
                QRect expected = start;
                const int edges = kHandleSpecs[i].edges;
                if (edges & LeftEdge) expected.setLeft(start.left() + delta.x());
                if (edges & RightEdge) expected.setRight(start.right() + delta.x());
                if (edges & TopEdge) expected.setTop(start.top() + delta.y());
                if (edges & BottomEdge) expected.setBottom(start.bottom() + delta.y());
                check(target->geometry() == expected && peer->geometry() == peerBefore,
                      "only the owner resizes, with the opposite edges anchored");
                check(!target->m_resizing && !peer->m_resizing &&
                          !target->m_resizeApplyTimer.isActive(),
                      "release commits the final resize without a delayed jump");
            }
        }
        first.setGeometry(80, 150, 620, 380);
        QApplication::processEvents();
        for (QWidget* button : {first.m_detachButton, first.m_maximizeButton,
                               first.m_closeButton}) {
            const QPoint topRight = button->mapTo(&first, QPoint(button->width() - 1, 0));
            check(first.childAt(topRight) == button,
                  "resize bands do not steal compact title button clicks");
        }
        QWidget* title = first.m_titleBar;
        const QPoint origin = title->mapToGlobal(QPoint(80, kTitleHeight / 2));
        const QRect beforeDrag = first.geometry();
        mouse(title, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        mouse(title, QEvent::MouseMove, origin + QPoint(40, 30),
              Qt::NoButton, Qt::LeftButton);
        mouse(title, QEvent::MouseMove, origin + QPoint(1, 1),
              Qt::NoButton, Qt::LeftButton);
        check(first.pos() == beforeDrag.topLeft() + QPoint(1, 1),
              "title drag reverses continuously through the original grab point");
        mouse(title, QEvent::MouseButtonRelease, origin,
              Qt::RightButton, Qt::LeftButton);
        check(first.m_dragging, "releasing another button does not end a title drag");
        mouse(title, QEvent::MouseButtonRelease, origin,
              Qt::LeftButton, Qt::NoButton);
        check(first.geometry() == beforeDrag && !first.m_dragging,
              "title release applies its final position");

        mouse(title, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
        mouse(title, QEvent::MouseMove, origin + QPoint(20, 10), Qt::NoButton, Qt::LeftButton);
        // Opening a plugin can asynchronously grow and clamp its frame while
        // the title is already held. The next sample starts at that live frame.
        first.resizeForContent(QSize(950, 500));
        const QPoint loadedPosition = first.pos();
        mouse(title, QEvent::MouseMove, origin + QPoint(21, 11), Qt::NoButton, Qt::LeftButton);
        check(first.pos() == first.constrainedGeometry(QRect(loadedPosition + QPoint(1, 1), first.size())).topLeft(),
              "editor loading during a title drag preserves one-pixel tracking");
        mouse(title, QEvent::MouseMove, origin + QPoint(-2000, 11), Qt::NoButton, Qt::LeftButton);
        const QPoint edgePosition = first.pos();
        mouse(title, QEvent::MouseMove, origin + QPoint(-1999, 11), Qt::NoButton, Qt::LeftButton);
        check(first.pos() == edgePosition + QPoint(1, 0),
              "title drag reverses immediately after reaching a workspace edge");
        mouse(title, QEvent::MouseButtonRelease, origin + QPoint(-1999, 11), Qt::LeftButton, Qt::NoButton);

        QWidget* handle = first.m_resizeHandles[7];
        const QPoint corner = handle->mapToGlobal(handle->rect().center());
        mouse(handle, QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
        mouse(handle, QEvent::MouseMove, corner + QPoint(30, 30),
              Qt::NoButton, Qt::LeftButton);
        QEvent ungrab(QEvent::UngrabMouse);
        QApplication::sendEvent(handle, &ungrab);
        check(!first.m_resizing && !first.m_resizeApplyTimer.isActive() &&
                  !first.m_pendingResizeGeometry.isValid(),
              "losing the mouse grab cancels any delayed resize");
        first.hide();
        second.hide();
    }
    {
        QWidget smallHost;
        smallHost.resize(1000, 700);
        smallHost.show();
        InternalEditorFrame limited(key + "/minimum", &smallHost);
        auto* content = new QWidget;
        content->setMinimumSize(860, 558);
        limited.setContent(content);
        limited.present();
        QApplication::processEvents();
        const auto shrink = [&limited](bool growMinimum) {
            QWidget* handle = limited.m_resizeHandles[7];
            const QPoint origin = handle->mapToGlobal(handle->rect().center());
            QMouseEvent press(QEvent::MouseButtonPress, handle->mapFromGlobal(origin), origin,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &press);
            if (growMinimum) limited.content()->setMinimumSize(900, 600);
            const QPoint end = origin - QPoint(10000, 10000);
            QMouseEvent release(QEvent::MouseButtonRelease, handle->mapFromGlobal(end), end,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(handle, &release);
            QApplication::processEvents();
        };
        shrink(false);
        check(content->minimumSize() == QSize(860, 558) && limited.size() == QSize(862, 582) &&
                  limited.rect().contains(content->geometry()),
              "interactive resizing preserves the editor minimum plus title and borders");
        shrink(true);
        check(limited.size() == QSize(902, 624) && limited.rect().contains(content->geometry()),
              "an editor minimum raised during a drag keeps resize limits valid");
        smallHost.resize(350, 260);
        QApplication::processEvents();
        check(limited.width() <= smallHost.width() && limited.height() <= smallHost.height() &&
                  smallHost.rect().contains(QRect(limited.m_titleBar->mapTo(&smallHost, QPoint()),
                                                    limited.m_titleBar->size())),
              "a content minimum larger than the workspace still leaves the title recoverable");
    }
    InternalEditorFrame frame(key, &host);
    frame.setWorkspaceArea(&body);
    auto* content = new QWidget;
    if (QGuiApplication::platformName() != QLatin1String("offscreen") &&
        QGuiApplication::platformName() != QLatin1String("minimal"))
        content->setAttribute(Qt::WA_NativeWindow);
    frame.setContent(content);
    frame.present();
    QApplication::processEvents();
    check(frame.geometry() == legacy.translated(body.pos()), "legacy body coordinates migrate without a visual jump");
    const auto drag = [](InternalEditorFrame& window, QPoint delta) {
        QWidget* title = window.m_titleBar;
        const QPoint grab(80, kTitleHeight / 2);
        const QPoint origin = title->mapToGlobal(grab);
        QMouseEvent press(QEvent::MouseButtonPress, grab, origin,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(title, &press);
        QMouseEvent move(QEvent::MouseMove, grab + delta, origin + delta,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(title, &move);
        QMouseEvent release(QEvent::MouseButtonRelease,
                            title->mapFromGlobal(origin + delta), origin + delta,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(title, &release);
    };
    const auto titleVisible = [&host](const InternalEditorFrame& window) {
        return host.rect().contains(QRect(window.m_titleBar->mapTo(&host, QPoint()),
                                           window.m_titleBar->size()));
    };
    const QSize originalSize = frame.size();
    drag(frame, QPoint(0, -10000));
    const QPoint hit = frame.m_titleBar->mapTo(&host, QPoint(80, 18));
    QWidget* hitWidget = host.childAt(hit);
    check(frame.y() == 0 && frame.size() == originalSize && titleVisible(frame) &&
          hitWidget && frame.isAncestorOf(hitWidget), "title can cover and receive input above the workspace header");
    drag(frame, QPoint(0, 10000));
    check(frame.geometry().bottom() > host.height() && titleVisible(frame) &&
          frame.size() == originalSize && frame.y() == host.height() - kTitleHeight - 2,
          "body can disappear below the viewport while the full title stays visible");
    const QRect parked = frame.geometry();
    frame.setMaximized(true);
    check(frame.geometry() == body.geometry(), "maximize still fits the editing body");
    frame.setMaximized(false);
    check(frame.geometry() == parked, "unmaximize restores the parked position");
    // Resizing a parked editor must never feed inverted limits to std::clamp.
    frame.m_pressGeometry = parked;
    frame.m_pressGlobal = QPoint();
    frame.m_resizeEdges = BottomEdge;
    const QRect taller = frame.interactiveResizeGeometry(QPoint(0, 10000));
    check(taller.top() == parked.top() && taller.height() <= host.height() &&
          taller.height() >= kMinimumHeight, "bottom resize remains valid below the viewport");
    frame.m_resizeEdges = TopEdge;
    const QRect top = frame.interactiveResizeGeometry(QPoint(0, 10000));
    check(top.top() <= host.height() - kTitleHeight - 2 && top.height() >= kMinimumHeight,
          "top resize never loses the title");
    frame.m_resizeEdges = NoEdge;
    drag(frame, QPoint(0, 250 - frame.y()));
    check(frame.y() == 250, "parked windows can be brought back with their title");
    const QRect docked = frame.geometry();
    const auto savedPlacement = settings.value(key + "/geometry");
    frame.setDetached(true);
    QApplication::processEvents();
    check(frame.isWindow() && frame.content() == content && content->parentWidget() == &frame,
          "detaching preserves the live editor and its command state");
    if (ui::graphics::gpuWorkspaceEnabled()) {
        QEventLoop frameReady;
        QTimer::singleShot(300, &frameReady, &QEventLoop::quit);
        frameReady.exec();
        check(frame.m_detachedSurface && !frame.m_detachedSurface->quickWindow()->grabWindow().isNull(),
              "the detached editor presents through its own hardware scene");
    }
    frame.setMaximized(true); frame.setMaximized(false);
    check(settings.value(key + "/geometry") == savedPlacement,
          "desktop coordinates do not overwrite the workspace placement");
    frame.setDetached(false);
    QApplication::processEvents();
    check(!frame.isWindow() && frame.geometry() == docked && frame.content() == content,
          "docking restores placement without recreating editor content");
    frame.hide();
    {
        InternalEditorFrame reopened(key, &host);
        reopened.setWorkspaceArea(&body);
        reopened.setContent(new QWidget);
        reopened.present();
        QApplication::processEvents();
        check(reopened.y() == 250 && settings.value(key + "/placementVersion").toInt() == 2,
              "saved full-workspace coordinates are not migrated twice");
        drag(reopened, QPoint(0, 10000));
        host.resize(350, 260);
        body.setGeometry(0, 80, 350, 180);
        QApplication::processEvents();
        check(titleVisible(reopened) && reopened.width() <= host.width(),
              "shrinking the host keeps the title accessible below the old minimum size");
        reopened.setMaximized(true);
        check(reopened.geometry() == body.geometry(), "maximize fits a short workspace");
        reopened.setMaximized(false);
        reopened.hide();
        host.resize(280, 200);
        body.setGeometry(0, 80, 280, 120);
        QApplication::processEvents();
        reopened.present();
        QApplication::processEvents();
        check(titleVisible(reopened) && reopened.width() <= host.width(),
              "a hidden window is recovered when reopened in a smaller host");
    }
    settings.remove(key);
    return ok;
}
