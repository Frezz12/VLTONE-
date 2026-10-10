#include "UiPerformanceChecks.hpp"
#include "UiPerformance.hpp"
#include "MainWindow.hpp"
#include "InternalEditorFrame.hpp"
#include "PluginEditorWindow.hpp"
#include "NotificationCenter.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "graphics/SceneRecorder.hpp"
#include "TrackListWidget.hpp"
#include "MixerWidget.hpp"
#include "ChannelStrip.hpp"
#include "TimelineWidget.hpp"
#include "PianoRollWindow.hpp"
#include "Controls.hpp"
#include "CreatorStyle.hpp"
#include "ToolPanel.hpp"
#include "UiFrameClock.hpp"
#include "UiConstants.hpp"
#include "Theme.hpp"
#include "WaveformPaint.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include <QQuickWindow>
#include <QVBoxLayout>
#include "Job/AudioWorkerRegistration.hpp"
#include <QTemporaryDir>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMenu>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QThread>
#include "platform/AudioFileDecoder.hpp"
#include <atomic>
#include <thread>
#include <ctime>
#include <limits>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#endif
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include "EngineController.hpp"
#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointer>
#include <QPointingDevice>
#include <QPaintEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

bool TimelineWidget::checkInterruptedPointerGestureForTest() {
    daw::EngineController controller{};
    controller.initialize(48000.0, 512, false);
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.emplace_back();
    project.tracks.back().id = "pointer-test-track";
    project.invalidateTrackIndex();
    TimelineWidget timeline(&controller);
    timeline.resize(400, 180);
    timeline.m_snapEnabled = false;
    const auto mouse = [&](QEvent::Type type, QPointF pos,
                           Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, pos, timeline.mapToGlobal(pos),
                          button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&timeline, &event);
    };
    const auto press = [&](QPointF pos) {
        mouse(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    };
    const auto drag = [&](QPointF pos) {
        mouse(QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton);
    };
    const auto hover = [&](QPointF pos) {
        mouse(QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
    };

    press({80, 22}); drag({160, 22});
    const double stoppedAt = controller.positionSeconds();
    hover({300, 22}); hover({350, 22});
    const bool scrubStopped = !timeline.m_scrubbing &&
        std::abs(controller.positionSeconds() - stoppedAt) < 1e-9;

    press({90, 22}); drag({190, 22});
    const double ungrabAt = controller.positionSeconds();
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&timeline, &ungrab);
    hover({330, 22});
    const bool ungrabStopped = !timeline.m_scrubbing &&
        std::abs(controller.positionSeconds() - ungrabAt) < 1e-9;

    timeline.setTool(Tool::Select);
    press({80, 55}); drag({180, 80});
    hover({320, 100});
    const bool marqueeStopped = !timeline.m_marqueeActive &&
        timeline.m_marqueeCurrent == QPoint(180, 80);

    timeline.setTool(Tool::SelectRegion);
    press({80, 55}); drag({200, 55});
    QCoreApplication::sendEvent(&timeline, &ungrab);
    const bool regionStopped = !timeline.m_regionPicking &&
        timeline.m_regionActive && timeline.m_regionEnd > timeline.m_regionStart;

    return scrubStopped && ungrabStopped && marqueeStopped && regionStopped;
}

bool TimelineWidget::checkGridAppearanceForTest() {
    const double savedWidth = ui::gridLineWidth();
    const int savedOpacity = ui::gridOpacity();
    const double savedHeadWidth = ui::playheadWidth();
    const double savedScale = m_pixelsPerSecond, savedScroll = m_scrollSeconds;
    const double savedGrid = m_gridBeats;
    m_pixelsPerSecond = 80.0; m_scrollSeconds = 0.0; m_gridBeats = 0.25;
    const double snapped = snap(1.234, true);
    const double snapStep = snapSeconds();
    bool ok = true;
    for (qreal dpr : {1.0, 1.5, 2.0}) {
        const bool beforeScale = ok;
        const auto gridInk = [&] {
            QImage raster(QSize(320 * dpr, 100 * dpr), QImage::Format_ARGB32_Premultiplied);
            raster.setDevicePixelRatio(dpr);
            raster.fill(Qt::transparent);
            { QPainter p(&raster); p.setClipRect(QRect(0, 0, 320, 100)); drawGrid(p); }
            int ink = 0;
            for (int x = 0; x < raster.width(); ++x)
                ink += qAlpha(raster.pixel(x, int(60 * dpr)));
            return ink;
        };
        ui::setGridLineWidth(1.0); ui::setGridOpacity(100);
        const int full = gridInk();
        ui::setGridOpacity(50);
        const int half = gridInk();
        // Includes coincident subdivision/beat/bar positions: layering them
        // twice would make 50% substantially more opaque than half strength.
        ok &= full > 0 && std::abs(half - full * 0.5) < full * 0.02 + 5;
        ui::setGridLineWidth(ui::kGridLineWidthDefault);
        ui::setGridOpacity(ui::kGridOpacityDefault);
        ok &= gridInk() > 0 && gridInk() < full * 0.85;
        ui::setGridLineWidth(2.5); ui::setGridOpacity(100);
        ok &= gridInk() > full * 1.8;
        ui::setGridOpacity(0);
        ok &= gridInk() == 0 && snap(1.234, true) == snapped && snapSeconds() == snapStep;

        QImage cursor(QSize(160 * dpr, 100 * dpr), QImage::Format_ARGB32_Premultiplied);
        cursor.setDevicePixelRatio(dpr);
        cursor.fill(Qt::transparent);
        ui::setPlayheadWidth(ui::kPlayheadWidthDefault);
        constexpr double x = 80.25;
        { QPainter p(&cursor); drawPlayhead(p, x, 0.0); }
        int cursorInk = 0;
        const int left = int(std::floor((x - ui::playheadWidth() / 2) * dpr));
        const int right = int(std::ceil((x + ui::playheadWidth() / 2) * dpr));
        for (int col = 0; col < cursor.width(); ++col) {
            const int alpha = qAlpha(cursor.pixel(col, int(60 * dpr)));
            cursorInk += alpha;
            if (col < left || col >= right) ok &= alpha == 0;
        }
        ok &= cursorInk > 0;
        if (beforeScale && !ok)
            std::fprintf(stderr, "grid/cursor raster failed at DPR %.1f (full=%d, half=%d, cursor=%d)\n",
                         double(dpr), full, half, cursorInk);
    }
    // Compare the actual selected and unselected lane rasters, including the
    // translucent sub-beat grid. A correct stacking order alone used to pass
    // while selection made those lines nearly disappear into the lane wash.
    {
        const Theme savedTheme = th();
        const auto savedTint = ui::selectionTint();
        daw::EngineController controller{};
        controller.initialize(48000.0, 512, false);
        auto& project = const_cast<daw::ProjectModel&>(controller.project());
        project.tempo = 120.0;
        project.tracks.clear();
        for (quint32 color : {0xe66157u, 0x91bf54u, 0xdaa24du, 0x529ddfu}) {
            daw::TrackModel track;
            track.id = "grid-selection-" + std::to_string(project.tracks.size());
            track.color = color;
            project.tracks.push_back(track);
        }
        project.invalidateTrackIndex();
        TimelineWidget timeline(&controller);
        timeline.resize(320, 400);
        timeline.m_pixelsPerSecond = 80.0;
        timeline.m_gridBeats = 0.25;
        timeline.m_scrollY = 17;
        ui::setGridLineWidth(ui::kGridLineWidthDefault);
        for (const QString& themeId : {QStringLiteral("dark"), QStringLiteral("light")}) {
            ThemeManager::instance().setThemeId(themeId, false);
            const Theme preset = th();
            for (bool coloredGrid : {false, true}) {
                Theme palette = preset;
                if (coloredGrid) {
                    palette.gridLine = preset.dark ? QColor(64, 45, 54) : QColor(203, 211, 230);
                    palette.gridLineStrong = preset.dark ? QColor(98, 65, 80) : QColor(163, 178, 207);
                }
                ThemeManager::instance().applyCustomTheme(palette, false);
                for (auto tint : {ui::SelectionTint::TrackColour, ui::SelectionTint::Neutral}) {
                    ui::setSelectionTint(tint);
                    for (qreal dpr : {1.0, 1.5, 2.0}) {
                        const auto render = [&](bool selected, int opacity) {
                            timeline.m_selectedTrackId = selected ? QStringLiteral("grid-selection-0") : QString();
                            timeline.m_selectedTrackIds = selected
                                ? QStringList{QStringLiteral("grid-selection-1"), QStringLiteral("grid-selection-2")}
                                : QStringList{};
                            ui::setGridOpacity(opacity);
                            QImage raster(QSize(320 * dpr, 400 * dpr), QImage::Format_ARGB32_Premultiplied);
                            raster.setDevicePixelRatio(dpr);
                            raster.fill(palette.background);
                            QPainter p(&raster);
                            p.setClipRect(QRect(0, timeline.tracksTop(), 320, 400 - timeline.tracksTop()));
                            timeline.drawLanes(p);
                            return raster;
                        };
                        const QImage idle = render(false, 75), idleBase = render(false, 0);
                        const QImage selected = render(true, 75), selectedBase = render(true, 0);
                        int largestLoss = 0;
                        for (int lane = 0; lane < 4; ++lane) {
                            const int y = int((timeline.laneTop(lane) + timeline.laneHeightAt(lane) / 2) * dpr);
                            for (int x = 0; x < idle.width(); ++x) {
                                const QColor a = idle.pixelColor(x, y), aBase = idleBase.pixelColor(x, y);
                                const QColor b = selected.pixelColor(x, y), bBase = selectedBase.pixelColor(x, y);
                                for (auto component : {&QColor::red, &QColor::green, &QColor::blue}) {
                                    const int before = (a.*component)() - (aBase.*component)();
                                    const int after = (b.*component)() - (bBase.*component)();
                                    largestLoss = std::max(largestLoss, std::abs(before - after));
                                }
                                if (lane == 3) ok &= a == b; // no spill into the unselected neighbour
                            }
                        }
                        ok &= largestLoss <= 2; // integer blending / antialiasing rounding only
                        if (largestLoss > 2)
                            std::fprintf(stderr, "selected grid contrast changed: %s colored=%d tint=%d DPR=%.1f delta=%d\n",
                                         qPrintable(themeId), int(coloredGrid), int(tint), double(dpr), largestLoss);
                    }
                }
            }
        }
        ui::setSelectionTint(savedTint);
        if (savedTheme.id == QLatin1String("custom")) ThemeManager::instance().applyCustomTheme(savedTheme, false);
        else ThemeManager::instance().setThemeId(savedTheme.id, false);
    }
    m_staticDirty = {};
    m_staticFrameValid = true;
    // Deliver the same notification as a palette change, without replacing
    // the theme or resetting the whole application's style in this probe.
    QMetaObject::invokeMethod(&ThemeManager::instance(), "changed", Qt::DirectConnection);
    ok &= m_staticDirty.intersected(rect()) == QRegion(rect());
    ui::setGridLineWidth(savedWidth); ui::setGridOpacity(savedOpacity);
    ui::setPlayheadWidth(savedHeadWidth);
    m_pixelsPerSecond = savedScale; m_scrollSeconds = savedScroll; m_gridBeats = savedGrid;
    m_staticFrameValid = false;
    update();
    if (!ok) std::fprintf(stderr, "grid appearance, hidden-grid snapping or crisp cursor check failed\n");
    return ok;
}

bool TimelineWidget::checkAdaptiveGridForTest() {
    auto& project = const_cast<daw::ProjectModel&>(m_controller->project());
    const double savedTempo = project.tempo;
    const int savedNumerator = project.timeSigNumerator;
    const int savedDenominator = project.timeSigDenominator;
    const double savedScale = m_pixelsPerSecond, savedScroll = m_scrollSeconds;
    const double savedGrid = m_gridBeats;
    const bool savedSnap = m_snapEnabled;
    const auto savedRuler = m_rulerFormat;
    bool ok = true;
    m_gridBeats = -1.0; m_snapEnabled = true;
    setRulerFormat(ui::RulerFormat::Bars);
    for (double tempo : {120.0, 240.0, 300.0}) {
        project.tempo = tempo;
        for (const auto meter : {std::pair{4, 4}, std::pair{3, 4}, std::pair{6, 8}, std::pair{7, 8}}) {
            project.timeSigNumerator = meter.first;
            project.timeSigDenominator = meter.second;
            const double barBeats = meter.first * 4.0 / meter.second;
            for (double scale : {4.0, 8.0, 80.0, 320.0}) {
                m_pixelsPerSecond = scale;
                const double barPixels = barBeats * 60.0 / tempo * scale;
                const double step = effectiveGridBeats();
                const double stepSeconds = step * 60.0 / tempo;
                if (barPixels >= 32.0) continue;
                const double bars = step / barBeats;
                const double stepPixels = stepSeconds * scale;
                ok &= bars >= 2 && std::abs(std::log2(bars) - std::round(std::log2(bars))) < 1e-9;
                ok &= stepPixels >= 32.0 && stepPixels < 64.0;
                ok &= std::abs(snapSeconds() - stepSeconds) < 1e-9;
                ok &= std::abs(snap(2.7 * stepSeconds, true) - 3.0 * stepSeconds) < 1e-9;

                // Inspect actual grid pixels after a fractional viewport pan.
                // Beat/bar overlays must not secretly reintroduce dense lines.
                m_scrollSeconds = stepSeconds * 0.375;
                QImage raster(width(), tracksTop() + 5, QImage::Format_ARGB32_Premultiplied);
                raster.fill(Qt::transparent);
                { QPainter p(&raster); p.setClipRect(raster.rect()); drawGrid(p); }
                int lastLine = -1, lines = 0;
                bool inside = false;
                for (int x = 0; x < raster.width(); ++x) {
                    const bool ink = qAlpha(raster.pixel(x, tracksTop() + 2)) > 0;
                    if (ink && !inside) {
                        if (lastLine >= 0) ok &= x - lastLine >= stepPixels - 1.01;
                        // Snapping and rendered lines share the same bar-1 origin.
                        const double nearest = snap(xToSeconds(x), true);
                        ok &= std::abs((nearest - m_scrollSeconds) * scale - x) <= 1.01;
                        lastLine = x; ++lines;
                    }
                    inside = ink;
                }
                ok &= lines >= int(width() / stepPixels) - 1 && lines <= int(width() / stepPixels) + 1;
            }
        }
    }
    project.tempo = 120.0; project.timeSigNumerator = 4; project.timeSigDenominator = 4;
    m_pixelsPerSecond = 4.0; m_scrollSeconds = 0.0;
    ok &= effectiveGridBeats() == 16.0 && gridBarStride() == 4; // 4-bar overview
    const QString screenshot = qEnvironmentVariable("VLT_GRID_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QImage overview(width(), 200, QImage::Format_RGB32);
        overview.fill(th().background);
        { QPainter p(&overview); p.setClipRect(overview.rect()); drawGrid(p); drawRuler(p); }
        ok &= overview.save(screenshot);
    }
    m_pixelsPerSecond = 80.0;
    ok &= effectiveGridBeats() == 0.25; // existing fine-editing step
    m_pixelsPerSecond = 4.0;
    m_gridBeats = 0.25;
    ok &= effectiveGridBeats() == 0.25 && std::abs(snapSeconds() - 0.125) < 1e-9;
    m_gridBeats = 0.0;
    ok &= snapSeconds() == 0.0;
    m_gridBeats = savedGrid; m_pixelsPerSecond = savedScale; m_scrollSeconds = savedScroll;
    m_snapEnabled = savedSnap;
    setRulerFormat(savedRuler);
    project.tempo = savedTempo; project.timeSigNumerator = savedNumerator;
    project.timeSigDenominator = savedDenominator;
    if (!ok) std::fprintf(stderr, "adaptive grid spacing, raster alignment or snap check failed\n");
    return ok;
}

bool TimelineWidget::checkBottomInsetInvalidationForTest() {
    const int savedInset = m_bottomInset;
    const bool savedScenePaint = m_lastPaintWasScene;
    const bool savedViewportDirty = m_staticViewportDirty;
    const QRegion savedPlaybackDirty = m_playbackOnlyDirty;
    const QRegion savedRecordingDirty = m_recordingOnlyDirty;
    const bool savedBackgroundRepaint = m_backgroundFrameRepaint;

    // Reproduce the retained-scene ordering behind the intermittent black
    // reveal: the mixer changes its clip, then a playhead frame arrives before
    // the next scene capture. Clip invalidation must survive that later frame,
    // without requiring unchanged lane geometry to be discarded.
    m_lastPaintWasScene = true;
    m_staticDirty = {};
    setBottomInset(savedInset == 0 ? 32 : 0);
    m_playbackOnlyDirty += rect();
    const bool invalidated = m_staticViewportDirty;

    m_bottomInset = savedInset;
    m_lastPaintWasScene = savedScenePaint;
    m_staticViewportDirty = savedViewportDirty;
    m_playbackOnlyDirty = savedPlaybackDirty;
    m_recordingOnlyDirty = savedRecordingDirty;
    m_backgroundFrameRepaint = savedBackgroundRepaint;
    layoutNavigationControls();
    clampVerticalScroll();
    update(rect());
    if (!invalidated)
        std::fprintf(stderr,
                     "mixer reveal did not invalidate the retained timeline\n");
    return invalidated;
}

bool TimelineWidget::checkGestureGridStabilityForTest() {
    const bool savedGesture = m_projectGestureActive;
    const quint64 buildsBefore = m_gpuLaneTiles.builds();
    m_projectGestureActive = true;
    ui::graphics::SceneRecorder recorder(size(), 1.0);
    {
        QPainter painter(&recorder);
        drawStaticFrame(painter, QRegion(rect()));
    }
    m_projectGestureActive = savedGesture;
    const bool tiled = recorder.supported() &&
                       m_gpuLaneTiles.builds() > buildsBefore;
    m_gpuLaneTiles.clear();
    if (!tiled)
        std::fprintf(stderr,
                     "clip gesture switched away from the stable grid tiles\n");
    return tiled;
}

bool TimelineWidget::checkFileDropPreviewForTest() {
    const auto chooseImport = [](int index = 0) {
        QTimer::singleShot(0, qApp, [index] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                menu->setActiveAction(menu->actions().at(index));
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(menu, &enter);
            }
        });
    };
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false)) return false;
    const std::string track = controller.addTrack(daw::TrackKind::Audio, "Drop preview");
    TimelineWidget timeline(&controller);
    timeline.resize(1100, 450);
    timeline.m_pixelsPerSecond = 100;
    timeline.m_scrollSeconds = 1;
    timeline.m_snapEnabled = true;
    QTemporaryDir fixture;
    const QString path = fixture.filePath("sample.wav");
    audio::platform::AudioFileWriter writer;
    std::vector<float> samples(72000);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = float(0.6 * std::sin(i * 0.04) * std::exp(-double(i) / 24000));
    const float* channel = samples.data();
    if (!writer.open(path.toStdString(), 48000, 1) ||
        !writer.write(&channel, samples.size()) || !writer.close()) return false;
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    const QPoint position(233, timeline.laneTop(0) + 15);
    QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &enter);
    if (!enter.isAccepted() || timeline.m_dropFile != path) return false;
    QElapsedTimer wait; wait.start();
    while (!timeline.m_dropPeaks && wait.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    if (!timeline.m_dropPeaks || timeline.m_dropDuration <= 0) return false;
    const QRectF snapped = timeline.fileDropRect();
    if (std::abs(snapped.width() - timeline.m_dropDuration * 100) > 0.01) return false;
    if (std::abs(snapped.left() - timeline.secondsToX(
            timeline.fileDropStart(position, Qt::NoModifier))) > 0.01) return false;
    const auto render = [&] {
        QImage image(timeline.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        timeline.drawStaticFrame(painter, QRegion(timeline.rect()));
        return image;
    };
    const QImage preview = render();
    timeline.m_dropActive = false;
    const QImage baseline = render();
    timeline.m_dropActive = true;
    const QPoint untouched(850, timeline.laneTop(0) + 30);
    if (preview.pixel(untouched) != baseline.pixel(untouched) ||
        preview.pixel(int(snapped.left()), untouched.y()) == baseline.pixel(int(snapped.left()), untouched.y()))
        return false;
    if (const QString shot = qEnvironmentVariable("VLT_FILE_DROP_SHOT"); !shot.isEmpty())
        if (!preview.save(shot)) return false;
    QDragMoveEvent move(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(&timeline, &move);
    const QRectF free = timeline.fileDropRect();
    if (std::abs(free.left() - position.x()) > 0.01 || free.top() != snapped.top()) return false;
    // Reverse direction and move onto empty space; both updates are immediate.
    const QPoint empty(131, timeline.lanesBottom() + 12);
    QDragMoveEvent reverse(empty, Qt::CopyAction, &mime, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(&timeline, &reverse);
    if (timeline.fileDropRect().left() != empty.x() ||
        timeline.fileDropRect().top() != timeline.lanesBottom() + 2) return false;
    QDragLeaveEvent leave;
    QApplication::sendEvent(&timeline, &leave);
    if (timeline.m_dropActive || !timeline.m_dropFile.isEmpty() || timeline.m_dropPeaks) return false;
    QDragEnterEvent reenter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(&timeline, &reenter);
    const double expected = timeline.fileDropStart(position, Qt::AltModifier);
    QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::AltModifier);
    chooseImport();
    QApplication::sendEvent(&timeline, &drop);
    const auto* landed = controller.project().findTrack(track);
    if (!(drop.isAccepted() && !timeline.m_dropActive && timeline.m_dropFile.isEmpty() &&
           landed && landed->clips.size() == 1 &&
           std::abs(landed->clips.front().startSeconds - expected) < 1e-9)) return false;

    // A browser sample dropped on a selected clip replaces only the selected
    // audio clips on that lane, even when the selection spans other tracks.
    const std::string first = landed->clips.front().id;
    const double originalDuration = landed->clips.front().durationSeconds;
    const std::string second = controller.importAudio(path.toStdString(), track, 6.1);
    const std::string untouchedClip = controller.importAudio(path.toStdString(), track, 9.2);
    const std::string otherTrack = controller.addTrack(daw::TrackKind::Audio, "Other lane");
    const std::string otherClip = controller.importAudio(path.toStdString(), otherTrack, 3.0);
    timeline.selectClips({{QString::fromStdString(track), QString::fromStdString(first)},
                          {QString::fromStdString(track), QString::fromStdString(second)},
                          {QString::fromStdString(otherTrack), QString::fromStdString(otherClip)}});
    controller.setClipGain(track, first, 0.4f);
    const auto originalSelectionImage = render();
    const QString replacement = fixture.filePath("new-kick.wav");
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = float(0.8 * std::cos(i * 0.1) * std::exp(-double(i) / 2000));
    if (!writer.open(replacement.toStdString(), 48000, 1) ||
        !writer.write(&channel, 12000) || !writer.close()) return false;
    // Avoid a modal preparation dialog in this synthetic drop check.
    daw::EngineController::PreparedAudio prepared;
    if (!daw::EngineController::prepareAudio(replacement.toStdString(), 48000, prepared) ||
        !controller.adoptPreparedAudio(std::move(prepared))) return false;
    QMimeData replacementMime;
    replacementMime.setUrls({QUrl::fromLocalFile(replacement)});
    const QPoint target(timeline.secondsToX(expected + 0.1), timeline.laneTop(0) + 15);
    QDragEnterEvent replaceEnter(target, Qt::CopyAction, &replacementMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &replaceEnter);
    if (!replaceEnter.isAccepted() || !timeline.m_dropSingleAudio ||
        timeline.audioReplacementTargets(target).size() != 2) return false;
    if (timeline.m_dropReplacementClips.size() != 2 ||
        timeline.m_dropReplacementClips.at(second).filePath != replacement.toStdString() ||
        timeline.m_dropReplacementClips.at(second).name != "new-kick" ||
        std::abs(timeline.m_dropReplacementClips.at(second).durationSeconds - 0.25) > 1e-9 ||
        controller.audioClip(track, second)->filePath != path.toStdString()) return false;
    const auto replacementPreview = render();
    // The non-hovered selected copy shows new media too. Compare inside its
    // waveform, away from both the dashed outline and the replacement badge.
    const QRect secondWave(timeline.secondsToX(6.1) + 3, timeline.laneTop(0) + 22,
                           18, 12);
    const QRect otherLane(0, timeline.laneTop(1), timeline.width(), timeline.laneBodyHeightAt(1));
    if (originalSelectionImage.copy(secondWave) == replacementPreview.copy(secondWave) ||
        originalSelectionImage.copy(otherLane) != replacementPreview.copy(otherLane)) return false;
    if (const QString shot = qEnvironmentVariable("VLT_FILE_REPLACE_SHOT"); !shot.isEmpty())
        if (!replacementPreview.save(shot)) return false;
    const QPoint gap(timeline.secondsToX(5.5), target.y());
    QDragMoveEvent intoGap(gap, Qt::CopyAction, &replacementMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &intoGap);
    if (!timeline.audioReplacementTargets(gap).empty() || !timeline.m_dropReplacementClips.empty()) return false;
    const auto movedAway = render();
    if (originalSelectionImage.copy(secondWave) != movedAway.copy(secondWave)) return false;
    QDragLeaveEvent replaceLeave;
    QApplication::sendEvent(&timeline, &replaceLeave);
    if (render() != originalSelectionImage || !timeline.m_dropReplacementClips.empty()) return false;
    QDragEnterEvent replaceReenter(target, Qt::CopyAction, &replacementMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &replaceReenter);
    QDragMoveEvent back(target, Qt::CopyAction, &replacementMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &back);
    const auto undoDepth = controller.undoDepth();
    QDropEvent replaceDrop(target, Qt::CopyAction, &replacementMime, Qt::LeftButton, Qt::NoModifier);
    chooseImport();
    QApplication::sendEvent(&timeline, &replaceDrop);
    const auto* replaced = controller.audioClip(track, first);
    if (!replaceDrop.isAccepted() || !replaced || replaced->filePath != replacement.toStdString() ||
        replaced->name != "new-kick" || replaced->startSeconds != expected ||
        std::abs(replaced->durationSeconds - 0.25) > 1e-9 || replaced->gain != 0.4f ||
        controller.audioClip(track, second)->filePath != replacement.toStdString() ||
        controller.audioClip(track, second)->startSeconds != 6.1 ||
        controller.audioClip(track, untouchedClip)->filePath != path.toStdString() ||
        controller.audioClip(otherTrack, otherClip)->filePath != path.toStdString() ||
        controller.project().findTrack(track)->clips.size() != 3 ||
        timeline.m_selection.size() != 3 || controller.undoDepth() != undoDepth + 1) return false;
    controller.undo();
    if (controller.audioClip(track, first)->filePath != path.toStdString() ||
        controller.audioClip(track, second)->filePath != path.toStdString() ||
        std::abs(controller.audioClip(track, first)->durationSeconds - originalDuration) > 1e-9) return false;
    controller.redo();
    if (controller.audioClip(track, second)->filePath != replacement.toStdString()) return false;
    const auto depth = controller.undoDepth();
    if (controller.replaceAudioClips(track, {first, second}, fixture.filePath("missing.wav").toStdString()) ||
        controller.replaceAudioClips(track, {first, otherClip}, path.toStdString()) ||
        controller.undoDepth() != depth ||
        controller.audioClip(track, first)->filePath != replacement.toStdString()) return false;
    // Unselected clips and multi-file drags keep ordinary insertion semantics.
    const QPoint unselected(timeline.secondsToX(9.3), target.y());
    if (!timeline.audioReplacementTargets(unselected).empty()) return false;
    QMimeData multiple;
    multiple.setUrls({QUrl::fromLocalFile(path), QUrl::fromLocalFile(replacement)});
    QDragEnterEvent multiEnter(target, Qt::CopyAction, &multiple, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &multiEnter);
    if (timeline.m_dropSingleAudio) return false;
    QDragLeaveEvent cancel;
    QApplication::sendEvent(&timeline, &cancel);
    QDragEnterEvent samplerEnter(gap, Qt::CopyAction, &replacementMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &samplerEnter);
    const auto beforeSampler = controller.undoDepth();
    const auto beforeTracks = controller.project().tracks.size();
    const double samplerStart = timeline.fileDropStart(gap, Qt::NoModifier);
    QDropEvent samplerDrop(gap, Qt::CopyAction, &replacementMime, Qt::LeftButton, Qt::NoModifier);
    chooseImport(1);
    QApplication::sendEvent(&timeline, &samplerDrop);
    if (!samplerDrop.isAccepted() || controller.project().tracks.size() != beforeTracks + 1 ||
        controller.undoDepth() != beforeSampler + 1) return false;
    const auto& sampled = controller.project().tracks.back();
    if (sampled.instrument.uid != "daw.sampler" || sampled.clips.size() != 1 ||
        sampled.clips.front().kind != daw::ClipKind::Midi ||
        sampled.clips.front().startSeconds != samplerStart ||
        sampled.clips.front().notes.size() != 1) return false;
    controller.undo();
    if (controller.project().tracks.size() != beforeTracks) return false;
    return !timeline.m_dropActive && !timeline.m_dropSingleAudio && timeline.m_dropFile.isEmpty();
}

bool TimelineWidget::checkClipTrimPreviewForTest() {
    daw::EngineController controller{};
    if (!controller.initialize(48000.0, 512, false)) return false;
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear();
    const daw::ClipKind kinds[] = {daw::ClipKind::Audio, daw::ClipKind::Midi,
                                   daw::ClipKind::Pattern, daw::ClipKind::Automation};
    const daw::TrackKind tracks[] = {daw::TrackKind::Audio, daw::TrackKind::Midi,
                                     daw::TrackKind::Pattern, daw::TrackKind::Automation};
    std::vector<std::pair<std::string, std::string>> addresses;
    for (int i = 0; i < 4; ++i) {
        daw::TrackModel track;
        track.id = "trim-preview-" + std::to_string(i);
        track.kind = tracks[i];
        daw::ClipModel clip;
        clip.id = track.id + "-clip";
        clip.kind = kinds[i];
        clip.startSeconds = 2.0;
        clip.offsetSeconds = 12.0;
        clip.durationSeconds = 2.0;
        addresses.emplace_back(track.id, clip.id);
        track.clips.push_back(std::move(clip));
        project.tracks.push_back(std::move(track));
    }
    project.invalidateTrackIndex();
    TimelineWidget timeline(&controller);
    timeline.resize(1600, 600);
    timeline.m_pixelsPerSecond = 80.0;
    timeline.m_scrollSeconds = 0.0;
    const auto render = [&](bool tiled) {
        QImage image(timeline.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        if (tiled) {
            // Match the GPU's independently culled 512px lane tiles.
            for (int x = 0; x < timeline.width(); x += 512)
                timeline.drawStaticFrame(painter, QRegion(QRect(x, 0, 512, timeline.height())));
        } else timeline.drawStaticFrame(painter, QRegion(timeline.rect()));
        return image;
    };
    (void)render(true); // Warm the interval cache at the original clip length.
    const bool savedRounded = ui::roundedClipCorners();
    ui::setRoundedClipCorners(true);
    const QImage rounded = render(false);
    ui::setRoundedClipCorners(false);
    const QImage square = render(false);
    ui::setRoundedClipCorners(savedRounded);
    bool ok = true;
    for (int lane = 0; lane < 4; ++lane) {
        const QRect corner = timeline.clipRect(lane, project.tracks[lane].clips.front())
                                 .toAlignedRect();
        bool changed = false;
        for (int y = corner.bottom() - 4; y <= corner.bottom(); ++y)
            for (int x = corner.left(); x < corner.left() + 5; ++x)
                changed |= rounded.pixel(x, y) != square.pixel(x, y);
        if (!changed) {
            std::fprintf(stderr, "clip corner preference did not redraw kind=%d\n", int(kinds[lane]));
            ok = false;
        }
    }
    if (const QString path = qEnvironmentVariable("DAW_CLIP_CORNERS_SCREENSHOT"); !path.isEmpty()) {
        rounded.save(path + "-rounded.png");
        square.save(path + "-square.png");
    }
    controller.beginClipTrimEdit(addresses);
    // Extend across multiple tile boundaries, reverse, and move the left edge
    // in both directions, all before committing the group gesture.
    for (const auto [start, duration] : {std::pair{2., 14.}, {2., 2.}, {8., 8.},
                                        {1., 15.}, {6., 10.}, {2., 2.}}) {
        for (const auto& [track, clip] : addresses)
            controller.setClipTrim(track, clip, start, 12. + start - 2., duration);
        const QImage tiled = render(true);
        const QImage whole = render(false);
        for (int lane = 0; lane < 4; ++lane) {
            bool visible = true;
            const int y = timeline.laneTop(lane) + 32;
            for (int x = 16; x < timeline.width(); x += 32)
                visible &= tiled.pixel(x, y) == whole.pixel(x, y);
            if (!visible) {
                std::fprintf(stderr, "live trim preview clipped: kind=%d start=%.1f duration=%.1f\n",
                             int(kinds[lane]), start, duration);
                ok = false;
            }
        }
        ok = timeline.checkClipIndexForTest() && ok;
    }
    controller.endClipTrimEdit();
    // Exercise pointer hit testing and the actual gesture, including dispatch
    // through the GPU workspace. Controller-only trim tests missed the MIDI
    // head being treated as a move, and an audio-only stretch lookup returned
    // zero for MIDI, magnifying its source offset by 100.
    daw::EngineController gestures{};
    if (!gestures.initialize(48000, 512, false)) return false;
    auto& document = const_cast<daw::ProjectModel&>(gestures.project());
    document.tracks.clear(); document.invalidateTrackIndex();
    std::vector<std::pair<std::string, std::string>> gestureClips;
    for (int i = 0; i < 4; ++i) {
        const auto id = gestures.addTrack(tracks[i], "Trim edge fixture");
        daw::ClipModel clip;
        clip.id = id + "-edge"; clip.kind = kinds[i];
        clip.startSeconds = 2; clip.durationSeconds = 2;
        if (clip.kind == daw::ClipKind::Midi) {
            for (int n = 0; n < 3; ++n) {
                daw::NoteModel note;
                note.id = clip.id + std::to_string(n); note.pitch = 60 + n * 2;
                note.startBeats = n == 2 ? 3 : n; note.lengthBeats = n == 1 ? 2.5 : .5;
                clip.notes.push_back(note);
            }
        }
        gestureClips.emplace_back(id, clip.id);
        document.findTrack(id)->clips.push_back(std::move(clip));
    }
    document.invalidateStructure();
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    TimelineWidget edges(&gestures, &host);
    layout->addWidget(&edges);
    edges.m_pixelsPerSecond = 80; edges.m_scrollSeconds = 0;
    edges.m_snapEnabled = false; edges.setTool(Tool::Select);
    const auto settle = [](int ms) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    host.resize(800, 600); host.show(); settle(80);
    std::unique_ptr<ui::graphics::WorkspaceSurface> surface;
    if (ui::graphics::gpuWorkspaceEnabled()) {
        surface = std::make_unique<ui::graphics::WorkspaceSurface>(&host);
        QObject::connect(surface.get(), &ui::graphics::WorkspaceSurface::failed, &host,
            [&](const QString& reason) {
                std::fprintf(stderr, "GPU trim dispatch failed: %s\n", qPrintable(reason)); ok = false;
            });
        settle(220);
        ok &= surface->quickWindow()->isExposed();
    }
    const auto pointer = [&](QEvent::Type type, QPoint local) {
        const QPointF global = edges.mapToGlobal(local);
        const QPointF position = surface ? surface->quickWindow()->mapFromGlobal(global) : QPointF(local);
        QMouseEvent event(type, position, global,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface ? static_cast<QObject*>(surface->quickWindow()) : &edges, &event);
        settle(10);
    };
    for (int lane = 0; lane < 4; ++lane) {
        const auto& [track, id] = gestureClips[lane];
        const auto clip = [&]() -> const daw::ClipModel& { return document.findTrack(track)->clips.front(); };
        const auto storage = clip().notes.data();
        const auto depth = gestures.undoDepth();
        const int y = edges.laneTop(lane) + edges.laneBodyHeightAt(lane) / 2;
        pointer(QEvent::MouseButtonPress, {160, y});
        const bool grabbed = edges.m_trimming && edges.m_trimEdge == Edge::Left && !edges.m_dragging;
        pointer(QEvent::MouseMove, {240, y});
        const bool moved = clip().startSeconds == 3 && clip().durationSeconds == 1 &&
            clip().offsetSeconds == (lane < 2 ? 1 : 0) && clip().notes.data() == storage;
        pointer(QEvent::MouseMove, {160, y}); // Reverse before release: restore the source.
        const bool reversed = clip().startSeconds == 2 && clip().durationSeconds == 2 && clip().offsetSeconds == 0;
        pointer(QEvent::MouseMove, {240, y});
        pointer(QEvent::MouseButtonRelease, {240, y});
        bool passed = grabbed && moved && reversed && gestures.undoDepth() == depth + 1;
        if (lane == 1) {
            const auto* nodes = gestures.trackNodes(track);
            const auto graph = gestures.routingGraph();
            const daw::engine::MidiClipPlayerNode* player = nullptr;
            for (const auto& node : graph->nodes)
                if (nodes && node.id == nodes->midiClips)
                    player = dynamic_cast<const daw::engine::MidiClipPlayerNode*>(node.node);
            const auto notes = player ? player->notes() : nullptr;
            passed &= notes && notes->size() == 2 && (*notes)[0].startBeats == 6 &&
                      (*notes)[0].lengthBeats == 1.5 && (*notes)[1].startBeats == 7;
            QImage preview(400, 40, QImage::Format_ARGB32_Premultiplied);
            preview.fill(Qt::transparent);
            { QPainter p(&preview); p.setClipRect(preview.rect());
              edges.drawMidiNotes(p, clip(), QRectF(240, 0, 80, 40), QRectF(240, 0, 80, 40),
                                 gestures.midiNotesRevision(track)); }
            bool stayed = false, shifted = false;
            for (int row = 0; row < 40; ++row) {
                stayed |= qAlpha(preview.pixel(290, row)) > 0;
                shifted |= qAlpha(preview.pixel(310, row)) > 0;
            }
            passed &= stayed && !shifted;
        }
        gestures.undo();
        passed &= clip().startSeconds == 2 && clip().offsetSeconds == 0 && clip().durationSeconds == 2;
        gestures.redo();
        passed &= clip().startSeconds == 3 && clip().durationSeconds == 1;
        // Recover the hidden head in a subsequent gesture, then trim the tail.
        pointer(QEvent::MouseButtonPress, {240, y}); pointer(QEvent::MouseMove, {160, y});
        pointer(QEvent::MouseButtonRelease, {160, y});
        passed &= clip().startSeconds == 2 && clip().durationSeconds == 2 && clip().offsetSeconds == 0;
        pointer(QEvent::MouseButtonPress, {320, y});
        passed &= edges.m_trimming && edges.m_trimEdge == Edge::Right;
        pointer(QEvent::MouseMove, {280, y}); pointer(QEvent::MouseButtonRelease, {280, y});
        passed &= clip().startSeconds == 2 && clip().durationSeconds == 1.5 && clip().notes.data() == storage;
        std::printf("%s  clip kind=%d both trim edges, source timing, reversal and history (%s)\n",
                    passed ? "PASS" : "FAIL", int(kinds[lane]), surface ? "gpu" : "widgets");
        ok &= passed;
    }
    return ok;
}

bool TimelineWidget::checkMoveGuidePaintForTest() {
    const bool savedActive = m_moveGuidesActive;
    const double savedStart = m_moveGuideStart;
    const double savedEnd = m_moveGuideEnd;
    const double savedScale = m_pixelsPerSecond;
    const double savedScroll = m_scrollSeconds;

    m_moveGuidesActive = true;
    m_moveGuideStart = 0.25;
    m_moveGuideEnd = 1.25;
    m_pixelsPerSecond = 80.0;
    m_scrollSeconds = 0.0;

    QImage image(160, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        drawMoveGuides(painter);
    }
    const auto opaqueWhite = [&](int x, int y) {
        const QColor pixel = image.pixelColor(x, y);
        return pixel.alpha() == 255 && pixel.red() == 255 &&
               pixel.green() == 255 && pixel.blue() == 255;
    };
    const int insideAlpha = image.pixelColor(60, 40).alpha();
    const QRegion damage = gestureDamage();
    const bool ok = opaqueWhite(20, 1) && opaqueWhite(20, 78) &&
                    opaqueWhite(100, 1) && opaqueWhite(100, 78) &&
                    insideAlpha > 0 && insideAlpha < 255 &&
                    image.pixelColor(120, 40).alpha() == 0 &&
                    damage.contains(QPoint(60, 40));

    m_moveGuidesActive = savedActive;
    m_moveGuideStart = savedStart;
    m_moveGuideEnd = savedEnd;
    m_pixelsPerSecond = savedScale;
    m_scrollSeconds = savedScroll;
    if (!ok)
        std::fprintf(stderr,
                     "clip move origin is not a fixed full-height outlined band\n");
    return ok;
}

namespace ui {
namespace {
double processPageIns() {
#if defined(__APPLE__)
    rusage_info_v0 usage{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V0,
                       reinterpret_cast<rusage_info_t*>(&usage)) == 0)
        return double(usage.ri_pageins);
#endif
    return std::numeric_limits<double>::quiet_NaN();
}
double guiThreadCpuMs() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](FILETIME time) { return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        return double(ticks(kernel) + ticks(user)) / 10000.;
    }
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
        return double(value.tv_sec) * 1000.0 + value.tv_nsec / 1e6;
#endif
    return std::numeric_limits<double>::quiet_NaN();
}
class NavigationTimeline : public TimelineWidget {
public:
    using TimelineWidget::TimelineWidget;
    bool profileAudioScroll = false;
    double renderThreadCpuMs = 0;
    int measuredRenderFrames = 0;
    std::vector<double> audioPaints, audioIntervals;
    QElapsedTimer audioInterval;
    bool profileNavigation = false;
    bool profilePlayback = false;
    int playbackPaints = 0;
    int playbackFullPaints = 0;
    std::uint64_t playbackNarrowStaticPaints = 0;
    std::vector<double> playbackPositions;
    QRegion lastPaintRegion;
    QElapsedTimer playbackPaintTime;
protected:
    void paintEvent(QPaintEvent* event) override {
        perf::Scope timing(profileNavigation ? "timeline.navigation.paint.ms" : nullptr);
        perf::Scope playbackTiming(profilePlayback ? "timeline.playback.paint.ms" : nullptr);
        const double cpuBefore = profileAudioScroll ? guiThreadCpuMs() : 0.0;
        const double pageInsBefore = profileAudioScroll ? processPageIns() : 0.0;
        const auto waveBefore = profileAudioScroll ? waveformPaintStatsForTest() : WaveformPaintStats{};
        QElapsedTimer paintTime;
        if (profileAudioScroll) paintTime.start();
        const auto staticBefore = staticFramePaintCountForTest();
        TimelineWidget::paintEvent(event);
        if (profileAudioScroll) {
            const double elapsed = paintTime.nsecsElapsed() / 1e6;
            audioPaints.push_back(elapsed);
            if (elapsed > 30.0) {
                const auto waveAfter = waveformPaintStatsForTest();
                std::fprintf(stderr, "SLOW_SCROLL paint=%.3f ms cpu=%.3f ms process_pageins=%.0f frame=%zu static=%llu tile_builds=%llu tile_hits=%llu raster_bytes=%zu scroll=%.5f\n",
                    elapsed, guiThreadCpuMs() - cpuBefore, processPageIns() - pageInsBefore, audioPaints.size(),
                    (unsigned long long)(staticFramePaintCountForTest() - staticBefore),
                    (unsigned long long)(waveAfter.tileBuilds - waveBefore.tileBuilds),
                    (unsigned long long)(waveAfter.tileHits - waveBefore.tileHits), waveAfter.bytes,
                    horizontalScrollForTest());
            }
            if (audioInterval.isValid()) audioIntervals.push_back(audioInterval.nsecsElapsed() / 1e6);
            audioInterval.restart();
        }
        lastPaintRegion = event->region();
        if (profilePlayback) {
            if (event->region().boundingRect() == rect()) ++playbackFullPaints;
            else playbackNarrowStaticPaints += staticFramePaintCountForTest() - staticBefore;
            if (playbackPaintTime.isValid())
                perf::sample("timeline.playback.interval.ms", playbackPaintTime.nsecsElapsed() / 1e6);
            playbackPaintTime.restart();
            ++playbackPaints;
            playbackPositions.push_back(displayedPlayheadSubpixelXForTest());
        }
    }
};
}
bool checkMixerPerformance() {
    ThemeManager::instance().apply();
    FrameClock::instance().setPreference(FrameMode::Display, 60);
    const auto settle = [](int ms) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false)) return false;
    // Real mixer controls with deterministic slots; no device or vendor DSP.
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear(); project.invalidateTrackIndex();
    for (int i = 0; i < 12; ++i) {
        daw::TrackModel track;
        track.id = "mixer-perf-" + std::to_string(i);
        track.name = "Channel " + std::to_string(i + 1);
        track.kind = daw::TrackKind::Audio;
        for (int j = 0; j < 8; ++j) {
            daw::InsertModel insert;
            insert.id = track.id + "-insert-" + std::to_string(j);
            insert.name = "Effect " + std::to_string(j + 1);
            track.inserts.push_back(std::move(insert));
        }
        project.tracks.push_back(std::move(track));
    }
    MixerWidget mixer(&controller);
    mixer.resize(1250, 440);
    auto* surface = graphics::gpuWorkspaceEnabled() ? new graphics::WorkspaceSurface(&mixer) : nullptr;
    bool failed = false;
    if (surface) QObject::connect(surface, &graphics::WorkspaceSurface::failed, &mixer,
        [&](const QString& reason) { failed = true; qWarning() << reason; });
    mixer.show();
    mixer.raise();
    mixer.activateWindow();
    if (surface) surface->quickWindow()->requestActivate();
    settle(1800);
    if (surface && !surface->quickWindow()->isExposed()) {
        std::fprintf(stderr, "Mixer GPU window is not exposed; benchmark is invalid\n");
        return false;
    }
    if (surface && surface->quickWindow()->grabWindow().isNull()) {
        std::fprintf(stderr, "Mixer has no GPU frame; benchmark is invalid\n");
        return false;
    }
    QScrollArea* scroll = nullptr;
    for (auto* area : mixer.findChildren<QScrollArea*>())
        if (area->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded) { scroll = area; break; }
    if (!scroll || scroll->verticalScrollBar()->maximum() < 100 || failed) return false;
    int frames = 0;
    if (surface) QObject::connect(surface, &graphics::WorkspaceSurface::frameMeasured, &mixer,
        [&](double preparation, double sync, double render, double interval, double) {
            ++frames;
            perf::sample("mixer.scene.prepare.ms", preparation);
            perf::sample("mixer.scene.sync.ms", sync);
            perf::sample("mixer.scene.render.ms", render);
            if (interval > 0 && frames > 1) perf::sample("mixer.scene.submission.ms", interval);
        });
    perf::reset();
    // Place the pointer in the gutter so the wheel scrolls, never changes a knob.
    const QPointF position = scroll->viewport()->mapTo(&mixer, QPoint(2, 100));
    int inputs = 0, scrollChanges = 0;
    QElapsedTimer elapsed; elapsed.start();
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &mixer, [&] {
        auto* bar = scroll->verticalScrollBar();
        const int before = bar->value();
        const double phase = std::fmod(elapsed.elapsed() / 1600., 1.);
        const int wanted = int((phase < .5 ? phase * 2 : 2 - phase * 2) * bar->maximum());
        QWheelEvent wheel(position, mixer.mapToGlobal(position), QPoint(0, bar->value() - wanted), {},
            Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
        perf::Scope inputCost("mixer.wheel.ms");
        if (surface) QCoreApplication::sendEvent(surface->quickWindow(), &wheel);
        // In compatibility mode QWidgetWindow performs pixel-wheel translation;
        // this harness drives the resulting scrollbar movement directly.
        else bar->setValue(wanted);
        if (bar->value() != before) ++scrollChanges;
        ++inputs;
    });
    timer.start(8);
    settle(1600); // warm one complete down/up cycle before measuring
    const int warmupInputs = inputs;
    const int warmupChanges = scrollChanges;
    frames = 0;
    perf::reset();
    QElapsedTimer measured; measured.start();
    settle(4800);
    timer.stop();
    perf::flush();
    std::printf("MIXER_SCROLL backend=%s viewport=%dx%d DPR=%.1f inputs=%d changes=%d frames=%d elapsed_ms=%lld\n",
        surface ? "gpu" : "widgets", mixer.width(), mixer.height(), mixer.devicePixelRatioF(),
        inputs - warmupInputs, scrollChanges - warmupChanges, frames, static_cast<long long>(measured.elapsed()));
    if (const auto shot = qEnvironmentVariable("VLT_SCROLL_SCREENSHOT"); !shot.isEmpty()) {
        if (surface) surface->quickWindow()->grabWindow().save(shot);
        else mixer.grab().save(shot);
    }
    return !failed && scrollChanges - warmupChanges > 30 && (!surface || frames > 30);
}

bool checkViewScrubInput() {
    bool ok = true;
    const auto check = [&](bool result, const char* name) {
        std::printf("%s  %s\n", result ? "PASS" : "FAIL", name);
        ok = ok && result;
    };
    const QRect bounds = QGuiApplication::primaryScreen()->geometry();
    const QPointF anchor = QPointF(bounds.center()) + QPointF(0.25, 0.75);
    LockedCursorDrag pointer;
    pointer.begin(anchor);
    check(pointer.takeDelta(anchor).isNull() &&
          pointer.takeDelta(anchor + QPointF(0.25, -0.5)) == QPointF(0.25, -0.5) &&
          pointer.takeDelta(anchor + QPointF(0.25, -0.5)).isNull() &&
          pointer.takeDelta(anchor + QPointF(0.5, -0.25)) == QPointF(0.25, 0.25),
          "relative scrub preserves fractions, duplicate samples and reversal");
    pointer.cancel();
    check(pointer.takeDelta(anchor + QPointF(30, 30)).isNull(),
          "cancelled pointer cannot apply late input");
    for (const bool vertical : {false, true}) {
        const QPointF edge = vertical ? QPointF(anchor.x(), bounds.top() + 1)
                                     : QPointF(bounds.right() - 1, anchor.y());
        const QPointF step = vertical ? QPointF(0, -4) : QPointF(4, 0);
        pointer.begin(edge - step);
        check(pointer.takeDelta(edge) == step, "screen-edge travel is counted once");
        const QPointF destination(QCursor::pos());
        check(pointer.takeDelta(edge).isNull() &&
              pointer.takeDelta(edge - step / 8).isNull() &&
              pointer.takeDelta(destination).isNull() &&
              pointer.takeDelta(destination + step) == step,
              "edge recenter ignores queued old positions and continues the drag");
        pointer.finish(destination + step);
    }

    QPointingDevice touchpad(QStringLiteral("scrub-test-trackpad"), 37,
        QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
        QInputDevice::Capability::Position, 2, 1);
    // Qt 6.8's QWidget dispatcher rounds global mouse positions. Observe the
    // delivered coordinates so this check also works with Qt versions that
    // preserve them; the direct pointer check above covers fractional deltas.
    class DeliveredPointer final : public QObject {
    public:
        QPointF press, position;
        bool eventFilter(QObject*, QEvent* event) override {
            if (auto* mouse = dynamic_cast<QMouseEvent*>(event)) {
                if (event->type() == QEvent::MouseButtonPress) press = mouse->globalPosition();
                position = mouse->globalPosition();
            }
            return false;
        }
    };
    ToolPanel panel;
    const auto mouse = [&](QWidget* control, QEvent::Type type, QPointF offset,
                           const QPointingDevice* device,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                           Qt::MouseButtons buttons = Qt::LeftButton) {
        const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, QPointF(control->rect().center()) + offset,
                          anchor + offset, button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : buttons,
                          modifiers, device);
        QApplication::sendEvent(control, &event);
    };
    for (const QPointingDevice* device : {QPointingDevice::primaryPointingDevice(),
                                         static_cast<const QPointingDevice*>(&touchpad)}) {
        for (const bool vertical : {false, true}) {
            auto* slider = panel.findChild<QSlider*>(vertical
                ? QStringLiteral("TimelineTrackHeightSlider")
                : QStringLiteral("TimelineZoomSlider"));
            if (!slider) return false;
            DeliveredPointer delivered;
            slider->installEventFilter(&delivered);
            const auto offset = [vertical](double distance) {
                return vertical ? QPointF(0, -distance) : QPointF(distance, 0);
            };
            const double perPixel = double(slider->maximum() - slider->minimum()) /
                                    (vertical ? 90.0 : 180.0);
            const int baseline = (slider->minimum() + slider->maximum()) / 2;
            slider->setValue(baseline);
            int starts = 0, finishes = 0;
            const auto started = QObject::connect(slider, &QSlider::sliderPressed,
                &panel, [&] { ++starts; });
            const auto finished = QObject::connect(slider, &QSlider::sliderReleased,
                &panel, [&] { ++finishes; });
            mouse(slider, QEvent::MouseButtonPress, {}, device);
            const QPoint cursor = QCursor::pos();
            bool exact = true;
            for (int sample = 1; sample <= 40; ++sample) {
                const double distance = sample * 0.25;
                // Touchpads can queue or repeat absolute positions before the
                // next paint. Event frequency must not change the distance.
                mouse(slider, QEvent::MouseMove, offset(distance), device);
                const int beforeDuplicate = slider->value();
                mouse(slider, QEvent::MouseMove, offset(distance), device);
                const QPointF travel = delivered.position - delivered.press;
                const double expected = baseline + (vertical ? -travel.y() : travel.x()) * perPixel;
                exact = exact && std::abs(slider->value() - expected) <= 0.500001 &&
                        slider->value() == beforeDuplicate &&
                        QCursor::pos() == cursor;
            }
            mouse(slider, QEvent::MouseButtonRelease, offset(12.5), device);
            const QPointF travel = delivered.position - delivered.press;
            const double expected = baseline + (vertical ? -travel.y() : travel.x()) * perPixel;
            if (!exact || std::abs(slider->value() - expected) > 0.500001 ||
                slider->isSliderDown() || starts != 1 || finishes != 1)
                std::fprintf(stderr, "scrub release (%s): exact=%d value=%d expected=%.6f pressed=%d starts=%d finishes=%d\n",
                    vertical ? "vertical" : "horizontal", int(exact), slider->value(),
                    expected, int(slider->isSliderDown()), starts, finishes);
            check(exact && std::abs(slider->value() - expected) <= 0.500001 &&
                  !slider->isSliderDown() && starts == 1 && finishes == 1,
                  "view arrow tracks dense input and the final release without duplicate jumps");

            slider->setValue(baseline);
            mouse(slider, QEvent::MouseButtonPress, {}, device);
            mouse(slider, QEvent::MouseMove, offset(1), device);
            mouse(slider, QEvent::MouseMove, offset(2), device, Qt::ShiftModifier);
            mouse(slider, QEvent::MouseMove, offset(2), device);
            mouse(slider, QEvent::MouseMove, offset(1.75), device, Qt::ShiftModifier);
            mouse(slider, QEvent::MouseButtonRelease, offset(1.75), device);
            check(slider->value() == int(std::lround(baseline + 1.1875 * perPixel)),
                  "Shift precision and direction changes do not jump");

            slider->setValue(slider->maximum() - 1);
            mouse(slider, QEvent::MouseButtonPress, {}, device);
            mouse(slider, QEvent::MouseMove, offset(80), device);
            mouse(slider, QEvent::MouseMove, offset(75), device);
            mouse(slider, QEvent::MouseButtonRelease, offset(75), device);
            check(slider->value() == int(std::lround(slider->maximum() - 5 * perPixel)),
                  "scrub reverses immediately at the value limit");

            for (const auto type : {QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
                slider->setValue(baseline);
                mouse(slider, QEvent::MouseButtonPress, {}, device);
                mouse(slider, QEvent::MouseMove, offset(5), device);
                const int before = slider->value();
                if (type == QEvent::Hide) {
                    QHideEvent hidden;
                    QApplication::sendEvent(slider, &hidden);
                } else {
                    QEvent interrupted(type);
                    QApplication::sendEvent(slider, &interrupted);
                }
                mouse(slider, QEvent::MouseMove, offset(50), device);
                mouse(slider, QEvent::MouseButtonRelease, offset(50), device);
                check(!slider->isSliderDown() && slider->value() == before,
                      "interrupted view arrow ignores late move and release");
            }
            mouse(slider, QEvent::MouseButtonPress, {}, device);
            mouse(slider, QEvent::MouseMove, offset(3), device);
            const int beforeLostRelease = slider->value();
            mouse(slider, QEvent::MouseMove, offset(40), device, Qt::NoModifier, Qt::NoButton);
            check(!slider->isSliderDown() && slider->value() == beforeLostRelease,
                  "trackpad drag ends when its release is lost");
            QObject::disconnect(started);
            QObject::disconnect(finished);
            slider->removeEventFilter(&delivered);
        }
        auto* waveform = panel.findChild<QWidget*>(QStringLiteral("WaveformScaleButton"));
        if (!waveform) return false;
        double scale = 1.0;
        const auto changed = QObject::connect(&panel, &ToolPanel::waveformScaleChanged,
            &panel, [&](double value) { scale = value; });
        mouse(waveform, QEvent::MouseButtonDblClick, {}, device);
        mouse(waveform, QEvent::MouseButtonPress, {}, device);
        for (int sample = 1; sample <= 40; ++sample) {
            mouse(waveform, QEvent::MouseMove, QPointF(0, -sample * 0.5), device);
            mouse(waveform, QEvent::MouseMove, QPointF(0, -sample * 0.5), device);
        }
        mouse(waveform, QEvent::MouseButtonRelease, QPointF(0, -20), device);
        check(std::abs(scale - std::pow(2.0, 20.0 / 80.0)) < 1.0e-9,
              "waveform arrow also counts physical travel instead of event count");
        QObject::disconnect(changed);

        CreatorNumberField number;
        number.setRange(0, 100);
        number.setDecimals(3);
        number.setSingleStep(1);
        number.setValue(50);
        mouse(&number, QEvent::MouseButtonPress, {}, device);
        for (int sample = 1; sample <= 40; ++sample)
            mouse(&number, QEvent::MouseMove, QPointF(0, -sample * 0.25), device);
        mouse(&number, QEvent::MouseButtonRelease, QPointF(0, -10), device);
        check(std::abs(number.value() - 52.5) < 1.0e-9,
              "shared numeric scrub retains small trackpad movements");
    }
    ValueBubble::dismiss();
    return ok;
}

bool checkUiScaling() {
    if (qEnvironmentVariableIsSet("VLT_VIEW_SCRUB_CHECK_ONLY"))
        return checkViewScrubInput();
    if (qEnvironmentVariableIsSet("VLT_TRACK_PRESENTATION_CHECK_ONLY"))
        return TimelineWidget::checkTrackPresentationForTest();
    bool ok = true;
    const auto check = [&](bool result, const char* name) {
        std::printf("%s  %s\n", result ? "PASS" : "FAIL", name); ok = ok && result;
    };
    // Exercise the same shared divider used by Browser, Mixer, Web and Warp.
    // Releases can arrive without a final move when native events coalesce.
    for (const auto orientation : {Qt::Horizontal, Qt::Vertical}) {
        QWidget host;
        ResizeHandle handle(orientation, &host);
        int delta = 0;
        handle.onDrag = [&](int value) { delta = value; };
        const auto mouse = [&](QEvent::Type type, int distance, Qt::MouseButtons buttons) {
            const QPointF global = orientation == Qt::Horizontal
                ? QPointF(100, 100 + distance) : QPointF(100 + distance, 100);
            QMouseEvent event(type, handle.mapFromGlobal(global), global,
                type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                buttons, Qt::NoModifier);
            QApplication::sendEvent(&handle, &event);
        };
        mouse(QEvent::MouseButtonPress, 0, Qt::LeftButton);
        mouse(QEvent::MouseMove, 20, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, 35, Qt::NoButton);
        check(delta == 35, "panel divider commits the final release coordinate");
        for (const auto type : {QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
            mouse(QEvent::MouseButtonPress, 0, Qt::LeftButton);
            mouse(QEvent::MouseMove, 10, Qt::LeftButton);
            QEvent cancelled(type);
            QApplication::sendEvent(&handle, &cancelled);
            mouse(QEvent::MouseMove, 80, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, 90, Qt::NoButton);
            check(delta == 10, "cancelled panel resize cannot resume from a stale press");
        }
        mouse(QEvent::MouseButtonPress, 0, Qt::LeftButton);
        mouse(QEvent::MouseMove, 12, Qt::LeftButton);
        mouse(QEvent::MouseMove, 40, Qt::NoButton);
        mouse(QEvent::MouseMove, 60, Qt::LeftButton);
        check(delta == 12, "missing release cancels the panel resize on hover");
    }
    check(TimelineWidget::checkInterruptedPointerGestureForTest(),
          "lost mouse release stops playhead, marquee and time selection");
    if (qEnvironmentVariableIsSet("VLT_POINTER_RELEASE_CHECK_ONLY")) return ok;
    check(TimelineWidget::checkFileDropPreviewForTest(),
          "live replacement waveforms, hover restoration, track scoping, cancellation and grouped undo/redo");
    if (qEnvironmentVariableIsSet("VLT_FILE_DROP_CHECK_ONLY")) return ok;
    check(TimelineWidget::checkClipTrimPreviewForTest(),
          "live audio/MIDI/Pattern/automation trim stays visible across tile boundaries and reversals");
    if (qEnvironmentVariableIsSet("VLT_CLIP_TRIM_CHECK_ONLY")) {
        daw::EngineController controller{};
        if (!controller.initialize(48000, 512, false)) return false;
        PianoRollWindow editor(&controller);
        check(editor.checkMidiFileActionsForTest(), "trimmed MIDI piano-roll source bounds, seeking and export");
        return ok;
    }
    const auto settle = [](int ms = 80) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    daw::EngineController controller{};
    controller.initialize(48000.0, 512, false);
    TimelineWidget gridProbe(&controller);
    gridProbe.resize(200, 120);
    check(gridProbe.checkGridAppearanceForTest(),
          "grid width/opacity, selected-lane contrast, invisible-grid snapping and crisp playhead at 100/150/200% scale");
    if (qEnvironmentVariableIsSet("VLT_GRID_APPEARANCE_CHECK_ONLY")) return ok;
    check(gridProbe.checkGestureGridStabilityForTest(),
          "clip gestures keep the grid on its stable retained-tile origin");
    check(gridProbe.checkMoveGuidePaintForTest(),
          "clip move origin stays fixed with full-height edges and a light fill");
    check(GlassSlider::checkInteractionForTest(),
          "glass sliders jump, capture, drag beyond bounds and accept trackpad pixels");
    check(graphics::WorkspaceSurface::checkPointerRoutingForTest(),
          "GPU input routing repairs stale grabs and preserves wheel propagation");
    // This benchmark isolates UI cost: no real device or 500 DSP channels.
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear(); project.invalidateTrackIndex();
    for (int t = 0; t < 500; ++t) {
        daw::TrackModel track;
        track.id = "ui-perf-track-" + std::to_string(t);
        track.name = "Track " + std::to_string(t + 1);
        track.kind = daw::TrackKind::Midi;
        for (int c = 0; c < 40; ++c) {
            daw::ClipModel clip;
            clip.id = track.id + "-clip-" + std::to_string(c);
            clip.kind = daw::ClipKind::Midi;
            clip.notes.resize(5);
            clip.startSeconds = c * 2.0; clip.durationSeconds = 1.5;
            track.clips.push_back(std::move(clip));
        }
        project.tracks.push_back(std::move(track));
    }
    TrackListWidget tracks(&controller);
    MixerWidget mixer(&controller);
    NavigationTimeline timeline(&controller);
    tracks.resize(280, 640); mixer.resize(1250, 760); timeline.resize(1250, 640);
    tracks.rebuild(); tracks.show(); mixer.show(); timeline.show(); settle();
    check(timeline.checkAdaptiveGridForTest(),
          "adaptive overview uses sparse whole-bar groups, aligned snapping and correct meters");
    check(timeline.checkBottomInsetInvalidationForTest(),
          "mixer reveal invalidates the retained timeline before playback repaint");
    const QString first = QString::fromStdString(project.tracks.front().id);
    const QPointer<FaderWidget> rowFader = tracks.rowFaderForTest(first);
    ChannelStrip* mixerStrip = nullptr;
    for (auto* strip : mixer.findChildren<ChannelStrip*>()) {
        if (!strip->isMaster()) { mixerStrip = strip; break; }
    }
    const QPointer<FaderWidget> fader =
        mixerStrip ? mixerStrip->findChild<FaderWidget*>() : nullptr;
    const QPointer<PanKnob> panControl =
        mixerStrip ? mixerStrip->findChild<PanKnob*>() : nullptr;
    check(fader && panControl, "visible mixer channel has a fader and pan control");
    int wheelFinishes = 0;
    int panWheelFinishes = 0;
    if (fader) {
        const double beforeWheel = fader->gain();
        QObject::connect(fader, &FaderWidget::editFinished, fader,
                         [&wheelFinishes] { ++wheelFinishes; });
        const QPointF point = fader->rect().center();
        for (int sample = 0; sample < 8; ++sample) {
            QWheelEvent event(point, fader->mapToGlobal(point), QPoint(0, 3),
                              {}, Qt::NoButton, Qt::NoModifier,
                              Qt::ScrollUpdate, false);
            QApplication::sendEvent(fader, &event);
        }
        check(fader->gain() == beforeWheel && wheelFinishes == 0,
              "scrolling over the mixer fader leaves its gain unchanged");
    }
    if (panControl) {
        const double beforeWheel = panControl->pan();
        QObject::connect(panControl, &PanKnob::editFinished, panControl,
                         [&panWheelFinishes] { ++panWheelFinishes; });
        const QPointF point = panControl->rect().center();
        for (int sample = 0; sample < 8; ++sample) {
            QWheelEvent event(point, panControl->mapToGlobal(point), QPoint(0, 3),
                              {}, Qt::NoButton, Qt::NoModifier,
                              Qt::ScrollUpdate, false);
            QApplication::sendEvent(panControl, &event);
        }
        check(panControl->pan() > beforeWheel && panWheelFinishes == 0,
              "mixer pan applies high-resolution wheel samples without committing each one");
    }
    settle(180);
    check(wheelFinishes == 0,
          "scrolling over the mixer fader creates no edit");
    check(panWheelFinishes == 1,
          "mixer pan wheel burst commits as one gesture");
    check(tracks.findChildren<FaderWidget*>().size() < 64, "500 track headers create only viewport controls");
    check(mixer.findChildren<FaderWidget*>().size() < 64, "500 mixer channels create only viewport controls");
    tracks.setSelectedTracks({first, QString::fromStdString(project.tracks[1].id)}, first);
    QElapsedTimer time; time.start();
    for (int i = 0; i < 20; ++i) {
        project.tracks.front().name = "Rename " + std::to_string(i);
        tracks.rebuild(); mixer.rebuild();
    }
    std::printf("20 UI structure synchronizations: %.3f ms\n", time.nsecsElapsed() / 1e6);
    check(rowFader && tracks.rowFaderForTest(first) == rowFader,
          "unchanged row controls survive repeated sync");
    check(tracks.selectedTrackIds().size() == 2, "multi-selection survives structure sync");
    tracks.setVerticalScroll(72 * 492); settle();
    check(tracks.rowFaderForTest(QString::fromStdString(project.tracks.back().id)), "scroll materializes final rows");
    check(tracks.findChildren<FaderWidget*>().size() < 64, "scroll releases distant controls");
    tracks.setVerticalScroll(0); settle();
    check(tracks.rowFaderForTest(first), "scroll back restores rows");
    check(timeline.checkClipIndexForTest(), "interval queries cover visible clip bodies and preserve ID lookup");
    controller.setClipStartSeconds(project.tracks.front().id, project.tracks.front().clips.front().id, 5000.0);
    check(timeline.checkClipIndexForTest(), "live placement invalidates interval geometry before undo commit");
    const auto unknown = project.findTrack("missing-track");
    check(!unknown && !project.findTrack(""), "missing and master IDs do not resolve to a track");
    std::swap(project.tracks[0], project.tracks[1]); project.invalidateTrackIndex();
    tracks.rebuild(); mixer.rebuild(); timeline.update(); settle();
    check(tracks.rowRectForTrack(first).top() > tracks.rowRectForTrack(QString::fromStdString(project.tracks[0].id)).top(), "same-size reorder updates row positions");
    time.start();
    for (int i = 0; i < 200; ++i) project.findTrack("");
    std::printf("200 empty ID lookups: %.3f ms\n", time.nsecsElapsed() / 1e6);
    std::vector<daw::EngineController::ClipStartChange> move;
    for (int t = 0; t < 25; ++t) for (const auto& clip : project.tracks[t].clips)
        move.push_back({project.tracks[t].id, clip.id, clip.startSeconds});
    controller.beginClipPositionEdit();
    for (int frame = 0; frame < 40; ++frame) {
        for (auto& clip : move) clip.startSeconds += 0.125;
        perf::Scope timing("gesture.group1000.ms");
        controller.setClipStartsSeconds(move);
    }
    controller.endClipPositionEdit("Scaling gesture");
    check(timeline.checkClipIndexForTest(), "1000-clip gesture preserves interval correctness");
    const auto wheel = [](QWidget& surface, QPoint pixels, QPoint angle,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                          Qt::ScrollPhase phase = Qt::ScrollUpdate) {
        const QPointF position(400.75, 100.25);
        QWheelEvent event(position, surface.mapToGlobal(position), pixels, angle,
                          Qt::NoButton, modifiers, phase, false);
        QApplication::sendEvent(&surface, &event);
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-8; };
    timeline.setFollowPlayhead(false);
    timeline.setVerticalScroll(100);
    const double startScroll = timeline.horizontalScrollForTest();
    const double startScale = timeline.pixelsPerSecondForTest();
    wheel(timeline, {-3, -2}, {-120, -120});
    check(near(timeline.horizontalScrollForTest(), startScroll + 3.0 / startScale) &&
          timeline.verticalScroll() == 102, "pixel scrolling preserves both axes and overrides wheel angles");
    wheel(timeline, {-1, -1}, {}, Qt::NoModifier, Qt::ScrollMomentum);
    check(near(timeline.horizontalScrollForTest(), startScroll + 4.0 / startScale) &&
          timeline.verticalScroll() == 103, "one-pixel momentum is not lost");
    wheel(timeline, {}, {0, -1}); wheel(timeline, {}, {0, -1});
    check(timeline.verticalScroll() == 104, "fractional mouse-wheel distances accumulate");
    const double shiftStart = timeline.horizontalScrollForTest();
    wheel(timeline, {0, -3}, {}, Qt::ShiftModifier);
    wheel(timeline, {}, {0, -120}, Qt::ShiftModifier);
    check(near(timeline.horizontalScrollForTest(), shiftStart + 33.0 / startScale) &&
          timeline.verticalScroll() == 104, "Shift preserves pixel precision and legacy mouse-wheel distance");
    const auto* horizontalBar = timeline.findChild<QScrollBar*>("TimelineHorizontalScroll");
    const auto centreTime = [&] {
        return timeline.horizontalScrollForTest() +
               horizontalBar->width() * 0.5 / timeline.pixelsPerSecondForTest();
    };
    const auto timeAt = [&](double x) {
        return timeline.horizontalScrollForTest() +
               x / timeline.pixelsPerSecondForTest();
    };
    const double wheelX = 400.75;
    controller.seekSeconds(20.125);
    const double wheelAnchor = timeAt(wheelX);
    wheel(timeline, {}, {0, 1}, Qt::ControlModifier);
    const double fineScale = timeline.pixelsPerSecondForTest();
    check(fineScale > startScale && fineScale < startScale * 1.01 &&
          near(timeAt(wheelX), wheelAnchor),
          "fine wheel zoom is proportional and stays under the pointer");
    wheel(timeline, {}, {0, -1}, Qt::ControlModifier);
    check(near(timeline.pixelsPerSecondForTest(), startScale), "opposite fine zoom restores the scale");
    wheel(timeline, {}, {0, 120}, Qt::ControlModifier);
    check(near(timeline.pixelsPerSecondForTest(), startScale * 1.15), "one wheel notch retains 15 percent zoom");
    wheel(timeline, {}, {0, -120}, Qt::ControlModifier);
    const double zoomScroll = timeline.horizontalScrollForTest();
    const double closeAnchor = timeAt(wheelX);
    timeline.zoomBy(ui::kMaxTimelineZoom / timeline.pixelsPerSecondForTest(), wheelX);
    check(near(timeline.pixelsPerSecondForTest(), ui::kMaxTimelineZoom) &&
          near(timeAt(wheelX), closeAnchor),
          "sample-level zoom reaches the new limit without moving the pointer anchor");
    timeline.zoomBy(startScale / timeline.pixelsPerSecondForTest(), wheelX);
    wheel(timeline, {}, {}, Qt::ControlModifier, Qt::ScrollBegin);
    wheel(timeline, {}, {}, Qt::ControlModifier, Qt::ScrollEnd);
    check(near(timeline.pixelsPerSecondForTest(), startScale) &&
          near(timeline.horizontalScrollForTest(), zoomScroll), "zero-delta gesture boundaries never zoom or move the anchor");
    for (int i = 0; i < 100; ++i) {
        wheel(timeline, {0, 1}, {}, Qt::ControlModifier);
        wheel(timeline, {0, -1}, {}, Qt::ControlModifier);
    }
    check(near(timeline.horizontalScrollForTest(), zoomScroll) &&
          near(timeline.pixelsPerSecondForTest(), startScale) &&
          near(timeAt(wheelX), wheelAnchor),
          "repeated pixel zoom keeps the pointer anchor without drift");

    const QPoint savedCursor = QCursor::pos();
    const int liveX = 720;
    QCursor::setPos(timeline.mapToGlobal(QPoint(liveX, 140)));
    const double liveAnchor = timeAt(liveX);
    timeline.zoomBy(1.3);
    check(near(timeAt(liveX), liveAnchor),
          "keyboard zoom follows the live pointer over the timeline");
    timeline.zoomBy(1.0 / 1.3);
    const auto pinch = [&](double value, const QPointF& position) {
        QNativeGestureEvent event(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
                                  position, position, timeline.mapToGlobal(position), value, {});
        QApplication::sendEvent(&timeline, &event);
    };
    const QPointF pinchPoint(70.25, 100.5);
    const double pinchAnchor = timeAt(pinchPoint.x());
    pinch(0.1, pinchPoint);
    check(near(timeAt(pinchPoint.x()), pinchAnchor),
          "native pinch stays under the gesture position");
    pinch(1.0 / 1.1 - 1.0, pinchPoint);
    check(near(timeAt(pinchPoint.x()), pinchAnchor) &&
          near(timeline.pixelsPerSecondForTest(), startScale),
          "reversing pinch preserves its pointer anchor and restores scale");

    const auto& focusTrack = project.tracks[0];
    const auto& focusClip = focusTrack.clips.back();
    const ui::ClipSel focusRef{QString::fromStdString(focusTrack.id), QString::fromStdString(focusClip.id)};
    timeline.selectClips({focusRef});
    const double selectedPointerAnchor = timeAt(wheelX);
    wheel(timeline, {}, {0, 120}, Qt::ControlModifier);
    check(near(timeAt(wheelX), selectedPointerAnchor),
          "clip selection does not override pointer zoom");
    wheel(timeline, {}, {0, -120}, Qt::ControlModifier);

    controller.seekSeconds(20.125);
    const double playheadAnchor = controller.presentationPositionSeconds();
    QCursor::setPos(timeline.mapToGlobal(
        QPoint(500, timeline.rulerHeight() / 2)));
    timeline.zoomBy(1.3);
    check(near(centreTime(), playheadAnchor),
          "zoom over the timeline header centres the playhead despite selection");
    timeline.zoomBy(1.0 / 1.3);
    timeline.clearClipSelection();
    controller.seekSeconds(0);
    timeline.zoomBy(1.1);
    check(near(timeline.horizontalScrollForTest(), 0), "zoom at project start respects the left boundary");
    controller.seekSeconds(playheadAnchor);
    timeline.zoomBy(1.0 / 1.1);

    int headerScroll = 0;
    const auto scrollConnection = QObject::connect(&tracks, &TrackListWidget::verticalScrollRequested,
        &tracks, [&](int delta) { headerScroll += delta; });
    wheel(tracks, {0, -2}, {0, -120});
    wheel(tracks, {}, {0, -1}); wheel(tracks, {}, {0, -1});
    check(headerScroll == 3, "track headers use the same pixel and fractional-wheel distances");
    QObject::disconnect(scrollConnection);

    const auto pan = [&](QEvent::Type type, const QPointF& position) {
        QMouseEvent event(type, position, timeline.mapToGlobal(position),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::MiddleButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MiddleButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&timeline, &event);
    };
    const QPointF grab(400.75, 100.25);
    const double panStart = timeline.horizontalScrollForTest();
    const int panY = timeline.verticalScroll();
    pan(QEvent::MouseButtonPress, grab);
    pan(QEvent::MouseMove, grab - QPointF(0.25, 0.25));
    check(near(timeline.horizontalScrollForTest(), panStart + 0.25 / startScale),
          "middle-button pan preserves fractional pointer movement");
    for (int step = 2; step <= 4; ++step)
        pan(QEvent::MouseMove, grab - QPointF(step * 0.25, step * 0.25));
    check(near(timeline.horizontalScrollForTest(), panStart + 1.0 / startScale) &&
          timeline.verticalScroll() == panY + 1, "fractional pan accumulates without losing either axis");
    pan(QEvent::MouseMove, grab);
    pan(QEvent::MouseButtonRelease, grab);
    check(near(timeline.horizontalScrollForTest(), panStart) && timeline.verticalScroll() == panY,
          "reversing a pan restores the original viewport");
    timeline.setVerticalScroll(0);
    pan(QEvent::MouseButtonPress, grab);
    pan(QEvent::MouseMove, grab + QPointF(0, 0.75));
    pan(QEvent::MouseMove, grab - QPointF(0, 0.25));
    pan(QEvent::MouseButtonRelease, grab - QPointF(0, 0.25));
    check(timeline.verticalScroll() == 1, "panning away from a bound has no overscroll dead zone");

    // Actual Qt paint delivery at a speed far below one logical pixel/frame.
    // Feed deterministic transport time without opening an audio device; this
    // isolates rendering from DSP and the audio block interpolation tests.
    FrameClock::instance().setPreference(FrameMode::Fixed, 60);
    controller.seekSeconds(200.0);
    timeline.clearClipSelection();
    timeline.zoomBy(4.0 / timeline.pixelsPerSecondForTest());
    timeline.setVerticalScroll(0);
    controller.play();
    timeline.refreshPlaybackFrame(); settle();
    const auto playbackStaticBefore = timeline.staticFramePaintCountForTest();
    FrameTimer playbackClock(&timeline);
    QElapsedTimer playbackTime; playbackTime.start();
    int playbackCallbacks = 0;
    QObject::connect(&playbackClock, &FrameTimer::timeout, &timeline, [&] {
        ++playbackCallbacks;
        controller.seekSeconds(200.0 + playbackTime.nsecsElapsed() / 1e9);
        timeline.refreshPlaybackFrame();
    });
    QEventLoop playback;
    QTimer::singleShot(1200, &playback, &QEventLoop::quit);
    timeline.profilePlayback = true;
    playbackClock.start(); playback.exec(); playbackClock.stop(); settle();
    timeline.profilePlayback = false;
    bool subpixel = false, monotonic = true;
    int distinctPixels = 0;
    double previous = -1.0;
    for (double x : timeline.playbackPositions) {
        subpixel |= std::abs(x - std::round(x)) > 0.01;
        if (previous >= 0.0) monotonic &= x >= previous;
        if (previous < 0.0 || int(x) != int(previous)) ++distinctPixels;
        previous = x;
    }
    std::printf("Slow playback (4 px/s): %d callbacks, %d paints, %d distinct whole pixels in %.3f s\n",
                playbackCallbacks, timeline.playbackPaints, distinctPixels, playbackTime.nsecsElapsed() / 1e9);
    check(subpixel && monotonic, "playback preserves fractional positions without stepping backwards");
    check(playbackCallbacks > 0 && timeline.playbackPaints >= playbackCallbacks * 0.85 &&
          timeline.playbackPaints > distinctPixels * 3,
          "zoomed-out playback paints intermediate frames within the same logical pixel");
    // Cocoa may expose the whole window while this real-window benchmark runs.
    // Such exposure may rebuild the cache; a cursor strip must never do so.
    const auto playbackStaticPaints = timeline.staticFramePaintCountForTest() - playbackStaticBefore;
    std::printf("Playback static paints: %llu (%d full window exposures)\n",
                static_cast<unsigned long long>(playbackStaticPaints), timeline.playbackFullPaints);
    check(timeline.playbackNarrowStaticPaints == 0 && timeline.playbackFullPaints <= 2 &&
          playbackStaticPaints <= std::uint64_t(timeline.playbackFullPaints),
          "cursor-only frames reuse the static timeline without whole-window repainting");

    const bool savedTrail = ui::playheadTrail();
    ui::setPlayheadTrail(true); // The trail is opt-in; explicitly exercise it.
    timeline.zoomBy(80.0 / timeline.pixelsPerSecondForTest());
    timeline.refreshPlaybackFrame(); settle();
    const double steadyTrail = timeline.displayedPlayheadTrailForTest();
    wheel(timeline, {-200, 0}, {});
    timeline.refreshPlaybackFrame(); settle();
    const double forwardPanTrail = timeline.displayedPlayheadTrailForTest();
    wheel(timeline, {400, 0}, {});
    timeline.refreshPlaybackFrame(); settle();
    check(steadyTrail > 0 && near(steadyTrail, forwardPanTrail) &&
          near(steadyTrail, timeline.displayedPlayheadTrailForTest()),
          "panning either way leaves the playback trail length unchanged");
    const double headBeforeSeek = timeline.displayedPlayheadSubpixelXForTest();
    controller.seekSeconds(controller.positionSeconds() - 0.5);
    timeline.refreshPlaybackFrame(); settle();
    check(near(timeline.displayedPlayheadSubpixelXForTest(), headBeforeSeek - 40.0) &&
          near(steadyTrail, timeline.displayedPlayheadTrailForTest()),
          "a backward seek jumps directly without stretching or reversing the trail");
    // Compare rasterized playback/paused images at one exact transport time.
    // The only difference must be behind (left of) the cursor.
    const QImage rollingImage = timeline.grab().toImage();
    const double trailHeadX = timeline.displayedPlayheadSubpixelXForTest();
    controller.pause(); timeline.refreshPlaybackFrame(); settle();
    const QImage parkedImage = timeline.grab().toImage();
    int leftDifference = 0, boundaryDifference = 0, rightDifference = 0;
    int firstRightDifference = rollingImage.width(), lastRightDifference = -1;
    const double dpr = rollingImage.devicePixelRatio();
    const double headPixel = trailHeadX * dpr;
    for (int y = int(100 * dpr); y < int(130 * dpr); ++y) {
        for (int x = int((trailHeadX - 55) * dpr); x < int((trailHeadX + 55) * dpr); ++x) {
            if (x < 0 || x >= rollingImage.width()) continue;
            if (rollingImage.pixel(x, y) == parkedImage.pixel(x, y)) continue;
            // A fractional cursor centre straddles one physical pixel. The
            // left-only wash can affect that pixel through antialiasing too;
            // only pixels starting at/after ceil(headPixel) are fully right.
            if (x + 1.0 <= headPixel) ++leftDifference;
            else if (x < std::ceil(headPixel)) ++boundaryDifference;
            else {
                ++rightDifference;
                firstRightDifference = std::min(firstRightDifference, x);
                lastRightDifference = std::max(lastRightDifference, x);
            }
        }
    }
    std::printf("Trail raster: headPx=%.6f pausedPx=%.6f boundaryColumn=%d left=%d boundary=%d oldRight=%d right=%d rightColumns=%d..%d\n",
        headPixel, timeline.displayedPlayheadSubpixelXForTest() * dpr, int(std::floor(headPixel)),
        leftDifference, boundaryDifference, boundaryDifference + rightDifference, rightDifference,
        firstRightDifference, lastRightDifference);
    check(leftDifference > 0 && rightDifference == 0 &&
          near(trailHeadX, timeline.displayedPlayheadSubpixelXForTest()),
          "after reverse pan the rendered trail extends only left from the cursor");
    check(near(timeline.displayedPlayheadTrailForTest(), 0.0), "pause removes the trail immediately");

    controller.play();
    timeline.zoomBy(1200.0 / timeline.pixelsPerSecondForTest());
    timeline.refreshPlaybackFrame(); settle();
    const double wideHeadX = timeline.displayedPlayheadSubpixelXForTest();
    const double wideTrail = timeline.displayedPlayheadTrailForTest();
    const auto stopStaticBefore = timeline.staticFramePaintCountForTest();
    controller.stop(); timeline.refreshPlaybackFrame(); settle();
    check(near(wideTrail, 52.0) && timeline.lastPaintRegion.contains(
              QPoint(int(wideHeadX - wideTrail + 1), 100)) &&
          timeline.staticFramePaintCountForTest() == stopStaticBefore,
          "stopping erases the whole previous trail with a cached narrow repaint");
    ui::setPlayheadTrail(savedTrail);
    controller.seekSeconds(20.0);
    timeline.zoomBy(80.0 / timeline.pixelsPerSecondForTest());
    timeline.refreshPlaybackFrame(); settle();

    // Sustained navigation through a populated viewport (no delay per input).
    // Timers/painting still run through the native event loop and frame clock.
    FrameClock::instance().setPreference(FrameMode::Fixed, 60);
    timeline.setVerticalScroll(0); settle();
    QTimer gesture;
    gesture.setTimerType(Qt::PreciseTimer);
    gesture.setInterval(8);
    int gestureSteps = 0;
    QObject::connect(&gesture, &QTimer::timeout, &timeline, [&] {
        const int direction = (gestureSteps++ / 40) % 2 ? 1 : -1;
        wheel(timeline, {direction * 2, direction}, {});
        wheel(timeline, {0, direction}, {}, Qt::ControlModifier);
    });
    const auto paintsBefore = timeline.staticFramePaintCountForTest();
    FrameTimer navigationClock(&timeline);
    int navigationFrames = 0;
    QObject::connect(&navigationClock, &FrameTimer::timeout, &timeline,
                     [&] { ++navigationFrames; });
    QEventLoop navigation;
    QTimer::singleShot(1200, &navigation, &QEventLoop::quit);
    timeline.profileNavigation = true;
    navigationClock.start();
    gesture.start(); navigation.exec(); gesture.stop(); settle();
    navigationClock.stop();
    timeline.profileNavigation = false;
    const auto navigationPaints = timeline.staticFramePaintCountForTest() - paintsBefore;
    std::printf("Continuous navigation: %d inputs, %d frames, %llu static paints (%dx%d, DPR %.1f, %.2f Hz)\n",
                gestureSteps, navigationFrames, static_cast<unsigned long long>(navigationPaints),
                timeline.width(), timeline.height(), timeline.devicePixelRatioF(), timeline.screen()->refreshRate());
    check(gestureSteps > 0 && timeline.staticFramePaintCountForTest() > paintsBefore,
          "continuous scroll and zoom paint while input is active");
    check(navigationFrames > 0 && navigationPaints <= std::uint64_t(navigationFrames * 2 + 4),
          "scrollbars do not repaint the canvas at input frequency");
    for (int fps : {60, 120, 144, 240}) {
        FrameClock::instance().setPreference(FrameMode::Fixed, fps);
        for (int step = 0; step < 4; ++step) {
            timeline.setVerticalScroll((step * 123) * 72);
            timeline.zoomBy(step % 2 ? 0.8 : 1.25);
            timeline.update(); settle();
        }
    }
    FrameClock::instance().setPreference(FrameMode::Unlimited, 60);
    timeline.update(); settle();
    FrameClock::instance().setPreference(FrameMode::Fixed, 60);
    perf::flush();
    QCursor::setPos(savedCursor);
    return ok;
}
} // namespace ui

bool ui::checkAudioTimelinePerformance() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;
    const auto check = [&](bool value, const char* name) {
        std::printf("%s  %s\n", value ? "PASS" : "FAIL", name); ok &= value;
    };
    const auto run = [](int ms) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    const auto stats = [](std::vector<double> data) {
        if (data.empty()) return std::array<double,3>{};
        std::sort(data.begin(), data.end());
        return std::array{data[data.size()/2], data[std::min(data.size()-1, data.size()*95/100)], data.back()};
    };
    QTemporaryDir files;
    audio::AudioBuffer source(2, 48000 * 40);
    for (unsigned i=0; i<source.numFrames(); ++i) {
        const float amplitude = .4f + .25f * std::sin(i / 17003.0);
        const float sample = amplitude * std::sin(i * .037);
        source.getChannel(0)[i] = sample; source.getChannel(1)[i] = sample;
    }
    const auto path = (files.path()+"/source.wav").toStdString();
    audio::AudioRecorder writer;
    if (!writer.writeWAVFile(path, source, 48000).isOk()) return false;
    for (unsigned block : {32,64,128,512}) for (bool recording : {false,true}) {
        daw::EngineController controller{};
        controller.initialize(48000,block,false);
        controller.setRecordDirectory(files.path().toStdString());
        const unsigned realtimeHelpers = controller.configureAudioWorkersForTest(true);
        std::vector<std::string> tracks;
        for (int i=0;i<8;++i) {
            const auto id=controller.addTrack(daw::TrackKind::Audio,"Audio " + std::to_string(i+1));
            tracks.push_back(id);
            controller.setTrackHeight(id,110);
            const auto clip=controller.importAudio(path,id,0);
            check(!clip.empty(),"audio benchmark imports playable PCM");
            // Distinct gains give each row its own raster cache content.
            controller.setClipGain(id,clip,.35f + i*.075f);
        }
        controller.setTrackInputChannel(tracks.front(),2);
        controller.setTrackInputChannelCount(tracks.front(),1);
        controller.setTrackInputEnabled(tracks.front(),true);
        auto prefs=controller.recordingPrefs(); prefs.mode=daw::RecordMode::Overwrite;
        controller.setRecordingPrefs(prefs);
        controller.seekSeconds(4);
        if(recording) check(controller.startRecording(tracks.front()),"start real live waveform capture");
        controller.play();
        audio::AudioBuffer input(4,block), output(2,block);
        for(unsigned ch=0;ch<4;++ch)for(unsigned f=0;f<block;++f)
            input.getChannel(ch)[f]=ch==2 ? .6f*std::sin(f*.19) : .99f;
        // Warm the capture and PCM path without a device; no fake preview data.
        for(unsigned i=0;i<96000/block;++i)controller.processDeviceBlockForTest(input,output,block);
        controller.pumpRecordingEnvelopes();
        const bool gpu = qEnvironmentVariableIntValue("VLT_GPU_WORKSPACE") == 1;
        // Both backends use exactly the same host and layout. In particular,
        // an OS-constrained top-level size must resize both canvases equally.
        QWidget gpuHost;
        NavigationTimeline timeline(&controller, &gpuHost);
        QVBoxLayout hostLayout(&gpuHost);
        hostLayout.setContentsMargins(0, 0, 0, 0);
        hostLayout.addWidget(&timeline);
        gpuHost.resize(1600, 960);
        timeline.setFollowPlayhead(false);
        std::unique_ptr<graphics::WorkspaceSurface> surface;
        if (gpu) {
            surface = std::make_unique<graphics::WorkspaceSurface>(&timeline);
            QObject::connect(surface.get(), &graphics::WorkspaceSurface::failed, &gpuHost,
                [&](const QString& reason) { check(false, qPrintable(reason)); });
            QObject::connect(surface.get(), &graphics::WorkspaceSurface::frameMeasured, &gpuHost,
                [&timeline](double prepare, double sync, double render, double interval, double renderThreadCpu) {
                    perf::sample("gpu.scene.prepare.ms", prepare);
                    perf::sample("gpu.scene.sync.ms", sync);
                    perf::sample("gpu.scene.render.cpu.ms", render);
                    if (interval > 0) perf::sample("gpu.scene.submission.interval.ms", interval);
                    if (renderThreadCpu >= 0) {
                        perf::sample("gpu.scene.render.thread.cpu.ms", renderThreadCpu);
                        if (timeline.profileAudioScroll) {
                            timeline.renderThreadCpuMs += renderThreadCpu;
                            ++timeline.measuredRenderFrames;
                        }
                    }
                });
        }
        gpuHost.show();
        FrameClock::instance().setPreference(FrameMode::Fixed,60);
        run(150);
        FrameTimer clock(&timeline);
        QObject::connect(&clock,&FrameTimer::timeout,&timeline,[&] {
            controller.pumpRecordingEnvelopes();
            timeline.refreshPlaybackFrame();
            if(recording)timeline.refreshRecordingFrame();
        });
        std::atomic<bool> active{true};
        std::vector<double> callbacks;
        callbacks.reserve(5000);
        unsigned callerRealtime=0;
        std::thread audioThread([&] {
            daw::engine::AudioWorkerRegistration registration;
            callerRealtime=registration.configure({true,48000,block,{}}) & 1u;
            const auto period=std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(double(block)/48000));
            while(active.load(std::memory_order_relaxed)) {
                const auto start=std::chrono::steady_clock::now();
                controller.processDeviceBlockForTest(input,output,block);
                callbacks.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
                std::this_thread::sleep_until(start+period);
            }
        });
        clock.start(); run(100);
        // Isolated live refresh must reuse the arrangement plate.
        const auto plate=timeline.staticFramePaintCountForTest();
        run(250);
        check(timeline.staticFramePaintCountForTest()==plate,
              "live audio/recording frames reuse the static arrangement plate");
        QTimer gesture;
        gesture.setTimerType(Qt::PreciseTimer); gesture.setInterval(8);
        int inputs=0;
        QElapsedTimer gestureTime;
        int previousTravel = 0;
        QObject::connect(&gesture,&QTimer::timeout,&timeline,[&] {
            // Both backends visit the same time-based path. A busy GUI can
            // coalesce input samples, but must not receive a shorter journey
            // simply because it delivered fewer timer callbacks.
            const auto phase = gestureTime.elapsed() % 720;
            const int travel = -int(std::lround(135. * (phase <= 360 ? phase : 720 - phase) / 360.));
            const int delta = travel - previousTravel;
            previousTravel = travel;
            if (!delta) return;
            ++inputs;
            const QPointF at(800,400);
            QWheelEvent event(at,timeline.mapToGlobal(at.toPoint()),
                QPoint(delta,0),{},Qt::NoButton,
                Qt::NoModifier,Qt::ScrollUpdate,false);
            QApplication::sendEvent(&timeline,&event);
        });
        timeline.profileAudioScroll=true;
        const auto guiCpuBefore = guiThreadCpuMs();
        const auto processCpuBefore = std::clock();
        gestureTime.start();gesture.start();run(1500);gesture.stop();
        const double guiCpu = guiThreadCpuMs() - guiCpuBefore;
        const double processCpu = double(std::clock() - processCpuBefore) * 1000. / CLOCKS_PER_SEC;
        timeline.profileAudioScroll=false;
        // Follow must yield immediately to manual horizontal navigation.
        timeline.setFollowPlayhead(true);
        QWheelEvent manual(QPointF(800,400),timeline.mapToGlobal(QPoint(800,400)),
            QPoint(-800,0),{},Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
        QApplication::sendEvent(&timeline,&manual);
        const double manualScroll=timeline.horizontalScrollForTest();
        timeline.refreshPlaybackFrame();
        check(timeline.horizontalScrollForTest()==manualScroll,"Follow cannot pull back a manually scrolled viewport");
        timeline.centerPlayhead();
        check(timeline.horizontalScrollForTest()!=manualScroll,"explicit recenter resumes cursor navigation");
        active.store(false);audioThread.join();clock.stop();
        const auto paints=stats(timeline.audioPaints), intervals=stats(timeline.audioIntervals), audio=stats(callbacks);
        const auto over=std::count_if(callbacks.begin(),callbacks.end(),[&](double ms){return ms>block*1000.0/48000;});
        std::printf("AUDIO_SCROLL frames=%u recording=%d DPR=%.1f realtime_helpers=%u realtime_caller=%u inputs=%d paints=%zu paint_ms[p50,p95,max]=%.3f,%.3f,%.3f interval_ms=%.3f,%.3f,%.3f callback_ms=%.3f,%.3f,%.3f over_budget=%zu/%zu\n",
            block,recording,timeline.devicePixelRatioF(),realtimeHelpers,callerRealtime,inputs,timeline.audioPaints.size(),
            paints[0],paints[1],paints[2],intervals[0],intervals[1],intervals[2],
            audio[0],audio[1],audio[2],size_t(over),callbacks.size());
        std::printf("SCROLL_CPU backend=%s frames=%u recording=%d gui_ms=%.3f process_ms=%.3f render_ms=%.3f render_frames=%d viewport=%dx%d\n",
                    gpu ? "gpu" : "widgets", block, recording, guiCpu, processCpu,
                    timeline.measuredRenderFrames ? timeline.renderThreadCpuMs : (gpu ? -1. : 0.), timeline.measuredRenderFrames,
                    timeline.width(), timeline.height());
        bool customPaintLimit=false;
        const double configuredLimit=qEnvironmentVariable("VLT_AUDIO_SCROLL_P95_MS").toDouble(&customPaintLimit);
        check(paints[1] < (customPaintLimit ? configuredLimit : 33.4),
              "scroll paint p95 stays below the regression ceiling (configurable per performance runner)");
        check(inputs>20 && timeline.audioPaints.size()>15,"continuous scroll delivers frames alongside the real audio callback");
        const auto screenshot=qEnvironmentVariable("VLT_AUDIO_SCROLL_SCREENSHOT");
        if(recording && block==32 && !screenshot.isEmpty()) {
            if (surface) surface->quickWindow()->grabWindow().save(screenshot);
            else timeline.grab().save(screenshot);
        }
        controller.stop(); if(recording)controller.stopRecording();
        surface.reset(); // the raster-cache checks below exercise compatibility mode
        timeline.update(); run(60);
        if(block==32) check(timeline.checkScrollCacheForTest(),
            "shifted plate matches full rendering across pan reversal, fractional motion and rounded corners");
    }
    perf::flush();
    return ok;
}

bool TimelineWidget::checkScrollCacheForTest() {
    const double savedScroll=m_scrollSeconds;
    const int savedRadius=m_rightRadius;
    bool ok=true;
    for(int radius:{0,12}) {
        setRightCornerRadius(radius);
        for(double pixels:{0.0,3.0,9.0,6.0,1.25,4.25,10.25,0.0}) {
            setHorizontalScroll(pixels/m_pixelsPerSecond);
            (void)grab();
            const QImage shifted=m_staticFrame.toImage();
            m_staticFrameValid=false;
            (void)grab();
            const QImage rebuilt=m_staticFrame.toImage();
            if(shifted!=rebuilt) {
                if(ok) {
                    shifted.save(QDir::tempPath()+"/vlt-scroll-cache-actual.png");
                    rebuilt.save(QDir::tempPath()+"/vlt-scroll-cache-expected.png");
                }
                std::fprintf(stderr,"scroll cache pixel mismatch radius=%d scroll=%.2f DPR=%.1f\n",radius,pixels,devicePixelRatioF());
                ok=false;
            }
        }
    }
    setRightCornerRadius(savedRadius);setHorizontalScroll(savedScroll);update();
    return ok;
}

bool ui::checkWorkspaceMotionPerformance() {
    // This entry point runs before main's normal theme initialization. Native
    // controls and scroll-area contents must use the production palette too.
    ThemeManager::instance().apply();
    MainWindow window(false);
    const bool passed = window.checkWorkspaceMotionForTest();
    window.endRecoverySessionForTest();
    return passed;
}

bool MainWindow::checkWorkspaceMotionForTest() {
    const QString targetName = qEnvironmentVariable("VLT_MOTION_TARGET", "sampler");
    if (targetName != "sampler" && targetName != "native" && targetName != "mixer" && targetName != "browser") return false;
    const bool overMixer = qEnvironmentVariableIntValue("VLT_MOTION_OVER_MIXER") != 0;
    const bool transparentFrame = qEnvironmentVariableIntValue("VLT_MOTION_TRANSPARENT_FRAME") != 0;
    const auto settle = [](int ms) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    m_controller.addTrack(daw::TrackKind::Midi);
    if (!openDemoSampler({})) return false;
    // Heavy UI, without an audio device or hundreds of vendor DSP instances.
    auto& project = const_cast<daw::ProjectModel&>(m_controller.project());
    for (int t = 1; t < 500; ++t) {
        daw::TrackModel track;
        track.id = "motion-track-" + std::to_string(t);
        track.name = "Track " + std::to_string(t);
        track.kind = daw::TrackKind::Midi;
        if (overMixer) {
            for (int i = 0; i < 12; ++i) {
                daw::InsertModel insert;
                insert.id = track.id + "-insert-" + std::to_string(i);
                insert.name = "Effect " + std::to_string(i + 1);
                track.inserts.push_back(std::move(insert));
            }
        }
        for (int c = 0; c < 40; ++c) {
            daw::ClipModel clip;
            clip.id = track.id + "-clip-" + std::to_string(c);
            clip.kind = daw::ClipKind::Midi;
            clip.startSeconds = c * 2.; clip.durationSeconds = 1.5;
            clip.notes.resize(5);
            track.clips.push_back(std::move(clip));
        }
        project.tracks.push_back(std::move(track));
    }
    project.invalidateTrackIndex();
    syncViews();
    resize(1600, 960);
    setMixerVisible(true);
    m_mixerHeight = 320; layoutBottomPanels();
    show(); raise(); activateWindow();
    ui::FrameClock::instance().setPreference(ui::FrameMode::Display, 60);
    settle(1800);
    auto* editor = m_pluginEditors.isEmpty() ? nullptr : m_pluginEditors.begin().value();
    auto* frame = editor ? m_internalEditorFrames.value(editor, nullptr) : nullptr;
    if (!frame || !editor->isEditorInitialized()) return false;
    QTimer noticeUpdates;
    if (qEnvironmentVariableIntValue("VLT_MOTION_NOTICES") != 0) {
        if (!m_audioNotifications) return false;
        for (int i = 0; i < 4; ++i) {
            ui::Notification notice;
            notice.id = QStringLiteral("motion-notice-%1").arg(i);
            notice.title = tr("%1 — %2").arg(QStringLiteral("Compressor"), QStringLiteral("Track %1").arg(i + 1));
            notice.message = tr("Effect disabled. The track continues without it. Recovery will wait until playback and monitoring stop.");
            notice.incident = i + 1;
            notice.actions.push_back({tr("Stop and restore"), [] {}});
            m_audioNotifications->showNotification(std::move(notice));
        }
        connect(&noticeUpdates, &QTimer::timeout, this, [this] {
            ui::Notification notice;
            notice.id = QStringLiteral("motion-notice-3");
            notice.title = tr("Audio device unavailable");
            notice.message = tr("Trying to reconnect the audio device. Any interrupted take has been preserved.");
            notice.incident = 4;
            notice.actions.push_back({tr("Reconnect"), [] {}});
            m_audioNotifications->showNotification(std::move(notice));
        });
        noticeUpdates.start(700);
    }
    if (targetName == "mixer" || targetName == "browser") hideInternalWindow(editor);
    else {
        // The native case exercises the same HWND host as vendor editors; the
        // content remains the deterministic built-in Sampler, not vendor DSP.
        if (targetName == "native") frame->prepareForNativeSurface();
        // Diagnostic A/B switch confined to this test entry point.
        if (overMixer && transparentFrame)
            frame->setAttribute(Qt::WA_OpaquePaintEvent, false);
        frame->resizeForContent(editor->size().expandedTo(editor->minimumSize()));
        frame->move(100, overMixer ? 420 : 130);
        frame->present();
    }
    QPointer<ui::graphics::WorkspaceSurface> surface = findChild<ui::graphics::WorkspaceSurface*>();
    if (ui::graphics::gpuWorkspaceEnabled() && !surface) return false;
    bool failed = false;
    int frames = 0;
    QObject measurementContext;
    if (surface) {
        connect(surface, &ui::graphics::WorkspaceSurface::failed, &measurementContext, [&](const QString&) { failed = true; });
        connect(surface, &ui::graphics::WorkspaceSurface::frameMeasured, &measurementContext,
            [&](double prep, double sync, double render, double interval, double) {
                ++frames;
                ui::perf::sample("motion.scene.prepare.ms", prep);
                ui::perf::sample("motion.scene.sync.ms", sync);
                ui::perf::sample("motion.scene.render.ms", render);
                if (interval > 0 && frames > 1) ui::perf::sample("motion.scene.submission.ms", interval);
            });
    }
    settle(500);
    if (surface && (!surface->quickWindow()->isExposed() || surface->quickWindow()->grabWindow().isNull())) return false;
    // Count damage separately from scene submissions: a native plugin moves
    // through Cocoa/Win32 and need not submit a new frame of the workspace.
    struct PaintProbe final : QObject {
        QWidget* mixer = nullptr;
        QWidget* editor = nullptr;
        bool measuring = false;
        quint64 mixerPaints = 0, editorPaints = 0, mixerPixels = 0;
        bool eventFilter(QObject* object, QEvent* event) override {
            if (!measuring || event->type() != QEvent::Paint) return false;
            auto* widget = qobject_cast<QWidget*>(object);
            if (widget && (widget == mixer || mixer->isAncestorOf(widget))) {
                ++mixerPaints;
                for (const QRect& rect : static_cast<QPaintEvent*>(event)->region())
                    mixerPixels += quint64(rect.width()) * rect.height();
            }
            if (widget && (widget == editor || editor->isAncestorOf(widget))) ++editorPaints;
            return false;
        }
    } paints;
    paints.mixer = m_mixer;
    paints.editor = frame;
    qApp->installEventFilter(&paints);
    if (targetName == "browser") { setBrowserVisible(true); settle(150); }
    QWidget* target = targetName == "mixer" ? m_mixerHandle : targetName == "browser" ? m_browserHandle
        : frame->findChild<QWidget*>("InternalEditorTitleBar");
    if (!target || failed) return false;
    const QPointF anchor = target->mapToGlobal(targetName == "browser" ? QPoint(3, 100)
        : QPoint(120, 2 + (targetName == "mixer" ? 0 : 16)));
    QPointF lastGlobal = anchor;
    const auto sendMouse = [&](QEvent::Type type, QPointF global) {
        // Native plugin frames receive QWidget input directly; other content
        // exercises the Quick-to-Widgets bridge used by the actual workspace.
        const bool throughQuick = surface && targetName != "native";
        const QPointF local = throughQuick ? surface->quickWindow()->mapFromGlobal(global) : target->mapFromGlobal(global);
        QMouseEvent event(type, local, global,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(throughQuick ? static_cast<QObject*>(surface->quickWindow()) : target, &event);
    };
    sendMouse(QEvent::MouseButtonPress, anchor);
    QElapsedTimer travel; travel.start();
    QElapsedTimer inputInterval; inputInterval.start();
    int inputs = 0, changes = 0;
    bool lostExposure = false;
    QTimer gesture;
    gesture.setTimerType(Qt::PreciseTimer);
    connect(&gesture, &QTimer::timeout, this, [&] {
        if (paints.measuring) ui::perf::sample("motion.input.interval.ms", inputInterval.nsecsElapsed() / 1e6);
        if (paints.measuring && surface && !surface->quickWindow()->isExposed()) lostExposure = true;
        inputInterval.restart();
        const double phase = travel.elapsed() * 6.283185307179586 / 1600.;
        lastGlobal = anchor + (targetName == "browser" ? QPointF(std::sin(phase) * 80, 0)
            : targetName == "mixer" ? QPointF(0, std::sin(phase) * 100)
            : QPointF(220 * (1 - std::cos(phase)), 60 * std::sin(phase)));
        QWidget* moving = targetName == "mixer" || targetName == "browser" ? target->parentWidget() : frame;
        const QRect before = moving->geometry();
        ui::perf::Scope cost("motion.input.ms");
        // The overlap benchmark measures repaint work even if another app
        // takes focus during the run. Gesture routing/cancellation is covered
        // by the ordinary motion case and InternalEditorFrame's input checks.
        if (overMixer && (targetName == "native" || targetName == "sampler")) {
            if (transparentFrame)
                frame->setAttribute(Qt::WA_OpaquePaintEvent, false);
            frame->move(QPoint(100, 420) + (lastGlobal - anchor).toPoint());
        } else sendMouse(QEvent::MouseMove, lastGlobal);
        if (before != moving->geometry()) ++changes;
        ++inputs;
    });
    gesture.start(8); settle(1600);
    frames = inputs = changes = 0;
    ui::perf::reset();
    const auto staticBefore = m_timeline->staticFramePaintCountForTest();
    const auto tilesBefore = m_timeline->gpuLaneTileBuildsForTest();
    QElapsedTimer measured; measured.start();
    const double cpuBefore = ui::guiThreadCpuMs();
    paints.measuring = true;
    std::printf("WORKSPACE_MOTION_READY pid=%lld over_mixer=%d\n", qint64(QCoreApplication::applicationPid()), overMixer);
    std::fflush(stdout);
    settle(qEnvironmentVariableIsSet("VLT_MOTION_MS") ? std::clamp(qEnvironmentVariableIntValue("VLT_MOTION_MS"), 2000, 60000) : 4800);
    const double guiCpu = ui::guiThreadCpuMs() - cpuBefore;
    paints.measuring = false;
    gesture.stop(); sendMouse(QEvent::MouseButtonRelease, lastGlobal);
    const auto elapsed = measured.elapsed();
    ui::perf::flush();
    const auto tileBuilds = m_timeline->gpuLaneTileBuildsForTest() - tilesBefore;
    std::printf("WORKSPACE_MOTION_PAINT gui_cpu_ms=%.3f mixer_paints=%llu editor_paints=%llu mixer_logical_pixels=%llu\n",
        guiCpu, static_cast<unsigned long long>(paints.mixerPaints),
        static_cast<unsigned long long>(paints.editorPaints), static_cast<unsigned long long>(paints.mixerPixels));
    std::printf("WORKSPACE_MOTION target=%s backend=%s tracks=%zu inputs=%d changes=%d frames=%d elapsed_ms=%lld timeline_static_paints=%llu lane_tile_builds=%llu\n",
        targetName.toUtf8().constData(), surface ? "gpu" : "widgets", project.tracks.size(), inputs, changes, frames,
        static_cast<long long>(elapsed), static_cast<unsigned long long>(m_timeline->staticFramePaintCountForTest() - staticBefore),
        static_cast<unsigned long long>(tileBuilds));
    if (lostExposure || (overMixer && targetName == "native" && !paints.editorPaints)) {
        std::fprintf(stderr, "Motion benchmark lost native exposure; discard these timings\n");
        return false;
    }
    // Release can commit a final layout after the last submitted GPU frame.
    // Pixel probes below compare the settled presentation with widget geometry.
    settle(150);
    if (const auto shot = qEnvironmentVariable("VLT_MOTION_SCREENSHOT"); !shot.isEmpty()) {
        if (surface && !failed) surface->quickWindow()->grabWindow().save(shot);
        else centralWidget()->grab().save(shot);
    }
    if (surface && !failed && (targetName == "mixer" || targetName == "browser")) {
        if (tileBuilds != 0) {
            std::fprintf(stderr, "Panel resize discarded unchanged lane geometry\n");
            return false;
        }
    }
    if (surface && !failed && targetName == "browser") {
        auto* scroll = findChild<QScrollArea*>(QStringLiteral("InspectorScrollArea"));
        if (!scroll || !scroll->widget()) return false;
        const QImage workspace = surface->quickWindow()->grabWindow();
        // QScrollArea enables autoFillBackground on its content. Probe the
        // empty left margin, where that inherited theme color must be visible.
        const QPointF probe = surface->quickWindow()->mapFromGlobal(
            scroll->viewport()->mapToGlobal(QPoint(2, 100)));
        const QPoint pixel(int(std::floor(probe.x() * workspace.devicePixelRatio())),
                           int(std::floor(probe.y() * workspace.devicePixelRatio())));
        const QColor expected = scroll->widget()->palette().color(scroll->widget()->backgroundRole());
        if (!workspace.rect().contains(pixel) ||
            (expected != th().background && expected != th().surface) ||
            workspace.pixelColor(pixel) != expected) {
            std::fprintf(stderr, "Inspector background differs from its theme at %d,%d: expected %s, got %s\n",
                pixel.x(), pixel.y(), expected.name().toUtf8().constData(),
                workspace.rect().contains(pixel) ? workspace.pixelColor(pixel).name().toUtf8().constData() : "outside");
            return false;
        }
        auto* handle = static_cast<ui::ResizeHandle*>(m_browserHandle);
        // Compare reuse with freshly recorded pixels, including both sides of
        // the 512 px tile threshold and expansion into previously clipped lanes.
        for (const int timelineWidth : {820, 600, 518, 512, 508, 516, 760, 940}) {
            const int delta = m_timeline->width() - timelineWidth;
            handle->onDragStart();
            handle->onDrag(m_browserOnLeft ? delta : -delta);
            settle(150);
            const QImage retained = surface->quickWindow()->grabWindow();
            m_timeline->update(); settle(150);
            if (retained != surface->quickWindow()->grabWindow()) {
                std::fprintf(stderr, "Browser resize differs from freshly rendered timeline at width %d\n",
                             m_timeline->width());
                return false;
            }
        }
    }
    if (surface && !failed && targetName == "mixer") {
        // Reveal lanes covered when the retained tiles were built, then compare
        // with a forced full rebuild at exactly the same geometry.
        m_mixerHeight = 180; layoutBottomPanels(); settle(150);
        const QImage retained = surface->quickWindow()->grabWindow();
        m_timeline->update(); settle(150);
        if (retained != surface->quickWindow()->grabWindow()) {
            std::fprintf(stderr, "Mixer reveal differs from freshly rendered timeline\n");
            return false;
        }
    }
    // A native window's motion is presented by the OS. An unchanged Quick
    // workspace is allowed to stay asleep beneath it.
    return !failed && changes > 20 && (!surface || targetName == "native" || frames > 20);
}

// Real-project fixture, using the complete workspace. It never opens a device
// or saves the project. Run against a disposable package copy with isolated prefs.
bool ui::checkProjectTimelinePerformance(const QString& path) {
    if (path.isEmpty()) { std::fprintf(stderr, "--project-scroll-check requires a package path\n"); return false; }
    MainWindow window(false);
    const bool passed = window.checkProjectScrollForTest(path);
    window.endRecoverySessionForTest();
    return passed;
}
bool MainWindow::checkProjectScrollForTest(const QString& path) {
    const unsigned audioBlock = qEnvironmentVariableIntValue("VLT_SCROLL_AUDIO_BLOCK");
    if (audioBlock && audioBlock != 32 && audioBlock != 64 && audioBlock != 128 && audioBlock != 512) return false;
    if (audioBlock && !m_controller.setBufferSizeFrames(audioBlock)) return false;
    const auto loaded = m_controller.openProject(path.toStdString());
    if (!loaded) { std::fprintf(stderr, "Project load failed: %s\n", loaded.message().c_str()); return false; }
    const double audioRate = m_controller.sampleRate();
    syncViews();
    if (qEnvironmentVariableIntValue("VLT_SCROLL_HIDE_MIXER")) setMixerVisible(false);
    m_timeline->setFollowPlayhead(false);
    resize(1600, 960);
    show();
    raise();
    activateWindow();
    ui::FrameClock::instance().setPreference(ui::FrameMode::Display, 60);
    const auto run = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    run(1200);
    QPointer<ui::graphics::WorkspaceSurface> surface = findChild<ui::graphics::WorkspaceSurface*>();
    if (ui::graphics::gpuWorkspaceEnabled() && !surface) {
        std::fprintf(stderr, "GPU workspace fell back before measurement\n"); return false;
    }
    if (surface) {
        surface->quickWindow()->requestActivate();
        if (!surface->quickWindow()->isExposed()) run(500);
        if (!surface->quickWindow()->isExposed()) {
            std::fprintf(stderr, "GPU window is not exposed; refusing an invalid frame benchmark\n");
            return false;
        }
    }
    if (qEnvironmentVariableIntValue("VLT_SCROLL_PLUGIN_EDITOR")) {
        QPointer<PluginEditorWindow> nativeEditor;
        for (const auto& track : m_controller.project().tracks) {
            for (const auto& insert : track.inserts) {
                auto editor = m_controller.insertEditorSnapshot(track.id, insert.id);
                if (!editor) {
                    run(100); // Subscribe before measuring; the first process readout is asynchronous.
                    editor = m_controller.insertEditorSnapshot(track.id, insert.id);
                }
                if (!editor || !editor->hasEditor) continue;
                const auto channel = QString::fromStdString(track.id), slot = QString::fromStdString(insert.id);
                openPluginEditor(channel, slot);
                nativeEditor = m_pluginEditors.value(channel + '/' + slot);
                break;
            }
            if (nativeEditor) break;
        }
        run(900);
        InternalEditorFrame* frame = nativeEditor
            ? m_internalEditorFrames.value(nativeEditor, nullptr)
            : nullptr;
        if (!nativeEditor || !nativeEditor->isEditorInitialized() || !nativeEditor->isEmbedded() ||
            nativeEditor->isWindow() || !nativeEditor->isVisible() || !frame ||
            frame->isWindow() || !frame->isVisible() || !surface ||
            !surface->quickWindow()->isVisible()) {
            std::fprintf(stderr,
                         "Native plugin did not open inside the GPU workspace\n");
            return false;
        }
        std::printf("PROJECT_SCROLL_NATIVE_EDITOR initialized=1 embedded=1 internal_window=1 gpu_active=1\n");
    }
    bool failed = false, measuring = false;
    std::vector<double> intervals, preparation, synchronization, renderCpu;
    double renderTotal = 0;
    QMetaObject::Connection measured, fallback;
    if (surface) {
        fallback = connect(surface.data(), &ui::graphics::WorkspaceSurface::failed, this,
                           [&](const QString&) { failed = true; });
        measured = connect(surface.data(), &ui::graphics::WorkspaceSurface::frameMeasured, this,
            [&](double prep, double sync, double, double interval, double cpu) {
                if (!measuring) return;
                preparation.push_back(prep); synchronization.push_back(sync);
                if (interval > 0) intervals.push_back(interval);
                if (cpu >= 0) { renderCpu.push_back(cpu); renderTotal += cpu; }
            });
    }
    // Optional actual project DSP with an inaudible, synthetic device callback.
    // No hardware is opened, and callback overruns are not hardware xruns.
    std::atomic<bool> audioActive{false};
    std::atomic<bool> measureAudio{false};
    std::vector<double> callbacks;
    callbacks.reserve(100000);
    std::thread audioThread;
    if (audioBlock) {
        m_controller.configureAudioWorkersForTest(true);
        m_controller.seekSeconds(35);
        m_controller.play();
        audioActive.store(true);
        audioThread = std::thread([&] {
            daw::engine::AudioWorkerRegistration registration;
            registration.configure({true, audioRate, audioBlock, {}});
            audio::AudioBuffer input(2, audioBlock), output(2, audioBlock);
            input.clear();
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(double(audioBlock) / audioRate));
            while (audioActive.load(std::memory_order_relaxed)) {
                const auto start = std::chrono::steady_clock::now();
                m_controller.processDeviceBlockForTest(input, output, audioBlock);
                if (measureAudio.load(std::memory_order_relaxed) && callbacks.size() < callbacks.capacity())
                    callbacks.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                std::this_thread::sleep_until(start + period);
            }
        });
    }
    QTimer gesture;
    gesture.setTimerType(Qt::PreciseTimer); gesture.setInterval(8);
    QElapsedTimer travelClock;
    int previous = 0, inputs = 0;
    const int distance = qEnvironmentVariableIsSet("VLT_SCROLL_DISTANCE") ?
        std::clamp(qEnvironmentVariableIntValue("VLT_SCROLL_DISTANCE"), 100, 20000) : 1800;
    connect(&gesture, &QTimer::timeout, this, [&] {
        const auto phase = travelClock.elapsed() % 1200;
        const int position = -int(std::lround(distance * (phase <= 600 ? phase : 1200 - phase) / 600.));
        const int delta = position - previous; previous = position;
        if (!delta) return;
        ++inputs;
        // Use the visible upper timeline (the mixer may cover its lower half)
        // and exercise the native Quick input adapter as a real gesture does.
        const QPointF at(m_timeline->width() * .5, 60);
        const auto global = m_timeline->mapToGlobal(at);
        const auto local = surface ? surface->quickWindow()->mapFromGlobal(global) : at;
        QWheelEvent wheel(local, global, QPoint(delta, 0), {}, Qt::NoButton,
                          Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(surface ? static_cast<QObject*>(surface->quickWindow()) : m_timeline, &wheel);
    });
    travelClock.start(); gesture.start(); run(2400); // warm one complete out/back path twice
    ui::perf::reset();
    const auto before = ui::guiThreadCpuMs();
    QElapsedTimer measuredTime; measuredTime.start();
    const int oldInputs = inputs;
    measuring = true;
    measureAudio.store(true);
    std::printf("PROJECT_SCROLL_READY pid=%lld backend=%s tracks=%zu viewport=%dx%d dpr=%.1f distance=%d\n",
        qint64(QCoreApplication::applicationPid()), surface ? "gpu" : "widgets", m_controller.project().tracks.size(),
        m_timeline->width(), m_timeline->height(), devicePixelRatioF(), distance);
    std::fflush(stdout);
    run(qEnvironmentVariableIsSet("VLT_SCROLL_MS") ? std::clamp(qEnvironmentVariableIntValue("VLT_SCROLL_MS"), 2000, 60000) : 7200);
    const auto guiCpu = ui::guiThreadCpuMs() - before;
    const double elapsedMs = measuredTime.nsecsElapsed() / 1e6;
    measureAudio.store(false);
    measuring = false; gesture.stop();
    audioActive.store(false);
    if (audioThread.joinable()) { audioThread.join(); m_controller.stop(); }
    const auto percentile = [](std::vector<double> values, double percentile) {
        if (values.empty()) return -1.;
        std::sort(values.begin(), values.end());
        return values[std::min(values.size()-1, std::size_t(values.size() * percentile))];
    };
    std::printf("PROJECT_SCROLL_RESULT inputs=%d frames=%zu elapsed_ms=%.3f gui_cpu_ms=%.3f render_cpu_ms=%.3f prepare_p95=%.3f sync_p95=%.3f render_cpu_p95=%.3f submission_p50=%.3f submission_p95=%.3f submission_p99=%.3f gaps_over_25ms=%zu\n",
        inputs-oldInputs, intervals.size(), elapsedMs, guiCpu, renderTotal, percentile(preparation,.95),
        percentile(synchronization,.95), percentile(renderCpu,.95), percentile(intervals,.50), percentile(intervals,.95), percentile(intervals,.99),
        std::size_t(std::count_if(intervals.begin(), intervals.end(), [](double value) { return value > 25.; })));
    if (audioBlock) std::printf("PROJECT_SCROLL_AUDIO block=%u callbacks=%zu p95_ms=%.3f over_budget=%zu\n",
        audioBlock, callbacks.size(), percentile(callbacks, .95),
        std::size_t(std::count_if(callbacks.begin(), callbacks.end(), [&](double ms) { return ms > audioBlock * 1000. / audioRate; })));
    ui::perf::flush();
    if (const auto shot = qEnvironmentVariable("VLT_SCROLL_SCREENSHOT"); !shot.isEmpty()) {
        if (surface && !failed) surface->quickWindow()->grabWindow().save(shot);
        else centralWidget()->grab().save(shot);
    }
    disconnect(measured); disconnect(fallback);
    if (surface && intervals.empty()) {
        std::fprintf(stderr, "GPU window delivered no frames; benchmark is invalid\n");
        return false;
    }
    return !failed && inputs-oldInputs > 20;
}
