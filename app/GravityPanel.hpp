#pragma once

#include "Internal/GravityInstance.hpp"

#include <QHash>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QTimer;
class QShowEvent;
class QHideEvent;

namespace ui {
class IconButton;
class Knob;
}

namespace daw { class EngineController; }

/// The functional particle field and Pitch/Size XY attractor.
class GravityField final : public QWidget {
public:
    explicit GravityField(QWidget* parent = nullptr);

    void setState(double gravity, double pitch, double feedback, double decay,
                  double size, double pitchSpread, double motion, double density,
                  int algorithm,
                  const daw::plugins::gravity::Telemetry& telemetry,
                  bool reducedMotion);
    void clearParticles();

    std::function<void()> gestureStarted;
    std::function<void(double pitch, double size)> valuesChanged;
    std::function<void()> gestureFinished;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    struct Particle {
        float angle = 0.0f;
        float radius = 0.0f;
        float speed = 0.0f;
        float life = 1.0f;
        float tint = 0.0f;
        float mass = 0.0f;
    };

    QRectF cloudRect() const;
    QPointF attractorPoint() const;
    void moveAttractor(const QPointF& point);
    void addParticles(std::uint64_t count);
    float randomUnit();

    std::vector<Particle> m_particles;
    std::uint64_t m_lastGrainSerial = 0;
    std::uint64_t m_randomState = 0x6a09e667f3bcc909ull;
    daw::plugins::gravity::Telemetry m_telemetry;
    double m_gravity = 0.5;
    double m_pitch = 0.0;
    double m_feedback = 0.5;
    double m_decay = 6.0;
    double m_size = 0.6;
    double m_pitchSpread = 0.0;
    double m_motion = 0.0;
    double m_density = 0.5;
    int m_algorithm = 0;
    bool m_reducedMotion = false;
    bool m_dragging = false;
};

/// Host-drawn editor for the built-in Gravity effect.
class GravityPanel final : public QWidget {
    Q_OBJECT
public:
    GravityPanel(daw::EngineController* controller, QString channelId,
                 QString insertId, QWidget* parent = nullptr);

    /// Headless UI invariant check: compound undo, dirty preset indication,
    /// XY gesture grouping, model-backed power and keyboard accessibility.
    bool checkForTest();

signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);

protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    using PresetValues = std::array<double, daw::plugins::gravity::kParameterCount>;
    struct UserPreset {
        QString name;
        PresetValues values{};
    };

    daw::plugins::gravity::GravityInstance* gravityInstance() const;
    ui::Knob* makeKnob(const QString& parameterId, int diameter);
    double readParameter(const QString& parameterId) const;
    void writeParameter(const QString& parameterId, double value);
    void beginGesture(const QString& parameterId);
    void endGesture(const QString& parameterId, const char* label = "Change Gravity Parameter");
    void beginXyGesture();
    void endXyGesture();
    void applyValues(const PresetValues& values, const QString& kind,
                     const QString& name, const char* undoLabel);
    void applyPreset(int index);
    void applyUserPreset(const QString& name);
    void showPresetMenu();
    void reloadUserPresets();
    void storeUserPresets() const;
    void saveCurrentUserPreset();
    void renameCurrentUserPreset();
    void deleteCurrentUserPreset();
    void setDrawerOpen(bool open);
    int matchingPreset() const;
    QString matchingUserPreset() const;
    void refresh();

    daw::EngineController* m_controller = nullptr;
    QString m_channelId;
    QString m_insertId;
    std::string m_channelKey;
    std::string m_insertKey;

    QHash<QString, ui::Knob*> m_knobs;
    QHash<QString, double> m_gestureStart;
    std::optional<std::pair<double, double>> m_xyStart;
    QPushButton* m_presetName = nullptr;
    QLabel* m_gravityReadout = nullptr;
    QComboBox* m_algorithm = nullptr;
    GravityField* m_field = nullptr;
    ui::IconButton* m_power = nullptr;
    ui::IconButton* m_settings = nullptr;
    ui::IconButton* m_freeze = nullptr;
    QWidget* m_drawer = nullptr;
    QTabWidget* m_drawerTabs = nullptr;
    QComboBox* m_pitchSnap = nullptr;
    QComboBox* m_detectorSource = nullptr;
    QComboBox* m_timingMode = nullptr;
    QComboBox* m_timingDivision = nullptr;
    QTimer* m_timer = nullptr;
    std::vector<UserPreset> m_userPresets;
    int m_selectedPreset = 0;
    QString m_selectedKind{QStringLiteral("factory")};
    QString m_selectedName{QStringLiteral("WAVEFARERS")};
    bool m_refreshing = false;
};
