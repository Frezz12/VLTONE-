#pragma once
#include "EngineController.hpp"
#include <QEvent>
#include <QPointer>
#include <QWidget>
#include <algorithm>
#include <string>
#include <vector>

// The editor and rack share one analyzer. Keep it running while either view
// is visible, without changing the user's Pre/Post/Freeze settings.
class EqualizerAnalyzerView final : public QObject {
public:
    EqualizerAnalyzerView(daw::EngineController* controller, std::string channel,
                          std::string slot, QWidget* view)
        : QObject(view), m_controller(controller), m_channel(std::move(channel)),
          m_slot(std::move(slot)), m_view(view) {
        views().push_back(this);
        view->installEventFilter(this);
        refresh();
    }
    ~EqualizerAnalyzerView() override {
        std::erase(views(), this);
        refresh();
    }
    bool requested() const {
        return std::any_of(views().begin(), views().end(), [this](const auto* other) {
            return other->m_controller == m_controller && other->m_channel == m_channel &&
                other->m_slot == m_slot && other->m_view && other->m_view->isVisible();
        });
    }
    void refresh() {
        if (!m_controller) return;
        if (const auto snapshot = m_controller->equalizerSnapshot(m_channel, m_slot))
            refresh(snapshot->analyzer);
    }
    void refresh(daw::plugins::equalizer::AnalyzerConfig config) {
        const bool enabled = requested();
        if (config.enabled == enabled) return;
        config.enabled = enabled;
        m_controller->setEqualizerAnalyzer(m_channel, m_slot, config);
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
            refresh();
        return QObject::eventFilter(watched, event);
    }
private:
    static std::vector<EqualizerAnalyzerView*>& views() {
        static std::vector<EqualizerAnalyzerView*> instances;
        return instances;
    }
    daw::EngineController* m_controller;
    std::string m_channel, m_slot;
    QPointer<QWidget> m_view;
};
