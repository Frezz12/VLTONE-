#pragma once
#include "Internal/CompressorInstance.hpp"
#include <QWidget>
#include <QVariantMap>
#include <array>
#include <optional>
#include <string>

class QTimer;
class CompressorPanel;
namespace daw { class EngineController; }
namespace ui::graphics { class BrowserSurface; class BrowserProfile; }

class CompressorWebBridge final : public QObject {
    Q_OBJECT
public:
    explicit CompressorWebBridge(CompressorPanel*);
public slots:
    void ready();
    void edit(int index, double value, bool finished, int revision);
    void finish(int index);
    void automate(int index);
signals:
    void snapshot(const QVariantMap& state);
private:
    CompressorPanel* m_panel;
};

class CompressorPanel final : public QWidget {
    Q_OBJECT
public:
    CompressorPanel(daw::EngineController*, QString channel, QString insert, QWidget* parent = nullptr);
    ~CompressorPanel() override;
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    friend class CompressorWebBridge;
    daw::plugins::compressor::CompressorInstance* instance() const;
    double read(unsigned) const;
    void write(unsigned, double);
    void finish(unsigned);
    void finishAll();
    void refresh(bool meters = false);
    daw::EngineController* m_controller;
    std::string m_channel, m_insert;
    ui::graphics::BrowserProfile* m_profile;
    ui::graphics::BrowserSurface* m_view;
    CompressorWebBridge* m_bridge;
    QTimer* m_timer;
    std::array<std::optional<double>, daw::plugins::compressor::kParameterCount> m_gestures{};
    std::array<double, 3> m_meters{-60, -60, 0};
    int m_revision = 0;
    bool m_ready = false;
};
