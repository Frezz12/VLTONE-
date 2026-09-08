#include "QuickImportSettingsPage.hpp"

#include "ProjectTemplates.hpp"
#include "QuickImportPrefs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

QuickImportSettingsPage::QuickImportSettingsPage(QWidget* parent) : QWidget(parent) {
    auto* intro = new QLabel(tr("Drop an audio file on the VLTONE icon to create a new project from the chosen template and place the file on this track."), this);
    intro->setWordWrap(true);

    m_template = new QComboBox(this);
    m_template->setAccessibleName(tr("Quick Import project template"));
    m_track = new QComboBox(this);
    m_track->setAccessibleName(tr("Quick Import audio track"));
    m_detectTempo = new QCheckBox(tr("Detect BPM"), this);
    m_detectKey = new QCheckBox(tr("Detect key"), this);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(tr("Project template:"), m_template);
    form->addRow(tr("Audio track:"), m_track);

    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    m_error->setProperty("role", QStringLiteral("warning"));
    m_error->setAccessibleName(tr("Quick Import configuration issue"));
    m_error->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);
    layout->addWidget(intro);
    layout->addSpacing(4);
    layout->addLayout(form);
    layout->addWidget(m_detectTempo);
    layout->addWidget(m_detectKey);
    layout->addWidget(m_error);
    layout->addStretch();

    connect(m_template, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
                refreshTracks();
                persist();
            });
    connect(m_track, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &QuickImportSettingsPage::persist);
    connect(m_detectTempo, &QCheckBox::toggled, this,
            &QuickImportSettingsPage::persist);
    connect(m_detectKey, &QCheckBox::toggled, this,
            &QuickImportSettingsPage::persist);
    refresh();
}

void QuickImportSettingsPage::refresh() {
    const ui::quickimport::Preferences preferences = ui::quickimport::load();
    const QSignalBlocker templateBlock(m_template);
    const QSignalBlocker tempoBlock(m_detectTempo);
    const QSignalBlocker keyBlock(m_detectKey);
    m_template->clear();
    m_template->addItem(tr("Choose a template…"), QString());
    for (const QString& path : ui::projecttemplates::files())
        m_template->addItem(ui::projecttemplates::displayName(path), path);
    int templateIndex = m_template->findData(preferences.templatePath);
    if (templateIndex < 0 && !preferences.templatePath.isEmpty()) {
        m_template->addItem(tr("Missing: %1").arg(QFileInfo(preferences.templatePath).fileName()),
                            preferences.templatePath);
        templateIndex = m_template->count() - 1;
    }
    m_template->setCurrentIndex(std::max(0, templateIndex));
    m_detectTempo->setChecked(preferences.detectTempo);
    m_detectKey->setChecked(preferences.detectKey);
    refreshTracks(preferences.trackId);
}

void QuickImportSettingsPage::refreshTracks(const QString& preferredTrackId) {
    const QSignalBlocker block(m_track);
    const QString templatePath = m_template->currentData().toString();
    QString error;
    const auto targets = ui::projecttemplates::audioTargets(templatePath, &error);
    m_track->clear();
    m_track->addItem(tr("Choose an audio track…"), QString());
    for (const auto& target : targets)
        m_track->addItem(target.displayPath, target.trackId);
    const int index = m_track->findData(preferredTrackId);
    m_track->setCurrentIndex(std::max(0, index));
    m_track->setEnabled(!targets.isEmpty());
    if (!templatePath.isEmpty() && !error.isEmpty()) showConfigurationError(
        tr("This template could not be read: %1").arg(error));
}

void QuickImportSettingsPage::persist() {
    ui::quickimport::Preferences preferences;
    preferences.templatePath = m_template->currentData().toString();
    preferences.trackId = m_track->currentData().toString();
    preferences.detectTempo = m_detectTempo->isChecked();
    preferences.detectKey = m_detectKey->isChecked();
    ui::quickimport::save(preferences);
    QString error;
    if (ui::quickimport::validate(preferences, &error)) {
        m_error->hide();
        m_error->clear();
    } else if (!preferences.templatePath.isEmpty() || !preferences.trackId.isEmpty()) {
        showConfigurationError(error);
    }
}

void QuickImportSettingsPage::showConfigurationError(const QString& message) {
    m_error->setText(message);
    m_error->setAccessibleDescription(message);
    m_error->show();
    m_template->setFocus(Qt::OtherFocusReason);
}
