#include "LevelMeter.h"

#include <QLinearGradient>
#include <QPainter>

#include <cmath>

namespace {
constexpr float kMinDb = -60.0f;
constexpr int   kHoldFrames = 45;   // ~0.75 c при 60 Гц
}

LevelMeter::LevelMeter(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

float LevelMeter::linearToNormalizedDb(float linear) {
    if (linear <= 0.0f)
        return 0.0f;
    const float db = 20.0f * std::log10(linear);
    if (db <= kMinDb)
        return 0.0f;
    if (db >= 0.0f)
        return 1.0f;
    return (db - kMinDb) / (0.0f - kMinDb);
}

void LevelMeter::setLevel(float linear) {
    const float v = linearToNormalizedDb(linear);

    // Быстрая атака, медленный спад — так метр читается глазом.
    level_ = (v > level_) ? v : level_ * 0.90f;

    if (v >= hold_) {
        hold_ = v;
        holdCountdown_ = kHoldFrames;
    } else if (--holdCountdown_ <= 0) {
        hold_ = hold_ * 0.95f;
    }

    update();
}

QSize LevelMeter::sizeHint()        const { return QSize(220, 14); }
QSize LevelMeter::minimumSizeHint() const { return QSize(60, 10); }

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRect r = rect().adjusted(0, 0, -1, -1);

    p.fillRect(r, QColor(24, 24, 26));

    const int filled = static_cast<int>(std::lround(level_ * r.width()));
    if (filled > 0) {
        QLinearGradient g(r.left(), 0, r.right(), 0);
        g.setColorAt(0.00, QColor(60, 180, 90));
        g.setColorAt(0.70, QColor(90, 200, 90));
        g.setColorAt(0.85, QColor(220, 190, 60));
        g.setColorAt(1.00, QColor(220, 70, 60));
        p.fillRect(QRect(r.left(), r.top(), filled, r.height()), g);
    }

    if (hold_ > 0.01f) {
        const int x = r.left() + static_cast<int>(std::lround(hold_ * r.width()));
        p.fillRect(QRect(x - 1, r.top(), 2, r.height()), QColor(240, 240, 240));
    }

    // Отметка -6 дБ — самый полезный ориентир при выставлении уровней.
    const int mark = r.left() + static_cast<int>(std::lround(((-6.0f - kMinDb) / 60.0f) * r.width()));
    p.setPen(QColor(90, 90, 95));
    p.drawLine(mark, r.top(), mark, r.bottom());

    p.setPen(QColor(70, 70, 75));
    p.drawRect(r);
}
