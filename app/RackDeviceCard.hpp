#pragma once
#include "EngineController.hpp"
#include <QFrame>
#include <QPointer>
#include <QStringList>
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QVBoxLayout;
class QToolButton;
class RackBuiltinView;
class RackParameterBinding;
namespace ui {
class IconButton;
}

class RackDeviceCard final : public QFrame {
    Q_OBJECT
  public:
    RackDeviceCard(daw::EngineController*, QString channel, QString slot, QWidget* parent = nullptr);
    ~RackDeviceCard() override;
    QString slotId() const {
        return m_slot;
    }
    void sync();
    void refresh();
    void finishEdits();
    void setSelected(bool);
    bool expanded() const {
        return m_expanded;
    }
    void setExpanded(bool);
  signals:
    void selected(const QString& id, Qt::KeyboardModifiers);
    void clicked(const QString& id, Qt::KeyboardModifiers);
    void dragRequested(const QString& id);
    void contextRequested(const QString& id, const QPoint& global);
    void editorRequested(const QString& channel, const QString& id);
    void automationRequested(const QString& channel, const QString& id, const QString& parameter);
    void edited(bool structural);
    void widthChanged();

  protected:
    bool eventFilter(QObject*, QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent*) override;

  private:
    void rebuildControls();
    void configurePins();
    void sidechainMenu();
    void updateTheme();
    daw::EngineController* m_controller;
    QString m_channel, m_slot, m_uid;
    daw::PluginIdentity m_identity;
    daw::AudioPluginRuntimeState m_runtimeState = daw::AudioPluginRuntimeState::Missing;
    daw::PluginEditorChannel m_side = daw::PluginEditorChannel::Left;
    std::vector<std::string> m_pins;
    RackParameterBinding* m_binding = nullptr;
    RackBuiltinView* m_builtin = nullptr;
    QWidget *m_body = nullptr, *m_header = nullptr;
    QVBoxLayout* m_root = nullptr;
    QLabel* m_title = nullptr;
    ui::IconButton *m_power = nullptr, *m_expand = nullptr, *m_configure = nullptr;
    QComboBox *m_mode = nullptr, *m_channelSide = nullptr;
    QDoubleSpinBox* m_mix = nullptr;
    QToolButton* m_sidechain = nullptr;
    std::optional<float> m_mixBefore;
    bool m_expanded = false, m_selected = false, m_pressed = false, m_syncing = false;
    QPoint m_press;
};
