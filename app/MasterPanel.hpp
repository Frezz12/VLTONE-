#pragma once

#include <QWidget>
#include <QString>
#include <QElapsedTimer>
#include <array>
#include <optional>

namespace daw { class EngineController; }
namespace ui { class FaderWidget; class PanKnob; class FrameTimer; class LevelMeter; }
class MasterMeter;
class QLabel;
class QToolButton;
class QVBoxLayout;

/// A persistent master workspace. Analysis inserts share the normal plugin
/// lifecycle, but their isolated audio role is owned by the document/engine.
class MasterPanel final : public QWidget {
    Q_OBJECT
public:
    explicit MasterPanel(daw::EngineController*, QWidget* parent = nullptr);
    void sync();
    void refreshMeters();
    static bool checkForTest(const QString& screenshotPrefix = {});
signals:
    void closeRequested();
    void edited();
    void structureChanged();
    void editorRequested(const QString& channel, const QString& slot);
    void automateRequested(bool pan);
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void rebuildInserts();
    void addPlugin(bool analysis, QWidget* anchor);
    void applyTheme();
    daw::EngineController* m_controller;
    MasterMeter* m_meter = nullptr;
    ui::FaderWidget* m_fader = nullptr;
    ui::PanKnob* m_pan = nullptr;
    ui::LevelMeter* m_outputMeter = nullptr;
    QLabel* m_gain = nullptr;
    QLabel* m_panValue = nullptr;
    QToolButton* m_peak = nullptr;
    std::array<QLabel*, 6> m_values{};
    QToolButton* m_analysisToggle = nullptr;
    QWidget* m_analysisBody = nullptr;
    QVBoxLayout* m_inserts = nullptr;
    QVBoxLayout* m_analysisInserts = nullptr;
    ui::FrameTimer* m_timer = nullptr;
    QString m_chainState;
    std::optional<float> m_gainBefore, m_panBefore;
    int m_reference = -18;
    QElapsedTimer m_readoutClock;
};
