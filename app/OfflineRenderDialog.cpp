#include "OfflineRenderDialog.hpp"

#include "ChannelStripPreset.hpp"
#include "ChannelStripPresets.hpp"
#include "PluginEditorWindow.hpp"
#include "PluginPickerMenu.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
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

    m_chain = new QListWidget(this);
    m_chain->setAccessibleName(tr("Offline plugin chain"));
    m_chain->setAlternatingRowColors(true);
    m_chain->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_chain, 1);

    auto* tools = new QHBoxLayout;
    m_add = new QToolButton(this);
    m_add->setText(tr("Add Plugin"));
    m_add->setPopupMode(QToolButton::InstantPopup);
    m_add->setMenu(ui::buildLazyPluginMenu(
        m_add, &m_scratch, false,
        [this](const daw::plugins::PluginDescriptor& plugin) {
            const std::string id =
                m_scratch.addInsert(m_chainTrackId, plugin);
            refreshChain(QString::fromStdString(id));
        }));
    m_remove = new QPushButton(tr("Remove"), this);
    m_up = new QPushButton(tr("Up"), this);
    m_down = new QPushButton(tr("Down"), this);
    m_bypass = new QPushButton(tr("Bypass"), this);
    m_edit = new QPushButton(tr("Open Editor"), this);
    tools->addWidget(m_add);
    tools->addWidget(m_remove);
    tools->addWidget(m_up);
    tools->addWidget(m_down);
    tools->addWidget(m_bypass);
    tools->addWidget(m_edit);
    tools->addStretch(1);
    root->addLayout(tools);

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

    connect(m_chain, &QListWidget::itemSelectionChanged, this,
            [this] { refreshChain(selectedSlot()); });
    connect(m_chain, &QListWidget::itemDoubleClicked, this,
            [this] { openSelectedEditor(); });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const QString id = selectedSlot();
        if (id.isEmpty()) return;
        m_scratch.removeInsert(m_chainTrackId, id.toStdString());
        refreshChain();
    });
    connect(m_up, &QPushButton::clicked, this,
            [this] { moveSelected(-1); });
    connect(m_down, &QPushButton::clicked, this,
            [this] { moveSelected(1); });
    connect(m_bypass, &QPushButton::clicked, this, [this] {
        const QString id = selectedSlot();
        const auto* insertModels = m_scratch.channelInserts(m_chainTrackId);
        if (id.isEmpty() || !insertModels) return;
        const auto found = std::find_if(insertModels->begin(), insertModels->end(),
                                       [&](const daw::InsertModel& slot) {
                                           return slot.id == id.toStdString();
                                       });
        if (found == insertModels->end()) return;
        m_scratch.setInsertBypassed(m_chainTrackId, found->id,
                                    !found->bypassed);
        refreshChain(id);
    });
    connect(m_edit, &QPushButton::clicked, this,
            &OfflineRenderDialog::openSelectedEditor);
    connect(m_loadPreset, &QPushButton::clicked, this,
            &OfflineRenderDialog::loadPreset);
    connect(m_savePreset, &QPushButton::clicked, this,
            &OfflineRenderDialog::savePreset);
    connect(m_renderButton, &QPushButton::clicked, this,
            &OfflineRenderDialog::startRender);
    connect(m_buttons, &QDialogButtonBox::rejected, this, [this] {
        if (m_rendering)
            m_cancelRequested = true;
        else
            reject();
    });

    reloadPresets();
    refreshChain();
    if (!ready || m_chainTrackId.empty()) {
        m_renderButton->setEnabled(false);
        m_status->setText(tr("Could not create the offline plugin rack"));
    }
}

OfflineRenderDialog::~OfflineRenderDialog() {
    const auto editors = findChildren<PluginEditorWindow*>();
    for (PluginEditorWindow* editor : editors) {
        editor->detachFromPlugin();
        delete editor;
    }
    m_scratch.shutdown();
}

QString OfflineRenderDialog::selectedSlot() const {
    const QListWidgetItem* item = m_chain->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void OfflineRenderDialog::refreshChain(const QString& keepSlot) {
    const QString selected = keepSlot.isEmpty() ? selectedSlot() : keepSlot;
    const QSignalBlocker blocker(m_chain);
    m_chain->clear();
    const auto* insertModels = m_scratch.channelInserts(m_chainTrackId);
    if (insertModels) {
        int row = 0;
        for (const daw::InsertModel& slot : *insertModels) {
            const bool available =
                m_scratch.insertInstance(m_chainTrackId, slot.id) != nullptr;
            QString label = QString::fromStdString(slot.name);
            if (!available) label += tr(" — Not Available");
            if (slot.bypassed) label += tr(" — Bypassed");
            auto* item = new QListWidgetItem(label, m_chain);
            item->setData(Qt::UserRole, QString::fromStdString(slot.id));
            if (QString::fromStdString(slot.id) == selected)
                m_chain->setCurrentRow(row);
            ++row;
        }
    }
    const bool has = m_chain->currentItem();
    m_remove->setEnabled(has && !m_rendering);
    m_up->setEnabled(has && m_chain->currentRow() > 0 && !m_rendering);
    m_down->setEnabled(has && m_chain->currentRow() + 1 < m_chain->count() &&
                       !m_rendering);
    m_bypass->setEnabled(has && !m_rendering);
    m_edit->setEnabled(has && !m_rendering);
    if (has) {
        const auto* currentSlots = m_scratch.channelInserts(m_chainTrackId);
        const int row = m_chain->currentRow();
        if (currentSlots && row >= 0 && row < int(currentSlots->size())) {
            m_bypass->setText((*currentSlots)[std::size_t(row)].bypassed
                                  ? tr("Enable")
                                  : tr("Bypass"));
        }
    }
}

void OfflineRenderDialog::moveSelected(int delta) {
    const QString id = selectedSlot();
    const int target = m_chain->currentRow() + delta;
    if (id.isEmpty() || target < 0 || target >= m_chain->count()) return;
    m_scratch.moveInsert(m_chainTrackId, id.toStdString(), std::size_t(target));
    refreshChain(id);
}

void OfflineRenderDialog::openSelectedEditor() {
    const QString id = selectedSlot();
    if (id.isEmpty()) return;
    if (!m_scratch.insertInstance(m_chainTrackId, id.toStdString())) {
        QMessageBox::information(this, tr("Offline Render"),
                                 tr("This plugin is not available."));
        return;
    }
    auto* editor = new PluginEditorWindow(
        &m_scratch, QString::fromStdString(m_chainTrackId), id, this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
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
    if (preset.inserts.empty()) {
        std::vector<std::string> ids;
        if (const auto* insertModels =
                m_scratch.channelInserts(m_chainTrackId)) {
            for (const daw::InsertModel& slot : *insertModels)
                ids.push_back(slot.id);
        }
        for (const std::string& id : ids)
            m_scratch.removeInsert(m_chainTrackId, id);
    } else {
        (void)m_scratch.pasteChannelInserts(m_chainTrackId, preset);
    }
    refreshChain();
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
    m_add->setEnabled(false);
    m_presets->setEnabled(false);
    m_loadPreset->setEnabled(false);
    m_savePreset->setEnabled(false);
    m_includeTail->setEnabled(false);
    refreshChain(selectedSlot());
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
    m_add->setEnabled(true);
    m_presets->setEnabled(true);
    m_savePreset->setEnabled(true);
    m_includeTail->setEnabled(true);
    m_loadPreset->setEnabled(m_presets->count() > 0);
    if (report.cancelled || m_cancelRequested) {
        m_status->setText(tr("Cancelled — the project was not changed"));
        m_renderButton->setEnabled(true);
        refreshChain(selectedSlot());
        return;
    }
    if (!result) {
        m_status->setText(tr("Offline render failed"));
        QMessageBox::critical(this, tr("Offline Render"),
                              QString::fromStdString(result.message()));
        m_renderButton->setEnabled(true);
        refreshChain(selectedSlot());
        return;
    }
    m_progress->setValue(1000);
    m_status->setText(tr("Offline render complete"));
    m_rendered = true;
    accept();
}
