#include "ColorKnob.hpp"

#include "Theme.hpp"

#include <QPaintEvent>
#include <QPainter>
#include <QLinearGradient>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

/// Light in this control comes from the upper left, the way it does on the
/// fader grips and the meter glass. A specular highlight up there with the
/// shadow falling down and to the right is the entire reason a painted disc
/// reads as a solid object instead of as a picture of one.
constexpr double kLightX = -0.5547;
constexpr double kLightY = -0.8321;

/// The metal. Fixed rather than themed on purpose: a real console's pots are the
/// same colour whatever the operator's screen is set to, and keeping them out of
/// the palette is what stops this block from drifting away from the strip it is
/// built into. Only the value ring takes the accent.
const QColor kSkirtDark(0x10, 0x12, 0x13);
const QColor kSkirtLight(0x33, 0x38, 0x3B);
const QColor kMetalDark(0x22, 0x26, 0x29);
const QColor kMetalLight(0xC2, 0xC7, 0xCA);

/// Smallest and largest body the strip layout can ask for. Below the first the
/// knurling turns into grey mud, and above the second the pot stops being a
/// control and starts being a picture of one.
constexpr int kMinimumSide = 24;
constexpr int kMaximumSide = 44;
constexpr int kDefaultSide = 32;

} // namespace

int colorKnobSide(int stripWidth) {
    if (stripWidth < 96) return 24;
    if (stripWidth < 132) return kDefaultSide;
    return 42;
}

ColorKnob::ColorKnob(const QString& caption, QWidget* parent) : Knob({}, parent) {
    m_side = kDefaultSide;
    setRange(-100.0, 100.0);
    setBipolar(true);
    // No caption line: the faceplate silkscreens the parameter name above the
    // pot, which is where a console puts it and the only place it fits here.
    setBare(m_side);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(caption);
}

void ColorKnob::setDiameter(int side) {
    side = std::clamp(side, kMinimumSide, kMaximumSide);
    if (side == m_side) return;
    m_side = side;
    setBare(side);
    m_face = QPixmap();
    update();
}

void ColorKnob::buildFace(qreal ratio) {
    m_face = QPixmap(size() * ratio);
    m_face.setDevicePixelRatio(ratio);
    m_face.fill(Qt::transparent);
    m_faceRatio = ratio;

    QPainter p(&m_face);
    p.setRenderHint(QPainter::Antialiasing, true);

    const double side = m_side;
    const QPointF centre(side / 2.0, side / 2.0);
    const double pocket = side * 0.5 - 1.0;
    const double body = pocket * 0.76;
    const double cap = body * 0.64;
    const bool detailed = body >= 7.5;

    // ── The pocket ─────────────────────────────────────────────────────────
    // A conical recess, lit from the same upper left as everything else: its
    // near wall is in shadow and its far wall carries the light. A linear ramp
    // gives the two walls, and the offset of the highlight gives the direction.
    QLinearGradient wall(centre.x() - pocket, centre.y() - pocket,
                         centre.x() + pocket, centre.y() + pocket);
    wall.setColorAt(0.00, QColor(0x1E, 0x21, 0x23));
    wall.setColorAt(0.46, QColor(0x26, 0x2A, 0x2D));
    wall.setColorAt(0.84, QColor(0x36, 0x3B, 0x3F));
    wall.setColorAt(1.00, QColor(0x46, 0x4C, 0x50));
    p.setPen(Qt::NoPen);
    p.setBrush(wall);
    p.drawEllipse(centre, pocket, pocket);

    // The lip: one bright arc on the wall the light reaches and one dark arc
    // on the wall that hides the light source. Together they say "recessed"
    // in a way a single outline cannot.
    const double lip = std::max(0.6, pocket * 0.045);
    const QRectF lipRect(centre.x() - pocket + lip, centre.y() - pocket + lip,
                         (pocket - lip) * 2.0, (pocket - lip) * 2.0);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 34), std::max(0.7, pocket * 0.05),
                  Qt::SolidLine, Qt::RoundCap));
    p.drawArc(lipRect, 200 * 16, 130 * 16);
    p.setPen(QPen(QColor(0, 0, 0, 120), std::max(0.7, pocket * 0.055),
                  Qt::SolidLine, Qt::RoundCap));
    p.drawArc(lipRect, 20 * 16, 130 * 16);

    // ── Seated in the pocket ───────────────────────────────────────────────
    // A short stack of soft ellipses under the body is what puts the pot inside
    // the plate rather than floating over it. The stack gets tighter as it
    // rises, which is what a contact shadow does on its own.
    for (int layer = 0; layer < 4; ++layer) {
        const double r = body * (1.0 + layer * 0.030);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 72 - layer * 16));
        p.drawEllipse(centre.x() - r,
                      centre.y() - r + pocket * 0.055 + layer * 0.4,
                      r * 2.0, r * 2.0);
    }

    // ── The body ───────────────────────────────────────────────────────────
    QLinearGradient skirt(centre.x() - body, centre.y() - body,
                          centre.x() + body, centre.y() + body);
    skirt.setColorAt(0.00, kSkirtLight);
    skirt.setColorAt(0.50, QColor(0x18, 0x1A, 0x1C));
    skirt.setColorAt(1.00, kSkirtDark);
    p.setPen(Qt::NoPen);
    p.setBrush(skirt);
    p.drawEllipse(centre, body, body);

    // Knurling. Shading each rib by its own angle to the light is the whole
    // trick: the ribs stop being a texture and become a turned metal edge. Fine
    // and low-contrast on purpose — coarse, bright ribs read as gear teeth, and
    // a pot is not a gear.
    if (detailed) {
        const int ribs = std::clamp(int(body * 3.6), 24, 48);
        const double inner = body * 0.76;
        const double thickness = std::max(0.8, 2.0 * kPi * body / ribs * 0.42);
        for (int rib = 0; rib < ribs; ++rib) {
            const double angle = 2.0 * kPi * rib / ribs;
            const double dx = std::cos(angle), dy = -std::sin(angle);
            const double lit = std::max(0.0, dx * kLightX + dy * kLightY);
            p.setPen(QPen(mixColors(kSkirtDark, kMetalLight,
                                    0.05 + 0.38 * std::pow(lit, 0.9)),
                          thickness, Qt::SolidLine, Qt::FlatCap));
            p.drawLine(centre + QPointF(dx, dy) * inner, centre + QPointF(dx, dy) * body);
        }
    }

    // The chamfer between skirt and cap: a cone, so its far side catches the
    // light and its near side falls away. Four stops, because two make a band.
    const double shoulder = body * 0.76;
    QLinearGradient chamfer(centre.x() - shoulder, centre.y() - shoulder,
                            centre.x() + shoulder, centre.y() + shoulder);
    chamfer.setColorAt(0.00, QColor(0xA6, 0xAC, 0xB0));
    chamfer.setColorAt(0.40, QColor(0x6E, 0x74, 0x79));
    chamfer.setColorAt(0.76, QColor(0x46, 0x4C, 0x51));
    chamfer.setColorAt(1.00, QColor(0x7C, 0x82, 0x86));
    p.setPen(QPen(QColor(0x12, 0x14, 0x16), std::max(0.7, body * 0.045)));
    p.setBrush(chamfer);
    p.drawEllipse(centre, shoulder, shoulder);

    // The cap: a shallow dome, brightest where the light lands on it. A short
    // falloff keeps the face flat and machined rather than inflated.
    QRadialGradient dome(QPointF(centre.x() - cap * 0.34, centre.y() - cap * 0.40),
                         cap * 1.40);
    dome.setColorAt(0.00, QColor(0x9A, 0xA0, 0xA4));
    dome.setColorAt(0.42, QColor(0x6A, 0x70, 0x75));
    dome.setColorAt(0.78, QColor(0x43, 0x48, 0x4C));
    dome.setColorAt(1.00, kMetalDark);
    p.setPen(QPen(QColor(0x11, 0x13, 0x14), std::max(0.6, body * 0.035)));
    p.setBrush(dome);
    p.drawEllipse(centre, cap, cap);

    // Two turning marks left by the lathe. Barely visible, and the cap stops
    // looking like a printed disc without them.
    p.setBrush(Qt::NoBrush);
    for (int turn = 1; turn <= 2; ++turn) {
        const double r = cap * (0.36 + 0.26 * turn);
        p.setPen(QPen(QColor(255, 255, 255, 13), std::max(0.5, body * 0.022)));
        p.drawArc(QRectF(centre.x() - r, centre.y() - r, r * 2.0, r * 2.0),
                  0, 360 * 16);
    }
}

void ColorKnob::paintEvent(QPaintEvent*) {
    const qreal ratio = devicePixelRatioF();
    if (m_face.isNull() || m_faceRatio != ratio || m_face.size() != size() * ratio)
        buildFace(ratio);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawPixmap(0, 0, m_face);

    const Theme& t = th();
    const double side = m_side;
    const QPointF centre(side / 2.0, side / 2.0);
    const double pocket = side * 0.5 - 1.0;
    const double body = pocket * 0.76;
    const double cap = body * 0.64;
    // The value is milled into a channel cut in the pocket floor. Keeping it
    // narrow is what leaves the surrounding metal visible: a channel wide
    // enough to read at 24 px becomes a black ring around the pot instead.
    const double ring = body + (pocket - body) * 0.55;
    const double ringPen = std::max(1.1, (pocket - body) * 0.42);

    const double span = maximumValue() - minimumValue();
    const double f = std::clamp(span > 0.0 ? (value() - minimumValue()) / span : 0.0,
                                0.0, 1.0);
    const double sweep = (f - 0.5) * 270.0;
    const bool atRest = std::abs(sweep) < 0.5;
    const double lit = isEditing() || underMouse() ? 1.0 : 0.0;
    const QRectF track(centre.x() - ring, centre.y() - ring, ring * 2.0, ring * 2.0);

    // The milled track first, cut all the way round so a control sitting at its
    // detent still has a channel to sit in rather than a bare faceplate.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0x12, 0x14, 0x16), ringPen, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(track, 225 * 16, -270 * 16);

    p.setPen(QPen(atRest ? mixColors(QColor(0x8E, 0x94, 0x98), t.background, 0.40)
                          : t.accent,
                  ringPen, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(track, 90 * 16, int(-sweep * 16));

    // The pointer is a cut, not a mark: a dark groove with a lit lip on the
    // side the light comes from.
    const double angle = (225.0 - f * 270.0) * kDeg;
    const QPointF dir(std::cos(angle), -std::sin(angle));
    const double groove = std::max(1.0, cap * 0.19);
    p.setPen(QPen(QColor(0, 0, 0, 195), groove, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(centre + dir * (cap * 0.22), centre + dir * (cap * 0.94));
    p.setPen(QPen(QColor(0xF2, 0xF5, 0xF6, 185 + int(lit * 55)),
                  std::max(0.7, groove * 0.46), Qt::SolidLine, Qt::RoundCap));
    p.drawLine(centre + dir * (cap * 0.22), centre + dir * (cap * 0.94));

    if (lit > 0.0) {
        // One extra stop of specular while the pointer is on the pot. It is
        // the only thing that changes on hover, so it cannot cost a frame.
        QRadialGradient sheen(QPointF(centre.x() - cap * 0.34,
                                      centre.y() - cap * 0.40), cap * 1.5);
        sheen.setColorAt(0.0, QColor(255, 255, 255, 22));
        sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(sheen);
        p.drawEllipse(centre, cap, cap);
    }

}

} // namespace ui
