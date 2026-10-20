#include "PluginBatchDialog.hpp"
#include "Icons.hpp"
#include "PluginEditorWindow.hpp"
#include "PluginPickerMenu.hpp"
#include "SelectionModel.hpp"
#include "Theme.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "ContextPanel.hpp"
#include "Recording/RecordingEngine.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
class SourceComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void wheelEvent(QWheelEvent* event) override {
        if (view()->isVisible()) QComboBox::wheelEvent(event);
        else event->ignore();
    }
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        icons::paint(painter, icons::Glyph::Chevron,
            QRectF(width() - 21, (height() - 12) / 2.0, 12, 12), th().textSecondary);
    }
};
}

std::vector<PluginBatchDialog::Target> PluginBatchDialog::selectedTargets(const ui::SelectionModel& selection) {
    std::vector<Target> targets;
    for (const auto& id : selection.tracks()) targets.push_back({id.toStdString(), {}});
    for (const auto& clip : selection.clips()) targets.push_back({clip.trackId.toStdString(), clip.clipId.toStdString()});
    return targets;
}

PluginBatchDialog::PluginBatchDialog(daw::EngineController& controller, std::vector<Target> targets, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_targets(std::move(targets)),
      m_projectGeneration(controller.projectGeneration()), m_projectRevision(controller.projectRevision()) {
    setObjectName("PluginBatchDialog");
    setWindowTitle(tr("Shared Plugins"));
    setWindowModality(Qt::NonModal);
    resize(440, 300);
    setMinimumSize(360, 240);
    const bool clips = !m_targets.empty() && !m_targets.front().clipId.empty();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 12);
    root->setSpacing(10);
    auto* sourceRow = new QHBoxLayout;
    m_source = new SourceComboBox(this);
    m_source->setObjectName("BatchSource");
    m_source->setMinimumHeight(36);
    m_source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_source->setMinimumContentsLength(16);
    m_source->setAccessibleName(tr("Choose preview track"));
    m_source->setToolTip(tr("Choose a track. Playback follows the timeline."));
    for (const auto& target : m_targets) {
        const auto* track = controller.project().findTrack(target.trackId);
        QString name = track ? QString::fromStdString(track->name) : tr("Unavailable");
        if (track && !target.clipId.empty()) {
            for (const auto& clip : track->clips) if (clip.id == target.clipId) {
                name += QStringLiteral(" / ") + QString::fromStdString(clip.name);
                break;
            }
        }
        m_source->addItem(icons::icon(icons::Glyph::BatchSource,
            track ? colorFromRgb(track->color) : th().textSecondary, 18), name);
        m_source->setItemData(m_source->count() - 1, name, Qt::ToolTipRole);
    }
    m_add = new QPushButton(this);
    m_add->setObjectName("BatchAddPlugin");
    m_add->setProperty("batchAction", true);
    m_add->setAutoDefault(false); m_add->setFixedSize(36, 36);
    m_add->setToolTip(tr("Add plugin…")); m_add->setAccessibleName(m_add->toolTip());
    m_add->setIconSize(QSize(20, 20));
    sourceRow->addWidget(m_source, 1); sourceRow->addWidget(m_add);
    root->addLayout(sourceRow);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("BatchScroll");
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* rackHost = new QWidget(scroll);
    m_rack = new QVBoxLayout(rackHost);
    m_rack->setContentsMargins(0, 0, 0, 0); m_rack->setSpacing(4);
    scroll->setWidget(rackHost);
    root->addWidget(scroll, 1);
    m_error = new QLabel(this);
    m_error->setObjectName("BatchError"); m_error->setWordWrap(true); m_error->hide();
    root->addWidget(m_error);
    auto* footer = new QHBoxLayout;
    m_status = new QLabel(clips ? tr("%1 clips").arg(m_targets.size()) : tr("%1 tracks").arg(m_targets.size()), this);
    m_status->setProperty("role", "secondary");
    m_status->setToolTip(tr("New effects are copied to this selection. Existing effects are kept."));
    footer->addWidget(m_status);
    footer->addStretch();
    auto* buttons = new QDialogButtonBox(this);
    m_cancel = buttons->addButton(QString(), QDialogButtonBox::RejectRole);
    m_cancel->setObjectName("BatchCancel"); m_cancel->setAutoDefault(false);
    m_cancel->setProperty("batchAction", true);
    m_cancel->setToolTip(tr("Discard changes")); m_cancel->setAccessibleName(m_cancel->toolTip());
    m_cancel->setFixedSize(36, 32); m_cancel->setIconSize(QSize(18, 18));
    m_apply = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    m_apply->setToolTip(tr("Apply to all (%1)").arg(m_targets.size()));
    m_apply->setAccessibleName(m_apply->toolTip());
    m_apply->setObjectName("BatchApply"); m_apply->setDefault(true);
    m_apply->setProperty("batchAction", true);
    m_apply->setMinimumWidth(104); m_apply->setFixedHeight(32);
    footer->addWidget(buttons);
    root->addLayout(footer);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, &PluginBatchDialog::apply);
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!switchSource(index)) { const QSignalBlocker blocker(m_source); m_source->setCurrentIndex(m_sourceIndex); }
        updatePreview();
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (!m_draft) return;
        auto* menu = ui::buildPluginMenu(this, &m_controller, false,
            [this](const auto& plugin) { addPlugin(plugin); });
        menu->exec(m_add->mapToGlobal(QPoint(0, m_add->height())));
        menu->deleteLater();
    });
    m_pump = new QTimer(this);
    connect(m_pump, &QTimer::timeout, this, [this] {
        if (m_projectGeneration != m_controller.projectGeneration()) { reject(); return; }
        if (m_controller.isRecording() || m_controller.isCountingIn()) stopAudition();
        else if (m_projectRevision != m_controller.projectRevision() || (m_draft &&
                 (m_draft->sampleRate() != m_controller.sampleRate() ||
                  m_draft->bufferSizeFrames() != m_controller.bufferSizeFrames()))) {
            if (m_controller.validatePluginBatch(m_targets)) {
                if (!switchSource(m_sourceIndex, true)) updatePreview();
            }
            else { stopAudition(); showError(tr("The selected tracks or clips are no longer available.")); }
            m_projectRevision = m_controller.projectRevision();
        }
        if (m_draft) (void)m_draft->pumpPreviewPluginEvents();
        refreshAvailability();
    });
    m_pump->start(33);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &PluginBatchDialog::applyTheme);
    applyTheme();
    const auto valid = m_controller.validatePluginBatch(m_targets);
    if (m_targets.size() < 2 || !valid) showError(tr("Select at least two compatible tracks or audio clips."));
    else if (!controller.sharedEditingAllowed()) showError(tr("This session is read-only."));
    else switchSource(0);
    rebuildRack();
}

void PluginBatchDialog::applyTheme() {
    const auto& t = th();
    setStyleSheet(QStringLiteral(R"(
QDialog#PluginBatchDialog { background: %1; color: %2; }
QLabel[role="secondary"] { color: %3; font-size: 11px; }
#BatchSource { background: %4; color: %2; border: 1px solid %5; border-radius: 7px; padding: 6px 10px; }
#BatchSource::drop-down { border: none; width: 26px; }
#BatchSource::down-arrow { image: none; width: 0; }
#BatchScroll, #BatchScroll > QWidget > QWidget { background: transparent; }
#BatchSlot { background: %4; border-radius: 7px; }
#BatchSlotName { text-align: left; background: transparent; color: %2; border: 1px solid transparent; border-radius: 5px; padding: 2px 6px; }
#BatchSlotName:hover { background: %5; }
QPushButton[batchAction="true"], QToolButton[batchAction="true"] { background: %4; border: 1px solid transparent; border-radius: 6px; padding: 0; }
QPushButton[batchAction="true"]:hover, QToolButton[batchAction="true"]:hover { background: %5; }
QPushButton[batchAction="true"]:pressed, QToolButton[batchAction="true"]:pressed { background: %7; }
QToolButton[batchAction="true"]:checked { background: %7; border-color: %5; }
#BatchApply { background: %6; color: %9; padding: 0 14px; }
#BatchApply:hover { background: %8; }
#BatchApply:disabled { background: %4; }
#BatchError { color: %2; border: 1px solid %6; border-radius: 6px; padding: 7px; }
)" ).arg(t.background.name(), t.textPrimary.name(), t.textSecondary.name(), t.surface.name(),
           t.separator().name(), t.accent.name(), t.well().name(), t.accentHighlight.name(), t.accentText().name()));
    m_add->setIcon(icons::icon(icons::Glyph::BatchAdd, t.textPrimary, 20));
    m_cancel->setIcon(icons::icon(icons::Glyph::BatchCancel, t.textSecondary, 18));
    m_apply->setIcon(QIcon());
    rebuildRack();
}

PluginBatchDialog::~PluginBatchDialog() {
    m_pump->stop(); stopAudition(); closeEditors();
    if (m_draft) m_draft->setPluginRetiringCallback({});
}

void PluginBatchDialog::done(int result) {
    m_finished = true;
    m_pump->stop(); stopAudition(); closeEditors();
    QDialog::done(result);
}

void PluginBatchDialog::showError(const QString& text) { m_error->setText(text); m_error->show(); }

bool PluginBatchDialog::switchSource(int index, bool refresh) {
    if (index < 0 || index >= int(m_targets.size())) return false;
    if (index == m_sourceIndex && !refresh) return true;
    if (m_finished || m_projectGeneration != m_controller.projectGeneration() || m_controller.isRecording() ||
        m_controller.isCountingIn()) return false;
    std::vector<std::size_t> reopen;
    for (std::size_t slot = 0; slot < m_slots.size(); ++slot)
        if (auto window = m_editors.value(QString::fromStdString(m_slots[slot])); window && window->isVisible())
            reopen.push_back(slot);
    stopAudition();
    daw::EngineController::ChannelSnapshot chain;
    if (m_draft) {
        (void)m_draft->pumpPreviewPluginEvents();
        if (const auto result = m_draft->capturePluginBatchChain(m_targets[m_sourceIndex], m_slots, chain); !result) {
            showError(QString::fromStdString(result.message())); return false;
        }
    }
    // Moving the new chain within the same mix keeps all other instruments
    // running with their current state instead of cloning the project again.
    std::shared_ptr<daw::EngineController> next = refresh ? nullptr : m_draft;
    if (!next) {
        if (const auto result = m_controller.createPluginBatchDraft(m_targets[index], next, true); !result) {
            showError(tr("Could not prepare this source: %1").arg(QString::fromStdString(result.message()))); return false;
        }
    }
    std::vector<std::vector<std::string>> ids;
    if (!chain.inserts.empty()) {
        if (const auto result = next->appendPluginBatch({m_targets[index]}, chain, ids); !result) {
            showError(QString::fromStdString(result.message())); return false;
        }
    }
    closeEditors();
    if (m_draft) m_draft->setPluginRetiringCallback({});
    if (next == m_draft) {
        const auto& previous = m_targets[m_sourceIndex];
        for (const auto& slot : m_slots) {
            if (previous.clipId.empty()) next->removeInsert(previous.trackId, slot);
            else next->removeClipFxInsert(previous.trackId, previous.clipId, slot);
        }
    }
    m_draft = std::move(next);
    m_draft->setPluginRetiringCallback([this](const std::string&, const std::string& slotId) {
        if (auto window = m_editors.value(QString::fromStdString(slotId))) {
            for (auto* editor : window->findChildren<PluginEditorWindow*>()) editor->detachFromPlugin();
            window->close();
        }
    });
    m_sourceIndex = index;
    m_projectRevision = m_controller.projectRevision();
    m_source->setToolTip(m_source->itemText(index) + QLatin1Char('\n') +
                         tr("Playback follows the timeline"));
    m_slots = ids.empty() ? std::vector<std::string>{} : std::move(ids.front());
    m_error->hide();
    rebuildRack();
    updatePreview();
    for (const auto slot : reopen) if (slot < m_slots.size()) openEditor(QString::fromStdString(m_slots[slot]));
    return true;
}

void PluginBatchDialog::refreshAvailability() {
    const bool available = !m_finished && m_projectGeneration == m_controller.projectGeneration() &&
        m_draft && !m_controller.isRecording() && !m_controller.isCountingIn() && m_controller.sharedEditingAllowed() &&
        m_controller.validatePluginBatch(m_targets).isOk();
    m_source->setEnabled(available);
    m_add->setEnabled(available && m_controller.validatePluginBatch(m_targets, m_slots.size() + 1).isOk());
    m_apply->setEnabled(available && !m_slots.empty() && m_controller.validatePluginBatch(m_targets, m_slots.size()).isOk());
}

void PluginBatchDialog::rebuildRack() {
    while (auto* item = m_rack->takeAt(0)) {
        if (auto* widget = item->widget()) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
    if (m_slots.empty()) {
        auto* empty = new QLabel(tr("Add an effect to start"), this);
        empty->setAlignment(Qt::AlignCenter); empty->setProperty("role", "secondary");
        empty->setMinimumHeight(58); m_rack->addWidget(empty);
    }
    for (std::size_t index = 0; index < m_slots.size(); ++index) {
        const auto id = m_slots[index];
        const auto& target = m_targets[m_sourceIndex];
        const auto* model = m_draft->insertModel(target.trackId, id);
        if (!model) continue;
        auto* row = new QWidget(this); row->setObjectName("BatchSlot"); row->setFixedHeight(40);
        auto* layout = new QHBoxLayout(row); layout->setContentsMargins(4, 3, 4, 3); layout->setSpacing(4);
        auto* power = new QToolButton(row);
        power->setObjectName("BatchSlotEnabled"); power->setProperty("batchAction", true);
        power->setIcon(icons::icon(icons::Glyph::BatchBypass, model->bypassed ? th().textSecondary : th().accent, 18));
        power->setIconSize(QSize(18, 18)); power->setFocusPolicy(Qt::StrongFocus);
        power->setFixedSize(32, 32); power->setCheckable(true); power->setChecked(!model->bypassed);
        power->setToolTip(tr("Enable effect")); power->setAccessibleName(power->toolTip());
        connect(power, &QToolButton::toggled, this, [this, id](bool enabled) {
            m_draft->setInsertBypassed(m_targets[m_sourceIndex].trackId, id, !enabled); rebuildRack();
        });
        layout->addWidget(power);
        auto* name = new QPushButton(QString::fromStdString(model->name), row);
        name->setAutoDefault(false); name->setObjectName("BatchSlotName");
        name->setIcon(icons::icon(icons::Glyph::BatchEdit, th().textSecondary, 18));
        name->setIconSize(QSize(18, 18)); name->setMinimumHeight(32);
        name->setToolTip(tr("Open plugin settings")); name->setAccessibleName(name->text() + ": " + name->toolTip());
        connect(name, &QPushButton::clicked, this, [this, id] { openEditor(QString::fromStdString(id)); });
        layout->addWidget(name, 1);
        auto* remove = new QToolButton(row); remove->setFixedSize(32, 32);
        remove->setObjectName("BatchSlotRemove"); remove->setProperty("batchAction", true);
        remove->setFocusPolicy(Qt::StrongFocus); remove->setIconSize(QSize(18, 18));
        remove->setIcon(icons::icon(icons::Glyph::BatchRemove, th().textSecondary, 18));
        remove->setToolTip(tr("Remove effect")); remove->setAccessibleName(remove->toolTip());
        connect(remove, &QToolButton::clicked, this, [this, id] {
            closeEditors();
            const auto& source = m_targets[m_sourceIndex];
            if (source.clipId.empty()) m_draft->removeInsert(source.trackId, id);
            else m_draft->removeClipFxInsert(source.trackId, source.clipId, id);
            std::erase(m_slots, id); rebuildRack();
        });
        layout->addWidget(remove); m_rack->addWidget(row);
    }
    m_rack->addStretch();
    refreshAvailability();
    updatePreview();
}

void PluginBatchDialog::addPlugin(const daw::plugins::PluginDescriptor& plugin, bool open) {
    if (m_finished || m_projectGeneration != m_controller.projectGeneration() || m_controller.isRecording() ||
        m_controller.isCountingIn() || !m_draft || !m_controller.sharedPluginAllowed(plugin) || !m_controller.sharedEditingAllowed() ||
        !m_controller.validatePluginBatch(m_targets, m_slots.size() + 1)) return;
    const auto& source = m_targets[m_sourceIndex];
    const auto id = source.clipId.empty() ? m_draft->addInsert(source.trackId, plugin)
        : m_draft->addClipFxInsert(source.trackId, source.clipId, plugin);
    if (id.empty() || !m_draft->hasInsert(source.trackId, id)) {
        if (!id.empty()) {
            if (source.clipId.empty()) m_draft->removeInsert(source.trackId, id);
            else m_draft->removeClipFxInsert(source.trackId, source.clipId, id);
        }
        showError(tr("This plugin could not be loaded.")); return;
    }
    m_slots.push_back(id); m_error->hide(); ui::rememberRecentPlugin(plugin); rebuildRack();
    if (open) openEditor(QString::fromStdString(id));
}

void PluginBatchDialog::openEditor(const QString& id) {
    if (auto window = m_editors.value(id)) { window->show(); window->raise(); window->activateWindow(); return; }
    const auto& track = m_targets[m_sourceIndex].trackId;
    if (!m_draft->hasInsert(track, id.toStdString())) return;
    auto* window = new QDialog(this); window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QString::fromStdString(m_draft->insertModel(track, id.toStdString())->name));
    auto* layout = new QVBoxLayout(window); layout->setContentsMargins(0, 0, 0, 0);
    auto* editor = new PluginEditorWindow(m_draft.get(), QString::fromStdString(track), id, window);
    layout->addWidget(editor); editor->installEventFilter(this);
    window->resize(720, 480); m_editors.insert(id, window);
    connect(window, &QDialog::finished, this, [this, id, editor] { editor->detachFromPlugin(); m_editors.remove(id); rebuildRack(); });
    connect(editor, &PluginEditorWindow::closing, window, &QDialog::reject);
    connect(editor, &PluginEditorWindow::nestedPluginEditorRequested, this, [this](const QString&, const QString& nested) { openEditor(nested); });
    editor->prepareNativeHostHierarchy(); window->show(); editor->initializeEditor();
}

bool PluginBatchDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        if (auto* editor = qobject_cast<PluginEditorWindow*>(watched)) {
            if (auto* window = qobject_cast<QDialog*>(editor->parentWidget())) {
                const auto size = static_cast<QResizeEvent*>(event)->size();
                if (editor->isEmbedded() && !editor->canResizeNativeEditor()) window->setFixedSize(size);
                else window->resize(size);
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void PluginBatchDialog::closeEditors() {
    const auto windows = m_editors.values(); m_editors.clear();
    for (const auto& window : windows) if (window) {
        window->disconnect(this);
        for (auto* editor : window->findChildren<PluginEditorWindow*>()) editor->detachFromPlugin();
        delete window;
    }
}

void PluginBatchDialog::stopAudition() {
    if (m_previewActive) m_controller.stopPluginAudition();
    m_previewActive = false;
}

void PluginBatchDialog::updatePreview() {
    if (m_finished || !m_draft || m_slots.empty() || m_projectGeneration != m_controller.projectGeneration() ||
        m_controller.isRecording() || m_controller.isCountingIn() || !m_controller.validatePluginBatch(m_targets)) {
        stopAudition(); return;
    }
    if (m_previewActive) return;
    if (const auto result = m_controller.startPluginAudition(m_draft, true); !result) {
        showError(QString::fromStdString(result.message())); return;
    }
    m_previewActive = true;
}

void PluginBatchDialog::apply() {
    if (m_finished || !m_draft || m_slots.empty() || m_projectGeneration != m_controller.projectGeneration()) return;
    stopAudition(); closeEditors();
    (void)m_draft->pumpPreviewPluginEvents();
    daw::EngineController::ChannelSnapshot chain;
    if (const auto result = m_draft->capturePluginBatchChain(m_targets[m_sourceIndex], m_slots, chain); !result) {
        showError(QString::fromStdString(result.message())); return;
    }
    std::vector<std::vector<std::string>> ids;
    if (const auto result = m_controller.appendPluginBatch(m_targets, chain, ids); !result) {
        showError(tr("Could not apply plugins: %1").arg(QString::fromStdString(result.message()))); return;
    }
    accept();
}

bool PluginBatchDialog::checkForTest(const QString& screenshotPath) {
    if (!icons::checkGlyphCoverageForTest()) return false;
    daw::EngineController controller{};
    if (!controller.initialize(48000, 256, false)) return false;
    const auto lead = controller.addTrack(daw::TrackKind::Audio, "Lead");
    const auto doubleTrack = controller.addTrack(daw::TrackKind::Audio, "Double");
    const auto right = controller.addTrack(daw::TrackKind::Midi, "R");
    const std::vector<Target> targets{{lead, {}}, {doubleTrack, {}}, {right, {}}};
    const auto undo = controller.undoDepth();
    const auto& eq = daw::plugins::equalizer::EqualizerInstance::staticDescriptor();
    {
        PluginBatchDialog cancelled(controller, targets);
        cancelled.addPlugin(eq, false);
        controller.play();
        cancelled.reject();
        if (!controller.isPlaying() || cancelled.m_previewActive) return false;
        controller.stop();
    }
    if (controller.undoDepth() != undo || !controller.project().findTrack(lead)->inserts.empty()) return false;
    PluginBatchDialog dialog(controller, targets);
    if (!dialog.m_draft || dialog.m_apply->isEnabled() || !dialog.m_slots.empty() ||
        dialog.isModal() || dialog.findChild<QPushButton*>("BatchListen")) return false;
    dialog.addPlugin(eq, false);
    if (dialog.m_slots.size() != 1 || !dialog.m_apply->isEnabled() || !dialog.m_previewActive ||
        controller.isPlaying() || dialog.m_draft->isPlaying()) return false;
    controller.play();
    if (!controller.isPlaying() || !dialog.m_draft->isPlaying()) return false;
    controller.pause(); controller.seekSeconds(.25);
    if (dialog.m_draft->isPlaying() || std::abs(dialog.m_draft->positionSeconds() - .25) > 1e-6) return false;
    controller.play();
    dialog.m_draft->setInsertParameter(lead, dialog.m_slots.front(), "output.gain", -6.0);
    const auto previousDraft = dialog.m_draft;
    const auto previousSlot = dialog.m_slots.front();
    dialog.m_source->setCurrentIndex(1);
    if (dialog.m_sourceIndex != 1 || dialog.m_slots.size() != 1 || !dialog.m_previewActive ||
        !controller.isPlaying() || !dialog.m_draft->isPlaying() || dialog.m_draft != previousDraft ||
        dialog.m_draft->hasInsert(lead, previousSlot)) return false;
    const auto* model = dialog.m_draft->insertModel(doubleTrack, dialog.m_slots.front());
    const auto gain = std::find_if(model->parameters.begin(), model->parameters.end(), [](const auto& parameter) { return parameter.id == "output.gain"; });
    if (gain == model->parameters.end() || std::abs(gain->value + 6.0) > 0.01) return false;
    dialog.show(); QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (QApplication::activeModalWidget() == &dialog) return false;
    for (auto* button : dialog.findChildren<QAbstractButton*>()) {
        if (button->property("batchAction").toBool() &&
            (button->accessibleName().isEmpty() || (button != dialog.m_apply && button->icon().isNull()) ||
             button->focusPolicy() == Qt::NoFocus)) return false;
    }
    if (dialog.m_apply->text() != tr("Apply") || !dialog.m_apply->icon().isNull()) return false;
    auto* power = dialog.findChild<QToolButton*>("BatchSlotEnabled");
    if (!power) return false;
    power->click();
    if (!dialog.m_draft->insertModel(doubleTrack, dialog.m_slots.front())->bypassed ||
        !controller.project().findTrack(doubleTrack)->inserts.empty()) return false;
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog.findChild<QToolButton*>("BatchSlotEnabled")->click();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const auto size : {QSize(360, 240), QSize(440, 300)}) {
        dialog.resize(size); QApplication::processEvents();
        for (auto* button : dialog.findChildren<QAbstractButton*>()) {
            if (button->isVisible() && button->property("batchAction").toBool() &&
                !dialog.rect().contains(QRect(button->mapTo(&dialog, QPoint()), button->size()))) return false;
        }
        if (dialog.m_source->width() < 100) return false;
    }
    dialog.openEditor(QString::fromStdString(dialog.m_slots.front()));
    auto editor = dialog.m_editors.value(QString::fromStdString(dialog.m_slots.front()));
    if (!editor || !editor->isVisible() || editor->isModal()) return false;
    controller.pause();
    if (dialog.m_draft->isPlaying()) return false;
    controller.play();
    dialog.m_source->setCurrentIndex(0);
    if (editor || dialog.m_editors.size() != 1 || !controller.isPlaying() || !dialog.m_draft->isPlaying()) return false;
    dialog.closeEditors();
    dialog.m_source->setCurrentIndex(1);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    if (!dialog.m_rack->itemAt(0)->widget() || !dialog.m_rack->itemAt(0)->widget()->isVisible()) return false;
    if (!screenshotPath.isEmpty()) {
        if (!dialog.grab().save(screenshotPath)) return false;
        dialog.m_source->showPopup(); QApplication::processEvents();
        if (!dialog.m_source->view()->window()->grab().save(screenshotPath + ".tracks.png")) return false;
        dialog.m_source->hidePopup();
    }
    if (dialog.m_apply->geometry().bottom() >= dialog.height() || dialog.m_source->width() < 100) return false;
    dialog.apply();
    if (dialog.result() != QDialog::Accepted || controller.undoDepth() != undo + 1 || !controller.isPlaying()) return false;
    controller.stop();
    for (const auto& target : targets) if (controller.project().findTrack(target.trackId)->inserts.size() != 1) return false;
    ui::SelectionModel selection;
    selection.setTracks({QString::fromStdString(lead), QString::fromStdString(right)});
    QWidget host; host.resize(1000, 120);
    ContextPanel panel(&controller, &selection, &host);
    panel.rebuild();
    auto* button = panel.findChild<QAbstractButton*>("ContextPanelSharedPlugins");
    if (!button || !button->isEnabled()) return false;
    bool requested = false;
    QObject::connect(&panel, &ContextPanel::sharedPluginsRequested, [&] { requested = true; });
    button->click();
    if (!requested) return false;
    const auto automation = controller.addTrack(daw::TrackKind::Automation, "Automation");
    selection.setTracks({QString::fromStdString(lead), QString::fromStdString(automation)});
    panel.rebuild();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (panel.findChild<QAbstractButton*>("ContextPanelSharedPlugins")) return false;
    QTemporaryDir media;
    const auto file = media.filePath("source.wav").toStdString();
    audio::AudioBuffer samples(2, 4800);
    audio::AudioRecorder recorder; recorder.initialize(48000, 2);
    if (!recorder.writeWAVFile(file, samples, 48000)) return false;
    const auto firstClip = controller.importAudio(file, lead, 0.0);
    const auto secondClip = controller.importAudio(file, doubleTrack, 0.0);
    const std::vector<Target> clipTargets{{lead, firstClip}, {doubleTrack, secondClip}};
    PluginBatchDialog clipDialog(controller, clipTargets);
    clipDialog.addPlugin(eq, false);
    clipDialog.m_source->setCurrentIndex(1);
    if (clipDialog.m_sourceIndex != 1 || clipDialog.m_slots.size() != 1) return false;
    clipDialog.apply();
    if (clipDialog.result() != QDialog::Accepted || controller.clipFx(lead, firstClip)->size() != 1 ||
        controller.clipFx(doubleTrack, secondClip)->size() != 1 || controller.project().findTrack(lead)->inserts.size() != 1) return false;
    selection.setClips({{QString::fromStdString(lead), QString::fromStdString(firstClip)},
                       {QString::fromStdString(doubleTrack), QString::fromStdString(secondClip)}});
    panel.rebuild();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    button = panel.findChild<QAbstractButton*>("ContextPanelSharedPlugins");
    if (!button || !button->isEnabled()) return false;
    std::puts("PASS Shared Plugins: modeless timeline playback, source switching, icon controls, cancel, track/clip apply and context actions");
    return true;
}
