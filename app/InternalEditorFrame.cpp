#include "InternalEditorFrame.hpp"

#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>
#include <cstdio>

#include <algorithm>
#include <utility>

namespace {

constexpr int kTitleHeight = 36;
constexpr int kResizeBand = 6;
constexpr int kCornerBand = 12;
constexpr int kMinimumWidth = 420;
constexpr int kMinimumHeight = 300;
constexpr int kInteractiveResizeFrameMs = 16;

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
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_column = new QVBoxLayout(this);
    m_column->setContentsMargins(1, 1, 1, 1);
    m_column->setSpacing(0);

    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName(QStringLiteral("InternalEditorTitleBar"));
    m_titleBar->setFixedHeight(kTitleHeight);
    m_titleBar->setCursor(Qt::OpenHandCursor);
    m_titleBar->installEventFilter(this);
    auto* titleRow = new QHBoxLayout(m_titleBar);
    titleRow->setContentsMargins(10, 2, 3, 2);
    titleRow->setSpacing(4);

    m_title = new QLabel(tr("Editor"), m_titleBar);
    m_title->setObjectName(QStringLiteral("InternalEditorTitle"));
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    titleRow->addWidget(m_title, 1);

    m_maximizeButton = new ui::IconButton(
        icons::Glyph::WindowMaximize, tr("Maximize editor"), m_titleBar);
    m_maximizeButton->setObjectName(QStringLiteral("InternalEditorMaximize"));
    m_maximizeButton->setButtonSize(32, 30);
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
    m_closeButton->setButtonSize(32, 30);
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
        m_content->setParent(nullptr);
    }

    m_content = content;
    content->setParent(this);
    m_preferredContentSize = content->size().expandedTo(QSize(640, 360));
    content->setMinimumSize(0, 0);
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_column->addWidget(content, 1);
    m_title->setText(content->windowTitle().isEmpty()
                         ? tr("Editor")
                         : content->windowTitle());
    connect(content, &QWidget::windowTitleChanged, m_title, &QLabel::setText);
    connect(content, &QObject::destroyed, this, [this] {
        m_content = nullptr;
        m_lastContentFocus = nullptr;
    });
}

void InternalEditorFrame::setAccessoryWidget(QWidget* accessory) {
    m_accessory = accessory;
}

void InternalEditorFrame::setWorkspaceArea(QWidget* area) {
    m_workspaceArea = area;
    if (m_placementRestored) constrainToParent();
}

void InternalEditorFrame::present() {
    if (!m_placementRestored) restorePlacement();
    if (m_content) m_content->show();
    show();
    raise();
    activateEditor();
    restoreContentFocus();
}

void InternalEditorFrame::activateEditor() {
    if (!isVisible()) return;
    installApplicationEventFilter();
    raise();
    setEditorActive(true);
}

void InternalEditorFrame::setMaximized(bool maximized) {
    if (!m_placementRestored) restorePlacement();
    if (m_maximized == maximized) return;
    cancelPendingInteractiveResize();
    m_resizing = false;
    m_resizeEdges = NoEdge;
    if (maximized) m_restoreGeometry = geometry();
    m_maximized = maximized;
    const QRect limits = m_maximized ? workspaceRect() : availableRect();
    setMinimumSize(std::min(kMinimumWidth, limits.width()),
                   std::min(kMinimumHeight, limits.height()));
    setGeometry(maximized ? workspaceRect()
                          : constrainedGeometry(m_restoreGeometry));
    updateMaximizeButton();
    updateResizeHandles();
    savePlacement();
    activateEditor();
}

void InternalEditorFrame::resizeForContent(const QSize& contentSize) {
    if (!contentSize.isValid()) return;
    if (!m_placementRestored) restorePlacement();
    m_preferredContentSize = contentSize;
    if (m_maximized) return;

    const QSize chrome(2, kTitleHeight + 2);
    QRect wanted(geometry().topLeft(), contentSize + chrome);
    QRect constrained = constrainedGeometry(wanted);
    // A plugin opening its parameter dock should stay fully visible if it was
    // fully visible before. An intentionally parked editor keeps its title in
    // place when the plugin requests another content size.
    const QRect bounds = availableRect();
    if (bounds.contains(geometry()) && constrained.bottom() > bounds.bottom())
        constrained.moveBottom(bounds.bottom());
    setGeometry(constrained);
    m_restoreGeometry = constrained;
    updateResizeHandles();
    savePlacement();
}

QSize InternalEditorFrame::maximumContentSize() const {
    const QSize chrome(2, kTitleHeight + 2);
    const QSize outer = availableRect().size();
    return QSize(std::max(1, outer.width() - chrome.width()),
                 std::max(1, outer.height() - chrome.height()));
}

bool InternalEditorFrame::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == parentWidget() && event->type() == QEvent::Resize) ||
        (watched == m_workspaceArea &&
         (event->type() == QEvent::Resize || event->type() == QEvent::Move))) {
        constrainToParent();
    }

    if (watched == m_titleBar) {
        auto* mouse = dynamic_cast<QMouseEvent*>(event);
        if (mouse && event->type() == QEvent::MouseButtonDblClick &&
            mouse->button() == Qt::LeftButton) {
            setMaximized(!m_maximized);
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
                m_titleBar->setCursor(Qt::ClosedHandCursor);
            }
            mouse->accept();
            return true;
        }
        if (mouse && event->type() == QEvent::MouseMove && m_dragging &&
            (mouse->buttons() & Qt::LeftButton)) {
            const QPoint delta = mouse->globalPosition().toPoint() - m_pressGlobal;
            if (delta.manhattanLength() < QApplication::startDragDistance()) {
                mouse->accept();
                return true;
            }
            setGeometry(constrainedGeometry(m_pressGeometry.translated(delta)));
            mouse->accept();
            return true;
        }
        if (mouse && event->type() == QEvent::MouseButtonRelease && m_dragging) {
            m_dragging = false;
            m_titleBar->setCursor(Qt::OpenHandCursor);
            m_restoreGeometry = geometry();
            savePlacement();
            mouse->accept();
            return true;
        }
    }

    if (auto* handle = qobject_cast<QWidget*>(watched);
        handle && handle->property("edges").isValid()) {
        auto* mouse = dynamic_cast<QMouseEvent*>(event);
        if (mouse && event->type() == QEvent::MouseButtonPress &&
            mouse->button() == Qt::LeftButton && !m_maximized) {
            activateEditor();
            cancelPendingInteractiveResize();
            m_resizing = true;
            m_resizeEdges = handle->property("edges").toInt();
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
        if (mouse && event->type() == QEvent::MouseButtonRelease && m_resizing) {
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

    auto* target = qobject_cast<QWidget*>(watched);
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
    // A hidden frame must not remain on QApplication's global filter list, and
    // no delayed resize is allowed to wake it after it has left the workspace.
    uninstallApplicationEventFilter();
    cancelPendingInteractiveResize();
    m_resizing = false;
    m_resizeEdges = NoEdge;
    m_dragging = false;
    if (m_titleBar && !m_maximized)
        m_titleBar->setCursor(Qt::OpenHandCursor);
    setEditorActive(false);
    QFrame::hideEvent(event);
}

void InternalEditorFrame::paintEvent(QPaintEvent*) {
    const Theme& theme = ThemeManager::instance().theme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF frameRect = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.surface);
    painter.drawRoundedRect(frameRect, 8.0, 8.0);

    if (m_titleBar) {
        painter.setBrush(theme.toolbarBackground);
        painter.drawRoundedRect(QRectF(m_titleBar->geometry()), 7.0, 7.0);
        painter.drawRect(m_titleBar->geometry().adjusted(0, 7, 0, 0));
        painter.setPen(theme.separator());
        painter.drawLine(m_titleBar->geometry().bottomLeft(),
                         m_titleBar->geometry().bottomRight());
    }

    painter.setBrush(Qt::NoBrush);
    // Focus remains visible through the title text and button states. The
    // contour itself is deliberately neutral so plugin artwork is not boxed
    // in by the application's accent colour.
    painter.setPen(QPen(QColor(0, 0, 0), 1.0));
    painter.drawRoundedRect(frameRect, 8.0, 8.0);
}

void InternalEditorFrame::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    updateResizeHandles();
}

void InternalEditorFrame::showEvent(QShowEvent* event) {
    // While hidden the parent may have changed size. Re-constrain before the
    // first exposed paint, then opt into the global focus/activation stream.
    if (m_placementRestored) constrainToParent();
    installApplicationEventFilter();
    QFrame::showEvent(event);
}

void InternalEditorFrame::applyTheme() {
    if (m_title) {
        QPalette palette = m_title->palette();
        const Theme& theme = ThemeManager::instance().theme();
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
    m_maximized = settings.value(m_settingsKey + "/maximized", false).toBool();
    const QRect movementBounds = m_maximized ? workspaceRect() : availableRect();
    setMinimumSize(std::min(kMinimumWidth, movementBounds.width()),
                   std::min(kMinimumHeight, movementBounds.height()));

    if (saved.isValid()) {
        m_restoreGeometry = constrainedGeometry(saved);
    } else {
        const QRect bounds = workspaceRect();
        const QSize chrome(2, kTitleHeight + 2);
        QSize wanted = m_preferredContentSize + chrome;
        wanted.setWidth(std::clamp(wanted.width(),
                                   std::min(kMinimumWidth, bounds.width()),
                                   bounds.width()));
        wanted.setHeight(std::clamp(wanted.height(),
                                    std::min(kMinimumHeight, bounds.height()),
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
    if (!m_placementRestored || m_settingsKey.isEmpty()) return;
    QSettings settings;
    settings.setValue(m_settingsKey + "/geometry", m_restoreGeometry);
    settings.setValue(m_settingsKey + "/maximized", m_maximized);
    settings.setValue(m_settingsKey + "/placementVersion", 2);
}

void InternalEditorFrame::constrainToParent() {
    if (!m_placementRestored) return;
    const QRect bounds = m_maximized ? workspaceRect() : availableRect();
    setMinimumSize(std::min(kMinimumWidth, bounds.width()),
                   std::min(kMinimumHeight, bounds.height()));
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
    if (!m_workspaceArea || !parentWidget()) return bounds;
    const QRect body(m_workspaceArea->mapTo(parentWidget(), QPoint()),
                     m_workspaceArea->size());
    const QRect visible = bounds.intersected(body);
    return visible.isEmpty() ? bounds : visible;
}

QRect InternalEditorFrame::constrainedGeometry(const QRect& wanted) const {
    const QRect bounds = availableRect();
    const int minW = std::min(kMinimumWidth, bounds.width());
    const int minH = std::min(kMinimumHeight, bounds.height());
    const int width = std::clamp(wanted.width(), minW, bounds.width());
    const int height = std::clamp(wanted.height(), minH, bounds.height());
    const int maxX = bounds.x() + bounds.width() - width;
    // The entire title bar (including its buttons) remains recoverable. Only
    // the body may disappear below the application viewport.
    const int visibleTitleHeight = std::min(kTitleHeight + 2, bounds.height());
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
                                          : Qt::OpenHandCursor);
}

void InternalEditorFrame::restoreContentFocus() {
    QTimer::singleShot(0, this, [this] {
        if (!isVisible() || !m_content) return;
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
    int left = m_pressGeometry.left();
    int top = m_pressGeometry.top();
    int right = m_pressGeometry.x() + m_pressGeometry.width();
    int bottom = m_pressGeometry.y() + m_pressGeometry.height();
    const int minW = std::min(kMinimumWidth, bounds.width());
    const int minH = std::min(kMinimumHeight, bounds.height());

    if (m_resizeEdges & LeftEdge) {
        left = std::clamp(m_pressGeometry.left() + delta.x(),
                          bounds.left(), right - minW);
    }
    if (m_resizeEdges & RightEdge) {
        right = std::clamp(m_pressGeometry.x() +
                               m_pressGeometry.width() + delta.x(),
                           left + minW, bounds.x() + bounds.width());
    }
    if (m_resizeEdges & TopEdge) {
        const int latestTitleTop = bounds.y() + bounds.height() -
                                   std::min(kTitleHeight + 2, bounds.height());
        top = std::clamp(m_pressGeometry.top() + delta.y(),
                         std::max(bounds.top(), bottom - bounds.height()),
                         std::min(bottom - minH, latestTitleTop));
    }
    if (m_resizeEdges & BottomEdge) {
        bottom = std::clamp(m_pressGeometry.y() +
                                m_pressGeometry.height() + delta.y(),
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
