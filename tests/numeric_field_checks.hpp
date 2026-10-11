#pragma once
#include "NumericFieldInteraction.hpp"
#include <QScrollArea>
#include <QScrollBar>
#include <QDateTimeEdit>
#include <QDial>
#include <QSlider>
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
    QComboBox choice(contents);
    choice.addItems({"A", "B", "C"});
    choice.setCurrentIndex(1);
    QComboBox editable(contents);
    editable.addItems({"A", "B", "C"});
    editable.setEditable(true);
    editable.setCurrentIndex(1);
    QSlider slider(Qt::Horizontal, contents);
    slider.setRange(0, 100); slider.setValue(50);
    QDial dial(contents);
    dial.setRange(0, 100); dial.setValue(50);
    QDateTimeEdit date(QDateTime(QDate(2026, 10, 11), QTime(12, 0)), contents);
    const auto initialDate = date.dateTime();
    int edits = 0;
    QObject::connect(&choice, &QComboBox::currentIndexChanged, [&] { ++edits; });
    QObject::connect(&editable, &QComboBox::currentIndexChanged, [&] { ++edits; });
    QObject::connect(&slider, &QSlider::valueChanged, [&] { ++edits; });
    QObject::connect(&dial, &QDial::valueChanged, [&] { ++edits; });
    QObject::connect(&date, &QDateTimeEdit::dateTimeChanged, [&] { ++edits; });
    const QList<QWidget*> controls{&choice, &editable, editable.lineEdit(), &slider, &dial,
                                 &date, date.findChild<QLineEdit*>()};
    for (auto* control : controls) {
        control->show();
        for (const auto modifiers : {Qt::NoModifier, Qt::ShiftModifier, Qt::ControlModifier}) {
            for (const bool pixels : {false, true}) {
                for (const int direction : {-1, 1}) {
                    control->setFocus();
                    QWheelEvent event(QPointF(10, 10), control->mapToGlobal(QPoint(10, 10)),
                        pixels ? QPoint(0, direction * 30) : QPoint{},
                        pixels ? QPoint{} : QPoint(0, direction * 120),
                        Qt::NoButton, modifiers, Qt::ScrollUpdate, false);
                    QApplication::sendEvent(control, &event);
                }
            }
        }
        scroll.verticalScrollBar()->setValue(0);
        wheel(control);
        ok &= require(scroll.verticalScrollBar()->value() > 0,
                      "wheel over a choice or slider scrolls its surrounding panel");
    }
    ok &= require(edits == 0 && choice.currentIndex() == 1 && editable.currentIndex() == 1 &&
                  slider.value() == 50 && dial.value() == 50 && date.dateTime() == initialDate,
                  "focused choices, sliders, dials and date editors never change from wheel input");
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(&slider, &right);
    ok &= require(slider.value() > 50, "keyboard editing remains available");
    scroll.verticalScrollBar()->setValue(0);
    wheel(scroll.verticalScrollBar());
    ok &= require(scroll.verticalScrollBar()->value() > 0, "scrollbars retain wheel navigation");
    return ok;
}
