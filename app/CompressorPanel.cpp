#include "CompressorPanel.hpp"
#include "EngineController.hpp"
#include "NativePluginView.hpp"
#include <QHideEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace comp = daw::plugins::compressor;
CompressorPanel::CompressorPanel(daw::EngineController* controller, QString channel, QString insert, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channel(channel.toStdString()), m_insert(insert.toStdString()) {
    setObjectName("CompressorPanel"); setAccessibleName(tr("Compressor Soft / Punch")); setMinimumSize(800, 340);
    m_view = new NativePluginView(NativePluginView::Kind::Compressor, this);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    m_bridge = new CompressorControls(this);
    connect(m_bridge, &CompressorControls::snapshot, m_view, &NativePluginView::setSnapshot);
    connect(m_view, &NativePluginView::edit, this, [this](int i, double v, bool done) { m_bridge->edit(i, v, done); });
    connect(m_view, &NativePluginView::finish, m_bridge, &CompressorControls::finish);
    connect(m_view, &NativePluginView::automate, m_bridge, &CompressorControls::automate);
    m_timer = new QTimer(this); m_timer->setObjectName("CompressorTelemetryTimer"); m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, [this] { refresh(true); });
}
CompressorPanel::~CompressorPanel() {
    finishAll();
}
bool CompressorPanel::available() const {
    return m_controller && m_controller->hasInsert(m_channel, m_insert, comp::CompressorInstance::uid());
}
double CompressorPanel::read(unsigned index) const {
    const auto& p = comp::parameterTable()[index];
    return available() ? m_controller->insertParameter(m_channel, m_insert, p.id) : p.defaultValue;
}
void CompressorPanel::write(unsigned index, double value) {
    if (index >= comp::kParameterCount || !std::isfinite(value) || !available()) return;
    const auto& p = comp::parameterTable()[index]; value = std::clamp(value, p.minValue, p.maxValue);
    if (p.isStepped) value = std::round(value);
    if (!m_gestures[index]) m_gestures[index] = read(index);
    m_controller->setInsertParameter(m_channel, m_insert, p.id, value);
}
void CompressorPanel::finish(unsigned index) {
    if (index >= comp::kParameterCount || !m_gestures[index]) return;
    if (available() && read(index) != *m_gestures[index]) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert, comp::parameterTable()[index].id,
                                               *m_gestures[index], "Change Compressor");
        emit projectEdited();
    }
    m_gestures[index].reset();
}
void CompressorPanel::finishAll() { for (unsigned i = 0; i < comp::kParameterCount; ++i) finish(i); }
void CompressorPanel::refresh(bool meters) {
    if (!isVisible()) return;
    QVariantList values;
    for (const auto& p : comp::parameterTable()) {
        values.append(read(p.index));
    }
    const double ratio = read(0), threshold = read(1), knee = read(5);
    if (meters) {
        const auto t = available() ? m_controller->effectMeterSnapshot(m_channel, m_insert) : daw::EffectMeterSnapshot{};
        const auto db = [](float v) { return std::clamp(20 * std::log10(std::max(double(v), 1e-9)), -60.0, 24.0); };
        m_meters[0] = std::max(db(t.input), m_meters[0] - .8);
        m_meters[1] = std::max(db(t.output), m_meters[1] - .8);
        m_meters[2] = std::max(double(t.reduction), m_meters[2] - .5);
    }
    const double level = std::clamp(m_meters[0], -60.0, 0.0);
    emit m_bridge->snapshot({{"available", available()}, {"values", values},
        {"input", m_meters[0]}, {"output", m_meters[1]}, {"reduction", m_meters[2]},
        {"marker", level - comp::reductionDb(level, threshold, ratio, knee)}});
}
void CompressorPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); refresh(); m_timer->start(); }
void CompressorPanel::hideEvent(QHideEvent* e) { m_timer->stop(); finishAll(); QWidget::hideEvent(e); }
CompressorControls::CompressorControls(CompressorPanel* panel) : QObject(panel), m_panel(panel) {}
void CompressorControls::edit(int index, double value, bool finished) {
    if (index < 0 || index >= int(comp::kParameterCount) || !std::isfinite(value)) return;
    m_panel->write(unsigned(index), value);
    if (finished) m_panel->finish(unsigned(index));
    m_panel->refresh();
}
void CompressorControls::finish(int index) { if (index >= 0) m_panel->finish(unsigned(index)); m_panel->refresh(); }
void CompressorControls::automate(int index) {
    if (index < 0 || index >= int(comp::kParameterCount)) return;
    m_panel->finishAll(); emit m_panel->automationRequested(QString::fromStdString(comp::parameterTable()[unsigned(index)].id));
}
