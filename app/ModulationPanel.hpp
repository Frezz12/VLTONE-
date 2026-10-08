#pragma once
#include "Internal/ModulationRackInstance.hpp"
#include <QWidget>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonObject>
#include <optional>
class QTimer;
class ModulationPanel;
class NativePluginView;
namespace daw { class EngineController; }
class ModulationControls final : public QObject {
    Q_OBJECT
public:
    explicit ModulationControls(ModulationPanel*);
public slots:
    void edit(int index, double value, bool finished);
    void finish();
    void reorder(int from, int to);
    void toggleBypass();
    void factoryPreset(int index);
    void loadPreset(const QString& name);
    QString savePreset(const QString& name, bool replace);
    QString renamePreset(const QString& name, const QString& next);
    void deletePreset(const QString& name);
    void automate(int index);
    void setEqExpanded(bool expanded);
signals:
    void snapshot(const QVariantMap& state);
private:
    ModulationPanel* m_panel;
};
// Native Qt cards for the rack and independent effects.
class ModulationPanel final : public QWidget {
    Q_OBJECT
public:
    ModulationPanel(daw::EngineController*, QString channel, QString insert, QWidget* parent = nullptr);
    ModulationPanel(daw::EngineController*, QString channel, QString insert,
                    daw::plugins::modulation::Kind, QWidget* parent = nullptr);
    ~ModulationPanel() override;
    void applyFactoryPreset(int index);
    bool visualUpdatesActive() const;
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
    void eqExpansionChanged(bool expanded);
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    friend class ModulationControls;
    bool available() const;
    std::span<const daw::plugins::ParameterInfo> parameters() const;
    void write(unsigned index, double value);
    void finishAll();
    void refresh();
    void applyValues(const QJsonObject&, const QString& name);
    QString settingsKey() const;
    void storePresets();
    daw::EngineController* m_controller;
    std::string m_channel, m_insert;
    std::string m_uid, m_title;
    std::optional<daw::plugins::modulation::Kind> m_kind;
    bool m_rack = false;
    double m_responseSampleRate = 0;
    NativePluginView* m_view = nullptr;
    ModulationControls* m_bridge = nullptr;
    QTimer* m_timer = nullptr;
    std::array<std::optional<double>, daw::plugins::modulation::ModulationRackInstance::parameterCount> m_gestures{};
    QVariantMap m_previous;
    QJsonArray m_presets;
    QJsonObject m_presetValues;
    QString m_presetName;
};
