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
class RackParameterBinding;
class RackBuiltinView;
class QScrollArea;
class RackModulationPanel;
class QAbstractButton;
class QButtonGroup;
class QGridLayout;
namespace ui {
class IconButton;
}

class RackDeviceCard final : public QFrame {
    Q_OBJECT
  public:
    enum class Page { Editor, Parameters, Modulation };
    RackDeviceCard(daw::EngineController*, QString channel, QString slot, QWidget* parent = nullptr);
    ~RackDeviceCard() override;
    QString slotId() const {
        return m_slot;
    }
    void sync();
    void refresh(bool synchronize = true);
    void finishEdits();
    void setSelected(bool);
    bool expanded() const {
        return m_expanded;
    }
    void setExpanded(bool);
    bool collapsed() const { return m_collapsed; }
    void setCollapsed(bool);
    void setPage(Page);
    Page page() const { return m_page; }
    std::function<bool()> editorIsOpen;
  signals:
    void selected(const QString& id, Qt::KeyboardModifiers);
    void clicked(const QString& id, Qt::KeyboardModifiers);
    void dragRequested(const QString& id);
    void contextRequested(const QString& id, const QPoint& global);
    void editorRequested(const QString& channel, const QString& id);
    void editorToggleRequested(const QString& channel, const QString& id);
    void automationRequested(const QString& channel, const QString& id, const QString& parameter);
    void edited(bool structural);
    void widthChanged();

  protected:
    bool eventFilter(QObject*, QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent*) override;

  private:
    void rebuildControls();
    void sidechainMenu();
    void updateTheme();
    void updateCollapsed();
    void refreshRecentParameters();
    void updateWidth();
    daw::EngineController* m_controller;
    QString m_channel, m_slot, m_uid;
    daw::PluginIdentity m_identity;
    daw::AudioPluginRuntimeState m_runtimeState = daw::AudioPluginRuntimeState::Missing;
    bool m_engineOff = false;
    daw::PluginEditorChannel m_side = daw::PluginEditorChannel::Left;
    std::vector<std::string> m_pins;
    RackParameterBinding* m_binding = nullptr;
    RackModulationPanel* m_modulation = nullptr;
    RackBuiltinView* m_builtin = nullptr;
    QScrollArea* m_bodyScroll = nullptr;
    QAbstractButton* m_openEditor = nullptr;
    QButtonGroup* m_pages = nullptr;
    QWidget* m_pageBar = nullptr;
    QWidget* m_pinned = nullptr;
    QGridLayout* m_pinnedGrid = nullptr;
    QStringList m_recent, m_displayed;
    Page m_page = Page::Editor;
    QWidget *m_body = nullptr, *m_header = nullptr, *m_footer = nullptr;
    QHBoxLayout* m_headerLayout = nullptr;
    QVBoxLayout* m_root = nullptr;
    QLabel* m_title = nullptr;
    ui::IconButton *m_power = nullptr, *m_expand = nullptr, *m_fold = nullptr, *m_headerEditor = nullptr;
    QComboBox *m_mode = nullptr, *m_channelSide = nullptr;
    QDoubleSpinBox* m_mix = nullptr;
    QToolButton* m_sidechain = nullptr;
    std::optional<float> m_mixBefore;
    bool m_expanded = false, m_selected = false, m_pressed = false, m_syncing = false;
    bool m_collapsed = false;
    bool m_internal = false;
    QPoint m_press;
};
