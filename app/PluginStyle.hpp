#pragma once

#include "Theme.hpp"
#include <QPainter>
#include <QPalette>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <numbers>

// The Slicer material shared by every built-in editor. No animation or timer
// is involved; controls retain their own parameter and gesture implementations.
namespace pluginStyle {
inline QColor shell() {
  return mixColors(th().surfaceElevated, th().textPrimary,
                   th().dark ? .08 : .025);
}
inline QColor accent() {
  return mixColors(th().accent,
                   th().dark ? QColor(221, 181, 122) : QColor(126, 87, 36),
                   .72);
}
inline QPalette palette() {
  const auto &t = th();
  QPalette p;
  p.setColor(QPalette::Window, shell());
  p.setColor(QPalette::WindowText, t.textPrimary);
  p.setColor(QPalette::Text, t.textPrimary);
  p.setColor(QPalette::Button, shell());
  p.setColor(QPalette::ButtonText, t.textPrimary);
  p.setColor(QPalette::Base, t.well());
  p.setColor(QPalette::AlternateBase, t.surface);
  p.setColor(QPalette::Mid, t.separator());
  p.setColor(QPalette::Light, t.edgeLight(shell()));
  p.setColor(QPalette::Dark, t.edgeDark(shell()));
  p.setColor(QPalette::Highlight, accent());
  auto selected = t;
  selected.accent = accent();
  p.setColor(QPalette::HighlightedText, selected.accentText());
  p.setColor(QPalette::Disabled, QPalette::Text, t.textSecondary);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textSecondary);
  return p;
}
inline void bind(QWidget *widget) {
  const auto apply = [widget] {
    widget->setPalette(palette());
    widget->update();
    for (auto *child : widget->findChildren<QWidget *>())
      child->update();
  };
  QObject::connect(&ThemeManager::instance(), &ThemeManager::changed, widget,
                   apply);
  apply();
}
inline void surface(QPainter &p, QRectF r, bool recessed = false,
                    double radius = 15) {
  const auto &t = th();
  const auto base = recessed ? t.well() : shell();
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0, 0, 0, t.dark ? 65 : 20));
  p.drawRoundedRect(r.translated(0, 2), radius, radius);
  QLinearGradient material(r.topLeft(), r.bottomRight());
  material.setColorAt(0, recessed ? t.edgeDark(base) : t.edgeLight(base));
  material.setColorAt(.45, base);
  material.setColorAt(1, recessed ? t.edgeLight(base) : t.edgeDark(base));
  p.setBrush(material);
  p.setPen(QPen(t.edgeLight(base), 1));
  p.drawRoundedRect(r, radius, radius);
  p.restore();
}
inline void knob(QPainter &p, QRectF bounds, double fraction, bool pressed,
                 bool enabled, bool focus) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setOpacity(enabled ? 1.0 : .45);
  const auto &t = th();
  const auto base = shell();
  const double side = std::min(bounds.width(), bounds.height());
  const double scale = side / 72.;
  // The moulded rubber stays anchored while editing. Press feedback belongs
  // to the inlay, never to the position or depth of the control.
  p.translate(bounds.center());
  p.scale(scale, scale);
  p.setPen(Qt::NoPen);
  // A broad, feathered foot joins the dome to the panel, without a metal rim
  // or a hard drop shadow. Both light lobes fade out before the widget edge.
  QRadialGradient shadow(QPointF(2, 3), 32);
  shadow.setColorAt(0, QColor(0, 0, 0, t.dark ? 115 : 48));
  shadow.setColorAt(.60, QColor(0, 0, 0, t.dark ? 95 : 40));
  shadow.setColorAt(.82, QColor(0, 0, 0, t.dark ? 30 : 12));
  shadow.setColorAt(1, Qt::transparent);
  p.setBrush(shadow);
  p.drawEllipse(QRectF(-30, -29, 64, 64));
  QRadialGradient shoulder(QPointF(-3, -4), 31);
  shoulder.setColorAt(0, QColor(255, 255, 255, t.dark ? 28 : 100));
  shoulder.setColorAt(.65, QColor(255, 255, 255, t.dark ? 24 : 85));
  shoulder.setColorAt(.85, QColor(255, 255, 255, t.dark ? 7 : 25));
  shoulder.setColorAt(1, Qt::transparent);
  p.setBrush(shoulder);
  p.drawEllipse(QRectF(-34, -35, 62, 62));

  QRadialGradient rubber(QPointF(-8, -10), 49);
  rubber.setColorAt(0, mixColors(base, Qt::white, t.dark ? .22 : .38));
  rubber.setColorAt(.38, mixColors(base, Qt::white, t.dark ? .09 : .15));
  rubber.setColorAt(.64, base);
  rubber.setColorAt(.86, mixColors(base, Qt::black, t.dark ? .23 : .14));
  rubber.setColorAt(1, mixColors(base, Qt::black, t.dark ? .32 : .20));
  p.setBrush(rubber);
  p.drawEllipse(QRectF(-23.5, -23.5, 47, 47));

  // A low-contrast moulded grip is useful on the larger dials and disappears
  // at compact sizes, where it would compete with the value indicator.
  if (side >= 60) {
    p.setPen(QPen(QColor(0, 0, 0, t.dark ? 28 : 18), .8, Qt::SolidLine,
                  Qt::RoundCap));
    for (int i = -1; i <= 1; ++i)
      p.drawLine(QPointF(i * 3., 15), QPointF(i * 3., 18));
  }
  const double angle =
      (225 - 270 * std::clamp(fraction, 0., 1.)) * std::numbers::pi / 180.;
  const QPointF direction(std::cos(angle), -std::sin(angle));
  p.setPen(QPen(QColor(0, 0, 0, t.dark ? 110 : 45), 4.2,
                Qt::SolidLine, Qt::RoundCap));
  p.drawLine(direction * 10.5, direction * 18);
  p.setPen(QPen(pressed ? accent() : t.textPrimary, 2.5,
                Qt::SolidLine, Qt::RoundCap));
  p.drawLine(direction * 10.5, direction * 18);
  if (focus) {
    p.setPen(QPen(accent(), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(-32, -32, 64, 64));
  }
  p.restore();
}
} // namespace pluginStyle
