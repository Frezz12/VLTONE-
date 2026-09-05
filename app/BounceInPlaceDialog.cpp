#include "BounceInPlaceDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <set>

namespace {
constexpr auto kSettings = "bounceInPlace";

bool enabled(std::uint32_t mask, daw::EngineController::BounceFxLayer layer) {
    return (mask & std::uint32_t(layer)) != 0;
}
} // namespace

BounceInPlaceDialog::BounceInPlaceDialog(
    daw::EngineController& controller,
    daw::EngineController::BounceRequest request,
    QString sourceDescription, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_baseRequest(std::move(request)) {
    setWindowTitle(tr("Bounce in Place"));
    setModal(true);
    setMinimumWidth(500);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* source = new QLabel(sourceDescription, this);
    source->setWordWrap(true);
    root->addWidget(source);

    auto* rangeBox = new QGroupBox(tr("Render range"), this);
    auto* rangeForm = new QFormLayout(rangeBox);
    m_ending = new QComboBox(rangeBox);
    m_ending->addItem(tr("End at selection"));
    m_ending->addItem(tr("Continue until silence"));
    m_ending->addItem(tr("Custom Start / End"));
    rangeForm->addRow(tr("Ending"), m_ending);
    m_start = new QDoubleSpinBox(rangeBox);
    m_end = new QDoubleSpinBox(rangeBox);
    for (QDoubleSpinBox* field : {m_start, m_end}) {
        field->setRange(0.0, 24.0 * 60.0 * 60.0);
        field->setDecimals(3);
        field->setSuffix(tr(" s"));
    }
    m_start->setValue(m_baseRequest.startSeconds);
    m_end->setValue(m_baseRequest.endSeconds);
    rangeForm->addRow(tr("Start"), m_start);
    rangeForm->addRow(tr("End"), m_end);
    m_preRoll = new QDoubleSpinBox(rangeBox);
    m_preRoll->setRange(-1.0, 86400.0);
    m_preRoll->setDecimals(1);
    m_preRoll->setSuffix(tr(" s"));
    m_preRoll->setSpecialValueText(tr("From project start"));
    m_preRoll->setToolTip(tr("Audio processed before the selection to restore effect tails. A shorter warm-up renders faster but may change long reverb or delay tails."));
    rangeForm->addRow(tr("Effect warm-up"), m_preRoll);
    root->addWidget(rangeBox);

    auto* fxBox = new QGroupBox(tr("Processing to print"), this);
    auto* fxLayout = new QVBoxLayout(fxBox);
    m_clipFx = new QCheckBox(tr("Clip FX"), fxBox);
    m_trackFx = new QCheckBox(tr("Track FX"), fxBox);
    m_sends = new QCheckBox(tr("Sends / Returns"), fxBox);
    m_summing = new QCheckBox(tr("Summing Folders"), fxBox);
    m_masterFx = new QCheckBox(tr("Master FX"), fxBox);
    for (QCheckBox* box : {m_clipFx, m_trackFx, m_sends, m_summing,
                           m_masterFx})
        fxLayout->addWidget(box);
    root->addWidget(fxBox);

    auto* destinationBox = new QGroupBox(tr("After rendering"), this);
    auto* destinationForm = new QFormLayout(destinationBox);
    m_destination = new QComboBox(destinationBox);
    m_destination->addItem(tr("Replace in place"),
                           int(daw::EngineController::BounceDestination::Replace));
    m_destination->addItem(tr("Create a new track"),
                           int(daw::EngineController::BounceDestination::NewTrack));
    destinationForm->addRow(tr("Destination"), m_destination);
    root->addWidget(destinationBox);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setAccessibleName(tr("Bounce summary"));
    root->addWidget(m_summary);
    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setAccessibleName(tr("Bounce warning"));
    root->addWidget(m_warning);

    m_status = new QLabel(tr("Ready"), this);
    m_status->setAccessibleName(tr("Bounce status"));
    m_progress = new QProgressBar(this);
    m_progress->setAccessibleName(tr("Bounce progress"));
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    root->addWidget(m_status);
    root->addWidget(m_progress);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_renderButton = m_buttons->addButton(tr("Bounce"),
                                         QDialogButtonBox::AcceptRole);
    m_renderButton->setDefault(true);
    root->addWidget(m_buttons);

    QSettings settings;
    settings.beginGroup(QLatin1String(kSettings));
    m_preRoll->setValue(settings.value("preRoll", -1.0).toDouble());
    m_clipFx->setChecked(settings.value("clipFx", true).toBool());
    m_trackFx->setChecked(settings.value("trackFx", true).toBool());
    m_sends->setChecked(settings.value("sends", false).toBool());
    m_summing->setChecked(settings.value("summing", true).toBool());
    m_masterFx->setChecked(settings.value("masterFx", false).toBool());
    m_ending->setCurrentIndex(settings.value("tail", 0).toInt());
    m_destination->setCurrentIndex(settings.value("destination", 0).toInt());
    settings.endGroup();

    connect(m_ending, &QComboBox::currentIndexChanged, this,
            &BounceInPlaceDialog::syncControls);
    for (QCheckBox* box : {m_clipFx, m_trackFx, m_sends, m_summing,
                           m_masterFx}) {
        connect(box, &QCheckBox::toggled, this,
                &BounceInPlaceDialog::updateSummary);
    }
    connect(m_destination, &QComboBox::currentIndexChanged, this,
            &BounceInPlaceDialog::updateSummary);
    connect(m_start, &QDoubleSpinBox::valueChanged, this,
            &BounceInPlaceDialog::updateSummary);
    connect(m_end, &QDoubleSpinBox::valueChanged, this,
            &BounceInPlaceDialog::updateSummary);
    connect(m_renderButton, &QPushButton::clicked, this,
            &BounceInPlaceDialog::startRender);
    connect(m_buttons, &QDialogButtonBox::rejected, this,
            &BounceInPlaceDialog::reject);

    syncControls();
}

void BounceInPlaceDialog::reject() {
    if (m_rendering) {
        m_cancelRequested = true;
        return;
    }
    QDialog::reject();
}

void BounceInPlaceDialog::syncControls() {
    const bool custom = m_ending->currentIndex() == 2;
    m_start->setEnabled(custom);
    m_end->setEnabled(custom);
    updateSummary();
}

daw::EngineController::BounceRequest
BounceInPlaceDialog::requestFromControls() const {
    auto request = m_baseRequest;
    request.preRollSeconds = m_preRoll->value();
    if (m_ending->currentIndex() == 2) {
        request.startSeconds = m_start->value();
        request.endSeconds = m_end->value();
    }
    request.tail = m_ending->currentIndex() == 1
                       ? daw::rendering::Tail::UntilSilence
                       : daw::rendering::Tail::None;
    request.fxLayers = 0;
    const auto add = [&](QCheckBox* box,
                         daw::EngineController::BounceFxLayer layer) {
        if (box->isChecked()) request.fxLayers |= std::uint32_t(layer);
    };
    add(m_clipFx, daw::EngineController::BounceFxLayer::Clip);
    add(m_trackFx, daw::EngineController::BounceFxLayer::Track);
    add(m_sends, daw::EngineController::BounceFxLayer::Sends);
    add(m_summing, daw::EngineController::BounceFxLayer::Summing);
    add(m_masterFx, daw::EngineController::BounceFxLayer::Master);
    request.destination = static_cast<daw::EngineController::BounceDestination>(
        m_destination->currentData().toInt());
    return request;
}

void BounceInPlaceDialog::updateSummary() {
    const auto request = requestFromControls();
    QStringList baked;
    QStringList live;
    QStringList skipped;
    const std::array<std::pair<QCheckBox*, QString>, 5> layers{{
        {m_clipFx, tr("Clip FX")},
        {m_trackFx, tr("Track FX")},
        {m_sends, tr("Sends / Returns")},
        {m_summing, tr("Summing Folders")},
        {m_masterFx, tr("Master FX")},
    }};
    const bool masterPrinted = m_masterFx->isChecked();
    const bool sharedCapture = request.fullMix || m_sends->isChecked() ||
                               m_summing->isChecked();
    const bool trackCapture = m_trackFx->isChecked();
    for (int i = 0; i < int(layers.size()); ++i) {
        const auto& [box, name] = layers[std::size_t(i)];
        if (box->isChecked())
            baked << name;
        else if (!masterPrinted && i == 4)
            live << name;
        else if (!masterPrinted && !sharedCapture && trackCapture && i >= 2)
            live << name;
        else if (!masterPrinted && !sharedCapture && !trackCapture && i >= 1)
            live << name;
        else
            skipped << name;
    }
    m_summary->setText(
        tr("Will be printed: %1\nWill remain live: %2\nWill be skipped: %3")
            .arg(baked.isEmpty() ? tr("source only") : baked.join(", "),
                 live.isEmpty() ? tr("none") : live.join(", "),
                 skipped.isEmpty() ? tr("none") : skipped.join(", ")));

    const bool shared = enabled(request.fxLayers,
                                daw::EngineController::BounceFxLayer::Sends) ||
                        enabled(request.fxLayers,
                                daw::EngineController::BounceFxLayer::Summing) ||
                        enabled(request.fxLayers,
                                daw::EngineController::BounceFxLayer::Master);
    std::set<std::string> sourceTracks(request.tracks.begin(),
                                       request.tracks.end());
    for (const auto& clip : request.clips) sourceTracks.insert(clip.trackId);
    const std::size_t sources = request.fullMix ? 1 : sourceTracks.size();
    m_warning->setText(shared && sources > 1
                           ? tr("Warning: shared nonlinear effects are rendered with each "
                                "source soloed; the sum of files can differ from "
                                "the live mix.")
                           : QString());
    m_renderButton->setEnabled(!m_rendering &&
                               request.endSeconds > request.startSeconds);
}

void BounceInPlaceDialog::startRender() {
    const auto request = requestFromControls();
    if (request.endSeconds <= request.startSeconds) {
        QMessageBox::warning(this, tr("Bounce in Place"),
                             tr("End must be later than Start."));
        return;
    }
    QSettings settings;
    settings.beginGroup(QLatin1String(kSettings));
    settings.setValue("clipFx", m_clipFx->isChecked());
    settings.setValue("preRoll", m_preRoll->value());
    settings.setValue("trackFx", m_trackFx->isChecked());
    settings.setValue("sends", m_sends->isChecked());
    settings.setValue("summing", m_summing->isChecked());
    settings.setValue("masterFx", m_masterFx->isChecked());
    settings.setValue("tail", m_ending->currentIndex());
    settings.setValue("destination", m_destination->currentIndex());
    settings.endGroup();

    m_rendering = true;
    m_cancelRequested = false;
    m_ending->setEnabled(false);
    m_start->setEnabled(false);
    m_end->setEnabled(false);
    m_preRoll->setEnabled(false);
    for (QCheckBox* box : {m_clipFx, m_trackFx, m_sends, m_summing,
                           m_masterFx})
        box->setEnabled(false);
    m_destination->setEnabled(false);
    m_renderButton->setEnabled(false);
    m_status->setText(tr("Rendering…"));
    daw::EngineController::BounceReport report;
    const audio::Result result = m_controller.bounceInPlace(
        request,
        [this](const daw::rendering::Progress& progress) {
            m_progress->setValue(
                std::clamp(int(progress.fraction * 1000.0), 0, 1000));
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
    m_preRoll->setEnabled(true);
    if (report.cancelled || m_cancelRequested) {
        m_status->setText(tr("Cancelled — the project was not changed"));
        m_ending->setEnabled(true);
        for (QCheckBox* box : {m_clipFx, m_trackFx, m_sends, m_summing,
                               m_masterFx})
            box->setEnabled(true);
        m_destination->setEnabled(true);
        syncControls();
        return;
    }
    if (!result) {
        m_status->setText(tr("Bounce failed"));
        QMessageBox::critical(this, tr("Bounce in Place"),
                              QString::fromStdString(result.message()));
        m_ending->setEnabled(true);
        for (QCheckBox* box : {m_clipFx, m_trackFx, m_sends, m_summing,
                               m_masterFx})
            box->setEnabled(true);
        m_destination->setEnabled(true);
        syncControls();
        return;
    }
    m_progress->setValue(1000);
    m_status->setText(tr("Bounce complete"));
    m_rendered = true;
    accept();
}
