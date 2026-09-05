#pragma once

#include "Theme.hpp"

#include <QDialog>
#include <QHash>
#include <QString>

namespace daw { class EngineController; }

class RecordingSettingsPage;
class AudioSettingsPage;
class ShortcutManager;
class QTabWidget;
class QListWidget;
class QKeySequenceEdit;
class QPushButton;
class QLineEdit;
class QComboBox;
class QLabel;
class QShowEvent;

/// The unified, non-modal settings window: one place for Audio, Themes and
/// Keyboard Shortcuts, titled with the application name. Replaces the standalone
/// Audio Settings dialog and the Settings ▸ Theme submenu.
class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    SettingsWindow(daw::EngineController* controller, ShortcutManager* shortcuts,
                   QWidget* parent = nullptr);

    /// The tabs, by name. They used to be addressed by bare number, and the
    /// comment saying which was which had already gone stale twice.
    enum Tab {
        kAudioTab = 0,
        kTransportTab,
        kRecordingTab,
        kContextPanelTab,
        kBrowserTab,
        kNotebookTab,
        kAiTab,
        kAccountTab,
        kLanguageTab,
        kRecoveryTab,
        kThemesTab,
        kThemeEditorTab,
        kShortcutsTab,
    };

    /// Bring a specific tab to the front.
    void showTab(int index);

    /// Re-read the recording preferences. The mode also changes from the
    /// transport's Layers button, and an open window has to follow.
    void reloadRecordingPage();
    bool checkAudioPageForTest() const;

signals:
    /// A context-panel profile or its transparency setting changed.
    void contextPanelSettingsChanged();
    /// The browser's folders, side or preview options changed.
    void browserSettingsChanged();
    /// The notebook's background, motion or custom fonts changed.
    void notebookSettingsChanged();
    /// The assistant's provider, key, model or step limit changed.
    void aiSettingsChanged();
    /// MainWindow owns the unsaved-project decision before credentials vanish.
    void accountLogoutRequested();
    /// The global recording mode changed on the Recording tab.
    void recordModeChanged();
    /// The optional compact audio-CPU strip changed on the Audio tab.
    void cpuStatusBarVisibilityChanged(bool visible);
    /// How selected tracks are tinted changed on the Themes tab.
    void selectionTintChanged();
    /// A local arrangement/header image, GIF, video or presentation changed.
    void themeBackgroundSettingsChanged();
    void restartRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// Keep the dialog inside the current monitor's usable area. Every tab is
    /// scrollable, so shrinking the shell never hides a setting.
    void constrainToScreen();
    QWidget* buildThemesTab();
    QWidget* buildThemeEditorTab();
    QWidget* buildShortcutsTab();
    QWidget* buildLanguageTab();
    void refreshLanguages();
    void refreshFontStatus();
    void refreshShortcutEditors();
    /// Push the working palette to the app (live), persisting it as "custom".
    void applyEditTheme();
    /// Repaint every colour swatch from the working palette.
    void refreshSwatches();

    daw::EngineController* m_controller = nullptr;
    ShortcutManager* m_shortcuts = nullptr;
    QTabWidget* m_tabs = nullptr;
    RecordingSettingsPage* m_recordingPage = nullptr;
    AudioSettingsPage* m_audioPage = nullptr;
    QListWidget* m_themeList = nullptr;
    QHash<QString, QKeySequenceEdit*> m_editors;   // command id → editor

    Theme m_editTheme;                             // the working custom palette
    QLineEdit* m_themeNameEdit = nullptr;
    QComboBox* m_languageList = nullptr;
    QLabel* m_languageStatus = nullptr;
    QPushButton* m_removeLanguage = nullptr;
    QLabel* m_fontStatus = nullptr;
    QPushButton* m_resetFont = nullptr;
    QHash<QString, QPushButton*> m_swatches;       // field key → swatch button
};
