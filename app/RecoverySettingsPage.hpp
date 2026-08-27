#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;

/// Settings ▸ Recovery: whether the program keeps a copy of your work while you
/// edit, and whether the watchdog process runs alongside it.
///
/// The page exists as much to explain the guarantee as to change it. Recovery
/// that silently restores less than a user assumes is worse than no recovery,
/// so the limits are written on the page rather than left to be discovered
/// after a crash.
class RecoverySettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit RecoverySettingsPage(QWidget* parent = nullptr);

signals:
    /// Takes effect on the next launch; the shell shows that where it can.
    void changed();

private:
    void refreshStatus();

    QCheckBox* m_enabled = nullptr;
    QCheckBox* m_watchdog = nullptr;
    QLabel* m_status = nullptr;
};
