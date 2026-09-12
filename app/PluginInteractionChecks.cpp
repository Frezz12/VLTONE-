#include "UiPerformanceChecks.hpp"
#include "ChannelStrip.hpp"
#include "PluginEditorWindow.hpp"
#include "InternalEditorFrame.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "graphics/WorkspaceSurface.hpp"
#ifdef DAW_ENABLE_VST
#include "Vst/VstFactory.hpp"
#endif
#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <cstdio>

bool ui::checkPluginInteractions() {
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
    daw::EngineController controller;
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
