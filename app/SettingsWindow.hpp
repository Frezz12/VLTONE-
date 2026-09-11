#pragma once

#include "Theme.hpp"

#include <QDialog>
#include <QHash>
#include <QString>

namespace daw { class EngineController; }

class RecordingSettingsPage;
class AudioSettingsPage;
class QuickImportSettingsPage;
class NotebookSettingsPage;
class ShortcutManager;
class QTabWidget;
class QListWidget;
class QKeySequenceEdit;
class QPushButton;
class QLineEdit;
class QComboBox;
class QLabel;
class QShowEvent;
class QCheckBox;
class QSlider;
class QRadioButton;
class QFileSystemWatcher;
class QWidget;

/// The unified, non-modal settings window: one place for Audio, Themes and
/// Keyboard Shortcuts, titled with the application name. Replaces the standalone
/// Audio Settings dialog and the Settings ▸ Theme submenu.
class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    SettingsWindow(daw::EngineController* controller, ShortcutManager* shortcuts,
                   QWidget* parent = nullptr);
    void refreshTimelineBackgroundSource();

    /// The tabs, by name. They used to be addressed by bare number, and the
    /// comment saying which was which had already gone stale twice.
    enum Tab {
        kAudioTab = 0,
        kQuickImportTab,
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
        kInterfaceTab,
    };

    /// Bring a specific tab to the front.
    void showTab(int index);

    /// Re-read the recording preferences. The mode also changes from the
    /// transport's Layers button, and an open window has to follow.
    void reloadRecordingPage();
    bool checkAudioPageForTest() const;
    static bool checkWheelRoutingForTest();
    void showQuickImportError(const QString& message);
    /// Import and apply a .vlttheme delivered by a file picker, Finder or
    /// Explorer. The operation owns a private copy before returning success.
    void importThemeFile(const QString& path);

signals:
    void transportPanelStyleChanged();
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
    QLineEdit* m_timelineBackgroundPath = nullptr;
    QPushButton* m_clearTimelineBackground = nullptr;
    QCheckBox* m_enableTimelineBackground = nullptr;
    /// Keep the dialog inside the current monitor's usable area. Every tab is
    /// scrollable, so shrinking the shell never hides a setting.
    void constrainToScreen();
    QWidget* buildThemesTab();
    QWidget* buildInterfaceTab();
    QWidget* buildThemeEditorTab();
    QWidget* buildShortcutsTab();
    QWidget* buildLanguageTab();
    void refreshLanguages();
    void refreshFontStatus();
    void refreshThemeLibrary();
    void refreshThemeControls();
    void refreshStartupTemplateOptions();
    bool applyInstalledTheme(const QString& filePath, const QString& storageId);
    void saveCurrentThemeToLibrary();
    void exportCurrentTheme();
    void applySelectedSavedTheme();
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
    QuickImportSettingsPage* m_quickImportPage = nullptr;
    NotebookSettingsPage* m_notebookPage = nullptr;
    QListWidget* m_themeList = nullptr;
    QListWidget* m_savedThemeList = nullptr;
    QPushButton* m_applySavedTheme = nullptr;
    QPushButton* m_exportSavedTheme = nullptr;
    QFileSystemWatcher* m_themeLibraryWatcher = nullptr;
    QHash<QString, QKeySequenceEdit*> m_editors;   // command id → editor

    Theme m_editTheme;                             // the working custom palette
    QLineEdit* m_themeNameEdit = nullptr;
    QWidget* m_themePreview = nullptr;
    QLabel* m_themeLiveStatus = nullptr;
    QLabel* m_themeContrastStatus = nullptr;
    QPushButton* m_themeSaveButton = nullptr;
    QComboBox* m_languageList = nullptr;
    QComboBox* m_startupTemplate = nullptr;
    QLabel* m_languageStatus = nullptr;
    QPushButton* m_removeLanguage = nullptr;
    QLabel* m_fontStatus = nullptr;
    QPushButton* m_resetFont = nullptr;
    QWidget* m_timelineFileRow = nullptr;
    QWidget* m_timelineBlurRow = nullptr;
    QComboBox* m_timelinePlacement = nullptr;
    QSlider* m_timelineVisibility = nullptr;
    QLabel* m_timelineVisibilityValue = nullptr;
    QSlider* m_timelineBlur = nullptr;
    QLabel* m_timelineBlurValue = nullptr;
    QCheckBox* m_timelineAnimate = nullptr;
    QLineEdit* m_headerBackgroundPath = nullptr;
    QPushButton* m_clearHeaderBackground = nullptr;
    QCheckBox* m_enableHeaderBackground = nullptr;
    QWidget* m_headerFileRow = nullptr;
    QWidget* m_headerBlurRow = nullptr;
    QComboBox* m_headerPlacement = nullptr;
    QSlider* m_headerVisibility = nullptr;
    QLabel* m_headerVisibilityValue = nullptr;
    QSlider* m_headerBlur = nullptr;
    QLabel* m_headerBlurValue = nullptr;
    QCheckBox* m_headerAnimate = nullptr;
    QSlider* m_playheadWidth = nullptr;
    QLabel* m_playheadWidthValue = nullptr;
    QCheckBox* m_playheadTrail = nullptr;
    QRadioButton* m_trackColourTint = nullptr;
    QRadioButton* m_neutralTint = nullptr;
    bool m_applyingInstalledTheme = false;
    QHash<QString, QPushButton*> m_swatches;       // field key → labelled colour button
};
