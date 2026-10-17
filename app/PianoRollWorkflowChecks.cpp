#include "PianoRollWindow.hpp"
#include "PianoRollTools.hpp"
#include "TimelineWidget.hpp"
#include "InternalEditorFrame.hpp"
#include "EngineController.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerParams.hpp"
#include "UiConstants.hpp"
#include "Theme.hpp"
#include "ScrollMotion.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "graphics/SceneRecorder.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QQuickWindow>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <algorithm>

namespace {
bool writeWave(const QString& path, qint16 amplitude) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4); out << quint32(36 + 2048);
    out.writeRawData("WAVEfmt ", 8); out << quint32(16) << quint16(1) << quint16(1)
        << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
    out.writeRawData("data", 4); out << quint32(2048);
    for (int i = 0; i < 1024; ++i) out << qint16(i % 64 < 32 ? amplitude : -amplitude);
    return out.status() == QDataStream::Ok;
}
}

bool PianoRollView::checkNavigationForTest(const QString& images) {
    const double savedGridWidth = ui::gridLineWidth();
    const auto restoreGridWidth = qScopeGuard([&] { ui::setGridLineWidth(savedGridWidth); });
    ui::setGridLineWidth(ui::kGridLineWidthDefault);
    auto& preferences = ui::ScrollPreferences::instance();
    const bool smooth = preferences.enabled();
    const auto restoreSmoothing = qScopeGuard([&] { preferences.setEnabled(smooth); });
    preferences.setEnabled(false);
    ThemeManager::instance().apply();
    daw::EngineController controller;
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Piano navigation");
    const auto clipId = controller.addMidiClip(track, 0, 120);
    QWidget host;
    host.setWindowTitle("Piano roll grid and navigation check (closes automatically)");
    if (qEnvironmentVariableIntValue("VLT_UI_CHECK_VISIBLE")) host.setWindowFlag(Qt::WindowStaysOnTopHint);
    QVBoxLayout layout(&host); layout.setContentsMargins(0, 0, 0, 0);
    PianoRollView view(&controller, &host);
    layout.addWidget(&view);
    view.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    view.m_pxPerBeat = 68; view.m_rowHeight = 18; view.m_scrollY = (127 - 84) * 18;
    view.m_showVelocityLane = false;
    host.resize(1280, 700);
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s piano navigation: %s\n", pass ? "PASS" : "FAIL", label); ok &= pass;
    };
    check(view.m_showNoteNames && view.m_showAllKeyNames, "note and key names are visible by default");
    // Replay the actual GPU image meshes through QPainter. This checks native
    // edge coverage and cap/middle seams without depending on display MSAA.
    bool noteEdges = true;
    for (qreal scale : {1., 1.25, 1.5, 1.75, 2.}) {
        for (qreal width : {3., 8., 13., 83., 610.}) for (qreal height : {7., 18., 30.}) {
            for (bool borders : {false, true}) for (bool muted : {false, true}) {
                view.m_noteBorders = borders;
                const QRectF bounds(2.37, 2.61, width, height);
                const QColor fill = muted ? QColor(76, 82, 91) : QColor(226, 91, 121);
                const QSize size(620, 36);
                QImage expected(QSize(qRound(size.width() * scale), qRound(size.height() * scale)),
                                QImage::Format_ARGB32_Premultiplied);
                expected.setDevicePixelRatio(scale); expected.fill(Qt::transparent);
                { QPainter p(&expected); view.paintNoteShape(p, bounds, fill, muted); }
                ui::graphics::SceneRecorder recorder(size, scale);
                { QPainter p(&recorder); view.paintNoteShape(p, bounds, fill, muted); }
                const auto meshes = recorder.takeMeshes();
                QImage actual(expected.size(), expected.format());
                actual.setDevicePixelRatio(scale); actual.fill(Qt::transparent);
                bool textured = recorder.supported() && !meshes.empty();
                {
                    QPainter p(&actual);
                    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    for (const auto& mesh : meshes) {
                        if (mesh.texture.isNull() || mesh.vertices.size() != 6 || mesh.group || mesh.clip) {
                            textured = false; break;
                        }
                        const auto& tl = mesh.vertices[0];
                        const auto& br = mesh.vertices[4];
                        p.drawImage(QRectF(QPointF(tl.x, tl.y), QPointF(br.x, br.y)), mesh.texture,
                            QRectF(QPointF(tl.u * mesh.texture.width(), tl.v * mesh.texture.height()),
                                   QPointF(br.u * mesh.texture.width(), br.v * mesh.texture.height())));
                    }
                }
                int difference = 0;
                for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x) {
                    const QRgb a = actual.pixel(x, y), b = expected.pixel(x, y);
                    difference = std::max({difference, std::abs(qRed(a) - qRed(b)),
                        std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b)),
                        std::abs(qAlpha(a) - qAlpha(b))});
                }
                if (!textured || difference > 4)
                    std::fprintf(stderr, "note edge DPR=%.2f size=%.0fx%.0f border=%d muted=%d textured=%d difference=%d\n",
                                 double(scale), double(width), double(height), borders, muted, textured, difference);
                noteEdges &= textured && difference <= 4;
            }
        }
    }
    view.m_noteBorders = true;
    check(noteEdges, "GPU note caps match raster coverage at 100-200% DPI, including short and muted notes");
    const auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    std::unique_ptr<ui::graphics::WorkspaceSurface> surface;
    if (qEnvironmentVariableIntValue("VLT_GPU_WORKSPACE") == 1) {
        surface = std::make_unique<ui::graphics::WorkspaceSurface>(&host);
        QObject::connect(surface.get(), &ui::graphics::WorkspaceSurface::failed, &host,
                         [&](const QString& why) { check(false, qPrintable(why)); });
    }
    host.show(); wait(250);
    const auto capture = [&](const QString& name) {
        const qreal dpr = view.devicePixelRatioF();
        QImage raster(QSize(qRound(view.width()*dpr), qRound(view.height()*dpr)), QImage::Format_ARGB32_Premultiplied);
        raster.setDevicePixelRatio(dpr); raster.fill(Qt::transparent);
        { QPainter p(&raster); view.paintGridAndNotes(p, view.rect()); view.paintOverlays(p, view.rect()); }
        if (!images.isEmpty()) {
            QDir().mkpath(images);
            check(raster.save(QDir(images).filePath(name + "-cpu.png")), "CPU preview saved");
            if (surface) {
                view.update(); wait(150);
                const QImage gpu = surface->quickWindow()->grabWindow();
                check(gpu.save(QDir(images).filePath(name + "-gpu.png")),
                      "GPU preview saved");
                if (name.startsWith("fractional-grid") || name == "four-bars-plus-four") {
                    bool continuous = gpu.size() == raster.size();
                    if (continuous) {
                        const QRgb background = th().background.rgb();
                        for (int y = qRound((ui::kRulerHeight + 3) * dpr);
                             y < qRound((view.laneTop() - 3) * dpr); ++y) {
                            int samples = 0, gaps = 0;
                            for (int x = qRound((view.keyboardWidth() + 12) * dpr);
                                 x < gpu.width() - 12; x += 17) {
                                ++samples;
                                gaps += gpu.pixelColor(x, y).rgb() == background &&
                                        raster.pixelColor(x, y).rgb() != background;
                            }
                            continuous &= gaps * 5 < samples * 4;
                        }
                    }
                    check(continuous, "fractional GPU tile boundaries have no horizontal background seams");
                }
            }
        }
        return raster;
    };
    const auto grid = capture("grid");
    const qreal dpr = grid.devicePixelRatio();
    const auto shade = [&](double x, double y) {
        // A one-DIP line can land on either physical pixel at fractional DPI.
        int ink = 255;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                ink = std::min(ink, qGray(grid.pixel(qRound(x*dpr)+dx, qRound(y*dpr)+dy)));
        return ink;
    };
    const double x = view.keyboardWidth(), y = view.pitchToY(81) + 9;
    check(shade(x+272, y) < shade(x+136, y) && shade(x+136, y) <= shade(x+68, y) &&
          shade(x+68, y) < shade(x+17, y) &&
          shade(x+17, y) < shade(x+8, y), "dark grid has subdivision, beat and bar hierarchy");
    check(shade(x+8, view.pitchToY(81)) < shade(x+8, y), "every pitch row has a dark dividing line");
    const auto lineInk = [&](const QImage& image, double beat) {
        const int center = int(std::floor(view.beatsToX(beat) * dpr));
        const int row = qRound(y * dpr);
        const int background = qGray(image.pixel(center - 9, row));
        int ink = 0;
        for (int offset = -6; offset <= 6; ++offset)
            ink += background - qGray(image.pixel(center + offset, row));
        return ink;
    };
    ui::setGridLineWidth(2.5);
    const auto thickGrid = capture("grid-thick");
    bool sharedWidth = true;
    for (double beat : {0.25, 1.0, 2.0, 4.0})
        sharedWidth &= lineInk(thickGrid, beat) > 2 * lineInk(grid, beat);
    check(sharedWidth, "timeline grid thickness changes subdivisions, beats and bars in the open piano roll");
    ui::setGridLineWidth(ui::kGridLineWidthDefault);
    const double initialScrollY = view.m_scrollY;
    view.setScrollX(13.25); view.setScrollY(initialScrollY + 11.25);
    capture("fractional-grid");
    const QSize fullSize = host.size();
    host.resize(641, 263); wait(50);
    view.setScrollX(43.25); view.setScrollY(927.0);
    capture("fractional-grid-compact");
    host.resize(fullSize); wait(50);
    view.setScrollX(0); view.setScrollY(initialScrollY);

    // Opaque white-key and scale rows must not erase the time-range wash.
    for (bool scale : {false, true}) {
        view.m_scaleHighlight = scale;
        view.m_timeRange = {};
        view.invalidateDocumentPaintCaches();
        const auto plain = capture(scale ? "scale-plain" : "range-plain");
        view.m_timeRange = {1.0, 3.0};
        view.invalidateDocumentPaintCaches();
        const auto tinted = capture(scale ? "scale-range" : "range");
        bool allRowsTinted = true;
        for (int pitch = 72; pitch < 84; ++pitch) {
            const QPoint sample(qRound(view.beatsToX(1.125) * dpr),
                                qRound((view.pitchToY(pitch) + 9) * dpr));
            allRowsTinted &= plain.pixel(sample) != tinted.pixel(sample);
        }
        check(allRowsTinted, "time range tints every pitch with and without scale highlighting");
    }
    view.m_scaleHighlight = false;
    view.m_timeRange = {};
    view.invalidateDocumentPaintCaches();

    daw::NoteModel note;
    note.id = "navigation-note"; note.startBeats = 6; note.lengthBeats = 2;
    note.pitch = 72; note.velocity = 96;
    controller.setClipNotes(track, clipId, {note}, "Navigation fixture");
    view.invalidateDocumentPaintCaches();
    view.m_scrollX = 100; capture("unselected-note");
    view.selectOnly(QString::fromStdString(note.id)); capture("selected-note");
    const auto notes = daw::midiNotes(*view.clip());
    const auto selection = view.m_selected;
    const auto depth = controller.undoDepth();
    int edits = 0, changes = 0;
    QObject::connect(&view, &PianoRollView::edited, &host, [&] { ++edits; });
    QObject::connect(&view, &PianoRollView::viewportChanged, &host, [&] { ++changes; });
    const auto mouse = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, surface ? view.mapTo(&host, pos) : pos,
                          view.mapToGlobal(pos), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(surface ? static_cast<QObject*>(surface->quickWindow()) : &view, &event);
    };
    const auto press = [&](QPointF pos) { mouse(QEvent::MouseButtonPress, pos, Qt::MiddleButton, Qt::MiddleButton); };
    const auto move = [&](QPointF pos) { mouse(QEvent::MouseMove, pos, Qt::NoButton, Qt::MiddleButton); };
    const auto release = [&](QPointF pos) { mouse(QEvent::MouseButtonRelease, pos, Qt::MiddleButton, Qt::NoButton); };
    const auto scroll = [&] { return QPointF(view.scrollX(), view.scrollY()); };
    const auto near = [](QPointF a, QPointF b) { return (a-b).manhattanLength() < 1e-7; };
    const QPointF start = view.noteRect(note).center();
    const QPointF before = scroll(), delta(24.25, -15.75);
    press(start);
    check(view.m_panning && !view.m_gestureUndoActive && view.m_auditionPitch < 0 &&
          view.cursor().shape() == Qt::SizeAllCursor, "middle press over a note starts navigation with the native move cursor");
    check(!surface || surface->quickWindow()->cursor().shape() == Qt::SizeAllCursor,
          "GPU window receives the native move cursor");
    move(start+delta);
    check(near(scroll(), before-delta) && changes == 1, "both axes track fractional mouse motion exactly");
    QEvent leave(QEvent::Leave); QApplication::sendEvent(&view, &leave);
    check(view.m_panning && view.cursor().shape() == Qt::SizeAllCursor,
          "leaving the canvas during a grab preserves its move cursor");
    release(start+delta+QPointF(.5,.25));
    check(near(scroll(), before-delta-QPointF(.5,.25)) && !view.m_panning,
          "release consumes its final movement");
    const QPointF stopped = scroll(); wait(200);
    mouse(QEvent::MouseMove, start+QPointF(80,80), Qt::NoButton, Qt::NoButton);
    check(near(scroll(), stopped), "no drift or inertia after release");

    view.setScrollX(1); view.setScrollY(1); press(start); move(start+QPointF(30,30));
    check(near(scroll(), {}), "pan clamps at the top and left boundaries");
    move(start+QPointF(29.75,29.5));
    check(near(scroll(), {.25,.5}), "reversing at a boundary responds immediately"); release(start+QPointF(29.75,29.5));
    view.setScrollX(view.maxScrollX()-1); view.setScrollY(view.maxScrollY()-1);
    press(start); move(start-QPointF(30,30));
    check(near(scroll(), {view.maxScrollX(), view.maxScrollY()}), "pan clamps at the bottom and right boundaries");
    release(start-QPointF(30,30));
    view.setScrollX(100); view.setScrollY(before.y()); press(start); move(start+delta);
    const QPointF interrupted = scroll();
    mouse(QEvent::MouseMove, start+QPointF(100,100), Qt::NoButton, Qt::NoButton);
    check(!view.m_panning && near(scroll(), interrupted), "missing release ends navigation without a jump");
    for (QEvent::Type type : {QEvent::UngrabMouse, QEvent::WindowDeactivate, QEvent::Hide}) {
        press(start); QEvent event(type); QApplication::sendEvent(&view, &event);
        check(!view.m_panning && !view.hasActivePointerGesture(), "interrupted navigation releases its state");
    }
    bool allTools = true;
    for (Tool tool : {Tool::Draw, Tool::Select, Tool::Slice, Tool::Erase, Tool::Mute}) {
        view.m_tool = tool;
        for (QPointF point : {QPointF(300,15), QPointF(20,200), QPointF(800,300)}) {
            press(point); allTools &= view.m_panning && !view.m_gestureUndoActive; release(point);
        }
    }
    view.m_showVelocityLane = true;
    press({300,view.laneTop()+20}); allTools &= view.m_panning && !view.m_laneDragging;
    release({300,view.laneTop()+20}); view.m_showVelocityLane = false;
    check(allTools && edits == 0 && controller.undoDepth() == depth && notes == daw::midiNotes(*view.clip()) &&
          selection == view.m_selected, "navigation across tools, ruler, keyboard and lane preserves notes, selection and undo");

    preferences.setEnabled(true);
    view.setScrollX(100); view.setScrollY(before.y());
    const auto smoothStart = scroll();
    press(start); move(start - QPointF(30, 15));
    check(scroll().x() > smoothStart.x() && scroll().x() < smoothStart.x() + 30,
          "middle drag interpolates between input events");
    release(start - QPointF(30.5, 15.25));
    const auto smoothEnd = smoothStart + QPointF(30.5, 15.25);
    check(near(scroll(), smoothEnd), "smooth drag release preserves the final fractional endpoint");
    wait(70);
    check(near(scroll(), smoothEnd), "smooth drag has no post-release inertia");
    preferences.setEnabled(false);

    ui::ScrollMotion::zoom(&view, {1.2,1}, false,
        [&] { return QPointF(view.m_pxPerBeat, view.m_rowHeight); },
        [&](QPointF scale) { view.m_pxPerBeat = scale.x(); view.update(); });
    press(start);
    const double grabbedZoom = view.m_pxPerBeat;
    wait(200);
    check(!ui::ScrollMotion::isZooming(&view) && view.m_pxPerBeat == grabbedZoom,
          "middle grab stops an in-progress smooth zoom");
    release(start);
    view.m_pxPerBeat = 68;
    view.m_tool = Tool::Draw; view.setScrollX(100); view.setScrollY(before.y());
    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    check(view.m_moving && view.cursor().shape() == Qt::SizeAllCursor, "note press uses the native four-arrow move cursor");
    mouse(QEvent::MouseMove, start+QPointF(68,-18), Qt::NoButton, Qt::LeftButton);
    check(view.cursor().shape() == Qt::SizeAllCursor, "note drag keeps the native move cursor");
    mouse(QEvent::MouseButtonRelease, start+QPointF(68,-18), Qt::LeftButton, Qt::NoButton);
    view.updateCursor({1100,100});
    check(!view.m_moving && view.cursor().shape() == Qt::BitmapCursor, "custom drawing tool icon is restored after release");
    const QRectF movedNote = view.noteRect(daw::midiNotes(*view.clip()).front());
    const QPointF edge(movedNote.left()+1, movedNote.center().y());
    mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
    check(view.m_resizing && view.cursor().shape() == Qt::SizeHorCursor, "note edge retains the native horizontal resize cursor");
    mouse(QEvent::MouseButtonRelease, edge, Qt::LeftButton, Qt::NoButton);
    press(start); view.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    check(!view.m_panning && !view.hasActivePointerGesture(), "changing clips clears navigation state");
    view.m_pxPerBeat = 68;
    view.setScrollX(0); view.m_pointerInside = false; capture("piano-note");
    // Use the same phrase for both silhouettes so their shared raised face and
    // the brightness-only selection are easy to compare at each display scale.
    view.m_scrollY = (127 - 65) * 18;
    view.m_showNoteNames = true;
    view.m_colorMode = ColorMode::Custom;
    note.pitch = 60; note.startBeats = 1; note.lengthBeats = 3.5;
    note.color = 0xef5068;
    auto selectedNote = note;
    selectedNote.id = "selected-style-note"; selectedNote.pitch = 57;
    selectedNote.lengthBeats = 1.75;
    controller.setClipNotes(track, clipId, {note, selectedNote}, "Style fixture");
    view.selectOnly(QString::fromStdString(selectedNote.id));
    for (NoteStyle style : {NoteStyle::Rounded, NoteStyle::Flat}) {
        view.setNoteStyle(style);
        capture(style == NoteStyle::Rounded ? "raised-rounded" : "raised-square");
    }
    view.setNoteStyle(NoteStyle::Rounded);
    for (const auto [numerator, denominator] : {std::pair{4, 4}, std::pair{3, 4}, std::pair{6, 8}}) {
        controller.setTimeSignature(numerator, denominator);
        const double barBeats = numerator * 4.0 / denominator;
        for (int bars : {1, 4}) {
            const double duration = daw::beatsToSeconds(bars * barBeats, controller.project().tempo);
            const auto shortClip = controller.addMidiClip(track, 0, duration);
            const auto undoBeforeOpen = controller.undoDepth();
            view.setClip(QString::fromStdString(track), QString::fromStdString(shortClip));
            const double expected = (bars + 4) * barBeats;
            check(std::abs(view.editorBeats() - expected) < 1e-9 &&
                  std::abs(view.clipBeats() - bars * barBeats) < 1e-9 &&
                  view.clip()->durationSeconds == duration && controller.undoDepth() == undoBeforeOpen,
                  "opening a clip adds four editor bars without changing its duration or undo");
            view.setPixelsPerBeat(100);
            check(std::abs(view.maxScrollX() - std::max(0.0, expected * 100 -
                      (view.width() - view.keyboardWidth()))) < 1e-9,
                  "horizontal scrolling includes the four extra bars");
            view.zoomToFit();
            check(std::abs(view.beatsToX(expected) - view.width()) < 1e-6,
                  "fit includes the clip and four extra bars in every meter");
            if (numerator == 4 && bars == 4) {
                const auto padded = capture("four-bars-plus-four");
                const double rowY = view.pitchToY(65) + view.m_rowHeight * 0.5;
                const auto pixel = [&](double beat, double sampleY) {
                    // Grid centres now match the timeline: the middle of the
                    // physical pixel containing this logical coordinate.
                    return padded.pixel(int(std::floor(view.beatsToX(beat) * dpr)),
                                        int(std::floor(sampleY * dpr)));
                };
                check(pixel(0.125, rowY) != pixel(16.125, rowY) &&
                      pixel(16.25, rowY) != pixel(16.125, rowY) &&
                      pixel(16.125, view.pitchToY(65)) != pixel(16.125, rowY),
                      "extra bars have a distinct background and retain both grid directions");
                const QPointF outside(view.beatsToX(18.125), rowY);
                mouse(QEvent::MouseButtonPress, outside, Qt::LeftButton, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
                const auto& outsideNotes = daw::midiNotes(*view.clip());
                check(outsideNotes.size() == 1 && outsideNotes.front().startBeats >= 16.0 &&
                      view.clip()->durationSeconds == duration,
                      "drawing in extra bars keeps the arrangement clip length unchanged");
                capture("note-beyond-clip");
            }
        }
    }
    return ok;
}

bool PianoRollWindow::checkNavigatorForTest(const QString& images) {
    ThemeManager::instance().apply();
    QSettings().remove("pianoRoll/navigation.height");
    daw::EngineController controller;
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Navigator phrase");
    const auto clipId = controller.addMidiClip(track, 0, 8);
    daw::miditools::Notes notes;
    for (int step = 0; step < 16; ++step) {
        daw::NoteModel note;
        note.id = daw::newUuid(); note.startBeats = step; note.lengthBeats = 0.75;
        note.pitch = 60 + (step * 5) % 12; note.velocity = 90;
        notes.push_back(note);
        if (step % 4 == 0) {
            note.id = daw::newUuid(); note.pitch = 48 + step / 4 * 2; note.lengthBeats = 3.5;
            notes.push_back(note);
        }
    }
    controller.setClipNotes(track, clipId, notes, "Fixture");
    QWidget host;
    host.setWindowTitle("Piano navigator check (closes automatically)");
    if (qEnvironmentVariableIntValue("VLT_UI_CHECK_VISIBLE")) host.setWindowFlag(Qt::WindowStaysOnTopHint);
    QVBoxLayout layout(&host); layout.setContentsMargins(0, 0, 0, 0);
    PianoRollWindow window(&controller, &host);
    layout.addWidget(&window);
    window.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    window.m_view->setPixelsPerBeat(90);
    window.m_view->setRowHeight(16);
    window.m_view->setScrollY((127 - 76) * 16);
    host.resize(1200, 680);
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s piano navigator: %s\n", pass ? "PASS" : "FAIL", label); ok &= pass;
    };
    const auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    std::unique_ptr<ui::graphics::WorkspaceSurface> surface;
    if (qEnvironmentVariableIntValue("VLT_GPU_WORKSPACE") == 1) {
        surface = std::make_unique<ui::graphics::WorkspaceSurface>(&host);
        connect(surface.get(), &ui::graphics::WorkspaceSurface::failed, &host,
                [&](const QString& why) { check(false, qPrintable(why)); });
    }
    host.show(); wait(250);
    const auto paintFrame = [&] {
        window.grab();
        // Native controls are rasterized on the next retained-scene frame.
        if (surface) { wait(80); surface->quickWindow()->grabWindow(); }
    };
    const auto capture = [&](const QString& name) {
        QApplication::processEvents();
        const QImage raster = window.grab().toImage();
        if (!images.isEmpty()) {
            QDir().mkpath(images);
            check(raster.save(QDir(images).filePath(name + "-cpu.png")), "CPU preview saved");
            if (surface) {
                surface->invalidate(); wait(100);
                const QImage gpu = surface->quickWindow()->grabWindow();
                check(!gpu.isNull() && gpu.save(QDir(images).filePath(name + "-gpu.png")), "GPU preview saved");
            }
        }
    };
    auto* bar = window.m_hScroll;
    const auto mouse = [&](QEvent::Type type, const QPointF& local, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF global = bar->mapToGlobal(local);
        const QPointF routed = surface ? bar->mapTo(&host, local) : local;
        QMouseEvent event(type, routed, global, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(surface ? static_cast<QObject*>(surface->quickWindow()) : bar, &event);
        QApplication::processEvents();
    };
    const auto resize = [&](int delta) {
        const QPointF edge(bar->width() * 0.5, bar->height() - 1);
        mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, edge + QPointF(0, delta), Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, edge + QPointF(0, delta), Qt::LeftButton, Qt::NoButton);
    };
    check(bar->height() == 10 && window.checkCompactLayoutForTest(), "default 10 px navigator joins the ruler and side rail");
    capture("navigator-10");
    const QImage original = window.m_navigatorPreview;
    const double before = window.m_view->scrollX();
    const QPointF thumb(12, 4);
    mouse(QEvent::MouseButtonPress, thumb, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, thumb + QPointF(120, 0), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, thumb + QPointF(120, 0), Qt::LeftButton, Qt::NoButton);
    check(window.m_view->scrollX() > before && bar->height() == 10, "native thumb drag scrolls without resizing");
    paintFrame();
    check(window.m_navigatorPreview.cacheKey() == original.cacheKey(), "panning reuses the note overview");
    resize(100);
    check(bar->height() == 30 && window.checkCompactLayoutForTest(), "lower-edge drag expands to 30 px and preserves panel alignment");
    bar->setValue(0);
    capture("navigator-30");
    resize(-100);
    check(bar->height() == 10, "lower-edge drag clamps at 10 px");
    resize(10);
    check(bar->height() == 20, "lower-edge drag supports intermediate heights");
    window.saveViewPreferences();
    {
        PianoRollWindow reopened(&controller);
        check(reopened.m_hScroll->height() == 20, "navigator height survives reopening");
    }
    mouse(QEvent::MouseButtonDblClick, QPointF(bar->width() / 2, 19), Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, QPointF(bar->width() / 2, 9), Qt::LeftButton, Qt::NoButton);
    check(bar->height() == 10, "double-click on the edge restores the compact height");
    paintFrame();
    const QImage beforeEdit = window.m_navigatorPreview;
    auto changed = notes;
    changed.front().startBeats = 25;
    controller.setClipNotes(track, clipId, changed, "Move note");
    window.refresh(); paintFrame();
    check(window.m_navigatorPreview != beforeEdit, "note edits update the overview, including the extra bars");
    controller.undo(); window.refresh(); paintFrame();
    check(window.m_navigatorPreview == beforeEdit, "undo restores the overview");
    const auto emptyClip = controller.addMidiClip(track, 12, 2);
    window.setClip(QString::fromStdString(track), QString::fromStdString(emptyClip)); paintFrame();
    check(window.m_navigatorPreview != beforeEdit, "switching to an empty clip discards the previous notes");
    capture("navigator-empty");
    window.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    bar->setValue(0);
    resize(20);
    window.m_view->setColorMode(PianoRollView::ColorMode::Pitch);
    capture("navigator-pitch");
    return ok;
}

bool PianoRollWindow::checkWorkflowsForTest(const QString& images) {
    if (qEnvironmentVariableIsSet("VLT_PIANO_SPLIT_CHECK_ONLY")) return PianoRollView::checkSplitPreviewForTest(images);
    if (qEnvironmentVariableIsSet("VLT_PIANO_NAVIGATOR_CHECK_ONLY")) return checkNavigatorForTest(images);
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Sampler phrase");
    const auto clipId = controller.addMidiClip(track, 0, 16);
    QWidget workspace;
    workspace.resize(1200, 740);
    InternalEditorFrame pianoFrame("checks/pianoWorkflows/piano", &workspace);
    PianoRollWindow window(&controller, &pianoFrame);
    window.resize(1080, 570);
    window.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    pianoFrame.setContent(&window);
    QPointer<InternalEditorFrame> chordFrame;
    connect(&window, &PianoRollWindow::internalWindowRequested, &workspace,
            [&](QWidget* content, const QString&) {
        if (!chordFrame) {
            QSettings().remove("checks/pianoWorkflows/chord");
            chordFrame = new InternalEditorFrame("checks/pianoWorkflows/chord", &workspace);
            chordFrame->setContent(content);
        }
        chordFrame->present();
    });
    workspace.show();
    pianoFrame.present();
    QApplication::processEvents();
    bool ok = PianoRollView::checkVelocityRampForTest(images);
    ok &= PianoRollView::checkSplitPreviewForTest(images);
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s piano workflow: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    const auto notes = [&] { return daw::midiNotes(*window.m_view->clip()); };
    const auto playbackMatches = [&](daw::miditools::Notes expected) {
        const auto graph = controller.routingGraph();
        const auto* ids = controller.trackNodes(track);
        if (!graph || !ids) return false;
        std::stable_sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
            return a.startBeats < b.startBeats;
        });
        for (const auto& entry : graph->nodes) {
            if (entry.id != ids->midiClips) continue;
            const auto* player = dynamic_cast<const daw::engine::MidiClipPlayerNode*>(entry.node);
            const auto heard = player ? player->notes() : nullptr;
            if (!heard || heard->size() != expected.size()) return false;
            for (std::size_t i = 0; i < expected.size(); ++i)
                if ((*heard)[i].key != expected[i].pitch || (*heard)[i].velocity != expected[i].velocity ||
                    std::abs((*heard)[i].startBeats - expected[i].startBeats) > 1e-6 ||
                    std::abs((*heard)[i].lengthBeats - expected[i].lengthBeats) > 1e-6) return false;
            return true;
        }
        return false;
    };
    window.m_chordAction->trigger();
    QApplication::processEvents();
    auto* dialog = window.m_chordDialog;
    if (!dialog || !chordFrame) return false;
    auto* source = dialog->findChild<QComboBox*>("ChordSource");
    auto* type = dialog->findChild<QComboBox*>("ChordType");
    auto* root = dialog->findChild<QComboBox*>("ChordRoot");
    auto* inversion = dialog->findChild<QSpinBox*>("ChordInversion");
    auto* start = dialog->findChild<QDoubleSpinBox*>("ChordStart");
    auto* length = dialog->findChild<QDoubleSpinBox*>("ChordLength");
    auto* velocity = dialog->findChild<QSpinBox*>("ChordVelocity");
    auto* bass = dialog->findChild<QCheckBox*>("ChordBass");
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    if (!source || !type || !root || !inversion || !start || !length ||
        !velocity || !bass || !buttons) return false;
    check(source->currentIndex() == 1 && window.m_view->m_preview && window.m_view->m_preview->size() == 3 && notes().empty(),
          "empty clip immediately previews a new chord without changing the document");
    check(window.m_view->m_preview && playbackMatches(*window.m_view->m_preview),
          "opening a MIDI tool publishes the painted preview to the audio source");
    check(window.m_view->soundingPitchesAtBeat(0).count() == 3,
          "keyboard highlights follow the sounding preview instead of the empty document");
    root->setCurrentIndex(62);
    type->setCurrentIndex(type->findData(int(daw::miditools::ChordParams::Type::Minor7)));
    inversion->setValue(1);
    start->setValue(1.25); length->setValue(2.5); velocity->setValue(87);
    bass->setChecked(true);
    const auto depth = controller.undoDepth();
    buttons->button(QDialogButtonBox::Apply)->click();
    const auto generated = notes();
    check(playbackMatches(generated) && !controller.clipNotePreviewActive(window.m_view),
          "Apply retains the exact audible result and ends the temporary override");
    check(generated.size() == 5 && generated[0].pitch == 50 && generated[1].pitch == 74 &&
          generated[2].pitch == 65 && generated[0].startBeats == 1.25 &&
          generated[0].lengthBeats == 2.5 && generated[0].velocity == 87 &&
          controller.undoDepth() == depth + 1,
          "Apply flushes the latest pitch, inversion, bass, time, length and velocity in one undo");
    controller.undo(); check(notes().empty(), "chord undo restores an empty clip");
    controller.redo(); check(notes() == generated, "chord redo preserves note identities");
    check(chordFrame->width() < 580 && chordFrame->height() < 420,
          "hosted chord editor opens compactly");
    const auto* shortcut = window.findChild<QToolButton*>("PianoRollBuildChordsButton");
    const auto* scope = window.findChild<QToolButton*>("PianoRollSlideScope");
    check(shortcut && shortcut->parentWidget() == window.m_toolbar &&
          scope && scope->width() == 28 && scope->menu() &&
          scope->menu()->actions().size() == 2 && window.m_clipSelector->width() <= 280,
          "chord shortcut lives with the tools and slide scope is compact");
    if (scope && scope->menu() && scope->menu()->actions().size() == 2) {
        scope->menu()->actions()[1]->trigger();
        check(window.m_view->m_slideChord && scope->menu()->actions()[1]->isChecked(),
              "compact slide scope selects the chord target");
        scope->menu()->actions()[0]->trigger();
        check(!window.m_view->m_slideChord && scope->menu()->actions()[0]->isChecked(),
              "compact slide scope returns to the single-note target");
    }
    check(window.m_viewMenu->actions().contains(window.m_noteStyleMenu->menuAction()),
          "note appearance remains reachable through Settings and View");
    QStyleOptionComboBox comboStyle;
    comboStyle.initFrom(window.m_clipSelector);
    const auto textArea = window.m_clipSelector->style()->subControlRect(
        QStyle::CC_ComboBox, &comboStyle, QStyle::SC_ComboBoxEditField, window.m_clipSelector);
    check(textArea.width() >= window.m_clipSelector->fontMetrics().horizontalAdvance(
              window.m_clipSelector->currentText()), "compact clip selector fits its complete caption");
    if (!images.isEmpty()) {
        QDir().mkpath(images);
        QApplication::processEvents();
        window.grab().save(QDir(images).filePath("piano.png"));
        chordFrame->grab().save(QDir(images).filePath("chord-new.png"));
    }
    dialog->close(); chordFrame->hide();
    controller.undo();
    daw::NoteModel base;
    base.id = daw::newUuid(); base.pitch = 60; base.startBeats = 0; base.lengthBeats = 2;
    daw::NoteModel untouched = base;
    untouched.id = daw::newUuid(); untouched.pitch = 55; untouched.startBeats = 4;
    controller.setClipNotes(track, clipId, {base, untouched}, "Fixture");
    window.m_view->m_selected = {QString::fromStdString(base.id)};
    window.m_chordAction->trigger();
    check(source->currentIndex() == 0 && !start->isVisible(),
          "existing selected notes open the harmonization controls");
    type->setCurrentIndex(type->findData(int(daw::miditools::ChordParams::Type::Major)));
    inversion->setValue(0); bass->setChecked(false);
    window.flushToolPreview(dialog);
    if (!images.isEmpty()) {
        QApplication::processEvents();
        chordFrame->grab().save(QDir(images).filePath("chord-existing.png"));
    }
    buttons->button(QDialogButtonBox::Apply)->click();
    const auto harmonized = notes();
    check(harmonized.size() == 4 && std::find(harmonized.begin(), harmonized.end(), untouched) != harmonized.end(),
          "harmonizing the selection preserves other notes");
    controller.undo();
    type->setCurrentIndex(type->findData(int(daw::miditools::ChordParams::Type::Minor)));
    window.flushToolPreview(dialog);
    dialog->close(); chordFrame->hide();
    check(!window.m_view->hasPreview() && notes() == daw::miditools::Notes({base, untouched}),
          "closing discards the unapplied preview");
    check(playbackMatches(notes()), "closing a MIDI tool restores original playback");
    window.m_chordAction->trigger();
    auto* previewToggle = dialog->findChild<QCheckBox*>();
    for (auto* box : dialog->findChildren<QCheckBox*>())
        if (box != bass) previewToggle = box;
    if (!previewToggle || previewToggle == bass) return false;
    previewToggle->setChecked(false);
    check(!window.m_view->hasPreview() && playbackMatches(notes()),
          "turning Preview off restores original playback immediately");
    previewToggle->setChecked(true);
    window.flushToolPreview(dialog);
    check(window.m_view->m_preview && playbackMatches(*window.m_view->m_preview),
          "turning Preview on restores the audible transform");
    type->setCurrentIndex(type->findData(int(daw::miditools::ChordParams::Type::Minor7)));
    window.hide(); QApplication::processEvents();
    check(!window.m_view->hasPreview() && playbackMatches(notes()) && !window.m_pendingToolPreview,
          "hiding the piano roll cancels both audible preview and queued parameter updates");
    dialog->close(); chordFrame->hide(); window.show();

    window.m_view->setPixelsPerBeat(150);
    window.m_view->setScrollX(1300);
    controller.seekSeconds(0);
    controller.play();
    window.m_view->refreshPlayheadFrame();
    const double manuallyScrolled = window.m_view->scrollX();
    check(controller.isPlaying() && manuallyScrolled == 1300,
          "playback never returns the viewport to an offscreen playhead");
    window.m_view->setScrollX(2000);
    controller.seekSeconds(1);
    window.m_view->refreshPlayheadFrame();
    check(window.m_view->scrollX() == 2000, "manual navigation stays free while playing");
    controller.stop();

    std::unique_ptr<ui::graphics::WorkspaceSurface> gpu;
    if (qEnvironmentVariableIsSet("DAW_PIANO_WORKFLOW_GPU")) {
        workspace.hide();
        gpu = std::make_unique<ui::graphics::WorkspaceSurface>(&workspace);
        workspace.show();
        QElapsedTimer ready;
        ready.start();
        while (!gpu->quickWindow()->isSceneGraphInitialized() && ready.elapsed() < 3000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        check(gpu->quickWindow()->isSceneGraphInitialized() &&
              !gpu->quickWindow()->grabWindow().isNull(), "native GPU scene is active for sample-drop routing");
    }
    QTemporaryDir samples;
    const QString first = samples.filePath("first.wav"), second = samples.filePath("second.wav");
    if (!writeWave(first, 2000) || !writeWave(second, 4000) ||
        !controller.loadInstrumentSampler(track, first.toStdString())) return false;
    namespace smp = daw::plugins::sampler;
    const auto slot = controller.project().findTrack(track)->instrument.id;
    auto* sampler = controller.samplerInstance(track, slot);
    sampler->setParameter(smp::indexOf(smp::Param::AmpAttack), 0.15);
    sampler->setParameter(smp::indexOf(smp::Param::Pan), -0.35);
    sampler->setParameter(smp::indexOf(smp::Param::LoopMode), 1);
    sampler->setParameter(smp::indexOf(smp::Param::RootNote), 57);
    sampler->setParameter(smp::indexOf(smp::SlideParam::TimeMs), 250);
    const auto parameters = [&](smp::SamplerInstance* instance) {
        std::vector<double> values;
        for (unsigned i = 0; i < smp::kParameterCount; ++i) values.push_back(instance->parameterValue(i));
        return values;
    };
    const auto beforeParameters = parameters(sampler);
    const auto beforeNotes = notes();
    auto* view = window.m_view;
    const QPoint gridPoint(int(view->keyboardWidth() + 70), int(ui::kRulerHeight + 60));
    const auto dropSample = [&](const QString& path, QPoint point) {
        QApplication::processEvents();
        QObject* receiver = gpu ? static_cast<QObject*>(gpu->quickWindow()) : view;
        if (gpu) point = view->mapTo(&workspace, point);
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &enter);
        if (enter.isAccepted() && !images.isEmpty()) {
            if (gpu) {
                gpu->invalidate(); QApplication::processEvents();
                gpu->quickWindow()->grabWindow().save(QDir(images).filePath("drop-gpu.png"));
            } else view->grab().save(QDir(images).filePath("drop.png"));
        }
        QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &drop);
        return enter.isAccepted() && drop.isAccepted();
    };
    const auto sampleDepth = controller.undoDepth();
    check(dropSample(second, gridPoint) && sampler == controller.samplerInstance(track, slot) &&
          sampler->samplePath() == second.toStdString() && parameters(sampler) == beforeParameters &&
          notes() == beforeNotes && controller.undoDepth() == sampleDepth + 1,
          "grid drop replaces only the sample, preserving all SMP/INS/slide parameters and notes");
    controller.undo();
    check(sampler->samplePath() == first.toStdString() && parameters(sampler) == beforeParameters,
          "sample replacement undo restores the file without changing parameters");
    controller.redo();
    check(sampler->samplePath() == second.toStdString(), "sample replacement redo restores the new file");
    check(!dropSample(first, QPoint(5, 5)) && sampler->samplePath() == second.toStdString(),
          "ruler and keyboard are not sample drop targets");
    const QString broken = samples.filePath("broken.wav");
    QFile invalid(broken); invalid.open(QIODevice::WriteOnly); invalid.write("not audio"); invalid.close();
    const auto invalidDepth = controller.undoDepth();
    check(!dropSample(broken, gridPoint) && sampler->samplePath() == second.toStdString() &&
          parameters(sampler) == beforeParameters && controller.undoDepth() == invalidDepth,
          "a corrupt sample leaves the current sound and history intact");
    const auto instrument = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
    const auto pattern = controller.addPattern("Pattern drop");
    const auto sound = controller.addPatternInstrument(pattern, *instrument);
    const auto* soundTrack = controller.project().findTrack(sound);
    const auto sourceClip = soundTrack->clips.front().id;
    const auto soundSlot = soundTrack->instrument.id;
    window.setClip(QString::fromStdString(sound), QString::fromStdString(sourceClip));
    check(dropSample(first, gridPoint) &&
          controller.samplerInstance(sound, soundSlot)->samplePath() == first.toStdString() &&
          sampler->samplePath() == second.toStdString(),
          "a pattern piano roll replaces only its open sound, leaving the previous instrument alone");
    const auto other = controller.addTrack(daw::TrackKind::Midi, "No sampler");
    const auto otherClip = controller.addMidiClip(other, 0, 8);
    window.setClip(QString::fromStdString(other), QString::fromStdString(otherClip));
    const auto rejectDepth = controller.undoDepth();
    check(!dropSample(first, gridPoint) && controller.undoDepth() == rejectDepth &&
          controller.project().findTrack(other)->instrument.id.empty(),
          "non-sampler tracks reject drops without installing an instrument");
    ok &= TimelineWidget::checkMidiClipOpeningForTest();
    return ok;
}

bool TimelineWidget::checkMidiClipOpeningForTest() {
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Create then open");
    TimelineWidget timeline(&controller);
    timeline.resize(1000, 500); timeline.setTool(Tool::Select); timeline.show();
    QApplication::processEvents();
    int opened = 0;
    QString openedClip;
    connect(&timeline, &TimelineWidget::openPianoRollRequested, &timeline,
            [&](const QString&, const QString& id) { ++opened; openedClip = id; });
    const QPointF point(timeline.secondsToX(0.5), timeline.laneCentreForTest(0));
    const auto doubleClick = [&] {
        QMouseEvent event(QEvent::MouseButtonDblClick, point, timeline.mapToGlobal(point),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &event);
        QMouseEvent release(QEvent::MouseButtonRelease, point, timeline.mapToGlobal(point),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &release);
    };
    doubleClick();
    const auto* created = controller.project().findTrack(track);
    const bool onlyCreate = created && created->clips.size() == 1 && opened == 0;
    doubleClick();
    const bool thenOpen = created && created->clips.size() == 1 && opened == 1 &&
                          openedClip.toStdString() == created->clips.front().id;
    std::printf("%s piano workflow: empty MIDI lane creates only, existing clip opens on double click\n",
                onlyCreate && thenOpen ? "PASS" : "FAIL");
    return onlyCreate && thenOpen;
}

bool PianoRollView::checkSplitPreviewForTest(const QString& images) {
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Split notes");
    const auto clipId = controller.addMidiClip(track, 0, 8);
    PianoRollWindow window(&controller);
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1080, 560);
    window.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    window.show();
    QApplication::setActiveWindow(&window);
    QApplication::processEvents();
    auto& view = *window.findChild<PianoRollView*>();
    view.setFocus();
    view.m_adaptiveSnap = false;
    view.m_gridBeats = 0.25;
    view.m_pxPerBeat = 140;
    view.m_rowHeight = 16;
    view.m_scrollY = (127 - 68) * 16;
    view.m_scrollX = 0;
    view.setTool(Tool::Draw);
    daw::NoteModel source;
    source.id = "split-source";
    source.startBeats = 0.5;
    source.lengthBeats = 1.0;
    source.pitch = 60;
    source.velocity = 79;
    source.pan = -0.3f;
    auto other = source;
    other.id = "untouched";
    other.startBeats = 3;
    other.pitch = 67;
    const daw::miditools::Notes original{source, other};
    controller.setClipNotes(track, clipId, original, "Prepare Split Check");
    const auto notes = [&] { return daw::midiNotes(*view.clip()); };
    const auto select = [&] {
        view.m_selected = {QString::fromStdString(source.id)};
        view.m_primary = QString::fromStdString(source.id);
        emit view.selectionChanged();
        view.setFocus();
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent overrideEvent(QEvent::ShortcutOverride, code, modifiers);
        QApplication::sendEvent(&view, &overrideEvent);
        QKeyEvent press(QEvent::KeyPress, code, modifiers);
        QApplication::sendEvent(&view, &press);
        QKeyEvent release(QEvent::KeyRelease, code, modifiers);
        QApplication::sendEvent(&view, &release);
    };
    const QPointF at(view.beatsToX(1), 230);
    const auto mouse = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button) {
        QMouseEvent event(type, pos, view.mapToGlobal(pos),
            type == QEvent::MouseMove ? Qt::NoButton : button,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : button, Qt::NoModifier);
        QApplication::sendEvent(&view, &event);
    };
    const auto wheel = [&](int delta, QPoint pixels = {}, Qt::ScrollPhase phase = Qt::NoScrollPhase) {
        QWheelEvent event(at, view.mapToGlobal(at), pixels, QPoint(0, delta),
                          Qt::NoButton, Qt::AltModifier, phase, false);
        QApplication::sendEvent(&view, &event);
    };
    const auto parts = [&] {
        daw::miditools::Notes result;
        if (view.m_preview) for (const auto& note : *view.m_preview)
            if (view.m_previewSelection.contains(QString::fromStdString(note.id))) result.push_back(note);
        return result;
    };
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s split preview: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    select();
    const auto depth = controller.undoDepth();
    key(Qt::Key_S, Qt::AltModifier);
    check(view.m_splitPreview && parts().size() == 4 && notes() == original &&
          controller.undoDepth() == depth, "Alt+S previews grid divisions without editing history");
    if (!view.m_splitPreview) return false;
    check(view.splitPreviewLabel() == tr("Split %1 · Notes: %2").arg("1/16").arg(4),
          "readout starts with the musical division and generated note count");
    const QPointF scroll(view.m_scrollX, view.m_scrollY);
    const double zoom = view.pxPerBeat();
    wheel(60);
    check(parts().size() == 4, "high-resolution wheel accumulates partial notches");
    wheel(60);
    check(parts().size() == 8 && view.m_gridBeats == 0.25 && view.pxPerBeat() == zoom &&
          scroll == QPointF(view.m_scrollX, view.m_scrollY), "wheel changes division without scrolling or zooming");
    wheel(-120);
    check(parts().size() == 4, "reverse wheel rebuilds from original notes");
    const auto readoutImage = [&] {
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        view.render(&painter);
        painter.end();
        return image.copy(QRect(view.width() - 350, ui::kRulerHeight, 350, 64));
    };
    const auto beforeReadout = readoutImage();
    wheel(360);
    check(parts().size() == 8 && view.splitPreviewLabel().contains("1/32") &&
          beforeReadout != readoutImage(),
          "accelerated wheel advances one division and repaints the visible readout");
    wheel(-360);
    check(parts().size() == 4, "accelerated reverse wheel also advances only one division");
    bool ordered = true;
    for (int denominator : {32, 64, 128}) {
        wheel(120);
        ordered &= std::abs(view.m_splitPreview->beats - 4.0 / denominator) < 1e-9 &&
                   view.splitPreviewLabel().contains(QStringLiteral("1/%1").arg(denominator));
    }
    for (int denominator : {64, 32, 16}) {
        wheel(-120);
        ordered &= std::abs(view.m_splitPreview->beats - 4.0 / denominator) < 1e-9;
    }
    check(ordered, "ordinary wheel traverses every division in order in both directions");
    wheel(120, QPoint(0, 90));
    check(parts().size() == 8, "detents take precedence over accelerated pixel deltas");
    wheel(-120);
    wheel(0, QPoint(0, 15), Qt::ScrollUpdate);
    check(parts().size() == 4, "pixel-only scrolling accumulates fine motion");
    wheel(0, QPoint(0, 15), Qt::ScrollUpdate);
    check(parts().size() == 8, "pixel-only scrolling advances a single division");
    wheel(0, QPoint(0, 90), Qt::ScrollMomentum);
    check(parts().size() == 8, "trackpad inertia cannot keep subdividing after release");
    wheel(-120);
    wheel(60);
    wheel(-120);
    check(parts().size() == 2, "direction reversal clears partial movement immediately");
    wheel(120);
    mouse(QEvent::MouseButtonPress, at, Qt::MiddleButton);
    mouse(QEvent::MouseMove, at - QPointF(0, 16), Qt::MiddleButton);
    check(parts().size() == 4 && parts().back().pitch == 63, "middle-drag up builds ascending stairs");
    mouse(QEvent::MouseButtonRelease, at - QPointF(0, 32), Qt::MiddleButton);
    check(parts().back().pitch == 66 && view.m_splitPreview && !view.m_splitPreview->dragging,
          "release includes the final drag endpoint and keeps preview active");
    mouse(QEvent::MouseButtonPress, at, Qt::MiddleButton);
    mouse(QEvent::MouseMove, at + QPointF(0, 48), Qt::MiddleButton);
    check(parts().back().pitch == 57 && scroll == QPointF(view.m_scrollX, view.m_scrollY),
          "regrabbing and dragging down reverses the stairs without panning");
    mouse(QEvent::MouseButtonRelease, at + QPointF(0, 48), Qt::MiddleButton);
    if (!images.isEmpty()) {
        QDir().mkpath(images);
        QImage frame(view.size(), QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::transparent);
        QPainter painter(&frame);
        view.render(&painter);
        painter.end();
        check(frame.save(QDir(images).filePath("split-stairs-cpu.png")), "stairs preview rendered");
    }
    const auto painted = view.visibleNotes();
    mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton);
    check(!view.m_preview && !view.m_splitPreview && notes() == painted &&
          view.m_selected.size() == 4 && controller.undoDepth() == depth + 1,
          "left click commits exactly the preview as one edit without adding a note");
    controller.undo();
    check(notes() == original, "one undo restores the original note");
    controller.redo();
    check(notes() == painted, "redo restores the exact generated identities and pitches");
    controller.undo();
    select();
    key(Qt::Key_S, Qt::AltModifier);
    wheel(120);
    key(Qt::Key_Escape);
    check(!view.m_preview && !view.m_splitPreview && notes() == original &&
          controller.undoDepth() == depth, "Escape discards preview without history");
    key(Qt::Key_S, Qt::AltModifier);
    QFocusEvent focusOut(QEvent::FocusOut);
    QApplication::sendEvent(&view, &focusOut);
    check(!view.m_splitPreview && notes() == original, "focus loss safely cancels the mode");
    select();
    key(Qt::Key_S, Qt::AltModifier);
    auto external = original;
    external.back().color = 0x123456;
    controller.setClipNotes(track, clipId, external, "External note edit");
    mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
    check(!view.m_splitPreview && notes() == external, "stale preview cannot overwrite another edit");
    controller.undo();
    view.m_selected.clear();
    emit view.selectionChanged();
    key(Qt::Key_S, Qt::AltModifier);
    check(!view.m_splitPreview && notes() == original, "empty selection does not split the whole clip");
    select();
    key(Qt::Key_S, Qt::AltModifier);
    view.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    check(!view.m_preview && !view.m_splitPreview, "switching clips discards the mode");
    select();
    key(Qt::Key_S, Qt::AltModifier | Qt::ShiftModifier);
    auto* strum = window.findChild<StrumDialog*>();
    check(strum && strum->isVisible() && !view.m_splitPreview,
          "Alt+Shift+S still opens Strum");
    if (strum) strum->reject();
    select();
    key(Qt::Key_S, Qt::AltModifier);
    for (int i = 0; i < 20; ++i) wheel(120);
    check(parts().size() == 32, "fine divisions stop at the supported minimum length");
    wheel(-120);
    check(parts().size() == 16, "reversing at the minimum has no accumulated wheel debt");
    for (int i = 0; i < 20; ++i) wheel(-120);
    check(parts().size() == 1 && view.m_splitPreview->beats == 16,
          "coarse divisions stop at the maximum");
    key(Qt::Key_Escape);
    key(Qt::Key_S, Qt::AltModifier);
    key(Qt::Key_Return);
    check(!view.m_splitPreview && notes().size() == 5,
          "Enter also applies for keyboard users");
    return ok;
}

bool PianoRollView::checkVelocityRampForTest(const QString& images) {
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Velocity ramp");
    const auto clipId = controller.addMidiClip(track, 0, 8);
    PianoRollView view(&controller);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(800, 400);
    view.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    view.show();
    QApplication::processEvents();
    view.m_showKeyboard = view.m_showVelocityLane = true;
    view.m_laneHeight = 110;
    view.m_laneParam = LaneParam::Velocity;
    view.m_pxPerBeat = 100;
    view.m_rowHeight = 16;
    view.m_scrollX = 0;
    view.m_scrollY = (127 - 66) * 16 - 150;
    view.m_tool = Tool::Draw;

    std::vector<daw::NoteModel> fixture(7);
    const double beats[] = {0.5, 1, 1.5, 2, 2, 3, 4};
    const int velocities[] = {90, 70, 50, 110, 80, 100, 60};
    for (std::size_t i = 0; i < fixture.size(); ++i) {
        fixture[i].id = "ramp-" + std::to_string(i);
        fixture[i].startBeats = beats[i];
        fixture[i].lengthBeats = 0.25;
        fixture[i].pitch = 60 + int(i);
        fixture[i].velocity = velocities[i];
        fixture[i].pan = float(i) * 0.1f - 0.3f;
    }
    controller.setClipNotes(track, clipId, fixture, "Prepare Velocity Ramp Check");
    view.invalidateSoundingPitchIndex();
    view.invalidateNotePaintIndex();
    const auto notes = [&] { return daw::midiNotes(*view.clip()); };
    const auto original = notes();
    view.m_selected = {QString::fromStdString(original.back().id)};
    view.m_primary = *view.m_selected.begin();
    const auto selection = view.m_selected;
    const auto primary = view.m_primary;
    const auto depth = controller.undoDepth();
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s velocity ramp: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    const auto point = [&](double beat, int velocity) {
        return QPointF(view.beatsToX(beat), view.laneValueToY(velocity / 127.0));
    };
    const auto mouse = [&](QEvent::Type type, QPointF pos,
                           Qt::MouseButton button = Qt::RightButton) {
        QMouseEvent event(type, pos, view.mapToGlobal(pos),
                          type == QEvent::MouseMove ? Qt::NoButton : button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                          Qt::NoModifier);
        QApplication::sendEvent(&view, &event);
    };
    const auto matches = [&](std::initializer_list<int> values) {
        if (values.size() != original.size()) return false;
        auto expected = original;
        auto value = values.begin();
        for (auto& note : expected) note.velocity = *value++;
        return notes() == expected;
    };
    mouse(QEvent::MouseButtonPress, point(1, 100) + QPointF(2, 0));
    mouse(QEvent::MouseMove, point(3, 20));
    check(matches({90, 100, 80, 60, 60, 20, 60}) &&
          view.m_selected == selection && view.m_primary == primary &&
          controller.undoDepth() == depth && view.m_auditionPitch < 0,
          "downward ramp follows note onset, includes chords and unselected notes, preserves other properties");
    if (!images.isEmpty()) {
        QDir().mkpath(images);
        view.grab().save(QDir(images).filePath("velocity-ramp.png"));
    }
    mouse(QEvent::MouseMove, point(2, 50));
    check(matches({90, 100, 75, 50, 50, 100, 60}),
          "shrinking the range restores notes outside the new line");
    mouse(QEvent::MouseMove, point(0.5, 40));
    check(matches({40, 100, 50, 110, 80, 100, 60}),
          "reversing left restores the previous span and draws in the other direction");
    mouse(QEvent::MouseMove, point(3, 20));
    mouse(QEvent::MouseButtonRelease, point(3, 40));
    const auto downward = notes();
    check(matches({90, 100, 85, 70, 70, 40, 60}) &&
          controller.undoDepth() == depth + 1 && !view.hasActivePointerGesture() &&
          view.m_laneOrig.empty(),
          "release applies its exact endpoint and commits one undo for the whole gesture");
    controller.undo();
    check(notes() == original, "undo restores all original velocities");
    controller.redo();
    check(notes() == downward, "redo restores the complete ramp");
    controller.undo();

    mouse(QEvent::MouseButtonPress, point(1, 20));
    mouse(QEvent::MouseButtonRelease, point(3, 100));
    check(matches({90, 20, 40, 60, 60, 100, 60}) &&
          controller.undoDepth() == depth + 1,
          "an upward ramp also works when the final move is coalesced into release");
    controller.undo();
    mouse(QEvent::MouseButtonPress, point(1, 127));
    mouse(QEvent::MouseMove, QPointF(view.beatsToX(3), view.laneTop() - 500));
    check(matches({90, 127, 127, 127, 127, 127, 60}),
          "dragging above the lane clamps velocity at 127");
    mouse(QEvent::MouseButtonRelease, QPointF(view.beatsToX(3), view.height() + 100));
    check(matches({90, 127, 96, 64, 64, 1, 60}),
          "dragging below the lane clamps at the existing minimum velocity of 1");
    controller.undo();

    mouse(QEvent::MouseButtonPress, point(1, 100));
    mouse(QEvent::MouseMove, point(2, 40));
    QEvent lostGrab(QEvent::UngrabMouse);
    QApplication::sendEvent(&view, &lostGrab);
    check(matches({90, 100, 70, 40, 40, 100, 60}) &&
          !view.hasActivePointerGesture() && controller.undoDepth() == depth + 1,
          "losing the mouse grab safely commits the last delivered endpoint");
    controller.undo();
    mouse(QEvent::MouseButtonPress, point(1, 100));
    mouse(QEvent::MouseMove, point(2, 40));
    view.setLaneParam(LaneParam::Pan);
    check(matches({90, 100, 70, 40, 40, 100, 60}) &&
          !view.hasActivePointerGesture() && controller.undoDepth() == depth + 1,
          "switching to pan ends the velocity gesture without changing pan");
    const auto panDepth = controller.undoDepth();
    mouse(QEvent::MouseButtonPress, point(1, 20));
    mouse(QEvent::MouseButtonRelease, point(3, 100));
    check(matches({90, 100, 70, 40, 40, 100, 60}) &&
          controller.undoDepth() == panDepth,
          "right-click behavior in the pan lane remains unchanged");
    view.setLaneParam(LaneParam::Velocity);
    controller.undo();

    mouse(QEvent::MouseButtonPress, point(5, 40));
    mouse(QEvent::MouseButtonRelease, point(6, 80));
    check(notes() == original && controller.undoDepth() == depth,
          "an empty span does not change notes or add history");
    const auto erasePoint = view.noteRect(original.front()).center();
    mouse(QEvent::MouseButtonPress, erasePoint);
    mouse(QEvent::MouseButtonRelease, erasePoint);
    auto erased = original;
    erased.erase(erased.begin());
    check(notes() == erased && controller.undoDepth() == depth + 1,
          "right-drag in the note grid still erases notes");
    controller.undo();
    view.m_selected = {QString::fromStdString(original[1].id),
                       QString::fromStdString(original[5].id)};
    view.m_primary = QString::fromStdString(original[1].id);
    mouse(QEvent::MouseButtonPress, point(1, 70), Qt::LeftButton);
    mouse(QEvent::MouseMove, point(1, 80), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, point(1, 80), Qt::LeftButton);
    check(matches({90, 80, 50, 110, 80, 110, 60}) &&
          controller.undoDepth() == depth + 1,
          "left-drag still adjusts the selected group by a relative velocity offset");
    return ok;
}
