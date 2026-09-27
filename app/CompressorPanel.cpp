#include "CompressorPanel.hpp"
#include "EngineController.hpp"
#include "graphics/BrowserSurface.hpp"
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
    m_profile = new ui::graphics::BrowserProfile(this, nullptr, false);
    m_view = new ui::graphics::BrowserSurface(m_profile, this);
    m_view->setObjectName("CompressorWebView"); m_view->setProperty("dawWebInput", true);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    auto* page = m_view->page(); page->setBackgroundColor(QColor("#e2e5e5")); page->setAudioMuted(true);
    page->navigationPolicy = [](const QUrl& url, bool) {
        return url.scheme() == "qrc" && url.host().isEmpty() && url.path() == "/vlt/compressor/index.html";
    };
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->setWebAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    m_bridge = new CompressorWebBridge(this); page->setWebChannelObject("compressor", m_bridge);
    connect(page, &ui::graphics::BrowserPage::loadStarted, this, [this] { m_ready = false; finishAll(); });
    m_timer = new QTimer(this); m_timer->setObjectName("CompressorTelemetryTimer"); m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, [this] { refresh(true); });
    page->load(QUrl("qrc:/vlt/compressor/index.html"));
}
CompressorPanel::~CompressorPanel() {
    finishAll(); delete m_view; delete m_profile;
}
comp::CompressorInstance* CompressorPanel::instance() const {
    return m_controller ? dynamic_cast<comp::CompressorInstance*>(m_controller->insertInstance(m_channel, m_insert)) : nullptr;
}
double CompressorPanel::read(unsigned index) const {
    const auto& p = comp::parameterTable()[index];
    return instance() ? m_controller->insertParameter(m_channel, m_insert, p.id) : p.defaultValue;
}
void CompressorPanel::write(unsigned index, double value) {
    if (index >= comp::kParameterCount || !std::isfinite(value) || !instance()) return;
    const auto& p = comp::parameterTable()[index]; value = std::clamp(value, p.minValue, p.maxValue);
    if (p.isStepped) value = std::round(value);
    if (!m_gestures[index]) m_gestures[index] = read(index);
    m_controller->setInsertParameter(m_channel, m_insert, p.id, value);
}
void CompressorPanel::finish(unsigned index) {
    if (index >= comp::kParameterCount || !m_gestures[index]) return;
    if (instance() && read(index) != *m_gestures[index]) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert, comp::parameterTable()[index].id,
                                               *m_gestures[index], "Change Compressor");
        emit projectEdited();
    }
    m_gestures[index].reset();
}
void CompressorPanel::finishAll() { for (unsigned i = 0; i < comp::kParameterCount; ++i) finish(i); }
void CompressorPanel::refresh(bool meters) {
    if (!m_ready || !isVisible()) return;
    QVariantList values, parameters, curve;
    for (const auto& p : comp::parameterTable()) {
        values.append(read(p.index));
        parameters.append(QVariantMap{{"id", QString::fromStdString(p.id)}, {"name", QString::fromStdString(p.name)},
            {"unit", QString::fromStdString(p.unit)}, {"min", p.minValue}, {"max", p.maxValue}, {"default", p.defaultValue}});
    }
    const double ratio = read(0), threshold = read(1), knee = read(5);
    for (int i = 0; i <= 120; ++i) {
        const double input = -60 + i * .5;
        curve.append(input - comp::reductionDb(input, threshold, ratio, knee));
    }
    if (meters) {
        const auto t = instance() ? instance()->consumeTelemetry() : comp::Telemetry{};
        const auto db = [](float v) { return std::clamp(20 * std::log10(std::max(double(v), 1e-9)), -60.0, 24.0); };
        m_meters[0] = std::max(db(t.input), m_meters[0] - .8);
        m_meters[1] = std::max(db(t.output), m_meters[1] - .8);
        m_meters[2] = std::max(double(t.reduction), m_meters[2] - .5);
    }
    const double level = std::clamp(m_meters[0], -60.0, 0.0);
    emit m_bridge->snapshot({{"values", values}, {"parameters", parameters}, {"curve", curve}, {"revision", m_revision},
        {"input", m_meters[0]}, {"output", m_meters[1]}, {"reduction", m_meters[2]},
        {"marker", level - comp::reductionDb(level, threshold, ratio, knee)}});
}
void CompressorPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); refresh(); m_timer->start(); }
void CompressorPanel::hideEvent(QHideEvent* e) { m_timer->stop(); finishAll(); QWidget::hideEvent(e); }
CompressorWebBridge::CompressorWebBridge(CompressorPanel* panel) : QObject(panel), m_panel(panel) {}
void CompressorWebBridge::ready() { m_panel->m_ready = true; m_panel->refresh(); }
void CompressorWebBridge::edit(int index, double value, bool finished, int revision) {
    if (index < 0 || index >= int(comp::kParameterCount) || !std::isfinite(value)) return;
    m_panel->write(unsigned(index), value); m_panel->m_revision = revision;
    if (finished) m_panel->finish(unsigned(index));
    m_panel->refresh();
}
void CompressorWebBridge::finish(int index) { if (index >= 0) m_panel->finish(unsigned(index)); m_panel->refresh(); }
void CompressorWebBridge::automate(int index) {
    if (index < 0 || index >= int(comp::kParameterCount)) return;
    m_panel->finishAll(); emit m_panel->automationRequested(QString::fromStdString(comp::parameterTable()[unsigned(index)].id));
}
