#include "RecordingSettingsPage.hpp"
#include "EngineController.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// One QSettings key per preference. Spelled out rather than serialised as a
// blob so a future field defaults sensibly instead of invalidating the lot.
constexpr const char* kMode = "recording/mode";
constexpr const char* kLoopTakes = "recording/loopCreatesTakes";
constexpr const char* kTrimTakes = "recording/trimTakesToRegion";
constexpr const char* kAutoExpand = "recording/autoExpandAfterRecord";
constexpr const char* kCrossfade = "recording/compCrossfadeMs";
constexpr const char* kMidiMerge = "recording/midiOverdubMerge";
constexpr const char* kRecordKey = "recording/recordKeyArmsAndStarts";
constexpr const char* kMonitorStop = "recording/monitorStopPolicy";
constexpr const char* kCountIn = "recording/countInBeats";
constexpr const char* kManualMonitor = "recording/manualMonitorDisablesAuto";
constexpr const char* kAutoMonitor = "recording/autoMonitorOnRecord";

} // namespace

void RecordingSettingsPage::restore(daw::EngineController& controller) {
    using Prefs = daw::EngineController::RecordingPrefs;
    using Policy = daw::EngineController::MonitorStopPolicy;
    QSettings s;
    Prefs prefs = controller.recordingPrefs();
    prefs.mode = s.value(kMode, int(prefs.mode)).toInt() == int(daw::RecordMode::Layers)
                     ? daw::RecordMode::Layers
                     : daw::RecordMode::Overwrite;
    prefs.loopCreatesTakes = s.value(kLoopTakes, prefs.loopCreatesTakes).toBool();
    prefs.trimTakesToRegion = s.value(kTrimTakes, prefs.trimTakesToRegion).toBool();
    prefs.autoExpandAfterRecord =
        s.value(kAutoExpand, prefs.autoExpandAfterRecord).toBool();
    prefs.compCrossfadeMs = s.value(kCrossfade, prefs.compCrossfadeMs).toDouble();
    prefs.midiOverdubMerge = s.value(kMidiMerge, prefs.midiOverdubMerge).toBool();
    prefs.recordKeyArmsAndStarts =
        s.value(kRecordKey, prefs.recordKeyArmsAndStarts).toBool();
    prefs.countInBeats =
        std::clamp(s.value(kCountIn, prefs.countInBeats).toInt(), 0, 8);
    const int policy = s.value(kMonitorStop, int(prefs.monitorStopPolicy)).toInt();
    prefs.monitorStopPolicy =
        policy == int(Policy::KeepOn)      ? Policy::KeepOn
        : policy == int(Policy::AutoDisable) ? Policy::AutoDisable
                                             : Policy::ReturnToPrevious;
    prefs.manualMonitorDisablesAuto =
        s.value(kManualMonitor, prefs.manualMonitorDisablesAuto).toBool();
    prefs.autoMonitorOnRecord =
        s.value(kAutoMonitor, prefs.autoMonitorOnRecord).toBool();
    // `inverted` is deliberately not restored: it is a key being held, and no
    // key is held at startup.
    prefs.inverted = false;
    controller.setRecordingPrefs(prefs);
}

void RecordingSettingsPage::persistMode(daw::RecordMode mode) {
    QSettings s;
    s.setValue(kMode, int(mode));
}

void RecordingSettingsPage::persistCountInBeats(int beats) {
    QSettings s;
    s.setValue(kCountIn, std::clamp(beats, 0, 8));
}

void RecordingSettingsPage::persistAutoMonitor(bool on) {
    QSettings s;
    s.setValue(kAutoMonitor, on);
}

RecordingSettingsPage::RecordingSettingsPage(daw::EngineController* controller,
                                             QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    using Policy = daw::EngineController::MonitorStopPolicy;

    m_mode = new QComboBox;
    m_mode->addItem(tr("Overwrite — a new take replaces what was there"),
                    int(daw::RecordMode::Overwrite));
    m_mode->addItem(tr("Layer recording — every take is kept inside the clip"),
                    int(daw::RecordMode::Layers));

    m_loopMode = new QComboBox;
    m_loopMode->addItem(tr("Keep each pass as a new take"), true);
    m_loopMode->addItem(tr("Overwrite the last take"), false);

    m_trimTakes = new QCheckBox(tr("Trim layers to the recorded region"));
    m_trimTakes->setToolTip(
        tr("On: a layer covers only where signal was actually recorded.\n"
           "Off: it spans the whole clip, silent outside the recorded stretch."));

    m_autoExpand = new QCheckBox(tr("Open the comp editor when recording stops"));

    m_crossfade = new QDoubleSpinBox;
    m_crossfade->setRange(0.0, 20.0);
    m_crossfade->setDecimals(1);
    m_crossfade->setSingleStep(0.5);
    m_crossfade->setSuffix(tr(" ms"));
    m_crossfade->setToolTip(
        tr("Applied at every seam between comp segments. Zero butts them "
           "together, which can click."));

    m_midiComp = new QComboBox;
    m_midiComp->addItem(tr("Replace — the new take's notes stand alone"), false);
    m_midiComp->addItem(tr("Overdub merge — notes are added to what is there"),
                        true);

    m_countIn = new QComboBox;
    m_countIn->addItem(tr("Off — recording starts immediately"), 0);
    for (int beats = 1; beats <= 4; ++beats)
        m_countIn->addItem(tr("Beats: %1").arg(beats), beats);
    m_countIn->setToolTip(
        tr("Clicks counted off before the take starts — three, two, one. The "
           "transport stays where it is while they play, so the take begins "
           "exactly at the playhead and nothing of the count is recorded."));

    m_recordKey = new QCheckBox(
        tr("R starts recording on its own (record arm + start)"));
    m_recordKey->setToolTip(
        tr("Off: the transport's Record button has to be engaged first, and R "
           "then starts the recording."));

    m_monitorStop = new QComboBox;
    m_monitorStop->addItem(tr("Keep monitoring on"), int(Policy::KeepOn));
    m_monitorStop->addItem(tr("Return to the previous state"),
                           int(Policy::ReturnToPrevious));
    m_monitorStop->addItem(tr("Switch monitoring off"), int(Policy::AutoDisable));

    m_autoMonitor = new QCheckBox(
        tr("Open input monitoring while recording"));
    m_autoMonitor->setToolTip(
        tr("On: the track being recorded is monitored for the take (and the "
           "count-in before it) and goes back to how it was on stop.\n"
           "Off: recording never touches monitoring — a track is only heard "
           "live if you switched its monitor on yourself."));

    m_manualMonitor = new QCheckBox(
        tr("A manual monitor click stops automatic management for that track"));

    auto* form = new QFormLayout;
    form->addRow(tr("Default recording mode"), m_mode);
    form->addRow(tr("When loop recording"), m_loopMode);
    form->addRow(QString(), m_trimTakes);
    form->addRow(QString(), m_autoExpand);
    form->addRow(tr("Comp crossfade"), m_crossfade);
    form->addRow(tr("MIDI comp mode"), m_midiComp);

    auto* monitorForm = new QFormLayout;
    monitorForm->addRow(tr("Count-in"), m_countIn);
    monitorForm->addRow(QString(), m_recordKey);
    monitorForm->addRow(QString(), m_autoMonitor);
    monitorForm->addRow(tr("When recording stops"), m_monitorStop);
    monitorForm->addRow(QString(), m_manualMonitor);

    auto* hint = new QLabel(
        tr("Input monitoring is decided per recording: if another track already "
           "monitors the same input, this one is left alone so you do not hear "
           "the source twice. Tracks whose monitor was set automatically show a "
           "small \"A\" on the button.\n\n"
           "Shift+L toggles layer recording. Holding Ctrl+Shift+L while a "
           "recording runs inverts the mode for that take only. E opens the comp "
           "editor on the selected clip; hold A and click a layer to audition it."));
    hint->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addSpacing(8);
    layout->addWidget(new QLabel(tr("<b>Recording start and monitoring</b>")));
    layout->addLayout(monitorForm);
    layout->addSpacing(8);
    layout->addWidget(hint);
    layout->addStretch(1);

    reload();

    const auto onCombo = [this](int) { commit(); };
    connect(m_mode, &QComboBox::currentIndexChanged, this, onCombo);
    connect(m_loopMode, &QComboBox::currentIndexChanged, this, onCombo);
    connect(m_midiComp, &QComboBox::currentIndexChanged, this, onCombo);
    connect(m_monitorStop, &QComboBox::currentIndexChanged, this, onCombo);
    connect(m_countIn, &QComboBox::currentIndexChanged, this, onCombo);
    connect(m_crossfade, &QDoubleSpinBox::valueChanged, this,
            [this](double) { commit(); });
    for (QCheckBox* box : {m_trimTakes, m_autoExpand, m_recordKey,
                           m_autoMonitor, m_manualMonitor}) {
        connect(box, &QCheckBox::toggled, this, [this](bool) { commit(); });
    }
}

void RecordingSettingsPage::reload() {
    const auto& prefs = m_controller->recordingPrefs();
    m_loading = true;
    m_mode->setCurrentIndex(m_mode->findData(int(prefs.mode)));
    m_loopMode->setCurrentIndex(m_loopMode->findData(prefs.loopCreatesTakes));
    m_trimTakes->setChecked(prefs.trimTakesToRegion);
    m_autoExpand->setChecked(prefs.autoExpandAfterRecord);
    m_crossfade->setValue(prefs.compCrossfadeMs);
    m_midiComp->setCurrentIndex(m_midiComp->findData(prefs.midiOverdubMerge));
    m_recordKey->setChecked(prefs.recordKeyArmsAndStarts);
    m_countIn->setCurrentIndex(m_countIn->findData(prefs.countInBeats));
    m_monitorStop->setCurrentIndex(
        m_monitorStop->findData(int(prefs.monitorStopPolicy)));
    m_autoMonitor->setChecked(prefs.autoMonitorOnRecord);
    m_manualMonitor->setChecked(prefs.manualMonitorDisablesAuto);
    m_loading = false;
}

void RecordingSettingsPage::commit() {
    if (m_loading) return;
    using Policy = daw::EngineController::MonitorStopPolicy;
    auto prefs = m_controller->recordingPrefs();
    const auto before = prefs.mode;

    prefs.mode = m_mode->currentData().toInt() == int(daw::RecordMode::Layers)
                     ? daw::RecordMode::Layers
                     : daw::RecordMode::Overwrite;
    prefs.loopCreatesTakes = m_loopMode->currentData().toBool();
    prefs.trimTakesToRegion = m_trimTakes->isChecked();
    prefs.autoExpandAfterRecord = m_autoExpand->isChecked();
    prefs.compCrossfadeMs = m_crossfade->value();
    prefs.midiOverdubMerge = m_midiComp->currentData().toBool();
    prefs.recordKeyArmsAndStarts = m_recordKey->isChecked();
    prefs.countInBeats = m_countIn->currentData().toInt();
    const int policy = m_monitorStop->currentData().toInt();
    prefs.monitorStopPolicy =
        policy == int(Policy::KeepOn)        ? Policy::KeepOn
        : policy == int(Policy::AutoDisable) ? Policy::AutoDisable
                                             : Policy::ReturnToPrevious;
    prefs.autoMonitorOnRecord = m_autoMonitor->isChecked();
    prefs.manualMonitorDisablesAuto = m_manualMonitor->isChecked();
    m_controller->setRecordingPrefs(prefs);

    QSettings s;
    s.setValue(kMode, int(prefs.mode));
    s.setValue(kLoopTakes, prefs.loopCreatesTakes);
    s.setValue(kTrimTakes, prefs.trimTakesToRegion);
    s.setValue(kAutoExpand, prefs.autoExpandAfterRecord);
    s.setValue(kCrossfade, prefs.compCrossfadeMs);
    s.setValue(kMidiMerge, prefs.midiOverdubMerge);
    s.setValue(kRecordKey, prefs.recordKeyArmsAndStarts);
    s.setValue(kCountIn, prefs.countInBeats);
    s.setValue(kMonitorStop, int(prefs.monitorStopPolicy));
    s.setValue(kManualMonitor, prefs.manualMonitorDisablesAuto);
    s.setValue(kAutoMonitor, prefs.autoMonitorOnRecord);

    // The crossfade length changes what the comp sounds like, so the graph has
    // to be rebuilt — setRecordingPrefs does that. The transport button only
    // needs telling when the mode itself moved.
    if (prefs.mode != before) emit recordModeChanged();
}
