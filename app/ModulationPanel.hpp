#pragma once
#include "Internal/ModulationInstance.hpp"
#include "UiFrameClock.hpp"
#include "graphics/ScenePaintSource.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;
namespace ui {
class Knob;
}
namespace daw {
class EngineController;
}

class ModulationField final : public ui::FrameWidget, public ui::graphics::ScenePaintSource {
    Q_OBJECT
  public:
    explicit ModulationField(daw::plugins::modulation::Kind kind, QWidget *parent = nullptr);
    void present(const daw::plugins::modulation::Telemetry &, double dt, bool reduced);
    void paintScene(QPainter &, const QRegion &) override;

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    daw::plugins::modulation::Kind m_kind;
    daw::plugins::modulation::Telemetry m_reading;
    double m_time = 0, m_level = 0, m_width = 0;
    bool m_reduced = false;
};

class ModulationPanel final : public ui::FrameWidget, public ui::graphics::ScenePaintSource {
    Q_OBJECT
  public:
    ModulationPanel(daw::EngineController *, QString channel, QString insert,
                    daw::plugins::modulation::Kind, QWidget *parent = nullptr);
    ~ModulationPanel() override;
    void paintScene(QPainter &, const QRegion &) override;
    void applyFactoryPreset(int index);
    bool visualUpdatesActive() const;
  signals:
    void projectEdited();
    void automationRequested(const QString &parameterId);

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

  private:
    using Kind = daw::plugins::modulation::Kind;
    using Values = daw::plugins::modulation::Values;
    struct UserPreset {
        QString name;
        Values values{};
    };
    daw::plugins::modulation::ModulationInstance *instance() const;
    double read(unsigned index) const;
    Values values() const;
    void write(unsigned index, double value);
    void finishGesture(unsigned index);
    void applyValues(const Values &, const QString &kind, const QString &name);
    void refresh();
    void refreshVisual();
    void applyTheme();
    void arrangeControls();
    void showPresetMenu();
    void loadUserPresets();
    void storeUserPresets();
    void saveUserPreset();
    void renameUserPreset();
    void deleteUserPreset();
    bool validPresetName(const QString &, const QString &except = {}) const;
    QString settingsKey() const;
    daw::EngineController *m_controller;
    std::string m_channel, m_insert;
    Kind m_kind;
    ModulationField *m_field = nullptr;
    QLabel *m_meter = nullptr;
    QLabel *m_mono = nullptr;
    QPushButton *m_preset = nullptr;
    QWidget *m_controls = nullptr;
    QGridLayout *m_grid = nullptr;
    std::array<QWidget *, daw::plugins::modulation::parameterCapacity> m_cells{};
    std::array<ui::Knob *, daw::plugins::modulation::parameterCapacity> m_knobs{};
    std::array<QDoubleSpinBox *, daw::plugins::modulation::parameterCapacity> m_numbers{};
    std::array<std::optional<double>, daw::plugins::modulation::parameterCapacity> m_gestures{};
    QTimer *m_refreshTimer = nullptr;
    ui::FrameTimer *m_visualTimer = nullptr;
    std::vector<UserPreset> m_users;
    QString m_selectedKind, m_selectedName;
    int m_lastFactory = 0, m_columns = 0;
    std::uint64_t m_lastTelemetrySerial = 0;
    double m_telemetryIdleSeconds = 0;
    bool m_refreshing = false, m_reduced = false;
};
