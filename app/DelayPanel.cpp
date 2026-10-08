#include "DelayPanel.hpp"
#include "EngineController.hpp"
#include "NativePluginView.hpp"
#include <QHideEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace delay = daw::plugins::delay;
DelayPanel::DelayPanel(daw::EngineController* controller, QString channel, QString insert, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channel(channel.toStdString()), m_insert(insert.toStdString()) {
    setObjectName("DelayPanel"); setAccessibleName(QStringLiteral("Classic Delay")); setMinimumSize(960, 420);
    m_view = new NativePluginView(NativePluginView::Kind::Delay, this);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    m_bridge = new DelayControls(this);
    connect(m_bridge, &DelayControls::snapshot, m_view, &NativePluginView::setSnapshot);
    connect(m_view, &NativePluginView::edit, this, [this](int i, double v, bool done) { m_bridge->edit(i, v, done); });
    connect(m_view, &NativePluginView::finish, m_bridge, &DelayControls::finish);
    connect(m_view, &NativePluginView::automate, m_bridge, &DelayControls::automate);
    m_timer = new QTimer(this); m_timer->setObjectName("DelayTelemetryTimer"); m_timer->setInterval(34);
    connect(m_timer, &QTimer::timeout, this, [this] { refresh(true); });
}
DelayPanel::~DelayPanel() { finishAll();  }
bool DelayPanel::available() const {
    return m_controller && m_controller->hasInsert(m_channel, m_insert, delay::DelayInstance::uid());
}
double DelayPanel::read(unsigned i) const {
    const auto& p = delay::parameterTable()[i];
    return available() ? m_controller->insertParameter(m_channel, m_insert, p.id) : p.defaultValue;
}
void DelayPanel::write(unsigned i, double v) {
    if (i >= delay::kParameterCount || !std::isfinite(v) || !available()) return;
    const auto& p = delay::parameterTable()[i]; v = std::clamp(v, p.minValue, p.maxValue);
    if (p.isStepped) v = std::round(v);
    if (!m_gestures[i]) m_gestures[i] = read(i);
    m_controller->setInsertParameter(m_channel, m_insert, p.id, v);
}
void DelayPanel::finish(unsigned i) {
    if (i >= delay::kParameterCount || !m_gestures[i]) return;
    if (available() && read(i) != *m_gestures[i]) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert, delay::parameterTable()[i].id, *m_gestures[i], "Change Classic Delay");
        emit projectEdited();
    }
    m_gestures[i].reset();
}
void DelayPanel::finishAll() { for (unsigned i = 0; i < delay::kParameterCount; ++i) finish(i); }
void DelayPanel::refresh(bool meters) {
    if (!isVisible()) return;
    QVariantList values;
    std::array<double, delay::kParameterCount> p{};
    for (const auto& info : delay::parameterTable()) {
        p[info.index] = read(info.index); values.append(p[info.index]);
    }
    daw::engine::TransportInfo transport;
    if (m_controller) {
        transport.tempo = m_controller->tempo();
        transport.timeSigNumerator = m_controller->project().timeSigNumerator;
        transport.timeSigDenominator = m_controller->project().timeSigDenominator;
    }
    const auto time = delay::timing(p, transport);
    if (meters) {
        const auto t = available() ? m_controller->effectMeterSnapshot(m_channel, m_insert) : daw::EffectMeterSnapshot{};
        m_meters[0] = std::max(double(t.input), m_meters[0] * .88);
        m_meters[1] = std::max(double(t.wet), m_meters[1] * .88);
        m_meters[2] = std::max(double(t.output), m_meters[2] * .88);
    }
    const auto* model = m_controller ? m_controller->insertModel(m_channel, m_insert) : nullptr;
    emit m_bridge->snapshot({{"available", available()}, {"values", values},
        {"playing", m_controller && m_controller->isPlaying()}, {"active", model && !model->bypassed},
        {"milliseconds", time.milliseconds}, {"bpm", time.bpm}, {"limited", time.requestedMs > 8000},
        {"input", m_meters[0]}, {"wet", m_meters[1]}, {"output", m_meters[2]}});
}
void DelayPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); refresh(); m_timer->start(); }
void DelayPanel::hideEvent(QHideEvent* e) { m_timer->stop(); finishAll(); QWidget::hideEvent(e); }
DelayControls::DelayControls(DelayPanel* panel) : QObject(panel), m_panel(panel) {}
void DelayControls::edit(int i, double v, bool finished) {
    if (i < 0 || i >= int(delay::kParameterCount) || !std::isfinite(v)) return;
    m_panel->write(unsigned(i), v);
    if (finished) m_panel->finish(unsigned(i));
    m_panel->refresh();
}
void DelayControls::finish(int i) { if (i >= 0) m_panel->finish(unsigned(i)); m_panel->refresh(); }
void DelayControls::automate(int i) {
    if (i < 0 || i >= int(delay::kParameterCount)) return;
    m_panel->finishAll(); emit m_panel->automationRequested(QString::fromStdString(delay::parameterTable()[unsigned(i)].id));
}
