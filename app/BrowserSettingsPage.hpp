#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QListWidget;
class QLineEdit;
class QPushButton;
class QSlider;

/// Settings ▸ Browser: which folders the browser shows, which side it sits on,
/// and how auditioning behaves.
///
/// Writes through on every change, like the other settings pages — there is no
/// OK button anywhere in this window. Everything it touches goes through
/// `ui::browserprefs`, so the panel and this page cannot disagree about where a
/// value lives.
class BrowserSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit BrowserSettingsPage(QWidget* parent = nullptr);

signals:
    /// Anything on this page changed; the shell rebuilds the panel.
    void changed();

private:
    void refreshFolders();
    void addFolder();
    void removeSelectedFolder();

    QListWidget* m_folders = nullptr;
    QPushButton* m_remove = nullptr;
    QComboBox* m_side = nullptr;
    QCheckBox* m_autoPreview = nullptr;
    QCheckBox* m_loop = nullptr;
    QSlider* m_gain = nullptr;
    QComboBox* m_midiTempo = nullptr;
    QLineEdit* m_webHome = nullptr;
    QCheckBox* m_webBookmarksBar = nullptr;
};
