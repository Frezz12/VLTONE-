#pragma once

#include "AudioPerformance.hpp"
#include <QDialog>
#include <QHash>

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;
class QPlainTextEdit;
class PerformanceHistory;
namespace daw { class EngineController; struct AudioFailureNotice; }

class PerformanceWindow final : public QDialog {
    Q_OBJECT
public:
    explicit PerformanceWindow(daw::EngineController* controller, QWidget* parent = nullptr);
    void refresh();
    void recordNotice(const daw::AudioFailureNotice& notice);
    void recordAction(const QString& text) { appendEvent(text); }
    static bool checkForTest(const QString& shots);
signals:
    void pluginActionRequested(daw::AudioPluginAddress address, quint64 project, int action);
    void audioSettingsRequested();
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void applyTheme();
    void filterRows();
    void updateSelection();
    void requestAction(int action);
    void appendEvent(const QString& text);
    QString channelName(const std::string& channel) const;
    QString kindName(daw::AudioPerformanceRow::Kind kind) const;
    QString rowStatus(const daw::AudioPerformanceRow& row, quint64 now) const;
    daw::EngineController* m_controller;
    QTimer* m_timer;
    QTreeWidget* m_tree;
    QTreeWidget* m_engine;
    QLineEdit* m_search;
    QCheckBox* m_problems;
    QCheckBox* m_internal;
    QPushButton* m_pause;
    QPushButton* m_bypass;
    QPushButton* m_restart;
    QPushButton* m_remove;
    QLabel* m_banner;
    QLabel* m_detail;
    QLabel* m_cards[4]{};
    PerformanceHistory* m_history;
    QPlainTextEdit* m_events;
    QHash<quint64, QTreeWidgetItem*> m_items;
    QHash<QString, QTreeWidgetItem*> m_channels;
    QHash<QString, QTreeWidgetItem*> m_engineItems;
    QHash<quint64, daw::engine::NodeTimingSnapshot> m_previous;
    QHash<quint64, QString> m_previousStatus;
    QHash<quint64, daw::AudioPerformanceRow> m_rows;
    quint64 m_project = 0, m_lastPoll = 0, m_lastXruns = 0, m_lastOverruns = 0;
    bool m_unresponsive = false;
};
