#include "TransportBar.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "ThemeMediaBackground.hpp"
#include "TimelineBackgroundPrefs.hpp"
#include "UiConstants.hpp"

#include <QSettings>

#include <algorithm>

#include "EngineController.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace {

// One quiet console, with larger hit targets around a labelled readout.
constexpr int kBlockHeight = 44;
constexpr int kButtonSize = 32;
constexpr int kPositionFontPx = 21;
constexpr int kStatsFontPx = 15;
constexpr int kChipFontPx = 12;
constexpr int kCompactClusterGap = 8;

int compactPositionWidth() {
    const QFont font = ui::transportDisplayFont(kPositionFontPx, QFont::Normal);
    return int(std::ceil(QFontMetricsF(font)
        .horizontalAdvance(QStringLiteral("999.16.999")))) + 20;
}

QString gridDivisionName(const ui::GridDivision& division) {
    if (division.beats < 0.0)
        return QCoreApplication::translate("TransportBar", "Adaptive");
    if (division.beats == 0.0)
        return QCoreApplication::translate("TransportBar", "Off");
    return division.name;
}

// Shared pointer tracking for the two transport scrubs. Recenter only at a
// screen edge, ignore the warp itself, and return to the grab point on release.
class VerticalScrubPointer {
public:
    void begin(const QPointF& position) {
        m_pressPosition = m_lastPosition = position;
        m_warpPending = false;
    }

    qreal takePixels(const QPointF& position, bool wrapAtEdge) {
        if (m_warpPending) {
            // Ignore queued old-edge events, including on release. A zero
            // delta at the warp destination is not physical movement either.
            if (std::abs(position.y() - m_lastPosition.y()) > m_warpTolerance) return 0.0;
            if (qFuzzyIsNull(position.y() - m_lastPosition.y())) return 0.0;
            m_warpPending = false;
        }
        const qreal delta = m_lastPosition.y() - position.y();
        m_lastPosition = position;
        if (wrapAtEdge && std::abs(delta) > 0.0) {
            if (const auto* screen = QGuiApplication::screenAt(position.toPoint())) {
                const QRect bounds = screen->geometry();
                if ((delta > 0 && position.y() <= bounds.top() + 2) ||
                    (delta < 0 && position.y() >= bounds.bottom() - 2)) {
                    m_lastPosition.setY(bounds.center().y());
                    m_warpTolerance = bounds.height() / 4.0;
                    m_warpPending = true;
                    QCursor::setPos(m_lastPosition.toPoint());
                }
            }
        }
        return delta;
    }

    void finish(bool restorePosition) {
        m_warpPending = false;
        if (restorePosition) QCursor::setPos(m_pressPosition.toPoint());
    }

private:
    QPointF m_pressPosition;
    QPointF m_lastPosition;
    qreal m_warpTolerance = 0.0;
    bool m_warpPending = false;
};

/// A tempo number with the interaction used by DAWs and graphics tools: drag
/// vertically for quick relative changes, or double-click to turn it into a
/// normal text editor. It remains a real QLineEdit for accessibility and for
/// keyboard entry; the gesture is an additional path, not the only one.
class TempoScrubEdit final : public QLineEdit {
public:
    using ScrubCallback = std::function<void(double, bool)>;

    explicit TempoScrubEdit(const QString& value, QWidget* parent = nullptr)
        : QLineEdit(value, parent) {
        setReadOnly(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
        setMouseTracking(true);
    }

    void setScrubCallback(ScrubCallback callback) {
        m_callback = std::move(callback);
    }

    void endTextEditing() {
        if (isReadOnly()) return;
        setReadOnly(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
        style()->unpolish(this);
        style()->polish(this);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (!isReadOnly()) {
            QLineEdit::mousePressEvent(event);
            return;
        }
        if (event->button() != Qt::LeftButton) {
            QLineEdit::mousePressEvent(event);
            return;
        }
        bool ok = false;
        double startValue = text().replace(',', '.').toDouble(&ok);
        if (!ok) startValue = 120.0;
        m_currentValue = std::clamp(startValue, 20.0, 300.0);
        m_pendingPixels = 0.0;
        m_pointer.begin(event->globalPosition());
        m_fineMode = event->modifiers() & Qt::ShiftModifier;
        m_pressed = true;
        m_dragging = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_pressed) {
            QLineEdit::mouseMoveEvent(event);
            return;
        }
        if (!(event->buttons() & Qt::LeftButton)) {
            finishDrag();
            event->accept();
            return;
        }
        applyDrag(m_pointer.takePixels(event->globalPosition(), true), event->modifiers());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!m_pressed || event->button() != Qt::LeftButton) {
            QLineEdit::mouseReleaseEvent(event);
            return;
        }
        applyDrag(m_pointer.takePixels(event->globalPosition(), false), event->modifiers());
        finishDrag();
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QLineEdit::mouseDoubleClickEvent(event);
            return;
        }
        m_pointer.finish(m_dragging);
        m_pressed = false;
        m_dragging = false;
        m_textBeforeEdit = text();
        setReadOnly(false);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::IBeamCursor);
        setFocus(Qt::MouseFocusReason);
        selectAll();
        style()->unpolish(this);
        style()->polish(this);
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!isReadOnly() && event->key() == Qt::Key_Escape) {
            setText(m_textBeforeEdit);
            clearFocus();
            // An inactive top-level window may reject focus even though it
            // still receives a synthetic/assistive key event, so do not rely
            // on focusOut to restore scrub mode.
            endTextEditing();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    void applyDrag(qreal pixels, Qt::KeyboardModifiers modifiers) {
        const bool fine = modifiers & Qt::ShiftModifier;
        if (fine != m_fineMode) { m_fineMode = fine; m_pendingPixels = 0.0; }
        if (qFuzzyIsNull(pixels)) return;
        if ((m_currentValue >= 300.0 && pixels > 0) ||
            (m_currentValue <= 20.0 && pixels < 0)) { m_pendingPixels = 0.0; return; }
        m_pendingPixels += pixels;
        if (!m_dragging && std::abs(m_pendingPixels) >= 3.0) {
            m_dragging = true;
            setCursor(Qt::BlankCursor);
        }
        constexpr double pixelsPerStep = 10.0;
        const double steps = std::trunc(m_pendingPixels / pixelsPerStep);
        if (steps == 0.0) return;
        m_pendingPixels -= steps * pixelsPerStep;
        const double value = std::clamp(std::round(
            (m_currentValue + steps * (fine ? 0.1 : 1.0)) * 1000.0) / 1000.0, 20.0, 300.0);
        if (value == 20.0 || value == 300.0) m_pendingPixels = 0.0;
        if (value == m_currentValue) return;
        m_currentValue = value;
        if (m_callback) m_callback(value, false);
    }

    void finishDrag() {
        const bool dragged = m_dragging;
        m_pressed = m_dragging = false;
        m_pointer.finish(dragged);
        setCursor(Qt::SizeVerCursor);
        if (dragged && m_callback) m_callback(m_currentValue, true);
    }

    ScrubCallback m_callback;
    QString m_textBeforeEdit;
    double m_currentValue = 120.0;
    qreal m_pendingPixels = 0.0;
    VerticalScrubPointer m_pointer;
    bool m_fineMode = false;
    bool m_pressed = false;
    bool m_dragging = false;
};

/// Read-only during normal transport use, but vertically scrubbable. A
/// double-click temporarily turns it into a normal text field, which is also
/// the single-pointer alternative to dragging.
class PositionScrubEdit final : public QLineEdit {
public:
    using SecondsGetter = std::function<double()>;
    using SeekCallback = std::function<void(double)>;

    explicit PositionScrubEdit(const QString& value, QWidget* parent = nullptr)
        : QLineEdit(value, parent) {
        setReadOnly(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
    }

    void setScrubCallbacks(SecondsGetter getter, SeekCallback seek) {
        m_seconds = std::move(getter);
        m_seek = std::move(seek);
    }

    void endTextEditing() {
        if (isReadOnly()) return;
        setReadOnly(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeVerCursor);
        style()->unpolish(this);
        style()->polish(this);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (!isReadOnly() || event->button() != Qt::LeftButton) {
            QLineEdit::mousePressEvent(event);
            return;
        }
        m_currentSeconds = m_seconds ? std::max(0.0, m_seconds()) : 0.0;
        m_pendingPixels = 0.0;
        m_pointer.begin(event->globalPosition());
        m_pressed = true;
        m_dragging = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_pressed) {
            QLineEdit::mouseMoveEvent(event);
            return;
        }
        if (!(event->buttons() & Qt::LeftButton)) {
            finishDrag();
            event->accept();
            return;
        }
        applyDrag(m_pointer.takePixels(event->globalPosition(), true), event->modifiers());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!m_pressed || event->button() != Qt::LeftButton) {
            QLineEdit::mouseReleaseEvent(event);
            return;
        }
        applyDrag(m_pointer.takePixels(event->globalPosition(), false), event->modifiers());
        finishDrag();
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QLineEdit::mouseDoubleClickEvent(event);
            return;
        }
        finishDrag();
        m_textBeforeEdit = text();
        setReadOnly(false);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::IBeamCursor);
        setFocus(Qt::MouseFocusReason);
        selectAll();
        style()->unpolish(this);
        style()->polish(this);
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!isReadOnly() && event->key() == Qt::Key_Escape) {
            setText(m_textBeforeEdit);
            clearFocus();
            endTextEditing();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    void applyDrag(qreal pixels, Qt::KeyboardModifiers modifiers) {
        if (!m_dragging) {
            m_pendingPixels += pixels;
            if (std::abs(m_pendingPixels) < 3.0) return;
            pixels = m_pendingPixels;
            m_pendingPixels = 0.0;
            m_dragging = true;
            setCursor(Qt::BlankCursor);
        }
        const double secondsPerPixel = modifiers & Qt::ShiftModifier ? 0.01 : 0.10;
        const double seconds = std::max(0.0, m_currentSeconds + pixels * secondsPerPixel);
        if (std::abs(seconds - m_currentSeconds) >= 0.0001) {
            m_currentSeconds = seconds;
            if (m_seek) m_seek(seconds);
        }
    }

    void finishDrag() {
        m_pointer.finish(m_dragging);
        m_pressed = m_dragging = false;
        setCursor(Qt::SizeVerCursor);
    }

    SecondsGetter m_seconds;
    SeekCallback m_seek;
    QString m_textBeforeEdit;
    VerticalScrubPointer m_pointer;
    qreal m_pendingPixels = 0.0;
    double m_currentSeconds = 0.0;
    bool m_pressed = false;
    bool m_dragging = false;
};

QString tempoText(double bpm) {
    if (std::abs(bpm - std::round(bpm)) < 0.000001)
        return QString::number(qRound64(bpm));
    QString value = QString::number(bpm, 'f', 3);
    while (value.endsWith(QLatin1Char('0'))) value.chop(1);
    if (value.endsWith(QLatin1Char('.'))) value.chop(1);
    return value;
}

struct ToolDef { icons::Glyph glyph; const char* name; };
const ToolDef kTools[] = {
    {icons::Glyph::Pointer, QT_TRANSLATE_NOOP("TransportBar", "Select")},
    {icons::Glyph::Knife, QT_TRANSLATE_NOOP("TransportBar", "Knife")},
    {icons::Glyph::Eraser, QT_TRANSLATE_NOOP("TransportBar", "Eraser")},
    {icons::Glyph::Crosshair, QT_TRANSLATE_NOOP("TransportBar", "Region")},
    {icons::Glyph::Power, QT_TRANSLATE_NOOP("TransportBar", "Mute")},
    {icons::Glyph::Brush, QT_TRANSLATE_NOOP("TransportBar", "Draw")},
    {icons::Glyph::ResizeHorizontal,
     QT_TRANSLATE_NOOP("TransportBar", "Stretch")},
    {icons::Glyph::Glue, QT_TRANSLATE_NOOP("TransportBar", "Glue")},
};
constexpr int kToolCount = int(sizeof(kTools) / sizeof(kTools[0]));

QString translatedToolName(int index) {
    return QCoreApplication::translate("TransportBar", kTools[index].name);
}

QIcon toolIcon(int index, const QColor& color, int size) {
    if (index == 0) return icons::svgIcon(QStringLiteral("cursor.svg"), color, size);
    return icons::icon(kTools[index].glyph, color, size);
}

// Qt styles can replace a checkmark with an action's icon. Keep both visible
// so the current tool remains identifiable independently of colour.
class ToolSelectorMenu final : public QMenu {
public:
    using QMenu::QMenu;
protected:
    void paintEvent(QPaintEvent* event) override {
        QMenu::paintEvent(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(th().textPrimary, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (QAction* action : actions()) {
            if (!action->isChecked()) continue;
            const QRect r = actionGeometry(action);
            const qreal x = r.right() - 14;
            const qreal y = r.center().y();
            p.drawPolyline(QPolygonF{QPointF(x - 4, y), QPointF(x - 1, y + 3),
                                     QPointF(x + 4, y - 3)});
        }
    }
};

// Keep QToolButton/QMenu semantics while drawing a quiet, explicit disclosure.
class ToolSelectorButton final : public QToolButton {
public:
    ToolSelectorButton(bool primary, QWidget* parent)
        : QToolButton(parent), m_primary(primary) {
        m_modifier = QKeySequence(Qt::ControlModifier).toString(QKeySequence::NativeText);
        m_modifierWidth = primary ? 0 :
            QFontMetrics(ui::transportControlFont(11)).horizontalAdvance(m_modifier) + 6;
        setFixedSize(42 + m_modifierWidth, kButtonSize);
        setAttribute(Qt::WA_Hover);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = th();
        constexpr qreal radius = Theme::cornerRadius;
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -1.5);
        const bool pressed = isDown();
        const bool hovered = underMouse() && isEnabled();
        const QColor fill = pressed ? t.wellBottom() : t.controlBottom();
        if (!isEnabled()) p.setOpacity(0.4);

        // Keep the one-pixel contact shadow inside the existing hit target.
        p.setPen(Qt::NoPen);
        p.setBrush(mixColors(t.headerBackground, t.edgeDark(t.headerBackground), pressed ? 0.15 : 0.45));
        p.drawRoundedRect(r.translated(0, 1), radius, radius);
        QLinearGradient face(r.topLeft(), r.bottomLeft());
        face.setColorAt(0, pressed ? t.wellTop() :
            mixColors(t.controlBottom(), t.controlTop(), hovered ? 1.0 : 0.65));
        face.setColorAt(1, fill);
        QLinearGradient rim(r.topLeft(), r.bottomLeft());
        rim.setColorAt(0, pressed ? t.edgeDark(fill) : t.edgeLight(fill));
        rim.setColorAt(1, t.edgeDark(fill));
        p.setPen(QPen(QBrush(rim), 1));
        p.setBrush(face);
        p.drawRoundedRect(r, radius, radius);
        int x = 7;
        if (!m_primary) {
            p.setFont(ui::transportControlFont(11));
            p.setPen(t.textPrimary);
            p.drawText(QRect(3, 0, m_modifierWidth, height()), Qt::AlignCenter, m_modifier);
            x += m_modifierWidth;
        }
        icon().paint(&p, QRect(x, (height() - 18) / 2, 18, 18));
        p.setPen(QPen(t.separator(), 1));
        p.drawLine(QPointF(width() - 15.5, 9), QPointF(width() - 15.5, height() - 10));
        p.setPen(QPen(t.textSecondary, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal cx = width() - 8;
        const qreal cy = height() / 2.0;
        p.drawPolyline(QPolygonF{QPointF(cx - 2.5, cy - 1),
                                 QPointF(cx, cy + 1.5), QPointF(cx + 2.5, cy - 1)});
    }
private:
    bool m_primary;
    QString m_modifier;
    int m_modifierWidth = 0;
};

QColor headerDockFill() {
    return mixColors(th().headerBackground, th().well(), 0.72);
}

QColor headerDockEdge() {
    return mixColors(headerDockFill(), th().textSecondary, 0.24);
}

void paintHeaderDock(QPainter& painter, const QRectF& bounds) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(headerDockFill());
    painter.setPen(QPen(headerDockEdge(), 1));
    painter.drawRoundedRect(bounds, Theme::cornerRadius, Theme::cornerRadius);
}

// The edge controls share one rail. A lit underline indicates an open panel;
// the button face responds only while the pointer hovers or presses it.
class HeaderDockButton final : public ui::IconButton {
public:
    HeaderDockButton(icons::Glyph glyph, const QString& tip, QWidget* parent)
        : ui::IconButton(glyph, tip, parent), m_glyph(glyph) {}
protected:
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = th();
        const bool disclosure = m_glyph == icons::Glyph::Workspace;
        const QRectF face = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        constexpr qreal radius = Theme::cornerRadius - 2;
        const QColor base = headerDockFill();
        QColor fill = base;
        if (isEnabled() && underMouse() && !isChecked())
            fill = mixColors(fill, t.textPrimary, 0.08);
        if (isDown()) fill = mixColors(base, t.textPrimary, 0.16);
        if (!isEnabled()) p.setOpacity(0.4);
        p.setBrush(fill);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(face, radius, radius);

        const QColor ink = t.textPrimary;
        const qreal iconX = disclosure ? 6 : (width() - 18) / 2.0;
        icons::paint(p, m_glyph, QRectF(iconX, (height() - 18) / 2.0, 18, 18), ink);
        p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (disclosure) {
            const qreal x = width() - 10;
            const qreal y = height() / 2.0;
            const qreal direction = isChecked() ? -1 : 1;
            p.drawPolyline(QPolygonF{QPointF(x - direction * 1.5, y - 3),
                QPointF(x + direction * 1.5, y), QPointF(x - direction * 1.5, y + 3)});
        }
        if (isChecked()) {
            const QLineF indicator(iconX + 6, height() - 4, iconX + 12, height() - 4);
            QColor glow = t.accent;
            glow.setAlphaF(underMouse() && isEnabled() ? 0.28 : 0.18);
            p.setPen(QPen(glow, 4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(indicator);
            p.setPen(QPen(mixColors(t.accent, t.textPrimary, t.dark ? 0.35 : 0.0),
                          1.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(indicator);
        }
    }
private:
    const icons::Glyph m_glyph;
};

// The same workspace commands can live inline or in an anchored popup. Moving
// their host preserves button state and signal connections across resizes.
class HeaderInsetPanel final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(HeaderInsetPanel)
public:
    explicit HeaderInsetPanel(bool collapsible, QWidget* parent = nullptr)
        : QWidget(parent), m_collapsible(collapsible) {
        setFixedHeight(kBlockHeight);
        m_row = new QHBoxLayout(this);
        m_row->setContentsMargins(5, 6, 5, 6);
        m_row->setSpacing(3);
        if (collapsible) {
            m_reveal = new HeaderDockButton(icons::Glyph::Workspace, tr("Show workspace controls"), this);
            m_reveal->setObjectName(QStringLiteral("HeaderDockReveal"));
            m_reveal->setAccessibleName(tr("Workspace controls"));
            m_reveal->setFocusPolicy(Qt::StrongFocus);
            m_reveal->setCheckable(true);
            m_reveal->setButtonSize(kButtonSize + 10, kButtonSize);
            m_row->addWidget(m_reveal);
            m_popup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
            m_popup->setObjectName(QStringLiteral("HeaderWorkspacePopup"));
            m_popup->setAttribute(Qt::WA_TranslucentBackground);
            auto* popupLayout = new QHBoxLayout(m_popup);
            popupLayout->setContentsMargins(6, 6, 6, 6);
            m_popup->installEventFilter(this);
            m_actionHost = new QWidget(this);
            m_actionRow = new QHBoxLayout(m_actionHost);
            m_actionRow->setContentsMargins(0, 0, 0, 0);
            m_actionRow->setSpacing(2);
            m_row->addWidget(m_actionHost);
            connect(m_reveal, &QAbstractButton::toggled, this,
                    [this](bool expanded) { setExpanded(expanded); });
        }
        connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
            if (m_popup) m_popup->update();
            update();
        });
    }
    void addAction(QWidget* action) {
        (m_collapsible ? m_actionRow : m_row)->addWidget(action);
    }
    void finish() {
        if (m_collapsible) m_actionHost->hide();
        fitWidth();
    }
    void setGeometryChangedCallback(std::function<void()> callback) {
        m_geometryChanged = std::move(callback);
    }
    void setInlineBudget(int budget) {
        if (!m_collapsible) return;
        const QMargins margins = m_row->contentsMargins();
        const int collapsedWidth = margins.left() + margins.right() + m_reveal->width();
        const bool popup = m_actionRow->sizeHint().width() + collapsedWidth + m_row->spacing() > budget;
        if (popup == m_popupMode) return;
        m_popupMode = popup;
        // Closing on a mode change also releases Qt's popup mouse grab.
        setExpanded(false);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        paintHeaderDock(p, QRectF(rect()).adjusted(0.5, 4.5, -0.5, -4.5));
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_popup && event->type() == QEvent::Paint) {
            // A translucent native popup needs an explicit opaque plate;
            // QFrame's styled background can otherwise leave only the buttons.
            QPainter p(m_popup);
            paintHeaderDock(p, QRectF(m_popup->rect()).adjusted(0.5, 0.5, -0.5, -0.5));
            return true;
        }
        if (watched == m_popup && event->type() == QEvent::Hide) {
            QSignalBlocker block(m_reveal);
            m_reveal->setChecked(false);
            m_reveal->setToolTip(tr("Show workspace controls"));
            m_reveal->setAccessibleName(tr("Show workspace controls"));
            if (isVisible()) m_reveal->setFocus(Qt::PopupFocusReason);
        }
        return QWidget::eventFilter(watched, event);
    }
private:
    void fitWidth() {
        m_row->invalidate();
        m_row->activate();
        setFixedWidth(m_row->sizeHint().width());
    }
    void setExpanded(bool expanded) {
        {
            QSignalBlocker block(m_reveal);
            m_reveal->setChecked(expanded);
        }
        const QString tip = expanded ? tr("Hide workspace controls") : tr("Show workspace controls");
        m_reveal->setToolTip(tip);
        m_reveal->setAccessibleName(tip);
        if (!expanded) {
            m_popup->hide();
            m_actionHost->hide();
        } else if (m_popupMode) {
            m_popup->layout()->addWidget(m_actionHost);
            m_actionHost->show();
            m_popup->adjustSize();
            // Anchor below the entire header, not below the shorter button
            // group. Native frame rounding differs between 1x and Retina.
            QPoint origin(mapToGlobal(QPoint()).x(),
                parentWidget()->mapToGlobal(QPoint(0, parentWidget()->height() + 4)).y());
            if (auto* screen = QGuiApplication::screenAt(origin)) {
                const QRect bounds = screen->availableGeometry();
                origin.setX(std::clamp(origin.x(), bounds.left(),
                    std::max(bounds.left(), bounds.right() - m_popup->width() + 1)));
                origin.setY(std::clamp(origin.y(), bounds.top(),
                    std::max(bounds.top(), bounds.bottom() - m_popup->height() + 1)));
            }
            m_popup->move(origin);
            m_popup->show();
            if (auto* first = m_actionHost->findChild<ui::IconButton*>())
                first->setFocus(Qt::PopupFocusReason);
        } else {
            m_row->addWidget(m_actionHost);
            m_actionHost->show();
        }
        fitWidth();
        if (m_geometryChanged) m_geometryChanged();
    }
    bool m_collapsible = false;
    bool m_popupMode = true;
    QHBoxLayout* m_row = nullptr;
    QHBoxLayout* m_actionRow = nullptr;
    QWidget* m_actionHost = nullptr;
    QFrame* m_popup = nullptr;
    ui::IconButton* m_reveal = nullptr;
    std::function<void()> m_geometryChanged;
};

} // namespace

TransportBar::TransportBar(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    // Read the remembered choices before anything is built, so the menus come
    // up already ticked on what the user last picked rather than on 1/16 and
    // the Select tool.
    m_gridIndex = std::clamp(QSettings().value(ui::kGridIndexSetting, 5).toInt(),
                             0, int(ui::gridDivisions().size()) - 1);
    m_toolIndex = std::clamp(QSettings().value(ui::kEditToolSetting, 0).toInt(),
                             0, kToolCount - 1);
    m_altToolIndex =
        std::clamp(QSettings().value(ui::kAltEditToolSetting, 1).toInt(),
                   0, kToolCount - 1);
    m_rulerFormat = ui::rulerFormatFromInt(
        QSettings().value(ui::kRulerFormatSetting, int(ui::RulerFormat::Bars)).toInt());
    setFixedHeight(ui::kTransportHeight);
    setAttribute(Qt::WA_StyledBackground, false);
    m_backgroundMedia = new ui::ThemeMediaBackground(this);
    connect(m_backgroundMedia, &ui::ThemeMediaBackground::frameChanged, this,
            [this](bool) { update(); });

    // Transparent layout groups keep the complete control row centred.
    m_rightGroup = buildRightGroup();
    m_pill = buildPill();
    m_pill->setParent(this);
    m_pill->raise();

    // Workspace commands stay at the edges; the left group can disclose into
    // a popup when the row has no room for its expanded width.
    m_leftDock = buildLeftDock();
    m_rightDock = buildRightDock();
    m_leftDock->raise();
    m_rightDock->raise();

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &TransportBar::applyTheme);
    connect(&ThemeManager::instance(), &ThemeManager::fontChanged, this,
            &TransportBar::updateResponsiveLayout);
    reloadPanelStyle();
    reloadBackgroundSettings();
    syncTempo();
    syncTimeSignature();
    updateResponsiveLayout();
}

QWidget* TransportBar::buildLeftDock() {
    auto* panel = new HeaderInsetPanel(/*collapsible=*/true, this);
    panel->setObjectName(QStringLiteral("HeaderLeftDock"));
    panel->setAccessibleName(tr("Workspace controls"));

    const auto panelButton = [panel](icons::Glyph glyph, const QString& tip,
                                     const char* objectName) {
        auto* button = new HeaderDockButton(glyph, tip, panel);
        button->setObjectName(QString::fromLatin1(objectName));
        button->setAccessibleName(tip);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setButtonSize(kButtonSize, kButtonSize);
        return button;
    };

    m_browserPanelButton = panelButton(
        icons::Glyph::Folder, tr("Show or hide the browser"),
        "HeaderBrowserButton");
    m_browserPanelButton->setCheckable(true);
    m_browserPanelButton->setChecked(true);
    connect(m_browserPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::browserToggled);
    panel->addAction(m_browserPanelButton);

    m_inspectorPanelButton = panelButton(
        icons::Glyph::Sidebar, tr("Show or hide the inspector"),
        "HeaderInspectorButton");
    m_inspectorPanelButton->setCheckable(true);
    m_inspectorPanelButton->setChecked(true);
    connect(m_inspectorPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::inspectorToggled);
    panel->addAction(m_inspectorPanelButton);

    panel->addAction(ui::separatorLine(Qt::Vertical, 14, panel));

    m_mixerPanelButton = panelButton(
        icons::Glyph::Mixer, tr("Show or hide the mixer (X)"),
        "HeaderMixerButton");
    m_mixerPanelButton->setCheckable(true);
    m_mixerPanelButton->setChecked(true);
    connect(m_mixerPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::mixerToggled);
    panel->addAction(m_mixerPanelButton);

    m_warpButton = panelButton(
        icons::Glyph::Waveform, tr("Open Warp for the selected audio clip (W)"),
        "HeaderWarpButton");
    m_warpButton->setCheckable(true);
    connect(m_warpButton, &QAbstractButton::clicked, this,
            &TransportBar::warpRequested);
    panel->addAction(m_warpButton);

    m_detachMixerButton = panelButton(
        icons::Glyph::Detach, tr("Open the mixer in its own window"),
        "HeaderDetachMixerButton");
    connect(m_detachMixerButton, &QAbstractButton::clicked, this,
            &TransportBar::detachMixerRequested);
    panel->addAction(m_detachMixerButton);

    panel->addAction(ui::separatorLine(Qt::Vertical, 14, panel));

    auto* addTrack = panelButton(icons::Glyph::Plus, tr("Add audio track"),
                                 "HeaderAddTrackButton");
    connect(addTrack, &QAbstractButton::clicked, this,
            &TransportBar::addTrackRequested);
    panel->addAction(addTrack);

    auto* settings = panelButton(icons::Glyph::Gear, tr("Audio settings"),
                                 "HeaderSettingsButton");
    connect(settings, &QAbstractButton::clicked, this,
            &TransportBar::settingsRequested);
    panel->addAction(settings);

    panel->setGeometryChangedCallback(
        [this] { updateResponsiveLayout(); });
    panel->finish();
    return panel;
}

QWidget* TransportBar::buildRightDock() {
    auto* panel = new HeaderInsetPanel(/*collapsible=*/false, this);
    panel->setObjectName(QStringLiteral("HeaderRightDock"));
    panel->setAccessibleName(tr("Connected panels"));

    m_webPanelButton = new HeaderDockButton(
        icons::Glyph::Globe, tr("Open the integrated web browser (Alt+W)"),
        panel);
    m_webPanelButton->setObjectName(QStringLiteral("HeaderWebButton"));
    m_webPanelButton->setAccessibleName(tr("Web browser"));
    m_webPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_webPanelButton->setCheckable(true);
    m_webPanelButton->setButtonSize(kButtonSize, kButtonSize);
    connect(m_webPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::webToggled);
    panel->addAction(m_webPanelButton);

    m_notebookPanelButton = new HeaderDockButton(
        icons::Glyph::Notebook, tr("Open the notebook"), panel);
    m_notebookPanelButton->setObjectName(QStringLiteral("HeaderNotebookButton"));
    m_notebookPanelButton->setAccessibleName(tr("Notebook"));
    m_notebookPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_notebookPanelButton->setCheckable(true);
    m_notebookPanelButton->setButtonSize(kButtonSize, kButtonSize);
    connect(m_notebookPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::notebookToggled);
    panel->addAction(m_notebookPanelButton);

    m_aiPanelButton = new HeaderDockButton(
        icons::Glyph::Assistant, tr("Open the AI assistant"), panel);
    m_aiPanelButton->setObjectName(QStringLiteral("HeaderAiButton"));
    m_aiPanelButton->setAccessibleName(tr("AI assistant"));
    m_aiPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_aiPanelButton->setCheckable(true);
    m_aiPanelButton->setButtonSize(kButtonSize, kButtonSize);
    connect(m_aiPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::aiToggled);
    panel->addAction(m_aiPanelButton);

    panel->finish();
    return panel;
}

QWidget* TransportBar::buildRightGroup() {
    auto* box = new QWidget(this);
    box->setObjectName(QStringLiteral("HeaderToolGroup"));
    box->setAccessibleName(tr("Editing tools"));
    box->setFixedHeight(kBlockHeight);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 6, 0, 6);
    row->setSpacing(4);

    m_snapButton = new ui::IconButton(icons::Glyph::Magnet, tr("Snap to grid"),
                                      box);
    m_snapButton->setObjectName(QStringLiteral("SnapButton"));
    m_snapButton->setAccessibleName(tr("Snap to grid"));
    m_snapButton->setFocusPolicy(Qt::StrongFocus);
    m_snapButton->setButtonSize(kButtonSize, kButtonSize);
    m_snapButton->setCheckable(true);
    m_snapButton->setChecked(m_snapEnabled);
    connect(m_snapButton, &QAbstractButton::toggled, this, [this](bool on) {
        m_snapEnabled = on;
        emit snapChanged(on);
    });

    m_typingKeysButton = new ui::IconButton(icons::Glyph::MidiKeys, QString(),
                                            box);
    m_typingKeysButton->setObjectName(QStringLiteral("MidiKeyboardButton"));
    m_typingKeysButton->setFocusPolicy(Qt::StrongFocus);
    m_typingKeysButton->setButtonSize(kButtonSize, kButtonSize);
    m_typingKeysButton->setCheckable(true);
    setTypingKeyboardOctave(m_typingOctave);
    connect(m_typingKeysButton, &QAbstractButton::toggled, this,
            &TransportBar::typingKeyboardToggled);

    auto buildToolChip = [this, box](bool primary) {
        auto* chip = new ToolSelectorButton(primary, box);
        chip->setObjectName(primary ? "PrimaryToolChip" : "SecondaryToolChip");
        chip->setPopupMode(QToolButton::InstantPopup);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFocusPolicy(Qt::StrongFocus);
        chip->setToolButtonStyle(Qt::ToolButtonIconOnly);
        chip->setIconSize(QSize(18, 18));

        auto* menu = new ToolSelectorMenu(chip);
        auto* group = new QActionGroup(menu);
        group->setExclusive(true);
        for (int i = 0; i < kToolCount; ++i) {
            QAction* action = menu->addAction(
                toolIcon(i, th().textPrimary, 18), translatedToolName(i) +
                    (primary ? QStringLiteral("\t%1").arg(i + 1) : QString()));
            action->setCheckable(true);
            group->addAction(action);
            (primary ? m_toolActions : m_altToolActions).push_back(action);
            connect(action, &QAction::triggered, this, [this, i, primary] {
                if (primary) setToolIndex(i);
                else setSecondaryToolIndex(i);
            });
        }
        menu->setObjectName(QStringLiteral("HeaderToolMenu"));
        chip->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, primary, menu] {
            const auto& actions = primary ? m_toolActions : m_altToolActions;
            const int selected = primary ? m_toolIndex : m_altToolIndex;
            menu->setActiveAction(actions.value(selected));
        });
        return chip;
    };

    m_toolButton = buildToolChip(/*primary=*/true);
    m_altToolButton = buildToolChip(/*primary=*/false);
    setToolIndex(m_toolIndex);
    setSecondaryToolIndex(m_altToolIndex);

    row->addWidget(m_snapButton);
    row->addWidget(m_typingKeysButton);
    auto* divider = ui::separatorLine(Qt::Vertical, 20, box);
    divider->setObjectName(QStringLiteral("ToolGroupDivider"));
    row->addWidget(divider, 0, Qt::AlignVCenter);
    row->addWidget(m_toolButton);
    row->addWidget(m_altToolButton);
    return box;
}

QWidget* TransportBar::buildPill() {
    auto* pill = new QWidget(this);
    pill->setObjectName(QStringLiteral("TransportPill"));
    pill->setAccessibleName(tr("Transport console"));
    pill->setFixedHeight(kBlockHeight);

    auto* row = new QHBoxLayout(pill);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);

    auto* transportPanel = new QWidget(pill);
    m_transportGroup = transportPanel;
    m_transportGroup->setObjectName(QStringLiteral("TransportGroup"));
    m_transportGroup->setAccessibleName(tr("Transport controls"));
    m_transportGroup->setFixedHeight(kBlockHeight);
    auto* buttonRow = new QHBoxLayout(m_transportGroup);
    buttonRow->setContentsMargins(0, 6, 0, 6);
    buttonRow->setSpacing(3);

    constexpr int kBtn = kButtonSize;
    m_toStartButton = new ui::IconButton(icons::Glyph::SkipStart,
                                         tr("Return to start"), transportPanel);
    m_toStartButton->setObjectName(QStringLiteral("TransportToStart"));
    m_toStartButton->setButtonSize(kBtn, kBtn);
    connect(m_toStartButton, &QAbstractButton::clicked, this,
            &TransportBar::returnToStartRequested);

    m_rewindButton = new ui::IconButton(icons::Glyph::Rewind,
                                        tr("Rewind one bar"), transportPanel);
    m_rewindButton->setObjectName(QStringLiteral("TransportRewind"));
    m_rewindButton->setButtonSize(kBtn, kBtn);
    connect(m_rewindButton, &QAbstractButton::clicked, this,
            [this] { emit nudgeRequested(-1); });

    m_stopButton = new ui::IconButton(icons::Glyph::Stop, tr("Stop"),
                                      transportPanel);
    m_stopButton->setObjectName(QStringLiteral("TransportStop"));
    m_stopButton->setButtonSize(kBtn, kBtn);
    connect(m_stopButton, &QAbstractButton::clicked, this,
            &TransportBar::stopRequested);

    // A softly filled square gives Play a stable place in the control row.
    m_playButton = new ui::IconButton(icons::Glyph::Play, tr("Play"),
                                      transportPanel);
    m_playButton->setObjectName(QStringLiteral("TransportPlay"));
    m_playButton->setAccentTint(true);
    m_playButton->setProminent(true);
    m_playButton->setCheckable(true);
    m_playButton->setButtonSize(kBtn, kBtn);
    connect(m_playButton, &QAbstractButton::clicked, this,
            &TransportBar::playPauseRequested);

    m_forwardButton = new ui::IconButton(icons::Glyph::Forward,
                                         tr("Forward one bar"), transportPanel);
    m_forwardButton->setObjectName(QStringLiteral("TransportForward"));
    m_forwardButton->setButtonSize(kBtn, kBtn);
    connect(m_forwardButton, &QAbstractButton::clicked, this,
            [this] { emit nudgeRequested(1); });

    m_recordButton = new ui::IconButton(icons::Glyph::Record, tr("Record"),
                                        transportPanel);
    m_recordButton->setObjectName(QStringLiteral("TransportRecord"));
    m_recordButton->setCheckable(true);
    m_recordButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_recordButton, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        auto* restore = menu.addAction(tr("Восстановить сыгранное"));
        restore->setEnabled(m_controller->hasRetrospectiveMidi());
        connect(restore, &QAction::triggered, this, &TransportBar::restoreMidiRequested);
        menu.exec(m_recordButton->mapToGlobal(pos));
    });
    m_recordButton->setActiveColor(Theme::record());
    m_recordButton->setIdleColor(Theme::record());
    m_recordButton->setButtonSize(kBtn, kBtn);
    connect(m_recordButton, &QAbstractButton::clicked, this,
            &TransportBar::recordRequested);

    m_loopButton = new ui::IconButton(icons::Glyph::Loop, tr("Cycle / loop"),
                                      transportPanel);
    m_loopButton->setObjectName(QStringLiteral("TransportLoop"));
    m_loopButton->setCheckable(true);
    m_loopButton->setActiveColor(Theme::cycle());
    m_loopButton->setButtonSize(kBtn, kBtn);
    connect(m_loopButton, &QAbstractButton::toggled, this,
            &TransportBar::loopToggled);

    m_metroButton = new ui::IconButton(icons::Glyph::Metronome,
                                       tr("Metronome"), transportPanel);
    m_metroButton->setObjectName(QStringLiteral("TransportMetronome"));
    m_metroButton->setCheckable(true);
    m_metroButton->setButtonSize(kBtn, kBtn);
    connect(m_metroButton, &QAbstractButton::toggled, this,
            &TransportBar::metronomeToggled);

    const QList<ui::IconButton*> transportButtons{
        m_toStartButton, m_rewindButton, m_stopButton, m_playButton,
        m_forwardButton, m_recordButton, m_loopButton, m_metroButton};
    for (ui::IconButton* button : transportButtons) {
        button->setFocusPolicy(Qt::StrongFocus);
        buttonRow->addWidget(button);
    }

    auto* centerPanel = new QWidget(pill);
    m_lcdScreen = centerPanel;
    m_lcdScreen->setObjectName(QStringLiteral("LcdScreen"));
    m_lcdScreen->setAccessibleName(tr("Project transport settings"));
    centerPanel->setFixedHeight(kBlockHeight);
    auto* glassRow = new QHBoxLayout(centerPanel);
    glassRow->setContentsMargins(8, 0, 8, 0);
    glassRow->setSpacing(8);

    m_positionGroup = buildPositionGroup();
    glassRow->addWidget(m_positionGroup, 0, Qt::AlignVCenter);
    glassRow->addWidget(ui::separatorLine(Qt::Vertical, 26, centerPanel), 0, Qt::AlignVCenter);

    auto* statsSection = new QWidget(centerPanel);
    m_statsGroup = statsSection;
    statsSection->setObjectName(QStringLiteral("StatsSection"));
    statsSection->setAccessibleName(tr("Tempo, time signature, grid and time display"));
    statsSection->setFixedHeight(kBlockHeight);
    auto* statsRow = new QHBoxLayout(statsSection);
    statsRow->setContentsMargins(0, 0, 0, 0);
    statsRow->setSpacing(6);
    const auto addStatsCell = [statsSection, statsRow](QLabel*& icon,
                                                        QWidget* value,
                                                        int, int) {
        if (statsRow->count())
            statsRow->addWidget(ui::separatorLine(Qt::Vertical, 24, statsSection), 0, Qt::AlignVCenter);
        auto* cell = new QWidget(statsSection);
        auto* cellRow = new QVBoxLayout(cell);
        cellRow->setContentsMargins(0, 2, 0, 2);
        cellRow->setSpacing(0);
        icon = new QLabel(cell);
        icon->setFixedHeight(12);
        icon->setFont(ui::transportControlFont(10));
        icon->setAlignment(Qt::AlignCenter);
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        value->setParent(cell);
        cellRow->addWidget(icon, 0, Qt::AlignHCenter);
        cellRow->addWidget(value, 0, Qt::AlignHCenter);
        statsRow->addWidget(cell);
    };

    auto* tempoEdit = new TempoScrubEdit(QStringLiteral("120"), statsSection);
    m_tempoEdit = tempoEdit;
    m_tempoEdit->setObjectName(QStringLiteral("TempoField"));
    m_tempoEdit->setFont(ui::transportControlFont(kStatsFontPx));
    m_tempoEdit->setFixedSize(68, 28);
    m_tempoEdit->setFrame(false);
    m_tempoEdit->setAlignment(Qt::AlignCenter);
    m_tempoEdit->setAccessibleName(tr("Tempo in BPM"));
    m_tempoEdit->setAccessibleDescription(
        tr("Drag up or down to change tempo. Hold Shift for fine adjustment. Double-click to type a value."));
    m_tempoEdit->setToolTip(
        tr("Drag up/down to change tempo · Shift: fine adjustment · Double-click to type"));
    tempoEdit->setScrubCallback([this](double bpm, bool finished) {
        const QString text = tempoText(bpm);
        if (m_tempoEdit->text() != text) m_tempoEdit->setText(text);
        previewTempo(text);
        if (finished) commitTempo();
    });
    connect(m_tempoEdit, &QLineEdit::textEdited, this,
            &TransportBar::previewTempo);
    connect(m_tempoEdit, &QLineEdit::editingFinished, this,
            [this, tempoEdit] {
                commitTempo();
                tempoEdit->endTextEditing();
              });
    addStatsCell(m_tempoIcon, m_tempoEdit, 0, 0);

    m_timeSignatureButton = new QToolButton(statsSection);
    m_timeSignatureButton->setObjectName(QStringLiteral("TimeSignatureButton"));
    m_timeSignatureButton->setPopupMode(QToolButton::InstantPopup);
    m_timeSignatureButton->setCursor(Qt::PointingHandCursor);
    m_timeSignatureButton->setFocusPolicy(Qt::StrongFocus);
    m_timeSignatureButton->setFixedSize(52, 28);
    m_timeSignatureButton->setFont(ui::transportControlFont(kStatsFontPx));
    m_timeSignatureButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_timeSignatureButton->setAccessibleName(tr("Project time signature"));
    auto* signatureMenu = new QMenu(m_timeSignatureButton);
    auto* signatureGroup = new QActionGroup(signatureMenu);
    signatureGroup->setExclusive(true);
    const std::pair<int, int> signatures[] = {
        {2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};
    for (const auto [numerator, denominator] : signatures) {
        QAction* action = signatureMenu->addAction(
            QStringLiteral("%1/%2").arg(numerator).arg(denominator));
        action->setCheckable(true);
        action->setData(QStringLiteral("%1/%2").arg(numerator).arg(denominator));
        signatureGroup->addAction(action);
        m_timeSignatureActions.push_back(action);
        connect(action, &QAction::triggered, this,
                [this, numerator, denominator] {
                    emit timeSignatureChanged(numerator, denominator);
                    syncTimeSignature();
                });
    }
    signatureMenu->addSeparator();
    QAction* customSignature = signatureMenu->addAction(tr("Other…"));
    connect(customSignature, &QAction::triggered, this,
            &TransportBar::chooseCustomTimeSignature);
    m_timeSignatureButton->setMenu(signatureMenu);
    addStatsCell(m_signatureIcon, m_timeSignatureButton, 1, 0);

    m_gridButton = new QToolButton(statsSection);
    m_gridButton->setObjectName(QStringLiteral("GridChip"));
    m_gridButton->setPopupMode(QToolButton::InstantPopup);
    m_gridButton->setCursor(Qt::PointingHandCursor);
    m_gridButton->setFocusPolicy(Qt::StrongFocus);
    m_gridButton->setFixedSize(68, 28);
    m_gridButton->setFont(ui::transportControlFont(kChipFontPx));
    m_gridButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* gridMenu = new QMenu(m_gridButton);
    auto* gridGroup = new QActionGroup(gridMenu);
    gridGroup->setExclusive(true);
    const auto& divisions = ui::gridDivisions();
    for (int i = 0; i < divisions.size(); ++i) {
        QAction* action = gridMenu->addAction(gridDivisionName(divisions[i]));
        action->setCheckable(true);
        action->setChecked(i == m_gridIndex);
        action->setData(i);
        gridGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, i] { setGridIndex(i); });
    }
    m_gridButton->setMenu(gridMenu);
    m_gridButton->setText(gridDivisionName(divisions[m_gridIndex]));
    const QString gridDescription =
        tr("Grid division — %1").arg(gridDivisionName(divisions[m_gridIndex]));
    m_gridButton->setToolTip(gridDescription);
    m_gridButton->setAccessibleName(gridDescription);
    addStatsCell(m_gridIcon, m_gridButton, 0, 1);

    m_timeFormatButton = new QToolButton(statsSection);
    m_timeFormatButton->setObjectName(QStringLiteral("RulerFormatButton"));
    m_timeFormatButton->setPopupMode(QToolButton::InstantPopup);
    m_timeFormatButton->setCursor(Qt::PointingHandCursor);
    m_timeFormatButton->setFocusPolicy(Qt::StrongFocus);
    m_timeFormatButton->setFixedSize(52, 28);
    m_timeFormatButton->setFont(ui::transportControlFont(kChipFontPx));
    m_timeFormatButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_timeFormatButton->setIconSize(QSize(18, 18));
    auto* timeMenu = new QMenu(m_timeFormatButton);
    const struct { const char* label; ui::RulerFormat format; } kFormats[] = {
        {QT_TRANSLATE_NOOP("TransportBar", "Bars"), ui::RulerFormat::Bars},
        {QT_TRANSLATE_NOOP("TransportBar", "Time"), ui::RulerFormat::Time}};
    for (const auto& fmt : kFormats) {
        QAction* action = timeMenu->addAction(
            QCoreApplication::translate("TransportBar", fmt.label));
        action->setCheckable(true);
        action->setData(int(fmt.format));
        const int flag = int(fmt.format);
        connect(action, &QAction::triggered, this,
                [this, flag](bool checked) {
                    const int next = checked ? int(m_rulerFormat) | flag
                                             : int(m_rulerFormat) & ~flag;
                    if (next) setRulerFormat(ui::rulerFormatFromInt(next));
                    else updateRulerControls();
                });
    }
    timeMenu->addSeparator();
    auto* both = timeMenu->addAction(tr("Bars and time"));
    both->setData(int(ui::RulerFormat::BarsAndTime));
    connect(both, &QAction::triggered, this,
            [this] { setRulerFormat(ui::RulerFormat::BarsAndTime); });
    m_timeFormatButton->setMenu(timeMenu);
    updateRulerControls();
    addStatsCell(m_formatIcon, m_timeFormatButton, 1, 1);

    glassRow->addWidget(statsSection, 0, Qt::AlignVCenter);

    row->addWidget(m_transportGroup, 0, Qt::AlignVCenter);
    row->addWidget(m_lcdScreen, 0, Qt::AlignVCenter);
    row->addWidget(m_rightGroup, 0, Qt::AlignVCenter);
    pill->adjustSize();
    return pill;
}

QWidget* TransportBar::buildPositionGroup() {
    auto* group = new QWidget(this);
    group->setObjectName(QStringLiteral("PositionSection"));
    group->setFixedSize(144, kBlockHeight);
    auto* row = new QVBoxLayout(group);
    row->setContentsMargins(0, 2, 0, 2);
    row->setSpacing(0);
    m_positionLabel = new QLabel(m_positionShowsBars ? tr("Bars") : tr("Time"), group);
    m_positionLabel->setFixedHeight(12);
    m_positionLabel->setFont(ui::transportControlFont(10));
    m_positionLabel->setAlignment(Qt::AlignCenter);
    m_positionLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(m_positionLabel);

    auto* scrub = new PositionScrubEdit(QStringLiteral("1.1.000"), group);
    m_positionValue = scrub;
    m_positionValue->setObjectName(QStringLiteral("BarsPosition"));
    m_positionValue->setFont(
        ui::transportDisplayFont(kPositionFontPx, QFont::Normal));
    m_positionValue->setFixedSize(136, 28);
    m_positionValue->setFrame(false);
    m_positionValue->setAlignment(Qt::AlignCenter);
    m_positionValue->setMaxLength(24);
    m_positionValue->setAccessibleName(
        m_positionShowsBars ? tr("Playhead musical position")
                           : tr("Playhead clock position"));
    m_positionValue->setAccessibleDescription(
        tr("Drag up or down to seek. Double-click to type a position."));
    m_positionValue->setToolTip(
        m_positionShowsBars
            ? tr("Drag to seek · Double-click to enter bar.beat.ticks")
            : tr("Drag to seek · Double-click to enter minutes.seconds.centiseconds"));
    scrub->setScrubCallbacks(
        [this] { return m_controller->presentationPositionSeconds(); },
          [this](double seconds) {
              m_controller->seekSeconds(seconds);
              refreshPosition();
              emit positionChanged();
          });
    connect(m_positionValue, &QLineEdit::editingFinished, this,
            [this] {
                commitPositionEdit(m_positionValue, m_positionShowsBars);
            });
    m_positionValue->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_positionValue, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& localPos) {
                if (!m_positionValue->isReadOnly()) {
                    QMenu* editMenu = m_positionValue->createStandardContextMenu();
                    editMenu->exec(m_positionValue->mapToGlobal(localPos));
                    delete editMenu;
                    return;
                }

                QMenu menu(m_positionValue);
                QActionGroup formats(&menu);
                formats.setExclusive(true);
                QAction* bars = menu.addAction(tr("Bars"));
                QAction* time = menu.addAction(tr("Time"));
                for (QAction* action : {bars, time}) {
                    action->setCheckable(true);
                    formats.addAction(action);
                }
                bars->setChecked(m_positionShowsBars);
                time->setChecked(!m_positionShowsBars);
                if (QAction* chosen = menu.exec(
                        m_positionValue->mapToGlobal(localPos))) {
                    setPositionDisplayBars(chosen == bars);
                }
            });
    row->addWidget(m_positionValue);
    return group;
}

double TransportBar::gridBeats() const {
    return ui::gridDivisions()[m_gridIndex].beats;
}

void TransportBar::setGridIndex(int index) {
    const auto& divisions = ui::gridDivisions();
    if (index < 0 || index >= divisions.size()) return;
    const bool changed = index != m_gridIndex;
    m_gridIndex = index;
    if (m_gridButton) {
        m_gridButton->setText(gridDivisionName(divisions[index]));
        const QString description =
            tr("Grid division — %1").arg(gridDivisionName(divisions[index]));
        m_gridButton->setToolTip(description);
        m_gridButton->setAccessibleName(description);
        if (m_gridButton->menu()) {
            for (QAction* action : m_gridButton->menu()->actions()) {
                if (action->data().isValid())
                    action->setChecked(action->data().toInt() == index);
            }
        }
    }
    if (!changed) return;
    QSettings().setValue(ui::kGridIndexSetting, index);
    emit gridChanged();
}

void TransportBar::setSnapEnabled(bool enabled) {
    if (m_snapButton) {
        if (m_snapButton->isChecked() != enabled)
            m_snapButton->setChecked(enabled);
        return;
    }
    if (m_snapEnabled == enabled) return;
    m_snapEnabled = enabled;
    emit snapChanged(enabled);
}

void TransportBar::setTimeDisplayBars(bool bars) {
    setRulerFormat(bars ? ui::RulerFormat::Bars : ui::RulerFormat::Time);
    setPositionDisplayBars(bars);
}

void TransportBar::setRulerFormat(ui::RulerFormat format) {
    format = ui::rulerFormatFromInt(int(format));
    if (m_rulerFormat == format) return;
    m_rulerFormat = format;
    updateRulerControls();
    QSettings().setValue(ui::kRulerFormatSetting, int(format));
    emit timeFormatChanged();
}

void TransportBar::updateRulerControls() {
    if (!m_timeFormatButton) return;
    const bool both = m_rulerFormat == ui::RulerFormat::BarsAndTime;
    m_timeFormatButton->setIcon(icons::icon(
        both ? icons::Glyph::Layers
             : showsBars() ? icons::Glyph::GridDivision : icons::Glyph::Clock,
        th().textPrimary, 18));
    const QString description = tr("Timeline ruler — %1").arg(
        both ? tr("Bars and time") : showsBars() ? tr("Bars") : tr("Time"));
    m_timeFormatButton->setToolTip(description);
    m_timeFormatButton->setAccessibleName(description);
    if (auto* menu = m_timeFormatButton->menu()) {
        for (QAction* action : menu->actions()) {
            if (!action->isCheckable()) continue;
            const int flag = action->data().toInt();
            const QSignalBlocker blocker(action);
            action->setChecked((int(m_rulerFormat) & flag) != 0);
            // Keep at least one row visible; the other row can always be added.
            action->setEnabled(int(m_rulerFormat) != flag);
        }
    }
}

void TransportBar::setPositionDisplayBars(bool bars) {
    m_positionShowsBars = bars;
    if (m_positionLabel) m_positionLabel->setText(bars ? tr("Bars") : tr("Time"));
    if (m_positionValue) {
        m_positionValue->setAccessibleName(
            bars ? tr("Playhead musical position")
                 : tr("Playhead clock position"));
        m_positionValue->setToolTip(
            bars
                ? tr("Drag to seek · Double-click to enter bar.beat.ticks")
                : tr("Drag to seek · Double-click to enter minutes.seconds.centiseconds"));
    }
    refreshPosition();
}

void TransportBar::setToolIndex(int index) {
    // Only a real change is written. The constructor calls this to apply the
    // stored value, and a launch that re-saves what it just read is a launch
    // that can only ever write over a good setting with a worse one.
    if (index < 0 || index >= kToolCount) return;
    if (index != m_toolIndex) QSettings().setValue(ui::kEditToolSetting, index);
    m_toolIndex = index;
    if (m_toolButton) {
        m_toolButton->setIcon(toolIcon(index, th().textPrimary, 18));
        const QString description =
            tr("%1 tool — on the pointer. 1…8 switch it.")
                .arg(translatedToolName(index));
        m_toolButton->setToolTip(description);
        m_toolButton->setAccessibleName(description);
    }
    if (index < m_toolActions.size() && m_toolActions[index])
        m_toolActions[index]->setChecked(true);
    emit toolChanged(index);
}

void TransportBar::setSecondaryToolIndex(int index) {
    if (index < 0 || index >= kToolCount) return;
    if (index != m_altToolIndex)
        QSettings().setValue(ui::kAltEditToolSetting, index);
    m_altToolIndex = index;
    if (m_altToolButton) {
        // The modifier symbol on the button distinguishes this tool from the
        // primary pointer without adding another persistent colour accent.
        m_altToolButton->setIcon(toolIcon(index, th().textPrimary, 18));
        const QString description =
            tr("%1 tool — while %2 is held.")
                .arg(translatedToolName(index),
                     QKeySequence(Qt::ControlModifier)
                         .toString(QKeySequence::NativeText));
        m_altToolButton->setToolTip(description);
        m_altToolButton->setAccessibleName(description);
    }
    if (index < m_altToolActions.size() && m_altToolActions[index])
        m_altToolActions[index]->setChecked(true);
    emit secondaryToolChanged(index);
}

void TransportBar::toggleCycle() {
    if (m_loopButton) m_loopButton->toggle();
}

void TransportBar::setCycleEnabled(bool on) {
    if (!m_loopButton || m_loopButton->isChecked() == on) return;
    QSignalBlocker block(m_loopButton);
    m_loopButton->setChecked(on);
}

void TransportBar::toggleMetronome() {
    if (m_metroButton) m_metroButton->toggle();
}

void TransportBar::reloadPanelStyle() {
    m_plainPanelStyle = QSettings().value(
        ui::kTransportPanelStyleSetting, QStringLiteral("plain")).toString() ==
        QLatin1String("plain");
    applyTheme();
}

void TransportBar::applyTheme() {
    const Theme& t = th();
    const QColor ink = m_plainPanelStyle ? t.textPrimary
        : mixColors(t.accent, t.textPrimary, t.dark ? 0.35 : 0.10);
    const QColor hover = mixColors(t.well(), t.textPrimary, t.dark ? 0.10 : 0.07);
    const QColor pressed = mixColors(t.headerBackground, t.textPrimary, 0.14);
    setStyleSheet(QStringLiteral(R"(
#TransportPill, #TransportGroup, #HeaderToolGroup, #LcdScreen,
#PositionSection, #StatsSection { background: transparent; border: none; }
#LcdScreen QLabel { background: transparent; color: %7; font-size: 10px; }
#TempoField, #TimeSignatureButton, #GridChip, #RulerFormatButton {
    background: transparent; color: %2; border: 1px solid transparent;
    border-radius: %RADIUS%px; padding: 0 3px; font-size: 15px;
    selection-background-color: %ACCENT%; selection-color: %ACCENT_TEXT%;
}
#GridChip { font-size: 12px; }
#TempoField:hover, #TimeSignatureButton:hover, #GridChip:hover, #RulerFormatButton:hover { background: %3; }
#GridChip::menu-indicator, #TimeSignatureButton::menu-indicator, #RulerFormatButton::menu-indicator { image: none; width: 0; }
QMenu#HeaderToolMenu {
    background: %5; color: %1; border: 1px solid %6;
    border-radius: %RADIUS%px; padding: 5px; font-size: 13px;
}
QMenu#HeaderToolMenu::item { min-height: 22px; padding: 4px 32px 4px 8px; border-radius: %RADIUS%px; font-size: 13px; }
QMenu#HeaderToolMenu::item:checked { color: %1; font-weight: 600; }
QMenu#HeaderToolMenu::item:selected { background: %4; }
)").replace("%RADIUS%", QString::number(Theme::cornerRadius))
          .replace("%ACCENT%", t.accent.name())
          .replace("%ACCENT_TEXT%", t.accentText().name())
          .arg(t.textPrimary.name(), ink.name(), hover.name(),
          pressed.name(), t.headerBackground.name(),
          t.separator().name(), t.textSecondary.name()));
    m_tempoIcon->setText(QStringLiteral("BPM"));
    m_signatureIcon->setText(tr("Meter"));
    m_gridIcon->setText(tr("Grid"));
    m_formatIcon->setText(tr("Ruler"));
    for (auto* button : findChildren<ui::IconButton*>()) {
        button->setProperty("consoleButton", true);
        if (button->accessibleName().isEmpty()) button->setAccessibleName(button->toolTip());
        button->update();
    }
    if (m_snapButton) m_snapButton->setIcon(icons::svgIcon(QStringLiteral("magnet-straight.svg"), t.textPrimary, 18));
    if (m_typingKeysButton) m_typingKeysButton->setIcon(icons::svgIcon(QStringLiteral("piano-keys.svg"), t.textPrimary, 18));
    if (m_metroButton) m_metroButton->setIcon(icons::svgIcon(QStringLiteral("metronome.svg"), t.textPrimary, 18));
    if (m_toolButton) m_toolButton->setIcon(toolIcon(m_toolIndex, t.textPrimary, 18));
    if (m_altToolButton) m_altToolButton->setIcon(toolIcon(m_altToolIndex, t.textPrimary, 18));
    updateRulerControls();
    for (int i = 0; i < kToolCount; ++i) {
        if (i < m_toolActions.size()) m_toolActions[i]->setIcon(toolIcon(i, t.textPrimary, 18));
        if (i < m_altToolActions.size()) m_altToolActions[i]->setIcon(toolIcon(i, t.textPrimary, 18));
    }
    updatePositionStyle();
    updateResponsiveLayout();
    update();
}

void TransportBar::updatePositionStyle() {
    if (!m_positionGroup || !m_positionValue) return;
    const Theme& t = th();
    const QColor ink = m_positionRecording ? Theme::record()
        : m_plainPanelStyle ? t.textPrimary : mixColors(t.accent, t.textPrimary, 0.35);
    m_positionGroup->setStyleSheet(QStringLiteral(
        "#BarsPosition { background: transparent; border: 1px solid transparent; "
        "border-radius: 4px; color: %1; padding: 0 5px; font-size: %2px; "
        "font-weight: 400; selection-color: %1; selection-background-color: %3; }"
        "#BarsPosition:hover { background: %3; }")
        .arg(ink.name(), QString::number(kPositionFontPx),
             mixColors(t.headerBackground, t.textPrimary, 0.10).name()));
}

void TransportBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    paintScene(p, QRegion(rect()));
}

void TransportBar::paintScene(QPainter& p, const QRegion&) {
    const Theme& t = th();
    // The context plate shares this surface; its darker travel rail below
    // keeps the moving controls distinct from their backdrop.
    QLinearGradient surface(0, 0, 0, height());
    surface.setColorAt(0, mixColors(t.headerBackground, t.surfaceElevated, 0.32));
    surface.setColorAt(1, t.headerBackground);
    p.fillRect(rect(), surface);

    if (m_backgroundEnabled && m_backgroundVisibility > 0 &&
        m_backgroundMedia && m_backgroundMedia->hasFrame()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setOpacity(double(m_backgroundVisibility) / 100.0);
        m_backgroundMedia->paint(p, QRectF(rect()));
        p.setOpacity(1.0);
    }

    p.setRenderHint(QPainter::Antialiasing);
    if (m_lcdScreen) {
        const QRectF display = QRectF(QPointF(m_lcdScreen->mapTo(this, QPoint())),
                                      QSizeF(m_lcdScreen->size()))
                                   .adjusted(0.5, 0.5, -0.5, -0.5);
        QLinearGradient bed(display.topLeft(), display.bottomLeft());
        bed.setColorAt(0, mixColors(t.headerBackground, t.well(), 0.64));
        bed.setColorAt(1, mixColors(t.headerBackground, t.well(), 0.42));
        QLinearGradient edge(display.topLeft(), display.bottomLeft());
        edge.setColorAt(0, t.edgeDark(t.headerBackground));
        edge.setColorAt(1, t.edgeLight(t.headerBackground));
        p.setPen(QPen(QBrush(edge), 1));
        p.setBrush(bed);
        p.drawRoundedRect(display, Theme::cornerRadius, Theme::cornerRadius);
    }
}

void TransportBar::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    if (m_backgroundMedia)
        m_backgroundMedia->setTargetSize(size(), devicePixelRatioF());
    updateResponsiveLayout();
}

bool TransportBar::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange && m_backgroundMedia)
        m_backgroundMedia->setTargetSize(size(), devicePixelRatioF());
    return handled;
}

void TransportBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_backgroundMedia)
        m_backgroundMedia->setPlaying(
            m_backgroundEnabled && m_backgroundAnimationEnabled &&
            m_backgroundVisibility > 0);
}

void TransportBar::hideEvent(QHideEvent* event) {
    if (m_backgroundMedia) m_backgroundMedia->setPlaying(false);
    QWidget::hideEvent(event);
}

void TransportBar::reloadBackgroundSettings() {
    using namespace ui::headerbackgroundprefs;
    m_backgroundEnabled = enabled();
    m_backgroundVisibility = visibility();
    m_backgroundAnimationEnabled =
        animatedBackgroundsEnabled() &&
        !QSettings().value(QStringLiteral("ui/reduceMotion"), false).toBool();
    m_backgroundMedia->setTargetSize(size(), devicePixelRatioF());
    m_backgroundMedia->setPlacement(placement());
    m_backgroundMedia->setBlurRadius(blurRadius());
    m_backgroundMedia->setSource(path());
    m_backgroundMedia->setPlaying(
        m_backgroundEnabled && m_backgroundAnimationEnabled &&
        m_backgroundVisibility > 0 && isVisible());
    update();
}

int TransportBar::minimumResponsiveWidth() const {
    if (!m_pill || !m_transportGroup || !m_lcdScreen || !m_positionGroup ||
        !m_rightGroup)
        return 0;

    const auto widgetWidth = [](const QWidget* widget) {
        if (!widget) return 0;
        int width = std::max(widget->sizeHint().width(),
                             widget->minimumSizeHint().width());
        width = std::max(width, widget->minimumWidth());
        if (widget->layout()) {
            width = std::max(width, widget->layout()->sizeHint().width());
            width = std::max(width, widget->layout()->minimumSize().width());
        }
        return std::max(0, width);
    };
    const auto layoutWidth = [&widgetWidth](const QLayout* layout,
                                           const QList<QWidget*>& widgets) {
        if (!layout) return 0;
        const QMargins margins = layout->contentsMargins();
        int width = margins.left() + margins.right();
        int visible = 0;
        for (QWidget* widget : widgets) {
            if (!widget) continue;
            width += widgetWidth(widget);
            ++visible;
        }
        if (visible > 1) width += layout->spacing() * (visible - 1);
        return width;
    };

    // The transport group's minimum state is deliberately just these three
    // core actions. Compute it without hiding live widgets, so querying the
    // panel budget cannot make the header flash during a resize.
    const int coreTransport = layoutWidth(
        m_transportGroup->layout(), {m_stopButton, m_playButton, m_recordButton});
    // Measure the compact layout, independently of the current window width.
    // Otherwise a spacious readout raises the minimum on the next resize and
    // unnecessarily takes space away from the Web/AI side panels.
    const int compactLcdWidth = widgetWidth(m_lcdScreen) -
        widgetWidth(m_positionGroup) + compactPositionWidth();
    const QMargins clusterMargins = m_pill->layout()->contentsMargins();
    const int completeCluster = coreTransport + compactLcdWidth +
        widgetWidth(m_rightGroup) + clusterMargins.left() +
        clusterMargins.right() + 2 * kCompactClusterGap;

    // The fixed right well is broader than the left well's collapsed state,
    // so it defines the symmetric edge reserve without making a temporarily
    // expanded drawer inflate the application's permanent minimum width.
    constexpr int kOuterMargin = 14;
    constexpr int kDockGap = 10;
    const int edgeDock = m_rightDock ? widgetWidth(m_rightDock) : 0;
    return 2 * (kOuterMargin + kDockGap + edgeDock) + completeCluster;
}

QPoint TransportBar::readoutCenterGlobal() const {
    return m_lcdScreen
               ? m_lcdScreen->mapToGlobal(m_lcdScreen->rect().center())
               : mapToGlobal(rect().center());
}

void TransportBar::updateResponsiveLayout() {
    if (!m_pill) return;

    const auto setDisplayCompact = [this](bool compact) {
        // Reserve a three-digit bar and a two-digit beat using the actual
        // application face. Inter is wider than the old condensed fallback;
        // fixed pixel widths clipped clock readings in the compact layout.
        const int fieldWidth = compactPositionWidth() + (compact ? 0 : 12);
        if (m_positionGroup) m_positionGroup->setFixedWidth(fieldWidth);
        if (m_positionValue) m_positionValue->setFixedWidth(fieldWidth);
    };

    const auto fitTransportGroup = [this] {
        if (!m_transportGroup || !m_transportGroup->layout()) return;
        QLayout* layout = m_transportGroup->layout();
        const QMargins margins = layout->contentsMargins();
        int width = margins.left() + margins.right();
        int visible = 0;
        for (int i = 0; i < layout->count(); ++i) {
            QWidget* widget = layout->itemAt(i)->widget();
            if (!widget || widget->isHidden()) continue;
            width += std::max(widget->sizeHint().width(),
                              widget->minimumWidth());
            ++visible;
        }
        if (visible > 1) width += layout->spacing() * (visible - 1);
        // QBoxLayout keeps the old group's broad size hint cached while child
        // buttons are hidden. Fixing the group to the width of what is really
        // visible is what lets the readout move left instead of being laid out
        // below the fixed trailing controls.
        m_transportGroup->setFixedWidth(std::max(1, width));
    };

    // Restore the comfortable display widths and every transport action before
    // measuring. On the way down the display tightens first, then secondary
    // actions disappear in one deterministic order. The three core actions
    // are never candidates.
    const QFontMetrics statsMetrics(ui::transportControlFont(kChipFontPx));
    int gridWidth = 0;
    for (const auto& division : ui::gridDivisions())
        gridWidth = std::max(gridWidth, statsMetrics.horizontalAdvance(gridDivisionName(division)) + 10);
    m_gridButton->setFixedWidth(std::max(60, gridWidth));
    m_timeFormatButton->setFixedWidth(std::max(44,
        QFontMetrics(m_formatIcon->font()).horizontalAdvance(m_formatIcon->text()) + 10));
    m_pill->layout()->setSpacing(16);
    setDisplayCompact(false);
    QWidget* const optional[] = {
        m_forwardButton, m_rewindButton, m_loopButton, m_metroButton,
        m_toStartButton};
    for (QWidget* item : optional)
        if (item) item->show();
    fitTransportGroup();

    constexpr int kOuterMargin = 14;
    constexpr int kDockGap = 10;
    const auto dockY = [this](QWidget* dock) {
        return dock ? (height() - dock->height()) / 2 : 0;
    };
    if (m_leftDock)
        m_leftDock->move(kOuterMargin, dockY(m_leftDock));
    if (m_rightDock)
        m_rightDock->move(std::max(kOuterMargin,
                                  width() - kOuterMargin - m_rightDock->width()),
                          dockY(m_rightDock));

    // Reserve the broader edge on both sides. The transport/LCD/edit cluster
    // therefore remains mathematically centred while the left well opens,
    // instead of being pushed sideways by the drawer animation.
    if (m_leftDock) {
        const int fullWidth = m_pill->layout()->sizeHint().width();
        static_cast<HeaderInsetPanel*>(m_leftDock)->setInlineBudget(
            (width() - fullWidth) / 2 - kOuterMargin - kDockGap);
    }
    const int edgeDock = std::max(m_leftDock ? m_leftDock->width() : 0,
                                  m_rightDock ? m_rightDock->width() : 0);
    const int safeLeft = kOuterMargin + edgeDock + kDockGap;
    const int safeRight = width() - safeLeft;
    const int available = std::max(0, safeRight - safeLeft);

    const auto measuredWidth = [this] {
        if (m_pill->layout()) {
            m_pill->layout()->invalidate();
            m_pill->layout()->activate();
        }
        m_pill->adjustSize();
        return m_pill->sizeHint().width();
    };

    int pillWidth = measuredWidth();
    if (pillWidth > available) {
        m_pill->layout()->setSpacing(kCompactClusterGap);
        setDisplayCompact(true);
        pillWidth = measuredWidth();
    }
    for (QWidget* item : optional) {
        if (pillWidth <= available) break;
        if (!item) continue;
        item->hide();
        fitTransportGroup();
        pillWidth = measuredWidth();
    }

    // This is the complete cluster, not only the LCD. Its centre therefore is
    // the visual centre of all three header elements at every usable width.
    int x = (width() - pillWidth) / 2;
    if (pillWidth <= available) {
        const int maxX = std::max(safeLeft, safeRight - pillWidth);
        x = std::clamp(x, safeLeft, maxX);
    } else {
        // The main window enforces minimumResponsiveWidth(); this fallback
        // only applies to an embedded header temporarily below its minimum.
        x = std::max(0, x);
    }
    m_pill->setGeometry(x, (height() - m_pill->height()) / 2, pillWidth,
                        m_pill->height());
    const int readoutCenter =
        m_lcdScreen
            ? m_lcdScreen->mapTo(this, m_lcdScreen->rect().center()).x()
            : width() / 2;
    if (m_lastReadoutCenterX != readoutCenter) {
        m_lastReadoutCenterX = readoutCenter;
        emit readoutGeometryChanged();
    }
}

QString TransportBar::positionText() const {
    const double seconds = std::max(0.0, m_controller->presentationPositionSeconds());
    const double tempo = std::max(1.0, m_controller->tempo());
    const double quarterNotes = seconds * tempo / 60.0;
    const int beatsPerBar = std::max(1, m_controller->timeSigNumerator());
    const int denominator = std::max(1, m_controller->timeSigDenominator());
    const double beatLength = 4.0 / denominator;
    const double barLength = beatsPerBar * beatLength;
    const int bar = int(std::floor(quarterNotes / barLength)) + 1;
    const double inBar = std::fmod(quarterNotes, barLength);
    const int beat = int(std::floor(inBar / beatLength)) + 1;
    const double inBeat = inBar - (beat - 1) * beatLength;
    const int ticks = std::clamp(int(std::floor(inBeat / beatLength * 1000.0)),
                                 0, 999);
    return QString::asprintf("%d.%d.%03d", bar, beat, ticks);
}

QString TransportBar::clockText() const {
    const double seconds = std::max(0.0, m_controller->presentationPositionSeconds());
    const qint64 centiseconds = qint64(std::floor(seconds * 100.0));
    const qint64 minutes = centiseconds / 6000;
    const int wholeSeconds = int((centiseconds / 100) % 60);
    const int fraction = int(centiseconds % 100);
    return QStringLiteral("%1.%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(wholeSeconds, 2, 10, QLatin1Char('0'))
        .arg(fraction, 2, 10, QLatin1Char('0'));
}

void TransportBar::refreshPosition() {
    if (!m_positionValue || !m_positionValue->isReadOnly()) return;
    const QString position =
        m_positionShowsBars ? positionText() : clockText();
    if (m_positionValue->text() != position)
        m_positionValue->setText(position);
}

void TransportBar::refresh() {
    // Analysis/import and other external edits change the model directly.
    // Preserve partial keyboard input and the current scrub's Undo gesture.
    if (m_tempoEdit && m_tempoEdit->isReadOnly() && !m_tempoEditing) syncTempo();
    const bool playing = m_controller->isPlaying();
    const bool recording = m_controller->isRecording();

    m_playButton->setGlyph(playing ? icons::Glyph::Pause : icons::Glyph::Play);
    m_playButton->setToolTip(playing ? tr("Pause") : tr("Play"));
    m_playButton->setAccessibleName(m_playButton->toolTip());
    {
        QSignalBlocker block(m_playButton);
        m_playButton->setChecked(playing);
    }
    // Engaged and rolling both light the button; only the shade differs, so the
    // two states are never mistaken for each other.
    if (recording) m_recordEngaged = true;
    const bool lit = recording || m_recordEngaged;
    if (m_recordButton->isChecked() != lit) {
        QSignalBlocker block(m_recordButton);
        m_recordButton->setChecked(lit);
    }
    m_recordButton->setActiveColor(Theme::record());
    // Console buttons draw an outline while armed and a fill while
    // recording. Preserve the state API without a decorative pulse.
    m_recordButton->setPulse(m_recordEngaged && !recording);
    m_recordButton->setToolTip(recording  ? tr("Stop recording")
                               : m_recordEngaged
                                   ? tr("Record engaged — start from the panel "
                                        "or press R")
                                   : tr("Record"));
    m_recordButton->setAccessibleName(m_recordButton->toolTip());

    // While rolling the dedicated lightweight playhead clock owns this at
    // display cadence. Avoid formatting the same position again on the slower
    // general UI tick.
    if (!playing) refreshPosition();
    if (m_positionRecording != recording) {
        m_positionRecording = recording;
        updatePositionStyle();
    }

    syncTimeSignature();
}

void TransportBar::commitPositionEdit(QLineEdit* edit, bool musical) {
    if (!edit || edit->isReadOnly()) return;

    const QString normalized = QString(edit->text()).trimmed().replace(':', '.');
    const QStringList parts = normalized.split(QLatin1Char('.'));
    bool valid = parts.size() == 3;
    double seconds = 0.0;

    if (valid && musical) {
        bool barOk = false;
        bool beatOk = false;
        bool ticksOk = false;
        const qlonglong bar = parts[0].toLongLong(&barOk);
        const int beat = parts[1].toInt(&beatOk);
        const int ticks = parts[2].toInt(&ticksOk);
        const int numerator = std::max(1, m_controller->timeSigNumerator());
        const int denominator = std::max(1, m_controller->timeSigDenominator());
        valid = barOk && beatOk && ticksOk && bar >= 1 &&
                beat >= 1 && beat <= numerator && ticks >= 0 && ticks <= 999;
        if (valid) {
            const double beatLength = 4.0 / denominator;
            const double quarterNotes =
                ((double(bar - 1) * numerator) + double(beat - 1) +
                 ticks / 1000.0) * beatLength;
            seconds = quarterNotes * 60.0 / std::max(1.0, m_controller->tempo());
        }
    } else if (valid) {
        bool minutesOk = false;
        bool secondsOk = false;
        bool centisecondsOk = false;
        const qlonglong minutes = parts[0].toLongLong(&minutesOk);
        const int wholeSeconds = parts[1].toInt(&secondsOk);
        const int centiseconds = parts[2].toInt(&centisecondsOk);
        valid = minutesOk && secondsOk && centisecondsOk && minutes >= 0 &&
                wholeSeconds >= 0 && wholeSeconds < 60 &&
                centiseconds >= 0 && centiseconds < 100;
        if (valid)
            seconds = double(minutes) * 60.0 + wholeSeconds +
                      centiseconds / 100.0;
    }

    if (valid) {
        m_controller->seekSeconds(seconds);
        emit positionChanged();
    } else {
        QMessageBox::warning(
            this, tr("Invalid playhead position"),
            musical
                ? tr("Enter the position as bar.beat.ticks. Beat must fit the current time signature and ticks must be from 000 to 999.")
                : tr("Enter the position as minutes.seconds.centiseconds. Seconds must be from 00 to 59 and centiseconds from 00 to 99."));
    }

    edit->clearFocus();
    static_cast<PositionScrubEdit*>(edit)->endTextEditing();
    refreshPosition();
}

void TransportBar::syncTimeSignature() {
    if (!m_controller || !m_timeSignatureButton) return;
    const int numerator = m_controller->timeSigNumerator();
    const int denominator = m_controller->timeSigDenominator();
    const QString value = QStringLiteral("%1/%2").arg(numerator).arg(denominator);
    m_timeSignatureButton->setText(value);
    m_timeSignatureButton->setToolTip(tr("Project time signature — %1").arg(value));
    for (QAction* action : m_timeSignatureActions)
        if (action) action->setChecked(action->data().toString() == value);
}

void TransportBar::chooseCustomTimeSignature() {
    const QString current = QStringLiteral("%1/%2")
        .arg(m_controller->timeSigNumerator())
        .arg(m_controller->timeSigDenominator());
    bool accepted = false;
    const QString value = QInputDialog::getText(
        this, tr("Custom time signature"),
        tr("Enter numerator/denominator:"), QLineEdit::Normal, current, &accepted)
                              .trimmed();
    if (!accepted) return;
    const QStringList parts = value.split(QLatin1Char('/'));
    bool numeratorOk = false;
    bool denominatorOk = false;
    const int numerator = parts.size() == 2
                              ? parts[0].trimmed().toInt(&numeratorOk)
                              : 0;
    const int denominator = parts.size() == 2
                                ? parts[1].trimmed().toInt(&denominatorOk)
                                : 0;
    const bool validDenominator = denominator == 1 || denominator == 2 ||
                                  denominator == 4 || denominator == 8 ||
                                  denominator == 16 || denominator == 32;
    if (!numeratorOk || !denominatorOk || numerator < 1 || numerator > 32 ||
        !validDenominator) {
        QMessageBox::warning(
            this, tr("Invalid time signature"),
            tr("Use a numerator from 1 to 32 and a denominator of 1, 2, 4, 8, 16, or 32."));
        return;
    }
    emit timeSignatureChanged(numerator, denominator);
    syncTimeSignature();
}

void TransportBar::setRecordEngaged(bool engaged) {
    if (m_recordEngaged == engaged) return;
    m_recordEngaged = engaged;
    refresh();   // the button's shade and tooltip both follow from the state
}

void TransportBar::setTypingKeyboardActive(bool active) {
    if (!m_typingKeysButton || m_typingKeysButton->isChecked() == active) return;
    // The signal is what MainWindow drives the keyboard with, and this setter
    // exists precisely to reflect a change that came from there.
    QSignalBlocker block(m_typingKeysButton);
    m_typingKeysButton->setChecked(active);
}

void TransportBar::setTypingKeyboardOctave(int octave) {
    m_typingOctave = octave;
    if (!m_typingKeysButton) return;
    const QString description =
        tr("Typing keyboard — the computer keys play notes\n"
           "Z…M and Q…P, two octaves from C%1  ·  [ and ] shift the octave")
            .arg(octave);
    m_typingKeysButton->setToolTip(description);
    m_typingKeysButton->setAccessibleName(description);
}

namespace {
void reflectToggle(ui::IconButton* button, bool checked) {
    if (!button || button->isChecked() == checked) return;
    QSignalBlocker block(button);
    button->setChecked(checked);
}
} // namespace

void TransportBar::setMixerVisible(bool visible) {
    reflectToggle(m_mixerPanelButton, visible);
}

void TransportBar::setWarpVisible(bool visible) {
    reflectToggle(m_warpButton, visible);
}

void TransportBar::setInspectorVisible(bool visible) {
    reflectToggle(m_inspectorPanelButton, visible);
}

void TransportBar::setBrowserVisible(bool visible) {
    reflectToggle(m_browserPanelButton, visible);
}

void TransportBar::setWebVisible(bool visible) {
    reflectToggle(m_webPanelButton, visible);
}

void TransportBar::setNotebookVisible(bool visible) {
    reflectToggle(m_notebookPanelButton, visible);
}

void TransportBar::setAiVisible(bool visible) {
    reflectToggle(m_aiPanelButton, visible);
}

void TransportBar::setMixerDetached(bool detached) {
    if (!m_detachMixerButton) return;
    m_detachMixerButton->setEnabled(!detached);
    const QString description = detached
        ? tr("Mixer is already open in its own window")
        : tr("Open the mixer in its own window");
    m_detachMixerButton->setToolTip(description);
    m_detachMixerButton->setAccessibleName(description);
}

void TransportBar::syncTempo() {
    if (!m_tempoEdit) return;
    m_tempoEditing = false;
    const QString text = tempoText(m_controller->tempo());
    if (m_tempoEdit->text() != text) m_tempoEdit->setText(text);
}

bool TransportBar::checkHeaderInteractionForTest(const QString& screenshotPath) {
    const auto fail = [](int line) {
        std::fprintf(stderr, "Header interaction check failed at line %d\n", line);
        return false;
    };
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    if (!controller.initialize(48000, 256, false)) return fail(__LINE__);
    QWidget host;
    host.resize(1920, 400);
    TransportBar bar(&controller, &host);
    host.show();
    const auto flush = [] { QCoreApplication::processEvents(); };
    const auto key = [](QWidget* target, int code) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &release);
    };
    auto* reveal = bar.findChild<ui::IconButton*>(QStringLiteral("HeaderDockReveal"));
    auto* browser = bar.m_browserPanelButton;
    if (!reveal || !browser) return fail(__LINE__);
    flush();
    const int minimumWidth = bar.minimumResponsiveWidth();
    for (int width : {1920, 1440, 1180, minimumWidth}) {
        host.resize(width, 400);
        bar.resize(width, ui::kTransportHeight);
        flush();
        if (bar.minimumResponsiveWidth() != minimumWidth) return fail(__LINE__);
        const QList<QWidget*> required{bar.m_stopButton, bar.m_playButton, bar.m_recordButton,
            bar.m_positionValue, bar.m_tempoEdit, bar.m_gridButton, bar.m_timeSignatureButton,
            bar.m_timeFormatButton, bar.m_toolButton, bar.m_altToolButton};
        for (QWidget* widget : required) {
            const QRect bounds(widget->mapTo(&bar, QPoint()), widget->size());
            if (!widget->isVisible() || !bar.rect().contains(bounds)) return fail(__LINE__);
            const bool readout = bar.m_lcdScreen->isAncestorOf(widget);
            const int expectedY = bar.rect().center().y() + (readout ? 6 : 0);
            if (std::abs(bounds.center().y() - expectedY) > 1) return fail(__LINE__);
        }
        if (bar.m_pill->geometry().intersects(bar.m_leftDock->geometry()) ||
            bar.m_pill->geometry().intersects(bar.m_rightDock->geometry())) return fail(__LINE__);
        const QPoint anchor = bar.readoutCenterGlobal();
        reveal->click(); flush();
        if (!browser->isVisible() || bar.readoutCenterGlobal() != anchor) return fail(__LINE__);
        if (browser->window() != &host) {
            if (browser->window()->geometry().top() < bar.mapToGlobal(QPoint(0, bar.height())).y()) {
                std::fprintf(stderr, "Header popup: width=%d hostY=%d headerBottom=%d popupY=%d popupHeight=%d screenHeight=%d\n",
                    width, host.y(), bar.mapToGlobal(QPoint(0, bar.height())).y(),
                    browser->window()->y(), browser->window()->height(), host.screen()->availableGeometry().height());
                return fail(__LINE__);
            }
            key(browser->window(), Qt::Key_Escape); flush();
            if (browser->isVisible() || reveal->isChecked()) return fail(__LINE__);
        } else {
            if (bar.m_leftDock->geometry().intersects(bar.m_pill->geometry())) return fail(__LINE__);
            reveal->click(); flush();
        }
    }
    int primaryChanges = 0, secondaryChanges = 0;
    QObject::connect(&bar, &TransportBar::toolChanged, &bar, [&](int) { ++primaryChanges; });
    QObject::connect(&bar, &TransportBar::secondaryToolChanged, &bar, [&](int) { ++secondaryChanges; });
    for (bool primary : {true, false}) {
        auto* chip = primary ? bar.m_toolButton : bar.m_altToolButton;
        auto* menu = chip->menu();
        if (!menu || menu->actions().size() != kToolCount) return fail(__LINE__);
        for (int index = 0; index < kToolCount; ++index) {
            menu->actions()[index]->trigger();
            const int selected = primary ? bar.m_toolIndex : bar.m_altToolIndex;
            if (selected != index || !menu->actions()[index]->isChecked()) return fail(__LINE__);
        }
        if (primary) bar.setToolIndex(0); else bar.setSecondaryToolIndex(0);
        bool navigated = false;
        QTimer::singleShot(0, menu, [&] {
            key(menu, Qt::Key_Down);
            navigated = menu->activeAction() == menu->actions()[1];
            key(menu, Qt::Key_Return);
            menu->hide(); // Also guarantees a failed check cannot block CI.
        });
        chip->showMenu();
        if (!navigated || (primary ? bar.m_toolIndex : bar.m_altToolIndex) != 1)
            return fail(__LINE__);
        QTimer::singleShot(0, menu, [&] { key(menu, Qt::Key_Escape); menu->hide(); });
        chip->showMenu();
        if ((primary ? bar.m_toolIndex : bar.m_altToolIndex) != 1) return fail(__LINE__);
        QTimer::singleShot(0, menu, [&] {
            const QPoint local = menu->actionGeometry(menu->actions()[5]).center();
            const QPoint global = menu->mapToGlobal(local);
            QMouseEvent press(QEvent::MouseButtonPress, local, global,
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(menu, &press);
            QCoreApplication::sendEvent(menu, &release);
            menu->hide();
        });
        chip->showMenu();
        if ((primary ? bar.m_toolIndex : bar.m_altToolIndex) != 5) return fail(__LINE__);
        bool dismissed = false;
        QTimer::singleShot(0, menu, [&] {
            const QPoint local(-20, -20);
            QMouseEvent outside(QEvent::MouseButtonPress, local, menu->mapToGlobal(local),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(menu, &outside);
            dismissed = !menu->isVisible();
            menu->hide();
        });
        chip->showMenu();
        if (!dismissed) return fail(__LINE__);
    }
    if (primaryChanges != 11 || secondaryChanges != 11) return fail(__LINE__);
    bar.setToolIndex(3); bar.setSecondaryToolIndex(7);
    TransportBar restored(&controller);
    if (restored.m_toolIndex != 3 || restored.secondaryToolIndex() != 7) return fail(__LINE__);
    int play = 0, stop = 0, record = 0, cycle = 0, metro = 0, snap = 0, typing = 0;
    QObject::connect(&bar, &TransportBar::playPauseRequested, &bar, [&] { ++play; });
    QObject::connect(&bar, &TransportBar::stopRequested, &bar, [&] { ++stop; });
    QObject::connect(&bar, &TransportBar::recordRequested, &bar, [&] { ++record; });
    QObject::connect(&bar, &TransportBar::loopToggled, &bar, [&](bool) { ++cycle; });
    QObject::connect(&bar, &TransportBar::metronomeToggled, &bar, [&](bool) { ++metro; });
    QObject::connect(&bar, &TransportBar::snapChanged, &bar, [&](bool) { ++snap; });
    QObject::connect(&bar, &TransportBar::typingKeyboardToggled, &bar, [&](bool) { ++typing; });
    bar.m_playButton->click(); bar.m_stopButton->click(); bar.m_recordButton->click();
    bar.toggleCycle(); bar.toggleMetronome(); bar.m_snapButton->click(); bar.m_typingKeysButton->click();
    if (play != 1 || stop != 1 || record != 1 || cycle != 1 || metro != 1 || snap != 1 || typing != 1)
        return fail(__LINE__);
    bar.setRecordEngaged(true);
    if (!bar.m_recordButton->isChecked()) return fail(__LINE__);
    bar.setRecordEngaged(false);
    bar.setGridIndex(0); if (bar.gridIndex() != 0) return fail(__LINE__);
    bar.setTimeDisplayBars(false); if (bar.showsBars()) return fail(__LINE__);
    bar.setPositionDisplayBars(false); if (bar.positionShowsBars()) return fail(__LINE__);
    if (!screenshotPath.isEmpty()) {
        bar.setToolIndex(0); bar.setSecondaryToolIndex(1);
        bar.setGridIndex(5); bar.setTimeDisplayBars(true); bar.setPositionDisplayBars(true);
        bar.setSnapEnabled(true); bar.setTypingKeyboardActive(false);
        bar.setCycleEnabled(false); bar.toggleMetronome();
        host.resize(1440, 400); bar.resize(1440, ui::kTransportHeight); flush();
        if (!bar.grab().save(screenshotPath)) return fail(__LINE__);
        // Keep the edge rails in their real inline, popup and active states
        // available for visual review at each theme and device scale.
        bar.setWebVisible(true); bar.setAiVisible(true);
        host.resize(1920, 400); bar.resize(1920, ui::kTransportHeight); flush();
        reveal->click(); flush();
        if (!bar.grab().save(screenshotPath + QStringLiteral(".expanded.png"))) return fail(__LINE__);
        reveal->click(); flush();
        host.resize(minimumWidth, 400); bar.resize(minimumWidth, ui::kTransportHeight); flush();
        reveal->click(); flush();
        if (!bar.grab().save(screenshotPath + QStringLiteral(".compact.png"))) return fail(__LINE__);
        if (browser->window() != &host &&
            !browser->window()->grab().save(screenshotPath + QStringLiteral(".popup.png"))) return fail(__LINE__);
        reveal->click(); flush();
        bar.setWebVisible(false); bar.setAiVisible(false);
        host.resize(1440, 400); bar.resize(1440, ui::kTransportHeight); flush();
        for (auto* chip : {bar.m_toolButton, bar.m_altToolButton}) {
            QMenu* menu = chip->menu();
            bool saved = false;
            QTimer::singleShot(0, menu, [&] {
                flush();
                saved = menu->grab().save(screenshotPath +
                    (chip == bar.m_toolButton ? QStringLiteral(".primary.png") : QStringLiteral(".secondary.png")));
                menu->hide();
            });
            chip->showMenu();
            if (!saved) return fail(__LINE__);
        }
    }
    return true;
}

bool TransportBar::checkTempoInteractionForTest() {
    const auto fail = [](int line) { std::fprintf(stderr, "BPM interaction check failed at line %d\n", line); return false; };
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    if (!controller.initialize(48000, 256, false)) return fail(__LINE__);
    QWidget host;
    host.resize(1200, 600);
    TransportBar bar(&controller, &host);
    QObject::connect(&bar, &TransportBar::tempoChanged, &bar,
                     [&](double bpm) { controller.setTempo(bpm); });
    bar.resize(1000, bar.sizeHint().height()); bar.move(50, 300);
    auto* edit = bar.m_tempoEdit;
    const QPointF local = edit->rect().center();
    const QPointF origin = edit->mapToGlobal(local.toPoint());
    const auto send = [&](QEvent::Type type, QPointF global,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, local + global - origin, global,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QCoreApplication::sendEvent(edit, &event);
    };
    const auto reset = [&](double value = 120.0) { controller.setTempo(value); bar.syncTempo(); };
    const auto near = [&](double value) { return std::abs(controller.tempo() - value) < 1.e-8; };
    reset();
    const auto depth = controller.undoDepth();
    send(QEvent::MouseButtonPress, origin);
    send(QEvent::MouseMove, origin - QPointF(0, 9));
    if (!near(120)) return fail(__LINE__); // Minor hand movement does not edit tempo.
    for (int i = 0; i < 100; ++i) send(QEvent::MouseMove, origin - QPointF(0, 10));
    if (!near(121)) return fail(__LINE__); // Repeated coordinates do not accelerate.
    send(QEvent::MouseMove, origin - QPointF(0, 20));
    if (!near(122)) return fail(__LINE__);
    bar.refresh(); // A UI tick must not split the active tempo gesture's Undo.
    send(QEvent::MouseMove, origin - QPointF(0, 10));
    send(QEvent::MouseButtonRelease, origin - QPointF(0, 10));
    if (!near(121) || controller.undoDepth() != depth + 1) return fail(__LINE__);
    controller.undo(); if (!near(120)) return fail(__LINE__);
    reset(120.5);
    send(QEvent::MouseButtonPress, origin, Qt::ShiftModifier);
    send(QEvent::MouseMove, origin - QPointF(0, 10), Qt::ShiftModifier);
    if (!near(120.6)) return fail(__LINE__);
    send(QEvent::MouseMove, origin - QPointF(0, 10));
    if (!near(120.6)) return fail(__LINE__); // Releasing Shift must not jump.
    send(QEvent::MouseMove, origin - QPointF(0, 20));
    send(QEvent::MouseButtonRelease, origin - QPointF(0, 20));
    if (!near(121.6)) return fail(__LINE__);
    for (int samples : {1, 100}) {
        reset(); send(QEvent::MouseButtonPress, origin);
        for (int i = 1; i <= samples; ++i)
            send(QEvent::MouseMove, origin - QPointF(0, 100.0 * i / samples));
        send(QEvent::MouseButtonRelease, origin - QPointF(0, 100));
        if (!near(130)) return fail(__LINE__); // Distance, not event count, sets tempo.
    }
    for (double direction : {-1.0, 1.0}) {
        reset(); send(QEvent::MouseButtonPress, origin);
        const QPointF limit = origin + QPointF(0, direction * 4000);
        send(QEvent::MouseMove, limit);
        if (!near(direction < 0 ? 300 : 20)) return fail(__LINE__);
        const QPointF reverse = limit - QPointF(0, direction * 10);
        send(QEvent::MouseMove, reverse);
        send(QEvent::MouseButtonRelease, reverse);
        if (!near(direction < 0 ? 299 : 21)) return fail(__LINE__);
    }
    if (const auto* screen = QGuiApplication::screenAt(origin.toPoint())) {
        reset(); send(QEvent::MouseButtonPress, origin);
        const QPointF edge(origin.x(), screen->geometry().top());
        send(QEvent::MouseMove, edge);
        const double atEdge = controller.tempo();
        for (int i = 0; i < 10; ++i) send(QEvent::MouseMove, edge);
        if (!near(atEdge)) return fail(__LINE__);
        const QPointF recentered(origin.x(), screen->geometry().center().y());
        send(QEvent::MouseMove, recentered);
        if (!near(atEdge)) return fail(__LINE__);
        send(QEvent::MouseMove, edge); // A queued old-edge event after the warp acknowledgment.
        if (!near(atEdge)) return fail(__LINE__);
        send(QEvent::MouseMove, recentered - QPointF(0, 10));
        send(QEvent::MouseButtonRelease, recentered - QPointF(0, 10));
        if (!near(atEdge + 1)) return fail(__LINE__);
    }
    reset(137.25);
    send(QEvent::MouseButtonPress, origin);
    send(QEvent::MouseButtonRelease, origin);
    if (!near(137.25)) return fail(__LINE__);
    send(QEvent::MouseButtonDblClick, origin);
    if (edit->isReadOnly()) return fail(__LINE__);
    edit->setText(QStringLiteral("138.5"));
    QMetaObject::invokeMethod(edit, "textEdited", Qt::DirectConnection, Q_ARG(QString, edit->text()));
    QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
    if (!near(138.5) || !edit->isReadOnly()) return fail(__LINE__);
    // The analyzer applies tempo through the controller, outside this field.
    controller.setTempo(143.5);
    bar.refresh();
    if (edit->text() != QStringLiteral("143.5")) return fail(__LINE__);
    controller.undo(); bar.refresh();
    if (edit->text() != QStringLiteral("138.5")) return fail(__LINE__);
    controller.redo(); bar.refresh();
    if (edit->text() != QStringLiteral("143.5")) return fail(__LINE__);
    send(QEvent::MouseButtonDblClick, origin);
    edit->setText(QStringLiteral("1"));
    bar.refresh();
    if (edit->text() != QStringLiteral("1")) return fail(__LINE__);
    QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
    if (edit->text() != QStringLiteral("143.5") || !edit->isReadOnly()) return fail(__LINE__);

    auto* position = bar.m_positionValue;
    const QPointF positionLocal = position->rect().center();
    const QPointF positionOrigin = position->mapToGlobal(positionLocal.toPoint());
    const auto sendPosition = [&](QEvent::Type type, QPointF global,
                                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, positionLocal + global - positionOrigin, global,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QCoreApplication::sendEvent(position, &event);
    };
    const auto atPosition = [&](double value) {
        return std::abs(controller.positionSeconds() - value) < 1.e-8;
    };
    controller.seekSeconds(10.0);
    sendPosition(QEvent::MouseButtonPress, positionOrigin);
    for (int i = 0; i < 10; ++i)
        sendPosition(QEvent::MouseMove, positionOrigin - QPointF(0, 20));
    if (!atPosition(12.0) || position->cursor().shape() != Qt::BlankCursor) return fail(__LINE__);
    sendPosition(QEvent::MouseMove, positionOrigin - QPointF(0, 20), Qt::ShiftModifier);
    if (!atPosition(12.0)) return fail(__LINE__);
    sendPosition(QEvent::MouseMove, positionOrigin - QPointF(0, 30), Qt::ShiftModifier);
    if (!atPosition(12.1)) return fail(__LINE__);
    sendPosition(QEvent::MouseMove, positionOrigin - QPointF(0, 20));
    sendPosition(QEvent::MouseButtonRelease, positionOrigin - QPointF(0, 20));
    if (!atPosition(11.1) || position->cursor().shape() != Qt::SizeVerCursor ||
        QCursor::pos() != positionOrigin.toPoint()) return fail(__LINE__);

    if (const auto* screen = QGuiApplication::screenAt(positionOrigin.toPoint())) {
        for (int direction : {-1, 1}) {
            controller.seekSeconds(1000.0);
            sendPosition(QEvent::MouseButtonPress, positionOrigin);
            const QPointF edge(positionOrigin.x(), direction < 0
                ? screen->geometry().top() : screen->geometry().bottom());
            const QPointF center(positionOrigin.x(), screen->geometry().center().y());
            const QPointF next = center + QPointF(0, direction * 10);
            for (int wrap = 0; wrap < 3; ++wrap) {
                sendPosition(QEvent::MouseMove, edge);
                const double atEdge = controller.positionSeconds();
                sendPosition(QEvent::MouseMove, edge);
                sendPosition(QEvent::MouseMove, center);
                sendPosition(QEvent::MouseMove, edge);
                if (!atPosition(atEdge)) return fail(__LINE__);
                sendPosition(QEvent::MouseMove, next);
                if (!atPosition(atEdge - direction)) return fail(__LINE__);
            }
            const double beforeRelease = controller.positionSeconds();
            sendPosition(QEvent::MouseButtonRelease, next);
            if (!atPosition(beforeRelease) || QCursor::pos() != positionOrigin.toPoint()) return fail(__LINE__);
        }
    } else return fail(__LINE__); // Do not silently skip the screen-edge regression.
    controller.seekSeconds(0.0);
    sendPosition(QEvent::MouseButtonPress, positionOrigin);
    sendPosition(QEvent::MouseMove, positionOrigin + QPointF(0, 20));
    if (!atPosition(0.0)) return fail(__LINE__);
    sendPosition(QEvent::MouseMove, positionOrigin + QPointF(0, 10));
    sendPosition(QEvent::MouseButtonRelease, positionOrigin + QPointF(0, 10));
    if (!atPosition(1.0)) return fail(__LINE__);
    std::puts("PASS external BPM refresh and unlimited position scrub: both edges, pointer restore, Shift, reversal and duplicate events");
    std::puts("PASS BPM scrub: distance, duplicate events, Shift precision, bounds, screen wrapping, text entry and one Undo");
    return true;
}

void TransportBar::previewTempo(const QString& text) {
    bool ok = false;
    const double bpm = QString(text).replace(',', '.').toDouble(&ok);
    // Partial input (empty, "1", "12.") is allowed to remain in the editor,
    // but only a complete project-valid value can drive the timeline.
    if (!ok || bpm < 20.0 || bpm > 300.0 || bpm == m_controller->tempo()) return;
    if (!m_tempoEditing) {
        m_tempoEditing = true;
        m_tempoUndoDepth = m_controller->undoDepth();
    }
    emit tempoChanged(bpm);
}

void TransportBar::commitTempo() {
    // Hand the keyboard back as soon as the value is in: the transport keys
    // matter more than the field.
    m_tempoEdit->clearFocus();
    bool ok = false;
    const double bpm = m_tempoEdit->text().replace(',', '.').toDouble(&ok);
    if (!ok || bpm < 20.0 || bpm > 300.0) {
        if (m_tempoEditing)
            m_controller->collapseUndo(m_tempoUndoDepth, "Set Tempo");
        syncTempo();
        return;
    }
    m_tempoEdit->setText(tempoText(bpm));
    if (bpm != m_controller->tempo()) emit tempoChanged(bpm);
    if (m_tempoEditing) {
        m_controller->collapseUndo(m_tempoUndoDepth, "Set Tempo");
        m_tempoEditing = false;
    }
}
