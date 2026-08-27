#include "AiSettingsPage.hpp"

#include "AccountService.hpp"
#include "PromptService.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QProcessEnvironment>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

using ui::aiprefs::Provider;

AiSettingsPage::AiSettingsPage(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setSpacing(12);

    auto* managedGroup = new QGroupBox(tr("Models provided by VLT"), this);
    auto* managedColumn = new QVBoxLayout(managedGroup);
    auto* managedRow = new QHBoxLayout;
    auto* managedNote = new QLabel(
        tr("These connections are configured by the administrator. In the "
           "chat they appear only by their display name."), managedGroup);
    managedNote->setWordWrap(true);
    managedRow->addWidget(managedNote, 1);
    auto* refreshModels = new QPushButton(tr("Refresh"), managedGroup);
    connect(refreshModels, &QPushButton::clicked, this, [] {
        if (auto* account = account::Service::instance())
            account->refreshAiModels();
    });
    managedRow->addWidget(refreshModels);
    managedColumn->addLayout(managedRow);
    m_managedModels = new QListWidget(managedGroup);
    m_managedModels->setSelectionMode(QAbstractItemView::NoSelection);
    m_managedModels->setFocusPolicy(Qt::NoFocus);
    m_managedModels->setMaximumHeight(112);
    managedColumn->addWidget(m_managedModels);
    column->addWidget(managedGroup);

    auto* customGroup = new QGroupBox(tr("Your models"), this);
    auto* customColumn = new QVBoxLayout(customGroup);
    auto* customNote = new QLabel(
        tr("Add as many OpenAI-compatible or Anthropic-compatible endpoints "
           "as you need. Their API keys stay in this computer's secure "
           "credential storage."), customGroup);
    customNote->setWordWrap(true);
    customColumn->addWidget(customNote);
    m_customModels = new QListWidget(customGroup);
    m_customModels->setMinimumHeight(88);
    m_customModels->setMaximumHeight(136);
    connect(m_customModels, &QListWidget::currentRowChanged, this,
            &AiSettingsPage::loadCustomEditor);
    customColumn->addWidget(m_customModels);

    auto* customForm = new QFormLayout;
    m_customName = new QLineEdit(customGroup);
    m_customName->setPlaceholderText(tr("For example: My local model"));
    customForm->addRow(tr("Name in chat"), m_customName);

    m_customProvider = new QComboBox(customGroup);
    m_customProvider->addItem(tr("GPT / OpenAI-compatible"),
                              ui::aiprefs::providerId(Provider::OpenAi));
    m_customProvider->addItem(tr("Claude / Anthropic-compatible"),
                              ui::aiprefs::providerId(Provider::Anthropic));
    connect(m_customProvider, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading || !m_customEndpoint->text().isEmpty()) return;
        m_customEndpoint->setText(
            m_customProvider->currentData().toString() == QLatin1String("openai")
                ? QStringLiteral("https://api.openai.com/v1")
                : QStringLiteral("https://api.anthropic.com/v1"));
    });
    customForm->addRow(tr("Connection type"), m_customProvider);

    m_customModel = new QLineEdit(customGroup);
    m_customModel->setPlaceholderText(QStringLiteral("gpt-4.1"));
    customForm->addRow(tr("Provider model ID"), m_customModel);

    m_customEndpoint = new QLineEdit(customGroup);
    m_customEndpoint->setPlaceholderText(
        QStringLiteral("https://api.example.com/v1"));
    customForm->addRow(tr("Base URL or request URL"), m_customEndpoint);

    m_customKey = new QLineEdit(customGroup);
    m_customKey->setEchoMode(QLineEdit::Password);
    m_customKey->setPlaceholderText(tr("Optional for a server without authorization"));
    auto* keyRow = new QHBoxLayout;
    keyRow->setSpacing(8);
    keyRow->addWidget(m_customKey, 1);
    m_revealCustomKey = new QPushButton(tr("Show"), customGroup);
    m_revealCustomKey->setCheckable(true);
    m_revealCustomKey->setMinimumWidth(64);
    connect(m_revealCustomKey, &QPushButton::toggled, this,
            [this](bool shown) {
                m_customKey->setEchoMode(shown ? QLineEdit::Normal
                                               : QLineEdit::Password);
                m_revealCustomKey->setText(shown ? tr("Hide") : tr("Show"));
            });
    keyRow->addWidget(m_revealCustomKey);
    customForm->addRow(tr("API key"), keyRow);
    m_customKeyNote = new QLabel(customGroup);
    m_customKeyNote->setWordWrap(true);
    customForm->addRow(QString(), m_customKeyNote);
    customColumn->addLayout(customForm);

    auto* customButtons = new QHBoxLayout;
    auto* newCustom = new QPushButton(tr("New"), customGroup);
    connect(newCustom, &QPushButton::clicked, this,
            &AiSettingsPage::clearCustomEditor);
    customButtons->addWidget(newCustom);
    customButtons->addStretch(1);
    m_removeCustom = new QPushButton(tr("Delete"), customGroup);
    connect(m_removeCustom, &QPushButton::clicked, this,
            &AiSettingsPage::removeCustomModel);
    customButtons->addWidget(m_removeCustom);
    m_saveCustom = new QPushButton(tr("Save model"), customGroup);
    connect(m_saveCustom, &QPushButton::clicked, this,
            &AiSettingsPage::saveCustomModel);
    customButtons->addWidget(m_saveCustom);
    customColumn->addLayout(customButtons);
    m_customStatus = new QLabel(customGroup);
    m_customStatus->setWordWrap(true);
    customColumn->addWidget(m_customStatus);
    column->addWidget(customGroup);

    auto* agentGroup = new QGroupBox(tr("Assistant behavior"), this);
    auto* form = new QFormLayout(agentGroup);
    column->addWidget(agentGroup);

    m_maxIterations = new QSpinBox(this);
    m_maxIterations->setRange(1, 200);
    m_maxIterations->setValue(ui::aiprefs::maxIterations());
    m_maxIterations->setToolTip(
        tr("How many rounds of tool calls one request may take before it is "
           "cut off. The guard against a request that keeps going."));
    connect(m_maxIterations, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMaxIterations(value);
        emit changed();
    });
    form->addRow(tr("Steps per request"), m_maxIterations);

    m_historyLimit = new QSpinBox(this);
    m_historyLimit->setRange(0, 200);
    m_historyLimit->setSpecialValueText(tr("the whole conversation"));
    m_historyLimit->setValue(ui::aiprefs::historyLimit());
    m_historyLimit->setToolTip(
        tr("How many past requests each new one carries with it. The chat "
           "still shows everything; this is only what is paid for. Lower is "
           "cheaper, higher lets the assistant remember more of what you "
           "asked earlier."));
    connect(m_historyLimit, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setHistoryLimit(value);
        emit changed();
    });
    form->addRow(tr("Requests remembered"), m_historyLimit);

    m_streaming = new QCheckBox(tr("Show the answer as it is written"), this);
    m_streaming->setChecked(ui::aiprefs::streaming());
    m_streaming->setToolTip(
        tr("Off waits for the whole reply before showing anything. Some "
           "OpenAI-compatible servers do not stream."));
    connect(m_streaming, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        ui::aiprefs::setStreaming(on);
        emit changed();
    });
    form->addRow(QString(), m_streaming);

    auto* warning = new QLabel(
        tr("The assistant edits the project directly. Each request lands on "
           "the undo stack as one entry, so Ctrl+Z takes back everything a "
           "request did."),
        this);
    warning->setWordWrap(true);
    column->addWidget(warning);

    // The instructions the assistant works under are served, not compiled in,
    // so which set is in force is a real question a user can ask — and one they
    // need answered when somebody has just edited them.
    auto* promptRow = new QWidget(this);
    auto* promptLayout = new QHBoxLayout(promptRow);
    promptLayout->setContentsMargins(0, 0, 0, 0);
    m_promptStatus = new QLabel(promptRow);
    m_promptStatus->setWordWrap(true);
    promptLayout->addWidget(m_promptStatus, 1);
    m_promptRefresh = new QPushButton(tr("Update"), promptRow);
    m_promptRefresh->setToolTip(
        tr("Fetch the latest instructions from the server."));
    connect(m_promptRefresh, &QPushButton::clicked, this, [this] {
        if (auto* prompts = ui::PromptService::instance()) prompts->refresh();
        refreshPromptStatus();
    });
    promptLayout->addWidget(m_promptRefresh);
    column->addWidget(promptRow);
    if (auto* prompts = ui::PromptService::instance()) {
        connect(prompts, &ui::PromptService::stateChanged, this,
                &AiSettingsPage::refreshPromptStatus);
    }
    refreshPromptStatus();

    // Music generation is built and working (`buildMusicGroup`, `loadMusic`,
    // the panel's second mode) but is not offered yet — the user asked for it
    // to be out of the way until it is ready to show. Nothing about it was
    // deleted; it is one line away from coming back.
    column->addStretch(1);

    if (auto* account = account::Service::instance()) {
        connect(account, &account::Service::aiModelsChanged, this,
                &AiSettingsPage::loadModels);
    }
    loadModels();
    clearCustomEditor();
}

void AiSettingsPage::refreshPromptStatus() {
    if (!m_promptStatus) return;
    auto* prompts = ui::PromptService::instance();
    if (!prompts) {
        m_promptStatus->setText(tr("Instructions: built in."));
        if (m_promptRefresh) m_promptRefresh->setEnabled(false);
        return;
    }
    QString where;
    switch (prompts->source()) {
        case ui::PromptService::Source::Server: where = tr("from the server"); break;
        case ui::PromptService::Source::Cache:  where = tr("last downloaded"); break;
        case ui::PromptService::Source::Builtin: where = tr("built in"); break;
    }
    QString text = tr("Instructions: %1 (%2). Playbooks: %3.")
                       .arg(prompts->version(), where)
                       .arg(prompts->pack().playbooks.size());
    if (!prompts->lastError().isEmpty())
        text += QLatin1Char(' ') +
                tr("Last update failed: %1").arg(prompts->lastError());
    m_promptStatus->setText(text);
    if (m_promptRefresh) m_promptRefresh->setEnabled(!prompts->busy());
}

QWidget* AiSettingsPage::buildMusicGroup() {
    auto* group = new QGroupBox(tr("Music generation"), this);
    auto* column = new QVBoxLayout(group);
    auto* form = new QFormLayout;
    column->addLayout(form);

    m_musicUrl = new QLineEdit(group);
    m_musicUrl->setToolTip(
        tr("The whole endpoint, not a base address. MiniMax's own is "
           "https://api.minimax.io/v1/music_generation; point this at your own "
           "server if the model runs there."));
    connect(m_musicUrl, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicUrl(m_musicUrl->text().trimmed());
        loadMusic();
        emit changed();
    });
    form->addRow(tr("Server URL"), m_musicUrl);

    m_musicModel = new QLineEdit(group);
    m_musicModel->setPlaceholderText(QStringLiteral("music-3.0"));
    connect(m_musicModel, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicModel(m_musicModel->text().trimmed());
        emit changed();
    });
    form->addRow(tr("Model"), m_musicModel);

    m_musicKey = new QLineEdit(group);
    m_musicKey->setEchoMode(QLineEdit::Password);
    // Never read back into the field, for the same reason as the chat key: the
    // page is built whenever this window opens, on any tab.
    connect(m_musicKey, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        const QString typed = m_musicKey->text().trimmed();
        if (typed.isEmpty()) return;
        ui::aiprefs::setMusicApiKey(typed);
        m_musicKey->clear();
        loadMusic();
        emit changed();
    });
    auto* keyRow = new QHBoxLayout();
    keyRow->setSpacing(8);
    keyRow->addWidget(m_musicKey, 1);
    m_forgetMusicKey = new QPushButton(tr("Forget"), group);
    connect(m_forgetMusicKey, &QPushButton::clicked, this, [this] {
        ui::aiprefs::setMusicApiKey(QString());
        m_musicKey->clear();
        loadMusic();
        emit changed();
    });
    keyRow->addWidget(m_forgetMusicKey);
    form->addRow(tr("API key"), keyRow);

    m_musicFormat = new QComboBox(group);
    m_musicFormat->addItem(QStringLiteral("mp3"), QStringLiteral("mp3"));
    m_musicFormat->addItem(QStringLiteral("wav"), QStringLiteral("wav"));
    connect(m_musicFormat, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicFormat(m_musicFormat->currentData().toString());
        emit changed();
    });
    form->addRow(tr("Format"), m_musicFormat);

    m_musicSampleRate = new QSpinBox(group);
    m_musicSampleRate->setRange(8000, 192000);
    m_musicSampleRate->setSingleStep(100);
    m_musicSampleRate->setSuffix(tr(" Hz"));
    connect(m_musicSampleRate, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMusicSampleRate(value);
        emit changed();
    });
    form->addRow(tr("Sample rate"), m_musicSampleRate);

    m_musicBitrate = new QSpinBox(group);
    m_musicBitrate->setRange(32000, 320000);
    m_musicBitrate->setSingleStep(1000);
    m_musicBitrate->setToolTip(tr("Only used when the format is mp3."));
    connect(m_musicBitrate, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMusicBitrate(value);
        emit changed();
    });
    form->addRow(tr("Bitrate"), m_musicBitrate);

    m_musicFolder = new QLineEdit(group);
    m_musicFolder->setToolTip(
        tr("Generated audio is written here and then imported. Keeping it "
           "inside a folder the browser shows means a take can be used again "
           "later."));
    connect(m_musicFolder, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        ui::aiprefs::setMusicFolder(m_musicFolder->text().trimmed());
        loadMusic();
        emit changed();
    });
    auto* folderRow = new QHBoxLayout();
    folderRow->setSpacing(8);
    folderRow->addWidget(m_musicFolder, 1);
    auto* browse = new QPushButton(tr("Browse…"), group);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(
            this, tr("Where to save generated audio"),
            ui::aiprefs::musicFolder());
        if (picked.isEmpty()) return;
        ui::aiprefs::setMusicFolder(picked);
        loadMusic();
        emit changed();
    });
    folderRow->addWidget(browse);
    form->addRow(tr("Save audio to"), folderRow);

    m_musicTimeout = new QSpinBox(group);
    m_musicTimeout->setRange(10, 3600);
    m_musicTimeout->setSuffix(tr(" s"));
    m_musicTimeout->setToolTip(
        tr("How long one generation may take before it is given up on. A "
           "four-minute song is not fast."));
    connect(m_musicTimeout, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_loading) return;
        ui::aiprefs::setMusicTimeoutSeconds(value);
        emit changed();
    });
    form->addRow(tr("Timeout"), m_musicTimeout);

    m_musicNote = new QLabel(group);
    m_musicNote->setWordWrap(true);
    column->addWidget(m_musicNote);

    auto* blurb = new QLabel(
        tr("The music mode sends the project's tempo, key, time signature and "
           "track names with the request, and puts what comes back on a new "
           "audio track at the playhead — one Ctrl+Z takes it away."),
        group);
    blurb->setWordWrap(true);
    column->addWidget(blurb);
    return group;
}

void AiSettingsPage::loadMusic() {
    m_loading = true;
    m_musicUrl->setText(ui::aiprefs::musicUrl());
    m_musicModel->setText(ui::aiprefs::musicModel());
    m_musicFolder->setText(ui::aiprefs::musicFolder());
    m_musicSampleRate->setValue(ui::aiprefs::musicSampleRate());
    m_musicBitrate->setValue(ui::aiprefs::musicBitrate());
    m_musicTimeout->setValue(ui::aiprefs::musicTimeoutSeconds());
    m_musicFormat->setCurrentIndex(
        m_musicFormat->findData(ui::aiprefs::musicFormat()));

    m_musicKey->clear();
    const bool stored = ui::aiprefs::hasMusicApiKey();
    m_musicKey->setPlaceholderText(
        stored ? tr("A key is stored — type to replace it")
               : tr("Optional — a server of your own may need none"));
    m_forgetMusicKey->setEnabled(stored);
    m_loading = false;

    const QString fromEnv = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("MINIMAX_API_KEY"));
    m_musicNote->setText(
        fromEnv.isEmpty()
            ? tr("A key typed here is stored unencrypted with the other "
                 "settings. Set MINIMAX_API_KEY in the environment instead to "
                 "keep it off disk. Leave it empty for a server that needs no "
                 "authorization.")
            : tr("MINIMAX_API_KEY is set in the environment and is used "
                 "instead of the key above."));
}

void AiSettingsPage::loadModels() {
    const QString selected = m_editingCustomId;
    m_loading = true;
    m_managedModels->clear();
    const QList<ui::aiprefs::ModelConnection> managed =
        ui::aiprefs::managedModels();
    if (managed.isEmpty()) {
        auto* item = new QListWidgetItem(tr("No administrator models are available."),
                                         m_managedModels);
        item->setFlags(Qt::NoItemFlags);
    } else {
        for (const ui::aiprefs::ModelConnection& model : managed) {
            auto* item = new QListWidgetItem(model.displayName, m_managedModels);
            item->setToolTip(tr("%1 · %2")
                                 .arg(model.provider == Provider::OpenAi
                                          ? tr("GPT-compatible")
                                          : tr("Claude-compatible"),
                                      model.model));
        }
    }

    m_customModels->clear();
    int selectedRow = -1;
    const QList<ui::aiprefs::ModelConnection> custom =
        ui::aiprefs::customModels();
    for (int i = 0; i < custom.size(); ++i) {
        const auto& model = custom.at(i);
        auto* item = new QListWidgetItem(model.displayName, m_customModels);
        item->setData(Qt::UserRole, model.id);
        item->setToolTip(tr("%1 · %2")
                             .arg(model.provider == Provider::OpenAi
                                      ? tr("GPT-compatible")
                                      : tr("Claude-compatible"),
                                  model.model));
        if (model.id == selected) selectedRow = i;
    }
    m_loading = false;
    if (selectedRow >= 0) m_customModels->setCurrentRow(selectedRow);
}

void AiSettingsPage::loadCustomEditor(int row) {
    if (m_loading || row < 0) return;
    const QString id = m_customModels->item(row)->data(Qt::UserRole).toString();
    ui::aiprefs::ModelConnection model;
    if (!ui::aiprefs::modelById(id, &model) ||
        model.source != ui::aiprefs::ModelSource::Custom)
        return;
    m_loading = true;
    m_editingCustomId = model.id;
    m_customName->setText(model.displayName);
    m_customProvider->setCurrentIndex(
        m_customProvider->findData(ui::aiprefs::providerId(model.provider)));
    m_customModel->setText(model.model);
    m_customEndpoint->setText(model.endpoint);
    m_customKey->clear();
    m_revealCustomKey->setChecked(false);
    m_customKey->setPlaceholderText(
        model.hasApiKey ? tr("A key is stored — leave empty to keep it")
                        : tr("Optional for a server without authorization"));
    m_customKeyNote->setText(
        model.hasApiKey
            ? tr("The stored key is hidden. Type a value only to replace it.")
            : tr("No key is stored for this model."));
    m_removeCustom->setEnabled(true);
    m_customStatus->clear();
    m_loading = false;
}

void AiSettingsPage::clearCustomEditor() {
    m_loading = true;
    m_editingCustomId.clear();
    m_customModels->clearSelection();
    m_customModels->setCurrentRow(-1);
    m_customName->clear();
    m_customProvider->setCurrentIndex(0);
    m_customModel->clear();
    m_customEndpoint->setText(
        QStringLiteral("https://api.openai.com/v1"));
    m_customKey->clear();
    m_revealCustomKey->setChecked(false);
    m_customKey->setPlaceholderText(
        tr("Optional for a server without authorization"));
    m_customKeyNote->setText(
        tr("Keys are saved in the operating-system credential vault, not in "
           "the settings file."));
    m_customStatus->clear();
    m_removeCustom->setEnabled(false);
    m_loading = false;
}

void AiSettingsPage::saveCustomModel() {
    const QString name = m_customName->text().trimmed();
    const QString modelName = m_customModel->text().trimmed();
    const QString endpoint = m_customEndpoint->text().trimmed();
    const QUrl url(endpoint);
    if (name.isEmpty() || modelName.isEmpty() || !url.isValid() ||
        url.host().isEmpty() ||
        (url.scheme() != QLatin1String("http") &&
         url.scheme() != QLatin1String("https"))) {
        m_customStatus->setText(
            tr("Enter a name, provider model ID, and a complete HTTP or HTTPS "
               "request URL."));
        return;
    }
    ui::aiprefs::ModelConnection connection;
    connection.id = m_editingCustomId;
    connection.displayName = name;
    connection.provider = ui::aiprefs::providerFromId(
        m_customProvider->currentData().toString());
    connection.model = modelName;
    connection.endpoint = endpoint;
    connection.source = ui::aiprefs::ModelSource::Custom;
    QString savedId;
    if (!ui::aiprefs::saveCustomModel(connection, m_customKey->text(),
                                      &savedId)) {
        m_customStatus->setText(
            tr("The operating-system credential vault could not save this API "
               "key. The model was not changed."));
        return;
    }
    m_editingCustomId = savedId;
    loadModels();
    m_customStatus->setText(tr("Model saved."));
    emit changed();
}

void AiSettingsPage::removeCustomModel() {
    if (m_editingCustomId.isEmpty()) return;
    if (QMessageBox::question(
            this, tr("Delete model"),
            tr("Delete “%1” and its stored API key?")
                .arg(m_customName->text().trimmed())) != QMessageBox::Yes)
        return;
    if (!ui::aiprefs::removeCustomModel(m_editingCustomId)) {
        m_customStatus->setText(
            tr("The model could not be removed from secure storage."));
        return;
    }
    loadModels();
    clearCustomEditor();
    m_customStatus->setText(tr("Model deleted."));
    emit changed();
}
