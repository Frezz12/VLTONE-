#include "LoudnessDisplay.hpp"
#include "Theme.hpp"
#include "Icons.hpp"
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace {
QString reading(float value) {
    if (std::isnan(value)) return QStringLiteral("—");
    if (!std::isfinite(value)) return QStringLiteral("−∞");
    return QString::number(value, 'f', 1);
}
}

LoudnessDisplay::LoudnessDisplay(QWidget* parent) : QAbstractButton(parent) {
    setObjectName("MasterLoudnessDisplay");
    setFixedHeight(68); setMinimumWidth(56);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor); setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, qOverload<>(&QWidget::update));
    setLevels({});
}

void LoudnessDisplay::setLevels(daw::engine::LoudnessLevels levels) {
    const QString summary = tr("Master loudness (LUFS)\nM · 400 ms: %1\nS · 3 s: %2\nI · since reset: %3\nI measures during playback. Click to reset.")
        .arg(reading(levels.momentary), reading(levels.shortTerm), reading(levels.integrated));
    if (summary == m_summary) return;
    m_levels = levels; m_summary = summary;
    setToolTip(summary); setAccessibleName(summary); update();
}

void LoudnessDisplay::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const auto& theme = ThemeManager::instance().theme();
    const QRectF face = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
    constexpr qreal radius = 4;
    QLinearGradient recess(face.topLeft(), face.bottomLeft());
    recess.setColorAt(0, theme.wellTop());
    recess.setColorAt(1, theme.wellBottom());
    QLinearGradient rim(face.topLeft(), face.bottomLeft());
    rim.setColorAt(0, theme.edgeDark(theme.well()));
    rim.setColorAt(0.22, theme.separator());
    rim.setColorAt(1, theme.edgeLight(theme.surface));
    p.setBrush(recess); p.setPen(QPen(QBrush(rim), 1));
    p.drawRoundedRect(face, radius, radius);
    QFont caption = font(); caption.setPixelSize(9); caption.setWeight(QFont::Medium);
    p.setFont(caption); p.setPen(theme.textSecondary);
    p.drawText(QRectF(6, 3, width() - 25, 13), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("LUFS"));
    icons::paint(p, icons::Glyph::Restart, QRectF(width() - 17, 3, 12, 12),
                 underMouse() || hasFocus() ? theme.textPrimary : theme.textSecondary);
    QFont number = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    number.setPixelSize(11); number.setWeight(QFont::Medium);
    const std::array<float, 3> values{m_levels.momentary, m_levels.shortTerm, m_levels.integrated};
    const std::array<const char*, 3> labels{"M", "S", "I"};
    for (int row = 0; row < 3; ++row) {
        const QRectF line(6, 19 + row * 15, width() - 12, 14);
        p.setFont(caption); p.setPen(theme.textSecondary);
        p.drawText(line, Qt::AlignLeft | Qt::AlignVCenter, QLatin1String(labels[row]));
        p.setFont(number); p.setPen(row == 1 ? theme.textPrimary : theme.textSecondary);
        p.drawText(line, Qt::AlignRight | Qt::AlignVCenter, reading(values[row]));
    }
}
