#include "OfflineRenderDialog.hpp"

#include "ChannelStrip.hpp"
#include "ChannelStripPreset.hpp"
#include "ChannelStripPresets.hpp"
#include "PluginEditorWindow.hpp"
#include "SamplerPanel.hpp"
#include "Internal/EqualizerInstance.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

OfflineRenderDialog::OfflineRenderDialog(
    daw::EngineController& controller,
    std::vector<daw::EngineController::ClipAddress> clips,
    QWidget* parent)
    : QDialog(parent), m_controller(controller), m_clips(std::move(clips)) {
    setWindowTitle(tr("Offline Render"));
    setModal(true);
    setObjectName(QStringLiteral("OfflineRenderDialog"));
    resize(620, 420);

    const audio::Result ready =
        m_scratch.initialize(controller.sampleRate(),
                             controller.bufferSizeFrames(), false);
    if (ready) m_chainTrackId = m_scratch.addTrack(daw::TrackKind::Audio,
                                                   "Offline Processing");
    m_scratch.setPluginRetiringCallback(
        [this](const std::string& channelId, const std::string& slotId) {
            const QString channel = QString::fromStdString(channelId);
            const QString slot = QString::fromStdString(slotId);
            for (PluginEditorWindow* editor :
                 findChildren<PluginEditorWindow*>()) {
                if (editor->channelId() != channel ||
                    editor->insertId() != slot)
                    continue;
                editor->detachFromPlugin();
                editor->close();
            }
        });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);
    auto* intro = new QLabel(
        tr("Add effects to the current audio. Render replaces the checked clips "
           "and saves a new version in Sampler → Clip FX → History."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* content = new QHBoxLayout;
    content->setSpacing(12);
    m_rackHost = new QWidget(this);
    m_rackHost->setAccessibleName(tr("Offline processing inserts"));
    m_rackHost->setFixedWidth(210);
    m_rackLayout = new QVBoxLayout(m_rackHost);
    m_rackLayout->setContentsMargins(0, 0, 0, 0);
    m_rackLayout->setAlignment(Qt::AlignTop);
    content->addWidget(m_rackHost);
    auto* clipsBox = new QGroupBox(tr("Clips to render"), this);
    auto* clipsLayout = new QVBoxLayout(clipsBox);
    m_clipSummary = new QLabel(clipsBox);
    m_clipSummary->setAccessibleName(tr("Render selection"));
    clipsLayout->addWidget(m_clipSummary);
    m_clipList = new QListWidget(clipsBox);
    m_clipList->setAccessibleName(tr("Clips to process"));
    m_clipList->setObjectName(QStringLiteral("OfflineRenderClips"));
    m_clipList->setSelectionMode(QAbstractItemView::NoSelection);
    m_clipList->setAlternatingRowColors(true);
    m_clipList->setTextElideMode(Qt::ElideRight);
    for (const auto& address : m_clips) {
        const daw::TrackModel* track =
            m_controller.project().findTrack(address.trackId);
        if (!track) continue;
        const auto clip = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const daw::ClipModel& item) {
                return item.id == address.clipId;
            });
        if (clip == track->clips.end()) continue;
        QString clipName = QString::fromStdString(clip->name);
        if (clipName.isEmpty())
            clipName = QFileInfo(QString::fromStdString(clip->filePath))
                           .completeBaseName();
        if (clipName.isEmpty()) clipName = tr("Audio clip");
        const double end = clip->startSeconds +
                           m_controller.clipPlaybackDuration(*clip);
        auto* item = new QListWidgetItem(
            tr("%1\n%2 · %3–%4 s")
                .arg(clipName, QString::fromStdString(track->name))
                .arg(clip->startSeconds, 0, 'f', 3)
                .arg(end, 0, 'f', 3),
            m_clipList);
        item->setData(Qt::UserRole, int(&address - m_clips.data()));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setSizeHint(QSize(0, 46));
        item->setToolTip(tr("Track: %1\nClip: %2")
                             .arg(QString::fromStdString(track->name),
                                  clipName));
    }
    m_clipList->setMinimumHeight(146);
    clipsLayout->addWidget(m_clipList);
    content->addWidget(clipsBox, 1);
    root->addLayout(content, 1);

    auto* presets = new QHBoxLayout;
    auto* presetLabel = new QLabel(tr("Chain preset"), this);
    m_presets = new QComboBox(this);
    m_presets->setAccessibleName(tr("Offline processing preset"));
    m_loadPreset = new QPushButton(tr("Load"), this);
    m_savePreset = new QPushButton(tr("Save As…"), this);
    presets->addWidget(presetLabel);
    presets->addWidget(m_presets, 1);
    presets->addWidget(m_loadPreset);
    presets->addWidget(m_savePreset);
    root->addLayout(presets);

    m_includeTail = new QCheckBox(tr("Include Tail"), this);
    QSettings settings;
    m_includeTail->setChecked(
        settings.value(QStringLiteral("offlineRender/includeTail"), false)
            .toBool());
    root->addWidget(m_includeTail);

    m_status = new QLabel(tr("Ready"), this);
    m_status->setAccessibleName(tr("Offline render status"));
    m_progress = new QProgressBar(this);
    m_progress->setAccessibleName(tr("Offline render progress"));
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setFixedHeight(5);
    m_progress->setTextVisible(false);
    root->addWidget(m_status);
    root->addWidget(m_progress);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_renderButton = m_buttons->addButton(tr("Render Offline"),
                                         QDialogButtonBox::AcceptRole);
    m_renderButton->setObjectName(QStringLiteral("OfflineRenderStart"));
    m_renderButton->setDefault(true);
    root->addWidget(m_buttons);

    connect(m_loadPreset, &QPushButton::clicked, this,
            &OfflineRenderDialog::loadPreset);
    connect(m_savePreset, &QPushButton::clicked, this,
            &OfflineRenderDialog::savePreset);
    connect(m_renderButton, &QPushButton::clicked, this,
            &OfflineRenderDialog::startRender);
    connect(m_buttons, &QDialogButtonBox::rejected, this,
            &OfflineRenderDialog::reject);
    connect(m_clipList, &QListWidget::itemChanged, this,
            &OfflineRenderDialog::updateRenderAvailability);

    reloadPresets();
    rebuildRack();
    updateRenderAvailability();
    if (!ready || m_chainTrackId.empty()) {
        m_renderButton->setEnabled(false);
        m_status->setText(tr("Could not create the offline plugin rack"));
    }
}

void OfflineRenderDialog::reject() {
    if (m_rendering) {
        m_cancelRequested = true;
        return;
    }
    QDialog::reject();
}

OfflineRenderDialog::~OfflineRenderDialog() {
    const auto editors = findChildren<PluginEditorWindow*>();
    for (PluginEditorWindow* editor : editors) {
        editor->detachFromPlugin();
        delete editor;
    }
    m_scratch.setPluginRetiringCallback({});
    m_scratch.shutdown();
}

void OfflineRenderDialog::rebuildRack() {
    if (m_rack) {
        m_rackLayout->removeWidget(m_rack);
        m_rack->deleteLater();
        m_rack = nullptr;
    }
    if (m_chainTrackId.empty()) return;
    m_rack = new ChannelStrip(&m_scratch,
                              QString::fromStdString(m_chainTrackId), false,
                              m_rackHost, true);
    m_rack->setEnabled(!m_rendering);
    m_rackLayout->addWidget(m_rack);
    connect(m_rack, &ChannelStrip::editorRequested, this,
            [this](const QString&, const QString& insertId) {
                openEditor(insertId);
            });
    connect(m_rack, &ChannelStrip::structureChanged, this,
            &OfflineRenderDialog::rebuildRack, Qt::QueuedConnection);
    updateRenderAvailability();
}

void OfflineRenderDialog::updateRenderAvailability() {
    int checked = 0;
    for (int row = 0; row < m_clipList->count(); ++row)
        if (m_clipList->item(row)->checkState() == Qt::Checked) ++checked;
    m_clipSummary->setText(tr("%1 of %2 selected").arg(checked).arg(m_clipList->count()));
    const auto* inserts = m_scratch.channelInserts(m_chainTrackId);
    const bool enabled = inserts && std::any_of(inserts->begin(), inserts->end(),
        [](const auto& slot) { return !slot.bypassed; });
    m_renderButton->setEnabled(!m_rendering && checked > 0 && enabled);
    m_renderButton->setText(tr("Render %1 clips").arg(checked));
    if (!m_rendering)
        m_status->setText(!checked ? tr("Check the clips to render")
                                  : enabled ? tr("Ready — a new version will be saved")
                                            : tr("Add or enable an effect to render"));
}

void OfflineRenderDialog::openEditor(const QString& insertId) {
    if (insertId.isEmpty()) return;
    for (auto* editor : findChildren<PluginEditorWindow*>()) {
        if (editor->insertId() == insertId) {
            editor->show();
            editor->raise();
            editor->activateWindow();
            return;
        }
    }
    if (!m_scratch.insertInstance(m_chainTrackId, insertId.toStdString())) {
        QMessageBox::information(this, tr("Offline Render"),
                                 tr("This plugin is not available."));
        return;
    }
    auto* editor = new PluginEditorWindow(
        &m_scratch, QString::fromStdString(m_chainTrackId), insertId, this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &PluginEditorWindow::projectEdited, this,
            &OfflineRenderDialog::rebuildRack, Qt::QueuedConnection);
    editor->show();
    editor->prepareNativeHostHierarchy();
    QTimer::singleShot(0, editor, &PluginEditorWindow::initializeEditor);
}

void OfflineRenderDialog::reloadPresets() {
    const QString current = m_presets->currentData().toString();
    m_presets->clear();
    for (const QString& file : ui::channelstrippresets::offlineFiles()) {
        m_presets->addItem(ui::channelstrippresets::displayName(file), file);
    }
    const int found = m_presets->findData(current);
    if (found >= 0) m_presets->setCurrentIndex(found);
    m_loadPreset->setEnabled(m_presets->count() > 0 && !m_rendering);
}

void OfflineRenderDialog::loadPreset() {
    const QString path = m_presets->currentData().toString();
    if (path.isEmpty()) return;
    daw::EngineController::ChannelSnapshot preset;
    const audio::Result result =
        daw::ChannelStripPreset::load(preset, path.toStdString());
    if (!result) {
        QMessageBox::critical(this, tr("Load preset"),
                              QString::fromStdString(result.message()));
        return;
    }
    (void)m_scratch.pasteChannelInserts(m_chainTrackId, preset);
    rebuildRack();
}

void OfflineRenderDialog::savePreset() {
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Save offline chain"), tr("Preset name"), QLineEdit::Normal,
        QString(), &accepted);
    if (!accepted) return;
    const QString path =
        ui::channelstrippresets::offlineFilePathForName(name);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Save preset"),
                             tr("Enter a valid preset name."));
        return;
    }
    auto snapshot = m_scratch.copyChannelStrip(m_chainTrackId, true);
    snapshot.sourceName = name.toStdString();
    const audio::Result result =
        daw::ChannelStripPreset::save(snapshot, path.toStdString());
    if (!result) {
        QMessageBox::critical(this, tr("Save preset"),
                              QString::fromStdString(result.message()));
        return;
    }
    reloadPresets();
    m_presets->setCurrentIndex(m_presets->findData(path));
}

void OfflineRenderDialog::startRender() {
    if (m_rendering || !m_renderButton->isEnabled()) return;
    std::vector<daw::EngineController::ClipAddress> checked;
    std::vector<int> rows;
    for (int row = 0; row < m_clipList->count(); ++row) {
        const auto* item = m_clipList->item(row);
        if (item->checkState() != Qt::Checked) continue;
        checked.push_back(m_clips.at(std::size_t(item->data(Qt::UserRole).toInt())));
        rows.push_back(row);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("offlineRender/includeTail"),
                      m_includeTail->isChecked());
    m_rendering = true;
    m_cancelRequested = false;
    m_renderButton->setEnabled(false);
    if (m_rack) m_rack->setEnabled(false);
    for (PluginEditorWindow* editor : findChildren<PluginEditorWindow*>())
        editor->setEnabled(false);
    m_presets->setEnabled(false);
    m_loadPreset->setEnabled(false);
    m_savePreset->setEnabled(false);
    m_includeTail->setEnabled(false);
    m_clipList->setEnabled(false);
    m_status->setText(tr("Rendering…"));

    const auto chain = m_scratch.copyChannelStrip(m_chainTrackId, false);
    daw::EngineController::OfflineRenderReport report;
    const audio::Result result = m_controller.renderClipsOffline(
        checked, chain, m_includeTail->isChecked(),
        [this, &report, &rows](const daw::rendering::Progress& progress) {
            if (report.clipIndex < rows.size()) {
                auto* item = m_clipList->item(rows[report.clipIndex]);
                m_clipList->scrollToItem(item);
                m_clipSummary->setText(tr("Rendering clip %1 of %2")
                    .arg(report.clipIndex + 1).arg(report.clipCount));
            }
            m_progress->setValue(
                std::clamp(int(progress.fraction * 1000.0), 0, 1000));
            if (progress.stage == daw::rendering::Progress::Stage::Preparing)
                m_status->setText(tr("Preparing audio and plugins…"));
            else if (progress.stage == daw::rendering::Progress::Stage::PreRoll)
                m_status->setText(tr("Warming up effects: %1 of %2 seconds")
                    .arg(progress.renderedSeconds, 0, 'f', 1)
                    .arg(progress.totalSeconds, 0, 'f', 1));
            else {
            m_status->setText(tr("Rendering %1 of %2 seconds")
                                  .arg(progress.renderedSeconds, 0, 'f', 1)
                                  .arg(progress.totalSeconds, 0, 'f', 1));
            }
            QApplication::processEvents();
            return !m_cancelRequested;
        },
        report);
    m_rendering = false;
    if (m_rack) m_rack->setEnabled(true);
    for (PluginEditorWindow* editor : findChildren<PluginEditorWindow*>())
        editor->setEnabled(true);
    m_presets->setEnabled(true);
    m_savePreset->setEnabled(true);
    m_includeTail->setEnabled(true);
    m_clipList->setEnabled(true);
    m_loadPreset->setEnabled(m_presets->count() > 0);
    updateRenderAvailability();
    if (report.cancelled || m_cancelRequested) {
        m_status->setText(tr("Cancelled — the project was not changed"));
        m_renderButton->setEnabled(true);
        return;
    }
    if (!result) {
        m_status->setText(tr("Offline render failed"));
        QMessageBox::critical(this, tr("Offline Render"),
                              QString::fromStdString(result.message()));
        m_renderButton->setEnabled(true);
        return;
    }
    m_progress->setValue(1000);
    m_status->setText(tr("Offline render complete"));
    m_rendered = true;
    accept();
}

bool OfflineRenderDialog::checkForTest(const QString& screenshotPath) {
    QTemporaryDir dir;
    if (!dir.isValid()) return false;
    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false)) return false;
    controller.setRecordDirectory(dir.path().toStdString());
    const auto file = (dir.path() + QStringLiteral("/Vocal.wav")).toStdString();
    audio::platform::AudioFileWriter writer;
    audio::platform::WriteSpec spec;
    spec.encoding = audio::platform::Encoding::Float32;
    if (!writer.open(file, spec, 48000, 2, 12000)) return false;
    std::vector<float> samples(12000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.2f * std::sin(double(i) * 440.0 * 6.283185307 / 48000.0);
    const float* channels[]{samples.data(), samples.data()};
    if (!writer.write(channels, samples.size()) || !writer.close()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Vocal");
    const auto first = controller.importAudio(file, track, 0.0);
    const auto second = controller.importAudio(file, track, 0.5);
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        std::fprintf(stderr, "%s offline UI: %s\n", condition ? "PASS" : "FAIL", message);
        ok &= condition;
    };
    OfflineRenderDialog dialog(controller, {{track, first}, {track, second}});
    dialog.show();
    QApplication::processEvents();
    check(!dialog.m_renderButton->isEnabled() && dialog.m_clipList->count() == 2,
          "empty rack disables render and lists every selected clip");
    const auto effect = dialog.m_scratch.addInsert(dialog.m_chainTrackId,
        daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    dialog.rebuildRack();
    QApplication::processEvents();
    check(!effect.empty() && dialog.m_renderButton->isEnabled(), "loading an effect enables render");
    const auto preset = (dir.path() + QStringLiteral("/Vocal.vlts")).toStdString();
    const auto savedPreset = daw::ChannelStripPreset::save(
        dialog.m_scratch.copyChannelStrip(dialog.m_chainTrackId, true), preset);
    check(bool(savedPreset), "offline chain template saves");
    if (!savedPreset) return false;
    dialog.m_presets->addItem("Vocal", QString::fromStdString(preset));
    dialog.m_presets->setCurrentIndex(dialog.m_presets->count() - 1);
    dialog.loadPreset();
    check(dialog.m_scratch.channelInserts(dialog.m_chainTrackId)->size() == 1, "saved template loads into the draft rack");
    dialog.m_clipList->item(1)->setCheckState(Qt::Unchecked);
    check(dialog.m_renderButton->isEnabled(), "one checked clip remains renderable");
    dialog.m_clipList->item(0)->setCheckState(Qt::Unchecked);
    check(!dialog.m_renderButton->isEnabled(), "zero checked clips disables render");
    dialog.m_clipList->item(0)->setCheckState(Qt::Checked);
    QApplication::processEvents();
    check(dialog.m_rack->width() <= 210 && dialog.height() < 500, "rack uses compact mixer slots");
    if (!screenshotPath.isEmpty()) check(dialog.grab().save(screenshotPath), "dialog screenshot saved");
    dialog.startRender();
    check(dialog.rendered() && controller.audioClip(track, first)->offlineHistory.size() == 2 &&
          controller.audioClip(track, second)->offlineHistory.empty(), "only the checked clip is replaced");
    OfflineRenderDialog next(controller, {{track, first}});
    check(next.m_scratch.channelInserts(next.m_chainTrackId)->empty() && !next.m_renderButton->isEnabled(),
          "reopening a processed clip starts with a completely empty chain");
    SamplerPanel panel(&controller, SamplerPanel::Context::Clip,
                       QString::fromStdString(track), QString::fromStdString(first));
    panel.resize(900, 540);
    panel.show();
    panel.refresh();
    QApplication::processEvents();
    if (!screenshotPath.isEmpty())
        check(panel.grab().save(screenshotPath + ".sampler.png"), "sampler screenshot saved");
    auto* history = panel.findChild<QToolButton*>(QStringLiteral("SamplerOfflineHistory"));
    check(history && history->isEnabled(), "clip sampler exposes its history dropdown");
    if (history) history->click();
    QApplication::processEvents();
    auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("OfflineHistoryTree"));
    check(tree && tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->childCount() == 1,
          "dropdown lists the original and its rendered version");
    if (tree) {
        if (!screenshotPath.isEmpty())
            check(tree->window()->grab().save(screenshotPath + ".history.png"), "history screenshot saved");
        tree->itemActivated(tree->topLevelItem(0), 0);
        check(controller.audioClip(track, first)->filePath == file, "keyboard activation restores original audio");
    }
    return ok;
}
