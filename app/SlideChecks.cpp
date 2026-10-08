#include "EngineController.hpp"
#include "PianoRollWindow.hpp"
#include "SlideCurveEditor.hpp"
#include "SlideNotes.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <cstdio>

bool PianoRollWindow::checkSlidesForTest(const QString &images) {
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    if (!controller.initialize(48000, 512, false).isOk())
        return false;
    auto track = controller.addTrack(daw::TrackKind::Midi, "Slide phrase");
    auto clip = controller.addMidiClip(track, 0, 8);
    daw::NoteModel base;
    base.id = daw::newUuid();
    base.pitch = 60;
    base.startBeats = 0;
    base.lengthBeats = 6;
    controller.setClipNotes(track, clip, {base}, "Fixture");
    PianoRollWindow window(&controller);
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1080, 620);
    window.setClip(QString::fromStdString(track), QString::fromStdString(clip));
    window.show();
    QApplication::processEvents();
    return window.m_view->checkSlidesForTest(images);
}
bool PianoRollView::checkSlidesForTest(const QString &images) {
    bool ok = true;
    auto check = [&](bool pass, const char *label) {
        std::printf("%s slide UI: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    m_pxPerBeat = 115;
    m_rowHeight = 19;
    m_scrollY = (127 - 77) * m_rowHeight;
    m_scrollX = 0;
    m_snapEnabled = true;
    m_adaptiveSnap = false;
    m_gridBeats = .25;
    selectAll();
    setFocus();
    QApplication::processEvents();
    auto mouse = [&](QWidget *target, QEvent::Type type, QPointF pos, Qt::MouseButton button,
                     Qt::MouseButtons buttons) {
        QMouseEvent e(type, pos, target->mapToGlobal(pos), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &e);
    };
    auto key = [&](QWidget *target, int value) {
        QKeyEvent e(QEvent::KeyPress, value, Qt::NoModifier);
        QApplication::sendEvent(target, &e);
    };
    key(this, Qt::Key_6);
    check(m_tool == Tool::Slide, "6 selects the slide tool");
    auto depth = m_controller->undoDepth();
    QPointF start(beatsToX(1), pitchToY(72) + m_rowHeight * .5), end(beatsToX(3), start.y());
    mouse(this, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(this, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(this, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    auto slides = daw::slides::editable(*clip());
    check(slides.size() == 1 && std::abs(slides[0].lengthBeats - 2) < 1e-6 &&
              !slides[0].targetNoteIds.empty(),
          "drag creates a linked silent slide of the requested length");
    check(m_controller->undoDepth() == depth + 1, "creation has one undo record");
    if (slides.empty())
        return false;
    auto before = slides;
    mouse(this, QEvent::MouseButtonPress, QPointF(beatsToX(1.5), start.y()), Qt::LeftButton,
          Qt::LeftButton);
    mouse(this, QEvent::MouseMove, QPointF(beatsToX(2), start.y() - 19), Qt::NoButton,
          Qt::LeftButton);
    key(this, Qt::Key_Escape);
    check(daw::slides::editable(*clip()) == before && m_controller->undoDepth() == depth + 1,
          "Escape restores a move without adding history");
    openSlideEditor(slides.front().id);
    QApplication::processEvents();
    auto *editor = m_slideEditor.data();
    check(editor && !editor->isModal(), "curve editor is nonmodal");
    if (!editor)
        return false;
    auto *canvas = editor->findChild<QWidget *>("slideCurveCanvas");
    check(canvas != nullptr, "curve is accessible to input");
    if (!canvas)
        return false;
    auto old = daw::slides::editable(*clip());
    auto gestureDepth = m_controller->undoDepth();
    QPointF p(canvas->width() * .55, canvas->height() * .4);
    mouse(canvas, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, p + QPointF(20, -20), Qt::NoButton, Qt::LeftButton);
    key(canvas, Qt::Key_Escape);
    check(daw::slides::editable(*clip()) == old && m_controller->undoDepth() == gestureDepth,
          "Escape cancels a curve stroke");
    mouse(canvas, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, p + QPointF(25, 25), Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, p + QPointF(25, 25), Qt::LeftButton, Qt::NoButton);
    check(m_controller->undoDepth() == gestureDepth + 1, "point drag is one undo record");
    auto final = daw::slides::editable(*clip());
    m_controller->undo();
    check(daw::slides::editable(*clip()) == old, "undo restores the exact curve");
    m_controller->redo();
    check(daw::slides::editable(*clip()) == final, "redo restores the edited curve");
    auto *drawing = editor->findChild<QComboBox *>("slideDrawingTool");
    if (drawing) {
        drawing->setCurrentIndex(1);
        const auto beforeStroke = daw::slides::editable(*clip());
        const auto strokeDepth = m_controller->undoDepth();
        const QPointF points[]{{canvas->width() * .22, canvas->height() * .22},
                               {canvas->width() * .4, canvas->height() * .72},
                               {canvas->width() * .6, canvas->height() * .26},
                               {canvas->width() * .8, canvas->height() * .65}};
        mouse(canvas, QEvent::MouseButtonPress, points[0], Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i < 4; ++i)
            mouse(canvas, QEvent::MouseMove, points[i], Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, points[3], Qt::LeftButton, Qt::NoButton);
        final = daw::slides::editable(*clip());
        int reversals = 0;
        double previous = 0;
        for (std::size_t i = 1; i < final[0].points.size(); ++i) {
            double delta = final[0].points[i].value - final[0].points[i - 1].value;
            if (delta * previous < 0)
                ++reversals;
            if (delta)
                previous = delta;
        }
        check(reversals >= 2 && m_controller->undoDepth() == strokeDepth + 1,
              "one pencil stroke retains multiple rises and falls with one undo");
        m_controller->undo();
        check(daw::slides::editable(*clip()) == beforeStroke,
              "pencil undo restores its complete original shape");
        m_controller->redo();
    } else
        check(false, "pencil tool is available");
    if (!images.isEmpty()) {
        QDir().mkpath(images);
        window()->grab().save(images + "/piano-slides.png");
        editor->grab().save(images + "/slide-editor.png");
    }
    editor->close();
    QApplication::processEvents();
    selectAll();
    duplicateSelection();
    auto copied = daw::slides::editable(*clip());
    check(copied.size() == 2 && copied[1].id != copied[0].id &&
              copied[1].referenceNoteId != copied[0].referenceNoteId,
          "duplicate remaps linked notes and slides together");
    m_controller->undo();
    check(daw::slides::editable(*clip()) == final, "duplicate undo restores both collections");
    auto multiple = final;
    multiple.push_back(daw::slides::create(daw::midiNotes(*clip()), 4, 1, 67));
    publishSlides(multiple, true);
    auto ordered = daw::slides::editable(*clip());
    QPointF center = slideRect(ordered.front()).center();
    mouse(this, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(this, QEvent::MouseMove, center + QPointF(30, -19), Qt::NoButton, Qt::LeftButton);
    key(this, Qt::Key_Escape);
    check(daw::slides::editable(*clip()) == ordered,
          "cancelling one of several slides preserves their exact order");
    return ok;
}
