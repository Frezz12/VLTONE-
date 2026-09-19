#include "PitchCorrectorPanel.hpp"
#include "EngineController.hpp"
#include "Internal/PitchCorrectorInstance.hpp"
#include "Theme.hpp"
#include "graphics/BrowserSurface.hpp"
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
    m_profile = new ui::graphics::BrowserProfile(this, nullptr, false);
    m_view = new ui::graphics::BrowserSurface(m_profile, this);
    m_view->setObjectName("PitchWebView"); m_view->setProperty("dawWebInput", true);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    auto* page = m_view->page();
    // QWidget's host mask clips the outside corners. Its Chromium backing
    // surface must be opaque to avoid exposing stale pixels on partial paints.
    // Quick composites in one scene and needs alpha for the HTML corner shape.
    page->setBackgroundColor(page->isQuick() ? QColor(Qt::transparent) :
        mixColors(QColor("#24262b"), th().accent, .06));
    page->setAudioMuted(true);
    page->navigationPolicy = [](const QUrl& url, bool) {
        return url.scheme() == QStringLiteral("qrc") && url.host().isEmpty() &&
               url.path() == QStringLiteral("/vlt/pitch/index.html");
    };
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->setWebAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    m_bridge = new PitchWebBridge(this); page->setWebChannelObject("pitch", m_bridge);
    connect(page, &ui::graphics::BrowserPage::loadStarted, this, [this] { m_ready = false; finishAll(); });
    m_timer = new QTimer(this); m_timer->setObjectName("PitchTelemetryTimer"); m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &PitchCorrectorPanel::refresh);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        if (!m_view->page()->isQuick())
            m_view->page()->setBackgroundColor(mixColors(QColor("#24262b"), th().accent, .06));
        if (isVisible()) refresh();
    });
    page->load(QUrl(QStringLiteral("qrc:/vlt/pitch/index.html")));
}
PitchCorrectorPanel::~PitchCorrectorPanel() {
    finishAll();
    // The Chromium profile must outlive all pages, including Quick items.
    delete m_view; m_view = nullptr;
    delete m_profile; m_profile = nullptr;
}
pitch::PitchCorrectorInstance* PitchCorrectorPanel::instance() const {
    return m_controller ? dynamic_cast<pitch::PitchCorrectorInstance*>(m_controller->insertInstance(m_channel, m_insert)) : nullptr;
}
double PitchCorrectorPanel::read(unsigned index) const {
    const auto& p = pitch::parameterTable()[index];
    return instance() ? m_controller->insertParameter(m_channel, m_insert, p.id) : p.defaultValue;
}
void PitchCorrectorPanel::write(unsigned index, double value) {
    if (index >= 12 || !std::isfinite(value) || !instance()) return;
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
    if (instance() && read(index) != *m_gestures[index]) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert,
            pitch::parameterTable()[index].id, *m_gestures[index], "Change Pitch Correction");
        emit projectEdited();
    }
    m_gestures[index].reset();
}
void PitchCorrectorPanel::finishAll() { for (unsigned i = 0; i < 12; ++i) finishGesture(i); }
void PitchCorrectorPanel::applyFactoryPreset(int index) {
    if (index < 0 || index >= 4 || !instance()) return;
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
    if (!instance()) return;
    if (m_controller->liveAudioActivity() && values[11] != read(11)) {
        m_status = tr("Stop playback and input monitoring to load this preset's quality."); refresh(); return;
    }
    finishAll(); const auto group = m_controller->beginUndoGroup();
    for (unsigned i = 0; i < 12; ++i) { write(i, values[i]); finishGesture(i); }
    m_controller->collapseUndo(group, "Apply Pitch Correction Preset");
    m_presetValues = values; m_presetName = name; m_userPreset = true; m_status.clear(); refresh();
}
void PitchCorrectorPanel::refresh() {
    if (!m_ready) return;
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
    QVariantMap state{{"values", params}, {"mask", allowedNotes(values)}, {"preset", m_presetName},
        {"modified", modified}, {"userPreset", m_userPreset}, {"presets", m_presets.keys()}, {"status", m_status},
        {"busy", m_controller && m_controller->liveAudioActivity()}};
    state["sendToAllLabel"] = tr("Send settings to all other VLT Pitch instances");
    state["theme"] = QVariantMap{
        {"accent", th().accent.name()},
        {"shellTop", mixColors(QColor("#2b2d31"), th().accent, .06).name()},
        {"shellBottom", mixColors(QColor("#24262b"), th().accent, .06).name()}};
    if (auto* p = instance()) {
        const auto telemetry = p->telemetrySnapshot();
        const auto* model = m_controller->insertModel(m_channel, m_insert);
        state["active"] = model && !model->bypassed;
        state["input"] = noteName(telemetry.inputHz, values[3]); state["target"] = noteName(telemetry.targetHz, values[3]);
        state["cents"] = std::round(telemetry.correctionCents);
        state["targetNote"] = telemetry.targetHz > 0 ? (int(std::lround(69+12*std::log2(telemetry.targetHz/values[3])))%12+12)%12 : -1;
        state["latency"] = p->latencySamples()*1000.0/std::max(1.0, m_controller->sampleRate());
        state["quality"] = p->activeQuality(); state["pending"] = p->qualityChangePending();
    }
    if (state != m_previous) { m_previous = state; emit m_bridge->snapshot(state); }
}
void PitchCorrectorPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); m_previous.clear(); refresh(); m_timer->start(); }
void PitchCorrectorPanel::hideEvent(QHideEvent* e) { m_timer->stop(); finishAll(); QWidget::hideEvent(e); }

PitchWebBridge::PitchWebBridge(PitchCorrectorPanel* panel) : QObject(panel), m_panel(panel) {}
void PitchWebBridge::ready() { m_panel->m_ready = true; m_panel->m_previous.clear(); m_panel->refresh(); }
void PitchWebBridge::edit(int index, double value, bool finished) {
    if (index < 0 || index >= 12 || !std::isfinite(value)) return;
    m_panel->m_status.clear(); m_panel->write(unsigned(index), index == 0 ? pitch::tuneFromMilliseconds(value) : value);
    if (finished) { m_panel->finishGesture(unsigned(index)); m_panel->refresh(); }
}
void PitchWebBridge::finish(int index) { if (index >= 0) m_panel->finishGesture(unsigned(index)); m_panel->refresh(); }
void PitchWebBridge::toggleNote(int note) {
    if (note < 0 || note >= 12 || !m_panel->instance()) return;
    PitchCorrectorPanel::Values values;
    for (unsigned i = 0; i < 12; ++i) values[i] = m_panel->read(i);
    const int next = allowedNotes(values) ^ (1 << note);
    if (!next) { m_panel->m_status = tr("Keep at least one note enabled."); m_panel->refresh(); return; }
    m_panel->finishAll(); const auto group = m_panel->m_controller->beginUndoGroup();
    m_panel->write(10, next); m_panel->finishGesture(10); m_panel->write(5, 7); m_panel->finishGesture(5);
    m_panel->m_controller->collapseUndo(group, "Change Custom Pitch Scale");
    m_panel->m_status.clear(); m_panel->refresh();
}
void PitchWebBridge::toggleBypass() {
    if (!m_panel->instance()) return;
    const auto* model = m_panel->m_controller->insertModel(m_panel->m_channel, m_panel->m_insert);
    if (!model) return;
    m_panel->m_controller->setInsertBypassed(m_panel->m_channel, m_panel->m_insert, !model->bypassed);
    emit m_panel->projectEdited(); m_panel->refresh();
}
void PitchWebBridge::factoryPreset(int index) { m_panel->applyFactoryPreset(index); }
void PitchWebBridge::sendToAll() {
    if (!m_panel->instance()) return;
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
void PitchWebBridge::loadPreset(const QString& name) {
    if (!m_panel->m_presets.value(name).isObject()) return;
    const auto saved = m_panel->m_presets.value(name).toObject(); PitchCorrectorPanel::Values values;
    for (const auto& p : pitch::parameterTable()) {
        const auto json = saved.value(QString::fromStdString(p.id));
        double v = json.isDouble() && std::isfinite(json.toDouble()) ? json.toDouble() : p.defaultValue;
        v = std::clamp(v, p.minValue, p.maxValue); values[p.index] = p.isStepped ? std::round(v) : v;
    }
    m_panel->applyValues(values, name);
}
QString PitchWebBridge::savePreset(const QString& text, bool replace) {
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
void PitchWebBridge::deletePreset(const QString& name) {
    if (!m_panel->m_presets.contains(name)) return;
    m_panel->m_presets.remove(name); QSettings().setValue(presetKey, QJsonDocument(m_panel->m_presets).toJson(QJsonDocument::Compact));
    if (name == m_panel->m_presetName) { m_panel->m_presetName = tr("Custom"); m_panel->m_userPreset = false; }
    m_panel->refresh();
}
void PitchWebBridge::automate(int index) {
    if (index < 0 || index >= 12 || !pitch::parameterTable()[unsigned(index)].isAutomatable) return;
    emit m_panel->automationRequested(QString::fromStdString(pitch::parameterTable()[unsigned(index)].id));
}
