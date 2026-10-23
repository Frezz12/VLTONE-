#include "RackModulationPanel.hpp"
#include "RackModulationEditor.hpp"
#include "Controls.hpp"
#include "Theme.hpp"
#include "Icons.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {
class WaveChoice final : public QAbstractButton, public ui::graphics::ScenePaintSource {
public:
    WaveChoice(daw::modulation::Wave wave, QWidget* parent) : QAbstractButton(parent), m_wave(wave) {
        setFixedSize(32, 24); setCheckable(true); setAutoExclusive(true); setFocusPolicy(Qt::StrongFocus);
    }
    void paintScene(QPainter& p, const QRegion&) override {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(hasFocus() ? th().accent : th().separator(), 1));
        p.setBrush(mixColors(th().well(), th().accent, isChecked() ? .20 : underMouse() ? .08 : 0));
        p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 3, 3);
        QPainterPath wave;
        for (int i = 0; i <= 32; ++i) {
            const QPointF at(5. + i * (width() - 10.) / 32.,
                6. + (1. - daw::modulation::waveValue(m_wave, i / 32.01)) * (height() - 12.));
            if (i == 0) wave.moveTo(at); else wave.lineTo(at);
        }
        p.setBrush(Qt::NoBrush); p.setPen(QPen(isChecked() ? th().accent : th().textSecondary, 1.4));
        p.drawPath(wave);
    }
protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); paintScene(p, QRegion(rect())); }
private:
    daw::modulation::Wave m_wave;
};
}

QWidget* RackModulationPanel::sourceControls(const QString& id, QWidget* parent, bool detailed,
                                             std::vector<std::function<void()>>& refreshers) {
    using namespace daw::modulation;
    auto* controls = new QWidget(parent);
    auto* column = new QVBoxLayout(controls); column->setContentsMargins(0, 0, 0, 0); column->setSpacing(3);
    const auto* initial = source(id); if (!initial) return controls;
    const auto slider = [&](const QString& label, const char* name,
        std::function<double(const Source&)> get, std::function<void(Source&, double)> set,
        std::function<QString(const Source&)> format, QWidget* extra = nullptr) {
        auto* field = new QWidget(controls);
        auto* layout = new QVBoxLayout(field); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        auto* heading = new QHBoxLayout; heading->setSpacing(4);
        auto* caption = new QLabel(label, field);
        auto* value = new QLabel(field); value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto font = caption->font(); font.setPixelSize(10);
        caption->setFont(font); value->setFont(font);
        heading->addWidget(caption); heading->addWidget(value, 1);
        if (extra) heading->addWidget(extra);
        layout->addLayout(heading);
        auto* control = new ui::GlassSlider(Qt::Horizontal, field);
        control->setObjectName(QString::fromLatin1(name)); control->setProperty("sourceId", id);
        control->setRange(0, 1000); control->setSingleStep(1); control->setPageStep(25); control->setFixedHeight(22);
        control->setFocusPolicy(Qt::StrongFocus); control->setAccessibleName(label);
        caption->setBuddy(control); layout->addWidget(control); column->addWidget(field);
        if (!detailed) {
            layout->setDirection(QBoxLayout::LeftToRight); layout->setSpacing(3);
            heading->setSpacing(2); caption->setFixedWidth(29); value->setFixedWidth(35);
            field->setFixedHeight(22);
        }
        connect(control, &QSlider::sliderPressed, this, [this] { beginGesture(); });
        connect(control, &QSlider::sliderReleased, this, [this] { finishGesture(); });
        connect(control, &QSlider::valueChanged, this, [this, id, set, control](int value) {
            beginGesture(); editSource(id, [&](auto& s) { set(s, value / 1000.); });
            if (!control->isSliderDown()) finishGesture();
        });
        refreshers.push_back([this, id, control, value, get, format] {
            if (const auto* s = source(id)) {
                QSignalBlocker block(control);
                if (!control->isSliderDown()) control->setValue(int(std::round(std::clamp(get(*s), 0., 1.) * 1000)));
                const auto text = format(*s); value->setText(text); control->setToolTip(control->accessibleName() + " · " + text);
            }
        });
        return control;
    };
    const auto combo = [&](const QString& label, const char* name) {
        auto* result = new QComboBox(controls); result->setObjectName(QString::fromLatin1(name));
        result->setMinimumWidth(0); result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        result->setFixedHeight(24); result->setAccessibleName(label); result->setToolTip(label);
        column->addWidget(result); return result;
    };
    if (initial->kind == Kind::Lfo) {
        auto* waves = new QHBoxLayout; waves->setSpacing(3);
        const QStringList names{tr("Sine"), tr("Triangle"), tr("Ramp up"), tr("Ramp down"), tr("Square")};
        for (int i = 0; i < names.size(); ++i) {
            auto* button = new WaveChoice(Wave(i), controls); button->setObjectName(QStringLiteral("ModulationWave%1").arg(i));
            button->setToolTip(names[i]); button->setAccessibleName(names[i]); waves->addWidget(button);
            connect(button, &QAbstractButton::clicked, this, [this, id, i] { editSource(id, [i](auto& s) { s.wave = Wave(i); }); });
            refreshers.push_back([this, id, button, i] { if (const auto* s = source(id)) button->setChecked(int(s->wave) == i); });
        }
        waves->addStretch(); column->addLayout(waves);
    }
    if (hasRate(initial->kind)) {
        auto* sync = new QToolButton(controls); sync->setCheckable(true); sync->setFixedSize(24, 20);
        sync->setObjectName("ModulationSync"); sync->setAccessibleName(tr("Tempo sync")); sync->setToolTip(tr("Tempo sync"));
        connect(sync, &QAbstractButton::clicked, this, [this, id](bool on) { editSource(id, [on](auto& s) { s.sync = on; }); });
        refreshers.push_back([this, id, sync] { if (const auto* s = source(id)) {
            QSignalBlocker block(sync); sync->setChecked(s->sync);
            sync->setIcon(icons::icon(icons::Glyph::Metronome, s->sync ? th().accent : th().textSecondary, 15));
        } });
        auto* rate = slider(tr("Rate"), "ModulationRate",
            [](const auto& s) { return s.sync ? (7. - std::log2(s.beats)) / 12. : std::log(s.rate / .01) / std::log(2000.); },
            [](auto& s, double t) { if (s.sync) s.beats = std::exp2(7. - std::round(t * 12.)); else s.rate = std::clamp(.01 * std::pow(2000., t), .01, 20.); },
            [this](const auto& s) { return s.sync ? (s.beats >= 4 ? tr("%1 bars").arg(s.beats / 4, 0, 'g', 3)
                      : QStringLiteral("1/%1").arg(4 / s.beats, 0, 'g', 3)) : QString::number(s.rate, 'f', 2) + " Hz"; }, sync);
        rate->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(rate, &QWidget::customContextMenuRequested, this, [this, id, rate](const QPoint& at) {
            QMenu menu(this); auto* action = menu.addAction(tr("Automate rate"));
            if (menu.exec(rate->mapToGlobal(at)) == action) automate(id.toStdString(), false);
        });
        if (detailed) {
            auto* period = combo(tr("Period"), "ModulationPeriod");
            for (const double beats : {128., 64., 32., 16., 8., 4., 2., 1., .5, .25, .125, .0625, .03125}) {
                const auto label = beats >= 4 ? tr("%1 bars").arg(beats / 4) : QStringLiteral("1/%1").arg(4 / beats);
                period->addItem(label, beats);
                if (beats * 1.5 <= 128) period->addItem(label + " ·", beats * 1.5);
                if (beats * 2 / 3 >= .03125) period->addItem(label + " T", beats * 2 / 3);
            }
            connect(period, &QComboBox::activated, this, [this, id, period] { editSource(id, [&](auto& s) { s.beats = period->currentData().toDouble(); }); });
            refreshers.push_back([this, id, period] { if (const auto* s = source(id)) {
                QSignalBlocker block(period); int at = -1;
                for (int i = 0; i < period->count(); ++i) if (std::abs(period->itemData(i).toDouble() - s->beats) < 1e-9) { at = i; break; }
                if (at < 0) { period->addItem(tr("%1 beats").arg(s->beats), s->beats); at = period->count() - 1; }
                period->setCurrentIndex(at); period->setVisible(s->sync);
            } });
        }
        if (initial->kind == Kind::Lfo || detailed)
            slider(tr("Phase"), "ModulationPhase", [](const auto& s) { return s.phase; },
                [](auto& s, double value) { s.phase = value; },
                [](const auto& s) { return QString::number(s.phase * 360, 'f', 0) + QChar(0x00b0); });
        if (detailed)
            slider(tr("Smoothing"), "ModulationSmoothing", [](const auto& s) { return std::log1p(s.smoothMs) / std::log(1001.); },
                [](auto& s, double value) { s.smoothMs = std::clamp(std::expm1(value * std::log(1001.)), 0., 1000.); },
                [](const auto& s) { return QString::number(s.smoothMs, 'f', 1) + " ms"; });
    }
    if (initial->kind == Kind::Follower) {
        auto* audio = combo(tr("Audio source"), "ModulationAudioSource"); audio->addItem(tr("Own input"), "");
        for (const auto& track : m_controller->project().tracks)
            if (daw::carriesAudio(track) && track.id != m_channel.toStdString())
                audio->addItem(QString::fromStdString(track.name), QString::fromStdString(track.id));
        if (m_channel != "master") audio->addItem(tr("Master"), "master");
        connect(audio, &QComboBox::activated, this, [this, id, audio] { editSource(id, [&](auto& s) { s.audioChannelId = audio->currentData().toString().toStdString(); }); });
        refreshers.push_back([this, id, audio] { if (const auto* s = source(id)) {
            QSignalBlocker block(audio); const auto channel = QString::fromStdString(s->audioChannelId);
            int at = audio->findData(channel); if (at < 0) { audio->addItem(tr("Missing audio source"), channel); at = audio->count() - 1; }
            audio->setCurrentIndex(at);
        } });
        slider(tr("Gain"), "ModulationGain", [](const auto& s) { return (s.gainDb + 48.) / 96.; },
            [](auto& s, double t) { s.gainDb = t * 96. - 48.; }, [](const auto& s) { return QString::number(s.gainDb, 'f', 1) + " dB"; });
        slider(tr("Attack"), "ModulationAttack", [](const auto& s) { return std::log(s.attackMs / .1) / std::log(20000.); },
            [](auto& s, double t) { s.attackMs = std::clamp(.1 * std::pow(20000., t), .1, 2000.); }, [](const auto& s) { return QString::number(s.attackMs, 'f', 1) + " ms"; });
        slider(tr("Release"), "ModulationRelease", [](const auto& s) { return std::log(s.releaseMs) / std::log(10000.); },
            [](auto& s, double t) { s.releaseMs = std::clamp(std::pow(10000., t), 1., 10000.); }, [](const auto& s) { return QString::number(s.releaseMs, 'f', 0) + " ms"; });
    }
    if (initial->kind == Kind::Shaper) {
        auto* preset = combo(tr("Shape presets"), "ModulationShapePreset");
        preset->addItems({tr("Ramp up"), tr("Ramp down"), tr("S-curve"), tr("Pulse"), tr("Ducking")});
        preset->setCurrentIndex(-1); preset->setPlaceholderText(tr("Shape…"));
        connect(preset, &QComboBox::activated, this, [this, id, preset](int i) {
            editSource(id, [i](auto& s) { shapePreset(s, unsigned(i)); }); QSignalBlocker block(preset); preset->setCurrentIndex(-1);
        });
    }
    if (initial->kind == Kind::Steps) {
        auto* count = combo(tr("Steps"), "ModulationStepCount");
        for (int i = 1; i <= 32; ++i) count->addItem(tr("%1 steps").arg(i), i);
        connect(count, &QComboBox::activated, this, [this, id, count] { editSource(id, [&](auto& s) { s.steps.resize(count->currentData().toUInt(), .5); }); });
        auto* direction = combo(tr("Direction"), "ModulationDirection");
        direction->addItems({tr("Forward"), tr("Reverse"), tr("Ping pong")});
        connect(direction, &QComboBox::activated, this, [this, id](int i) { editSource(id, [i](auto& s) { s.direction = Direction(i); }); });
        refreshers.push_back([this, id, count, direction] { if (const auto* s = source(id)) {
            QSignalBlocker a(count), b(direction); count->setCurrentIndex(int(s->steps.size()) - 1); direction->setCurrentIndex(int(s->direction));
        } });
        auto* pattern = new QToolButton(controls); pattern->setText(tr("Pattern…")); pattern->setObjectName("ModulationPattern");
        column->addWidget(pattern);
        connect(pattern, &QAbstractButton::clicked, this, [this, id] {
            QMenu menu(this); auto* invert = menu.addAction(tr("Invert"));
            auto* left = menu.addAction(tr("Shift left")); auto* right = menu.addAction(tr("Shift right"));
            auto* generate = menu.addAction(tr("Euclidean…")); const auto action = menu.exec(QCursor::pos());
            if (!action) return;
            if (action == generate) {
                const auto* value = source(id); if (!value) return;
                const int count = int(value->steps.size()); QDialog dialog(this); dialog.setWindowTitle(tr("Euclidean pattern"));
                auto* form = new QFormLayout(&dialog); auto* pulses = new QSpinBox(&dialog); pulses->setRange(0, count); pulses->setValue(std::min(5, count));
                auto* rotation = new QSpinBox(&dialog); rotation->setRange(0, count - 1);
                form->addRow(tr("Pulses"), pulses); form->addRow(tr("Rotation"), rotation);
                auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); form->addRow(buttons);
                connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                if (dialog.exec() == QDialog::Accepted) editSource(id, [&](auto& s) { euclideanSteps(s, pulses->value(), rotation->value()); });
            } else editSource(id, [&](auto& s) {
                if (action == invert) for (auto& v : s.steps) v = 1 - v;
                else if (action == left) std::rotate(s.steps.begin(), s.steps.begin() + 1, s.steps.end());
                else if (action == right) std::rotate(s.steps.rbegin(), s.steps.rbegin() + 1, s.steps.rend());
            });
        });
    }
    if (initial->kind == Kind::Random) {
        auto* mode = combo(tr("Random mode"), "ModulationRandomMode"); mode->addItems({tr("Stepped"), tr("Smooth")});
        auto* loop = combo(tr("Repeat pattern"), "ModulationRepeat"); loop->addItem(tr("No repeat"), 0);
        for (int n : {2, 4, 8, 16, 32}) loop->addItem(tr("%1 steps").arg(n), n);
        connect(mode, &QComboBox::activated, this, [this, id](int i) { editSource(id, [i](auto& s) { s.randomSmooth = i == 1; }); });
        connect(loop, &QComboBox::activated, this, [this, id, loop] { editSource(id, [&](auto& s) { s.randomLoop = loop->currentData().toUInt(); }); });
        refreshers.push_back([this, id, mode, loop] { if (const auto* s = source(id)) {
            QSignalBlocker a(mode), b(loop); mode->setCurrentIndex(s->randomSmooth ? 1 : 0); loop->setCurrentIndex(loop->findData(int(s->randomLoop)));
        } });
        auto* reseed = new QToolButton(controls); reseed->setText(tr("New pattern")); reseed->setObjectName("ModulationReseed");
        column->addWidget(reseed);
        connect(reseed, &QAbstractButton::clicked, this, [this, id] { editSource(id, [](auto& s) { s.seed = QRandomGenerator::global()->generate(); }); });
    }
    if (detailed) {
        auto* settings = new QToolButton(controls); settings->setText(tr("Source settings")); settings->setMinimumHeight(24);
        column->addWidget(settings);
        connect(settings, &QAbstractButton::clicked, this, [this, id] { advanced(id); });
    }
    return controls;
}

void RackModulationPanel::addMenu() {
    QMenu menu(this);
    for (const auto kind : {daw::modulation::Kind::Lfo, daw::modulation::Kind::Follower,
            daw::modulation::Kind::Shaper, daw::modulation::Kind::Steps, daw::modulation::Kind::Random}) {
        auto* action = menu.addAction(QCoreApplication::translate("RackWidget", daw::modulation::kindName(kind)));
        connect(action, &QAction::triggered, this, [this, kind] { addSource(kind); });
    }
    menu.exec(QCursor::pos());
}

void RackModulationPanel::rebuild() {
    finishEdits();
    m_refreshers.clear();
    while (auto* item = m_layout->takeAt(0)) { delete item->widget(); delete item; }
    m_structureRevision = m_controller->project().structureRevision();
    const auto* chosen = source(m_selected);
    if (!chosen || !includes(*chosen)) {
        m_assigning = false; m_selected.clear(); m_route.clear();
        for (const auto& s : m_shown.sources) if (includes(s)) { m_selected = QString::fromStdString(s.id); break; }
    }
    QSettings().setValue(key("selected"), m_selected);
    int totalWidth = 0;
    for (const auto& initial : m_shown.sources) {
        if (!includes(initial)) continue;
        const auto id = QString::fromStdString(initial.id);
        auto* card = new QFrame(this); card->setObjectName("RackModulatorCard"); card->setProperty("sourceId", id);
        card->setFocusPolicy(Qt::StrongFocus); card->setFixedWidth(210);
        totalWidth += 210 + (totalWidth ? 8 : 0);
        auto* column = new QVBoxLayout(card); column->setContentsMargins(6, 3, 6, 3); column->setSpacing(3);
        auto* header = new QHBoxLayout; header->setSpacing(1); column->addLayout(header);
        auto* power = new ui::IconButton(icons::Glyph::Power, tr("Enable source"), card);
        power->setButtonSize(24, 24); power->setCheckable(true); header->addWidget(power);
        connect(power, &QAbstractButton::clicked, this, [this, id](bool on) { editSource(id, [on](auto& s) { s.enabled = on; }); });
        auto* title = new QToolButton(card); title->setObjectName("ModulationSourceTitle"); title->setProperty("sourceId", id);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); title->setMinimumHeight(24);
        title->setFocusPolicy(Qt::StrongFocus); header->addWidget(title, 1);
        connect(title, &QAbstractButton::clicked, this, [this, id] {
            setAssigning(false); m_selected = id; QSettings().setValue(key("selected"), id); synchronize(); highlight();
        });
        title->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(title, &QWidget::customContextMenuRequested, this, [this, id, title](const QPoint& at) { sourceMenu(id, title->mapToGlobal(at)); });
        auto* assign = new QToolButton(card); assign->setObjectName("ModulationAssign"); assign->setProperty("sourceId", id);
        assign->setFixedSize(24, 24); assign->setCheckable(true); assign->setFocusPolicy(Qt::StrongFocus);
        assign->setAccessibleName(tr("Assign modulation")); header->addWidget(assign);
        connect(assign, &QAbstractButton::clicked, this, [this, id] {
            const bool on = m_assigning && m_selected == id; m_selected = id; setAssigning(!on); synchronize();
        });
        auto* expand = new ui::IconButton(icons::Glyph::WindowMaximize, tr("Enlarge editor"), card);
        expand->setObjectName("ModulationExpand"); expand->setButtonSize(24, 24); expand->setFocusPolicy(Qt::StrongFocus);
        header->addWidget(expand); connect(expand, &QAbstractButton::clicked, this, [this, id] { expandEditor(id); });
        auto* graph = new RackModulationEditor(card); graph->setSource(initial); graph->setProperty("sourceId", id);
        if (initial.kind != daw::modulation::Kind::Shaper) {
            graph->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(graph, &QWidget::customContextMenuRequested, this, [this, id, graph](const QPoint& at) { sourceMenu(id, graph->mapToGlobal(at)); });
        }
        graph->setMinimumHeight(48);
        graph->setMaximumHeight(112); column->addWidget(graph, 1);
        graph->begin = [this] { beginGesture(); }; graph->finish = [this] { finishGesture(); };
        graph->changed = [this, id](const auto& next) { editSource(id, [&](auto& s) { s.points = next.points; s.steps = next.steps; }); };
        column->addWidget(sourceControls(id, card, false, m_refreshers));
        column->addStretch();
        m_refreshers.push_back([this, id, card, title, power, assign, graph] {
            const auto* s = source(id); if (!s) return;
            QSignalBlocker a(power), b(assign); power->setChecked(s->enabled);
            const auto name = QString::fromStdString(s->name);
            title->setText(title->fontMetrics().elidedText(name, Qt::ElideRight, std::max(18, title->width() - 4)));
            title->setToolTip(name); title->setAccessibleName(name); card->setAccessibleName(name);
            assign->setChecked(m_assigning && m_selected == id);
            assign->setIcon(icons::icon(icons::Glyph::RackAssign, assign->isChecked() ? th().accent : th().textSecondary, 17));
            assign->setToolTip(assign->isChecked() ? tr("Click a parameter · Esc cancels") : tr("Assign modulation"));
            const auto style = QString("#RackModulatorCard{background:%1;border:1px solid %2;border-radius:6px;}")
                .arg(th().well().name(), (m_selected == id ? mixColors(th().separator(), th().accent, .5) : th().separator()).name());
            if (card->styleSheet() != style) card->setStyleSheet(style);
            graph->setSource(*s);
        });
        m_layout->addWidget(card);
    }
    if (!m_slot.isEmpty()) {
        auto* add = new QToolButton(this); add->setObjectName("RackAddModulation"); add->setFixedWidth(28);
        add->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding); add->setMinimumHeight(120);
        add->setIcon(icons::icon(icons::Glyph::Plus, th().textSecondary, 16));
        add->setStyleSheet(QString("#RackAddModulation{background:%1;border:1px dashed %2;border-radius:4px;}"
                                  "#RackAddModulation:hover{border:1px solid %3;}")
            .arg(th().well().name(), th().separator().name(), th().accent.name()));
        add->setToolTip(tr("Add modulation")); add->setAccessibleName(tr("Add modulation"));
        add->setFocusPolicy(Qt::StrongFocus); m_layout->addWidget(add);
        connect(add, &QAbstractButton::clicked, this, &RackModulationPanel::addMenu);
        totalWidth += 28 + (totalWidth ? 8 : 0);
    }
    setFixedWidth(totalWidth); setVisible(totalWidth > 0 && !m_controller->hasCloudProjectBinding());
    synchronize(); highlight(); emit layoutChanged();
}
