#include "RackModulationPanel.hpp"
#include "RackModulationEditor.hpp"
#include <QShortcut>
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
}
RackModulationPanel::RackModulationPanel(daw::EngineController* c, QWidget* rack)
    : QWidget(rack), m_controller(c), m_rack(rack), m_layout(new QHBoxLayout(this)) {
    setObjectName("RackModulationPanel"); setMinimumWidth(0);
    m_layout->setContentsMargins(0, 0, 0, 0); m_layout->setSpacing(8);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { rebuild(); });
    QTimer::singleShot(0, this, [this] { qApp->installEventFilter(this); });
}
RackModulationPanel::~RackModulationPanel() {
    for (auto* widget : findChildren<QWidget*>()) if (auto* graph = dynamic_cast<RackModulationEditor*>(widget)) {
        graph->finishGesture(); graph->begin = {}; graph->finish = {}; graph->changed = {};
    }
    finishGesture(); qApp->removeEventFilter(this);
}
QString RackModulationPanel::key(const char* suffix) const {
    return "rack/" + QString::fromStdString(m_controller->project().miniModuleProjectId) + "/" + m_channel + "/modulation/" + suffix;
}
void RackModulationPanel::setChannel(const QString& channel) {
    finishGesture();
    if (m_expanded) m_expanded->close();
    setAssigning(false); m_channel = channel;
    m_selected = QSettings().value(key("selected")).toString();
    m_shown = m_controller->rackModulation(channel.toStdString()); rebuild();
}
void RackModulationPanel::change(const std::function<void(daw::modulation::Rack&)>& edit) {
    const auto& before = m_controller->rackModulation(m_channel.toStdString());
    auto next = before; edit(next);
    bool structural = before.sources.size() != next.sources.size() || before.routes.size() != next.routes.size();
    if (!structural) {
        for (std::size_t i = 0; i < next.sources.size(); ++i) structural |= before.sources[i].id != next.sources[i].id || before.sources[i].kind != next.sources[i].kind;
        for (std::size_t i = 0; i < next.routes.size(); ++i) structural |= before.routes[i].id != next.routes[i].id;
    }
    if (!m_controller->setRackModulation(m_channel.toStdString(), next)) {
        QMessageBox::information(this, tr("Modulation"), tr("This connection is unavailable or would create a feedback loop. Choose an independent audio source."));
        return;
    }
    emit edited(structural); refresh();
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
    bool structure = current.sources.size() != m_shown.sources.size() || current.routes.size() != m_shown.routes.size() ||
        m_structureRevision != m_controller->project().structureRevision();
    if (!structure) {
        for (std::size_t i = 0; i < current.sources.size(); ++i)
            structure |= current.sources[i].id != m_shown.sources[i].id || current.sources[i].kind != m_shown.sources[i].kind;
        for (std::size_t i = 0; i < current.routes.size(); ++i) {
            const auto& a = current.routes[i]; const auto& b = m_shown.routes[i];
            structure |= a.id != b.id || a.sourceId != b.sourceId || a.slotId != b.slotId || a.parameterId != b.parameterId || a.pluginUid != b.pluginUid;
        }
    }
    if (structure && !m_pending) {
        m_pending = true;
        QTimer::singleShot(0, this, [this] { m_pending = false; m_shown = m_controller->rackModulation(m_channel.toStdString()); rebuild(); });
    } else if (!structure && m_shown != current) {
        m_shown = current; synchronize();
    }
    for (auto* widget : findChildren<QWidget*>()) if (auto* graph = dynamic_cast<RackModulationEditor*>(widget)) {
        if (graph->visibleRegion().isEmpty()) continue;
        const auto& id = graph->source().id;
        graph->sample(m_controller->rackModulationValue(m_channel.toStdString(), id),
                      m_controller->rackModulationPosition(m_channel.toStdString(), id));
    }
}
const daw::modulation::Source* RackModulationPanel::source(const QString& id) const {
    for (const auto& value : m_shown.sources) if (value.id == id.toStdString()) return &value;
    return nullptr;
}
void RackModulationPanel::beginGesture() {
    if (!m_gesture) m_gesture = m_controller->beginUndoGroup();
}
void RackModulationPanel::finishGesture() {
    if (m_gesture) { m_controller->collapseUndo(m_gesture, "Edit modulation shape"); m_gesture = {}; }
}
void RackModulationPanel::synchronize() {
    m_rack->setProperty("modulationSource", m_selected);
    for (const auto& update : m_refreshers) update();
    if (m_expanded) {
        const auto* value = source(m_expandedSource);
        if (!value) m_expanded->close();
        else {
            m_expanded->setWindowTitle(QString::fromStdString(value->name));
            for (auto* widget : m_expanded->findChildren<QWidget*>())
                if (auto* graph = dynamic_cast<RackModulationEditor*>(widget)) graph->setSource(*value);
        }
    }
}
void RackModulationPanel::expandEditor(const QString& id) {
    if (m_expanded && m_expandedSource == id) { m_expanded->raise(); m_expanded->activateWindow(); return; }
    if (m_expanded) m_expanded->close();
    const auto* value = source(id); if (!value) return;
    auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setModal(false);
    dialog->setObjectName("ModulationExpandedEditor");
    dialog->setWindowTitle(QString::fromStdString(value->name)); dialog->resize(720, 360); dialog->setMinimumSize(420, 240);
    m_expanded = dialog; m_expandedSource = id;
    auto* layout = new QVBoxLayout(dialog);
    auto* graph = new RackModulationEditor(dialog); graph->setSource(*value); layout->addWidget(graph, 1);
    graph->begin = [this] { beginGesture(); }; graph->finish = [this] { finishGesture(); };
    graph->changed = [this, id](const auto& next) { editSource(id, [&](auto& s) { s.points = next.points; s.steps = next.steps; }); };
    auto* hint = new QLabel(graph->toolTip(), dialog); hint->setWordWrap(true); layout->addWidget(hint);
    for (const auto& entry : {std::pair{QKeySequence::Undo, "undo"}, std::pair{QKeySequence::Redo, "redo"}}) {
        auto* shortcut = new QShortcut(QKeySequence(entry.first), dialog);
        connect(shortcut, &QShortcut::activated, dialog, [this, action = QString::fromLatin1(entry.second), graph] {
            graph->finishGesture(); command(action);
        });
    }
    connect(dialog, &QDialog::finished, this, [this, graph] { graph->finishGesture(); finishGesture(); });
    dialog->show(); graph->setFocus();
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
    if (auto* graph = dynamic_cast<RackModulationEditor*>(QApplication::focusWidget()); graph && isAncestorOf(graph)) {
        graph->finishGesture();
        if (action == "delete") { graph->removeSelected(); return true; }
    }
    finishGesture();
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
    if (daw::modulation::hasRate(value.kind)) { field(tr("Phase · 0–1"), value.phase, 0, 1); field(tr("Smoothing · ms"), value.smoothMs, 0, 1000); }
    else {
        auto* tap = new QComboBox(&dialog); tap->addItems({tr("Before effects"), tr("After effects"), tr("After fader")});
        tap->setCurrentIndex(value.audioChannelId.empty() ? 0 : int(value.tap)); tap->setEnabled(!value.audioChannelId.empty());
        tap->setAccessibleName(tr("Audio tap")); layout->addRow(tr("Audio tap"), tap);
        connect(tap, &QComboBox::activated, &dialog, [&](int i) { value.tap = daw::modulation::Tap(i); });
        auto* filter = new QCheckBox(tr("Detector filter"), &dialog); filter->setChecked(value.filter); layout->addRow(filter);
        connect(filter, &QCheckBox::clicked, &dialog, [&](bool v) { value.filter = v; });
        field(tr("Low cutoff · Hz"), value.lowHz, 10, 20000); field(tr("High cutoff · Hz"), value.highHz, 20, 24000);
    }
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addRow(actions);
    connect(actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) editSource(id, [&](auto& s) { s = value; });
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
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) { setAssigning(false); synchronize(); return true; }
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
