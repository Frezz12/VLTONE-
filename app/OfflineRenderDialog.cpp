#include "OfflineRenderDialog.hpp"

#include "ChannelStrip.hpp"
#include "ChannelStripPreset.hpp"
#include "ChannelStripPresets.hpp"
#include "PluginEditorWindow.hpp"
#include "SamplerPanel.hpp"
#include "Theme.hpp"
#include "Internal/EqualizerInstance.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSizePolicy>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kPresetPathRole = Qt::UserRole + 1;

/// The preset shelf behaves like a desktop collection: one click applies a
/// card, while the same card can be dragged onto the Audio FX rack. File URLs
/// deliberately reuse ChannelStrip's existing preset drop path.
class OfflinePresetList final : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        auto* data = new QMimeData;
        QList<QUrl> urls;
        for (const QListWidgetItem* item : items) {
            const QString path = item->data(kPresetPathRole).toString();
            if (!path.isEmpty()) urls.push_back(QUrl::fromLocalFile(path));
        }
        data->setUrls(urls);
        return data;
    }

    Qt::DropActions supportedDropActions() const override {
        return Qt::CopyAction;
    }
};

} // namespace

OfflineRenderDialog::OfflineRenderDialog(
    daw::EngineController& controller,
    std::vector<daw::EngineController::ClipAddress> clips,
    QWidget* parent)
    : QDialog(parent), m_controller(controller), m_clips(std::move(clips)),
      m_scratch() {
    setWindowTitle(tr("Offline Render"));
    setModal(true);
    setObjectName(QStringLiteral("OfflineRenderDialog"));
    resize(700, 500);
    setMinimumSize(620, 440);

    const audio::Result ready =
        m_scratch.initialize(controller.sampleRate(),
                             controller.bufferSizeFrames(), false);
    if (ready) m_chainTrackId = m_scratch.addTrack(daw::TrackKind::Audio,
                                                   "Offline Processing");
    m_scratch.setPluginRetiringCallback(
        [this](const std::string& channelId, const std::string& slotId) {
            const QString channel = QString::fromStdString(channelId);
            const QString slot = QString::fromStdString(slotId);
            if (channel != QString::fromStdString(m_chainTrackId)) return;
            QDialog* window = m_editors.value(slot);
            if (!window) return;
            for (PluginEditorWindow* editor :
                 window->findChildren<PluginEditorWindow*>())
                editor->detachFromPlugin();
            window->close();
        });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    m_status = new QLabel(tr("Ready"), this);
    m_status->setObjectName(QStringLiteral("OfflineRenderStatus"));
    m_status->setAccessibleName(tr("Offline render status"));
    m_progressPercent = new QLabel(QStringLiteral("0%"), this);
    m_progressPercent->setObjectName(QStringLiteral("OfflineProgressPercent"));
    m_progressPercent->setAccessibleName(tr("Offline render percentage"));
    statusRow->addWidget(m_status);
    statusRow->addStretch(1);
    statusRow->addWidget(m_progressPercent);
    root->addLayout(statusRow);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("OfflineRenderProgress"));
    m_progress->setAccessibleName(tr("Offline render progress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFixedHeight(4);
    m_progress->setTextVisible(false);
    root->addWidget(m_progress);

    auto* content = new QHBoxLayout;
    content->setSpacing(12);

    auto* effects = new QWidget(this);
    effects->setObjectName(QStringLiteral("OfflineEffectsColumn"));
    effects->setFixedWidth(238);
    auto* effectsLayout = new QVBoxLayout(effects);
    effectsLayout->setContentsMargins(0, 0, 0, 0);
    effectsLayout->setSpacing(10);

    m_rackHost = new QWidget(effects);
    m_rackHost->setAccessibleName(tr("Offline processing inserts"));
    m_rackLayout = new QVBoxLayout(m_rackHost);
    m_rackLayout->setContentsMargins(0, 0, 0, 0);
    m_rackLayout->setAlignment(Qt::AlignTop);
    effectsLayout->addWidget(m_rackHost);

    auto* presetPanel = new QWidget(effects);
    presetPanel->setObjectName(QStringLiteral("OfflinePresetPanel"));
    auto* presetLayout = new QVBoxLayout(presetPanel);
    presetLayout->setContentsMargins(9, 8, 9, 9);
    presetLayout->setSpacing(6);
    auto* presetHeader = new QHBoxLayout;
    presetHeader->setContentsMargins(0, 0, 0, 0);
    auto* presetTitle = new QLabel(tr("Presets"), presetPanel);
    presetTitle->setObjectName(QStringLiteral("OfflinePresetTitle"));
    m_savePreset = new QPushButton(tr("Save…"), presetPanel);
    m_savePreset->setObjectName(QStringLiteral("OfflinePresetSave"));
    m_savePreset->setAutoDefault(false);
    m_savePreset->setFixedHeight(26);
    presetHeader->addWidget(presetTitle);
    presetHeader->addStretch(1);
    presetHeader->addWidget(m_savePreset);
    presetLayout->addLayout(presetHeader);

    m_presets = new OfflinePresetList(presetPanel);
    m_presets->setObjectName(QStringLiteral("OfflinePresetCards"));
    m_presets->setAccessibleName(tr("Offline render presets"));
    m_presets->setViewMode(QListView::IconMode);
    m_presets->setFlow(QListView::LeftToRight);
    m_presets->setMovement(QListView::Static);
    m_presets->setResizeMode(QListView::Adjust);
    m_presets->setWrapping(true);
    m_presets->setWordWrap(true);
    m_presets->setUniformItemSizes(true);
    m_presets->setGridSize(QSize(100, 48));
    m_presets->setSpacing(3);
    m_presets->setDragEnabled(true);
    m_presets->setDragDropMode(QAbstractItemView::DragOnly);
    m_presets->setDefaultDropAction(Qt::CopyAction);
    m_presets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_presets->setMinimumHeight(116);
    presetLayout->addWidget(m_presets);
    effectsLayout->addWidget(presetPanel, 1);
    content->addWidget(effects);

    auto* clipsBox = new QGroupBox(tr("Clips to render"), this);
    clipsBox->setObjectName(QStringLiteral("OfflineClipsPanel"));
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

    m_includeTail = new QCheckBox(tr("Include Tail"), this);
    m_includeTail->setObjectName(QStringLiteral("OfflineIncludeTail"));
    QSettings settings;
    m_includeTail->setChecked(
        settings.value(QStringLiteral("offlineRender/includeTail"), false)
            .toBool());
    root->addWidget(m_includeTail);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    m_autoRender = new QPushButton(tr("Auto Render"), this);
    m_autoRender->setObjectName(QStringLiteral("OfflineAutoRender"));
    m_autoRender->setAccessibleName(tr("Auto Render"));
    m_autoRender->setToolTip(
        tr("Render the selected clips whenever the offline effect chain changes"));
    m_autoRender->setCheckable(true);
    m_autoRender->setAutoDefault(false);
    m_autoRender->setFixedHeight(28);
    footer->addWidget(m_autoRender);
    footer->addStretch(1);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_closeButton = m_buttons->button(QDialogButtonBox::Cancel);
    m_closeButton->setText(tr("Close"));
    m_closeButton->setFixedHeight(28);
    m_renderButton = m_buttons->addButton(tr("Render to Clips"),
                                         QDialogButtonBox::AcceptRole);
    m_renderButton->setObjectName(QStringLiteral("OfflineRenderStart"));
    m_renderButton->setDefault(true);
    m_renderButton->setFixedHeight(28);
    m_renderButton->setMaximumWidth(148);
    footer->addWidget(m_buttons);
    root->addLayout(footer);

    connect(m_savePreset, &QPushButton::clicked, this,
            &OfflineRenderDialog::savePreset);
    const auto applyCard = [this](QListWidgetItem* item) {
        if (item) loadPreset(item->data(kPresetPathRole).toString());
    };
    connect(m_presets, &QListWidget::itemClicked, this, applyCard);
    connect(m_presets, &QListWidget::itemActivated, this, applyCard);
    connect(m_autoRender, &QPushButton::toggled, this, [this](bool on) {
        if (on) scheduleAutoRender();
    });
    connect(m_renderButton, &QPushButton::clicked, this,
            &OfflineRenderDialog::startRender);
    connect(m_buttons, &QDialogButtonBox::rejected, this,
            &OfflineRenderDialog::reject);
    connect(m_clipList, &QListWidget::itemChanged, this,
            &OfflineRenderDialog::updateRenderAvailability);

    m_autoRenderTimer = new QTimer(this);
    m_autoRenderTimer->setSingleShot(true);
    connect(m_autoRenderTimer, &QTimer::timeout, this,
            &OfflineRenderDialog::startRender);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &OfflineRenderDialog::applyTheme);

    reloadPresets();
    rebuildRack();
    updateRenderAvailability();
    applyTheme();
    if (!ready || m_chainTrackId.empty()) {
        m_renderButton->setEnabled(false);
        m_autoRender->setEnabled(false);
        m_status->setText(tr("Could not create the offline plugin rack"));
    }
}

void OfflineRenderDialog::reject() {
    if (m_rendering) {
        m_cancelRequested = true;
        return;
    }
    closeEditors();
    QDialog::reject();
}

OfflineRenderDialog::~OfflineRenderDialog() {
    closeEditors();
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
            [this] { rackChanged(false); }, Qt::QueuedConnection);
    updateRenderAvailability();
}

void OfflineRenderDialog::rackChanged(bool settle) {
    rebuildRack();
    scheduleAutoRender(settle ? 240 : 0);
}

void OfflineRenderDialog::scheduleAutoRender(int delayMs) {
    if (!m_autoRender || !m_autoRender->isChecked()) return;
    if (m_rendering) {
        m_autoRenderPending = true;
        return;
    }
    updateRenderAvailability();
    if (!m_renderButton->isEnabled()) return;
    m_autoRenderTimer->start(std::max(0, delayMs));
}

void OfflineRenderDialog::updateRenderAvailability() {
    int checked = 0;
    for (int row = 0; row < m_clipList->count(); ++row)
        if (m_clipList->item(row)->checkState() == Qt::Checked) ++checked;
    m_clipSummary->setText(
        tr("%1 of %2 selected").arg(checked).arg(m_clipList->count()));
    const auto* inserts = m_scratch.channelInserts(m_chainTrackId);
    const bool hasInserts = inserts && !inserts->empty();
    const bool enabled = inserts && std::any_of(inserts->begin(), inserts->end(),
        [](const auto& slot) { return !slot.bypassed; });
    m_renderButton->setEnabled(!m_rendering && checked > 0 && enabled);
    m_savePreset->setEnabled(!m_rendering && hasInserts);
    m_renderButton->setAccessibleName(
        tr("Render to Clips — %1 selected").arg(checked));
    if (!m_rendering)
        m_status->setText(!checked ? tr("Check the clips to render")
                                  : enabled ? tr("Ready — a new version will be saved")
                                            : tr("Add or enable an effect to render"));
}

void OfflineRenderDialog::openEditor(const QString& insertId) {
    if (insertId.isEmpty()) return;
    if (QDialog* open = m_editors.value(insertId)) {
        open->show();
        open->raise();
        open->activateWindow();
        return;
    }
    if (!m_scratch.hasInsert(m_chainTrackId, insertId.toStdString())) {
        QMessageBox::information(this, tr("Offline Render"),
                                 tr("This plugin is not available."));
        return;
    }

    const auto* model = m_scratch.insertModel(m_chainTrackId,
                                               insertId.toStdString());
    auto* window = new QDialog(this);
    window->setObjectName(QStringLiteral("OfflinePluginEditorWindow"));
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowModality(Qt::NonModal);
    window->setWindowTitle(model ? QString::fromStdString(model->name)
                                 : tr("Plugin"));
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* editor = new PluginEditorWindow(
        &m_scratch, QString::fromStdString(m_chainTrackId), insertId, window);
    layout->addWidget(editor);
    editor->installEventFilter(this);
    window->resize(720, 480);
    m_editors.insert(insertId, window);
    connect(window, &QDialog::finished, this, [this, insertId, editor] {
        editor->detachFromPlugin();
        m_editors.remove(insertId);
        QTimer::singleShot(0, this, &OfflineRenderDialog::rebuildRack);
    });
    connect(editor, &PluginEditorWindow::closing, window, &QDialog::reject);
    connect(editor, &PluginEditorWindow::projectEdited, this,
            [this] { rackChanged(true); }, Qt::QueuedConnection);
    connect(editor, &PluginEditorWindow::nestedPluginEditorRequested, this,
            [this](const QString&, const QString& id) { openEditor(id); });
    editor->prepareNativeHostHierarchy();
    window->show();
    editor->initializeEditor();
}

bool OfflineRenderDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        if (auto* editor = qobject_cast<PluginEditorWindow*>(watched)) {
            if (auto* window = qobject_cast<QDialog*>(editor->parentWidget())) {
                const QSize requested =
                    static_cast<QResizeEvent*>(event)->size();
                if (editor->isEmbedded() && !editor->canResizeNativeEditor()) {
                    window->setFixedSize(requested);
                } else {
                    window->resize(requested);
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void OfflineRenderDialog::closeEditors() {
    const auto windows = m_editors.values();
    m_editors.clear();
    for (const QPointer<QDialog>& window : windows) {
        if (!window) continue;
        window->disconnect(this);
        for (PluginEditorWindow* editor :
             window->findChildren<PluginEditorWindow*>()) {
            editor->disconnect(this);
            editor->detachFromPlugin();
        }
        delete window;
    }
}

void OfflineRenderDialog::reloadPresets() {
    const QString current = m_presets->currentItem()
                                ? m_presets->currentItem()
                                      ->data(kPresetPathRole)
                                      .toString()
                                : QString();
    m_presets->clear();
    for (const QString& file : ui::channelstrippresets::offlineFiles()) {
        daw::EngineController::ChannelSnapshot preset;
        const audio::Result loaded =
            daw::ChannelStripPreset::load(preset, file.toStdString());
        QString name = loaded
                           ? QString::fromStdString(preset.sourceName).trimmed()
                           : QString();
        if (name.isEmpty() && loaded && !preset.inserts.empty())
            name = QString::fromStdString(preset.inserts.front().model.name);
        if (name.isEmpty())
            name = ui::channelstrippresets::displayName(file);
        auto* item = new QListWidgetItem(name, m_presets);
        item->setData(kPresetPathRole, file);
        item->setTextAlignment(Qt::AlignCenter);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                       Qt::ItemIsDragEnabled);
        QStringList plugins;
        if (loaded) {
            for (const auto& slot : preset.inserts)
                plugins << QString::fromStdString(slot.model.name);
        }
        item->setToolTip(
            plugins.isEmpty()
                ? tr("Click to load. Drag onto Audio FX.")
                : tr("%1\n\nClick to load. Drag onto Audio FX.")
                      .arg(plugins.join(QStringLiteral(" · "))));
    }
    if (m_presets->count() == 0) {
        auto* empty = new QListWidgetItem(tr("No saved presets"), m_presets);
        empty->setTextAlignment(Qt::AlignCenter);
        empty->setFlags(Qt::NoItemFlags);
    }
    for (int row = 0; row < m_presets->count(); ++row) {
        if (m_presets->item(row)->data(kPresetPathRole).toString() == current) {
            m_presets->setCurrentRow(row);
            break;
        }
    }
}

void OfflineRenderDialog::loadPreset(const QString& path) {
    if (path.isEmpty()) return;
    daw::EngineController::ChannelSnapshot preset;
    const audio::Result result =
        daw::ChannelStripPreset::load(preset, path.toStdString());
    if (!result) {
        QMessageBox::critical(this, tr("Load preset"),
                              QString::fromStdString(result.message()));
        return;
    }
    if (!m_scratch.pasteChannelInserts(m_chainTrackId, preset)) {
        QMessageBox::warning(this, tr("Load preset"),
                             tr("This preset has no plugins to load."));
        return;
    }
    rackChanged(false);
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
    for (int row = 0; row < m_presets->count(); ++row) {
        if (m_presets->item(row)->data(kPresetPathRole).toString() == path) {
            m_presets->setCurrentRow(row);
            break;
        }
    }
}

void OfflineRenderDialog::startRender() {
    if (m_rendering || !m_renderButton->isEnabled()) return;
    m_autoRenderTimer->stop();
    m_autoRenderPending = false;
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
    m_progress->setValue(0);
    m_progressPercent->setText(QStringLiteral("0%"));
    m_renderButton->setEnabled(false);
    if (m_rack) m_rack->setEnabled(false);
    for (PluginEditorWindow* editor : findChildren<PluginEditorWindow*>())
        editor->setEnabled(false);
    m_presets->setEnabled(false);
    m_savePreset->setEnabled(false);
    m_autoRender->setEnabled(false);
    m_includeTail->setEnabled(false);
    m_clipList->setEnabled(false);
    m_closeButton->setText(tr("Cancel"));
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
            const int percentage =
                std::clamp(int(std::round(progress.fraction * 100.0)), 0, 100);
            m_progress->setValue(percentage);
            m_progressPercent->setText(QString::number(percentage) +
                                       QLatin1Char('%'));
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
    m_autoRender->setEnabled(true);
    m_includeTail->setEnabled(true);
    m_clipList->setEnabled(true);
    m_closeButton->setText(tr("Close"));
    updateRenderAvailability();
    const bool rerender = m_autoRenderPending;
    m_autoRenderPending = false;
    if (report.cancelled || m_cancelRequested) {
        m_status->setText(tr("Cancelled — the project was not changed"));
        return;
    }
    if (!result) {
        m_status->setText(tr("Offline render failed"));
        QMessageBox::critical(this, tr("Offline Render"),
                              QString::fromStdString(result.message()));
        return;
    }
    m_progress->setValue(100);
    m_progressPercent->setText(QStringLiteral("100%"));
    m_status->setText(tr("Offline render complete"));
    m_rendered = true;
    if (rerender) scheduleAutoRender();
}

void OfflineRenderDialog::applyTheme() {
    const Theme& t = th();
    const QColor hover = mixColors(t.surfaceElevated, t.accent, 0.12);
    const QColor selected = mixColors(t.surface, t.accent, 0.20);
    setStyleSheet(QString(R"(
#OfflineRenderDialog { background: %BG%; color: %TEXT%; }
#OfflineRenderStatus { color: %TEXT2%; }
#OfflineProgressPercent { color: %TEXT%; font-weight: 700; }
#OfflineRenderProgress { background: %WELL%; border: none; border-radius: 2px; }
#OfflineRenderProgress::chunk { background: %ACCENT%; border-radius: 2px; }
#OfflinePresetPanel, #OfflineClipsPanel {
    background: %SURFACE%; border: 1px solid %SEP%; border-radius: %RADIUS%px;
}
#OfflinePresetTitle { color: %TEXT%; font-weight: 600; }
#OfflinePresetSave {
    background: transparent; color: %TEXT2%; border: 1px solid %SEP%;
    border-radius: %RADIUS%px; padding: 2px 8px;
}
#OfflinePresetSave:hover { color: %TEXT%; background: %HOVER%; }
#OfflinePresetCards {
    background: %WELL%; color: %TEXT%; border: 1px solid %SEP%;
    border-radius: %RADIUS%px; outline: none;
}
#OfflinePresetCards::item {
    background: %ELEVATED%; color: %TEXT%; border: 1px solid %SEP%;
    border-radius: %RADIUS%px; padding: 5px; margin: 2px;
}
#OfflinePresetCards::item:hover { background: %HOVER%; }
#OfflinePresetCards::item:selected { background: %SELECTED%; border-color: %ACCENT%; }
#OfflinePresetCards::item:disabled { background: transparent; color: %TEXT2%; border: none; }
#OfflineClipsPanel { margin-top: 8px; padding-top: 10px; }
#OfflineClipsPanel::title {
    subcontrol-origin: margin; left: 12px; padding: 0 4px;
    color: %TEXT%; font-weight: 600;
}
#OfflineRenderClips {
    background: %WELL%; color: %TEXT%; border: 1px solid %SEP%;
    border-radius: %RADIUS%px; alternate-background-color: %ALT%; outline: none;
}
#OfflineRenderClips::item { border: none; padding: 4px 6px; }
#OfflineRenderClips::item:selected { background: %SELECTED%; color: %TEXT%; }
#OfflineAutoRender, #OfflineRenderStart {
    min-height: 20px; padding: 3px 10px;
}
)").replace("%RADIUS%", QString::number(Theme::cornerRadius))
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%ELEVATED%", t.surfaceElevated.name())
        .replace("%WELL%", t.well().name())
        .replace("%ALT%", mixColors(t.well(), t.surfaceElevated, 0.35).name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%SEP%", t.separator().name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%HOVER%", hover.name())
        .replace("%SELECTED%", selected.name()));
}

bool OfflineRenderDialog::checkForTest(const QString& screenshotPath) {
    QTemporaryDir dir;
    if (!dir.isValid()) return false;
    qputenv("DAW_PRESET_ROOT", dir.filePath(QStringLiteral("presets")).toUtf8());
    daw::EngineController controller{};
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
    const QString preset =
        ui::channelstrippresets::offlineFilePathForName(QStringLiteral("Vocal Chain"));
    auto snapshot = dialog.m_scratch.copyChannelStrip(dialog.m_chainTrackId, true);
    snapshot.sourceName = "Vocal Chain";
    snapshot.volume = 0.25f;
    snapshot.pan = 0.5f;
    const auto savedPreset = daw::ChannelStripPreset::save(
        snapshot, preset.toStdString());
    check(bool(savedPreset), "offline chain template saves");
    if (!savedPreset) return false;
    dialog.reloadPresets();
    check(dialog.m_presets->count() == 1 &&
              dialog.m_presets->item(0)->text() == QStringLiteral("Vocal Chain") &&
              dialog.m_presets->dragEnabled() &&
              dialog.m_presets->item(0)->flags().testFlag(Qt::ItemIsDragEnabled),
          "offline presets are named, clickable drag cards");
    check(dialog.m_scratch.pasteChannelInserts(dialog.m_chainTrackId, {}),
          "draft rack clears before applying the card");
    dialog.rebuildRack();
    dialog.loadPreset(preset);
    const auto loadedStrip =
        dialog.m_scratch.copyChannelStrip(dialog.m_chainTrackId, true);
    check(loadedStrip.inserts.size() == 1 && loadedStrip.volume == 1.0f &&
              loadedStrip.pan == 0.0f,
          "preset card loads plugins without channel-strip settings");
    check(dialog.m_scratch.pasteChannelInserts(dialog.m_chainTrackId, {}),
          "draft rack clears before testing the card drop");
    dialog.rebuildRack();
    QMimeData presetMime;
    presetMime.setUrls({QUrl::fromLocalFile(preset)});
    QDragEnterEvent enter(QPoint(12, 12), Qt::CopyAction, &presetMime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dialog.m_rack, &enter);
    QDropEvent drop(QPointF(12, 12), Qt::CopyAction, &presetMime,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dialog.m_rack, &drop);
    QApplication::processEvents();
    check(enter.isAccepted() && drop.isAccepted() &&
              dialog.m_scratch.channelInserts(dialog.m_chainTrackId)->size() == 1,
          "dragging a preset card onto Audio FX loads its plugins");
    check(!dialog.m_rack->findChild<QToolButton*>(
              QStringLiteral("ChannelStripMenu")),
          "offline Audio FX has no shared channel-strip settings menu");
    const QString loadedId = QString::fromStdString(
        dialog.m_scratch.channelInserts(dialog.m_chainTrackId)->front().id);
    dialog.openEditor(loadedId);
    QApplication::processEvents();
    QDialog* pluginWindow = dialog.m_editors.value(loadedId);
    check(pluginWindow && pluginWindow->isWindow() && pluginWindow->isVisible() &&
              pluginWindow != &dialog,
          "plugin editor opens in its own window");
    if (pluginWindow) pluginWindow->close();
    QApplication::processEvents();
    dialog.m_clipList->item(1)->setCheckState(Qt::Unchecked);
    check(dialog.m_renderButton->isEnabled(), "one checked clip remains renderable");
    dialog.m_clipList->item(0)->setCheckState(Qt::Unchecked);
    check(!dialog.m_renderButton->isEnabled(), "zero checked clips disables render");
    dialog.m_clipList->item(0)->setCheckState(Qt::Checked);
    QApplication::processEvents();
    check(dialog.m_rack->width() <= 238 && dialog.height() <= 550,
          "rack and render controls remain compact");
    if (!screenshotPath.isEmpty()) check(dialog.grab().save(screenshotPath), "dialog screenshot saved");
    dialog.m_autoRender->setChecked(true);
    QElapsedTimer autoRenderWait;
    autoRenderWait.start();
    while (!dialog.rendered() && autoRenderWait.elapsed() < 5000)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    check(dialog.rendered() && dialog.isVisible() &&
              dialog.m_progressPercent->text() == QStringLiteral("100%") &&
              controller.audioClip(track, first)->offlineHistory.size() == 2 &&
              controller.audioClip(track, second)->offlineHistory.empty(),
          "Auto Render replaces only checked clips, reports percent, and keeps the dialog open");
    dialog.m_autoRender->setChecked(false);
    dialog.reject();
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
