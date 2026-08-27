#include "PianoRollTools.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace mt = daw::miditools;

namespace {

/// The note-value vocabulary every tool shares: the denominator of the division
/// (1 = whole note … 128), which `mt::gridBeatsFor` turns into beats.
const QVector<int>& divisions() {
    static const QVector<int> values = {1, 2, 4, 8, 16, 32, 64, 128};
    return values;
}

void fillDivisions(QComboBox* box, int defaultDenominator) {
    for (int denominator : divisions()) {
        box->addItem(QString("1/%1").arg(denominator), denominator);
    }
    box->setCurrentIndex(int(divisions().indexOf(defaultDenominator)));
}

void fillFlavours(QComboBox* box) {
    box->addItem(QObject::tr("Straight"), int(mt::GridFlavour::Straight));
    box->addItem(QObject::tr("Triplet"), int(mt::GridFlavour::Triplet));
    box->addItem(QObject::tr("Dotted"), int(mt::GridFlavour::Dotted));
}

double beatsFrom(const QComboBox* division, const QComboBox* flavour) {
    return mt::gridBeatsFor(division->currentData().toInt(),
                            mt::GridFlavour(flavour->currentData().toInt()));
}

/// Pick the division/flavour pair that reproduces `beats`, so a dialog opens
/// showing the grid the roll is actually on.
void selectBeats(QComboBox* division, QComboBox* flavour, double beats) {
    for (int flavourIndex = 0; flavourIndex < flavour->count(); ++flavourIndex) {
        const auto kind = mt::GridFlavour(flavour->itemData(flavourIndex).toInt());
        for (int divisionIndex = 0; divisionIndex < division->count(); ++divisionIndex) {
            const int denominator = division->itemData(divisionIndex).toInt();
            if (std::abs(mt::gridBeatsFor(denominator, kind) - beats) < 1e-6) {
                division->setCurrentIndex(divisionIndex);
                flavour->setCurrentIndex(flavourIndex);
                return;
            }
        }
    }
}

QSlider* numberSlider(int low, int high, int value, int singleStep = 1,
                      int pageStep = 0) {
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(low, high);
    slider->setValue(value);
    slider->setSingleStep(std::max(1, singleStep));
    slider->setPageStep(pageStep > 0 ? pageStep
                                     : std::max(1, (high - low) / 10));
    slider->setProperty("midiNumericSlider", true);
    return slider;
}

QSlider* percentSlider(int low, int high, int value) {
    return numberSlider(low, high, value);
}

QString compactDecimal(double value, int decimals = 3) {
    QString text = QString::number(value, 'f', decimals);
    while (text.contains('.') && text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}

using SliderFormatter = std::function<QString(int)>;

/// Every numeric tool parameter is a slider, but never an anonymous one: the
/// live read-out preserves the exact value for fine keyboard adjustment.
QWidget* withReadout(QSlider* slider, SliderFormatter format,
                     int readoutWidth = 72) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* value = new QLabel(format(slider->value()), row);
    value->setMinimumWidth(readoutWidth);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setProperty("midiNumericReadout", true);
    QObject::connect(slider, &QSlider::valueChanged, value, [value, format](int v) {
        value->setText(format(v));
    });
    layout->addWidget(slider, 1);
    layout->addWidget(value);
    return row;
}

QWidget* withReadout(QSlider* slider, const QString& suffix) {
    return withReadout(slider,
                       [suffix](int value) {
                           return QString::number(value) + suffix;
                       },
                       suffix.isEmpty() ? 42 : 52);
}

QWidget* beatsReadout(QSlider* slider, double scale,
                      const QString& prefix = QString(),
                      const QString& zeroText = QString(), int decimals = 3) {
    return withReadout(
        slider,
        [scale, prefix, zeroText, decimals](int raw) {
            if (raw == 0 && !zeroText.isEmpty()) return zeroText;
            return prefix + compactDecimal(double(raw) / scale, decimals) +
                   QObject::tr(" beats");
        },
        92);
}

QSlider* cellSlider(const QTableWidget* table, int row, int column) {
    QWidget* cell = table ? table->cellWidget(row, column) : nullptr;
    return cell ? cell->findChild<QSlider*>() : nullptr;
}

const QStringList& pitchClasses() {
    static const QStringList names = {"C",  "C#", "D",  "D#", "E",  "F",
                                      "F#", "G",  "G#", "A",  "A#", "B"};
    return names;
}

} // namespace

// ── ToolDialog ──────────────────────────────────────────────────────────────

ToolDialog::ToolDialog(const QString& title, QWidget* parent)
    : QDialog(parent, Qt::Widget) {
    setWindowTitle(title);
    // Modeless: a tool is something you sit with, nudging a slider against the
    // grid behind it. A modal dialog would hide the very thing being edited.
    setWindowModality(Qt::NonModal);

    auto* column = new QVBoxLayout(this);
    m_form = new QGridLayout;
    m_form->setColumnStretch(1, 1);
    column->addLayout(m_form);
    column->addStretch(1);
}

QGridLayout* ToolDialog::form() { return m_form; }

bool ToolDialog::previewEnabled() const {
    return m_preview && m_preview->isChecked();
}

void ToolDialog::finishLayout() {
    m_preview = new QCheckBox(tr("Preview"), this);
    m_preview->setChecked(true);
    m_preview->setToolTip(
        tr("Draw the result in the grid as you change the settings. Nothing is "
           "written to the clip until you press Apply."));
    connect(m_preview, &QCheckBox::toggled, this, &ToolDialog::paramsChanged);

    m_buttons = new QDialogButtonBox(this);
    QPushButton* apply = m_buttons->addButton(QDialogButtonBox::Apply);
    m_buttons->addButton(QDialogButtonBox::Close);
    connect(apply, &QPushButton::clicked, this, &ToolDialog::applyRequested);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto* row = new QHBoxLayout;
    row->addWidget(m_preview);
    row->addStretch(1);
    row->addWidget(m_buttons);
    static_cast<QVBoxLayout*>(layout())->addLayout(row);
}

void ToolDialog::watch(QWidget* widget) {
    if (auto* box = qobject_cast<QComboBox*>(widget)) {
        connect(box, &QComboBox::currentIndexChanged, this,
                &ToolDialog::paramsChanged);
    } else if (auto* slider = qobject_cast<QSlider*>(widget)) {
        connect(slider, &QSlider::valueChanged, this, &ToolDialog::paramsChanged);
    } else if (auto* check = qobject_cast<QCheckBox*>(widget)) {
        connect(check, &QCheckBox::toggled, this, &ToolDialog::paramsChanged);
    }
}

void ToolDialog::closeEvent(QCloseEvent* event) {
    // Closing has to drop the preview, or the grid keeps showing a result that
    // is never going to be applied. `rejected` is what the roll listens to.
    emit rejected();
    QDialog::closeEvent(event);
}

// ── Quantize ────────────────────────────────────────────────────────────────

QuantizeDialog::QuantizeDialog(QWidget* parent)
    : ToolDialog(tr("Quantize"), parent) {
    auto* grid = form();
    int row = 0;

    m_grid = new QComboBox(this);
    fillDivisions(m_grid, 16);
    m_flavour = new QComboBox(this);
    fillFlavours(m_flavour);
    auto* gridRow = new QWidget(this);
    auto* gridLayout = new QHBoxLayout(gridRow);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->addWidget(m_grid, 1);
    gridLayout->addWidget(m_flavour, 1);
    grid->addWidget(new QLabel(tr("Grid"), this), row, 0);
    grid->addWidget(gridRow, row++, 1);

    m_target = new QComboBox(this);
    m_target->addItem(tr("Start"), int(mt::QuantizeParams::Target::Start));
    m_target->addItem(tr("End"), int(mt::QuantizeParams::Target::End));
    m_target->addItem(tr("Note length"), int(mt::QuantizeParams::Target::Length));
    m_target->addItem(tr("Start + length"),
                      int(mt::QuantizeParams::Target::StartLength));
    m_target->addItem(tr("Start + end"), int(mt::QuantizeParams::Target::StartEnd));
    grid->addWidget(new QLabel(tr("Quantize"), this), row, 0);
    grid->addWidget(m_target, row++, 1);

    m_strength = percentSlider(0, 100, 100);
    grid->addWidget(new QLabel(tr("Strength"), this), row, 0);
    grid->addWidget(withReadout(m_strength, "%"), row++, 1);

    m_swing = percentSlider(50, 90, 50);
    grid->addWidget(new QLabel(tr("Swing"), this), row, 0);
    grid->addWidget(withReadout(m_swing, "%"), row++, 1);

    m_swingUnit = new QComboBox(this);
    m_swingUnit->addItem(tr("1/8"), 0.5);
    m_swingUnit->addItem(tr("1/16"), 0.25);
    m_swingUnit->addItem(tr("1/4"), 1.0);
    grid->addWidget(new QLabel(tr("Swing unit"), this), row, 0);
    grid->addWidget(m_swingUnit, row++, 1);

    m_tolerance = numberSlider(0, 1000, 0, 5, 100);
    m_tolerance->setToolTip(
        tr("Notes already this close to the grid are left exactly where they "
           "are — the dead zone that stops quantize from flattening detail."));
    grid->addWidget(new QLabel(tr("Leave notes within"), this), row, 0);
    grid->addWidget(beatsReadout(m_tolerance, 1000.0), row++, 1);

    m_randomize = numberSlider(0, 250, 0, 1, 25);
    grid->addWidget(new QLabel(tr("Randomize after"), this), row, 0);
    grid->addWidget(beatsReadout(m_randomize, 1000.0), row++, 1);

    m_preserveOrder = new QCheckBox(tr("Preserve note order"), this);
    m_preserveOrder->setChecked(true);
    grid->addWidget(m_preserveOrder, row++, 1);

    m_groove = new QComboBox(this);
    for (const auto& groove : mt::groovePresets()) {
        m_groove->addItem(QString::fromStdString(groove.name));
    }
    grid->addWidget(new QLabel(tr("Groove"), this), row, 0);
    grid->addWidget(m_groove, row++, 1);

    m_grooveTiming = percentSlider(0, 100, 100);
    grid->addWidget(new QLabel(tr("Groove timing"), this), row, 0);
    grid->addWidget(withReadout(m_grooveTiming, "%"), row++, 1);

    m_grooveVelocity = percentSlider(0, 100, 100);
    grid->addWidget(new QLabel(tr("Groove velocity"), this), row, 0);
    grid->addWidget(withReadout(m_grooveVelocity, "%"), row++, 1);

    for (QWidget* widget : {static_cast<QWidget*>(m_grid),
                            static_cast<QWidget*>(m_flavour),
                            static_cast<QWidget*>(m_target),
                            static_cast<QWidget*>(m_strength),
                            static_cast<QWidget*>(m_swing),
                            static_cast<QWidget*>(m_swingUnit),
                            static_cast<QWidget*>(m_tolerance),
                            static_cast<QWidget*>(m_randomize),
                            static_cast<QWidget*>(m_preserveOrder),
                            static_cast<QWidget*>(m_groove),
                            static_cast<QWidget*>(m_grooveTiming),
                            static_cast<QWidget*>(m_grooveVelocity)}) {
        watch(widget);
    }
    finishLayout();
}

void QuantizeDialog::setGridBeats(double beats) {
    if (beats <= 0.0) return;
    selectBeats(m_grid, m_flavour, beats);
}

mt::QuantizeParams QuantizeDialog::params() const {
    mt::QuantizeParams p;
    p.gridBeats = beatsFrom(m_grid, m_flavour);
    p.strength = m_strength->value() / 100.0;
    p.target = mt::QuantizeParams::Target(m_target->currentData().toInt());
    p.swing = m_swing->value() / 100.0;
    p.swingUnitBeats = m_swingUnit->currentData().toDouble();
    p.toleranceBeats = m_tolerance->value() / 1000.0;
    p.preserveOrder = m_preserveOrder->isChecked();
    p.randomizeBeats = m_randomize->value() / 1000.0;
    const int grooveIndex = m_groove->currentIndex();
    if (grooveIndex > 0 && grooveIndex < int(mt::groovePresets().size())) {
        p.groove = mt::groovePresets()[size_t(grooveIndex)];
    }
    p.grooveTiming = m_grooveTiming->value() / 100.0;
    p.grooveVelocity = m_grooveVelocity->value() / 100.0;
    // A fixed seed keeps the preview and the apply identical; the jitter is
    // still different for each note, which is all it needs to be.
    p.seed = 20260808u;
    return p;
}

// ── Arpeggiator ─────────────────────────────────────────────────────────────

ArpeggiatorDialog::ArpeggiatorDialog(QWidget* parent)
    : ToolDialog(tr("Arpeggiator"), parent) {
    auto* grid = form();
    int row = 0;

    m_direction = new QComboBox(this);
    m_direction->addItem(tr("Up"), int(mt::ArpParams::Direction::Up));
    m_direction->addItem(tr("Down"), int(mt::ArpParams::Direction::Down));
    m_direction->addItem(tr("Up / Down"), int(mt::ArpParams::Direction::UpDown));
    m_direction->addItem(tr("Down / Up"), int(mt::ArpParams::Direction::DownUp));
    m_direction->addItem(tr("Random"), int(mt::ArpParams::Direction::Random));
    m_direction->addItem(tr("Chord"), int(mt::ArpParams::Direction::Chord));
    grid->addWidget(new QLabel(tr("Direction"), this), row, 0);
    grid->addWidget(m_direction, row++, 1);

    m_octaves = numberSlider(1, 5, 1);
    grid->addWidget(new QLabel(tr("Octave range"), this), row, 0);
    grid->addWidget(withReadout(m_octaves, QString()), row++, 1);

    m_rate = new QComboBox(this);
    fillDivisions(m_rate, 16);
    m_flavour = new QComboBox(this);
    fillFlavours(m_flavour);
    auto* rateRow = new QWidget(this);
    auto* rateLayout = new QHBoxLayout(rateRow);
    rateLayout->setContentsMargins(0, 0, 0, 0);
    rateLayout->addWidget(m_rate, 1);
    rateLayout->addWidget(m_flavour, 1);
    grid->addWidget(new QLabel(tr("Rate"), this), row, 0);
    grid->addWidget(rateRow, row++, 1);

    m_gate = percentSlider(10, 200, 90);
    grid->addWidget(new QLabel(tr("Gate"), this), row, 0);
    grid->addWidget(withReadout(m_gate, "%"), row++, 1);

    m_stepCount = numberSlider(1, 16, 1);
    grid->addWidget(new QLabel(tr("Pattern steps"), this), row, 0);
    grid->addWidget(withReadout(m_stepCount, QString()), row++, 1);

    m_steps = new QTableWidget(0, 4, this);
    m_steps->setHorizontalHeaderLabels(
        {tr("Velocity"), tr("Skip"), tr("Tie"), tr("Transpose")});
    m_steps->verticalHeader()->setDefaultSectionSize(34);
    m_steps->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_steps->setMinimumWidth(600);
    m_steps->setMaximumHeight(260);
    grid->addWidget(m_steps, row++, 0, 1, 2);
    connect(m_stepCount, &QSlider::valueChanged, this,
            &ArpeggiatorDialog::rebuildSteps);
    rebuildSteps(1);

    m_ramp = percentSlider(-100, 100, 0);
    grid->addWidget(new QLabel(tr("Velocity ramp"), this), row, 0);
    grid->addWidget(withReadout(m_ramp, "%"), row++, 1);

    m_swing = percentSlider(50, 90, 50);
    grid->addWidget(new QLabel(tr("Swing"), this), row, 0);
    grid->addWidget(withReadout(m_swing, "%"), row++, 1);

    m_humanizeVelocity = numberSlider(0, 30, 0);
    grid->addWidget(new QLabel(tr("Humanize velocity"), this), row, 0);
    grid->addWidget(withReadout(m_humanizeVelocity, QString()), row++, 1);

    m_humanizeTiming = numberSlider(0, 250, 0, 1, 25);
    grid->addWidget(new QLabel(tr("Humanize timing"), this), row, 0);
    grid->addWidget(beatsReadout(m_humanizeTiming, 1000.0), row++, 1);

    m_playMode = new QComboBox(this);
    m_playMode->addItem(tr("Once"), int(mt::ArpParams::PlayMode::Once));
    m_playMode->addItem(tr("Loop"), int(mt::ArpParams::PlayMode::Loop));
    m_playMode->addItem(tr("Loop && fill clip"), int(mt::ArpParams::PlayMode::Fill));
    m_playMode->setCurrentIndex(1);
    grid->addWidget(new QLabel(tr("Play mode"), this), row, 0);
    grid->addWidget(m_playMode, row++, 1);

    m_merge = new QCheckBox(tr("Merge — keep the original chord"), this);
    grid->addWidget(m_merge, row++, 1);

    for (QWidget* widget : {static_cast<QWidget*>(m_direction),
                            static_cast<QWidget*>(m_octaves),
                            static_cast<QWidget*>(m_rate),
                            static_cast<QWidget*>(m_flavour),
                            static_cast<QWidget*>(m_gate),
                            static_cast<QWidget*>(m_ramp),
                            static_cast<QWidget*>(m_swing),
                            static_cast<QWidget*>(m_humanizeVelocity),
                            static_cast<QWidget*>(m_humanizeTiming),
                            static_cast<QWidget*>(m_playMode),
                            static_cast<QWidget*>(m_merge)}) {
        watch(widget);
    }
    connect(m_steps, &QTableWidget::cellChanged, this,
            &ArpeggiatorDialog::paramsChanged);
    finishLayout();
}

void ArpeggiatorDialog::rebuildSteps(int count) {
    const int previous = m_steps->rowCount();
    m_steps->setRowCount(count);
    for (int row = previous; row < count; ++row) {
        auto* velocity = numberSlider(1, 127, 100);
        auto* skip = new QTableWidgetItem();
        skip->setCheckState(Qt::Unchecked);
        skip->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        auto* tie = new QTableWidgetItem();
        tie->setCheckState(Qt::Unchecked);
        tie->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        auto* transpose = numberSlider(-24, 24, 0);
        m_steps->setCellWidget(row, 0, withReadout(velocity, QString()));
        m_steps->setItem(row, 1, skip);
        m_steps->setItem(row, 2, tie);
        m_steps->setCellWidget(
            row, 3,
            withReadout(transpose, [](int value) {
                return QString(value > 0 ? "+%1 st" : "%1 st").arg(value);
            }, 48));
        connect(velocity, &QSlider::valueChanged, this,
                &ArpeggiatorDialog::paramsChanged);
        connect(transpose, &QSlider::valueChanged, this,
                &ArpeggiatorDialog::paramsChanged);
    }
    emit paramsChanged();
}

mt::ArpParams ArpeggiatorDialog::params() const {
    mt::ArpParams p;
    p.direction = mt::ArpParams::Direction(m_direction->currentData().toInt());
    p.octaves = m_octaves->value();
    p.rateBeats = beatsFrom(m_rate, m_flavour);
    p.gate = m_gate->value() / 100.0;
    p.velocityRamp = m_ramp->value() / 100.0;
    p.swing = m_swing->value() / 100.0;
    p.humanizeVelocity = m_humanizeVelocity->value();
    p.humanizeTiming = m_humanizeTiming->value() / 1000.0;
    p.playMode = mt::ArpParams::PlayMode(m_playMode->currentData().toInt());
    p.merge = m_merge->isChecked();
    p.seed = 20260808u;

    for (int row = 0; row < m_steps->rowCount(); ++row) {
        mt::ArpParams::Step step;
        if (auto* slider = cellSlider(m_steps, row, 0))
            step.velocity = slider->value();
        if (auto* item = m_steps->item(row, 1)) {
            step.skip = item->checkState() == Qt::Checked;
        }
        if (auto* item = m_steps->item(row, 2)) {
            step.tie = item->checkState() == Qt::Checked;
        }
        if (auto* slider = cellSlider(m_steps, row, 3))
            step.transpose = slider->value();
        p.steps.push_back(step);
    }
    return p;
}

void ArpeggiatorDialog::setRateBeats(double beats) {
    if (beats <= 0.0) return;
    selectBeats(m_rate, m_flavour, beats);
}

// ── Glue ────────────────────────────────────────────────────────────────────

GlueDialog::GlueDialog(QWidget* parent) : ToolDialog(tr("Glue"), parent) {
    auto* grid = form();
    int row = 0;

    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Merge overlapping"),
                    int(mt::GlueParams::Mode::MergeOverlapping));
    m_mode->addItem(tr("Legato"), int(mt::GlueParams::Mode::Legato));
    grid->addWidget(new QLabel(tr("Mode"), this), row, 0);
    grid->addWidget(m_mode, row++, 1);

    m_samePitch = new QCheckBox(tr("Same pitch only"), this);
    m_samePitch->setChecked(true);
    grid->addWidget(m_samePitch, row++, 1);

    m_gap = numberSlider(0, 4000, 0, 5, 250);
    grid->addWidget(new QLabel(tr("Gap tolerance"), this), row, 0);
    grid->addWidget(beatsReadout(m_gap, 1000.0), row++, 1);

    m_legatoMax = numberSlider(0, 32000, 0, 50, 2000);
    grid->addWidget(new QLabel(tr("Legato max length"), this), row, 0);
    grid->addWidget(beatsReadout(m_legatoMax, 1000.0, QString(), tr("no limit")),
                    row++, 1);

    // The two modes use disjoint settings; greying the irrelevant ones out is
    // cheaper to read than a tab or a stacked page.
    auto syncEnabled = [this] {
        const bool legato =
            m_mode->currentData().toInt() == int(mt::GlueParams::Mode::Legato);
        m_samePitch->setEnabled(!legato);
        m_gap->setEnabled(!legato);
        m_legatoMax->setEnabled(legato);
    };
    connect(m_mode, &QComboBox::currentIndexChanged, this, syncEnabled);
    syncEnabled();

    for (QWidget* widget : {static_cast<QWidget*>(m_mode),
                            static_cast<QWidget*>(m_samePitch),
                            static_cast<QWidget*>(m_gap),
                            static_cast<QWidget*>(m_legatoMax)}) {
        watch(widget);
    }
    finishLayout();
}

mt::GlueParams GlueDialog::params() const {
    mt::GlueParams p;
    p.mode = mt::GlueParams::Mode(m_mode->currentData().toInt());
    p.samePitchOnly = m_samePitch->isChecked();
    p.gapToleranceBeats = m_gap->value() / 1000.0;
    p.legatoMaxBeats = m_legatoMax->value() / 1000.0;
    return p;
}

// ── Articulate ──────────────────────────────────────────────────────────────

ArticulateDialog::ArticulateDialog(QWidget* parent)
    : ToolDialog(tr("Articulate"), parent) {
    auto* grid = form();
    int row = 0;

    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Gate — against the next note"),
                    int(mt::ArticulateParams::LengthMode::Gate));
    m_mode->addItem(tr("Scale — against its own length"),
                    int(mt::ArticulateParams::LengthMode::Scale));
    m_mode->addItem(tr("Leave lengths alone"),
                    int(mt::ArticulateParams::LengthMode::Keep));
    m_mode->setToolTip(
        tr("Gate measures each note against the distance to the next one, so "
           "one setting means the same thing to quarters and to sixteenths."));
    grid->addWidget(new QLabel(tr("Length"), this), row, 0);
    grid->addWidget(m_mode, row++, 1);

    // Past 100% a gate overlaps the next note, which is a legitimate thing to
    // ask for on a pad or a sustained bass.
    m_gate = percentSlider(5, 200, 80);
    grid->addWidget(new QLabel(tr("Gate"), this), row, 0);
    grid->addWidget(withReadout(m_gate, "%"), row++, 1);

    m_amount = percentSlider(0, 100, 100);
    m_amount->setToolTip(
        tr("How far each note travels toward the gated length. Below 100% the "
           "phrasing already in the part survives."));
    grid->addWidget(new QLabel(tr("Amount"), this), row, 0);
    grid->addWidget(withReadout(m_amount, "%"), row++, 1);

    // One slider step is 1/32 beat, matching the musical increment the old
    // numeric field used without losing the exact fraction to decimal rounding.
    m_minLength = numberSlider(1, 128, 2);
    grid->addWidget(new QLabel(tr("Shortest"), this), row, 0);
    grid->addWidget(beatsReadout(m_minLength, 32.0, QString(), QString(), 5),
                    row++, 1);

    m_maxLength = numberSlider(0, 128, 0);
    m_maxLength->setToolTip(
        tr("Stops a note gating against a distant next note from stretching "
           "across the whole bar."));
    grid->addWidget(new QLabel(tr("Longest"), this), row, 0);
    grid->addWidget(beatsReadout(m_maxLength, 4.0, QString(), tr("no limit")),
                    row++, 1);

    m_accentOn = new QCheckBox(tr("Accent every"), this);
    m_accentEvery = numberSlider(2, 16, 4);
    auto* accentRow = new QWidget(this);
    auto* accentLayout = new QHBoxLayout(accentRow);
    accentLayout->setContentsMargins(0, 0, 0, 0);
    accentLayout->addWidget(m_accentOn);
    accentLayout->addWidget(withReadout(
        m_accentEvery,
        [](int value) { return QObject::tr("%1th note").arg(value); }, 68), 1);
    grid->addWidget(accentRow, row++, 1);

    m_accentVelocity = numberSlider(-64, 64, 14);
    grid->addWidget(new QLabel(tr("Velocity"), this), row, 0);
    grid->addWidget(withReadout(
                        m_accentVelocity,
                        [](int value) {
                            return QObject::tr("accented %1%2")
                                .arg(value > 0 ? "+" : "")
                                .arg(value);
                        },
                        84),
                    row++, 1);

    m_otherVelocity = numberSlider(-64, 64, -6);
    m_otherVelocity->setToolTip(
        tr("Pulling the unaccented notes down is what makes an accent audible "
           "without the part getting louder overall."));
    grid->addWidget(withReadout(
                        m_otherVelocity,
                        [](int value) {
                            return QObject::tr("the rest %1%2")
                                .arg(value > 0 ? "+" : "")
                                .arg(value);
                        },
                        84),
                    row++, 1);

    // Gate and Scale share the length controls; Keep uses none of them, and the
    // accent settings mean nothing until the accent is switched on.
    auto syncEnabled = [this] {
        const bool lengths =
            m_mode->currentData().toInt() != int(mt::ArticulateParams::LengthMode::Keep);
        for (QWidget* widget : {static_cast<QWidget*>(m_gate),
                                static_cast<QWidget*>(m_amount),
                                static_cast<QWidget*>(m_minLength),
                                static_cast<QWidget*>(m_maxLength)}) {
            widget->setEnabled(lengths);
        }
        const bool accent = m_accentOn->isChecked();
        m_accentEvery->setEnabled(accent);
        m_accentVelocity->setEnabled(accent);
        m_otherVelocity->setEnabled(accent);
    };
    connect(m_mode, &QComboBox::currentIndexChanged, this, syncEnabled);
    connect(m_accentOn, &QCheckBox::toggled, this, syncEnabled);
    syncEnabled();

    for (QWidget* widget : {static_cast<QWidget*>(m_mode),
                            static_cast<QWidget*>(m_gate),
                            static_cast<QWidget*>(m_amount),
                            static_cast<QWidget*>(m_minLength),
                            static_cast<QWidget*>(m_maxLength),
                            static_cast<QWidget*>(m_accentOn),
                            static_cast<QWidget*>(m_accentEvery),
                            static_cast<QWidget*>(m_accentVelocity),
                            static_cast<QWidget*>(m_otherVelocity)}) {
        watch(widget);
    }
    finishLayout();
}

mt::ArticulateParams ArticulateDialog::params() const {
    mt::ArticulateParams p;
    p.lengthMode = mt::ArticulateParams::LengthMode(m_mode->currentData().toInt());
    p.gate = m_gate->value() / 100.0;
    p.amount = m_amount->value() / 100.0;
    p.minLengthBeats = m_minLength->value() / 32.0;
    p.maxLengthBeats = m_maxLength->value() / 4.0;
    p.accentEvery = m_accentOn->isChecked() ? m_accentEvery->value() : 0;
    p.accentVelocity = m_accentVelocity->value();
    p.otherVelocity = m_otherVelocity->value();
    return p;
}

// ── Strum ───────────────────────────────────────────────────────────────────

StrumDialog::StrumDialog(QWidget* parent) : ToolDialog(tr("Strum"), parent) {
    auto* grid = form();
    int row = 0;

    m_direction = new QComboBox(this);
    m_direction->addItem(tr("Up (low to high)"),
                         int(mt::StrumParams::Direction::Up));
    m_direction->addItem(tr("Down (high to low)"),
                         int(mt::StrumParams::Direction::Down));
    grid->addWidget(new QLabel(tr("Direction"), this), row, 0);
    grid->addWidget(m_direction, row++, 1);

    m_span = numberSlider(5, 2000, 125, 1, 125);
    grid->addWidget(new QLabel(tr("Time"), this), row, 0);
    grid->addWidget(beatsReadout(m_span, 1000.0), row++, 1);

    m_shape = new QComboBox(this);
    m_shape->addItem(tr("Linear"), int(mt::StrumParams::Shape::Linear));
    m_shape->addItem(tr("Accelerating"), int(mt::StrumParams::Shape::Accelerate));
    m_shape->addItem(tr("Decelerating"), int(mt::StrumParams::Shape::Decelerate));
    grid->addWidget(new QLabel(tr("Shape"), this), row, 0);
    grid->addWidget(m_shape, row++, 1);

    m_taper = percentSlider(-100, 100, 0);
    grid->addWidget(new QLabel(tr("Velocity taper"), this), row, 0);
    grid->addWidget(withReadout(m_taper, "%"), row++, 1);

    m_window = numberSlider(0, 1000, 20, 1, 50);
    m_window->setToolTip(
        tr("Notes starting within this of each other count as one chord. "
           "Anything further apart is a separate event and is left alone."));
    grid->addWidget(new QLabel(tr("Chord window"), this), row, 0);
    grid->addWidget(beatsReadout(m_window, 1000.0), row++, 1);

    m_adjustEnds = new QCheckBox(tr("Move note ends too (keep lengths)"), this);
    grid->addWidget(m_adjustEnds, row++, 1);

    for (QWidget* widget : {static_cast<QWidget*>(m_direction),
                            static_cast<QWidget*>(m_span),
                            static_cast<QWidget*>(m_shape),
                            static_cast<QWidget*>(m_taper),
                            static_cast<QWidget*>(m_window),
                            static_cast<QWidget*>(m_adjustEnds)}) {
        watch(widget);
    }
    finishLayout();
}

mt::StrumParams StrumDialog::params() const {
    mt::StrumParams p;
    p.direction = mt::StrumParams::Direction(m_direction->currentData().toInt());
    p.spanBeats = m_span->value() / 1000.0;
    p.shape = mt::StrumParams::Shape(m_shape->currentData().toInt());
    p.velocityTaper = m_taper->value() / 100.0;
    p.chordWindowBeats = m_window->value() / 1000.0;
    p.adjustEnds = m_adjustEnds->isChecked();
    return p;
}

// ── Randomize ───────────────────────────────────────────────────────────────

RandomizeDialog::RandomizeDialog(QWidget* parent)
    : ToolDialog(tr("Randomize"), parent) {
    auto* grid = form();
    int row = 0;

    m_velocityOn = new QCheckBox(tr("Velocity"), this);
    m_velocity = numberSlider(1, 127, 20);
    grid->addWidget(m_velocityOn, row, 0);
    grid->addWidget(withReadout(m_velocity, [](int value) {
                        return QString::fromUtf8("± %1").arg(value);
                    }),
                    row++, 1);

    m_pitchOn = new QCheckBox(tr("Pitch"), this);
    m_pitch = numberSlider(1, 48, 2);
    grid->addWidget(m_pitchOn, row, 0);
    grid->addWidget(withReadout(
                        m_pitch,
                        [](int value) {
                            return QString::fromUtf8("± %1 ").arg(value) +
                                   QObject::tr("semitones");
                        },
                        96),
                    row++, 1);

    m_scaleAware = new QCheckBox(tr("Keep pitches in scale"), this);
    grid->addWidget(m_scaleAware, row++, 1);

    m_scaleRoot = new QComboBox(this);
    m_scaleRoot->addItems(pitchClasses());
    m_scale = new QComboBox(this);
    for (auto scale : mt::allScales()) {
        m_scale->addItem(QString::fromStdString(mt::scaleName(scale)), int(scale));
    }
    m_scale->setCurrentIndex(1);   // Major
    auto* scaleRow = new QWidget(this);
    auto* scaleLayout = new QHBoxLayout(scaleRow);
    scaleLayout->setContentsMargins(0, 0, 0, 0);
    scaleLayout->addWidget(m_scaleRoot, 1);
    scaleLayout->addWidget(m_scale, 2);
    grid->addWidget(new QLabel(tr("Scale"), this), row, 0);
    grid->addWidget(scaleRow, row++, 1);

    m_timingOn = new QCheckBox(tr("Timing"), this);
    m_timing = numberSlider(1, 1000, 20, 1, 50);
    grid->addWidget(m_timingOn, row, 0);
    grid->addWidget(beatsReadout(m_timing, 1000.0, QString::fromUtf8("± ")),
                    row++, 1);

    m_constrain = new QCheckBox(tr("Round the result back onto the grid"), this);
    grid->addWidget(m_constrain, row++, 1);

    m_durationOn = new QCheckBox(tr("Duration"), this);
    m_duration = numberSlider(1, 100, 20);
    grid->addWidget(m_durationOn, row, 0);
    grid->addWidget(withReadout(m_duration, [](int value) {
                        return QString::fromUtf8("± %1%").arg(value);
                    }),
                    row++, 1);

    m_gaussian = new QCheckBox(tr("Normal distribution (most notes barely move)"),
                               this);
    m_gaussian->setChecked(true);
    grid->addWidget(m_gaussian, row++, 1);

    m_preserveTotal = new QCheckBox(tr("Keep everything inside the clip"), this);
    m_preserveTotal->setChecked(true);
    grid->addWidget(m_preserveTotal, row++, 1);

    m_seed = numberSlider(1, 999999, 1, 1, 10000);
    auto* seedRow = new QWidget(this);
    auto* seedLayout = new QHBoxLayout(seedRow);
    seedLayout->setContentsMargins(0, 0, 0, 0);
    auto* dice = new QPushButton(tr("Dice"), seedRow);
    dice->setToolTip(tr("Another roll of the same settings."));
    connect(dice, &QPushButton::clicked, this,
            [this] { m_seed->setValue(QRandomGenerator::global()->bounded(1, 999999)); });
    seedLayout->addWidget(withReadout(m_seed, QString()), 1);
    seedLayout->addWidget(dice);
    grid->addWidget(new QLabel(tr("Seed"), this), row, 0);
    grid->addWidget(seedRow, row++, 1);

    for (QWidget* widget : {static_cast<QWidget*>(m_velocityOn),
                            static_cast<QWidget*>(m_velocity),
                            static_cast<QWidget*>(m_pitchOn),
                            static_cast<QWidget*>(m_pitch),
                            static_cast<QWidget*>(m_scaleAware),
                            static_cast<QWidget*>(m_scaleRoot),
                            static_cast<QWidget*>(m_scale),
                            static_cast<QWidget*>(m_timingOn),
                            static_cast<QWidget*>(m_timing),
                            static_cast<QWidget*>(m_constrain),
                            static_cast<QWidget*>(m_durationOn),
                            static_cast<QWidget*>(m_duration),
                            static_cast<QWidget*>(m_gaussian),
                            static_cast<QWidget*>(m_preserveTotal),
                            static_cast<QWidget*>(m_seed)}) {
        watch(widget);
    }
    finishLayout();
}

void RandomizeDialog::setRegionEndBeats(double beats) {
    m_regionEndBeats = beats;
}

void RandomizeDialog::setGridBeats(double beats) {
    if (beats > 0.0) m_gridBeats = beats;
}

mt::RandomParams RandomizeDialog::params() const {
    mt::RandomParams p;
    p.velocity = m_velocityOn->isChecked();
    p.velocityAmount = m_velocity->value();
    p.pitch = m_pitchOn->isChecked();
    p.pitchAmount = m_pitch->value();
    p.timing = m_timingOn->isChecked();
    p.timingBeats = m_timing->value() / 1000.0;
    p.duration = m_durationOn->isChecked();
    p.durationPercent = m_duration->value();
    p.gaussian = m_gaussian->isChecked();
    p.scaleAware = m_scaleAware->isChecked();
    p.scaleRoot = m_scaleRoot->currentIndex();
    p.scale = mt::Scale(m_scale->currentData().toInt());
    p.constrainToGrid = m_constrain->isChecked();
    p.gridBeats = m_gridBeats;
    p.preserveTotalDuration = m_preserveTotal->isChecked();
    p.regionEndBeats = m_regionEndBeats;
    p.seed = uint32_t(m_seed->value());
    return p;
}

// ── Chord generator ─────────────────────────────────────────────────────────

ChordDialog::ChordDialog(QWidget* parent)
    : ToolDialog(tr("Chord Generator"), parent) {
    auto* grid = form();
    int row = 0;

    m_type = new QComboBox(this);
    for (auto type : mt::allChordTypes()) {
        m_type->addItem(QString::fromStdString(mt::chordName(type)), int(type));
    }
    grid->addWidget(new QLabel(tr("Chord"), this), row, 0);
    grid->addWidget(m_type, row++, 1);

    m_inversion = numberSlider(0, 4, 0);
    grid->addWidget(new QLabel(tr("Inversion"), this), row, 0);
    grid->addWidget(withReadout(m_inversion, QString()), row++, 1);

    m_addOctave = new QCheckBox(tr("Double the root an octave up"), this);
    grid->addWidget(m_addOctave, row++, 1);
    m_bassOctave = new QCheckBox(tr("Add a root an octave down"), this);
    grid->addWidget(m_bassOctave, row++, 1);

    for (QWidget* widget : {static_cast<QWidget*>(m_type),
                            static_cast<QWidget*>(m_inversion),
                            static_cast<QWidget*>(m_addOctave),
                            static_cast<QWidget*>(m_bassOctave)}) {
        watch(widget);
    }
    finishLayout();
}

mt::ChordParams ChordDialog::params() const {
    mt::ChordParams p;
    p.type = mt::ChordParams::Type(m_type->currentData().toInt());
    p.inversion = m_inversion->value();
    p.addOctave = m_addOctave->isChecked();
    p.bassOctave = m_bassOctave->isChecked();
    return p;
}
