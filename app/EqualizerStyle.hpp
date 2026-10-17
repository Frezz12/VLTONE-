#pragma once

#include "Theme.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QListView>
#include <QPainter>
#include <algorithm>
#include <iterator>
#include <string_view>

namespace eqUi {
inline QColor canvas() { return th().dark ? QColor("#15191c") : QColor("#f6f6f1"); }
inline QColor field() { return mixColors(canvas(), th().textPrimary, th().dark ? .065 : .045); }
inline QColor bandColor(int band, bool dark) {
    static constexpr int hues[] = {316, 190, 264, 142, 44, 8, 288, 168, 218, 96, 26, 340};
    return QColor::fromHsv(hues[std::size_t(std::max(0, band)) % std::size(hues)],
                          dark ? 168 : 205, dark ? 246 : 190);
}

inline QString displayName(std::string_view uid, std::string_view name) {
    if (uid == "daw.equalizer" &&
        (name == "VLTONE Equalizer" || name == "Equalizer" || name == "VLT Equalizer"))
        return QStringLiteral("EQ");
    return QString::fromUtf8(name.data(), int(name.size()));
}

// Keep Qt's keyboard navigation, popup dismissal and accessibility, while
// drawing the closed field without the application's bevelled chrome.
class ComboBox final : public QComboBox {
public:
    explicit ComboBox(QWidget* parent) : QComboBox(parent) {
        setView(new QListView(this));
        setMinimumHeight(30);
        setFocusPolicy(Qt::StrongFocus);
        const auto restyle = [this] {
            view()->setStyleSheet(QStringLiteral(
                "QListView { background: %1; color: %2; border: 1px solid %3;"
                "padding: 4px; outline: none; }"
                "QListView::item { min-height: 28px; padding: 0 10px; border-radius: 4px; }"
                "QListView::item:selected { background: %4; color: %2; }")
                .arg(canvas().name(), th().textPrimary.name(), th().separator().name(),
                     mixColors(canvas(), th().accent, .22).name()));
            update();
        };
        connect(&ThemeManager::instance(), &ThemeManager::changed, this, restyle);
        restyle();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(isEnabled() ? 1. : .38);
        p.setPen(hasFocus() ? QPen(th().accent, 1.3) : QPen(Qt::NoPen));
        p.setBrush(mixColors(field(), th().textPrimary, underMouse() ? .045 : 0.));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 5, 5);
        p.setPen(th().textPrimary);
        p.drawText(QRect(10, 0, width() - 32, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(currentText(), Qt::ElideRight, width() - 32));
        p.setPen(QPen(th().textSecondary, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF c(width() - 14., height() * .5);
        p.drawPolyline(QPolygonF{c + QPointF(-3, -1.5), c + QPointF(0, 1.5), c + QPointF(3, -1.5)});
    }
};

class CheckBox final : public QCheckBox {
public:
    CheckBox(const QString& text, QWidget* parent) : QCheckBox(text, parent) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(28);
    }
    QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(text()) + 30, 28}; }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    bool hitButton(const QPoint& p) const override { return rect().contains(p); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(isEnabled() ? 1. : .38);
        p.setPen(hasFocus() ? QPen(th().accent, 1.2) : QPen(Qt::NoPen));
        p.setBrush(isChecked() ? mixColors(canvas(), th().accent, .17)
                              : underMouse() ? field() : Qt::transparent);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 5, 5);
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? th().accentHighlight : th().textSecondary);
        p.drawEllipse(QPointF(11, height() * .5), 2.5, 2.5);
        p.setPen(isChecked() ? th().textPrimary : th().textSecondary);
        p.drawText(QRect(21, 0, width() - 25, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    }
};
} // namespace eqUi
