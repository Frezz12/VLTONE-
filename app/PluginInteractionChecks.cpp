#include "UiPerformanceChecks.hpp"
#include "ChannelStrip.hpp"
#include "PluginEditorWindow.hpp"
#include "InternalEditorFrame.hpp"
#include "MainWindow.hpp"
#include "FileBrowserPanel.hpp"
#include "InspectorWidget.hpp"
#include "MixerWidget.hpp"
#include "TimelineWidget.hpp"
#include "TrackListWidget.hpp"
#include "EngineController.hpp"
#include "TypingKeyboard.hpp"
#include "Theme.hpp"
#include "SamplerPanel.hpp"
#include "Controls.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include <QCheckBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <cmath>
#include "graphics/GraphicsPreferences.hpp"
#include "graphics/WorkspaceSurface.hpp"
#ifdef DAW_ENABLE_VST
#include "Vst/VstFactory.hpp"
#endif
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QDir>
#include <QPainter>
#include <QAbstractButton>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPointer>
#include <QPixmap>
#include <QQuickWindow>
#include <QTimer>
#include <QScopeGuard>
#include <cstdio>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

bool MainWindow::checkInspectorNormalizeForTest() {
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    const auto path = directory.filePath("quiet.wav").toStdString();
    audio::AudioBuffer tone(2, 96000);
    for (audio::BufferSize frame = 0; frame < 96000; ++frame) {
        const float value = 0.2f * std::sin(float(frame) * 0.0576f);
        tone.getChannel(0)[frame] = tone.getChannel(1)[frame] = value;
    }
    audio::AudioRecorder recorder;
    recorder.initialize(48000, 2);
    if (!recorder.writeWAVFile(path, tone, 48000).isOk()) return false;
    const auto track = m_controller.addTrack(daw::TrackKind::Audio, "Normalize");
    const auto clip = m_controller.importAudio(path, track, 0.0);
    const auto copy = m_controller.duplicateClip(track, clip);
    const auto id = QString::fromStdString(track);
    syncViews();
    m_selectedTrackId = id;
    m_selection.setClips({{id, QString::fromStdString(clip)}});
    m_timeline->selectClips({{id, QString::fromStdString(clip)}});
    m_inspector->setSelection(id, QString::fromStdString(clip));
    setMixerShownForShot(false);
    resize(1280, 850); show();
    const auto wait = [](int ms) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    wait(300);
    auto* more = m_inspector->findChild<QToolButton*>("InspectorMoreClipSettings");
    auto* toggle = m_inspector->findChild<QCheckBox*>("ClipParameter_pre.normalize");
    auto* scroll = m_inspector->findChild<QScrollArea*>("InspectorScrollArea");
    if (!more || !toggle || !scroll) return false;
    more->setChecked(true); wait(60);
    scroll->ensureWidgetVisible(toggle, 0, 18); wait(300);
    auto* surface = centralWidget()->findChild<ui::graphics::WorkspaceSurface*>();
    const auto click = [&](QWidget* button) {
        const QPoint global = button->mapToGlobal(button->rect().center());
        const QPoint at = surface ? surface->quickWindow()->mapFromGlobal(global) : button->rect().center();
        QObject* receiver = surface ? static_cast<QObject*>(surface->quickWindow()) : button;
        QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &press); QApplication::sendEvent(receiver, &release);
        wait(350);
    };
    const auto envelope = [&](const std::string& clipId) {
        return m_controller.clipWaveform(*m_controller.audioClip(track, clipId));
    };
    const auto peak = [](const daw::WaveformPeaks* peaks) {
        float result = 0;
        if (peaks) for (const float value : peaks->maxima) result = std::max(result, value);
        return result;
    };
    const auto picture = [&] {
        if (!surface) return m_timeline->grab().toImage();
        const auto scene = surface->quickWindow()->grabWindow();
        const auto origin = surface->quickWindow()->mapFromGlobal(m_timeline->mapToGlobal(QPoint{}));
        const auto dpr = scene.devicePixelRatio();
        return scene.copy(QRect(qRound(origin.x() * dpr), qRound(origin.y() * dpr),
            qRound(m_timeline->width() * dpr), qRound(m_timeline->height() * dpr)));
    };
    const auto shots = qEnvironmentVariable("DAW_NORMALIZE_TEST_SHOTS");
    const auto save = [&](const QString& name) {
        if (shots.isEmpty()) return true;
        QDir().mkpath(shots);
        return (surface ? surface->quickWindow()->grabWindow() : grab().toImage())
            .save(shots + '/' + name + ".png");
    };
    bool ok = true;
    const auto check = [&](bool valid, const char* label) {
        std::fprintf(stderr, "%s normalize: %s\n", valid ? "PASS" : "FAIL", label);
        ok &= valid;
    };
    check(peak(envelope(clip).peaks) > .19f && peak(envelope(clip).peaks) < .21f,
          "initial waveform matches the quiet file");
    const auto originalPicture = picture();
    const auto undoDepth = m_controller.undoDepth();
    check(save("before"), "before screenshot");
    click(toggle);
    // Wait for the ordinary control-thread tick, without forcing a bake via
    // clipSampleData(). Native GPU startup can delay its first delivery.
    QElapsedTimer publicationWait;
    publicationWait.start();
    while (peak(envelope(clip).peaks) < .99f && publicationWait.elapsed() < 3000)
        wait(20);
    const auto normalized = envelope(clip);
    // Keep the editor's sample alive across Undo/Redo to exercise shared-cache
    // reuse even after the last clip drops its processed envelope.
    const auto samplerAudio = m_controller.clipSampleData(track, clip);
    check(toggle->isChecked() && m_controller.undoDepth() == undoDepth + 1,
          "scrolled Inspector click enables Normalize in one undo step");
    check(peak(normalized.peaks) > .99f && normalized.samples && samplerAudio &&
          normalized.samples == samplerAudio->audio.get(),
          "waveform uses the same normalized audio as Sampler and playback");
    check(!originalPicture.isNull() && originalPicture != picture(),
          "timeline pixels change without opening Sampler or moving the clip");
    check(peak(envelope(copy).peaks) < .21f &&
          peak(m_controller.waveforms().cached(path)) < .21f,
          "other clips and the source envelope remain unchanged");
    check(save("normalized"), "normalized screenshot");
    onUndo(); wait(350);
    check(!toggle->isChecked() && peak(envelope(clip).peaks) < .21f && originalPicture == picture(),
          "Undo restores both the switch and original waveform pixels");
    onRedo(); wait(350);
    check(toggle->isChecked() && peak(envelope(clip).peaks) > .99f,
          "Redo restores the normalized waveform");
    click(toggle);
    check(!toggle->isChecked() && peak(envelope(clip).peaks) < .21f,
          "switching Normalize off restores the source waveform");
    // The editor's other entry point must publish to the arrangement too.
    SamplerPanel sampler(&m_controller, SamplerPanel::Context::Clip, id, QString::fromStdString(clip));
    auto* samplerToggle = sampler.findChild<ui::Led*>("SamplerParameter.pre.normalize");
    if (!samplerToggle) return false;
    samplerToggle->click(); wait(350);
    check(peak(envelope(clip).peaks) > .99f && originalPicture != picture(),
          "Sampler Normalize also refreshes the arrangement");
    return ok;
}

bool MainWindow::checkPluginSidechainForTest() {
    auto& controller = m_controller;
    const auto descriptor = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.compressor");
    if (!descriptor) return false;
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Bass");
    const auto kick = controller.addTrack(daw::TrackKind::Audio, "Kick");
    const auto snare = controller.addTrack(daw::TrackKind::Audio, "Snare");
    const auto percussion = controller.addTrack(daw::TrackKind::Audio, "Percussion");
    const auto slot = controller.addInsert(track, *descriptor);
    syncViews(); resize(1280, 860); show(); hide(); show(); raise(); activateWindow();
    openPluginEditor(QString::fromStdString(track), QString::fromStdString(slot));
    auto* editor = m_pluginEditors.value(QString::fromStdString(track + '/' + slot));
    auto* frame = editor ? m_internalEditorFrames.value(editor) : nullptr;
    if (!frame) return false;
    const auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    wait(1500);
    auto* combo = editor->findChild<QComboBox*>("PluginSidechain");
    auto* wrapper = editor->findChild<QWidget*>("PluginWrapper");
    if (!combo || !wrapper) return false;
    auto* surface = centralWidget()->findChild<ui::graphics::WorkspaceSurface*>();
    const QPoint openPoint = combo->rect().center();
    const QPoint globalPoint = combo->mapToGlobal(openPoint);
    const QPoint inputPoint = surface ? surface->quickWindow()->mapFromGlobal(globalPoint) : openPoint;
    QObject* input = surface ? static_cast<QObject*>(surface->quickWindow()) : combo;
    QMouseEvent openPress(QEvent::MouseButtonPress, inputPoint, globalPoint, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent openRelease(QEvent::MouseButtonRelease, inputPoint, globalPoint, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(input, &openPress); QApplication::sendEvent(input, &openRelease); wait(100);
    auto* menu = editor->findChild<QMenu*>("PluginSidechainMenu");
    if (!menu) return false;
    const auto actionFor = [&](const std::string& id) -> QAction* {
        for (auto* action : menu->actions())
            if (action->isCheckable() && action->data().toString().toStdString() == id) return action;
        return nullptr;
    };
    const auto click = [&](QAction* action) {
        if (!action) return false;
        const QPoint point = menu->actionGeometry(action).center();
        QMouseEvent press(QEvent::MouseButtonPress, point, menu->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, menu->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(menu, &press); QApplication::sendEvent(menu, &release); wait(40);
        return true;
    };
    const auto selected = [&] { return controller.insertModel(track, slot)->sidechainTrackIds; };
    bool ok = true;
    const auto check = [&](bool valid, const char* what) {
        if (!valid) std::fprintf(stderr, "FAIL sidechain %s (selected=%zu, popup=%d, header=%d)\n",
            what, selected().size(), menu->isVisible(), wrapper->height());
        ok &= valid;
    };
    check(wrapper->height() == 28 && !actionFor(track), "thin header and no self route");
    check(click(actionFor(kick)) && menu->isVisible() && selected() == std::vector<std::string>{kick}, "first mouse selection");
    check(click(actionFor(snare)) && menu->isVisible() && selected() == std::vector<std::string>{kick, snare}, "second mouse selection");
    check(combo->toolTip().contains("Kick") && combo->toolTip().contains("Snare") && combo->currentText().contains("2"), "count and source tooltip");
    controller.undo(); editor->pollForTest();
    check(selected() == std::vector<std::string>{kick} && !actionFor(snare)->isChecked(), "undo updates checks");
    controller.redo(); editor->pollForTest();
    check(selected() == std::vector<std::string>{kick, snare} && actionFor(snare)->isChecked(), "redo updates checks");
    const auto screenshots = qEnvironmentVariable("DAW_SIDECHAIN_TEST_SHOTS");
    if (!screenshots.isEmpty()) {
        QDir().mkpath(screenshots);
        wait(200);
        QImage picture;
        if (surface) {
            const auto scene = surface->quickWindow()->grabWindow();
            const auto origin = surface->quickWindow()->mapFromGlobal(frame->mapToGlobal(QPoint(0, 0)));
            const auto dpr = scene.devicePixelRatio();
            picture = scene.copy(QRect(qRound(origin.x() * dpr), qRound(origin.y() * dpr),
                qRound(frame->width() * dpr), qRound(frame->height() * dpr)));
            picture.setDevicePixelRatio(dpr);
        } else picture = frame->grab().toImage();
        const auto popup = menu->grab().toImage();
        if (!picture.isNull()) {
            QPainter painter(&picture);
            painter.drawImage(frame->mapFromGlobal(menu->pos()), popup);
            painter.end();
        }
        check(!picture.isNull() && picture.pixelColor(picture.width() / 2,
            qRound(32 * picture.devicePixelRatio())).lightness() < 180, "rendered header is present in screenshot");
        ok &= picture.save(screenshots + "/sidechain-selection.png");
    }
    menu->setActiveAction(actionFor(kick));
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(menu, &space); wait(40);
    check(menu->isVisible() && selected() == std::vector<std::string>{snare} && !controller.isPlaying(), "Space toggles without starting transport or closing");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(menu, &escape); wait(40);
    check(!menu->isVisible(), "Escape closes");
    controller.renameTrack(snare, "Snare renamed");
    editor->pollForTest();
    check(combo->toolTip().contains("Snare renamed"), "closed header refreshes selected source names");
    const auto added = controller.addTrack(daw::TrackKind::Audio, "Added after editor opened");
    combo->showPopup(); wait(40);
    check(actionFor(added) != nullptr, "opening picker discovers newly added tracks");
    check(click(actionFor({})) && selected().empty() && menu->isVisible(), "Off clears all sources");
    menu->hide();
    std::fprintf(stderr, "%s multi-sidechain UI: pointer, persistent popup, keyboard, undo/redo, 28px header\n", ok ? "PASS" : "FAIL");
    editor->close(); wait(40);
    for (const auto& id : {track, kick, snare, percussion, added}) controller.removeTrack(id);
    return ok;
}

bool MainWindow::checkPluginWindowPolicyForTest() {
    auto descriptor = m_controller.pluginManager().find(
        daw::plugins::Format::Internal, "daw.graphit");
#ifdef DAW_ENABLE_VST
    const QString path = qEnvironmentVariable("VLT_TEST_EDITOR_PLUGIN");
    if (!path.isEmpty()) {
        daw::plugins::VstFactory factory;
        const auto descriptors = factory.inspect(path.toStdString());
        if (descriptors.empty()) return false;
        descriptor = descriptors.front();
    }
#endif
    if (!descriptor) return false;
    const auto track = m_controller.addTrack(daw::TrackKind::Audio, "Window policy");
    const auto slot = m_controller.addInsert(track, *descriptor);
    if (slot.empty()) return false;
    const QString channel = QString::fromStdString(track);
    const QString insert = QString::fromStdString(slot);
    const QString key = channel + '/' + insert;
    syncViews();
    setBrowserVisible(true);
    setInspectorVisible(true);
    setMixerVisible(true);
    resize(1440, 1000);
    show();
    // A Windows test runner can start the process with SW_HIDE. Consume that
    // first-show hint before checking real native mapping and hit testing.
    hide();
    show();
    raise();
    activateWindow();
    const auto settle = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    settle(250);
    auto* surface = findChild<ui::graphics::WorkspaceSurface*>();
    const auto click = [surface](QWidget* target) {
        const QPointF local(16, 12);
        const QPointF global = target->mapToGlobal(local);
        QObject* receiver = surface ? static_cast<QObject*>(surface->quickWindow()) : target;
        const QPointF at = surface ? surface->quickWindow()->mapFromGlobal(global) : local;
        QMouseEvent press(QEvent::MouseButtonPress, at, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &release);
    };
    const auto exposed = [](InternalEditorFrame* frame) {
        if (!frame || !frame->isVisible()) return false;
#ifdef Q_OS_WIN
        if (frame->testAttribute(Qt::WA_NativeWindow)) {
            const HWND handle = reinterpret_cast<HWND>(frame->winId());
            const HWND parent = GetParent(handle);
            RECT bounds;
            if (!parent || !GetWindowRect(handle, &bounds)) return false;
            POINT point{bounds.left + 30, bounds.top + 10};
            ScreenToClient(parent, &point);
            const HWND hit = ChildWindowFromPointEx(parent, point, CWP_SKIPINVISIBLE);
            return hit == handle || IsChild(handle, hit);
        }
#endif
        auto* hit = frame->parentWidget()->childAt(frame->pos() + QPoint(30, 10));
        return hit == frame || (hit && frame->isAncestorOf(hit));
    };
    bool ok = true;
    const auto check = [&ok](bool passed, const char* message) {
        std::fprintf(stderr, "%s plugin window: %s\n", passed ? "PASS" : "FAIL", message);
        ok &= passed;
    };
    openPluginEditor(channel, insert);
    settle(1000);
    QPointer<PluginEditorWindow> editor = m_pluginEditors.value(key);
    auto* frame = editor ? m_internalEditorFrames.value(editor) : nullptr;
    if (!editor || !frame) return false;
    frame->move(700, 200);
    check(editor->isEditorInitialized(), "editor finishes loading");
    check(exposed(frame), "editor opens above the workspace");
    const QString headerShot = qEnvironmentVariable("DAW_PLUGIN_HEADER_SCREENSHOT");
    if (!headerShot.isEmpty()) frame->grab().save(headerShot);
    // Native activation may change the OS stack while QWidget's sibling order
    // still puts the editor last. Reproduce that drift without opening a second
    // plugin, then use the same open request as a mixer/inspector insert button.
    if (frame->testAttribute(Qt::WA_NativeWindow) && frame->windowHandle())
        frame->windowHandle()->lower();
    openPluginEditor(channel, insert);
    settle(50);
    check(m_pluginEditors.value(key) == editor && exposed(frame),
          "reopening the same editor repairs native stacking");
    for (QWidget* panel : std::array<QWidget*, 4>{m_mixer, m_inspector, m_browser, m_trackList}) {
        // Use a focusable child in the real panel hierarchy, without invoking
        // a user's browser folder, mixer button or track-header command.
        QWidget target(panel);
        target.setGeometry(8, 8, 40, 28);
        target.setFocusPolicy(Qt::StrongFocus);
        target.show();
        if (frame->testAttribute(Qt::WA_NativeWindow) && frame->windowHandle())
            frame->windowHandle()->lower();
        target.setFocus(Qt::MouseFocusReason);
        click(&target);
        settle(80);
        check(editor && m_pluginEditors.value(key) == editor && !editor->isClosing() &&
                  exposed(frame) && QApplication::focusWidget() == &target,
              "panel clicks preserve the editor without stealing panel focus");
        check(!m_auxiliaryLowered, "panel clicks do not lower editor windows");
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        click(m_timeline);
        check(!m_pluginEditors.contains(key), "only timeline clicks unregister the editor");
        openPluginEditor(channel, insert);
        settle(1000);
        editor = m_pluginEditors.value(key);
        frame = editor ? m_internalEditorFrames.value(editor) : nullptr;
        check(editor && editor->isEditorInitialized() && exposed(frame),
              "timeline close followed by immediate reopen works repeatedly");
        if (!editor || !frame) break;
        frame->move(700, 200);
        click(m_mixer);
        settle(80);
        check(editor && !editor->isClosing() && exposed(frame),
              "mixer keeps the reopened editor visible after timeline dismissal");
    }
    closeInternalWindows();
    return ok;
}

bool MainWindow::checkPluginKeyboardForTest() {
    auto instrument = m_controller.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
    auto nativeEffect = instrument;
#ifdef DAW_ENABLE_VST
    const QString path = qEnvironmentVariable("VLT_TEST_EDITOR_PLUGIN");
    if (!path.isEmpty()) {
        daw::plugins::VstFactory factory;
        for (const auto& descriptor : factory.inspect(path.toStdString())) {
            if (descriptor.isInstrument) instrument = descriptor;
            if (!descriptor.isInstrument && descriptor.hasEditor) nativeEffect = descriptor;
        }
    }
#endif
    if (!instrument) return false;
    const auto other = m_controller.addTrack(daw::TrackKind::Midi, "Other instrument");
    const auto track = m_controller.addTrack(daw::TrackKind::Midi, "Keyboard instrument");
    if (!m_controller.setTrackInstrumentPlugin(track, *instrument)) return false;
    const QString channel = QString::fromStdString(track);
    const QString insert = QString::fromStdString(m_controller.project().findTrack(track)->instrument.id);
    syncViews();
    m_selection.setTracks({QString::fromStdString(other)});
    const auto settle = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    openPluginEditor(channel, insert);
    settle(1000);
    auto* editor = m_pluginEditors.value(channel + '/' + insert);
    auto* frame = editor ? m_internalEditorFrames.value(editor) : nullptr;
    if (!editor || !frame) return false;
    QApplication::setActiveWindow(this);
    editor->setFocus();
    bool ok = true;
    const auto check = [&ok](bool passed, const char* message) {
        std::fprintf(stderr, "%s plugin keyboard: %s\n", passed ? "PASS" : "FAIL", message);
        ok &= passed;
    };
    check(liveInputTarget() == track, "opening the instrument routes notes despite another selected track");
    check(frame->findChild<QWidget*>("InternalEditorMaximize")->isHidden() &&
          frame->findChild<QWidget*>("InternalEditorDetach")->isHidden() &&
          frame->findChild<QWidget*>("InternalEditorClose")->isVisible(), "only Close remains in plugin chrome");
    frame->setMaximized(true);
    frame->setDetached(true);
    auto* title = frame->findChild<QWidget*>("InternalEditorTitleBar");
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(30, 10),
        title->mapToGlobal(QPoint(30, 10)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(title, &doubleClick);
    check(!frame->isMaximized() && !frame->isDetached(), "plugin expansion is disabled for buttons and double-click");
    setTypingKeyboardEnabled(true);
    const auto key = [](QWidget* target, int code, QEvent::Type type, bool repeat = false,
                        const QString& text = {}) {
        if (type == QEvent::KeyPress) {
            QKeyEvent overrideEvent(QEvent::ShortcutOverride, code, Qt::NoModifier, text, repeat);
            QApplication::sendEvent(target, &overrideEvent);
        }
        QKeyEvent event(type, code, Qt::NoModifier, text, repeat);
        QApplication::sendEvent(target, &event);
    };
    auto* combo = new QComboBox(editor);
    combo->addItems({"First", "Second"});
    combo->setGeometry(12, 42, 90, 24);
    combo->show();
    combo->setFocus();
    key(combo, Qt::Key_Z, QEvent::KeyPress);
    check(m_typingKeyboard->heldCount() == 1 && livePitchesForTrack(channel).any(),
          "notes still play after a non-editable plugin combo receives focus");
    key(combo, Qt::Key_Z, QEvent::KeyRelease);
    m_controller.pause();
    key(combo, Qt::Key_Space, QEvent::KeyPress);
    key(combo, Qt::Key_Space, QEvent::KeyPress, true);
    key(combo, Qt::Key_Space, QEvent::KeyRelease);
    check(m_controller.isPlaying() && !QApplication::activePopupWidget(),
          "Space starts transport once instead of opening a focused combo");
    auto* text = new QLineEdit(editor);
    text->setGeometry(110, 42, 100, 24);
    text->show();
    text->setFocus();
    key(text, Qt::Key_Z, QEvent::KeyPress, false, "z");
    key(text, Qt::Key_Z, QEvent::KeyRelease);
    check(text->text() == "z" && m_typingKeyboard->heldCount() == 0, "text entry keeps letters instead of playing notes");
    key(text, Qt::Key_Space, QEvent::KeyPress);
    key(text, Qt::Key_Space, QEvent::KeyRelease);
    check(!m_controller.isPlaying(), "Space pauses the main transport from a plugin text control");
    {
        QDialog modal(this);
        modal.setWindowModality(Qt::ApplicationModal);
        modal.show();
        key(text, Qt::Key_Space, QEvent::KeyPress);
        key(text, Qt::Key_Space, QEvent::KeyRelease);
        check(!m_controller.isPlaying(), "a modal dialog blocks plugin transport input");
        modal.hide();
    }
    QApplication::setActiveWindow(this);
#ifdef Q_OS_WIN
    if (QApplication::platformName() != QStringLiteral("offscreen")) {
        if (!nativeEffect || !nativeEffect->hasEditor) return false;
        const QString effectSlot = QString::fromStdString(m_controller.addInsert(track, *nativeEffect));
        openPluginEditor(channel, effectSlot);
        settle(1000);
        editor = m_pluginEditors.value(channel + '/' + effectSlot);
        frame = editor ? m_internalEditorFrames.value(editor) : nullptr;
        if (!editor || !frame) return false;
        QWidget* container = nullptr;
        for (auto* child : editor->findChildren<QWidget*>())
            if (child->property("vlt.foreignSurface").toBool()) container = child;
        check(container != nullptr, "native fixture exposes a real plugin host");
        if (!container) return false;
        // A real non-Qt child consumes keys through Win32. Posted messages take
        // the same GetMessage/TranslateMessage path as keyboard input.
        const HWND native = CreateWindowExW(0, L"BUTTON", L"Plugin control",
            WS_CHILD | WS_VISIBLE, 0, 0, 100, 30,
            reinterpret_cast<HWND>(container->winId()), nullptr, GetModuleHandleW(nullptr), nullptr);
        const HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
            100, 0, 100, 30, reinterpret_cast<HWND>(container->winId()), nullptr,
            GetModuleHandleW(nullptr), nullptr);
        const auto cleanup = qScopeGuard([&] { DestroyWindow(native); DestroyWindow(edit); });
        if (!native || !edit) return false;
        const auto post = [&](HWND window, UINT message, WPARAM vk, int scan, bool repeat = false) {
            LPARAM flags = 1 | (LPARAM(scan) << 16);
            if (repeat || message == WM_KEYUP) flags |= LPARAM(1) << 30;
            if (message == WM_KEYUP) flags |= LPARAM(1) << 31;
            PostMessageW(window, message, vk, flags);
            settle(30);
        };
        text->setFocus(); // Qt may still remember a text control after native focus changes.
        SetFocus(native);
        post(native, WM_KEYDOWN, VK_SPACE, 0x39);
        post(native, WM_KEYDOWN, VK_SPACE, 0x39, true);
        post(native, WM_KEYUP, VK_SPACE, 0x39);
        check(m_controller.isPlaying() && GetFocus() == native, "native plugin Space starts once and preserves its focus");
        post(native, WM_KEYDOWN, VK_SPACE, 0x39);
        post(native, WM_KEYUP, VK_SPACE, 0x39);
        check(!m_controller.isPlaying(), "native plugin Space pauses without reopening");
        post(native, WM_KEYDOWN, 'Z', 0x2c);
        post(native, WM_KEYDOWN, 'Z', 0x2c, true);
        check(m_typingKeyboard->heldCount() == 1 && livePitchesForTrack(channel).any(),
              "native notes reach the open instrument despite stale Qt text focus");
        SetFocus(edit);
        post(edit, WM_KEYUP, 'Z', 0x2c);
        check(m_typingKeyboard->heldCount() == 0, "note-off survives moving focus into a native text field");
        post(edit, WM_KEYDOWN, 'Z', 0x2c);
        post(edit, WM_KEYUP, 'Z', 0x2c);
        check(m_typingKeyboard->heldCount() == 0 && GetWindowTextLengthW(edit) > 0,
              "native text entry keeps letters");
        post(edit, WM_KEYDOWN, VK_SPACE, 0x39);
        post(edit, WM_KEYUP, VK_SPACE, 0x39);
        check(m_controller.isPlaying(), "native text control cannot swallow transport Space");
        m_controller.pause();
    }
#endif
    editor->setFocus();
    key(editor, Qt::Key_Z, QEvent::KeyPress);
    frame->findChild<QAbstractButton*>("InternalEditorClose")->click();
    check(m_typingKeyboard->heldCount() == 0, "closing a playing instrument releases held notes");
    setTypingKeyboardEnabled(false);
    closeInternalWindows();
    return ok;
}

bool ui::checkPluginInteractions() {
    {
        MainWindow window(false);
        const bool passed = window.checkPluginWindowPolicyForTest() && window.checkPluginKeyboardForTest();
        window.endRecoverySessionForTest();
        if (!passed) return false;
    }
    if (QApplication::platformName() == QStringLiteral("offscreen"))
        return ChannelStrip::checkDragLifecycleForTest();
#ifndef DAW_ENABLE_VST
    std::fprintf(stderr, "Native editor check requires the VST fixture\n");
    return false;
#else
    // Only load the explicitly supplied test fixture; no scan, device or user
    // project is involved. Run with both VLT_GPU_WORKSPACE=0 and =1.
    const QString path = qEnvironmentVariable("VLT_TEST_EDITOR_PLUGIN");
    if (path.isEmpty()) return false;
    daw::plugins::VstFactory factory;
    const auto descriptors = factory.inspect(path.toStdString());
    if (descriptors.empty()) return false;
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    if (!controller.initialize(48000, 512, false)) return false;
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Editor check");
    const auto slot = controller.addInsert(track, descriptors.front());
    if (slot.empty()) return false;
    ThemeManager::instance().apply();
    QWidget workspace;
    workspace.resize(1050, 720);
    workspace.show();
    std::unique_ptr<graphics::WorkspaceSurface> surface;
    if (graphics::gpuWorkspaceEnabled()) surface = std::make_unique<graphics::WorkspaceSurface>(&workspace);
    auto settle = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    bool ok = true;
    auto check = [&](bool passed, const char* message) {
        std::fprintf(stderr, "%s editor: %s\n", passed ? "PASS" : "FAIL", message);
        ok &= passed;
    };
    int gpuFrames = 0;
    if (surface) {
        QObject::connect(surface.get(), &graphics::WorkspaceSurface::failed, &workspace,
                         [&](const QString&) { check(false, "GPU surface stays active"); });
        QObject::connect(surface.get(), &graphics::WorkspaceSurface::frameMeasured, &workspace,
                         [&] { ++gpuFrames; });
    }
    const auto open = [&]() -> QPointer<PluginEditorWindow> {
        auto* frame = new InternalEditorFrame(QStringLiteral("test/editor"), &workspace);
        auto* editor = new PluginEditorWindow(&controller, QString::fromStdString(track),
                                               QString::fromStdString(slot));
        if (surface) frame->prepareForNativeSurface();
        frame->setContent(editor);
        QObject::connect(editor, &QObject::destroyed, frame, &QObject::deleteLater);
        editor->prepareNativeHostHierarchy();
        frame->present();
        editor->initializeEditor();
        return editor;
    };
    auto editor = open();
    settle(350);
    check(editor && editor->isEmbedded(), "native fixture opens in its final host");
    if (surface) check(gpuFrames > 0 && surface->quickWindow()->isVisible(),
                       "native editor coexists with actual GPU frames");
    for (int i = 0; i < 8 && editor; ++i) {
        QPointer<PluginEditorWindow> next;
        auto* closing = editor.data();
        QObject::connect(closing, &PluginEditorWindow::closing, &workspace, [&] {
            check(!closing->isEmbedded(), "native editor is detached before publishing close");
            next = open();
            // Keep the old closeEvent on the stack. Deferred deletion must
            // not be necessary for opening a new GUI on this same instance.
            settle(350);
            check(next && next->isEmbedded(), "reopen works while old window awaits deletion");
        });
        closing->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        settle(30);
        check(!editor && next && next->isEmbedded(), "old window teardown leaves reopened editor alive");
        editor = next;
    }
    if (editor) {
        editor->onEditorClosed();
        auto* mode = editor->findChild<QComboBox*>(QStringLiteral("PluginMode"));
        check(mode != nullptr, "channel mode control is available");
        if (mode) mode->setCurrentIndex(1);
        settle(350);
        check(editor && !editor->isClosing() && editor->isEmbedded(),
              "a queued close from the old GUI cannot close the rebuilt editor");
    }
    if (editor) editor->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    editor = open();
    QObject::connect(editor, &PluginEditorWindow::closing, &workspace, [&] {
        editor->initializeEditor();
        settle(350);
        check(editor && !editor->isEmbedded() && !editor->isEditorInitialized(),
              "closing during loading cancels every deferred attach");
    });
    editor->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    surface.reset();
    return ok;
#endif
}
