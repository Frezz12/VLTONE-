#include "ModulationPanel.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include "NativePluginView.hpp"
#include <QHideEvent>
#include <QShowEvent>
#include <QJsonDocument>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace mod = daw::plugins::modulation;
using Rack = mod::ModulationRackInstance;
ModulationPanel::ModulationPanel(daw::EngineController* c, QString channel, QString insert,
                                 mod::Kind, QWidget* parent) : ModulationPanel(c, channel, insert, parent) {}
ModulationPanel::ModulationPanel(daw::EngineController* c, QString channel, QString insert, QWidget* parent)
    : QWidget(parent), m_controller(c), m_channel(channel.toStdString()), m_insert(insert.toStdString()) {
    setObjectName("ModulationPanel"); setAccessibleName(tr("Modulation"));
    if (const auto* model = c ? c->insertModel(m_channel, m_insert) : nullptr) {
        m_uid = model->uid;
        m_rack = m_uid == Rack::staticDescriptor().uid;
        if (m_rack) m_title = Rack::staticDescriptor().name;
        else for (int i = 0; i < mod::kindCount; ++i) {
            const auto kind = mod::Kind(i);
            if (m_uid == mod::descriptorFor(kind).uid) {
                m_kind = kind; m_title = mod::descriptorFor(kind).name; break;
            }
        }
    }
    setMinimumSize(m_rack ? 800 : 360, 490);
    m_presets = QJsonDocument::fromJson(QSettings().value(settingsKey()).toByteArray()).object()["presets"].toArray();
    m_view = new NativePluginView(NativePluginView::Kind::Modulation, this);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    m_bridge = new ModulationControls(this);
    connect(m_bridge, &ModulationControls::snapshot, m_view, &NativePluginView::setSnapshot);
    connect(m_view, &NativePluginView::edit, m_bridge, &ModulationControls::edit);
    connect(m_view, &NativePluginView::finish, this, [this](int) { m_bridge->finish(); });
    connect(m_view, &NativePluginView::automate, m_bridge, &ModulationControls::automate);
    connect(m_view, &NativePluginView::toggleBypass, m_bridge, &ModulationControls::toggleBypass);
    connect(m_view, &NativePluginView::factoryPreset, m_bridge, &ModulationControls::factoryPreset);
    connect(m_view, &NativePluginView::loadPreset, m_bridge, &ModulationControls::loadPreset);
    connect(m_view, &NativePluginView::deletePreset, m_bridge, &ModulationControls::deletePreset);
    m_view->savePreset = [this](const QString& name, bool replace) { return m_bridge->savePreset(name, replace); };
    connect(m_view, &NativePluginView::reorder, m_bridge, &ModulationControls::reorder);
    connect(m_view, &NativePluginView::eqExpanded, m_bridge, &ModulationControls::setEqExpanded);
    m_view->renamePreset = [this](const QString& name, const QString& next) { return m_bridge->renamePreset(name, next); };
    m_timer = new QTimer(this); m_timer->setObjectName("ModulationTelemetryTimer"); m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &ModulationPanel::refresh);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &ModulationPanel::refresh);
}
ModulationPanel::~ModulationPanel() { finishAll();  }
bool ModulationPanel::available() const {
    return m_controller && (m_rack || m_kind) && m_controller->hasInsert(m_channel, m_insert, m_uid);
}
std::span<const daw::plugins::ParameterInfo> ModulationPanel::parameters() const {
    return m_rack ? Rack::parameterTable() : m_kind ? mod::parameterTable(*m_kind)
                                                   : std::span<const daw::plugins::ParameterInfo>{};
}
void ModulationPanel::write(unsigned index, double value) {
    if (!available() || index >= parameters().size() || !std::isfinite(value)) return;
    const auto& info = parameters()[index];
    value = std::clamp(value, info.minValue, info.maxValue);
    if (info.isStepped) value = std::round(value);
    if (!m_gestures[index]) m_gestures[index] = m_controller->insertParameter(m_channel, m_insert, info.id);
    m_controller->setInsertParameter(m_channel, m_insert, info.id, value);
}
void ModulationPanel::finishAll() {
    if (available()) {
        const auto group = m_controller->beginUndoGroup();
        bool changed = false;
        for (const auto& info : parameters()) if (m_gestures[info.index]) {
            changed |= m_controller->insertParameter(m_channel, m_insert, info.id) != *m_gestures[info.index];
            m_controller->commitInsertParameterEdit(m_channel, m_insert, info.id,
                *m_gestures[info.index], "Change Modulation Parameter");
        }
        m_controller->collapseUndo(group, "Change Modulation Parameter");
        if (changed) emit projectEdited();
    }
    m_gestures = {};
}
QString ModulationPanel::settingsKey() const {
    return (m_uid.empty() ? QStringLiteral("daw.modulation") : QString::fromStdString(m_uid)) + "/userPresets.v1";
}
void ModulationPanel::storePresets() {
    QSettings().setValue(settingsKey(), QJsonDocument(QJsonObject{{"version", 1}, {"presets", m_presets}}).toJson(QJsonDocument::Compact));
}
void ModulationPanel::applyValues(const QJsonObject& values, const QString& name) {
    if (!available()) return;
    finishAll();
    for (const auto& info : parameters()) write(info.index, values.value(QString::fromStdString(info.id)).toDouble(info.defaultValue));
    finishAll(); m_presetName = name; m_presetValues = values;
    if (m_kind) m_controller->setInsertPresetReference(m_channel, m_insert, "user", name.toStdString());
    refresh();
}
void ModulationPanel::applyFactoryPreset(int index) {
    if (!available()) return;
    QJsonObject values; QString name = tr("Init");
    if (m_kind) {
        const auto presets = mod::factoryPresets(*m_kind);
        if (index < 0 || index >= int(presets.size())) return;
        const auto& preset = presets[std::size_t(index)];
        name = QString::fromUtf8(preset.name.data(), qsizetype(preset.name.size()));
        for (const auto& info : parameters()) values[QString::fromStdString(info.id)] = preset.values[info.index];
    } else {
        if (index != 0) return;
        for (const auto& info : parameters()) values[QString::fromStdString(info.id)] = info.defaultValue;
    }
    applyValues(values, name);
    if (m_kind) m_controller->setInsertPresetReference(m_channel, m_insert, "factory", name.toStdString());
}
void ModulationPanel::refresh() {
    if (!available()) { emit m_bridge->snapshot({{"available", false}, {"playing", false}}); return; }
    QVariantList params, values, order, factories;
    QJsonObject current;
    for (const auto& info : parameters()) {
        const auto value = m_controller->insertParameter(m_channel, m_insert, info.id);
        values.append(value); current[QString::fromStdString(info.id)] = value;
        params.append(QVariantMap{{"id", QString::fromStdString(info.id)}, {"name", QString::fromStdString(info.name)},
            {"unit", QString::fromStdString(info.unit)}, {"min", info.minValue}, {"max", info.maxValue}, {"default", info.defaultValue}, {"stepped", info.isStepped}, {"automatable", info.isAutomatable}});
    }
    if (m_rack) for (auto m : Rack::decodeOrder(unsigned(m_controller->insertParameter(m_channel, m_insert, parameters()[Rack::orderParameter].id)))) order.append(int(m));
    else order.append(*m_kind == mod::Kind::DoublerPro ? 4 : *m_kind == mod::Kind::Doubler ? 1 : *m_kind == mod::Kind::Chorus ? 0 : int(*m_kind));
    if (m_kind) {
        for (const auto& preset : mod::factoryPresets(*m_kind)) {
            const auto name = QString::fromUtf8(preset.name.data(), qsizetype(preset.name.size()));
            factories.append(name); bool matches = true;
            for (const auto& info : parameters()) matches &= std::abs(values[info.index].toDouble() - preset.values[info.index]) < 1.e-6;
            if (matches && (m_presetName.isEmpty() || current != m_presetValues)) { m_presetName = name; m_presetValues = current; }
        }
    } else {
        factories.append(tr("Init"));
        bool initial = true;
        for (const auto& info : parameters()) initial &= values[info.index].toDouble() == info.defaultValue;
        if (initial) { m_presetName = tr("Init"); m_presetValues = current; }
    }
    for (const auto& v : m_presets) {
        const auto preset = v.toObject();
        if (preset["params"].toObject() == current) { m_presetName = preset["name"].toString(); m_presetValues = current; break; }
    }
    const auto* model = m_controller->insertModel(m_channel, m_insert);
    QVariantMap state{{"available", true}, {"rack", m_rack}, {"order", order}, {"values", values}, {"params", params},
        {"title", QString::fromStdString(m_title)}, {"factories", factories}, {"presets", m_presets.toVariantList()},
        {"preset", m_presetName.isEmpty() ? tr("Custom") : m_presetName}, {"modified", current != m_presetValues},
        {"active", model && !model->bypassed}, {"playing", isVisible() && m_controller->isPlaying() && model && !model->bypassed},
        {"accent", th().accent.name()}, {"reduced", QSettings().value("ui/modulationReduceMotion", false).toBool()}};
    if (m_rack) {
        const auto oldValues = m_previous.value("values").toList();
        bool changed = oldValues.size() != values.size() || m_responseSampleRate != m_controller->sampleRate();
        for (int i = Rack::eqOffset; !changed && i < values.size(); ++i) changed |= values[i] != oldValues[i];
        if (changed || !m_previous.contains("response")) {
            QVariantList response;
            if (const auto computed = m_controller->modulationResponse(m_channel, m_insert))
                for (double value : *computed) response.append(value);
            m_responseSampleRate = m_controller->sampleRate();
            state["response"] = response;
        } else state["response"] = m_previous["response"];
    }
    if (state != m_previous) { m_previous = state; emit m_bridge->snapshot(state); }
}
bool ModulationPanel::visualUpdatesActive() const { return m_timer->isActive(); }
void ModulationPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); m_previous.clear(); refresh(); m_timer->start(); }
void ModulationPanel::hideEvent(QHideEvent* e) {
    m_timer->stop(); finishAll();
    m_previous["playing"] = false; emit m_bridge->snapshot(m_previous);
    QWidget::hideEvent(e);
}
ModulationControls::ModulationControls(ModulationPanel* panel) : QObject(panel), m_panel(panel) {}
void ModulationControls::edit(int index, double value, bool finished) {
    if (index < 0) return;
    m_panel->write(unsigned(index), value);
    if (finished) { m_panel->finishAll(); m_panel->refresh(); }
}
void ModulationControls::finish() { m_panel->finishAll(); m_panel->refresh(); }
void ModulationControls::reorder(int from, int to) {
    if (!m_panel->available() || !m_panel->m_rack || from < 0 || from > 3 || to < 0 || to > 3) return;
    auto order = Rack::decodeOrder(unsigned(m_panel->m_controller->insertParameter(m_panel->m_channel,
        m_panel->m_insert, Rack::parameterTable()[Rack::orderParameter].id)));
    const auto item = order[std::size_t(from)];
    if (from < to) std::move(order.begin() + from + 1, order.begin() + to + 1, order.begin() + from);
    else std::move_backward(order.begin() + to, order.begin() + from, order.begin() + from + 1);
    order[std::size_t(to)] = item; edit(Rack::orderParameter, Rack::encodeOrder(order), true);
}
void ModulationControls::toggleBypass() {
    if (!m_panel->available()) return;
    const auto* model = m_panel->m_controller->insertModel(m_panel->m_channel, m_panel->m_insert);
    if (!model) return;
    m_panel->m_controller->setInsertBypassed(m_panel->m_channel, m_panel->m_insert, !model->bypassed);
    emit m_panel->projectEdited(); m_panel->refresh();
}
void ModulationControls::factoryPreset(int index) { m_panel->applyFactoryPreset(index); }
void ModulationControls::loadPreset(const QString& name) {
    for (const auto& v : m_panel->m_presets) if (v.toObject()["name"].toString() == name) {
        m_panel->applyValues(v.toObject()["params"].toObject(), name); return;
    }
}
QString ModulationControls::savePreset(const QString& text, bool replace) {
    if (!m_panel->available()) return tr("Plugin unavailable");
    const auto name = text.trimmed();
    if (name.isEmpty() || name.size() > 48) return tr("Use a name with 1–48 characters.");
    if (m_panel->m_kind)
        for (const auto& preset : mod::factoryPresets(*m_panel->m_kind))
            if (!name.compare(QString::fromUtf8(preset.name.data(), qsizetype(preset.name.size())), Qt::CaseInsensitive))
                return tr("Choose a name different from the factory presets.");
    int found = -1;
    for (int i = 0; i < m_panel->m_presets.size(); ++i)
        if (!m_panel->m_presets[i].toObject()["name"].toString().compare(name, Qt::CaseInsensitive)) found = i;
    if (found >= 0 && !replace) return tr("A preset with this name exists. Use Replace to overwrite it.");
    if (found < 0 && m_panel->m_presets.size() >= 128) return tr("Up to 128 user presets are supported.");
    QJsonObject values;
    for (const auto& info : m_panel->parameters()) values[QString::fromStdString(info.id)] = m_panel->m_controller->insertParameter(m_panel->m_channel, m_panel->m_insert, info.id);
    const QJsonObject preset{{"name", name}, {"params", values}};
    if (found >= 0) m_panel->m_presets[found] = preset; else m_panel->m_presets.append(preset);
    m_panel->storePresets(); m_panel->m_presetName = name; m_panel->m_presetValues = values;
    if (m_panel->m_kind) m_panel->m_controller->setInsertPresetReference(m_panel->m_channel, m_panel->m_insert, "user", name.toStdString());
    emit m_panel->projectEdited(); m_panel->refresh(); return {};
}
QString ModulationControls::renamePreset(const QString& name, const QString& text) {
    const auto next = text.trimmed();
    if (next.isEmpty() || next.size() > 48) return tr("Use a name with 1–48 characters.");
    for (const auto& v : m_panel->m_presets) if (!v.toObject()["name"].toString().compare(next, Qt::CaseInsensitive)) return tr("Name already exists.");
    for (int i = 0; i < m_panel->m_presets.size(); ++i) if (m_panel->m_presets[i].toObject()["name"].toString() == name) {
        auto preset = m_panel->m_presets[i].toObject(); preset["name"] = next; m_panel->m_presets[i] = preset;
        if (m_panel->m_kind)
            m_panel->m_controller->setInsertPresetReference(m_panel->m_channel, m_panel->m_insert, "user", next.toStdString());
        m_panel->storePresets(); m_panel->m_presetName = next; m_panel->refresh(); return {};
    }
    return tr("Select a user preset first.");
}
void ModulationControls::deletePreset(const QString& name) {
    for (int i = 0; i < m_panel->m_presets.size(); ++i) if (m_panel->m_presets[i].toObject()["name"].toString() == name) {
        m_panel->m_presets.removeAt(i); m_panel->storePresets(); m_panel->m_presetName.clear(); m_panel->refresh(); return;
    }
}
void ModulationControls::automate(int index) {
    const auto parameters = m_panel->parameters();
    if (m_panel->available() && index >= 0 && index < int(parameters.size()))
        emit m_panel->automationRequested(QString::fromStdString(parameters[std::size_t(index)].id));
}
void ModulationControls::setEqExpanded(bool expanded) {
    if (m_panel->available() && m_panel->m_rack) emit m_panel->eqExpansionChanged(expanded);
}
