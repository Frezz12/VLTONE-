#include "TransportSettingsPage.hpp"
#include "EngineController.hpp"
#include "UiConstants.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
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

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addStretch(1);
}
