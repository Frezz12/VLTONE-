#include "OfflineRenderDialog.hpp"

#include "ChannelStrip.hpp"
#include "ChannelStripPreset.hpp"
#include "ChannelStripPresets.hpp"
#include "PluginEditorWindow.hpp"

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
#include <QVBoxLayout>

#include <algorithm>

OfflineRenderDialog::OfflineRenderDialog(
    daw::EngineController& controller,
    std::vector<daw::EngineController::ClipAddress> clips,
    bool chainsDiffer, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_clips(std::move(clips)) {
    setWindowTitle(tr("Offline Render"));
    setModal(true);
    resize(620, 560);

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

    if (!m_clips.empty() && !m_chainTrackId.empty()) {
        const auto initial = m_controller.offlineProcessChain(m_clips.front());
        if (!initial.inserts.empty())
            (void)m_scratch.pasteChannelInserts(m_chainTrackId, initial);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);
    auto* intro = new QLabel(
        tr("The original clip audio is preserved. This chain is rendered into "
           "a managed cache; realtime Clip FX remain live."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setAccessibleName(tr("Offline render warning"));
    if (chainsDiffer) {
        m_warning->setText(
            tr("Warning: selected clips have different offline chains. The primary "
               "clip's chain is shown; Render Offline will replace the chains "
               "on all selected clips."));
    }
    root->addWidget(m_warning);

    auto* clipsBox = new QGroupBox(
        tr("Selected clips (%1)").arg(m_clips.size()), this);
    auto* clipsLayout = new QVBoxLayout(clipsBox);
    m_clipList = new QListWidget(clipsBox);
    m_clipList->setAccessibleName(tr("Clips to process"));
    m_clipList->setSelectionMode(QAbstractItemView::NoSelection);
    m_clipList->setAlternatingRowColors(true);
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
            tr("%1 — %2  (%3–%4 s)")
                .arg(QString::fromStdString(track->name), clipName)
                .arg(clip->startSeconds, 0, 'f', 3)
                .arg(end, 0, 'f', 3),
            m_clipList);
        item->setToolTip(tr("Track: %1\nClip: %2")
                             .arg(QString::fromStdString(track->name),
                                  clipName));
    }
    const int clipRows = std::clamp(m_clipList->count(), 1, 4);
    m_clipList->setFixedHeight(
        clipRows * m_clipList->sizeHintForRow(0) +
        2 * m_clipList->frameWidth() + 4);
    clipsLayout->addWidget(m_clipList);
    root->addWidget(clipsBox);

    m_rackHost = new QWidget(this);
    m_rackHost->setAccessibleName(tr("Offline processing inserts"));
    m_rackLayout = new QVBoxLayout(m_rackHost);
    m_rackLayout->setContentsMargins(0, 0, 0, 0);
    root->addWidget(m_rackHost);

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
    root->addWidget(m_status);
    root->addWidget(m_progress);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_renderButton = m_buttons->addButton(tr("Render Offline"),
                                         QDialogButtonBox::AcceptRole);
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

    reloadPresets();
    rebuildRack();
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
}

void OfflineRenderDialog::openEditor(const QString& insertId) {
    if (insertId.isEmpty()) return;
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
    m_status->setText(tr("Rendering…"));

    const auto chain = m_scratch.copyChannelStrip(m_chainTrackId, false);
    daw::EngineController::OfflineRenderReport report;
    const audio::Result result = m_controller.renderClipsOffline(
        m_clips, chain, m_includeTail->isChecked(),
        [this](const daw::rendering::Progress& progress) {
            m_progress->setValue(
                std::clamp(int(progress.fraction * 1000.0), 0, 1000));
            m_status->setText(tr("Rendering %1 of %2 seconds")
                                  .arg(progress.renderedSeconds, 0, 'f', 1)
                                  .arg(progress.totalSeconds, 0, 'f', 1));
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
    m_loadPreset->setEnabled(m_presets->count() > 0);
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
