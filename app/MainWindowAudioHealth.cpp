#include "MainWindow.hpp"
#include "NotificationCenter.hpp"
#include "ChannelStrip.hpp"
#include "SettingsWindow.hpp"
#include "PerformanceWindow.hpp"
#include <QApplication>

namespace {
QString noticeKey(const daw::AudioFailureNotice& notice) {
    return QStringLiteral("%1/%2/%3/%4/%5").arg(notice.project).arg(int(notice.source))
        .arg(QString::fromStdString(notice.address.channelId), QString::fromStdString(notice.address.slotId),
             QString::fromStdString(notice.operation));
}
}

void MainWindow::serviceAudioHealth() {
    if (!m_audioNotifications) return;
    if (checkPluginProcessingHealth()) return;
    const auto generation = audioController().projectGeneration();
    const auto document = QString::fromStdString(audioController().documentIdentity());
    if (m_audioNotificationProject != generation || m_audioNotifications->property("vlt.audioDocument").toString() != document) {
        m_audioNotifications->clear();
        m_audioNotificationProject = generation;
        m_audioNotifications->setProperty("vlt.audioDocument", document);
    }
    const bool canMaintainPlugins = !m_projectFileJob && QApplication::mouseButtons() == Qt::NoButton && !QWidget::mouseGrabber();
    if (!m_audioDeviceHealthClock.isValid() || m_audioDeviceHealthClock.elapsed() >= 1000) {
        m_audioDeviceHealthClock.start();
        if (audioController().audioDeviceNeedsRecovery()) {
            // The device watchdog and processor watchdog both use two seconds.
            // Recheck without the UI cache before stopping a stream whose
            // callback may still be inside a vendor call.
            m_pluginHealthClock.invalidate();
            if (checkPluginProcessingHealth()) return;
            const auto processing = audioController().audioPerformanceSnapshot();
            if (std::any_of(processing.rows.begin(), processing.rows.end(), [&](const auto& row) {
                return row.timing.activeSinceNs > row.timing.completedAtNs &&
                    processing.timestampNs > row.timing.activeSinceNs &&
                    processing.timestampNs - row.timing.activeSinceNs > 100'000'000ull;
            })) return;
            if (audioController().isRecording()) {
                audioController().markRecordingInterrupted();
                finishAudioRecordingForHealth();
            }
            audioController().cancelCountIn();
            if (!audioController().audioDeviceRecoveryExhausted()) (void)audioController().recoverAudioDevice();
        } else (void)audioController().recoverAudioDevice();
        // Native checkpoints remain available with recovery-to-disk disabled.
        if (canMaintainPlugins && !m_journal.running()) audioController().refreshRecoveryPluginStates(1);
    }
    audioController().serviceAudioHealth(canMaintainPlugins);
    for (const auto& notice : audioController().takeAudioFailureNotices()) {
        if (notice.project != audioController().projectGeneration()) continue;
        showAudioFailure(notice);
    }
}

void MainWindow::retryAudioFailure(const daw::AudioFailureNotice& notice) {
    if (notice.project != audioController().projectGeneration()) return;
    if (notice.source == daw::AudioFailureNotice::Source::Device) {
        if (audioController().isRecording()) {
            audioController().markRecordingInterrupted(); finishAudioRecordingForHealth();
        }
        audioController().cancelCountIn();
        (void)audioController().recoverAudioDevice(true);
    } else {
        const auto current = audioController().insertRuntimeStatus(notice.address.channelId, notice.address.slotId);
        if (current.address.instance != notice.address.instance || !current.canRetry) return;
        audioController().cancelCountIn();
        if (audioController().isRecording()) finishAudioRecordingForHealth();
        audioController().pause();
        audioController().stopPreview();
        audioController().stopPluginAudition();
        (void)audioController().retryInsertRecovery(notice.address.channelId, notice.address.slotId,
            {notice.project, notice.address.instance});
    }
    for (const auto& update : audioController().takeAudioFailureNotices()) showAudioFailure(update);
}

void MainWindow::showAudioFailure(const daw::AudioFailureNotice& notice, bool reveal) {
    if (!m_audioNotifications || notice.project != audioController().projectGeneration()) return;
    const auto document = audioController().documentIdentity();
    if (notice.state != daw::AudioFailureNotice::State::Retired) {
        ensurePerformanceWindow();
        m_performanceWindow->recordNotice(notice);
    }
    using Source = daw::AudioFailureNotice::Source;
    using State = daw::AudioFailureNotice::State;
    if (notice.state == State::Stopped) {
        if (audioController().isRecording()) {
            audioController().markRecordingInterrupted();
            finishAudioRecordingForHealth();
        }
        audioController().cancelCountIn();
    }
    ui::Notification card;
    card.id = noticeKey(notice);
    if (notice.state == State::Retired) {
        m_audioNotifications->remove(card.id);
        return;
    }
    card.incident = notice.incident;
    card.detail = QString::fromStdString(notice.detail);
    card.resolved = notice.state == State::Recovered;
    if (notice.source == Source::Plugin) {
        bool analysis = false;
        if (const auto* chain = audioController().channelInserts(notice.address.channelId))
            for (const auto& slot : *chain)
                if (slot.id == notice.address.slotId) analysis = slot.analysisOnly;
        for (auto* strip : findChildren<ChannelStrip*>())
            if (notice.master ? strip->isMaster() : strip->trackId().toStdString() == notice.address.channelId)
                strip->refreshPluginHealth();
        const QString owner = notice.master ? tr("Master") : QString::fromStdString(notice.channelName);
        card.title = tr("%1 — %2").arg(QString::fromStdString(notice.name), owner);
        if (notice.state == State::Recovered) card.message = tr("Plugin restored from its saved state.");
        else if (notice.state == State::Recovering) card.message = tr("Restoring plugin…");
        else if (!notice.canRetry) card.message = tr("Plugin disabled. No saved state is available. Replace or remove it from the slot.");
        else if (notice.state == State::Failed) card.message = tr("Could not restore the plugin. It remains disabled.");
        else if (notice.recording) card.message = tr("Plugin disabled. Recovery will wait until recording has finished.");
        else if (analysis) card.message = tr("Analyzer disabled. The master output is unchanged. Recovery will wait until playback stops.");
        else if (notice.master) card.message = tr("Master output muted after a plugin error. Recovery will wait until playback stops.");
        else if (notice.instrument) card.message = tr("Instrument disabled after an error. Recovery will wait until playback stops.");
        else card.message = tr("Effect disabled. The track continues without it. Recovery will wait until playback and monitoring stop.");
        if (notice.canRetry && notice.state != State::Recovering && !card.resolved) {
            const auto label = notice.recording ? tr("Finish recording and restore")
                : notice.requiresStop ? tr("Stop and restore") : tr("Retry recovery");
            card.actions.push_back({label, [this, notice, document] {
                if (audioController().documentIdentity() == document) retryAudioFailure(notice);
            }});
        }
        card.actions.push_back({tr("Performance & diagnostics…"), [this] { openPerformanceWindow(); }});
    } else if (notice.source == Source::Device) {
        card.title = card.resolved ? tr("Audio reconnected") : tr("Audio device unavailable");
        card.message = card.resolved ? tr("Audio is ready. Recording has not been restarted.")
            : notice.state == State::Waiting ? tr("Trying to reconnect the audio device. Any interrupted take has been preserved.")
            : tr("Automatic reconnection failed. Check the device and try again.");
        if (notice.canRetry) card.actions.push_back({tr("Reconnect"), [this, notice, document] {
            if (audioController().documentIdentity() == document) retryAudioFailure(notice);
        }});
        card.actions.push_back({tr("Audio settings"), [this] { openSettings(SettingsWindow::kAudioTab); }});
    } else if (notice.source == Source::Overload) {
        card.title = tr("Too many plugin events");
        card.message = tr("An audio block could not be processed completely. Reduce MIDI or automation density.");
    } else {
        if (notice.operation == "Recording input") {
            card.title = tr("Recording could not start");
            card.message = tr("The selected audio input is unavailable. Choose an input device in Audio settings, press Apply, and check the track's input channels.");
            card.actions.push_back({tr("Audio settings"), [this] { openSettings(SettingsWindow::kAudioTab); }});
            m_audioNotifications->showNotification(std::move(card), reveal);
            return;
        }
        if (notice.operation == "Recording") {
            card.title = tr("Recording interrupted");
            card.message = tr("The take contains missing audio or an incomplete file. Available audio has been retained. Check the take before continuing.");
            m_audioNotifications->showNotification(std::move(card), reveal);
            return;
        }
        card.title = notice.state == State::Stopped ? tr("Audio engine stopped") : tr("Action could not be completed");
        if (notice.state != State::Stopped) {
            QString operation;
            if (notice.operation == "New project") operation = tr("New project");
            else if (notice.operation == "Open project") operation = tr("Open project");
            else if (notice.operation == "Add plugin") operation = tr("Add plugin");
            else if (notice.operation == "Replace plugin") operation = tr("Replace plugin");
            else if (notice.operation == "Load plugin state") operation = tr("Load plugin state");
            else if (notice.operation == "Load plugin chain") operation = tr("Load plugin chain");
            else if (notice.operation == "Plugin maintenance") operation = tr("Plugin maintenance");
            else if (notice.operation == "Audio configuration") operation = tr("Audio configuration");
            if (!operation.isEmpty()) card.title = operation;
        }
        card.message = notice.state == State::Stopped
            ? tr("Audio could not be restored. Save the project before reopening it.")
            : notice.operation == "Audio configuration"
                ? tr("The requested audio settings could not be applied. Check the active device and format in Audio settings.")
                : tr("The change was not applied. The previous state has been preserved.");
        if (notice.operation == "Audio configuration")
            card.actions.push_back({tr("Audio settings"), [this] { openSettings(SettingsWindow::kAudioTab); }});
        if (!notice.detail.empty()) card.message += QStringLiteral("\n") + QString::fromStdString(notice.detail);
    }
    m_audioNotifications->showNotification(std::move(card), reveal);
}
