#include "MainWindow.hpp"
#include "PerformanceWindow.hpp"
#include "NotificationCenter.hpp"
#include "SettingsWindow.hpp"
#include <QMessageBox>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QProgressBar>
#include <QToolButton>

void MainWindow::ensurePerformanceWindow() {
    if (m_performanceWindow) return;
    m_performanceWindow = new PerformanceWindow(&audioController(), this);
    connect(m_performanceWindow, &PerformanceWindow::audioSettingsRequested, this,
        [this] { openSettings(SettingsWindow::kAudioTab); });
    connect(m_performanceWindow, &PerformanceWindow::pluginActionRequested,
        this, &MainWindow::handlePerformanceAction);
    hostInternalWindow(m_performanceWindow, QStringLiteral("internalEditors/performance"));
    if (const auto* owner = m_projects.audible()) m_performanceWindow->setProperty("vlt.projectId", owner->id);
}

void MainWindow::openPerformanceWindow() {
    ensurePerformanceWindow();
    presentInternalWindow(m_performanceWindow);
}

void MainWindow::handlePerformanceAction(daw::AudioPluginAddress address, quint64 project, int action) {
    if (m_projectFileJob || project != audioController().projectGeneration() ||
        audioController().offlineRenderInProgress() || audioController().audioProcessingUnresponsive()) return;
    if (audioController().isRecording() || audioController().isCountingIn()) {
        QMessageBox::information(m_performanceWindow, tr("Recording in progress"),
            tr("Finish recording before restarting or removing plugins."));
        return;
    }
    const auto current = audioController().audioPerformanceSnapshot();
    const auto found = std::find_if(current.rows.begin(), current.rows.end(), [&](const auto& row) {
        return row.address.channelId == address.channelId && row.address.slotId == address.slotId &&
            row.address.right == address.right && row.address.instance == address.instance;
    });
    if (found == current.rows.end()) return;
    const auto row = *found;
    if (action == 2 && QMessageBox::question(m_performanceWindow, tr("Remove plugin"),
        tr("Remove “%1” from this project? This can be undone.").arg(QString::fromStdString(row.name)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    // A confirmation can run a nested event loop: revalidate the native identity.
    const auto latest = audioController().audioPerformanceSnapshot();
    if (m_projectFileJob || audioController().offlineRenderInProgress() ||
        project != audioController().projectGeneration() || audioController().audioProcessingUnresponsive() ||
        audioController().isRecording() || audioController().isCountingIn() ||
        std::none_of(latest.rows.begin(), latest.rows.end(), [&](const auto& r) {
            return r.address.channelId == address.channelId && r.address.slotId == address.slotId &&
                r.address.right == address.right && r.address.instance == address.instance;
        })) return;
    if (action == 0) {
        audioController().setInsertBypassed(address.channelId, address.slotId, !row.bypassed);
        m_performanceWindow->recordAction(tr("Plugin bypass changed: %1").arg(QString::fromStdString(row.name)));
    } else if (action == 1) {
        audioController().cancelCountIn(); audioController().pause(); audioController().stopPreview(); audioController().stopPluginAudition();
        const auto result = audioController().restartInsert(address, project);
        m_performanceWindow->recordAction(result
            ? tr("Plugin restarted with settings: %1").arg(QString::fromStdString(row.name))
            : tr("Plugin restart failed: %1").arg(QString::fromStdString(result.message())));
        if (!result) QMessageBox::warning(m_performanceWindow, tr("Plugin could not be restarted"), QString::fromStdString(result.message()));
    } else if (action == 2) {
        using K = daw::AudioPerformanceRow::Kind;
        if (row.kind == K::ClipFx) audioController().removeClipFxInsert(address.channelId, row.clipId, address.slotId);
        else if (row.kind == K::SamplerFx) {
            if (const auto* track = audioController().project().findTrack(address.channelId))
                audioController().removeSamplerFxInsert(address.channelId, track->instrument.id, address.slotId);
        } else if (row.kind == K::MiniModule) audioController().removeMiniModule(address.channelId, address.slotId);
        else audioController().removeRackSelection(address.channelId, {address.slotId});
        m_performanceWindow->recordAction(tr("Plugin removal requested: %1").arg(QString::fromStdString(row.name)));
    }
    syncViews();
    if (action != 1) {
        if (auto* owner = m_projects.audible(); owner && owner != &m_projects.current()) {
            owner->dirty = true; ++owner->revision; m_journalStale = true;
        } else markDirty({QString::fromStdString(address.channelId)});
    }
    for (const auto& notice : audioController().takeAudioFailureNotices()) showAudioFailure(notice);
}

bool MainWindow::checkPluginProcessingHealth() {
    if (audioController().offlineRenderInProgress()) return false;
    if (m_pluginHealthClock.isValid() && m_pluginHealthClock.elapsed() < 500) return m_pluginProcessingStalled;
    m_pluginHealthClock.start();
    const bool stalled = audioController().audioProcessingUnresponsive();
    if (stalled != m_pluginProcessingStalled && m_audioNotifications) {
        const QString key = QStringLiteral("audio-processing-stalled");
        if (stalled) {
            ui::Notification notice;
            notice.id = key;
            notice.title = tr("Audio processing is not responding");
            notice.message = tr("A processor has been running for over 2 seconds. Open diagnostics to identify it. Recovery actions become available when the call returns.");
            notice.actions.push_back({tr("Open diagnostics"), [this] { openPerformanceWindow(); }});
            m_audioNotifications->showNotification(std::move(notice), true);
            ensurePerformanceWindow();
            m_performanceWindow->refresh();
        } else m_audioNotifications->remove(key);
    }
    m_pluginProcessingStalled = stalled;
    return stalled;
}

bool MainWindow::checkPerformanceWindowForTest(const QString& shots) {
    resize(1360, 900); show(); QApplication::processEvents();
    if (!m_cpuStatusMeter || m_cpuStatusMeter->isHidden() || !m_cpuStatusButton || m_cpuStatusButton->isCheckable()) return false;
    m_cpuStatusButton->click(); QApplication::processEvents();
    auto* first = m_performanceWindow;
    if (!first || !first->isVisible()) return false;
    if (!shots.isEmpty()) {
        QDir().mkpath(shots);
        if (!grab().save(QDir(shots).filePath(QStringLiteral("hosted.png")))) return false;
    }
    hideInternalWindow(first);
    QAction* menuAction = nullptr;
    for (auto* action : findChildren<QAction*>())
        if (action->text() == tr("Performance & diagnostics…")) { menuAction = action; break; }
    if (!menuAction) return false;
    menuAction->trigger(); QApplication::processEvents();
    const bool reused = first == m_performanceWindow && first->isVisible();
    hideInternalWindow(first);
    return reused && PerformanceWindow::checkForTest(shots);
}
