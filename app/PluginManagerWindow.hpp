#pragma once

#include "plugins/PluginManager.hpp"

#include <QDialog>
#include <QString>

#include <vector>

namespace daw { class EngineController; }

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;

/// The plugin manager: search paths, the scan, the results, the blacklist.
///
/// A window of its own rather than a Settings tab. Settings is 600×500 and
/// every one of its pages is a short form; this needs a path list, a progress
/// bar, a table of hundreds of rows and a blacklist side by side, and it is a
/// place the user leaves open while a scan runs rather than a dialog they open
/// to change one value.
class PluginManagerWindow : public QDialog {
    Q_OBJECT
public:
    explicit PluginManagerWindow(daw::EngineController* controller,
                                 QWidget* parent = nullptr);

    /// Bring a specific tab to the front (0 Plugins, 1 Search Paths, 2 Blacklist).
    void showTab(int index);

signals:
    /// A scan finished and the plugin list changed. The browser lists the
    /// scanned plugins too, and a rescan that only refreshed this window would
    /// leave it showing yesterday's.
    void pluginsChanged();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* buildPluginsTab();
    QWidget* buildPathsTab();
    QWidget* buildBlacklistTab();

    void applyTheme();
    /// Progress bar, status line and button enablement from the manager's
    /// atomics. Called from the poll timer, so it must stay cheap.
    void refreshScanState();
    void refreshPlugins();
    void refreshPaths();
    void refreshBlacklist();

    void startScan(bool rescanAll);
    /// The format the Search Paths tab is currently editing.
    daw::plugins::Format selectedPathFormat() const;

    daw::EngineController* m_controller = nullptr;
    QTabWidget* m_tabs = nullptr;
    QTimer* m_poll = nullptr;

    // Scan header, shared by every tab.
    QPushButton* m_rescanButton = nullptr;
    QPushButton* m_rescanAllButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    bool m_wasScanning = false;

    // Plugins tab.
    QLineEdit* m_filter = nullptr;
    QComboBox* m_formatFilter = nullptr;
    QComboBox* m_typeFilter = nullptr;
    QTableWidget* m_pluginTable = nullptr;
    QLabel* m_pluginCount = nullptr;
    std::vector<daw::plugins::PluginDescriptor> m_plugins;

    // Search Paths tab.
    QComboBox* m_pathFormat = nullptr;
    QListWidget* m_pathList = nullptr;
    QPushButton* m_removePathButton = nullptr;

    // Blacklist tab.
    QTableWidget* m_blacklistTable = nullptr;
    QPushButton* m_unblacklistButton = nullptr;
    QPushButton* m_clearBlacklistButton = nullptr;
    std::vector<daw::PluginManager::BlacklistEntry> m_blacklistRows;
};
