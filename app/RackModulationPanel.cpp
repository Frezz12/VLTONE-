#include "RackModulationPanel.hpp"
#include "RackModulationEditor.hpp"
#include "RackParameterBinding.hpp"
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
#include <QContextMenuEvent>
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
#include <QToolTip>
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
RackModulationPanel::RackModulationPanel(daw::EngineController* c, QWidget* rack, QString slot)
    : QWidget(rack), m_controller(c), m_rack(rack), m_slot(std::move(slot)), m_layout(new QHBoxLayout(this)) {
    setObjectName("RackModulationPanel"); setMinimumWidth(0);
    m_layout->setContentsMargins(0, 0, 0, 0); m_layout->setSpacing(8);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { rebuild(); });
    QTimer::singleShot(0, this, [this] { qApp->installEventFilter(this); });
}
RackModulationPanel::~RackModulationPanel() {
    finishDepthGesture();
    for (auto* widget : findChildren<QWidget*>()) if (auto* graph = dynamic_cast<RackModulationEditor*>(widget)) {
        graph->finishGesture(); graph->begin = {}; graph->finish = {}; graph->changed = {};
    }
    finishGesture(); qApp->removeEventFilter(this);
}
QString RackModulationPanel::key(const char* suffix) const {
    return "rack/" + QString::fromStdString(m_controller->project().miniModuleProjectId) + "/" + m_channel + "/modulation/" + m_slot + "/" + suffix;
}
bool RackModulationPanel::includes(const daw::modulation::Source& s) const {
    if (m_slot.isEmpty()) return s.ownerSlotId.empty() || !m_controller->insertModel(m_channel.toStdString(), s.ownerSlotId);
    if (s.ownerSlotId == m_slot.toStdString()) return true;
    return s.ownerSlotId.empty() && std::any_of(m_shown.routes.begin(), m_shown.routes.end(), [&](const auto& r) {
        return r.sourceId == s.id && r.slotId == m_slot.toStdString();
    });
}
void RackModulationPanel::finishEdits() {
    finishDepthGesture();
    for (auto* widget : findChildren<QWidget*>()) if (auto* graph = dynamic_cast<RackModulationEditor*>(widget)) graph->finishGesture();
    finishGesture();
}
void RackModulationPanel::hideEvent(QHideEvent* e) {
    finishEdits(); setAssigning(false); QWidget::hideEvent(e);
}
void RackModulationPanel::setChannel(const QString& channel) {
    finishEdits();
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
            if (m_slot.isEmpty() || m_slot.toStdString() == slot)
                assign(QString::fromStdString(slot), QString::fromStdString(target->parameterId));
        }
    }
    const auto& current = m_controller->rackModulation(m_channel.toStdString());
    bool structure = current.sources.size() != m_shown.sources.size() || current.routes.size() != m_shown.routes.size() ||
        m_structureRevision != m_controller->project().structureRevision();
    if (!structure) {
        for (std::size_t i = 0; i < current.sources.size(); ++i)
            structure |= current.sources[i].id != m_shown.sources[i].id || current.sources[i].kind != m_shown.sources[i].kind || current.sources[i].ownerSlotId != m_shown.sources[i].ownerSlotId;
        for (std::size_t i = 0; i < current.routes.size(); ++i) {
            const auto& a = current.routes[i]; const auto& b = m_shown.routes[i];
            structure |= a.id != b.id || a.sourceId != b.sourceId || a.slotId != b.slotId || a.parameterId != b.parameterId || a.pluginUid != b.pluginUid;
        }
    }
    if (structure && !m_pending && !QApplication::activePopupWidget()) {
        m_pending = true;
        QTimer::singleShot(0, this, [this] {
            m_pending = false;
            if (QApplication::activePopupWidget()) return;
            m_shown = m_controller->rackModulation(m_channel.toStdString()); rebuild();
        });
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
    if (m_rack) m_rack->setProperty("modulationSource", m_selected);
    for (const auto& update : m_refreshers) update();
    for (const auto& update : m_expandedRefreshers) update();
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
    dialog->setWindowTitle(QString::fromStdString(value->name)); dialog->resize(720, 520); dialog->setMinimumSize(420, 380);
    m_expanded = dialog; m_expandedSource = id;
    auto* layout = new QVBoxLayout(dialog);
    auto* graph = new RackModulationEditor(dialog); graph->setSource(*value); layout->addWidget(graph, 1);
    graph->begin = [this] { beginGesture(); }; graph->finish = [this] { finishGesture(); };
    graph->changed = [this, id](const auto& next) { editSource(id, [&](auto& s) { s.points = next.points; s.steps = next.steps; }); };
    m_expandedRefreshers.clear();
    layout->addWidget(sourceControls(id, dialog, true, m_expandedRefreshers));
    auto* hint = new QLabel(graph->toolTip(), dialog); hint->setWordWrap(true); layout->addWidget(hint);
    for (const auto& entry : {std::pair{QKeySequence::Undo, "undo"}, std::pair{QKeySequence::Redo, "redo"}}) {
        auto* shortcut = new QShortcut(QKeySequence(entry.first), dialog);
        connect(shortcut, &QShortcut::activated, dialog, [this, action = QString::fromLatin1(entry.second), graph] {
            graph->finishGesture(); command(action);
        });
    }
    connect(dialog, &QDialog::finished, this, [this, graph] { graph->finishGesture(); finishGesture(); m_expandedRefreshers.clear(); });
    synchronize(); dialog->show(); graph->setFocus();
}
void RackModulationPanel::automate(const std::string& id, bool depth) {
    daw::AutomationTarget target;
    target.channelId = m_channel.toStdString(); target.slotId = id;
    target.kind = depth ? daw::AutomationTargetKind::ModulationDepth : daw::AutomationTargetKind::ModulatorRate;
    target.parameterId = depth ? "depth" : "rate";
    m_controller->ensureAutomation(target); emit edited();
}
void RackModulationPanel::addSource(daw::modulation::Kind kind) {
    m_selected = QString::fromStdString(m_controller->addRackModulator(m_channel.toStdString(), kind, m_slot.toStdString()));
    m_shown = m_controller->rackModulation(m_channel.toStdString()); rebuild(); emit edited();
}
bool RackModulationPanel::command(const QString& action) {
    if (auto* graph = dynamic_cast<RackModulationEditor*>(QApplication::focusWidget()); graph && isAncestorOf(graph)) {
        graph->finishGesture();
        if (action == "delete" && (graph->source().kind == daw::modulation::Kind::Shaper ||
                                   graph->source().kind == daw::modulation::Kind::Steps)) {
            graph->removeSelected(); return true;
        }
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
void RackModulationPanel::sourceMenu(QString id, const QPoint& at) {
    QMenu menu(this);
    menu.setObjectName("ModulationSourceMenu");
    auto* rename = menu.addAction(tr("Rename")); auto* duplicate = menu.addAction(tr("Duplicate"));
    auto* remove = menu.addAction(tr("Delete"));
    remove->setObjectName("DeleteModulationSource");
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
    if (value) for (auto* panel : window()->findChildren<RackModulationPanel*>())
        if (panel != this && panel->assigning()) { panel->setAssigning(false); panel->synchronize(); }
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
void RackModulationPanel::finishDepthGesture(bool cancel) {
    if (m_depthRoute.isEmpty()) return;
    const auto route = std::exchange(m_depthRoute, QString{}).toStdString();
    auto control = m_depthControl; m_depthControl.clear();
    if (cancel) m_controller->setRackModulationDepth(m_channel.toStdString(), route, m_depthBefore);
    else m_controller->commitRackModulationDepth(m_channel.toStdString(), route, m_depthBefore);
    if (control) {
        control->setProperty("modulationDepthDragging", false); control->unsetCursor();
        if (QWidget::mouseGrabber() == control) control->releaseMouse();
        control->update();
    }
    QToolTip::hideText(); emit edited(false); refresh();
}
bool RackModulationPanel::depthEvent(QWidget* widget, QEvent* event) {
    if (!m_depthRoute.isEmpty()) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            finishDepthGesture(true); return true;
        }
        if (widget == m_depthControl) {
            if (event->type() == QEvent::MouseMove) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (!(mouse->buttons() & Qt::LeftButton)) { finishDepthGesture(); return true; }
                const double delta = (m_depthY - mouse->globalPosition().y()) / ((mouse->modifiers() & Qt::ShiftModifier) ? 1500. : 150.);
                m_depthY = mouse->globalPosition().y();
                for (const auto& r : m_controller->rackModulation(m_channel.toStdString()).routes) if (r.id == m_depthRoute.toStdString()) {
                    const auto routeId = r.id;
                    const double depth = std::clamp(r.depth + delta, -1., 1.);
                    m_controller->setRackModulationDepth(m_channel.toStdString(), routeId, depth);
                    QToolTip::showText(mouse->globalPosition().toPoint() + QPoint(12, -26), tr("Depth %1%").arg(depth * 100, 0, 'f', 1), widget);
                    if (auto* knob = qobject_cast<ui::Knob*>(widget))
                        RackParameterBinding::refreshModulation(knob, m_controller, m_channel.toStdString(),
                            widget->property("modulationSlot").toString().toStdString(), widget->property("modulationParameter").toString().toStdString());
                    break;
                }
                refresh(); return true;
            }
            if (event->type() == QEvent::MouseButtonRelease) { finishDepthGesture(); return true; }
            if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse ||
                event->type() == QEvent::WindowDeactivate || event->type() == QEvent::FocusOut) finishDepthGesture();
        }
    }
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::KeyPress && event->type() != QEvent::ContextMenu) return false;
    if (!isVisible() || m_assigning || !widget || !widget->isEnabled() ||
        !widget->property("modulationEligible").toBool() || widget->property("modulationChannel").toString() != m_channel)
        return false;
    QWidget* scope = widget->parentWidget();
    while (scope && !scope->property("modulationDepthEditing").toBool()) scope = scope->parentWidget();
    if (!scope || scope != m_rack || scope->property("modulationSource").toString() != m_selected) return false;
    const auto slot = widget->property("modulationSlot").toString().toStdString();
    const auto parameter = widget->property("modulationParameter").toString().toStdString();
    const auto& routes = m_controller->rackModulation(m_channel.toStdString()).routes;
    const auto found = std::find_if(routes.begin(), routes.end(), [&](const auto& r) {
        return r.slotId == slot && r.parameterId == parameter && r.sourceId == m_selected.toStdString();
    });
    if (found == routes.end()) return false;
    const auto route = *found;
    if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        m_depthControl = widget; m_depthRoute = QString::fromStdString(route.id); m_depthBefore = route.depth;
        m_depthY = static_cast<QMouseEvent*>(event)->globalPosition().y();
        widget->setProperty("modulationDepthDragging", true); widget->setCursor(Qt::SizeVerCursor);
        widget->setFocus(Qt::MouseFocusReason); widget->grabMouse(); return true;
    }
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            const double step = (key->modifiers() & Qt::ShiftModifier) ? .001 : .01;
            m_controller->setRackModulationDepth(m_channel.toStdString(), route.id, route.depth + (key->key() == Qt::Key_Up ? step : -step));
            m_controller->commitRackModulationDepth(m_channel.toStdString(), route.id, route.depth);
            emit edited(false); refresh(); return true;
        }
    }
    if (event->type() == QEvent::ContextMenu) {
        QMenu menu(this);
        const auto status = m_controller->rackModulationStatus(m_channel.toStdString(), route.id);
        if (!status.empty()) { menu.addAction(QString::fromStdString(status))->setEnabled(false); menu.addSeparator(); }
        auto* enabled = menu.addAction(tr("Enable assignment")); enabled->setCheckable(true); enabled->setChecked(route.enabled);
        auto* bipolar = menu.addAction(tr("Two-sided movement")); bipolar->setCheckable(true); bipolar->setChecked(route.bipolar);
        auto* automateAction = menu.addAction(tr("Automate depth")); menu.addSeparator();
        auto* remove = menu.addAction(tr("Remove assignment"));
        const auto action = menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        if (action == automateAction) automate(route.id, true);
        else if (action == remove) change([&](auto& rack) { std::erase_if(rack.routes, [&](const auto& r) { return r.id == route.id; }); });
        else if (action == enabled || action == bipolar) change([&](auto& rack) {
            for (auto& r : rack.routes) if (r.id == route.id) {
                if (action == enabled) r.enabled = enabled->isChecked(); else r.bipolar = bipolar->isChecked();
            }
        });
        return true;
    }
    return false;
}
bool RackModulationPanel::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(object);
        if (widget && isAncestorOf(widget)) for (auto* parent = widget; parent && parent != this; parent = parent->parentWidget()) {
            if (parent->objectName() != "RackModulatorCard") continue;
            const auto id = parent->property("sourceId").toString();
            if (id != m_selected) {
                setAssigning(false); m_selected = id; QSettings().setValue(key("selected"), id); synchronize(); highlight();
            }
            break;
        }
    }
    if (object == this && event->type() == QEvent::LanguageChange)
        QTimer::singleShot(0, this, [this] { rebuild(); });
    if (m_consumed && object == m_consumed && (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)) { m_consumed.clear(); return true; }
    if (depthEvent(qobject_cast<QWidget*>(object), event)) return true;
    if (!isVisible() || !m_assigning) return false;
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) { setAssigning(false); synchronize(); return true; }
    const bool click = event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton;
    const bool keyboard = event->type() == QEvent::KeyPress && (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return || static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space);
    if (!click && !keyboard) return false;
    auto* widget = qobject_cast<QWidget*>(object);
    for (auto* control = widget; control && control != m_rack; control = control->parentWidget()) {
        if (!control->property("modulationEligible").toBool() || control->property("modulationChannel").toString() != m_channel) continue;
        if (!m_slot.isEmpty() && control->property("modulationSlot").toString() != m_slot) continue;
        if (click) m_consumed = widget;
        assign(control->property("modulationSlot").toString(), control->property("modulationParameter").toString()); return true;
    }
    return false;
}
