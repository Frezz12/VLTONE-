#include "Cla2aPanel.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QHideEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace comp = daw::plugins::cla2a;
namespace {
const QColor kInk(0x25, 0x28, 0x29), kRed(0x9C, 0x24, 0x31);
QFont faceFont(const QWidget* widget, double points, bool bold = false) {
    QFont font = widget->font(); font.setPointSizeF(std::max(points, widget->font().pointSizeF()));
    font.setWeight(bold ? QFont::DemiBold : QFont::Normal); return font;
}
QPointF polar(QPointF center, double radius, double degrees) {
    const double angle = degrees * std::numbers::pi / 180;
    return center + QPointF(radius * std::cos(angle), -radius * std::sin(angle));
}
}

/// Keep the DAW's keyboard and automation conventions, with a physical face
/// and a local incremental drag. Rebasing each move means changing Shift or
/// reversing at a limit never requires traversing an invisible overshoot.
class Cla2aKnob final : public ui::Knob {
    Q_DECLARE_TR_FUNCTIONS(Cla2aKnob)
public:
    Cla2aKnob(QString name, double defaultValue, QWidget* parent) : ui::Knob({}, parent), m_default(defaultValue), m_wheel(this) {
        setRange(0, 100); setDefaultValue(defaultValue); setBare(148); setAutomatable(true);
        setFocusPolicy(Qt::StrongFocus); setAccessibleName(name);
        setAccessibleDescription(tr("Drag vertically. Hold Shift for fine adjustment. Press Enter to enter a value."));
        m_wheel.setSingleShot(true); m_wheel.setInterval(180);
        connect(&m_wheel, &QTimer::timeout, this, [this] { emit editFinished(); });
        connect(this, &ui::Knob::valueChanged, this, [this](double value) { announce(value); });
    }
    bool isEditing() const { return m_dragging || m_wheel.isActive(); }
    void finishEditing() {
        const bool editing = isEditing(); m_dragging = false; m_wheel.stop();
        if (QWidget::mouseGrabber() == this) releaseMouse();
        if (editing) emit editFinished();
    }
    void editValue(double next, bool finished = true) {
        if (!isEnabled() || !std::isfinite(next)) return;
        next = std::clamp(next, 0., 100.);
        if (value() != next) { setValue(next); emit valueChanged(next); }
        if (finished) emit editFinished();
    }
    void refreshValue(double next) {
        if (isEditing() || next == value()) return;
        setValue(next); announce(next);
    }
    void enterValue() {
        finishEditing();
        QInputDialog dialog(this); dialog.setObjectName("Cla2aValueEditor");
        dialog.setWindowTitle(accessibleName()); dialog.setLabelText(tr("Value (0–100)"));
        dialog.setInputMode(QInputDialog::DoubleInput); dialog.setDoubleRange(0, 100);
        dialog.setDoubleDecimals(2); dialog.setDoubleStep(.1); dialog.setDoubleValue(value());
        if (dialog.exec() == QDialog::Accepted) editValue(dialog.doubleValue());
    }
protected:
    void paintEvent(QPaintEvent*) override {
        const auto ratio = devicePixelRatioF();
        if (m_face.isNull() || m_face.size() != this->size() * ratio || m_face.devicePixelRatio() != ratio || m_faceFont != font()) {
            m_face = QPixmap(this->size() * ratio); m_face.setDevicePixelRatio(ratio); m_face.fill(Qt::transparent); m_faceFont = font();
            QPainter p(&m_face); p.setRenderHint(QPainter::Antialiasing);
            const double size = std::min(width(), height()); const QPointF center(width() / 2., height() / 2.);
            const double r = size * .295;
            p.setFont(faceFont(this, 10));
            for (int i = 0; i <= 20; ++i) {
                const double angle = 225 - i * 13.5;
                p.setPen(QPen(kInk, i % 2 ? .8 : 1.2));
                p.drawLine(polar(center, r + 7, angle), polar(center, r + (i % 2 ? 11 : 14), angle));
                if (i % 2 == 0) {
                    const QPointF at = polar(center, r + 19, angle);
                    p.drawText(QRectF(at.x() - 15, at.y() - 10, 30, 20), Qt::AlignCenter, QString::number(i * 5));
                }
            }
            const QRectF body(center.x() - r, center.y() - r, r * 2, r * 2);
            p.setPen(Qt::NoPen);
            for (int i = 5; i >= 0; --i) { p.setBrush(QColor(0, 0, 0, 12)); p.drawEllipse(body.adjusted(-i, -i, i, i).translated(0, 4)); }
            QLinearGradient collar(body.topLeft(), body.bottomRight()); collar.setColorAt(0, QColor("#696c6b")); collar.setColorAt(.5, QColor("#212423")); collar.setColorAt(1, QColor("#080b0a"));
            p.setBrush(collar); p.setPen(QPen(QColor("#151817"), 1.4)); p.drawEllipse(body);
            const QRectF core = body.adjusted(5, 5, -5, -5);
            QRadialGradient face(core.topLeft() + QPointF(r * .45, r * .38), r * 1.8);
            face.setColorAt(0, QColor("#535755")); face.setColorAt(.55, QColor("#2c302e")); face.setColorAt(1, QColor("#101412"));
            p.setBrush(face); p.setPen(QPen(QColor("#0c100e"), 1)); p.drawEllipse(core);
            p.setPen(QPen(QColor(255, 255, 255, 80), 1)); p.setBrush(Qt::NoBrush); p.drawArc(core.adjusted(1, 1, -1, -1), 25 * 16, 130 * 16);
        }
        QPainter p(this); p.drawPixmap(0, 0, m_face); p.setRenderHint(QPainter::Antialiasing);
        const QPointF center(width() / 2., height() / 2.);
        const double r = std::min(width(), height()) * .295;
        const QRectF body(center.x() - r, center.y() - r, r * 2, r * 2);
        const double angle = 225 - value() * 2.7;
        p.save(); p.translate(center); p.rotate(-angle);
        QRectF grip(-r * .62, -r * .17, r * 1.4, r * .34);
        p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 125)); p.drawRoundedRect(grip.translated(-1, 3), 4, 4);
        QLinearGradient metal(grip.topLeft(), grip.bottomLeft()); metal.setColorAt(0, QColor("#d0d2cf")); metal.setColorAt(.22, QColor("#92968f")); metal.setColorAt(.68, QColor("#686d65")); metal.setColorAt(1, QColor("#3b403a"));
        p.setPen(QPen(QColor("#d4d6d1"), .8)); p.setBrush(metal); p.drawRoundedRect(grip, 3, 3);
        p.setPen(QPen(QColor("#f3f2e9"), 2, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(r * .45, 0), QPointF(r * .82, 0)); p.restore();
        if (hasFocus()) { p.setPen(QPen(kRed, 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(body.adjusted(-4, -4, 4, 4)); }
    }
    void enterEvent(QEnterEvent*) override {}
    void leaveEvent(QEvent*) override {}
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !isEnabled()) return;
        finishEditing(); setFocus(Qt::MouseFocusReason); m_dragging = true;
        m_start = value(); m_lastY = e->globalPosition().y(); update(); e->accept();
    }
    void drag(QMouseEvent* e) {
        if (!m_dragging) return;
        const double travel = e->modifiers().testFlag(Qt::ShiftModifier) ? 600 : 150;
        const double delta = (m_lastY - e->globalPosition().y()) * 100 / travel;
        m_lastY = e->globalPosition().y(); editValue(value() + delta, false);
    }
    void mouseMoveEvent(QMouseEvent* e) override { drag(e); e->accept(); }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        drag(e); finishEditing(); update(); e->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        finishEditing();
        if (ui::automationCreationMode()) emit automateRequested();
        else editValue(m_default);
    }
    void wheelEvent(QWheelEvent* e) override {
        const double delta = e->angleDelta().y() ? e->angleDelta().y() / 120. : e->pixelDelta().y() / 30.;
        if (delta == 0) { e->ignore(); return; }
        editValue(value() + delta * (e->modifiers().testFlag(Qt::ShiftModifier) ? .5 : 2), false);
        m_wheel.start(); e->accept();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { enterValue(); e->accept(); return; }
        if (e->key() == Qt::Key_Escape && m_dragging) { editValue(m_start, false); finishEditing(); e->accept(); return; }
        ui::Knob::keyPressEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        finishEditing(); QMenu menu(this);
        auto* numeric = menu.addAction(tr("Enter value…")); numeric->setObjectName("Cla2aEnterValue");
        auto* automate = menu.addAction(tr("Create automation")); automate->setObjectName("Cla2aAutomate");
        auto* reset = menu.addAction(tr("Reset to default"));
        const auto selected = menu.exec(e->globalPos());
        if (selected == numeric) enterValue();
        else if (selected == automate) emit automateRequested();
        else if (selected == reset) editValue(m_default);
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::UngrabMouse || e->type() == QEvent::Hide || e->type() == QEvent::WindowDeactivate || e->type() == QEvent::FocusOut)
            finishEditing();
        return ui::Knob::event(e);
    }
private:
    void announce(double next) { QAccessibleValueChangeEvent event(this, next); QAccessible::updateAccessibility(&event); }
    double m_default, m_start = 0, m_lastY = 0;
    bool m_dragging = false;
    QTimer m_wheel;
    QPixmap m_face;
    QFont m_faceFont;
};

namespace {
class AccessibleKnob final : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit AccessibleKnob(Cla2aKnob* knob) : QAccessibleWidget(knob, QAccessible::Slider) {}
    void* interface_cast(QAccessible::InterfaceType type) override {
        return type == QAccessible::ValueInterface ? static_cast<QAccessibleValueInterface*>(this) : QAccessibleWidget::interface_cast(type);
    }
    QVariant currentValue() const override { return static_cast<Cla2aKnob*>(object())->value(); }
    void setCurrentValue(const QVariant& value) override {
        bool ok = false; const double next = value.toDouble(&ok);
        if (ok) static_cast<Cla2aKnob*>(object())->editValue(next);
    }
    QVariant maximumValue() const override { return 100.; }
    QVariant minimumValue() const override { return 0.; }
    QVariant minimumStepSize() const override { return .1; }
};
QAccessibleInterface* accessibleFactory(const QString&, QObject* object) {
    if (auto* knob = dynamic_cast<Cla2aKnob*>(object)) return new AccessibleKnob(knob);
    return nullptr;
}
class ModeButton final : public QAbstractButton {
    Q_DECLARE_TR_FUNCTIONS(Cla2aModeButton)
public:
    explicit ModeButton(QWidget* parent) : QAbstractButton(parent) {
        setCheckable(true); setText(tr("Compress / Limit")); setAccessibleName(tr("Compression mode: Compress / Limit"));
        setFocusPolicy(Qt::StrongFocus); setCursor(Qt::PointingHandCursor); setFixedSize(124, 50);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QRectF slot(4, 4, 25, 42); QLinearGradient well(slot.topLeft(), slot.bottomRight());
        well.setColorAt(0, QColor("#353c37")); well.setColorAt(.5, QColor("#7f8580")); well.setColorAt(1, QColor("#d1d4ce"));
        p.setPen(QPen(QColor("#858c84"), 1)); p.setBrush(well); p.drawRoundedRect(slot, 12, 12);
        const QRectF toggle(6.5, isChecked() ? 6.5 : 23.5, 20, 20);
        QRadialGradient cap(toggle.topLeft() + QPointF(6, 4), 22);
        cap.setColorAt(0, QColor("#f4f3e9")); cap.setColorAt(.65, QColor("#c9cec4")); cap.setColorAt(1, QColor("#91998d"));
        p.setBrush(cap); p.setPen(QPen(QColor("#353c36"), 1.2)); p.drawEllipse(toggle);
        p.setPen(kInk); p.setFont(faceFont(this, 10, isChecked())); p.drawText(QRect(39, 4, width() - 39, 22), Qt::AlignVCenter, "LIMIT");
        p.setFont(faceFont(this, 10, !isChecked())); p.drawText(QRect(39, 25, width() - 39, 22), Qt::AlignVCenter, "COMPRESS");
        if (hasFocus()) { p.setBrush(Qt::NoBrush); p.setPen(QPen(kRed, 1.5)); p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4); }
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu menu(this); auto* action = menu.addAction(tr("Create automation"));
        if (menu.exec(e->globalPos()) == action && automate) automate();
    }
public:
    std::function<void()> automate;
};
}

Cla2aPanel::Cla2aPanel(daw::EngineController* controller, QString channel, QString insert, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channel(channel.toStdString()), m_insert(insert.toStdString()) {
    setObjectName("Cla2aPanel"); setAccessibleName(tr("VLT 2A optical compressor"));
    setMinimumSize(688, 286); resize(820, 310); setAttribute(Qt::WA_OpaquePaintEvent);
    static const bool registered = [] { QAccessible::installFactory(accessibleFactory); return true; }(); (void)registered;
    for (unsigned i = 0; i < m_knobs.size(); ++i) {
        const auto& info = comp::parameterTable()[i];
        auto* knob = new Cla2aKnob(QString::fromStdString(info.name), info.defaultValue, this);
        knob->setObjectName(i == 0 ? "Cla2aGain" : "Cla2aPeakReduction"); knob->setValue(read(i)); m_knobs[i] = knob;
        connect(knob, &ui::Knob::valueChanged, this, [this, i](double value) { write(i, value); });
        connect(knob, &ui::Knob::editFinished, this, [this, i] { finish(i); });
        connect(knob, &ui::Knob::automateRequested, this, [this, i] { finishAll(); emit automationRequested(QString::fromStdString(comp::parameterTable()[i].id)); });
        auto* value = new QPushButton(this); value->setObjectName(i == 0 ? "Cla2aGainValue" : "Cla2aPeakReductionValue");
        value->setAccessibleName(tr("Enter %1 value").arg(QString::fromStdString(info.name)));
        value->setCursor(Qt::PointingHandCursor); value->setFlat(true);
        value->setStyleSheet("QPushButton { color: #333b34; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0; } QPushButton:hover { background: rgba(255,255,255,55); border-color: #8b9388; } QPushButton:focus { border: 2px solid #9c2431; }");
        value->setFont(faceFont(this, 10)); connect(value, &QPushButton::clicked, knob, &Cla2aKnob::enterValue); m_values[i] = value;
    }
    auto* mode = new ModeButton(this); mode->setObjectName("Cla2aMode"); m_mode = mode;
    mode->automate = [this] { finishAll(); emit automationRequested(QStringLiteral("mode")); };
    connect(mode, &QAbstractButton::clicked, this, [this](bool limit) { write(2, limit ? 1 : 0); finish(2); });
    QWidget::setTabOrder(m_knobs[0], m_values[0]); QWidget::setTabOrder(m_values[0], m_knobs[1]);
    QWidget::setTabOrder(m_knobs[1], m_values[1]); QWidget::setTabOrder(m_values[1], m_mode);
    m_timer = new QTimer(this); m_timer->setObjectName("Cla2aTelemetryTimer"); m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &Cla2aPanel::refresh);
    refresh();
}
Cla2aPanel::~Cla2aPanel() { m_timer->stop(); finishAll(); }
comp::Cla2aInstance* Cla2aPanel::instance() const {
    return m_controller ? dynamic_cast<comp::Cla2aInstance*>(m_controller->insertInstance(m_channel, m_insert)) : nullptr;
}
double Cla2aPanel::read(unsigned index) const {
    const auto& p = comp::parameterTable()[index];
    return instance() ? m_controller->insertParameter(m_channel, m_insert, p.id) : p.defaultValue;
}
void Cla2aPanel::write(unsigned index, double value) {
    if (index >= comp::kParameterCount || !std::isfinite(value) || !instance()) return;
    if (!m_gestures[index]) m_gestures[index] = read(index);
    const auto& p = comp::parameterTable()[index]; value = std::clamp(value, p.minValue, p.maxValue);
    if (p.isStepped) value = std::round(value);
    m_controller->setInsertParameter(m_channel, m_insert, p.id, value);
    if (index < 2) m_values[index]->setText(QString::number(read(index), 'f', 2));
}
void Cla2aPanel::finish(unsigned index) {
    if (!m_gestures[index]) return;
    if (instance() && read(index) != *m_gestures[index]) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert, comp::parameterTable()[index].id, *m_gestures[index], "Change VLT 2A");
        emit projectEdited();
    }
    m_gestures[index].reset();
}
void Cla2aPanel::finishAll() {
    for (auto* knob : m_knobs) if (knob) knob->finishEditing();
    for (unsigned i = 0; i < comp::kParameterCount; ++i) finish(i);
}
void Cla2aPanel::refresh() {
    auto* processor = instance();
    for (unsigned i = 0; i < 2; ++i) {
        m_knobs[i]->setEnabled(processor != nullptr); m_values[i]->setEnabled(processor != nullptr);
        m_knobs[i]->refreshValue(read(i)); m_values[i]->setText(QString::number(m_knobs[i]->value(), 'f', 2));
    }
    m_mode->setEnabled(processor != nullptr); m_mode->setChecked(read(2) > .5);
    if (!isVisible()) return;
    const auto telemetry = processor ? processor->consumeTelemetry() : comp::Telemetry{};
    const auto db = [](float amplitude) { return std::clamp(20 * std::log10(std::max(double(amplitude), 1e-12)), -60., 24.); };
    m_meters[0] = std::max(db(telemetry.input), m_meters[0] - 1.4);
    m_meters[1] = std::max(db(telemetry.output), m_meters[1] - 1.4);
    m_meters[2] = std::max(double(telemetry.reduction), m_meters[2] - .5);
    setAccessibleDescription(tr("Input %1 dBFS, gain reduction %2 dB, output %3 dBFS")
        .arg(m_meters[0], 0, 'f', 1).arg(m_meters[2], 0, 'f', 1).arg(m_meters[1], 0, 'f', 1));
    update(meterRect().adjusted(-4, -4, 4, 43).toAlignedRect());
}
QRectF Cla2aPanel::meterRect() const {
    const double w = std::clamp(width() * .24, 200., 260.);
    return QRectF((width() - w) / 2, height() * .25, w, std::clamp(height() * .33, 100., 138.));
}
void Cla2aPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e); m_face = {};
    const int diameter = std::clamp(int(width() * .18), 140, 164);
    for (unsigned i = 0; i < 2; ++i) {
        m_knobs[i]->setFixedSize(diameter, diameter);
        const int x = int(width() * (i ? .77 : .23)) - diameter / 2;
        const int y = int(height() * .53) - diameter / 2;
        m_knobs[i]->move(x, y); m_values[i]->setGeometry(x + (diameter - 80) / 2, y + diameter + 24, 80, 25);
    }
    m_mode->move((width() - m_mode->width()) / 2, height() - m_mode->height() - 14);
}
void Cla2aPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); refresh(); m_timer->start(); }
void Cla2aPanel::hideEvent(QHideEvent* e) { m_timer->stop(); finishAll(); QWidget::hideEvent(e); }
void Cla2aPanel::changeEvent(QEvent* e) { if (e->type() == QEvent::FontChange || e->type() == QEvent::PaletteChange) m_face = {}; QWidget::changeEvent(e); }
void Cla2aPanel::cacheFace() {
    const double ratio = devicePixelRatioF();
    if (!m_face.isNull() && m_face.size() == size() * ratio && m_face.devicePixelRatio() == ratio) return;
    m_face = QPixmap(size() * ratio); m_face.setDevicePixelRatio(ratio); m_face.fill(QColor("#25292a"));
    QPainter p(&m_face); p.setRenderHint(QPainter::Antialiasing);
    const QRectF plate = QRectF(rect()).adjusted(6, 6, -6, -6);
    QLinearGradient silver(plate.topLeft(), plate.bottomLeft());
    silver.setColorAt(0, QColor("#d9d9d7")); silver.setColorAt(.18, QColor("#cececb")); silver.setColorAt(.65, QColor("#c8c9c6")); silver.setColorAt(1, QColor("#bfc1bd"));
    p.setPen(QPen(QColor("#eeeeE6"), 1.6)); p.setBrush(silver); p.drawRoundedRect(plate, 10, 10);
    p.save(); QPainterPath clip; clip.addRoundedRect(plate.adjusted(2, 2, -2, -2), 9, 9); p.setClipPath(clip);
    // Deterministic, barely visible brushing; cached rather than regenerated
    // in every telemetry frame, and rendered at the display's native DPR.
    for (int y = 9; y < height() - 8; y += 3) {
        p.setPen(QColor(255, 255, 255, y % 9 == 0 ? 8 : 3)); p.drawLine(9, y, width() - 9, y);
    }
    p.restore();
    p.setPen(kRed); p.setFont(faceFont(this, 16, true)); p.drawText(QRect(26, 16, width() - 52, 28), Qt::AlignVCenter, "VLT 2A");
    for (unsigned i = 0; i < 2; ++i) {
        const auto geometry = m_knobs[i]->geometry(); p.setFont(faceFont(this, 11, true)); p.setPen(kInk);
        p.drawText(QRect(geometry.x() - 14, geometry.bottom() + 4, geometry.width() + 28, 19), Qt::AlignCenter, i ? tr("PEAK REDUCTION") : tr("GAIN"));
    }
    const auto meter = meterRect();
    p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 36)); p.drawRoundedRect(meter.adjusted(-1, -1, 1, 1).translated(0, 3), 15, 15);
    QLinearGradient bezel(meter.topLeft(), meter.bottomRight()); bezel.setColorAt(0, QColor("#fff1d1")); bezel.setColorAt(.45, QColor("#b8a78a")); bezel.setColorAt(1, QColor("#f6ebce"));
    p.setPen(QPen(QColor("#939387"), 1)); p.setBrush(bezel); p.drawRoundedRect(meter, 14, 14);
    const auto glass = meter.adjusted(5, 5, -5, -5); QRadialGradient warm(glass.center(), glass.width() * .66);
    warm.setColorAt(0, QColor("#fae4b5")); warm.setColorAt(.65, QColor("#e8c993")); warm.setColorAt(1, QColor("#c8a374"));
    p.setPen(QPen(QColor("#9e8462"), .8)); p.setBrush(warm); p.drawRoundedRect(glass, 10, 10);
    p.setPen(kInk); p.setFont(faceFont(this, 10));
    p.drawText(QRectF(meter.x() + 12, meter.bottom() - 28, 30, 22), Qt::AlignCenter, tr("IN"));
    p.drawText(QRectF(meter.right() - 44, meter.bottom() - 28, 34, 22), Qt::AlignCenter, tr("OUT"));
    p.setFont(faceFont(this, 9, true)); p.drawText(QRectF(meter.x() + 45, meter.bottom() - 28, meter.width() - 90, 22), Qt::AlignCenter, "GR");
}
void Cla2aPanel::paintEvent(QPaintEvent*) {
    cacheFace(); QPainter p(this); p.drawPixmap(0, 0, m_face); p.setRenderHint(QPainter::Antialiasing);
    const auto meter = meterRect(); const double top = meter.y() + 15, bottom = meter.bottom() - 35;
    const double step = (bottom - top) / 24;
    for (unsigned column = 0; column < 3; ++column) {
        const unsigned source = column == 1 ? 2 : column == 2 ? 1 : 0;
        const double level = source == 2 ? m_meters[source] / 40 : (m_meters[source] + 60) / 60;
        const int lit = int(std::round(std::clamp(level, 0., 1.) * 24));
        const double x = column == 0 ? meter.x() + 17 : column == 2 ? meter.right() - 34 : meter.x() + 53;
        const double width = column == 1 ? meter.width() - 106 : 17;
        for (int row = 0; row < 24; ++row) {
            const bool active = row < lit;
            const bool clip = source != 2 && m_meters[source] >= 0 && row > 21;
            p.setPen(Qt::NoPen); p.setBrush(active ? (clip ? kRed : QColor("#62441e")) : QColor(118, 87, 45, 39));
            p.drawRoundedRect(QRectF(x, bottom - (row + 1) * step, width, std::max(1.2, step - 1.2)), 1, 1);
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned source = i == 1 ? 2 : i == 2 ? 1 : 0;
        const QRectF value(meter.x() + i * meter.width() / 3, meter.bottom() + 7, meter.width() / 3, 20);
        p.setPen(kInk); p.setFont(faceFont(this, 10));
        p.drawText(value, Qt::AlignCenter, QString::number(m_meters[source], 'f', 1));
        p.setPen(QColor("#505452")); p.setFont(faceFont(this, 8.5));
        p.drawText(value.translated(0, 16), Qt::AlignCenter, source == 2 ? "dB" : "dBFS");
    }
}
