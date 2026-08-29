#include "TransportSettingsPage.hpp"
#include "EngineController.hpp"
#include "FileTypes.hpp"
#include "UiConstants.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

TransportSettingsPage::TransportSettingsPage(daw::EngineController* controller,
                                             QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    using Mode = daw::EngineController::PlaybackMode;

    m_modeCombo = new QComboBox;
    m_modeCombo->addItem(tr("Resume — continue from the playhead"),
                         static_cast<int>(Mode::Resume));
    m_modeCombo->addItem(tr("Restart — return to the start of the run"),
                         static_cast<int>(Mode::Restart));
    m_modeCombo->setCurrentIndex(int(m_controller->playbackMode()));

    auto* form = new QFormLayout;
    form->addRow(tr("Play / pause (Space)"), m_modeCombo);

    auto* metronomeRow = new QWidget(this);
    auto* metronomeLayout = new QHBoxLayout(metronomeRow);
    metronomeLayout->setContentsMargins(0, 0, 0, 0);
    metronomeLayout->setSpacing(6);
    m_metronomeFile = new QLineEdit(metronomeRow);
    m_metronomeFile->setObjectName(QStringLiteral("MetronomeSamplePath"));
    m_metronomeFile->setReadOnly(true);
    m_metronomeFile->setClearButtonEnabled(false);
    m_metronomeFile->setPlaceholderText(tr("Built-in muted knock"));
    m_metronomeFile->setText(QString::fromStdString(
        m_controller->metronomeSamplePath()));
    m_metronomeFile->setToolTip(m_metronomeFile->text());
    auto* chooseMetronome = new QPushButton(tr("Choose..."), metronomeRow);
    auto* resetMetronome = new QPushButton(tr("Default"), metronomeRow);
    resetMetronome->setEnabled(!m_metronomeFile->text().isEmpty());
    metronomeLayout->addWidget(m_metronomeFile, 1);
    metronomeLayout->addWidget(chooseMetronome);
    metronomeLayout->addWidget(resetMetronome);
    form->addRow(tr("Metronome sound"), metronomeRow);

    auto* hint = new QLabel(
        tr("Resume: each Space press toggles play and continues from the current "
           "position.\n"
           "Restart: when playback is paused, Space jumps back to the position "
           "where the current run started and plays from there."));
    hint->setWordWrap(true);

    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        using Mode = daw::EngineController::PlaybackMode;
        const Mode mode = i == int(Mode::Restart) ? Mode::Restart : Mode::Resume;
        m_controller->setPlaybackMode(mode);
        QSettings().setValue(ui::kPlaybackModeSetting, int(mode));
    });
    connect(chooseMetronome, &QPushButton::clicked, this,
            [this, resetMetronome] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Choose Metronome Sound"), m_metronomeFile->text(),
            ui::audioNameFilter());
        if (path.isEmpty()) return;
        if (!m_controller->setMetronomeSample(path.toStdString())) {
            QMessageBox::warning(
                this, tr("Metronome sound could not be loaded"),
                tr("The selected audio file could not be decoded."));
            return;
        }
        m_metronomeFile->setText(path);
        m_metronomeFile->setToolTip(path);
        resetMetronome->setEnabled(true);
        QSettings().setValue(ui::kMetronomeSampleSetting, path);
    });
    connect(resetMetronome, &QPushButton::clicked, this,
            [this, resetMetronome] {
        m_controller->setMetronomeSample({});
        m_metronomeFile->clear();
        m_metronomeFile->setToolTip(tr("Built-in muted knock"));
        resetMetronome->setEnabled(false);
        QSettings().remove(ui::kMetronomeSampleSetting);
    });

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addStretch(1);
}
