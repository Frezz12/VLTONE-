#pragma once

#include <QWidget>

class QLabel;

class AccountSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit AccountSettingsPage(QWidget* parent = nullptr);

signals:
    void logoutRequested();

private:
    void refresh();
    QLabel* m_identity = nullptr;
    QLabel* m_plan = nullptr;
    QLabel* m_quota = nullptr;
    QLabel* m_sync = nullptr;
    QLabel* m_device = nullptr;
};
