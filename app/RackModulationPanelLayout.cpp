#include "RackModulationPanel.hpp"
#include "RackModulationEditor.hpp"
#include "Controls.hpp"
#include "Theme.hpp"
#include "Icons.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
class CompactNumber final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;
    QString textFromValue(double value) const override {
        auto text = QDoubleSpinBox::textFromValue(value);
        const auto decimal = locale().decimalPoint();
        if (text.contains(decimal)) {
            while (text.endsWith('0')) text.chop(1);
            if (text.endsWith(decimal)) text.chop(decimal.size());
        }
        return text;
    }
};
class ElidedButton final : public QToolButton {
public:
    using QToolButton::QToolButton;
    void setLabel(const QString& text) { full = text; setAccessibleName(text); setToolTip(text); fit(); }
protected:
    void resizeEvent(QResizeEvent* e) override { QToolButton::resizeEvent(e); fit(); }
private:
    void fit() { setText(fontMetrics().elidedText(full, Qt::ElideRight, std::max(8, width() - 8))); }
    QString full;
};
QToolButton* button(const QString& text, QWidget* parent, const std::function<void()>& run) {
    auto* b = new QToolButton(parent); b->setText(text); b->setAccessibleName(text); b->setToolTip(text);
    b->setMinimumHeight(26); b->setFocusPolicy(Qt::StrongFocus);
    QObject::connect(b, &QAbstractButton::clicked, parent, run); return b;
}
}

void RackModulationPanel::rebuild() {
    using namespace daw::modulation;
    finishGesture();
    QString focusedSource;
    for (auto* widget = QApplication::focusWidget(); widget && widget != this; widget = widget->parentWidget())
        if (widget->objectName() == "RackModulatorCard" && isAncestorOf(widget)) { focusedSource = widget->property("sourceId").toString(); break; }
    QHash<QString, int> scrolls;
    for (auto* area : findChildren<QScrollArea*>("ModulationAssignments"))
        scrolls.insert(area->property("sourceId").toString(), area->verticalScrollBar()->value());
    m_refreshers.clear();
    while (auto* item = m_layout->takeAt(0)) { delete item->widget(); delete item; }
    m_structureRevision = m_controller->project().structureRevision();
    if (!source(m_selected)) {
        m_assigning = false; m_route.clear();
        m_selected = m_shown.sources.empty() ? QString{} : QString::fromStdString(m_shown.sources.front().id);
    }
    QSettings().setValue(key("selected"), m_selected);
    int totalWidth = 0;
    for (const auto& initial : m_shown.sources) {
        const auto id = QString::fromStdString(initial.id);
        const bool collapsed = QSettings().value(key("collapsed/") + id, false).toBool();
        const auto count = std::count_if(m_shown.routes.begin(), m_shown.routes.end(), [&](const auto& r) { return r.sourceId == initial.id; });
        auto* card = new QFrame(this); card->setObjectName("RackModulatorCard"); card->setProperty("sourceId", id);
        card->setFocusPolicy(Qt::StrongFocus); card->setFixedWidth(collapsed ? 170 : 560); card->setMinimumHeight(220);
        totalWidth += card->width() + (totalWidth ? 8 : 0);
        auto* column = new QVBoxLayout(card); column->setContentsMargins(8, 5, 8, 7); column->setSpacing(5);
        auto* header = new QHBoxLayout; header->setSpacing(4); column->addLayout(header);
        auto* on = new ui::IconButton(icons::Glyph::Power, tr("Enable %1").arg(QString::fromStdString(initial.name)), card);
        on->setButtonSize(24, 24); on->setCheckable(true); on->setFocusPolicy(Qt::StrongFocus); header->addWidget(on);
        connect(on, &QAbstractButton::clicked, this, [this, id](bool v) { editSource(id, [&](auto& s) { s.enabled = v; }); });
        auto* title = new ElidedButton(card); title->setMinimumHeight(26); title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        title->setFocusPolicy(Qt::StrongFocus); title->setObjectName("ModulationSourceTitle"); title->setProperty("sourceId", id); header->addWidget(title, 1);
        connect(title, &QAbstractButton::clicked, this, [this, id] { setAssigning(false); m_selected = id; QSettings().setValue(key("selected"), id); synchronize(); });
        title->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(title, &QWidget::customContextMenuRequested, this, [this, id, title](const QPoint& at) { sourceMenu(id, title->mapToGlobal(at)); });
        auto* menu = button(QStringLiteral("…"), card, [this, id] {
            if (auto* trigger = qobject_cast<QWidget*>(sender())) sourceMenu(id, trigger->mapToGlobal(QPoint(0, trigger->height())));
        });
        menu->setFixedSize(26, 26); menu->setAccessibleName(tr("Module menu")); menu->setToolTip(tr("Module menu")); header->addWidget(menu);
        auto* fold = new ui::IconButton(collapsed ? icons::Glyph::ChevronRight : icons::Glyph::Chevron, tr("Expand or collapse module"), card);
        fold->setButtonSize(24, 24); fold->setObjectName("ModulationFold"); fold->setProperty("sourceId", id); fold->setFocusPolicy(Qt::StrongFocus); header->addWidget(fold);
        connect(fold, &QAbstractButton::clicked, this, [this, id, collapsed] { QSettings().setValue(key("collapsed/") + id, !collapsed); setAssigning(false); rebuild(); });
        m_refreshers.push_back([this, id, card, title, on] {
            const auto* s = source(id); if (!s) return;
            QSignalBlocker block(on); on->setChecked(s->enabled);
            title->setLabel(QString::fromStdString(s->name)); card->setAccessibleName(QString::fromStdString(s->name));
            const auto style = "#RackModulatorCard{background:" + th().surface.name() + ";border:1px solid " +
                (id == m_selected ? th().accent : th().separator()).name() + ";border-radius:7px;}";
            if (card->styleSheet() != style) card->setStyleSheet(style);
        });
        auto* body = new QHBoxLayout; body->setSpacing(8); column->addLayout(body, 1);
        auto* controls = new QWidget(card); controls->setFixedWidth(collapsed ? 154 : 220); body->addWidget(controls);
        auto* left = new QVBoxLayout(controls); left->setContentsMargins(0, 0, 0, 0); left->setSpacing(4);
        auto* graph = new RackModulationEditor(controls); graph->setSource(initial); graph->setProperty("sourceId", id); left->addWidget(graph, 1);
        graph->begin = [this] { beginGesture(); }; graph->finish = [this] { finishGesture(); };
        graph->changed = [this, id](const Source& next) { editSource(id, [&](auto& s) { s.points = next.points; s.steps = next.steps; }); };
        m_refreshers.push_back([this, id, graph] { if (const auto* s = source(id)) graph->setSource(*s); });
        if (collapsed) {
            graph->setMaximumHeight(84); left->addWidget(new QLabel(tr("Assignments: %1").arg(count), controls)); left->addStretch();
            m_layout->addWidget(card); continue;
        }
        const auto row = [&] { auto* r = new QHBoxLayout; r->setSpacing(4); left->addLayout(r); return r; };
        const auto combo = [&](QHBoxLayout* r, const QString& label) {
            auto* box = new QComboBox(controls); box->setMinimumSize(0, 24); box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            box->setStyleSheet("QComboBox{padding:2px 5px;min-height:18px;} QComboBox::drop-down{width:18px;}");
            box->setAccessibleName(label); box->setToolTip(label); r->addWidget(box, 1); return box;
        };
        const auto number = [&](QHBoxLayout* r, const QString& label, double low, double high, const QString& suffix,
                                std::function<double(const Source&)> get, std::function<void(Source&, double)> set, bool caption = true) {
            auto* field = new QWidget(controls); auto* box = new QVBoxLayout(field); box->setContentsMargins(0, 0, 0, 0); box->setSpacing(1);
            auto* spin = new CompactNumber(field); spin->setRange(low, high); spin->setDecimals(2); spin->setSuffix(suffix);
            spin->setKeyboardTracking(false); spin->setMinimumSize(0, 24); spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons); spin->setAccessibleName(label); spin->setToolTip(label);
            if (caption) { auto* text = new QLabel(label, field); text->setBuddy(spin); box->addWidget(text); }
            box->addWidget(spin); r->addWidget(field, 1);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, id, set](double value) { editSource(id, [&](auto& s) { set(s, value); }); });
            m_refreshers.push_back([this, id, spin, get] { if (const auto* s = source(id)) { QSignalBlocker block(spin); if (spin->value() != get(*s)) spin->setValue(get(*s)); } });
            return spin;
        };
        if (initial.kind == Kind::Lfo) {
            auto* shape = combo(row(), tr("Waveform")); shape->addItems({tr("Sine"), tr("Triangle"), tr("Ramp up"), tr("Ramp down"), tr("Square")});
            connect(shape, &QComboBox::activated, this, [this, id](int i) { editSource(id, [&](auto& s) { s.wave = Wave(i); }); });
            m_refreshers.push_back([this, id, shape] { if (const auto* s = source(id)) { QSignalBlocker block(shape); shape->setCurrentIndex(int(s->wave)); } });
        }
        if (hasRate(initial.kind)) {
            auto* rateRow = row(); auto* period = combo(rateRow, initial.kind == Kind::Steps || initial.kind == Kind::Random ? tr("Step duration") : tr("Period"));
            for (const double beats : {128., 64., 32., 16., 8., 4., 2., 1., .5, .25, .125, .0625, .03125}) {
                const QString label = beats >= 4 ? tr("%1 bars").arg(beats / 4) : "1/" + QString::number(4 / beats);
                period->addItem(label, beats);
                if (beats * 1.5 <= 128) period->addItem(label + " ·", beats * 1.5);
                if (beats * 2 / 3 >= .03125) period->addItem(label + " T", beats * 2 / 3);
            }
            period->setObjectName("ModulationPeriod");
            connect(period, &QComboBox::activated, this, [this, id, period] { editSource(id, [&](auto& s) { s.beats = period->currentData().toDouble(); }); });
            auto* hz = number(rateRow, tr("Rate"), .01, 20, " Hz", [](const auto& s) { return s.rate; }, [](auto& s, double v) { s.rate = v; }, false);
            auto* mode = combo(rateRow, tr("Rate mode")); mode->addItems({tr("Sync"), tr("Hz")});
            mode->ensurePolished(); mode->setFixedWidth(std::max(70, mode->sizeHint().width()));
            connect(mode, &QComboBox::activated, this, [this, id](int i) { editSource(id, [&](auto& s) { s.sync = i == 0; }); });
            m_refreshers.push_back([this, id, period, hz, mode] {
                if (const auto* s = source(id)) {
                    QSignalBlocker p(period), m(mode); int index = -1;
                    for (int i = 0; i < period->count(); ++i) if (std::abs(period->itemData(i).toDouble() - s->beats) < 1e-9) { index = i; break; }
                    if (index < 0) { period->addItem(tr("%1 beats").arg(s->beats, 0, 'g', 4), s->beats); index = period->count() - 1; }
                    period->setCurrentIndex(index); period->setVisible(s->sync); hz->parentWidget()->setVisible(!s->sync); mode->setCurrentIndex(s->sync ? 0 : 1);
                }
            });
            for (auto* control : {static_cast<QWidget*>(period), static_cast<QWidget*>(hz)}) {
                control->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(control, &QWidget::customContextMenuRequested, this, [this, id, control](const QPoint& at) {
                    QMenu menu(this); auto* action = menu.addAction(tr("Automate rate"));
                    if (menu.exec(control->mapToGlobal(at)) == action) automate(id.toStdString(), false);
                });
            }
        }
        if (initial.kind == Kind::Follower) {
            auto* audioRow = row(); auto* audio = combo(audioRow, tr("Audio source")); audio->addItem(tr("Own input"), "");
            for (const auto& track : m_controller->project().tracks) if (daw::carriesAudio(track) && track.id != m_channel.toStdString())
                audio->addItem(QString::fromStdString(track.name), QString::fromStdString(track.id));
            if (m_channel != "master") audio->addItem(tr("Master"), "master");
            connect(audio, &QComboBox::activated, this, [this, id, audio] { editSource(id, [&](auto& s) { s.audioChannelId = audio->currentData().toString().toStdString(); }); });
            m_refreshers.push_back([this, id, audio] {
                if (const auto* s = source(id)) {
                    QSignalBlocker a(audio); const auto channel = QString::fromStdString(s->audioChannelId); int index = audio->findData(channel);
                    if (index < 0) { audio->addItem(tr("Missing audio source"), channel); index = audio->count() - 1; }
                    audio->setCurrentIndex(index); audio->setToolTip(tr("Audio source") + "\n" + audio->currentText());
                }
            });
            auto* dynamics = row();
            number(dynamics, tr("Gain"), -48, 48, " dB", [](const auto& s) { return s.gainDb; }, [](auto& s, double v) { s.gainDb = v; });
            number(dynamics, tr("Attack"), .1, 2000, " ms", [](const auto& s) { return s.attackMs; }, [](auto& s, double v) { s.attackMs = v; });
            number(dynamics, tr("Release"), 1, 10000, " ms", [](const auto& s) { return s.releaseMs; }, [](auto& s, double v) { s.releaseMs = v; });
        }
        if (initial.kind == Kind::Steps) {
            auto* sequence = row();
            auto* length = number(sequence, tr("Steps"), 1, 32, "", [](const auto& s) { return double(s.steps.size()); }, [](auto& s, double v) { s.steps.resize(std::size_t(v), .5); }, false);
            length->setDecimals(0); length->parentWidget()->setMaximumWidth(56); length->setObjectName("ModulationStepCount");
            auto* direction = combo(sequence, tr("Direction")); direction->addItems({tr("Forward"), tr("Reverse"), tr("Ping pong")});
            connect(direction, &QComboBox::activated, this, [this, id](int i) { editSource(id, [&](auto& s) { s.direction = Direction(i); }); });
            m_refreshers.push_back([this, id, direction] { if (const auto* s = source(id)) { QSignalBlocker block(direction); direction->setCurrentIndex(int(s->direction)); } });
        }
        if (initial.kind == Kind::Random) {
            auto* options = row(); auto* mode = combo(options, tr("Random mode")); mode->addItems({tr("Stepped"), tr("Smooth")});
            auto* loop = combo(options, tr("Repeat pattern")); loop->addItem(tr("No repeat"), 0);
            for (int n : {2, 4, 8, 16, 32}) loop->addItem(tr("%1 steps").arg(n), n);
            connect(mode, &QComboBox::activated, this, [this, id](int i) { editSource(id, [&](auto& s) { s.randomSmooth = i == 1; }); });
            connect(loop, &QComboBox::activated, this, [this, id, loop] { editSource(id, [&](auto& s) { s.randomLoop = loop->currentData().toUInt(); }); });
            m_refreshers.push_back([this, id, mode, loop] { if (const auto* s = source(id)) {
                QSignalBlocker a(mode), b(loop); mode->setCurrentIndex(s->randomSmooth ? 1 : 0); loop->setCurrentIndex(loop->findData(int(s->randomLoop)));
            } });
        }
        auto* actions = row();
        if (initial.kind == Kind::Shaper) {
            auto* presets = combo(actions, tr("Shape presets")); presets->setObjectName("ModulationShapePreset");
            presets->addItems({tr("Ramp up"), tr("Ramp down"), tr("S-curve"), tr("Pulse"), tr("Ducking")});
            presets->setCurrentIndex(-1); presets->setPlaceholderText(tr("Shape…"));
            connect(presets, &QComboBox::activated, this, [this, id, presets](int i) {
                editSource(id, [&](auto& s) { shapePreset(s, unsigned(i)); }); QSignalBlocker block(presets); presets->setCurrentIndex(-1);
            });
        } else if (initial.kind == Kind::Steps) {
            auto* pattern = button(tr("Pattern…"), controls, [this, id] {
                QMenu menu(this); auto* invert = menu.addAction(tr("Invert")); auto* left = menu.addAction(tr("Shift left")); auto* right = menu.addAction(tr("Shift right"));
                auto* generate = menu.addAction(tr("Euclidean…")); const auto action = menu.exec(QCursor::pos());
                if (!action) return;
                if (action == generate) {
                    const auto* value = source(id); if (!value) return;
                    const int count = int(value->steps.size()); QDialog dialog(this); dialog.setWindowTitle(tr("Euclidean pattern")); auto* form = new QFormLayout(&dialog);
                    auto* pulses = new QSpinBox(&dialog); pulses->setRange(0, count); pulses->setValue(std::min(5, count));
                    auto* rotation = new QSpinBox(&dialog); rotation->setRange(0, count - 1);
                    form->addRow(tr("Pulses"), pulses); form->addRow(tr("Rotation"), rotation);
                    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); form->addRow(buttons);
                    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                    if (dialog.exec() == QDialog::Accepted) editSource(id, [&](auto& s) { euclideanSteps(s, pulses->value(), rotation->value()); });
                } else editSource(id, [&](auto& s) {
                    if (action == invert) for (auto& v : s.steps) v = 1 - v;
                    else if (action == left) std::rotate(s.steps.begin(), s.steps.begin() + 1, s.steps.end());
                    else std::rotate(s.steps.rbegin(), s.steps.rbegin() + 1, s.steps.rend());
                });
            });
            pattern->setObjectName("ModulationPattern"); actions->addWidget(pattern, 1);
        } else if (initial.kind == Kind::Random) {
            auto* reseed = button(tr("New pattern"), controls, [this, id] { editSource(id, [](auto& s) {
                auto seed = QRandomGenerator::global()->generate(); if (seed == s.seed) ++seed; s.seed = seed;
            }); });
            reseed->setObjectName("ModulationReseed"); actions->addWidget(reseed, 1);
            m_refreshers.push_back([this, id, reseed] { if (const auto* s = source(id)) reseed->setToolTip(tr("Seed: %1. Saved with the project.").arg(s->seed)); });
        } else actions->addStretch();
        if (initial.kind == Kind::Shaper || initial.kind == Kind::Steps) {
            auto* expand = new ui::IconButton(icons::Glyph::WindowMaximize, tr("Enlarge editor"), controls); expand->setButtonSize(26, 26);
            expand->setObjectName("ModulationExpand"); expand->setFocusPolicy(Qt::StrongFocus); actions->addWidget(expand);
            connect(expand, &QAbstractButton::clicked, this, [this, id] { expandEditor(id); });
        }
        auto* advancedButton = new ui::IconButton(icons::Glyph::Gear, tr("Source settings"), controls); advancedButton->setButtonSize(26, 26);
        advancedButton->setFocusPolicy(Qt::StrongFocus); actions->addWidget(advancedButton);
        connect(advancedButton, &QAbstractButton::clicked, this, [this, id] { advanced(id); });
        auto* divider = new QFrame(card); divider->setFrameShape(QFrame::VLine); body->addWidget(divider);
        auto* targets = new QWidget(card); body->addWidget(targets, 1);
        auto* right = new QVBoxLayout(targets); right->setContentsMargins(0, 0, 0, 0); right->setSpacing(3);
        auto* toolbar = new QHBoxLayout; toolbar->setSpacing(3); right->addLayout(toolbar);
        toolbar->addWidget(new QLabel(tr("Connections %1").arg(count), targets), 1);
        auto* assignButton = button(tr("Assign"), targets, [this, id] {
            const bool active = m_assigning && m_selected == id; m_selected = id; setAssigning(!active); synchronize();
        });
        assignButton->setObjectName("ModulationAssign"); assignButton->setProperty("sourceId", id); toolbar->addWidget(assignButton);
        m_refreshers.push_back([this, id, assignButton] { assignButton->setText(m_assigning && m_selected == id ? tr("Cancel · Esc") : tr("Assign")); });
        auto* parameters = button(QStringLiteral("…"), targets, [this, id] { m_selected = id; chooseParameter(); synchronize(); });
        parameters->setFixedWidth(26); parameters->setObjectName("ModulationParameters"); parameters->setAccessibleName(tr("Choose parameter")); parameters->setToolTip(tr("Choose parameter")); toolbar->addWidget(parameters);
        auto* assignments = new QWidget; auto* list = new QVBoxLayout(assignments); list->setContentsMargins(0, 0, 0, 0); list->setSpacing(0);
        for (const auto& route : m_shown.routes) if (route.sourceId == initial.id) {
            const auto routeId = QString::fromStdString(route.id);
            auto* group = new QWidget(assignments); group->setObjectName("ModulationRoute"); group->setProperty("routeId", routeId); group->setFixedHeight(28);
            auto* line = new QHBoxLayout(group); line->setContentsMargins(0, 1, 0, 1); line->setSpacing(3);
            const auto edit = [this, routeId](const std::function<void(Route&)>& fn) { change([&](auto& rack) { for (auto& r : rack.routes) if (r.id == routeId.toStdString()) fn(r); }); };
            auto* enabled = new ui::IconButton(icons::Glyph::Power, tr("Enable assignment"), group); enabled->setButtonSize(24, 24); enabled->setCheckable(true); enabled->setFocusPolicy(Qt::StrongFocus); line->addWidget(enabled);
            connect(enabled, &QAbstractButton::clicked, this, [edit](bool v) { edit([&](auto& r) { r.enabled = v; }); });
            const auto* slot = m_controller->insertModel(m_channel.toStdString(), route.slotId);
            QString name = QString::fromStdString(slot ? slot->name : route.pluginUid) + " → " + QString::fromStdString(route.parameterId);
            for (const auto& info : m_controller->insertParameters(m_channel.toStdString(), route.slotId))
                if (info.id == route.parameterId) name = QString::fromStdString(slot ? slot->name : route.pluginUid) + " → " + QString::fromStdString(info.name);
            auto* label = new ElidedButton(group); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); label->setMinimumHeight(24); label->setFocusPolicy(Qt::StrongFocus); line->addWidget(label, 1);
            connect(label, &QAbstractButton::clicked, this, [this, id, routeId] { m_selected = id; m_route = routeId; synchronize(); highlight(); });
            auto* depth = new CompactNumber(group); depth->setRange(-100, 100); depth->setDecimals(1); depth->setSuffix("%"); depth->setButtonSymbols(QAbstractSpinBox::NoButtons);
            depth->setKeyboardTracking(false); depth->setFixedWidth(66); depth->setMinimumHeight(24); depth->setAccessibleName(tr("Depth")); depth->setObjectName("ModulationDepth");
            depth->setToolTip(tr("Maximum deviation as a percentage of the parameter range. Negative values reverse the movement.")); line->addWidget(depth);
            connect(depth, &QDoubleSpinBox::valueChanged, this, [edit](double value) { edit([&](auto& r) { r.depth = value / 100; }); });
            auto* polarity = button(QStringLiteral("±"), group, [] {}); polarity->setCheckable(true); polarity->setFixedSize(26, 24); polarity->setAccessibleName(tr("Two-sided movement"));
            polarity->setToolTip(tr("One-sided starts at the base; two-sided moves above and below it.")); line->addWidget(polarity);
            connect(polarity, &QAbstractButton::clicked, this, [edit](bool v) { edit([&](auto& r) { r.bipolar = v; }); });
            const auto contextMenu = [this, routeId](const QPoint& at) {
                QMenu menu(this); auto* automateAction = menu.addAction(tr("Automate depth")); auto* remove = menu.addAction(tr("Remove")); const auto action = menu.exec(at);
                if (action == automateAction) automate(routeId.toStdString(), true);
                else if (action == remove) change([&](auto& rack) { std::erase_if(rack.routes, [&](const auto& r) { return r.id == routeId.toStdString(); }); });
            };
            for (auto* control : {static_cast<QWidget*>(group), static_cast<QWidget*>(label), static_cast<QWidget*>(depth), static_cast<QWidget*>(polarity)}) {
                control->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(control, &QWidget::customContextMenuRequested, this, [contextMenu, control](const QPoint& at) { contextMenu(control->mapToGlobal(at)); });
            }
            m_refreshers.push_back([this, routeId, enabled, depth, polarity, label, name] {
                for (const auto& r : m_shown.routes) if (r.id == routeId.toStdString()) {
                    QSignalBlocker a(enabled), b(depth), c(polarity); enabled->setChecked(r.enabled);
                    if (depth->value() != r.depth * 100) depth->setValue(r.depth * 100);
                    polarity->setChecked(r.bipolar); polarity->setText(r.bipolar ? QStringLiteral("±") : QStringLiteral("+"));
                    const auto status = m_controller->rackModulationStatus(m_channel.toStdString(), r.id); label->setLabel(status.empty() ? name : "! " + name);
                    if (!status.empty()) {
                        QString reason = tr("Assignment unavailable");
                        if (status == "Device unavailable") reason = tr("Device unavailable");
                        else if (status == "Parameter unavailable") reason = tr("Parameter unavailable");
                        else if (status == "Plugin identity changed") reason = tr("Plugin identity changed");
                        else if (status == "Audio source unavailable") reason = tr("Audio source unavailable");
                        label->setToolTip(name + "\n" + reason);
                    }
                    break;
                }
            });
            list->addWidget(group);
        }
        if (!count) {
            auto* help = new QLabel(tr("Assign this source to a parameter to hear its movement."), assignments); help->setWordWrap(true); list->addWidget(help);
            m_refreshers.push_back([this, id, help] { help->setText(m_assigning && id == m_selected
                ? tr("Click a highlighted control, or move a knob in the plugin editor. Esc cancels.")
                : tr("Assign this source to a parameter to hear its movement.")); });
        }
        list->addStretch();
        auto* area = new QScrollArea(targets); area->setFrameShape(QFrame::NoFrame); area->setWidgetResizable(true); area->setWidget(assignments); area->setMinimumWidth(0);
        area->setObjectName("ModulationAssignments"); area->setProperty("sourceId", id);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); right->addWidget(area, 1);
        if (scrolls.contains(id)) QTimer::singleShot(0, area, [area, value = scrolls.value(id)] { area->verticalScrollBar()->setValue(value); });
        m_layout->addWidget(card);
    }
    setFixedWidth(totalWidth); setVisible(totalWidth > 0 && !m_controller->hasCloudProjectBinding()); synchronize();
    if (!focusedSource.isEmpty()) for (auto* title : findChildren<QToolButton*>("ModulationSourceTitle"))
        if (title->property("sourceId").toString() == focusedSource) { title->setFocus(Qt::OtherFocusReason); break; }
    highlight();
}
