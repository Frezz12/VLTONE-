#include "UtilityPanel.hpp"
#include "Controls.hpp"
#include "RackParameterBinding.hpp"
#include "PluginStyle.hpp"
#include "Theme.hpp"
#include "Internal/UtilityInstance.hpp"
#include <QButtonGroup>
#include <QAccessibleWidget>
#include <QDoubleSpinBox>
#include <QHideEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace {
using Kind = daw::plugins::utility::Kind;
Kind kindFor(const QString& uid) {
    return uid == "daw.gain" ? Kind::Gain : uid == "daw.stereo-utility" ? Kind::Stereo : Kind::Reverse;
}
QColor accent(Kind kind) {
    if (kind == Kind::Gain) return QColor(th().dark ? "#94c6a7" : "#2f5e3f");
    if (kind == Kind::Stereo) return QColor(th().dark ? "#91b8e7" : "#2e517c");
    return QColor(th().dark ? "#d9a0bf" : "#793e5d");
}
QColor panelColor() { return pluginStyle::shell(); }
QFont controlFont(const QWidget* widget, int pixels, QFont::Weight weight = QFont::Medium) {
    auto font = widget->font();
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

class UtilityButton final : public QToolButton {
public:
    UtilityButton(Kind kind, const QString& text, QWidget* parent, bool lamp = false)
        : QToolButton(parent), m_kind(kind), m_lamp(lamp) {
        setText(text);
        setAccessibleName(text);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(26);
        setFont(controlFont(this, 11));
    }
    QSize sizeHint() const override {
        return QSize(fontMetrics().horizontalAdvance(text()) + (m_lamp ? 28 : 18), 28);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto ink = m_kind == Kind::Gain && m_lamp
            ? (th().dark ? Theme::mute() : mixColors(Theme::mute(), th().textPrimary, .4)) : accent(m_kind);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        const auto base = isChecked() ? mixColors(panelColor(), ink, th().dark ? .20 : .13)
                                     : mixColors(panelColor(), th().textPrimary, underMouse() ? .07 : .025);
        QLinearGradient face(r.topLeft(), r.bottomLeft());
        face.setColorAt(0, isDown() ? th().edgeDark(base) : th().edgeLight(base));
        face.setColorAt(1, base);
        p.setBrush(face);
        p.setPen(QPen(th().separator(), 1));
        p.drawRoundedRect(r, 5, 5);
        if (!isEnabled()) p.setOpacity(.40);
        QRectF textRect = r.adjusted(6, 0, -6, 0);
        if (m_lamp) {
            p.setPen(Qt::NoPen);
            p.setBrush(isChecked() ? ink : mixColors(th().textSecondary, base, .65));
            p.drawEllipse(QPointF(r.left() + 10, r.center().y()), 2.5, 2.5);
            textRect.setLeft(r.left() + 18);
        } else if (isChecked()) {
            p.setPen(QPen(ink, 2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(r.center().x() - 7, r.bottom() - 3), QPointF(r.center().x() + 7, r.bottom() - 3));
        }
        p.setFont(font());
        p.setPen(isChecked() ? ink : th().textPrimary);
        p.drawText(textRect, Qt::AlignCenter, text());
    }
private:
    Kind m_kind;
    bool m_lamp;
};

// Native slider keyboard/focus semantics, with an instrument face. Gain gives
// the useful -18..0 dB region more travel than the bottom of its range.
class UtilitySlider final : public QSlider {
public:
    UtilitySlider(Kind kind, const QString& id, double minimum, double maximum, double initial, QWidget* parent)
        : QSlider(id == "gain" ? Qt::Vertical : Qt::Horizontal, parent), m_kind(kind), m_id(id),
          m_default(initial), m_scale(id == "timeMs" ? 1 : 10) {
        setRange(int(minimum * m_scale), int(maximum * m_scale));
        setSingleStep(1);
        setPageStep(int((maximum - minimum) * m_scale / 20));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(orientation() == Qt::Vertical ? Qt::SizeVerCursor : Qt::SizeHorCursor);
        setMinimumSize(orientation() == Qt::Vertical ? QSize(76, 72) : QSize(100, 28));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    double plainValue() const { return value() / m_scale; }
    double plainMinimum() const { return minimum() / m_scale; }
    double plainMaximum() const { return maximum() / m_scale; }
    double plainStep() const { return singleStep() / m_scale; }
    void setPlainValue(double value) { setValue(int(std::lround(value * m_scale))); }
    std::function<void()> resetOrAutomate;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (!isEnabled()) p.setOpacity(.38);
        const auto ink = accent(m_kind);
        const bool vertical = orientation() == Qt::Vertical;
        const auto rail = railRect();
        const double pos = fraction(plainValue());
        const double at = vertical ? rail.bottom() - pos * rail.height() : rail.left() + pos * rail.width();
        p.setPen(QPen(th().edgeLight(panelColor()), 1));
        p.setBrush(mixColors(th().well(), Qt::black, th().dark ? .35 : .12));
        p.drawRoundedRect(rail.adjusted(-1, -1, 1, 1), 3, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        if (vertical) p.drawRoundedRect(QRectF(rail.left(), at, rail.width(), rail.bottom() - at), 2, 2);
        else p.drawRoundedRect(QRectF(rail.left(), rail.top(), at - rail.left(), rail.height()), 2, 2);
        p.setFont(controlFont(this, 10));
        if (vertical) {
            for (double db : {-60., -36., -18., -6., 0., 6., 12., 24.}) {
                const double y = rail.bottom() - fraction(db) * rail.height();
                p.setPen(QPen(db == 0 ? ink : th().textSecondary, db == 0 ? 1.5 : 1));
                p.drawLine(QPointF(rail.left() - 15, y), QPointF(rail.left() - 9, y));
                if (db == -60 || db == 0 || db == 24 || (rail.height() >= 195 && db != 6)) {
                    const QString text = db > 0 ? QStringLiteral("+%1").arg(db) : QString::number(db);
                    p.drawText(QRectF(0, y - 7, rail.left() - 20, 14), Qt::AlignRight | Qt::AlignVCenter, text);
                }
            }
        } else {
            const QString left = m_id == "mix" ? QStringLiteral("Dry") : m_id == "width" ? QStringLiteral("0") : QStringLiteral("50 ms");
            const QString right = m_id == "mix" ? QStringLiteral("Wet") : m_id == "width" ? QStringLiteral("200%") : QStringLiteral("4 s");
            p.setPen(th().textSecondary);
            p.drawText(QRectF(0, height() - 12, width() / 2., 12), Qt::AlignLeft, left);
            p.drawText(QRectF(width() / 2., height() - 12, width() / 2., 12), Qt::AlignRight, right);
            if (m_id == "width") {
                p.drawText(QRectF(0, height() - 12, width(), 12), Qt::AlignCenter, QStringLiteral("100"));
                p.drawLine(QPointF(rail.center().x(), rail.top() - 4), QPointF(rail.center().x(), rail.bottom() + 4));
            }
        }
        const QRectF cap = handleRect();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, th().dark ? 110 : 40));
        p.drawRoundedRect(cap.translated(0, 1.5), 3, 3);
        QLinearGradient grip(cap.topLeft(), vertical ? cap.bottomLeft() : cap.topRight());
        grip.setColorAt(0, th().edgeLight(th().controlTop()));
        grip.setColorAt(.18, th().controlTop());
        grip.setColorAt(.50, th().controlBottom());
        grip.setColorAt(.85, th().controlTop());
        grip.setColorAt(1, th().edgeDark(th().controlBottom()));
        p.setPen(QPen(th().edgeDark(th().well()), 1));
        p.setBrush(grip);
        p.drawRoundedRect(cap, 3, 3);
        // Ribbed rectangular grip, like the mixer channel strips.
        for (int side : {-1, 1}) for (int rib = 0; rib < (vertical ? 3 : 1); ++rib) {
            const double offset = side * (4 + rib * 3);
            p.setPen(QPen(th().edgeDark(th().controlBottom()), 1));
            if (vertical) p.drawLine(QPointF(cap.left() + 4, cap.center().y() + offset), QPointF(cap.right() - 4, cap.center().y() + offset));
            else p.drawLine(QPointF(cap.center().x() + offset, cap.top() + 3), QPointF(cap.center().x() + offset, cap.bottom() - 3));
        }
        p.setPen(QPen(isSliderDown() ? ink : th().textPrimary, 2));
        if (vertical) p.drawLine(QPointF(cap.left() + 3, at), QPointF(cap.right() - 3, at));
        else p.drawLine(QPointF(at, cap.top() + 3), QPointF(at, cap.bottom() - 3));
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) { event->ignore(); return; }
        setFocus(Qt::MouseFocusReason);
        setSliderDown(true);
        m_last = axis(event->position());
        m_dragFraction = fraction(plainValue());
        if (!handleRect().adjusted(-4, -4, 4, 4).contains(event->position()) && !(event->modifiers() & Qt::ShiftModifier)) {
            const auto rail = railRect();
            m_dragFraction = orientation() == Qt::Vertical ? (rail.bottom() - m_last) / rail.height()
                                                         : (m_last - rail.left()) / rail.width();
            commitFraction();
        }
        update();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!isSliderDown()) return;
        const double at = axis(event->position());
        const auto rail = railRect();
        const double travel = orientation() == Qt::Vertical ? -rail.height() : rail.width();
        m_dragFraction += (at - m_last) / travel * (event->modifiers() & Qt::ShiftModifier ? .15 : 1.);
        m_last = at;
        commitFraction();
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) { setSliderDown(false); update(); event->accept(); }
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            setSliderDown(false);
            if (resetOrAutomate) resetOrAutomate();
            event->accept();
        }
    }
private:
    double axis(const QPointF& p) const { return orientation() == Qt::Vertical ? p.y() : p.x(); }
    double fraction(double value) const {
        if (m_id == "gain") {
            if (value <= -18) return (value + 60) / 42 * .30;
            if (value <= 0) return .30 + (value + 18) / 18 * .42;
            return .72 + value / 24 * .28;
        }
        return (value * m_scale - minimum()) / (maximum() - minimum());
    }
    void commitFraction() {
        m_dragFraction = std::clamp(m_dragFraction, 0., 1.);
        double value;
        if (m_id == "gain") value = m_dragFraction <= .30 ? -60 + m_dragFraction / .30 * 42
            : m_dragFraction <= .72 ? -18 + (m_dragFraction - .30) / .42 * 18 : (m_dragFraction - .72) / .28 * 24;
        else value = (minimum() + m_dragFraction * (maximum() - minimum())) / m_scale;
        if (std::abs(m_dragFraction - fraction(m_default)) < .006) value = m_default;
        setPlainValue(value);
    }
    QRectF railRect() const {
        if (orientation() == Qt::Vertical) return QRectF(width() * .62, 16, 4, std::max(1, height() - 32));
        return QRectF(8, 8, std::max(1, width() - 16), 4);
    }
    QRectF handleRect() const {
        const auto rail = railRect();
        const double pos = fraction(plainValue());
        if (orientation() == Qt::Vertical) return QRectF(rail.center().x() - 14, rail.bottom() - pos * rail.height() - 15, 28, 30);
        return QRectF(rail.left() + pos * rail.width() - 6, rail.center().y() - 8, 12, 16);
    }
    Kind m_kind;
    QString m_id;
    double m_default, m_scale, m_last = 0, m_dragFraction = 0;
};

class AccessibleUtilitySlider final : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit AccessibleUtilitySlider(UtilitySlider* slider) : QAccessibleWidget(slider, QAccessible::Slider) {}
    void* interface_cast(QAccessible::InterfaceType type) override {
        return type == QAccessible::ValueInterface ? static_cast<QAccessibleValueInterface*>(this)
                                                   : QAccessibleWidget::interface_cast(type);
    }
    QVariant currentValue() const override { return slider()->plainValue(); }
    QVariant minimumValue() const override { return slider()->plainMinimum(); }
    QVariant maximumValue() const override { return slider()->plainMaximum(); }
    QVariant minimumStepSize() const override { return slider()->plainStep(); }
    void setCurrentValue(const QVariant& value) override {
        bool ok = false;
        const double next = value.toDouble(&ok);
        if (ok && std::isfinite(next) && slider()->isEnabled())
            slider()->setPlainValue(std::clamp(next, slider()->plainMinimum(), slider()->plainMaximum()));
    }
private:
    UtilitySlider* slider() const { return static_cast<UtilitySlider*>(object()); }
};

class UtilityDiagram final : public QWidget {
public:
    UtilityDiagram(Kind kind, QWidget* parent) : QWidget(parent), m_kind(kind) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(30);
    }
    void setState(const std::array<double, 5>& state) {
        if (m_state == state) return;
        m_state = state;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        const auto ink = accent(m_kind);
        p.setPen(QPen(th().edgeDark(panelColor()), 1));
        p.setBrush(mixColors(th().well(), panelColor(), .25));
        p.drawRoundedRect(r, 6, 6);
        p.setFont(controlFont(this, 10, QFont::DemiBold));
        if (m_kind == Kind::Stereo) {
            const double w = m_state[1] / 100.;
            std::array<double, 4> a;
            switch (int(m_state[0])) {
                case 1: a = {.5, .5, .5, .5}; break;
                case 2: a = {.5, -.5, -.5, .5}; break;
                case 3: a = {1, 0, 1, 0}; break;
                case 4: a = {0, 1, 0, 1}; break;
                default: a = {(1+w)*.5, (1-w)*.5, (1-w)*.5, (1+w)*.5}; break;
            }
            if (m_state[3] > .5) { a[0] = -a[0]; a[2] = -a[2]; }
            if (m_state[4] > .5) { a[1] = -a[1]; a[3] = -a[3]; }
            if (m_state[2] > .5) { std::swap(a[0], a[1]); std::swap(a[2], a[3]); }
            const double top = height() * .28, bottom = height() * .72;
            for (int in = 0; in < 2; ++in) for (int out = 0; out < 2; ++out) {
                const double coefficient = a[out * 2 + in];
                if (std::abs(coefficient) < .0001) continue;
                QPainterPath path(QPointF(29, in ? bottom : top));
                path.cubicTo(width() * .42, in ? bottom : top, width() * .58, out ? bottom : top, width() - 29, out ? bottom : top);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(mixColors(ink, th().well(), coefficient < 0 ? .30 : .05), 1.6, coefficient < 0 ? Qt::DashLine : Qt::SolidLine));
                p.drawPath(path);
                p.setPen(QPen(ink, 1.2));
                const double y = out ? bottom : top;
                p.drawLine(QPointF(width() - 34, y - 3), QPointF(width() - 29, y));
                p.drawLine(QPointF(width() - 34, y + 3), QPointF(width() - 29, y));
            }
            if (height() >= 70) {
                p.setPen(th().textSecondary);
                p.drawText(QRectF(7, 5, 50, 12), Qt::AlignLeft, QStringLiteral("IN"));
                p.drawText(QRectF(width() - 57, 5, 50, 12), Qt::AlignRight, QStringLiteral("OUT"));
            }
            for (int side = 0; side < 2; ++side) for (int channel = 0; channel < 2; ++channel) {
                const QRectF node(side ? width() - 26 : 6, (channel ? bottom : top) - 7, 20, 14);
                p.setPen(Qt::NoPen);
                p.setBrush(panelColor());
                p.drawRoundedRect(node, 3, 3);
                p.setPen(side ? ink : th().textPrimary);
                p.drawText(node, Qt::AlignCenter, channel ? QStringLiteral("R") : QStringLiteral("L"));
            }
        } else {
            // Static illustration of recording order and reverse playback;
            // never pretend this is a live waveform or audio meter.
            const double lane = (height() - 8.) / 2.;
            for (int row = 0; row < 2; ++row) {
                const double center = 4 + lane * (row + .5);
                p.setPen(row ? ink : th().textSecondary);
                p.drawText(QRectF(7, center - 7, 43, 14), Qt::AlignVCenter, row ? QStringLiteral("← OUT") : QStringLiteral("IN →"));
                const double start = 57, extent = width() - start - 10;
                for (int i = 0; i < 36; ++i) {
                    const double x = (row ? 35 - i : i) / 35.;
                    const double envelope = std::exp(-x * 3.4) * (.50 + .50 * std::abs(std::sin(x * 29)));
                    const double amplitude = std::max(1., envelope * (lane * .40));
                    p.setPen(QPen(row ? ink : mixColors(th().textSecondary, th().well(), .2), 2, Qt::SolidLine, Qt::RoundCap));
                    p.drawLine(QPointF(start + i * extent / 35, center - amplitude), QPointF(start + i * extent / 35, center + amplitude));
                }
            }
        }
    }
private:
    Kind m_kind;
    std::array<double, 5> m_state{};
};
}

struct UtilityPanel::Impl {
    std::vector<std::function<void()>> sync;
    QWidget* width = nullptr;
    QStackedWidget* time = nullptr;
    UtilityDiagram* diagram = nullptr;
    QLabel* duration = nullptr;
    QLabel* hint = nullptr;
};

UtilityPanel::UtilityPanel(RackParameterBinding* binding, QString uid, QWidget* parent, bool embedded)
    : QWidget(parent), m_binding(binding), m_uid(std::move(uid)), m_timer(embedded ? nullptr : new QTimer(this)),
      m_embedded(embedded), m_impl(std::make_unique<Impl>()) {
    const auto kind = kindFor(m_uid);
    static const bool accessible = [] {
        QAccessible::installFactory([](const QString&, QObject* object) -> QAccessibleInterface* {
            if (auto* slider = dynamic_cast<UtilitySlider*>(object)) return new AccessibleUtilitySlider(slider);
            return nullptr;
        });
        return true;
    }();
    (void)accessible;
    setObjectName("UtilityPanel");
    if (!embedded) setMinimumSize(minimumSizeHint());
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(embedded ? 8 : 16, embedded ? 0 : 14, embedded ? 8 : 16, embedded ? 0 : 14);
    root->setSpacing(embedded ? 2 : 10);
    const auto label = [&](const QString& text) {
        auto* result = new QLabel(text, this);
        result->setFont(controlFont(this, embedded ? 11 : 12, QFont::DemiBold));
        return result;
    };
    if (!embedded) {
        auto* title = label(kind == Kind::Gain ? "GAIN" : kind == Kind::Stereo ? "STEREO" : "REVERSE");
        title->setProperty("utilityTitle", true);
        title->setFont(controlFont(this, 18, QFont::DemiBold));
        if (kind == Kind::Gain) title->setAlignment(Qt::AlignCenter);
        root->addWidget(title);
    }
    const auto parameterMenu = [this](QWidget* widget, const QString& id) {
        widget->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(widget, &QWidget::customContextMenuRequested, this, [this, widget, id](const QPoint& at) {
            QMenu menu(widget);
            auto* reset = menu.addAction(tr("Reset"));
            auto* automate = menu.addAction(tr("Create automation"));
            const auto* parameter = m_binding->info(id);
            automate->setEnabled(parameter && parameter->isAutomatable && m_binding->available());
            const auto* selected = menu.exec(widget->mapToGlobal(at));
            if (selected == reset && parameter) {
                m_binding->write(id, parameter->defaultValue);
                m_binding->finish(id);
                refresh();
            } else if (selected == automate) m_binding->automate(id);
        });
    };
    const auto reset = [&] {
        auto* button = new UtilityButton(kind, QString::fromUtf8("↺"), this);
        button->setObjectName("UtilityReset");
        button->setFixedWidth(28);
        button->setFont(controlFont(this, 17));
        button->setToolTip(tr("Reset parameters"));
        button->setAccessibleName(tr("Reset parameters"));
        connect(button, &QToolButton::clicked, this, [this] {
            for (const auto& p : m_binding->parameters()) m_binding->write(QString::fromStdString(p.id), p.defaultValue);
            m_binding->finishAll();
            refresh();
        });
        return button;
    };
    const auto segments = [&](const QString& id, const QStringList& labels, const QStringList& tips = QStringList{}) {
        auto* group = new QWidget(this);
        group->setObjectName("RackChoice." + id);
        auto* row = new QHBoxLayout(group);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(3);
        auto* exclusive = new QButtonGroup(group);
        for (int i = 0; i < labels.size(); ++i) {
            auto* button = new UtilityButton(kind, labels[i], group);
            button->setObjectName(QStringLiteral("UtilityChoice.%1.%2").arg(id).arg(i));
            button->setCheckable(true);
            if (i < tips.size()) { button->setToolTip(tips[i]); button->setAccessibleName(tips[i]); }
            exclusive->addButton(button, i);
            row->addWidget(button);
            parameterMenu(button, id);
            m_impl->sync.push_back([this, button, id, i] { button->setChecked(int(m_binding->value(id)) == i); });
        }
        connect(exclusive, &QButtonGroup::idClicked, this, [this, id](int index) {
            m_binding->write(id, index);
            m_binding->finish(id);
            refresh();
        });
        return group;
    };
    const auto toggle = [&](const QString& id, const QString& text, const QString& tip) {
        auto* button = new UtilityButton(kind, text, this, true);
        button->setObjectName("UtilityToggle." + id);
        button->setCheckable(true);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        parameterMenu(button, id);
        connect(button, &QToolButton::clicked, this, [this, id](bool checked) {
            m_binding->write(id, checked ? 1 : 0);
            m_binding->finish(id);
            refresh();
        });
        m_impl->sync.push_back([this, button, id] { button->setChecked(m_binding->value(id) >= .5); });
        return button;
    };
    const auto slider = [&](const QString& id, const QString& caption, const QString& suffix, bool gain = false) {
        const auto* info = m_binding->info(id);
        auto* group = new QWidget(this);
        group->setObjectName("UtilityParameter." + id);
        auto* column = new QVBoxLayout(group);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(embedded ? 1 : 5);
        auto* control = new UtilitySlider(kind, id, info->minValue, info->maxValue, info->defaultValue, group);
        control->setObjectName("RackParameter." + id);
        control->setAccessibleName(caption);
        control->setToolTip(tr("Drag to adjust · Shift for fine control · double-click to reset"));
        control->resetOrAutomate = [this, id, info] {
            if (ui::automationCreationMode()) m_binding->automate(id);
            else { m_binding->write(id, info->defaultValue); m_binding->finish(id); refresh(); }
        };
        parameterMenu(control, id);
        auto* value = new QDoubleSpinBox(group);
        value->setObjectName("UtilityValue." + id);
        value->setAccessibleName(caption);
        value->setRange(info->minValue, info->maxValue);
        value->setDecimals(id == "timeMs" ? 0 : 1);
        value->setSingleStep(id == "timeMs" ? 1 : .1);
        value->setSuffix(suffix);
        value->setKeyboardTracking(false);
        value->setButtonSymbols(QAbstractSpinBox::NoButtons);
        value->setAlignment(Qt::AlignRight);
        value->setFont(controlFont(this, gain && !embedded ? 25 : embedded ? 12 : 16, QFont::DemiBold));
        value->setProperty("utilityReadout", embedded ? "compact" : gain ? "gain" : "normal");
        value->setFixedWidth(gain && !embedded ? 148 : id == "timeMs" ? 105 : 92);
        if (embedded && !gain) value->setFixedWidth(id == "timeMs" ? 88 : 80);
        value->setMinimumHeight(gain && !embedded ? 40 : 24);
        value->setToolTip(tr("Type an exact value"));
        parameterMenu(value, id);
        auto* heading = new QHBoxLayout;
        auto* captionLabel = label(caption);
        captionLabel->setBuddy(value);
        const bool inlineSlider = embedded && !gain;
        if (inlineSlider) {
            if (id == "timeMs") captionLabel->hide();
            else heading->addWidget(captionLabel);
            heading->addWidget(control, 1);
        }
        else if (!gain || embedded) heading->addWidget(captionLabel);
        else { captionLabel->hide(); heading->addStretch(); }
        heading->addWidget(value);
        if (gain && !embedded) heading->addStretch();
        column->addLayout(heading);
        if (!inlineSlider) column->addWidget(control, 1);
        if (gain) {
            control->setMaximumWidth(112);
            if (embedded) control->setMinimumHeight(60);
            column->setAlignment(control, Qt::AlignHCenter);
        }
        connect(control, &QSlider::valueChanged, this, [this, control, value, id] {
            m_binding->write(id, control->plainValue());
            { const QSignalBlocker block(value); value->setValue(control->plainValue()); }
            if (!control->isSliderDown()) m_binding->finish(id);
            refresh();
        });
        connect(control, &QSlider::sliderReleased, this, [this, id] { m_binding->finish(id); });
        connect(value, &QDoubleSpinBox::valueChanged, this, [this, id](double next) { m_binding->write(id, next); refresh(); });
        connect(value, &QDoubleSpinBox::editingFinished, this, [this, id] { m_binding->finish(id); });
        m_impl->sync.push_back([this, id, control, value] {
            const double next = m_binding->value(id);
            const QSignalBlocker sliderBlock(control), valueBlock(value);
            if (!control->isSliderDown()) control->setPlainValue(next);
            if (!value->hasFocus()) value->setValue(next);
        });
        return group;
    };

    if (kind == Kind::Gain) {
        root->addWidget(slider("gain", tr("Gain"), " dB", true), 1);
        auto* footer = new QHBoxLayout;
        footer->setSpacing(6);
        footer->addWidget(toggle("mute", tr("Mute"), tr("Mute output")), 1);
        footer->addWidget(reset());
        root->addLayout(footer);
    } else if (kind == Kind::Stereo) {
        root->addWidget(segments("mode", {tr("Stereo"), tr("Mono / Mid"), tr("Side"), "L", "R"},
            {tr("Stereo"), tr("Mono / Mid"), tr("Side"), tr("Left channel to both outputs"), tr("Right channel to both outputs")}));
        m_impl->diagram = new UtilityDiagram(kind, this);
        m_impl->diagram->setAccessibleName(tr("Channel routing"));
        m_impl->diagram->setToolTip(tr("Channel routing. Dashed lines indicate inverted polarity."));
        root->addWidget(m_impl->diagram, 1);
        m_impl->width = slider("width", tr("Width"), " %");
        root->addWidget(m_impl->width);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(4);
        buttons->addWidget(toggle("swap", tr("Swap L/R"), tr("Swap left and right before processing")));
        buttons->addWidget(toggle("invertLeft", QString::fromUtf8("Ø L"), tr("Invert left polarity")));
        buttons->addWidget(toggle("invertRight", QString::fromUtf8("Ø R"), tr("Invert right polarity")));
        buttons->addWidget(reset());
        root->addLayout(buttons);
        if (!embedded) { m_impl->hint = label({}); m_impl->hint->setWordWrap(true); root->addWidget(m_impl->hint); }
    } else {
        auto* header = new QHBoxLayout;
        header->setSpacing(8);
        auto* source = segments("timeMode", {tr("Sync"), "ms"}, {tr("Host sync"), tr("Milliseconds")});
        source->setMaximumWidth(embedded ? 116 : 160);
        header->addWidget(source);
        header->addStretch();
        m_impl->duration = label({});
        m_impl->duration->setObjectName("UtilityReverseDuration");
        m_impl->duration->setFont(controlFont(this, embedded ? 13 : 22, QFont::DemiBold));
        m_impl->duration->setProperty("utilityDuration", !embedded);
        m_impl->duration->setToolTip(tr("Actual slice length, including tempo and the 50–4000 ms limit"));
        header->addWidget(m_impl->duration);
        header->addWidget(reset());
        root->addLayout(header);
        m_impl->time = new QStackedWidget(this);
        m_impl->time->addWidget(segments("division", {"1/16", "1/8", "1/4", "1/2", "1/1"}));
        m_impl->time->addWidget(slider("timeMs", tr("Length"), tr(" ms")));
        root->addWidget(m_impl->time);
        m_impl->diagram = new UtilityDiagram(kind, this);
        m_impl->diagram->setAccessibleName(tr("Record a slice, then play it backwards"));
        m_impl->diagram->setToolTip(tr("Reverse plays buffered slices. The dry signal passes immediately."));
        root->addWidget(m_impl->diagram, 1);
        root->addWidget(slider("mix", tr("Dry / Wet"), " %"));
        if (!embedded) {
            m_impl->hint = label(tr("Record a slice, then play it backwards"));
            m_impl->hint->setWordWrap(true);
            root->addWidget(m_impl->hint);
        }
    }
    const auto applyTheme = [this, kind] {
        const auto ink = accent(kind).name();
        setStyleSheet(QStringLiteral(
            "UtilityPanel QLabel { color: %1; background: transparent; }"
            "UtilityPanel QLabel[utilityTitle=\"true\"] { font-size: 18px; font-weight: 600; }"
            "UtilityPanel QLabel[utilityDuration=\"true\"] { font-size: 22px; font-weight: 600; }"
            "UtilityPanel QDoubleSpinBox { color: %2; background: %3; border: 1px solid %4; border-radius: 5px; padding: 1px 5px; selection-background-color: %5; }"
            "UtilityPanel QDoubleSpinBox[utilityReadout=\"gain\"] { font-size: 25px; font-weight: 600; }"
            "UtilityPanel QDoubleSpinBox[utilityReadout=\"normal\"] { font-size: 16px; font-weight: 600; }"
            "UtilityPanel QDoubleSpinBox:disabled { color: %6; }")
            .arg(th().textPrimary.name(), ink, th().well().name(), th().separator().name(), th().selection.name(), th().textSecondary.name()));
        for (auto* child : findChildren<QWidget*>()) child->update();
        update();
    };
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, applyTheme);
    applyTheme();
    if (m_timer) {
        m_timer->setInterval(50);
        connect(m_timer, &QTimer::timeout, this, &UtilityPanel::refresh);
    }
    refresh();
}
UtilityPanel::~UtilityPanel() = default;
QSize UtilityPanel::sizeHint() const {
    if (m_embedded) return {m_uid == "daw.gain" ? 164 : 340, 128};
    return m_uid == "daw.gain" ? QSize(208, 408) : QSize(460, 350);
}
QSize UtilityPanel::minimumSizeHint() const {
    if (m_embedded) return {0, 0};
    return m_uid == "daw.gain" ? QSize(184, 300) : QSize(360, 290);
}
void UtilityPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto r = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
    const auto base = mixColors(panelColor(), accent(kindFor(m_uid)), .045);
    QLinearGradient face(r.topLeft(), r.bottomRight());
    face.setColorAt(0, th().edgeLight(base));
    face.setColorAt(.35, base);
    face.setColorAt(1, th().edgeDark(base));
    p.setBrush(face);
    p.setPen(QPen(th().separator(), 1));
    p.drawRoundedRect(r, m_embedded ? 7 : 12, m_embedded ? 7 : 12);
}
void UtilityPanel::refresh() {
    setEnabled(m_binding->available());
    for (const auto& sync : m_impl->sync) sync();
    if (m_impl->width) {
        const int mode = int(m_binding->value("mode"));
        m_impl->width->setEnabled(mode == 0);
        const std::array<double, 5> state{double(mode), m_binding->value("width"), m_binding->value("swap"),
            m_binding->value("invertLeft"), m_binding->value("invertRight")};
        m_impl->diagram->setState(state);
        if (m_impl->hint) {
            const QStringList hints{tr("Width changes the stereo spread. 100% keeps the original image."),
                tr("The shared center, sent equally to both outputs."), tr("The difference between channels. The shared center is removed."),
                tr("Left channel, sent equally to both outputs."), tr("Right channel, sent equally to both outputs.")};
            m_impl->hint->setText(hints[std::clamp(mode, 0, 4)]);
        }
    }
    if (m_impl->time) {
        const int mode = m_binding->value("timeMode") >= .5 ? 1 : 0;
        m_impl->time->setCurrentIndex(mode);
        for (int i = 0; i < 2; ++i) m_impl->time->widget(i)->setEnabled(i == mode);
        m_impl->time->setFixedHeight(mode && !m_embedded ? 57 : 28);
        const double ms = daw::plugins::utility::reverseMilliseconds(m_binding->value("timeMode"),
            m_binding->value("timeMs"), m_binding->value("division"), m_binding->controller()->tempo());
        m_impl->duration->setText(tr("%1 ms").arg(ms, 0, 'f', 1));
    }
}
void UtilityPanel::showEvent(QShowEvent* event) { QWidget::showEvent(event); refresh(); if (m_timer) m_timer->start(); }
void UtilityPanel::hideEvent(QHideEvent* event) {
    if (m_timer) m_timer->stop();
    for (auto* slider : findChildren<QSlider*>()) slider->setSliderDown(false);
    m_binding->finishAll();
    QWidget::hideEvent(event);
}
