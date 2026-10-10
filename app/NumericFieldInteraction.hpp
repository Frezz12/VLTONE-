#pragma once

#include "LockedCursorDrag.hpp"
#include <QLineEdit>
#include <algorithm>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QSpinBox>
#include <QWheelEvent>
#include <cmath>

namespace ui {
namespace numeric_fields {

// Keep Qt's numeric validation, units, accessibility and keyboard editing.
// Only pointer input changes: drag the readout; double-click to edit text.
class Interaction final : public QObject {
public:
    explicit Interaction(QAbstractSpinBox* box) : QObject(box), m_box(box) {
        setObjectName(QStringLiteral("vlt.numericFieldInteraction"));
        m_edit = box->findChild<QLineEdit*>();
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        box->setMouseTracking(true);
        box->installEventFilter(this);
        if (m_edit) {
            m_edit->setMouseTracking(true);
            m_edit->installEventFilter(this);
        }
        cursor(Qt::SizeVerCursor);
        connect(box, &QAbstractSpinBox::editingFinished, this, [this] {
            m_textEditing = false;
            cursor(Qt::SizeVerCursor);
        });
        connect(box, &QObject::destroyed, this, [this] {
            if (m_drag.active()) m_drag.finish(QCursor::pos());
        });
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
            event->type() == QEvent::UngrabMouse ||
            (event->type() == QEvent::EnabledChange && !m_box->isEnabled())) {
            const QPointer<QObject> receiver(object);
            finish();
            return !receiver;
        }
        if (!m_box->isEnabled() || m_box->isReadOnly()) {
            finish();
            return false;
        }
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            const bool numeric = !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier)) &&
                !key->text().isEmpty() && (key->text().front().isDigit() ||
                QStringLiteral("-+.,").contains(key->text().front()));
            const bool paste = key->matches(QKeySequence::Paste);
            const bool escape = key->key() == Qt::Key_Escape && (m_pressed || m_textEditing);
            if (event->type() == QEvent::ShortcutOverride) {
                if (enter || escape || numeric || key->key() == Qt::Key_F2 ||
                    key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
                    event->accept();
                    return true;
                }
            } else {
                if (escape) {
                    const QPointer<Interaction> alive(this);
                    setValue(m_startValue);
                    if (alive) {
                        m_textEditing = false;
                        if (m_pressed) finish();
                        else { cursor(Qt::SizeVerCursor); emit m_box->editingFinished(); }
                    }
                    event->accept();
                    return true;
                }
                if ((enter || key->key() == Qt::Key_F2) && !m_textEditing) {
                    beginText(); event->accept(); return true;
                }
                if ((numeric || paste) && !m_textEditing) beginText();
                // Native spin boxes retain arrow keys, paste, locale-aware
                // parsing, selection and Enter/focus-out commit semantics.
            }
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                beginText(); event->accept(); return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress && !m_textEditing) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            const QPointer<Interaction> alive(this);
            m_box->interpretText();
            if (!alive) return true;
            m_startValue = m_value = value();
            m_pending = 0;
            m_pressed = true;
            m_dragging = false;
            m_drag.begin(mouse->globalPosition());
            m_box->grabMouse();
            cursor(Qt::ClosedHandCursor);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::MouseMove && m_pressed) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!(mouse->buttons() & Qt::LeftButton)) finish();
            else scrub(mouse);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && m_pressed &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            // The last move already applied the delta. A release may contain
            // an old pre-warp position, so don't count it as additional input.
            finish(); event->accept(); return true;
        }
        return QObject::eventFilter(object, event);
    }

private:
    double value() const {
        if (auto* box = qobject_cast<QDoubleSpinBox*>(m_box)) return box->value();
        return static_cast<QSpinBox*>(m_box)->value();
    }
    void setValue(double value) {
        if (auto* box = qobject_cast<QDoubleSpinBox*>(m_box)) box->setValue(value);
        else static_cast<QSpinBox*>(m_box)->setValue(int(std::round(value)));
    }
    void cursor(Qt::CursorShape shape) {
        m_box->setCursor(shape);
        if (m_edit) m_edit->setCursor(shape);
    }
    void beginText() {
        const QPointer<Interaction> alive(this);
        finish();
        if (!alive) return;
        m_startValue = value();
        m_textEditing = true;
        cursor(Qt::IBeamCursor);
        m_box->setFocus(Qt::OtherFocusReason);
        m_box->selectAll();
    }
    void finish() {
        if (!m_pressed) return;
        const bool dragged = m_dragging;
        m_pressed = m_dragging = false;
        if (dragged) m_drag.finish(QCursor::pos());
        else m_drag.cancel();
        if (QWidget::mouseGrabber() == m_box) m_box->releaseMouse();
        cursor(m_textEditing ? Qt::IBeamCursor : Qt::SizeVerCursor);
        // Some editors apply values only on editingFinished; others use it
        // to close a live undo transaction. Both need one commit on release.
        if (dragged) emit m_box->editingFinished();
    }
    void scrub(QMouseEvent* mouse) {
        m_pending -= m_drag.takeDelta(mouse->globalPosition()).y();
        if (!m_dragging && std::abs(m_pending) < 3) return;
        if (!m_dragging) {
            m_dragging = true;
            cursor(Qt::BlankCursor);
        }
        double step, minimum, maximum, quantum;
        if (auto* box = qobject_cast<QDoubleSpinBox*>(m_box)) {
            step = box->singleStep(); minimum = box->minimum(); maximum = box->maximum();
            quantum = std::pow(10.0, -box->decimals());
        } else {
            auto* integer = static_cast<QSpinBox*>(m_box);
            step = integer->singleStep(); minimum = integer->minimum(); maximum = integer->maximum();
            quantum = 1;
        }
        const double sensitivity = mouse->modifiers().testFlag(Qt::ShiftModifier) ? .01 : .1;
        m_value = std::clamp(m_value + m_pending * step * sensitivity, minimum, maximum);
        m_pending = 0;
        const QPointer<Interaction> alive(this);
        setValue(m_value);
        // A linked editor may further constrain this value synchronously
        // (for example Start against End). Follow that accepted value while
        // retaining fractional motion when the only difference is rounding.
        if (alive && std::abs(value() - m_value) > quantum * .51) m_value = value();
    }
    QAbstractSpinBox* m_box;
    QPointer<QLineEdit> m_edit;
    LockedCursorDrag m_drag;
    double m_startValue = 0, m_value = 0;
    qreal m_pending = 0;
    bool m_pressed = false, m_dragging = false, m_textEditing = false;
};

class Installer final : public QObject {
public:
    explicit Installer(QApplication* app) : QObject(app) {
        setObjectName(QStringLiteral("vlt.numericFields"));
        app->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        // Avoid inspecting every paint/timer event in the application.
        switch (event->type()) {
        case QEvent::Polish: case QEvent::Show: case QEvent::Wheel:
        case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick:
        case QEvent::ShortcutOverride: case QEvent::KeyPress: break;
        default: return false;
        }
        auto* box = qobject_cast<QAbstractSpinBox*>(object);
        if (!box && qobject_cast<QLineEdit*>(object))
            box = qobject_cast<QAbstractSpinBox*>(object->parent());
        if (!box || (!qobject_cast<QSpinBox*>(box) && !qobject_cast<QDoubleSpinBox*>(box)))
            return false;
        if (event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            // Deliver scrolling to the surrounding viewport, including when
            // a focused field's embedded line editor received the wheel.
            for (auto* parent = box->parentWidget(); parent; parent = parent->parentWidget()) {
                if (auto* area = qobject_cast<QAbstractScrollArea*>(parent)) {
                    QWheelEvent forwarded(area->viewport()->mapFromGlobal(wheel->globalPosition()),
                        wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
                        wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source());
                    QApplication::sendEvent(area->viewport(), &forwarded);
                    break;
                }
            }
            event->accept();
            return true;
        }
        // Inspector and Creator already implement their own domain-specific
        // scrubs. Wheel suppression still applies to them, even in text mode.
        if (!box->property("vlt.customNumericScrub").toBool() && !box->isReadOnly() &&
            !box->findChild<QObject*>(QStringLiteral("vlt.numericFieldInteraction"), Qt::FindDirectChildrenOnly))
            new Interaction(box);
        return false;
    }
};
} // namespace numeric_fields

inline void installNumericFieldInteraction(QApplication& app) {
    if (!app.findChild<QObject*>(QStringLiteral("vlt.numericFields"), Qt::FindDirectChildrenOnly))
        new numeric_fields::Installer(&app);
}
} // namespace ui
