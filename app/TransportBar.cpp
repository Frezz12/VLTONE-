#include "TransportBar.hpp"
#include "Controls.hpp"
#include "GlassPanel.hpp"
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
#include <QButtonGroup>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace {

// The compact console leaves enough room around its glass plate for a soft
// shadow, while its two wells remain large enough to read at a glance.
constexpr int kLcdHeight = 44;
constexpr int kLcdShadow = 4;

// The chassis: one bezel around transport, readout and tools. Its plate is
// inset by the shadow it casts onto the header, and every plate inside it is
// centred in the band the chassis margins leave.
constexpr int kChassisHeight = 70;
constexpr int kChassisShadow = 4;
constexpr int kChassisRadius = 18;
constexpr int kBlockHeight = 44;      // transport and tool groups
constexpr int kBlockShadow = 4;
constexpr int kBlockRadius = 11;

// The readout type scale. These sizes are applied twice on purpose: once
// through setFont(), which is what carries the family and the tabular-figure
// feature, and once as a `font-size` declaration, because the application-wide
// sheet has a `QWidget { font-size: 12px }` rule and a style sheet always wins
// over setFont() for the properties it declares.
constexpr int kPositionFontPx = 25;
constexpr int kStatsFontPx = 16;
constexpr int kChipFontPx = 15;

QString gridDivisionName(const ui::GridDivision& division) {
    if (division.beats < 0.0)
        return QCoreApplication::translate("TransportBar", "Adaptive");
    if (division.beats == 0.0)
        return QCoreApplication::translate("TransportBar", "Off");
    return division.name;
}

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
        m_startValue = text().replace(',', '.').toDouble(&ok);
        if (!ok) m_startValue = 120.0;
        m_startValue = std::round(m_startValue);
        m_currentValue = m_startValue;
        m_pressGlobalY = event->globalPosition().y();
        m_pressed = true;
        m_dragging = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_pressed || !(event->buttons() & Qt::LeftButton)) {
            QLineEdit::mouseMoveEvent(event);
            return;
        }
        const qreal delta = m_pressGlobalY - event->globalPosition().y();
        if (!m_dragging && std::abs(delta) < 3.0) {
            event->accept();
            return;
        }
        m_dragging = true;
        // Scrubbing is deliberately quantised to whole BPM. Decimal tempo is
        // still available through the explicit double-click text entry path.
        const double perPixel = event->modifiers() & Qt::ShiftModifier ? 0.08 : 0.25;
        const double raw = std::clamp(m_startValue + delta * perPixel, 20.0, 300.0);
        const double value = std::round(raw);
        if (value != m_currentValue) {
            m_currentValue = value;
            if (m_callback) m_callback(value, false);
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!m_pressed || event->button() != Qt::LeftButton) {
            QLineEdit::mouseReleaseEvent(event);
            return;
        }
        const bool changed = m_dragging;
        m_pressed = false;
        m_dragging = false;
        setCursor(Qt::SizeVerCursor);
        if (changed && m_callback) m_callback(m_currentValue, true);
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QLineEdit::mouseDoubleClickEvent(event);
            return;
        }
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
    ScrubCallback m_callback;
    QString m_textBeforeEdit;
    qreal m_pressGlobalY = 0.0;
    double m_startValue = 120.0;
    double m_currentValue = 120.0;
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
        m_startSeconds = m_seconds ? std::max(0.0, m_seconds()) : 0.0;
        m_currentSeconds = m_startSeconds;
        m_pressGlobalY = event->globalPosition().y();
        m_pressed = true;
        m_dragging = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_pressed || !(event->buttons() & Qt::LeftButton)) {
            QLineEdit::mouseMoveEvent(event);
            return;
        }
        const qreal delta = m_pressGlobalY - event->globalPosition().y();
        if (!m_dragging && std::abs(delta) < 3.0) {
            event->accept();
            return;
        }
        m_dragging = true;
        const double secondsPerPixel =
            event->modifiers() & Qt::ShiftModifier ? 0.01 : 0.10;
        const double seconds = std::max(0.0, m_startSeconds +
                                                delta * secondsPerPixel);
        if (std::abs(seconds - m_currentSeconds) >= 0.0001) {
            m_currentSeconds = seconds;
            if (m_seek) m_seek(seconds);
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!m_pressed || event->button() != Qt::LeftButton) {
            QLineEdit::mouseReleaseEvent(event);
            return;
        }
        m_pressed = false;
        m_dragging = false;
        setCursor(Qt::SizeVerCursor);
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QLineEdit::mouseDoubleClickEvent(event);
            return;
        }
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
            endTextEditing();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    SecondsGetter m_seconds;
    SeekCallback m_seek;
    QString m_textBeforeEdit;
    qreal m_pressGlobalY = 0.0;
    double m_startSeconds = 0.0;
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
};
constexpr int kToolCount = int(sizeof(kTools) / sizeof(kTools[0]));

QString translatedToolName(int index) {
    return QCoreApplication::translate("TransportBar", kTools[index].name);
}

QIcon toolIcon(int index, const QColor& color, int size) {
    if (index == 0) return icons::svgIcon(QStringLiteral("cursor.svg"), color, size);
    if (index == 1) return icons::svgIcon(QStringLiteral("knife.svg"), color, size);
    return icons::icon(kTools[index].glyph, color, size);
}

QIcon toolChipIcon(int index, const QColor& color) {
    QIcon result;
    for (int scale = 1; scale <= 3; ++scale) {
        QPixmap pixmap(24 * scale, 18 * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        icons::svgIcon(QStringLiteral("caret-down.svg"), color, 7)
            .paint(&painter, QRect(0, 6, 7, 7));
        toolIcon(index, color, 16).paint(&painter, QRect(8, 1, 16, 16));
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

void paintHeaderInsetSurface(QWidget* surface) {
    QPainter painter(surface);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const Theme& t = th();
    const QRectF panel = QRectF(surface->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(panel, 11, 11);

    const QColor base = mixColors(t.headerBackground, t.surfaceElevated, 0.48);
    QLinearGradient fill(0, panel.top(), 0, panel.bottom());
    fill.setColorAt(0.0, mixColors(base, t.textPrimary, 0.045));
    fill.setColorAt(1.0, mixColors(base, t.background, 0.10));
    painter.fillPath(shape, fill);

    // Dark at the upper edge and a hairline reflected edge below are the two
    // cues that make the surface read as inset rather than floating glass.
    const QColor rim = mixColors(t.separator(), t.headerBackground, 0.24);
    painter.setPen(QPen(rim, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);

    painter.save();
    painter.setClipPath(shape);
    QColor upper = t.background;
    upper.setAlpha(150);
    painter.setPen(QPen(upper, 1));
    painter.drawLine(QPointF(panel.left() + 8, panel.top() + 1),
                     QPointF(panel.right() - 8, panel.top() + 1));
    QColor lower(255, 255, 255);
    lower.setAlpha(t.dark ? 18 : 120);
    painter.setPen(QPen(lower, 1));
    painter.drawLine(QPointF(panel.left() + 8, panel.bottom() - 1),
                     QPointF(panel.right() - 8, panel.bottom() - 1));
    painter.restore();
}

/// The LCD's recessed socket. The outer GlassPanel supplies refraction and a
/// soft shadow; this inner layer supplies the dark cavity, inset bevel, and a
/// restrained reflected edge without turning each value into another pill.
///
/// The playhead readout asks for setBacklit(): the same socket, but lit from
/// behind by the project colour and ringed in it, so the one number the eye
/// keeps returning to reads as a powered screen rather than another label.
class LcdInsetWell final : public QWidget {
public:
    explicit LcdInsetWell(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, false);
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                QOverload<>::of(&QWidget::update));
    }

    void setBacklit(bool backlit) {
        if (m_backlit == backlit) return;
        m_backlit = backlit;
        update();
    }

    /// Score a groove down the cavity at this fraction of its width, so one
    /// socket can carry two columns of readings instead of two sockets with a
    /// gap between them. Zero, the default, leaves the cavity unbroken.
    void setColumnDivider(qreal fraction) {
        if (qFuzzyCompare(m_divider + 1.0, fraction + 1.0)) return;
        m_divider = fraction;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const Theme& theme = th();
        const QRectF cavity = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        constexpr qreal radius = 9.0;
        QPainterPath shape;
        shape.addRoundedRect(cavity, radius, radius);

        QLinearGradient depth(0, cavity.top(), 0, cavity.bottom());
        if (m_backlit) {
            // A lit panel is not a darker hole: the glass itself carries the
            // colour, densest at the bottom where it would be thickest.
            depth.setColorAt(0.0, mixColors(theme.well(), theme.accent,
                                            theme.dark ? 0.14 : 0.08));
            depth.setColorAt(0.55, mixColors(theme.well(), theme.accent,
                                             theme.dark ? 0.18 : 0.10));
            depth.setColorAt(1.0, mixColors(theme.well(), theme.accent,
                                            theme.dark ? 0.24 : 0.13));
        } else {
            depth.setColorAt(0.0,
                             mixColors(theme.well(), theme.background, 0.46));
            depth.setColorAt(0.46,
                             mixColors(theme.well(), theme.background, 0.25));
            depth.setColorAt(1.0,
                             mixColors(theme.well(), theme.surfaceElevated,
                                       0.16));
        }
        painter.fillPath(shape, depth);

        // Each cavity is softly backlit by the active project colour. The
        // values use the same hue, so the glow feels reflected by the glass
        // instead of painted around the text.
        QRadialGradient glow(cavity.center(), cavity.width() * 0.68);
        QColor glowCore = theme.accent;
        glowCore.setAlpha(m_backlit ? (theme.dark ? 70 : 40)
                                    : (theme.dark ? 34 : 22));
        QColor glowEdge = glowCore;
        glowEdge.setAlpha(0);
        glow.setColorAt(0.0, glowCore);
        glow.setColorAt(1.0, glowEdge);
        painter.fillPath(shape, glow);

        painter.save();
        painter.setClipPath(shape);
        QColor innerShadow = theme.background;
        innerShadow.setAlpha(theme.dark ? 185 : 72);
        painter.setPen(QPen(innerShadow, 1.2));
        painter.drawLine(QPointF(cavity.left() + radius, cavity.top() + 1),
                         QPointF(cavity.right() - radius, cavity.top() + 1));
        painter.drawLine(QPointF(cavity.left() + 1, cavity.top() + radius),
                         QPointF(cavity.left() + 1,
                                 cavity.bottom() - radius));

        // Light bounces white whatever the palette is. Using the ink colour
        // here drew a second dark line under every well on a light theme,
        // which read as another shadow rather than as a lit lower lip.
        QColor reflection(255, 255, 255);
        reflection.setAlpha(theme.dark ? 22 : 170);
        painter.setPen(QPen(reflection, 1.0));
        painter.drawLine(QPointF(cavity.left() + radius,
                                 cavity.bottom() - 1),
                         QPointF(cavity.right() - radius,
                                 cavity.bottom() - 1));

        QLinearGradient sheen(0, cavity.top() + 2, 0, cavity.center().y());
        sheen.setColorAt(0.0,
                         QColor(255, 255, 255, theme.dark ? 18 : 54));
        sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(sheen);
        painter.drawRoundedRect(cavity.adjusted(2, 2, -2,
                                                 -cavity.height() * 0.48),
                                radius - 2, radius - 2);

        // The groove between two columns of readings: a dark line with a
        // reflected one beside it, the same pair that makes the header wells
        // read as stamped rather than drawn on.
        if (m_divider > 0.0) {
            const qreal x =
                std::round(cavity.left() + cavity.width() * m_divider) + 0.5;
            const qreal top = cavity.top() + 5;
            const qreal bottom = cavity.bottom() - 5;
            QColor groove = theme.background;
            groove.setAlpha(theme.dark ? 150 : 90);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(groove, 1.0));
            painter.drawLine(QPointF(x, top), QPointF(x, bottom));
            QColor lip = theme.textPrimary;
            lip.setAlpha(theme.dark ? 18 : 38);
            painter.setPen(QPen(lip, 1.0));
            painter.drawLine(QPointF(x + 1, top), QPointF(x + 1, bottom));
        }
        painter.restore();

        painter.setBrush(Qt::NoBrush);
        if (m_backlit) {
            // A halo one step outside the ring: the light a lit panel throws
            // back onto the socket it is seated in.
            QColor halo = theme.accent;
            halo.setAlpha(theme.dark ? 60 : 40);
            painter.setPen(QPen(halo, 1.0));
            painter.drawRoundedRect(cavity.adjusted(-1, -1, 1, 1), radius + 1,
                                    radius + 1);
            QColor ring = theme.accent;
            ring.setAlpha(190);
            painter.setPen(QPen(ring, 1.4));
        } else {
            painter.setPen(QPen(mixColors(theme.separator(), theme.background,
                                         theme.dark ? 0.38 : 0.18),
                                1.0));
        }
        painter.drawPath(shape);
    }

private:
    bool m_backlit = false;
    qreal m_divider = 0.0;
};

/// The frame the whole console sits in: one machined bezel around transport,
/// readout and tools, instead of three plates parked separately on the header.
/// It is painted rather than made of glass — the three plates inside already
/// refract what is behind them, and a fourth capture underneath them would buy
/// a second host render for a surface the eye reads as solid metal anyway.
class TransportChassis final : public QWidget {
public:
    explicit TransportChassis(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, false);
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                QOverload<>::of(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const Theme& t = th();
        const QRectF plate =
            QRectF(rect())
                .adjusted(kChassisShadow, kChassisShadow, -kChassisShadow,
                          -kChassisShadow)
                .adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath shape;
        shape.addRoundedRect(plate, kChassisRadius, kChassisRadius);

        // Stacked, ever-fainter copies of the outline approximate a gaussian
        // at a fraction of its cost — the same trick GlassPanel uses, so the
        // bezel and the plates inside it sit on the header with one weight.
        for (int i = kChassisShadow; i >= 1; --i) {
            QPainterPath halo;
            halo.addRoundedRect(plate.adjusted(-i, -i * 0.6, i, i * 1.2),
                                kChassisRadius + i, kChassisRadius + i);
            p.fillPath(halo, QColor(0, 0, 0, t.dark ? 10 : 7));
        }

        QLinearGradient body(plate.topLeft(), plate.bottomLeft());
        body.setColorAt(0.0, mixColors(t.headerBackground, t.textPrimary, 0.07));
        body.setColorAt(1.0, mixColors(t.headerBackground, t.background, 0.12));
        p.fillPath(shape, body);

        // Two strokes are what make the bezel read as thick: a dark groove one
        // step in from the edge, and a graded rim over it for the lit face.
        p.setBrush(Qt::NoBrush);
        QColor lip = t.background;
        lip.setAlpha(t.dark ? 110 : 60);
        p.setPen(QPen(lip, 1.0));
        p.drawRoundedRect(plate.adjusted(2, 2, -2, -2), kChassisRadius - 2,
                          kChassisRadius - 2);

        QLinearGradient edge(plate.topLeft(), plate.bottomLeft());
        QColor top = mixColors(QColor(255, 255, 255), t.textPrimary, 0.25);
        top.setAlphaF(t.dark ? 0.30 : 0.70);
        QColor mid = t.separator();
        mid.setAlphaF(0.30);
        QColor bottom(0, 0, 0);
        bottom.setAlphaF(t.dark ? 0.42 : 0.22);
        edge.setColorAt(0.0, top);
        edge.setColorAt(0.5, mid);
        edge.setColorAt(1.0, bottom);
        QPen rim(QBrush(edge), 1.0);
        rim.setJoinStyle(Qt::RoundJoin);
        p.setPen(rim);
        p.drawPath(shape);
    }
};

/// A shallow well punched into the header. The left instance can crop its
/// action row down to one disclosure button; animating the well itself (rather
/// than translating loose buttons over the header) keeps the control feeling
/// like part of the chrome throughout the transition.
class HeaderInsetPanel final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(HeaderInsetPanel)
public:
    explicit HeaderInsetPanel(bool collapsible, QWidget* parent = nullptr)
        : QWidget(parent), m_collapsible(collapsible) {
        setFixedHeight(34);
        setAttribute(Qt::WA_StyledBackground, false);

        m_row = new QHBoxLayout(this);
        m_row->setContentsMargins(4, 3, 4, 3);
        m_row->setSpacing(2);

        if (m_collapsible) {
            m_reveal = new ui::IconButton(
                icons::Glyph::Layers, tr("Show workspace controls"), this);
            m_reveal->setObjectName(QStringLiteral("HeaderDockReveal"));
            m_reveal->setAccessibleName(tr("Workspace controls"));
            m_reveal->setFocusPolicy(Qt::StrongFocus);
            m_reveal->setCheckable(true);
            m_reveal->setButtonSize(28, 28);
            m_row->addWidget(m_reveal);
            connect(m_reveal, &QAbstractButton::toggled, this,
                    [this](bool expanded) { setExpanded(expanded, true); });
        }

        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                QOverload<>::of(&QWidget::update));
    }

    void addAction(QWidget* action) {
        if (!action) return;
        m_actions.push_back(action);
        m_row->addWidget(action);
    }

    void finish() {
        m_row->invalidate();
        m_row->activate();
        const QMargins margins = m_row->contentsMargins();
        m_collapsedWidth = m_collapsible && m_reveal
                               ? margins.left() + m_reveal->width() +
                                     margins.right()
                               : m_row->sizeHint().width();
        m_expandedWidth = std::max(m_collapsedWidth, m_row->sizeHint().width());
        setFixedWidth(m_expandedWidth);
        if (m_collapsible) {
            m_expanded = true;
            setExpanded(false, false);
        }
    }

    void setGeometryChangedCallback(std::function<void()> callback) {
        m_geometryChanged = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent*) override { paintHeaderInsetSurface(this); }

private:
    void setExpanded(bool expanded, bool animate) {
        if (!m_collapsible || m_expanded == expanded) return;
        m_expanded = expanded;

        if (m_reveal && m_reveal->isChecked() != expanded) {
            QSignalBlocker block(m_reveal);
            m_reveal->setChecked(expanded);
        }
        if (m_reveal) {
            const QString tip = expanded ? tr("Hide workspace controls")
                                         : tr("Show workspace controls");
            m_reveal->setToolTip(tip);
            m_reveal->setAccessibleName(tip);
        }

        if (expanded) {
            for (QWidget* action : m_actions) action->show();
            m_row->invalidate();
            m_row->activate();
            m_expandedWidth =
                std::max(m_collapsedWidth, m_row->sizeHint().width());
        }

        const int target = expanded ? m_expandedWidth : m_collapsedWidth;
        if (!animate) {
            setFixedWidth(target);
            if (!expanded)
                for (QWidget* action : m_actions) action->hide();
            if (m_geometryChanged) m_geometryChanged();
            return;
        }

        if (!m_animation) {
            m_animation = new QVariantAnimation(this);
            m_animation->setDuration(180);
            connect(m_animation, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& value) {
                        setFixedWidth(value.toInt());
                        if (m_geometryChanged) m_geometryChanged();
                    });
            connect(m_animation, &QVariantAnimation::finished, this, [this] {
                if (!m_expanded)
                    for (QWidget* action : m_actions) action->hide();
                if (m_geometryChanged) m_geometryChanged();
            });
        }
        m_animation->stop();
        m_animation->setStartValue(width());
        m_animation->setEndValue(target);
        m_animation->setEasingCurve(expanded ? QEasingCurve::OutCubic
                                             : QEasingCurve::InOutCubic);
        m_animation->start();
    }

    bool m_collapsible = false;
    bool m_expanded = false;
    int m_collapsedWidth = 0;
    int m_expandedWidth = 0;
    QHBoxLayout* m_row = nullptr;
    ui::IconButton* m_reveal = nullptr;
    QList<QWidget*> m_actions;
    QVariantAnimation* m_animation = nullptr;
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
    setFixedHeight(ui::kTransportHeight);
    setAttribute(Qt::WA_StyledBackground, false);
    m_backgroundMedia = new ui::ThemeMediaBackground(this);
    connect(m_backgroundMedia, &ui::ThemeMediaBackground::frameChanged, this,
            [this](bool) { update(); });

    // Build the trailing controls first, then restore the original three-part
    // composition: transport, glass LCD, and editing tools. Their shared
    // wrapper centres the complete cluster rather than any individual block.
    m_rightGroup = buildRightGroup();
    m_pill = buildPill();
    m_pill->setParent(this);
    m_pill->raise();

    // Window controls used to consume a whole strip at the bottom of the
    // workspace. They now sit in shallow header wells at the outer edges: the
    // left one discloses the less frequent commands, while Web and AI remain
    // one-click actions on the right.
    m_leftDock = buildLeftDock();
    m_rightDock = buildRightDock();
    m_leftDock->raise();
    m_rightDock->raise();

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &TransportBar::applyTheme);
    applyTheme();
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
        auto* button = new ui::IconButton(glyph, tip, panel);
        button->setObjectName(QString::fromLatin1(objectName));
        button->setAccessibleName(tip);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setButtonSize(28, 28);
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

    m_webPanelButton = new ui::IconButton(
        icons::Glyph::Globe, tr("Open the integrated web browser (Alt+W)"),
        panel);
    m_webPanelButton->setObjectName(QStringLiteral("HeaderWebButton"));
    m_webPanelButton->setAccessibleName(tr("Web browser"));
    m_webPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_webPanelButton->setCheckable(true);
    m_webPanelButton->setButtonSize(28, 28);
    connect(m_webPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::webToggled);
    panel->addAction(m_webPanelButton);

    m_notebookPanelButton = new ui::IconButton(
        icons::Glyph::Notebook, tr("Open the notebook"), panel);
    m_notebookPanelButton->setObjectName(QStringLiteral("HeaderNotebookButton"));
    m_notebookPanelButton->setAccessibleName(tr("Notebook"));
    m_notebookPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_notebookPanelButton->setCheckable(true);
    m_notebookPanelButton->setButtonSize(28, 28);
    connect(m_notebookPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::notebookToggled);
    panel->addAction(m_notebookPanelButton);

    m_aiPanelButton = new ui::IconButton(
        icons::Glyph::Assistant, tr("Open the AI assistant"), panel);
    m_aiPanelButton->setObjectName(QStringLiteral("HeaderAiButton"));
    m_aiPanelButton->setAccessibleName(tr("AI assistant"));
    m_aiPanelButton->setFocusPolicy(Qt::StrongFocus);
    m_aiPanelButton->setCheckable(true);
    m_aiPanelButton->setButtonSize(28, 28);
    connect(m_aiPanelButton, &QAbstractButton::toggled, this,
            &TransportBar::aiToggled);
    panel->addAction(m_aiPanelButton);

    panel->finish();
    return panel;
}

QWidget* TransportBar::buildRightGroup() {
    auto* box = new ui::GlassPanel(this);
    box->setObjectName(QStringLiteral("HeaderToolGroup"));
    box->setAccessibleName(tr("Editing tools"));
    box->setShadowMargin(kBlockShadow);
    box->setCornerRadius(kBlockRadius);
    box->setSubtleVerticalGradient(true);
    box->setFixedHeight(kBlockHeight);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(9, 8, 9, 8);
    row->setSpacing(2);

    m_snapButton = new ui::IconButton(icons::Glyph::Magnet, tr("Snap to grid"),
                                      box);
    m_snapButton->setObjectName(QStringLiteral("SnapButton"));
    m_snapButton->setAccessibleName(tr("Snap to grid"));
    m_snapButton->setFocusPolicy(Qt::StrongFocus);
    m_snapButton->setButtonSize(28, 28);
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
    m_typingKeysButton->setButtonSize(28, 28);
    m_typingKeysButton->setCheckable(true);
    setTypingKeyboardOctave(m_typingOctave);
    connect(m_typingKeysButton, &QAbstractButton::toggled, this,
            &TransportBar::typingKeyboardToggled);

    auto buildToolChip = [this, box](bool primary) {
        auto* chip = new QToolButton(box);
        chip->setObjectName(primary ? "PrimaryToolChip" : "SecondaryToolChip");
        chip->setPopupMode(QToolButton::InstantPopup);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFocusPolicy(Qt::StrongFocus);
        chip->setFixedSize(36, 28);
        chip->setToolButtonStyle(Qt::ToolButtonIconOnly);
        chip->setIconSize(QSize(24, 18));

        auto* menu = new QMenu(chip);
        auto* group = new QActionGroup(menu);
        group->setExclusive(true);
        for (int i = 0; i < kToolCount; ++i) {
            QAction* action = menu->addAction(
                toolIcon(i, th().textPrimary, 14), translatedToolName(i));
            action->setCheckable(true);
            group->addAction(action);
            (primary ? m_toolActions : m_altToolActions).push_back(action);
            connect(action, &QAction::triggered, this, [this, i, primary] {
                if (primary) setToolIndex(i);
                else setSecondaryToolIndex(i);
            });
        }
        chip->setMenu(menu);
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
    auto* pill = new TransportChassis(this);
    pill->setObjectName(QStringLiteral("TransportPill"));
    pill->setAccessibleName(tr("Transport console"));
    pill->setFixedHeight(kChassisHeight);

    // The chassis margins are its shadow plus the bezel it shows around the
    // plates; what is left is one band every block is vertically centred in.
    auto* row = new QHBoxLayout(pill);
    row->setContentsMargins(kChassisShadow + 10, kChassisShadow + 5,
                            kChassisShadow + 10, kChassisShadow + 5);
    row->setSpacing(10);

    auto* transportPanel = new ui::GlassPanel(pill);
    m_transportGroup = transportPanel;
    m_transportGroup->setObjectName(QStringLiteral("TransportGroup"));
    m_transportGroup->setAccessibleName(tr("Transport controls"));
    transportPanel->setShadowMargin(kBlockShadow);
    transportPanel->setCornerRadius(kBlockRadius);
    transportPanel->setSubtleVerticalGradient(true);
    m_transportGroup->setFixedHeight(kBlockHeight);
    auto* buttonRow = new QHBoxLayout(m_transportGroup);
    buttonRow->setContentsMargins(9, 8, 9, 8);
    buttonRow->setSpacing(2);

    constexpr int kBtn = 28;
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

    // Play stays the primary action but flat — an accent-tinted glyph, not a
    // filled circle, so the transport block reads slim.
    m_playButton = new ui::IconButton(icons::Glyph::Play, tr("Play"),
                                      transportPanel);
    m_playButton->setObjectName(QStringLiteral("TransportPlay"));
    m_playButton->setAccentTint(true);
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
    m_recordButton->setActiveColor(Theme::record());
    m_recordButton->setIdleColor(Theme::record());
    m_recordButton->setButtonSize(kBtn, kBtn);
    connect(m_recordButton, &QAbstractButton::clicked, this,
            &TransportBar::recordRequested);

    m_loopButton = new ui::IconButton(icons::Glyph::Loop, tr("Cycle / loop"),
                                      transportPanel);
    m_loopButton->setObjectName(QStringLiteral("TransportLoop"));
    m_loopButton->setCheckable(true);
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

    auto* centerPanel = new ui::GlassPanel(pill);
    m_lcdScreen = centerPanel;
    m_lcdScreen->setObjectName(QStringLiteral("LcdScreen"));
    m_lcdScreen->setAccessibleName(tr("Project transport settings"));
    centerPanel->setShadowMargin(kLcdShadow);
    centerPanel->setCornerRadius(kBlockRadius);
    centerPanel->setSubtleVerticalGradient(true);
    centerPanel->setFixedHeight(kLcdHeight + 2 * kLcdShadow);
    auto* glassRow = new QHBoxLayout(centerPanel);
    glassRow->setContentsMargins(kLcdShadow + 5, kLcdShadow + 2,
                                 kLcdShadow + 5, kLcdShadow + 2);
    glassRow->setSpacing(5);

    m_positionGroup = buildPositionGroup();
    glassRow->addWidget(m_positionGroup, 0, Qt::AlignVCenter);

    // Tempo, signature, grid and time format used to be two sockets with a gap
    // between them. Four readings of the same kind belong in one: a 2x2 field
    // split by a groove, which is both shorter and easier to scan.
    auto* statsSection = new LcdInsetWell(centerPanel);
    m_statsGroup = statsSection;
    statsSection->setObjectName(QStringLiteral("StatsSection"));
    statsSection->setAccessibleName(
        tr("Tempo, time signature, grid and time display"));
    statsSection->setFixedSize(188, 40);
    statsSection->setColumnDivider(0.5);
    auto* statsGrid = new QGridLayout(statsSection);
    statsGrid->setContentsMargins(6, 1, 6, 1);
    statsGrid->setHorizontalSpacing(8);
    statsGrid->setVerticalSpacing(0);

    // Every cell is an icon and its value on one 16 px line. The icon lives in
    // its own label rather than inside the control, so a QLineEdit and a
    // QToolButton line up on the same baseline in the same column.
    const auto addStatsCell = [statsSection, statsGrid](QLabel*& icon,
                                                        QWidget* value,
                                                        int line, int column) {
        auto* cell = new QWidget(statsSection);
        cell->setFixedHeight(19);
        auto* cellRow = new QHBoxLayout(cell);
        cellRow->setContentsMargins(0, 0, 0, 0);
        cellRow->setSpacing(2);
        icon = new QLabel(cell);
        icon->setFixedSize(16, 16);
        icon->setAlignment(Qt::AlignCenter);
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        value->setParent(cell);
        cellRow->addWidget(icon);
        cellRow->addWidget(value);
        statsGrid->addWidget(cell, line, column);
    };

    auto* tempoEdit = new TempoScrubEdit(QStringLiteral("120"), statsSection);
    m_tempoEdit = tempoEdit;
    m_tempoEdit->setObjectName(QStringLiteral("TempoField"));
    m_tempoEdit->setFont(ui::transportDisplayFont(kStatsFontPx));
    m_tempoEdit->setFixedSize(66, 19);
    m_tempoEdit->setFrame(false);
    m_tempoEdit->setAlignment(Qt::AlignCenter);
    m_tempoEdit->setAccessibleName(tr("Tempo in BPM"));
    m_tempoEdit->setAccessibleDescription(
        tr("Drag up or down to change tempo. Double-click to type a value."));
    m_tempoEdit->setToolTip(
        tr("Drag up/down to change tempo · Double-click to type"));
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
    m_timeSignatureButton->setFixedSize(66, 19);
    m_timeSignatureButton->setFont(ui::transportDisplayFont(kStatsFontPx));
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
    m_gridButton->setFixedSize(66, 19);
    m_gridButton->setFont(ui::transportDisplayFont(kChipFontPx));
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
    m_timeFormatButton->setObjectName(QStringLiteral("GridChip"));
    m_timeFormatButton->setPopupMode(QToolButton::InstantPopup);
    m_timeFormatButton->setCursor(Qt::PointingHandCursor);
    m_timeFormatButton->setFocusPolicy(Qt::StrongFocus);
    m_timeFormatButton->setFixedSize(66, 19);
    m_timeFormatButton->setFont(ui::transportDisplayFont(kChipFontPx));
    m_timeFormatButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* timeMenu = new QMenu(m_timeFormatButton);
    auto* timeGroup = new QActionGroup(timeMenu);
    timeGroup->setExclusive(true);
    const struct { const char* label; bool bars; } kFormats[] = {
        {QT_TRANSLATE_NOOP("TransportBar", "Bars"), true},
        {QT_TRANSLATE_NOOP("TransportBar", "Time"), false}};
    for (const auto& fmt : kFormats) {
        QAction* action = timeMenu->addAction(
            QCoreApplication::translate("TransportBar", fmt.label));
        action->setCheckable(true);
        action->setChecked(fmt.bars == m_showBars);
        action->setData(fmt.bars);
        timeGroup->addAction(action);
        const bool bars = fmt.bars;
        connect(action, &QAction::triggered, this,
                [this, bars] { setTimeDisplayBars(bars); });
    }
    m_timeFormatButton->setMenu(timeMenu);
    m_timeFormatButton->setText(m_showBars ? tr("Bars") : tr("Time"));
    const QString timeDescription =
        tr("Time display — %1").arg(m_timeFormatButton->text());
    m_timeFormatButton->setToolTip(timeDescription);
    m_timeFormatButton->setAccessibleName(timeDescription);
    addStatsCell(m_formatIcon, m_timeFormatButton, 1, 1);

    glassRow->addWidget(statsSection, 0, Qt::AlignVCenter);

    row->addWidget(m_transportGroup, 0, Qt::AlignVCenter);
    row->addWidget(m_lcdScreen, 0, Qt::AlignVCenter);
    row->addWidget(m_rightGroup, 0, Qt::AlignVCenter);
    pill->adjustSize();
    return pill;
}

QWidget* TransportBar::buildPositionGroup() {
    auto* group = new LcdInsetWell(this);
    group->setObjectName(QStringLiteral("PositionSection"));
    group->setFixedSize(168, 40);
    // The one reading the eye keeps coming back to. It is the only socket that
    // is lit from behind, which is what makes it findable without hunting.
    group->setBacklit(true);
    // One inset on every side. The counter used to sit in a field two pixels
    // wider on the right than the socket was deep, which reads as a slip.
    auto* row = new QHBoxLayout(group);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(4);

    m_positionIcon = new QLabel(group);
    m_positionIcon->setFixedSize(20, 20);
    m_positionIcon->setAlignment(Qt::AlignCenter);
    m_positionIcon->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto* scrub = new PositionScrubEdit(QStringLiteral("1.1.000"), group);
    m_positionValue = scrub;
    m_positionValue->setObjectName(QStringLiteral("BarsPosition"));
    m_positionValue->setFont(
        ui::transportDisplayFont(kPositionFontPx, QFont::Bold));
    m_positionValue->setFixedSize(136, 32);
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
    row->addWidget(m_positionIcon);
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
    const bool changed = m_showBars != bars;
    m_showBars = bars;
    if (m_timeFormatButton) {
        m_timeFormatButton->setText(bars ? tr("Bars") : tr("Time"));
        const QString description =
            tr("Time display — %1").arg(bars ? tr("Bars") : tr("Time"));
        m_timeFormatButton->setToolTip(description);
        m_timeFormatButton->setAccessibleName(description);
        if (m_timeFormatButton->menu()) {
            for (QAction* action : m_timeFormatButton->menu()->actions()) {
                if (action->data().isValid())
                    action->setChecked(action->data().toBool() == bars);
            }
        }
    }
    setPositionDisplayBars(bars);
    if (changed) emit timeFormatChanged();
}

void TransportBar::setPositionDisplayBars(bool bars) {
    m_positionShowsBars = bars;
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
        m_toolButton->setIcon(toolChipIcon(index, th().accent));
        const QString description =
            tr("%1 tool — on the pointer. 1…7 switch it.")
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
        // Drawn in the secondary ink, so the pair reads as "this one, and this
        // one while you hold the key" rather than as two equal tools.
        m_altToolButton->setIcon(toolChipIcon(index, th().textPrimary));
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

void TransportBar::applyTheme() {
    const Theme& t = th();

    if (m_pill) {
        const QColor nested =
            mixColors(t.surfaceElevated, t.headerBackground, 0.45);
        const QColor panelBorder =
            mixColors(t.separator(), t.textPrimary, 0.08);
        const QColor field = mixColors(t.well(), t.surfaceElevated, 0.26);
        const QColor hover =
            mixColors(t.surfaceElevated, t.textPrimary, 0.14);
        const QColor fieldHover = mixColors(field, t.textPrimary, 0.10);
        const QColor displayInk = mixColors(
            t.accent, t.textPrimary, t.dark ? 0.25 : 0.30);
        const QColor focus(t.accent.red(), t.accent.green(), t.accent.blue(),
                           220);
        const QColor accentSoft = mixColors(
            nested, t.accent, t.dark ? 0.20 : 0.14);

        // Keep the LCD recognisably glass, but calm the theme colour locally.
        // No other GlassPanel in the application is affected.
        if (auto* lcd = qobject_cast<ui::GlassPanel*>(m_lcdScreen))
            lcd->setAccentColor(
                mixColors(t.accent, t.surfaceElevated, 0.24));
        for (QWidget* group : {m_transportGroup, m_rightGroup}) {
            if (auto* glass = qobject_cast<ui::GlassPanel*>(group))
                glass->setAccentColor(
                    mixColors(t.accent, t.surfaceElevated, 0.55));
        }

        // Its own %1..%3, applied separately: adding them to the sheet below
        // would push it past nine markers again.
        const QString typeScale =
            QStringLiteral("#BarsPosition { font-size: %1px; }"
                           "#TempoField, #TimeSignatureButton { font-size: %2px; }"
                           "#GridChip { font-size: %3px; }")
                .arg(kPositionFontPx)
                .arg(kStatsFontPx)
                .arg(kChipFontPx);

        setStyleSheet(typeScale + QString(R"(
#TransportPill { background: transparent; }
#TransportGroup, #HeaderToolGroup {
    background: transparent; border: none;
}
#LcdScreen QLabel { color: %3; background: transparent; }
#PositionSection, #StatsSection {
    background: transparent; border: none; border-radius: 7px;
}
#BarsPosition, #TempoField, #TimeSignatureButton, #GridChip {
    background: transparent; border: 1px solid transparent; border-radius: 7px;
    padding: 0 2px; color: %3; selection-background-color: %8;
}
#BarsPosition, #TempoField { padding: 0; font-weight: 700; }
#BarsPosition:hover, #TempoField:hover, #TimeSignatureButton:hover, #GridChip:hover {
    background: %5;
}
#BarsPosition:focus, #TempoField:focus, #TimeSignatureButton:focus, #GridChip:focus {
    border-color: %6; background: %4;
}
#PrimaryToolChip, #SecondaryToolChip {
    background: %1; border: 1px solid %2; border-radius: 9px;
    padding: 0; color: %3;
}
#PrimaryToolChip:hover, #SecondaryToolChip:hover { background: %7; }
#PrimaryToolChip:focus, #SecondaryToolChip:focus { border-color: %6; }
#PrimaryToolChip { background: %9; border-color: %6; }
#GridChip::menu-indicator, #TimeSignatureButton::menu-indicator,
#PrimaryToolChip::menu-indicator, #SecondaryToolChip::menu-indicator {
    image: none; width: 0;
}
)")
            // Contiguous %1..%9, in this order — see the note above.
            .arg(nested.name(), panelBorder.name(), displayInk.name(),
                  field.name(), fieldHover.name(),
                  focus.name(QColor::HexArgb), hover.name(), t.accent.name(),
                  accentSoft.name()));
    }
    updatePositionStyle();
    // One label per reading in the 2x2 field, so the icons stay put while the
    // values beside them change width.
    const std::pair<QLabel*, const char*> statsIcons[] = {
        {m_tempoIcon, "metronome.svg"},
        {m_signatureIcon, "dots-nine.svg"},
        {m_gridIcon, "grid-four.svg"},
        {m_formatIcon, "clock.svg"}};
    for (const auto& [label, file] : statsIcons) {
        if (!label) continue;
        label->setPixmap(
            icons::svgIcon(QLatin1String(file), t.textSecondary, 16)
                .pixmap(QSize(16, 16)));
    }
    if (m_snapButton)
        m_snapButton->setIcon(
            icons::svgIcon(QStringLiteral("magnet-straight.svg"), t.textPrimary, 18));
    if (m_typingKeysButton)
        m_typingKeysButton->setIcon(
            icons::svgIcon(QStringLiteral("piano-keys.svg"), t.textPrimary, 18));
    if (m_metroButton)
        m_metroButton->setIcon(
            icons::svgIcon(QStringLiteral("metronome.svg"), t.textPrimary, 18));
    if (m_toolButton && m_toolIndex >= 0 && m_toolIndex < kToolCount)
        m_toolButton->setIcon(toolChipIcon(
            m_toolIndex,
            mixColors(t.accent, t.textPrimary, t.dark ? 0.12 : 0.30)));
    if (m_altToolButton && m_altToolIndex >= 0 && m_altToolIndex < kToolCount)
        m_altToolButton->setIcon(toolChipIcon(m_altToolIndex, t.textPrimary));
    for (int i = 0; i < m_toolActions.size() && i < kToolCount; ++i) {
        if (m_toolActions[i])
            m_toolActions[i]->setIcon(toolIcon(i, t.textPrimary, 14));
    }
    for (int i = 0; i < m_altToolActions.size() && i < kToolCount; ++i) {
        if (m_altToolActions[i])
            m_altToolActions[i]->setIcon(toolIcon(i, t.textPrimary, 14));
    }
    update();
}

void TransportBar::updatePositionStyle() {
    if (!m_positionGroup || !m_positionValue) return;
    const Theme& t = th();
    const QColor background = mixColors(t.well(), t.surfaceElevated, 0.26);
    // Brighter than the values around it: this socket is backlit, so its ink
    // has to stay ahead of its own glow.
    const QColor text = m_positionRecording
                            ? Theme::record()
                            : mixColors(t.accent, t.textPrimary,
                                        t.dark ? 0.10 : 0.34);
    const QColor hover = mixColors(
        background, m_positionRecording ? Theme::record() : t.accent, 0.14);
    const QColor focus = m_positionRecording ? Theme::record() : t.accent;
    const QString style =
        QString("#PositionSection { background: transparent; border: none; "
                "border-radius: 7px; } "
                "#BarsPosition { background: transparent; border: 1px solid "
                "transparent; border-radius: 7px; color: %1; padding: 0 7px; "
                "font-size: %5px; "
                "selection-background-color: %2; } "
                "#BarsPosition:hover { background: %3; } "
                "#BarsPosition:focus { border-color: %2; background: %4; }")
            .arg(text.name(), focus.name(), hover.name(QColor::HexArgb),
                  background.name(), QString::number(kPositionFontPx));
    m_positionGroup->setStyleSheet(style);
    if (m_positionIcon)
        m_positionIcon->setPixmap(
            icons::svgIcon(QStringLiteral("signpost.svg"), text, 18)
                .pixmap(QSize(18, 18)));
}

void TransportBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    // The header has its own colour (per-theme `headerBackground`); compact
    // glass modules float above it without changing the workspace geometry.
    p.fillRect(rect(), t.headerBackground);
    if (m_backgroundEnabled && m_backgroundVisibility > 0 &&
        m_backgroundMedia && m_backgroundMedia->hasFrame()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setOpacity(double(m_backgroundVisibility) / 100.0);
        p.drawPixmap(QPoint(0, 0), m_backgroundMedia->frame());
        p.setOpacity(1.0);
    }

    p.setPen(QPen(t.sectionDivider(), 1));
    p.drawLine(0, height() - 1, width(), height() - 1);
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
    const int completeCluster = layoutWidth(
        m_pill->layout(), {m_transportGroup, m_lcdScreen, m_rightGroup}) -
        widgetWidth(m_transportGroup) + coreTransport;

    // The fixed right well is broader than the left well's collapsed state,
    // so it defines the symmetric edge reserve without making a temporarily
    // expanded drawer inflate the application's permanent minimum width.
    constexpr int kOuterMargin = 14;
    constexpr int kDockGap = 10;
    const int edgeDock = m_rightDock ? widgetWidth(m_rightDock) : 0;
    return 2 * (kOuterMargin + kDockGap + edgeDock) + completeCluster;
}

void TransportBar::updateResponsiveLayout() {
    if (!m_pill) return;

    const auto setDisplayCompact = [this](bool compact) {
        if (m_positionGroup) m_positionGroup->setFixedWidth(compact ? 156 : 168);
        if (m_positionValue) m_positionValue->setFixedWidth(compact ? 124 : 136);
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
        // At the application's deliberately squeezed Web+AI test width there
        // is not enough horizontal space for the opened drawer and even the
        // irreducible transport readout at once. Keep the transport centred
        // and let the short-lived drawer overlay its far-left controls; moving
        // the timeline's principal readout would be the more disruptive
        // failure. At normal workspace widths the reserved wells never meet.
        x = std::max(0, x);
    }
    m_pill->setGeometry(x, (height() - m_pill->height()) / 2, pillWidth,
                        m_pill->height());
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
    const bool playing = m_controller->isPlaying();
    const bool recording = m_controller->isRecording();

    m_playButton->setGlyph(playing ? icons::Glyph::Pause : icons::Glyph::Play);
    m_playButton->setToolTip(playing ? tr("Pause") : tr("Play"));
    // Engaged and rolling both light the button; only the shade differs, so the
    // two states are never mistaken for each other.
    if (recording) m_recordEngaged = true;
    const bool lit = recording || m_recordEngaged;
    if (m_recordButton->isChecked() != lit) {
        QSignalBlocker block(m_recordButton);
        m_recordButton->setChecked(lit);
    }
    m_recordButton->setActiveColor(Theme::record());
    // Engaged glows: the take is set up in the context panel and starts on the
    // next press. Rolling is steady red — a light that breathes would be
    // saying "waiting" while the machine is already recording.
    m_recordButton->setPulse(m_recordEngaged && !recording);
    m_recordButton->setToolTip(recording  ? tr("Stop recording")
                               : m_recordEngaged
                                   ? tr("Record engaged — start from the panel "
                                        "or press R")
                                   : tr("Record"));

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
    m_tempoEdit->setText(tempoText(m_controller->tempo()));
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
