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
  p.translate(bounds.center() + QPointF(0, pressed ? scale : 0));
  p.scale(scale, scale);
  // Concentric circles keep the rim round. Directional lighting changes
  // colour, never the silhouette or the centre of the knob.
  const QRectF shade(-30, -30, 60, 60);
  p.setPen(Qt::NoPen);
  QRadialGradient shadow(shade.center(), 30);
  shadow.setColorAt(.70, QColor(0, 0, 0, t.dark ? 100 : 45));
  shadow.setColorAt(1, Qt::transparent);
  p.setBrush(shadow);
  p.drawEllipse(shade);
  QLinearGradient rim(QPointF(-22, -22), QPointF(22, 22));
  rim.setColorAt(0, mixColors(base, t.textPrimary, t.dark ? .22 : .12));
  rim.setColorAt(.5, base);
  rim.setColorAt(1, t.edgeDark(base));
  p.setBrush(rim);
  p.drawEllipse(QRectF(-23, -23, 46, 46));
  QLinearGradient rubber(QPointF(-22, -22), QPointF(22, 22));
  rubber.setColorAt(0, mixColors(base, t.textPrimary, t.dark ? .15 : .06));
  rubber.setColorAt(.6, base);
  rubber.setColorAt(1, t.edgeDark(base));
  p.setBrush(rubber);
  p.drawEllipse(QRectF(-22, -22, 44, 44));
  const double angle =
      (225 - 270 * std::clamp(fraction, 0., 1.)) * std::numbers::pi / 180.;
  const QPointF direction(std::cos(angle), -std::sin(angle));
  p.setPen(QPen(t.textPrimary, 2.6, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(direction * 11, direction * 18);
  if (focus) {
    p.setPen(QPen(accent(), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(-25, -25, 50, 50));
  }
  p.restore();
}
} // namespace pluginStyle
