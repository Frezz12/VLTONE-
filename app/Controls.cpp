#include "Controls.hpp"
#include "EngineController.hpp"
#include "GlassPanel.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QCursor>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <iterator>
#include <cmath>

namespace ui {

namespace {
bool g_automationCreationMode = false;
}

void setAutomationCreationMode(bool enabled) {
    g_automationCreationMode = enabled;
}

bool automationCreationMode() { return g_automationCreationMode; }

namespace {
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kMinDb = -60.0;
constexpr double kMaxDb = 6.0;
constexpr double kRangeDb = kMaxDb - kMinDb;   // 66
constexpr double kTaper = 1.5;

double dbFromGain(double g) {
    return g <= 0.00001 ? -1000.0 : 20.0 * std::log10(g);
}

bool automationContextMenu(QWidget* owner, QContextMenuEvent* event) {
    QMenu menu(owner);
    QAction* create = menu.addAction(
        icons::icon(icons::Glyph::AutomationCreate, th().textPrimary),
        QObject::tr("Create Automation Clip"));
    const bool requested = menu.exec(event->globalPos()) == create;
    event->accept();
    return requested;
}
} // namespace

// Forwarded to the document layer, which owns the taper. A volume automation
// curve is drawn against the same scale, and two copies of these four lines
// would agree only until one of them was tuned.
double gainFromFaderPosition(double position) {
    return daw::gainFromNormalized(position);
}

double faderPositionFromGain(double gain) {
    return daw::normalizedFromGain(gain);
}

QString formatGainDb(double gain) {
    if (gain <= 0.00001) return QStringLiteral("−∞ dB");
    return QString::asprintf("%+.1f dB", dbFromGain(gain));
}

// ── The one slider look ────────────────────────────────────────────────────

namespace {

/// Along-axis start and end of a track, in painting order: for a vertical
/// slider the *start* of the throw is the bottom edge.
struct Axis {
    double start = 0.0;   // where position 0 lies
    double end = 0.0;     // where position 1 lies
};

Axis axisOf(const QRectF& track, Qt::Orientation orientation) {
    return orientation == Qt::Vertical ? Axis{track.bottom(), track.top()}
                                       : Axis{track.left(), track.right()};
}

/// A rect spanning `from`…`to` along the axis and the full thickness across it.
QRectF spanRect(const QRectF& track, Qt::Orientation orientation, double from,
                double to) {
    if (to < from) std::swap(from, to);
    return orientation == Qt::Vertical
               ? QRectF(track.left(), from, track.width(), to - from)
               : QRectF(from, track.top(), to - from, track.height());
}

/// The handle: a circle centred on `axis`, its diameter the groove's thickness
/// less the inset, so it rides *inside* the track rather than over it.
QRectF handleRect(const QRectF& track, Qt::Orientation orientation, double axis,
                  bool flush) {
    const double thickness =
        orientation == Qt::Vertical ? track.width() : track.height();
    const double d = flush ? thickness : sliderHandleDiameter(thickness);
    return orientation == Qt::Vertical
               ? QRectF(track.center().x() - d / 2.0, axis - d / 2.0, d, d)
               : QRectF(axis - d / 2.0, track.center().y() - d / 2.0, d, d);
}

/// The glass cap. Same construction as `ui::GlassPanel` — translucent body,
/// sheen over the top, rim graded from lit to shadowed — minus the captured
/// backdrop, which at this size would cost more than it shows. What it *does*
/// have behind it is the track: the accent fill reads straight through the
/// glass, which is what makes the handle look like part of the slider instead
/// of an object parked on it.
void paintGlassHandle(QPainter& p, const QRectF& handle, bool active,
                      const QColor& accent) {
    const Theme& t = th();
    const double radius = std::min(handle.width(), handle.height()) / 2.0;

    // A dark contact ring first, so the pane has an edge against both halves of
    // the track it rides between — the lit accent below it and the empty groove
    // above. Without it the glass dissolves into whichever it happens to be on.
    QColor contact(0, 0, 0);
    contact.setAlphaF(t.dark ? 0.55 : 0.28);
    p.setPen(Qt::NoPen);
    p.setBrush(contact);
    p.drawRoundedRect(handle.adjusted(-0.7, -0.7, 0.7, 0.7), radius + 0.7,
                      radius + 0.7);

    // Milky enough to stay one object over both halves of the track — a glass
    // that let the accent through unchecked read as a half-lit moon, since the
    // handle always straddles the boundary between the filled and empty groove.
    QLinearGradient body(handle.topLeft(), handle.bottomLeft());
    QColor top(255, 255, 255);
    QColor bottom(255, 255, 255);
    if (t.dark) {
        top.setAlphaF(active ? 0.86 : 0.74);
        bottom.setAlphaF(active ? 0.66 : 0.54);
    } else {
        top.setAlphaF(active ? 0.98 : 0.92);
        bottom.setAlphaF(active ? 0.86 : 0.76);
    }
    body.setColorAt(0.0, top);
    body.setColorAt(1.0, bottom);
    p.setBrush(body);
    p.drawRoundedRect(handle, radius, radius);

    QLinearGradient sheen(handle.topLeft(),
                          QPointF(handle.left(),
                                  handle.top() + handle.height() * 0.60));
    QColor lit(255, 255, 255);
    lit.setAlphaF(t.dark ? 0.16 : 0.45);
    sheen.setColorAt(0.0, lit);
    lit.setAlphaF(0.0);
    sheen.setColorAt(1.0, lit);
    p.setBrush(sheen);
    p.drawRoundedRect(handle.adjusted(0.6, 0.6, -0.6, 0.0), radius, radius);

    // One small specular, up and to the left, where the light in this UI comes
    // from. It is what tells a circle from a disc.
    QRadialGradient spot(QPointF(handle.center().x() - radius * 0.34,
                                 handle.center().y() - radius * 0.40),
                         radius * 1.05);
    QColor hot(255, 255, 255);
    hot.setAlphaF(t.dark ? 0.55 : 0.85);
    spot.setColorAt(0.0, hot);
    hot.setAlphaF(0.0);
    spot.setColorAt(0.75, hot);
    p.setBrush(spot);
    p.drawRoundedRect(handle, radius, radius);

    QLinearGradient edge(handle.topLeft(), handle.bottomLeft());
    QColor edgeTop = mixColors(QColor(255, 255, 255), accent, 0.30);
    edgeTop.setAlphaF(t.dark ? (active ? 0.85 : 0.62) : 0.95);
    QColor edgeMid = accent;
    edgeMid.setAlphaF(active ? 0.55 : 0.30);
    QColor edgeBottom(0, 0, 0);
    edgeBottom.setAlphaF(t.dark ? 0.45 : 0.22);
    edge.setColorAt(0.0, edgeTop);
    edge.setColorAt(0.5, edgeMid);
    edge.setColorAt(1.0, edgeBottom);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(edge), 1.0));
    p.drawRoundedRect(handle.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

} // namespace

double sliderHandleDiameter(double trackThickness) {
    // Two pixels of groove all the way round the handle: enough that the track
    // reads as continuous behind it, not so much that the handle looks lost in
    // it. Never below 8, so a small flyout track still gets a real target.
    return std::max(8.0, trackThickness - 4.0);
}

double sliderHandleAxis(const QRectF& track, Qt::Orientation orientation,
                        double position, bool flush) {
    const Axis axis = axisOf(track, orientation);
    const double thickness =
        orientation == Qt::Vertical ? track.width() : track.height();
    const double half =
        (flush ? thickness : sliderHandleDiameter(thickness)) / 2.0;
    const double from = axis.start + (axis.end > axis.start ? half : -half);
    const double to = axis.end + (axis.end > axis.start ? -half : half);
    return from + (to - from) * std::clamp(position, 0.0, 1.0);
}

double sliderPositionAt(const QRectF& track, Qt::Orientation orientation,
                        double coordinate, bool flush) {
    const Axis axis = axisOf(track, orientation);
    const double thickness =
        orientation == Qt::Vertical ? track.width() : track.height();
    const double half =
        (flush ? thickness : sliderHandleDiameter(thickness)) / 2.0;
    const double from = axis.start + (axis.end > axis.start ? half : -half);
    const double to = axis.end + (axis.end > axis.start ? -half : half);
    if (std::abs(to - from) < 1e-6) return 0.0;
    return std::clamp((coordinate - from) / (to - from), 0.0, 1.0);
}

void paintSlider(QPainter& p, const QRectF& track, const SliderPaint& spec) {
    const Theme& t = th();
    const QColor accent = spec.accent.isValid() ? spec.accent : t.accent;
    const bool vertical = spec.orientation == Qt::Vertical;
    const double thickness = vertical ? track.width() : track.height();
    const double radius = thickness / 2.0;
    const Axis axis = axisOf(track, spec.orientation);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    // ── The track ──
    // Recessed, but by shading alone: a darker lip on the lit side and nothing
    // else. No bevel, no cast shadow — the depth cue is that the glass on top
    // is the only thing catching the light.
    QLinearGradient bed(track.topLeft(),
                        vertical ? track.topRight() : track.bottomLeft());
    bed.setColorAt(0.0, mixColors(t.well(), QColor(0, 0, 0), t.dark ? 0.45 : 0.16));
    bed.setColorAt(0.55, t.well());
    bed.setColorAt(1.0, mixColors(t.well(), t.textPrimary, t.dark ? 0.07 : 0.04));
    p.setPen(Qt::NoPen);
    p.setBrush(bed);
    p.drawRoundedRect(track, radius, radius);

    // ── The value ──
    const double handleAxis =
        sliderHandleAxis(track, spec.orientation, spec.position, spec.flush);
    const double fillFrom = spec.fillFrom <= 0.0   ? axis.start
                            : spec.fillFrom >= 1.0 ? axis.end
                                                   : sliderHandleAxis(
                                                         track, spec.orientation,
                                                         spec.fillFrom,
                                                         spec.flush);
    QLinearGradient value(vertical ? track.center().x() : fillFrom,
                          vertical ? fillFrom : track.center().y(),
                          vertical ? track.center().x() : handleAxis,
                          vertical ? handleAxis : track.center().y());
    value.setColorAt(0.0, mixColors(accent, t.background, 0.22));
    value.setColorAt(1.0, spec.active ? t.accentHighlight : accent);
    p.setBrush(value);
    p.drawRoundedRect(spanRect(track, spec.orientation, fillFrom, handleAxis),
                      radius, radius);

    // ── The detent ──
    // Inside the track, not a pair of ticks hung outside it: at this thickness
    // there is room for the mark where it belongs, and the fader stops needing
    // clearance around itself.
    if (spec.detent >= 0.0) {
        const double at =
            sliderHandleAxis(track, spec.orientation, spec.detent, spec.flush);
        QColor mark = t.ink(t.dark ? 70 : 90);
        p.setPen(QPen(mark, 1.0));
        if (vertical) {
            p.drawLine(QPointF(track.left() + 3.0, at),
                       QPointF(track.right() - 3.0, at));
        } else {
            p.drawLine(QPointF(at, track.top() + 3.0),
                       QPointF(at, track.bottom() - 3.0));
        }
    }

    paintGlassHandle(p, handleRect(track, spec.orientation, handleAxis, spec.flush),
                     spec.active, accent);
    p.restore();
}

// ── ThemedWidget ──

ThemedWidget::ThemedWidget(QWidget* parent) : QWidget(parent) {
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

// ── Fade ──

Fade::Fade(QWidget* owner, int durationMs) : m_owner(owner) {
    m_anim = new QVariantAnimation(owner);
    m_anim->setDuration(durationMs);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(m_anim, &QVariantAnimation::valueChanged, owner,
                     [this](const QVariant& v) {
                         m_value = v.toDouble();
                         m_owner->update();
                     });
}

void Fade::jumpTo(double value) {
    m_anim->stop();
    m_value = value;
    m_owner->update();
}

void Fade::setTarget(double target) {
    if (std::abs(target - m_value) < 0.001 &&
        m_anim->state() != QAbstractAnimation::Running) {
        return;
    }
    if (m_anim->state() == QAbstractAnimation::Running &&
        std::abs(m_anim->endValue().toDouble() - target) < 0.001) {
        return;   // already heading there
    }
    m_anim->stop();
    m_anim->setStartValue(m_value);
    m_anim->setEndValue(target);
    m_anim->start();
}

// ── ValueBubble ──

namespace {

/// The one bubble instance. A top-level tool-tip window so it can float above
/// controls that have no room for it inside their own rect.
class BubbleWindow : public QWidget {
public:
    BubbleWindow()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint |
                               Qt::NoDropShadowWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }

    void setText(const QString& text) {
        m_text = text;
        QFont f = font();
        f.setPixelSize(11);
        f.setBold(true);
        setFont(f);
        const int w = QFontMetrics(f).horizontalAdvance(text) + 16;
        resize(std::max(38, w), 22);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(t.accent, 1));
        p.setBrush(mixColors(t.surfaceElevated, t.background, 0.15));
        p.drawRoundedRect(r, 6, 6);
        p.setPen(t.textPrimary);
        p.drawText(r, Qt::AlignCenter, m_text);
    }

private:
    QString m_text;
};

BubbleWindow* bubble() {
    static BubbleWindow* instance = new BubbleWindow;
    return instance;
}

}  // namespace

void ValueBubble::showFor(QWidget* owner, const QPoint& anchor,
                          const QString& text) {
    BubbleWindow* b = bubble();
    b->setText(text);
    const QPoint global = owner->mapToGlobal(anchor);
    b->move(global.x() - b->width() / 2, global.y() - b->height() - 8);
    b->show();
    b->raise();
}

void ValueBubble::dismiss() {
    if (bubble()->isVisible()) bubble()->hide();
}

// ── IconButton ──

IconButton::IconButton(icons::Glyph glyph, const QString& tip, QWidget* parent)
    : QAbstractButton(parent), m_glyph(glyph) {
    setToolTip(tip);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(28, 24);
    setFocusPolicy(Qt::NoFocus);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void IconButton::setGlyph(icons::Glyph glyph) {
    if (m_glyph == glyph) return;
    m_glyph = glyph;
    update();
}

void IconButton::setProminent(bool on) {
    if (m_prominent == on) return;
    m_prominent = on;
    update();
}

void IconButton::setButtonSize(int w, int h) { setFixedSize(w, h); }

void IconButton::setPulse(bool on) {
    if (m_pulse == on) return;
    m_pulse = on;
    if (!on) {
        if (m_pulseAnim) m_pulseAnim->stop();
        m_pulseValue = 0.0;
        update();
        return;
    }
    if (!m_pulseAnim) {
        // Symmetric by construction: 0 → 1 → 0 in one loop, so the breath in
        // and the breath out take the same time without reversing direction.
        m_pulseAnim = new QVariantAnimation(this);
        m_pulseAnim->setDuration(1300);
        m_pulseAnim->setLoopCount(-1);
        m_pulseAnim->setKeyValueAt(0.0, 0.0);
        m_pulseAnim->setKeyValueAt(0.5, 1.0);
        m_pulseAnim->setKeyValueAt(1.0, 0.0);
        m_pulseAnim->setEasingCurve(QEasingCurve::InOutSine);
        connect(m_pulseAnim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& v) {
                    m_pulseValue = v.toDouble();
                    update();
                });
    }
    m_pulseAnim->start();
}

void IconButton::enterEvent(QEnterEvent*) { m_hoverFade.setTarget(1.0); }
void IconButton::leaveEvent(QEvent*) { m_hoverFade.setTarget(0.0); }

void IconButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // isDown() flips instantly; the fade gives the press its release tail, so a
    // quick click still reads as a press rather than a flicker.
    m_pressFade.setTarget(isDown() ? 1.0 : 0.0);
    const double hover = m_hoverFade.value();
    const double press = m_pressFade.value();

    const Theme& t = th();
    const QColor active = m_activeColor.isValid() ? m_activeColor : t.accent;
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = m_prominent ? r.height() / 2.0 : 6.0;

    auto plate = [&](const QColor& c) {
        if (c.alpha() <= 0) return;
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(r, radius, radius);
    };

    QColor fill(Qt::transparent);
    if (isChecked()) {
        fill = QColor(active.red(), active.green(), active.blue(), 62);
    } else if (m_prominent) {
        // A prominent button is filled in whatever it means, not always the
        // theme accent: the panel's Start chip is a record button and has to
        // be red the way every other record control is.
        fill = active;
    }
    // A pulsing button glows from its own plate outwards: the fill deepens and
    // a soft halo grows past the edge, so it reads as lit and waiting rather
    // than merely a different shade of the same button.
    if (m_pulse) {
        const double g = m_pulseValue;
        fill = QColor(active.red(), active.green(), active.blue(),
                      int(70 + 150 * g));
        for (int ring = 3; ring >= 1; --ring) {
            QColor halo = active;
            halo.setAlphaF(0.16 * g / ring);
            p.setPen(QPen(halo, 2.0));
            p.setBrush(Qt::NoBrush);
            const qreal grow = 1.5 * ring;
            p.drawRoundedRect(r.adjusted(-grow, -grow, grow, grow),
                              radius + grow, radius + grow);
        }
    }
    plate(fill);
    if (m_pulse) {
        QColor edge = active;
        edge.setAlphaF(0.55 + 0.4 * m_pulseValue);
        p.setPen(QPen(edge, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius, radius);
    }

    // Hover lifts the button towards the light; the press darkens it, which is
    // what makes the button feel pushed in rather than just recoloured.
    if (hover > 0.005) {
        QColor lift = m_prominent ? QColor(255, 255, 255)
                                  : (isChecked() ? active : t.textPrimary);
        lift.setAlphaF(m_prominent ? 0.16 * hover : 0.13 * hover);
        plate(lift);
    }
    if (press > 0.005) {
        QColor sink(0, 0, 0);
        sink.setAlphaF(0.22 * press);
        plate(sink);
    }

    QColor tint = (isChecked() || m_pulse)
                      ? active
                      : (m_prominent ? Qt::white : t.textPrimary);
    if (m_accentTint && !isChecked() && !m_prominent) tint = t.accent;
    if (!isEnabled()) tint = t.textSecondary;

    // The glyph shrinks a hair under the press — the physical part of the
    // "вдавливание".
    const qreal side = std::min(r.width(), r.height()) *
                       (m_prominent ? 0.74 : 0.68) * (1.0 - 0.07 * press);
    const QRectF box(r.center().x() - side / 2, r.center().y() - side / 2,
                     side, side);
    icons::paint(p, m_glyph, box, tint);

    // Most dense DAW controls deliberately opt out of Tab focus, but panels
    // with form-like navigation can opt back in. When they do, the focus must
    // be as visible as the hover state rather than existing only to Qt.
    if (hasFocus()) {
        QColor ring = t.accentHighlight;
        ring.setAlphaF(0.9);
        p.setPen(QPen(ring, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), radius - 1.0,
                          radius - 1.0);
    }
}

// ── MsrButton ──

MsrButton::MsrButton(const QString& letter, const QColor& activeColor,
                     const QString& tip, QWidget* parent)
    : QAbstractButton(parent), m_letter(letter), m_active(activeColor) {
    setCheckable(true);
    setToolTip(tip);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(22, 18);
    setFocusPolicy(Qt::NoFocus);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void MsrButton::enterEvent(QEnterEvent*) { m_hoverFade.setTarget(1.0); }
void MsrButton::leaveEvent(QEvent*) { m_hoverFade.setTarget(0.0); }

void MsrButton::setChipSize(int w, int h) {
    setFixedSize(w, h);
    update();
}

void MsrButton::setLetter(const QString& letter) {
    if (m_letter == letter) return;
    m_letter = letter;
    update();
}

void MsrButton::setAutoMark(bool on) {
    if (m_autoMark == on) return;
    m_autoMark = on;
    update();
}

void MsrButton::setActiveColor(const QColor& color) {
    if (m_active == color) return;
    m_active = color;
    update();
}

void MsrButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    m_pressFade.setTarget(isDown() ? 1.0 : 0.0);
    const double hover = m_hoverFade.value();
    const double press = m_pressFade.value();
    const Theme& t = th();
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QColor fill = isChecked() ? m_active : t.well();
    fill = mixColors(fill, t.textPrimary, (isChecked() ? 0.12 : 0.16) * hover);
    fill = mixColors(fill, QColor(0, 0, 0), 0.20 * press);

    p.setPen(QPen(isChecked() ? m_active.darker(115) : t.separator(), 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 4, 4);

    QFont f = font();
    // Tied to the chip rather than fixed: the track headers run these at 14 px
    // tall, where a 10 px letter has no room to breathe inside its border.
    f.setPixelSize(std::clamp(height() - 6, 8, 10));
    f.setBold(true);
    p.setFont(f);
    // Lit chips carry dark text so the colour reads as a lamp, not a label.
    p.setPen(isChecked() ? QColor(20, 20, 20) : t.textSecondary);
    p.drawText(r, Qt::AlignCenter, m_letter);

    // The auto mark sits in the corner rather than beside the chip: it is a
    // note about who is driving this button, not a second control, and the
    // track header has no width to spare.
    if (m_autoMark) {
        QFont af = f;
        af.setPixelSize(7);
        p.setFont(af);
        const QRectF corner(r.right() - 7.0, r.top() + 0.5, 6.0, 6.0);
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? QColor(20, 20, 20, 140) : t.accent);
        p.drawEllipse(corner);
        p.setPen(isChecked() ? QColor(235, 235, 235) : QColor(20, 20, 20));
        p.drawText(corner, Qt::AlignCenter, QStringLiteral("A"));
    }
}

void MsrButton::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_automatable) {
        QAbstractButton::contextMenuEvent(event);
        return;
    }
    if (automationContextMenu(this, event)) emit automateRequested();
}

// ── FaderWidget ──

namespace {

/// The scale, in dB, exactly as it is printed down a Logic channel: dense where
/// the fader is used and sparse at the bottom, which is what a dB taper makes
/// of an evenly spaced set of marks anyway.
constexpr double kFaderScale[] = {6, 3, 0, -3, -6, -9, -12, -18,
                                  -24, -30, -40, -50, -60};
/// Width the printed scale takes: labels, then the ticks against the slot.
constexpr double kScaleWidth = 22.0;
constexpr double kTickLength = 5.0;
/// The cap, at Logic's proportions: wide enough to read, short enough that the
/// slot either side of it still tells you where it is.
constexpr double kCapAlong = 16.0;
constexpr double kCapAcross = 24.0;
/// The slot the cap rides in.
constexpr double kSlotThickness = 4.0;
/// Closest two printed numbers may come. Both the thinning in `paintScale` and
/// the fader's minimum height are derived from it, so "the shortest fader that
/// still prints every number" is one fact with one definition.
constexpr double kMinLabelGap = 8.0;
/// Room above the first mark, where the unit is printed.
constexpr double kScaleHeadroom = 11.0;

double positionForDb(double db) {
    return faderPositionFromGain(std::pow(10.0, db / 20.0));
}

/// The shortest travel that still prints every number on the scale: the taper
/// bunches the marks tightest around −9…−12, so that pair decides it.
double travelForFullScale() {
    double tightest = 1.0;
    for (std::size_t i = 1; i < std::size(kFaderScale); ++i) {
        tightest = std::min(tightest, std::abs(positionForDb(kFaderScale[i]) -
                                               positionForDb(kFaderScale[i - 1])));
    }
    return kMinLabelGap / std::max(tightest, 1e-6);
}

} // namespace

FaderWidget::FaderWidget(Qt::Orientation orientation, QWidget* parent)
    : QWidget(parent), m_orientation(orientation) {
    if (orientation == Qt::Vertical) {
        setMinimumSize(int(kCapAcross + 4), 90);
    } else {
        setMinimumSize(80, 22);
        setFixedHeight(22);
    }
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("Drag to set level · double-click for unity · Shift for fine"));
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void FaderWidget::setGain(double gain) {
    const double g = std::clamp(gain, 0.0, 2.0);
    if (std::abs(g - m_gain) < 1e-6) return;
    m_gain = g;
    update();
}

void FaderWidget::setScaleVisible(bool visible) {
    if (m_scale == visible || m_orientation != Qt::Vertical) return;
    m_scale = visible;
    setMinimumWidth(int(kCapAcross + 4 + (visible ? kScaleWidth : 0.0)));
    // A printed scale with numbers missing from it is worse than no scale: the
    // marks stop being a ruler and become decoration. So a fader that carries
    // one is never shorter than the height every number fits in — the mixer
    // pane scrolls instead, which is the honest trade.
    setMinimumHeight(visible ? int(std::ceil(travelForFullScale() + kCapAlong +
                                             kScaleHeadroom + 1.0))
                             : 90);
    updateGeometry();
    update();
}

void FaderWidget::setCompactKnob(bool compact) {
    if (m_orientation != Qt::Horizontal || m_compactKnob == compact) return;
    constexpr int kSide = 24;
    if (compact) {
        m_regularMinimumWidth = minimumWidth();
        m_regularMaximumWidth = maximumWidth();
        setMinimumWidth(kSide);
        setMaximumWidth(kSide);
        setFixedHeight(kSide);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    } else {
        setMinimumWidth(std::max(1, m_regularMinimumWidth));
        setMaximumWidth(m_regularMaximumWidth);
        setFixedHeight(22);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    m_compactKnob = compact;
    updateGeometry();
    update();
}

QRectF FaderWidget::faderColumn() const {
    const QRectF r(rect());
    if (m_orientation != Qt::Vertical || !m_scale) return r;
    return r.adjusted(kScaleWidth, 0, 0, 0);
}

QSizeF FaderWidget::capSize() const {
    if (m_orientation == Qt::Vertical) return QSizeF(kCapAcross, kCapAlong);
    // Laid on its side the cap has to fit the header row, which is shorter than
    // the mixer's cap is wide: it keeps its proportions, scaled to fit.
    const double across = std::min(kCapAcross, double(height()) - 2.0);
    return QSizeF(kCapAlong - 2.0, across);
}

QRectF FaderWidget::trackRect() const {
    const QRectF column = faderColumn();
    // The scale wants a line above its first mark for the unit, exactly as it
    // is printed on the console, so the throw starts below it.
    const double top = m_scale ? kScaleHeadroom : 1.0;
    return m_orientation == Qt::Vertical
               ? QRectF(column.center().x() - kSlotThickness / 2.0, column.top() + top,
                        kSlotThickness, column.height() - top - 1.0)
               : QRectF(column.left() + 1.0,
                        column.center().y() - kSlotThickness / 2.0,
                        column.width() - 2.0, kSlotThickness);
}

double FaderWidget::travel() const {
    const QRectF track = trackRect();
    const QSizeF cap = capSize();
    return std::max(1.0, (m_orientation == Qt::Vertical
                              ? track.height() - cap.height()
                              : track.width() - cap.width()));
}

double FaderWidget::knobPos(double position) const {
    const QRectF track = trackRect();
    const QSizeF cap = capSize();
    const double clamped = std::clamp(position, 0.0, 1.0);
    if (m_orientation == Qt::Vertical) {
        const double bottom = track.bottom() - cap.height() / 2.0;
        return bottom - travel() * clamped;
    }
    return track.left() + cap.width() / 2.0 + travel() * clamped;
}

QRectF FaderWidget::capRect(double position) const {
    const QSizeF cap = capSize();
    const double axis = knobPos(position);
    const QRectF column = faderColumn();
    return m_orientation == Qt::Vertical
               ? QRectF(column.center().x() - cap.width() / 2.0,
                        axis - cap.height() / 2.0, cap.width(), cap.height())
               : QRectF(axis - cap.width() / 2.0,
                        column.center().y() - cap.height() / 2.0, cap.width(),
                        cap.height());
}

void FaderWidget::paintScale(QPainter& p) const {
    const Theme& t = th();
    QFont f = p.font();
    f.setPixelSize(7);
    f.setLetterSpacing(QFont::PercentageSpacing, 102);
    p.setFont(f);

    const double tickRight = kScaleWidth - 2.0;
    const double tickLeft = tickRight - kTickLength;
    const QColor tick = mixColors(t.textSecondary, t.background, 0.35);
    const QColor text = mixColors(t.textSecondary, t.background, 0.15);

    // The unit, once, at the head of the column — the marks below it are then
    // just numbers, which is how it reads on the panel.
    p.setPen(text);
    p.drawText(QRectF(0.0, 0.0, tickRight, 9.0), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("dB"));

    // Every mark gets its tick; the numbers are thinned out to whatever the
    // fader is tall enough to print. Unity is chosen first and the rest are
    // spaced away from it in both directions, so the one number that has to be
    // legible is never the one that gets dropped.
    constexpr int kCount = int(std::size(kFaderScale));
    double y[kCount];
    int unityIndex = 0;
    for (int i = 0; i < kCount; ++i) {
        y[i] = knobPos(positionForDb(kFaderScale[i]));
        if (std::abs(kFaderScale[i]) < 0.001) unityIndex = i;
    }
    bool labelled[kCount] = {};
    labelled[unityIndex] = true;
    double last = y[unityIndex];
    for (int i = unityIndex - 1; i >= 0; --i) {
        if (last - y[i] < kMinLabelGap) continue;
        labelled[i] = true;
        last = y[i];
    }
    last = y[unityIndex];
    for (int i = unityIndex + 1; i < kCount; ++i) {
        if (y[i] - last < kMinLabelGap) continue;
        labelled[i] = true;
        last = y[i];
    }

    for (int i = 0; i < kCount; ++i) {
        const double db = kFaderScale[i];
        const bool major = i == unityIndex;
        p.setPen(QPen(major ? mixColors(tick, t.textPrimary, 0.45) : tick,
                      major ? 1.2 : 1.0));
        p.drawLine(QPointF(major ? tickLeft - 2.0 : tickLeft, y[i]),
                   QPointF(tickRight, y[i]));
        if (!labelled[i]) continue;
        p.setPen(major ? mixColors(text, t.textPrimary, 0.5) : text);
        p.drawText(QRectF(0.0, y[i] - 5.0, tickLeft - 4.0, 10.0),
                   Qt::AlignRight | Qt::AlignVCenter,
                   db > 0 ? QStringLiteral("+%1").arg(db) : QString::number(db));
    }
}

void FaderWidget::paintCap(QPainter& p, const QRectF& cap) const {
    const Theme& t = th();
    const bool vertical = m_orientation == Qt::Vertical;
    const bool lit = m_dragging || m_hovered;

    // It sits *on* the slot, so it gets a real shadow under it — the one place
    // in this UI where something is genuinely on top of something else.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, t.dark ? 120 : 55));
    p.drawRoundedRect(cap.translated(0.0, 1.6), 3.0, 3.0);

    // Milled metal: bright along the lit edge, falling away across the cap,
    // with the two halves of the face split by the finger groove. On a light
    // palette the same mix lands as brushed aluminium rather than silver.
    const QColor bright = mixColors(t.surfaceElevated, t.textPrimary,
                                    t.dark ? 0.82 : 0.34);
    const QColor mid = mixColors(t.surfaceElevated, t.textPrimary,
                                 t.dark ? 0.55 : 0.20);
    const QColor dim = mixColors(t.surfaceElevated, QColor(0, 0, 0),
                                 t.dark ? 0.30 : 0.06);
    QLinearGradient metal(cap.topLeft(),
                          vertical ? cap.bottomLeft() : cap.topRight());
    metal.setColorAt(0.00, mixColors(bright, QColor(255, 255, 255), 0.25));
    metal.setColorAt(0.18, bright);
    metal.setColorAt(0.50, mid);
    metal.setColorAt(0.52, dim);
    metal.setColorAt(0.62, mid);
    metal.setColorAt(1.00, dim);
    p.setBrush(metal);
    p.drawRoundedRect(cap, 2.5, 2.5);

    // The milling: hairlines across the grip, skipping the groove. Drawn inside
    // a clip so they cannot escape the rounded corners.
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(cap, 2.5, 2.5);
    p.setClipPath(clip);
    QColor line = t.ink(t.dark ? 34 : 26);
    QColor shade(0, 0, 0, t.dark ? 46 : 26);
    const double from = vertical ? cap.top() : cap.left();
    const double to = vertical ? cap.bottom() : cap.right();
    const double centre = (from + to) / 2.0;
    for (double at = from + 2.0; at < to - 1.0; at += 2.0) {
        if (std::abs(at - centre) < 2.0) continue;
        p.setPen(QPen(line, 1.0));
        if (vertical) {
            p.drawLine(QPointF(cap.left() + 2.0, at), QPointF(cap.right() - 2.0, at));
            p.setPen(QPen(shade, 1.0));
            p.drawLine(QPointF(cap.left() + 2.0, at + 1.0),
                       QPointF(cap.right() - 2.0, at + 1.0));
        } else {
            p.drawLine(QPointF(at, cap.top() + 2.0), QPointF(at, cap.bottom() - 2.0));
            p.setPen(QPen(shade, 1.0));
            p.drawLine(QPointF(at + 1.0, cap.top() + 2.0),
                       QPointF(at + 1.0, cap.bottom() - 2.0));
        }
    }

    // The groove across the middle: where the finger sits, and the line the
    // value is actually read against.
    p.setPen(QPen(QColor(0, 0, 0, t.dark ? 170 : 90), 1.4));
    if (vertical) {
        p.drawLine(QPointF(cap.left() + 1.0, centre), QPointF(cap.right() - 1.0, centre));
        p.setPen(QPen(t.ink(t.dark ? 90 : 120), 1.0));
        p.drawLine(QPointF(cap.left() + 1.0, centre + 1.2),
                   QPointF(cap.right() - 1.0, centre + 1.2));
    } else {
        p.drawLine(QPointF(centre, cap.top() + 1.0), QPointF(centre, cap.bottom() - 1.0));
        p.setPen(QPen(t.ink(t.dark ? 90 : 120), 1.0));
        p.drawLine(QPointF(centre + 1.2, cap.top() + 1.0),
                   QPointF(centre + 1.2, cap.bottom() - 1.0));
    }

    // A film of glass over the face, so the cap belongs to this UI and not to
    // 2004: one soft sheen across the lit half, nothing more.
    QLinearGradient sheen(cap.topLeft(),
                          vertical ? QPointF(cap.left(), centre)
                                   : QPointF(centre, cap.top()));
    QColor gloss(255, 255, 255);
    gloss.setAlphaF(t.dark ? 0.13 : 0.34);
    sheen.setColorAt(0.0, gloss);
    gloss.setAlphaF(0.0);
    sheen.setColorAt(1.0, gloss);
    p.setPen(Qt::NoPen);
    p.setBrush(sheen);
    p.drawRect(cap);
    p.restore();

    // Touched, in our accent rather than Logic's blue.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(lit ? t.accent : QColor(0, 0, 0, t.dark ? 190 : 90),
                  lit ? 1.4 : 1.0));
    p.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), 2.5, 2.5);
}

void FaderWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    const bool vertical = m_orientation == Qt::Vertical;
    const double position = faderPositionFromGain(m_gain);

    if (m_compactKnob) {
        const double side = std::min(width(), height()) - 3.0;
        const QRectF ring((width() - side) / 2.0, (height() - side) / 2.0,
                          side, side);
        const QRectF body = ring.adjusted(3.0, 3.0, -3.0, -3.0);
        const bool lit = m_dragging || m_hovered;

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, t.dark ? 105 : 42));
        p.drawEllipse(body.translated(0.0, 1.0));
        QRadialGradient face(body.center(), body.width() / 2.0);
        face.setColorAt(0.0, mixColors(t.surfaceElevated, t.textPrimary,
                                      t.dark ? 0.10 : 0.05));
        face.setColorAt(1.0, mixColors(t.well(), QColor(0, 0, 0),
                                      t.dark ? 0.28 : 0.06));
        p.setBrush(face);
        p.setPen(QPen(t.separator(), 1.0));
        p.drawEllipse(body);

        QPen arcPen(mixColors(t.separator(), t.background, 0.22), 2.1,
                    Qt::SolidLine, Qt::RoundCap);
        p.setPen(arcPen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(ring, 225 * 16, -270 * 16);
        arcPen.setColor(lit ? t.accent
                            : mixColors(t.accent, t.textSecondary, 0.18));
        p.setPen(arcPen);
        p.drawArc(ring, 225 * 16,
                  int(std::lround(-270.0 * position * 16.0)));

        const double angle = (225.0 - 270.0 * position) * kDegToRad;
        const double radius = body.width() * 0.31;
        const QPointF end = body.center() +
            QPointF(std::cos(angle) * radius, -std::sin(angle) * radius);
        p.setPen(QPen(lit ? t.accent : t.textPrimary, 1.7,
                      Qt::SolidLine, Qt::RoundCap));
        p.drawLine(body.center(), end);
        return;
    }

    const QRectF track = trackRect();
    const double axis = knobPos(position);

    if (m_scale && vertical) paintScale(p);

    // The slot: cut into the panel, not laid on it.
    p.setPen(Qt::NoPen);
    p.setBrush(mixColors(t.well(), QColor(0, 0, 0), t.dark ? 0.55 : 0.18));
    p.drawRoundedRect(track, 2.0, 2.0);
    p.setPen(QPen(t.ink(t.dark ? 22 : 40), 1.0));
    if (vertical) {
        p.drawLine(QPointF(track.right() - 0.5, track.top() + 2.0),
                   QPointF(track.right() - 0.5, track.bottom() - 2.0));
    } else {
        p.drawLine(QPointF(track.left() + 2.0, track.bottom() - 0.5),
                   QPointF(track.right() - 2.0, track.bottom() - 0.5));
    }

    // Ours: the travelled part of the slot is lit. Logic leaves it dark; this
    // is the one place the accent belongs on a fader, and it is what makes a
    // wall of them readable at a glance.
    const QColor lamp = mixColors(t.accent, t.background, m_dragging ? 0.0 : 0.18);
    p.setPen(Qt::NoPen);
    p.setBrush(lamp);
    if (vertical) {
        p.drawRoundedRect(QRectF(track.left(), axis, track.width(),
                                 track.bottom() - axis),
                          2.0, 2.0);
    } else {
        p.drawRoundedRect(QRectF(track.left(), track.top(), axis - track.left(),
                                 track.height()),
                          2.0, 2.0);
    }

    // Unity, marked on the panel beside the slot rather than on the scale — a
    // header fader has no scale, and it is the one level worth finding blind.
    if (!m_scale) {
        const double unity = knobPos(faderPositionFromGain(1.0));
        p.setPen(QPen(mixColors(t.textSecondary, t.background, 0.3), 1.0));
        if (vertical) {
            const QRectF column = faderColumn();
            p.drawLine(QPointF(column.center().x() + kCapAcross / 2.0 - 2.0, unity),
                       QPointF(column.center().x() + kCapAcross / 2.0 + 1.0, unity));
        } else {
            p.drawLine(QPointF(unity, track.top() - 3.0),
                       QPointF(unity, track.top() - 1.0));
        }
    }

    paintCap(p, capRect(position));
}

void FaderWidget::showBubble() {
    // Anchored to the knob, not the cursor, so the readout tracks the value
    // rather than the hand.
    const double axis = m_compactKnob
                            ? width() / 2.0
                            : knobPos(faderPositionFromGain(m_gain));
    const QPoint anchor = m_compactKnob
                              ? QPoint(width() / 2, 0)
                              : (m_orientation == Qt::Vertical
                                     ? QPoint(width() / 2, int(axis) - 8)
                                     : QPoint(int(axis), 0));
    ValueBubble::showFor(this, anchor, formatGainDb(m_gain));
}

void FaderWidget::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_dragStartPosition = faderPositionFromGain(m_gain);
    m_dragStartCoord = int((m_orientation == Qt::Vertical || m_compactKnob)
                               ? ev->position().y()
                               : ev->position().x());
    setCursor(Qt::ClosedHandCursor);
    showBubble();
}

void FaderWidget::mouseMoveEvent(QMouseEvent* ev) {
    if (!m_dragging) return;
    const double sensitivity =
        (ev->modifiers() & Qt::ShiftModifier) ? 4.0 : 1.0;
    const double moved = ((m_orientation == Qt::Vertical || m_compactKnob)
                              ? ev->position().y()
                              : ev->position().x()) -
                         m_dragStartCoord;
    const double throwPixels = m_compactKnob ? 96.0 : travel();
    const double delta =
        ((m_orientation == Qt::Vertical || m_compactKnob) ? -moved : moved) /
        (throwPixels * sensitivity);
    const double pos = std::clamp(m_dragStartPosition + delta, 0.0, 1.0);
    m_gain = gainFromFaderPosition(pos);
    update();
    showBubble();
    emit gainChanged(m_gain);
}

void FaderWidget::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) return;
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
    ValueBubble::dismiss();
    emit editFinished();
}

void FaderWidget::enterEvent(QEnterEvent*) {
    m_hovered = true;
    update();
}

void FaderWidget::leaveEvent(QEvent*) {
    m_hovered = false;
    update();
}

void FaderWidget::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (m_automatable &&
        (automationCreationMode() || (ev->modifiers() & Qt::AltModifier))) {
        emit automateRequested();
        return;
    }
    m_gain = 1.0;
    update();
    emit gainChanged(m_gain);
    emit editFinished();
}

void FaderWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_automatable) {
        QWidget::contextMenuEvent(event);
        return;
    }
    if (automationContextMenu(this, event)) emit automateRequested();
}

void FaderWidget::wheelEvent(QWheelEvent* ev) {
    const double step = (ev->modifiers() & Qt::ShiftModifier) ? 0.005 : 0.02;
    const double pos = std::clamp(
        faderPositionFromGain(m_gain) + (ev->angleDelta().y() > 0 ? step : -step),
        0.0, 1.0);
    m_gain = gainFromFaderPosition(pos);
    update();
    emit gainChanged(m_gain);
    emit editFinished();
}

// ── MiniSlider ──

namespace {

constexpr int kFlyoutWidth = 176;
constexpr int kFlyoutHeight = 28;
constexpr int kFlyoutTrackInset = 11;
/// The number, at the right of the flyout. A control you reach for to set a
/// level should say what the level *is*; the drag bubble only shows up once
/// you are already dragging, which is too late to be useful.
constexpr int kFlyoutReadout = 50;
/// A flyout that opens as a knob needs room for a ring and the number, and
/// nothing else.
constexpr int kFlyoutRotaryWidth = 92;
/// The slim track a flyout slider uses: exactly the handle's own width, so the
/// control is no wider than the thing you drag.
constexpr double kFlyoutTrack = 12.0;
/// How often the open flyout checks whether the pointer is still on it. Only
/// needed for leaving the flyout itself; leaving the icon closes it at once.
constexpr int kFlyoutPollMs = 90;

}  // namespace

/// The slider that slides out from a MiniSlider.
///
/// A tool window rather than a popup: a popup grabs the mouse, which would stop
/// the icon underneath receiving hover events and make the whole thing flicker.
/// One instance for the whole application, retargeted on hover — see the note
/// on MiniSlider.
class MiniSliderFlyout : public QWidget {
public:
    static MiniSliderFlyout* instance() {
        static MiniSliderFlyout* one = new MiniSliderFlyout;
        return one;
    }

    MiniSlider* owner() const { return m_owner; }

    void openFor(MiniSlider* slider) {
        m_owner = slider;
        resize(slider->m_rotary ? kFlyoutRotaryWidth : kFlyoutWidth,
               kFlyoutHeight);
        // Flush against the bottom of the icon, with no gap: a gap is a dead
        // zone the pointer has to cross, and crossing it would read as leaving
        // both widgets and close the thing being reached for.
        const QPoint anchor =
            slider->mapToGlobal(QPoint(slider->width() / 2, slider->height()));
        move(anchor.x() - width() / 2, anchor.y());
        show();
        raise();
        update();
        m_poll->start();
    }

    void dismiss() {
        m_poll->stop();
        m_owner = nullptr;
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!m_owner) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        p.setPen(QPen(mixColors(t.separator(), t.accent, 0.4), 1));
        p.setBrush(mixColors(t.surfaceElevated, t.background, 0.1));
        p.drawRoundedRect(r, kFlyoutHeight / 2.0, kFlyoutHeight / 2.0);

        if (m_owner->m_rotary) {
            paintRing(p);
        } else {
            SliderPaint spec;
            spec.orientation = Qt::Horizontal;
            spec.position = m_owner->fraction();
            // Inverted sliders fill from the right, so the fade-out's control
            // grows in the same direction its ramp does.
            spec.fillFrom = m_owner->m_inverted ? 1.0
                            : m_owner->m_bipolar ? 0.5
                                                 : 0.0;
            spec.active = true;   // it is only up while the pointer is on it
            spec.flush = true;    // no wider than the handle
            const QRectF track = controlRect();
            paintSlider(p, QRectF(track.left(),
                                  track.center().y() - kFlyoutTrack / 2.0,
                                  track.width(), kFlyoutTrack),
                        spec);
        }

        QFont f = p.font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(t.textPrimary);
        p.drawText(QRectF(width() - kFlyoutReadout - kFlyoutTrackInset + 2.0, 0.0,
                          double(kFlyoutReadout), double(height())),
                   Qt::AlignRight | Qt::AlignVCenter, m_owner->text());
    }

    /// Pan, and anything else bipolar, as the ring it is everywhere else in the
    /// application: 270° with the gap at the bottom, the arc growing out of the
    /// middle, dragged vertically like every other knob here.
    void paintRing(QPainter& p) const {
        const Theme& t = th();
        const QRectF area = controlRect();
        const double d = std::min(area.width(), double(height()) - 8.0);
        const QRectF ring(area.left(), (height() - d) / 2.0, d, d);
        const QPointF centre = ring.center();
        const double radius = ring.width() / 2.0;
        constexpr double pen = 2.5;

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(mixColors(t.well(), t.textSecondary, 0.35), pen));
        p.drawArc(ring.adjusted(pen / 2, pen / 2, -pen / 2, -pen / 2),
                  225 * 16, -270 * 16);

        const double f = m_owner->fraction();
        const double from = m_owner->m_bipolar ? 90.0 : 225.0;
        const double sweep = m_owner->m_bipolar ? (f - 0.5) * 270.0 : f * 270.0;
        p.setPen(QPen(std::abs(sweep) < 0.5 ? t.textSecondary : t.accent, pen));
        p.drawArc(ring.adjusted(pen / 2, pen / 2, -pen / 2, -pen / 2),
                  int(from * 16), int(-sweep * 16));

        const double angle = (225.0 - f * 270.0) * kDegToRad;
        p.setPen(QPen(t.textPrimary, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(centre,
                   QPointF(centre.x() + std::cos(angle) * (radius - pen - 1.0),
                           centre.y() - std::sin(angle) * (radius - pen - 1.0)));
    }

    /// Where the slider or the ring lives: everything left of the readout.
    QRectF controlRect() const {
        const double left = kFlyoutTrackInset;
        const double right = width() - kFlyoutTrackInset - kFlyoutReadout;
        return QRectF(left, 0.0, std::max(12.0, right - left), double(height()));
    }

    void mousePressEvent(QMouseEvent* ev) override {
        if (m_owner) emit m_owner->editStarted();
        if (m_owner && m_owner->m_rotary) {
            m_dragStart = m_owner->m_value;
            m_dragY = int(ev->position().y());
            return;
        }
        setFromX(ev->position().x());
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        if (!(ev->buttons() & Qt::LeftButton) || !m_owner) return;
        if (m_owner->m_rotary) {
            // Up raises, the way every knob in this UI is dragged; a knob is
            // never dragged around its circumference.
            const double travel =
                (ev->modifiers() & Qt::ShiftModifier) ? 480.0 : 120.0;
            const double span = m_owner->m_max - m_owner->m_min;
            m_owner->commit(m_dragStart +
                            (m_dragY - ev->position().y()) / travel * span);
            return;
        }
        setFromX(ev->position().x());
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        if (m_owner) emit m_owner->editFinished();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        if (!m_owner) return;
        emit m_owner->editStarted();
        m_owner->commit(m_owner->m_default);
        emit m_owner->editFinished();
    }
    void wheelEvent(QWheelEvent* ev) override {
        if (!m_owner) return;
        emit m_owner->editStarted();
        const double span = m_owner->m_max - m_owner->m_min;
        m_owner->commit(m_owner->m_value +
                        (ev->angleDelta().y() > 0 ? 1 : -1) * span / 60.0);
        emit m_owner->editFinished();
    }

private:
    MiniSliderFlyout()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint |
                               Qt::NoDropShadowWindowHint |
                               Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        resize(kFlyoutWidth, kFlyoutHeight);

        m_poll = new QTimer(this);
        m_poll->setInterval(kFlyoutPollMs);
        connect(m_poll, &QTimer::timeout, this, [this] {
            if (!m_owner || !m_owner->pointerNearby()) dismiss();
        });
    }

    void setFromX(double x) {
        if (!m_owner) return;
        const QRectF track = controlRect();
        // Through the same geometry the handle is drawn with, so the cap lands
        // under the pointer instead of a couple of pixels off it.
        double f = sliderPositionAt(
            QRectF(track.left(), 0.0, track.width(), kFlyoutTrack),
            Qt::Horizontal, x, /*flush=*/true);
        if (m_owner->m_inverted) f = 1.0 - f;
        m_owner->commit(m_owner->m_min + f * (m_owner->m_max - m_owner->m_min));
    }

    MiniSlider* m_owner = nullptr;
    QTimer* m_poll = nullptr;
    double m_dragStart = 0.0;
    int m_dragY = 0;
};

MiniSlider::MiniSlider(icons::Glyph glyph, const QString& tip, QWidget* parent)
    : QWidget(parent), m_glyph(glyph) {
    setFixedSize(24, 20);
    setCursor(Qt::SizeHorCursor);
    setToolTip(tip);
    setAccessibleName(tip);
    setAccessibleDescription(
        tr("Drag horizontally to adjust; double-click to reset."));
    setMouseTracking(true);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

MiniSlider::~MiniSlider() {
    // The shared flyout outlives us, so it must not be left pointing here.
    if (MiniSliderFlyout::instance()->owner() == this)
        MiniSliderFlyout::instance()->dismiss();
}

void MiniSlider::setRange(double minimum, double maximum) {
    m_min = minimum;
    m_max = maximum;
    setValue(m_value);
}

void MiniSlider::setStep(double perPixel) { m_step = perPixel; }

void MiniSlider::openFlyoutForTest() {
    MiniSliderFlyout::instance()->openFor(this);
}

void MiniSlider::setRotary(bool rotary) {
    m_rotary = rotary;
    setCursor(rotary ? Qt::SizeVerCursor : Qt::SizeHorCursor);
    setAccessibleDescription(
        rotary ? tr("Drag vertically to adjust; double-click to reset.")
               : tr("Drag horizontally to adjust; double-click to reset."));
    update();
}

void MiniSlider::setBipolar(bool bipolar) {
    m_bipolar = bipolar;
    update();
}

void MiniSlider::setInverted(bool inverted) {
    m_inverted = inverted;
    update();
}

void MiniSlider::setFormatter(std::function<QString(double)> formatter) {
    m_formatter = std::move(formatter);
    update();
}

void MiniSlider::setValue(double value) {
    const double v = std::clamp(value, m_min, m_max);
    if (std::abs(v - m_value) < 1e-9) return;
    m_value = v;
    update();
    if (MiniSliderFlyout::instance()->owner() == this)
        MiniSliderFlyout::instance()->update();
}

void MiniSlider::commit(double value) {
    const double v = std::clamp(value, m_min, m_max);
    if (std::abs(v - m_value) < 1e-9) return;
    m_value = v;
    update();
    if (MiniSliderFlyout::instance()->owner() == this)
        MiniSliderFlyout::instance()->update();
    ValueBubble::showFor(this, QPoint(width() / 2, 0), text());
    emit valueChanged(m_value);
}

double MiniSlider::fraction() const {
    if (m_max - m_min < 1e-9) return 0.0;
    return (m_value - m_min) / (m_max - m_min);
}

QString MiniSlider::text() const {
    if (m_formatter) return m_formatter(m_value);
    return QString::number(m_value, 'f', 1);
}

bool MiniSlider::pointerNearby() const {
    // A grab has no pointer to hold over the island, so a screenshot asks for
    // the flyout outright and it stays up until the process ends.
    static const bool forced = qEnvironmentVariableIsSet("DAW_SHOT_FLYOUT");
    if (forced) return true;
    if (m_hover || m_dragging) return true;
    auto* flyout = MiniSliderFlyout::instance();
    if (flyout->owner() != this || !flyout->isVisible()) return false;
    return flyout->frameGeometry().adjusted(-2, -2, 2, 2).contains(QCursor::pos());
}

void MiniSlider::enterEvent(QEnterEvent*) {
    m_hover = true;
    m_hoverFade.setTarget(1.0);
    MiniSliderFlyout::instance()->openFor(this);
}

void MiniSlider::leaveEvent(QEvent*) {
    m_hover = false;
    m_hoverFade.setTarget(0.0);
    // Gone the moment the pointer is off, unless it went straight down into the
    // flyout. Sweeping along a row of these should leave nothing behind.
    if (!pointerNearby()) {
        auto* flyout = MiniSliderFlyout::instance();
        if (flyout->owner() == this) flyout->dismiss();
    }
}

void MiniSlider::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double hover = m_hoverFade.value();

    if (hover > 0.005 || m_dragging) {
        QColor lift = t.textPrimary;
        lift.setAlphaF(0.12 * std::max(hover, m_dragging ? 1.0 : 0.0));
        p.setPen(Qt::NoPen);
        p.setBrush(lift);
        p.drawRoundedRect(r, 6, 6);
    }

    // A hairline under the glyph carrying the value, so the island still shows
    // *something* about the level without spending width on digits.
    const double inset = 3.0;
    const double railLeft = r.left() + inset;
    const double railWidth = r.width() - inset * 2;
    QColor rail = t.textSecondary;
    rail.setAlphaF(0.35);
    p.setPen(Qt::NoPen);
    p.setBrush(rail);
    p.drawRoundedRect(QRectF(railLeft, r.bottom() - 2.5, railWidth, 1.5), 0.75,
                      0.75);
    p.setBrush(t.accent);
    const double filled = railWidth * fraction();
    p.drawRoundedRect(m_inverted ? QRectF(railLeft + railWidth - filled,
                                          r.bottom() - 2.5, filled, 1.5)
                                 : QRectF(railLeft, r.bottom() - 2.5, filled, 1.5),
                      0.75, 0.75);

    const qreal side = 14.0;
    icons::paint(p, m_glyph,
                 QRectF(r.center().x() - side / 2, r.center().y() - side / 2 - 1,
                        side, side),
                 m_dragging ? t.accent : t.textPrimary);
}

void MiniSlider::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;
    emit editStarted();
    m_dragging = true;
    m_dragStartValue = m_value;
    m_dragStartX = int(ev->position().x());
    ValueBubble::showFor(this, QPoint(width() / 2, 0), text());
    update();
}

void MiniSlider::mouseMoveEvent(QMouseEvent* ev) {
    if (!m_dragging) return;
    const double fine = (ev->modifiers() & Qt::ShiftModifier) ? 0.25 : 1.0;
    const double moved = ev->position().x() - m_dragStartX;
    commit(m_dragStartValue + (m_inverted ? -moved : moved) * m_step * fine);
}

void MiniSlider::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) return;
    m_dragging = false;
    ValueBubble::dismiss();
    update();
    emit editFinished();
}

void MiniSlider::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_automatable &&
        (automationCreationMode() || (event->modifiers() & Qt::AltModifier))) {
        emit automateRequested();
        return;
    }
    emit editStarted();
    commit(m_default);
    emit editFinished();
}

void MiniSlider::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_automatable) {
        QWidget::contextMenuEvent(event);
        return;
    }
    if (automationContextMenu(this, event)) emit automateRequested();
}

void MiniSlider::wheelEvent(QWheelEvent* ev) {
    emit editStarted();
    const double fine = (ev->modifiers() & Qt::ShiftModifier) ? 0.25 : 1.0;
    commit(m_value + (ev->angleDelta().y() > 0 ? 1 : -1) * m_step * 4.0 * fine);
    emit editFinished();
}

// ── PanKnob ──

PanKnob::PanKnob(QWidget* parent) : QWidget(parent) {
    setFixedSize(34, 34);
    setCursor(Qt::SizeVerCursor);
    setToolTip(tr("Pan · drag vertically · double-click to centre"));
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void PanKnob::setPan(double pan) {
    const double v = std::clamp(pan, -1.0, 1.0);
    if (std::abs(v - m_pan) < 1e-6) return;
    m_pan = v;
    update();
}

void PanKnob::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();

    const double side = std::min(width(), height()) - 5.0;
    const QRectF body((width() - side) / 2.0, (height() - side) / 2.0 - 0.5,
                      side, side);
    const QPointF centre = body.center();
    const double radius = side / 2.0;

    // A single physical dial — no outer progress ring. The shadow and the
    // asymmetric radial light make the object itself readable as something
    // that can be turned.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, t.dark ? 105 : 48));
    p.drawEllipse(body.translated(0.0, 1.8));

    QRadialGradient metal(body.topLeft() + QPointF(side * 0.33, side * 0.28),
                          side * 0.78);
    metal.setColorAt(0.0,
                     mixColors(t.surfaceElevated, t.textPrimary,
                               t.dark ? 0.30 : 0.10));
    metal.setColorAt(0.52, mixColors(t.surfaceElevated, t.background, 0.12));
    metal.setColorAt(1.0,
                     mixColors(t.surfaceElevated, QColor(0, 0, 0),
                               t.dark ? 0.43 : 0.16));
    p.setBrush(metal);
    p.setPen(QPen(t.sectionDivider(), 1.0));
    p.drawEllipse(body);

    // Upper specular picks up the same light direction as the fader caps.
    QColor sheen = t.ink(t.dark ? 62 : 105);
    p.setPen(QPen(sheen, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawArc(body.adjusted(1.5, 1.5, -1.5, -1.5), 35 * 16, 110 * 16);

    // The value is a small embedded indicator dot on the face. Centre pan puts
    // it at twelve o'clock; left and right rotate it through the usual 270°.
    const double angle = (-90.0 + m_pan * 135.0) * kDegToRad;
    const QPointF indicator(centre.x() + std::cos(angle) * radius * 0.56,
                            centre.y() + std::sin(angle) * radius * 0.56);
    const QColor marker = std::abs(m_pan) < 0.005 ? t.textPrimary : t.accentHighlight;
    QColor markerGlow = marker;
    markerGlow.setAlpha(t.dark ? 55 : 35);
    p.setPen(Qt::NoPen);
    p.setBrush(markerGlow);
    p.drawEllipse(indicator, 3.3, 3.3);
    p.setBrush(marker);
    p.drawEllipse(indicator, 1.7, 1.7);

    // A tiny centre dimple makes the rotation axis tangible without turning
    // back into the old line pointer.
    p.setBrush(mixColors(t.well(), t.textPrimary, t.dark ? 0.12 : 0.06));
    p.drawEllipse(centre, 1.15, 1.15);
}

void PanKnob::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_dragStart = m_pan;
    m_dragStartY = int(ev->position().y());
    showBubble();
}

void PanKnob::showBubble() {
    // "C" at centre, then L/R with the percentage off-centre.
    QString text = QStringLiteral("C");
    if (std::abs(m_pan) >= 0.005) {
        text = (m_pan < 0 ? QStringLiteral("L") : QStringLiteral("R")) +
               QString::number(int(std::round(std::abs(m_pan) * 100)));
    }
    ValueBubble::showFor(this, QPoint(width() / 2, 0), text);
}

void PanKnob::mouseMoveEvent(QMouseEvent* ev) {
    if (!m_dragging) return;
    const double sensitivity =
        (ev->modifiers() & Qt::ShiftModifier) ? 400.0 : 120.0;
    m_pan = std::clamp(
        m_dragStart - (ev->position().y() - m_dragStartY) / sensitivity,
        -1.0, 1.0);
    update();
    showBubble();
    emit panChanged(m_pan);
}

void PanKnob::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) return;
    m_dragging = false;
    ValueBubble::dismiss();
    emit editFinished();
}

void PanKnob::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (m_automatable &&
        (automationCreationMode() || (ev->modifiers() & Qt::AltModifier))) {
        emit automateRequested();
        return;
    }
    m_pan = 0.0;
    update();
    emit panChanged(m_pan);
    emit editFinished();
}

void PanKnob::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_automatable) {
        QWidget::contextMenuEvent(event);
        return;
    }
    if (automationContextMenu(this, event)) emit automateRequested();
}

// ── Knob ──

namespace {
/// The knob's full travel, in pixels of vertical drag. Shift divides it, which
/// is the same fine-drag convention every other control here uses.
constexpr double kKnobTravel = 150.0;
constexpr int kKnobSize = 38;
constexpr int kKnobCompactSize = 28;
/// Every rotary control in the unified Sampler/Clip editor uses one footprint.
/// Density now comes from spacing and tabs, not from mixing three dial scales.
constexpr int kSamplerKnobSize = 34;
constexpr int kSamplerKnobPad = 10;
constexpr int kKnobCaptionHeight = 13;
} // namespace

Knob::Knob(const QString& caption, QWidget* parent)
    : QWidget(parent), m_caption(caption) {
    setFixedSize(kKnobSize + 12, kKnobSize + kKnobCaptionHeight);
    setCursor(Qt::SizeVerCursor);
    setFocusPolicy(Qt::StrongFocus);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void Knob::setRange(double minimum, double maximum) {
    m_min = minimum;
    m_max = maximum;
    m_value = std::clamp(m_value, m_min, m_max);
    update();
}

void Knob::setBipolar(bool bipolar) {
    m_bipolar = bipolar;
    update();
}

void Knob::setFormatter(std::function<QString(double)> formatter) {
    m_formatter = std::move(formatter);
    update();
}

void Knob::setCaption(const QString& caption) {
    m_caption = caption;
    update();
}

void Knob::setCompact(bool compact) {
    m_compact = compact;
    if (m_visualStyle == VisualStyle::SamplerDigital) {
        setFixedSize(kSamplerKnobSize + kSamplerKnobPad,
                     kSamplerKnobSize + kKnobCaptionHeight + 2);
    } else {
        const int size = compact ? kKnobCompactSize : kKnobSize;
        setFixedSize(size + 12, size + kKnobCaptionHeight);
    }
    update();
}

void Knob::setBare(int diameter) {
    m_bare = diameter;
    m_caption.clear();
    setFixedSize(diameter, diameter);
    update();
}

void Knob::setDetent(std::function<double(double)> detent) {
    m_detent = std::move(detent);
}

void Knob::setVisualStyle(VisualStyle style) {
    m_visualStyle = style;
    if (!m_bare) {
        if (style == VisualStyle::SamplerDigital) {
            setFixedSize(kSamplerKnobSize + kSamplerKnobPad,
                         kSamplerKnobSize + kKnobCaptionHeight + 2);
        } else {
            const int size = m_compact ? kKnobCompactSize : kKnobSize;
            setFixedSize(size + 12, size + kKnobCaptionHeight + 2);
        }
    }
    update();
}

void Knob::setValue(double value) {
    const double clamped = std::clamp(value, m_min, m_max);
    if (std::abs(clamped - m_value) < 1e-9) return;
    m_value = clamped;
    update();
}

double Knob::fraction() const {
    const double span = m_max - m_min;
    return span > 0.0 ? std::clamp((m_value - m_min) / span, 0.0, 1.0) : 0.0;
}

QString Knob::text() const {
    if (m_formatter) return m_formatter(m_value);
    return QString::number(m_value, 'f', 2);
}

namespace {
/// A knob's ring is a fixed size, and some captions are wider than it. Cutting
/// the last letters off mid-stroke reads as a rendering fault ("LOOP STAR");
/// an ellipsis reads as a name that did not fit, which is what it is.
QString elidedCaption(const QPainter& p, const QString& text, int width) {
    return QFontMetrics(p.font()).elidedText(text, Qt::ElideRight,
                                             std::max(0, width - 2));
}
} // namespace

void Knob::commit(double value) {
    double next = std::clamp(value, m_min, m_max);
    if (m_detent) next = std::clamp(m_detent(next), m_min, m_max);
    if (m_stepped) next = std::round(next);
    if (std::abs(next - m_value) < 1e-9) return;
    m_value = next;
    update();
    emit valueChanged(m_value);
}

void Knob::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();

    const bool digital = m_visualStyle == VisualStyle::SamplerDigital;
    const bool gravityStyle = m_visualStyle == VisualStyle::Gravity;
    const int size = m_bare ? m_bare
                            : digital ? kSamplerKnobSize
                                      : (m_compact ? kKnobCompactSize : kKnobSize);
    const double inset = m_bare ? 1.5 : 2.0;
    const QRectF ring(double(width() - size) / 2.0 + inset, inset,
                      size - inset * 2.0, size - inset * 2.0);
    const QPointF centre = ring.center();
    const double radius = ring.width() / 2.0;
    const double pen = m_bare ? 2.0 : (digital ? 2.4 : (m_compact ? 2.5 : 3.0));

    if (gravityStyle) {
        const double f = fraction();
        const double interaction = m_dragging ? 1.0 : m_hoverFade.value();
        const QColor red(0xE8, 0x10, 0x48);
        const QColor magenta(0xFF, 0x26, 0xB5);
        const QColor accent = mixColors(red, magenta, f * 0.65);

        // Discrete orbital ticks make the value readable even when the dark
        // body is viewed at the panel's minimum size.
        for (int tick = 0; tick < 17; ++tick) {
            const double tf = double(tick) / 16.0;
            const double angle = (225.0 - tf * 270.0) * kDegToRad;
            const bool active = m_bipolar ? tf >= std::min(0.5, f) &&
                                               tf <= std::max(0.5, f)
                                           : tf <= f;
            const double outer = radius - 0.5;
            const double inner = outer - (tick % 4 == 0 ? 5.5 : 3.2);
            QColor ink = active ? accent : QColor(0x72, 0x76, 0x7D);
            ink.setAlpha(active ? 205 : 115);
            p.setPen(QPen(ink, active ? 1.45 : 0.85,
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(centre.x() + std::cos(angle) * inner,
                               centre.y() - std::sin(angle) * inner),
                       QPointF(centre.x() + std::cos(angle) * outer,
                               centre.y() - std::sin(angle) * outer));
        }

        const QRectF body = ring.adjusted(radius * 0.19, radius * 0.19,
                                           -radius * 0.19, -radius * 0.19);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 145));
        p.drawEllipse(body.translated(0.0, std::max(1.0, radius * 0.045)));

        QRadialGradient graphite(body.topLeft() +
                                     QPointF(body.width() * 0.30,
                                             body.height() * 0.24),
                                 body.width() * 0.82);
        graphite.setColorAt(0.0, QColor(0x3A, 0x3D, 0x40));
        graphite.setColorAt(0.46, QColor(0x25, 0x27, 0x29));
        graphite.setColorAt(1.0, QColor(0x0D, 0x0E, 0x10));
        p.setPen(QPen(QColor(0x08, 0x09, 0x0A), 1.4));
        p.setBrush(graphite);
        p.drawEllipse(body);

        QColor sheen(255, 255, 255, int(24 + interaction * 18));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(sheen, 1.0, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(body.adjusted(2.0, 2.0, -2.0, -2.0), 32 * 16, 112 * 16);

        const double angle = (225.0 - f * 270.0) * kDegToRad;
        const QPointF start(centre.x() + std::cos(angle) * body.width() * 0.12,
                            centre.y() - std::sin(angle) * body.width() * 0.12);
        const QPointF end(centre.x() + std::cos(angle) * body.width() * 0.41,
                          centre.y() - std::sin(angle) * body.width() * 0.41);
        p.setPen(QPen(mixColors(QColor(0xC8, 0xCB, 0xD0), accent,
                                0.20 + interaction * 0.35),
                      std::max(1.6, radius * 0.045), Qt::SolidLine,
                      Qt::RoundCap));
        p.drawLine(start, end);

        if (hasFocus()) {
            QColor focus = accent;
            focus.setAlpha(230);
            p.setPen(QPen(focus, 2.0, Qt::DashLine));
            p.drawEllipse(ring.adjusted(-1.0, -1.0, 1.0, 1.0));
        }
        return;
    }

    if (digital) {
        const double f = fraction();
        const double startDegrees = 225.0;
        const int tickCount = 11;
        const double interaction = m_dragging ? 1.0 : m_hoverFade.value() * 0.55;
        QColor idle = mixColors(t.textSecondary, t.background, 0.62);
        idle.setAlphaF(0.62);

        // Frosted outer bezel: a restrained glass rim while idle, a brighter
        // halo while the value is actively being changed. The geometry never
        // grows, so the feedback cannot disturb the surrounding control row.
        QColor bezelGlow = t.accent;
        bezelGlow.setAlphaF(0.10 + interaction * 0.42);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(bezelGlow, 2.5 + interaction * 1.8,
                      Qt::SolidLine, Qt::RoundCap));
        p.drawArc(ring.adjusted(1.0, 1.0, -1.0, -1.0),
                  225 * 16, -270 * 16);

        for (int tick = 0; tick < tickCount; ++tick) {
            const double tf = double(tick) / double(tickCount - 1);
            const double angle = (startDegrees - tf * 270.0) * kDegToRad;
            const bool active = m_bipolar ? (tf >= std::min(0.5, f) &&
                                              tf <= std::max(0.5, f))
                                           : tf <= f;
            const double outer = radius - 0.25;
            const double inner = outer - (active ? 3.4 : 2.2);
            QColor tickInk = active ? mixColors(t.accent, t.accentHighlight,
                                                 interaction * 0.45)
                                    : idle;
            p.setPen(QPen(tickInk, active ? 1.55 + interaction * 0.3 : 0.9,
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(centre.x() + std::cos(angle) * inner,
                               centre.y() - std::sin(angle) * inner),
                       QPointF(centre.x() + std::cos(angle) * outer,
                               centre.y() - std::sin(angle) * outer));
        }

        QRectF body = ring.adjusted(4.6, 4.6, -4.6, -4.6);
        QColor shadow = t.background;
        shadow.setAlpha(t.dark ? 165 : 70);
        p.setPen(Qt::NoPen);
        p.setBrush(shadow);
        p.drawEllipse(body.translated(0.0, 1.4));

        QRadialGradient graphite(body.topLeft() +
                                     QPointF(body.width() * 0.32,
                                             body.height() * 0.25),
                                 body.width() * 0.82);
        graphite.setColorAt(0.0, mixColors(t.surfaceElevated, t.textPrimary,
                                           t.dark ? 0.18 : 0.08));
        graphite.setColorAt(0.48, mixColors(t.surfaceElevated, t.well(), 0.28));
        graphite.setColorAt(1.0, mixColors(t.well(), t.background, 0.58));
        QColor glassEdge = mixColors(t.separator(), t.textSecondary, 0.20);
        glassEdge.setAlphaF(0.72);
        p.setPen(QPen(glassEdge, 1.0));
        p.setBrush(graphite);
        p.drawEllipse(body);

        QColor glassSheen = t.textPrimary;
        glassSheen.setAlphaF(0.12 + interaction * 0.08);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(glassSheen, 1.0, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(body.adjusted(1.2, 1.2, -1.2, -1.2), 28 * 16, 112 * 16);

        const double sweep = m_bipolar ? (f - 0.5) * 270.0 : f * 270.0;
        const double from = m_bipolar ? 90.0 : 225.0;
        QRectF glowRing = ring.adjusted(2.3, 2.3, -2.3, -2.3);
        QColor glow = t.accent;
        glow.setAlphaF(0.20 + interaction * 0.48);
        p.setPen(QPen(glow, 4.0 + interaction * 2.0,
                      Qt::SolidLine, Qt::RoundCap));
        p.drawArc(glowRing, int(from * 16), int(-sweep * 16));
        const QColor activeInk = mixColors(t.accent, t.accentHighlight,
                                           interaction * 0.60);
        p.setPen(QPen(activeInk, 1.65 + interaction * 0.35,
                      Qt::SolidLine, Qt::RoundCap));
        p.drawArc(glowRing, int(from * 16), int(-sweep * 16));

        const double angle = (startDegrees - f * 270.0) * kDegToRad;
        const QPointF stem(centre.x() + std::cos(angle) * body.width() * 0.16,
                           centre.y() - std::sin(angle) * body.width() * 0.16);
        const QPointF tip(centre.x() + std::cos(angle) * body.width() * 0.38,
                          centre.y() - std::sin(angle) * body.width() * 0.38);
        p.setPen(QPen(mixColors(t.textPrimary, activeInk, interaction * 0.44),
                      1.55 + interaction * 0.25, Qt::SolidLine,
                      Qt::RoundCap));
        p.drawLine(stem, tip);
        p.setBrush(activeInk);
        p.setPen(Qt::NoPen);
        p.drawEllipse(centre, 1.25 + interaction * 0.25,
                      1.25 + interaction * 0.25);

        if (hasFocus()) {
            QColor focus = t.accent;
            focus.setAlpha(150);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(focus, 1.0, Qt::DashLine));
            p.drawEllipse(ring.adjusted(-1.0, -1.0, 1.0, 1.0));
        }

        if (!m_caption.isEmpty()) {
            const QRect labelRect(0, height() - kKnobCaptionHeight - 1, width(),
                                  kKnobCaptionHeight + 1);
            QFont font = p.font();
            font.setPixelSize(8);
            font.setLetterSpacing(QFont::PercentageSpacing, 105);
            const bool showValue = m_hoverFade.value() > 0.35 || m_dragging;
            if (showValue) {
                font.setFamily(QStringLiteral("Monaco"));
                p.setPen(Qt::NoPen);
                QColor oled = mixColors(t.background, t.well(), 0.25);
                oled.setAlphaF(0.94);
                p.setBrush(oled);
                p.drawRoundedRect(QRectF(labelRect).adjusted(1.0, 0.5, -1.0, -0.5),
                                  3.0, 3.0);
            }
            p.setFont(font);
            p.setPen(showValue ? activeInk : t.textSecondary);
            p.drawText(labelRect, Qt::AlignCenter,
                       elidedCaption(p, showValue ? text() : m_caption.toUpper(),
                                     labelRect.width()));
        }
        return;
    }

    // The track: 270° with the gap at the bottom, the shape every knob in a
    // DAW has, so the value's position reads without a scale. A bare knob sits
    // on a slot row rather than on the strip's own surface, where `well` is
    // nearly the background — it needs its ring lifted to read as one.
    p.setPen(QPen(m_bare ? mixColors(t.well(), t.textSecondary, 0.40) : t.well(),
                  pen));
    p.setBrush(Qt::NoBrush);
    p.drawArc(ring, 225 * 16, -270 * 16);

    const double f = fraction();
    const double sweep = m_bipolar ? (f - 0.5) * 270.0 : f * 270.0;
    const double from = m_bipolar ? 90.0 : 225.0;
    const bool atRest = std::abs(sweep) < 0.5;
    p.setPen(QPen(atRest ? t.textSecondary : t.accent, pen));
    p.drawArc(ring, int(from * 16), int(-sweep * 16 * (m_bipolar ? 1.0 : 1.0)));

    p.setPen(Qt::NoPen);
    p.setBrush(mixColors(t.surfaceElevated, t.background, 0.35));
    p.drawEllipse(centre, radius - pen - 1.0, radius - pen - 1.0);

    const double angle = (225.0 - f * 270.0) * kDegToRad;
    const QPointF tip(centre.x() + std::cos(angle) * (radius - pen - 1.5),
                      centre.y() - std::sin(angle) * (radius - pen - 1.5));
    p.setPen(QPen(t.textPrimary, (m_compact || m_bare) ? 1.5 : 2.0, Qt::SolidLine,
                  Qt::RoundCap));
    p.drawLine(centre, tip);

    if (!m_caption.isEmpty()) {
        QFont font = p.font();
        font.setPixelSize(m_compact ? 8 : 9);
        font.setLetterSpacing(QFont::PercentageSpacing, 105);
        p.setFont(font);
        // Hovering swaps the caption for the value: the panel is dense enough
        // that a permanent readout under every knob would be unreadable.
        const bool showValue = m_hoverFade.value() > 0.5 || m_dragging;
        p.setPen(showValue ? t.textPrimary : t.textSecondary);
        p.drawText(QRect(0, height() - kKnobCaptionHeight, width(), kKnobCaptionHeight),
                   Qt::AlignCenter,
                   elidedCaption(p, showValue ? text() : m_caption.toUpper(),
                                 width()));
    }
}

void Knob::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_dragStartValue = m_value;
    m_dragStartY = int(ev->position().y());
    ValueBubble::showFor(this, QPoint(width() / 2, 0), text());
    update();
}

void Knob::mouseMoveEvent(QMouseEvent* ev) {
    if (!m_dragging) return;
    const double travel = (ev->modifiers() & Qt::ShiftModifier) ? kKnobTravel * 4.0
                                                                : kKnobTravel;
    const double moved = double(m_dragStartY) - ev->position().y();
    commit(m_dragStartValue + (moved / travel) * (m_max - m_min));
    ValueBubble::showFor(this, QPoint(width() / 2, 0), text());
}

void Knob::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) return;
    m_dragging = false;
    ValueBubble::dismiss();
    update();
    emit editFinished();
}

void Knob::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (m_automatable &&
        (automationCreationMode() || (ev->modifiers() & Qt::AltModifier))) {
        emit automateRequested();
        return;
    }
    commit(m_default);
    emit editFinished();
}

void Knob::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_automatable) {
        QWidget::contextMenuEvent(event);
        return;
    }
    if (automationContextMenu(this, event)) emit automateRequested();
}

void Knob::wheelEvent(QWheelEvent* ev) {
    const double fine = (ev->modifiers() & Qt::ShiftModifier) ? 0.25 : 1.0;
    const double step = m_stepped ? 1.0 : (m_max - m_min) * 0.02;
    commit(m_value + (ev->angleDelta().y() > 0 ? 1 : -1) * step * fine);
    emit editFinished();
}

void Knob::keyPressEvent(QKeyEvent* event) {
    const double base = m_stepped ? 1.0 : (m_max - m_min) * 0.01;
    const double fine = (event->modifiers() & Qt::ShiftModifier) ? 0.25 : 1.0;
    double next = m_value;
    switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Down: next -= base * fine; break;
        case Qt::Key_Right:
        case Qt::Key_Up: next += base * fine; break;
        case Qt::Key_PageDown: next -= base * 10.0 * fine; break;
        case Qt::Key_PageUp: next += base * 10.0 * fine; break;
        case Qt::Key_Home: next = m_min; break;
        case Qt::Key_End: next = m_max; break;
        default:
            QWidget::keyPressEvent(event);
            return;
    }
    commit(next);
    emit editFinished();
    event->accept();
}

void Knob::enterEvent(QEnterEvent*) { m_hoverFade.setTarget(1.0); }

void Knob::leaveEvent(QEvent*) { m_hoverFade.setTarget(0.0); }

// ── Led ──

Led::Led(const QString& caption, QWidget* parent)
    : QAbstractButton(parent), m_color(QColor(0xF5, 0x9E, 0x0B)) {
    setCheckable(true);
    setText(caption);
    setCursor(Qt::PointingHandCursor);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

QSize Led::sizeHint() const {
    const int textWidth =
        text().isEmpty() ? 0 : fontMetrics().horizontalAdvance(text().toUpper()) + 6;
    return QSize(14 + textWidth, 16);
}

void Led::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();

    const QRectF lamp(1.0, double(height()) / 2.0 - 4.0, 8.0, 8.0);
    p.setPen(QPen(mixColors(t.well(), t.background, 0.3), 1));
    p.setBrush(isChecked() ? m_color : t.well());
    p.drawEllipse(lamp);
    if (isChecked()) {
        // A soft halo, so a lit lamp reads at a glance in a dense panel.
        p.setPen(Qt::NoPen);
        QColor glow = m_color;
        glow.setAlpha(70);
        p.setBrush(glow);
        p.drawEllipse(lamp.adjusted(-2.5, -2.5, 2.5, 2.5));
    }

    if (text().isEmpty()) return;
    QFont font = p.font();
    font.setPixelSize(9);
    p.setFont(font);
    p.setPen(isChecked() ? t.textPrimary : t.textSecondary);
    p.drawText(QRect(14, 0, width() - 14, height()),
               Qt::AlignLeft | Qt::AlignVCenter, text().toUpper());
}

// ── ModeSwitch ──

ModeSwitch::ModeSwitch(const QString& leftLabel, const QString& rightLabel,
                       QWidget* parent)
    : QWidget(parent), m_leftLabel(leftLabel.toUpper()),
      m_rightLabel(rightLabel.toUpper()), m_active(th().accent) {
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(24);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Focusable by click only, like every other control in the shell: nothing
    // may take the keyboard on its own, or the transport keys stop working.
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_Hover, true);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        m_active = th().accent;
        update();
    });
}

void ModeSwitch::setActiveColor(const QColor& color) {
    m_active = color;
    update();
}

void ModeSwitch::setRight(bool right, bool animate) {
    if (m_right == right) {
        // Still make sure the knob is where the state says, in case this is the
        // first call and the fade has never run.
        if (!animate) m_slide.jumpTo(right ? 1.0 : 0.0);
        return;
    }
    m_right = right;
    if (animate && !ui::GlassPanel::reduceTransparency())
        m_slide.setTarget(right ? 1.0 : 0.0);
    else
        m_slide.jumpTo(right ? 1.0 : 0.0);
    update();
    emit toggled(m_right);
}

QSize ModeSwitch::sizeHint() const {
    const int text = fontMetrics().horizontalAdvance(m_leftLabel) +
                     fontMetrics().horizontalAdvance(m_rightLabel);
    return QSize(text + 44, 24);
}

void ModeSwitch::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    // Clicking a half *selects* that half rather than toggling: on a two-state
    // control the halves are targets, and clicking the one that is already on
    // should do nothing at all.
    setRight(event->position().x() > width() / 2.0);
}

void ModeSwitch::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Left:  setRight(false); return;
        case Qt::Key_Right: setRight(true); return;
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter: setRight(!m_right); return;
        default: QWidget::keyPressEvent(event);
    }
}

void ModeSwitch::enterEvent(QEnterEvent*) { m_hoverFade.setTarget(1.0); }
void ModeSwitch::leaveEvent(QEvent*) { m_hoverFade.setTarget(0.0); }

void ModeSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    const double slide = m_slide.value();
    const double hover = m_hoverFade.value();

    const QRectF track = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = track.height() / 2.0;
    QColor well = t.well();
    well = mixColors(well, t.textPrimary, 0.05 * hover);
    p.setPen(QPen(t.separator(), 1));
    p.setBrush(well);
    p.drawRoundedRect(track, radius, radius);

    // The knob: one half of the track, slid between the two ends.
    const double inset = 2.0;
    const double knobWidth = track.width() / 2.0 - inset;
    const QRectF knob(track.left() + inset +
                          slide * (track.width() / 2.0),
                      track.top() + inset, knobWidth,
                      track.height() - inset * 2.0);
    const double knobRadius = knob.height() / 2.0;
    QLinearGradient sheen(knob.topLeft(), knob.bottomLeft());
    sheen.setColorAt(0.0, mixColors(m_active, QColor(255, 255, 255), 0.18));
    sheen.setColorAt(1.0, mixColors(m_active, QColor(0, 0, 0), 0.10));
    p.setPen(QPen(mixColors(m_active, QColor(0, 0, 0), 0.25), 1));
    p.setBrush(sheen);
    p.drawRoundedRect(knob, knobRadius, knobRadius);

    QFont f = font();
    f.setPixelSize(9);
    f.setBold(true);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    p.setFont(f);

    // Each label crosses from "quiet text on the well" to "dark text on the
    // lit knob" as the knob passes under it, so the colours never lag the
    // movement.
    const QColor onKnob = mixColors(m_active, QColor(0, 0, 0), 0.72);
    const QRectF leftHalf(track.left(), track.top(), track.width() / 2.0,
                          track.height());
    const QRectF rightHalf(track.center().x(), track.top(), track.width() / 2.0,
                           track.height());
    p.setPen(mixColors(t.textSecondary, onKnob, 1.0 - slide));
    p.drawText(leftHalf, Qt::AlignCenter, m_leftLabel);
    p.setPen(mixColors(t.textSecondary, onKnob, slide));
    p.drawText(rightHalf, Qt::AlignCenter, m_rightLabel);

    if (hasFocus()) {
        p.setPen(QPen(mixColors(t.accent, t.textPrimary, 0.3), 1,
                      Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(track.adjusted(1, 1, -1, -1), radius, radius);
    }
}

// ── LevelMeter ──

LevelMeter::LevelMeter(Qt::Orientation orientation, int channels,
                       QWidget* parent)
    : QWidget(parent), m_orientation(orientation),
      m_channels(std::clamp(channels, 1, 2)) {
    if (orientation == Qt::Vertical) {
        setFixedWidth(m_channels == 2 ? 14 : 8);
        setMinimumHeight(60);
    } else {
        setFixedHeight(m_channels == 2 ? 14 : 8);
        setMinimumWidth(40);
    }
    setToolTip(tr("Level · click to clear the clip indicator"));
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void LevelMeter::setPeak(float peak) { setPeaks(peak, peak); }

void LevelMeter::setPeaks(float left, float right) {
    const float in[2] = {left, right};
    bool dirty = false;
    // A rail draws one bar out of both sides, so it has to *hold* both even
    // when it was built as a single channel.
    const int tracked = m_style == Style::Rail ? 2 : m_channels;
    for (int i = 0; i < tracked; ++i) {
        const float v = std::max(0.0f, in[i]);
        // Fast attack, slow release so short transients stay readable.
        const float next = v > m_level[i] ? v : m_level[i] * 0.80f + v * 0.20f;
        if (std::abs(next - m_level[i]) > 0.0005f) dirty = true;
        m_level[i] = next;
        const float previousHold = m_hold[i];
        if (v > previousHold) {
            m_hold[i] = v;
        } else if (v < previousHold) {
            m_hold[i] = std::max(0.0f, previousHold - 0.006f);
        }
        if (std::abs(m_hold[i] - previousHold) > 0.0005f) dirty = true;
        if (v >= 0.999f && !m_clipped) {
            m_clipped = true;
            dirty = true;
        }
    }
    if (dirty) update();
}

void LevelMeter::clearClip() {
    m_clipped = false;
    update();
}

void LevelMeter::mousePressEvent(QMouseEvent*) { clearClip(); }

namespace {
/// Linear amplitude → 0…1 meter travel, on **the fader's scale**.
///
/// This used to be its own linear −60…0 dB mapping, which put 0 dBFS at the top
/// of the bar. The meter stands against the fader's printed ruler, and that
/// ruler runs +6…−60 dB with a taper — so a signal peaking at 0 dBFS drew a bar
/// level with the "+6" mark, and a bass sitting a comfortable 6 dB down read as
/// "+3". Everything metered looked hotter than it was, by about six decibels.
///
/// Sharing the fader's mapping is what makes the ruler true for both: 0 dBFS
/// lands on the 0 mark, and the strip above it is the headroom an over uses.
float meterScale(float linear) {
    if (linear <= 0.0001f) return 0.f;
    return float(faderPositionFromGain(double(linear)));
}

/// Where a dB level sits on that scale, for the gradient's colour stops. They
/// are decibels, not fractions of the bar: "amber from −6 dB" survives a change
/// to the taper, "amber at 0.88" does not.
float meterStop(double db) {
    return float(faderPositionFromGain(std::pow(10.0, db / 20.0)));
}
} // namespace

void LevelMeter::drawBar(QPainter& p, const QRectF& r, float level,
                         float hold) const {
    const Theme& t = th();
    const qreal radius = m_style == Style::Rail ? 0.0 : 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(t.well());
    p.drawRoundedRect(r, radius, radius);

    const float f = meterScale(level);
    if (f > 0.001f) {
        QLinearGradient grad;
        QRectF bar;
        if (m_orientation == Qt::Vertical) {
            bar = QRectF(r.left(), r.bottom() - r.height() * f, r.width(),
                         r.height() * f);
            grad = QLinearGradient(r.bottomLeft(), r.topLeft());
        } else {
            bar = QRectF(r.left(), r.top(), r.width() * f, r.height());
            grad = QLinearGradient(r.topLeft(), r.topRight());
        }
        // Green to about −18, warming through −6, amber at unity, and red only
        // in the headroom above 0 dBFS — where the signal really is too loud.
        grad.setColorAt(0.0, QColor(0x4C, 0xC4, 0x8A));
        grad.setColorAt(meterStop(-18.0), QColor(0x7A, 0xD0, 0x6D));
        grad.setColorAt(meterStop(-6.0), QColor(0xC8, 0xCC, 0x58));
        grad.setColorAt(meterStop(0.0), QColor(0xE8, 0xC0, 0x4A));
        grad.setColorAt(1.0, QColor(0xE0, 0x4B, 0x4B));
        p.setBrush(grad);
        p.drawRoundedRect(bar, radius, radius);
    }

    const float h = meterScale(hold);
    if (h > 0.02f) {
        p.setBrush(t.textPrimary);
        if (m_orientation == Qt::Vertical) {
            p.drawRect(QRectF(r.left(), r.bottom() - r.height() * h - 1,
                              r.width(), 1.4));
        } else {
            p.drawRect(QRectF(r.left() + r.width() * h - 1, r.top(), 1.4,
                              r.height()));
        }
    }
}

void LevelMeter::setMeterStyle(Style style) {
    if (m_style == style) return;
    m_style = style;
    if (style == Style::Rail) {
        // A rail is sized by whatever it is flush against, not by itself.
        setMinimumHeight(0);
        setMinimumWidth(0);
    }
    update();
}

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, m_style == Style::Panel);

    if (m_style == Style::Rail) {
        // One strip, edge to edge, square. The two-bar version at this width
        // read as a pair of hairlines with a gap that looked like a mistake;
        // the level being shown is a single peak either way.
        const QRectF area(rect());
        p.setPen(Qt::NoPen);
        p.setBrush(th().well());
        p.drawRect(area);
        drawBar(p, area, std::max(m_level[0], m_level[1]),
                std::max(m_hold[0], m_hold[1]));
        if (m_clipped) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::record());
            p.drawRect(m_orientation == Qt::Vertical
                           ? QRectF(area.left(), area.top(), area.width(), 3.0)
                           : QRectF(area.right() - 3.0, area.top(), 3.0,
                                    area.height()));
        }
        return;
    }

    QRectF area = rect().adjusted(0, 0, 0, 0);
    // Reserve a clip lamp at the top (or right) of the meter.
    const qreal lamp = 4.0;
    if (m_orientation == Qt::Vertical) {
        QRectF lampRect(area.left(), area.top(), area.width(), lamp);
        p.setPen(Qt::NoPen);
        p.setBrush(m_clipped ? Theme::record() : th().well());
        p.drawRoundedRect(lampRect, 1.5, 1.5);
        area.setTop(area.top() + lamp + 2);
    } else {
        QRectF lampRect(area.right() - lamp, area.top(), lamp, area.height());
        p.setPen(Qt::NoPen);
        p.setBrush(m_clipped ? Theme::record() : th().well());
        p.drawRoundedRect(lampRect, 1.5, 1.5);
        area.setRight(area.right() - lamp - 2);
    }

    if (m_channels == 1) {
        drawBar(p, area, m_level[0], m_hold[0]);
        return;
    }
    if (m_orientation == Qt::Vertical) {
        const qreal w = (area.width() - 2) / 2.0;
        drawBar(p, QRectF(area.left(), area.top(), w, area.height()),
                m_level[0], m_hold[0]);
        drawBar(p, QRectF(area.left() + w + 2, area.top(), w, area.height()),
                m_level[1], m_hold[1]);
    } else {
        const qreal h = (area.height() - 2) / 2.0;
        drawBar(p, QRectF(area.left(), area.top(), area.width(), h),
                m_level[0], m_hold[0]);
        drawBar(p, QRectF(area.left(), area.top() + h + 2, area.width(), h),
                m_level[1], m_hold[1]);
    }
}

// ── InlineNameEdit ──

InlineNameEdit::InlineNameEdit(const QString& text, QWidget* parent)
    : QLineEdit(text, parent) {
    setReadOnly(true);
    setFocusPolicy(Qt::NoFocus);          // a single click must not focus it
    setCursor(Qt::ArrowCursor);
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // QLineEdit normally keeps the cursor (and therefore its scroll origin) at
    // the end of a long value. An inert track name should show its beginning
    // and clip the tail instead.
    setCursorPosition(0);
    setToolTip(tr("%1\nDouble-click to rename").arg(text));
}

bool InlineNameEdit::event(QEvent* ev) {
    // Return is also an application-wide transport shortcut. Claim it during
    // an active rename at the ShortcutOverride stage, before QAction can move
    // the playhead and prevent the editor's keyPressEvent from seeing it.
    if (!isReadOnly() && ev->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(ev);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
            key->key() == Qt::Key_Escape) {
            ev->accept();
            return true;
        }
    }
    return QLineEdit::event(ev);
}

void InlineNameEdit::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (isReadOnly()) {
        const int textRight = textMargins().left() +
                              std::max(8, fontMetrics().horizontalAdvance(text())) +
                              4;
        if (ev->position().x() > textRight) {
            ev->ignore();
            return;
        }
        setReadOnly(false);
        setFocusPolicy(Qt::StrongFocus);
        setFocus(Qt::MouseFocusReason);
        selectAll();
        return;
    }
    QLineEdit::mouseDoubleClickEvent(ev);
}

void InlineNameEdit::keyPressEvent(QKeyEvent* ev) {
    if (!isReadOnly() &&
        (ev->key() == Qt::Key_Return || ev->key() == Qt::Key_Enter ||
         ev->key() == Qt::Key_Escape)) {
        endEditing();
        ev->accept();
        return;
    }
    QLineEdit::keyPressEvent(ev);
}

void InlineNameEdit::focusOutEvent(QFocusEvent* ev) {
    QLineEdit::focusOutEvent(ev);
    endEditing();
}

void InlineNameEdit::endEditing() {
    if (isReadOnly()) return;
    setReadOnly(true);
    setFocusPolicy(Qt::NoFocus);
    deselect();
    clearFocus();
    emit editingFinished();
    // Synchronous model listeners may normalize the text with setText(),
    // which moves QLineEdit's cursor back to the end.
    deselect();
    setCursorPosition(0);
    setToolTip(tr("%1\nDouble-click to rename").arg(text()));
}

// ── helpers ──

QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setProperty("role", "section");
    QFont f = label->font();
    f.setPixelSize(9);
    f.setBold(true);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
    label->setFont(f);
    return label;
}

QWidget* separatorLine(Qt::Orientation orientation, int length,
                       QWidget* parent) {
    auto* line = new ThemedWidget(parent);
    if (orientation == Qt::Vertical) {
        line->setFixedWidth(1);
        if (length > 0) line->setFixedHeight(length);
    } else {
        line->setFixedHeight(1);
        if (length > 0) line->setFixedWidth(length);
    }
    line->setAutoFillBackground(false);
    QObject::connect(&ThemeManager::instance(), &ThemeManager::changed, line,
                     [line] {
                         line->setStyleSheet(
                             QString("background: %1;").arg(th().separator().name()));
                     });
    line->setStyleSheet(
        QString("background: %1;").arg(th().separator().name()));
    return line;
}

// ── ResizeHandle ────────────────────────────────────────────────────────────

ResizeHandle::ResizeHandle(Qt::Orientation orientation, QWidget* parent)
    : QWidget(parent), m_orientation(orientation) {
    // Wide enough to grab without hunting, thin enough not to read as a gap.
    if (orientation == Qt::Horizontal) {
        setFixedHeight(7);
        setCursor(Qt::SizeVerCursor);
    } else {
        setFixedWidth(5);
        setCursor(Qt::SizeHorCursor);
    }
}

double ResizeHandle::along(const QMouseEvent* ev) const {
    return m_orientation == Qt::Horizontal ? ev->globalPosition().y()
                                           : ev->globalPosition().x();
}

void ResizeHandle::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;
    m_start = along(ev);
    m_dragging = true;
    if (onDragStart) onDragStart();
    ev->accept();
}

void ResizeHandle::mouseMoveEvent(QMouseEvent* ev) {
    if (!m_dragging || !(ev->buttons() & Qt::LeftButton) || !onDrag) return;
    onDrag(int(along(ev) - m_start));
    ev->accept();
}

void ResizeHandle::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton) return;
    m_dragging = false;
    ev->accept();
}

void ResizeHandle::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), th().separator());
    // Grip notches across the middle, so it reads as a drag handle.
    p.setPen(mixColors(th().surfaceElevated, th().textSecondary, 0.35));
    if (m_orientation == Qt::Horizontal) {
        const int y = height() / 2;
        for (int x = width() / 2 - 8; x <= width() / 2 + 8; x += 8)
            p.drawLine(x, y, x + 3, y);
    } else {
        const int x = width() / 2;
        for (int y = height() / 2 - 8; y <= height() / 2 + 8; y += 8)
            p.drawLine(x, y, x, y + 3);
    }
}

std::string NewTrackSpec::create(daw::EngineController& controller) const {
    if (kind == daw::TrackKind::Pattern) return controller.addPattern();
    if (kind == daw::TrackKind::Automation) {
        // Free-standing: no parent, and no target until a curve on it is given
        // one.
        return controller.addAutomationLane({}, {});
    }
    if (kind == daw::TrackKind::Folder) return controller.addFolder(summing);
    return controller.addTrack(kind);
}

QHash<QAction*, NewTrackSpec> addTrackKindItems(QMenu& menu) {
    struct Entry { const char* label; NewTrackSpec spec; };
    static const Entry entries[] = {
        {QT_TRANSLATE_NOOP("ui", "Add Audio Track"), {daw::TrackKind::Audio, false}},
        {QT_TRANSLATE_NOOP("ui", "Add MIDI Track"), {daw::TrackKind::Midi, false}},
        {QT_TRANSLATE_NOOP("ui", "Add Instrument Track"), {daw::TrackKind::Instrument, false}},
        {QT_TRANSLATE_NOOP("ui", "Add Pattern Track"), {daw::TrackKind::Pattern, true}},
        // A lane with no track over it, for curves that drive anything in the
        // project. The per-track lanes are made with A, not from here.
        {QT_TRANSLATE_NOOP("ui", "Add Automation Track"), {daw::TrackKind::Automation, false}},
        {QT_TRANSLATE_NOOP("ui", "Add Bus Track"), {daw::TrackKind::Bus, false}},
        // Two folders, because they are two different things: a drawer, and a
        // bus with tracks in it.
        {QT_TRANSLATE_NOOP("ui", "Add Folder"), {daw::TrackKind::Folder, false}},
        {QT_TRANSLATE_NOOP("ui", "Add Summing Folder"), {daw::TrackKind::Folder, true}},
    };
    QHash<QAction*, NewTrackSpec> byAction;
    for (const auto& entry : entries) {
        byAction.insert(
            menu.addAction(QCoreApplication::translate("ui", entry.label)),
            entry.spec);
    }
    return byAction;
}

} // namespace ui
