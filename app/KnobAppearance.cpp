#include "KnobAppearance.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QPainter>
#include <QPixmapCache>
#include <QSettings>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ui {
namespace {
KnobDesign& currentDesign() {
    static auto design = QSettings().value("ui/knobDesign").toString() == "brushed-metal"
        ? KnobDesign::BrushedMetal : KnobDesign::Classic;
    return design;
}

QPixmap metalFace(int pixels, const QColor& base, bool dark) {
    const auto key = QString("vlt-metal-knob:%1:%2:%3").arg(pixels).arg(base.rgba()).arg(dark);
    QPixmap face;
    if (QPixmapCache::find(key, &face)) return face;
    face = QPixmap(pixels, pixels);
    face.fill(Qt::transparent);
    QPainter p(&face);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(pixels / 100.0, pixels / 100.0);
    const QColor light = mixColors(base, Qt::white, .66);
    const QColor shade = mixColors(base, Qt::black, .46);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, dark ? 32 : 15));
    p.drawEllipse(QRectF(0, 6, 100, 94));
    p.setBrush(QColor(0, 0, 0, dark ? 55 : 24));
    p.drawEllipse(QRectF(1, 7, 98, 92));
    p.setBrush(QColor(0, 0, 0, dark ? 110 : 62));
    p.drawEllipse(QRectF(4, 10, 92, 88));
    QLinearGradient collar(0, 2, 0, 99);
    collar.setColorAt(0, mixColors(base, Qt::black, .62));
    collar.setColorAt(.50, mixColors(base, Qt::black, .84));
    collar.setColorAt(1, mixColors(base, Qt::black, .61));
    p.setBrush(collar); p.drawEllipse(QRectF(2, 3, 96, 95));

    // A small exposed sidewall and a soft contact shadow seat the bright face.
    QLinearGradient wall(0, 7, 0, 96);
    wall.setColorAt(0, mixColors(base, Qt::black, .20));
    wall.setColorAt(.65, mixColors(base, Qt::black, .42));
    wall.setColorAt(1, mixColors(base, Qt::black, .63));
    p.setBrush(wall); p.drawEllipse(QRectF(5, 7, 90, 89));
    QLinearGradient bevel(25, 3, 70, 92);
    bevel.setColorAt(0, light);
    bevel.setColorAt(.24, mixColors(base, Qt::white, .20));
    bevel.setColorAt(.54, mixColors(base, Qt::black, .10));
    bevel.setColorAt(.82, shade);
    bevel.setColorAt(1, mixColors(base, Qt::white, .26));
    p.setBrush(bevel);
    p.setPen(QPen(mixColors(base, Qt::black, .50), .65));
    p.drawEllipse(QRectF(6, 5, 88, 88));

    const QPointF centre(50, 49);
    const QRectF cap(9, 8, 82, 82);
    QConicalGradient steel(centre, 32);
    // Four broad shadow lobes with eased shoulders instead of three linear,
    // triangular wedges. The periodic curve also joins cleanly at the seam.
    for (int i = 0; i <= 64; ++i) {
        const double position = i / 64.0;
        const double reflection = .5 + .5 * std::cos(position * 8 * std::numbers::pi);
        steel.setColorAt(position, mixColors(shade, light, reflection));
    }
    p.setBrush(steel); p.setPen(QPen(mixColors(base, Qt::black, .22), .65));
    p.drawEllipse(cap);
    // Diffuse the meeting point so the reflections read as a surface, rather
    // than hard spokes converging on a pin-sharp centre.
    QRadialGradient hub(centre, cap.width() * .5);
    QColor haze = base;
    haze.setAlpha(95); hub.setColorAt(0, haze);
    haze.setAlpha(30); hub.setColorAt(.25, haze);
    haze.setAlpha(0); hub.setColorAt(.65, haze);
    p.setBrush(hub); p.setPen(Qt::NoPen); p.drawEllipse(cap);
    QLinearGradient crown(0, cap.top(), 0, cap.bottom());
    crown.setColorAt(0, QColor(255, 255, 255, 20));
    crown.setColorAt(.30, Qt::transparent);
    crown.setColorAt(1, QColor(0, 0, 0, 28));
    p.setBrush(crown); p.setPen(Qt::NoPen);
    p.drawEllipse(cap);

    // Fine machining only survives on larger faces; omit it on tiny pan pots.
    if (pixels >= 48) {
        p.setBrush(Qt::NoBrush);
        for (double radius = 6; radius < 40; radius += 1.4) {
            p.setPen(QPen(QColor(0, 0, 0, 13), .35));
            p.drawEllipse(centre, radius, radius);
            p.setPen(QPen(QColor(255, 255, 255, 12), .25));
            p.drawEllipse(centre, radius + .45, radius + .45);
        }
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 62), .75));
    p.drawArc(cap, 28 * 16, 126 * 16);
    p.end();
    QPixmapCache::insert(key, face);
    return face;
}
}

KnobDesign knobDesign() { return currentDesign(); }

void setKnobDesign(KnobDesign design) {
    if (currentDesign() == design) return;
    currentDesign() = design;
    QSettings().setValue("ui/knobDesign", design == KnobDesign::BrushedMetal ? "brushed-metal" : "classic");
    // A preference change invalidates existing widgets once, including custom
    // plugin controls. Painting reads the cached enum, never the settings file.
    for (auto* widget : QApplication::allWidgets()) widget->update();
}

bool paintMetalKnob(QPainter& p, const QRectF& body, double angleDegrees) {
    if (knobDesign() != KnobDesign::BrushedMetal) return false;
    if (body.width() <= 0 || body.height() <= 0) return true;
    const auto& theme = th();
    const QColor base = mixColors(QColor(173, 180, 188), theme.surfaceElevated, theme.dark ? .22 : .12);
    const double scale = std::hypot(p.worldTransform().m11(), p.worldTransform().m12());
    const int pixels = std::clamp(int(std::ceil(body.width() * scale * p.device()->devicePixelRatioF())), 8, 1024);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto face = metalFace(pixels, base, theme.dark);
    p.drawPixmap(body, face, face.rect());

    const double angle = angleDegrees * std::numbers::pi / 180.0;
    const QPointF centre = body.center() - QPointF(0, body.height() * .01);
    const QPointF marker = centre + QPointF(std::cos(angle), -std::sin(angle)) * (body.width() * .31);
    // Some plugin editors paint in scaled coordinates. Keep the luminous
    // inlay readable in screen units, even on the smallest pan controls.
    const double markerScale = std::max(scale, .001);
    const double radius = std::clamp(body.width() * markerScale * .028, 1.45, 3.6) / markerScale;
    p.setPen(Qt::NoPen);
    for (int layer = 4; layer >= 1; --layer) {
        QColor glow = theme.accent;
        glow.setAlpha(24 - layer * 4);
        p.setBrush(glow);
        const double halo = radius * (1.0 + layer * .32);
        p.drawEllipse(marker, halo, halo);
    }
    QLinearGradient light(marker - QPointF(0, radius), marker + QPointF(0, radius));
    light.setColorAt(0, mixColors(theme.accent, Qt::white, .55));
    light.setColorAt(.45, mixColors(theme.accent, Qt::white, .22));
    light.setColorAt(1, theme.accent);
    // A dark socket separates the lit centre from the metal's brightest lobes.
    p.setPen(QPen(mixColors(theme.accent, Qt::black, .72),
                  std::max(.7 / markerScale, radius * .22)));
    p.setBrush(light);
    p.drawEllipse(marker, radius, radius);
    p.restore();
    return true;
}
}
