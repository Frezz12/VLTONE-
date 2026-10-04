#pragma once

#include "Internal/Cla2aInstance.hpp"
#include <QPixmap>
#include <QWidget>
#include <array>
#include <optional>
#include <string>

class Cla2aKnob;
class QAbstractButton;
class QPushButton;
class QTimer;
namespace daw { class EngineController; }

class Cla2aPanel final : public QWidget {
    Q_OBJECT
public:
    Cla2aPanel(daw::EngineController*, QString channel, QString insert, QWidget* parent = nullptr);
    ~Cla2aPanel() override;
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void changeEvent(QEvent*) override;
private:
    daw::plugins::cla2a::Cla2aInstance* instance() const;
    double read(unsigned) const;
    void write(unsigned, double);
    void finish(unsigned);
    void finishAll();
    void refresh();
    void cacheFace();
    QRectF meterRect() const;
    daw::EngineController* m_controller;
    std::string m_channel, m_insert;
    std::array<Cla2aKnob*, 2> m_knobs{};
    std::array<QPushButton*, 2> m_values{};
    QAbstractButton* m_mode = nullptr;
    QTimer* m_timer = nullptr;
    std::array<std::optional<double>, daw::plugins::cla2a::kParameterCount> m_gestures{};
    std::array<double, 3> m_meters{-60, -60, 0};
    QPixmap m_face;
};
