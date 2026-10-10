#pragma once

#include "model/Document.hpp"
#include <QRectF>
#include <QString>
#include <algorithm>
#include <cmath>

namespace ui::sequence {

inline QString pitchName(int pitch) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    pitch = std::clamp(pitch, 0, 127);
    return QString::fromLatin1(names[pitch % 12]) + QString::number(pitch / 12);
}

struct Division { const char* name; double beats; };
inline constexpr Division divisions[] = {
    {"1/4", 1}, {"1/8", .5}, {"1/16", .25}, {"1/32", .125}, {"1/64", .0625},
    {"1/4T", 2.0 / 3}, {"1/8T", 1.0 / 3}, {"1/16T", 1.0 / 6}
};
inline QString divisionName(double step) {
    for (const auto& d : divisions)
        if (std::abs(step - d.beats) < 1e-8) return QString::fromLatin1(d.name);
    return QString::number(step, 'g', 3);
}

/// The same rectangles serve painting and hit testing. Controls stay in the
/// visible part of a clipped header; steps retain their exact musical anchor.
struct Layout {
    QRectF header, name, pitch, division, midi, sequence, steps, footer;
    Layout(QRectF clip, bool sequenced, double viewportRight) {
        if (clip.height() < 36) return;
        header = QRectF(std::max(0.0, clip.left()) + 1, clip.top(),
                        std::max(0.0, std::min(viewportRight, clip.right()) - std::max(0.0, clip.left()) - 2), 24);
        double right = header.right() - 5;
        if (header.width() >= 62) {
            sequence = QRectF(right - 24, header.top(), 24, 24);
            midi = sequence.translated(-24, 0);
            right = midi.left() - 4;
        }
        double left = header.left() + 5;
        // Keep a header area available for moving the clip. Narrow clips
        // expose the same pitch/grid controls through their context menu.
        if (sequenced && right - left >= 124) {
            pitch = QRectF(left, header.top(), 44, 24);
            division = QRectF(left + 46, header.top(), 48, 24);
            left += 100;
        }
        name = QRectF(left, header.top(), std::max(0.0, right - left), 24);
        const double footerHeight = clip.height() >= 60 ? 12 : 0;
        steps = QRectF(clip.left(), header.bottom() + 7, clip.width(),
                       std::max(0.0, clip.bottom() - header.bottom() - 10 - footerHeight));
        footer = QRectF(header.left() + 5, clip.bottom() - footerHeight,
                        std::max(0.0, header.width() - 10), footerHeight);
    }
};

inline double stepAt(double beat, double step) {
    return std::floor((beat + 1e-8) / step) * step;
}

inline QRectF cellRect(const Layout& layout, double x, double stepPixels, int velocity = 127) {
    const double inset = std::min(3.0, stepPixels * .15);
    const double fullHeight = layout.steps.height();
    const double h = std::min(fullHeight, 5.0 + std::clamp(velocity, 1, 127) / 127.0 * std::max(0.0, fullHeight - 5));
    return QRectF(x + inset, layout.steps.bottom() - h,
                  std::max(1.0, stepPixels - 2 * inset), h);
}
} // namespace ui::sequence
