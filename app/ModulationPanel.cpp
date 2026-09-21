#include "ModulationPanel.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include "graphics/BrowserSurface.hpp"
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
    const bool rack = dynamic_cast<Rack*>(instance());
    setMinimumSize(rack ? 800 : 360, 490);
    m_presets = QJsonDocument::fromJson(QSettings().value(settingsKey()).toByteArray()).object()["presets"].toArray();
    m_profile = new ui::graphics::BrowserProfile(this, nullptr, false);
    m_view = new ui::graphics::BrowserSurface(m_profile, this);
    m_view->setObjectName("ModulationWebView"); m_view->setProperty("dawWebInput", true);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    auto* page = m_view->page();
    page->setBackgroundColor(QColor("#171c20")); page->setAudioMuted(true);
    page->navigationPolicy = [](const QUrl& url, bool) {
        return url.scheme() == "qrc" && url.host().isEmpty() && url.path() == "/vlt/modulation/index.html";
    };
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->setWebAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    m_bridge = new ModulationWebBridge(this); page->setWebChannelObject("modulation", m_bridge);
    connect(page, &ui::graphics::BrowserPage::loadStarted, this, [this] { m_ready = false; finishAll(); });
    m_timer = new QTimer(this); m_timer->setObjectName("ModulationTelemetryTimer"); m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &ModulationPanel::refresh);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &ModulationPanel::refresh);
    page->load(QUrl("qrc:/vlt/modulation/index.html"));
}
ModulationPanel::~ModulationPanel() { finishAll(); delete m_view; delete m_profile; }
daw::plugins::PluginInstance* ModulationPanel::instance() const {
    auto* p = m_controller ? m_controller->insertInstance(m_channel, m_insert) : nullptr;
    return dynamic_cast<Rack*>(p) || dynamic_cast<mod::ModulationInstance*>(p) ? p : nullptr;
}
void ModulationPanel::write(unsigned index, double value) {
    auto* p = instance();
    if (!p || index >= p->parameters().size() || !std::isfinite(value)) return;
    const auto& info = p->parameters()[index];
    value = std::clamp(value, info.minValue, info.maxValue);
    if (info.isStepped) value = std::round(value);
    if (!m_gestures[index]) m_gestures[index] = m_controller->insertParameter(m_channel, m_insert, info.id);
    m_controller->setInsertParameter(m_channel, m_insert, info.id, value);
}
void ModulationPanel::finishAll() {
    if (auto* p = instance()) {
        const auto group = m_controller->beginUndoGroup();
        bool changed = false;
        for (const auto& info : p->parameters()) if (m_gestures[info.index]) {
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
    return (instance() ? QString::fromStdString(instance()->descriptor().uid) : QStringLiteral("daw.modulation")) + "/userPresets.v1";
}
void ModulationPanel::storePresets() {
    QSettings().setValue(settingsKey(), QJsonDocument(QJsonObject{{"version", 1}, {"presets", m_presets}}).toJson(QJsonDocument::Compact));
}
void ModulationPanel::applyValues(const QJsonObject& values, const QString& name) {
    auto* p = instance(); if (!p) return;
    finishAll();
    for (const auto& info : p->parameters()) write(info.index, values.value(QString::fromStdString(info.id)).toDouble(info.defaultValue));
    finishAll(); m_presetName = name; m_presetValues = values;
    if (auto* single = dynamic_cast<mod::ModulationInstance*>(p)) single->setPresetReference("user", name.toStdString());
    refresh();
}
void ModulationPanel::applyFactoryPreset(int index) {
    auto* p = instance(); if (!p) return;
    QJsonObject values; QString name = tr("Init");
    if (auto* single = dynamic_cast<mod::ModulationInstance*>(p)) {
        const auto presets = mod::factoryPresets(single->kind());
        if (index < 0 || index >= int(presets.size())) return;
        const auto& preset = presets[std::size_t(index)];
        name = QString::fromUtf8(preset.name.data(), qsizetype(preset.name.size()));
        for (const auto& info : p->parameters()) values[QString::fromStdString(info.id)] = preset.values[info.index];
    } else {
        if (index != 0) return;
        for (const auto& info : p->parameters()) values[QString::fromStdString(info.id)] = info.defaultValue;
    }
    applyValues(values, name);
    if (auto* single = dynamic_cast<mod::ModulationInstance*>(p)) single->setPresetReference("factory", name.toStdString());
}
void ModulationPanel::refresh() {
    if (!m_ready) return;
    auto* p = instance();
    if (!p) { emit m_bridge->snapshot({{"available", false}, {"playing", false}}); return; }
    auto* rack = dynamic_cast<Rack*>(p);
    auto* single = dynamic_cast<mod::ModulationInstance*>(p);
    QVariantList params, values, order, factories;
    QJsonObject current;
    for (const auto& info : p->parameters()) {
        const auto value = m_controller->insertParameter(m_channel, m_insert, info.id);
        values.append(value); current[QString::fromStdString(info.id)] = value;
        params.append(QVariantMap{{"id", QString::fromStdString(info.id)}, {"name", QString::fromStdString(info.name)},
            {"unit", QString::fromStdString(info.unit)}, {"min", info.minValue}, {"max", info.maxValue}, {"default", info.defaultValue}});
    }
    if (rack) for (auto m : Rack::decodeOrder(unsigned(p->parameterValue(Rack::orderParameter)))) order.append(int(m));
    else order.append(single->kind() == mod::Kind::DoublerPro ? 4 : single->kind() == mod::Kind::Doubler ? 1 : single->kind() == mod::Kind::Chorus ? 0 : int(single->kind()));
    if (single) {
        for (const auto& preset : mod::factoryPresets(single->kind())) {
            const auto name = QString::fromUtf8(preset.name.data(), qsizetype(preset.name.size()));
            factories.append(name); bool matches = true;
            for (const auto& info : p->parameters()) matches &= std::abs(values[info.index].toDouble() - preset.values[info.index]) < 1.e-6;
            if (matches && (m_presetName.isEmpty() || current != m_presetValues)) { m_presetName = name; m_presetValues = current; }
        }
    } else {
        factories.append(tr("Init"));
        bool initial = true;
        for (const auto& info : p->parameters()) initial &= values[info.index].toDouble() == info.defaultValue;
        if (initial) { m_presetName = tr("Init"); m_presetValues = current; }
    }
    for (const auto& v : m_presets) {
        const auto preset = v.toObject();
        if (preset["params"].toObject() == current) { m_presetName = preset["name"].toString(); m_presetValues = current; break; }
    }
    const auto* model = m_controller->insertModel(m_channel, m_insert);
    QVariantMap state{{"available", true}, {"rack", rack != nullptr}, {"order", order}, {"values", values}, {"params", params},
        {"title", QString::fromStdString(p->descriptor().name)}, {"factories", factories}, {"presets", m_presets.toVariantList()},
        {"preset", m_presetName.isEmpty() ? tr("Custom") : m_presetName}, {"modified", current != m_presetValues},
        {"active", model && !model->bypassed}, {"playing", isVisible() && m_controller->isPlaying() && model && !model->bypassed},
        {"accent", th().accent.name()}, {"reduced", QSettings().value("ui/modulationReduceMotion", false).toBool()}};
    if (rack) {
        const auto oldValues = m_previous.value("values").toList();
        bool changed = oldValues.size() != values.size();
        for (int i = Rack::eqOffset; !changed && i < values.size(); ++i) changed |= values[i] != oldValues[i];
        if (changed || !m_previous.contains("response")) {
            QVariantList response;
            for (int i = 0; i < 180; ++i) response.append(rack->equalizer().responseDb(20 * std::pow(1000., double(i) / 179)));
            state["response"] = response;
        } else state["response"] = m_previous["response"];
    }
    if (state != m_previous) { m_previous = state; emit m_bridge->snapshot(state); }
}
bool ModulationPanel::visualUpdatesActive() const { return m_timer->isActive(); }
void ModulationPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); m_previous.clear(); refresh(); m_timer->start(); }
void ModulationPanel::hideEvent(QHideEvent* e) {
    m_timer->stop(); finishAll();
    if (m_ready) { m_previous["playing"] = false; emit m_bridge->snapshot(m_previous); }
    QWidget::hideEvent(e);
}
ModulationWebBridge::ModulationWebBridge(ModulationPanel* panel) : QObject(panel), m_panel(panel) {}
void ModulationWebBridge::ready() { m_panel->m_ready = true; m_panel->m_previous.clear(); m_panel->refresh(); }
void ModulationWebBridge::edit(int index, double value, bool finished) {
    if (index < 0) return;
    m_panel->write(unsigned(index), value);
    if (finished) { m_panel->finishAll(); m_panel->refresh(); }
}
void ModulationWebBridge::finish() { m_panel->finishAll(); m_panel->refresh(); }
void ModulationWebBridge::reorder(int from, int to) {
    auto* rack = dynamic_cast<Rack*>(m_panel->instance());
    if (!rack || from < 0 || from > 3 || to < 0 || to > 3) return;
    auto order = Rack::decodeOrder(unsigned(rack->parameterValue(Rack::orderParameter)));
    const auto item = order[std::size_t(from)];
    if (from < to) std::move(order.begin() + from + 1, order.begin() + to + 1, order.begin() + from);
    else std::move_backward(order.begin() + to, order.begin() + from, order.begin() + from + 1);
    order[std::size_t(to)] = item; edit(Rack::orderParameter, Rack::encodeOrder(order), true);
}
void ModulationWebBridge::toggleBypass() {
    const auto* model = m_panel->m_controller->insertModel(m_panel->m_channel, m_panel->m_insert);
    if (!model) return;
    m_panel->m_controller->setInsertBypassed(m_panel->m_channel, m_panel->m_insert, !model->bypassed);
    emit m_panel->projectEdited(); m_panel->refresh();
}
void ModulationWebBridge::factoryPreset(int index) { m_panel->applyFactoryPreset(index); }
void ModulationWebBridge::loadPreset(const QString& name) {
    for (const auto& v : m_panel->m_presets) if (v.toObject()["name"].toString() == name) {
        m_panel->applyValues(v.toObject()["params"].toObject(), name); return;
    }
}
QString ModulationWebBridge::savePreset(const QString& text, bool replace) {
    auto* p = m_panel->instance(); if (!p) return tr("Plugin unavailable");
    const auto name = text.trimmed();
    if (name.isEmpty() || name.size() > 48) return tr("Use a name with 1–48 characters.");
    if (auto* single = dynamic_cast<mod::ModulationInstance*>(p))
        for (const auto& preset : mod::factoryPresets(single->kind()))
            if (!name.compare(QString::fromUtf8(preset.name.data(), qsizetype(preset.name.size())), Qt::CaseInsensitive))
                return tr("Choose a name different from the factory presets.");
    int found = -1;
    for (int i = 0; i < m_panel->m_presets.size(); ++i)
        if (!m_panel->m_presets[i].toObject()["name"].toString().compare(name, Qt::CaseInsensitive)) found = i;
    if (found >= 0 && !replace) return tr("A preset with this name exists. Use Replace to overwrite it.");
    if (found < 0 && m_panel->m_presets.size() >= 128) return tr("Up to 128 user presets are supported.");
    QJsonObject values;
    for (const auto& info : p->parameters()) values[QString::fromStdString(info.id)] = m_panel->m_controller->insertParameter(m_panel->m_channel, m_panel->m_insert, info.id);
    const QJsonObject preset{{"name", name}, {"params", values}};
    if (found >= 0) m_panel->m_presets[found] = preset; else m_panel->m_presets.append(preset);
    m_panel->storePresets(); m_panel->m_presetName = name; m_panel->m_presetValues = values;
    if (auto* single = dynamic_cast<mod::ModulationInstance*>(p)) single->setPresetReference("user", name.toStdString());
    emit m_panel->projectEdited(); m_panel->refresh(); return {};
}
QString ModulationWebBridge::renamePreset(const QString& name, const QString& text) {
    const auto next = text.trimmed();
    if (next.isEmpty() || next.size() > 48) return tr("Use a name with 1–48 characters.");
    for (const auto& v : m_panel->m_presets) if (!v.toObject()["name"].toString().compare(next, Qt::CaseInsensitive)) return tr("Name already exists.");
    for (int i = 0; i < m_panel->m_presets.size(); ++i) if (m_panel->m_presets[i].toObject()["name"].toString() == name) {
        auto preset = m_panel->m_presets[i].toObject(); preset["name"] = next; m_panel->m_presets[i] = preset;
        if (auto* single = dynamic_cast<mod::ModulationInstance*>(m_panel->instance()))
            single->setPresetReference("user", next.toStdString());
        m_panel->storePresets(); m_panel->m_presetName = next; m_panel->refresh(); return {};
    }
    return tr("Select a user preset first.");
}
void ModulationWebBridge::deletePreset(const QString& name) {
    for (int i = 0; i < m_panel->m_presets.size(); ++i) if (m_panel->m_presets[i].toObject()["name"].toString() == name) {
        m_panel->m_presets.removeAt(i); m_panel->storePresets(); m_panel->m_presetName.clear(); m_panel->refresh(); return;
    }
}
void ModulationWebBridge::automate(int index) {
    const auto* p = m_panel->instance();
    if (p && index >= 0 && index < int(p->parameters().size())) emit m_panel->automationRequested(QString::fromStdString(p->parameters()[std::size_t(index)].id));
}
void ModulationWebBridge::setEqExpanded(bool expanded) {
    if (dynamic_cast<Rack*>(m_panel->instance())) emit m_panel->eqExpansionChanged(expanded);
}
