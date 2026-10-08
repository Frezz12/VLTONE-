#include "PitchCorrectorPanel.hpp"
#include "EngineController.hpp"
#include "Internal/PitchCorrectorInstance.hpp"
#include "Theme.hpp"
#include "NativePluginView.hpp"
#include <QCoreApplication>
#include <QHideEvent>
#include <QShowEvent>
#include <QJsonDocument>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace pitch = daw::plugins::pitch;
namespace {
constexpr auto presetKey = "pitchCorrector/userPresets.v1";
constexpr std::array<const char*, 12> noteNames{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
int allowedNotes(const std::array<double, 12>& values) {
    constexpr std::array<int, 7> scales{0xfff, 0xab5, 0x5ad, 0x9ad, 0xaad, 0x295, 0x4a9};
    if (int(values[5]) == 7) return int(values[10]);
    const int mask = scales[std::size_t(std::clamp(int(values[5]), 0, 6))];
    const int root = std::clamp(int(values[4]), 0, 11);
    return ((mask << root) | (mask >> (12-root))) & 0xfff;
}
QString noteName(double hz, double reference) {
    if (!(hz > 0) || !std::isfinite(hz)) return QStringLiteral("—");
    const int note = int(std::lround(69+12*std::log2(hz/reference)));
    return QString::fromLatin1(noteNames[std::size_t((note%12+12)%12)])+QString::number(note/12-1);
}
}

PitchCorrectorPanel::PitchCorrectorPanel(daw::EngineController* controller, QString channel,
                                       QString insert, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channel(channel.toStdString()), m_insert(insert.toStdString()) {
    setObjectName("PitchCorrectorPanel"); setAccessibleName(tr("VLT Pitch"));
    setMinimumSize(560, 360);
    m_presets = QJsonDocument::fromJson(QSettings().value(presetKey).toByteArray()).object();
    for (unsigned i = 0; i < 12; ++i) m_presetValues[i] = read(i);
    m_view = new NativePluginView(NativePluginView::Kind::Pitch, this);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    m_bridge = new PitchControls(this);
    connect(m_bridge, &PitchControls::snapshot, m_view, &NativePluginView::setSnapshot);
    connect(m_view, &NativePluginView::edit, m_bridge, &PitchControls::edit);
    connect(m_view, &NativePluginView::finish, m_bridge, &PitchControls::finish);
    connect(m_view, &NativePluginView::automate, m_bridge, &PitchControls::automate);
    connect(m_view, &NativePluginView::toggleBypass, m_bridge, &PitchControls::toggleBypass);
    connect(m_view, &NativePluginView::factoryPreset, m_bridge, &PitchControls::factoryPreset);
    connect(m_view, &NativePluginView::loadPreset, m_bridge, &PitchControls::loadPreset);
    connect(m_view, &NativePluginView::deletePreset, m_bridge, &PitchControls::deletePreset);
    m_view->savePreset = [this](const QString& name, bool replace) { return m_bridge->savePreset(name, replace); };
    connect(m_view, &NativePluginView::toggleNote, m_bridge, &PitchControls::toggleNote);
    connect(m_view, &NativePluginView::sendToAll, m_bridge, &PitchControls::sendToAll);
    m_timer = new QTimer(this); m_timer->setObjectName("PitchTelemetryTimer"); m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &PitchCorrectorPanel::refresh);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        if (isVisible()) refresh();
    });
}
PitchCorrectorPanel::~PitchCorrectorPanel() {
    finishAll();

}
bool PitchCorrectorPanel::available() const {
    return m_controller && m_controller->hasInsert(m_channel, m_insert, pitch::PitchCorrectorInstance::uid());
}
double PitchCorrectorPanel::read(unsigned index) const {
    const auto& p = pitch::parameterTable()[index];
    return available() ? m_controller->insertParameter(m_channel, m_insert, p.id) : p.defaultValue;
}
void PitchCorrectorPanel::write(unsigned index, double value) {
    if (index >= 12 || !std::isfinite(value) || !available()) return;
    if (index == 11 && m_controller->liveAudioActivity()) {
        m_status = tr("Stop playback and input monitoring to change quality."); return;
    }
    const auto& p = pitch::parameterTable()[index];
    value = std::clamp(value, p.minValue, p.maxValue);
    if (p.isStepped) value = std::round(value);
    if (!m_gestures[index]) m_gestures[index] = read(index);
    m_controller->setInsertParameter(m_channel, m_insert, p.id, value);
}
void PitchCorrectorPanel::finishGesture(unsigned index) {
    if (index >= 12 || !m_gestures[index]) return;
    if (available() && read(index) != *m_gestures[index]) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert,
            pitch::parameterTable()[index].id, *m_gestures[index], "Change Pitch Correction");
        emit projectEdited();
    }
    m_gestures[index].reset();
}
void PitchCorrectorPanel::finishAll() { for (unsigned i = 0; i < 12; ++i) finishGesture(i); }
void PitchCorrectorPanel::applyFactoryPreset(int index) {
    if (index < 0 || index >= 4 || !available()) return;
    finishAll(); const auto group = m_controller->beginUndoGroup();
    const auto& preset = pitch::factoryPresets()[std::size_t(index)];
    const std::array<double, 3> style{preset.tune, preset.humanize, preset.vibrato};
    for (unsigned i = 0; i < 3; ++i) { write(i, style[i]); finishGesture(i); }
    m_controller->collapseUndo(group, "Apply Pitch Correction Preset");
    for (unsigned i = 0; i < 12; ++i) m_presetValues[i] = read(i);
    m_presetName = QString::fromUtf8(preset.name.data(), qsizetype(preset.name.size()));
    m_userPreset = false; m_status.clear(); refresh();
}
void PitchCorrectorPanel::applyValues(const Values& values, const QString& name) {
    if (!available()) return;
    if (m_controller->liveAudioActivity() && values[11] != read(11)) {
        m_status = tr("Stop playback and input monitoring to load this preset's quality."); refresh(); return;
    }
    finishAll(); const auto group = m_controller->beginUndoGroup();
    for (unsigned i = 0; i < 12; ++i) { write(i, values[i]); finishGesture(i); }
    m_controller->collapseUndo(group, "Apply Pitch Correction Preset");
    m_presetValues = values; m_presetName = name; m_userPreset = true; m_status.clear(); refresh();
}
void PitchCorrectorPanel::refresh() {
    Values values; QVariantList params;
    for (unsigned i = 0; i < 12; ++i) { values[i] = read(i); params.append(i == 0 ? pitch::retuneMilliseconds(values[i]) : values[i]); }
    if (!m_userPreset && std::abs(values[0]-pitch::kDefaultTune) < .001 && values[1] == 0 && values[2] == 0) {
        m_presetName = tr("Default"); m_presetValues = values;
    }
    if (!m_userPreset) for (const auto& p : pitch::factoryPresets()) {
        if (std::abs(values[0]-p.tune) < .001 && std::abs(values[1]-p.humanize) < .001 && std::abs(values[2]-p.vibrato) < .001) {
            m_presetName = QString::fromUtf8(p.name.data(), qsizetype(p.name.size())); m_presetValues = values; break;
        }
    }
    bool modified = false;
    for (unsigned i = 0; i < (m_userPreset ? 12u : 3u); ++i) modified |= std::abs(values[i]-m_presetValues[i]) > .001;
    QVariantMap state{{"available", available()}, {"values", params}, {"mask", allowedNotes(values)}, {"preset", m_presetName},
        {"modified", modified}, {"userPreset", m_userPreset}, {"presets", m_presets.keys()}, {"status", m_status},
        {"busy", m_controller && m_controller->liveAudioActivity()}};
    state["sendToAllLabel"] = tr("Send settings to all other VLT Pitch instances");
    state["theme"] = QVariantMap{
        {"accent", th().accent.name()},
        {"shellTop", mixColors(QColor("#2b2d31"), th().accent, .06).name()},
        {"shellBottom", mixColors(QColor("#24262b"), th().accent, .06).name()}};
    if (available()) {
        const auto telemetry = m_controller->effectMeterSnapshot(m_channel, m_insert);
        const auto* model = m_controller->insertModel(m_channel, m_insert);
        state["active"] = model && !model->bypassed;
        state["input"] = noteName(telemetry.inputHz, values[3]); state["target"] = noteName(telemetry.targetHz, values[3]);
        state["cents"] = std::round(telemetry.correctionCents);
        state["targetNote"] = telemetry.targetHz > 0 ? (int(std::lround(69+12*std::log2(telemetry.targetHz/values[3])))%12+12)%12 : -1;
        state["latency"] = telemetry.latencySamples*1000.0/std::max(1.0, m_controller->sampleRate());
        state["quality"] = telemetry.quality; state["pending"] = telemetry.qualityPending;
    }
    if (state != m_previous) { m_previous = state; emit m_bridge->snapshot(state); }
}
void PitchCorrectorPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); m_previous.clear(); refresh(); m_timer->start(); }
void PitchCorrectorPanel::hideEvent(QHideEvent* e) { m_timer->stop(); finishAll(); QWidget::hideEvent(e); }

PitchControls::PitchControls(PitchCorrectorPanel* panel) : QObject(panel), m_panel(panel) {}
void PitchControls::edit(int index, double value, bool finished) {
    if (index < 0 || index >= 12 || !std::isfinite(value)) return;
    m_panel->m_status.clear(); m_panel->write(unsigned(index), index == 0 ? pitch::tuneFromMilliseconds(value) : value);
    if (finished) { m_panel->finishGesture(unsigned(index)); m_panel->refresh(); }
}
void PitchControls::finish(int index) { if (index >= 0) m_panel->finishGesture(unsigned(index)); m_panel->refresh(); }
void PitchControls::toggleNote(int note) {
    if (note < 0 || note >= 12 || !m_panel->available()) return;
    PitchCorrectorPanel::Values values;
    for (unsigned i = 0; i < 12; ++i) values[i] = m_panel->read(i);
    const int next = allowedNotes(values) ^ (1 << note);
    if (!next) { m_panel->m_status = tr("Keep at least one note enabled."); m_panel->refresh(); return; }
    m_panel->finishAll(); const auto group = m_panel->m_controller->beginUndoGroup();
    m_panel->write(10, next); m_panel->finishGesture(10); m_panel->write(5, 7); m_panel->finishGesture(5);
    m_panel->m_controller->collapseUndo(group, "Change Custom Pitch Scale");
    m_panel->m_status.clear(); m_panel->refresh();
}
void PitchControls::toggleBypass() {
    if (!m_panel->available()) return;
    const auto* model = m_panel->m_controller->insertModel(m_panel->m_channel, m_panel->m_insert);
    if (!model) return;
    m_panel->m_controller->setInsertBypassed(m_panel->m_channel, m_panel->m_insert, !model->bypassed);
    emit m_panel->projectEdited(); m_panel->refresh();
}
void PitchControls::factoryPreset(int index) { m_panel->applyFactoryPreset(index); }
void PitchControls::sendToAll() {
    if (!m_panel->available()) return;
    m_panel->finishAll();
    std::size_t updated = 0;
    const auto result = m_panel->m_controller->copyPitchCorrectorSettings(
        m_panel->m_channel, m_panel->m_insert, &updated);
    m_panel->m_status = !result ? QCoreApplication::translate("PitchSettings", result.message().c_str())
        : updated ? tr("Settings sent to %1 VLT Pitch instance(s).").arg(updated)
                  : tr("All VLT Pitch instances already use these settings.");
    if (result && updated) emit m_panel->projectEdited();
    m_panel->refresh();
}
void PitchControls::loadPreset(const QString& name) {
    if (!m_panel->m_presets.value(name).isObject()) return;
    const auto saved = m_panel->m_presets.value(name).toObject(); PitchCorrectorPanel::Values values;
    for (const auto& p : pitch::parameterTable()) {
        const auto json = saved.value(QString::fromStdString(p.id));
        double v = json.isDouble() && std::isfinite(json.toDouble()) ? json.toDouble() : p.defaultValue;
        v = std::clamp(v, p.minValue, p.maxValue); values[p.index] = p.isStepped ? std::round(v) : v;
    }
    m_panel->applyValues(values, name);
}
QString PitchControls::savePreset(const QString& text, bool replace) {
    if (!m_panel->available()) return tr("Plugin unavailable");
    const auto name = text.trimmed();
    if (name.isEmpty() || name.size() > 48) return tr("Use a name with 1–48 characters.");
    for (const auto& p : pitch::factoryPresets())
        if (!name.compare(QString::fromUtf8(p.name.data(), qsizetype(p.name.size())), Qt::CaseInsensitive))
            return tr("Choose a name other than a factory preset.");
    auto& presets = m_panel->m_presets; QString existing;
    for (auto it = presets.begin(); it != presets.end(); ++it)
        if (!it.key().compare(name, Qt::CaseInsensitive)) { existing = it.key(); break; }
    if (!existing.isEmpty() && !replace) return QStringLiteral("replace");
    if (existing.isEmpty() && presets.size() >= 128) return tr("The preset library is full (128 presets).");
    if (!existing.isEmpty()) presets.remove(existing);
    QJsonObject values;
    for (const auto& p : pitch::parameterTable()) {
        values[QString::fromStdString(p.id)] = m_panel->read(p.index); m_panel->m_presetValues[p.index] = m_panel->read(p.index);
    }
    presets[name] = values; QSettings().setValue(presetKey, QJsonDocument(presets).toJson(QJsonDocument::Compact));
    m_panel->m_presetName = name; m_panel->m_userPreset = true; m_panel->refresh(); return {};
}
void PitchControls::deletePreset(const QString& name) {
    if (!m_panel->m_presets.contains(name)) return;
    m_panel->m_presets.remove(name); QSettings().setValue(presetKey, QJsonDocument(m_panel->m_presets).toJson(QJsonDocument::Compact));
    if (name == m_panel->m_presetName) { m_panel->m_presetName = tr("Custom"); m_panel->m_userPreset = false; }
    m_panel->refresh();
}
void PitchControls::automate(int index) {
    if (index < 0 || index >= 12 || !pitch::parameterTable()[unsigned(index)].isAutomatable) return;
    emit m_panel->automationRequested(QString::fromStdString(pitch::parameterTable()[unsigned(index)].id));
}
