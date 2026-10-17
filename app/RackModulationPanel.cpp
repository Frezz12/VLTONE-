#include "RackModulationPanel.hpp"
#include "Controls.hpp"
#include "Theme.hpp"
#include "Icons.hpp"
#include <QFrame>
#include <QHash>
#include <QFormLayout>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <cmath>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <array>

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
class SourceGraph final : public QWidget {
public:
    std::string source;
    std::array<float, 64> points{};
    explicit SourceGraph(std::string id, QWidget* parent) : QWidget(parent), source(std::move(id)) {
        setFixedHeight(34); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void add(float v) { std::move(points.begin() + 1, points.end(), points.begin()); points.back() = v; update(); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(th().separator(), 1)); p.drawLine(0, height() - 2, width(), height() - 2);
        QPainterPath line; line.moveTo(0, height() - 2 - points[0] * (height() - 4));
        for (int i = 1; i < 64; ++i) line.lineTo(i * width() / 63., height() - 2 - points[i] * (height() - 4));
        p.setPen(QPen(th().accent, 1.5)); p.drawPath(line);
    }
};
class TargetOutline final : public QWidget {
public:
    explicit TargetOutline(QWidget* parent) : QWidget(parent) {
        setObjectName("ModulationTargetOutline"); setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(parent->rect()); show();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(th().accent, 1.5, Qt::DashLine)); p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
};
QToolButton* button(const QString& text, QWidget* parent, const std::function<void()>& run) {
    auto* b = new QToolButton(parent); b->setText(text); b->setAccessibleName(text); b->setToolTip(text);
    b->setMinimumHeight(26); b->setFocusPolicy(Qt::StrongFocus);
    QObject::connect(b, &QAbstractButton::clicked, parent, run); return b;
}
QScrollArea* makeScrollArea(QWidget* content, QWidget* parent, int height = 0) {
    auto* s = new QScrollArea(parent); s->setFrameShape(QFrame::NoFrame); s->setWidgetResizable(true);
    s->setWidget(content); s->setMinimumWidth(0); s->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (height) s->setFixedHeight(height); return s;
}
}
RackModulationPanel::RackModulationPanel(daw::EngineController* c, QWidget* rack)
    : QWidget(rack), m_controller(c), m_rack(rack), m_layout(new QHBoxLayout(this)) {
    setObjectName("RackModulationPanel"); setMinimumWidth(0);
    m_layout->setContentsMargins(0, 0, 0, 0); m_layout->setSpacing(8);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { rebuild(); });
    QTimer::singleShot(0, this, [this] { qApp->installEventFilter(this); });
}
RackModulationPanel::~RackModulationPanel() { qApp->removeEventFilter(this); }
QString RackModulationPanel::key(const char* suffix) const {
    return "rack/" + QString::fromStdString(m_controller->project().miniModuleProjectId) + "/" + m_channel + "/modulation/" + suffix;
}
void RackModulationPanel::setChannel(const QString& channel) {
    setAssigning(false); m_channel = channel;
    m_selected = QSettings().value(key("selected")).toString();
    m_shown = m_controller->rackModulation(channel.toStdString()); rebuild();
}
void RackModulationPanel::change(const std::function<void(daw::modulation::Rack&)>& edit) {
    auto next = m_controller->rackModulation(m_channel.toStdString()); edit(next);
    if (!m_controller->setRackModulation(m_channel.toStdString(), next)) {
        QMessageBox::information(this, tr("Modulation"), tr("This connection is unavailable or would create a feedback loop. Choose an independent audio source."));
        return;
    }
    emit edited(); refresh();
}
void RackModulationPanel::editSource(const QString& id, const std::function<void(daw::modulation::Source&)>& edit) {
    change([&](auto& rack) { for (auto& source : rack.sources) if (source.id == id.toStdString()) edit(source); });
}
void RackModulationPanel::refresh() {
    if (m_assigning && m_touch != m_controller->parameterTouchRevision(m_channel.toStdString())) {
        m_touch = m_controller->parameterTouchRevision(m_channel.toStdString());
        if (const auto target = m_controller->lastEditedPluginParameter(m_channel.toStdString())) {
            auto slot = target->slotId;
            if (slot.empty()) if (const auto* t = m_controller->project().findTrack(target->channelId)) slot = t->instrument.id;
            assign(QString::fromStdString(slot), QString::fromStdString(target->parameterId));
        }
    }
    const auto& current = m_controller->rackModulation(m_channel.toStdString());
    if ((m_shown != current || m_structureRevision != m_controller->project().structureRevision()) && !m_pending) {
        m_pending = true;
        QTimer::singleShot(0, this, [this] { m_pending = false; m_shown = m_controller->rackModulation(m_channel.toStdString()); rebuild(); });
    }
    if (isVisible()) for (auto* widget : findChildren<QWidget*>()) if (auto* graph = dynamic_cast<SourceGraph*>(widget))
        graph->add(m_controller->rackModulationValue(m_channel.toStdString(), graph->source));
}
void RackModulationPanel::automate(const std::string& id, bool depth) {
    daw::AutomationTarget target;
    target.channelId = m_channel.toStdString(); target.slotId = id;
    target.kind = depth ? daw::AutomationTargetKind::ModulationDepth : daw::AutomationTargetKind::ModulatorRate;
    target.parameterId = depth ? "depth" : "rate";
    m_controller->ensureAutomation(target); emit edited();
}
void RackModulationPanel::addSource(daw::modulation::Kind kind) {
    m_selected = QString::fromStdString(m_controller->addRackModulator(m_channel.toStdString(), kind));
    m_shown = m_controller->rackModulation(m_channel.toStdString()); rebuild(); emit edited();
}
bool RackModulationPanel::command(const QString& action) {
    auto id = m_selected;
    for (auto* widget = QApplication::focusWidget(); widget && widget != this; widget = widget->parentWidget())
        if (widget->objectName() == "RackModulatorCard") { id = widget->property("sourceId").toString(); break; }
    if (action == "undo" || action == "redo") {
        if (action == "undo") m_controller->undo(); else m_controller->redo();
        emit edited(); refresh();
    } else if (action == "duplicate") {
        const auto copy = m_controller->duplicateRackModulator(m_channel.toStdString(), id.toStdString());
        if (!copy.empty()) { m_selected = QString::fromStdString(copy); emit edited(); refresh(); }
    } else if (action == "delete") change([&](auto& rack) {
        std::erase_if(rack.routes, [&](const auto& route) { return route.sourceId == id.toStdString(); });
        std::erase_if(rack.sources, [&](const auto& source) { return source.id == id.toStdString(); });
    });
    // A focused source must never dispatch device commands to a stale plugin selection.
    return true;
}
void RackModulationPanel::sourceMenu(const QString& id, const QPoint& at) {
    QMenu menu(this);
    auto* rename = menu.addAction(tr("Rename")); auto* duplicate = menu.addAction(tr("Duplicate"));
    auto* remove = menu.addAction(tr("Delete"));
    menu.addSeparator();
    auto* left = menu.addAction(tr("Move left")); auto* right = menu.addAction(tr("Move right"));
    auto* all = menu.addAction(tr("Enable all modulation")); all->setCheckable(true); all->setChecked(m_shown.enabled);
    const auto action = menu.exec(at);
    if (action == rename) {
        QString old;
        for (const auto& s : m_shown.sources) if (s.id == id.toStdString()) old = QString::fromStdString(s.name);
        bool ok = false; const auto name = QInputDialog::getText(this, tr("Rename source"), tr("Name"), QLineEdit::Normal, old, &ok);
        if (ok && !name.trimmed().isEmpty()) editSource(id, [&](auto& s) { s.name = name.trimmed().left(128).toStdString(); });
    } else if (action == duplicate) {
        const auto copy = m_controller->duplicateRackModulator(m_channel.toStdString(), id.toStdString());
        if (!copy.empty()) { m_selected = QString::fromStdString(copy); emit edited(); refresh(); }
    }
    else if (action == remove) change([&](auto& r) {
        std::erase_if(r.routes, [&](const auto& x) { return x.sourceId == id.toStdString(); });
        std::erase_if(r.sources, [&](const auto& x) { return x.id == id.toStdString(); });
    });
    else if (action == left || action == right) change([&](auto& r) {
        const auto found = std::find_if(r.sources.begin(), r.sources.end(), [&](const auto& s) { return s.id == id.toStdString(); });
        if (found == r.sources.end()) return;
        if (action == left && found != r.sources.begin()) std::iter_swap(found, found - 1);
        if (action == right && found + 1 != r.sources.end()) std::iter_swap(found, found + 1);
    });
    else if (action == all) change([&](auto& r) { r.enabled = all->isChecked(); });
}
void RackModulationPanel::advanced(const QString& id) {
    auto found = std::find_if(m_shown.sources.begin(), m_shown.sources.end(), [&](const auto& s) { return s.id == id.toStdString(); });
    if (found == m_shown.sources.end()) return;
    auto value = *found;
    QDialog dialog(this); dialog.setWindowTitle(tr("Source settings")); auto* layout = new QFormLayout(&dialog);
    const auto field = [&](const QString& name, double& current, double min, double max) {
        auto* spin = new QDoubleSpinBox(&dialog); spin->setDecimals(2); spin->setRange(min, max); spin->setValue(current);
        spin->setAccessibleName(name); layout->addRow(name, spin);
        connect(spin, &QDoubleSpinBox::valueChanged, &dialog, [&current](double v) { current = v; });
    };
    if (value.kind == daw::modulation::Kind::Lfo) { field(tr("Phase · 0–1"), value.phase, 0, 1); field(tr("Smoothing · ms"), value.smoothMs, 0, 1000); }
    else {
        auto* filter = new QCheckBox(tr("Detector filter"), &dialog); filter->setChecked(value.filter); layout->addRow(filter);
        connect(filter, &QCheckBox::clicked, &dialog, [&](bool v) { value.filter = v; });
        field(tr("Low cutoff · Hz"), value.lowHz, 10, 20000); field(tr("High cutoff · Hz"), value.highHz, 20, 24000);
    }
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addRow(actions);
    connect(actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) editSource(id, [&](auto& s) { s = value; });
}
void RackModulationPanel::rebuild() {
    QString focusedSource;
    for (auto* widget = QApplication::focusWidget(); widget && widget != this; widget = widget->parentWidget())
        if (widget->objectName() == "RackModulatorCard" && isAncestorOf(widget)) { focusedSource = widget->property("sourceId").toString(); break; }
    QHash<QString, std::array<float, 64>> history;
    for (auto* widget : findChildren<QWidget*>()) if (auto* graph = dynamic_cast<SourceGraph*>(widget))
        history.insert(QString::fromStdString(graph->source), graph->points);
    while (auto* item = m_layout->takeAt(0)) { delete item->widget(); delete item; }
    m_structureRevision = m_controller->project().structureRevision();
    const auto& rack = m_shown;
    if (std::none_of(rack.sources.begin(), rack.sources.end(), [&](const auto& s) { return s.id == m_selected.toStdString(); })) {
        m_assigning = false; m_route.clear();
        m_selected = rack.sources.empty() ? QString{} : QString::fromStdString(rack.sources.front().id);
    }
    QSettings().setValue(key("selected"), m_selected); m_rack->setProperty("modulationSource", m_selected);
    int totalWidth = 0;
    for (const auto& source : rack.sources) {
        const auto id = QString::fromStdString(source.id);
        const bool collapsed = QSettings().value(key("collapsed/") + id, false).toBool();
        const auto count = std::count_if(rack.routes.begin(), rack.routes.end(), [&](const auto& r) { return r.sourceId == source.id; });
        auto* card = new QFrame(this); card->setObjectName("RackModulatorCard"); card->setProperty("sourceId", id);
        card->setFocusPolicy(Qt::StrongFocus);
        card->setAccessibleName(QString::fromStdString(source.name)); card->setFixedWidth(collapsed ? 170 : 278);
        card->setMinimumHeight(242); totalWidth += card->width() + (totalWidth ? 8 : 0);
        card->setStyleSheet("#RackModulatorCard{background:" + th().surface.name() + ";border:1px solid " +
            (id == m_selected ? th().accent : th().separator()).name() + ";border-radius:7px;}");
        auto* column = new QVBoxLayout(card); column->setContentsMargins(7, 4, 7, 6); column->setSpacing(5);
        auto* header = new QHBoxLayout; header->setSpacing(4);
        auto* on = new ui::IconButton(icons::Glyph::Power, tr("Enable %1").arg(QString::fromStdString(source.name)), card);
        on->setButtonSize(24, 24); on->setCheckable(true); on->setChecked(source.enabled); on->setFocusPolicy(Qt::StrongFocus);
        connect(on, &QAbstractButton::clicked, this, [this, id](bool value) { editSource(id, [&](auto& s) { s.enabled = value; }); });
        header->addWidget(on);
        auto* title = button(card->fontMetrics().elidedText(QString::fromStdString(source.name), Qt::ElideRight, collapsed ? 62 : 170), card, [this, id] {
            setAssigning(false); m_selected = id; rebuild();
        });
        title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        title->setAccessibleName(QString::fromStdString(source.name));
        title->setToolTip(QString::fromStdString(source.name)); title->setObjectName("ModulationSourceTitle"); title->setProperty("sourceId", id);
        title->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(title, &QWidget::customContextMenuRequested, this, [this, id, title](const QPoint& p) { sourceMenu(id, title->mapToGlobal(p)); });
        header->addWidget(title, 1);
        auto* menu = button(QStringLiteral("..."), card, [this, id] {
            if (auto* trigger = qobject_cast<QWidget*>(sender())) sourceMenu(id, trigger->mapToGlobal(QPoint(0, trigger->height())));
        });
        menu->setFixedSize(24, 26); menu->setAccessibleName(tr("Module menu")); menu->setToolTip(tr("Module menu"));
        header->addWidget(menu);
        auto* fold = new ui::IconButton(collapsed ? icons::Glyph::ChevronRight : icons::Glyph::Chevron, tr("Expand or collapse module"), card);
        fold->setButtonSize(24, 24); fold->setObjectName("ModulationFold"); fold->setProperty("sourceId", id); fold->setFocusPolicy(Qt::StrongFocus);
        connect(fold, &QAbstractButton::clicked, this, [this, id, collapsed] { QSettings().setValue(key("collapsed/") + id, !collapsed); setAssigning(false); rebuild(); });
        header->addWidget(fold); column->addLayout(header);
        auto* graph = new SourceGraph(source.id, card);
        if (history.contains(id)) graph->points = history.value(id);
        graph->setToolTip(tr("Assignments: %1").arg(count)); column->addWidget(graph);
        if (collapsed) {
            auto* countLabel = new QLabel(tr("Assignments: %1").arg(count), card); countLabel->setAlignment(Qt::AlignCenter);
            column->addWidget(countLabel); column->addStretch(); m_layout->addWidget(card); continue;
        }
        const auto row = [&] { auto* layout = new QHBoxLayout; layout->setSpacing(5); column->addLayout(layout); return layout; };
        const auto number = [&](QHBoxLayout* layout, const QString& title, double value, double min, double max, const QString& unit, const std::function<void(double)>& edit) {
            auto* field = new QWidget(card); auto* box = new QVBoxLayout(field); box->setContentsMargins(0, 0, 0, 0); box->setSpacing(2);
            auto* label = new QLabel(title, field); auto* spin = new CompactNumber(field);
            spin->setKeyboardTracking(false); spin->setRange(min, max); spin->setDecimals(2); spin->setValue(value); spin->setSuffix(unit);
            spin->setMinimumWidth(0); spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); spin->setAccessibleName(title); spin->setToolTip(title); label->setBuddy(spin);
            box->addWidget(label); box->addWidget(spin); layout->addWidget(field, 1);
            connect(spin, &QDoubleSpinBox::editingFinished, this, [spin, edit] { edit(spin->value()); });
        };
        if (source.kind == daw::modulation::Kind::Lfo) {
            auto* form = row();
            auto* shape = new QComboBox(card); shape->addItems({tr("Sine"), tr("Triangle"), tr("Ramp up"), tr("Ramp down"), tr("Square")});
            shape->setCurrentIndex(int(source.wave)); shape->setAccessibleName(tr("Waveform")); form->addWidget(shape, 1);
            connect(shape, &QComboBox::activated, this, [this, id](int i) { editSource(id, [&](auto& s) { s.wave = daw::modulation::Wave(i); }); });
            auto* mode = new QComboBox(card); mode->addItems({tr("Sync"), tr("Hz")}); mode->setCurrentIndex(source.sync ? 0 : 1); mode->setAccessibleName(tr("Rate mode")); form->addWidget(mode, 1);
            connect(mode, &QComboBox::activated, this, [this, id](int i) { editSource(id, [&](auto& s) { s.sync = i == 0; }); });
            auto* rateRow = row();
            if (source.sync) {
                auto* period = new QComboBox(card); period->setAccessibleName(tr("Period")); period->setToolTip(tr("Period"));
                for (double beats : {128., 64., 32., 16., 8., 4., 2., 1., .5, .25, .125, .0625, .03125}) {
                    period->addItem(beats >= 4 ? tr("%1 bars").arg(beats / 4) : "1/" + QString::number(4 / beats), beats);
                    if (source.beats == beats) period->setCurrentIndex(period->count() - 1);
                }
                rateRow->addWidget(period, 1); connect(period, &QComboBox::activated, this, [this, id, period] { editSource(id, [&](auto& s) { s.beats = period->currentData().toDouble(); }); });
            } else number(rateRow, tr("Rate"), source.rate, .01, 20, " Hz", [this, id](double v) { editSource(id, [&](auto& s) { s.rate = v; }); });
            rateRow->addWidget(button(tr("Automate rate"), card, [this, source] { automate(source.id, false); }));
        } else {
            auto* audioRow = row(); auto* audio = new QComboBox(card); audio->setAccessibleName(tr("Audio source")); audio->addItem(tr("Own input"), "");
            audio->setMinimumWidth(0); audio->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            for (const auto& track : m_controller->project().tracks) if (daw::carriesAudio(track) && track.id != m_channel.toStdString()) audio->addItem(QString::fromStdString(track.name), QString::fromStdString(track.id));
            if (m_channel != "master") audio->addItem(tr("Master"), "master");
            int index = audio->findData(QString::fromStdString(source.audioChannelId));
            if (index < 0) { audio->addItem(tr("Missing audio source"), QString::fromStdString(source.audioChannelId)); index = audio->count() - 1; }
            audio->setCurrentIndex(index); audioRow->addWidget(audio, 1);
            connect(audio, &QComboBox::activated, this, [this, id, audio] { editSource(id, [&](auto& s) { s.audioChannelId = audio->currentData().toString().toStdString(); }); });
            auto* tap = new QComboBox(card); tap->addItems({tr("Before effects"), tr("After effects"), tr("After fader")});
            tap->setAccessibleName(tr("Audio tap")); tap->setMinimumWidth(0); tap->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            tap->setCurrentIndex(source.audioChannelId.empty() ? 0 : int(source.tap)); tap->setEnabled(!source.audioChannelId.empty()); audioRow->addWidget(tap, 1);
            connect(tap, &QComboBox::activated, this, [this, id](int i) { editSource(id, [&](auto& s) { s.tap = daw::modulation::Tap(i); }); });
            auto* dynamics = row();
            number(dynamics, tr("Sensitivity"), source.gainDb, -48, 48, " dB", [this, id](double v) { editSource(id, [&](auto& s) { s.gainDb = v; }); });
            number(dynamics, tr("Attack"), source.attackMs, .1, 2000, " ms", [this, id](double v) { editSource(id, [&](auto& s) { s.attackMs = v; }); });
            number(dynamics, tr("Release"), source.releaseMs, 1, 10000, " ms", [this, id](double v) { editSource(id, [&](auto& s) { s.releaseMs = v; }); });
        }
        auto* actions = row();
        auto* assignButton = button(m_assigning && id == m_selected ? tr("Cancel · Esc") : tr("Assign"), card, [this, id] {
            const bool active = m_assigning && m_selected == id; m_selected = id; setAssigning(!active); rebuild();
        });
        assignButton->setObjectName("ModulationAssign"); assignButton->setProperty("sourceId", id); actions->addWidget(assignButton, 1);
        auto* parameters = button(tr("Parameters…"), card, [this, id] { m_selected = id; chooseParameter(); });
        parameters->setObjectName("ModulationParameters"); actions->addWidget(parameters, 1);
        auto* advancedButton = new ui::IconButton(icons::Glyph::Gear, tr("Advanced…"), card); advancedButton->setButtonSize(26, 26);
        advancedButton->setFocusPolicy(Qt::StrongFocus);
        connect(advancedButton, &QAbstractButton::clicked, this, [this, id] { advanced(id); }); actions->addWidget(advancedButton);
        auto* assignments = new QWidget; auto* list = new QVBoxLayout(assignments); list->setContentsMargins(0, 0, 0, 0); list->setSpacing(6);
        if (count) list->addWidget(new QLabel(tr("Assignments: %1").arg(count), assignments));
        for (const auto& route : rack.routes) if (route.sourceId == source.id) {
            auto* group = new QWidget(assignments); auto* item = new QVBoxLayout(group); item->setContentsMargins(0, 0, 0, 0); item->setSpacing(2);
            const auto edit = [this, routeId = route.id](const std::function<void(daw::modulation::Route&)>& f) { change([&](auto& r) { for (auto& value : r.routes) if (value.id == routeId) f(value); }); };
            const auto* slot = m_controller->insertModel(m_channel.toStdString(), route.slotId);
            auto name = QString::fromStdString(slot ? slot->name : route.pluginUid) + " → " + QString::fromStdString(route.parameterId);
            for (const auto& info : m_controller->insertParameters(m_channel.toStdString(), route.slotId)) if (info.id == route.parameterId) name = QString::fromStdString(slot ? slot->name : route.pluginUid) + " → " + QString::fromStdString(info.name);
            auto* label = button(card->fontMetrics().elidedText(name, Qt::ElideRight, 234), group, [this, id, route] { m_selected = id; m_rack->setProperty("modulationSource", id); m_route = QString::fromStdString(route.id); highlight(); });
            label->setToolTip(name); label->setAccessibleName(name); item->addWidget(label);
            const auto status = m_controller->rackModulationStatus(m_channel.toStdString(), route.id);
            if (!status.empty()) {
                QString reason = tr("Assignment unavailable");
                if (status == "Device unavailable") reason = tr("Device unavailable");
                else if (status == "Parameter unavailable") reason = tr("Parameter unavailable");
                else if (status == "Plugin identity changed") reason = tr("Plugin identity changed");
                else if (status == "Audio source unavailable") reason = tr("Audio source unavailable");
                label->setText("! " + label->text()); label->setToolTip(name + "\n" + reason);
            }
            auto* line = new QHBoxLayout; line->setSpacing(3); item->addLayout(line);
            auto* enabled = new QCheckBox(group); enabled->setChecked(route.enabled); enabled->setAccessibleName(tr("Enable assignment")); line->addWidget(enabled);
            connect(enabled, &QCheckBox::clicked, this, [edit](bool value) { edit([&](auto& r) { r.enabled = value; }); });
            auto* depth = new QDoubleSpinBox(group); depth->setRange(-100, 100); depth->setDecimals(1); depth->setSuffix(" %"); depth->setValue(route.depth * 100); depth->setKeyboardTracking(false);
            depth->setAccessibleName(tr("Depth")); depth->setToolTip(tr("Maximum deviation as a percentage of the parameter range. Negative values reverse the movement.")); depth->setFixedWidth(75); line->addWidget(depth);
            connect(depth, &QDoubleSpinBox::editingFinished, this, [edit, depth] { edit([&](auto& r) { r.depth = depth->value() / 100; }); });
            depth->setContextMenuPolicy(Qt::CustomContextMenu); connect(depth, &QWidget::customContextMenuRequested, this, [this, route](const QPoint&) { QMenu menu(this); auto* a = menu.addAction(tr("Automate depth")); if (menu.exec(QCursor::pos()) == a) automate(route.id, true); });
            auto* polarity = new QComboBox(group); polarity->addItems({tr("One-sided"), tr("Two-sided")}); polarity->setCurrentIndex(route.bipolar ? 1 : 0); polarity->setMinimumWidth(0); polarity->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); line->addWidget(polarity, 1);
            polarity->setAccessibleName(tr("Movement")); polarity->setToolTip(tr("One-sided starts at the base; two-sided moves above and below it."));
            connect(polarity, &QComboBox::activated, this, [edit](int i) { edit([&](auto& r) { r.bipolar = i == 1; }); });
            auto* remove = new ui::IconButton(icons::Glyph::Close, tr("Remove"), group); remove->setButtonSize(22, 22); line->addWidget(remove);
            remove->setFocusPolicy(Qt::StrongFocus);
            connect(remove, &QAbstractButton::clicked, this, [this, route] { change([&](auto& r) { std::erase_if(r.routes, [&](const auto& x) { return x.id == route.id; }); }); });
            list->addWidget(group);
        }
        if (!count) {
            auto* help = new QLabel(m_assigning && id == m_selected ? tr("Click a highlighted control, or move a knob in the plugin editor. Esc cancels.") : tr("Assign this source to a parameter to hear its movement."), assignments);
            help->setWordWrap(true); list->addWidget(help);
        }
        list->addStretch(); auto* area = makeScrollArea(assignments, card); area->setMinimumHeight(50); area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); column->addWidget(area, 1);
        card->setToolTip(source.kind == daw::modulation::Kind::Lfo ? tr("Moves parameters in a repeating pattern.") : tr("Turns audio loudness into parameter movement."));
        if (!rack.enabled) card->setToolTip(tr("All modulation is bypassed. Enable it in the module menu."));
        m_layout->addWidget(card);
    }
    setFixedWidth(totalWidth); setVisible(totalWidth > 0 && !m_controller->hasCloudProjectBinding());
    if (!focusedSource.isEmpty()) for (auto* title : findChildren<QToolButton*>("ModulationSourceTitle"))
        if (title->property("sourceId").toString() == focusedSource) { title->setFocus(Qt::OtherFocusReason); break; }
    updateGeometry(); highlight();
}
void RackModulationPanel::setAssigning(bool value) {
    m_assigning = value && !m_selected.isEmpty(); m_touch = m_controller->parameterTouchRevision(m_channel.toStdString()); highlight();
}
void RackModulationPanel::highlight() {
    if (!m_rack) return;
    const daw::modulation::Route* selected = nullptr;
    for (const auto& r : m_controller->rackModulation(m_channel.toStdString()).routes) if (r.id == m_route.toStdString()) selected = &r;
    for (auto* widget : m_rack->findChildren<QWidget*>()) {
        if (widget->property("modulationChannel").toString() != m_channel || !widget->property("modulationEligible").toBool()) continue;
        const bool chosen = selected && widget->property("modulationSlot").toString().toStdString() == selected->slotId && widget->property("modulationParameter").toString().toStdString() == selected->parameterId;
        auto* outline = widget->findChild<QWidget*>("ModulationTargetOutline", Qt::FindDirectChildrenOnly);
        if (m_assigning || chosen) { if (!outline) outline = new TargetOutline(widget); outline->setGeometry(widget->rect()); outline->raise(); outline->show(); }
        else if (outline) outline->hide();
        widget->update();
    }
}
void RackModulationPanel::assign(const QString& slot, const QString& parameter) {
    const auto route = m_controller->assignRackModulation(m_channel.toStdString(), m_selected.toStdString(), slot.toStdString(), parameter.toStdString());
    if (route.empty()) { QMessageBox::information(this, tr("Modulation"), tr("This connection is unavailable or would create a feedback loop. Choose an independent audio source.")); return; }
    m_route = QString::fromStdString(route); setAssigning(false); emit edited(); m_shown = m_controller->rackModulation(m_channel.toStdString()); rebuild();
}
bool RackModulationPanel::eventFilter(QObject* object, QEvent* event) {
    if (object == this && event->type() == QEvent::LanguageChange)
        QTimer::singleShot(0, this, [this] { rebuild(); });
    if (m_consumed && object == m_consumed && (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)) { m_consumed.clear(); return true; }
    if (!isVisible() || !m_assigning) return false;
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) { setAssigning(false); rebuild(); return true; }
    const bool click = event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton;
    const bool keyboard = event->type() == QEvent::KeyPress && (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return || static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space);
    if (!click && !keyboard) return false;
    auto* widget = qobject_cast<QWidget*>(object);
    for (auto* control = widget; control && control != m_rack; control = control->parentWidget()) {
        if (!control->property("modulationEligible").toBool() || control->property("modulationChannel").toString() != m_channel) continue;
        if (click) m_consumed = widget;
        assign(control->property("modulationSlot").toString(), control->property("modulationParameter").toString()); return true;
    }
    return false;
}
void RackModulationPanel::chooseParameter() {
    setAssigning(false);
    QDialog dialog(this); dialog.setWindowTitle(tr("Choose parameter")); dialog.resize(470, 440); auto* layout = new QVBoxLayout(&dialog);
    auto* search = new QLineEdit(&dialog); search->setPlaceholderText(tr("Search devices and parameters")); search->setAccessibleName(search->placeholderText()); layout->addWidget(search);
    auto* list = new QListWidget(&dialog); layout->addWidget(list, 1);
    std::vector<const daw::InsertModel*> devices;
    const auto id = m_channel.toStdString();
    if (const auto* track = m_controller->project().findTrack(id); track && track->instrument.isLoaded()) devices.push_back(&track->instrument);
    if (const auto* chain = m_controller->channelInserts(id)) for (const auto& slot : *chain) if (slot.isLoaded()) devices.push_back(&slot);
    for (const auto& slot : m_controller->miniModules(id)) devices.push_back(&slot);
    for (const auto* slot : devices) for (const auto& parameter : m_controller->insertParameters(id, slot->id)) {
        if (!parameter.isAutomatable || parameter.isReadOnly || parameter.id.empty() || !std::isfinite(parameter.minValue) || !std::isfinite(parameter.maxValue) || parameter.minValue >= parameter.maxValue) continue;
        auto* item = new QListWidgetItem(QString::fromStdString(slot->name) + " → " + QString::fromStdString(parameter.name), list);
        item->setData(Qt::UserRole, QString::fromStdString(slot->id)); item->setData(Qt::UserRole + 1, QString::fromStdString(parameter.id));
    }
    connect(search, &QLineEdit::textChanged, &dialog, [list](const QString& text) { for (int i = 0; i < list->count(); ++i) list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive)); });
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(actions);
    connect(actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept); search->setFocus();
    if (dialog.exec() == QDialog::Accepted) if (const auto* item = list->currentItem()) assign(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString());
}
