#include "MidiGeneratorDialog.hpp"
#include "EngineController.hpp"
#include "Icons.hpp"
#include "SlideNotes.hpp"
#include "Theme.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace ai = daw::ai;
namespace mt = daw::miditools;

namespace {
const daw::ClipModel* findClip(const daw::EngineController& controller,
                              const std::string& trackId, const std::string& clipId) {
    if (const auto* track = controller.project().findTrack(trackId))
        for (const auto& clip : track->clips)
            if (clip.id == clipId && clip.kind == daw::ClipKind::Midi) return &clip;
    return nullptr;
}
std::string takeId(const daw::ClipModel& clip) {
    for (const auto& take : clip.takes)
        if (&daw::midiNotes(clip) == &take.notes) return take.id;
    return {};
}
}

class MidiGeneratorPreview final : public QWidget {
public:
    explicit MidiGeneratorPreview(QWidget* parent) : QWidget(parent) {
        setMinimumHeight(150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(MidiGeneratorDialog::tr("Generated MIDI preview"));
    }
    void setNotes(const mt::Notes& notes, double start, double length, double bar) {
        m_notes = notes; m_start = start; m_length = length; m_bar = bar; update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), th().well());
        const QRectF grid(36, 22, std::max(1, width() - 46), std::max(1, height() - 32));
        int low = 127, high = 0;
        for (const auto& note : m_notes) {
            if (note.startBeats >= m_start + m_length || note.startBeats + note.lengthBeats <= m_start) continue;
            low = std::min(low, note.pitch); high = std::max(high, note.pitch);
        }
        if (low > high) { low = 60; high = 72; }
        low = std::max(0, low - 2); high = std::min(127, std::max(high + 2, low + 12));
        const double row = grid.height() / (high - low + 1);
        p.setPen(th().gridLine);
        for (int pitch = low; pitch <= high; ++pitch) {
            const double y = grid.bottom() - (pitch - low + .5) * row;
            if (pitch % 12 == 0) {
                p.drawLine(QPointF(grid.left(), y), QPointF(grid.right(), y));
                p.setPen(th().textSecondary);
                p.drawText(QRectF(0, y - 8, 31, 16), Qt::AlignRight | Qt::AlignVCenter,
                           QString::fromStdString(mt::pitchName(pitch)));
                p.setPen(th().gridLine);
            }
        }
        const int bars = std::max(1, int(std::ceil(m_length / m_bar)));
        const int labelStep = std::max(1, int(std::ceil(30.0 * bars / grid.width())));
        for (int bar = 0; bar <= bars; ++bar) {
            const double x = grid.left() + grid.width() * bar / bars;
            p.setPen(th().gridLineStrong); p.drawLine(QPointF(x, grid.top()), QPointF(x, grid.bottom()));
            if (bar < bars && bar % labelStep == 0) {
                p.setPen(th().textSecondary);
                p.drawText(QRectF(x + 3, 1, 35, 18), QString::number(bar + 1));
            }
        }
        p.setPen(Qt::NoPen);
        for (const auto& note : m_notes) {
            const double from = std::max(m_start, note.startBeats), to = std::min(m_start + m_length, note.startBeats + note.lengthBeats);
            if (to <= from) continue;
            QColor color = note.muted ? th().textSecondary : th().accent;
            color.setAlpha(note.muted ? 60 : 120 + note.velocity * 135 / 127);
            p.setBrush(color);
            p.drawRoundedRect(QRectF(grid.left() + (from - m_start) / m_length * grid.width(),
                grid.bottom() - (note.pitch - low + 1) * row,
                std::max(1.0, (to - from) / m_length * grid.width() - 1), std::max(2.0, row - 1)), 2, 2);
        }
    }
private:
    mt::Notes m_notes;
    double m_start = 0, m_length = 32, m_bar = 4;
};

MidiGeneratorDialog::MidiGeneratorDialog(daw::EngineController& controller,
    std::string trackId, std::string clipId, double startBeats, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_trackId(std::move(trackId)),
      m_clipId(std::move(clipId)), m_startBeats(std::max(0.0, startBeats)) {
    setObjectName("MidiGeneratorDialog");
    setWindowTitle(tr("MIDI Generator"));
    resize(650, 600); setMinimumWidth(540);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16); root->setSpacing(10);
    auto* settings = new QWidget(this);
    auto* settingsLayout = new QVBoxLayout(settings); settingsLayout->setContentsMargins(0, 0, 8, 0);
    auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); scroll->setWidget(settings);
    scroll->setMinimumHeight(215); root->addWidget(scroll, 2);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto combo = [this](const char* name, const QStringList& items) {
        auto* field = new QComboBox(this); field->setObjectName(name); field->addItems(items); return field;
    };
    const auto addRow = [](QFormLayout* layout, const QString& label, QWidget* field) {
        auto* text = new QLabel(label); text->setBuddy(field); field->setAccessibleName(label); layout->addRow(text, field);
    };
    m_mode = combo("GeneratorMode", {tr("Melody"), tr("Chords")});
    addRow(form, tr("Generate"), m_mode);
    auto* lengthRow = new QHBoxLayout;
    m_bars = new QSpinBox(this); m_bars->setObjectName("GeneratorBars"); m_bars->setRange(1, 64); m_bars->setValue(8);
    m_bars->setSuffix(tr(" bars")); m_bars->setAccessibleName(tr("Length in bars"));
    lengthRow->addWidget(m_bars);
    for (int bars : {2, 4, 8, 16}) {
        auto* button = new QPushButton(QString::number(bars), this); button->setAutoDefault(false);
        button->setAccessibleName(tr("%1 bars").arg(bars)); button->setMaximumWidth(44);
        connect(button, &QPushButton::clicked, this, [this, bars] { m_bars->setValue(bars); });
        lengthRow->addWidget(button);
    }
    lengthRow->addStretch(); form->addRow(tr("Length"), lengthRow);
    auto* keyRow = new QHBoxLayout;
    m_key = combo("GeneratorKey", {"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"});
    m_key->setCurrentIndex(std::clamp(controller.project().keyRoot, 0, 11)); m_key->setAccessibleName(tr("Key"));
    m_scale = combo("GeneratorScale", {}); m_scale->setAccessibleName(tr("Scale"));
    const auto scaleLabel = [](mt::Scale scale) {
        switch (scale) {
        case mt::Scale::Chromatic: return tr("Chromatic");
        case mt::Scale::Major: return tr("Major");
        case mt::Scale::NaturalMinor: return tr("Natural Minor");
        case mt::Scale::HarmonicMinor: return tr("Harmonic Minor");
        case mt::Scale::MelodicMinor: return tr("Melodic Minor");
        case mt::Scale::Dorian: return tr("Dorian");
        case mt::Scale::Phrygian: return tr("Phrygian");
        case mt::Scale::Lydian: return tr("Lydian");
        case mt::Scale::Mixolydian: return tr("Mixolydian");
        case mt::Scale::Locrian: return tr("Locrian");
        case mt::Scale::PentatonicMajor: return tr("Pentatonic Major");
        case mt::Scale::PentatonicMinor: return tr("Pentatonic Minor");
        case mt::Scale::Blues: return tr("Blues");
        case mt::Scale::WholeTone: return tr("Whole Tone");
        }
        return tr("Scale");
    };
    for (const auto scale : mt::allScales()) m_scale->addItem(scaleLabel(scale), QString::fromStdString(mt::scaleId(scale)));
    m_scale->setCurrentIndex(std::max(0, m_scale->findData(QString::fromStdString(controller.project().scale))));
    keyRow->addWidget(m_key); keyRow->addWidget(m_scale, 1); form->addRow(tr("Key / scale"), keyRow);
    m_character = combo("GeneratorCharacter", {tr("Singing"), tr("Rhythmic"), tr("Spacious")});
    addRow(form, tr("Character"), m_character);
    const auto slider = [this, form, &addRow](const char* name, const QString& label, int value) {
        auto* host = new QWidget(this); auto* layout = new QHBoxLayout(host); layout->setContentsMargins(0, 0, 0, 0);
        auto* field = new QSlider(Qt::Horizontal, host); field->setObjectName(name); field->setRange(0, 100); field->setValue(value);
        field->setAccessibleName(label);
        auto* number = new QLabel(QString::number(value) + "%", host); number->setMinimumWidth(38);
        connect(field, &QSlider::valueChanged, number, [number](int n) { number->setText(QString::number(n) + "%"); });
        layout->addWidget(field, 1); layout->addWidget(number); host->setFocusProxy(field); addRow(form, label, host); return field;
    };
    m_density = slider("GeneratorDensity", tr("Density"), 50);
    m_creativity = slider("GeneratorCreativity", tr("Variation"), 50);
    m_harmony = combo("GeneratorHarmony", {tr("From key and scale")});
    m_harmonySources.push_back({});
    for (const auto& track : controller.project().tracks) for (const auto& clip : track.clips) {
        if (clip.kind != daw::ClipKind::Midi || daw::midiNotes(clip).empty()) continue;
        m_harmony->addItem(QString::fromStdString(track.name + " / " + clip.name));
        m_harmonySources.emplace_back(track.id, clip.id);
    }
    m_harmony->setToolTip(tr("Use the selected clip's harmony at the same arrangement position. Outside it, use the selected key."));
    addRow(form, tr("Harmony"), m_harmony);
    settingsLayout->addLayout(form);

    auto* advancedButton = new QToolButton(this); advancedButton->setText(tr("More settings"));
    advancedButton->setCheckable(true); advancedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedButton->setArrowType(Qt::RightArrow); settingsLayout->addWidget(advancedButton);
    auto* advanced = new QWidget(this); advanced->hide();
    auto* extras = new QFormLayout(advanced); extras->setContentsMargins(0, 0, 0, 0);
    auto* range = new QHBoxLayout;
    m_low = combo("GeneratorLow", {}); m_high = combo("GeneratorHigh", {});
    for (int pitch = 0; pitch < 128; ++pitch) {
        const auto text = QString::fromStdString(mt::pitchName(pitch)); m_low->addItem(text); m_high->addItem(text);
    }
    m_low->setCurrentIndex(60); m_high->setCurrentIndex(84);
    m_low->setAccessibleName(tr("Lowest note")); m_high->setAccessibleName(tr("Highest note"));
    range->addWidget(m_low, 1); range->addWidget(new QLabel("–", this), 0); range->addWidget(m_high, 1); extras->addRow(tr("Pitch range"), range);
    m_swing = new QSpinBox(this); m_swing->setObjectName("GeneratorSwing"); m_swing->setRange(50, 75); m_swing->setSuffix("%");
    addRow(extras, tr("Swing"), m_swing);
    m_expression = new QSpinBox(this); m_expression->setObjectName("GeneratorExpression"); m_expression->setRange(0, 100); m_expression->setValue(15); m_expression->setSuffix("%");
    addRow(extras, tr("Expression"), m_expression);
    m_ending = combo("GeneratorEnding", {tr("Resolved phrase"), tr("Loop")}); addRow(extras, tr("Ending"), m_ending);
    m_chordRate = combo("GeneratorChordRate", {tr("Half a bar"), tr("One bar"), tr("Two bars")}); m_chordRate->setCurrentIndex(1);
    addRow(extras, tr("Chord changes"), m_chordRate);
    m_voicing = combo("GeneratorVoicing", {tr("Triads"), tr("Seventh chords")}); addRow(extras, tr("Chords"), m_voicing);
    m_seed = new QSpinBox(this); m_seed->setObjectName("GeneratorSeed"); m_seed->setRange(0, std::numeric_limits<int>::max());
    m_seed->setValue(int(QRandomGenerator::global()->bounded(quint32(std::numeric_limits<int>::max())))); addRow(extras, tr("Seed"), m_seed);
    settingsLayout->addWidget(advanced); settingsLayout->addStretch();
    connect(advancedButton, &QToolButton::toggled, this, [advanced, advancedButton](bool show) {
        advanced->setVisible(show); advancedButton->setArrowType(show ? Qt::DownArrow : Qt::RightArrow);
    });

    auto* variants = new QHBoxLayout;
    for (int i = 0; i < 3; ++i) {
        m_variants[i] = new QRadioButton(tr("Variant %1").arg(i + 1), this);
        m_variants[i]->setObjectName(QString("GeneratorVariant%1").arg(i + 1)); m_variants[i]->setEnabled(false);
        variants->addWidget(m_variants[i]);
        connect(m_variants[i], &QRadioButton::clicked, this, [this, i] { selectCandidate(i); });
    }
    variants->addStretch();
    m_generate = new QPushButton(tr("Generate more"), this); m_generate->setObjectName("GeneratorMore"); m_generate->setAutoDefault(false);
    variants->addWidget(m_generate); root->addLayout(variants);
    m_preview = new MidiGeneratorPreview(this); root->addWidget(m_preview, 1);
    m_insert = combo("GeneratorInsert", {tr("Replace notes in generated range"), tr("Add to existing notes")});
    m_insert->setAccessibleName(tr("Insert mode")); root->addWidget(m_insert);
    m_status = new QLabel(this); m_status->setObjectName("GeneratorStatus"); m_status->setWordWrap(true); root->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    m_listen = buttons->addButton(tr("Listen"), QDialogButtonBox::ActionRole); m_listen->setObjectName("GeneratorListen"); m_listen->setAutoDefault(false);
    m_apply = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole); m_apply->setObjectName("GeneratorApply"); m_apply->setDefault(true);
    root->addWidget(buttons);
    m_debounce = new QTimer(this); m_debounce->setSingleShot(true); m_debounce->setInterval(120);
    connect(m_debounce, &QTimer::timeout, this, &MidiGeneratorDialog::generate);
    m_poll = new QTimer(this); m_poll->setInterval(33);
    connect(m_poll, &QTimer::timeout, this, &MidiGeneratorDialog::poll); m_poll->start();
    connect(buttons, &QDialogButtonBox::rejected, this, &MidiGeneratorDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, &MidiGeneratorDialog::apply);
    connect(m_listen, &QPushButton::clicked, this, &MidiGeneratorDialog::toggleListen);
    connect(m_generate, &QPushButton::clicked, this, [this] {
        const QSignalBlocker block(m_seed); m_seed->setValue(int((m_seed->value() + 1LL) % std::numeric_limits<int>::max())); scheduleGeneration();
    });
    for (auto* field : {m_mode, m_key, m_scale, m_character, m_harmony, m_low, m_high, m_ending, m_chordRate, m_voicing, m_insert})
        connect(field, &QComboBox::currentIndexChanged, this, &MidiGeneratorDialog::scheduleGeneration);
    for (auto* field : {m_bars, m_swing, m_expression, m_seed})
        connect(field, &QSpinBox::valueChanged, this, &MidiGeneratorDialog::scheduleGeneration);
    for (auto* field : {m_density, m_creativity})
        connect(field, &QSlider::valueChanged, this, &MidiGeneratorDialog::scheduleGeneration);
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int mode) {
        const QSignalBlocker low(m_low), high(m_high);
        if (mode == 1 && m_low->currentIndex() == 60 && m_high->currentIndex() == 84) {
            m_low->setCurrentIndex(48); m_high->setCurrentIndex(72);
        } else if (mode == 0 && m_low->currentIndex() == 48 && m_high->currentIndex() == 72) {
            m_low->setCurrentIndex(60); m_high->setCurrentIndex(84);
        }
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &MidiGeneratorDialog::style);
    style(); scheduleGeneration();
}

MidiGeneratorDialog::~MidiGeneratorDialog() { stopListening(); }
void MidiGeneratorDialog::hideEvent(QHideEvent* event) { stopListening(); QDialog::hideEvent(event); }
void MidiGeneratorDialog::done(int result) {
    m_closed = true; ++m_serial; m_debounce->stop(); m_poll->stop(); stopListening();
    QDialog::done(result);
}
const daw::ClipModel* MidiGeneratorDialog::target() const { return findClip(m_controller, m_trackId, m_clipId); }

bool MidiGeneratorDialog::contextMatches() const {
    const auto* clip = target();
    if (!clip || m_controller.projectRevision() != m_revision || m_controller.tempo() != m_tempo ||
        m_controller.timeSigNumerator() != m_request.meterNumerator || m_controller.timeSigDenominator() != m_request.meterDenominator ||
        clip->startSeconds != m_clipStart || clip->durationSeconds != m_clipDuration || clip->offsetSeconds != m_clipOffset ||
        clip->contentOffsetBeats != m_contentOffset || takeId(*clip) != m_takeId ||
        daw::midiNotes(*clip) != m_baseNotes || daw::slides::editable(*clip) != m_baseSlides) return false;
    if (!m_harmonySource.second.empty()) {
        const auto* source = findClip(m_controller, m_harmonySource.first, m_harmonySource.second);
        if (!source || source->startSeconds != m_harmonyStart || source->offsetSeconds != m_harmonyOffset ||
            source->durationSeconds != m_harmonyDuration || daw::midiNotes(*source) != m_harmonyNotes) return false;
    }
    return true;
}

void MidiGeneratorDialog::scheduleGeneration() {
    if (m_closed) return;
    stopListening(); ++m_serial; m_candidates.clear(); m_selected = -1;
    m_preview->setNotes({}, 0, m_bars->value() * 4.0, 4);
    m_status->setText(tr("Generating…")); m_pending = true; m_debounce->start(); refreshAvailability();
}

void MidiGeneratorDialog::generate() {
    if (m_closed || !m_pending || m_job.valid()) return;
    m_pending = false; m_draft.reset();
    const auto* clip = target();
    if (!clip) { m_status->setText(tr("The target clip is no longer available.")); refreshAvailability(); return; }
    m_revision = m_controller.projectRevision(); m_tempo = m_controller.tempo();
    m_clipStart = clip->startSeconds; m_clipDuration = clip->durationSeconds; m_clipOffset = clip->offsetSeconds;
    m_contentOffset = clip->contentOffsetBeats; m_baseNotes = daw::midiNotes(*clip); m_baseSlides = daw::slides::editable(*clip); m_takeId = takeId(*clip);
    auto& request = m_request; request = {};
    request.role = m_mode->currentIndex() == 0 ? ai::CompositionRole::Melody : ai::CompositionRole::Chords;
    request.bars = m_bars->value(); request.seed = std::uint64_t(m_seed->value());
    request.keyRoot = m_key->currentIndex(); request.scale = m_scale->currentData().toString().toStdString();
    request.pitchRange = ai::CompositionPitchRange{m_low->currentIndex(), m_high->currentIndex()};
    request.meterNumerator = m_controller.timeSigNumerator(); request.meterDenominator = m_controller.timeSigDenominator();
    request.beatsPerBar = request.meterNumerator * 4.0 / request.meterDenominator;
    request.character = ai::CompositionCharacter(m_character->currentIndex()); request.ending = ai::CompositionEnding(m_ending->currentIndex());
    request.rhythmicDensity = m_density->value() / 100.0; request.creativity = m_creativity->value() / 100.0;
    request.swing = m_swing->value() / 100.0; request.expression = m_expression->value() / 100.0;
    request.chordBars = std::array{.5, 1.0, 2.0}[m_chordRate->currentIndex()]; request.seventhChords = m_voicing->currentIndex() == 1;
    m_harmonySource = m_harmonySources[std::size_t(m_harmony->currentIndex())]; m_harmonyNotes.clear();
    mt::Notes harmonyInput;
    double harmonyFrom = 0, harmonyTo = request.bars * request.beatsPerBar;
    if (!m_harmonySource.second.empty()) {
        const auto* source = findClip(m_controller, m_harmonySource.first, m_harmonySource.second);
        if (!source) { m_status->setText(tr("Choose an available harmony clip.")); refreshAvailability(); return; }
        m_harmonyNotes = daw::midiNotes(*source); m_harmonyStart = source->startSeconds;
        m_harmonyOffset = source->offsetSeconds; m_harmonyDuration = source->durationSeconds;
        const double offset = (source->startSeconds - source->offsetSeconds - m_clipStart + m_clipOffset) * m_tempo / 60.0 - m_startBeats;
        const double sourceFrom = source->offsetSeconds * m_tempo / 60.0;
        const double sourceTo = (source->offsetSeconds + source->durationSeconds) * m_tempo / 60.0;
        harmonyFrom = std::max(0.0, sourceFrom + offset); harmonyTo = std::min(harmonyTo, sourceTo + offset);
        for (auto note : m_harmonyNotes) {
            if (note.muted) continue;
            const double from = std::max({0.0, sourceFrom + offset, note.startBeats + offset});
            const double to = std::min({request.bars * request.beatsPerBar, sourceTo + offset, note.startBeats + note.lengthBeats + offset});
            if (to > from) { note.startBeats = from; note.lengthBeats = to - from; harmonyInput.push_back(std::move(note)); }
        }
    }
    const auto valid = ai::validateCompositionRequest(request);
    if (!valid.valid()) {
        m_status->setText(tr("Choose a valid pitch range containing notes from the scale.")); refreshAvailability(); return;
    }
    m_jobSerial = m_serial;
    m_job = std::async(std::launch::async, [request, harmonyInput = std::move(harmonyInput), harmonyFrom, harmonyTo]() mutable {
        for (const auto& segment : mt::analyzeHarmony(harmonyInput, request.beatsPerBar, request.beatsPerBar * request.chordBars)) {
            const double from = std::max(harmonyFrom, segment.startBeats);
            const double to = std::min(harmonyTo, segment.startBeats + segment.lengthBeats);
            if (to > from && segment.root >= 0 && !segment.pitchClasses.empty())
                request.harmony.push_back({from, to - from, segment.root, segment.pitchClasses});
        }
        return ai::generateCompositionCandidates(request);
    });
    refreshAvailability();
}

void MidiGeneratorDialog::poll() {
    if (m_closed) return;
    if (m_draft) (void)m_draft->pumpPreviewPluginEvents();
    if (m_job.valid() && m_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<ai::CompositionCandidate> candidates;
        try { candidates = m_job.get(); } catch (...) { m_status->setText(tr("Generation failed. Try again.")); }
        if (m_jobSerial == m_serial && contextMatches()) {
            m_candidates = std::move(candidates);
            for (std::size_t i = 0; i < m_candidates.size() && i < m_prepared.size(); ++i) {
                mt::Notes generated;
                for (const auto& note : m_candidates[i].notes)
                    generated.push_back({{}, note.pitch, note.startBeats, note.lengthBeats, note.velocity});
                m_prepared[i] = mt::insertGeneratedNotes(m_baseNotes, std::move(generated), m_startBeats,
                    m_startBeats + m_request.bars * m_request.beatsPerBar, m_insert->currentIndex() == 1);
                for (auto& note : m_prepared[i]) if (note.id.empty()) note.id = daw::newUuid();
            }
            if (m_candidates.empty()) m_status->setText(tr("No playable variants. Widen the pitch range and generate again."));
            else selectCandidate(0);
        } else if (!m_pending) m_status->setText(tr("The project changed. Generate again to use the current clip."));
        if (m_pending && !m_debounce->isActive()) m_debounce->start(0);
    }
    if (!m_candidates.empty() && !contextMatches()) {
        stopListening(); m_candidates.clear(); m_selected = -1;
        m_status->setText(tr("The project changed. Generate again to use the current clip."));
    }
    if (m_listening && m_controller.isRecording()) stopListening();
    refreshAvailability();
}

void MidiGeneratorDialog::selectCandidate(int index) {
    if (index < 0 || index >= int(m_candidates.size())) return;
    stopListening(); m_selected = index; m_variants[index]->setChecked(true);
    const double length = m_request.bars * m_request.beatsPerBar;
    m_preview->setNotes(m_prepared[index], m_startBeats, length, m_request.beatsPerBar);
    auto text = tr("%1 bars · %2 notes · starts at beat %3").arg(m_request.bars).arg(m_candidates[index].notes.size()).arg(m_startBeats + 1, 0, 'f', 2);
    if ((m_startBeats + length) * 60.0 / m_tempo - m_clipOffset > m_clipDuration + 1e-8) text += tr(" · clip will be extended");
    const auto* track = m_controller.project().findTrack(m_trackId);
    if (track && !track->instrument.isLoaded()) text += tr(". Load an instrument on this track to listen.");
    m_status->setText(text); refreshAvailability();
}

void MidiGeneratorDialog::refreshAvailability() {
    const bool ready = m_selected >= 0 && m_selected < int(m_candidates.size()) && !m_closed;
    const auto* track = m_controller.project().findTrack(m_trackId);
    m_apply->setEnabled(ready && m_controller.sharedEditingAllowed() && !m_controller.isRecording());
    m_listen->setEnabled(ready && track && track->instrument.isLoaded() && !m_controller.isRecording());
    m_generate->setEnabled(!m_closed && target());
    const bool chords = m_mode->currentIndex() == 1;
    m_character->setEnabled(!chords); m_chordRate->setEnabled(chords || m_harmony->currentIndex() > 0);
    m_voicing->setEnabled(chords && m_harmony->currentIndex() == 0);
    m_voicing->setToolTip(m_harmony->currentIndex() > 0 ? tr("Chord tones follow the selected harmony clip.") : QString{});
    for (int i = 0; i < 3; ++i) m_variants[i]->setEnabled(i < int(m_candidates.size()));
}

void MidiGeneratorDialog::stopListening() {
    if (m_listening) m_controller.stopPluginAudition();
    m_listening = false;
    if (m_listen) { m_listen->setText(tr("Listen")); m_listen->setIcon(icons::icon(icons::Glyph::Play, th().textPrimary, 14)); }
}

void MidiGeneratorDialog::toggleListen() {
    if (m_listening) { stopListening(); return; }
    if (m_selected < 0 || !contextMatches()) { poll(); return; }
    if (!m_draft) {
        const auto result = m_controller.createPluginBatchDraft({m_trackId, {}}, m_draft);
        if (!result) { m_status->setText(tr("Could not prepare listening: %1").arg(QString::fromStdString(result.message()))); return; }
        // All document edits below belong to the disposable audition controller.
        std::vector<std::pair<std::string, std::string>> mute;
        for (const auto& track : m_draft->project().tracks) for (const auto& clip : track.clips)
            if (clip.kind != daw::ClipKind::Pattern && clip.id != m_clipId) mute.emplace_back(track.id, clip.id);
        for (const auto& [track, clip] : mute) m_draft->setClipMuted(track, clip, true);
        m_draft->setClipMuted(m_trackId, m_clipId, false);
    }
    const double end = m_startBeats + m_request.bars * m_request.beatsPerBar;
    if (!m_draft->applyGeneratedMidi(m_trackId, m_clipId, m_prepared[m_selected], end)) return;
    m_draft->setLoopRangeSeconds(m_clipStart - m_clipOffset + m_startBeats * 60.0 / m_tempo, m_clipStart - m_clipOffset + end * 60.0 / m_tempo);
    m_draft->setLoopEnabled(true); m_draft->seekSeconds(m_clipStart - m_clipOffset + m_startBeats * 60.0 / m_tempo);
    const auto result = m_controller.startPluginAudition(m_draft);
    if (!result) { m_status->setText(tr("Could not start listening: %1").arg(QString::fromStdString(result.message()))); return; }
    m_listening = true; m_listen->setText(tr("Stop")); m_listen->setIcon(icons::icon(icons::Glyph::Stop, th().textPrimary, 14));
}

void MidiGeneratorDialog::apply() {
    if (m_selected < 0 || !contextMatches()) { poll(); return; }
    stopListening();
    if (!m_controller.applyGeneratedMidi(m_trackId, m_clipId, m_prepared[m_selected], m_startBeats + m_request.bars * m_request.beatsPerBar)) {
        m_status->setText(tr("Could not apply MIDI. Check that the project is editable and recording is stopped.")); return;
    }
    emit applied(); accept();
}

void MidiGeneratorDialog::style() {
    setStyleSheet(QStringLiteral("QDialog#MidiGeneratorDialog { background: %1; color: %2; } #GeneratorStatus { color: %3; }")
        .arg(th().background.name(), th().textPrimary.name(), th().textSecondary.name()));
    m_preview->update();
    m_listen->setIcon(icons::icon(m_listening ? icons::Glyph::Stop : icons::Glyph::Play, th().textPrimary, 14));
}
