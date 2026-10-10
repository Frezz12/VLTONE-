#include "MainWindow.hpp"
#include "PluginEditorWindow.hpp"
#include "InternalEditorFrame.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
#include <cstdio>

bool MainWindow::checkEmbeddedEditorForTest(const std::string& fixturePath) {
    auto* factory = daw::plugins::factoryFor(daw::plugins::Format::Vst);
    if (!factory) return false;
    const auto plugins = factory->inspect(fixturePath);
    const auto descriptor = std::find_if(plugins.begin(), plugins.end(),
        [](const auto& plugin) { return plugin.uid == "54465831"; });
    if (descriptor == plugins.end()) return false;
    const auto track = m_controller.addTrack(daw::TrackKind::Audio, "Embedded editor");
    const auto slot = m_controller.addInsert(track, *descriptor);
    if (slot.empty()) return false;
    onTracksChanged();
    resize(1000, 700); show();
    const auto channel = QString::fromStdString(track), insert = QString::fromStdString(slot);
    const auto key = channel + '/' + insert;
    const auto identity = m_controller.insertIdentity(track, slot);
    const auto until = [&](const auto& condition) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 3000) {
            QEventLoop loop;
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return condition();
    };
    const auto attached = [&] {
        const auto* editor = m_pluginEditors.value(key);
        const auto* frame = editor ? qobject_cast<InternalEditorFrame*>(editor->parentWidget()) : nullptr;
        const auto snapshot = m_controller.insertEditorSnapshot(track, slot);
        return editor && editor->isEmbedded() && frame && !frame->isWindow() &&
            isAncestorOf(frame) && snapshot &&
            snapshot->identity == identity;
    };
    openPluginEditor(channel, insert);
    if (!until(attached)) return false;
    auto* original = m_pluginEditors.value(key);
    openPluginEditor(channel, insert);
    if (!attached() || m_pluginEditors.value(key) != original) return false;
    original->close();
    if (!until([&] { return !m_pluginEditors.contains(key); })) return false;
    openPluginEditor(channel, insert);
    if (!until(attached)) return false;
    m_pluginEditors.value(key)->close();
    std::fprintf(stderr, "PASS desktop plugin editor: internal frame, shared process, reuse and reopen\n");
    return true;
}
