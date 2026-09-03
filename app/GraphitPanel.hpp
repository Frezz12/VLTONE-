#pragma once

#include "Internal/GraphitInstance.hpp"

#include <QWidget>

#include <array>
#include <optional>
#include <string>

class QEvent;
class QHideEvent;
class QLabel;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QSlider;
class QTimer;

namespace ui { class Knob; }
namespace daw { class EngineController; }

/// Compact host-drawn editor for the built-in Graphit effect.
class GraphitPanel final : public QWidget {
    Q_OBJECT
public:
    GraphitPanel(daw::EngineController* controller, QString channelId,
                 QString insertId, QWidget* parent = nullptr);

    bool checkForTest();

signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    daw::plugins::graphit::GraphitInstance* graphitInstance() const;
    double readParameter(const char* parameterId) const;
    void writeParameter(const char* parameterId, double value);
    void beginAmountGesture();
    void endAmountGesture();
    void beginPriorityGesture();
    void endPriorityGesture();
    void toggleActive(bool active);
    void selectMode(int mode);
    void showModeAutomationMenu(QPushButton* button, const QPoint& position);
    void refresh();

    daw::EngineController* m_controller = nullptr;
    QString m_channelId;
    QString m_insertId;
    std::string m_channelKey;
    std::string m_insertKey;
    ui::Knob* m_amount = nullptr;
    QSlider* m_priority = nullptr;
    QPushButton* m_activeButton = nullptr;
    QLabel* m_amountReadout = nullptr;
    std::array<QPushButton*, 5> m_modeButtons{};
    QTimer* m_timer = nullptr;
    std::optional<double> m_amountGestureStart;
    std::optional<double> m_priorityGestureStart;
    std::array<float, daw::plugins::graphit::kHistorySize> m_history{};
    float m_meterLevel = 0.0f;
    float m_gainReduction = 0.0f;
    double m_amountValue = 0.35;
    double m_priorityValue = 0.0;
    int m_mode = 0;
    bool m_bypassed = false;
    bool m_refreshing = false;
};
