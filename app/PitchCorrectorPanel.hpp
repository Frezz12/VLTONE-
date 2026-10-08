#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QVariantMap>
#include <array>
#include <optional>
#include <string>
class QTimer;
class PitchCorrectorPanel;
class NativePluginView;
namespace daw { class EngineController; }
namespace daw::plugins::pitch { class PitchCorrectorInstance; }

// Native UI actions execute on the control thread and preserve undo gestures.
class PitchControls final : public QObject {
    Q_OBJECT
public:
    explicit PitchControls(PitchCorrectorPanel* panel);
public slots:
    void edit(int index, double value, bool finished);
    void finish(int index);
    void toggleNote(int note);
    void toggleBypass();
    void factoryPreset(int index);
    void loadPreset(const QString& name);
    QString savePreset(const QString& name, bool replace);
    void deletePreset(const QString& name);
    void automate(int index);
    void sendToAll();
signals:
    void snapshot(const QVariantMap& state);
private:
    PitchCorrectorPanel* m_panel;
};

class PitchCorrectorPanel final : public QWidget {
    Q_OBJECT
public:
    PitchCorrectorPanel(daw::EngineController* controller, QString channel,
                        QString insert, QWidget* parent = nullptr);
    ~PitchCorrectorPanel() override;
    void applyFactoryPreset(int index);
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    friend class PitchControls;
    using Values = std::array<double, 12>;
    bool available() const;
    double read(unsigned index) const;
    void write(unsigned index, double value);
    void finishGesture(unsigned index);
    void finishAll();
    void applyValues(const Values&, const QString& name);
    void refresh();
    daw::EngineController* m_controller = nullptr;
    std::string m_channel, m_insert;
    NativePluginView* m_view = nullptr;
    PitchControls* m_bridge = nullptr;
    QTimer* m_timer = nullptr;
    std::array<std::optional<double>, 12> m_gestures{};
    Values m_presetValues{};
    QString m_presetName = QStringLiteral("Custom"), m_status;
    QJsonObject m_presets;
    QVariantMap m_previous;
    bool m_userPreset = false;
};
