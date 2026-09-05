#include "Icons.hpp"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

namespace icons {
namespace {

constexpr qreal kPi = 3.14159265358979323846;

// Every glyph is authored in a 24×24 box and scaled into the target rect, so
// the shapes stay consistent whatever size a button asks for.
constexpr qreal kUnit = 24.0;

/// A filled triangle with rounded corners, drawn as three quadratic corners.
QPainterPath roundedTriangle(const QPolygonF& poly, qreal radius) {
    const int n = poly.size();
    QPointF v[3];
    for (int i = 0; i < n; ++i) v[i] = poly[i];

    QPointF dir[3];
    for (int i = 0; i < n; ++i) {
        const QPointF d = v[(i + 1) % n] - v[i];
        const qreal L = std::hypot(d.x(), d.y());
        dir[i] = L > 1e-6 ? QPointF(d.x() / L, d.y() / L) : QPointF();
    }

    QPainterPath path;
    path.moveTo(v[0] + dir[0] * radius);
    for (int i = 0; i < n; ++i) {
        const QPointF corner = v[(i + 1) % n];
        path.lineTo(corner - dir[i] * radius);
        path.quadTo(corner, corner + dir[(i + 1) % n] * radius);
    }
    path.closeSubpath();
    return path;
}

void strokePath(QPainter& p, const QPainterPath& path, const QColor& c,
                qreal width = 2.0) {
    QPen pen(c, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void drawGlyph(QPainter& p, Glyph g, const QColor& c) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);

    switch (g) {
    case Glyph::Play:
        // Slightly larger, bolder triangle for a more prominent play button.
        p.drawPath(roundedTriangle(
            QPolygonF({QPointF(8, 5), QPointF(19, 12), QPointF(8, 19)}), 2.2));
        break;
    case Glyph::Pause:
        // Thicker bars with a touch more rounding — reads as "modern" rather
        // than the classic thin double-bar.
        p.drawRoundedRect(QRectF(7.5, 5, 3.6, 14), 1.6, 1.6);
        p.drawRoundedRect(QRectF(12.9, 5, 3.6, 14), 1.6, 1.6);
        break;
    case Glyph::Stop:
        // A rounded square, slightly larger and with softer corners.
        p.drawRoundedRect(QRectF(6.5, 6.5, 11, 11), 3.0, 3.0);
        break;
    case Glyph::Record:
        // Filled dot with a thin ring — clean and unambiguous.
        p.drawEllipse(QPointF(12, 12), 5.0, 5.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 1.4));
        p.drawEllipse(QPointF(12, 12), 7.6, 7.6);
        break;
    case Glyph::Loop: {
        // The conventional repeat/cycle mark: two horizontal arrows with
        // rounded returns. Unlike a single circular arrow it cannot be read as
        // Reload, and it stays clear at the transport's 14-pixel icon size.
        QPainterPath top;
        top.moveTo(5.0, 9.0);
        top.cubicTo(5.0, 6.8, 6.8, 5.0, 9.0, 5.0);
        top.lineTo(18.5, 5.0);
        strokePath(p, top, c, 1.9);
        QPainterPath topHead;
        topHead.moveTo(15.4, 2.4);
        topHead.lineTo(18.5, 5.0);
        topHead.lineTo(15.4, 7.6);
        strokePath(p, topHead, c, 1.9);

        QPainterPath bottom;
        bottom.moveTo(19.0, 15.0);
        bottom.cubicTo(19.0, 17.2, 17.2, 19.0, 15.0, 19.0);
        bottom.lineTo(5.5, 19.0);
        strokePath(p, bottom, c, 1.9);
        QPainterPath bottomHead;
        bottomHead.moveTo(8.6, 16.4);
        bottomHead.lineTo(5.5, 19.0);
        bottomHead.lineTo(8.6, 21.6);
        strokePath(p, bottomHead, c, 1.9);
        break;
    }
    case Glyph::ClipLoop: {
        // A waveform that loops back on itself: 19 pill bars, mirror-symmetric
        // heights around the centre bar, with a playhead cursor pointing down
        // at that centre bar. The centre bar, the cursor and the tip all share
        // the same x=12 axis so nothing reads crooked.
        constexpr qreal barW = 0.75;
        constexpr qreal spacing = 1.2;
        constexpr qreal cy = 15.5;
        const qreal heights[] = {
            0.9, 1.8, 3.0, 4.9, 7.6, 3.2, 5.9, 9.0, 4.6,   // left of centre
            7.4,                                             // centre bar
            4.6, 9.0, 5.9, 3.2, 7.6, 4.9, 3.0, 1.8, 0.9    // right of centre
        };
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (int i = 0; i < 19; ++i) {
            const qreal cx = 12.0 + (i - 9) * spacing;
            p.drawRoundedRect(
                QRectF(cx - barW / 2, cy - heights[i] / 2, barW, heights[i]),
                barW / 2, barW / 2);
        }

        // Playhead cursor: a symmetric flag with a flat top and rounded tip on
        // the centre axis.
        QPainterPath cur;
        cur.moveTo(9.2, 3.2);
        cur.quadTo(9.2, 2.5, 10.0, 2.5);
        cur.lineTo(14.0, 2.5);
        cur.quadTo(14.8, 2.5, 14.8, 3.2);
        cur.quadTo(14.8, 7.5, 12.0, 10.0);
        cur.quadTo(9.2, 7.5, 9.2, 3.2);
        p.drawPath(cur);
        break;
    }
    case Glyph::Metronome: {
        // A solid trapezoid body on a base, with the pendulum rod and weight
        // rising out of the top so they read clearly at any size.
        QPainterPath body = roundedTriangle(
            QPolygonF({QPointF(12, 8.5), QPointF(17.5, 18.5), QPointF(6.5, 18.5)}),
            1.6);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(body);
        // Base plinth.
        p.drawRoundedRect(QRectF(5.0, 18.5, 14, 2.6), 1.2, 1.2);
        // Pendulum rod sticking out above the body, with its weight.
        QPainterPath rod;
        rod.moveTo(11.0, 16.5);
        rod.lineTo(15.5, 3.5);
        strokePath(p, rod, c, 1.9);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(14.2, 7.0), 1.8, 1.8);
        break;
    }
    case Glyph::SkipStart:
        // Bar on the left, triangle pointing left — "return to start".
        p.drawRoundedRect(QRectF(6.5, 6, 2.2, 12), 1.1, 1.1);
        p.drawPath(roundedTriangle(
            QPolygonF({QPointF(18, 6), QPointF(18, 18), QPointF(9.5, 12)}), 1.4));
        break;
    case Glyph::SkipEnd:
        p.drawRoundedRect(QRectF(15.3, 6, 2.2, 12), 1.1, 1.1);
        p.drawPath(roundedTriangle(
            QPolygonF({QPointF(6, 6), QPointF(6, 18), QPointF(14.5, 12)}), 1.4));
        break;
    case Glyph::Rewind:
        // Two left-pointing triangles, slightly tighter for a cleaner look.
        p.drawPath(roundedTriangle(
            QPolygonF({QPointF(12, 6), QPointF(12, 18), QPointF(4.5, 12)}), 1.4));
        p.drawPath(roundedTriangle(
            QPolygonF({QPointF(19.5, 6), QPointF(19.5, 18), QPointF(12, 12)}), 1.4));
        break;
    case Glyph::Forward:
        p.drawPath(roundedTriangle(
            QPolygonF({QPointF(4.5, 6), QPointF(4.5, 18), QPointF(12, 12)}), 1.4));
        p.drawPath(roundedTriangle(
            QPolygonF({QPointF(12, 6), QPointF(12, 18), QPointF(19.5, 12)}), 1.4));
        break;
    case Glyph::Restart: {
        // Playback returns to the anchored spot: a faint timeline, an anchor
        // marker, a return arc up to the top and a play triangle on the right.
        QColor faint = c;
        faint.setAlphaF(0.28);
        QPen line(faint, 1.4);
        line.setCapStyle(Qt::RoundCap);
        p.setPen(line);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(3.5, 17), QPointF(20.5, 17));

        // Anchor marker: filled dot on the timeline with a short stem.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(6, 17), 2.0, 2.0);
        QPen stem(c, 1.6);
        stem.setCapStyle(Qt::RoundCap);
        p.setPen(stem);
        p.drawLine(QPointF(6, 14.5), QPointF(6, 17));

        // Return arc: from the anchor up and right, then a flat top run.
        QPainterPath ret;
        ret.moveTo(6, 13);
        ret.cubicTo(6, 8.7, 8.8, 5.5, 12.8, 5.5);
        ret.lineTo(15.8, 5.5);
        strokePath(p, ret, c, 1.7);

        // Rounded return arrowhead.
        QPainterPath arrow;
        arrow.moveTo(14.1, 3.8);
        arrow.lineTo(16.7, 5.5);
        arrow.lineTo(14.1, 7.2);
        strokePath(p, arrow, c, 1.7);

        // Play triangle riding on the top run.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(16.2, 9.7), QPointF(20.2, 12),
                                 QPointF(16.2, 14.3)}));
        break;
    }
    case Glyph::Magnet: {
        // The familiar upright horseshoe: open pole caps at the top and a
        // broad U at the bottom. The previous upside-down arch read as a bell
        // at toolbar size, which made the snap command unnecessarily cryptic.
        QPainterPath path;
        path.moveTo(6.2, 6.0);
        path.lineTo(6.2, 12.6);
        path.cubicTo(6.2, 20.1, 17.8, 20.1, 17.8, 12.6);
        path.lineTo(17.8, 6.0);
        strokePath(p, path, c, 2.8);
        QPen cap(c, 2.8);
        cap.setCapStyle(Qt::SquareCap);
        p.setPen(cap);
        p.drawLine(QPointF(4.9, 7.7), QPointF(8.1, 7.7));
        p.drawLine(QPointF(15.9, 7.7), QPointF(19.1, 7.7));
        break;
    }
    case Glyph::Grid: {
        QPen pen(c, 1.5);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
        p.drawLine(QPointF(12, 5), QPointF(12, 19));
        break;
    }
    case Glyph::Clock: {
        QPen pen(c, 1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 7.5, 7.5);
        p.drawLine(QPointF(12, 12), QPointF(12, 7.5));    // hour hand
        p.drawLine(QPointF(12, 12), QPointF(15.5, 13.5)); // minute hand
        break;
    }
    case Glyph::TimeFormat: {
        // A clock sitting over a short ruler: one symbol covers both choices
        // in the menu — absolute time and musical bars — without spelling a
        // mutable value into a 14-pixel chip.
        QPen pen(c, 1.65);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(11.5, 9.7), 5.2, 5.2);
        p.drawLine(QPointF(11.5, 9.7), QPointF(11.5, 6.7));
        p.drawLine(QPointF(11.5, 9.7), QPointF(14.1, 10.9));
        p.drawLine(QPointF(4.8, 18.3), QPointF(19.2, 18.3));
        for (qreal x : {5.2, 9.7, 14.3, 18.8})
            p.drawLine(QPointF(x, 16.1), QPointF(x, 20.0));
        break;
    }
    case Glyph::GridDivision: {
        // A DAW subdivision ruler rather than an app-launcher grid: tall beat
        // lines with shorter divisions between them.
        QPen pen(c, 1.55);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(4.5, 18.5), QPointF(19.5, 18.5));
        for (int i = 0; i < 5; ++i) {
            const qreal x = 5.0 + i * 3.5;
            const qreal top = (i % 2 == 0) ? 5.0 : 10.0;
            p.drawLine(QPointF(x, top), QPointF(x, 18.5));
        }
        break;
    }
    case Glyph::CountIn: {
        // The film-leader countdown: a ring with a sweeping wedge and a
        // crosshair through the middle. Reads as "counting down to a start"
        // without needing a digit, which would be unreadable at 14 px.
        // A timer running down: the ring plus the wedge still to go. The pie's
        // own straight edges read as the hand, so no crosshair is needed — at
        // 14 px extra lines only turn the middle to mush.
        QColor wedge = c;
        wedge.setAlphaF(0.6);
        p.setPen(Qt::NoPen);
        p.setBrush(wedge);
        p.drawPie(QRectF(4.5, 4.5, 15.0, 15.0), 90 * 16, -90 * 16);

        QPen pen(c, 1.8);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 7.5, 7.5);
        break;
    }
    case Glyph::Undo:
    case Glyph::Redo: {
        if (g == Glyph::Redo) { // same arrow, mirrored
            p.translate(kUnit, 0);
            p.scale(-1, 1);
        }
        QPainterPath path;
        path.moveTo(7, 10);
        path.arcTo(QRectF(6, 8, 12, 11), 180, -230);
        strokePath(p, path, c, 1.9);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(4, 10.5), QPointF(10, 10.5),
                                 QPointF(7, 5.5)}));
        break;
    }
    case Glyph::ZoomIn:
    case Glyph::ZoomOut: {
        QPen pen(c, 1.9);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(11, 11), 5.4, 5.4);
        p.drawLine(QPointF(15, 15), QPointF(19, 19));
        p.drawLine(QPointF(8.4, 11), QPointF(13.6, 11));
        if (g == Glyph::ZoomIn) p.drawLine(QPointF(11, 8.4), QPointF(11, 13.6));
        break;
    }
    case Glyph::ZoomFit: {
        QPen pen(c, 1.9);
        p.setPen(pen);
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(4, 12), QPointF(9, 8.5),
                                 QPointF(9, 15.5)}));
        p.drawPolygon(QPolygonF({QPointF(20, 12), QPointF(15, 8.5),
                                 QPointF(15, 15.5)}));
        break;
    }
    case Glyph::Save: {
        QPen pen(c, 1.7);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(5, 5, 14, 14), 2.5, 2.5);
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(9, 5.5, 6, 4.5));
        p.drawRoundedRect(QRectF(8, 13, 8, 5.5), 1, 1);
        break;
    }
    case Glyph::Folder: {
        QPainterPath path;
        path.moveTo(4.5, 8);
        path.lineTo(9.5, 8);
        path.lineTo(11, 10);
        path.lineTo(19.5, 10);
        path.lineTo(19.5, 18);
        path.lineTo(4.5, 18);
        path.closeSubpath();
        strokePath(p, path, c, 1.7);
        break;
    }
    case Glyph::FolderSum: {
        // The same folder, raised to make room for what leaves it: one arrow
        // going down into the bus everything inside is summed to.
        QPainterPath path;
        path.moveTo(4.5, 5.5);
        path.lineTo(9.5, 5.5);
        path.lineTo(11, 7.5);
        path.lineTo(19.5, 7.5);
        path.lineTo(19.5, 14.5);
        path.lineTo(4.5, 14.5);
        path.closeSubpath();
        strokePath(p, path, c, 1.7);

        QPen pen(c, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, 15.5), QPointF(12, 18.5));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(12, 21.0), QPointF(8.8, 17.2),
                                 QPointF(15.2, 17.2)}));
        break;
    }
    case Glyph::Import:
    case Glyph::Export: {
        QPen pen(c, 1.9);
        p.setPen(pen);
        const qreal dir = (g == Glyph::Import) ? 1.0 : -1.0;
        p.drawLine(QPointF(12, 12 - 6 * dir), QPointF(12, 12 + 4 * dir));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(12, 12 + 7 * dir),
                                 QPointF(8.4, 12 + 2.6 * dir),
                                 QPointF(15.6, 12 + 2.6 * dir)}));
        p.setPen(QPen(c, 1.9));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5.5, 19), QPointF(18.5, 19));
        break;
    }
    case Glyph::Mixer: {
        QPen pen(c, 1.7);
        p.setPen(pen);
        for (int i = 0; i < 3; ++i) {
            const qreal x = 7 + i * 5;
            p.drawLine(QPointF(x, 4.5), QPointF(x, 19.5));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(4.6, 8, 4.8, 3.2), 1.2, 1.2);
        p.drawRoundedRect(QRectF(9.6, 13, 4.8, 3.2), 1.2, 1.2);
        p.drawRoundedRect(QRectF(14.6, 6.5, 4.8, 3.2), 1.2, 1.2);
        break;
    }
    case Glyph::Inspector:
    case Glyph::Sidebar: {
        QPen pen(c, 1.7);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4.5, 5.5, 15, 13), 2.5, 2.5);
        const qreal x = (g == Glyph::Sidebar) ? 9.5 : 14.5;
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        if (g == Glyph::Sidebar)
            p.drawRect(QRectF(4.5, 5.5, x - 4.5, 13));
        else
            p.drawRect(QRectF(x, 5.5, 19.5 - x, 13));
        break;
    }
    case Glyph::Detach: {
        QPen pen(c, 1.7);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4, 7.5, 11, 10), 2, 2);
        p.drawRoundedRect(QRectF(9, 4.5, 11, 10), 2, 2);
        break;
    }
    case Glyph::Plus:
    case Glyph::Minus: {
        QPen pen(c, 2.2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(6.5, 12), QPointF(17.5, 12));
        if (g == Glyph::Plus) p.drawLine(QPointF(12, 6.5), QPointF(12, 17.5));
        break;
    }
    case Glyph::Chevron:
    case Glyph::ChevronUp:
    case Glyph::ChevronRight: {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        if (g == Glyph::Chevron) {
            p.drawPolyline(QPolygonF({QPointF(7, 10), QPointF(12, 15),
                                      QPointF(17, 10)}));
        } else if (g == Glyph::ChevronUp) {
            p.drawPolyline(QPolygonF({QPointF(7, 14), QPointF(12, 9),
                                      QPointF(17, 14)}));
        } else {
            p.drawPolyline(QPolygonF({QPointF(10, 7), QPointF(15, 12),
                                      QPointF(10, 17)}));
        }
        break;
    }
    case Glyph::ArrowUp:
    case Glyph::ArrowDown: {
        // Stem plus a solid head. The head is what separates this from the
        // chevrons above: a chevron says "there is more this way", an arrow
        // says "this moves".
        const bool up = (g == Glyph::ArrowUp);
        const qreal tip = up ? 4.6 : 19.4;
        const qreal tail = up ? 19.0 : 5.0;
        const qreal shoulder = up ? 10.4 : 13.6;
        QPen pen(c, 2.1);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, tail), QPointF(12, shoulder));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(12, tip), QPointF(17.0, shoulder + (up ? 0.6 : -0.6)),
                                 QPointF(7.0, shoulder + (up ? 0.6 : -0.6))}));
        break;
    }
    case Glyph::ArrowLeft:
    case Glyph::ArrowRight: {
        const bool left = (g == Glyph::ArrowLeft);
        const qreal tip = left ? 5.0 : 19.0;
        const qreal tail = left ? 19.0 : 5.0;
        const qreal shoulder = left ? 10.8 : 13.2;
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(tail, 12), QPointF(tip, 12));
        p.drawPolyline(QPolygonF({QPointF(shoulder, 7.0), QPointF(tip, 12),
                                  QPointF(shoulder, 17.0)}));
        break;
    }
    case Glyph::Home: {
        QPainterPath house;
        house.moveTo(4.8, 11.2);
        house.lineTo(12, 5.0);
        house.lineTo(19.2, 11.2);
        house.moveTo(7.0, 10.0);
        house.lineTo(7.0, 18.7);
        house.lineTo(17.0, 18.7);
        house.lineTo(17.0, 10.0);
        strokePath(p, house, c, 1.9);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(10.4, 13.4, 3.2, 5.3), 0.8, 0.8);
        break;
    }
    case Glyph::Globe: {
        QPen pen(c, 1.75);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(4.5, 4.5, 15, 15));
        p.drawEllipse(QRectF(8.3, 4.5, 7.4, 15));
        p.drawLine(QPointF(5.2, 9.2), QPointF(18.8, 9.2));
        p.drawLine(QPointF(5.2, 14.8), QPointF(18.8, 14.8));
        break;
    }
    case Glyph::Reload: {
        // One clockwise turn is the standard page-reload affordance. It is
        // intentionally distinct from the two-arrow Loop glyph used for cycle
        // playback and preview looping.
        QPainterPath turn;
        turn.moveTo(18.2, 8.0);
        turn.arcTo(QRectF(5.0, 5.0, 14.0, 14.0), 35.0, -300.0);
        strokePath(p, turn, c, 1.9);
        QPainterPath head;
        head.moveTo(14.6, 5.1);
        head.lineTo(18.7, 7.7);
        head.lineTo(18.0, 2.9);
        strokePath(p, head, c, 1.9);
        break;
    }
    case Glyph::Download: {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, 4.7), QPointF(12, 14.2));
        p.drawPolyline(QPolygonF({QPointF(7.8, 10.4), QPointF(12, 14.7),
                                  QPointF(16.2, 10.4)}));
        p.drawPolyline(QPolygonF({QPointF(5.5, 17.0), QPointF(5.5, 19.2),
                                  QPointF(18.5, 19.2), QPointF(18.5, 17.0)}));
        break;
    }
    case Glyph::ResizeHorizontal:
    case Glyph::ResizeVertical: {
        // A plain double-headed resize arrow: no plate or glass behind it, so
        // it can serve both as an inline group-stretch handle and as a compact
        // row-height scrubber without looking like a separate floating panel.
        const bool vertical = g == Glyph::ResizeVertical;
        QPen pen(c, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        if (vertical) {
            p.drawLine(QPointF(12, 5), QPointF(12, 19));
            p.drawPolyline(QPolygonF({QPointF(8.5, 8.5), QPointF(12, 5),
                                      QPointF(15.5, 8.5)}));
            p.drawPolyline(QPolygonF({QPointF(8.5, 15.5), QPointF(12, 19),
                                      QPointF(15.5, 15.5)}));
        } else {
            p.drawLine(QPointF(5, 12), QPointF(19, 12));
            p.drawPolyline(QPolygonF({QPointF(8.5, 8.5), QPointF(5, 12),
                                      QPointF(8.5, 15.5)}));
            p.drawPolyline(QPolygonF({QPointF(15.5, 8.5), QPointF(19, 12),
                                      QPointF(15.5, 15.5)}));
        }
        break;
    }
    case Glyph::WindowMaximize: {
        // One unambiguous frame, matching the custom internal editor rather
        // than the horizontal fit arrows used by timeline zoom controls.
        QPen pen(c, 1.8);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(5.0, 5.0, 14.0, 14.0), 2.0, 2.0);
        p.drawLine(QPointF(6.2, 8.3), QPointF(17.8, 8.3));
        break;
    }
    case Glyph::Gear: {
        // A gear with real teeth, filled, with the hub punched out of it. It
        // used to be a small circle with eight spokes radiating past it, which
        // at chip size reads as a torch beam or an asterisk — anything but a
        // settings icon.
        constexpr int kTeeth = 8;
        constexpr qreal kOuter = 9.4;   // tooth tip
        constexpr qreal kRoot = 6.9;    // between the teeth
        constexpr qreal kHub = 2.9;     // the hole in the middle
        const qreal pitch = 2.0 * kPi / qreal(kTeeth);
        const qreal tooth = pitch * 0.19;   // half-width at the tip
        const qreal gap = pitch * 0.30;     // half-width at the root
        auto at = [](qreal radius, qreal angle) {
            return QPointF(12 + radius * std::cos(angle),
                           12 + radius * std::sin(angle));
        };

        QPainterPath gear;
        for (int i = 0; i < kTeeth; ++i) {
            const qreal a = qreal(i) * pitch;
            if (i == 0) gear.moveTo(at(kRoot, a - gap));
            else gear.lineTo(at(kRoot, a - gap));
            gear.lineTo(at(kOuter, a - tooth));
            gear.lineTo(at(kOuter, a + tooth));
            gear.lineTo(at(kRoot, a + gap));
        }
        gear.closeSubpath();
        gear.addEllipse(QPointF(12, 12), kHub, kHub);
        gear.setFillRule(Qt::OddEvenFill);

        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(gear);
        break;
    }
    case Glyph::Headphones: {
        QPainterPath arc;
        arc.moveTo(5.5, 15);
        arc.arcTo(QRectF(5.5, 4.5, 13, 13), 180, -180);
        strokePath(p, arc, c, 1.8);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(4.2, 13.5, 3.6, 6), 1.6, 1.6);
        p.drawRoundedRect(QRectF(16.2, 13.5, 3.6, 6), 1.6, 1.6);
        break;
    }
    case Glyph::Waveform: {
        QPen pen(c, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        const qreal h[] = {4, 8, 12, 7, 10, 5};
        for (int i = 0; i < 6; ++i) {
            const qreal x = 5.5 + i * 2.7;
            p.drawLine(QPointF(x, 12 - h[i] / 2), QPointF(x, 12 + h[i] / 2));
        }
        break;
    }
    case Glyph::MidiKeys: {
        // A three-octave-fragment keyboard: the outline with two black keys
        // dropped into it. Enough to read as "keys" at 14 px, where anything
        // more detailed turns into a grey block.
        // The outline is the white keys and the two filled bars are the black
        // ones; at 14 px the dividers between white keys turn to mush, so the
        // black keys carry the whole read and are drawn wide.
        QPen pen(c, 1.7);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRectF board(3.5, 6.5, 17.0, 11.0);
        p.drawRoundedRect(board, 1.8, 1.8);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(8.2, 7.4, 3.2, 6.2), 1.0, 1.0);
        p.drawRoundedRect(QRectF(13.4, 7.4, 3.2, 6.2), 1.0, 1.0);
        break;
    }
    case Glyph::Layers: {
        // Three offset rectangles — the same stack the timeline draws in a
        // clip's take badge, so the button and the badge say the same word.
        QPen pen(c, 1.5);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 3; ++i) {
            const qreal y = 6.0 + i * 4.0;
            p.drawRoundedRect(QRectF(5.0 + i * 1.2, y, 14.0 - i * 2.4, 3.4),
                              1.2, 1.2);
        }
        break;
    }
    case Glyph::Volume: {
        // Speaker cone plus one radiating arc — enough at 14 px, where a second
        // arc turns into mush.
        QPainterPath cone;
        cone.moveTo(5, 9.5);
        cone.lineTo(8, 9.5);
        cone.lineTo(12, 5.5);
        cone.lineTo(12, 18.5);
        cone.lineTo(8, 14.5);
        cone.lineTo(5, 14.5);
        cone.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(cone);

        QPen pen(c, 1.7);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(11.5, 7.5, 7, 9), -70 * 16, 140 * 16);
        break;
    }
    case Glyph::Pan: {
        // A compact pan dial rather than a generic resize arrow. The centre
        // detent and angled pointer echo the mixer knob even at island size.
        QPen pen(c, 1.55);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(6.0, 6.0, 12.0, 12.0), 220 * 16, -260 * 16);
        p.drawLine(QPointF(12.0, 12.0), QPointF(15.2, 8.8));
        p.setPen(QPen(c, 1.25, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12.0, 4.8), QPointF(12.0, 6.7));
        p.drawLine(QPointF(4.7, 15.4), QPointF(6.3, 14.5));
        p.drawLine(QPointF(19.3, 15.4), QPointF(17.7, 14.5));
        break;
    }
    case Glyph::FadeIn:
    case Glyph::FadeOut: {
        // The ramp itself, filled, with the baseline drawn through — the same
        // shape the clip's own fade handle makes.
        const bool in = g == Glyph::FadeIn;
        QPainterPath ramp;
        ramp.moveTo(5, 17.5);
        if (in) {
            ramp.lineTo(19, 6.5);
            ramp.lineTo(19, 17.5);
        } else {
            ramp.lineTo(5, 6.5);
            ramp.lineTo(19, 17.5);
        }
        ramp.closeSubpath();
        QColor fill = c;
        fill.setAlphaF(c.alphaF() * 0.45);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(ramp);

        QPen pen(c, 1.7);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5, 17.5), QPointF(19, 17.5));
        p.drawLine(in ? QPointF(5, 17.5) : QPointF(5, 6.5),
                   in ? QPointF(19, 6.5) : QPointF(19, 17.5));
        break;
    }
    case Glyph::Trash: {
        QPen pen(c, 1.7);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5.5, 7.5), QPointF(18.5, 7.5));
        QPainterPath body;
        body.moveTo(7, 7.5);
        body.lineTo(8, 19);
        body.lineTo(16, 19);
        body.lineTo(17, 7.5);
        strokePath(p, body, c, 1.7);
        p.setPen(QPen(c, 1.7));
        p.drawLine(QPointF(9.5, 7.5), QPointF(10, 5));
        p.drawLine(QPointF(14.5, 7.5), QPointF(14, 5));
        break;
    }
    case Glyph::Pointer: {
        // Classic selection arrow.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(7, 4.5), QPointF(7, 18.5),
                                 QPointF(10.8, 14.8), QPointF(13.2, 19.5),
                                 QPointF(15.2, 18.6), QPointF(12.9, 14),
                                 QPointF(17.5, 13.8)}));
        break;
    }
    case Glyph::Knife: {
        // Plain chef's knife: handle on the left, pointed blade on the right.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(3.0, 12.0, 8.5, 4.5), 1.8, 1.8);
        QPainterPath blade;
        blade.moveTo(10.0, 9.5);
        blade.lineTo(21.0, 6.0);
        blade.lineTo(18.4, 12.8);
        blade.quadTo(15.2, 16.0, 10.0, 15.0);
        blade.closeSubpath();
        p.drawPath(blade);
        break;
    }
    case Glyph::Eraser: {
        // A rubber block on a slight angle, with the band divider.
        p.save();
        p.translate(12, 12);
        p.rotate(-35);
        p.setPen(QPen(c, 1.7));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(-8, -4, 16, 8), 1.6, 1.6);
        p.drawLine(QPointF(-1.5, -4), QPointF(-1.5, 4));
        p.restore();
        break;
    }
    case Glyph::Brush: {
        // A round brush on the diagonal: ferrule, handle, and a soft tip — the
        // Draw tool, and the shape of its cursor.
        p.save();
        p.translate(12, 12);
        // Handle up-right, tip down-left — the way a right-handed grip actually
        // holds one, and the mirror of how this first shipped.
        p.rotate(45);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        // A thin handle against a fat, round tip: at 18 px the silhouette is
        // the only thing that survives, and this one cannot be mistaken for the
        // blade next to it in the toolbar.
        p.drawRoundedRect(QRectF(-1.3, -9.0, 2.6, 7.4), 1.3, 1.3);   // handle
        p.drawRoundedRect(QRectF(-3.4, -1.9, 6.8, 3.0), 1.2, 1.2);   // ferrule
        QPainterPath tip;
        tip.moveTo(-3.6, 1.6);
        tip.lineTo(3.6, 1.6);
        tip.quadTo(3.4, 6.4, 0.0, 9.2);
        tip.quadTo(-3.4, 6.4, -3.6, 1.6);
        p.drawPath(tip);
        p.restore();
        break;
    }
    case Glyph::Ghost: {
        // A sheet-ghost silhouette: domed head, scalloped hem, two eyes punched
        // out. Reads at 14 px, which a translucent note stack would not.
        QPainterPath body;
        body.moveTo(5.0, 19.0);
        body.lineTo(5.0, 11.0);
        body.arcTo(QRectF(5.0, 4.0, 14.0, 14.0), 180.0, -180.0);
        body.lineTo(19.0, 19.0);
        body.lineTo(16.5, 16.6);
        body.lineTo(14.0, 19.0);
        body.lineTo(11.5, 16.6);
        body.lineTo(9.0, 19.0);
        body.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(body);
        // Punch the eyes back out, whatever is behind the icon.
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setBrush(Qt::black);
        p.drawEllipse(QPointF(9.9, 11.0), 1.35, 1.7);
        p.drawEllipse(QPointF(14.1, 11.0), 1.35, 1.7);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        break;
    }
    case Glyph::NoteStyle: {
        // Two note blocks, the front one glassy: the note-appearance menu.
        p.setPen(Qt::NoPen);
        QColor back = c;
        back.setAlphaF(back.alphaF() * 0.45);
        p.setBrush(back);
        p.drawRoundedRect(QRectF(4.0, 6.0, 13.0, 5.0), 2.0, 2.0);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(7.0, 13.0, 13.0, 5.0), 2.0, 2.0);
        break;
    }
    case Glyph::Arpeggio: {
        // Four note blocks climbing a staircase: one chord played as a run.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (int step = 0; step < 4; ++step) {
            p.drawRoundedRect(QRectF(4.0 + step * 4.2, 16.0 - step * 3.4, 3.4, 2.6),
                              1.1, 1.1);
        }
        break;
    }
    case Glyph::Chord: {
        // Three blocks stacked at the same instant: notes struck together.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (int voice = 0; voice < 3; ++voice) {
            p.drawRoundedRect(QRectF(5.0, 6.0 + voice * 4.8, 14.0, 2.8), 1.3, 1.3);
        }
        break;
    }
    case Glyph::Strum: {
        // The same stack, but each voice starting a little later — the rake.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (int voice = 0; voice < 3; ++voice) {
            p.drawRoundedRect(QRectF(4.5 + voice * 3.0, 6.0 + voice * 4.8,
                                     14.0 - voice * 3.0, 2.8),
                              1.3, 1.3);
        }
        break;
    }
    case Glyph::Glue: {
        // Two blocks meeting and fusing: the seam between them is what goes.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(3.5, 10.0, 8.5, 4.0), 1.6, 1.6);
        p.drawRoundedRect(QRectF(12.0, 10.0, 8.5, 4.0), 1.6, 1.6);
        p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12.0, 7.0), QPointF(12.0, 17.0));
        break;
    }
    case Glyph::Dice: {
        // A die showing three pips: randomise, and roll it again.
        QPen pen(c, 1.7);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4.5, 4.5, 15.0, 15.0), 3.4, 3.4);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(8.4, 8.4), 1.5, 1.5);
        p.drawEllipse(QPointF(12.0, 12.0), 1.5, 1.5);
        p.drawEllipse(QPointF(15.6, 15.6), 1.5, 1.5);
        break;
    }
    case Glyph::Invert: {
        // Two arrows trading places: the melody turned upside down.
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(8.5, 5.0), QPointF(8.5, 19.0));
        p.drawLine(QPointF(15.5, 5.0), QPointF(15.5, 19.0));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({QPointF(8.5, 3.4), QPointF(11.6, 7.4),
                                 QPointF(5.4, 7.4)}));
        p.drawPolygon(QPolygonF({QPointF(15.5, 20.6), QPointF(12.4, 16.6),
                                 QPointF(18.6, 16.6)}));
        break;
    }
    case Glyph::NoteMute: {
        // A hollow note bar with a slash through it. Hollow because that is how
        // a muted note is already drawn in the grid, and a slash because the
        // note is switched off rather than removed — a solid bar cut in two
        // would say "sliced", which is the tool next to it.
        QPen pen(c, 1.8);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(3.6, 9.4, 16.8, 5.2), 2.4, 2.4);
        p.setPen(QPen(c, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(6.4, 17.4), QPointF(17.6, 6.6));
        break;
    }
    case Glyph::Articulate: {
        // The accent wedge from a score, over two shortened note bars. Both
        // halves of what the tool does: lengths cut back, velocities accented.
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF({QPointF(5.6, 5.6), QPointF(12.8, 8.3),
                                  QPointF(5.6, 11.0)}));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(3.6, 13.2, 7.6, 4.8), 2.1, 2.1);
        p.drawRoundedRect(QRectF(13.4, 13.2, 5.2, 4.8), 2.1, 2.1);
        break;
    }
    case Glyph::Crosshair: {
        // A target reticle — a ring with four tick marks and a centre dot.
        p.setPen(QPen(c, 1.7));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 6.0, 6.0);
        p.drawLine(QPointF(12, 1.5), QPointF(12, 6));
        p.drawLine(QPointF(12, 18), QPointF(12, 22.5));
        p.drawLine(QPointF(1.5, 12), QPointF(6, 12));
        p.drawLine(QPointF(18, 12), QPointF(22.5, 12));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(12, 12), 1.4, 1.4);
        break;
    }
    case Glyph::Automation: {
        // Two straight runs and a breakpoint between them, which is what an
        // automation curve looks like at any size worth drawing.
        QPen pen(c, 1.7);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF({QPointF(4, 17), QPointF(9, 17),
                                  QPointF(14.5, 7.5), QPointF(20, 7.5)}));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(9, 17), 2.1, 2.1);
        p.drawEllipse(QPointF(14.5, 7.5), 2.1, 2.1);
        break;
    }
    case Glyph::AutomationCreate: {
        // A compact automation curve plus an explicit add mark. It stays
        // distinct from the neighbouring show/hide-automation button even
        // when both are rendered at the toolbar's small icon size.
        QPen pen(c, 1.7);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF({QPointF(3.5, 18), QPointF(8, 18),
                                  QPointF(13, 10), QPointF(17, 10)}));
        p.drawLine(QPointF(19.2, 4.2), QPointF(19.2, 9.0));
        p.drawLine(QPointF(16.8, 6.6), QPointF(21.6, 6.6));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(8, 18), 1.9, 1.9);
        p.drawEllipse(QPointF(13, 10), 1.9, 1.9);
        break;
    }
    case Glyph::MonoRing: {
        // One ring — a single (mono) channel.
        p.setPen(QPen(c, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 5.4, 5.4);
        break;
    }
    case Glyph::StereoRings: {
        // Two interlocking rings — a stereo pair.
        p.setPen(QPen(c, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(8.5, 12), 4.6, 4.6);
        p.drawEllipse(QPointF(15.5, 12), 4.6, 4.6);
        break;
    }
    case Glyph::Power: {
        QPainterPath arc;
        arc.moveTo(8, 7.5);
        arc.arcTo(QRectF(5.5, 5.5, 13, 13), 130, 280);
        strokePath(p, arc, c, 1.9);
        p.setPen(QPen(c, 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12, 4), QPointF(12, 11));
        break;
    }
    case Glyph::Search: {
        // A magnifier: a ring with a short handle off its lower-right corner.
        p.setPen(QPen(c, 1.9, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(10.5, 10.5), 5.6, 5.6);
        p.drawLine(QPointF(14.8, 14.8), QPointF(19.5, 19.5));
        break;
    }
    case Glyph::Star: {
        // A five-point star, for favourites.
        QPainterPath star;
        const QPointF centre(12, 12);
        for (int i = 0; i < 10; ++i) {
            const qreal radius = i % 2 == 0 ? 8.2 : 3.6;
            const qreal angle = -90.0 + i * 36.0;
            const QPointF pt(centre.x() + radius * std::cos(angle * kPi / 180.0),
                             centre.y() + radius * std::sin(angle * kPi / 180.0));
            if (i == 0) star.moveTo(pt);
            else star.lineTo(pt);
        }
        star.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(star);
        break;
    }
    case Glyph::Close: {
        // A thin X — the clear / dismiss glyph.
        p.setPen(QPen(c, 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(6.5, 6.5), QPointF(17.5, 17.5));
        p.drawLine(QPointF(17.5, 6.5), QPointF(6.5, 17.5));
        break;
    }
    case Glyph::Mic: {
        // A microphone: capsule, stem and a small stand foot.
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(9.2, 3.5, 5.6, 9.5), 2.8, 2.8);
        p.drawLine(QPointF(12, 13), QPointF(12, 17));
        p.drawLine(QPointF(7.5, 17), QPointF(16.5, 17));
        p.drawArc(QRectF(8.5, 8.5, 7, 7), 0, 180 * 16);
        break;
    }
    case Glyph::Eq: {
        // A stylised equaliser: three vertical bars of different heights.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(5.0, 10.0, 3.4, 8.0), 1.2, 1.2);
        p.drawRoundedRect(QRectF(10.3, 5.5, 3.4, 12.5), 1.2, 1.2);
        p.drawRoundedRect(QRectF(15.6, 8.0, 3.4, 10.0), 1.2, 1.2);
        break;
    }
    case Glyph::Synth: {
        // A stylised synthesiser: a row of knobs over a keyboard strip.
        p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4.0, 4.0, 16.0, 9.0), 2.0, 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(8.0, 8.5), 1.4, 1.4);
        p.drawEllipse(QPointF(12.0, 8.5), 1.4, 1.4);
        p.drawEllipse(QPointF(16.0, 8.5), 1.4, 1.4);
        p.drawRoundedRect(QRectF(4.0, 15.0, 16.0, 5.0), 1.6, 1.6);
        break;
    }
    case Glyph::Plugin: {
        // A generic plugin block: a rounded square with a small plug pin.
        p.setPen(QPen(c, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4.5, 4.5, 15.0, 15.0), 3.0, 3.0);
        p.drawLine(QPointF(12, 8.5), QPointF(12, 12));
        p.drawLine(QPointF(9.5, 12), QPointF(14.5, 12));
        p.drawLine(QPointF(12, 12), QPointF(12, 15.5));
        break;
    }
    case Glyph::Assistant: {
        // A conversation bubble with a tiny connected-node motif: assistant,
        // not a generic sparkle or a brand-specific logo.
        QPainterPath bubble;
        bubble.addRoundedRect(QRectF(4.0, 5.0, 16.0, 13.0), 4.0, 4.0);
        bubble.moveTo(8.0, 17.0);
        bubble.lineTo(6.6, 21.0);
        bubble.lineTo(11.3, 17.8);
        strokePath(p, bubble, c, 1.7);
        p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8.5, 12.8), QPointF(12.0, 9.4));
        p.drawLine(QPointF(12.0, 9.4), QPointF(15.8, 12.2));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(8.5, 12.8), 1.35, 1.35);
        p.drawEllipse(QPointF(12.0, 9.4), 1.35, 1.35);
        p.drawEllipse(QPointF(15.8, 12.2), 1.35, 1.35);
        break;
    }
    case Glyph::Image: {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(4.2, 5.0, 15.6, 14.0), 2.0, 2.0);
        p.drawEllipse(QPointF(15.3, 9.0), 1.55, 1.55);
        QPainterPath landscape;
        landscape.moveTo(6.3, 16.8);
        landscape.lineTo(10.3, 12.5);
        landscape.lineTo(12.8, 14.8);
        landscape.lineTo(15.0, 12.7);
        landscape.lineTo(18.0, 16.8);
        p.drawPath(landscape);
        break;
    }
    case Glyph::Notebook: {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(6.0, 3.8, 13.5, 16.4), 2.1, 2.1);
        p.drawLine(QPointF(9.1, 4.2), QPointF(9.1, 19.8));
        p.drawLine(QPointF(11.7, 8.2), QPointF(16.8, 8.2));
        p.drawLine(QPointF(11.7, 11.8), QPointF(16.8, 11.8));
        p.drawLine(QPointF(11.7, 15.4), QPointF(15.1, 15.4));
        p.drawLine(QPointF(4.2, 7.0), QPointF(7.2, 7.0));
        p.drawLine(QPointF(4.2, 11.8), QPointF(7.2, 11.8));
        p.drawLine(QPointF(4.2, 16.6), QPointF(7.2, 16.6));
        break;
    }
    // ── Collaboration ──
    case Glyph::Cloud:
    case Glyph::CloudUpload:
    case Glyph::CloudOff: {
        // One silhouette for all three so the strip reads as a single family;
        // the arrow and the slash are what distinguish the states, and both
        // stay legible at 14px.
        QPainterPath cloud;
        cloud.moveTo(6.6, 17.4);
        cloud.arcTo(QRectF(2.6, 10.0, 8.0, 8.0), 270.0, -160.0);
        cloud.arcTo(QRectF(5.2, 5.6, 9.4, 9.4), 160.0, -150.0);
        cloud.arcTo(QRectF(12.0, 9.2, 8.8, 8.8), 100.0, -170.0);
        cloud.closeSubpath();
        strokePath(p, cloud, c, 1.7);
        if (g == Glyph::CloudUpload) {
            QPainterPath arrow;
            arrow.moveTo(12.0, 20.4);
            arrow.lineTo(12.0, 13.6);
            arrow.moveTo(9.4, 16.0);
            arrow.lineTo(12.0, 13.4);
            arrow.lineTo(14.6, 16.0);
            strokePath(p, arrow, c, 1.7);
        } else if (g == Glyph::CloudOff) {
            p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(4.6, 19.6), QPointF(19.4, 4.8));
        }
        break;
    }
    case Glyph::Users: {
        // Two heads, the rear one clipped, so a participant count reads as
        // people rather than as a generic contact card.
        p.setPen(QPen(c, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(9.4, 8.8), 3.3, 3.3);
        QPainterPath body;
        body.moveTo(3.6, 19.2);
        body.arcTo(QRectF(3.6, 12.6, 11.6, 13.2), 180.0, -180.0);
        strokePath(p, body, c, 1.7);
        QPainterPath second;
        second.moveTo(14.4, 6.2);
        second.arcTo(QRectF(13.2, 5.5, 6.6, 6.6), 90.0, -180.0);
        strokePath(p, second, c, 1.6);
        QPainterPath secondBody;
        secondBody.moveTo(16.4, 14.0);
        secondBody.arcTo(QRectF(14.0, 13.4, 8.0, 11.0), 120.0, -120.0);
        strokePath(p, secondBody, c, 1.6);
        break;
    }
    case Glyph::Link: {
        // Two interlocking capsules on the diagonal — a shareable link, not a
        // chain of arbitrary length.
        p.setPen(QPen(c, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(12.0, 12.0);
        p.rotate(-45.0);
        p.drawRoundedRect(QRectF(-7.6, -3.1, 8.4, 6.2), 3.1, 3.1);
        p.drawRoundedRect(QRectF(-0.8, -3.1, 8.4, 6.2), 3.1, 3.1);
        p.drawLine(QPointF(-2.6, 0.0), QPointF(2.6, 0.0));
        p.restore();
        break;
    }
    case Glyph::Key: {
        // A password on the session. The bit points down-right so it does not
        // collide with the Gear glyph at small sizes.
        p.setPen(QPen(c, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(8.6, 8.6), 4.0, 4.0);
        QPainterPath bit;
        bit.moveTo(11.5, 11.5);
        bit.lineTo(19.4, 19.4);
        bit.moveTo(17.6, 17.6);
        bit.lineTo(15.6, 19.6);
        bit.moveTo(15.0, 15.0);
        bit.lineTo(13.0, 17.0);
        strokePath(p, bit, c, 1.75);
        break;
    }
    case Glyph::Check: {
        QPainterPath tick;
        tick.moveTo(5.2, 12.6);
        tick.lineTo(10.0, 17.2);
        tick.lineTo(18.9, 7.0);
        strokePath(p, tick, c, 2.1);
        break;
    }
    case Glyph::Warning: {
        // A triangle rather than a circle: the strip already uses round marks
        // for state, so shape alone separates "attention" from "status".
        QPainterPath frame;
        frame.moveTo(12.0, 3.9);
        frame.lineTo(21.4, 20.1);
        frame.lineTo(2.6, 20.1);
        frame.closeSubpath();
        QPen pen(c, 1.7);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(frame);
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12.0, 9.9), QPointF(12.0, 14.6));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(12.0, 17.4), 1.05, 1.05);
        break;
    }
    case Glyph::Spinner: {
        // A three-quarter arc with a tapered tail. Callers rotate it; drawing
        // an animation here would tie the glyph to a timer it does not own.
        QPainterPath arc;
        arc.arcMoveTo(QRectF(4.4, 4.4, 15.2, 15.2), 90.0);
        arc.arcTo(QRectF(4.4, 4.4, 15.2, 15.2), 90.0, -270.0);
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(arc);
        break;
    }
    }
}

} // namespace

void paint(QPainter& p, Glyph glyph, const QRectF& rect, const QColor& color) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal side = std::min(rect.width(), rect.height());
    p.translate(rect.center());
    p.scale(side / kUnit, side / kUnit);
    p.translate(-kUnit / 2.0, -kUnit / 2.0);
    drawGlyph(p, glyph, color);
    p.restore();
}

bool checkGlyphCoverageForTest(QString* error) {
    for (int value = 0; value <= int(Glyph::Spinner); ++value) {
        const auto glyph = static_cast<Glyph>(value);
        QImage canvas(48, 48, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        {
            QPainter painter(&canvas);
            paint(painter, glyph, QRectF(0, 0, 48, 48), QColor(0, 0, 0));
        }
        int inked = 0;
        for (int y = 0; y < canvas.height() && inked < 8; ++y) {
            const auto* row =
                reinterpret_cast<const QRgb*>(canvas.constScanLine(y));
            for (int x = 0; x < canvas.width(); ++x) {
                if (qAlpha(row[x]) > 8) ++inked;
            }
        }
        if (inked < 8) {
            if (error) {
                *error = QStringLiteral("glyph %1 draws nothing").arg(value);
            }
            return false;
        }
    }
    return true;
}

QIcon icon(Glyph glyph, const QColor& color, int size) {
    if (size <= 0) return {};
    QIcon result;
    for (int scale = 1; scale <= 3; ++scale) {
        QPixmap pixmap(size * scale, size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(painter, glyph, QRectF(0, 0, size, size), color);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

QIcon svgIcon(const QString& fileName, const QColor& color, int size) {
    const QString path = fileName.startsWith(QLatin1Char(':'))
                             ? fileName
                             : QStringLiteral(":/icons/%1").arg(fileName);
    QSvgRenderer renderer(path);
    if (!renderer.isValid() || size <= 0) return {};

    QIcon result;
    for (int scale = 1; scale <= 3; ++scale) {
        QPixmap pixmap(size * scale, size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter, QRectF(0, 0, size, size));
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRectF(0, 0, size, size), color);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

} // namespace icons
