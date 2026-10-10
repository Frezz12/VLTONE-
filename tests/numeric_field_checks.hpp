#pragma once
#include "NumericFieldInteraction.hpp"
#include <QScrollArea>
#include <QScrollBar>
#include <cstdio>

inline bool numericFieldChecks(QApplication& app) {
    ui::installNumericFieldInteraction(app);
    QDoubleSpinBox field;
    field.setRange(-10, 10);
    field.setDecimals(2);
    field.setSingleStep(.5);
    field.setKeyboardTracking(false);
    field.setValue(2);
    field.resize(140, 30);
    field.show();
    QApplication::processEvents();
    auto* edit = field.findChild<QLineEdit*>();
    int commits = 0;
    QObject::connect(&field, &QAbstractSpinBox::editingFinished, [&] { ++commits; });
    const auto require = [](bool ok, const char* label) {
        std::fprintf(stderr, "%s numeric field: %s\n", ok ? "PASS" : "FAIL", label);
        return ok;
    };
    auto mouse = [&](QObject* target, QEvent::Type type, double y,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, QPointF(20, 12), QPointF(300, y),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(target, &event);
    };
    auto key = [&](int code, const QString& text = {}) {
        QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier, text);
        QApplication::sendEvent(&field, &event);
    };
    auto wheel = [](QWidget* target) {
        QWheelEvent event(QPointF(10, 10), target->mapToGlobal(QPoint(10, 10)), {},
            QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &event);
    };
    bool ok = true;
    wheel(&field); wheel(edit);
    ok &= require(field.value() == 2, "wheel never changes a focused number or its line editor");
    mouse(edit, QEvent::MouseButtonPress, 300);
    mouse(&field, QEvent::MouseMove, 280);
    ok &= require(field.value() == 3 && field.cursor().shape() == Qt::BlankCursor,
                  "up increases live and hides cursor");
    mouse(&field, QEvent::MouseMove, 290);
    ok &= require(field.value() == 2.5, "down reverses immediately");
    mouse(&field, QEvent::MouseButtonRelease, 290);
    ok &= require(commits == 1 && QCursor::pos() == QPoint(300, 300) &&
                  field.cursor().shape() == Qt::SizeVerCursor, "release commits once and restores cursor anchor");
    mouse(edit, QEvent::MouseButtonPress, 300);
    mouse(&field, QEvent::MouseMove, 290, Qt::ShiftModifier);
    ok &= require(field.value() == 2.55, "Shift gives fine adjustment");
    key(Qt::Key_Escape);
    ok &= require(field.value() == 2.5 && QWidget::mouseGrabber() != &field, "Escape restores value and releases pointer");
    field.setValue(9.5);
    mouse(edit, QEvent::MouseButtonPress, 300);
    mouse(&field, QEvent::MouseMove, 200);
    mouse(&field, QEvent::MouseMove, 210);
    ok &= require(field.value() == 9.5, "clamped limit has no reverse dead zone");
    mouse(&field, QEvent::MouseButtonRelease, 210);
    mouse(edit, QEvent::MouseButtonDblClick, 300);
    edit->setText(field.locale().toString(4.25, 'f', 2));
    key(Qt::Key_Return);
    ok &= require(field.value() == 4.25, "double click and Enter retain text editing");
    wheel(edit);
    ok &= require(field.value() == 4.25, "wheel remains disabled after text entry");
    field.setReadOnly(true);
    mouse(edit, QEvent::MouseButtonPress, 300);
    mouse(&field, QEvent::MouseMove, 200);
    mouse(&field, QEvent::MouseButtonRelease, 200);
    ok &= require(field.value() == 4.25, "read-only fields stay read-only");
    field.setReadOnly(false);
    mouse(edit, QEvent::MouseButtonPress, 300);
    mouse(&field, QEvent::MouseMove, 280);
    field.hide();
    ok &= require(QWidget::mouseGrabber() != &field && field.cursor().shape() != Qt::BlankCursor,
                  "hiding during a drag restores input state");
    QScrollArea scroll;
    auto* contents = new QWidget;
    contents->setMinimumSize(200, 1000);
    auto* integer = new QSpinBox(contents);
    integer->setRange(0, 100); integer->setValue(50);
    scroll.setWidget(contents); scroll.resize(220, 100); scroll.show();
    QApplication::processEvents();
    wheel(integer->findChild<QLineEdit*>());
    ok &= require(integer->value() == 50 && scroll.verticalScrollBar()->value() > 0,
                  "wheel scrolls surrounding panel without changing integer");
    mouse(integer->findChild<QLineEdit*>(), QEvent::MouseButtonPress, 300);
    mouse(integer, QEvent::MouseMove, 280);
    mouse(integer, QEvent::MouseButtonRelease, 280);
    ok &= require(integer->value() == 52, "integer fields use their native step");
    return ok;
}
