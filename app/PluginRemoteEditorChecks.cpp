#include "MainWindow.hpp"
#include "PluginEditorWindow.hpp"
#include "MixerWidget.hpp"
#include "PluginPickerMenu.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <cstdio>
#include <cwchar>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace {
struct FixtureWindows { DWORD process; HWND window = nullptr; int count = 0; };
FixtureWindows fixtureWindows(DWORD process) {
    FixtureWindows result{process};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& result = *reinterpret_cast<FixtureWindows*>(data);
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (!result.process || process != result.process) return TRUE;
        bool fixture = false;
        EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
            wchar_t title[64]{}; GetWindowTextW(child, title, 64);
            if (std::wcscmp(title, L"DAW fixture editor") != 0) return TRUE;
            *reinterpret_cast<bool*>(data) = true; return FALSE;
        }, reinterpret_cast<LPARAM>(&fixture));
        if (fixture) { result.window = window; ++result.count; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
}
#endif
bool MainWindow::checkRemoteEditorForTest(const std::string& fixturePath) {
#ifndef Q_OS_WIN
    (void)fixturePath; return false;
#else
    if (QApplication::platformName() != QStringLiteral("windows")) return false;
    const auto track = m_controller.addTrack(daw::TrackKind::Audio, "Remote editor check");
    daw::plugins::PluginDescriptor descriptor;
    descriptor.format = daw::plugins::Format::Clap;
    descriptor.path = fixturePath; descriptor.uid = "com.daw.test.fault.editor";
    descriptor.name = "Remote editor fixture";
    const auto slot = m_controller.addInsert(track, descriptor);
    if (slot.empty()) return false;
    onTracksChanged(); setMixerVisible(true);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setGeometry(-16000, -16000, 1000, 700); show();
    const auto channel = QString::fromStdString(track), insert = QString::fromStdString(slot);
    const auto key = channel + '/' + insert;
    const auto identity = m_controller.insertIdentity(track, slot);
    const auto wait = [&](int milliseconds) {
        QEventLoop loop; QTimer pump;
        QObject::connect(&pump, &QTimer::timeout, &loop, [&] {
            m_controller.pumpPreviewPluginEvents(); serviceRemotePluginEditors();
        });
        pump.start(5); QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit); loop.exec();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    const auto until = [&](const auto& condition) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 5000) wait(20);
        return condition();
    };
    const auto check = [](bool condition, const char* description) {
        std::fprintf(stderr, "%s remote editor: %s\n", condition ? "PASS" : "FAIL", description);
        return condition;
    };
    const auto windows = [&] {
        const auto snapshot = m_controller.insertEditorSnapshot(track, slot);
        return fixtureWindows(snapshot ? DWORD(snapshot->processId) : 0);
    };
    const auto oneWindow = [&] {
        const auto native = windows();
        const auto snapshot = m_controller.insertEditorSnapshot(track, slot);
        return native.count == 1 && IsWindowVisible(native.window) && !IsIconic(native.window) &&
            GetParent(native.window) == nullptr && snapshot && snapshot->open &&
            !m_pluginEditors.contains(key) && m_remotePluginEditors.contains(key);
    };
    openPluginEditor(channel, insert);
    if (!check(until(oneWindow), "first click opens one helper window without an app wrapper")) return false;
    QMenu controlsMenu(this);
    auto* controls = ui::addPluginControlsAction(&controlsMenu, &m_controller, channel, insert);
    if (!check(controls != nullptr, "shared slot menu offers parameters and routing")) return false;
    controls->trigger();
    bool ok = check(until([&] {
        auto* panel = m_pluginEditors.value(key, nullptr);
        if (!panel || !panel->isEditorInitialized()) return false;
        auto* dock = panel->findChild<QWidget*>(QStringLiteral("PluginParamDock"));
        auto* mode = panel->findChild<QComboBox*>(QStringLiteral("PluginMode"));
        auto* sidechain = panel->findChild<QComboBox*>(QStringLiteral("PluginSidechain"));
        return dock && dock->isVisible() && mode && sidechain &&
            mode->findData(int(daw::PluginChannelMode::DualMono)) >= 0 &&
            !m_remotePluginEditors.contains(key) && windows().count == 0;
    }), "host controls replace the helper window and retain routing plus parameter dock");
    QPushButton* openNative = nullptr;
    if (!check(until([&] {
        auto* panel = m_pluginEditors.value(key, nullptr);
        openNative = panel ? panel->findChild<QPushButton*>(QStringLiteral("OpenIsolatedPlugin")) : nullptr;
        return openNative && openNative->isVisible() && openNative->isEnabled();
    }), "host controls offer an enabled return to the plugin editor")) return false;
    openNative->click();
    ok &= check(until(oneWindow), "returning to plugin editor closes the host controls panel");
    const auto first = windows().window;
    const QPoint at(m_mixer->width() - 4, m_mixer->height() - 4);
    QMouseEvent press(QEvent::MouseButtonPress, at, m_mixer->mapToGlobal(at),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_mixer, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, m_mixer->mapToGlobal(at),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_mixer, &release);
    m_mixer->setFocus(Qt::MouseFocusReason);
    openPluginEditor(channel, insert);
    ok &= check(until(oneWindow) && windows().window == first,
                    "mixer click then slot click presents the existing helper");
    ShowWindow(first, SW_MINIMIZE); openPluginEditor(channel, insert);
    ok &= check(until(oneWindow), "reopening restores a minimized editor");
    PostMessageW(windows().window, WM_CLOSE, 0, 0);
    openPluginEditor(channel, insert);
    const bool reopenedImmediately = until(oneWindow);
    // Keep pumping after the open acknowledgment: a queued close of the old
    // native window must not close the view which just accepted the reopen.
    wait(100);
    ok &= check(reopenedImmediately && oneWindow() &&
                m_controller.insertIdentity(track, slot) == identity,
                "immediate reopen survives an already queued native close");
    PostMessageW(windows().window, WM_CLOSE, 0, 0);
    ok &= check(until([&] { return !m_remotePluginEditors.contains(key); }),
                "native close retires its app registry entry");
    openPluginEditor(channel, insert);
    ok &= check(until(oneWindow) && m_controller.insertIdentity(track, slot) == identity,
                "closed GUI reopens without replacing its DSP instance");
    const bool dualMono = m_controller.setInsertChannelMode(track, slot, daw::PluginChannelMode::DualMono);
    ok &= check(dualMono, "fixture supports independent dual-mono instances");
    if (dualMono) {
        m_controller.setInsertEditorChannel(track, slot, daw::PluginEditorChannel::Left);
        openPluginEditor(channel, insert);
        ok &= check(until(oneWindow), "dual-mono left editor opens once");
        const auto left = m_controller.insertEditorSnapshot(track, slot, daw::AudioRuntimeEndpoint::Readout::Current);
        m_controller.setInsertEditorChannel(track, slot, daw::PluginEditorChannel::Right);
        // Deliberately do not service the registry between changing sides and
        // opening the new view: replacing the key must retire its old owner.
        openPluginEditor(channel, insert);
        ok &= check(until([&] {
            const auto right = m_controller.insertEditorSnapshot(track, slot);
            return left && right && left->identity != right->identity &&
                fixtureWindows(DWORD(left->processId)).count == 0 && oneWindow();
        }), "switching mono sides closes the previous helper before replacing its registry entry");
    }
    const auto faultedIdentity = m_controller.insertIdentity(track, slot);
    m_controller.setInsertParameter(track, slot, "0", 1);
    using State = daw::EngineController::PluginRuntimeState;
    ok &= check(until([&] {
        return m_controller.insertRuntimeStatus(track, slot).state == State::Failed &&
            m_pluginEditors.contains(key) && !m_remotePluginEditors.contains(key);
    }), "a crashed helper exposes the recovery panel");
    QPushButton* restart = nullptr;
    ok &= check(until([&] {
        auto* panel = m_pluginEditors.value(key, nullptr);
        restart = panel ? panel->findChild<QPushButton*>(QStringLiteral("RestartIsolatedPlugin")) : nullptr;
        return restart && restart->isVisible() && restart->isEnabled();
    }), "recovery remains available after the GUI process crashes");
    if (!restart) return false;
    restart->click();
    ok &= check(until([&] {
        return m_controller.insertRuntimeStatus(track, slot).state == State::Running &&
            m_controller.insertIdentity(track, slot) != faultedIdentity;
    }), "plugin restarts with a new instance identity");
    ok &= check(until(oneWindow), "restart restores one editor and removes the recovery wrapper");
    const auto snapshot = m_controller.insertEditorSnapshot(track, slot);
    const auto process = snapshot ? DWORD(snapshot->processId) : 0;
    closeInternalWindows();
    ok &= check(until([&] { return fixtureWindows(process).count == 0; }) && m_remotePluginEditors.isEmpty(),
                "closing project editors also closes the helper GUI");
    return ok;
#endif
}
