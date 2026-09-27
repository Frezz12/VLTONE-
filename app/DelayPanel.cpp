#include "DelayPanel.hpp"
#include "EngineController.hpp"
#include "graphics/BrowserSurface.hpp"
#include <QHideEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace delay = daw::plugins::delay;
DelayPanel::DelayPanel(daw::EngineController* controller, QString channel, QString insert, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channel(channel.toStdString()), m_insert(insert.toStdString()) {
    setObjectName("DelayPanel"); setAccessibleName(QStringLiteral("Flowers Delay")); setMinimumSize(960, 420);
    m_profile = new ui::graphics::BrowserProfile(this, nullptr, false);
    m_view = new ui::graphics::BrowserSurface(m_profile, this);
    m_view->setObjectName("DelayWebView"); m_view->setProperty("dawWebInput", true);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(m_view);
    auto* page = m_view->page(); page->setBackgroundColor(QColor("#f8d954")); page->setAudioMuted(true);
    page->navigationPolicy = [](const QUrl& url, bool) {
        return url.scheme() == "qrc" && url.host().isEmpty() && url.path() == "/vlt/delay/index.html";
    };
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->setWebAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->setWebAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    m_bridge = new DelayWebBridge(this); page->setWebChannelObject("delay", m_bridge);
    connect(page, &ui::graphics::BrowserPage::loadStarted, this, [this] { m_ready = false; finishAll(); });
    m_timer = new QTimer(this); m_timer->setObjectName("DelayTelemetryTimer"); m_timer->setInterval(34);
    connect(m_timer, &QTimer::timeout, this, [this] { refresh(true); });
    page->load(QUrl("qrc:/vlt/delay/index.html"));
}
DelayPanel::~DelayPanel() { finishAll(); delete m_view; delete m_profile; }
delay::DelayInstance* DelayPanel::instance() const {
    return m_controller ? dynamic_cast<delay::DelayInstance*>(m_controller->insertInstance(m_channel, m_insert)) : nullptr;
}
double DelayPanel::read(unsigned i) const {
    const auto& p = delay::parameterTable()[i];
    return instance() ? m_controller->insertParameter(m_channel, m_insert, p.id) : p.defaultValue;
}
void DelayPanel::write(unsigned i, double v) {
    if (i >= delay::kParameterCount || !std::isfinite(v) || !instance()) return;
    const auto& p = delay::parameterTable()[i]; v = std::clamp(v, p.minValue, p.maxValue);
    if (p.isStepped) v = std::round(v);
    if (!m_gestures[i]) m_gestures[i] = read(i);
    m_controller->setInsertParameter(m_channel, m_insert, p.id, v);
}
void DelayPanel::finish(unsigned i) {
    if (i >= delay::kParameterCount || !m_gestures[i]) return;
    if (instance() && read(i) != *m_gestures[i]) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert, delay::parameterTable()[i].id, *m_gestures[i], "Change Flowers Delay");
        emit projectEdited();
    }
    m_gestures[i].reset();
}
void DelayPanel::finishAll() { for (unsigned i = 0; i < delay::kParameterCount; ++i) finish(i); }
void DelayPanel::refresh(bool meters) {
    if (!m_ready || !isVisible()) return;
    QVariantList values, parameters, divisions, characters;
    std::array<double, delay::kParameterCount> p{};
    for (const auto& info : delay::parameterTable()) {
        p[info.index] = read(info.index); values.append(p[info.index]);
        parameters.append(QVariantMap{{"id", QString::fromStdString(info.id)}, {"name", QString::fromStdString(info.name)},
            {"unit", QString::fromStdString(info.unit)}, {"min", info.minValue}, {"max", info.maxValue},
            {"default", info.defaultValue}, {"stepped", info.isStepped}});
    }
    for (unsigned i = 0; i < delay::kDivisionCount; ++i) divisions.append(QString::fromUtf8(delay::divisionName(i).data(), qsizetype(delay::divisionName(i).size())));
    for (unsigned i = 0; i < delay::kCharacterCount; ++i) characters.append(QString::fromUtf8(delay::characterName(i).data(), qsizetype(delay::characterName(i).size())));
    daw::engine::TransportInfo transport;
    if (m_controller) {
        transport.tempo = m_controller->tempo();
        transport.timeSigNumerator = m_controller->project().timeSigNumerator;
        transport.timeSigDenominator = m_controller->project().timeSigDenominator;
    }
    const auto time = delay::timing(p, transport);
    if (meters) {
        const auto t = instance() ? instance()->consumeTelemetry() : delay::Telemetry{};
        m_meters[0] = std::max(double(t.input), m_meters[0] * .88);
        m_meters[1] = std::max(double(t.wet), m_meters[1] * .88);
        m_meters[2] = std::max(double(t.output), m_meters[2] * .88);
    }
    emit m_bridge->snapshot({{"available", instance() != nullptr}, {"values", values}, {"parameters", parameters},
        {"divisions", divisions}, {"characters", characters}, {"revision", m_revision},
        {"milliseconds", time.milliseconds}, {"bpm", time.bpm}, {"limited", time.requestedMs > 8000},
        {"input", m_meters[0]}, {"wet", m_meters[1]}, {"output", m_meters[2]}});
}
void DelayPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); refresh(); m_timer->start(); }
void DelayPanel::hideEvent(QHideEvent* e) { m_timer->stop(); finishAll(); QWidget::hideEvent(e); }
DelayWebBridge::DelayWebBridge(DelayPanel* panel) : QObject(panel), m_panel(panel) {}
void DelayWebBridge::ready() { m_panel->m_ready = true; m_panel->refresh(); }
void DelayWebBridge::edit(int i, double v, bool finished, int revision) {
    if (i < 0 || i >= int(delay::kParameterCount) || !std::isfinite(v)) return;
    m_panel->write(unsigned(i), v); m_panel->m_revision = revision;
    if (finished) m_panel->finish(unsigned(i));
    m_panel->refresh();
}
void DelayWebBridge::finish(int i) { if (i >= 0) m_panel->finish(unsigned(i)); m_panel->refresh(); }
void DelayWebBridge::automate(int i) {
    if (i < 0 || i >= int(delay::kParameterCount)) return;
    m_panel->finishAll(); emit m_panel->automationRequested(QString::fromStdString(delay::parameterTable()[unsigned(i)].id));
}
