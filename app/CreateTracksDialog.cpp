#include "CreateTracksDialog.hpp"

#include "Controls.hpp"
#include "Icons.hpp"
#include "PluginEditorWindow.hpp"
#include "PluginPickerMenu.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QResizeEvent>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace {
class CreationComboBox final : public QComboBox {
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

QWidget* field(const QString& title, QWidget* control, QWidget* parent) {
    auto* result = new QWidget(parent);
    auto* layout = new QVBoxLayout(result);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* label = new QLabel(title, result);
    label->setProperty("role", "fieldLabel");
    label->setBuddy(control);
    control->setAccessibleName(title);
    control->setMinimumHeight(32);
    layout->addWidget(label);
    layout->addWidget(control);
    return result;
}

void clearLayout(QVBoxLayout* layout) {
    while (auto* item = layout->takeAt(0)) {
        if (auto* widget = item->widget()) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
}
}

CreateTracksDialog::CreateTracksDialog(daw::EngineController& controller, QWidget* parent)
    : QDialog(parent), m_controller(controller) {
    setObjectName(QStringLiteral("CreateTracksDialog"));
    setWindowTitle(tr("Create tracks"));
    setWindowModality(Qt::WindowModal);
    setMinimumSize(520, 510);
    resize(600, 640);

    m_ready = m_draft.initialize(controller.sampleRate(), controller.bufferSizeFrames(), false).isOk();
    if (m_ready) {
        m_draft.pluginManager().copyCatalogFrom(controller.pluginManager());
        m_draftTrack = m_draft.addTrack(daw::TrackKind::Midi, "Track template");
        m_ready = !m_draftTrack.empty();
    }
    m_draft.setPluginRetiringCallback([this](const std::string&, const std::string& slotId) {
        const auto window = m_editors.value(QString::fromStdString(slotId));
        if (!window) return;
        for (auto* editor : window->findChildren<PluginEditorWindow*>()) editor->detachFromPlugin();
        window->close();
    });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 18);
    root->setSpacing(12);
    auto* heading = new QLabel(tr("Create tracks"), this);
    heading->setObjectName(QStringLiteral("CreationHeading"));
    root->addWidget(heading);
    auto* intro = new QLabel(tr("Set up once. Apply to every new track."), this);
    intro->setProperty("role", "secondary");
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("CreationScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_form = new QWidget(scroll);
    auto* form = new QVBoxLayout(m_form);
    form->setContentsMargins(0, 0, 4, 0);
    form->setSpacing(12);
    scroll->setWidget(m_form);
    root->addWidget(scroll, 1);

    m_kind = new CreationComboBox(m_form);
    m_kind->setObjectName(QStringLiteral("TrackType"));
    const auto addType = [this](const QString& label, daw::TrackKind kind, bool sum = false) {
        m_kind->addItem(label, int(kind));
        m_kind->setItemData(m_kind->count() - 1, sum, Qt::UserRole + 1);
    };
    addType(tr("Audio"), daw::TrackKind::Audio);
    addType(tr("MIDI / Instrument"), daw::TrackKind::Midi);
    addType(tr("Pattern"), daw::TrackKind::Pattern);
    addType(tr("Automation"), daw::TrackKind::Automation);
    addType(tr("Bus"), daw::TrackKind::Bus);
    addType(tr("Send"), daw::TrackKind::Aux);
    addType(tr("Folder"), daw::TrackKind::Folder);
    addType(tr("Summing folder"), daw::TrackKind::Folder, true);
    m_count = new QSpinBox(m_form);
    m_count->setObjectName(QStringLiteral("TrackCount"));
    m_count->setRange(1, 64);
    m_count->setValue(1);
    m_count->setKeyboardTracking(false);
    m_count->setFixedWidth(128);
    auto* firstRow = new QHBoxLayout;
    firstRow->setSpacing(12);
    firstRow->addWidget(field(tr("Track type"), m_kind, m_form), 1);
    firstRow->addWidget(field(tr("Quantity"), m_count, m_form));
    form->addLayout(firstRow);

    m_name = new QLineEdit(m_form);
    m_name->setObjectName(QStringLiteral("TrackName"));
    m_name->setMaxLength(120);
    m_name->setClearButtonEnabled(true);
    m_channels = new CreationComboBox(m_form);
    m_channels->setObjectName(QStringLiteral("TrackChannels"));
    m_channels->addItem(icons::icon(icons::Glyph::StereoRings, th().textPrimary), tr("Stereo"), 2);
    m_channels->addItem(icons::icon(icons::Glyph::MonoRing, th().textPrimary), tr("Mono"), 1);
    m_channels->setCurrentIndex(1);
    m_channelField = field(tr("Channels"), m_channels, m_form);
    m_channelField->setFixedWidth(128);
    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(12);
    nameRow->addWidget(field(tr("Name"), m_name, m_form), 1);
    nameRow->addWidget(m_channelField);
    form->addLayout(nameRow);
    m_typeHint = new QLabel(m_form);
    m_typeHint->setProperty("role", "secondary");
    m_typeHint->setWordWrap(true);
    form->addWidget(m_typeHint);

    m_audioSection = new QWidget(m_form);
    auto* routing = new QGridLayout(m_audioSection);
    routing->setContentsMargins(0, 0, 0, 0);
    routing->setHorizontalSpacing(12);
    routing->setVerticalSpacing(12);
    m_input = new CreationComboBox(m_audioSection);
    m_input->setObjectName(QStringLiteral("TrackInput"));
    m_output = new CreationComboBox(m_audioSection);
    m_output->setObjectName(QStringLiteral("TrackOutput"));
    m_output->addItem(tr("Master"), QString());
    for (const auto& track : controller.project().tracks) {
        if (track.kind == daw::TrackKind::Bus || track.kind == daw::TrackKind::Aux ||
            track.kind == daw::TrackKind::Group || daw::isSummingFolder(track))
            m_output->addItem(QString::fromStdString(track.name), QString::fromStdString(track.id));
    }
    routing->addWidget(field(tr("Output"), m_output, m_audioSection), 0, 1);
    m_inputField = field(tr("Audio input"), m_input, m_audioSection);
    routing->addWidget(m_inputField, 0, 0);
    routing->setColumnStretch(0, 1); routing->setColumnStretch(1, 1);
    form->addWidget(m_audioSection);

    m_instrumentSection = new QWidget(m_form);
    m_instrumentLayout = new QVBoxLayout(m_instrumentSection);
    m_instrumentLayout->setContentsMargins(0, 0, 0, 0);
    m_instrumentLayout->setSpacing(8);
    form->addWidget(m_instrumentSection);
    m_pluginSection = new QWidget(m_form);
    auto* plugins = new QVBoxLayout(m_pluginSection);
    plugins->setContentsMargins(0, 0, 0, 0);
    plugins->setSpacing(8);
    auto* rackHeader = new QHBoxLayout;
    auto* rackTitle = new QLabel(tr("Inserts"), m_pluginSection);
    rackTitle->setProperty("role", "sectionTitle");
    rackHeader->addWidget(rackTitle);
    rackHeader->addStretch();
    auto* addSlot = m_addSlot = new QPushButton(tr("Add slot"), m_pluginSection);
    addSlot->setObjectName(QStringLiteral("AddInsertSlot"));
    addSlot->setAutoDefault(false);
    addSlot->setFlat(true);
    addSlot->setIcon(icons::icon(icons::Glyph::Plus, th().textSecondary, 14));
    rackHeader->addWidget(addSlot);
    connect(addSlot, &QPushButton::clicked, this, [this] {
        if (m_slots.size() >= 32) return;
        m_slots.append(QString());
        rebuildRack();
    });
    plugins->addLayout(rackHeader);
    m_rack = new QWidget(m_pluginSection);
    m_rack->setObjectName(QStringLiteral("CreationRack"));
    m_rackLayout = new QVBoxLayout(m_rack);
    m_rackLayout->setContentsMargins(1, 1, 1, 1);
    m_rackLayout->setSpacing(0);
    plugins->addWidget(m_rack);
    auto* rackHint = new QLabel(tr("Open a plugin to edit it. Its settings will be copied to every track."), m_pluginSection);
    rackHint->setWordWrap(true);
    rackHint->setProperty("role", "secondary");
    plugins->addWidget(rackHint);
    form->addWidget(m_pluginSection);
    form->addStretch();

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("CreationError"));
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    root->addWidget(m_error);
    root->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("CreationSummary"));
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);
    auto* buttons = new QDialogButtonBox(this);
    m_cancel = buttons->addButton(QDialogButtonBox::Cancel);
    m_create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    m_create->setObjectName(QStringLiteral("ConfirmCreateTracks"));
    m_create->setDefault(true);
    m_create->setMinimumHeight(34);
    m_cancel->setMinimumHeight(34);
    root->addWidget(buttons);
    connect(m_cancel, &QPushButton::clicked, this, &CreateTracksDialog::reject);
    connect(m_create, &QPushButton::clicked, this, &CreateTracksDialog::create);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &CreateTracksDialog::syncType);
    connect(m_count, &QSpinBox::valueChanged, this, &CreateTracksDialog::updateSummary);
    connect(m_name, &QLineEdit::textChanged, this, &CreateTracksDialog::updateSummary);
    connect(m_channels, &QComboBox::currentIndexChanged, this, [this] {
        rebuildInputs();
        if (m_ready) m_draft.setTrackMono(m_draftTrack, m_channels->currentData().toInt() == 1);
        updateSummary();
    });
    m_pump = new QTimer(this);
    m_pump->setInterval(33);
    connect(m_pump, &QTimer::timeout, this, [this] {
        if (isVisible() && !m_creating && m_ready) (void)m_draft.pumpPreviewPluginEvents();
    });
    m_pump->start();
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &CreateTracksDialog::applyTheme);
    rebuildInputs();
    rebuildRack();
    syncType();
    applyTheme();
    if (!m_ready) showError(tr("The plugin preview could not be initialized."));
    m_create->setEnabled(m_ready);
    m_name->setFocus();
    if (screen()) resize(size().boundedTo(screen()->availableGeometry().size() - QSize(48, 80)));
}

CreateTracksDialog::~CreateTracksDialog() {
    m_pump->stop();
    closeEditors();
    m_draft.setPluginRetiringCallback({});
    m_draft.shutdown();
}

daw::TrackKind CreateTracksDialog::kind() const { return daw::TrackKind(m_kind->currentData().toInt()); }
bool CreateTracksDialog::summing() const { return m_kind->currentData(Qt::UserRole + 1).toBool(); }

void CreateTracksDialog::syncType() {
    daw::TrackModel model;
    model.kind = kind(); model.summing = summing();
    const bool audio = daw::carriesAudio(model);
    m_audioSection->setVisible(audio);
    m_channelField->setVisible(audio);
    m_inputField->setVisible(kind() == daw::TrackKind::Audio);
    m_pluginSection->setVisible(audio);
    m_instrumentSection->setVisible(kind() == daw::TrackKind::Midi);
    const bool shared = m_controller.hasCloudProjectBinding();
    m_pluginSection->setEnabled(!shared);
    m_instrumentSection->setEnabled(!shared);
    m_name->setPlaceholderText(tr("Automatic naming"));
    m_typeHint->setText(!audio ? tr("An organizational track, without an audio channel or inserts.")
        : shared ? tr("In a shared project, add plugins after creating the tracks.") : QString());
    m_typeHint->setVisible(!m_typeHint->text().isEmpty());
    updateSummary();
}

void CreateTracksDialog::rebuildInputs() {
    const int previous = m_input->currentIndex() < 0 ? 0 : m_input->currentData().toInt();
    m_input->clear();
    m_input->addItem(tr("No input"), -1);
    const auto device = m_controller.currentInputDeviceInfo();
    const int channels = int(device.inputChannels);
    const int width = m_channels->currentData().toInt();
    for (int i = 0; i + width <= channels; i += width) {
        const QString name = width == 1 ? tr("Input %1").arg(i + 1)
                                       : tr("Input %1–%2").arg(i + 1).arg(i + 2);
        m_input->addItem(QString::fromStdString(device.name) + QStringLiteral(" · ") + name, i);
    }
    m_input->setCurrentIndex(std::max(0, m_input->findData(previous)));
}

QWidget* CreateTracksDialog::slotRow(int index, const QString& slotId, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("CreationSlot"));
    row->setMinimumHeight(44);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 4, 6, 4);
    layout->setSpacing(8);
    auto* number = new QLabel(index < 0 ? QStringLiteral("♪")
        : QStringLiteral("%1").arg(index + 1, 2, 10, QLatin1Char('0')), row);
    number->setProperty("role", "secondary"); number->setFixedWidth(24);
    layout->addWidget(number);
    const auto* model = slotId.isEmpty() ? nullptr
        : m_draft.insertModel(m_draftTrack, slotId.toStdString());
    auto* name = new QToolButton(row);
    name->setObjectName(QStringLiteral("CreationSlotName"));
    name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    name->setMinimumHeight(32);
    name->setCursor(Qt::PointingHandCursor);
    name->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    if (model) {
        name->setText(QString::fromStdString(model->name));
        name->setIcon(icons::icon(icons::Glyph::Plugin, th().textSecondary, 16));
        name->setToolTip(tr("Open %1").arg(name->text()));
        connect(name, &QToolButton::clicked, this, [this, slotId] { openEditor(slotId); });
    } else {
        name->setText(index < 0 ? tr("Choose instrument…") : tr("Add plugin…"));
        name->setIcon(icons::icon(icons::Glyph::Plus, th().textSecondary, 14));
        connect(name, &QToolButton::clicked, this, [this, index, name] { pickPlugin(index, name); });
    }
    name->setAccessibleName(index < 0 ? tr("Instrument: %1").arg(name->text())
        : tr("Insert %1: %2").arg(index + 1).arg(name->text()));
    layout->addWidget(name, 1);
    if (model) {
        auto* power = new ui::IconButton(icons::Glyph::Power, tr("Enable plugin"), row);
        power->setCheckable(true); power->setChecked(!model->bypassed);
        power->setAccessibleName(tr("Enable %1").arg(name->text()));
        connect(power, &QAbstractButton::toggled, this, [this, slotId](bool on) {
            m_draft.setInsertBypassed(m_draftTrack, slotId.toStdString(), !on);
        });
        layout->addWidget(power);
    }
    auto* more = new QToolButton(row);
    more->setText(QStringLiteral("···"));
    more->setFixedSize(28, 28);
    more->setAccessibleName(tr("Slot options"));
    more->setToolTip(tr("Slot options"));
    more->setVisible(model || index >= 3);
    connect(more, &QToolButton::clicked, this, [this, index, more, slotId] {
        QMenu menu(this);
        if (!slotId.isEmpty()) {
            connect(menu.addAction(tr("Replace plugin…")), &QAction::triggered, this,
                [this, index, anchor = QPointer<QWidget>(more)] {
                    QTimer::singleShot(0, this, [this, index, anchor] {
                        if (anchor) pickPlugin(index, anchor);
                    });
                });
            menu.addSeparator();
        }
        if (index >= 0) {
            auto* up = menu.addAction(tr("Move up")); up->setEnabled(index > 0);
            connect(up, &QAction::triggered, this, [this, index] { moveSlot(index, -1); });
            auto* down = menu.addAction(tr("Move down")); down->setEnabled(index + 1 < m_slots.size());
            connect(down, &QAction::triggered, this, [this, index] { moveSlot(index, 1); });
            menu.addSeparator();
        }
        connect(menu.addAction(tr("Remove")), &QAction::triggered, this, [this, index] { removePlugin(index); });
        menu.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    layout->addWidget(more);
    return row;
}

void CreateTracksDialog::rebuildRack() {
    m_addSlot->setEnabled(m_slots.size() < 32);
    clearLayout(m_rackLayout);
    for (int i = 0; i < m_slots.size(); ++i) m_rackLayout->addWidget(slotRow(i, m_slots[i], m_rack));
    clearLayout(m_instrumentLayout);
    auto* label = new QLabel(tr("Instrument"), m_instrumentSection);
    label->setProperty("role", "sectionTitle");
    m_instrumentLayout->addWidget(label);
    const auto* track = m_draft.project().findTrack(m_draftTrack);
    m_instrumentLayout->addWidget(slotRow(-1, track ? QString::fromStdString(track->instrument.id) : QString(), m_instrumentSection));
    if (m_summary) updateSummary();
}

void CreateTracksDialog::pickPlugin(int index, QWidget* anchor) {
    if (!m_ready || m_creating) return;
    const auto* track = m_draft.project().findTrack(m_draftTrack);
    const QString slotId = index < 0
        ? (track ? QString::fromStdString(track->instrument.id) : QString())
        : m_slots.value(index);
    auto* menu = ui::buildPluginMenu(this, &m_draft, index < 0,
        [this, index](const daw::plugins::PluginDescriptor& descriptor) {
            QTimer::singleShot(0, this, [this, index, descriptor] { addPlugin(index, descriptor); });
        }, {QString::fromStdString(m_draftTrack), slotId, [this, index, slotId] {
            if (index >= 0 && index < m_slots.size() &&
                !m_draft.insertModel(m_draftTrack, slotId.toStdString()))
                m_slots[index].clear();
            rebuildRack();
        }});
    menu->exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
    delete menu;
}

void CreateTracksDialog::addPlugin(int index, const daw::plugins::PluginDescriptor& descriptor) {
    if (!m_ready || m_creating || index >= m_slots.size()) return;
    bool ok = false;
    if (index < 0) {
        ok = m_draft.setTrackInstrumentPlugin(m_draftTrack, descriptor);
    } else if (!m_slots[index].isEmpty()) {
        ok = m_draft.replaceInsert(m_draftTrack, m_slots[index].toStdString(), descriptor);
    } else {
        std::size_t position = 0;
        for (int i = 0; i < index; ++i) position += !m_slots[i].isEmpty();
        m_slots[index] = QString::fromStdString(m_draft.addInsert(m_draftTrack, descriptor, position));
        ok = !m_slots[index].isEmpty();
    }
    if (!ok) showError(tr("Could not load %1. Choose another plugin or rescan plugins.").arg(QString::fromStdString(descriptor.name)));
    else {
        m_error->hide();
        ui::rememberRecentPlugin(descriptor);
    }
    rebuildRack();
}

void CreateTracksDialog::removePlugin(int index) {
    if (index < 0) m_draft.setTrackInstrumentPlugin(m_draftTrack, {});
    else if (index < m_slots.size()) {
        if (!m_slots[index].isEmpty()) m_draft.removeInsert(m_draftTrack, m_slots[index].toStdString());
        m_slots.removeAt(index);
        while (m_slots.size() < 3) m_slots.append(QString());
    }
    rebuildRack();
}

void CreateTracksDialog::moveSlot(int index, int direction) {
    const int target = index + direction;
    if (index < 0 || index >= m_slots.size() || target < 0 || target >= m_slots.size()) return;
    m_slots.swapItemsAt(index, target);
    std::size_t position = 0;
    for (const auto& id : m_slots)
        if (!id.isEmpty()) m_draft.moveInsert(m_draftTrack, id.toStdString(), position++);
    rebuildRack();
}

void CreateTracksDialog::openEditor(const QString& slotId) {
    if (auto existing = m_editors.value(slotId)) { existing->show(); existing->raise(); existing->activateWindow(); return; }
    if (!m_draft.insertInstance(m_draftTrack, slotId.toStdString())) {
        showError(tr("This plugin is not available.")); return;
    }
    auto* window = new QDialog(this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QString::fromStdString(m_draft.insertModel(m_draftTrack, slotId.toStdString())->name));
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* editor = new PluginEditorWindow(&m_draft, QString::fromStdString(m_draftTrack), slotId, window);
    layout->addWidget(editor);
    editor->installEventFilter(this);
    window->resize(720, 480);
    m_editors.insert(slotId, window);
    connect(window, &QDialog::finished, this, [this, slotId, editor] {
        editor->detachFromPlugin(); m_editors.remove(slotId);
        rebuildRack();
    });
    connect(editor, &PluginEditorWindow::closing, window, &QDialog::reject);
    connect(editor, &PluginEditorWindow::nestedPluginEditorRequested, this,
        [this](const QString&, const QString& id) { openEditor(id); });
    editor->prepareNativeHostHierarchy();
    window->show();
    editor->initializeEditor();
}

bool CreateTracksDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize) {
        if (auto* editor = qobject_cast<PluginEditorWindow*>(watched)) {
            // Native plugins request their own size on the embedded editor.
            // Keep its outer dialog in sync so a layout cannot crop that view.
            if (auto* window = qobject_cast<QDialog*>(editor->parentWidget())) {
                const auto requested = static_cast<QResizeEvent*>(event)->size();
                const auto* plugin = m_draft.insertInstance(m_draftTrack, editor->insertId().toStdString());
                if (editor->isEmbedded() && plugin && !plugin->editorCanResize())
                    window->setFixedSize(requested);
                else window->resize(requested);
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CreateTracksDialog::closeEditors() {
    const auto windows = m_editors.values();
    m_editors.clear();
    for (const auto& window : windows) {
        if (!window) continue;
        window->disconnect(this);
        for (auto* editor : window->findChildren<PluginEditorWindow*>()) editor->detachFromPlugin();
        delete window;
    }
}

void CreateTracksDialog::updateSummary() {
    if (!m_summary) return;
    const int count = m_count->value();
    const int plugins = m_pluginSection->isHidden() || !m_pluginSection->isEnabled() ? 0
        : int(std::count_if(m_slots.begin(), m_slots.end(), [](const QString& id) { return !id.isEmpty(); }));
    QString text = tr("Tracks: %1").arg(count);
    if (!m_audioSection->isHidden()) text += QStringLiteral(" · ") + m_channels->currentText();
    if (plugins) text += QStringLiteral(" · ") + tr("Inserts per track: %1").arg(plugins);
    m_summary->setText(text);
    m_create->setText(tr("Create tracks (%1)").arg(count));
    m_create->setAccessibleName(m_create->text());
}

void CreateTracksDialog::showError(const QString& message) {
    m_error->setText(message); m_error->setVisible(true);
}

bool CreateTracksDialog::capture(daw::EngineController::TrackCreationRequest& request) {
    (void)m_draft.pumpPreviewPluginEvents();
    request.kind = kind(); request.summing = summing();
    request.count = std::uint32_t(m_count->value());
    request.name = m_name->text().trimmed().toStdString();
    request.mono = m_channels->currentData().toInt() == 1;
    request.inputChannelCount = std::uint32_t(m_channels->currentData().toInt());
    request.inputEnabled = m_input->currentData().toInt() >= 0;
    request.inputChannel = std::uint32_t(std::max(0, m_input->currentData().toInt()));
    daw::TrackModel model; model.kind = request.kind; model.summing = request.summing;
    if (!daw::carriesAudio(model)) return true;
    request.outputBusId = m_output->currentData().toString().toStdString();
    if (m_controller.hasCloudProjectBinding()) return true;
    auto chain = m_draft.copyChannelStrip(m_draftTrack, false);
    request.inserts = std::move(chain.inserts);
    const auto* track = m_draft.project().findTrack(m_draftTrack);
    if (kind() == daw::TrackKind::Midi && track && track->instrument.isLoaded()) {
        request.instrument.emplace();
        request.instrument->model = track->instrument;
    }
    const auto save = [this](daw::EngineController::ChainSlotSnapshot& slot) {
        const auto original = slot.model.editorChannel;
        m_draft.setInsertEditorChannel(m_draftTrack, slot.model.id, daw::PluginEditorChannel::Left);
        auto* plugin = m_draft.insertInstance(m_draftTrack, slot.model.id);
        bool ok = plugin && plugin->saveState(slot.state);
        // AU native controls do not necessarily emit host parameter events.
        // Capture their current values rather than replay stale mirrors over
        // the freshly saved native state when constructing the new instances.
        const auto refreshAu = [&](auto& parameters) {
            if (!plugin || slot.model.format != daw::PluginFormat::AudioUnit) return;
            parameters.clear();
            const auto& infos = plugin->parameters();
            for (std::size_t i = 0; i < infos.size(); ++i) {
                const double value = plugin->parameterValue(std::uint32_t(i));
                if (std::isfinite(value)) parameters.push_back({infos[i].id, value});
            }
        };
        refreshAu(slot.model.parameters);
        if (ok && slot.model.channelMode == daw::PluginChannelMode::DualMono) {
            m_draft.setInsertEditorChannel(m_draftTrack, slot.model.id, daw::PluginEditorChannel::Right);
            plugin = m_draft.insertInstance(m_draftTrack, slot.model.id);
            ok = plugin && plugin->saveState(slot.rightState);
            refreshAu(slot.model.rightParameters);
        }
        m_draft.setInsertEditorChannel(m_draftTrack, slot.model.id, original);
        if (!ok) showError(tr("Could not save the settings for %1. The tracks have not been created.").arg(QString::fromStdString(slot.model.name)));
        return ok;
    };
    for (auto& slot : request.inserts) if (!save(slot)) return false;
    return !request.instrument || save(*request.instrument);
}

void CreateTracksDialog::create() {
    if (m_creating || !m_ready) return;
    closeEditors();
    daw::EngineController::TrackCreationRequest request;
    if (!capture(request)) return;
    m_creating = true;
    m_form->setEnabled(false); m_create->setEnabled(false); m_cancel->setEnabled(false);
    m_summary->setText(tr("Creating tracks…"));
    m_error->hide();
    QTimer::singleShot(0, this, [this, request = std::move(request)] {
        const auto result = m_controller.createTracks(request, m_createdIds);
        m_creating = false;
        if (result) { accept(); return; }
        m_form->setEnabled(true); m_create->setEnabled(true); m_cancel->setEnabled(true);
        updateSummary();
        showError(tr("Tracks could not be created: %1").arg(QString::fromStdString(result.message())));
    });
}

void CreateTracksDialog::reject() {
    if (m_creating) return;
    closeEditors();
    QDialog::reject();
}

void CreateTracksDialog::applyTheme() {
    const auto& t = th();
    setStyleSheet(QStringLiteral(R"(
QDialog#CreateTracksDialog { background: %1; color: %2; }
#CreationScroll, #CreationScroll > QWidget > QWidget { background: transparent; }
#CreationHeading { font-size: 22px; font-weight: 600; }
QLabel[role="secondary"] { color: %3; font-size: 12px; }
QLabel[role="fieldLabel"], QLabel[role="sectionTitle"] { color: %2; font-weight: 600; }
#CreationRack { background: %4; border: 1px solid %5; border-radius: 8px; }
#CreationSlot { border-bottom: 1px solid %5; }
#CreationSlotName { text-align: left; background: transparent; border: none; padding: 4px; }
#CreationSlotName:hover { background: %6; border-radius: 5px; }
#CreationSlotName:focus { border: 1px solid %7; border-radius: 5px; }
#CreationError { color: %2; background: %6; border: 1px solid %7; border-radius: 6px; padding: 8px; }
#ConfirmCreateTracks { background: %7; color: %8; border: none; border-radius: 7px; padding: 7px 18px; font-weight: 600; }
#ConfirmCreateTracks:hover { background: %9; }
#ConfirmCreateTracks:disabled { background: %5; color: %3; }
)").arg(t.background.name(), t.textPrimary.name(), t.textSecondary.name(),
        t.well().name(), t.separator().name(), mixColors(t.well(), t.textPrimary, 0.07).name(),
        t.accent.name(), t.accent.lightnessF() > 0.55 ? QStringLiteral("#101114") : QStringLiteral("#ffffff"),
        t.accentHighlight.name()));
}

bool CreateTracksDialog::checkForTest(daw::EngineController& controller, const QString& screenshotPath) {
    const auto before = controller.project().tracks.size();
    const auto depth = controller.undoDepth();
    const auto gravity = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.gravity");
    if (!gravity) return false;
    {
        CreateTracksDialog cancelled(controller);
        cancelled.addPlugin(0, *gravity);
        cancelled.reject();
    }
    if (controller.project().tracks.size() != before || controller.undoDepth() != depth) return false;
    CreateTracksDialog dialog(controller);
    dialog.m_count->setValue(3);
    dialog.m_name->setText(QStringLiteral("Vocal"));
    dialog.m_channels->setCurrentIndex(dialog.m_channels->findData(1));
    dialog.addPlugin(0, *gravity);
    if (dialog.m_slots[0].isEmpty()) return false;
    const auto slot = dialog.m_slots[0];
    dialog.m_draft.setInsertParameter(dialog.m_draftTrack, slot.toStdString(), "pitch", 4.5);
    dialog.moveSlot(0, 1);
    if (dialog.m_slots[1] != slot) return false;
    dialog.moveSlot(1, -1);
    dialog.show();
    QApplication::processEvents();
    // Exercise the same editor lifetime used by native plugin views.
    dialog.openEditor(slot);
    QApplication::processEvents();
    if (!dialog.m_editors.value(slot) || !dialog.m_editors.value(slot)->isVisible()) return false;
    dialog.closeEditors();
    QApplication::processEvents();
    // Optional manual integration check against an installed AU, without
    // opening the user's project or an audio device.
    const auto nativePath = qgetenv("DAW_TRACK_CREATION_TEST_AU");
    std::vector<double> nativeValues;
    if (!nativePath.isEmpty()) {
        auto* factory = daw::plugins::factoryFor(daw::plugins::Format::AudioUnit);
        const auto found = factory ? factory->inspect(nativePath.toStdString())
                                   : std::vector<daw::plugins::PluginDescriptor>{};
        if (found.empty()) return false;
        dialog.addPlugin(1, found.front());
        const auto nativeSlot = dialog.m_slots[1];
        if (nativeSlot.isEmpty()) return false;
        dialog.openEditor(nativeSlot);
        const auto nativeWindow = dialog.m_editors.value(nativeSlot);
        auto* editor = nativeWindow ? nativeWindow->findChild<PluginEditorWindow*>() : nullptr;
        if (!editor) return false;
        QElapsedTimer opening; opening.start();
        while (!editor->isEditorInitialized() && opening.elapsed() < 5000) QApplication::processEvents();
        if (!editor->isEmbedded()) return false;
        // Let foreign Cocoa/Win32 views paint; QWidget::grab cannot capture
        // their compositor surfaces. A bounded pause allows native UI review.
        QEventLoop paint;
        QTimer::singleShot(std::clamp(qEnvironmentVariableIntValue("DAW_TRACK_CREATION_TEST_REVIEW_MS"), 100, 60000),
                          &paint, &QEventLoop::quit);
        paint.exec();
        auto* plugin = dialog.m_draft.insertInstance(dialog.m_draftTrack, nativeSlot.toStdString());
        const auto parameters = plugin->parameters();
        const auto parameter = std::find_if(parameters.begin(), parameters.end(), [](const auto& info) {
            return info.isAutomatable && !info.isStepped && !info.isBypass &&
                std::isfinite(info.minValue) && std::isfinite(info.maxValue) && info.maxValue > info.minValue;
        });
        if (parameter == parameters.end()) return false;
        const double range = parameter->maxValue - parameter->minValue;
        const double hostValue = parameter->minValue + range * 0.25;
        dialog.m_draft.setInsertParameter(dialog.m_draftTrack, nativeSlot.toStdString(), parameter->id, hostValue);
        (void)dialog.m_draft.pumpPreviewPluginEvents();
        if (std::abs(plugin->parameterValue(parameter->index) - hostValue) > std::max(1.0, range) * 1e-5) return false;
        // Emulate a native GUI changing the AU without a host notification:
        // its value must win over the previously stored host-control mirror.
        daw::plugins::PluginEvent edit;
        edit.paramIndex = parameter->index;
        edit.value = parameter->minValue + range * 0.7;
        std::array<float, 8> input{}, left{}, right{};
        const float* inputs[]{input.data(), input.data()};
        float* outputs[]{left.data(), right.data()};
        daw::plugins::PluginProcessContext block;
        block.inputs = inputs; block.inputChannels = 2;
        block.outputs = outputs; block.outputChannels = 2;
        block.frames = 8; block.inputEvents = {&edit, 1};
        plugin->process(block);
        if (std::abs(plugin->parameterValue(parameter->index) - edit.value) > std::max(1.0, range) * 1e-5) return false;
        for (const auto& parameter : plugin->parameters())
            nativeValues.push_back(plugin->parameterValue(parameter.index));
        if (!screenshotPath.isEmpty() && !nativeWindow->grab().save(screenshotPath + QStringLiteral(".editor.png"))) return false;
        dialog.closeEditors();
        QApplication::processEvents();
    }
    if (qEnvironmentVariableIsSet("DAW_TRACK_CREATION_TEST_NARROW")) {
        dialog.resize(520, 510);
        QApplication::processEvents();
    }
    if (!screenshotPath.isEmpty() && !dialog.grab().save(screenshotPath)) return false;
    dialog.create();
    QElapsedTimer timeout; timeout.start();
    while (dialog.m_creating && timeout.elapsed() < 10000) QApplication::processEvents();
    if (dialog.result() != QDialog::Accepted || dialog.m_createdIds.size() != 3 ||
        controller.undoDepth() != depth + 1) return false;
    for (const auto& id : dialog.m_createdIds) {
        const auto* track = controller.project().findTrack(id);
        if (!track || !track->mono || track->inserts.size() != (nativePath.isEmpty() ? 1 : 2)) return false;
        const auto* plugin = controller.insertInstance(id, track->inserts[0].id);
        if (!plugin || std::abs(plugin->parameterValue(plugin->parameterIndexForId("pitch")) - 4.5) > 1e-6) return false;
        if (!nativePath.isEmpty()) {
            const auto* native = controller.insertInstance(id, track->inserts[1].id);
            if (!native || native->parameters().size() != nativeValues.size()) return false;
            for (std::size_t i = 0; i < nativeValues.size(); ++i)
                if (std::isfinite(nativeValues[i]) && std::abs(native->parameterValue(std::uint32_t(i)) - nativeValues[i]) > 1e-5) return false;
        }
    }
    controller.undo();
    if (controller.project().tracks.size() != before) return false;
    controller.redo();
    if (controller.project().tracks.size() != before + 3) return false;
    std::fprintf(stdout, "PASS track creation dialog: cancel, editor, ordering, settings, batch undo/redo\n");
    return true;
}
