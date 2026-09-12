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

namespace {
class SourceComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
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
    : QDialog(parent), m_controller(controller), m_targets(std::move(targets)) {
    setObjectName("PluginBatchDialog");
    setWindowTitle(tr("Shared Plugins"));
    setWindowModality(Qt::WindowModal);
    resize(480, 380);
    setMinimumSize(440, 340);
    const bool clips = !m_targets.empty() && !m_targets.front().clipId.empty();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(10);
    auto* heading = new QLabel(clips ? tr("Clip FX · %1 clips").arg(m_targets.size())
                                    : tr("Inserts · %1 tracks").arg(m_targets.size()), this);
    heading->setObjectName("BatchHeading");
    root->addWidget(heading);
    auto* hint = new QLabel(tr("Listen to one source and configure the new effects. Apply independent copies to the entire selection."), this);
    hint->setWordWrap(true); hint->setProperty("role", "secondary");
    root->addWidget(hint);
    auto* sourceLabel = new QLabel(tr("Source for listening"), this);
    root->addWidget(sourceLabel);
    auto* sourceRow = new QHBoxLayout;
    m_source = new SourceComboBox(this);
    m_source->setObjectName("BatchSource");
    m_source->setMinimumHeight(32);
    m_source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_source->setMinimumContentsLength(16);
    sourceLabel->setBuddy(m_source);
    m_source->setAccessibleName(sourceLabel->text());
    for (const auto& target : m_targets) {
        const auto* track = controller.project().findTrack(target.trackId);
        QString name = track ? QString::fromStdString(track->name) : tr("Unavailable");
        if (track && !target.clipId.empty()) {
            for (const auto& clip : track->clips) if (clip.id == target.clipId) {
                name += QStringLiteral(" / ") + QString::fromStdString(clip.name);
                break;
            }
        }
        m_source->addItem(name);
        m_source->setItemData(m_source->count() - 1, name, Qt::ToolTipRole);
    }
    m_listen = new QPushButton(tr("Listen"), this);
    m_listen->setObjectName("BatchListen");
    m_listen->setAutoDefault(false);
    m_listen->setMinimumHeight(32);
    m_listen->setIcon(icons::icon(icons::Glyph::Play, th().textPrimary, 14));
    sourceRow->addWidget(m_source, 1); sourceRow->addWidget(m_listen);
    root->addLayout(sourceRow);
    auto* rackHeader = new QHBoxLayout;
    rackHeader->addWidget(new QLabel(tr("New effects"), this));
    rackHeader->addStretch();
    m_add = new QPushButton(tr("Add plugin…"), this);
    m_add->setObjectName("BatchAddPlugin");
    m_add->setAutoDefault(false); m_add->setFlat(true);
    m_add->setIcon(icons::icon(icons::Glyph::Plus, th().textSecondary, 14));
    rackHeader->addWidget(m_add);
    root->addLayout(rackHeader);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("BatchScroll");
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* rackHost = new QWidget(scroll);
    m_rack = new QVBoxLayout(rackHost);
    m_rack->setContentsMargins(0, 0, 0, 0); m_rack->setSpacing(2);
    scroll->setWidget(rackHost);
    root->addWidget(scroll, 1);
    m_status = new QLabel(tr("Existing effects are kept. Cancel leaves the project unchanged."), this);
    m_status->setWordWrap(true); m_status->setProperty("role", "secondary");
    root->addWidget(m_status);
    m_error = new QLabel(this);
    m_error->setObjectName("BatchError"); m_error->setWordWrap(true); m_error->hide();
    root->addWidget(m_error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_apply = buttons->addButton(tr("Apply to all (%1)").arg(m_targets.size()), QDialogButtonBox::AcceptRole);
    m_apply->setObjectName("BatchApply"); m_apply->setDefault(true); m_apply->setMinimumHeight(32);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, &PluginBatchDialog::apply);
    connect(m_listen, &QPushButton::clicked, this, &PluginBatchDialog::toggleAudition);
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!switchSource(index)) { const QSignalBlocker blocker(m_source); m_source->setCurrentIndex(m_sourceIndex); }
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (!m_draft) return;
        auto* menu = ui::buildPluginMenu(this, m_draft.get(), false,
            [this](const auto& plugin) { addPlugin(plugin); });
        menu->exec(m_add->mapToGlobal(QPoint(0, m_add->height())));
        menu->deleteLater();
    });
    m_pump = new QTimer(this);
    connect(m_pump, &QTimer::timeout, this, [this] { if (m_draft) (void)m_draft->pumpPreviewPluginEvents(); });
    m_pump->start(33);
    const auto& t = th();
    setStyleSheet(QStringLiteral(R"(
QDialog#PluginBatchDialog { background: %1; color: %2; }
#BatchHeading { font-size: 18px; font-weight: 600; }
QLabel[role="secondary"] { color: %3; font-size: 12px; }
#BatchScroll, #BatchScroll > QWidget > QWidget { background: transparent; }
#BatchSlot { background: %4; border-radius: 4px; }
#BatchSlotName { text-align: left; background: transparent; border: none; padding: 2px 4px; }
#BatchSlotName:hover { background: %5; }
#BatchSlotName:focus { border: 1px solid %6; }
#BatchError { color: %2; border: 1px solid %6; border-radius: 5px; padding: 6px; }
#BatchApply { background: %6; color: %7; border: none; border-radius: 6px; padding: 6px 14px; }
#BatchApply:disabled { background: %5; color: %3; }
)" ).arg(t.background.name(), t.textPrimary.name(), t.textSecondary.name(), t.well().name(),
           t.separator().name(), t.accent.name(), t.accent.lightnessF() > 0.55 ? "#101114" : "#ffffff"));
    const auto valid = m_controller.validatePluginBatch(m_targets);
    if (m_targets.size() < 2 || !valid) showError(tr("Select at least two compatible tracks or audio clips."));
    else if (controller.hasCloudProjectBinding()) showError(tr("Shared Plugins are available in local projects."));
    else switchSource(0);
    rebuildRack();
}

PluginBatchDialog::~PluginBatchDialog() {
    m_pump->stop(); stopAudition(); closeEditors();
    if (m_draft) m_draft->setPluginRetiringCallback({});
}

void PluginBatchDialog::done(int result) {
    m_pump->stop(); stopAudition(); closeEditors();
    QDialog::done(result);
}

void PluginBatchDialog::showError(const QString& text) { m_error->setText(text); m_error->show(); }

bool PluginBatchDialog::switchSource(int index) {
    if (index < 0 || index >= int(m_targets.size())) return false;
    if (index == m_sourceIndex) return true;
    const bool resume = m_listening;
    stopAudition();
    daw::EngineController::ChannelSnapshot chain;
    if (m_draft) {
        (void)m_draft->pumpPreviewPluginEvents();
        if (const auto result = m_draft->capturePluginBatchChain(m_targets[m_sourceIndex], m_slots, chain); !result) {
            showError(QString::fromStdString(result.message())); return false;
        }
    }
    std::shared_ptr<daw::EngineController> next;
    if (const auto result = m_controller.createPluginBatchDraft(m_targets[index], next); !result) {
        showError(tr("Could not prepare this source: %1").arg(QString::fromStdString(result.message()))); return false;
    }
    std::vector<std::vector<std::string>> ids;
    if (!chain.inserts.empty()) {
        if (const auto result = next->appendPluginBatch({m_targets[index]}, chain, ids); !result) {
            showError(QString::fromStdString(result.message())); return false;
        }
    }
    closeEditors();
    if (m_draft) m_draft->setPluginRetiringCallback({});
    m_draft = std::move(next);
    m_draft->setPluginRetiringCallback([this](const std::string&, const std::string& slotId) {
        if (auto window = m_editors.value(QString::fromStdString(slotId))) {
            for (auto* editor : window->findChildren<PluginEditorWindow*>()) editor->detachFromPlugin();
            window->close();
        }
    });
    m_sourceIndex = index;
    m_slots = ids.empty() ? std::vector<std::string>{} : std::move(ids.front());
    m_error->hide();
    rebuildRack();
    if (resume) toggleAudition();
    return true;
}

void PluginBatchDialog::refreshAvailability() {
    m_listen->setEnabled(bool(m_draft));
    m_add->setEnabled(m_draft && m_controller.validatePluginBatch(m_targets, m_slots.size() + 1).isOk());
    m_apply->setEnabled(m_draft && !m_slots.empty() && m_controller.validatePluginBatch(m_targets, m_slots.size()).isOk());
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
        auto* row = new QWidget(this); row->setObjectName("BatchSlot"); row->setFixedHeight(28);
        auto* layout = new QHBoxLayout(row); layout->setContentsMargins(4, 0, 4, 0); layout->setSpacing(2);
        auto* power = new QToolButton(row);
        power->setIcon(icons::icon(icons::Glyph::Power, model->bypassed ? th().textSecondary : th().accent, 13));
        power->setFixedSize(24, 24); power->setCheckable(true); power->setChecked(!model->bypassed);
        power->setToolTip(tr("Enable effect")); power->setAccessibleName(power->toolTip());
        connect(power, &QToolButton::toggled, this, [this, id](bool enabled) {
            m_draft->setInsertBypassed(m_targets[m_sourceIndex].trackId, id, !enabled); rebuildRack();
        });
        layout->addWidget(power);
        auto* name = new QPushButton(QString::fromStdString(model->name), row);
        name->setAutoDefault(false); name->setObjectName("BatchSlotName");
        name->setToolTip(tr("Open plugin settings"));
        connect(name, &QPushButton::clicked, this, [this, id] { openEditor(QString::fromStdString(id)); });
        layout->addWidget(name, 1);
        auto* remove = new QToolButton(row); remove->setFixedSize(24, 24);
        remove->setIcon(icons::icon(icons::Glyph::Close, th().textSecondary, 12));
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
}

void PluginBatchDialog::addPlugin(const daw::plugins::PluginDescriptor& plugin, bool open) {
    if (!m_draft || !m_controller.validatePluginBatch(m_targets, m_slots.size() + 1)) return;
    const auto& source = m_targets[m_sourceIndex];
    const auto id = source.clipId.empty() ? m_draft->addInsert(source.trackId, plugin)
        : m_draft->addClipFxInsert(source.trackId, source.clipId, plugin);
    if (id.empty() || !m_draft->insertInstance(source.trackId, id)) {
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
    if (!m_draft->insertInstance(track, id.toStdString())) return;
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
                const auto* plugin = m_draft->insertInstance(m_targets[m_sourceIndex].trackId, editor->insertId().toStdString());
                if (editor->isEmbedded() && plugin && !plugin->editorCanResize()) window->setFixedSize(size);
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
    if (m_listening) m_controller.stopPluginAudition();
    m_listening = false;
    m_listen->setText(tr("Listen"));
    m_listen->setIcon(icons::icon(icons::Glyph::Play, th().textPrimary, 14));
}

void PluginBatchDialog::toggleAudition() {
    if (m_listening) { stopAudition(); return; }
    if (const auto result = m_controller.startPluginAudition(m_draft); !result) {
        showError(QString::fromStdString(result.message())); return;
    }
    m_listening = true; m_error->hide();
    m_listen->setText(tr("Stop")); m_listen->setIcon(icons::icon(icons::Glyph::Stop, th().textPrimary, 14));
}

void PluginBatchDialog::apply() {
    if (!m_draft || m_slots.empty()) return;
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
    daw::EngineController controller;
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
        cancelled.reject();
    }
    if (controller.undoDepth() != undo || !controller.project().findTrack(lead)->inserts.empty()) return false;
    PluginBatchDialog dialog(controller, targets);
    if (!dialog.m_draft || dialog.m_apply->isEnabled() || !dialog.m_slots.empty()) return false;
    dialog.addPlugin(eq, false);
    if (dialog.m_slots.size() != 1 || !dialog.m_apply->isEnabled()) return false;
    dialog.m_draft->setInsertParameter(lead, dialog.m_slots.front(), "output.gain", -6.0);
    dialog.m_source->setCurrentIndex(1);
    if (dialog.m_sourceIndex != 1 || dialog.m_slots.size() != 1) return false;
    const auto* model = dialog.m_draft->insertModel(doubleTrack, dialog.m_slots.front());
    const auto gain = std::find_if(model->parameters.begin(), model->parameters.end(), [](const auto& parameter) { return parameter.id == "output.gain"; });
    if (gain == model->parameters.end() || std::abs(gain->value + 6.0) > 0.01) return false;
    dialog.show(); QApplication::processEvents();
    if (!screenshotPath.isEmpty() && !dialog.grab().save(screenshotPath)) return false;
    if (dialog.m_apply->geometry().bottom() >= dialog.height() || dialog.m_source->width() < 100) return false;
    dialog.apply();
    if (dialog.result() != QDialog::Accepted || controller.undoDepth() != undo + 1) return false;
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
    std::puts("PASS Shared Plugins: cancel, source settings, track/clip apply, context buttons and incompatible selection");
    return true;
}
