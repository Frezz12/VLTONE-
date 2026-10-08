#pragma once
#include "Internal/DelayInstance.hpp"
#include <QWidget>
#include <QVariantMap>
#include <array>
#include <optional>

class QTimer;
class DelayPanel;
namespace daw { class EngineController; }
class NativePluginView;
class DelayControls final : public QObject {
    Q_OBJECT
public:
    explicit DelayControls(DelayPanel*);
public slots:
    void edit(int index, double value, bool finished);
    void finish(int index);
    void automate(int index);
signals:
    void snapshot(const QVariantMap& state);
private:
    DelayPanel* m_panel;
};
class DelayPanel final : public QWidget {
    Q_OBJECT
public:
    DelayPanel(daw::EngineController*, QString channel, QString insert, QWidget* parent = nullptr);
    ~DelayPanel() override;
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    friend class DelayControls;
    bool available() const;
    double read(unsigned) const;
    void write(unsigned, double);
    void finish(unsigned);
    void finishAll();
    void refresh(bool meters = false);
    daw::EngineController* m_controller;
    std::string m_channel, m_insert;
    NativePluginView* m_view;
    DelayControls* m_bridge;
    QTimer* m_timer;
    std::array<std::optional<double>, daw::plugins::delay::kParameterCount> m_gestures{};
    std::array<double, 3> m_meters{};
};
