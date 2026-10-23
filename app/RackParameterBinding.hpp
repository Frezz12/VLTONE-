#pragma once
#include "EngineController.hpp"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <vector>

namespace ui {
class Knob;
}
class QComboBox;
class QDoubleSpinBox;
class QAbstractButton;
class QLabel;

/// One slot's control-thread binding, shared by every compact presentation.
/// No widget retains a DSP pointer, including while an editor is also open.
class RackParameterBinding final : public QObject {
    Q_OBJECT
  public:
    RackParameterBinding(daw::EngineController*, QString channel, QString slot, QObject* parent);
    ~RackParameterBinding() override;
    const std::vector<daw::plugins::ParameterInfo>& parameters() const {
        return m_parameters;
    }
    const daw::plugins::ParameterInfo* info(const QString& id) const;
    ui::Knob* knob(const QString& id, QWidget* parent, int width = 62);
    QWidget* slider(const QString& id, QWidget* parent);
    QDoubleSpinBox* number(const QString& id, QWidget* parent);
    QComboBox* choice(const QString& id, const QStringList& labels, QWidget* parent);
    QAbstractButton* toggle(const QString& id, const QString& text, QWidget* parent);
    double value(const QString& id) const;
    void write(const QString& id, double value);
    void finish(const QString& id);
    void finishAll();
    void refresh();
    /// Extension windows must recheck permissions at every live write.
    void setEditGuard(std::function<bool()> guard) { m_editGuard = std::move(guard); }
    void setDocumentPreviewParameters(std::vector<daw::plugins::ParameterInfo>);
    static void refreshModulation(ui::Knob*, daw::EngineController*, const std::string& channel, const std::string& slot, const std::string& parameter);
    void automate(const QString& id);
    bool available() const;
    daw::EngineController* controller() const {
        return m_controller;
    }
    const std::string& channel() const {
        return m_channel;
    }
    const std::string& slot() const {
        return m_slot;
    }
  signals:
    void edited();
    void automationRequested(const QString& id);

  private:
    daw::EngineController* m_controller;
    std::string m_channel, m_slot;
    daw::PluginIdentity m_identity;
    std::vector<daw::plugins::ParameterInfo> m_parameters;
    QHash<QString, double> m_gestures;
    std::function<bool()> m_editGuard;
    std::uint64_t m_previewGeneration=0;
    std::string m_previewUid;
    struct Control {
        QString id;
        QPointer<QWidget> widget;
        QPointer<QLabel> readout;
    };
    std::vector<Control> m_controls;
};
