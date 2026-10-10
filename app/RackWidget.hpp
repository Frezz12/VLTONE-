#pragma once
#include "EngineController.hpp"
#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <optional>
class QScrollArea;
class QHBoxLayout;
class QVBoxLayout;
class QRubberBand;
class QTimer;
class QLabel;
class RackDeviceCard;
namespace ui {
class MiniModuleRack;
class Knob;
} // namespace ui

class RackWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit RackWidget(daw::EngineController*, QWidget* parent = nullptr);
    ~RackWidget() override;
    void setChannel(const QString&);
    QString channelId() const {
        return m_channel;
    }
    QString channelName() const;
    void sync();
    void finishEdits();
    bool ownsFocus() const;
    bool command(const QString& command);
    void selectDevices(const QStringList&);
    QStringList selectedDevices() const;
    static bool checkForTest(QString* error = nullptr, const QString& screenshot = {});
  signals:
    void edited(bool structural);
    void editorRequested(const QString& channel, const QString& slot);
    void automationRequested(const QString& channel, const QString& slot, const QString& parameter);
    void sendAutomationRequested(const QString& channel, const QString& send);
    void channelRequested(const QString&);
    void trackCreated();

  protected:
    bool eventFilter(QObject*, QEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;

  private:
    void rebuild();
    void refresh();
    void rebuildSends();
    void select(const QString&, Qt::KeyboardModifiers, bool group = false);
    void updateSelection();
    void updateGroupAppearance();
    void clearDrop();
    void startDrag(const QString&);
    void contextMenu(const QString&, const QPoint&, bool group = false);
    void addDevice(const QPoint&, bool instrument = false);
    void addSend(const QPoint&, const QString& replace = {});
    void groupSelection(bool ungroup);
    void transferTo(const QString& channel, bool copy);
    void changed(bool structural = true);
    bool report(const audio::Result&);
    void updateDrop(const QPoint& global);
    void saveView();
    QString settingsKey(const QString& suffix = {}) const;
    daw::EngineController* m_controller;
    QString m_channel, m_project, m_anchor;
    QWidget *m_left = nullptr, *m_sends = nullptr, *m_chain = nullptr, *m_marker = nullptr;
    QScrollArea* m_scroll = nullptr;
    QScrollArea* m_sendScroll = nullptr;
    QHBoxLayout* m_row = nullptr;
    ui::MiniModuleRack* m_mini = nullptr;
    QHash<QString, QPointer<RackDeviceCard>> m_cards;
    QHash<QString, QPointer<QWidget>> m_groups;
    QHash<QString, QPointer<ui::Knob>> m_sendKnobs;
    QHash<QString, std::shared_ptr<std::optional<float>>> m_sendBefore;
    QSet<QString> m_selection;
    QHash<QString, QSet<QString>> m_channelSelection;
    QByteArray m_structure, m_sendStructure;
    QRubberBand* m_rubber = nullptr;
    QPoint m_rubberOrigin, m_dragPosition;
    QSet<QString> m_rubberBefore;
    QTimer *m_timer = nullptr, *m_scrollTimer = nullptr;
    int m_insertAt = -1, m_dropAt = -1;
    QString m_dropGroup;
    bool m_rebuilding = false, m_rubberActive = false;
};
